// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"

namespace Pal { struct PageFaultStatus; }
namespace Pal::Wddm
{
// Native30fd60 preserves the caller's output unless an execution page fault
// requires a detail query. This request boundary does not quarantine a device.
Result QueryDeviceExecution(uint32 device, PageFaultStatus* fault);
}
