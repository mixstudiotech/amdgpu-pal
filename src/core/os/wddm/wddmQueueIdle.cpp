// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmQueueIdle.h"
#include "core/os/wddm/wddmResult.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
namespace
{
uint64_t Frequency(void*)
{
    LARGE_INTEGER value = {};
    QueryPerformanceFrequency(&value);
    return uint64_t(value.QuadPart);
}
uint64_t Counter(void*)
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return uint64_t(value.QuadPart);
}
void YieldThread(void*) { Sleep(0); }
}
Result CheckQueueExecutionRequest(uint32_t device)
{
    D3DKMT_GETDEVICESTATE request = {};
    request.hDevice = device;
    request.StateType = D3DKMT_DEVICESTATE_EXECUTION;
    const Result result = ConvertNativeStatus(D3DKMTGetDeviceState(&request));
    if ((result == Result::Success) && (request.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT))
    {
        TraceDevicePageFault(device, "CheckQueueExecutionRequest");
    }
    return result == Result::Success ? ConvertExecutionOrdinal(uint32_t(request.ExecutionState)) : result;
}

void TraceDevicePageFault(uint32_t device, const char* where)
{
    // Diagnostics only (PAL_WDDM_TRACE): the first DMA page fault of the process with the faulting GPU address.
    static const bool trace = GetEnvironmentVariableW(L"PAL_WDDM_TRACE", nullptr, 0) != 0;
    static volatile LONG reported = 0;
    if (!trace || (InterlockedExchange(&reported, 1) != 0)) { return; }
    D3DKMT_GETDEVICESTATE request = {};
    request.hDevice = device;
    request.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
    const NTSTATUS status = D3DKMTGetDeviceState(&request);
    const D3DKMT_DEVICEPAGEFAULT_STATE& fault = request.PageFaultState;
    std::fprintf(stderr, "[pal-wddm] PAGE_FAULT where=%s query=%08lx va=%llx flags=%x error=%x:%x stage=%u bind=%u "
                 "sequence=%llx\n", where, static_cast<unsigned long>(status),
                 static_cast<unsigned long long>(fault.FaultedVirtualAddress), uint32_t(fault.PageFaultFlags),
                 uint32_t(fault.FaultErrorCode.IsDeviceSpecificCode), uint32_t(fault.FaultErrorCode.GeneralErrorCode),
                 uint32_t(fault.FaultedPipelineStage), fault.FaultedBindTableEntry,
                 static_cast<unsigned long long>(fault.FaultedPrimitiveAPISequenceNumber));
    std::fflush(stderr);
}
Result WaitQueueIdle(const QueueIdleCallbacks& callbacks, uint32_t gfxTimeoutSeconds)
{
    if ((callbacks.lastTimestamp == nullptr) || (*callbacks.lastTimestamp == 0)) { return Result::Success; }
    const auto frequency = callbacks.frequency != nullptr ? callbacks.frequency : Frequency;
    const auto counter = callbacks.counter != nullptr ? callbacks.counter : Counter;
    const auto yield = callbacks.yield != nullptr ? callbacks.yield : YieldThread;
    const uint64_t ticksPerSecond = frequency(callbacks.userData);
    const uint64_t interval = (ticksPerSecond * 100) / 1000;
    const uint64_t start = counter(callbacks.userData);
    uint64_t nextHealthCheck = start + interval;
    const uint64_t deadline = start + uint64_t(gfxTimeoutSeconds) * ticksPerSecond;
    if (callbacks.isRetired(callbacks.userData, *callbacks.lastTimestamp)) { return Result::Success; }
    for (;;)
    {
        const uint64_t now = counter(callbacks.userData);
        if (now >= nextHealthCheck)
        {
            const Result result = callbacks.checkExecution(callbacks.userData);
            if (result != Result::Success) { return result; }
            nextHealthCheck = now + interval;
        }
        if ((gfxTimeoutSeconds != 0) && (now >= deadline)) { return Result::Timeout; }
        yield(callbacks.userData);
        if (callbacks.isRetired(callbacks.userData, *callbacks.lastTimestamp)) { return Result::Success; }
    }
}
}
