// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "palDevice.h"

namespace Pal::Wddm
{
class Transport;

// Recovered Windows PAL clock query for the verified AMD UMD/KMD profile.
// Like the original method, only GPU/QPC/maxDeviation fields are committed;
// the two POSIX clock fields retain the caller's values.
Result QueryCalibratedTimestamps(const Transport& transport, CalibratedTimestamps* timestamps);
}
