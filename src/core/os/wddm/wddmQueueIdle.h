// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"
#include <cstdint>

namespace Pal::Wddm
{
// Native context readers 383890 (legacy) and 37a150 (HWS) observe the low DWORD.
inline constexpr uint64_t NativeCompletedTimestamp(uint64_t lastSubmitted, uint32_t completedLow)
{
    const uint64_t candidate = (lastSubmitted & 0xffffffff00000000ull) | completedLow;
    return candidate > lastSubmitted ? candidate - 0x100000000ull : candidate;
}
inline constexpr bool NativeTimestampRetired(uint64_t lastSubmitted, uint32_t completedLow, uint64_t requested)
{ return NativeCompletedTimestamp(lastSubmitted, completedLow) >= requested; }

// Native 300ba0, including the separate page-fault result at ordinal7.
inline constexpr Result ConvertExecutionOrdinal(uint32_t ordinal)
{
    switch (ordinal)
    {
    case 1: return Result::Success;
    case 2:
    case 3:
    case 4:
    case 6: return Result::ErrorDeviceLost;
    case 5: return Result::ErrorOutOfGpuMemory;
    case 7: return Result::ErrorGpuPageFaultDetected;
    default: return Result::ErrorUnknown;
    }
}
// The original idle poll queries only EXECUTION and does not quarantine.
Result CheckQueueExecutionRequest(uint32_t device);
// Diagnostics only (PAL_WDDM_TRACE=1, first call of the process): D3DKMT_DEVICESTATE_PAGE_FAULT of a device whose
// execution state is DMA page fault, with the faulting GPU virtual address.
void TraceDevicePageFault(uint32_t device, const char* where);

struct QueueIdleCallbacks
{
    void* userData = nullptr;
    const uint64_t* lastTimestamp = nullptr;
    bool (*isRetired)(void*, uint64_t) = nullptr;
    Result (*checkExecution)(void*) = nullptr;
    uint64_t (*frequency)(void*) = nullptr;
    uint64_t (*counter)(void*) = nullptr;
    void (*yield)(void*) = nullptr;
};

// Native 4d8d80: gfxTimeout seconds, 0=infinite; health checks every 100ms.
// Returns the native completion/health/timeout result without quarantine policy.
// Null lastTimestamp represents a null submission context. Its live value is
// reread for every retirement check. Clock failures produce zero, as in the
// native zero-initialized LARGE_INTEGER output; no clock error is propagated.
Result WaitQueueIdle(const QueueIdleCallbacks& callbacks, uint32_t gfxTimeoutSeconds);
}
