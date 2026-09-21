# NInfer (YaRN)

This is a fork of [Ninfer](https://github.com/gzenz/ninfer), an inference engine designed to be optimized for Qwen3.x models and an RTX 5090. This project adds YaRN context extension and other enhancements:

## Gzenz's fork of Doelfke's YaRN fork

Thanks to [Doelfke](https://github.com/Doelfke/ninfer-yarn) for the YaRN work — this tree
is our fork of his fork of [our fork](https://github.com/gzenz/ninfer). On top of
Doelfke's tree we have added:

- **KV / materialization correctness** — an atomic "seal-window claim" in the
  materialization planner that closes a TOCTOU race: a concurrent demote could bump a
  victim's slot generation between the final `assess` and `seal`, forcing a full
  re-prefill on a re-touch. The claim serializes that window so re-touches restore from
  the (device- or host-resident) KV instead of re-prefilling.
- **Value-aware demote-to-host** — under device saturation the pressure planner ranks
  private victims by re-prefill cost and charges a bounded rank to the evict cost, so it
  demotes the highest-value (most expensive to rebuild) victims to host and
  evicts-and-drops the cheapest, preserving restorable checkpoints on re-touch. Exposed as
  `pressure.private_owners_demoted` in `/stats` and the request log.
- **Tool-call hardening** — a generalized tolerant opener that recovers `<function=…>`
  when the model drops the `<`, leaks a ChatML `<|im_start|>` marker, drops the `function`
  keyword, or doubles the `<`.
- **Monitoring** — a self-contained dashboard (`tools/monitor/`) adapted to the v3 stats
  shape, a dedicated `/stats` port, and an `http {in_flight, max_in_flight}` block in
  `/stats`; it shows live device/host KV occupancy, per-request queue time, and the
  detected speculative backend.

**A note on long context (YaRN):** pushing the NVFP4 context past ~350k surfaces a
*behavioral* symptom, not an NInfer defect. Over a very long agentic history (~350k+), the
model **re-announces the same state and next-step every turn without completing the task**
("macro stuckness") until Claude Code's compaction truncates the context and it recovers.
Our forensics rule out an engine cause:

- A multi-window long-context **recall probe** returns **4/4 on both cold prefill and the
  cached/restore path across the 262144 YaRN ramp** (at 213k / 330k / 380k real tokens),
  spanning below and above the ramp — attention recall is intact on both paths.
- The long-dump rate climbs **only above ~350k** (≥8k completions: 0.5% <200k → 1.5%
  300–350k → 3.4% >350k), not at the 256k ramp.
- The rope/position path is untouched since before the symptom and the YaRN formula is
  verified; the ChatML template renders the full history with no truncation.

The symptom is task-state tracking over an extremely long agentic history — a model
behavior, not an attention/cache/YaRN defect. We therefore currently recommend running the
**Swift model at a 262k context** (see the deploy config below) rather than a
YaRN-extended NVFP4 context.

**Swift model artifact.** We run the single-file v3 NInfer artifact
[`CaptainArni/Swift-Qwen3.8-27B-NInfer`](https://huggingface.co/CaptainArni/Swift-Qwen3.8-27B-NInfer)
(21.2 GiB, DFlash2 draft included, sha256 `5412a0e7…`):

```bash
hf download CaptainArni/Swift-Qwen3.8-27B-NInfer \
  qwen3_8_27b_nvfp4swift.ninfer --local-dir ~/ninfer-models
```

Provenance: built from
[`ukisai/Swift-Qwen3.8-27B-NVFP4`](https://huggingface.co/ukisai/Swift-Qwen3.8-27B-NVFP4)
(the pre-quantized partner of
[`ukisai/Swift-Qwen3.8-27b`](https://huggingface.co/ukisai/Swift-Qwen3.8-27b), a
reasoning-efficient fine-tune of Qwen3.8-27B). Swift uses its own MTP head;
`--spec dflash2 --draft-tokens 7` uses the DFlash2 draft for lighter in-VRAM
footprint (18.9 vs 20.5 GiB).

The original YaRN enhancement list:

- `--rope-scaling-factor` and `--rope-scaling-original-context` 
- `--tolerant-tool-calls` for Qwen, to ensure tool calls are OpenAI compatible.  
- `--vision` to enable vision (device-resident Vision weights). A CPU-offload variant
  (`--vision-cpu`) dequantizes the Vision encoder into host DRAM at load and runs the ViT on the
  CPU during prefill, keeping the ~1.7 GB Vision weight set out of the device arena. Implies
  `--vision`; prefill is slower when the prompt carries media.

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

With dflash2:
```
  "--max-context", "400000",
  "--kv-capacity", "400000",
  "--spec", "dflash2",
  "--draft-tokens", "7",
```