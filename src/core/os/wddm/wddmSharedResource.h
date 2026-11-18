// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/os/wddm/wddmTransport.h"
#include "core/os/wddm/wddmResult.h"

namespace Pal { class Platform; }
namespace Pal::Wddm
{
// A successful NT export is a new caller-owned handle. Global KMT IDs are borrowed.
Status ExportResourceNt(Transport& transport, const Allocation& allocation, void** handle);
// Native 35ddf0: D3DKMTShareObjects(1, &resource, attributes, access, &handle). The object attributes carry the
// SECURITY_ATTRIBUTES inheritance and descriptor; a name is resolved in BaseGetNamedObjectDirectory's directory
// (kernel32), and a missing helper or its failure is Unsupported. *handle stays null on failure.
Result ShareResourceNt(Transport& transport, uint32_t resource, const void* securityAttributes,
                       const wchar_t* name, uint32_t access, void** handle);
// Native Device slot 111 (317700): the object attributes as in 35ddf0 (name under BaseGetNamedObjectDirectory, missing
// or failing helper -> Unsupported), then D3DKMTOpenNtHandleFromName (type 0) or D3DKMTOpenSyncObjectNtHandleFromName
// (type 1); *handle receives the call's output (null on failure). Any other type returns Success without a call.
Result OpenNamedNtHandle(uint32_t type, const void* securityAttributes, const wchar_t* name, uint32_t access,
                         void** handle);
// Native 37fd10: opens the keyed mutex and its synchronization object of a global (KMT) shared resource from the D3D
// runtime data (keyed mutex share handle at offset 28, or 32 when the runtime data has at least 0x68 bytes, the sync
// object share handle right after it). The resource is opened only to read that data and then destroyed.
Result OpenSharedResourceKeyedMutex(Transport& transport, Pal::Platform* platform, uint32_t globalShare,
                                    uint32_t* keyedMutex, uint32_t* syncObject);
// Opens a borrowed global KMT resource handle. The returned allocation/resource
// are local owned handles; the input global handle is never closed.
Status OpenGlobalSharedAllocation(Transport& transport, uint32_t globalHandle, Allocation* allocation,
                                   SharedAllocationProperties* properties, bool* resourceRecognized);

// Raw open of an external resource as done by native 3120c0: nothing is made resident or mapped.
// KMT branch = native 302970 (QueryResourceInfo + OpenResource, 24-byte allocation records); NT branch
// is inline in 3120c0 (QueryResourceInfoFromNtHandle + OpenResourceFromNtHandle, 80-byte records).
// One temporary PAL allocation holds total | resource | runtime private data | allocation records.
struct OpenedSharedResource
{
    uint32_t resource = 0;          // Local resource handle, owned until released or adopted.
    uint32_t keyedMutex = 0;        // NT branch outputs, owned.
    uint32_t syncObject = 0;
    uint32_t globalHandle = 0;      // KMT branch input, borrowed and never closed.
    uint32_t allocationCount = 0;
    uint32_t allocationStride = 0;  // 24 (D3DDDI_OPENALLOCATIONINFO) or 80 (D3DDDI_OPENALLOCATIONINFO2).
    void* storage = nullptr;        // Util::AllocInternalTemp allocation.
    const uint8_t* total = nullptr; // Total private-data buffer; allocation private pointers lie inside it.
    uint32_t totalBytes = 0;        // Allocated bytes (reported size + 64, as the original allocates).
    const uint8_t* resourceData = nullptr;
    uint32_t resourceBytes = 0;
    const uint8_t* allocations = nullptr;
};
// Returns the raw NTSTATUS of the first failing request, or NoMemory if the temporary storage cannot
// be allocated. Handles written by a failed open stay in *resource for ReleaseSharedResource.
Status OpenSharedResource(Transport& transport, Pal::Platform* platform, void* handle, bool ntHandle,
                          OpenedSharedResource* resource);
// Allocation record 'index': handle and private data. False when the KMD pointer/size pair does not lie
// inside the total buffer this process supplied.
bool SharedResourceAllocation(const OpenedSharedResource& resource, uint32_t index, uint32_t* handle,
                              const uint8_t** data, uint32_t* bytes);
// Native 3124f0. With destroy: D3DKMTDestroyAllocation2{resource, AssumeNotInUse|SynchronousDestroy},
// then D3DKMTDestroyKeyedMutex and D3DKMTDestroySynchronizationObject, each only after the previous
// step succeeded. The temporary storage is always freed. Returns the last request's NTSTATUS.
Status ReleaseSharedResource(Transport& transport, Pal::Platform* platform, OpenedSharedResource* resource,
                             bool destroy);
// Destroys only the keyed mutex and synchronization object returned by an NT open.
Status CloseSharedResourceSyncObjects(OpenedSharedResource* resource);
}

