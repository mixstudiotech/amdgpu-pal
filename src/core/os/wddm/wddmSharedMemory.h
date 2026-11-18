// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

namespace Pal::Wddm
{
// Recovered v2/v8/v9 AMD buffer-object fields. Heap IDs retain the native encoding:
// 1=CPU-visible local, 2=invisible local, 3=GART USWC, 4=GART cacheable.
struct SharedAllocationProperties
{
    uint64_t size = 0;
    uint64_t clientSize = 0;
    uint64_t uniqueId = 0;
    uint32_t alignment = 0;
    uint32_t heapCount = 0;
    uint8_t heapIds[4] = {};
    bool linearBuffer = false;
};
bool ParseSharedAllocationProperties(const uint8_t* records, size_t bytes,
                                     SharedAllocationProperties* properties);
}

