// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2024, Advanced Micro Devices, Inc. All rights reserved.

#pragma once

#include "ck_tile/core.hpp"
#include "ck_tile/ops/common.hpp"
#include "ck_tile/ops/elementwise.hpp"
#include "ck_tile/host/hip_check_error.hpp"
#include <string>
#include <type_traits>

namespace ck_tile {

#define MOE_SORTING_MOCK_ID(token_id_, topk_id_) \
    static_cast<uint32_t>(((token_id_)&0x00ffffff) | (((topk_id_)&0xff) << 24))

// clang-format off
// [indexing implementation-1]
// using M_a as constexpr block_size to partition all tokens into different slices
// each slice map to one expert, and one expert can have multiple slices
// e.g. num_experts = 6, topk=3, M_a = 4, input_tokens = 5
// before sort, topk_ids is : [[0, 3, 5], [2, 3, 5], [1, 3, 5], [1, 2, 3], [1, 3, 5]]
//                            tok-0      tok-1      tok-2      tok-3      tok-4
//           topk_weight is : [[a, b, c], [d, e, f], [g, h, i], [j, k, l], [m, n, o]] (some float number)
//
// token_id_per_expert is : [[0], [2, 3, 4], [1, 3], [0, 1, 2, 3, 4], [], [0, 1, 2, 5]]
//  (only for reference)    exp-0  exp-1     exp-2   exp-3          exp-4  exp-5
// weight_id_per_expert is: [[a], [g, j, m], [d, k], [b, e, h, l, n], [], [c, f, i, o]]
//
// max_num_tokens_padded : topk * input_tokens + num_experts * (M_a - 1)
// * this could be larger than actual, since actual tokens are on GPU
//
// sorted_token_ids_ptr   : [0, 6, 6, 6, 2, 3, 4, 6, 1, 3, 6, 6, 0, 1, 2, 3, 4, 6, 6, 6, 6, 6, 6, 6, 0, 1, 2, 5]
//                          |-  exp-0  -|-  exp-1  -|-  exp-2  -|-      exp-3          -|-  exp-4 -|-  exp-5  -|
// sorted_weight_ptr      : [a, *, *, *, g, j, m, *, d, k, *, *, b, e, h, l, n, *, *, *, *, *, *, *, c, f, i, o]
//
// * length is max_num_tokens_padded, actual size is num_tokens_post_padded_ptr
//
// * Note on token_id_per_expert/sorted_token_ids_ptr data:
// currently we do not have topk information from the data of token_id_per_expert/sorted_token_ids_ptr.
// In some cases(like smooth-quant), we need topk information to indexing into tokens quant from 
// different expert smooth quant. So we modify the number stored inside token_id_per_expert/sorted_token_ids_ptr
//
//       32bit    0........23 24.....31 bit
//      (data) -> (token_id | topk_id)
// low 24 bit is for token id, top 8 bit is for topk id
//
// the input after smooth-quant is [topk, token, hidden_dim], originally it is [token, hidden_dim]
// the input scale for token is [topk, token, 1], the smooth-quant scale for first gemm is [expert, interm_dim]
//
// sorted_expert_ids_ptr  : [0, 1, 2, 3, 3, 4, 5]
// * length is (max_num_tokens_padded + block_size - 1) / block_size
//
// num_tokens_post_padded_ptr : [28]
// num_sorted_tiles_ptr : [7]
//
// * different from vLLM
//   1) token_id stored in sorted_token_ids_ptr is actual token_id, not token_id*top_K expanded id
//   2）need sorted_weight_ptr
//   3) use num_sorted_tiles_ptr, already divided by M_a
//
// * below used for indexing
//  1) sorted_token_ids_ptr [max_num_tokens_padded]
//  2) sorted_weight_ptr
//  3) sorted_expert_ids_ptr
//  4）num_tokens_post_padded_ptr/num_sorted_tiles_ptr (select one)
//
//   max_num_tokens_padded: opk_ids.numel() + num_experts * (block_size - 1)
struct MoeSortingHostArgs
{
    const void* p_topk_ids;     // [token, topk]
    const void* p_weights;      // [token, topk]
    void* p_sorted_token_ids;
    void* p_sorted_weights;
    void* p_sorted_expert_ids;
    void* p_total_tokens_post_pad;
    // we fused the setzero of output of fused-moe buffer
    // set this pointer to nullptr will skip this operation
    void* p_moe_buf;
    index_t tokens;
    index_t unit_size;      // this is the M_a of fused-moe kernel
    index_t num_experts;
    index_t topk;
    index_t moe_buf_bytes;  // byte size of p_moe_buf
};

template <typename Problem_>
struct MoeSortingKernel
{
    using Problem = remove_cvref_t<Problem_>;

    using IndexType  = typename Problem::IndexType;
    using WeightType = typename Problem::WeightType;

    typedef MoeSortingHostArgs MoeSortingKargs;

    using Hargs = MoeSortingHostArgs;

    struct Kargs
    {
        const void* p_topk_ids;
        const void* p_weights;
        void* p_sorted_token_ids;
        void* p_sorted_weights;
        void* p_sorted_expert_ids;
        void* p_total_tokens_post_pad;
        void* p_moe_buf;
        index_t tokens;
        index_t num_experts;
        index_t moe_buf_bytes;

        index_t tokens_per_thread;
        mdiv unit_size_mdiv;
        mdiv topk_mdiv;
    };

    CK_TILE_HOST static constexpr auto GridSize(const Hargs& h)
    {
        // TODO: assume num-experts not too much
        return dim3(1 + ck_tile::integer_divide_ceil(h.moe_buf_bytes, BlockSize(h).x * 16));
    }

    CK_TILE_HOST static constexpr auto BlockSize(const Hargs& h)
    {
        return dim3(ck_tile::integer_least_multiple(h.num_experts, ck_tile::get_warp_size()));
    }

    // in byte
    CK_TILE_HOST static constexpr auto GetSmemSize(const Hargs& h)
    {
        const auto blocks = BlockSize(h);
        return ((blocks.x + 1) * h.num_experts + (h.num_experts + 1)) * sizeof(index_t);
    }

    CK_TILE_HOST static constexpr auto MakeKargs(const Hargs& h)
    {
        Kargs k;
        k.p_topk_ids              = h.p_topk_ids;
        k.p_weights               = h.p_weights;
        k.p_sorted_token_ids      = h.p_sorted_token_ids;
        k.p_sorted_weights        = h.p_sorted_weights;
        k.p_sorted_expert_ids     = h.p_sorted_expert_ids;
        k.p_moe_buf               = h.p_moe_buf;
        k.p_total_tokens_post_pad = h.p_total_tokens_post_pad;
        k.tokens                  = h.tokens;
        k.num_experts             = h.num_experts;
        k.moe_buf_bytes           = h.moe_buf_bytes;

        const auto blocks   = BlockSize(h);
        k.tokens_per_thread = integer_divide_ceil(h.tokens * h.topk, blocks.x);
        k.unit_size_mdiv    = mdiv{static_cast<uint32_t>(h.unit_size)};
        k.topk_mdiv         = mdiv{static_cast<uint32_t>(h.topk)};
        return k;
    }

    CK_TILE_DEVICE index_t calc_index(index_t total_col, index_t row, index_t col) const
    {
        return row * total_col + col;
    }

    CK_TILE_DEVICE void moe_buf_set_zero_kernel(uint8x16_t* buf, index_t buf_bytes) const
    {
        const index_t offset = (blockIdx.x - 1) * blockDim.x + threadIdx.x;
        if(offset < buf_bytes / 16)
        {
            buf[offset] = uint8x16_t{0};
        }
    }

    CK_TILE_DEVICE void moe_align_block_size_kernel(const IndexType* __restrict__ topk_id,
                                                    const WeightType* __restrict__ weights,
                                                    index_t* p_sorted_token_ids,
                                                    WeightType* p_sorted_weights,
                                                    index_t* p_sorted_expert_ids,
                                                    index_t* p_total_tokens_post_pad,
                                                    const index_t num_experts,
                                                    const index_t tokens_per_thread,
                                                    const index_t numel,
                                                    const mdiv unit_size_mdiv,
                                                    const mdiv topk_mdiv,
                                                    void* smem) const
    {
        const index_t tid       = static_cast<index_t>(threadIdx.x);
        const index_t start_idx = tid * tokens_per_thread;

        index_t* shared_mem = reinterpret_cast<index_t*>(smem);

        index_t* tokens_cnts = shared_mem; // 2d: (blockDim.x + 1, num_experts)
        index_t* cumsum      = shared_mem + (blockDim.x + 1) * num_experts; // 1: (num_experts + 1)
        for(int i = 0; i < num_experts; ++i)
        {
            tokens_cnts[calc_index(num_experts, tid + 1, i)] = 0;
        }
#pragma unroll Problem_::InternalLoadUnroll
        for(int i = start_idx; i < numel && i < start_idx + tokens_per_thread; ++i)
        {
            ++tokens_cnts[calc_index(num_experts, tid + 1, topk_id[i])];
        }
        __syncthreads();

        if(tid < num_experts)
        {
            tokens_cnts[calc_index(num_experts, 0, tid)] = 0;
            for(int i = 1; i <= static_cast<index_t>(blockDim.x); ++i)
            {
                tokens_cnts[calc_index(num_experts, i, tid)] +=
                    tokens_cnts[calc_index(num_experts, i - 1, tid)];
            }
        }

        // __syncthreads();
        if(tid == 0)
        {
            cumsum[0] = 0;
            for(int i = 1; i <= num_experts; ++i)
            {
                auto current_units = [&]() {
                    index_t x_ = tokens_cnts[calc_index(num_experts, blockDim.x, i - 1)] +
                                 unit_size_mdiv.divisor - 1;
                    index_t y_ = unit_size_mdiv.div(x_);
                    return max(y_, 1) * unit_size_mdiv.divisor;
                }();
                cumsum[i] = cumsum[i - 1] + current_units;
            }
            *p_total_tokens_post_pad = cumsum[num_experts];
        }
        __syncthreads();
        if(tid < num_experts)
        {
            for(int i = cumsum[tid]; i < cumsum[tid + 1]; i += unit_size_mdiv.divisor)
            {
                p_sorted_expert_ids[unit_size_mdiv.div(i)] = tid;
            }
        }

#pragma unroll Problem_::InternalLoadUnroll
        for(int i = start_idx; i < numel && i < start_idx + tokens_per_thread; ++i)
        {
            index_t expert_id = topk_id[i];
            index_t rank_post_pad =
                tokens_cnts[calc_index(num_experts, tid, expert_id)] + cumsum[expert_id];
#if CK_TILE_REFERENCE_MOE_SORTING_MOCK_ID
            uint32_t curr_token_id, curr_topk_id;
            topk_mdiv.divmod(i, curr_token_id, curr_topk_id);
            p_sorted_token_ids[rank_post_pad] = MOE_SORTING_MOCK_ID(curr_token_id, curr_topk_id);
#else
            p_sorted_token_ids[rank_post_pad] = topk_mdiv.div(i);
#endif
            p_sorted_weights[rank_post_pad] = weights[i];
            ++tokens_cnts[calc_index(num_experts, tid, expert_id)];
        }

        const index_t prefill_token = topk_mdiv.div(numel);
        if(tid < num_experts)
        {
            index_t expert_offset =
                cumsum[tid] + tokens_cnts[calc_index(num_experts, blockDim.x, tid)];
            while(expert_offset < cumsum[tid + 1])
            {
#if CK_TILE_REFERENCE_MOE_SORTING_MOCK_ID
                p_sorted_token_ids[expert_offset] =
                    MOE_SORTING_MOCK_ID(prefill_token, topk_mdiv.divisor);
#else
                p_sorted_token_ids[expert_offset] = prefill_token;
#endif
                p_sorted_weights[expert_offset] = static_cast<WeightType>(0.0);
                expert_offset++;
            }
        }
    }

    CK_TILE_DEVICE void operator()(Kargs kargs) const
    {
        if(blockIdx.x > 0)
        {
            if(kargs.p_moe_buf)
            {
                moe_buf_set_zero_kernel(reinterpret_cast<uint8x16_t*>(kargs.p_moe_buf),
                                        kargs.moe_buf_bytes);
            }
            return;
        }
        const size_t numel = kargs.tokens * kargs.topk_mdiv.divisor;
        extern __shared__ char smem[];
        return moe_align_block_size_kernel(static_cast<const IndexType*>(kargs.p_topk_ids),
                                           static_cast<const WeightType*>(kargs.p_weights),
                                           static_cast<IndexType*>(kargs.p_sorted_token_ids),
                                           static_cast<WeightType*>(kargs.p_sorted_weights),
                                           static_cast<IndexType*>(kargs.p_sorted_expert_ids),
                                           static_cast<IndexType*>(kargs.p_total_tokens_post_pad),
                                           kargs.num_experts,
                                           kargs.tokens_per_thread,
                                           numel,
                                           kargs.unit_size_mdiv,
                                           kargs.topk_mdiv,
                                           smem);
    }
};

#undef MOE_SORTING_MOCK_ID

} // namespace ck_tile
