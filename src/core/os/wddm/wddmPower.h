// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "palDevice.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace Pal::Wddm
{
class Transport;
struct PowerRequestTrace
{
    std::array<uint8_t, 0xe8> request = {};
    int32_t ntStatus = 0;
    uint32 privateResult = 0;
    bool responseCopied = false;
};
// Internal diagnostic replay of a bounded native-built power record. The same
// real KMT execution path is used by product helpers; no driver-call substitute.
Result ReplayNativePowerRecord(const Transport& transport, void* record, size_t bytes,
                              PowerRequestTrace* trace = nullptr);
Result ApplyPowerProfile(const Transport& transport, PowerProfile profile, CustomPowerProfile* info,
                         PowerRequestTrace* trace = nullptr);
Result ApplyMlPowerOptimization(const Transport& transport, bool enableOptimization,
                                PowerRequestTrace* trace = nullptr);
}
