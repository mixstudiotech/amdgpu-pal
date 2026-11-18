// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Pal::Wddm
{
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif
inline uint32_t ConvertTimerDelay(float microseconds, bool microsecondUnits)
{
    // Native38b5b0 divides in float, ceilf, then CVTTSS2SI64 and DWORD narrowing.
    // The x64 indefinite integer for an out-of-range/NaN conversion has lowDWORD0.
    const float units = microsecondUnits ? microseconds : microseconds / 1000.0f;
    const float ticks = std::ceil(units);
    constexpr float SignedLimit = 9223372036854775808.0f;
    if (!(ticks < SignedLimit) || (ticks < -SignedLimit)) { return 0; }
    return static_cast<uint32_t>(static_cast<int64_t>(ticks));
}
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
// 38b5b0: +28 is the private screen's index (VidPN source) and +32 bit0 the after-vsync request for DelayAfterVsync.
inline std::array<uint8_t, 80> TimerDelayRecord(float microseconds, bool microsecondUnits, uint32_t version,
                                                uint32_t sourceId = 0, bool afterVsync = false)
{
    std::array<uint8_t, 80> record = {};
    const auto put = [&](size_t offset, uint32_t value) { std::memcpy(record.data() + offset, &value, sizeof(value)); };
    put(0, version);
    put(4, 1);
    put(8, 4);
    put(12, 72);
    put(16, 0x2fc00u | uint32_t(microsecondUnits));
    put(24, ConvertTimerDelay(microseconds, microsecondUnits));
    put(28, sourceId);
    put(32, afterVsync ? 1u : 0u);
    return record;
}
}
