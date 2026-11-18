// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "palDevice.h"

namespace Pal::Wddm
{
class Transport;
enum class ClockModePolicy : uint32
{
    AllowChanges = 0,
    QueryOnly = 1,
    IgnoreAll = 2, // Recovered legacy native setting; current PAL bool expresses0/1.
};

// On query success, returns live native limits. On query failure, supplies the
// recovered GPU-record fallback limits and returns that failure to the caller.
// Native initialization retains those fallback values instead of failing init.
Result QueryClockLimits(const Transport& transport, GpuType gpuType, SetClockModeOutput* output);

// Pure native reply conversion, separated for bounded arithmetic differential tests.
SetClockModeOutput DecodeClockModeReply(uint32 memoryRatio, uint32 engineRatio,
                                       uint32 absoluteMemoryMHz, uint32 absoluteEngineMHz,
                                       uint32 maxMemoryMHz, uint32 maxEngineMHz);

Result ApplyClockMode(const Transport& transport, const SetClockModeInput& input,
                     SetClockModeOutput* output, ClockModePolicy policy,
                     uint32 maxMemoryClockMHz, uint32 maxEngineClockMHz);
}
