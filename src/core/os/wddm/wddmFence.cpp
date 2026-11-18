// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmFence.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmResult.h"
#include "core/os/wddm/wddmTimeout.h"
#include "core/platform.h"
#include "palAutoBuffer.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Pal::Wddm
{
namespace
{
Result EventError(DWORD error)
{
    // Native Win32 error translator RVA 0x368510, including its jump-table cases.
    switch (error)
    {
    case ERROR_SUCCESS: return Result::Success;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: return Result::NotFound;
    case ERROR_ACCESS_DENIED: return Result::ErrorPermissionDenied;
    case ERROR_INVALID_HANDLE:
    case ERROR_BUFFER_OVERFLOW: return Result::ErrorInvalidValue;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return Result::ErrorOutOfMemory;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return Result::ErrorNotShareable;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return Result::AlreadyExists;
    case ERROR_NOT_READY:
    case ERROR_BUSY: return Result::NotReady;
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_FULL: return Result::ErrorDiskFull;
    case ERROR_TOO_MANY_OPEN_FILES:
    case ERROR_FILE_TOO_LARGE:
    case ERROR_VIRUS_INFECTED:
    case ERROR_VIRUS_DELETED: return Result::ErrorUnavailable;
    case WAIT_TIMEOUT: return Result::Timeout;
    default: return Result::ErrorUnknown;
    }
}
DWORD WaitMilliseconds(std::chrono::nanoseconds timeout)
{
    return ConvertFenceWaitTimeout(timeout.count());
}
}
Fence::Fence(Device*) : m_event(nullptr), m_isReference(false) {}
Fence::~Fence()
{
    if ((m_event != nullptr) && !m_isReference) { CloseHandle(m_event); }
    m_event = nullptr;
}
Result Fence::Init(const FenceCreateInfo& info)
{
    // Native slot 5 (3850c0, inline in Device slot 114 3115f0): a manual-reset event, optionally named.
    m_fenceState.initialSignalState = info.flags.signaled;
    if (m_event != nullptr) { return Result::Success; }
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = info.flags.eventCanBeInherited;
    m_event = CreateEventW(&attributes, TRUE, info.flags.signaled, info.pName);
    return (m_event != nullptr) ? Result::Success : Result::ErrorInitializationFailed;
}
Result Fence::GetStatus() const
{
    const DWORD result = WaitForSingleObject(m_event, 0);
    if (result == WAIT_OBJECT_0) { return Result::Success; }
    if (result == WAIT_TIMEOUT)
    {
        return WasNeverSubmitted() ? Result::ErrorFenceNeverSubmitted : Result::NotReady;
    }
    if (result == WAIT_FAILED) { return EventError(GetLastError()); }
    return Result::ErrorUnknown;
}
Result Fence::OpenHandle(const FenceOpenInfo& info)
{
    // Native slot 6 (385380, inline in Device slot 115 3116e0): a supplied event is borrowed for reference opens and
    // owned otherwise; without one a named event is opened (or created) unsignaled with default attributes. Neither
    // handle nor name leaves the fence without an event, as in the original.
    Result result = Result::Success;
    if (info.externalFence != nullptr)
    {
        m_event = info.externalFence;
        m_isReference = info.flags.isReference;
    }
    else if ((info.pName != nullptr) && (m_event == nullptr))
    {
        SECURITY_ATTRIBUTES attributes = {};
        m_event = CreateEventW(&attributes, TRUE, FALSE, info.pName);
        if (m_event == nullptr) { result = Result::ErrorInitializationFailed; }
    }
    m_fenceState.isOpened = 1;
    return result;
}
OsExternalHandle Fence::ExportExternalHandle(const FenceExportInfo& info) const
{
    // Native slot 4 (385410): the event handle itself, whatever the export flags; the fence keeps ownership.
    return m_event;
}
void Fence::AssociateWithContext(Pal::SubmissionContext*)
{
    m_fenceState.neverSubmitted = 0;
}
Result Fence::Signal(Queue& queue)
{
    auto* device = static_cast<Device*>(queue.GetDevice());
    const Result result = ConvertStatus(device->OsTransport().RegisterQueueEvent(queue.NativeQueue(), m_event));
    if (result == Result::Success) { m_fenceState.neverSubmitted = 0; }
    return result;
}
Result Fence::Reset()
{
    m_fenceState.initialSignalState = 0;
    m_fenceState.privateScreenPresentUsed = 0;
    if (m_event == nullptr) { return Result::ErrorUnavailable; }
    return ResetEvent(m_event) ? Result::Success : Result::ErrorUnknown;
}
Result Fence::WaitForFences(const Pal::Device& device, uint32 count, const Pal::Fence*const* fences,
                            bool waitAll, std::chrono::nanoseconds timeout) const
{
    if (count == 0) { return Result::ErrorInvalidValue; }
    if (fences == nullptr) { return Result::ErrorInvalidPointer; }
    Util::AutoBuffer<HANDLE, 16, Pal::Platform> events(count, device.GetPlatform());
    if (events.Capacity() < count) { return Result::ErrorOutOfMemory; }
    uint32 eligible = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        if (fences[i] == nullptr) { return Result::ErrorInvalidPointer; }
        const auto* fence = static_cast<const Fence*>(fences[i]);
        const bool canWait = fence->IsOpened() || !fence->WasNeverSubmitted() ||
                             fence->WasPrivateScreenPresentUsed() || fence->InitialState();
        if (canWait) { ++eligible; }
        else if (waitAll) { return Result::ErrorFenceNeverSubmitted; }
        events[i] = fence->m_event;
    }
    if (eligible == 0) { return Result::ErrorFenceNeverSubmitted; }
    const DWORD result = WaitForMultipleObjects(count, &events[0], waitAll, WaitMilliseconds(timeout));
    if (result < WAIT_OBJECT_0 + count) { return Result::Success; }
    return (result == WAIT_TIMEOUT) ? Result::Timeout : Result::ErrorUnknown;
}
}
