# ninfer Decode Benchmark Methodology

## Purpose

Standardized benchmark for measuring decode throughput on the ninfer inference
engine (qwen3.8-27b NVFP4, RTX 5090, WSL2). Use this as the go-to comparison
basis for future optimizations.

## Test environment

- **Hardware**: RTX 5090, 32GB VRAM, 64GB host RAM
- **OS**: Windows 11 + WSL2 Ubuntu-24.04
- **CUDA**: 13.3, sm_120a
- **Model**: qwen3.8-27b NVFP4 (17GB)
- **Server**: ninfer with MTP speculative decoding (draft-tokens 5), int8 KV,
  host-KV-cache 36864 MiB, temperature 0.7, top-p 0.8, thinking enabled
- **Max concurrency**: 3 (default from ninfer-start.sh)

## Benchmark prompt

The benchmark uses a realistic coding conversation extracted from an actual
Claude Code session (`~/.claude/projects/.../5fb86527-...jsonl`). This replicates
the real agentic coding workload:

- **~135k tokens** of multi-turn conversation (system + user + assistant messages)
- **Tool definitions** (read_file, write_file, run_command) - triggers tool-call detection
- **Non-greedy sampling** (temperature=0.7, top_p=0.8) - stochastic, requires multiple runs
- **Thinking enabled** - reasoning channel state machine active
- **max_tokens=8000** - allows long generation but typically stops at tool_call

The prompt file is at `/tmp/bench_real.json` on the Mac.

### Why this prompt

Real coding sessions show 50-150 tok/s (not the 300+ of synthetic benchmarks).
The difference is:
1. Non-greedy sampling overhead (host-side RNG, sampling kernel)
2. Thinking channel state machine (per-token `</think>` detection, BPE detokenization)
3. Tool-call detection (stream filter scanning for `<tool_call>` markers)
4. Stop-string matching (substring search over decoded text buffer)
5. Large context (135k+ tokens) - longer attention, more KV cache pressure

Synthetic benchmarks (greedy, no tools, simple prompt) measure pure GPU decode
throughput (~305 tok/s) but don't capture the host-side overhead that dominates
real workloads.

## Running the benchmark

### Prerequisites

- Server running via `~/ninfer-start.sh --thinking` on strix
- Benchmark prompt file at `/tmp/bench_real.json` on the Mac

### Procedure

1. **Warm the cache**: Send the prompt once (cold run, ~40s). Discard the result.
2. **Run 5 warm-cache iterations**: Send the same prompt 5 times. The KV cache
   hits on the prefix, so prefill is ~0.5s and decode dominates.
3. **Record**: gen_tokens, wall_time, tok/s, finish_reason for each run.
4. **Filter**: Exclude runs where gen_tokens < 200 (too short to measure decode)
   or tok/s < 100 (anomalous).
5. **Report**: Mean and median tok/s of the remaining runs.

### Script

```bash
# Warm cache (discard result)
curl -s http://strix.lan:8080/v1/chat/completions \
    -H "Content-Type: application/json" \
    -d @/tmp/bench_real.json > /dev/null

# 5 warm runs
for i in 1 2 3 4 5; do
    START=$(python3 -c "import time; print(time.time())")
    curl -s --max-time 600 http://strix.lan:8080/v1/chat/completions \
        -H "Content-Type: application/json" \
        -d @/tmp/bench_real.json -o /tmp/bench_result_$i.json
    END=$(python3 -c "import time; print(time.time())")
    python3 -c "
import json
with open('/tmp/bench_result_$i.json') as f:
    d = json.load(f)
gen = d.get('usage', {}).get('completion_tokens', 0)
tps = gen / ($END - $START) if gen > 0 else 0
print(f'Run $i: gen={gen} tok/s={tps:.1f}')
"
done
```

### A/B testing between versions

1. Build and start version A (e.g., `git checkout a684b3a7 && cmake --build ...`)
2. Run the benchmark (1 warm + 5 measured)
3. Build and start version B
4. Run the benchmark (1 warm + 5 measured)
5. Compare medians

## Results: 3-way comparison (2026-08-29)

| Version | Commit | Median tok/s | Mean tok/s | Runs |
|---------|--------|-------------|-----------|------|
| Baseline | ff8c3089 | 144.8 | 145.9 | 4 |
| Phase 0 | a684b3a7 | 147.2 | 147.0 | 4 |
| Phase 3 | 25d85902 | 140.9 | 137.6 | 5 |

### Conclusions

- **Phase 0** (delete unnecessary sync 2): marginal improvement (+1.6%), within
  noise at this workload. The sync that was removed overlapped naturally with
  host work, so removing it doesn't help when host work is the bottleneck.
- **Phase 3** (device-side terminal check + pipelined executor): slight regression
  (-3.7%). The terminal_check kernel, event overhead, and larger egress struct
  add per-round cost that exceeds the overlap benefit at batch size 1.
- **At batch size 1 with warm cache, the bottleneck is the GPU graph (~8ms/round),
  not the sync.** The host work (~2ms preview + streaming) is already hidden
  inside the GPU time. Pipelining only helps when host work exceeds GPU time.

### When pipelining WOULD help

- **Higher batch sizes** (3+ concurrent decoders): GPU graph takes ~24ms (batch 3),
  host work takes ~6ms (3x preview). The overlap saves ~6ms.
- **Heavier host work**: stop-string matching on long decoded text, complex
  tool-call parsing, or multi-lane streaming.
- **Cold cache (full prefill)**: prefill syncs dominate, pipelining can't help.

## Generating the benchmark prompt

```python
import json

session_file = "~/.claude/projects/.../<session>.jsonl"
# Extract last ~100k tokens of real conversation
# Add tools and a coding prompt that triggers thinking + tool calls
# Save to /tmp/bench_real.json
```

See the session analysis script for extracting messages from Claude Code
session files.
