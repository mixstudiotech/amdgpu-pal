// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmSharedResource.h"
#include "core/os/wddm/wddmPrivate.h"
#include "core/platform.h"
#include "palSysMemory.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace Pal::Wddm
{
namespace
{
struct ImportTrace
{
    bool enabled = false;
    uint32_t sequence = 0;
    explicit ImportTrace(uint32_t handle)
    {
        static std::atomic<uint32_t> count{0};
        sequence = count.fetch_add(1, std::memory_order_relaxed);
        enabled = (sequence < 16) && (GetEnvironmentVariableW(L"PAL_WDDM_TRACE", nullptr, 0) != 0);
        if (enabled) { std::fprintf(stderr, "[pal-wddm] global import[%u] handle=%08x\n", sequence, handle); }
    }
    void Stage(const char* stage, Status status) const
    {
        if (enabled) { std::fprintf(stderr, "[pal-wddm] global import[%u] %s status=%08x\n",
                                    sequence, stage, uint32_t(status)); }
    }
    void Dump(const char* part, const void* bytes, size_t count) const
    {
        if (!enabled || (bytes == nullptr) || (count == 0)) { return; }
        const wchar_t* variable = L"PAL_WDDM_DUMP_SHARED";
        DWORD length = GetEnvironmentVariableW(variable, nullptr, 0);
        if (length == 0)
        {
            variable = L"PAL_WDDM_DUMP_IB";
            length = GetEnvironmentVariableW(variable, nullptr, 0);
        }
        if ((length == 0) || (length > 32768)) { return; }
        std::vector<wchar_t> directory(length);
        if (GetEnvironmentVariableW(variable, directory.data(), length) >= length) { return; }
        std::wstring path(directory.data());
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\global-" + std::to_wstring(sequence) + L"-";
        while (*part != 0) { path += wchar_t(*part++); }
        path += L".bin";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            const BOOL okay = WriteFile(file, bytes, DWORD(count), &written, nullptr);
            std::fprintf(stderr, "[pal-wddm] global import[%u] dump bytes=%zu written=%lu okay=%u\n",
                         sequence, count, written, unsigned(okay));
            CloseHandle(file);
        }
    }
};
}
Status ExportResourceNt(Transport& transport, const Allocation& allocation, void** handle)
{
    if (!transport.IsOpen()) { return DeviceRemoved; }
    if ((handle == nullptr) || (allocation.resource == 0)) { return InvalidParameter; }
    OBJECT_ATTRIBUTES attributes = {};
    attributes.Length = sizeof(attributes);
    HANDLE exported = nullptr;
    // ShareObjects takes the RESOURCE handle and resource access rights, not
    // the allocation handle or the synchronization-object access mask.
    const Status status = D3DKMTShareObjects(1, &allocation.resource, &attributes,
                                           SHARED_ALLOCATION_ALL_ACCESS, &exported);
    if (Succeeded(status)) { *handle = exported; }
    return status;
}

Result ShareResourceNt(Transport& transport, uint32_t resource, const void* securityAttributes,
                       const wchar_t* name, uint32_t access, void** handle)
{
    if (handle == nullptr) { return Result::ErrorInvalidPointer; }
    *handle = nullptr;
    OBJECT_ATTRIBUTES attributes = {};
    attributes.Length = sizeof(attributes);
    UNICODE_STRING objectName = {};
    if (securityAttributes != nullptr)
    {
        const auto* sa = static_cast<const SECURITY_ATTRIBUTES*>(securityAttributes);
        attributes.Attributes = (sa->bInheritHandle != FALSE) ? OBJ_INHERIT : 0;
        attributes.SecurityDescriptor = sa->lpSecurityDescriptor;
    }
    if (name != nullptr)
    {
        const USHORT bytes = USHORT(2 * lstrlenW(name));
        objectName.Length = bytes;
        objectName.MaximumLength = bytes;
        objectName.Buffer = const_cast<PWSTR>(name);
        attributes.ObjectName = &objectName;
        using GetDirectory = NTSTATUS(WINAPI*)(HANDLE*);
        static const auto getDirectory = reinterpret_cast<GetDirectory>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "BaseGetNamedObjectDirectory"));
        HANDLE directory = nullptr;
        if ((getDirectory == nullptr) || (getDirectory(&directory) != 0)) { return Result::Unsupported; }
        attributes.RootDirectory = directory;
    }
    if (!transport.IsOpen()) { return Result::ErrorDeviceLost; }
    HANDLE exported = nullptr;
    const Status status = D3DKMTShareObjects(1, &resource, &attributes, access, &exported);
    if (status == Success) { *handle = exported; }
    return ConvertNativeStatus(status);
}

Result OpenNamedNtHandle(uint32_t type, const void* securityAttributes, const wchar_t* name, uint32_t access,
                         void** handle)
{
    if (handle == nullptr) { return Result::ErrorInvalidPointer; }
    // 317700 builds the counted name and the attributes before resolving the directory (lstrlenW(null) is 0).
    const USHORT bytes = USHORT(2 * lstrlenW(name));
    UNICODE_STRING objectName = {};
    objectName.Length = bytes;
    objectName.MaximumLength = bytes;
    objectName.Buffer = const_cast<PWSTR>(name);
    OBJECT_ATTRIBUTES attributes = {};
    attributes.Length = sizeof(attributes);
    if (securityAttributes != nullptr)
    {
        const auto* sa = static_cast<const SECURITY_ATTRIBUTES*>(securityAttributes);
        attributes.Attributes = (sa->bInheritHandle != FALSE) ? OBJ_INHERIT : 0;
        attributes.SecurityDescriptor = sa->lpSecurityDescriptor;
    }
    using GetDirectory = NTSTATUS(WINAPI*)(HANDLE*);
    static const auto getDirectory = reinterpret_cast<GetDirectory>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "BaseGetNamedObjectDirectory"));
    HANDLE directory = nullptr;
    if ((getDirectory == nullptr) || (getDirectory(&directory) != 0)) { return Result::Unsupported; }
    attributes.RootDirectory = directory;
    attributes.ObjectName = &objectName;
    Status status = Success;
    if (type == 0)
    {
        D3DKMT_OPENNTHANDLEFROMNAME open = {};
        open.dwDesiredAccess = access;
        open.pObjAttrib = &attributes;
        status = D3DKMTOpenNtHandleFromName(&open);
        *handle = open.hNtHandle;
    }
    else if (type == 1)
    {
        D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME open = {};
        open.dwDesiredAccess = access;
        open.pObjAttrib = &attributes;
        status = D3DKMTOpenSyncObjectNtHandleFromName(&open);
        *handle = open.hNtHandle;
    }
    return ConvertNativeStatus(status);
}

Status OpenGlobalSharedAllocation(Transport& transport, uint32_t globalHandle, Allocation* allocation,
                                   SharedAllocationProperties* properties, bool* resourceRecognized)
{
    const ImportTrace trace(globalHandle);
    if (resourceRecognized != nullptr) { *resourceRecognized = false; }
    if (!transport.IsOpen()) { trace.Stage("closed transport", DeviceRemoved); return DeviceRemoved; }
    if ((globalHandle == 0) || (allocation == nullptr) || (allocation->handle != 0))
    { trace.Stage("invalid input", InvalidParameter); return InvalidParameter; }
    static_assert(sizeof(D3DDDI_OPENALLOCATIONINFO) == 24, "Recovered legacy allocation-info layout");
    D3DKMT_QUERYRESOURCEINFO query = {};
    query.hDevice = transport.DeviceHandle();
    query.hGlobalShare = globalHandle;
    Status status = D3DKMTQueryResourceInfo(&query);
    trace.Stage("QueryResourceInfo", status);
    trace.Dump("query-info", &query, sizeof(query));
    if (trace.enabled)
    {
        std::fprintf(stderr, "[pal-wddm] global import[%u] allocations=%u total=%u resource=%u runtime=%u\n",
                     trace.sequence, query.NumAllocations, query.TotalPrivateDriverDataSize,
                     query.ResourcePrivateDriverDataSize, query.PrivateRuntimeDataSize);
    }
    if (!Succeeded(status)) { return status; }
    constexpr uint32_t MaxPrivateBytes = 16 * 1024 * 1024;
    if ((query.NumAllocations != 1) || (query.TotalPrivateDriverDataSize > MaxPrivateBytes) ||
        (query.ResourcePrivateDriverDataSize > MaxPrivateBytes) || (query.PrivateRuntimeDataSize > MaxPrivateBytes))
    { trace.Stage("unsupported count or private sizes", NotSupported); return NotSupported; }
    std::vector<uint8_t> total(query.TotalPrivateDriverDataSize);
    std::vector<uint8_t> resource(query.ResourcePrivateDriverDataSize);
    std::vector<uint8_t> runtime(query.PrivateRuntimeDataSize);
    D3DDDI_OPENALLOCATIONINFO info = {};
    D3DKMT_OPENRESOURCE open = {};
    open.hDevice = transport.DeviceHandle();
    open.hGlobalShare = globalHandle;
    open.NumAllocations = 1;
    open.pOpenAllocationInfo = &info;
    open.PrivateRuntimeDataSize = uint32_t(runtime.size());
    open.pPrivateRuntimeData = runtime.data();
    open.ResourcePrivateDriverDataSize = uint32_t(resource.size());
    open.pResourcePrivateDriverData = resource.data();
    open.TotalPrivateDriverDataBufferSize = uint32_t(total.size());
    open.pTotalPrivateDriverDataBuffer = total.data();
    status = D3DKMTOpenResource(&open);
    trace.Stage("OpenResource", status);
    trace.Dump("open-info", &info, sizeof(info));
    if (!Succeeded(status)) { return status; }
    allocation->handle = info.hAllocation;
    allocation->resource = open.hResource;
    const auto start = reinterpret_cast<uintptr_t>(info.pPrivateDriverData);
    const auto base = reinterpret_cast<uintptr_t>(total.data());
    const uint8_t* records = total.data();
    size_t recordBytes = total.size();
    if ((start >= base) && (start - base <= total.size()) &&
        (info.PrivateDriverDataSize <= total.size() - (start - base)))
    {
        records = total.data() + (start - base);
        recordBytes = info.PrivateDriverDataSize;
    }
    if (trace.enabled)
    {
        std::fprintf(stderr, "[pal-wddm] global import[%u] resource=%08x allocation=%08x private=%u selected=%zu\n",
                     trace.sequence, open.hResource, info.hAllocation, info.PrivateDriverDataSize, recordBytes);
    }
    trace.Dump("runtime", runtime.data(), runtime.size());
    trace.Dump("resource", resource.data(), resource.size());
    trace.Dump("total", total.data(), total.size());
    trace.Dump("allocation", records, recordBytes);
    SharedAllocationProperties imported;
    if (!ParseSharedAllocationProperties(records, recordBytes, &imported))
    { status = NotSupported; trace.Stage("private parser rejected", status); }
    else
    {
        allocation->size = imported.size;
        if (resourceRecognized != nullptr) { *resourceRecognized = true; }
        if (trace.enabled)
        {
            std::fprintf(stderr, "[pal-wddm] global import[%u] size=%llu client=%llu alignment=%u heaps=%u linear=%u uid=%llx\n",
                         trace.sequence, imported.size, imported.clientSize, imported.alignment,
                         imported.heapCount, unsigned(imported.linearBuffer), imported.uniqueId);
        }
        if (!imported.linearBuffer) { status = NotSupported; trace.Stage("nonbuffer metadata", status); }
    }
    if (status == Success) { status = transport.MakeResident(allocation); trace.Stage("MakeResident", status); }
    if (status == Success) { status = transport.MapAllocation(allocation, 0, false); trace.Stage("MapAllocation", status); }
    if (status != Success) { trace.Stage("Destroy failed import", transport.DestroyAllocation(allocation)); }
    else if (properties != nullptr) { *properties = imported; }
    return status;
}

namespace
{
static_assert(sizeof(D3DDDI_OPENALLOCATIONINFO) == 24, "Native 302970 allocation record");
static_assert(sizeof(D3DDDI_OPENALLOCATIONINFO2) == 80, "Native 3120c0 allocation record");
static_assert((offsetof(D3DDDI_OPENALLOCATIONINFO, pPrivateDriverData) == 8) &&
              (offsetof(D3DDDI_OPENALLOCATIONINFO, PrivateDriverDataSize) == 16) &&
              (offsetof(D3DDDI_OPENALLOCATIONINFO2, pPrivateDriverData) == 8) &&
              (offsetof(D3DDDI_OPENALLOCATIONINFO2, PrivateDriverDataSize) == 16),
              "The original's entry selection reads both record kinds at +8/+16");
// Bounds for replies the original trusts blindly (it sizes one allocation from them).
constexpr uint64_t MaxSharedPrivateBytes = 16 * 1024 * 1024;
constexpr uint32_t MaxSharedAllocations  = 4096;
// Both branches allocate TotalPrivateDriverDataSize + 64 bytes for the total buffer.
constexpr uint32_t TotalSlackBytes = 64;

// One AllocInternalTemp block laid out total | resource | runtime | allocation records, alignment 8.
Status AllocateSharedStorage(Pal::Platform* platform, uint32_t totalSize, uint32_t resourceSize, uint32_t runtimeSize,
                             uint32_t count, uint32_t stride, OpenedSharedResource* out, uint8_t** runtime)
{
    if ((totalSize > MaxSharedPrivateBytes) || (resourceSize > MaxSharedPrivateBytes) ||
        (runtimeSize > MaxSharedPrivateBytes) || (count > MaxSharedAllocations)) { return NotSupported; }
    const size_t totalBytes = size_t(totalSize) + TotalSlackBytes;
    const size_t bytes = totalBytes + resourceSize + runtimeSize + size_t(count) * stride;
    auto* storage = static_cast<uint8_t*>(PAL_MALLOC_ALIGNED(bytes, 8, platform, Util::AllocInternalTemp));
    if (storage == nullptr) { return NoMemory; }
    std::memset(storage, 0, bytes);
    out->storage = storage;
    out->total = storage;
    out->totalBytes = uint32_t(totalBytes);
    out->resourceData = (resourceSize != 0) ? storage + totalBytes : nullptr;
    out->resourceBytes = resourceSize;
    *runtime = (runtimeSize != 0) ? storage + totalBytes + resourceSize : nullptr;
    out->allocations = (count != 0) ? storage + totalBytes + resourceSize + runtimeSize : nullptr;
    out->allocationCount = count;
    out->allocationStride = stride;
    return Success;
}
}

Status OpenSharedResource(Transport& transport, Pal::Platform* platform, void* handle, bool ntHandle,
                          OpenedSharedResource* resource)
{
    if ((resource == nullptr) || (platform == nullptr)) { return InvalidParameter; }
    *resource = {};
    if (!transport.IsOpen()) { return DeviceRemoved; }
    uint8_t* runtime = nullptr;
    if (ntHandle)
    {
        D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
        query.hDevice = transport.DeviceHandle();
        query.hNtHandle = handle;
        Status status = D3DKMTQueryResourceInfoFromNtHandle(&query);
        // Original2d07c0 maps only raw0 to Success; positive informational codes stop the open.
        if (status != Success) { return status; }
        status = AllocateSharedStorage(platform, query.TotalPrivateDriverDataSize, query.ResourcePrivateDriverDataSize,
                                       query.PrivateRuntimeDataSize, query.NumAllocations,
                                       sizeof(D3DDDI_OPENALLOCATIONINFO2), resource, &runtime);
        if (status != Success) { return status; }
        D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
        open.hDevice = transport.DeviceHandle();
        open.hNtHandle = handle;
        open.NumAllocations = query.NumAllocations;
        open.pOpenAllocationInfo2 = reinterpret_cast<D3DDDI_OPENALLOCATIONINFO2*>(
            const_cast<uint8_t*>(resource->allocations));
        open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
        open.pPrivateRuntimeData = runtime;
        open.ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize;
        open.pResourcePrivateDriverData = const_cast<uint8_t*>(resource->resourceData);
        // The NT branch passes the enlarged size; the KMT branch passes the reported size.
        open.TotalPrivateDriverDataBufferSize = resource->totalBytes;
        open.pTotalPrivateDriverDataBuffer = const_cast<uint8_t*>(resource->total);
        status = D3DKMTOpenResourceFromNtHandle(&open);
        if (status == Success)
        {
            resource->resource = open.hResource;
            resource->keyedMutex = open.hKeyedMutex;
            resource->syncObject = open.hSyncObject;
        }
        return status;
    }
    const uint32_t globalHandle = uint32_t(reinterpret_cast<uintptr_t>(handle));
    D3DKMT_QUERYRESOURCEINFO query = {};
    query.hDevice = transport.DeviceHandle();
    query.hGlobalShare = globalHandle;
    Status status = D3DKMTQueryResourceInfo(&query);
    if (status != Success) { return status; }
    status = AllocateSharedStorage(platform, query.TotalPrivateDriverDataSize, query.ResourcePrivateDriverDataSize,
                                   query.PrivateRuntimeDataSize, query.NumAllocations,
                                   sizeof(D3DDDI_OPENALLOCATIONINFO), resource, &runtime);
    if (status != Success) { return status; }
    D3DKMT_OPENRESOURCE open = {};
    open.hDevice = transport.DeviceHandle();
    open.hGlobalShare = globalHandle;
    open.NumAllocations = query.NumAllocations;
    open.pOpenAllocationInfo =
        reinterpret_cast<D3DDDI_OPENALLOCATIONINFO*>(const_cast<uint8_t*>(resource->allocations));
    open.pPrivateRuntimeData = runtime;
    open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
    open.pResourcePrivateDriverData = const_cast<uint8_t*>(resource->resourceData);
    open.ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize;
    open.pTotalPrivateDriverDataBuffer = const_cast<uint8_t*>(resource->total);
    open.TotalPrivateDriverDataBufferSize = query.TotalPrivateDriverDataSize;
    status = D3DKMTOpenResource(&open);
    if (status == Success)
    {
        resource->resource = open.hResource;
        resource->globalHandle = globalHandle;
    }
    return status;
}

bool SharedResourceAllocation(const OpenedSharedResource& resource, uint32_t index, uint32_t* handle,
                              const uint8_t** data, uint32_t* bytes)
{
    if ((index >= resource.allocationCount) || (resource.allocations == nullptr) ||
        (handle == nullptr) || (data == nullptr) || (bytes == nullptr)) { return false; }
    const uint8_t* record = resource.allocations + size_t(index) * resource.allocationStride;
    const void* pointer = nullptr;
    std::memcpy(handle, record, sizeof(uint32_t));
    std::memcpy(&pointer, record + 8, sizeof(pointer));
    std::memcpy(bytes, record + 16, sizeof(uint32_t));
    const auto start = reinterpret_cast<uintptr_t>(pointer);
    const auto base = reinterpret_cast<uintptr_t>(resource.total);
    if ((pointer == nullptr) || (start < base) || ((start - base) > resource.totalBytes) ||
        (*bytes > resource.totalBytes - (start - base))) { return false; }
    *data = static_cast<const uint8_t*>(pointer);
    return true;
}

Status CloseSharedResourceSyncObjects(OpenedSharedResource* resource)
{
    if (resource == nullptr) { return InvalidParameter; }
    Status status = Success;
    if (resource->keyedMutex != 0)
    {
        D3DKMT_DESTROYKEYEDMUTEX destroy = {};
        destroy.hKeyedMutex = resource->keyedMutex;
        status = D3DKMTDestroyKeyedMutex(&destroy);
        if (status == Success) { resource->keyedMutex = 0; }
    }
    if ((status == Success) && (resource->syncObject != 0))
    {
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
        destroy.hSyncObject = resource->syncObject;
        status = D3DKMTDestroySynchronizationObject(&destroy);
        if (status == Success) { resource->syncObject = 0; }
    }
    return status;
}

Status ReleaseSharedResource(Transport& transport, Pal::Platform* platform, OpenedSharedResource* resource,
                             bool destroy)
{
    if (resource == nullptr) { return InvalidParameter; }
    Status status = Success;
    if (destroy && (resource->resource != 0))
    {
        D3DKMT_DESTROYALLOCATION2 request = {};
        request.hDevice = transport.DeviceHandle();
        request.hResource = resource->resource;
        request.Flags.AssumeNotInUse = 1;
        request.Flags.SynchronousDestroy = 1;
        status = D3DKMTDestroyAllocation2(&request);
        if (status == Success) { resource->resource = 0; }
    }
    if (destroy && (status == Success)) { status = CloseSharedResourceSyncObjects(resource); }
    if ((resource->storage != nullptr) && (platform != nullptr)) { PAL_FREE(resource->storage, platform); }
    resource->storage = nullptr;
    resource->total = nullptr;
    resource->resourceData = nullptr;
    resource->allocations = nullptr;
    resource->totalBytes = 0;
    resource->resourceBytes = 0;
    resource->allocationCount = 0;
    return status;
}

Result OpenSharedResourceKeyedMutex(Transport& transport, Pal::Platform* platform, uint32_t globalShare,
                                    uint32_t* keyedMutex, uint32_t* syncObject)
{
    if ((keyedMutex == nullptr) || (syncObject == nullptr) || (platform == nullptr))
        { return Result::ErrorInvalidPointer; }
    D3DKMT_QUERYRESOURCEINFO query = {};
    query.hDevice = transport.DeviceHandle();
    query.hGlobalShare = globalShare;
    Status status = D3DKMTQueryResourceInfo(&query);
    Result result = ConvertNativeStatus(status);
    if (result != Result::Success) { return result; }
    // One AllocInternalTemp block: total (+64) | resource private | runtime | 24-byte allocation records.
    const uint64_t totalBytes = uint64_t(query.TotalPrivateDriverDataSize) + TotalSlackBytes;
    if ((totalBytes > MaxSharedPrivateBytes) || (query.ResourcePrivateDriverDataSize > MaxSharedPrivateBytes) ||
        (query.PrivateRuntimeDataSize > MaxSharedPrivateBytes) || (query.NumAllocations > MaxSharedAllocations))
        { return Result::ErrorOutOfMemory; }
    const size_t bytes = size_t(totalBytes) + query.ResourcePrivateDriverDataSize + query.PrivateRuntimeDataSize +
                         size_t(query.NumAllocations) * sizeof(D3DDDI_OPENALLOCATIONINFO);
    auto* storage = static_cast<uint8_t*>(PAL_MALLOC_ALIGNED(bytes, 8, platform, Util::AllocInternalTemp));
    if (storage == nullptr) { return Result::ErrorOutOfMemory; }
    std::memset(storage, 0, bytes);
    size_t offset = 0;
    uint8_t* total = (totalBytes != 0) ? storage : nullptr;
    offset += size_t(totalBytes);
    uint8_t* resource = (query.ResourcePrivateDriverDataSize != 0) ? storage + offset : nullptr;
    offset += query.ResourcePrivateDriverDataSize;
    uint8_t* runtime = (query.PrivateRuntimeDataSize != 0) ? storage + offset : nullptr;
    offset += query.PrivateRuntimeDataSize;
    auto* infos = (query.NumAllocations != 0) ? reinterpret_cast<D3DDDI_OPENALLOCATIONINFO*>(storage + offset)
                                              : nullptr;
    D3DKMT_OPENRESOURCE open = {};
    open.hDevice = transport.DeviceHandle();
    open.hGlobalShare = globalShare;
    open.NumAllocations = query.NumAllocations;
    open.pOpenAllocationInfo = infos;
    open.pPrivateRuntimeData = runtime;
    open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
    open.pResourcePrivateDriverData = resource;
    open.ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize;
    open.pTotalPrivateDriverDataBuffer = total;
    open.TotalPrivateDriverDataBufferSize = query.TotalPrivateDriverDataSize;
    result = ConvertNativeStatus(D3DKMTOpenResource(&open));
    uint32_t mutexShare = 0;
    uint32_t syncShare = 0;
    if (result == Result::Success)
    {
        const uint32_t at = (open.PrivateRuntimeDataSize >= 0x68) ? 32 : 28;
        if (at + 4 <= open.PrivateRuntimeDataSize) { std::memcpy(&mutexShare, runtime + at, 4); }
        if (at + 8 <= open.PrivateRuntimeDataSize) { std::memcpy(&syncShare, runtime + at + 4, 4); }
        D3DKMT_OPENKEYEDMUTEX openMutex = {};
        openMutex.hSharedHandle = mutexShare;
        result = ConvertNativeStatus(D3DKMTOpenKeyedMutex(&openMutex));
        if (result == Result::Success)
        {
            *keyedMutex = openMutex.hKeyedMutex;
            D3DKMT_OPENSYNCHRONIZATIONOBJECT openSync = {};
            openSync.hSharedHandle = syncShare;
            result = ConvertNativeStatus(D3DKMTOpenSynchronizationObject(&openSync));
            if (result == Result::Success) { *syncObject = openSync.hSyncObject; }
        }
        // 37fd10 destroys the temporarily opened resource only after both opens succeeded (it leaks it otherwise);
        // this port destroys it on every path.
        D3DKMT_DESTROYALLOCATION2 destroy = {};
        destroy.hDevice = transport.DeviceHandle();
        destroy.hResource = open.hResource;
        destroy.Flags.AssumeNotInUse = 1;
        destroy.Flags.SynchronousDestroy = (open.hResource != 0) ? 1 : 0;
        const Result destroyed = ConvertNativeStatus(D3DKMTDestroyAllocation2(&destroy));
        if (result == Result::Success) { result = destroyed; }
    }
    PAL_FREE(storage, platform);
    return result;
}
}

