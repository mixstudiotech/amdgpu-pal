// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmEventWait.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <immintrin.h>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
uint32_t Wait(void*, void* event, uint32_t milliseconds)
{ return WaitForSingleObject(event, milliseconds); }
uint32_t LastError(void*) { return GetLastError(); }
}
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif
Result WaitEvent(void* event, float seconds, const EventWaitCallbacks* callbacks)
{
    const __m128 input = _mm_set_ss(seconds);
    // COMISS also preserves the original invalid-operation flag for NaNs.
    if (_mm_comigt_ss(_mm_setzero_ps(), input)) { return Result::ErrorInvalidValue; }
    uint32_t bits = 0;
    std::memcpy(&bits, &seconds, sizeof(bits));
    if (((bits & 0x7f800000u) == 0x7f800000u) && ((bits & 0x7fffffu) != 0))
        { return Result::ErrorInvalidValue; }
    const uint32_t milliseconds = (bits == 0x7f7fffffu) ? UINT32_MAX :
        uint32_t(_mm_cvttss_si64(_mm_mul_ss(input, _mm_set_ss(1000.0f))));
    const auto wait = ((callbacks != nullptr) && (callbacks->wait != nullptr)) ? callbacks->wait : Wait;
    const auto lastError = ((callbacks != nullptr) && (callbacks->lastError != nullptr)) ? callbacks->lastError : LastError;
    void* userData = (callbacks != nullptr) ? callbacks->userData : nullptr;
    switch (wait(userData, event, milliseconds))
    {
    case WAIT_OBJECT_0: return Result::Success;
    case WAIT_ABANDONED: return Result::Aborted;
    case WAIT_TIMEOUT: return Result::Timeout;
    case WAIT_FAILED: return ConvertWin32Error(lastError(userData));
    default: return Result::ErrorUnknown;
    }
}
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
}
