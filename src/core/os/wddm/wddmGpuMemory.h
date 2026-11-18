// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/gpuMemory.h"
#include "core/os/wddm/wddmTransport.h"
#include "core/os/wddm/wddmCpuMap.h"

namespace Pal::Wddm
{
namespace Private { struct NativeAllocationDesc; struct NativeImageDesc; struct NativeTypedBufferDesc; }
class Device;
struct EscapeRoute;
// Inputs of native 381bb0/318150 (private-screen flip-address registration).
struct FlipAddressRegistration
{
    bool   hasPrivateScreen = false; // image+0x250
    uint32 screenIndex      = 0;     // image+0x25c
    uint32 imageId          = 0;     // image+0x258
    uint32 allocation       = 0;     // first KMT allocation handle (2b3f20)
    int32  wddmVersion      = 0;     // adapter+0x8b8 (BASIC record DWORD 2224), compared signed
    uint32 gpuCaps          = 0;     // Device+0x9f48 (GPU record DWORD 2920)
};
// Raw D3DKMTEscape TRANSLATEALLOCATIONHANDLE in place (Transport::TranslateAllocationHandle).
using TranslateHandleFn = Status (*)(void* context, uint32* handle);
// Native 381bb0: record 0xc8, command 0x03006003, sent through the shared escape (30f860).
Result RegisterFlipAddress(const EscapeRoute& route, const FlipAddressRegistration& input,
                           TranslateHandleFn translate, void* context);
// Native 312620 flags 0x10 (Dopp desktop, object+344 bit4) and 0x20 (creator VA, bit5): the external object keeps
// this GPU VA (the Dopp desktop texture's or the creator's) and 37f4e0 neither reserves nor maps.
struct ExternalAddress
{
    uint64 address  = 0; // query+16
    uint64 sizeHint = 0; // query+32, creator VA only: object+16 before the generic initializer
};
class GpuMemory final : public Pal::GpuMemory
{
  public:
    explicit GpuMemory(Device *device);
    ~GpuMemory() override;
    static Result OpenExternal(Device* device, const ExternalGpuMemoryOpenInfo& openInfo, void* placement,
                               GpuMemoryCreateInfo* createInfo, IGpuMemory** memory);
    // Native 312620 with an image (external shared image open, wddmExternalImage.cpp). Adopts an opened,
    // not yet resident or mapped allocation. Ownership of its KMT resource moves into the new object on
    // entry; a failed call has already released the resource through the destroyed object.
    // kmdAddress: the KMD-assigned VA path (Dopp / creator VA). standaloneAllocation: a further allocation outside
    // the opened resource (DirectCapture, 317dd0) that the object owns; 'opened.handle' is then that allocation.
    static Result AdoptExternalImageMemory(Device* device, const Allocation& opened, uint64 uniqueId,
                                           OsExternalHandle hExternal, const GpuMemoryCreateInfo& createInfo,
                                           void* placement, IGpuMemory** memory,
                                           const ExternalAddress* kmdAddress = nullptr,
                                           uint32 standaloneAllocation = 0);
    // Native slot 5 (380240). NT handles returned here are caller-owned; legacy KMT global IDs are borrowed.
    OsExternalHandle ExportExternalHandle(const GpuMemoryExportInfo& exportInfo) const override;
    // Native slot 10 (3800c0 with 37fba0/37fd10): CPU acquire of the keyed mutex, then a GPU wait of the queue's
    // context for the mutex's synchronization object at the acquired fence value.
    Result AcquireKeyedMutex(IQueue* pQueue, uint64 key, uint64 timeoutMs) override;
    // Native slot 11 (380170 with 37fc70): GPU signal of the next fence value from the queue's context, then the CPU
    // release of the keyed mutex with that value.
    Result ReleaseKeyedMutex(IQueue* pQueue, uint64 key) override;
    // Native 312620: an opened resource's global share handle (object+320), keyed mutex (+324) and synchronization
    // object (+328). The object owns and destroys the last two (37e430).
    void AdoptKeyedMutex(uint32 globalShare, uint32 keyedMutex, uint32 syncObject);
    using Pal::GpuMemory::Init;
    Result Init(const GpuMemoryCreateInfo &createInfo, const GpuMemoryInternalCreateInfo &internalInfo) override;
    // Native slot 8 (37eb10).
    Result Init(const PinnedGpuMemoryCreateInfo& createInfo) override;
    // Native slot 7 (37ebc0).
    Result Init(const SvmGpuMemoryCreateInfo& createInfo) override;
    Result AllocateOrPinMemory(gpusize base, uint64 *pagingFence, VirtualGpuMemAccessMode mode, uint32 deviceCount,
                               IDevice *const *devices, Pal::Image *const *images) override;
    Result OpenSharedMemory(OsExternalHandle handle) override;
    // Native slot 16 (37f6c0).
    Result OpenPeerMemory() override;
    // Native slot 6 (381d00).
    Result SetSdiRemoteBusAddress(gpusize surfaceBusAddr, gpusize markerBusAddr) override;
    Result AllocateSvmVirtualAddress(gpusize base, gpusize size, gpusize alignment, bool commitCpu) override;
    Result FreeSvmVirtualAddress() override;
    Result OsSetPriority(GpuMemPriority, GpuMemPriorityOffset) override;
    Result OsMap(void **data) override;
    Result OsUnmap() override;
    // First KMT allocation of the object (native 2b3f20 without a parent chain).
    const Allocation &OsAllocation() const
    {
        return (m_chunks != nullptr) ? m_chunks[0] : m_allocation;
    }
    // Native object+296: number of KMT allocations backing the object (0 for virtual and SDI objects).
    uint32 OsAllocationCount() const { return m_allocationCount; }
    const Allocation* OsAllocations() const { return (m_chunks != nullptr) ? m_chunks : &m_allocation; }
    bool OwnsOsAllocation() const { return m_ownsAllocation; }

  private:
    // Native slot 21 (3816c0).
    void OsFinalizeHeaps() override;
    bool UseCachedCpuMap() const;
    // Native 37e870/37eb10/37ebc0 allocation-count selection. Allocates the per-allocation records.
    Result SelectAllocationCount(gpusize size, bool eligible, bool fallBackToOne);
    void FillNativeDesc(Private::NativeAllocationDesc* desc, Private::NativeImageDesc* image,
                        Private::NativeTypedBufferDesc* typed) const;
    Result AllocateNative(gpusize base, uint64 *pagingFence);
    // Native 381bb0: registers a private-screen image's flip address with the KMD.
    Result RegisterPrivateScreenFlipAddress();
    Result MapBorrowed(const GpuMemory& original, bool reserve);
    Device *m_device;
    Allocation m_allocation = {};
    Allocation* m_chunks = nullptr;
    uint32 m_allocationCount = 1;
    bool m_ownsAllocation = true;
    bool m_nativeCpuInvisible = false;
    bool m_nativeRoute = false;
    CpuMapState m_cpuMap;
    gpusize m_virtualReservation = 0;
    gpusize m_ownedReservation = 0;
    bool m_pageFaultDebugReservation = false;
    bool m_externalPrepared = false;
    bool m_kmdAddress = false;          // object+344 bits 4/5: the VA is not this object's mapping.
    uint32 m_standaloneAllocation = 0;  // DirectCapture allocation created beside the opened resource.
    bool m_ordinaryGartCandidate = false;
    bool m_sharedResource = false;
    bool m_sharedViaNt = false;
    uint32 m_sharedGlobal = 0;
    bool m_svmVaOwned = false;
    bool m_svmCpuCommitted = false;
    bool m_svmBorrowed = false;
    bool m_svmGpuBorrowed = false;
    bool m_svmVirtualMapped = false;
    gpusize m_svmCpuAddress = 0;
    VirtualGpuMemAccessMode m_virtualMode = VirtualGpuMemAccessMode::Undefined;
    uint32 m_keyedMutexGlobalShare = 0; // Native object+320.
    uint32 m_keyedMutex = 0;            // Native object+324.
    uint32 m_keyedMutexSync = 0;        // Native object+328.
    uint64 m_keyedMutexFence = 0;       // Native object+336: fence value of the last acquire or release.
};
} // namespace Pal::Wddm
