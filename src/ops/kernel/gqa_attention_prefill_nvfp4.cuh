#pragma once

// NVFP4 (4-bit E2M1 + E4M3 scale) group-16 KV cache fill and prefill attention
// kernels for the registered Qwen3.6 head geometries. QK stays NVFP4 through
// m16n8k64 block-scaled Tensor Cores (mma.sync.kind::mxf4nvf4); V alone is
// dequantized to BF16 with packed FP4 arithmetic while producer warps execute QK.
// Sixteen warps split each 16-row BF16 PV output across four 64-dimension slices.
//
// The structure mirrors gqa_attention_prefill_i8.cuh. Key differences:
//   - Group size 16 (not 64): 16 groups per head_dim=256
//   - Codes are packed E2M1 (4 bits, 2 per byte, 128 bytes per key)
//   - Scales are E4M3 bytes (1 per group, 16 per key)
//   - QK uses mma_nvfp4_e4m3 (m16n8k64) with built-in block scales (sfa/sfb)
//   - D=256 = 4 k-steps of k=64 (vs int8's 8 k-steps of k=32)
//   - No external scale multiplication: the MMA applies scales internally
//   - V dequant uses gqa_kv_nvfp4_dequant_e2m1x8_from (4 bytes -> 8 bf16)
//   - PV uses mma_bf16 (V is dequantized to BF16, not FP16)

#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <math_constants.h>

#include "ops/kernel/gqa_attention_kv_quant_nvfp4.cuh"
#include "ops/kernel/gqa_attention_prefill_common.cuh"

#include <cstdint>

namespace ninfer::ops {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr int kGqaPrefillNvfp4Warps   = 16;
inline constexpr int kGqaPrefillNvfp4Threads = kGqaPrefillNvfp4Warps * 32;
inline constexpr int kGqaPrefillNvfp4Br      = 64;
inline constexpr int kGqaPrefillNvfp4Bc      = 64;
inline constexpr int kGqaPrefillNvfp4Groups  = kGqaKvNvfp4HeadDim / kGqaKvNvfp4Group; // 16
inline constexpr int kGqaPrefillNvfp4DB16    = kGqaKvNvfp4HeadDim / 2 / 2;             // 64 b16 cols
inline constexpr int kGqaPrefillNvfp4RowTiles = kGqaPrefillNvfp4Br / 16;               // 4
inline constexpr int kGqaPrefillNvfp4DConsumers =
    kGqaPrefillNvfp4Warps / kGqaPrefillNvfp4RowTiles;                                 // 4

// SMEM byte sizes
inline constexpr int kGqaPrefillNvfp4QBytes =
    kGqaPrefillNvfp4Br * (kGqaKvNvfp4HeadDim / 2);                                    // 8192
inline constexpr int kGqaPrefillNvfp4QScaleBytes =
    kGqaPrefillNvfp4Br * kGqaPrefillNvfp4Groups;                                      // 1024
inline constexpr int kGqaPrefillNvfp4KBytes =
    kGqaPrefillNvfp4Bc * (kGqaKvNvfp4HeadDim / 2);                                    // 8192
inline constexpr int kGqaPrefillNvfp4KScaleBytes =
    kGqaPrefillNvfp4Bc * kGqaPrefillNvfp4Groups;                                      // 1024
inline constexpr int kGqaPrefillNvfp4VBytes =
    kGqaPrefillNvfp4Bc * (kGqaKvNvfp4HeadDim / 2);                                    // 8192
inline constexpr int kGqaPrefillNvfp4VScaleBytes =
    kGqaPrefillNvfp4Bc * kGqaPrefillNvfp4Groups;                                      // 1024
inline constexpr int kGqaPrefillNvfp4VStageBytes =
    kGqaPrefillNvfp4Bc * kGqaKvNvfp4HeadDim * static_cast<int>(sizeof(__nv_bfloat16)); // 32768
inline constexpr int kGqaPrefillNvfp4PBytes =
    kGqaPrefillNvfp4Br * kGqaPrefillNvfp4Bc * static_cast<int>(sizeof(__nv_bfloat16)); // 8192
inline constexpr int kGqaPrefillNvfp4StatsBytes =
    2 * kGqaPrefillNvfp4Br * static_cast<int>(sizeof(float));                          // 512

inline constexpr int kGqaPrefillNvfp4SmemBytes =
    kGqaPrefillNvfp4QBytes + kGqaPrefillNvfp4QScaleBytes + kGqaPrefillNvfp4KBytes +
    kGqaPrefillNvfp4KScaleBytes + kGqaPrefillNvfp4VBytes + kGqaPrefillNvfp4VScaleBytes +
    kGqaPrefillNvfp4VStageBytes + kGqaPrefillNvfp4PBytes + kGqaPrefillNvfp4StatsBytes;

static_assert(kGqaPrefillNvfp4Groups == 16);
static_assert(kGqaPrefillNvfp4DConsumers == 4);
static_assert(kGqaPrefillNvfp4SmemBytes == 69120);

// ---------------------------------------------------------------------------
// SMEM swizzle helpers (same XOR pattern as int8, adapted for NVFP4 b16 layout)
// ---------------------------------------------------------------------------

__device__ __forceinline__ void
gqa_prefill_nvfp4_store_code_swz(std::uint8_t* code_tile, int row, int group,
                                std::uint32_t codes_lo, std::uint32_t codes_hi) {
    // Each group of 16 E2M1 elements = 8 bytes = 4 b16 elements.
    // b16 column = group * 4. Swizzle at b16 granularity.
    const int b16_col = group * 4;
    const int swz_col = gqa_prefill_swz(row, b16_col);
    std::uint8_t* dst = code_tile + (row * kGqaPrefillNvfp4DB16 + swz_col) * 2;
    store_vec(dst, make_uint2(codes_lo, codes_hi));
}

// ---------------------------------------------------------------------------
// Fill kernels (prefill cache write)
// ---------------------------------------------------------------------------

// Small-append fill: one warp per (token, kv_head, group). Lane 0 calls the
// vectorized quantize_nvfp4_k16 with a direct source pointer (the bf16 K/V
// tensor is head_dim-contiguous per head per token).
template <typename Geometry, typename Metadata>
__launch_bounds__(256) __global__ void gqa_attention_prefill_fill_nvfp4_kernel(
    const __nv_bfloat16* __restrict__ k, const __nv_bfloat16* __restrict__ v,
    const std::int32_t* __restrict__ positions, Metadata metadata,
    std::uint8_t* __restrict__ cache_k, std::uint8_t* __restrict__ cache_v,
    std::uint8_t* __restrict__ scale_k, std::uint8_t* __restrict__ scale_v,
    std::int32_t width) {
    constexpr int Warps         = 8;
    constexpr unsigned FullMask = 0xffffffffu;
    const int tokens            = metadata.valid_tokens(width);
    const int warp              = static_cast<int>(threadIdx.x) >> 5;
    const int lane              = static_cast<int>(threadIdx.x) & 31;
    const int unit              = static_cast<int>(blockIdx.x) * Warps + warp;
    const int units             = tokens * Geometry::KVHeads * kGqaKvNvfp4Groups;
    if (unit >= units) { return; }

    const int group                 = unit % kGqaKvNvfp4Groups;
    const int tmp                   = unit / kGqaKvNvfp4Groups;
    const int kv_head               = tmp % Geometry::KVHeads;
    const int token                 = tmp / Geometry::KVHeads;
    const int position              = positions[0] + token;
    const std::int32_t* block_table = metadata.block_table();
    int page = lane == 0 ? paged_kv_physical_page(block_table, position) : 0;
    page     = __shfl_sync(FullMask, page, 0);

    if (lane == 0) {
        const int page_off = position & kPagedKVPageMask;
        // Code byte offset: each group of 16 elements occupies 8 packed bytes.
        const int byte_off = group * (kGqaKvNvfp4Group / 2);
        // Source: 16 consecutive bf16 values at dims [group*16, group*16+16).
        const int64_t src =
            gqa_kv_nvfp4_src_index<Geometry>(kv_head, group * kGqaKvNvfp4Group, token);

        const int64_t k_code_off =
            gqa_kv_nvfp4_code_index<Geometry>(page, kv_head, byte_off, page_off);
        const int64_t v_code_off =
            gqa_kv_nvfp4_code_index<Geometry>(page, kv_head, byte_off, page_off);
        const int64_t k_scale_off =
            gqa_kv_nvfp4_scale_index<Geometry>(page, kv_head, group, page_off);
        const int64_t v_scale_off =
            gqa_kv_nvfp4_scale_index<Geometry>(page, kv_head, group, page_off);

        gqa_kv_nvfp4_quantize_k16(&k[src], &cache_k[k_code_off], &scale_k[k_scale_off]);
        gqa_kv_nvfp4_quantize_k16(&v[src], &cache_v[v_code_off], &scale_v[v_scale_off]);
    }
}

// Large-append fill: scheduled in absolute eight-token tiles for page locality.
// Grid: (max_tiles, kv_heads, groups). Each warp handles one token for one group.
template <typename Geometry, typename Metadata>
__launch_bounds__(256) __global__ void gqa_attention_prefill_fill_nvfp4_page_kernel(
    const __nv_bfloat16* __restrict__ k, const __nv_bfloat16* __restrict__ v,
    const std::int32_t* __restrict__ positions, Metadata metadata,
    std::uint8_t* __restrict__ cache_k, std::uint8_t* __restrict__ cache_v,
    std::uint8_t* __restrict__ scale_k, std::uint8_t* __restrict__ scale_v,
    std::int32_t width) {
    constexpr int TokensPerTile = 8;
    constexpr unsigned FullMask = 0xffffffffu;
    const int tokens            = metadata.valid_tokens(width);
    const int warp              = static_cast<int>(threadIdx.x) >> 5;
    const int lane              = static_cast<int>(threadIdx.x) & 31;
    const int kv_head           = static_cast<int>(blockIdx.y);
    const int group             = static_cast<int>(blockIdx.z);
    const int tile_delta        = static_cast<int>(blockIdx.x);
    const int base_position     = positions[0];
    const int tile_position     = (base_position / TokensPerTile + tile_delta) * TokensPerTile;
    const int logical_page      = tile_position >> kPagedKVPageShift;
    const int token_begin       = max(0, tile_position - base_position);
    const int token_end         = min(tokens, tile_position + TokensPerTile - base_position);
    if (token_begin >= token_end) { return; }

    const std::int32_t* block_table = metadata.block_table();
    int physical_page               = lane == 0 ? block_table[logical_page] : 0;
    physical_page                   = __shfl_sync(FullMask, physical_page, 0);

    const int token  = token_begin + warp;
    const bool valid = token < token_end;
    if (!valid) { return; }

    if (lane == 0) {
        const int position = base_position + token;
        const int page_off = position & kPagedKVPageMask;
        const int byte_off = group * (kGqaKvNvfp4Group / 2);
        const int64_t src =
            gqa_kv_nvfp4_src_index<Geometry>(kv_head, group * kGqaKvNvfp4Group, token);

        const int64_t code_base =
            paged_kv_page_head_offset<kGqaKvNvfp4HeadDim / 2, Geometry::KVHeads>(
                physical_page, kv_head) +
            static_cast<int64_t>(page_off) * (kGqaKvNvfp4HeadDim / 2) + byte_off;

        const int64_t scale_base =
            paged_kv_page_head_offset<kGqaKvNvfp4Groups, Geometry::KVHeads>(physical_page,
                                                                             kv_head) +
            static_cast<int64_t>(page_off) * kGqaKvNvfp4Groups + group;

        gqa_kv_nvfp4_quantize_k16(&k[src], &cache_k[code_base], &scale_k[scale_base]);
        gqa_kv_nvfp4_quantize_k16(&v[src], &cache_v[code_base], &scale_v[scale_base]);
    }
}

// ---------------------------------------------------------------------------
// Prefill attention kernel
// ---------------------------------------------------------------------------

template <typename Geometry, typename Metadata>
__global__ __maxnreg__(120) void gqa_attention_prefill_nvfp4_kernel(
    const __nv_bfloat16* __restrict__ q, const std::uint8_t* __restrict__ cache_k,
    const std::uint8_t* __restrict__ cache_v, const std::uint8_t* __restrict__ cache_k_scale,
    const std::uint8_t* __restrict__ cache_v_scale, Metadata metadata,
    const std::int32_t* __restrict__ positions, float scale, __nv_bfloat16* __restrict__ out,
    std::int32_t width) {
    constexpr int D             = kGqaKvNvfp4HeadDim;   // 256
    constexpr int Br            = kGqaPrefillNvfp4Br;   // 64
    constexpr int Bc            = kGqaPrefillNvfp4Bc;   // 64
    constexpr int DB16          = kGqaPrefillNvfp4DB16;  // 64
    constexpr int Groups        = kGqaPrefillNvfp4Groups; // 16
    constexpr int KKSteps       = D / 64;                // 4 (k=64 per MMA step)
    constexpr int GroupsPerKStep = 64 / kGqaKvNvfp4Group; // 4
    constexpr int QKNt          = Bc / 8;                // 8
    constexpr int PVNtPerWarp   = D / (kGqaPrefillNvfp4DConsumers * 8); // 8
    constexpr int PVKs          = Bc / 16;               // 4
    constexpr int ProducerWarps = kGqaPrefillNvfp4RowTiles; // 4
    constexpr int VWorkerWarps  = kGqaPrefillNvfp4Warps - ProducerWarps; // 12
    constexpr int WorkerThreads = VWorkerWarps * 32;     // 384
    constexpr float Log2E       = 1.4426950408889634074f;
    constexpr unsigned FullMask = 0xffffffffu;

    static_assert(PVNtPerWarp == 8);
    static_assert(KKSteps == 4);
    static_assert(GroupsPerKStep == 4);

    extern __shared__ __align__(16) unsigned char smem_raw[];
    // Q codes (packed E2M1, swizzled b16 layout)
    std::uint8_t* q_codes = reinterpret_cast<std::uint8_t*>(smem_raw);
    // Q scales (E4M3 bytes, natural row-major: [row * Groups + group])
    std::uint8_t* q_scale_s = q_codes + kGqaPrefillNvfp4QBytes;
    // K codes (packed E2M1, swizzled b16 layout)
    std::uint8_t* k_codes = q_scale_s + kGqaPrefillNvfp4QScaleBytes;
    // K scales (E4M3 bytes, natural: [key * Groups + group])
    std::uint8_t* k_scale_s = k_codes + kGqaPrefillNvfp4KBytes;
    // V codes (packed E2M1, natural layout for dequant)
    std::uint8_t* v_codes = k_scale_s + kGqaPrefillNvfp4KScaleBytes;
    // V scales (E4M3 bytes, natural: [key * Groups + group])
    std::uint8_t* v_scale_s = v_codes + kGqaPrefillNvfp4VBytes;
    // V dequant (bf16, swizzled for ldmatrix)
    __nv_bfloat16* v_bf16 =
        reinterpret_cast<__nv_bfloat16*>(v_scale_s + kGqaPrefillNvfp4VScaleBytes);
    // P (bf16, swizzled for ldmatrix)
    __nv_bfloat16* p_s =
        reinterpret_cast<__nv_bfloat16*>(reinterpret_cast<unsigned char*>(v_bf16) +
                                          kGqaPrefillNvfp4VStageBytes);
    // Stats: alpha (fp32, Br) + final_l (fp32, Br)
    float* alpha_s = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(p_s) +
                                              kGqaPrefillNvfp4PBytes);
    float* final_l_s = alpha_s + Br;

    // b16 reinterpretations for ldmatrix
    __nv_bfloat16* q_bf16 = reinterpret_cast<__nv_bfloat16*>(q_codes);
    __nv_bfloat16* k_bf16 = reinterpret_cast<__nv_bfloat16*>(k_codes);

    const int q_block = static_cast<int>(blockIdx.x);
    const int q_head  = static_cast<int>(blockIdx.y);
    const int tid     = static_cast<int>(threadIdx.x);
    const int warp    = tid >> 5;
    const int lane    = tid & 31;
    const int q0      = q_block * Br;
    const int kv_head = q_head / Geometry::GroupSize;
    const int tokens  = metadata.valid_tokens(width);
    if (q_head >= Geometry::QHeads || q0 >= width) { return; }
    if (q0 >= tokens) {
        gqa_prefill_zero_output_rows<Geometry>(out, q_head, q0, min(q0 + Br, width), tid,
                                               kGqaPrefillNvfp4Threads);
        return;
    }
    const int base_pos              = positions[0];
    const std::int32_t* block_table = metadata.block_table();

    const int tile_rows     = min(Br, tokens - q0);
    const int max_query_abs = base_pos + q0 + tile_rows - 1;
    const int key_blocks    = max_query_abs / Bc + 1;

    // ---- Q quantization: bf16 -> NVFP4 (E2M1 codes + E4M3 scales) ----
    // Each thread handles one (row, group) independently.
    for (int unit = tid; unit < Br * Groups; unit += kGqaPrefillNvfp4Threads) {
        const int row = unit / Groups;
        const int grp = unit - row * Groups;
        const int64_t src =
            gqa_prefill_q_index<Geometry>(q_head, grp * kGqaKvNvfp4Group, q0 + row);
        const detail::Nvfp4QuantizedK16 result =
            detail::quantize_nvfp4_k16(&q[src], 1.0f);
        gqa_prefill_nvfp4_store_code_swz(q_codes, row, grp, result.codes_lo, result.codes_hi);
        q_scale_s[row * Groups + grp] = result.scale;
    }
    __syncthreads();

    // ---- KV tile staging lambda ----
    auto issue_kv_tile = [&](int tile_k0) {
        const int physical_page = block_table[tile_k0 >> kPagedKVPageShift];
        // Stage K and V scales: 16 E4M3 bytes per key, one cp_async<16>.
        for (int key_l = tid; key_l < Bc; key_l += kGqaPrefillNvfp4Threads) {
            const int key = tile_k0 + key_l;
            std::uint8_t* kd = &k_scale_s[key_l * Groups];
            std::uint8_t* vd = &v_scale_s[key_l * Groups];
            if (key <= max_query_abs) {
                const int64_t off =
                    gqa_kv_nvfp4_scale_index<Geometry>(physical_page, kv_head, 0, key_l);
                cp_async<16>(kd, &cache_k_scale[off]);
                cp_async<16>(vd, &cache_v_scale[off]);
            } else {
                store_vec(kd, make_uint4(0, 0, 0, 0));
                store_vec(vd, make_uint4(0, 0, 0, 0));
            }
        }
        // Stage K and V codes: 128 bytes per key, 8 chunks of 16 bytes.
        // K uses swizzled b16 layout; V uses natural layout.
#pragma unroll 1
        for (int chunk = tid; chunk < Bc * (D / 2 / 16); chunk += kGqaPrefillNvfp4Threads) {
            const int key_l = chunk / (D / 2 / 16);
            const int dc    = chunk - key_l * (D / 2 / 16);
            const int d     = dc * 16; // byte offset within row
            const int key   = tile_k0 + key_l;
            // K: swizzled b16 layout (same as int8)
            std::uint8_t* kd = &k_codes[(key_l * DB16 + gqa_prefill_swz(key_l, dc * 8)) * 2];
            // V: natural byte layout
            std::uint8_t* vd = &v_codes[key_l * (D / 2) + d];
            if (key <= max_query_abs) {
                const int64_t off =
                    gqa_kv_nvfp4_code_index<Geometry>(physical_page, kv_head, d, key_l);
                cp_async<16, Cache::cg>(kd, &cache_k[off]);
                cp_async<16, Cache::cg>(vd, &cache_v[off]);
            } else {
                store_vec(kd, make_int4(0, 0, 0, 0));
                store_vec(vd, make_int4(0, 0, 0, 0));
            }
        }
        cp_commit();
    };

    issue_kv_tile(0);
    cp_wait<0>();
    __syncthreads();

    // ---- Lane mappings for ldmatrix (same as int8) ----
    const int gid      = lane >> 2;
    const int lid      = lane & 3;
    const int a_mat    = lane >> 3;
    const int a_rin    = lane & 7;
    const int a_rowoff = a_rin + ((a_mat & 1) << 3);
    const int a_coloff = (a_mat >> 1) << 3;
    const int b_rin    = lane & 7;
    const int b_koff   = ((lane >> 3) & 1) << 3;

    // NVFP4 scale row mappings (from nvfp4_w4a4_mma.cuh:245-246)
    const int sfa_row = ((lane & 1) << 3) | (lane >> 2);
    const int sfb_row = lane >> 2;

    // ---- Accumulators and online softmax state ----
    float acc[PVNtPerWarp][4];
#pragma unroll
    for (int n = 0; n < PVNtPerWarp; ++n) {
#pragma unroll
        for (int i = 0; i < 4; ++i) { acc[n][i] = 0.0f; }
    }
    float running_m0     = -CUDART_INF_F;
    float running_m1     = -CUDART_INF_F;
    float running_l0     = 0.0f;
    float running_l1     = 0.0f;
    const float scale_l2 = scale * Log2E;

    // ---- Main loop over key tiles ----
    for (int kb = 0; kb < key_blocks; ++kb) {
        const int k0 = kb * Bc;
        if (warp < ProducerWarps) {
            // ---- Producer warps: QK computation + online softmax ----
            const int row_base = warp * 16;
            float score[QKNt][4];
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                score[nt][0] = score[nt][1] = score[nt][2] = score[nt][3] = 0.0f;
            }

            // D=256 = 4 k-steps of k=64. Each k-step covers 4 groups of 16.
#pragma unroll
            for (int ks = 0; ks < KKSteps; ++ks) {
                // Load A (Q) fragment: 4 8x8 b16 matrices via ldmatrix_x4.
                // Column = ks * 16 + a_coloff (b16 units).
                unsigned af[4];
                const int acol = ks * 16 + a_coloff;
                ldmatrix_x4(af[0], af[1], af[2], af[3],
                            smem_addr(&q_bf16[(row_base + a_rowoff) * DB16 +
                                             gqa_prefill_swz(row_base + a_rowoff, acol)]));

                // Load Q scales (sfa): 4 E4M3 bytes packed as uint32.
                // One per group, 4 groups per k=64 step.
                const unsigned sfa = load_vec<unsigned>(
                    &q_scale_s[(row_base + sfa_row) * Groups + ks * GroupsPerKStep]);

#pragma unroll
                for (int nt = 0; nt < QKNt; ++nt) {
                    // Load B (K) fragment: 2 8x8 b16 matrices via ldmatrix_x2.
                    const int brow = nt * 8 + b_rin;
                    const int bcol = ks * 16 + b_koff;
                    unsigned bf[2];
                    ldmatrix_x2(bf[0], bf[1],
                                smem_addr(&k_bf16[brow * DB16 + gqa_prefill_swz(brow, bcol)]));

                    // Load K scales (sfb): 4 E4M3 bytes for this key and k-step.
                    const int sfb_key = nt * 8 + sfb_row;
                    const unsigned sfb = load_vec<unsigned>(
                        &k_scale_s[sfb_key * Groups + ks * GroupsPerKStep]);

                    // mma_nvfp4_e4m3: m16n8k64 with built-in block scales.
                    // The accumulator already includes the scale factoring.
                    mma_nvfp4_e4m3(score[nt][0], score[nt][1], score[nt][2], score[nt][3],
                                   af[0], af[1], af[2], af[3], bf[0], bf[1], sfa, sfb);
                }
            }

            // ---- Online softmax (same structure as int8, no external scale multiply) ----
            const int row0             = row_base + gid;
            const int row1             = row0 + 8;
            const int qabs0            = row0 < tile_rows ? base_pos + q0 + row0 : -1;
            const int qabs1            = row1 < tile_rows ? base_pos + q0 + row1 : -1;
            const bool full_score_tile = q0 + Br <= tokens && k0 + Bc - 1 <= base_pos + q0;
            float bm0                  = -CUDART_INF_F;
            float bm1                  = -CUDART_INF_F;
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                const int key0 = k0 + nt * 8 + 2 * lid;
                const int key1 = key0 + 1;
                if (!full_score_tile) {
                    score[nt][0] = key0 <= qabs0 ? score[nt][0] : -CUDART_INF_F;
                    score[nt][1] = key1 <= qabs0 ? score[nt][1] : -CUDART_INF_F;
                    score[nt][2] = key0 <= qabs1 ? score[nt][2] : -CUDART_INF_F;
                    score[nt][3] = key1 <= qabs1 ? score[nt][3] : -CUDART_INF_F;
                }
                bm0 = fmaxf(bm0, fmaxf(score[nt][0], score[nt][1]));
                bm1 = fmaxf(bm1, fmaxf(score[nt][2], score[nt][3]));
            }
            bm0 = warp_max<4>(bm0, FullMask);
            bm1 = warp_max<4>(bm1, FullMask);

            const float nm0        = fmaxf(running_m0, bm0);
            const float nm1        = fmaxf(running_m1, bm1);
            const float nm0_scaled = nm0 * scale_l2;
            const float nm1_scaled = nm1 * scale_l2;
            const float alpha0     = running_m0 == -CUDART_INF_F
                                         ? 0.0f
                                         : exp2_approx(__fmaf_rn(running_m0, scale_l2, -nm0_scaled));
            const float alpha1     = running_m1 == -CUDART_INF_F
                                         ? 0.0f
                                         : exp2_approx(__fmaf_rn(running_m1, scale_l2, -nm1_scaled));
            float bl0              = 0.0f;
            float bl1              = 0.0f;
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                const int col0  = nt * 8 + 2 * lid;
                const int col1  = col0 + 1;
                const float p00 = score[nt][0] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][0], scale_l2, -nm0_scaled))
                                      : 0.0f;
                const float p01 = score[nt][1] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][1], scale_l2, -nm0_scaled))
                                      : 0.0f;
                const float p10 = score[nt][2] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][2], scale_l2, -nm1_scaled))
                                      : 0.0f;
                const float p11 = score[nt][3] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][3], scale_l2, -nm1_scaled))
                                      : 0.0f;
                bl0 += p00 + p01;
                bl1 += p10 + p11;
                p_s[row0 * Bc + gqa_prefill_swz(row0, col0)] = __float2bfloat16_rn(p00);
                p_s[row0 * Bc + gqa_prefill_swz(row0, col1)] = __float2bfloat16_rn(p01);
                p_s[row1 * Bc + gqa_prefill_swz(row1, col0)] = __float2bfloat16_rn(p10);
                p_s[row1 * Bc + gqa_prefill_swz(row1, col1)] = __float2bfloat16_rn(p11);
            }
            bl0        = warp_sum<4>(bl0, FullMask);
            bl1        = warp_sum<4>(bl1, FullMask);
            running_l0 = __fmaf_rn(running_l0, alpha0, bl0);
            running_l1 = __fmaf_rn(running_l1, alpha1, bl1);
            running_m0 = nm0;
            running_m1 = nm1;
            if (lid == 0) {
                alpha_s[row0] = alpha0;
                alpha_s[row1] = alpha1;
            }
        } else if (warp < ProducerWarps + VWorkerWarps) {
            // ---- V dequant: consumer warps dequantize V from E2M1 to bf16 ----
            const int worker_tid = tid - ProducerWarps * 32;
#pragma unroll 1
            for (int chunk = worker_tid; chunk < Bc * (D / 8); chunk += WorkerThreads) {
                const int key_l = chunk / (D / 8);
                const int dc    = chunk - key_l * (D / 8);
                const int d     = dc * 8;
                const int key    = k0 + key_l;
                __nv_bfloat16* dst = &v_bf16[key_l * D + gqa_prefill_swz(key_l, d)];
                if (key <= max_query_abs) {
                    const int grp     = d >> 4; // d / 16
                    const std::uint8_t scale_byte = v_scale_s[key_l * Groups + grp];
                    store_vec(dst, gqa_kv_nvfp4_dequant_e2m1x8_from(
                                       &v_codes[key_l * (D / 2) + d / 2], scale_byte));
                } else {
                    store_vec(dst, make_int4(0, 0, 0, 0));
                }
            }
        }
        __syncthreads();

        // ---- Issue next KV tile while computing PV ----
        const bool has_next = kb + 1 < key_blocks;
        if (has_next) { issue_kv_tile((kb + 1) * Bc); }

        // ---- PV: all warps participate ----
        const int row_tile = warp % kGqaPrefillNvfp4RowTiles;
        const int d_slice  = warp / kGqaPrefillNvfp4RowTiles;
        const int row_base = row_tile * 16;
        const float alpha0 = alpha_s[row_base + gid];
        const float alpha1 = alpha_s[row_base + gid + 8];
#pragma unroll
        for (int n = 0; n < PVNtPerWarp; ++n) {
            acc[n][0] *= alpha0;
            acc[n][1] *= alpha0;
            acc[n][2] *= alpha1;
            acc[n][3] *= alpha1;
        }

#pragma unroll
        for (int k = 0; k < PVKs; ++k) {
            unsigned pf[4];
            const int pcol = k * 16 + a_coloff;
            ldmatrix_x4(pf[0], pf[1], pf[2], pf[3],
                        smem_addr(&p_s[(row_base + a_rowoff) * Bc +
                                       gqa_prefill_swz(row_base + a_rowoff, pcol)]));
#pragma unroll
            for (int n = 0; n < PVNtPerWarp; ++n) {
                const int global_n = d_slice * PVNtPerWarp + n;
                unsigned vf[2];
                const int vrow = k * 16 + b_koff + b_rin;
                const int vcol = global_n * 8;
                ldmatrix_x2_t(vf[0], vf[1],
                              smem_addr(&v_bf16[vrow * D + gqa_prefill_swz(vrow, vcol)]));
                mma_bf16(acc[n][0], acc[n][1], acc[n][2], acc[n][3], pf[0], pf[1], pf[2], pf[3],
                         vf[0], vf[1]);
            }
        }
        if (has_next) { cp_wait<0>(); }
        __syncthreads();
    }

    // ---- Output: normalize by final logsumexp and write bf16 ----
    if (warp < ProducerWarps && lid == 0) {
        const int row0  = warp * 16 + gid;
        const int row1  = row0 + 8;
        final_l_s[row0] = running_l0;
        final_l_s[row1] = running_l1;
    }
    __syncthreads();

    const int row_tile = warp % kGqaPrefillNvfp4RowTiles;
    const int d_slice  = warp / kGqaPrefillNvfp4RowTiles;
    const int row_base = row_tile * 16;
    const int row0     = row_base + gid;
    const int row1     = row0 + 8;
    const float inv_l0 = final_l_s[row0] > 0.0f ? __frcp_rn(final_l_s[row0]) : 0.0f;
    const float inv_l1 = final_l_s[row1] > 0.0f ? __frcp_rn(final_l_s[row1]) : 0.0f;
#pragma unroll
    for (int n = 0; n < PVNtPerWarp; ++n) {
        const int d0 = (d_slice * PVNtPerWarp + n) * 8 + 2 * lid;
        if (row0 < tile_rows) {
            *reinterpret_cast<unsigned*>(
                &out[gqa_prefill_q_index<Geometry>(q_head, d0, q0 + row0)]) =
                pack_bf16x2(acc[n][0] * inv_l0, acc[n][1] * inv_l0);
        }
        if (row1 < tile_rows) {
            *reinterpret_cast<unsigned*>(
                &out[gqa_prefill_q_index<Geometry>(q_head, d0, q0 + row1)]) =
                pack_bf16x2(acc[n][2] * inv_l1, acc[n][3] * inv_l1);
        }
    }
    gqa_prefill_zero_output_rows<Geometry>(out, q_head, tokens, min(q0 + Br, width), tid,
                                           kGqaPrefillNvfp4Threads);
}

} // namespace ninfer::ops
