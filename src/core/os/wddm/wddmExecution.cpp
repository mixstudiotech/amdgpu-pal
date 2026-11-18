// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmExecution.h"
#include "core/os/wddm/wddmQueueIdle.h"
#include "core/os/wddm/wddmResult.h"
#include "palDevice.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
Result QueryDeviceExecution(uint32 device, PageFaultStatus* fault)
{
    const Result result = CheckQueueExecutionRequest(device);
    if ((result == Result::ErrorGpuPageFaultDetected) && (fault != nullptr))
    {
        D3DKMT_GETDEVICESTATE request = {};
        request.hDevice = device;
        request.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
        const Result details = ConvertNativeStatus(D3DKMTGetDeviceState(&request));
        if (details == Result::Success)
        {
            fault->flags.pageFault = 1;
            fault->flags.readFault = (request.PageFaultState.PageFaultFlags & 1) == 0;
            fault->faultAddress = request.PageFaultState.FaultedVirtualAddress;
        }
        else
        {
            fault->flags.pageFault = 0;
        }
    }
    return result;
}
}
