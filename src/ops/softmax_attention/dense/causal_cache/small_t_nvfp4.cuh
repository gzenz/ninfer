#pragma once

// Row-scaled NVFP4 (E2M1 + E4M3 group-16) split-KV causal attention for T=1..6.
// A CTA owns one KV head and all of its GQA query heads. Q/K use native NVFP4
// m16n8k64 block-scaled Tensor Cores (mma.sync.kind::mxf4nvf4) with FP32
// accumulation; the MMA applies the per-group E4M3 scales internally so no
// external scale multiply is needed after QK. P and decoded V use BF16 MMA
// operands with FP32 accumulation. Split numerators remain FP32 through the
// reducer.
//
// Structure mirrors small_t_fp8.cuh. Key NVFP4 differences:
//   - Group size 16 (not 256): 16 groups per head_dim=256
//   - Codes are packed E2M1 (4 bits, 2 per byte, 128 bytes per key)
//   - Scales are E4M3 bytes (1 per group, 16 per key)
//   - QK uses mma_nvfp4_e4m3 (m16n8k64) with built-in block scales (sfa/sfb)
//   - D=256 = 4 k-steps of k=64 (vs FP8's 8 k-steps of k=32)
//   - No external scale multiplication: the MMA applies scales internally
//   - V dequant uses kv_cache_nvfp4_dequant_e2m1x8_from (4 bytes -> 8 bf16)
//   - PV uses mma_bf16 (V is dequantized to BF16, not FP16)
//   - Q/K quantization uses group-16 quantize_nvfp4_k16 after Hadamard rotation; V is quantized without rotation

#include "ops/kv_cache/nvfp4_g16_codec.cuh"
#include "ops/kv_cache/hadamard_d256.cuh"
#include "ops/linear/nvfp4/nvfp4_codec.cuh"
#include "ops/softmax_attention/dense/causal_cache/small_t.cuh"

#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <math_constants.h>

#include <cstdint>

namespace ninfer::ops {

namespace detail {

__device__ __forceinline__ void causal_small_t_nvfp4_store_code_swz(
    std::uint8_t* tile, int row, int group, std::uint32_t codes_lo,
    std::uint32_t codes_hi, int DB16) {
    // Each group of 16 E2M1 elements = 8 bytes = 4 b16 elements.
    const int b16_col = group * 4;
    const int swz_col = causal_small_t_tc_swz(row, b16_col);
    std::uint8_t* dst = tile + (row * DB16 + swz_col) * 2;
    store_vec(dst, make_uint2(codes_lo, codes_hi));
}

} // namespace detail

template <typename Geometry, int TokenTile, int WarpsPerCta, int MinBlocksPerSm,
          int KeyBlock, bool DynamicArena, bool MultiBatch, bool Masked,
          typename CacheInput>
__launch_bounds__(WarpsPerCta * 32, MinBlocksPerSm) __global__
    void causal_attention_small_t_nvfp4_tiled_kernel(
        const __nv_bfloat16* q, CacheInput input, const std::int32_t* positions,
        std::uint8_t* cache_k, std::uint8_t* cache_v, std::uint8_t* cache_k_scale,
        std::uint8_t* cache_v_scale, const std::int32_t* block_tables,
        const std::int32_t* valid_columns, const std::int32_t* table_rows,
        std::int32_t table_stride, std::int32_t full_width,
        std::int32_t column_begin, std::int32_t logical_capacity,
        float attention_scale, float* partial_acc, float* partial_m,
        float* partial_l) {
    constexpr int Wc                   = WarpsPerCta;
    constexpr int RowCount             = TokenTile * Geometry::GroupSize;
    constexpr int RowTiles             = (RowCount + 15) / 16;
    constexpr int Br                   = RowTiles * 16;
    constexpr int Bc                   = KeyBlock;
    constexpr int D                    = kCausalHeadDim;
    constexpr int DB16                 = D / 4;   // 64 b16 cols (4 E2M1 per b16)
    constexpr int Groups               = kKVCacheNvfp4Groups;  // 16
    constexpr int GroupsPerKStep       = 64 / kKVCacheNvfp4Group; // 4
    constexpr int CodeChunksPerKey     = D / 2 / 16;  // 8 (16-byte cp_async chunks)
    constexpr int Threads              = Wc * 32;
    constexpr int QKKs                 = D / 64;  // 4 (k=64 per MMA step)
    constexpr int QKNt                 = Bc / 8;
    constexpr int PStride              = Bc == 32 ? 64 : Bc;
    constexpr int ConsumerWarpsPerTile = Wc / RowTiles;
    constexpr int PVNtPerWarp          = D / (ConsumerWarpsPerTile * 8);
    constexpr int PVKs                 = Bc / 16;
    constexpr int PageIds              = 128;
    constexpr int ProducerThreads      = RowTiles * 32;
    constexpr int VLoaderThreads       = Threads - ProducerThreads;
    constexpr float Log2E              = 1.4426950408889634074F;
    constexpr unsigned FullMask        = 0xffffffffU;

    static_assert(TokenTile >= 1 && TokenTile <= 6);
    static_assert(Bc == 32 || Bc == 64);
    static_assert(RowTiles >= 1 && RowTiles <= 3);
    static_assert(Wc > RowTiles && Wc % RowTiles == 0);
    static_assert(PVNtPerWarp == 4 || PVNtPerWarp == 8 || PVNtPerWarp == 16);
    static_assert(QKKs == 4);
    static_assert(GroupsPerKStep == 4);
    static_assert(Groups == 16);

    // Shared memory layout.
    __shared__ __align__(16) std::uint8_t q_s[Br * (D / 2)];
    __shared__ __align__(16) std::uint8_t q_scale_s[Br * Groups];
    __shared__ __align__(16) std::uint8_t static_arena[DynamicArena ? 16 : 3 * Bc * D];
    extern __shared__ __align__(16) std::uint8_t dynamic_arena[];
    std::uint8_t* arena       = DynamicArena ? dynamic_arena : static_arena;
    std::uint8_t* k_codes     = arena;
    std::uint8_t* v_codes     = arena + Bc * (D / 2);
    __nv_bfloat16* v_bf16     = reinterpret_cast<__nv_bfloat16*>(arena + Bc * D);
    __nv_bfloat16* q_b16      = reinterpret_cast<__nv_bfloat16*>(q_s);
    __nv_bfloat16* k_b16      = reinterpret_cast<__nv_bfloat16*>(k_codes);
    __shared__ __align__(16) __nv_bfloat16 p_s[Br * PStride];
    __shared__ float alpha_s[Br];
    __shared__ __align__(16) std::uint8_t k_scale_s[Bc * Groups];
    __shared__ __align__(16) std::uint8_t v_scale_s[Bc * Groups];
    __shared__ std::int32_t physical_pages_s[PageIds];
    // Hadamard temp: one D256 row per warp (Wc * 256 * sizeof(bf16)).
    __shared__ __align__(16) __nv_bfloat16 had_s[Wc * D];

    const int kv_head     = static_cast<int>(blockIdx.x);
    const int split       = static_cast<int>(blockIdx.y);
    const int batch       = MultiBatch ? static_cast<int>(blockIdx.z) : 0;
    const int split_count = static_cast<int>(gridDim.y);
    const int tid         = static_cast<int>(threadIdx.x);
    const int warp        = tid >> 5;
    const int lane        = tid & 31;

    int valid_tokens = TokenTile;
    if constexpr (Masked) {
        const int remaining = valid_columns[batch] - column_begin;
        valid_tokens        = remaining <= 0 ? 0 : min(remaining, TokenTile);
    }
    std::int64_t column_base = column_begin;
    if constexpr (MultiBatch) { column_base += static_cast<std::int64_t>(batch) * full_width; }
    q += static_cast<std::int64_t>(D) * Geometry::QHeads * column_base;
    positions += column_base;
    if constexpr (CacheInput::writes_cache) {
        input.k += static_cast<std::int64_t>(D) * Geometry::KVHeads * column_base;
        input.v += static_cast<std::int64_t>(D) * Geometry::KVHeads * column_base;
    }
    const int table_row = table_rows == nullptr ? 0 : table_rows[batch];
    const std::int32_t* block_table =
        block_tables + static_cast<std::int64_t>(table_row) * table_stride;
    if constexpr (MultiBatch) {
        partial_acc +=
            static_cast<std::int64_t>(batch) * D * Geometry::QHeads * TokenTile * split_count;
        partial_m += static_cast<std::int64_t>(batch) * Geometry::QHeads * TokenTile * split_count;
        partial_l += static_cast<std::int64_t>(batch) * Geometry::QHeads * TokenTile * split_count;
    }

    auto write_neutral = [&]() {
        for (int row = tid; row < RowCount; row += Threads) {
            int q_head = 0;
            int token  = 0;
            causal_small_t_tc_row_to_qt<Geometry>(row, TokenTile, kv_head, q_head, token);
            if (causal_valid_q_head<Geometry>(kv_head, q_head)) {
                partial_m[causal_partial_stat_index<Geometry>(q_head, token, split, TokenTile)] =
                    -CUDART_INF_F;
                partial_l[causal_partial_stat_index<Geometry>(q_head, token, split, TokenTile)] =
                    0.0F;
            }
        }
        for (int index = tid; index < RowCount * D; index += Threads) {
            const int row = index / D;
            const int d   = index - row * D;
            int q_head    = 0;
            int token     = 0;
            causal_small_t_tc_row_to_qt<Geometry>(row, TokenTile, kv_head, q_head, token);
            if (causal_valid_q_head<Geometry>(kv_head, q_head)) {
                partial_acc[causal_partial_acc_index<Geometry>(q_head, d, token, split,
                                                               TokenTile)] = 0.0F;
            }
        }
    };
    if (kv_head < 0 || kv_head >= Geometry::KVHeads || split_count <= 0) return;
    if (valid_tokens == 0) {
        write_neutral();
        return;
    }

    const std::int32_t first_pos = positions[0];
    const std::int32_t last_pos  = positions[TokenTile - 1];
    if (first_pos < 0 || last_pos < 0 || last_pos >= logical_capacity) {
        write_neutral();
        return;
    }

    const int window = last_pos + 1;
    const int active_split_count =
        causal_small_t_nvfp4_active_splits<Geometry>(window, split_count, TokenTile);
    if (split >= active_split_count) return;

    const int logical_tiles = div_up(window, Bc);
    const bool tile_split   = logical_tiles >= active_split_count;
    int split_start         = 0;
    int split_end           = 0;
    if constexpr (TokenTile == 1 && Bc == 32) {
        const int first_owned_tile = split * logical_tiles / active_split_count;
        const int end_owned_tile   = (split + 1) * logical_tiles / active_split_count;
        split_start                = first_owned_tile * Bc;
        split_end                  = min(end_owned_tile * Bc, window);
    } else {
        const int units_per_split = tile_split ? div_up(logical_tiles, active_split_count)
                                               : div_up(window, active_split_count);
        split_start               = split * units_per_split * (tile_split ? Bc : 1);
        split_end = min(split_start + units_per_split * (tile_split ? Bc : 1), window);
    }
    if (split_start >= split_end) {
        write_neutral();
        return;
    }
    const int first_tile = (split_start / Bc) * Bc;
    const int key_blocks = div_up(split_end - first_tile, Bc);
    const int first_page = first_tile >> kPagedKVPageShift;
    const int page_count = ((split_end - 1) >> kPagedKVPageShift) - first_page + 1;
    for (int page = tid; page < page_count; page += Threads) {
        physical_pages_s[page] = block_table[first_page + page];
    }
    __syncthreads();

    // ---- Fused K/V append: quantize current K/V to NVFP4 inside the kernel ----
    if constexpr (CacheInput::writes_cache) {
        for (int token = warp; token < valid_tokens; token += Wc) {
            const int position = positions[token];
            if (position < split_start || position >= split_end) continue;
            const int physical_page =
                physical_pages_s[(position >> kPagedKVPageShift) - first_page];
            const int page_offset = position & kPagedKVPageMask;

            // K: load, Hadamard, write to had_s, then group-quantize.
            float values[8];
#pragma unroll
            for (int r = 0; r < 8; ++r) {
                const int d = lane + 32 * r;
                values[r] =
                    __bfloat162float(input.k[kv_cache_nvfp4_src_index<Geometry>(kv_head, d, token)]);
            }
            normalized_hadamard_d256_inplace(values, lane);
#pragma unroll
            for (int r = 0; r < 8; ++r) {
                had_s[warp * D + lane + 32 * r] = __float2bfloat16(values[r]);
            }
            __syncwarp();
            if (lane < Groups) {
                const int byte_off = lane * (kKVCacheNvfp4Group / 2);
                const std::int64_t code_off = kv_cache_nvfp4_code_index<Geometry>(
                    physical_page, kv_head, byte_off, page_offset);
                const std::int64_t scale_off = kv_cache_nvfp4_scale_index<Geometry>(
                    physical_page, kv_head, lane, page_offset);
                kv_cache_nvfp4_quantize_k16(
                    &had_s[warp * D + lane * kKVCacheNvfp4Group],
                    &cache_k[code_off], &cache_k_scale[scale_off]);
            }
            __syncwarp();

            // V: load (NO Hadamard - only K is rotated, matching FP8/int8), write to had_s, then group-quantize.
#pragma unroll
            for (int r = 0; r < 8; ++r) {
                const int d = lane + 32 * r;
                values[r] =
                    __bfloat162float(input.v[kv_cache_nvfp4_src_index<Geometry>(kv_head, d, token)]);
            }
            // V is NOT rotated (only K is rotated for quant quality, matching FP8/int8).
#pragma unroll
            for (int r = 0; r < 8; ++r) {
                had_s[warp * D + lane + 32 * r] = __float2bfloat16(values[r]);
            }
            __syncwarp();
            if (lane < Groups) {
                const int byte_off = lane * (kKVCacheNvfp4Group / 2);
                const std::int64_t code_off = kv_cache_nvfp4_code_index<Geometry>(
                    physical_page, kv_head, byte_off, page_offset);
                const std::int64_t scale_off = kv_cache_nvfp4_scale_index<Geometry>(
                    physical_page, kv_head, lane, page_offset);
                kv_cache_nvfp4_quantize_k16(
                    &had_s[warp * D + lane * kKVCacheNvfp4Group],
                    &cache_v[code_off], &cache_v_scale[scale_off]);
            }
            __syncwarp();
        }
        __syncthreads();
    }

    // ---- Q quantization: bf16 -> Hadamard -> NVFP4 (E2M1 codes + E4M3 scales) ----
    for (int index = tid; index < Br * (D / 2); index += Threads) q_s[index] = 0;
    for (int index = tid; index < Br * Groups; index += Threads) q_scale_s[index] = 0;
    __syncthreads();

    for (int row = warp; row < RowCount; row += Wc) {
        int q_head = 0;
        int token  = 0;
        causal_small_t_tc_row_to_qt<Geometry>(row, TokenTile, kv_head, q_head, token);
        float values[8];
#pragma unroll
        for (int r = 0; r < 8; ++r) {
            const int d = lane + 32 * r;
            values[r]   = __bfloat162float(q[causal_q_index<Geometry>(q_head, d, token)]);
        }
        normalized_hadamard_d256_inplace(values, lane);
#pragma unroll
        for (int r = 0; r < 8; ++r) {
            had_s[warp * D + lane + 32 * r] = __float2bfloat16(values[r]);
        }
        __syncwarp();
        if (lane < Groups) {
            const detail::Nvfp4QuantizedK16 result =
                detail::quantize_nvfp4_k16(&had_s[warp * D + lane * kKVCacheNvfp4Group], 1.0F);
            detail::causal_small_t_nvfp4_store_code_swz(
                q_s, row, lane, result.codes_lo, result.codes_hi, DB16);
            q_scale_s[row * Groups + lane] = result.scale;
        }
        __syncwarp();
    }
    __syncthreads();

    // ---- Lane mappings for ldmatrix (same as FP8) ----
    const int gid      = lane >> 2;
    const int lid      = lane & 3;
    const int a_mat    = lane >> 3;
    const int a_rin    = lane & 7;
    const int a_rowoff = a_rin + ((a_mat & 1) << 3);
    const int a_coloff = (a_mat >> 1) << 3;
    const int b_rin    = lane & 7;
    const int b_koff   = ((lane >> 3) & 1) << 3;

    // NVFP4 scale row mappings for the mma_nvfp4_e4m3 sfa/sfb packing.
    const int sfa_row = ((lane & 1) << 3) | (lane >> 2);
    const int sfb_row = lane >> 2;

    float acc[PVNtPerWarp][4];
#pragma unroll
    for (int n = 0; n < PVNtPerWarp; ++n) {
#pragma unroll
        for (int i = 0; i < 4; ++i) acc[n][i] = 0.0F;
    }
    float m0 = -CUDART_INF_F;
    float m1 = -CUDART_INF_F;
    float l0 = 0.0F;
    float l1 = 0.0F;

    // ---- KV tile staging lambda ----
    auto issue_kv_tile = [&](int tile_k0, int physical_page) {
        // Stage K and V scales: 16 E4M3 bytes per key, one cp_async<16>.
        for (int key_l = tid; key_l < Bc; key_l += Threads) {
            const int key = tile_k0 + key_l;
            if (key >= split_start && key < split_end) {
                const std::int64_t scale_offset = kv_cache_nvfp4_scale_index<Geometry>(
                    physical_page, kv_head, 0, key & kPagedKVPageMask);
                cp_async<16, Cache::cg>(&k_scale_s[key_l * Groups],
                                        &cache_k_scale[scale_offset]);
                cp_async<16, Cache::cg>(&v_scale_s[key_l * Groups],
                                        &cache_v_scale[scale_offset]);
            } else {
                store_vec(&k_scale_s[key_l * Groups], make_uint4(0, 0, 0, 0));
                store_vec(&v_scale_s[key_l * Groups], make_uint4(0, 0, 0, 0));
            }
        }
        // Stage K and V codes: 128 bytes per key, 8 chunks of 16 bytes.
        // K uses swizzled b16 layout; V uses natural byte layout.
#pragma unroll 1
        for (int chunk = tid; chunk < Bc * CodeChunksPerKey; chunk += Threads) {
            const int key_l = chunk / CodeChunksPerKey;
            const int dc    = chunk - key_l * CodeChunksPerKey;
            const int d     = dc * 16;
            const int key   = tile_k0 + key_l;
            std::uint8_t* k_dst =
                &k_codes[(key_l * DB16 + causal_small_t_tc_swz(key_l, dc * 8)) * 2];
            std::uint8_t* v_dst = &v_codes[key_l * (D / 2) + d];
            if (key >= split_start && key < split_end) {
                const std::int64_t code_offset = kv_cache_nvfp4_code_index<Geometry>(
                    physical_page, kv_head, d, key & kPagedKVPageMask);
                cp_async<16, Cache::cg>(k_dst, &cache_k[code_offset]);
                cp_async<16, Cache::cg>(v_dst, &cache_v[code_offset]);
            } else {
                store_vec(k_dst, make_int4(0, 0, 0, 0));
                store_vec(v_dst, make_int4(0, 0, 0, 0));
            }
        }
        ninfer::ops::cp_commit();
    };

    int physical_page = physical_pages_s[0];
    issue_kv_tile(first_tile, physical_page);
    ninfer::ops::cp_wait<0>();
    __syncthreads();

    // ---- Main loop over key tiles ----
    for (int kb = 0; kb < key_blocks; ++kb) {
        const int k0 = first_tile + kb * Bc;
        if (warp < RowTiles) {
            // ---- Producer warps: QK computation + online softmax ----
            const int row_base = warp * 16;
            __nv_bfloat16* p_sw = &p_s[row_base * PStride];
            float score[QKNt][4];
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                score[nt][0] = score[nt][1] = score[nt][2] = score[nt][3] = 0.0F;
            }
            // D=256 = 4 k-steps of k=64. Each k-step covers 4 groups of 16.
#pragma unroll
            for (int ks = 0; ks < QKKs; ++ks) {
                const int acol = ks * 16 + a_coloff;
                unsigned af[4];
                ldmatrix_x4(af[0], af[1], af[2], af[3],
                            smem_addr(&q_b16[(row_base + a_rowoff) * DB16 +
                                             causal_small_t_tc_swz(row_base + a_rowoff, acol)]));
                // Load Q scales (sfa): 4 E4M3 bytes packed as uint32.
                const unsigned sfa = load_vec<unsigned>(
                    &q_scale_s[(row_base + sfa_row) * Groups + ks * GroupsPerKStep]);
#pragma unroll
                for (int nt = 0; nt < QKNt; ++nt) {
                    const int brow = nt * 8 + b_rin;
                    const int bcol = ks * 16 + b_koff;
                    unsigned bf[2];
                    ldmatrix_x2(bf[0], bf[1],
                                smem_addr(&k_b16[brow * DB16 +
                                                 causal_small_t_tc_swz(brow, bcol)]));
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

            // ---- Online softmax (no external scale multiply needed) ----
            const int row0 = row_base + gid;
            const int row1 = row0 + 8;
            int q_head0 = 0, token0 = 0, q_head1 = 0, token1 = 0;
            causal_small_t_tc_row_to_qt<Geometry>(row0, TokenTile, kv_head, q_head0, token0);
            causal_small_t_tc_row_to_qt<Geometry>(row1, TokenTile, kv_head, q_head1, token1);
            const int qabs0 = row0 < RowCount ? positions[token0] : -1;
            const int qabs1 = row1 < RowCount ? positions[token1] : -1;
            float bm0       = -CUDART_INF_F;
            float bm1       = -CUDART_INF_F;
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                const int col0 = nt * 8 + 2 * lid;
                const int key0 = k0 + col0;
                const int key1 = key0 + 1;
                score[nt][0] =
                    row0 < RowCount && key0 >= split_start && key0 < split_end && key0 <= qabs0
                        ? score[nt][0] * attention_scale
                        : -CUDART_INF_F;
                score[nt][1] =
                    row0 < RowCount && key1 >= split_start && key1 < split_end && key1 <= qabs0
                        ? score[nt][1] * attention_scale
                        : -CUDART_INF_F;
                score[nt][2] =
                    row1 < RowCount && key0 >= split_start && key0 < split_end && key0 <= qabs1
                        ? score[nt][2] * attention_scale
                        : -CUDART_INF_F;
                score[nt][3] =
                    row1 < RowCount && key1 >= split_start && key1 < split_end && key1 <= qabs1
                        ? score[nt][3] * attention_scale
                        : -CUDART_INF_F;
                bm0 = fmaxf(bm0, fmaxf(score[nt][0], score[nt][1]));
                bm1 = fmaxf(bm1, fmaxf(score[nt][2], score[nt][3]));
            }
            bm0                = warp_max<4>(bm0, FullMask);
            bm1                = warp_max<4>(bm1, FullMask);
            const float nm0    = fmaxf(m0, bm0);
            const float nm1    = fmaxf(m1, bm1);
            const float alpha0 = m0 == -CUDART_INF_F ? 0.0F : exp2_approx((m0 - nm0) * Log2E);
            const float alpha1 = m1 == -CUDART_INF_F ? 0.0F : exp2_approx((m1 - nm1) * Log2E);
            float bl0          = 0.0F;
            float bl1          = 0.0F;
#pragma unroll
            for (int nt = 0; nt < QKNt; ++nt) {
                const int col0  = nt * 8 + 2 * lid;
                const float p00 = nm0 > -CUDART_INF_F && score[nt][0] > -CUDART_INF_F
                                      ? exp2_approx((score[nt][0] - nm0) * Log2E)
                                      : 0.0F;
                const float p01 = nm0 > -CUDART_INF_F && score[nt][1] > -CUDART_INF_F
                                      ? exp2_approx((score[nt][1] - nm0) * Log2E)
                                      : 0.0F;
                const float p10 = nm1 > -CUDART_INF_F && score[nt][2] > -CUDART_INF_F
                                      ? exp2_approx((score[nt][2] - nm1) * Log2E)
                                      : 0.0F;
                const float p11 = nm1 > -CUDART_INF_F && score[nt][3] > -CUDART_INF_F
                                      ? exp2_approx((score[nt][3] - nm1) * Log2E)
                                      : 0.0F;
                bl0 += p00 + p01;
                bl1 += p10 + p11;
                *reinterpret_cast<unsigned*>(
                    &p_sw[gid * PStride + causal_small_t_tc_swz(gid, col0)]) =
                    pack_bf16x2(p00, p01);
                *reinterpret_cast<unsigned*>(
                    &p_sw[(gid + 8) * PStride + causal_small_t_tc_swz(gid + 8, col0)]) =
                    pack_bf16x2(p10, p11);
            }
            bl0 = warp_sum<4>(bl0, FullMask);
            bl1 = warp_sum<4>(bl1, FullMask);
            l0  = __fmaf_rn(l0, alpha0, bl0);
            l1  = __fmaf_rn(l1, alpha1, bl1);
            m0  = nm0;
            m1  = nm1;
            if (lid == 0) {
                alpha_s[row0] = alpha0;
                alpha_s[row1] = alpha1;
            }
        } else {
            // ---- V dequant: consumer warps dequantize V from E2M1 to BF16 ----
            const int loader_tid = tid - ProducerThreads;
#pragma unroll 1
            for (int chunk = loader_tid; chunk < Bc * (D / 8); chunk += VLoaderThreads) {
                const int key_l = chunk / (D / 8);
                const int dc    = chunk - key_l * (D / 8);
                const int d     = dc * 8;
                const int key   = k0 + key_l;
                __nv_bfloat16* dst = &v_bf16[key_l * D + causal_small_t_tc_swz(key_l, d)];
                if (key >= split_start && key < split_end) {
                    const int grp     = d >> 4;
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
        if (has_next) {
            const int next_k0 = k0 + Bc;
            if ((next_k0 & kPagedKVPageMask) == 0) {
                physical_page = physical_pages_s[(next_k0 >> kPagedKVPageShift) - first_page];
            }
            issue_kv_tile(next_k0, physical_page);
        }

        // ---- PV: all warps participate ----
        const int consumer_tile     = warp % RowTiles;
        const int consumer_slice    = warp / RowTiles;
        const int consumer_row_base = consumer_tile * 16;
        __nv_bfloat16* p_consumer   = &p_s[consumer_row_base * PStride];
        const float alpha0          = alpha_s[consumer_row_base + gid];
        const float alpha1          = alpha_s[consumer_row_base + gid + 8];
#pragma unroll
        for (int n = 0; n < PVNtPerWarp; ++n) {
            acc[n][0] *= alpha0;
            acc[n][1] *= alpha0;
            acc[n][2] *= alpha1;
            acc[n][3] *= alpha1;
        }
#pragma unroll
        for (int n = 0; n < PVNtPerWarp; ++n) {
            const int global_n = consumer_slice * PVNtPerWarp + n;
#pragma unroll
            for (int k = 0; k < PVKs; ++k) {
                unsigned pf[4];
                const int pcol = k * 16 + a_coloff;
                ldmatrix_x4(
                    pf[0], pf[1], pf[2], pf[3],
                    smem_addr(
                        &p_consumer[a_rowoff * PStride +
                                    causal_small_t_tc_swz(a_rowoff, pcol)]));
                unsigned vf[2];
                const int vrow = k * 16 + b_koff + b_rin;
                const int vcol = global_n * 8;
                ldmatrix_x2_t(vf[0], vf[1],
                              smem_addr(&v_bf16[vrow * D +
                                                causal_small_t_tc_swz(vrow, vcol)]));
                mma_bf16(acc[n][0], acc[n][1], acc[n][2], acc[n][3], pf[0], pf[1], pf[2],
                         pf[3], vf[0], vf[1]);
            }
        }
        if (has_next) { ninfer::ops::cp_wait<0>(); }
        __syncthreads();
    }

    // ---- Write partial results ----
    if (warp < RowTiles && lid == 0) {
        const int row0 = warp * 16 + gid;
        const int row1 = row0 + 8;
        if (row0 < RowCount) {
            int q_head = 0;
            int token  = 0;
            causal_small_t_tc_row_to_qt<Geometry>(row0, TokenTile, kv_head, q_head, token);
            partial_m[causal_partial_stat_index<Geometry>(q_head, token, split, TokenTile)] = m0;
            partial_l[causal_partial_stat_index<Geometry>(q_head, token, split, TokenTile)] = l0;
        }
        if (row1 < RowCount) {
            int q_head = 0;
            int token  = 0;
            causal_small_t_tc_row_to_qt<Geometry>(row1, TokenTile, kv_head, q_head, token);
            partial_m[causal_partial_stat_index<Geometry>(q_head, token, split, TokenTile)] = m1;
            partial_l[causal_partial_stat_index<Geometry>(q_head, token, split, TokenTile)] = l1;
        }
    }

#pragma unroll
    for (int n = 0; n < PVNtPerWarp; ++n) {
        const int consumer_tile     = warp % RowTiles;
        const int consumer_slice    = warp / RowTiles;
        const int consumer_row_base = consumer_tile * 16;
        const int d0                = (consumer_slice * PVNtPerWarp + n) * 8 + 2 * lid;
        const int row0              = consumer_row_base + gid;
        const int row1              = row0 + 8;
        if (row0 < RowCount) {
            int q_head = 0;
            int token  = 0;
            causal_small_t_tc_row_to_qt<Geometry>(row0, TokenTile, kv_head, q_head, token);
            const std::int64_t dst =
                causal_partial_acc_index<Geometry>(q_head, d0, token, split, TokenTile);
            *reinterpret_cast<float2*>(&partial_acc[dst]) = make_float2(acc[n][0], acc[n][1]);
        }
        if (row1 < RowCount) {
            int q_head = 0;
            int token  = 0;
            causal_small_t_tc_row_to_qt<Geometry>(row1, TokenTile, kv_head, q_head, token);
            const std::int64_t dst =
                causal_partial_acc_index<Geometry>(q_head, d0, token, split, TokenTile);
            *reinterpret_cast<float2*>(&partial_acc[dst]) = make_float2(acc[n][2], acc[n][3]);
        }
    }
}

// ---- Reduce kernel: identical to FP8 (reads FP32 partial_acc) ----
template <typename Geometry, int DChunk, bool MultiBatch, bool Masked, bool Offset>
__launch_bounds__(256) __global__ void causal_attention_small_t_nvfp4_reduce_output_kernel(
    const float* partial_acc, const float* partial_m, const float* partial_l,
    const std::int32_t* positions, const std::int32_t* valid_columns, std::int32_t tokens,
    std::int32_t full_width, std::int32_t column_begin, std::int32_t batch_size,
    std::int32_t split_count, __nv_bfloat16* out) {
    const int q_head      = static_cast<int>(blockIdx.x);
    const int d_start     = static_cast<int>(blockIdx.y) * DChunk;
    const int flat_column = static_cast<int>(blockIdx.z);
    int batch             = 0;
    int token             = flat_column;
    if constexpr (MultiBatch) {
        batch = flat_column / tokens;
        token = flat_column - batch * tokens;
    }
    const int tid = static_cast<int>(threadIdx.x);
    if (q_head >= Geometry::QHeads || token >= tokens) return;
    if constexpr (MultiBatch) {
        if (batch >= batch_size) return;
    }
    if constexpr (Offset) positions += column_begin;
    if constexpr (MultiBatch) positions += static_cast<std::int64_t>(batch) * full_width;
    const int window  = positions[tokens - 1] + 1;
    int output_column = token;
    if constexpr (Offset) output_column += column_begin;
    if constexpr (MultiBatch) output_column += batch * full_width;

    if constexpr (MultiBatch) {
        partial_acc += static_cast<std::int64_t>(batch) * kCausalHeadDim * Geometry::QHeads *
                       tokens * split_count;
        partial_m += static_cast<std::int64_t>(batch) * Geometry::QHeads * tokens * split_count;
        partial_l += static_cast<std::int64_t>(batch) * Geometry::QHeads * tokens * split_count;
    }
    const int active_splits =
        causal_small_t_nvfp4_active_splits<Geometry>(window, split_count, tokens);
    __shared__ float reduce[256];
    float local_m = -CUDART_INF_F;
    for (int split = tid; split < active_splits; split += blockDim.x) {
        local_m = fmaxf(
            local_m, partial_m[causal_partial_stat_index<Geometry>(q_head, token, split, tokens)]);
    }
    reduce[tid] = local_m;
    __syncthreads();
    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) reduce[tid] = fmaxf(reduce[tid], reduce[tid + stride]);
        __syncthreads();
    }
    const float head_m = reduce[0];
    __syncthreads();
    if (head_m == -CUDART_INF_F) {
        const int d = d_start + tid;
        if (tid < DChunk && d < kCausalHeadDim) {
            out[causal_q_index<Geometry>(q_head, d, output_column)] = __float2bfloat16(0.0F);
        }
        return;
    }

    float local_l = 0.0F;
    for (int split = tid; split < active_splits; split += blockDim.x) {
        const float tile_l =
            partial_l[causal_partial_stat_index<Geometry>(q_head, token, split, tokens)];
        if (tile_l > 0.0F) {
            local_l +=
                tile_l *
                expf(partial_m[causal_partial_stat_index<Geometry>(q_head, token, split, tokens)] -
                     head_m);
        }
    }
    reduce[tid] = local_l;
    __syncthreads();
    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) reduce[tid] += reduce[tid + stride];
        __syncthreads();
    }
    const float head_l = reduce[0];
    __syncthreads();
    if (tid < active_splits) {
        const float tile_l =
            partial_l[causal_partial_stat_index<Geometry>(q_head, token, tid, tokens)];
        reduce[tid] =
            tile_l > 0.0F && head_l > 0.0F
                ? expf(partial_m[causal_partial_stat_index<Geometry>(q_head, token, tid, tokens)] -
                       head_m)
                : 0.0F;
    }
    __syncthreads();
    const int d = d_start + tid;
    if (tid >= DChunk || d >= kCausalHeadDim) return;

    float numerator = 0.0F;
    for (int split = 0; split < active_splits; ++split) {
        numerator +=
            partial_acc[causal_partial_acc_index<Geometry>(q_head, d, token, split, tokens)] *
            reduce[split];
    }
    bool valid = true;
    if constexpr (Masked) {
        int absolute_column = token;
        if constexpr (Offset) absolute_column += column_begin;
        valid = absolute_column < valid_columns[batch];
    }
    const float value = valid && head_l > 0.0F ? numerator / head_l : 0.0F;
    out[causal_q_index<Geometry>(q_head, d, output_column)] = __float2bfloat16(value);
}

} // namespace ninfer::ops
