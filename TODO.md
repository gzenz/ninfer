# TODO — decode speedup experiments for the ninfer use case

See the full file in the repo; quick reference:

1. Batch occupancy (config, no code) — in progress
2. MTP draft-tokens 4->5 — flag flip, 30-min measurement, +5-15% if pos-4 accepts
3. Decode roofline profiling (nsys capture first) — find the missing 52%, +10-20%
4. W4A4 decode kernels — up to 2x, weeks of work, microbenchmark the ceiling first
5. GDN state bf16 — +4-6%, rider on next restart, eval-gated

Ground rule: judge everything by accepted_tokens/decode_seconds from the
request log; 30-60 min real-workload samples; exclude >10k-round requests.
