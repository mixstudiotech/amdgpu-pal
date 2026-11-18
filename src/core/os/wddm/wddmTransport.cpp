// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmTransport.h"
#include "wddmPrivate.h"
#include "wddmMemoryRecords.h"
#include "wddmSharedResource.h"
#include "wddmExternalImage.h"
#include "wddmGpuVaMapPacket.h"
#include "wddmTimeout.h"
#include "wddmTimer.h"
#include "wddmGang.h"
#include "wddmSubmit.h"
#include "wddmQueueIdle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace Pal::Wddm
{
namespace
{
constexpr uint64_t PageBytes = 4096;
constexpr uint64_t AllocationAlignment = 65536;
constexpr uint64_t DefaultWaitNs = 5'000'000'000;
constexpr uint64_t PagingWaitNs = 60'000'000'000;
constexpr uint32_t MaxAdapters = 256;
constexpr Status Unsuccessful = static_cast<Status>(0xc0000001u);
bool ValidRange(uint64_t address, uint64_t bytes, uint64_t alignment)
{
    return (bytes != 0) && ((address % alignment) == 0) && ((bytes % alignment) == 0) &&
           (address <= UINT64_MAX - bytes);
}
uint64_t ReadFence(const volatile uint64_t* p)
{
    // Aligned 64-bit loads are indivisible on Windows x64. InterlockedCompareExchange64 must
    // not be used: the kernel deliberately gives the UMD a read-only CPU mapping.
    const uint64_t value = (p != nullptr) ? *p : 0;
    std::atomic_thread_fence(std::memory_order_acquire);
    return value;
}
template<typename T>
Status Query(uint32_t adapter, KMTQUERYADAPTERINFOTYPE type, T* value)
{
    D3DKMT_QUERYADAPTERINFO request = {};
    request.hAdapter = adapter;
    request.Type = type;
    request.pPrivateDriverData = value;
    request.PrivateDriverDataSize = sizeof(T);
    return D3DKMTQueryAdapterInfo(&request);
}
Status QueryIdentity(const D3DKMT_ADAPTERINFO& info, AdapterInfo* identity)
{
    D3DKMT_QUERY_DEVICE_IDS ids = {};
    Status status = Query(info.hAdapter, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &ids);
    if (!Succeeded(status)) { return status; }
    D3DKMT_KMD_DRIVER_VERSION version = {};
    status = Query(info.hAdapter, KMTQAITYPE_KMD_DRIVER_VERSION, &version);
    if (!Succeeded(status)) { return status; }
    identity->vendorId = ids.DeviceIds.VendorID;
    identity->deviceId = ids.DeviceIds.DeviceID;
    identity->revisionId = ids.DeviceIds.RevisionID;
    identity->driverVersion = uint64_t(version.DriverVersion.QuadPart);
    identity->luidLow = info.AdapterLuid.LowPart;
    identity->luidHigh = info.AdapterLuid.HighPart;
    D3DKMT_SEGMENTSIZEINFO memory = {};
    if (Succeeded(Query(info.hAdapter, KMTQAITYPE_GETSEGMENTSIZE, &memory)))
    {
        identity->dedicatedVideoMemory = memory.DedicatedVideoMemorySize;
        identity->dedicatedSystemMemory = memory.DedicatedSystemMemorySize;
        identity->sharedSystemMemory = memory.SharedSystemMemorySize;
    }
    D3DKMT_QUERY_GPUMMU_CAPS mmu = {};
    if (Succeeded(Query(info.hAdapter, KMTQAITYPE_QUERY_GPUMMU_CAPS, &mmu)))
        { identity->virtualAddressBits = mmu.Caps.VirtualAddressBitCount; }
    D3DKMT_QUERYSTATISTICS statistics = {};
    statistics.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    statistics.AdapterLuid = info.AdapterLuid;
    status = D3DKMTQueryStatistics(&statistics);
    if (!Succeeded(status)) { return status; }
    const uint32_t count = statistics.QueryResult.AdapterInformation.NodeCount;
    if (count > MaxAdapters) { return NotSupported; }
    for (uint32_t ordinal = 0; ordinal < count; ++ordinal)
    {
        D3DKMT_NODEMETADATA metadata = {};
        metadata.NodeOrdinalAndAdapterIndex = ordinal;
        status = Query(info.hAdapter, KMTQAITYPE_NODEMETADATA, &metadata);
        if (!Succeeded(status)) { return status; }
        NodeInfo node;
        node.ordinal = ordinal;
        node.engineType = metadata.NodeData.EngineType;
        const auto& name = metadata.NodeData.FriendlyName;
        size_t length = 0;
        while ((length < ARRAYSIZE(name)) && (name[length] != 0)) { ++length; }
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, int(length), nullptr, 0, nullptr, nullptr);
        if (bytes > 0)
        {
            node.name.resize(bytes);
            WideCharToMultiByte(CP_UTF8, 0, name, int(length), node.name.data(), bytes, nullptr, nullptr);
        }
        std::string lower = node.name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return char((c >= 'A' && c <= 'Z') ? (c + 'a' - 'A') : c); });
        node.isCompute = lower.find("compute") != std::string::npos;
        node.isGraphics = node.engineType == DXGK_ENGINE_TYPE_3D;
        node.isDma = node.engineType == DXGK_ENGINE_TYPE_COPY;
        node.isTimer = lower.find("timer") != std::string::npos;
        identity->nodes.push_back(std::move(node));
    }
    return Transport::IsVerifiedAdapter(*identity) ? QueryPhysicalProperties(info.hAdapter, &identity->physical) : Success;
}
}

struct Transport::Impl
{
    AdapterInfo identity;
    uint32_t adapter = 0;
    uint32_t device = 0;
    uint32_t paging = 0;
    SyncObject pagingFence;
    std::atomic<bool> quarantined = false;

    void QuarantineAt(const char* origin, Status status, bool statusKnown = true,
                      uint64_t detail0 = 0, uint64_t detail1 = 0)
    {
        const bool wasQuarantined = quarantined.exchange(true, std::memory_order_acq_rel);
        static const bool traceEnabled = GetEnvironmentVariableW(L"PAL_WDDM_TRACE", nullptr, 0) != 0;
        if (wasQuarantined || !traceEnabled) { return; }
        std::fprintf(stderr,
            "[pal-wddm] FIRST_QUARANTINE origin=%s status=%08x status-known=%u "
            "device=%08x adapter=%08x paging=%08x detail0=%llx detail1=%llx\n",
            origin, uint32_t(status), unsigned(statusKnown), device, adapter, paging, detail0, detail1);
        void* frames[12] = {};
        const USHORT count = CaptureStackBackTrace(1, ARRAYSIZE(frames), frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
        {
            HMODULE module = nullptr;
            char path[MAX_PATH] = {};
            const bool found = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[i]), &module) != FALSE;
            if (found && (GetModuleFileNameA(module, path, ARRAYSIZE(path)) != 0))
            {
                const char* name = std::strrchr(path, '\\');
                name = (name != nullptr) ? name + 1 : path;
                std::fprintf(stderr, "[pal-wddm] quarantine-stack[%u]=%s+0x%llx\n", unsigned(i), name,
                    uint64_t(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module)));
            }
            else
                { std::fprintf(stderr,"[pal-wddm] quarantine-stack[%u]=%p\n",unsigned(i),frames[i]); }
        }
        std::fflush(stderr);
    }

    Status CheckExecutionForMaxFence()
    {
        D3DKMT_GETDEVICESTATE state = {};
        state.hDevice = device;
        state.StateType = D3DKMT_DEVICESTATE_EXECUTION;
        const Status status = D3DKMTGetDeviceState(&state);
        if (!Succeeded(status))
        {
            QuarantineAt("CheckExecutionForMaxFence.query", status);
            return status;
        }
        if (state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE)
        {
            if (state.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT)
            {
                TraceDevicePageFault(device, "CheckExecutionForMaxFence");
            }
            QuarantineAt("CheckExecutionForMaxFence.execution", status, true, state.ExecutionState);
            return DeviceRemoved;
        }
        return Success;
    }

    Status WaitFence(const SyncObject& object, uint64_t value, uint64_t timeoutNs)
    {
        if (object.handle == 0) { return InvalidParameter; }
        // 35e880 / 3118b0 have no gpuOnly test; an object without a CPU mapping is waited through KMT only.
        if (object.cpuAddress != nullptr)
        {
            const uint64_t current = ReadFence(object.cpuAddress);
            if (current == UINT64_MAX)
            {
                const Status execution = CheckExecutionForMaxFence();
                if (execution != Success) { return execution; }
            }
            if (current >= value) { return Success; }
        }
        if (timeoutNs == 0) { return Timeout; }
        uint32_t milliseconds = 0;
        if (!ConvertWaitTimeout(timeoutNs, &milliseconds)) { return InvalidParameter; }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) { return NoMemory; }
        D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU request = {};
        request.hDevice = device;
        request.ObjectCount = 1;
        request.ObjectHandleArray = &object.handle;
        request.FenceValueArray = &value;
        request.hAsyncEvent = event;
        const Status status = D3DKMTWaitForSynchronizationObjectFromCpu(&request);
        if (!Succeeded(status)) { CloseHandle(event); return status; }
        const DWORD result = WaitForSingleObject(event, milliseconds);
        // KMT retains the event object while its async registration is pending. The
        // native semaphore wait closes the caller's handle on every return path.
        CloseHandle(event);
        if (result == WAIT_OBJECT_0)
        {
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((object.cpuAddress != nullptr) && (ReadFence(object.cpuAddress) == UINT64_MAX))
                { return CheckExecutionForMaxFence(); }
            return Success;
        }
        if (object.cpuAddress != nullptr)
        {
            const uint64_t current = ReadFence(object.cpuAddress);
            if (current == UINT64_MAX)
            {
                const Status execution = CheckExecutionForMaxFence();
                if (execution != Success) { return execution; }
            }
            if (current >= value) { return Success; }
        }
        return (result == WAIT_TIMEOUT) ? Timeout : Unsuccessful;
    }
};

Transport::Transport() : m_impl(new (std::nothrow) Impl) {}
Transport::~Transport() { Close(); delete m_impl; }

Status Transport::Enumerate(std::vector<AdapterInfo>* adapters)
{
    if (adapters == nullptr) { return InvalidParameter; }
    adapters->clear();
    D3DKMT_ENUMADAPTERS2 enumerate = {};
    Status status = D3DKMTEnumAdapters2(&enumerate);
    if (!Succeeded(status)) { return status; }
    if (enumerate.NumAdapters > MaxAdapters) { return NotSupported; }
    std::vector<D3DKMT_ADAPTERINFO> infos(enumerate.NumAdapters);
    enumerate.pAdapters = infos.data();
    status = D3DKMTEnumAdapters2(&enumerate);
    if (!Succeeded(status)) { return status; }
    Status queryStatus = Success;
    for (uint32_t i = 0; i < enumerate.NumAdapters; ++i)
    {
        AdapterInfo identity;
        const Status current = QueryIdentity(infos[i], &identity);
        if (Succeeded(current)) { adapters->push_back(std::move(identity)); }
        else if (Succeeded(queryStatus)) { queryStatus = current; }
        D3DKMT_CLOSEADAPTER close = { infos[i].hAdapter };
        const Status closed = D3DKMTCloseAdapter(&close);
        if (!Succeeded(closed) && Succeeded(queryStatus)) { queryStatus = closed; }
    }
    return adapters->empty() ? queryStatus : Success;
}
bool Transport::IsVerifiedAdapter(const AdapterInfo& adapter)
{
    return (adapter.vendorId == Private::VendorId) && (adapter.deviceId == Private::DeviceId) &&
           (adapter.revisionId == Private::RevisionId) && (adapter.driverVersion == Private::DriverVersion);
}
Status Transport::Open(const AdapterInfo& adapter)
{
    if (m_impl == nullptr) { return NoMemory; }
    if (m_impl->device != 0) { return InvalidParameter; }
    if (!IsVerifiedAdapter(adapter)) { return NotSupported; }
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid.LowPart = adapter.luidLow;
    open.AdapterLuid.HighPart = adapter.luidHigh;
    Status status = D3DKMTOpenAdapterFromLuid(&open);
    if (!Succeeded(status)) { return status; }
    m_impl->adapter = open.hAdapter;
    D3DKMT_ADAPTERINFO info = {};
    info.hAdapter = open.hAdapter;
    info.AdapterLuid = open.AdapterLuid;
    AdapterInfo current;
    status = QueryIdentity(info, &current);
    if (Succeeded(status) && !IsVerifiedAdapter(current)) { status = NotSupported; }
    if (!Succeeded(status)) { Close(); return status; }
    m_impl->identity = std::move(current);
    m_impl->quarantined.store(false, std::memory_order_release);
    D3DKMT_CREATEDEVICE device = {};
    device.hAdapter = m_impl->adapter;
    status = D3DKMTCreateDevice(&device);
    if (!Succeeded(status)) { Close(); return status; }
    m_impl->device = device.hDevice;
    D3DKMT_CREATEPAGINGQUEUE paging = {};
    paging.hDevice = device.hDevice;
    status = D3DKMTCreatePagingQueue(&paging);
    if (!Succeeded(status)) { Close(); return status; }
    m_impl->paging = paging.hPagingQueue;
    m_impl->pagingFence = { paging.hSyncObject,
        static_cast<const volatile uint64_t*>(paging.FenceValueCPUVirtualAddress), 0 };
    return Success;
}
void Transport::Close()
{
    if (m_impl == nullptr) { return; }
    const bool quarantined = m_impl->quarantined.load(std::memory_order_acquire);
    if (!quarantined)
    {
        if (m_impl->paging != 0)
        {
            D3DDDI_DESTROYPAGINGQUEUE destroy = { m_impl->paging };
            D3DKMTDestroyPagingQueue(&destroy);
        }
        if (m_impl->device != 0)
        {
            D3DKMT_DESTROYDEVICE destroy = { m_impl->device };
            D3DKMTDestroyDevice(&destroy);
        }
        if (m_impl->adapter != 0)
        {
            D3DKMT_CLOSEADAPTER close = { m_impl->adapter };
            D3DKMTCloseAdapter(&close);
        }
    }
    // Quarantined objects and GPU-visible backing deliberately remain alive
    // until process exit: completion was not confirmed and their destruction would race GPU use.
    m_impl->adapter = 0;
    m_impl->device = 0;
    m_impl->paging = 0;
    m_impl->pagingFence = {};
}
const AdapterInfo& Transport::Identity() const
{
    static const AdapterInfo empty;
    return (m_impl != nullptr) ? m_impl->identity : empty;
}
uint32_t Transport::DeviceHandle() const { return (m_impl != nullptr) ? m_impl->device : 0; }
uint32_t Transport::AdapterHandle() const { return (m_impl != nullptr) ? m_impl->adapter : 0; }
bool Transport::IsOpen() const
{ return (m_impl != nullptr) && (m_impl->device != 0) && !m_impl->quarantined.load(std::memory_order_acquire); }
void Transport::Quarantine()
{ if (m_impl != nullptr) { m_impl->QuarantineAt("Transport::Quarantine.external", Success, false); } }

Status Transport::ReserveVa(uint64_t base, uint64_t minimum, uint64_t maximum, uint64_t size,
                            bool zero, uint64_t* address)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((address == nullptr) || !ValidRange(base, size, AllocationAlignment)) { return InvalidParameter; }
    D3DDDI_RESERVEGPUVIRTUALADDRESS reserve = {};
    // NoAccess reserves only VA metadata. A ZERO reservation also installs zero PTEs,
    // so the same union must name the paging queue and its paging fence must retire.
    if (zero) { reserve.hPagingQueue = m_impl->paging; }
    else { reserve.hAdapter = m_impl->adapter; }
    reserve.BaseAddress = base;
    reserve.MinimumAddress = (base == 0) ? minimum : 0;
    reserve.MaximumAddress = (base == 0) ? maximum : 0;
    reserve.Size = size;
    reserve.ReservationType = zero ? D3DDDIGPUVIRTUALADDRESS_RESERVE_ZERO : D3DDDIGPUVIRTUALADDRESS_RESERVE_NO_ACCESS;
    Status status = D3DKMTReserveGpuVirtualAddress(&reserve);
    if (Succeeded(status) && zero && (reserve.PagingFenceValue != 0))
    {
        status = m_impl->WaitFence(m_impl->pagingFence, reserve.PagingFenceValue, PagingWaitNs);
        if (status != Success)
        {
            m_impl->QuarantineAt("ReserveVa.zero_paging_wait", status, true,
                                 reserve.PagingFenceValue, reserve.VirtualAddress);
            return status;
        }
    }
    if (Succeeded(status)) { *address = reserve.VirtualAddress; }
    return status;
}
Status Transport::FreeVa(uint64_t address, uint64_t size)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (!ValidRange(address, size, PageBytes)) { return InvalidParameter; }
    D3DKMT_FREEGPUVIRTUALADDRESS release = {};
    release.hAdapter = m_impl->adapter;
    release.BaseAddress = address;
    release.Size = size;
    return D3DKMTFreeGpuVirtualAddress(&release);
}
Status Transport::Allocate(uint64_t size, uint64_t alignment, bool cpuVisible, bool executable,
                           void* hostPtr, uint64_t requestedGpuVa, Allocation* allocation,
                           uint64_t minimum, uint64_t maximum, CacheMode cache, MemoryHeap heap,
                           uint32_t priority, uint32_t priorityOffset, bool lockCpuOnCreate,
                           const Private::AllocationData* ordinaryGartRecords)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocation == nullptr) || (allocation->handle != 0) || (size == 0) ||
        (priority >= 6) || (priorityOffset >= 8)) { return InvalidParameter; }
    const bool host = (hostPtr != nullptr);
    if ((ordinaryGartRecords != nullptr) &&
        (host || ((heap != MemoryHeap::GartCacheable) && (heap != MemoryHeap::GartUswc))))
        { return NotSupported; }
    if ((host && (heap != MemoryHeap::GartCacheable)) ||
        ((heap == MemoryHeap::Invisible) && cpuVisible)) { return NotSupported; }
    const uint64_t quantum = host ? PageBytes : AllocationAlignment;
    if ((alignment == 0) || ((alignment & (alignment - 1)) != 0) || (alignment > UINT32_MAX) ||
        (host && ((reinterpret_cast<uintptr_t>(hostPtr) & (PageBytes - 1)) != 0)) ||
        (size > UINT32_MAX - (quantum - 1))) { return InvalidParameter; }
    size = (size + quantum - 1) & ~(quantum - 1);
    alignment = std::max(alignment, quantum);
    if ((requestedGpuVa != 0) && ((requestedGpuVa & (alignment - 1)) != 0)) { return InvalidParameter; }
    if (ordinaryGartRecords != nullptr)
    {
        const auto* records = ordinaryGartRecords->records.data();
        if ((Private::Get32(ordinaryGartRecords->privateData.data(), 60) != Private::RecordsBytes) ||
            (Private::Get32(records, 0) != Private::EntryBytes + Private::MetadataBytes) ||
            (Private::Get32(records, 60) != 1) ||
            (Private::Get32(records, Private::HeaderBytes) != Private::EntryBytes + Private::MetadataBytes) ||
            (Private::Get32(records, Private::HeaderBytes + 8) != 128) ||
            (Private::Get32(records, Private::HeaderBytes + 188) != Private::AbiVersion) ||
            (Private::Get64(records, Private::HeaderBytes + 104) != size)) { return InvalidParameter; }
    }
    Status status;
    if (ordinaryGartRecords != nullptr)
    {
        uint32_t nativeAllocation = 0, nativeResource = 0;
        status = CreateAllocationRequest(m_impl->device, ordinaryGartRecords->privateData.data(),
            uint32_t(ordinaryGartRecords->privateData.size()), ordinaryGartRecords->records.data(),
            uint32_t(ordinaryGartRecords->records.size()), nullptr, 0, &nativeAllocation, &nativeResource);
        if (status != Success) { return status; }
        allocation->handle = nativeAllocation;
        allocation->resource = nativeResource;
        allocation->ordinaryLegacy = true;
    }
    else
    {
        auto records = Private::AllocationRecords(uint32_t(size), uint32_t(alignment), host,
                                                   requestedGpuVa, cache == CacheMode::Gl2Bypass, heap,
                                                   priority, priorityOffset);
        D3DDDI_ALLOCATIONINFO2 info = {};
        info.pSystemMem = hostPtr;
        info.pPrivateDriverData = records.records.data();
        info.PrivateDriverDataSize = uint32_t(records.records.size());
        info.VidPnSourceId = UINT32_MAX;
        info.Priority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        D3DKMT_CREATEALLOCATION create = {};
        create.hDevice = m_impl->device;
        create.pPrivateDriverData = records.privateData.data();
        create.PrivateDriverDataSize = uint32_t(records.privateData.size());
        create.NumAllocations = 1;
        create.pAllocationInfo2 = &info;
        create.Flags.CreateResource = 1;
        create.Flags.NonSecure = 1;
        status = D3DKMTCreateAllocation2(&create);
        if (!Succeeded(status)) { return status; }
        allocation->handle = info.hAllocation;
        allocation->resource = create.hResource;
    }
    allocation->size = size;
    allocation->originalCpuAddress = hostPtr;
    allocation->hostImported = host;
    allocation->cpuAddress = hostPtr;
    status = MakeResident(allocation);
    if (status == Success) { status = MapAllocation(allocation, requestedGpuVa, executable, minimum, maximum); }
    if ((status == Success) && cpuVisible && !host && lockCpuOnCreate) { status = LockAllocation(allocation); }
    if (status != Success) { DestroyAllocation(allocation); }
    return status;
}
Status CreateAllocationRequest(uint32_t device, const void* headerData, uint32_t headerBytes,
                               const void* allocationData, uint32_t allocationBytes, void* hostPtr,
                               uint32_t createFlags, uint32_t* allocationOut, uint32_t* resourceInOut)
{
    if ((allocationOut == nullptr) || (resourceInOut == nullptr)) { return InvalidParameter; }
    static_assert(sizeof(D3DKMT_CREATEALLOCATION) == 72);
    static_assert(sizeof(D3DDDI_ALLOCATIONINFO) == 40);
    D3DDDI_ALLOCATIONINFO info = {};
    info.pSystemMem = hostPtr;
    info.pPrivateDriverData = const_cast<void*>(allocationData);
    info.PrivateDriverDataSize = allocationBytes;
    info.VidPnSourceId = UINT32_MAX;
    D3DKMT_CREATEALLOCATION create = {};
    create.hDevice = device;
    create.hResource = *resourceInOut;
    create.pPrivateDriverData = const_cast<void*>(headerData);
    create.PrivateDriverDataSize = headerBytes;
    create.NumAllocations = 1;
    create.pAllocationInfo = &info;
    static_assert(sizeof(create.Flags) == sizeof(createFlags));
    std::memcpy(&create.Flags, &createFlags, sizeof(createFlags));
    const Status status = D3DKMTCreateAllocation(&create);
    if (status == Success)
    {
        *allocationOut = info.hAllocation;
        *resourceInOut = create.hResource;
    }
    return status;
}
Status Transport::DestroyAllocation(Allocation* allocation)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (allocation == nullptr) { return InvalidParameter; }
    if (allocation->handle == 0) { return Success; }
    Status status = UnlockAllocation(allocation);
    if (status != Success) { m_impl->QuarantineAt("DestroyAllocation.unlock", status, true, allocation->handle); return status; }
    if (allocation->gpuAddress != 0)
    {
        // Caller-reserved mappings must become NoAccess again before destruction. Letting
        // DestroyAllocation unmap them implicitly can leave a hole in the enclosing VA range.
        status = UnmapAllocation(allocation);
        if (status != Success) { m_impl->QuarantineAt("DestroyAllocation.unmap", status, true, allocation->gpuAddress, allocation->size); return status; }
    }
    if (allocation->ordinaryLegacy)
    {
        status = DestroyAllocationRequest(m_impl->device, allocation->resource, 1, &allocation->handle, false);
    }
    else
    {
        D3DKMT_DESTROYALLOCATION2 destroy = {};
        destroy.hDevice = m_impl->device;
        destroy.hResource = allocation->resource;
        if (destroy.hResource == 0)
        {
            destroy.phAllocationList = &allocation->handle;
            destroy.AllocationCount = 1;
        }
        status = D3DKMTDestroyAllocation2(&destroy);
    }
    if (allocation->ordinaryLegacy ? status == Success : Succeeded(status)) { *allocation = {}; }
    else { m_impl->QuarantineAt("DestroyAllocation.destroy", status, true, allocation->handle, allocation->resource); }
    return status;
}
Status DestroyAllocationRequest(uint32_t device, uint32_t resource, uint32_t count,
                                const uint32_t* allocations, bool nativeClearFlags)
{
    static_assert(sizeof(D3DKMT_DESTROYALLOCATION2) == 24);
    D3DKMT_DESTROYALLOCATION2 destroy = {};
    destroy.hDevice = device;
    destroy.hResource = resource;
    if (resource == 0)
    {
        destroy.phAllocationList = allocations;
        destroy.AllocationCount = count;
    }
    if (!nativeClearFlags)
    {
        destroy.Flags.AssumeNotInUse = 1;
        destroy.Flags.SynchronousDestroy = resource != 0;
    }
    return D3DKMTDestroyAllocation2(&destroy);
}
Status CreateAllocationsRequest(uint32_t device, const void* headerData, uint32_t headerBytes,
                                const uint8_t* blocks, uint32_t blockBytes, uint32_t count, void* const* hostPtrs,
                                uint32_t createFlags, uint32_t* allocationsOut, uint32_t* resourceInOut,
                                uint32_t* globalShareOut, uint32_t vidPnSourceId, uint32_t infoFlags)
{
    if ((blocks == nullptr) || (count == 0) || (allocationsOut == nullptr) || (resourceInOut == nullptr) ||
        (globalShareOut == nullptr)) { return InvalidParameter; }
    std::vector<D3DDDI_ALLOCATIONINFO> infos(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        infos[index] = {};
        infos[index].pSystemMem = (hostPtrs != nullptr) ? hostPtrs[index] : nullptr;
        infos[index].pPrivateDriverData = const_cast<uint8_t*>(blocks) + size_t(index) * blockBytes;
        infos[index].PrivateDriverDataSize = blockBytes;
        infos[index].VidPnSourceId = vidPnSourceId;
        std::memcpy(&infos[index].Flags, &infoFlags, sizeof(infoFlags));
    }
    D3DKMT_CREATEALLOCATION create = {};
    create.hDevice = device;
    create.hResource = *resourceInOut;
    create.pPrivateDriverData = const_cast<void*>(headerData);
    create.PrivateDriverDataSize = headerBytes;
    create.NumAllocations = count;
    create.pAllocationInfo = infos.data();
    std::memcpy(&create.Flags, &createFlags, sizeof(createFlags));
    const Status status = D3DKMTCreateAllocation(&create);
    if (status == Success)
    {
        for (uint32_t index = 0; index < count; ++index) { allocationsOut[index] = infos[index].hAllocation; }
        *resourceInOut = create.hResource;
        *globalShareOut = create.hGlobalShare;
    }
    return status;
}
Status Transport::CreateNativeAllocations(const Private::NativeAllocationRequest& request, void* hostBase,
                                          uint64_t base, uint64_t minimum, uint64_t maximum,
                                          Allocation* allocations, uint32_t* resource, uint32_t* globalShare)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocations == nullptr) || (resource == nullptr) || (globalShare == nullptr) || (request.count == 0) ||
        (request.blocks.size() != size_t(request.count) * request.blockBytes) ||
        (request.hostOffsets.size() != request.count)) { return InvalidParameter; }
    std::vector<uint32_t> handles(request.count, 0);
    std::vector<void*> hosts(request.count, nullptr);
    for (uint32_t index = 0; index < request.count; ++index)
    {
        if (allocations[index].handle != 0) { return InvalidParameter; }
        if (hostBase != nullptr) { hosts[index] = static_cast<uint8_t*>(hostBase) + request.hostOffsets[index]; }
    }
    uint32_t nativeResource = 0, nativeShare = 0;
    Status status = CreateAllocationsRequest(m_impl->device, request.resourceHeader.data(),
        uint32_t(request.resourceHeader.size()), request.blocks.data(), request.blockBytes, request.count,
        (hostBase != nullptr) ? hosts.data() : nullptr, request.createFlags, handles.data(), &nativeResource,
        &nativeShare, request.vidPnSourceId, request.infoFlags);
    static const bool trace = GetEnvironmentVariableW(L"PAL_WDDM_TRACE", nullptr, 0) != 0;
    if (trace)
    {
        std::fprintf(stderr, "[pal-wddm] native create count=%u block=%u flags=%x status=%08x\n", request.count,
                     request.blockBytes, request.createFlags, uint32_t(status));
    }
    if (status != Success) { return status; }
    for (uint32_t index = 0; index < request.count; ++index)
    {
        const uint8_t* bo = request.blocks.data() + size_t(index) * request.blockBytes + Private::HeaderBytes + 8;
        auto& allocation = allocations[index];
        allocation = {};
        allocation.handle = handles[index];
        allocation.resource = nativeResource;
        allocation.size = Private::Get64(bo, 96);
        allocation.originalCpuAddress = hosts[index];
        allocation.cpuAddress = hosts[index];
        allocation.hostImported = (hosts[index] != nullptr);
        allocation.ordinaryLegacy = true;
    }
    for (uint32_t index = 0; (status == Success) && (index < request.count); ++index)
    {
        status = MakeResident(&allocations[index]);
        if (trace && (status != Success))
        { std::fprintf(stderr, "[pal-wddm] native resident[%u] status=%08x\n", index, uint32_t(status)); }
        if (status == Success)
        {
            status = MapAllocation(&allocations[index], base + (uint64_t(index) << Private::ChunkShift), false,
                                   minimum, maximum);
            if (trace && (status != Success))
            { std::fprintf(stderr, "[pal-wddm] native map[%u] status=%08x\n", index, uint32_t(status)); }
        }
    }
    if (status != Success)
    {
        const Status destroyed = DestroyAllocations(allocations, request.count);
        if (destroyed != Success) { m_impl->QuarantineAt("CreateNativeAllocations.cleanup", destroyed, true); }
        return status;
    }
    *resource = nativeResource;
    *globalShare = nativeShare;
    return Success;
}
Status Transport::DestroyAllocations(Allocation* allocations, uint32_t count)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocations == nullptr) || (count == 0)) { return InvalidParameter; }
    std::vector<uint32_t> handles;
    uint32_t resource = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        auto& allocation = allocations[index];
        if (allocation.handle == 0) { break; } // The original destroys the leading non-zero handles.
        Status status = UnlockAllocation(&allocation);
        if ((status == Success) && (allocation.gpuAddress != 0)) { status = UnmapAllocation(&allocation); }
        if (status != Success)
        {
            m_impl->QuarantineAt("DestroyAllocations.unmap", status, true, allocation.handle, index);
            return status;
        }
        handles.push_back(allocation.handle);
        resource = allocation.resource;
    }
    if (handles.empty()) { return Success; }
    const Status status = DestroyAllocationRequest(m_impl->device, resource, uint32_t(handles.size()),
                                                   handles.data(), false);
    if (status == Success)
    {
        for (uint32_t index = 0; index < handles.size(); ++index) { allocations[index] = {}; }
    }
    else { m_impl->QuarantineAt("DestroyAllocations.destroy", status, true, handles[0], resource); }
    return status;
}
Status TranslateAllocationHandleRequest(uint32_t adapter, uint32_t device, uint32_t* handle)
{
    if (handle == nullptr) { return InvalidParameter; }
    D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE data = {};
    data.EscapeType = D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE;
    data.hAllocation = *handle;
    D3DKMT_ESCAPE escape = {};
    escape.hAdapter = adapter;
    escape.hDevice = device;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.Flags.DriverKnownEscape = 1;
    escape.pPrivateDriverData = &data;
    escape.PrivateDriverDataSize = sizeof(data);
    const Status status = D3DKMTEscape(&escape);
    if (status == Success) { *handle = data.hAllocation; }
    return status;
}
Status Transport::TranslateAllocationHandle(uint32_t handle, uint32_t* translated)
{
    if (translated == nullptr) { return InvalidParameter; }
    if (!IsOpen()) { return DeviceRemoved; }
    uint32_t value = handle;
    const Status status = TranslateAllocationHandleRequest(m_impl->adapter, m_impl->device, &value);
    if (status == Success) { *translated = value; }
    return status;
}
Status Transport::QueryHybridDiscrete(bool* value) const
{
    if (value == nullptr) { return InvalidParameter; }
    *value = false;
    if ((m_impl == nullptr) || (m_impl->adapter == 0)) { return DeviceRemoved; }
    D3DKMT_ADAPTERTYPE type = {};
    const Status status = Query(m_impl->adapter, KMTQAITYPE_ADAPTERTYPE, &type);
    if (status == Success) { *value = (type.HybridDiscrete != 0); }
    return status;
}
Status Transport::QueryWddmVersion(uint32_t* version) const
{
    if (version == nullptr) { return InvalidParameter; }
    if (m_impl == nullptr || m_impl->adapter == 0) { return DeviceRemoved; }
    D3DKMT_DRIVERVERSION value = {};
    const Status status = Query(m_impl->adapter, KMTQAITYPE_DRIVERVERSION, &value);
    if (Succeeded(status)) { *version = uint32_t(value); }
    return status;
}
Status Transport::MapAllocation(Allocation* allocation, uint64_t requestedGpuVa, bool executable,
                                uint64_t minimum, uint64_t maximum)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocation == nullptr) || (allocation->handle == 0) || (allocation->gpuAddress != 0) ||
        !ValidRange(requestedGpuVa, allocation->size, PageBytes)) { return InvalidParameter; }
    if (!allocation->resident)
    {
        const Status resident = MakeResident(allocation);
        if (resident != Success) { return resident; }
    }
    D3DDDI_MAPGPUVIRTUALADDRESS map = {};
    map.hPagingQueue = m_impl->paging;
    map.hAllocation = allocation->handle;
    map.SizeInPages = allocation->size / PageBytes;
    map.BaseAddress = requestedGpuVa;
    map.MinimumAddress = (allocation->ordinaryLegacy || requestedGpuVa == 0) ? minimum : 0;
    map.MaximumAddress = (allocation->ordinaryLegacy || requestedGpuVa == 0) ? maximum : 0;
    map.Protection.Write = 1;
    map.Protection.Execute = allocation->ordinaryLegacy ? false : executable;
    Status status = D3DKMTMapGpuVirtualAddress(&map);
    if (!Succeeded(status)) { return status; }
    allocation->gpuAddress = map.VirtualAddress;
    allocation->ownsVa = (requestedGpuVa == 0);
    if ((requestedGpuVa != 0) && (map.VirtualAddress != requestedGpuVa))
        { m_impl->QuarantineAt("MapAllocation.address_mismatch", status, true, requestedGpuVa, map.VirtualAddress); return DeviceRemoved; }
    status = m_impl->WaitFence(m_impl->pagingFence, map.PagingFenceValue, PagingWaitNs);
    if (status != Success) { m_impl->QuarantineAt("MapAllocation.paging_wait", status, true, map.PagingFenceValue, allocation->gpuAddress); }
    return status;
}
Status MapGpuVaRequest(GpuVaMapPacket* packet)
{
    if(packet==nullptr){return InvalidParameter;}
    static_assert(sizeof(D3DDDI_MAPGPUVIRTUALADDRESS)==sizeof(GpuVaMapPacket));
    static_assert(offsetof(D3DDDI_MAPGPUVIRTUALADDRESS,VirtualAddress)==offsetof(GpuVaMapPacket,virtualAddress));
    D3DDDI_MAPGPUVIRTUALADDRESS request;
    std::memcpy(&request,packet,sizeof(request));
    const Status status=D3DKMTMapGpuVirtualAddress(&request);
    std::memcpy(packet,&request,sizeof(request));
    return status;
}
Status Transport::ResetVa(uint64_t address, uint64_t size, bool zero)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (!ValidRange(address, size, PageBytes) || (address == 0)) { return InvalidParameter; }
    D3DDDI_MAPGPUVIRTUALADDRESS map = {};
    map.hPagingQueue = m_impl->paging;
    map.BaseAddress = address;
    map.SizeInPages = size / PageBytes;
    map.Protection.Zero = zero;
    map.Protection.NoAccess = !zero;
    Status status = D3DKMTMapGpuVirtualAddress(&map);
    if (Succeeded(status))
    {
        if (map.VirtualAddress != address) { status = DeviceRemoved; }
        else { status = m_impl->WaitFence(m_impl->pagingFence, map.PagingFenceValue, PagingWaitNs); }
    }
    if (status != Success) { m_impl->QuarantineAt("ResetVa", status, true, address, size); }
    return status;
}
Status Transport::UnmapAllocation(Allocation* allocation)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocation == nullptr) || (allocation->handle == 0)) { return InvalidParameter; }
    if (allocation->gpuAddress == 0) { return Success; }
    Status status;
    if (allocation->ownsVa) { status = FreeVa(allocation->gpuAddress, allocation->size); }
    else { status = ResetVa(allocation->gpuAddress, allocation->size, allocation->zeroOnUnmap); }
    if (status == Success) { allocation->gpuAddress = 0; allocation->ownsVa = false; }
    else { m_impl->QuarantineAt("UnmapAllocation", status, true, allocation->gpuAddress, allocation->size); }
    return status;
}
Status Transport::LockCpuPerCall(const Allocation& allocation, void** cpuAddress)
{
    if (cpuAddress == nullptr) { return InvalidParameter; }
    *cpuAddress = nullptr;
    if (!IsOpen()) { return DeviceRemoved; }
    if (allocation.handle == 0) { return InvalidParameter; }
    return LockCpuPerCallRequest(m_impl->device, allocation.handle, cpuAddress);
}
Status LockCpuPerCallRequest(uint32_t device, uint32_t allocation, void** cpuAddress)
{
    if (cpuAddress == nullptr) { return InvalidParameter; }
    *cpuAddress = nullptr;
    D3DKMT_LOCK2 lock = {};
    lock.hDevice = device;
    lock.hAllocation = allocation;
    const Status status = D3DKMTLock2(&lock);
    // Native37f9f0 increments its caller-owned map count even if a successful
    // kernel response containsnullptr. Do not turn that case into another error.
    if (Succeeded(status)) { *cpuAddress = lock.pData; }
    return status;
}
Status Transport::UnlockCpuPerCall(const Allocation& allocation)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (allocation.handle == 0) { return InvalidParameter; }
    return UnlockCpuPerCallRequest(m_impl->device, allocation.handle);
}
Status UnlockCpuPerCallRequest(uint32_t device, uint32_t allocation)
{
    D3DKMT_UNLOCK2 unlock = {};
    unlock.hDevice = device;
    unlock.hAllocation = allocation;
    return D3DKMTUnlock2(&unlock);
}
Status Transport::LockAllocation(Allocation* allocation)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocation == nullptr) || (allocation->handle == 0)) { return InvalidParameter; }
    if (allocation->hostImported || allocation->locked) { return Success; }
    D3DKMT_LOCK2 lock = {};
    lock.hDevice = m_impl->device;
    lock.hAllocation = allocation->handle;
    const Status status = D3DKMTLock2(&lock);
    if (Succeeded(status))
    {
        allocation->cpuAddress = lock.pData;
        allocation->locked = true;
        if (lock.pData == nullptr) { return Unsuccessful; }
    }
    return status;
}
Status Transport::UnlockAllocation(Allocation* allocation)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocation == nullptr) || (allocation->handle == 0)) { return InvalidParameter; }
    if (!allocation->locked) { return Success; }
    D3DKMT_UNLOCK2 unlock = {};
    unlock.hDevice = m_impl->device;
    unlock.hAllocation = allocation->handle;
    const Status status = D3DKMTUnlock2(&unlock);
    if (Succeeded(status)) { allocation->locked = false; allocation->cpuAddress = allocation->originalCpuAddress; }
    else { m_impl->QuarantineAt("UnlockAllocation", status, true, allocation->handle); }
    return status;
}
Status Transport::MakeResident(Allocation* allocation)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocation == nullptr) || (allocation->handle == 0)) { return InvalidParameter; }
    if (allocation->resident) { return Success; }
    D3DDDI_MAKERESIDENT resident = {};
    resident.hPagingQueue = m_impl->paging;
    resident.NumAllocations = 1;
    resident.AllocationList = &allocation->handle;
    resident.Flags.MustSucceed = 1;
    Status status = D3DKMTMakeResident(&resident);
    if (!Succeeded(status)) { return status; }
    status = m_impl->WaitFence(m_impl->pagingFence, resident.PagingFenceValue, PagingWaitNs);
    if (status == Success) { allocation->resident = true; }
    else { m_impl->QuarantineAt("MakeResident.paging_wait", status, true, resident.PagingFenceValue, allocation->handle); }
    return status;
}
Status Transport::Evict(Allocation* allocation)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocation == nullptr) || (allocation->handle == 0)) { return InvalidParameter; }
    if (!allocation->resident) { return Success; }
    D3DKMT_EVICT evict = {};
    evict.hDevice = m_impl->device;
    evict.NumAllocations = 1;
    evict.AllocationList = &allocation->handle;
    const Status status = D3DKMTEvict(&evict);
    if (Succeeded(status)) { allocation->resident = false; }
    return status;
}
Status Transport::SetAllocationPriority(const Allocation& allocation, uint32_t priority)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (allocation.handle == 0) { return InvalidParameter; }
    D3DKMT_SETALLOCATIONPRIORITY request = {};
    request.hDevice = m_impl->device;
    request.phAllocationList = &allocation.handle;
    request.AllocationCount = 1;
    request.pPriorities = &priority;
    return D3DKMTSetAllocationPriority(&request);
}
Status Transport::SetAllocationPriority(const Allocation* allocations, uint32_t count, uint32_t priority)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((allocations == nullptr) || (count == 0)) { return InvalidParameter; }
    std::vector<uint32_t> handles(count);
    // The original passes a single priority for 'count' handles; the kernel reads one per handle.
    std::vector<uint32_t> priorities(count, priority);
    for (uint32_t index = 0; index < count; ++index)
    {
        if (allocations[index].handle == 0) { return InvalidParameter; }
        handles[index] = allocations[index].handle;
    }
    D3DKMT_SETALLOCATIONPRIORITY request = {};
    request.hDevice = m_impl->device;
    request.phAllocationList = handles.data();
    request.AllocationCount = count;
    request.pPriorities = priorities.data();
    return D3DKMTSetAllocationPriority(&request);
}

Status Transport::CreateQueue(uint32_t node, bool compute, QueueState* queue, uint32_t priority)
{
    return CreateQueue(node, compute ? QueueEngine::Compute : QueueEngine::Graphics, queue, priority);
}
Status Transport::CreateQueue(uint32_t node, QueueEngine engine, QueueState* queue, uint32_t priority)
{
    QueueCreateOptions options = {};
    options.priority = priority;
    return CreateQueue(node, engine, queue, options);
}
Status Transport::CreateQueue(uint32_t node, QueueEngine engine, QueueState* queue, const QueueCreateOptions& options)
{
    const uint32_t priority = options.priority;
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue == nullptr) || (queue->handle != 0) || (queue->context != 0) || (priority >= 5))
        { return InvalidParameter; }
    const auto found = std::find_if(m_impl->identity.nodes.begin(), m_impl->identity.nodes.end(),
        [node](const NodeInfo& n) { return n.ordinal == node; });
    if (found == m_impl->identity.nodes.end()) { return NotSupported; }
    if (((engine == QueueEngine::Graphics) && !found->isGraphics) ||
        ((engine == QueueEngine::Compute) && !found->isCompute) ||
        ((engine == QueueEngine::Dma) && !found->isDma) ||
        ((engine == QueueEngine::Timer) && !found->isTimer))
        { return NotSupported; }
    if ((engine != QueueEngine::Graphics) && (engine != QueueEngine::Compute) &&
        (engine != QueueEngine::Dma) && (engine != QueueEngine::Timer)) { return InvalidParameter; }
    if (((engine == QueueEngine::Dma) || (engine == QueueEngine::Timer)) && (priority != 0))
        { return NotSupported; }
    // COPY node1 on the gated profile has ContextSchedulingSupported0. It accepts
    // the common424-byte AMD context through the legacy virtual-context path.
    const bool hardwareQueue = (engine == QueueEngine::Graphics) || (engine == QueueEngine::Compute);
    auto data = Private::Context(engine == QueueEngine::Compute);
    if (engine == QueueEngine::Timer)
    {
        // Native38b140 uses only the64-byte common header on the virtual Timer node.
        data.fill(0);
        Private::Put32(data.data(), 0, 64);
    }
    else if (hardwareQueue && options.nullRendering)
    {
        // 37a980: common flags bit23 accompanies the NullRendering context flag.
        Private::Put32(data.data(), 4, Private::Get32(data.data(), 4) | 0x800000u);
    }
    else if (engine == QueueEngine::Dma)
    {
        // Legacy context 383e57..383e94: +56 bit19 = tmzOnly, bits 23..26 = Pal::Queue+0x9c.
        const uint32_t word56 = Private::Get32(data.data(), 56) & ~((1u << 19) | (0xfu << 23));
        Private::Put32(data.data(), 56, word56 | (options.tmzOnly ? (1u << 19) : 0u) | ((options.tmzLevel & 0xf) << 23));
    }
    D3DKMT_CREATECONTEXTVIRTUAL context = {};
    context.hDevice = m_impl->device;
    context.NodeOrdinal = node;
    context.EngineAffinity = 1;
    context.pPrivateDriverData = data.data();
    context.PrivateDriverDataSize = (engine == QueueEngine::Timer) ? 64 : uint32_t(data.size());
    context.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
    context.Flags.HwQueueSupported = hardwareQueue;
    context.Flags.NullRendering = options.nullRendering;
    // 383daf / 38b2a4: legacy and timer contexts carry DisableGpuTimeout; HWS queues carry it on the HW queue.
    context.Flags.DisableGpuTimeout = !hardwareQueue && options.disableGpuTimeout;
    Status status = D3DKMTCreateContextVirtual(&context);
    if (!Succeeded(status)) { return status; }
    queue->engine = engine;
    queue->hardwareQueue = hardwareQueue;
    queue->context = context.hContext;
    // Native Timer Init38b140 creates only the context. Its vtable does not
    // provide ordinary idle/fence association or create progress/mapping objects.
    if (engine == QueueEngine::Timer) { return Success; }
    if (hardwareQueue)
    {
        auto queueData = Private::QueueData(options);
        D3DKMT_CREATEHWQUEUE create = {};
        create.hHwContext = queue->context;
        // 37a980: DisableGpuTimeout 0x1, NativeProgressFence 0x20 (leader only).
        create.Flags.Value = (options.disableGpuTimeout ? 0x1u : 0u) | (options.nativeProgressFence ? 0x20u : 0u);
        create.pPrivateDriverData = queueData.data();
        create.PrivateDriverDataSize = uint32_t(queueData.size());
        status = D3DKMTCreateHwQueue(&create);
        if (!Succeeded(status)) { DestroyQueue(queue); return status; }
        queue->handle = create.hHwQueue;
        queue->queueId = Private::Get32(queueData.data(), 20);
        queue->progressFence = create.hHwQueueProgressFence;
        queue->progressCpu = static_cast<const volatile uint64_t*>(create.HwQueueProgressFenceCPUVirtualAddress);
        queue->progressGpu = create.HwQueueProgressFenceGPUVirtualAddress;
    }
    else
    {
        SyncObject progress;
        status = CreateSync(0, false, &progress);
        if (!Succeeded(status)) { DestroyQueue(queue); return status; }
        queue->progressFence = progress.handle;
        queue->progressCpu = progress.cpuAddress;
        queue->progressGpu = progress.gpuAddress;
        queue->ownsProgressFence = true;
    }
    SyncObject mapping;
    status = CreateSync(0, false, &mapping);
    if (!Succeeded(status)) { DestroyQueue(queue); return status; }
    queue->mappingFence = mapping.handle;
    queue->mappingCpu = mapping.cpuAddress;
    queue->mappingGpu = mapping.gpuAddress;
    return Success;
}
Status Transport::DestroyQueue(QueueState* queue)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (queue == nullptr) { return InvalidParameter; }
    if ((queue->context != 0) && (queue->submitted > Completed(*queue)))
    {
        const Status waited = Wait(*queue, queue->submitted, DefaultWaitNs);
        if (waited != Success) { m_impl->QuarantineAt("DestroyQueue.wait", waited, true, queue->submitted, queue->handle); return waited; }
    }
    if (queue->mappingSerial != 0)
    {
        const Status waited = WaitMapping(*queue, DefaultWaitNs);
        if (waited != Success)
        {
            m_impl->QuarantineAt("DestroyQueue.mapping_wait", waited, true,
                queue->mappingSerial, queue->mappingFence);
            return waited;
        }
    }
    if (queue->handle != 0)
    {
        D3DKMT_DESTROYHWQUEUE destroy = { queue->handle };
        const Status status = D3DKMTDestroyHwQueue(&destroy);
        if (!Succeeded(status)) { return status; }
        queue->handle = 0;
    }
    if (queue->mappingFence != 0)
    {
        SyncObject mapping = { queue->mappingFence, queue->mappingCpu, queue->mappingGpu };
        const Status status = DestroySync(&mapping);
        if (!Succeeded(status)) { return status; }
        queue->mappingFence = 0;
        queue->mappingCpu = nullptr;
        queue->mappingGpu = 0;
        queue->mappingSerial = 0;
    }
    if (queue->ownsProgressFence && (queue->progressFence != 0))
    {
        SyncObject progress = { queue->progressFence, queue->progressCpu, queue->progressGpu };
        const Status status = DestroySync(&progress);
        if (!Succeeded(status)) { return status; }
        queue->progressFence = 0;
        queue->progressCpu = nullptr;
        queue->progressGpu = 0;
        queue->ownsProgressFence = false;
    }
    if (queue->context != 0)
    {
        D3DKMT_DESTROYCONTEXT destroy = { queue->context };
        const Status status = D3DKMTDestroyContext(&destroy);
        if (!Succeeded(status)) { return status; }
    }
    *queue = {};
    return Success;
}
Status Transport::Submit(QueueState* queue, const IbEntry* entries, size_t count, uint64_t* serial)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue == nullptr) || (queue->context == 0) ||
        (queue->hardwareQueue && (queue->handle == 0)) || (serial == nullptr) ||
        ((queue->submitted == UINT64_MAX) && (queue->engine != QueueEngine::Dma))) { return InvalidParameter; }
    if (queue->engine == QueueEngine::Timer) { return NotSupported; }
    std::vector<uint8_t> data;
    Status status = Private::Submission(entries, count, &data, (queue->engine == QueueEngine::Dma) ? 0 : 1);
    if (!Succeeded(status)) { return status; }
    const uint64_t next = queue->submitted + 1;
    static const bool trace = std::getenv("PAL_WDDM_TRACE") != nullptr;
    static std::atomic<uint32_t> tracedSubmissions = 0;
    if (trace && (tracedSubmissions.fetch_add(1,std::memory_order_relaxed) < 32))
    {
        std::fprintf(stderr,"[pal-wddm] submit queue=%x serial=%llu ibs=%zu\n",queue->handle,next,count);
        for (size_t i=0; i<std::min<size_t>(count,8); ++i)
        { std::fprintf(stderr,"[pal-wddm] ib[%zu] va=%llx bytes=%u flags=%x\n",i,entries[i].address,entries[i].bytes,entries[i].flags); }
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (queue->hardwareQueue)
    {
        D3DKMT_SUBMITCOMMANDTOHWQUEUE submit = {};
        submit.hHwQueue = queue->handle;
        submit.HwQueueProgressFenceId = next;
        submit.CommandBuffer = entries[0].address;
        submit.CommandLength = entries[0].bytes;
        submit.PrivateDriverDataSize = uint32_t(data.size());
        submit.pPrivateDriverData = data.data();
        status = D3DKMTSubmitCommandToHwQueue(&submit);
    }
    else
    {
        // Native383770/384670 use this named KMT boundary. A separate ordered
        // monitored-fence signal supplies progress for a legacy virtual context.
        D3DKMT_SUBMITCOMMAND submit = {};
        submit.Commands = entries[0].address;
        submit.CommandLength = entries[0].bytes;
        submit.PrivateDriverDataSize = uint32_t(data.size());
        submit.pPrivateDriverData = data.data();
        submit.BroadcastContextCount = 1;
        submit.BroadcastContext[0] = queue->context;
        if (queue->redirectedPresentPending)
        {
            // Native 3847e4..384802.
            submit.Flags.PresentRedirected = 1;
            submit.PresentHistoryToken = queue->redirectedPresentToken;
            queue->redirectedPresentPending = false;
        }
        status = D3DKMTSubmitCommand(&submit);
        if (status == Success)
        {
            status = SignalLegacyProgressRequest(queue->context, queue->progressFence, &queue->submitted);
        }
    }
    if (queue->hardwareQueue ? Succeeded(status) : status == Success) { queue->submitted = next; *serial = next; }
    else if (!Succeeded(status)) { m_impl->QuarantineAt("Submit", status, true, queue->handle, next); }
    return status;
}
Status Transport::SubmitPrivate(QueueState* queue, uint64_t commandBuffer, uint32_t commandLength,
                                const uint8_t* data, size_t bytes, const uint32_t* primaries, uint32_t primaryCount,
                                uint64_t* serial)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue == nullptr) || (queue->context == 0) || (queue->hardwareQueue && (queue->handle == 0)) ||
        (serial == nullptr) || (data == nullptr) || (bytes < 8) || (bytes > UINT32_MAX) ||
        (primaryCount > D3DDDI_MAX_WRITTEN_PRIMARIES) || ((primaryCount != 0) && (primaries == nullptr)) ||
        ((queue->submitted == UINT64_MAX) && (queue->engine != QueueEngine::Dma))) { return InvalidParameter; }
    if (queue->engine == QueueEngine::Timer) { return NotSupported; }
    const uint64_t next = queue->submitted + 1;
    static const bool trace = std::getenv("PAL_WDDM_TRACE") != nullptr;
    static std::atomic<uint32_t> tracedSubmissions = 0;
    if (trace && (tracedSubmissions.fetch_add(1, std::memory_order_relaxed) < 32))
    {
        std::fprintf(stderr, "[pal-wddm] submit queue=%x serial=%llu records=%u bytes=%zu first=%llx/%u primaries=%u\n",
                     queue->handle, next, Private::Get32(data, 4), bytes, commandBuffer, commandLength, primaryCount);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    Status status;
    if (queue->hardwareQueue)
    {
        D3DKMT_SUBMITCOMMANDTOHWQUEUE submit = {};
        submit.hHwQueue = queue->handle;
        submit.HwQueueProgressFenceId = next;
        submit.CommandBuffer = commandBuffer;
        submit.CommandLength = commandLength;
        submit.PrivateDriverDataSize = uint32_t(bytes);
        submit.pPrivateDriverData = const_cast<uint8_t*>(data);
        submit.NumPrimaries = primaryCount;
        submit.WrittenPrimaries = (primaryCount != 0) ? primaries : nullptr;
        status = D3DKMTSubmitCommandToHwQueue(&submit);
    }
    else
    {
        D3DKMT_SUBMITCOMMAND submit = {};
        submit.Commands = commandBuffer;
        submit.CommandLength = commandLength;
        submit.PrivateDriverDataSize = uint32_t(bytes);
        submit.pPrivateDriverData = const_cast<uint8_t*>(data);
        submit.BroadcastContextCount = 1;
        submit.BroadcastContext[0] = queue->context;
        submit.NumPrimaries = primaryCount;
        for (uint32_t i = 0; i < primaryCount; ++i) { submit.WrittenPrimaries[i] = primaries[i]; }
        status = D3DKMTSubmitCommand(&submit);
        if (status == Success)
        {
            status = SignalLegacyProgressRequest(queue->context, queue->progressFence, &queue->submitted);
        }
    }
    if (queue->hardwareQueue ? Succeeded(status) : status == Success) { queue->submitted = next; *serial = next; }
    else if (!Succeeded(status)) { m_impl->QuarantineAt("SubmitPrivate", status, true, queue->handle, next); }
    return status;
}
Status Transport::WaitPagingFenceOnQueue(const QueueState& queue, uint64_t value)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue.context == 0) || (queue.hardwareQueue && (queue.handle == 0))) { return InvalidParameter; }
    // 30ee80 compares the paging fence's CPU value first and issues nothing when it is already reached.
    if (ReadFence(m_impl->pagingFence.cpuAddress) >= value) { return Success; }
    return WaitQueue(queue, m_impl->pagingFence, value);
}
Status SignalLegacyProgressRequest(uint32_t context, uint32_t fence, uint64_t* lastSubmitted)
{
    if (lastSubmitted == nullptr) { return InvalidParameter; }
    const uint64_t next = ++*lastSubmitted;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {};
    signal.ObjectCount = 1;
    signal.ObjectHandleArray = &fence;
    signal.BroadcastContextCount = 1;
    signal.BroadcastContextArray = &context;
    signal.MonitoredFenceValueArray = &next;
    return D3DKMTSignalSynchronizationObjectFromGpu2(&signal);
}
Status Transport::Delay(QueueState* queue, float microseconds)
{
    return Delay(queue, microseconds, 0, false);
}
Status Transport::Delay(QueueState* queue, float microseconds, uint32_t sourceId, bool afterVsync)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue == nullptr) || (queue->context == 0)) { return InvalidParameter; }
    if ((queue->engine != QueueEngine::Timer) || queue->hardwareQueue) { return NotSupported; }
    // Native30a060 copies the14848-byte GPU body into Device+40256. Timer's
    // Device+52032 bit7 is therefore full private GPU query offset14176 bit7.
    const bool microsecondUnits = (Private::Get32(m_impl->identity.physical.gpuRecord.data(), 14176) & 0x80u) != 0;
    auto record = TimerDelayRecord(microseconds, microsecondUnits, Private::AbiVersion, sourceId, afterVsync);
    D3DKMT_SUBMITCOMMAND submit = {};
    submit.CommandLength = 4;
    submit.BroadcastContextCount = 1;
    submit.BroadcastContext[0] = queue->context;
    submit.pPrivateDriverData = record.data();
    submit.PrivateDriverDataSize = uint32_t(record.size());
    // Do not insert a progress signal or counter update.38b5b0 issues only this
    // request; the caller may separately use the native semaphore operations.
    return D3DKMTSubmitCommand(&submit);
}
Status Transport::Wait(const QueueState& queue, uint64_t serial, uint64_t timeoutNs)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue.context == 0) || (queue.progressFence == 0) || (serial > queue.submitted)) { return InvalidParameter; }
    return m_impl->WaitFence({ queue.progressFence, queue.progressCpu, queue.progressGpu }, serial, timeoutNs);
}
uint64_t Transport::Completed(const QueueState& queue) const
{
    if (!IsOpen()) { return 0; }
    const uint64_t current = ReadFence(queue.progressCpu);
    if ((current == UINT64_MAX) && (m_impl->CheckExecutionForMaxFence() != Success)) { return 0; }
    return std::min(current, queue.submitted);
}
Status Transport::RegisterQueueEvent(const QueueState& queue, void* event)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue.context == 0) || (queue.progressFence == 0) || (event == nullptr))
        { return InvalidParameter; }
    const uint64_t serial = queue.submitted;
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU request = {};
    request.hDevice = m_impl->device;
    request.ObjectCount = 1;
    request.ObjectHandleArray = &queue.progressFence;
    request.FenceValueArray = &serial;
    request.hAsyncEvent = event;
    return D3DKMTWaitForSynchronizationObjectFromCpu(&request);
}
Status Transport::WaitMapping(const QueueState& queue, uint64_t timeoutNs)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (queue.mappingFence == 0) { return InvalidParameter; }
    return m_impl->WaitFence({ queue.mappingFence, queue.mappingCpu, queue.mappingGpu },
                            queue.mappingSerial, timeoutNs);
}
Status Transport::UpdateVa(QueueState* queue, const SparseOperation* operations,
                            size_t count, bool doNotWait)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue == nullptr) || (queue->context == 0) ||
        (queue->hardwareQueue && (queue->handle == 0)) ||
        (queue->mappingFence == 0) || (count == 0) || (count > UINT32_MAX) ||
        (operations == nullptr) || (queue->mappingSerial > UINT64_MAX - 2))
        { return InvalidParameter; }
    if (queue->engine == QueueEngine::Timer) { return NotSupported; }
    std::vector<D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION> native(count);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& op = operations[i];
        if (!ValidRange(op.virtualAddress, op.size, PageBytes)) { return InvalidParameter; }
        auto& output = native[i];
        switch (op.type)
        {
        case SparseOperationType::Map:
            if ((op.allocationHandle == 0) || ((op.allocationOffset % PageBytes) != 0) ||
                (op.allocationOffset > UINT64_MAX - op.size)) { return InvalidParameter; }
            output.OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT;
            output.MapProtect.BaseAddress = op.virtualAddress;
            output.MapProtect.SizeInBytes = op.size;
            output.MapProtect.hAllocation = op.allocationHandle;
            output.MapProtect.AllocationOffsetInBytes = op.allocationOffset;
            output.MapProtect.AllocationSizeInBytes = op.size;
            output.MapProtect.Protection.Write = 1;
            output.MapProtect.DriverProtection = 1;
            break;
        case SparseOperationType::Unmap:
            if (op.zero && op.noAccess) { return InvalidParameter; }
            output.OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP;
            output.Unmap.BaseAddress = op.virtualAddress;
            output.Unmap.SizeInBytes = op.size;
            output.Unmap.Protection.Zero = op.zero;
            output.Unmap.Protection.NoAccess = op.noAccess;
            break;
        case SparseOperationType::Copy:
            if (!ValidRange(op.sourceAddress, op.size, PageBytes)) { return InvalidParameter; }
            output.OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY;
            output.Copy.SourceAddress = op.sourceAddress;
            output.Copy.SizeInBytes = op.size;
            output.Copy.DestAddress = op.virtualAddress;
            break;
        default:
            return InvalidParameter;
        }
    }
    const uint64_t waitValue = queue->mappingSerial + 1;
    if (!doNotWait)
    {
        Status status;
        if (queue->hardwareQueue)
        {
            D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE signal = {};
            signal.BroadcastHwQueueCount = 1;
            signal.BroadcastHwQueueArray = &queue->handle;
            signal.ObjectCount = 1;
            signal.ObjectHandleArray = &queue->mappingFence;
            signal.FenceValueArray = &waitValue;
            status = D3DKMTSubmitSignalSyncObjectsToHwQueue(&signal);
        }
        else
        {
            // Native384350's legacy mapping signal leavesFlags0.
            D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {};
            signal.BroadcastContextCount = 1;
            signal.BroadcastContextArray = &queue->context;
            signal.ObjectCount = 1;
            signal.ObjectHandleArray = &queue->mappingFence;
            signal.MonitoredFenceValueArray = &waitValue;
            status = D3DKMTSignalSynchronizationObjectFromGpu2(&signal);
        }
        if (!Succeeded(status)) { return status; }
        queue->mappingSerial = waitValue;
    }
    D3DKMT_UPDATEGPUVIRTUALADDRESS update = {};
    update.hDevice = m_impl->device;
    update.hContext = queue->context;
    update.hFenceObject = queue->mappingFence;
    update.NumOperations = uint32_t(count);
    update.Operations = native.data();
    update.FenceValue = waitValue;
    update.Flags.DoNotWait = doNotWait;
    Status status = D3DKMTUpdateGpuVirtualAddress(&update);
    if (!Succeeded(status)) { return status; }
    queue->mappingSerial = waitValue + 1;
    return WaitQueue(*queue, { queue->mappingFence, queue->mappingCpu, queue->mappingGpu }, queue->mappingSerial);
}

Status Transport::CreateSync(uint64_t initialValue, bool shareable, SyncObject* object)
{
    return CreateSync(initialValue, SyncCreateFlags{ shareable, shareable, false }, object);
}
Status Transport::CreateSync(uint64_t initialValue, const SyncCreateFlags& flags, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((object == nullptr) || (object->handle != 0)) { return InvalidParameter; }
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {};
    create.hDevice = m_impl->device;
    create.Info.Type = D3DDDI_MONITORED_FENCE;
    create.Info.Flags.Shared = flags.shareable;
    create.Info.Flags.NtSecuritySharing = flags.sharedViaNtHandle;
    create.Info.Flags.NoSignalMaxValueOnTdr = flags.noSignalOnDeviceLost;
    create.Info.MonitoredFence.InitialFenceValue = initialValue;
    create.Info.MonitoredFence.EngineAffinity = 1;
    const Status status = D3DKMTCreateSynchronizationObject2(&create);
    if (Succeeded(status))
    {
        *object = {};
        object->handle = create.hSyncObject;
        object->cpuAddress = static_cast<const volatile uint64_t*>(create.Info.MonitoredFence.FenceValueCPUVirtualAddress);
        object->gpuAddress = create.Info.MonitoredFence.FenceValueGPUVirtualAddress;
        object->sharedHandle = create.Info.SharedHandle;
        object->nativeFlags = create.Info.Flags.Value;
    }
    return status;
}
Status Transport::QueryNativeFenceSupport(bool* supported) const
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (supported == nullptr) { return InvalidParameter; }
    D3DKMT_WDDM_3_1_CAPS caps = {};
    const Status status = Query(m_impl->adapter, KMTQAITYPE_WDDM_3_1_CAPS, &caps);
    if (Succeeded(status)) { *supported = caps.NativeGpuFenceSupported != 0; }
    return status;
}
Status Transport::CreateNativeSync(uint64_t initialValue, bool gpuOnly,
                                    const SyncCreateFlags& flags, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((object == nullptr) || (object->handle != 0)) { return InvalidParameter; }
    // 35df80 calls D3DKMTCreateNativeFence without a capability query; its caller's selector decides.
    PFND3DKMT_CREATENATIVEFENCE createNative = nullptr;
    const FARPROC symbol = GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTCreateNativeFence");
    static_assert(sizeof(symbol) == sizeof(createNative));
    std::memcpy(&createNative, &symbol, sizeof(createNative));
    if (createNative == nullptr) { return NotSupported; }
    D3DKMT_CREATENATIVEFENCE create = {};
    create.hDevice = m_impl->device;
    create.Info.InitialFenceValue = initialValue;
    create.Info.EngineAffinity = 1;
    create.Info.Type = gpuOnly ? D3DDDI_NATIVEFENCE_TYPE_INTRA_GPU : D3DDDI_NATIVEFENCE_TYPE_DEFAULT;
    create.Info.Flags.Shared = flags.shareable;
    create.Info.Flags.NtSecuritySharing = flags.sharedViaNtHandle;
    create.Info.Flags.NoSignalMaxValueOnTdr = flags.noSignalOnDeviceLost;
    const Status status = createNative(&create);
    if (Succeeded(status))
    {
        *object = {};
        object->handle = create.hSyncObject;
        object->cpuAddress = static_cast<const volatile uint64_t*>(create.Info.NativeFenceMapping.CurrentValueCpuVa);
        object->gpuAddress = create.Info.NativeFenceMapping.CurrentValueGpuVa;
        object->monitoredGpuAddress = create.Info.NativeFenceMapping.MonitoredValueGpuVa;
        object->nativeFlags = create.Info.Flags.Value;
        object->nativeFence = true;
        object->gpuOnly = gpuOnly;
    }
    return status;
}
Status Transport::OpenNativeSync(void* ntHandle, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((ntHandle == nullptr) || (object == nullptr) || (object->handle != 0)) { return InvalidParameter; }
    // 35e400 reaches its native open only through the selector; the original's slot (+0x2b60) is never loaded, so
    // the named export is used here.
    PFND3DKMT_OPENNATIVEFENCEFROMNTHANDLE openNative = nullptr;
    const FARPROC symbol = GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTOpenNativeFenceFromNTHandle");
    static_assert(sizeof(symbol) == sizeof(openNative));
    std::memcpy(&openNative, &symbol, sizeof(openNative));
    if (openNative == nullptr) { return NotSupported; }
    D3DKMT_OPENNATIVEFENCEFROMNTHANDLE open = {};
    open.hNtHandle = ntHandle;
    open.hDevice = m_impl->device;
    open.EngineAffinity = 1;
    open.Flags.Shared = 1;
    open.Flags.NtSecuritySharing = 1;
    const Status status = openNative(&open);
    if (Succeeded(status))
    {
        *object = {};
        object->handle = open.hSyncObject;
        object->cpuAddress = static_cast<const volatile uint64_t*>(open.NativeFenceMapping.CurrentValueCpuVa);
        object->gpuAddress = open.NativeFenceMapping.CurrentValueGpuVa;
        object->monitoredGpuAddress = open.NativeFenceMapping.MonitoredValueGpuVa;
        object->nativeFlags = open.Flags.Value;
        object->nativeFence = true;
        object->gpuOnly = object->cpuAddress == nullptr;
    }
    return status;
}
Status Transport::CreateCountingSync(uint32_t initialCount, uint32_t maxCount, bool shareable, SyncObject* object)
{
    return CreateCountingSync(initialCount, maxCount, SyncCreateFlags{ shareable, shareable, false }, object);
}
Status Transport::CreateCountingSync(uint32_t initialCount, uint32_t maxCount,
                                     const SyncCreateFlags& flags, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((object == nullptr) || (object->handle != 0) || (maxCount == 0) || (initialCount > maxCount))
        { return InvalidParameter; }
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {};
    create.hDevice = m_impl->device;
    create.Info.Type = D3DDDI_SEMAPHORE;
    create.Info.Flags.Shared = flags.shareable;
    create.Info.Flags.NtSecuritySharing = flags.sharedViaNtHandle;
    create.Info.Semaphore.InitialCount = initialCount;
    create.Info.Semaphore.MaxCount = maxCount;
    const Status status = D3DKMTCreateSynchronizationObject2(&create);
    if (Succeeded(status))
    {
        *object = {};
        object->handle = create.hSyncObject;
        object->sharedHandle = create.Info.SharedHandle;
        object->nativeFlags = create.Info.Flags.Value;
    }
    return status;
}
Status Transport::DestroySync(SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (object == nullptr) { return InvalidParameter; }
    if (object->handle == 0) { return Success; }
    if (!object->ownsHandle) { *object = {}; return Success; }
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = { object->handle };
    const Status status = D3DKMTDestroySynchronizationObject(&destroy);
    if (Succeeded(status)) { *object = {}; }
    return status;
}
Status Transport::WaitCpu(const SyncObject& object, uint64_t value, uint64_t timeoutNs)
{
    if (!IsOpen()) { return DeviceRemoved; }
    return m_impl->WaitFence(object, value, timeoutNs);
}
Status Transport::WaitCpuMany(const SyncObject* objects, const uint64_t* values, size_t count,
                              bool waitAll, uint64_t timeoutNs)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((objects == nullptr) || (values == nullptr) || (count == 0) || (count > UINT32_MAX))
        { return InvalidParameter; }
    std::vector<uint32_t> handles;
    handles.reserve(count);
    size_t reached = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (objects[i].handle == 0) { return InvalidParameter; }
        handles.push_back(objects[i].handle);
        if (objects[i].cpuAddress != nullptr)
        {
            const uint64_t current = ReadFence(objects[i].cpuAddress);
            if (current == UINT64_MAX)
            {
                const Status execution = m_impl->CheckExecutionForMaxFence();
                if (execution != Success) { return execution; }
            }
            reached += current >= values[i];
        }
    }
    if (waitAll ? (reached == count) : (reached != 0)) { return Success; }
    if (timeoutNs == 0) { return Timeout; }
    uint32_t milliseconds = 0;
    if (!ConvertWaitTimeout(timeoutNs, &milliseconds)) { return InvalidParameter; }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) { return NoMemory; }
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU request = {};
    request.hDevice = m_impl->device;
    request.ObjectCount = uint32_t(count);
    request.ObjectHandleArray = handles.data();
    request.FenceValueArray = values;
    request.hAsyncEvent = event;
    request.Flags.WaitAny = !waitAll;
    const Status status = D3DKMTWaitForSynchronizationObjectFromCpu(&request);
    if (!Succeeded(status)) { CloseHandle(event); return status; }
    const DWORD result = WaitForSingleObject(event, milliseconds);
    CloseHandle(event);
    if (result == WAIT_OBJECT_0)
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        for (size_t i = 0; i < count; ++i)
        {
            if ((objects[i].cpuAddress != nullptr) && (ReadFence(objects[i].cpuAddress) == UINT64_MAX))
            {
                const Status execution = m_impl->CheckExecutionForMaxFence();
                if (execution != Success) { return execution; }
            }
        }
        return Success;
    }
    return (result == WAIT_TIMEOUT) ? Timeout : Unsuccessful;
}
Status Transport::SignalCpu(const SyncObject& object, uint64_t value)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if (object.handle == 0) { return InvalidParameter; }
    // 3638c0 / 35e9e0 signal native and classic objects alike; the kernel decides.
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal = {};
    signal.hDevice = m_impl->device;
    signal.ObjectCount = 1;
    signal.ObjectHandleArray = &object.handle;
    signal.FenceValueArray = &value;
    signal.Flags.AllowFenceRewind = 1;
    std::atomic_thread_fence(std::memory_order_release);
    return D3DKMTSignalSynchronizationObjectFromCpu(&signal);
}
Status Transport::WaitQueue(const QueueState& queue, const SyncObject& object, uint64_t value)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue.context == 0) || (object.handle == 0) ||
        (queue.hardwareQueue && (queue.handle == 0))) { return InvalidParameter; }
    if (!queue.hardwareQueue)
    {
        D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {};
        wait.hContext = queue.context;
        wait.ObjectCount = 1;
        wait.ObjectHandleArray = &object.handle;
        wait.MonitoredFenceValueArray = (value != 0) ? &value : nullptr;
        return D3DKMTWaitForSynchronizationObjectFromGpu(&wait);
    }
    D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE wait = {};
    wait.hHwQueue = queue.handle;
    wait.ObjectCount = 1;
    wait.ObjectHandleArray = &object.handle;
    wait.FenceValueArray = &value;
    return D3DKMTSubmitWaitForSyncObjectsToHwQueue(&wait);
}
Status Transport::SignalQueue(const QueueState& queue, const SyncObject& object, uint64_t value)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((queue.context == 0) || (object.handle == 0) ||
        (queue.hardwareQueue && (queue.handle == 0))) { return InvalidParameter; }
    if (!queue.hardwareQueue)
    {
        // Native3840d0 supplies the payload pointer and rewind flag only when
        // nonzero; a zero payload also serves classic counting-semaphore signals.
        D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {};
        signal.BroadcastContextCount = 1;
        signal.BroadcastContextArray = &queue.context;
        signal.ObjectCount = 1;
        signal.ObjectHandleArray = &object.handle;
        signal.MonitoredFenceValueArray = (value != 0) ? &value : nullptr;
        signal.Flags.AllowFenceRewind = value != 0;
        return D3DKMTSignalSynchronizationObjectFromGpu2(&signal);
    }
    D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE signal = {};
    signal.BroadcastHwQueueCount = 1;
    signal.BroadcastHwQueueArray = &queue.handle;
    signal.ObjectCount = 1;
    signal.ObjectHandleArray = &object.handle;
    signal.FenceValueArray = &value;
    // The native AMD wrapper enables rewind only for a nonzero fence payload.
    signal.Flags.AllowFenceRewind = (value != 0);
    return D3DKMTSubmitSignalSyncObjectsToHwQueue(&signal);
}
Status Transport::OpenSync(void* ntHandle, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((ntHandle == nullptr) || (object == nullptr) || (object->handle != 0)) { return InvalidParameter; }
    D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {};
    open.hDevice = m_impl->device;
    open.hNtHandle = ntHandle;
    open.Flags.Shared = 1;
    open.Flags.NtSecuritySharing = 1;
    // Keep the original object's GPU access and affinity. On the verified driver, adding
    // NoGPUAccess to an already GPU-mapped shared object returns STATUS_INVALID_PARAMETER.
    // A read-only CPU map is still returned; scheduler waits/signals use its KMT handle.
    open.MonitoredFence.EngineAffinity = 1;
    Status status = D3DKMTOpenSyncObjectFromNtHandle2(&open);
    if ((status == InvalidParameter) && (open.hSyncObject == 0))
    {
        // D3D12's shared fence can require a scheduler-only open. This is the verified
        // interop.rs path: no GPU VA mapping and no engine affinity. Retry only when the
        // first call rejected its parameters without creating a kernel object.
        open = {};
        open.hDevice = m_impl->device;
        open.hNtHandle = ntHandle;
        open.Flags.Shared = 1;
        open.Flags.NtSecuritySharing = 1;
        open.Flags.NoGPUAccess = 1;
        status = D3DKMTOpenSyncObjectFromNtHandle2(&open);
    }
    // 35e400 tries the native open only when the native-fence selector (311840) is true.
    if (!Succeeded(status) && (open.hSyncObject == 0) &&
        NativeFenceSelected(m_impl->identity.physical, ReadGangTopology(m_impl->identity.physical).hwsPresent))
    {
        return OpenNativeSync(ntHandle, object);
    }
    if (Succeeded(status))
    {
        *object = {};
        object->handle = open.hSyncObject;
        object->cpuAddress = static_cast<const volatile uint64_t*>(open.MonitoredFence.FenceValueCPUVirtualAddress);
        object->gpuAddress = open.MonitoredFence.FenceValueGPUVirtualAddress;
        object->nativeFlags = open.Flags.Value;
    }
    return status;
}
Status Transport::ExportSync(const SyncObject& object, void** ntHandle)
{
    return ExportSync(object, true, ntHandle);
}
Status Transport::ExportSync(const SyncObject& object, bool sharedViaNtHandle, void** handle)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((object.handle == 0) || (handle == nullptr)) { return InvalidParameter; }
    if (!sharedViaNtHandle)
    {
        if (object.sharedHandle == 0) { return NotSupported; }
        *handle = reinterpret_cast<void*>(uintptr_t(object.sharedHandle));
        return Success;
    }
    OBJECT_ATTRIBUTES attributes = {};
    attributes.Length = sizeof(attributes);
    return D3DKMTShareObjects(1, &object.handle, &attributes, 0x001f0003, handle);
}
Status Transport::OpenGlobalSync(uint32_t globalHandle, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((globalHandle == 0) || (object == nullptr) || (object->handle != 0)) { return InvalidParameter; }
    D3DKMT_OPENSYNCHRONIZATIONOBJECT open = {};
    open.hSharedHandle = globalHandle;
    const Status status = D3DKMTOpenSynchronizationObject(&open);
    if (Succeeded(status))
    {
        *object = {};
        object->handle = open.hSyncObject;
        object->sharedHandle = globalHandle;
    }
    return status;
}
Status Transport::BorrowSync(uint32_t localHandle, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((localHandle == 0) || (object == nullptr) || (object->handle != 0)) { return InvalidParameter; }
    *object = {};
    object->handle = localHandle;
    object->ownsHandle = false;
    return Success;
}
Status Transport::OpenCountingSync(void* ntHandle, SyncObject* object)
{
    if (!IsOpen()) { return DeviceRemoved; }
    if ((ntHandle == nullptr) || (object == nullptr) || (object->handle != 0)) { return InvalidParameter; }
    D3DKMT_OPENSYNCOBJECTFROMNTHANDLE open = {};
    open.hNtHandle = ntHandle;
    const Status status = D3DKMTOpenSyncObjectFromNtHandle(&open);
    if (Succeeded(status)) { *object = {}; object->handle = open.hSyncObject; }
    return status;
}
Status Transport::OpenSharedAllocation(void* ntHandle, bool executable, Allocation* allocation,
                                       SharedAllocationProperties* properties, bool* resourceRecognized,
                                       bool dataBufferOnly)
{
    if (resourceRecognized != nullptr) { *resourceRecognized = false; }
    if (!IsOpen()) { return DeviceRemoved; }
    if ((ntHandle == nullptr) || (allocation == nullptr) || (allocation->handle != 0)) { return InvalidParameter; }
    // Native 381870 maps imported memory with Write only; 'executable' never adds Execute (as the original).
    (void)executable;
    D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
    query.hDevice = m_impl->device;
    query.hNtHandle = ntHandle;
    Status status = D3DKMTQueryResourceInfoFromNtHandle(&query);
    if (status != Success) { return status; }
    // Port bounds for replies the original trusts (it sizes one allocation from them).
    constexpr uint32_t MaxPrivateBytes = 16 * 1024 * 1024, MaxAllocations = 4096;
    if ((query.NumAllocations == 0) || (query.NumAllocations > MaxAllocations) ||
        (query.TotalPrivateDriverDataSize > MaxPrivateBytes) ||
        (query.ResourcePrivateDriverDataSize > MaxPrivateBytes) || (query.PrivateRuntimeDataSize > MaxPrivateBytes))
        { return NotSupported; }
    // 3120c0 NT branch: total + 64 bytes, all allocations of the resource opened at once.
    std::vector<uint8_t> total(size_t(query.TotalPrivateDriverDataSize) + 64);
    std::vector<uint8_t> resource(query.ResourcePrivateDriverDataSize);
    std::vector<uint8_t> runtime(query.PrivateRuntimeDataSize);
    std::vector<D3DDDI_OPENALLOCATIONINFO2> infos(query.NumAllocations);
    D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
    open.hDevice = m_impl->device;
    open.hNtHandle = ntHandle;
    open.NumAllocations = query.NumAllocations;
    open.pOpenAllocationInfo2 = infos.data();
    open.PrivateRuntimeDataSize = uint32_t(runtime.size());
    open.pPrivateRuntimeData = runtime.data();
    open.ResourcePrivateDriverDataSize = uint32_t(resource.size());
    open.pResourcePrivateDriverData = resource.data();
    open.TotalPrivateDriverDataBufferSize = uint32_t(total.size());
    open.pTotalPrivateDriverDataBuffer = total.data();
    status = D3DKMTOpenResourceFromNtHandle(&open);
    if (status != Success) { return status; }
    // The keyed mutex and sync object have no consumer in this backend.
    if (open.hKeyedMutex != 0) { D3DKMT_DESTROYKEYEDMUTEX m = {}; m.hKeyedMutex = open.hKeyedMutex; D3DKMTDestroyKeyedMutex(&m); }
    if (open.hSyncObject != 0)
    { D3DKMT_DESTROYSYNCHRONIZATIONOBJECT o = {}; o.hSyncObject = open.hSyncObject; D3DKMTDestroySynchronizationObject(&o); }
    OpenedSharedResource view;
    view.resource = open.hResource;
    view.allocationCount = query.NumAllocations;
    view.allocationStride = sizeof(D3DDDI_OPENALLOCATIONINFO2);
    view.total = total.data();
    view.totalBytes = uint32_t(total.size());
    view.resourceData = resource.empty() ? nullptr : resource.data();
    view.resourceBytes = uint32_t(resource.size());
    view.allocations = reinterpret_cast<const uint8_t*>(infos.data());
    uint32_t selected = 0;
    const uint8_t* entry = nullptr;
    size_t entryBytes = 0;
    allocation->resource = open.hResource;
    SharedAllocationProperties imported;
    if (!SelectExternalAllocationEntry(view, 0, &selected, &entry, &entryBytes) ||
        !ParseSharedAllocationProperties(entry, entryBytes, &imported))
    {
        status = NotSupported;
    }
    else
    {
        allocation->handle = selected;
        allocation->size = imported.size;
        if (resourceRecognized != nullptr) { *resourceRecognized = true; }
        if (dataBufferOnly && !imported.linearBuffer) { status = NotSupported; }
    }
    if (status == Success) { status = MakeResident(allocation); }
    if (status == Success) { status = MapAllocation(allocation, 0, false); }
    if (status != Success)
    {
        // Destroy the opened resource (3124f0) whether or not an allocation was selected.
        if (allocation->handle != 0) { DestroyAllocation(allocation); }
        else
        {
            D3DKMT_DESTROYALLOCATION2 destroy = {};
            destroy.hDevice = m_impl->device;
            destroy.hResource = open.hResource;
            destroy.Flags.AssumeNotInUse = 1;
            destroy.Flags.SynchronousDestroy = 1;
            D3DKMTDestroyAllocation2(&destroy);
            *allocation = {};
        }
    }
    else if (properties != nullptr) { *properties = imported; }
    return status;
}
}
