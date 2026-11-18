// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace Pal::Wddm
{
// The native semaphore wrappers convert signed nanoseconds to float seconds; their
// event helper multiplies by float 1000 and truncates to a Win32 DWORD. Keep each
// rounding step even when the rest of PAL is built with relaxed floating arithmetic.
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif
inline bool ConvertWaitTimeout(uint64_t nanoseconds, uint32_t* milliseconds)
{
    if (milliseconds == nullptr) { return false; }
    if (nanoseconds == UINT64_MAX) { *milliseconds = UINT32_MAX; return true; }
    if (nanoseconds > uint64_t(INT64_MAX)) { return false; }
    const float seconds = static_cast<float>(static_cast<int64_t>(nanoseconds)) / 1'000'000'000.0f;
    const float elapsedMilliseconds = seconds * 1000.0f;
    *milliseconds = static_cast<uint32_t>(static_cast<int64_t>(elapsedMilliseconds));
    return true;
}
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
// Event-Fence wait-many has a different native contract: signed integer division,
// DWORD narrowing and the signed nanoseconds maximum as its infinite sentinel.
inline constexpr uint32_t ConvertFenceWaitTimeout(int64_t nanoseconds)
{
    return (nanoseconds == INT64_MAX) ? UINT32_MAX :
        static_cast<uint32_t>(nanoseconds / 1'000'000);
}
}
