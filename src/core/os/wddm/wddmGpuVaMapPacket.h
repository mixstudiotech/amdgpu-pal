// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pal::Wddm
{
// Native MAPGPUVA104 record, including every input, output and padding byte.
struct GpuVaMapPacket
{
    uint32_t pagingQueue=0, padding0=0;
    uint64_t base=0, minimum=0, maximum=0;
    uint32_t allocation=0, padding1=0;
    uint64_t offsetInPages=0, sizeInPages=0, protection=0, driverProtection=0;
    uint32_t reserved0=0, padding2=0;
    uint64_t reserved1=0, virtualAddress=0, pagingFence=0;
};
static_assert(std::is_standard_layout_v<GpuVaMapPacket> && sizeof(GpuVaMapPacket)==104);
static_assert(offsetof(GpuVaMapPacket,base)==8 && offsetof(GpuVaMapPacket,allocation)==32 &&
              offsetof(GpuVaMapPacket,virtualAddress)==88 && offsetof(GpuVaMapPacket,pagingFence)==96);
}
