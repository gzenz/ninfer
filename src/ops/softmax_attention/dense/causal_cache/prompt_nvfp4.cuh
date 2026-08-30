#pragma once

// NVFP4 (E2M1 + E4M3 group-16 scale) KV-cache causal prompt kernel. Q and cached K
// use the same fixed D256 rotation, native E2M1 K64 Tensor Cores via
// mma.sync.kind::mxf4nvf4 with built-in E4M3 block scales, and FP32 dot-product
// accumulation. V codes dequantize to BF16 and feed BF16/FP32 PV MMA.

#include "ops/kv_cache/nvfp4_g16_codec.cuh"
#include "ops/kv_cache/hadamard_d256.cuh"
#include "ops/softmax_attention/dense/causal_cache/prompt_common.cuh"

#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <math_constants.h>

#include <cstdint>

namespace ninfer::ops {

inline constexpr int kCausalPromptNvfp4Warps         = 16;
inline constexpr int kCausalPromptNvfp4Threads       = kCausalPromptNvfp4Warps * 32;
inline constexpr int kCausalPromptNvfp4Br            = 64;
inline constexpr int kCausalPromptNvfp4Bc            = 64;
inline constexpr int kCausalPromptNvfp4DB16          = kCausalPromptHeadDim / 4;
inline constexpr int kCausalPromptNvfp4Groups        = kKVCacheNvfp4Groups;
inline constexpr int kCausalPromptNvfp4RowTiles      = kCausalPromptNvfp4Br / 16;
inline constexpr int kCausalPromptNvfp4ProducerWarps = 2 * kCausalPromptNvfp4RowTiles;
inline constexpr int kCausalPromptNvfp4DConsumers = kCausalPromptNvfp4Warps / kCausalPromptNvfp4RowTiles;

inline constexpr int kCausalPromptNvfp4QBytes = kCausalPromptNvfp4Br * (kCausalPromptHeadDim / 2);
inline constexpr int kCausalPromptNvfp4QScaleBytes = kCausalPromptNvfp4Br * kCausalPromptNvfp4Groups;
inline constexpr int kCausalPromptNvfp4KBytes = kCausalPromptNvfp4Bc * (kCausalPromptHeadDim / 2);
inline constexpr int kCausalPromptNvfp4KScaleBytes = kCausalPromptNvfp4Bc * kCausalPromptNvfp4Groups;
inline constexpr int kCausalPromptNvfp4VBytes = kCausalPromptNvfp4Bc * (kCausalPromptHeadDim / 2);
inline constexpr int kCausalPromptNvfp4VScaleBytes = kCausalPromptNvfp4Bc * kCausalPromptNvfp4Groups;
inline constexpr int kCausalPromptNvfp4VStageBytes =
    kCausalPromptNvfp4Bc * kCausalPromptHeadDim * static_cast<int>(sizeof(__nv_bfloat16));
inline constexpr int kCausalPromptNvfp4PBytes =
    kCausalPromptNvfp4Br * kCausalPromptNvfp4Bc * static_cast<int>(sizeof(__nv_bfloat16));
inline constexpr int kCausalPromptNvfp4StatsBytes =
    7 * kCausalPromptNvfp4Br * static_cast<int>(sizeof(float));
inline constexpr int kCausalPromptNvfp4SmemBytes =
    kCausalPromptNvfp4QBytes + kCausalPromptNvfp4QScaleBytes + kCausalPromptNvfp4KBytes +
    kCausalPromptNvfp4KScaleBytes + kCausalPromptNvfp4VBytes + kCausalPromptNvfp4VScaleBytes +
    kCausalPromptNvfp4VStageBytes + kCausalPromptNvfp4PBytes + kCausalPromptNvfp4StatsBytes;

static_assert(kCausalPromptNvfp4DConsumers == 4);
static_assert(kCausalPromptNvfp4SmemBytes == 70400);

__device__ __forceinline__ void causal_prompt_nvfp4_store_code_swz(
    std::uint8_t* code_tile, int row, int group, std::uint32_t codes_lo,
    std::uint32_t codes_hi) {
    const int b16_col = group * 4;
    const int swz_col = causal_prompt_swz(row, b16_col);
    std::uint8_t* dst = code_tile + (row * kCausalPromptNvfp4DB16 + swz_col) * 2;
    store_vec(dst, make_uint2(codes_lo, codes_hi));
}

__device__ __forceinline__ int causal_prompt_nvfp4_p_swz(int row, int col) {
    if constexpr (kCausalPromptNvfp4Bc == 32) { return (((col >> 3) ^ (row & 3)) << 3) | (col & 7); }
    return causal_prompt_swz(row, col);
}

template <typename Geometry, typename Metadata>
__global__ __maxnreg__(120) void causal_attention_prompt_nvfp4_kernel(
    const __nv_bfloat16* __restrict__ q, const std::uint8_t* __restrict__ cache_k,
    const std::uint8_t* __restrict__ cache_v, const std::uint8_t* __restrict__ cache_k_scale,
    const std::uint8_t* __restrict__ cache_v_scale, Metadata metadata,
    const std::int32_t* __restrict__ positions, float scale, __nv_bfloat16* __restrict__ out,
    std::int32_t width) {
    constexpr int D             = kCausalPromptHeadDim;
    constexpr int Br            = kCausalPromptNvfp4Br;
    constexpr int Bc            = kCausalPromptNvfp4Bc;
    constexpr int DB16          = kCausalPromptNvfp4DB16;
    constexpr int Groups        = kCausalPromptNvfp4Groups;
    constexpr int QKKs          = D / 64;
    constexpr int GroupsPerKStep = 64 / kKVCacheNvfp4Group;
    constexpr int QKNt          = (Bc / 2) / 8;
    constexpr int PVNtPerWarp   = D / (kCausalPromptNvfp4DConsumers * 8);
    constexpr int PVKs          = Bc / 16;
    constexpr int ProducerWarps = kCausalPromptNvfp4ProducerWarps;
    constexpr int VWorkerWarps  = kCausalPromptNvfp4Warps - ProducerWarps;
    constexpr int WorkerThreads = VWorkerWarps * 32;
    constexpr float Log2E       = 1.4426950408889634074f;
    constexpr unsigned FullMask = 0xffffffffU;
    static_assert(QKKs == 4);
    static_assert(PVNtPerWarp == 8);
    static_assert(GroupsPerKStep == 4);

    extern __shared__ __align__(16) unsigned char smem_raw[];
    std::uint8_t* q_codes = reinterpret_cast<std::uint8_t*>(smem_raw);
    std::uint8_t* q_scale_s = q_codes + kCausalPromptNvfp4QBytes;
    std::uint8_t* k_codes = q_scale_s + kCausalPromptNvfp4QScaleBytes;
    std::uint8_t* k_scale_s = k_codes + kCausalPromptNvfp4KBytes;
    std::uint8_t* v_codes = k_scale_s + kCausalPromptNvfp4KScaleBytes;
    std::uint8_t* v_scale_s = v_codes + kCausalPromptNvfp4VBytes;
    __nv_bfloat16* v_bf16 =
        reinterpret_cast<__nv_bfloat16*>(v_scale_s + kCausalPromptNvfp4VScaleBytes);
    __nv_bfloat16* p_s =
        reinterpret_cast<__nv_bfloat16*>(reinterpret_cast<unsigned char*>(v_bf16) +
                                          kCausalPromptNvfp4VStageBytes);
    float* running_m_s = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(p_s) +
                                                   kCausalPromptNvfp4PBytes);
    float* running_l_s = running_m_s + Br;
    float* partial_m_s   = running_l_s + Br;
    float* partial_l_s   = partial_m_s + 2 * Br;
    float* alpha_s       = partial_l_s + 2 * Br;
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
    if (q_head >= Geometry::QHeads || q0 >= width) return;
    if (q0 >= tokens) {
        causal_prompt_zero_output_rows<Geometry>(out, q_head, q0, min(q0 + Br, width), tid,
                                                 kCausalPromptNvfp4Threads);
        return;
    }
    const int base_pos              = positions[0];
    const std::int32_t* block_table = metadata.block_table();
    const int tile_rows             = min(Br, tokens - q0);
    const int max_query_abs         = base_pos + q0 + tile_rows - 1;
    const int key_blocks            = max_query_abs / Bc + 1;

    // ---- Q quantization: bf16 -> Hadamard -> NVFP4 (E2M1 codes + E4M3 scales) ----
    // Phase 1: Load Q, apply Hadamard, store to v_bf16 (temp buffer, free during Q phase).
    for (int row = warp; row < Br; row += kCausalPromptNvfp4Warps) {
        float values[8];
#pragma unroll
        for (int r = 0; r < 8; ++r) {
            const int d = lane + 32 * r;
            values[r] =
                row < tile_rows
                    ? __bfloat162float(q[causal_prompt_q_index<Geometry>(q_head, d, q0 + row)])
                    : 0.0F;
        }
        normalized_hadamard_d256_inplace(values, lane);
#pragma unroll
        for (int r = 0; r < 8; ++r) {
            const int d = lane + 32 * r;
            v_bf16[row * D + d] = __float2bfloat16(values[r]);
        }
    }
    __syncthreads();

    // Phase 2: Quantize from v_bf16 to q_codes (swizzled) + q_scale_s (natural).
    for (int unit = tid; unit < Br * Groups; unit += kCausalPromptNvfp4Threads) {
        const int row = unit / Groups;
        const int grp = unit - row * Groups;
        const detail::Nvfp4QuantizedK16 result =
            detail::quantize_nvfp4_k16(&v_bf16[row * D + grp * kKVCacheNvfp4Group], 1.0F);
        causal_prompt_nvfp4_store_code_swz(q_codes, row, grp, result.codes_lo, result.codes_hi);
        q_scale_s[row * Groups + grp] = result.scale;
    }
    if (tid < Br) {
        running_m_s[tid] = -CUDART_INF_F;
        running_l_s[tid] = 0.0F;
    }
    __syncthreads();

    const int gid      = lane >> 2;
    const int lid      = lane & 3;
    const int a_mat    = lane >> 3;
    const int a_rin    = lane & 7;
    const int a_rowoff = a_rin + ((a_mat & 1) << 3);
    const int a_coloff = (a_mat >> 1) << 3;
    const int b_rin    = lane & 7;
    const int b_koff   = ((lane >> 3) & 1) << 3;

    // NVFP4 scale-fragment row mappings for mma_nvfp4_e4m3 (m16n8k64).
    const int sfa_row = ((lane & 1) << 3) | (lane >> 2);
    const int sfb_row = lane >> 2;

    auto issue_kv_scales = [&](int tile_k0, int cooperative_tid, int cooperative_threads) {
        const int physical_page = block_table[tile_k0 >> kPagedKVPageShift];
        const int page_offset0  = tile_k0 & (kPagedKVPageSize - 1);
        for (int key_l = cooperative_tid; key_l < Bc; key_l += cooperative_threads) {
            const int key = tile_k0 + key_l;
            std::uint8_t* kd = &k_scale_s[key_l * Groups];
            std::uint8_t* vd = &v_scale_s[key_l * Groups];
            if (key <= max_query_abs) {
                const std::int64_t off = kv_cache_nvfp4_scale_index<Geometry>(physical_page, kv_head,
                                                                             0, page_offset0 + key_l);
                cp_async<16>(kd, &cache_k_scale[off]);
                cp_async<16>(vd, &cache_v_scale[off]);
            } else {
                store_vec(kd, make_uint4(0, 0, 0, 0));
                store_vec(vd, make_uint4(0, 0, 0, 0));
            }
        }
    };

    auto issue_kv_codes = [&](int tile_k0, int cooperative_tid, int cooperative_threads) {
        const int physical_page = block_table[tile_k0 >> kPagedKVPageShift];
        const int page_offset0  = tile_k0 & (kPagedKVPageSize - 1);
#pragma unroll 1
        for (int chunk = cooperative_tid; chunk < Bc * (D / 2 / 16); chunk += cooperative_threads) {
            const int key_l  = chunk / (D / 2 / 16);
            const int dc     = chunk - key_l * (D / 2 / 16);
            const int d      = dc * 16;
            const int key    = tile_k0 + key_l;
            std::uint8_t* kd = &k_codes[(key_l * DB16 + causal_prompt_swz(key_l, dc * 8)) * 2];
            std::uint8_t* vd = &v_codes[key_l * (D / 2) + d];
            if (key <= max_query_abs) {
                const std::int64_t off = kv_cache_nvfp4_code_index<Geometry>(physical_page, kv_head,
                                                                             d, page_offset0 + key_l);
                cp_async<16, Cache::cg>(kd, &cache_k[off]);
                cp_async<16, Cache::cg>(vd, &cache_v[off]);
            } else {
                store_vec(kd, make_int4(0, 0, 0, 0));
                store_vec(vd, make_int4(0, 0, 0, 0));
            }
        }
        ninfer::ops::cp_commit();
    };

    auto issue_kv_tile = [&](int tile_k0, int cooperative_tid, int cooperative_threads) {
        issue_kv_scales(tile_k0, cooperative_tid, cooperative_threads);
        issue_kv_codes(tile_k0, cooperative_tid, cooperative_threads);
    };

    issue_kv_tile(0, tid, kCausalPromptNvfp4Threads);
    ninfer::ops::cp_wait<0>();
    __syncthreads();

    float acc[PVNtPerWarp][4];
#pragma unroll
    for (int n = 0; n < PVNtPerWarp; ++n) {
#pragma unroll
        for (int i = 0; i < 4; ++i) acc[n][i] = 0.0F;
    }
    const float scale_l2 = scale * Log2E;
    for (int kb = 0; kb < key_blocks; ++kb) {
        const int k0 = kb * Bc;
        if (warp < ProducerWarps) {
            const int row_base = (warp >> 1) * 16;
            const int col_half = warp & 1;
            const int col_base = col_half * (Bc / 2);
            float score[QKNt][4];
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt)
                score[nt][0] = score[nt][1] = score[nt][2] = score[nt][3] = 0.0F;

#pragma unroll
            for (int kk = 0; kk < QKKs; ++kk) {
                const int acol = kk * 16 + a_coloff;
                unsigned af[4];
                ldmatrix_x4(af[0], af[1], af[2], af[3],
                            smem_addr(&q_bf16[(row_base + a_rowoff) * DB16 +
                                             causal_prompt_swz(row_base + a_rowoff, acol)]));
                const unsigned sfa = load_vec<unsigned>(
                    &q_scale_s[(row_base + sfa_row) * Groups + kk * GroupsPerKStep]);
#pragma unroll
                for (int nt = 0; nt < QKNt; ++nt) {
                    const int brow = col_base + nt * 8 + b_rin;
                    const int bcol = kk * 16 + b_koff;
                    unsigned bf[2];
                    ldmatrix_x2(bf[0], bf[1],
                                smem_addr(&k_bf16[brow * DB16 + causal_prompt_swz(brow, bcol)]));
                    const int sfb_key = col_base + nt * 8 + sfb_row;
                    const unsigned sfb = load_vec<unsigned>(
                        &k_scale_s[sfb_key * Groups + kk * GroupsPerKStep]);
                    mma_nvfp4_e4m3(score[nt][0], score[nt][1], score[nt][2], score[nt][3], af[0],
                                   af[1], af[2], af[3], bf[0], bf[1], sfa, sfb);
                }
            }

            const int row0             = row_base + gid;
            const int row1             = row0 + 8;
            const int qabs0            = row0 < tile_rows ? base_pos + q0 + row0 : -1;
            const int qabs1            = row1 < tile_rows ? base_pos + q0 + row1 : -1;
            const bool full_score_tile = q0 + Br <= tokens && k0 + Bc - 1 <= base_pos + q0;
            float bm0                  = -CUDART_INF_F;
            float bm1                  = -CUDART_INF_F;
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                const int key0 = k0 + col_base + nt * 8 + 2 * lid;
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
            if (lid == 0) {
                partial_m_s[col_half * Br + row0] = bm0;
                partial_m_s[col_half * Br + row1] = bm1;
            }
            asm volatile("bar.sync 1, 256;" ::: "memory");

            bm0                     = fmaxf(partial_m_s[row0], partial_m_s[Br + row0]);
            bm1                     = fmaxf(partial_m_s[row1], partial_m_s[Br + row1]);
            const float previous_m0 = running_m_s[row0];
            const float previous_m1 = running_m_s[row1];
            const float nm0         = fmaxf(previous_m0, bm0);
            const float nm1         = fmaxf(previous_m1, bm1);
            const float nm0_scaled  = nm0 * scale_l2;
            const float nm1_scaled  = nm1 * scale_l2;
            const float alpha0      = previous_m0 == -CUDART_INF_F
                                          ? 0.0F
                                          : exp2_approx(__fmaf_rn(previous_m0, scale_l2, -nm0_scaled));
            const float alpha1      = previous_m1 == -CUDART_INF_F
                                          ? 0.0F
                                          : exp2_approx(__fmaf_rn(previous_m1, scale_l2, -nm1_scaled));
            float bl0               = 0.0F;
            float bl1               = 0.0F;
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                const int col0  = col_base + nt * 8 + 2 * lid;
                const int col1  = col0 + 1;
                const float p00 = score[nt][0] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][0], scale_l2, -nm0_scaled))
                                      : 0.0F;
                const float p01 = score[nt][1] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][1], scale_l2, -nm0_scaled))
                                      : 0.0F;
                const float p10 = score[nt][2] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][2], scale_l2, -nm1_scaled))
                                      : 0.0F;
                const float p11 = score[nt][3] > -CUDART_INF_F
                                      ? exp2_approx(__fmaf_rn(score[nt][3], scale_l2, -nm1_scaled))
                                      : 0.0F;
                bl0 += p00 + p01;
                bl1 += p10 + p11;
                p_s[row0 * Bc + causal_prompt_nvfp4_p_swz(row0, col0)] = __float2bfloat16_rn(p00);
                p_s[row0 * Bc + causal_prompt_nvfp4_p_swz(row0, col1)] = __float2bfloat16_rn(p01);
                p_s[row1 * Bc + causal_prompt_nvfp4_p_swz(row1, col0)] = __float2bfloat16_rn(p10);
                p_s[row1 * Bc + causal_prompt_nvfp4_p_swz(row1, col1)] = __float2bfloat16_rn(p11);
            }
            bl0 = warp_sum<4>(bl0, FullMask);
            bl1 = warp_sum<4>(bl1, FullMask);
            if (lid == 0) {
                partial_l_s[col_half * Br + row0] = bl0;
                partial_l_s[col_half * Br + row1] = bl1;
            }
            asm volatile("bar.sync 1, 256;" ::: "memory");
            if (col_half == 0 && lid == 0) {
                const float tile_l0 = partial_l_s[row0] + partial_l_s[Br + row0];
                const float tile_l1 = partial_l_s[row1] + partial_l_s[Br + row1];
                running_l_s[row0]   = __fmaf_rn(running_l_s[row0], alpha0, tile_l0);
                running_l_s[row1]   = __fmaf_rn(running_l_s[row1], alpha1, tile_l1);
                running_m_s[row0]   = nm0;
                running_m_s[row1]   = nm1;
                alpha_s[row0]       = alpha0;
                alpha_s[row1]       = alpha1;
            }
        } else if (warp < ProducerWarps + VWorkerWarps) {
            const int worker_tid = tid - ProducerWarps * 32;
#pragma unroll 1
            for (int chunk = worker_tid; chunk < Bc * (D / 8); chunk += WorkerThreads) {
                const int key_l = chunk / (D / 8);
                const int dc    = chunk - key_l * (D / 8);
                const int d     = dc * 8;
                const int key   = k0 + key_l;
                __nv_bfloat16* dst = &v_bf16[key_l * D + causal_prompt_swz(key_l, d)];
                if (key <= max_query_abs) {
                    const int grp = d >> 4;
                    const std::uint8_t scale_byte = v_scale_s[key_l * Groups + grp];
                    store_vec(dst, kv_cache_nvfp4_dequant_e2m1x8_from(
                                       &v_codes[key_l * (D / 2) + d / 2], scale_byte));
                } else {
                    store_vec(dst, make_int4(0, 0, 0, 0));
                }
            }
        }
        __syncthreads();

        const bool has_next = kb + 1 < key_blocks;
        if (has_next) issue_kv_tile((kb + 1) * Bc, tid, kCausalPromptNvfp4Threads);

        const int row_tile = warp % kCausalPromptNvfp4RowTiles;
        const int d_slice  = warp / kCausalPromptNvfp4RowTiles;
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
                                       causal_prompt_nvfp4_p_swz(row_base + a_rowoff, pcol)]));
#pragma unroll
            for (int n = 0; n < PVNtPerWarp; ++n) {
                const int global_n = d_slice * PVNtPerWarp + n;
                unsigned vf[2];
                const int vrow = k * 16 + b_koff + b_rin;
                const int vcol = global_n * 8;
                ldmatrix_x2_t(vf[0], vf[1],
                              smem_addr(&v_bf16[vrow * D + causal_prompt_swz(vrow, vcol)]));
                mma_bf16(acc[n][0], acc[n][1], acc[n][2], acc[n][3], pf[0], pf[1], pf[2], pf[3],
                         vf[0], vf[1]);
            }
        }
        if (has_next) ninfer::ops::cp_wait<0>();
        __syncthreads();
    }

    const int row_tile = warp % kCausalPromptNvfp4RowTiles;
    const int d_slice  = warp / kCausalPromptNvfp4RowTiles;
    const int row_base = row_tile * 16;
    const int row0     = row_base + gid;
    const int row1     = row0 + 8;
    const float inv_l0 = running_l_s[row0] > 0.0F ? __frcp_rn(running_l_s[row0]) : 0.0F;
    const float inv_l1 = running_l_s[row1] > 0.0F ? __frcp_rn(running_l_s[row1]) : 0.0F;
#pragma unroll
    for (int n = 0; n < PVNtPerWarp; ++n) {
        const int d0 = (d_slice * PVNtPerWarp + n) * 8 + 2 * lid;
        if (row0 < tile_rows) {
            *reinterpret_cast<unsigned*>(
                &out[causal_prompt_q_index<Geometry>(q_head, d0, q0 + row0)]) =
                pack_bf16x2(acc[n][0] * inv_l0, acc[n][1] * inv_l0);
        }
        if (row1 < tile_rows) {
            *reinterpret_cast<unsigned*>(
                &out[causal_prompt_q_index<Geometry>(q_head, d0, q0 + row1)]) =
                pack_bf16x2(acc[n][2] * inv_l1, acc[n][3] * inv_l1);
        }
    }

    causal_prompt_zero_output_rows<Geometry>(out, q_head, tokens, min(q0 + Br, width), tid,
                                             kCausalPromptNvfp4Threads);
}

} // namespace ninfer::ops
