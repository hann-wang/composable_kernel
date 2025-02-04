// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2024, Advanced Micro Devices, Inc. All rights reserved.

#pragma once

namespace ck {

enum struct DataTypeEnum
{
    Half     = 0,
    Float    = 1,
    Int32    = 2,
    Int8     = 3,
    Int8x4   = 4,
    BFloat16 = 5,
    Double   = 6,
    Float8   = 7,
    Unknown  = 100,
};

} // namespace ck
