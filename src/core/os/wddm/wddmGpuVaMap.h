// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"
#include "core/os/wddm/wddmTransport.h"
#include "core/os/wddm/wddmGpuVaMapPacket.h"

namespace Pal::Wddm
{
struct GpuVaMapRange { uint64_t base=0, size=0; };
struct GpuVaMapPolicyState
{
    uint32_t pagingQueue=0;
    const uint32_t* allocations=nullptr;
    uint32_t allocationCount=0;
    uint64_t size=0, gpuAddress=0, gpuvmOffsetForSvm=0;
    uint32_t partition=0;
    GpuVaMapRange primary, defaultBackup;
    bool defaultVaRangeSplit=false, readOnly=false;
    uint64_t recordedPagingFence=0;
};
struct GpuVaMapPolicyCallbacks
{
    void* context=nullptr;
    Status (*map)(void*,GpuVaMapPacket*)=nullptr;
    Result (*waitPaging)(void*,uint64_t)=nullptr;
};
// Caller constructs valid ranges/chunks. No residency, quarantine or NTSTATUS
// aliasing is added. This policy remains separate from Status transport APIs.
Result MapGpuVaPolicy(GpuVaMapPolicyState* state,uint64_t logicalHint,uint64_t* callerPagingFence,
                      bool waitForPaging,const GpuVaMapPolicyCallbacks& callbacks);
}
