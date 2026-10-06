# NInfer

An inference engine for Qwen3.x on an RTX 5090 — device- and host-tiered KV caching, a pressure planner
that demotes before it evicts, and YaRN context extension.

## What this line adds

- **KV / materialization correctness** — an atomic "seal-window claim" in the materialization planner that
  closes a TOCTOU race: a concurrent demote could bump a victim's slot generation between the final
  `assess` and the `seal`, failing the seal's revalidate and forcing a full re-prefill on a re-touch. The
  claim serializes that window so re-touches restore from the (device- or host-resident) KV instead of
  re-prefilling.
- **Value-aware demote-to-host** — under device saturation the pressure planner ranks private victims by
  re-prefill cost and charges a bounded rank to the evict cost, so it demotes the highest-value (most
  expensive to rebuild) victims to host and evicts-and-drops the cheapest, preserving restorable
  checkpoints on re-touch. Exposed as `pressure.private_owners_demoted` in `/stats` and the request log.
- **Host-tiered memory on one elastic pinned budget** — the host KV arena and the host state slots draw on
  a single pinned byte budget instead of two fixed, mutually-blind allocations, so the split between them is
  emergent rather than configured. `--host-kv-mib` is now the ceiling for **all** host caching, not the KV
  arena's private span: the arena starts at a quarter of it and grows on demand into the same pile the state
  slots use. Growth is decided fail-closed from `/proc/meminfo` against `--host-ram-reserve-mib`; shrink is
  trailing-only, floored at the configured slot count, and applies to the state slots only — KV arena growth
  is permanent for the life of the process. Within a pinned chunk, placement now takes the **tightest fitting
  extent**; on an 8-agent load this raised the contiguous share of free memory (13% → 42%) and cut placement
  misses (25 → 12). The pool's capacity, free bytes, largest free run and fragmentation misses are exported
  in `/stats` and graphed in the monitor. *Limit as shipped:* the planner prices a demote against the pool's
  **current** capacity, so elastic growth is reachable from the KV `prepare` path but **not** from the demote
  path — this is an allocator with the planning-time reachability step still to do, not a fix for eviction.
- **Tool-call hardening** — a generalized tolerant opener that recovers `<function=…>` when the model drops
  the `<`, leaks a ChatML turn marker, drops the `function` keyword, or doubles the `<`.
- **Monitoring** — a self-contained dashboard (`tools/monitor/`) adapted to the v3 stats shape, a dedicated
  `/stats` port (`--stats-port`), and an `http {in_flight, max_in_flight}` block in `/stats`; it shows live
  device/host KV occupancy, per-request queue time, the detected speculative backend, and the pinned pool's
  fragmentation.

**A note on long context (YaRN):** pushing the NVFP4 context past ~350k surfaces a *behavioral* symptom, not
an NInfer defect. Over a very long agentic history (~350k+), the model **re-announces the same state and
next-step every turn without completing the task** ("macro stuckness") until Claude Code's compaction
truncates the context and it recovers. Our forensics rule out an engine cause:

- A multi-window long-context **recall probe** returns **4/4 on both cold prefill and the cached/restore
  path across the 262144 YaRN ramp** (at 213k / 330k / 380k real tokens), spanning below and above the ramp —
  attention recall is intact on both paths.
- The long-dump rate climbs **only above ~350k** (≥8k completions: 0.5% <200k → 1.5% 300–350k → 3.4%
  >350k), not at the 256k ramp.
- The rope/position path is untouched since before the symptom and the YaRN formula is verified; the ChatML
  template renders the full history with no truncation.

The symptom is task-state tracking over an extremely long agentic history — a model behavior, not an
attention/cache/YaRN defect. We therefore currently recommend running the **Swift model at a 262k context**
rather than a YaRN-extended NVFP4 context at 350k+: the `dflash2` block at the end of this file is our
production configuration, and the docker-compose example in the middle is the older YaRN/NVFP4 setup at
420k, which is not what we deploy.

**Swift model artifact.** We run the single-file v3 NInfer artifact
[`CaptainArni/Swift-1.5-Qwen3.8-27B-NInfer`](https://huggingface.co/CaptainArni/Swift-1.5-Qwen3.8-27B-NInfer)
(21.2 GiB, text + vision + MTP + DFlash2, sha256
`683f5086a0e24e9b7c5caad256d373e13eff4e0491564f5b8c2acde33e52a11e`):

```bash
hf download CaptainArni/Swift-1.5-Qwen3.8-27B-NInfer \
  qwen3_8_27b_nvfp4swift15.ninfer --local-dir ~/ninfer-models/swift15
```

Provenance: converted from
[`ukisai/Swift-1.5-Qwen3.8-27b-NVFP4`](https://huggingface.co/ukisai/Swift-1.5-Qwen3.8-27b-NVFP4) (the
pre-quantized partner of
[`ukisai/Swift-1.5-Qwen3.8-27b`](https://huggingface.co/ukisai/Swift-1.5-Qwen3.8-27b), a
reasoning-efficient fine-tune of Qwen3.8-27B). Swift ships its own MTP head and a DFlash2 draft: the weights
cost 18.9 GiB in VRAM under `--spec mtp` and 20.5 GiB under `--spec dflash2` — the DFlash2 draft is the
heavier option, and the one we deploy.

The original YaRN enhancement list:

- `--rope-scaling-factor` and `--rope-scaling-original-context` 
- `--tolerant-tool-calls` for Qwen, to ensure tool calls are OpenAI compatible.  
- `--vision` to enable vision (device-resident Vision weights). A CPU-offload variant
  (`--vision-cpu`) dequantizes the Vision encoder into host DRAM at load and runs the ViT on the
  CPU during prefill, keeping the Vision weight set out of the device arena. Implies `--vision`;
  prefill is slower when the prompt carries media.

When using an NVFP4 KV cache, this allows you to reach a 420,000 token context, while having vision enabled, MTP -- supporting 2 sessions at once.  This assumes `maxOutputTokens` is set to 130,000 in your code editor.



Example:

```bash

services:
  ninfer:
    image: ninfer:local
    command: [
        "ninfer-serve", 
        "/models/qwen3_8_27b_nvfp4.ninfer", 
        "--host", "0.0.0.0",
        "--max-context", "420000",
        "--kv-dtype", "nvfp4",
        "--kv-capacity", "420000",
        "--max-concurrency", "2",
        "--spec", "mtp",
        "--draft-tokens", "5",
        "--lm-head-draft",
        "--preserve-thinking",
        "--tolerant-tool-calls",
        "--rope-scaling-factor", "2",
        "--rope-scaling-original-context", "262144",
        "--vision-cpu",
        "--pending-timeout-ms", "300000",
        "--device-state-slots", "2",
        "--host-state-slots", "8",
        "--host-kv-mib", "8192"
      ]
    ports:
      - "1234:8080"
    volumes:
      - /home/x/models:/models:ro
      
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              device_ids: ["0"]
              capabilities: [gpu]

```

With dflash2 (this is what we actually run in production, at the 262k context recommended above — the
larger number in the older block was never the deployed setting):

```
  "--max-context", "262144",
  "--kv-capacity", "262144",
  "--spec", "dflash2",
  "--draft-tokens", "7",
```
