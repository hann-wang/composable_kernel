// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2024, Advanced Micro Devices, Inc. All rights reserved.

#pragma once

#include "ck_tile/core.hpp"
#include <string>
#include <type_traits>

namespace ck_tile {

template <typename IndexType_, typename WeightType_, index_t InternalLoadUnroll_>
struct MoeSortingProblem
{
    // TODO: this kernel only support warp per row
    using WeightType = remove_cvref_t<WeightType_>;
    using IndexType  = remove_cvref_t<IndexType_>;

    static constexpr index_t WarpSize           = get_warp_size();
    static constexpr index_t WarpsPerBlock      = 1;
    static constexpr index_t InternalLoadUnroll = InternalLoadUnroll_;
};
} // namespace ck_tile
