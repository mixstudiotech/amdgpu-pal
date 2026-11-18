// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"
#include <cstdint>

namespace Pal::Wddm
{
// Original pure Win32-to-PAL result translator368510. Keep this result domain
// separate from D3DKMT NTSTATUS, particularly for successful warning values.
inline constexpr Result ConvertWin32Error(uint32_t error)
{
    switch (error)
    {
    case 0: return Result::Success;
    case 2: case 3: case 123: return Result::NotFound;
    case 4: case 223: case 225: case 226: return Result::ErrorUnavailable;
    case 5: return Result::ErrorPermissionDenied;
    case 6: case 111: return Result::ErrorInvalidValue;
    case 8: case 14: return Result::ErrorOutOfMemory;
    case 21: case 170: return Result::NotReady;
    case 32: case 33: return Result::ErrorNotShareable;
    case 39: case 112: return Result::ErrorDiskFull;
    case 80: case 183: return Result::AlreadyExists;
    case 258: return Result::Timeout;
    default: return Result::ErrorUnknown;
    }
}

struct EventWaitCallbacks
{
    void* userData = nullptr;
    uint32_t (*wait)(void*, void* event, uint32_t milliseconds) = nullptr;
    uint32_t (*lastError)(void*) = nullptr;
};

// Original54e690 event boundary. Event ownership stays with the caller. Seconds
// is the original float input: FLT_MAX selects infinity, negative/NaN is invalid.
// Other out-of-range conversion follows x64 CVTTSS2SI64 and DWORD narrowing.
Result WaitEvent(void* event, float seconds, const EventWaitCallbacks* callbacks = nullptr);
}
