// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmCpuWait.h"
#include "core/os/wddm/wddmResult.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <cfloat>

namespace Pal::Wddm
{
namespace
{
void* Create(void*, const void* attributes, uint32_t manualReset, uint32_t initialState, const wchar_t* name)
{
    return CreateEventW(static_cast<SECURITY_ATTRIBUTES*>(const_cast<void*>(attributes)),
                        manualReset, initialState, name);
}
void Close(void*, void* event) { CloseHandle(event); }

#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif
Result RegisterAndWait(uint32_t device, const uint32_t* handles, const uint64_t* values,
                        uint32_t count, bool waitAny, int64_t nanoseconds, bool infinite,
                        const CpuWaitCallbacks* callbacks)
{
    const auto create = ((callbacks != nullptr) && (callbacks->createEvent != nullptr)) ? callbacks->createEvent : Create;
    const auto close = ((callbacks != nullptr) && (callbacks->closeEvent != nullptr)) ? callbacks->closeEvent : Close;
    void* userData = (callbacks != nullptr) ? callbacks->userData : nullptr;
    SECURITY_ATTRIBUTES attributes = {};
    void* event = create(userData, &attributes, FALSE, FALSE, nullptr);
    if (event == nullptr) { return Result::ErrorInitializationFailed; }
    const Status status = RegisterCpuWaitRequest(device, handles, values, count, waitAny, event, callbacks);
    Result result = ConvertNativeStatus(status);
    if (result == Result::Success)
    {
        // infinite is a separate public API shim. The native finite wrapper has
        // no INT64_MAX special case and performs both float rounding steps.
        const float seconds = infinite ? FLT_MAX : static_cast<float>(nanoseconds) / 1'000'000'000.0f;
        result = WaitEvent(event, seconds, (callbacks != nullptr) ? &callbacks->event : nullptr);
    }
    close(userData, event);
    return result;
}
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
}

Status RegisterCpuWaitRequest(uint32_t device, const uint32_t* handles, const uint64_t* values,
                              uint32_t count, bool waitAny, void* event, const CpuWaitCallbacks* callbacks)
{
    static_assert(sizeof(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU) == 40);
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU request = {};
    request.hDevice = device;
    request.ObjectCount = count;
    request.ObjectHandleArray = handles;
    request.FenceValueArray = values;
    request.hAsyncEvent = event;
    request.Flags.WaitAny = waitAny;
    return ((callbacks != nullptr) && (callbacks->registerWait != nullptr)) ?
        callbacks->registerWait(callbacks->userData, &request) : D3DKMTWaitForSynchronizationObjectFromCpu(&request);
}
Result WaitCpuPayload(uint32_t device, uint32_t handle, const volatile uint64_t& completed,
                       uint64_t target, int64_t nanoseconds, bool infinite, const CpuWaitCallbacks* callbacks)
{
    if (completed >= target) { return Result::Success; }
    return RegisterAndWait(device, &handle, &target, 1, false, nanoseconds, infinite, callbacks);
}
Result WaitCpuPayloads(uint32_t device, const uint32_t* handles, const uint64_t* values,
                        uint32_t count, bool waitAny, int64_t nanoseconds, bool infinite, const CpuWaitCallbacks* callbacks)
{
    if (count == 0) { return Result::ErrorInvalidValue; }
    if ((handles == nullptr) || (values == nullptr)) { return Result::ErrorInvalidPointer; }
    return RegisterAndWait(device, handles, values, count, waitAny, nanoseconds, infinite, callbacks);
}
}
