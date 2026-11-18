// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"
#include "core/os/wddm/wddmTransport.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>

namespace Pal::Wddm
{
inline void TraceResult(Result result, const char* operation)
{
    static const bool enabled = std::getenv("PAL_WDDM_TRACE") != nullptr;
    if (enabled) { std::fprintf(stderr,"[pal-wddm] %s: PAL=%d\n",operation,static_cast<int>(result)); }
}
// Exact leaf translator at original RVA2d07c0..2d08c1. Positive NTSTATUS
// values are not interchangeable with PAL Success (notably STATUS_PENDING).
inline Result ConvertNativeStatus(Status status)
{
    switch (uint32_t(status))
    {
    case 0:          return Result::Success;
    case 0x102:      return Result::Timeout;
    case 0x103:      return Result::NotReady;
    case 0xc0000001: return Result::ErrorUnavailable;
    case 0xc0000002: return Result::Unsupported;
    case 0xc000000d: return Result::ErrorInvalidValue;
    case 0xc0000017: return Result::ErrorOutOfMemory;
    case 0xc0000034: return Result::NotFound;
    case 0xc00002b6: return Result::ErrorDeviceLost;
    case 0xc01e0000:
    case 0xc01e0005:
    case 0xc01e0342: return Result::ErrorFullscreenUnavailable;
    case 0xc01e0006: return Result::PresentOccluded;
    case 0xc01e0100: return Result::ErrorOutOfGpuMemory;
    case 0xc01e0102: return Result::ErrorGpuMemoryMapFailed;
    case 0xc01e0304:
    case 0xc01e0333: return Result::ErrorScreenRemoved;
    default:         return Result::ErrorUnknown;
    }
}
inline Result ConvertStatus(Status status, const char* operation = "KMT")
{
    static const bool enabled = std::getenv("PAL_WDDM_TRACE") != nullptr;
    static std::atomic<uint32_t> messages = 0;
    if (enabled && (status != Success) && (status != Timeout))
    {
        const uint32_t message = messages.fetch_add(1,std::memory_order_relaxed);
        if (message < 32) { std::fprintf(stderr,"[pal-wddm] %s: NTSTATUS=%08x\n",operation,uint32_t(status)); }
        else if (message == 32) { std::fputs("[pal-wddm] further native status messages suppressed\n",stderr); }
    }
    // NotSupported is also the transport's explicit preflight rejection code.
    // This bridge is separate from the original leaf's exact native-status map.
    if (status == NotSupported) { return Result::Unsupported; }
    return ConvertNativeStatus(status);
}
}
