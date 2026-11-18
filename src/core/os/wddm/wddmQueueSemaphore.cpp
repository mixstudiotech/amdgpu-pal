// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmQueueSemaphore.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmResult.h"
#include "core/os/wddm/wddmCpuWait.h"
#include "core/os/wddm/wddmGang.h"
#include "core/os/wddm/wddmSubmit.h"
#include "core/os/wddm/wddmSharedResource.h"
#include <algorithm>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Pal
{
QueueSemaphore::~QueueSemaphore()
{
    if (m_wddmSync.handle != 0)
        { static_cast<Wddm::Device*>(m_pDevice)->OsTransport().DestroySync(&m_wddmSync); }
}
Result QueueSemaphore::OsInit(const QueueSemaphoreCreateInfo& info)
{
    Result result = ValidateInit(m_pDevice, info);
    if (result != Result::Success) { return result; }
    auto& transport = static_cast<Wddm::Device*>(m_pDevice)->OsTransport();
    const Wddm::SyncCreateFlags flags = { info.flags.shareable != 0,
        info.flags.sharedViaNtHandle != 0, info.flags.noSignalOnDeviceLost != 0 };
    // The native HWQueue backend emulates local counting with a monitored fence.
    const bool monitoredCounting = !info.flags.timeline && !info.flags.shareable && !info.flags.externalOpened;
    // Native 35e1cf: selector 311840 decides for every semaphore; gpuOnly only picks INTRA_GPU inside the native
    // creator 35df80 and is ignored by the classic branch. The selector depends on the EnableNativeFence setting,
    // which is 0 here, so classic objects are created (KMTQAITYPE_WDDM_3_1_CAPS is never consulted).
    const auto& physical = static_cast<Wddm::Device*>(m_pDevice)->Adapter().physical;
    const bool nativeFence = Wddm::NativeFenceSelected(physical, Wddm::ReadGangTopology(physical).hwsPresent);
    const Wddm::Status status = nativeFence ?
        transport.CreateNativeSync(info.initialCount, info.flags.gpuOnly != 0, flags, &m_wddmSync) :
        ((info.flags.timeline || monitoredCounting) ? transport.CreateSync(info.initialCount, flags, &m_wddmSync) :
            transport.CreateCountingSync(uint32(info.initialCount), info.maxCount, flags, &m_wddmSync));
    if (Wddm::Succeeded(status))
    {
        m_flags.shareable = info.flags.shareable;
        m_flags.externalOpened = info.flags.externalOpened;
        m_flags.timeline = info.flags.timeline;
        m_wddmSharedViaNtHandle = info.flags.sharedViaNtHandle;
        m_wddmMonitoredCounting = monitoredCounting;
        m_maxWaitsPerSignal = info.maxCount;
        m_wddmLastSubmitted.store(info.initialCount, std::memory_order_release);
    }
    return Wddm::ConvertStatus(status);
}
Result QueueSemaphore::Open(const QueueSemaphoreOpenInfo& info)
{
    if (info.pSharedQueueSemaphore == nullptr) { return Result::ErrorInvalidPointer; }
    const auto* original = static_cast<const QueueSemaphore*>(info.pSharedQueueSemaphore);
    if (!original->IsShareable() && !original->IsShared()) { return Result::ErrorNotShareable; }
    auto& transport = static_cast<Wddm::Device*>(m_pDevice)->OsTransport();
    void* handle = nullptr;
    if (transport.ExportSync(original->m_wddmSync, original->m_wddmSharedViaNtHandle, &handle) != Wddm::Success)
        { handle = nullptr; }
    if (handle == nullptr) { return Result::ErrorUnavailable; }
    const Result result = Wddm::ConvertStatus(original->m_wddmSharedViaNtHandle ?
        (original->m_wddmSync.nativeFence ? transport.OpenNativeSync(handle, &m_wddmSync) :
         original->IsTimeline() ? transport.OpenSync(handle, &m_wddmSync) :
                                  transport.OpenCountingSync(handle, &m_wddmSync)) :
        transport.OpenGlobalSync(uint32(reinterpret_cast<uintptr_t>(handle)), &m_wddmSync));
    if (original->m_wddmSharedViaNtHandle) { CloseHandle(handle); }
    if (result == Result::Success)
    {
        m_flags.shared = 1;
        m_flags.timeline = original->IsTimeline();
        m_wddmSharedViaNtHandle = original->m_wddmSharedViaNtHandle;
        m_maxWaitsPerSignal = original->m_maxWaitsPerSignal;
        m_wddmLastSubmitted.store(original->m_wddmLastSubmitted.load(std::memory_order_acquire), std::memory_order_release);
    }
    return result;
}
Result QueueSemaphore::OpenExternal(const ExternalQueueSemaphoreOpenInfo& info)
{
    // Native Windows open ignores the generic copy hint: both values reference
    // the same kernel object through NT/global/local handle semantics.
    auto& transport = static_cast<Wddm::Device*>(m_pDevice)->OsTransport();
    const uintptr_t handle = reinterpret_cast<uintptr_t>(info.externalSemaphore);
    if (!info.flags.sharedViaNtHandle && (handle > UINT32_MAX)) { return Result::ErrorInvalidValue; }
    const Result result = Wddm::ConvertStatus(info.flags.sharedViaNtHandle ?
        (info.flags.timeline ? transport.OpenSync(info.externalSemaphore, &m_wddmSync) :
                               transport.OpenCountingSync(info.externalSemaphore, &m_wddmSync)) :
        (info.flags.crossProcess ? transport.OpenGlobalSync(uint32(handle), &m_wddmSync) :
                                  transport.BorrowSync(uint32(handle), &m_wddmSync)));
    if (result == Result::Success)
    {
        m_flags.shared = 1;
        m_flags.externalOpened = 1;
        m_flags.timeline = info.flags.timeline;
        m_wddmSharedViaNtHandle = info.flags.sharedViaNtHandle;
    }
    return result;
}
OsExternalHandle QueueSemaphore::ExportExternalHandle(const QueueSemaphoreExportInfo& info) const
{
    // Native slot 6 (35e6b0): without NT sharing (F bit6) the global share handle (+0x24, zero for a local object) is
    // returned; with it the synchronization object is shared through 35ddf0 with the caller's security attributes, name
    // and access, and the new NT handle (null on failure) belongs to the caller.
    if (!m_wddmSharedViaNtHandle) { return reinterpret_cast<OsExternalHandle>(uintptr_t(m_wddmSync.sharedHandle)); }
    void* handle = nullptr;
    Wddm::ShareResourceNt(static_cast<Wddm::Device*>(m_pDevice)->OsTransport(), m_wddmSync.handle,
                          info.pSecurityAttributes, info.pName, info.ntHandleAccess, &handle);
    return handle;
}
Result QueueSemaphore::OsSignal(Queue* queue, uint64 value)
{
    if (queue == nullptr) { return Result::ErrorInvalidPointer; }
    const uint64 payload = IsTimeline() ? value :
        (m_wddmMonitoredCounting ? m_wddmLastSubmitted.fetch_add(1, std::memory_order_acq_rel) + 1 : 0);
    const Result result = static_cast<Wddm::Queue*>(queue)->SignalNativeSync(m_wddmSync, payload);
    if (result == Result::Success && IsTimeline())
        { m_wddmLastSubmitted.store(value, std::memory_order_release); }
    return result;
}
Result QueueSemaphore::OsWait(Queue* queue, uint64 value)
{
    if (queue == nullptr) { return Result::ErrorInvalidPointer; }
    const uint64 payload = IsTimeline() ? value :
        (m_wddmMonitoredCounting ? m_wddmLastSubmitted.load(std::memory_order_acquire) : 0);
    if (m_wddmMonitoredCounting && !m_wddmSync.gpuOnly && (m_wddmSync.cpuAddress != nullptr))
    {
        const uint64 completed = *m_wddmSync.cpuAddress;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (completed == UINT64_MAX)
        {
            const Result state = static_cast<Wddm::Device*>(m_pDevice)->CheckExecutionState(nullptr);
            if (state != Result::Success) { return state; }
        }
        if (completed >= payload) { return Result::Success; }
    }
    return static_cast<Wddm::Queue*>(queue)->WaitNativeSync(m_wddmSync, payload);
}
Result QueueSemaphore::QuerySemaphoreValue(uint64* value)
{
    // 35e860 reads the object's CPU value without a gpuOnly test; a missing CPU mapping (dereferenced by the original)
    // reports ErrorUnavailable below.
    if (!IsTimeline()) { return Result::ErrorInvalidObjectType; }
    if (value == nullptr) { return Result::ErrorInvalidPointer; }
    if (!static_cast<Wddm::Device*>(m_pDevice)->OsTransport().IsOpen()) { return Result::ErrorDeviceLost; }
    if (m_wddmSync.cpuAddress == nullptr) { return Result::ErrorUnavailable; }
    *value = *m_wddmSync.cpuAddress;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (*value == UINT64_MAX)
    {
        return static_cast<Wddm::Device*>(m_pDevice)->CheckExecutionState(nullptr);
    }
    return Result::Success;
}
Result QueueSemaphore::WaitSemaphoreValue(uint64 value, std::chrono::nanoseconds timeout)
{
    // 35e880 has no gpuOnly test either.
    if (!IsTimeline()) { return Result::ErrorInvalidObjectType; }
    const uint32 device = static_cast<Wddm::Device*>(m_pDevice)->OsTransport().DeviceHandle();
    const bool infinite = timeout == std::chrono::nanoseconds::max();
    if (m_wddmSync.cpuAddress != nullptr)
    {
        return Wddm::WaitCpuPayload(device, m_wddmSync.handle, *m_wddmSync.cpuAddress, value, timeout.count(), infinite);
    }
    // A CPU-unmapped imported object is an existing public extension, outside
    // original35e880's valid mapped-pointer domain. Register its one payload.
    return Wddm::WaitCpuPayloads(device, &m_wddmSync.handle, &value, 1, false, timeout.count(), infinite);
}
Result QueueSemaphore::OsSignalSemaphoreValue(uint64 value)
{
    if (!IsTimeline()) { return Result::ErrorInvalidObjectType; }
    const Result result = Wddm::ConvertStatus(static_cast<Wddm::Device*>(m_pDevice)->OsTransport().SignalCpu(m_wddmSync, value));
    if (result == Result::Success) { m_wddmLastSubmitted.store(value, std::memory_order_release); }
    return result;
}
Result QueueSemaphore::OsQuerySemaphoreLastValue(uint64* value)
{
    if (!IsTimeline()) { return Result::ErrorInvalidObjectType; }
    if (value == nullptr) { return Result::ErrorInvalidPointer; }
    *value = m_wddmLastSubmitted.load(std::memory_order_acquire);
    return Result::Success;
}
bool QueueSemaphore::IsWaitBeforeSignal(uint64 value)
{ return IsTimeline() && (m_wddmLastSubmitted.load(std::memory_order_acquire) < value); }
Result QueueSemaphore::WaitSemaphoreValueAvailable(uint64 value, std::chrono::nanoseconds timeout)
{
    if (!IsTimeline()) { return Result::ErrorInvalidObjectType; }
    if (m_wddmLastSubmitted.load(std::memory_order_acquire) >= value) { return Result::Success; }
    // Windows does not use the Linux availability/batching path: its scheduler accepts
    // timeline waits before signal submission. A CPU fallback waits for actual completion.
    return WaitSemaphoreValue(value, timeout);
}
}
