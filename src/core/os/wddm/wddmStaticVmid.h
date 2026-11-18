// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"
#include <array>
#include <cstdint>

namespace Pal::Wddm
{
class Transport;
using StaticVmidRecord = std::array<uint8_t, 264>;
StaticVmidRecord MakeStaticVmidRecord(bool enable);
Result DecodeStaticVmidResult(Result escapeResult, const StaticVmidRecord& response);
Result ReplayStaticVmidRecord(const Transport& transport, StaticVmidRecord* record);
Result ApplyStaticVmid(const Transport& transport, bool enable);
}
