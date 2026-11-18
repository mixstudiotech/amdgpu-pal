// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "palDevice.h"
#include "wddmProperties.h"

namespace Pal::Wddm
{
class Transport;
struct ApplicationProfileEscapeCalls;
uint32 DecodeWorkstationCapabilities(const PhysicalProperties& properties);
Result QueryWorkstationCapabilities(const Transport& transport, WorkStationCaps* caps);
Result QueryWorkstationStereoModeRecord(const ApplicationProfileEscapeCalls& route, WorkstationStereoMode* mode);
Result QueryWorkstationStereoMode(const Transport& transport, WorkstationStereoMode* mode);
}
