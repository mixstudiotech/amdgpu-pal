// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmExecution.h"

namespace Pal::Wddm
{
Result Device::CheckExecutionState(PageFaultStatus *fault)
{
    return QueryDeviceExecution(m_transport.DeviceHandle(), fault);
}
} // namespace Pal::Wddm
