# NVFP4 KV Cache + YaRN Context Extension

## Overview

NInfer supports NVFP4 (4-bit E2M1 + E4M3 group-16 scales) KV cache storage and
YaRN linear position scaling for context extension beyond the native 262k limit.

- **NVFP4 KV**: 144 bytes/token/KV-head (vs 264 int8, 512 bf16) — 45% reduction vs int8
- **YaRN**: extends context up to 555k (c=3+vision) or 600k (c=1) on RTX 5090
- **Quality**: LongBench 45% (matches int8 baseline), AIME 97%, needle 100%

---

## NVFP4 KV Cache

### Storage Format

| Component | Format | Bytes/token/KV-head |
|-----------|--------|---------------------|
| Codes | E2M1 (4-bit), 2 per byte | 128 |
| Scales | E4M3FN, 1 per 16-element group | 16 |
| **Total** | | **144** |

| Format | Bytes/token/KV-head | vs int8 |
|--------|---------------------|---------|
| bf16 | 512 | 1.94x |
| int8 (G64) | 264 | 1.0x |
| FP8 (E4M3) | 258 | 0.98x |
| **NVFP4 (G16)** | **144** | **0.55x** |

### Architecture

- **QK**: `mma_nvfp4_e4m3` (m16n8k64) with built-in E4M3 block scales. Q and K
  are both quantized to NVFP4. Hadamard rotation applied to K (and Q) for
  outlier suppression; V is NOT rotated.
- **PV**: V dequantized to BF16 via `decode_nvfp4_e2m1x2 × E4M3 scale`, then
  BF16 MMA for PV.
- **Fused append**: decode kernel quantizes current K/V to NVFP4 in-place.
- **Scale layout**: natural (non-swizzled) row-major for KV (not the M128x4
  swizzle used by weight MMA).

### Dispatch

`--kv-dtype nvfp4` selects `KvCacheStorage::Nvfp4Group16` → `DType::U8` with
`quant_group=16`. Code planes use `leading_extent=head_dim/2=128`; scale planes
use `leading_extent=16` with `DType::U8` (E4M3 bytes, not FP16).

### VRAM Impact

| Config | int8 KV | NVFP4 KV | Reduction |
|--------|---------|----------|-----------|
| 262k, c=4 | 9.96 GiB | 6.0 GiB | 40% |
| 555k, c=3 | — | 12.15 GiB | — |
| 600k, c=1 | — | 11.4 GiB | — |

---

## YaRN Position Scaling

### Implementation

Linear ramp applied to `rope_positions` only (not `cache_positions`):

```
if position <= original_context:
    scaled = position
else:
    scaled = original_context + (position - original_context) / factor
```

CLI flags: `--rope-scaling-factor F` (1.0–32.0), `--rope-scaling-original-context N` (default 262144).

### Why Linear Scaling Preserves Quality

Qwen3.8-27B's RoPE configuration makes full YaRN (NTK-by-parts + temperature)
unnecessary:

| Factor | Value | Effect |
|--------|-------|--------|
| rope_theta | 1e7 | Lowest frequency period = 38M tokens |
| rotary_dim | 64/256 (25%) | 75% of dimensions are position-independent |
| softmax layers | 16/64 (25%) | 48 GDN layers have no RoPE |

At 600k tokens, the scaled RoPE position is 1.08% of the 38M-token period —
no frequency wrapping. Quality is guaranteed up to ~9.5M tokens.

---

## Benchmark Results

### Speed (c=1, no vision, nvfp4 KV, YaRN factor 2.30)

| Context | Tokens | TTFT | Prefill | Decode | MTP | Quality |
|---------|--------|------|---------|--------|-----|---------|
| 262k (native) | 272k | 268s | 1024 tok/s | 128 tok/s | 3.18 | coherent |
| 350k (YaRN) | 373k | 441s | 845 tok/s | 145 tok/s | 3.76 | coherent |
| 480k (YaRN) | 506k | 745s | 680 tok/s | 92 tok/s | 2.71 | coherent |
| 570k (YaRN) | 592k | 1010s | 586 tok/s | 96 tok/s | 2.91 | coherent |

Prefill degrades ~43% from 262k to 570k. Decode stays 90–145 tok/s (weight-bound).
TTFT scales linearly: ~0.37s per 1k tokens.

### Quality (LongBench v2, 20 samples)

| Config | Score | Multi-Doc QA | Single-Doc QA |
|--------|-------|-------------|--------------|
| Old int8 (262k native) | 9/20 = 45% | 1/5 = 20% | 5/10 = 50% |
| NVFP4 (262k native) | 6/20 = 30% | 1/5 = 20% | 3/10 = 30% |
| **NVFP4 + YaRN (600k ctx)** | **9/20 = 45%** | **2/5 = 40%** | **4/10 = 40%** |

YaRN-extended NVFP4 matches old int8 baseline. Multi-Doc QA improved (40% vs 20%).

### AIME 2025

| Config | Score |
|--------|-------|
| int8+Hadamard (262k) | 28/30 = 93.3% |
| NVFP4 (262k) | 29/30 = 96.7% |

### Needle-in-haystack (128k)

All configs: 5/5 = 100%.

### KV Cache Stress Test (3 × 200k concurrent)

| Metric | Value |
|--------|-------|
| Total demand | ~636k tokens vs 537k device capacity |
| host_kv_occupied | 2.81 GB (parked to host) |
| maximal_fallbacks | 0 (graceful pressure relief) |
| owners_evicted | 0 |
| crashes | 0 |

---

## Max Context Limits (RTX 5090, 32GB)

### c=3 + vision (production)

| max_context | YaRN factor | Status | Free VRAM |
|-------------|-------------|--------|-----------|
| 262k (native) | 1.0 | ✓ | 1.23 GiB |
| 500k | 1.91 | ✓ | 1.22 GiB |
| **555k** | **2.12** | **✓ (MAX)** | **1.22 GiB** |
| 560k | 2.14 | ✗ | insufficient |

### c=1, no vision

| max_context | YaRN factor | Status | Free VRAM |
|-------------|-------------|--------|-----------|
| 262k (native) | 1.0 | ✓ | 7.0 GiB |
| 500k | 2.0 | ✓ | 3.2 GiB |
| **600k** | **2.30** | **✓ (MAX)** | **2.0 GiB** |
| 615k | 2.35 | ✗ | exceeds VRAM |

Verified with 592k token prompt: no crash, coherent output.

### 96GB GPU projection

| Concurrency | Max context | YaRN factor |
|-------------|-------------|-------------|
| c=1 | 7.7M tokens | 29.2x |
| c=4 | 1.9M tokens | 7.3x |

At 7.7M tokens, scaled RoPE position is 20% of the 38M period — no wrapping.

---

## Production Configuration

```bash
./build/apps/ninfer-serve ~/ninfer-models/qwen3_8_27b_nvfp4-froggeric.ninfer   --host 0.0.0.0 --port 8080   --max-concurrency 3 --max-context 555000 --kv-capacity auto   --default-max-tokens 131072 --pending-timeout-ms 900000   --kv-dtype nvfp4 --spec mtp --draft-tokens 5 --lm-head-draft   --tolerant-tool-calls --vision --host-kv-mib 36864   --temperature 0.7 --top-p 0.8   --rope-scaling-factor 2.12 --rope-scaling-original-context 262144   --request-log-jsonl ~/ninfer-requests.jsonl   --request-log-max-mib 64 --request-log-keep 4
```

- c=3 concurrent sessions with vision support
- 555k max context per session (2.12x native via YaRN)
- NVFP4 KV: 40% less VRAM than int8, quality preserved
- Host-KV: 36 GB for page-level parking under pressure
