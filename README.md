<!-- fork-changelog -->
## Fork changes vs upstream

This fork targets **reliable 555k-context inference with 3 concurrent agentic sessions** (Claude Code, etc.) on a single RTX 5090 with NVFP4 KV. It is a clean divergence — upstream has its own host-KV cache implementation; ours is independently developed and battle-tested with real workloads.

### Context cache and eviction

- **Host-KV safety net** (`--host-kv-mib`, `--host-state-slots`): when device KV is full, evicted continuations are spilled to a pinned host arena (D2H) and restored via H2D on cache reuse. Scatter-gather allocation handles arena fragmentation. Smallest-first eviction with pinned-entry protection. Verified across 140+ requests with 3 concurrent 330k–470k sessions.
- **Rewrite checkpoint at turn boundary**: checkpoint is captured where the prompt ends (before reasoning begins), not at the execution frontier. Follow-up prompts with `preserve_thinking=off` match the stored ledger up to the checkpoint.
- **Token stability with `preserve_thinking=off`**: reasoning is dropped from ALL assistant messages when `preserve_thinking=off`, keeping prompt tokens stable across turns. Without this, the last assistant message's reasoning was kept on its turn but dropped on the next, shifting all subsequent tokens and breaking prefix reuse.
- **Unified checkpoint host demotion**: KV and checkpoint state move together to host, or not at all. The pressure planner evicts entire continuations (KV + state) instead of dropping rewrite checkpoints. State-only safety net entries replace the old `dropped_checkpoint_captures_` side store. Backend KV spill uses scatter-gather (no fragmentation failures). No artificial count or fragment limits.
- **Pressure accounting fix**: `complete_pressure_delta` no longer overwrites actual freed resources with planned values. Bad_alloc retry path cleans up partial allocations (state images, KV activations, root addresses, restore vectors) before retrying.

### Engine robustness

- **OOM recovery** (`std::bad_alloc` catch): materialization reserve and worker loop catch OOM, clear active state while preserving pending requests, back off admission for 4 iterations, fail all after 8 consecutive recoveries. Bad_alloc retry cleans up partial allocations before retrying `prepare_materialization`.

### Context and model

- **YaRN context extension** (`--rope-scaling-factor`, `--rope-scaling-original-context`): linear RoPE scaling to 555k context (c=3+vision) or 600k (c=1). No quality loss measured up to 600k.
- **NVFP4 KV cache** (`--kv-dtype nvfp4`): 4-bit E2M1 codes + E4M3 group-16 scales. 45% less KV VRAM than int8.
- **Froggeric v22.5 chat template**: executed from the template file with no-dangling-intent enforcement, effort aliases, leading system-message merge, tool-error tiering, and think-close variant handling.
- **Tolerant tool-call recovery** (`--tolerant-tool-calls`): recovers complete Qwen calls with malformed wrappers.
- **Reasoning-effort tier mapping**: High/Max map to XHigh instead of rejecting.
- **Post-thinking sampling**: a thinking request switches to a dedicated lower-temperature preset
  from the token after the model closes its reasoning block (e.g. temperature 1.0 while
  reasoning, 0.2 for the answer). Registered per model; override with
  `--post-thinking-temperature/-top-p/-top-k` or the request's `post_thinking` field.

### Monitoring and tooling

- **/stats endpoint**: runtime gauges, KV transfer counters, pressure metrics, cache reuse paths.
- **Monitoring dashboard** (`tools/monitor/`): live GPU/util/decode/prefill/TTFT graphs, KV cache occupancy, request log, 12VHPWR sensor.
- **E2E test suite** (`tools/e2e/`): multi-phase KV eviction, device-KV pressure, slot pressure, trash mode.
- **Request-log rotation** (`--request-log-max-mib`, `--request-log-keep`): size-based JSONL rotation.

Details: [docs/maintainer/kv-nvfp4-yarn.md](docs/maintainer/kv-nvfp4-yarn.md)

<!-- /fork-changelog -->


## Supported Models and Templates

> **⚠️ Artifact incompatibility notice:** This fork is **NOT compatible** with the
> maintainer's upstream artifact
> [neroued/Qwen3.8-27B-nvfp4-NInfer](https://huggingface.co/neroued/Qwen3.8-27B-nvfp4-NInfer)
> (23.7 GB). That artifact requires upstream revision `385b30ce` and uses a newer
> tensor descriptor format that this fork does not support.
>
> **Use QUASAR or Ostfralla artifacts instead.** Both maintain quality at a
> significantly smaller size (~17.5 GB vs 23.7 GB):
> - [QUASAR-QAT/Qwen3.8-27B-QUASAR-NVFP4](https://huggingface.co/QUASAR-QAT/Qwen3.8-27B-QUASAR-NVFP4/)
> - [Ostfralla/Qwen3.8-27B-NVFP4-NInfer](https://huggingface.co/Ostfralla/Qwen3.8-27B-NVFP4-NInfer)

This fork works with any Qwen3.8-27B NVFP4 `.ninfer` artifact from QUASAR or
Ostfralla converters. The binding auto-detects the GDN control layout (split
`a_projection`/`b_projection` vs fused `a_b_projection`) and quantization format
(NVFP4 vs BF16) per layer, so both Ostfralla and QUASAR images work with
`--weights-profile qwen36-nvfp4`.

### Quick start

```bash
./build/apps/ninfer-serve <model.ninfer> \
   --host 0.0.0.0 --port 8080 --kv-dtype nvfp4 --vision \
   --spec mtp --draft-tokens 5 --lm-head-draft \
   --tolerant-tool-calls --host-kv-mib 30720 \
   --rope-scaling-factor 2.12 --rope-scaling-original-context 262144 \
   --chat-template tests/fixtures/frontend/froggeric_v225_chat_template.jinja \
   --chat-template-semantics froggeric \
   --weights-profile qwen36-nvfp4
```

### Chat template loading

**`--chat-template PATH`** loads a jinja template from disk, overriding the artifact's
embedded template. No artifact patching needed — any `.ninfer` image works with any
template. The file is executed: it is the renderer, so its prompt text, tool-instruction
wording, and message composition are what the engine emits. The engine implements the
HuggingFace Jinja environment plus the constructs these templates use; an unsupported
filter fails loudly at load instead of silently rendering different text.

**`--chat-template-semantics MODE`** selects the advertised prompt capabilities and the
registered-template diagnostic: `auto` (hash-match, default), `froggeric`,
`thinking-toggle`, `reasoning-effort`, `generic` (accept any template with ThinkingToggle
capabilities). It no longer selects a separate C++ renderer, because the template is the
only renderer.

The Froggeric v22.5 template (`tests/fixtures/frontend/froggeric_v225_chat_template.jinja`)
improves tool-call reliability for agentic workloads with stricter instructions,
no-dangling-intent enforcement, and think-close variant handling.

### Weights profile

**`--weights-profile PROFILE`** overrides the weights profile resolved from artifact
identity: `qwen36-nvfp4`, `qwen38-nvfp4`, `qwen36-groupwise-int`, `qwen38-groupwise-int`.
Use `qwen36-nvfp4` for Ostfralla and QUASAR artifacts that carry the Qwen3.6 NVFP4
tensor layout (W8G32 embedding + NVFP4 quantization). The binding auto-detects
per-layer layout differences (fused vs split GDN control, NVFP4 vs BF16 attention).

### Post-thinking sampling

Registered Qwen models carry a **post-thinking preset** (temperature 0.2, top-p 0.95,
top-k 20). For a thinking request, the engine resolves the normal thinking preset at
submission and switches to the post-thinking preset from the token after the model closes
its reasoning block. The answer and tool calls are therefore sampled with the lower
temperature while reasoning keeps the higher one. Non-thinking requests and models
without a registered preset are unaffected.

Override per request or per server:

```bash
# CLI: explicit post-thinking fields, or the combined form
./build/apps/ninfer model.ninfer --prompt "..." \
  --post-thinking-temperature 0.2 --post-thinking-top-p 0.95 --post-thinking-top-k 20
#   --post-thinking-sampler temp=0.2,top_p=0.95,top_k=20

# HTTP: optional post_thinking object (OpenAI Chat, OpenAI Responses, Anthropic Messages)
curl http://127.0.0.1:8080/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "messages": [{"role": "user", "content": "Hello"}],
    "temperature": 1.0,
    "post_thinking": {"temperature": 0.2, "top_p": 0.95, "top_k": 20}
  }'
```

Omitted `post_thinking` fields fall back to the model's registered preset; an omitted
seed inherits the request's resolved seed. `--greedy` forces both phases to exact argmax.

The lower emission temperature is a reliability setting rather than a quality one: see
[the post-thinking temperature study](docs/maintainer/post-thinking-temperature.md) for the measured
effect - neutral on accuracy across BFCL v4, AIME and IFBench, with reproducibility of long-form
answers under load as the measurable contribution.
See [docs/cli.md](docs/cli.md) and [docs/serving.md](docs/serving.md) for the full field
list and ranges.


## Quick start

NInfer requires 64-bit Linux, an NVIDIA GeForce RTX 5090, CUDA Toolkit 13.1 or newer, CMake 3.28 or
newer, a C++20 host compiler, Ninja, `pkg-config`, FFmpeg development libraries
(`libavformat >= 60`, `libavcodec >= 60`, `libavutil >= 58`, and `libswscale >= 7`), and
`libcurl >= 7.85`. The build rejects CUDA architectures other than `sm_120a`.

Build the product binaries:

```bash
git clone https://github.com/Neroued/ninfer.git
cd ninfer

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Tests, benchmarks, and maintainer tools are excluded from the default build. There is no install
target or packaged binary distribution; run NInfer from its source build tree.

Download a compatible QUASAR or Ostfralla artifact with the Hugging Face CLI:

```bash
# QUASAR (recommended — smaller, tuned for agentic workloads)
hf download QUASAR-QAT/Qwen3.8-27B-QUASAR-NVFP4 \
  --local-dir models

# Ostfralla (abliterated variant)
hf download Ostfralla/Qwen3.8-27B-NVFP4-NInfer \
  --local-dir models
```

After download, pass the `.ninfer` file to `ninfer-serve` (filename varies by repo).

Start a long-running text/agent server with two active-request lanes and explicit Device/Host
checkpoint capacity:

```bash
./build/apps/ninfer-serve models/qwen3_8_27b_nvfp4.ninfer \
  --max-context 240000 \
  --kv-capacity 240000 \
  --max-concurrency 2 \
  --kv-dtype fp8 \
  --device-state-slots 2 \
  --host-state-slots 8 \
  --host-kv-mib 8192 \
  --spec mtp --draft-tokens 3 \
  --lm-head-draft \
  --preserve-thinking
```

Each request has a 240,000-token logical ceiling. A shared 240,000-token Device KV pool serves
admitted requests; two requests run concurrently when their combined reservations fit. The cache
tiers provide two Device checkpoint slots, eight pinned Host State slots, and 8 GiB of pinned Host
KV beyond the two active StateImages.

Send an OpenAI-style request:

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "messages": [{"role": "user", "content": "Reply with one short sentence."}],
    "max_tokens": 64
  }'
```

Run a one-shot CLI request with a 32,768-token allocation:

```bash
./build/apps/ninfer models/qwen3_8_27b_nvfp4.ninfer \
  --prompt "Explain prefill and decode, then give a concise conclusion." \
  --max-context 32768 \
  --max-new 8192 \
  --kv-dtype fp8 \
  --spec mtp --draft-tokens 3 \
  --lm-head-draft
```

Answer content is written to stdout. Loading progress, reasoning, timings, throughput, memory, and
speculative-decoding statistics are written to stderr. Use `--messages FILE` and `--vision` for
structured image/video input; see the [CLI guide](docs/cli.md) and [committed examples](examples/cli/).

## Resource-aware long-context reuse

A reusable prefix checkpoint contains KV and the complete continuation state for its exact prompt
frontier. A Device-resident checkpoint resumes directly. Under pressure, the planner weighs Device
retention, pinned Host State/KV, and eviction by immediate restore work and later reuse cost. Active
requests retain their completion reservations.

See [Resource scheduling and context cache](docs/maintainer/resource-scheduling-and-context-cache.md)
for the algorithm and [Serve TTFT benchmark](tools/bench/ttft/) for public-HTTP coverage of hot
reuse, Host resume, eviction, shared prefixes, scheduling boundaries, and multimodal load.

## Performance

Published measurements use an RTX 5090. [Performance](docs/performance.md) records the exact
benchmark profiles and methodology.

### Concurrent MTP3 decode

Saturated decode used INT8 group-64 KV, CUDA Graphs, MTP3, and one 8,192-token generation per active
request. Values are aggregate committed decode throughput and MTP acceptance from complete
intervals whose actual decode batch equaled the configured concurrency.

| Model profile | C=1 tok/s / accept | C=2 tok/s / accept | C=4 tok/s / accept | C=8 tok/s / accept | C8 / C1 |
|---|---:|---:|---:|---:|---:|
| Qwen3.6-27B `groupwise-int` | 185.8 / 68.2% | 247.0 / 69.0% | 309.5 / 68.4% | 535.0 / 68.3% | 2.88× |
| Qwen3.6-27B `nvfp4` | 202.4 / 69.3% | 399.7 / 71.4% | 699.7 / 69.3% | 1,146.9 / 68.6% | 5.67× |
| Qwen3.6-35B-A3B `groupwise-int` | 593.0 / 67.2% | 877.7 / 68.2% | 1,166.0 / 69.8% | 1,313.8 / 67.3% | 2.22× |
| Qwen3.8-27B `nvfp4` | 143.8 / 48.9% | 267.6 / 48.1% | 461.1 / 45.8% | 766.6 / 46.0% | 5.33× |

### Single-request serving

The serial serving corpus used INT8 group-64 KV, CUDA Graphs, a 1,024-token prefill chunk, and five
fixed seeds after warm-up. The table keeps one short-prefill, one extreme-prefill, and one
structured-output MTP3 point for each published profile; the full context and scenario matrices are
in the performance document.

| Model profile | 7,680-token prefill | 260,096-token prefill | Structured MTP3 decode |
|---|---:|---:|---:|
| Qwen3.6-35B-A3B `groupwise-int` | 15,544.3 tok/s | 5,157.1 tok/s | 770.9 tok/s |
| Qwen3.6-27B `groupwise-int` | 3,218.1 tok/s | 1,614.8 tok/s | 193.0 tok/s |
| Qwen3.6-27B `nvfp4` | 11,191.5 tok/s | 2,510.6 tok/s | 252.2 tok/s |
| Qwen3.8-27B `groupwise-int` | 3,274.7 tok/s | 1,609.7 tok/s | 224.4 tok/s |
| Qwen3.8-27B `nvfp4` | 8,340.4 tok/s | 2,203.1 tok/s | 219.8 tok/s |

## Evaluation

> **Note:** The scores below were measured with the upstream maintainer's artifact.
> QUASAR and Ostfralla artifacts use the same base model (Qwen3.8-27B) and are
> expected to produce comparable results (Ostfralla is an abliterated variant with
> modified refusal behavior). The upstream artifact is not compatible with this
> fork's current build — see the [artifact notice](#supported-models-and-templates) above.

Capability scores were measured through NInfer's OpenAI-compatible serving route with thinking
enabled, MTP3, and EvalScope 1.9.0 (0-shot, rule scoring, one sample per problem):

| Model profile | AIME 2025 | AIME 2026 | GPQA-Diamond | ERQA | RealWorldQA |
|---|---:|---:|---:|---:|---:|
| [Qwen3.6-27B groupwise-int](model-cards/Qwen3.6-27B-NInfer/README.md) | 86.67% | 93.33% | 86.87% | — | — |
| [Qwen3.6-27B NVFP4](model-cards/Qwen3.6-27B-nvfp4-NInfer/README.md) | 93.33% | 93.33% | 84.34% | — | — |
| [Qwen3.6-35B-A3B groupwise-int](model-cards/Qwen3.6-35B-A3B-NInfer/README.md) | 90.00% | 90.00% | 85.35% | — | — |
| [Qwen3.8-27B groupwise-int](model-cards/Qwen3.8-27B-NInfer/README.md) | 96.67% | 96.67% | 87.37% | 66.25% | 82.22% |
| [Qwen3.8-27B NVFP4](model-cards/Qwen3.8-27B-nvfp4-NInfer/README.md) | 96.67% | 96.67% | 90.40% | 66.25% | 83.53% |

The Qwen3.6 rows used temperature 0.6 and presence penalty 1.0; the Qwen3.8 rows used temperature
1.0 and presence penalty 0.0. Multimodal evaluation used `--vision` and an 81,920-token context
limit. Text evaluation used 262,144 tokens except Qwen3.8-27B NVFP4, which used 252,928 tokens to
fit the RTX 5090 after weights. Each score is one sample per problem; model cards contain the
correct/total counts and evaluation notes.

### Perplexity

Run the fixed four-domain quick corpus through the artifact's tokenizer and Text model:

```bash
./build/apps/ninfer-perplexity models/qwen3_8_27b_nvfp4.ninfer \
  --corpus eval/corpora/perplexity-1m/manifest.json \
  --quick --kv-dtype fp8
```

The evaluator reports token-weighted fixed-window causal perplexity and writes a complete JSON
record under `profiles/perplexity/`. See [Perplexity evaluation](docs/perplexity.md) for the metric,
corpus, custom-text mode, and comparison rules.

## Startup notes

GPU residency is fixed at process startup. `--spec` selects speculative decoding residency, and
`--vision` selects Vision residency. DFlash is available for text-only Qwen3.6-35B-A3B execution.

## Docker

Build the runtime image on a host with the NVIDIA Container Toolkit:

```bash
docker build --tag ninfer:local .
```

Mount the downloaded model and run the same example server profile:

```bash
docker run --rm \
  --gpus '"device=0"' \
  --publish 8080:8080 \
  --volume "$PWD/models:/models:ro" \
  ninfer:local \
  ninfer-serve /models/qwen3_8_27b_nvfp4.ninfer \
  --host 0.0.0.0 \
  --max-context 240000 \
  --kv-capacity 240000 \
  --max-concurrency 2 \
  --kv-dtype fp8 \
  --device-state-slots 2 \
  --host-state-slots 8 \
  --host-kv-mib 8192 \
  --spec mtp --draft-tokens 3 \
  --lm-head-draft \
  --preserve-thinking
```

## Capabilities and limits

All registered model IDs support:

- text generation with thinking and non-thinking prompt modes;
- image, multi-image, video, and mixed multimodal messages;
- chunked prefill, exact-batch CUDA Graph decode, and startup-bounded batched decode;
- MTP speculative decoding with draft windows from one to five;
- BF16, INT8 group-64, and row-scaled FP8 E4M3 KV storage;
- offline causal-perplexity scoring with the same Text model and selectable KV storage;
- private and shared exact-prefix reuse with Device/Host State and KV retention;
- model-aware sampling defaults, explicit sampler overrides, and a registered post-thinking
  preset that takes over from the token after the model closes its reasoning block;
- OpenAI Responses Core, OpenAI Chat Completions, and Anthropic Messages, including streaming,
  tools, local response state, token counting, and usage accounting.

The 35B-A3B target additionally supports text-only DFlash with draft windows from one to fifteen.

The product boundary remains intentionally small:

- one RTX 5090 and one resident model per Engine;
- a startup-fixed capacity of one to eight active requests with bounded FIFO ingress;
- no request preemption, priority/QoS, active-request swapping, weight offload, multi-GPU, or
  distributed serving;
- one shared startup-fixed KV pool across active requests and retained prefixes;
- no runtime model discovery or unregistered checkpoint fallback;
- parsed tool calls are returned to the client; NInfer does not execute tools;
- the in-tree C++ headers are not distributed as an installed SDK.

`--max-context` is each sequence's logical limit. `--kv-capacity` sizes the shared Main Text KV pool
used by active requests and retained prefixes; `auto` resolves the largest legal capacity at
startup from the memory remaining after weights while keeping 1 GiB of sizing headroom. Explicit
capacities remain fixed for the process lifetime.

## Documentation

- [Documentation index](docs/README.md)
- [CLI](docs/cli.md)
- [HTTP serving](docs/serving.md)
- [Performance](docs/performance.md)
- [Perplexity evaluation](docs/perplexity.md)
- [Resource scheduling and context cache](docs/maintainer/resource-scheduling-and-context-cache.md)
- [Serve TTFT benchmark](tools/bench/ttft/)
- [CLI examples](examples/cli/)
- [Contributing](CONTRIBUTING.md)

Run the relevant `--help` for the exact current option contract.

## License

NInfer is licensed under the [Apache License 2.0](LICENSE).

The published artifacts are derived from
[Qwen/Qwen3.6-27B](https://huggingface.co/Qwen/Qwen3.6-27B),
[Qwen/Qwen3.8-27B](https://huggingface.co/Qwen/Qwen3.8-27B), and
[Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B). This fork uses QUASAR and
Ostfralla NVFP4 artifacts for Qwen3.8-27B, which maintain quality at a significantly smaller
size than the upstream maintainer's artifact:
[QUASAR-QAT/Qwen3.8-27B-QUASAR-NVFP4](https://huggingface.co/QUASAR-QAT/Qwen3.8-27B-QUASAR-NVFP4/)
and
[Ostfralla/Qwen3.8-27B-NVFP4-NInfer](https://huggingface.co/Ostfralla/Qwen3.8-27B-NVFP4-NInfer).
The Qwen3.8-27B NVFP4 weights are derived from
[unsloth/Qwen3.8-27B-NVFP4](https://huggingface.co/unsloth/Qwen3.8-27B-NVFP4). The Qwen3.6-27B NVFP4 artifact
also uses the fixed packed weights from
[rdtand/Qwen3.6-27B-PrismaSCOUT-Blackwell-NVFP4-BF16-vllm](https://huggingface.co/rdtand/Qwen3.6-27B-PrismaSCOUT-Blackwell-NVFP4-BF16-vllm).
These source repositories are distributed under Apache-2.0. Vendored dependencies retain their own license files
under `third_party/`.
