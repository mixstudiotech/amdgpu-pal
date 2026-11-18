// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Native GpuMemory vtable 656af70; analysis in .build/pal-wddm-logic/memory/analysis.txt.
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmSharedMemory.h"
#include "core/os/wddm/wddmSharedResource.h"
#include "core/os/wddm/wddmExternalImage.h"
#include "core/os/wddm/wddmPrivate.h"
#include "core/os/wddm/wddmMemoryRecords.h"
#include "core/os/wddm/wddmDeviceControls.h"
#include "core/os/wddm/wddmPrivateScreen.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmImage.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmResult.h"
#include "core/addrMgr/addrMgr2/addrMgr2.h"
#include "core/hw/gfxip/gfxImage.h"
#include "core/image.h"
#include "core/svmMgr.h"
#include "palSysMemory.h"
#include "palGpuUtil.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace Pal::Wddm
{
namespace
{
// Native TranslateHeapInfo 350f80 return value: the request does not need CPU access. Its extra setting at
// PalSettings+0x6aa (hash 0x230c9caa, default false) is absent from this tree and keeps the default.
bool NativeCpuInvisible(const GpuMemoryCreateInfo& info)
{
    if (info.flags.cpuInvisible) { return true; }
    if (info.heapAccess == GpuHeapAccessExplicit)
    {
        for (uint32 heap = 0; heap < info.heapCount; ++heap)
        {
            if (info.heaps[heap] == GpuHeapInvisible) { return true; }
        }
        return false;
    }
    return info.heapAccess == GpuHeapAccessCpuNoAccess;
}

// 380e00's reads of one subresource: the image's SubResourceInfo and AddrMgr2 tile info lists, and
// GfxImage::GetSwTileMode (original Gfx9::Image vtable 6574840 slot 18).
bool NativeImageSubresource(const void* context, uint32 index, Private::NativeSubresource* out)
{
    const auto* image = static_cast<const Pal::Image*>(context);
    if (index >= image->GetImageInfo().numSubresources) { return false; }
    const SubResourceInfo* info = image->SubresourceInfo(index);
    const auto* tile = static_cast<const AddrMgr2::TileInfo*>(image->SubresourceTileInfo(index));
    out->plane              = info->subresId.plane;
    out->bitsPerTexel       = info->bitsPerTexel;
    out->extentWidth        = info->extentTexels.width;
    out->extentHeight       = info->extentTexels.height;
    out->extentDepth        = info->extentTexels.depth;
    out->actualWidth        = info->actualExtentTexels.width;
    out->actualHeight       = info->actualExtentTexels.height;
    out->actualDepth        = info->actualExtentTexels.depth;
    out->size               = info->size;
    out->offset             = info->offset;
    out->rowPitch           = info->rowPitch;
    out->depthPitch         = info->depthPitch;
    out->stereoOffset       = uint32(info->stereoOffset);
    out->stereoLineOffset   = info->stereoLineOffset;
    out->swizzleMode        = image->GetGfxImage()->GetSwTileMode(info);
    out->backingStoreOffset = tile->backingStoreOffset;
    out->pipeBankXor        = tile->pipeBankXor;
    out->ePitch             = tile->ePitch;
    return true;
}

// Image fields of 37ef20/380960/3802c0/380e00 (original Wddm::Image offsets in wddmMemoryRecords.h). The GfxImage
// methods are called under the original's conditions.
void FillNativeImageDesc(const Pal::Image& image, bool flippable, Private::NativeImageDesc* desc)
{
    const ImageCreateInfo& info = image.GetImageCreateInfo();
    const ImageInfo& imageInfo  = image.GetImageInfo();
    GfxImage* gfxImage          = image.GetGfxImage();
    desc->createFlags        = info.flags.u32All;
    desc->usageFlags         = info.usageFlags.u32All;
    desc->imageType          = uint32(info.imageType);
    desc->format             = uint32(info.swizzledFormat.format);
    std::memcpy(&desc->swizzle, &info.swizzledFormat.swizzle, sizeof(desc->swizzle));
    desc->mipLevels          = info.mipLevels;
    desc->arraySize          = info.arraySize;
    desc->samples            = info.samples;
    desc->refreshNumerator   = info.refreshRate.numerator;
    desc->refreshDenominator = info.refreshRate.denominator;
    desc->viewFormatCount    = info.viewFormatCount;
    desc->turbosync          = (imageInfo.internalCreateInfo.flags.turbosync != 0);
    desc->numPlanes          = imageInfo.numPlanes;
    desc->privateScreen      = (image.GetPrivateScreen() != nullptr);
    // Original Wddm::Image+0x268 is the fullscreen screen of a fullscreen presentable image; this backend creates
    // no fullscreen presentable images, so no image has one.
    desc->hasScreen          = false;
    desc->sharedResourceId   = static_cast<const Image&>(image).SharedResourceId();
    desc->hasDisplayDcc      = gfxImage->HasDisplayDccData();
    if (desc->hasDisplayDcc)
    {
        DccState state = {};
        gfxImage->GetDisplayDccState(&state);
        desc->displayDccPrimaryOffset   = state.primaryOffset;
        desc->displayDccSecondaryOffset = state.secondaryOffset;
        desc->displayDccPitch           = state.pitch;
        desc->displayDccFlags = state.maxUncompressedBlockSize | (state.maxCompressedBlockSize << 2) |
                                (state.independentBlk64B << 4) | (state.independentBlk128B << 5) |
                                (state.isDccForceEnabled << 6);
    }
    if (flippable) { desc->mallCursorCacheOffset = gfxImage->GetMallCursorCacheOffset(); }
    if (info.flags.optimalShareable != 0)
    {
        SharedMetadataInfo shared = {};
        gfxImage->GetSharedMetadataInfo(&shared);
        desc->sharedFlags = (shared.flags.shaderFetchable ? 1u : 0u) | (shared.flags.hasEqGpuAccess ? 4u : 0u) |
                            (shared.flags.htileHasDsMetadata ? 8u : 0u);
        desc->sharedDccOffset        = shared.dccOffset[0];
        desc->sharedFmaskOffset      = shared.fmaskOffset;
        desc->sharedFmaskXor         = shared.fmaskXor;
        desc->sharedHtileOffset      = shared.htileOffset;
        desc->sharedDccStateOffset   = shared.dccStateMetaDataOffset[0];
        desc->sharedFastClearOffset  = shared.fastClearMetaDataOffset[0];
        desc->sharedHisPretestOffset = shared.hisPretestMetaDataOffset;
        desc->sharedFmaskSwizzle     = uint32(shared.fmaskSwizzleMode.v2);
    }
    desc->getSubresource = &NativeImageSubresource;
    desc->context        = &image;
}
}

GpuMemory::GpuMemory(Device *device) : Pal::GpuMemory(device), m_device(device)
{
}

// =====================================================================================================================
// Native Device slot 53 (312cb0) with 3120c0 (query/open), 312620 (create info) and 3124f0 (release), buffer path.
Result GpuMemory::OpenExternal(Device* device, const ExternalGpuMemoryOpenInfo& openInfo, void* placement,
                               GpuMemoryCreateInfo* createInfo, IGpuMemory** memory)
{
    if ((device == nullptr) || (placement == nullptr) || (createInfo == nullptr) || (memory == nullptr))
    { return Result::ErrorInvalidPointer; }
    *memory = nullptr;
    // 3120c0/312cb0/312620 read only the ntHandle, Dopp and globalGpuVa bits of the resource flags: a DirectCapture
    // or Android-flagged handle opens as an ordinary KMT/NT resource. A Dopp open keeps the desktop texture's VA.
    OpenedSharedResource opened;
    Status status = OpenSharedResource(device->OsTransport(), device->GetPlatform(),
                                       openInfo.resourceInfo.hExternalResource,
                                       openInfo.resourceInfo.flags.ntHandle != 0, &opened);
    uint32 handle = 0;
    const uint8* entry = nullptr;
    size_t entryBytes = 0;
    SharedAllocationProperties properties;
    bool recognized = (status == Success) &&
        SelectExternalAllocationEntry(opened, 0, &handle, &entry, &entryBytes) &&
        ParseSharedAllocationProperties(entry, entryBytes, &properties);
    // 312620 describes any entry by its BO; an image's memory opens like a buffer's (no image check).
    Result result = recognized ? Result::Success : Result::ErrorNotShareable;
    GpuMemoryCreateInfo info = {};
    if (result == Result::Success)
    {
        // 312cb0 flags: presentable kept from the caller's create info, interprocess, externalOpened,
        // sharedViaNtHandle, globalGpuVa, gl2Uncached, mallRangeActive.
        info.flags.presentable       = createInfo->flags.presentable;
        info.flags.interprocess      = 1;
        info.flags.externalOpened    = 1;
        info.flags.sharedViaNtHandle = openInfo.resourceInfo.flags.ntHandle;
        info.flags.globalGpuVa       = openInfo.resourceInfo.flags.globalGpuVa;
        info.flags.gl2Uncached       = openInfo.flags.gl2Uncached;
        info.flags.mallRangeActive   = openInfo.flags.mallRangeActive;
        info.vaRange                 = createInfo->vaRange;
        info.mallPolicy              = openInfo.mallPolicy;
        info.mallRange               = openInfo.mallRange;
        // 312620
        info.size = properties.size;
        info.alignment = properties.alignment;
        info.priority = GpuMemPriority::High;
        info.heapCount = properties.heapCount;
        for (uint32 heap = 0; heap < properties.heapCount; ++heap)
        { info.heaps[heap] = static_cast<GpuHeap>(properties.heapIds[heap] - 1); }
        info.flags.typedBuffer = openInfo.flags.typedBuffer;
        if (openInfo.flags.typedBuffer) { info.typedBufferInfo = openInfo.typedBufferInfo; }
        const size_t metadataOffset = Private::Get32(entry, 176);
        if ((metadataOffset != 0) && (metadataOffset + 224 <= entryBytes))
        {
            const uint8* metadata = entry + metadataOffset;
            if (Private::Get32(metadata, 212) & 4)
            {
                info.flags.mallRangeActive = 1;
                info.mallRange.startPage = Private::Get32(metadata, 216);
                info.mallRange.numPages  = Private::Get32(metadata, 220);
            }
        }
        Allocation adopted = {};
        adopted.handle = handle;
        adopted.resource = opened.resource;
        adopted.size = properties.size;
        adopted.ordinaryLegacy = true;
        // The resource moves into the object here; the object releases it on any later failure.
        opened.resource = 0;
        ExternalAddress dopp;
        dopp.address = openInfo.resourceInfo.doppDesktopInfo.gpuVirtAddr;
        result = AdoptExternalImageMemory(device, adopted, properties.uniqueId,
                                          openInfo.resourceInfo.hExternalResource, info, placement, memory,
                                          openInfo.resourceInfo.flags.isDopp ? &dopp : nullptr);
    }
    if (result == Result::Success)
    {
        // 312620: the object takes the global share handle of a KMT open and the keyed mutex and synchronization
        // object of an NT open (object+320/+324/+328); they are used by the keyed mutex slots.
        static_cast<GpuMemory*>(*memory)->AdoptKeyedMutex(opened.globalHandle, opened.keyedMutex, opened.syncObject);
        opened.keyedMutex = 0;
        opened.syncObject = 0;
    }
    // 3124f0: destroy what is still owned here (only on failure: resource, keyed mutex, sync object) and the storage.
    const Status released = ReleaseSharedResource(device->OsTransport(), device->GetPlatform(), &opened,
                                                  result != Result::Success);
    (void)released;
    if (result == Result::Success)
    {
        // Native untyped opens report the creator's buffer byte width while
        // the memory object's descriptor retains the full physical allocation.
        if (!openInfo.flags.typedBuffer) { info.size = properties.clientSize; }
        *createInfo = info;
    }
    return result;
}

Result GpuMemory::AdoptExternalImageMemory(Device* device, const Allocation& opened, uint64 uniqueId,
                                           OsExternalHandle hExternal, const GpuMemoryCreateInfo& createInfo,
                                           void* placement, IGpuMemory** memory, const ExternalAddress* kmdAddress,
                                           uint32 standaloneAllocation)
{
    *memory = nullptr;
    auto* object = PAL_PLACEMENT_NEW(placement) GpuMemory(device);
    object->m_allocation = opened;
    object->m_standaloneAllocation = standaloneAllocation;
    Transport& transport = device->OsTransport();
    const gpusize granularity = device->MemoryProperties().realMemAllocGranularity;
    // The original's 312620 calls the generic initializer without the public validation; globalGpuVa is
    // honoured below even when the device does not advertise it. 351930 accepts any opened size: it raises the
    // descriptor size to Pow2Align(size, realMemAllocGranularity) (no rejection), so a D3D11 buffer of 16 KiB
    // opens. The reservation below covers the aligned size; the mapping keeps the allocation's own size.
    GpuMemoryCreateInfo validated = createInfo;
    validated.flags.globalGpuVa = 0;
    Result result = Pal::GpuMemory::ValidateCreateInfo(device, validated);
    if ((result == Result::Success) && (kmdAddress != nullptr))
    {
        // 312620 flags 0x10/0x20: the VA is the Dopp desktop texture's or the creator's; 37f4e0 reserves and maps
        // nothing. Residency stays this backend's eager policy.
        object->m_kmdAddress = true;
        object->m_desc.gpuVirtAddr = kmdAddress->address;
        object->m_desc.size = kmdAddress->sizeHint;
        result = ConvertStatus(transport.MakeResident(&object->m_allocation), "MakeExternalResident");
    }
    else if (result == Result::Success)
    {
        // Same residency and auto-VA steps as Transport::OpenSharedAllocation for imported buffers.
        Status status = transport.MakeResident(&object->m_allocation);
        if (status == Success) { status = transport.MapAllocation(&object->m_allocation, 0, false); }
        result = ConvertStatus(status, "MapExternalImageMemory");
    }
    const gpusize alignment = Util::Max<gpusize>(createInfo.alignment, granularity);
    if ((result == Result::Success) && !object->m_kmdAddress &&
        (createInfo.flags.globalGpuVa || ((object->m_allocation.gpuAddress % alignment) != 0)))
    {
        // As OpenExternal: reserve the aligned range first, then move the mapping into it. Native 37f4e0
        // reserves (3817b0) for globalGpuVa objects before mapping.
        object->m_desc.size = createInfo.size;
        object->m_desc.alignment = alignment;
        gpusize address = 0;
        result = device->AssignVirtualAddress(*object, &address, VaPartition::Default);
        if (result == Result::Success)
        {
            object->m_desc.gpuVirtAddr = address;
            object->m_ownedReservation = Util::Pow2Align(createInfo.size, granularity);
            Status status = transport.UnmapAllocation(&object->m_allocation);
            if (status == Success) { status = transport.MapAllocation(&object->m_allocation, address, false); }
            result = ConvertStatus(status, "AlignExternalImageMemory");
        }
    }
    if (result == Result::Success)
    {
        GpuMemoryInternalCreateInfo internal = {};
        internal.flags.isExternal = 1;
        internal.flags.isClient = 1;
        internal.hExternalResource = hExternal;
        if (createInfo.flags.gl2Uncached) { internal.mtype = MType::Uncached; } // 351930 with 312620's mtype 3
        object->m_desc.uniqueId = uniqueId;
        object->m_externalPrepared = true;
        result = object->Init(createInfo, internal);
        object->m_externalPrepared = false;
    }
    if (result == Result::Success) { *memory = object; }
    else { object->Destroy(); }
    return result;
}

// =====================================================================================================================
Result GpuMemory::SelectAllocationCount(gpusize size, bool eligible, bool fallBackToOne)
{
    m_allocationCount = 1;
    if (!eligible || !m_pDevice->Settings().enableWddmAllocChunking || m_pDevice->PhysicalEnginesAvailable() ||
        (size < Private::ChunkThreshold))
    {
        return Result::Success;
    }
    const uint32 count = Private::ChunkCount(size);
    m_chunks = PAL_NEW_ARRAY(Allocation, count, m_pDevice->GetPlatform(), Util::AllocInternal);
    if (m_chunks == nullptr)
    {
        // 37e870 falls back to its single inline handle; 37eb10/37ebc0 would write through the null array.
        return fallBackToOne ? Result::Success : Result::ErrorOutOfMemory;
    }
    m_allocationCount = count;
    return Result::Success;
}

// =====================================================================================================================
// Native slot 9 (37e870).
Result GpuMemory::Init(const GpuMemoryCreateInfo &createInfo, const GpuMemoryInternalCreateInfo &internalInfo)
{
    GpuMemoryCreateFlags ordinaryFlags = {};
    ordinaryFlags.gl2Uncached = 1;
    m_ordinaryGartCandidate = internalInfo.flags.isClient && !internalInfo.flags.isExternal &&
        ((createInfo.flags.u64All & ~ordinaryFlags.u64All) == 0) &&
        (createInfo.vaRange == VaRange::Default) && (createInfo.pImage == nullptr) &&
        (createInfo.mallPolicy == GpuMemMallPolicy::Default);
    if ((createInfo.flags.interprocess || createInfo.flags.shareable) && !internalInfo.flags.isExternal)
    {
        // Native 37ef20 creates the KMT resource (interprocess) and 380960 marks the BO shared.
        m_sharedResource = true;
        m_sharedViaNt = (createInfo.flags.sharedViaNtHandle != 0);
    }
    if (createInfo.vaRange == VaRange::Svm)
    {
        if ((m_device->GetSvmMgr() == nullptr) || internalInfo.flags.isExternal)
        { return Result::Unsupported; }
        if (createInfo.flags.virtualAlloc && (createInfo.virtualAccessMode >= VirtualGpuMemAccessMode::Count))
        { return Result::ErrorInvalidValue; }
        const Result validation = Pal::GpuMemory::ValidateCreateInfo(m_device, createInfo);
        if (validation != Result::Success) { return validation; }
        if (createInfo.flags.useReservedGpuVa)
        {
            const auto& owner = *static_cast<const GpuMemory*>(createInfo.pReservedGpuVaOwner);
            if (owner.GetDevice() != m_device) { return Result::ErrorIncompatibleDevice; }
            m_svmGpuBorrowed = true;
            m_svmCpuAddress = owner.Desc().gpuVirtAddr;
        }
    }
    m_virtualMode = createInfo.virtualAccessMode;
    if (createInfo.flags.useReservedGpuVa && (createInfo.pReservedGpuVaOwner != nullptr))
    {
        const auto* owner = static_cast<const GpuMemory*>(createInfo.pReservedGpuVaOwner);
        m_allocation.zeroOnUnmap = (owner->m_virtualMode == VirtualGpuMemAccessMode::ReadZero);
    }
    // Device::InitPageFaultDebugSrd reserves this exact range before creating
    // the backing memory, and Device::Cleanup releases it after destruction.
    m_pageFaultDebugReservation = (internalInfo.flags.pageFaultDebugSrd != 0);

    // 37e870: virtual and SDI objects have no KMT allocation; large CPU-invisible ordinary allocations may be
    // backed by 128 MiB allocations.
    m_nativeCpuInvisible = !createInfo.flags.virtualAlloc && NativeCpuInvisible(createInfo);
    if (createInfo.flags.virtualAlloc || createInfo.flags.sdiExternal)
    {
        m_allocationCount = 0;
    }
    else
    {
        const bool eligible = !createInfo.flags.shareable && !createInfo.flags.interprocess &&
            !createInfo.flags.xdmaBuffer && !createInfo.flags.busAddressable && !createInfo.flags.privPrimary &&
            (createInfo.pImage == nullptr) && !internalInfo.flags.pageDirectory &&
            !internalInfo.flags.pageTableBlock && !internalInfo.flags.userQueue && m_nativeCpuInvisible;
        SelectAllocationCount(createInfo.size, eligible, true);
    }
    Result result = Pal::GpuMemory::Init(createInfo, internalInfo);
    if (((result == Result::Success) || (result == Result::TooManyFlippableAllocations)) &&
        createInfo.flags.sdiExternal)
    {
        // 37e870 SDI tail: the object owns no allocation; given bus addresses register the remote surface (3160b0).
        m_ownsAllocation = false;
        if ((createInfo.surfaceBusAddr != 0) || (createInfo.markerBusAddr != 0))
        {
            SdiMemoryFields fields;
            fields.externPhys     = true;
            fields.size           = m_desc.size;
            fields.surfaceBusAddr = createInfo.surfaceBusAddr;
            fields.markerBusAddr  = createInfo.markerBusAddr;
            m_desc.surfaceBusAddr = createInfo.surfaceBusAddr;
            m_desc.markerBusAddr  = createInfo.markerBusAddr;
            const EscapeRoute route = ControlsEscapeRoute(*m_device, m_device->OsTransport());
            const Result registered = RegisterSdiRemoteSurface(SharedControlEscape(&route),
                                                               m_device->ChipProperties().gfxTriple.major, &fields);
            m_desc.gpuVirtAddr = fields.gpuVirtAddr;
            SetRemoteSdiSurfaceIndex(fields.surfaceIndex);
            SetRemoteSdiMarkerIndex(fields.markerIndex);
            SetBusAddrMarkerVa(fields.markerVa);
            if (registered != Result::Success) { result = registered; }
        }
    }
    return result;
}

// =====================================================================================================================
// Native slot 8 (37eb10): pinned memory of at least 256 MiB is pinned as 128 MiB allocations.
Result GpuMemory::Init(const PinnedGpuMemoryCreateInfo& createInfo)
{
    const Result result = SelectAllocationCount(createInfo.size, true, false);
    return (result == Result::Success) ? Pal::GpuMemory::Init(createInfo) : result;
}

// =====================================================================================================================
// Native slot 7 (37ebc0, base 352680).
Result GpuMemory::Init(const SvmGpuMemoryCreateInfo& createInfo)
{
    if (m_device->GetSvmMgr() == nullptr) { return Result::Unsupported; }
    if (createInfo.size == 0) { return Result::ErrorInvalidValue; }
    const Result validation = Pal::GpuMemory::ValidateSvmInfo(m_device, createInfo);
    if (validation != Result::Success) { return validation; }
    if (createInfo.flags.useReservedGpuVa)
    {
        if (createInfo.pReservedGpuVaOwner == nullptr) { return Result::ErrorInvalidPointer; }
        const auto& owner = *static_cast<const GpuMemory*>(createInfo.pReservedGpuVaOwner);
        if (owner.GetDevice() != m_device) { return Result::ErrorIncompatibleDevice; }
        if (owner.IsVirtual() || owner.IsPinned() || owner.IsShared()) { return Result::Unsupported; }
        if ((owner.VirtAddrPartition() != VaPartition::Svm) || !owner.m_svmVaOwned ||
            (owner.Desc().size < createInfo.size)) { return Result::ErrorInvalidValue; }
        const gpusize alignment = Util::Max(createInfo.alignment,
                                             m_device->MemoryProperties().realMemAllocGranularity);
        if ((owner.Desc().gpuVirtAddr % alignment) != 0) { return Result::ErrorInvalidAlignment; }
        m_svmBorrowed = true;
        m_svmCpuAddress = owner.Desc().gpuVirtAddr;
    }
    // 352680 records interprocess (bit2); other flags besides useReservedGpuVa/gl2Uncached have no effect.
    m_flags.interprocess = createInfo.flags.interprocess;
    Result result = SelectAllocationCount(createInfo.size, true, false);
    if (result == Result::Success) { result = Pal::GpuMemory::Init(createInfo); }
    // The public coarse-grain staging contract uses independent CPU and GPU
    // addresses. Common initialization assigns this pointer from the GPUVA.
    if (result == Result::Success)
    { m_pPinnedMemory = reinterpret_cast<const void*>(uintptr_t(m_svmCpuAddress)); }
    return result;
}

// =====================================================================================================================
// Native slot 12 (37f7a0).
Result GpuMemory::AllocateSvmVirtualAddress(gpusize base, gpusize size, gpusize alignment, bool commitCpu)
{
    if (m_device->GetSvmMgr() == nullptr) { return Result::Unsupported; }
    if (m_svmVaOwned) { return Result::ErrorUnavailable; }
    const gpusize granularity = m_device->MemoryProperties().realMemAllocGranularity;
    alignment = Util::Max(alignment, granularity);
    if ((size == 0) || (granularity == 0) ||
        (alignment > UINT32_MAX) || !Util::IsPowerOfTwo(alignment)) { return Result::ErrorInvalidValue; }
    if (size > UINT64_MAX - (granularity - 1)) { return Result::ErrorInvalidValue; }
    size = Util::Pow2Align(size, granularity);
    gpusize address = 0;
    Result result = Result::Success;
    if (base != 0)
    {
        if (base != m_svmCpuAddress) { return Result::ErrorInvalidValue; }
        if (m_svmBorrowed && commitCpu)
        {
            MEMORY_BASIC_INFORMATION region = {};
            if ((VirtualQuery(reinterpret_cast<void*>(uintptr_t(base)), &region, sizeof(region)) == 0) ||
                (region.State != MEM_RESERVE) || (region.Type != MEM_PRIVATE) || (region.RegionSize < size) ||
                (uint64(reinterpret_cast<uintptr_t>(region.AllocationBase)) != m_device->GetSvmMgr()->GetStartAddr()))
            { return Result::ErrorUnavailable; }
        }
        else if (!m_svmGpuBorrowed || commitCpu) { return Result::Unsupported; }
        address = base;
    }
    else
    {
        result = m_device->GetSvmMgr()->AllocVa(size, uint32(alignment), &address);
        m_svmVaOwned = (result == Result::Success);
    }
    if (result == Result::Success)
    {
        m_svmCpuAddress = address;
        m_desc.gpuVirtAddr = address;
        m_desc.size = size;
        m_desc.alignment = alignment;
        if (commitCpu)
        {
            result = Util::VirtualCommit(reinterpret_cast<void*>(uintptr_t(address)), size_t(size));
            m_svmCpuCommitted = (result == Result::Success);
        }
        if (result == Result::Success)
        { m_pPinnedMemory = reinterpret_cast<const void*>(uintptr_t(address)); }
    }
    return result;
}

Result GpuMemory::FreeSvmVirtualAddress()
{
    if (!m_svmVaOwned && !m_svmCpuCommitted) { return Result::Success; }
    if (m_device->GetSvmMgr() == nullptr) { return Result::ErrorUnavailable; }
    if (m_svmCpuCommitted)
    {
        const Result result = Util::VirtualDecommit(reinterpret_cast<void*>(uintptr_t(m_svmCpuAddress)),
                                                     size_t(m_desc.size));
        if (result != Result::Success) { return result; }
        m_svmCpuCommitted = false;
    }
    if (m_svmVaOwned) { m_device->GetSvmMgr()->FreeVa(m_svmCpuAddress); }
    m_svmVaOwned = false;
    m_svmCpuAddress = 0;
    return Result::Success;
}

// =====================================================================================================================
// Native slot 1 (37e430).
GpuMemory::~GpuMemory()
{
    if (IsPresentable() && (std::getenv("PAL_WDDM_TRACE") != nullptr))
    {
        std::fprintf(stderr, "[pal-wddm] presentable memory destroy va=%llx size=%llx\n",
                     static_cast<unsigned long long>(m_desc.gpuVirtAddr), static_cast<unsigned long long>(m_desc.size));
    }
    // 37e430 destroys the keyed mutex (Platform+0x2b20) and its synchronization object of an opened resource. They are
    // independent kernel objects; this port releases them before the allocation teardown below, whose quarantine
    // paths return early.
    if (m_keyedMutex != 0)
    {
        D3DKMT_DESTROYKEYEDMUTEX destroy = {};
        destroy.hKeyedMutex = m_keyedMutex;
        D3DKMTDestroyKeyedMutex(&destroy);
        m_keyedMutex = 0;
    }
    if (m_keyedMutexSync != 0)
    {
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
        destroy.hSyncObject = m_keyedMutexSync;
        D3DKMTDestroySynchronizationObject(&destroy);
        m_keyedMutexSync = 0;
    }
    if ((VirtAddrPartition() == VaPartition::Svm) && IsVirtual() &&
        (m_svmVaOwned || m_svmGpuBorrowed))
    {
        if (!m_device->OsTransport().IsOpen()) { return; }
        if (m_svmVirtualMapped)
        {
            // Restore only the child PTEs. FreeVa would release the entire
            // common SVM pool reservation on the verified KMD.
            const bool zero = m_svmGpuBorrowed && m_allocation.zeroOnUnmap;
            if (m_device->OsTransport().ResetVa(m_desc.gpuVirtAddr, m_desc.size, zero) != Success)
            { m_device->OsTransport().Quarantine(); return; }
        }
    }
    if (m_virtualReservation != 0)
    {
        if (m_device->FreeGpuVirtualAddress(m_desc.gpuVirtAddr, m_virtualReservation) != Result::Success)
        {
            m_device->OsTransport().Quarantine();
        }
    }
    else if (m_chunks != nullptr)
    {
        Status status = Success;
        if (!m_ownsAllocation)
        {
            for (uint32 index = 0; (status == Success) && (index < m_allocationCount); ++index)
            {
                if (m_chunks[index].gpuAddress != 0)
                {
                    status = m_device->OsTransport().UnmapAllocation(&m_chunks[index]);
                }
            }
        }
        else if (m_chunks[0].handle != 0)
        {
            status = m_device->OsTransport().DestroyAllocations(m_chunks, m_allocationCount);
        }
        if (status != Success)
        {
            m_device->OsTransport().Quarantine();
            return;
        }
    }
    else if (m_allocation.handle != 0)
    {
        if (m_standaloneAllocation != 0)
        {
            // DirectCapture (317dd0): the capture allocation is not part of the opened resource; the original leaves
            // it to device destruction. This port unmaps and destroys it by handle, then the resource below.
            Allocation standalone = m_allocation;
            standalone.resource = 0;
            const Status status = m_device->OsTransport().DestroyAllocation(&standalone);
            if (status != Success) { m_device->OsTransport().Quarantine(); return; }
            Allocation resource = {};
            resource.handle = m_standaloneAllocation; // not read: a non-zero resource is destroyed as a whole
            resource.resource = m_allocation.resource;
            resource.ordinaryLegacy = true;
            m_allocation = resource;
            m_standaloneAllocation = 0;
        }
        // Borrowed handles (37f4e0/37f6c0 clear ownership) only lose this object's mapping.
        const Status status = m_ownsAllocation ? m_device->OsTransport().DestroyAllocation(&m_allocation)
                                               : m_device->OsTransport().UnmapAllocation(&m_allocation);
        if (status != Success)
        {
            m_device->OsTransport().Quarantine();
            return;
        }
    }
    if (m_ownedReservation != 0)
    {
        if (m_device->FreeGpuVirtualAddress(m_desc.gpuVirtAddr, m_ownedReservation) != Result::Success)
        {
            m_device->OsTransport().Quarantine();
        }
    }
    // KMT backing has retired before CPU pages can be decommitted or reused.
    // A failed native destruction above deliberately retains both lifetimes.
    if (FreeSvmVirtualAddress() != Result::Success) { m_device->OsTransport().Quarantine(); }
    if (IsExternPhys() && (m_desc.gpuVirtAddr != 0) && m_device->OsTransport().IsOpen())
    {
        // 37e430: release the registered SDI surface (3160b0 record with operation 2, hardware access).
        auto record = BuildSdiSurfaceRecord(m_device->ChipProperties().gfxTriple.major, m_desc.size,
                                            m_desc.surfaceBusAddr, m_desc.markerBusAddr);
        PutRecord32(record.data(), 0xe0, 2);
        SharedEscape(m_device->KmdEscapeRoute(), record.data(), record.size(), true, 0);
    }
    if (m_chunks != nullptr) { PAL_DELETE_ARRAY(m_chunks, m_pDevice->GetPlatform()); m_chunks = nullptr; }
}

// =====================================================================================================================
void GpuMemory::FillNativeDesc(Private::NativeAllocationDesc* desc, Private::NativeImageDesc* image,
                               Private::NativeTypedBufferDesc* typed) const
{
    desc->gpuVirtAddr       = m_desc.gpuVirtAddr;
    desc->size              = m_desc.size;
    desc->clientSize        = m_desc.clientSize;
    desc->physicalAlignment = GetPhysicalAddressAlignment();
    desc->uniqueId          = m_desc.uniqueId;
    desc->allocationCount   = Util::Max(m_allocationCount, 1u);
    desc->adapterIndex      = 0;
    desc->abiVersion        = Private::AbiVersion;
    desc->surfaceBytes      = 288;
    const auto& chip        = m_device->ChipProperties();
    desc->gfxLevelWord      = (uint32(chip.gfxTriple.major) << 24) | (uint32(chip.gfxTriple.minor) << 16);
    desc->integrated        = (chip.gpuType == GpuType::Integrated);
    desc->boFlag200         = false;
    desc->heapCount         = m_heapCount;
    for (uint32 index = 0; index < m_heapCount; ++index)
    {
        desc->heaps[index] = uint32(m_heaps[index]);
    }
    for (uint32 heap = 0; heap < 4; ++heap)
    {
        desc->heapSizes[heap] = m_pDevice->HeapLogicalSize(static_cast<GpuHeap>(heap));
    }
    desc->priority          = uint32(Priority());
    desc->priorityOffset    = uint32(PriorityOffset());
    desc->mtype             = uint32(Mtype());
    desc->mallPolicy        = uint32(MallPolicy());
    desc->mallStartPage     = MallRange().startPage;
    desc->mallNumPages      = MallRange().numPages;
    desc->virtualAlloc      = IsVirtual();
    desc->pinned            = IsPinned();
    desc->kmdShareUmdSysMem = false;
    desc->client            = IsClient();
    desc->shareable         = IsShareable();
    desc->interprocess      = IsInterprocess();
    desc->sharedViaNtHandle = m_sharedViaNt;
    desc->cpuInvisible      = m_nativeCpuInvisible;
    desc->cpuVisible        = IsCpuVisible() && !m_nativeCpuInvisible;
    desc->stereo            = IsStereo();
    desc->privateScreen     = IsPrivateScreen();
    desc->globallyCoherent  = IsGloballyCoherent();
    desc->xdmaBuffer        = IsXdmaBuffer();
    desc->peerWritable      = IsPeerWritable();
    desc->tmzProtected      = IsTmzProtected();
    desc->busAddressable    = IsBusAddressable();
    desc->accessedPhysically = IsAccessedPhysically();
    desc->privPrimary       = IsPrivPrimary();
    desc->mallRangeActive   = IsMallRangeActive();
    desc->flippable         = IsFlippable();
    desc->restrictedContent = IsRestrictedContent();
    desc->restrictedAccess  = IsRestrictedAccess();
    desc->crossAdapter      = IsCrossAdapter();
    desc->deferCpuVaReservation = IsLockableOnDemand();
    desc->dfSpmTraceBuffer  = IsDfSpmTraceBuffer();
    // 309c00: object+344 bit3 from Device+0xcb40 (GPU record DWORD 14176) bit23.
    uint32 gpuCaps = 0;
    std::memcpy(&gpuCaps, m_device->Adapter().physical.gpuRecord.data() + 14176, sizeof(gpuCaps));
    desc->singleSurface     = ((gpuCaps & (1u << 23)) != 0);
    if (m_pImage != nullptr)
    {
        FillNativeImageDesc(*m_pImage, IsFlippable(), image);
        desc->image = image;
    }
    else if (IsTypedBuffer())
    {
        typed->format        = uint32(m_typedBufferInfo.swizzledFormat.format);
        std::memcpy(&typed->swizzle, &m_typedBufferInfo.swizzledFormat.swizzle, sizeof(typed->swizzle));
        typed->width         = m_typedBufferInfo.extent.width;
        typed->height        = m_typedBufferInfo.extent.height;
        typed->depth         = m_typedBufferInfo.extent.depth;
        typed->rowPitch      = m_typedBufferInfo.rowPitch;
        typed->depthPitch    = m_typedBufferInfo.depthPitch;
        typed->depthIsSubres = m_typedBufferInfo.depthIsSubres;
        desc->typedBuffer    = typed;
    }
}

// =====================================================================================================================
// Native 37ec70 for the objects whose requests this backend builds from the native records: images, typed buffers,
// multi-allocation, above-4-GiB, shared, protected, bus-addressable and MALL-policy objects.
Result GpuMemory::AllocateNative(gpusize base, uint64 *pagingFence)
{
    const VaPartition partition = m_svmBorrowed ? VaPartition::Default : VirtAddrPartition();
    const auto &range = m_device->MemoryProperties().vaRange[static_cast<uint32>(partition)];
    if (m_desc.uniqueId == 0)
    {
        // 351930 publishes the creator ID before the allocation metadata is built.
        m_desc.uniqueId = GpuUtil::GenerateGpuMemoryUniqueId(IsInterprocess());
    }
    gpusize mapBase = base;
    if (m_svmBorrowed)
    {
        gpusize address = 0;
        const Result result = m_device->AssignVirtualAddress(*this, &address, VaPartition::Default);
        if (result != Result::Success) { return result; }
        m_desc.gpuVirtAddr = address;
        m_ownedReservation = Util::Pow2Align(m_desc.size, gpusize(65536));
        mapBase = address;
    }
    else if (IsGpuVaPreReserved())
    {
        m_desc.gpuVirtAddr = base;
    }
    else if (!m_pageFaultDebugReservation)
    {
        const bool unreserved = !IsVirtual() && !IsUserQueue() && !IsGlobalGpuVa() && (m_allocationCount <= 1) &&
                                (m_desc.alignment == GetPhysicalAddressAlignment());
        if (!unreserved && (partition != VaPartition::Svm))
        {
            // 3817b0: rounded size plus alignment padding, the descriptor VA aligned inside it.
            gpusize address = base;
            const gpusize bytes = Util::Pow2Align(m_desc.size, gpusize(65536));
            const Result result = (base == 0)
                ? m_device->AssignVirtualAddress(*this, &address, partition)
                : m_device->ReserveGpuVirtualAddress(partition, base, bytes, false,
                                                     VirtualGpuMemAccessMode::Undefined, &address);
            if (result != Result::Success) { return result; }
            m_desc.gpuVirtAddr = address;
            m_ownedReservation = bytes;
            mapBase = address;
        }
    }
    Private::NativeAllocationDesc desc;
    Private::NativeImageDesc image;
    Private::NativeTypedBufferDesc typed;
    FillNativeDesc(&desc, &image, &typed);
    Private::NativeAllocationRequest request;
    const int32_t built = Private::BuildNativeAllocationRequest(desc, &request);
    if (built == 10) { return Result::NotFound; } // 380960: no requested heap exists on this device.
    if (built != 0) { return Result::Unsupported; }
    Allocation* allocations = (m_chunks != nullptr) ? m_chunks : &m_allocation;
    uint32 resource = 0, globalShare = 0;
    const Status status = m_device->OsTransport().CreateNativeAllocations(
        request, IsPinned() ? const_cast<void*>(m_pPinnedMemory) : nullptr, mapBase, range.baseVirtAddr,
        range.baseVirtAddr + range.size, allocations, &resource, &globalShare);
    if (status == Success)
    {
        m_desc.gpuVirtAddr = allocations[0].gpuAddress;
        m_sharedGlobal = globalShare;
    }
    (void)pagingFence;
    // 37ef20 translates the create status with 2d07c0; mapping/residency statuses use the same translator.
    const Result result = ConvertNativeStatus(status);
    TraceResult(result, "CreateNativeAllocations");
    return result;
}

// =====================================================================================================================
// Native slot 14 (37ec70).
Result GpuMemory::AllocateOrPinMemory(gpusize base, uint64 *pagingFence, VirtualGpuMemAccessMode mode,
                                      uint32 deviceCount, IDevice *const *devices, Pal::Image *const *images)
{
    if (pagingFence != nullptr)
    {
        *pagingFence = 0;
    }
    if (IsVirtual())
    {
        if (VirtAddrPartition() == VaPartition::Svm)
        {
            const Status status = m_device->OsTransport().ResetVa(
                m_desc.gpuVirtAddr, m_desc.size, mode == VirtualGpuMemAccessMode::ReadZero);
            m_svmVirtualMapped = (status == Success);
            return ConvertStatus(status, "Initialize virtual SVM pages");
        }
        Result result = m_device->AssignVirtualAddress(*this, &m_desc.gpuVirtAddr, VirtAddrPartition(), base, mode);
        m_virtualReservation = (result == Result::Success) ? Util::Pow2Align(m_desc.size, gpusize(65536)) : 0;
        return result;
    }
    // Objects whose KMD request differs from the two existing encoders take the native record builder. Every image
    // and typed buffer does: their entries carry the SURF records (380e00/3814c0) the KMD uses to recognise them.
    m_nativeRoute = (m_allocationCount > 1) || (m_desc.size > UINT32_MAX) || (m_desc.alignment > UINT32_MAX) ||
                    m_sharedResource || IsTmzProtected() || IsBusAddressable() || IsMallRangeActive() ||
                    (MallPolicy() != GpuMemMallPolicy::Default) || (m_pImage != nullptr) || IsTypedBuffer();
    if (m_nativeRoute)
    {
        // The user-queue entry fields (3802c0 with 2cc5f0) are not modelled; no WDDM path creates such memory.
        if (IsUserQueue()) { return Result::Unsupported; }
        Result result = AllocateNative(base, pagingFence);
        if ((result == Result::Success) && IsPrivateScreen() && !IsAlwaysResident())
        {
            result = RegisterPrivateScreenFlipAddress();
        }
        return result;
    }
    const VaPartition partition = m_svmBorrowed ? VaPartition::Default : VirtAddrPartition();
    const auto &range = m_device->MemoryProperties().vaRange[static_cast<uint32>(partition)];
    const bool cpuVisible = (PreferredHeap() != GpuHeapInvisible);
    const MemoryHeap heap = IsPinned()                            ? MemoryHeap::GartCacheable
                            : PreferredHeap() == GpuHeapLocal     ? MemoryHeap::Local
                            : PreferredHeap() == GpuHeapInvisible ? MemoryHeap::Invisible
                            : PreferredHeap() == GpuHeapGartUswc  ? MemoryHeap::GartUswc
                                                                  : MemoryHeap::GartCacheable;
    bool ordinaryGart = m_ordinaryGartCandidate && !IsPinned() && !IsSvmAlloc() &&
        (partition != VaPartition::Svm) && !m_svmBorrowed && !m_svmGpuBorrowed &&
        (m_heapCount != 0) && (m_heapCount <= 4) && (m_desc.size < 0x10000000);
    for (uint32 index = 0; ordinaryGart && (index < m_heapCount); ++index)
    {
        ordinaryGart = (m_heaps[index] == GpuHeapGartUswc) || (m_heaps[index] == GpuHeapGartCacheable);
    }
    if (ordinaryGart && (m_desc.uniqueId == 0))
    {
        // Original351930 publishes the creator ID before allocation metadata.
        // The existing WDDM common-tail guard preserves this preassigned ID.
        m_desc.uniqueId = GpuUtil::GenerateGpuMemoryUniqueId(false);
    }
    uint64 requested = m_svmBorrowed ? 0 : base;
    if (m_svmBorrowed || (!IsGpuVaPreReserved() && !m_pageFaultDebugReservation &&
                          (partition != VaPartition::Svm)))
    {
        const gpusize reservationBytes = Util::Pow2Align(m_desc.size, gpusize(65536));
        Result result = (requested == 0)
                            ? m_device->AssignVirtualAddress(*this, &requested, partition)
                            : m_device->ReserveGpuVirtualAddress(partition, requested, reservationBytes, false,
                                                                 VirtualGpuMemAccessMode::Undefined, &requested);
        if (result != Result::Success)
        {
            return result;
        }
        m_desc.gpuVirtAddr = requested;
        m_ownedReservation = reservationBytes;
    }
    Status status;
    if (ordinaryGart)
    {
        const gpusize physicalAlignment = GetPhysicalAddressAlignment();
        Private::OrdinaryGartAllocationInfo info;
        info.size = uint32(m_desc.size);
        info.physicalAlignment = uint32(physicalAlignment);
        info.address = requested;
        info.uniqueId = m_desc.uniqueId;
        info.heapCount = m_heapCount;
        for (uint32 index = 0; index < m_heapCount; ++index)
        {
            // Native3816c0 converts later Cacheable preferences to USWC.
            // The first preference remains exactly the translated client heap.
            info.heapIds[index] = (m_heaps[index] == GpuHeapGartCacheable && index == 0) ? 4 : 3;
        }
        info.priority = static_cast<uint32>(Priority());
        info.priorityOffset = static_cast<uint32>(PriorityOffset());
        info.mtype = static_cast<uint32>(Mtype());
        info.mallPolicy = static_cast<uint32>(MallPolicy());
        Private::AllocationData records;
        status = Private::OrdinaryGartAllocationRecords(info, &records);
        if (status == Success)
        {
            status = m_device->OsTransport().Allocate(
                m_desc.size, m_desc.alignment, cpuVisible, false, nullptr, requested, &m_allocation,
                range.baseVirtAddr, range.baseVirtAddr + range.size,
                Mtype() == MType::Uncached ? CacheMode::Gl2Bypass : CacheMode::Default, heap,
                static_cast<uint32>(Priority()), static_cast<uint32>(PriorityOffset()), false, &records);
        }
    }
    else
    {
        status = m_device->OsTransport().Allocate(
            m_desc.size, m_desc.alignment, cpuVisible, true, IsPinned() ? const_cast<void *>(m_pPinnedMemory) : nullptr,
            requested, &m_allocation, range.baseVirtAddr, range.baseVirtAddr + range.size - 1,
            Mtype() == MType::Uncached ? CacheMode::Gl2Bypass : CacheMode::Default, heap,
            static_cast<uint32>(Priority()), static_cast<uint32>(PriorityOffset()), false);
    }
    if (ordinaryGart ? (status == Success) : Succeeded(status))
    {
        m_desc.gpuVirtAddr = m_allocation.gpuAddress;
    }
    Result result = ConvertStatus(status);
    // 37ec70: private-screen memory that is not always resident registers its flip address after mapping.
    if ((result == Result::Success) && IsPrivateScreen() && !IsAlwaysResident())
    {
        result = RegisterPrivateScreenFlipAddress();
    }
    return result;
}

// =====================================================================================================================
// Native 381bb0. Record 0xc8, command 0x03006003: +0x80 input 0x20, +0x84 private-screen index, +0x88 image id,
// +0x8c allocation handle (translated by 318150), +0xa4 output 0x20, status at +0xa8.
Result RegisterFlipAddress(const EscapeRoute& route, const FlipAddressRegistration& input,
                           TranslateHandleFn translate, void* context)
{
    if (!input.hasPrivateScreen) { return Result::Success; }
    std::array<uint8, 0xc8> record = {};
    InitPrivateRecord(record.data(), record.size(), 0x03006003);
    PutRecord32(record.data(), 0x0c, 3);
    PutRecord32(record.data(), 0x80, 0x20);
    PutRecord32(record.data(), 0x84, input.screenIndex);
    PutRecord32(record.data(), 0x88, input.imageId);
    PutRecord32(record.data(), 0xa4, 0x20);
    // 318150: WDDM >= 2.5 and the KMD's translate capability -> kernel handle (0 if the escape fails).
    uint32 handle = input.allocation;
    if ((input.wddmVersion >= 0x2500) && ((input.gpuCaps & 2) != 0))
    {
        uint32 translated = input.allocation;
        handle = (translate(context, &translated) == Success) ? translated : 0;
    }
    PutRecord32(record.data(), 0x8c, handle);
    Result result = SharedEscape(route, record.data(), record.size());
    if (result == Result::Success)
    {
        // Port: statuses beyond the native seven-entry table map to ErrorUnknown (the original reads past it).
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    return result;
}

Result GpuMemory::RegisterPrivateScreenFlipAddress()
{
    const Pal::Image* image = m_pImage;
    FlipAddressRegistration input;
    input.hasPrivateScreen = (image != nullptr) && (image->GetPrivateScreen() != nullptr);
    if (!input.hasPrivateScreen) { return Result::Success; }
    input.screenIndex = image->GetPrivateScreenIndex();
    input.imageId     = image->GetPrivateScreenImageId();
    input.allocation  = OsAllocation().handle;
    std::memcpy(&input.wddmVersion, m_device->Adapter().physical.basicRecord.data() + 2224, sizeof(input.wddmVersion));
    std::memcpy(&input.gpuCaps, m_device->Adapter().physical.gpuRecord.data() + 2920, sizeof(input.gpuCaps));
    auto translate = [](void* context, uint32* handle) -> Status
    {
        uint32 translated = 0;
        const Status status = static_cast<Transport*>(context)->TranslateAllocationHandle(*handle, &translated);
        if (status == Success) { *handle = translated; }
        return status;
    };
    return RegisterFlipAddress(m_device->KmdEscapeRoute(), input, translate, &m_device->OsTransport());
}

// =====================================================================================================================
// Maps another object's KMT allocation for this object (native 37f4e0/37f6c0 tails through 3817b0 and 381870).
Result GpuMemory::MapBorrowed(const GpuMemory& original, bool reserve)
{
    // The borrowed handles are only valid on the KMT device that created them: the original's opener shares
    // its adapter object (Device+0xd9c0) with the creator.
    if (&original.m_device->OsTransport() != &m_device->OsTransport()) { return Result::ErrorIncompatibleDevice; }
    const Allocation& source = original.OsAllocation();
    if (source.handle == 0) { return Result::ErrorInvalidValue; }
    m_allocation = {};
    m_allocation.handle = source.handle;
    m_allocation.resource = source.resource;
    m_allocation.size = m_desc.size;
    m_allocation.resident = source.resident;
    m_allocation.ordinaryLegacy = true;
    m_ownsAllocation = false;
    gpusize mapBase = m_desc.gpuVirtAddr;
    if (reserve)
    {
        gpusize address = mapBase;
        const gpusize bytes = Util::Pow2Align(m_desc.size, gpusize(65536));
        const Result result = (mapBase == 0)
            ? m_device->AssignVirtualAddress(*this, &address, VirtAddrPartition())
            : m_device->ReserveGpuVirtualAddress(VirtAddrPartition(), mapBase, bytes, false,
                                                 VirtualGpuMemAccessMode::Undefined, &address);
        if (result != Result::Success) { return result; }
        m_desc.gpuVirtAddr = address;
        m_ownedReservation = bytes;
        mapBase = address;
    }
    const auto& range = m_device->MemoryProperties().vaRange[static_cast<uint32>(VirtAddrPartition())];
    // 381870 with wait=1: the paging fence is settled before returning (the transport waits synchronously).
    const Status status = m_device->OsTransport().MapAllocation(&m_allocation, mapBase, false, range.baseVirtAddr,
                                                                range.baseVirtAddr + range.size);
    if (status == Success) { m_desc.gpuVirtAddr = m_allocation.gpuAddress; }
    else { m_allocation = {}; }
    return ConvertStatus(status, "MapBorrowedMemory");
}

// =====================================================================================================================
// Native slot 15 (37f4e0).
Result GpuMemory::OpenSharedMemory(OsExternalHandle handle)
{
    if (m_externalPrepared)
    {
        // 37f4e0: a KMD-assigned VA (object+344 & 0x30) stays the descriptor's; nothing was mapped for it.
        if (!m_kmdAddress)
        {
            m_desc.gpuVirtAddr = m_allocation.gpuAddress;
            m_desc.size = m_allocation.size;
        }
        return Result::Success;
    }
    if ((handle == nullptr) && IsShared() && (m_pOriginalMem != nullptr))
    {
        // Non-external open: borrow the creator's allocation and resource; this object does not own them.
        const auto* original = static_cast<const GpuMemory*>(m_pOriginalMem);
        m_sharedViaNt = original->m_sharedViaNt;
        m_sharedGlobal = original->m_sharedGlobal;
        Result result = MapBorrowed(*original, IsGlobalGpuVa());
        if ((result == Result::Success) && IsPrivateScreen()) { result = RegisterPrivateScreenFlipAddress(); }
        return result;
    }
    // The recovered NT-resource import protocol covers data buffers, including
    // the shared linear buffers used for DXGI presentation.
    Status status = m_device->OsTransport().OpenSharedAllocation(handle, false, &m_allocation);
    if (Succeeded(status))
    {
        m_desc.gpuVirtAddr = m_allocation.gpuAddress;
        m_desc.size = m_allocation.size;
    }
    return ConvertStatus(status);
}

// =====================================================================================================================
// Native slot 16 (37f6c0): borrow the original's handle list, reserve VA for multi-allocation objects, map (wait).
Result GpuMemory::OpenPeerMemory()
{
    const auto* original = static_cast<const GpuMemory*>(m_pOriginalMem);
    if (original == nullptr) { return Result::ErrorInvalidPointer; }
    m_allocationCount = original->m_allocationCount;
    if (m_allocationCount <= 1)
    {
        return MapBorrowed(*original, false);
    }
    if (&original->m_device->OsTransport() != &m_device->OsTransport()) { return Result::ErrorIncompatibleDevice; }
    m_chunks = PAL_NEW_ARRAY(Allocation, m_allocationCount, m_pDevice->GetPlatform(), Util::AllocInternal);
    if (m_chunks == nullptr) { return Result::ErrorOutOfMemory; }
    m_ownsAllocation = false;
    // 3817b0 with the descriptor VA (the original's VA for globalGpuVa/SVM peers, otherwise 0).
    gpusize address = m_desc.gpuVirtAddr;
    const gpusize bytes = Util::Pow2Align(m_desc.size, gpusize(65536));
    Result result = (address == 0)
        ? m_device->AssignVirtualAddress(*this, &address, VirtAddrPartition())
        : m_device->ReserveGpuVirtualAddress(VirtAddrPartition(), address, bytes, false,
                                             VirtualGpuMemAccessMode::Undefined, &address);
    if (result != Result::Success) { return result; }
    m_desc.gpuVirtAddr = address;
    m_ownedReservation = bytes;
    const auto& range = m_device->MemoryProperties().vaRange[static_cast<uint32>(VirtAddrPartition())];
    for (uint32 index = 0; index < m_allocationCount; ++index)
    {
        const Allocation& source = original->m_chunks[index];
        m_chunks[index].handle = source.handle;
        m_chunks[index].size = source.size;
        m_chunks[index].resident = source.resident;
        m_chunks[index].ordinaryLegacy = true;
        const Status status = m_device->OsTransport().MapAllocation(&m_chunks[index],
            address + (uint64(index) << Private::ChunkShift), false, range.baseVirtAddr,
            range.baseVirtAddr + range.size);
        if (status != Success)
        {
            result = ConvertStatus(status, "MapPeerMemory");
            break;
        }
    }
    if (result != Result::Success)
    {
        // Undo this object's mappings; the handles stay owned by the original.
        for (uint32 index = 0; index < m_allocationCount; ++index)
        {
            if (m_chunks[index].gpuAddress != 0) { m_device->OsTransport().UnmapAllocation(&m_chunks[index]); }
            m_chunks[index] = {};
        }
    }
    return result;
}

// =====================================================================================================================
// Native slot 5 (380240) with 35ddf0 (NT export through D3DKMTShareObjects).
OsExternalHandle GpuMemory::ExportExternalHandle(const GpuMemoryExportInfo& exportInfo) const
{
    if (!m_sharedViaNt)
    {
        return reinterpret_cast<OsExternalHandle>(uintptr_t(m_sharedGlobal));
    }
    void* handle = nullptr;
    ShareResourceNt(m_device->OsTransport(), OsAllocation().resource, exportInfo.pSecurityAttributes,
                    exportInfo.pName, exportInfo.ntHandleAccess, &handle);
    return handle;
}

// =====================================================================================================================
void GpuMemory::AdoptKeyedMutex(uint32 globalShare, uint32 keyedMutex, uint32 syncObject)
{
    m_keyedMutexGlobalShare = globalShare;
    m_keyedMutex            = keyedMutex;
    m_keyedMutexSync        = syncObject;
}

// =====================================================================================================================
// Native slot 10 (3800c0). The parent chain of 37fba0 (object+256) is always empty in this backend.
Result GpuMemory::AcquireKeyedMutex(IQueue* pQueue, uint64 key, uint64 timeoutMs)
{
    Result result = Result::Success;
    if ((m_keyedMutex == 0) || (m_keyedMutexSync == 0))
    {
        // 37fd10: a KMT-opened resource gets its keyed mutex on first use, from the global share handle.
        result = OpenSharedResourceKeyedMutex(m_device->OsTransport(), m_device->GetPlatform(),
                                              m_keyedMutexGlobalShare, &m_keyedMutex, &m_keyedMutexSync);
    }
    if (result == Result::Success)
    {
        // 37fba0: a relative timeout of -10000 * ms (100 ns units) and the acquired fence value at object+336.
        LARGE_INTEGER timeout = {};
        timeout.QuadPart = int64(timeoutMs) * -10000LL;
        D3DKMT_ACQUIREKEYEDMUTEX acquire = {};
        acquire.hKeyedMutex = m_keyedMutex;
        acquire.Key         = key;
        acquire.pTimeout    = &timeout;
        result = ConvertNativeStatus(D3DKMTAcquireKeyedMutex(&acquire));
        if (result == Result::Success) { m_keyedMutexFence = acquire.FenceValue; }
    }
    if (result == Result::Success)
    {
        // 3800c0: the queue's first context (*(queue+0x228)) waits for the synchronization object at that value.
        const uint32 context = static_cast<Queue*>(pQueue)->NativeQueue().context;
        D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {};
        wait.hContext          = context;
        wait.ObjectCount       = 1;
        wait.ObjectHandleArray = &m_keyedMutexSync;
        wait.FenceValue        = m_keyedMutexFence;
        result = ConvertNativeStatus(D3DKMTWaitForSynchronizationObjectFromGpu(&wait));
    }
    return result;
}

// =====================================================================================================================
// Native slot 11 (380170) with 37fc70.
Result GpuMemory::ReleaseKeyedMutex(IQueue* pQueue, uint64 key)
{
    const uint32 context = static_cast<Queue*>(pQueue)->NativeQueue().context;
    ++m_keyedMutexFence;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {};
    signal.ObjectCount           = 1;
    signal.ObjectHandleArray     = &m_keyedMutexSync;
    signal.BroadcastContextCount = 1;
    signal.BroadcastContextArray = &context;
    signal.FenceValue            = m_keyedMutexFence;
    Result result = ConvertNativeStatus(D3DKMTSignalSynchronizationObjectFromGpu2(&signal));
    if (result == Result::Success)
    {
        D3DKMT_RELEASEKEYEDMUTEX release = {};
        release.hKeyedMutex = m_keyedMutex;
        release.Key         = key;
        release.FenceValue  = m_keyedMutexFence;
        result = ConvertNativeStatus(D3DKMTReleaseKeyedMutex(&release));
    }
    return result;
}

// =====================================================================================================================
// Native slot 17 (37f960).
Result GpuMemory::OsSetPriority(GpuMemPriority priority, GpuMemPriorityOffset offset)
{
    // The native setter indexes these six bases, then adds its fine offset <<8.
    constexpr uint32 priorities[] = {0x28000000, 0x28000000, 0x50000000,
                                      0x78000000, 0xa0000000, 0xc8000000};
    const uint32 baseIndex = static_cast<uint32>(priority);
    const uint32 offsetIndex = static_cast<uint32>(offset);
    if ((baseIndex >= static_cast<uint32>(GpuMemPriority::Count)) ||
        (offsetIndex >= static_cast<uint32>(GpuMemPriorityOffset::Count)))
    {
        return Result::ErrorInvalidValue;
    }
    if (m_allocationCount == 0) { return Result::Success; }
    if (m_chunks != nullptr)
    {
        return ConvertStatus(m_device->OsTransport().SetAllocationPriority(
            m_chunks, m_allocationCount, priorities[baseIndex] + (offsetIndex << 8)), "SetAllocationPriority");
    }
    return ConvertStatus(m_device->OsTransport().SetAllocationPriority(
        m_allocation, priorities[baseIndex] + (offsetIndex << 8)), "SetAllocationPriority");
}

Result GpuMemory::OsMap(void **data)
{
    if (data == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }
    // Native 37f9f0 locks the first allocation of the object.
    return m_cpuMap.Map(UseCachedCpuMap(), [this](void** address)
        { return ConvertStatus(m_device->OsTransport().LockCpuPerCall(OsAllocation(), address)); }, data);
}

Result GpuMemory::OsUnmap()
{
    return m_cpuMap.Unmap(UseCachedCpuMap(), [this]()
        { return ConvertStatus(m_device->OsTransport().UnlockCpuPerCall(OsAllocation())); });
}

bool GpuMemory::UseCachedCpuMap() const
{
    // Native3816c0 excludes client nonlocal memory, then calls314f20.
    // The getter's Device+d314 bit8 comes from the GPU query DWORD16180.
    uint32 flags = 0;
    std::memcpy(&flags, m_device->Adapter().physical.gpuRecord.data() + 16180, sizeof(flags));
    return IsCpuVisible() && !(IsClient() && IsNonLocalOnly()) && ((flags & 0x100) != 0);
}

// Native slot 6 (381d00) via the shared field-view implementation.
Result GpuMemory::SetSdiRemoteBusAddress(gpusize surfaceBusAddr, gpusize markerBusAddr)
{
    SdiMemoryFields fields;
    fields.externPhys     = IsExternPhys();
    fields.size           = m_desc.size;
    fields.gpuVirtAddr    = m_desc.gpuVirtAddr;
    fields.surfaceBusAddr = m_desc.surfaceBusAddr;
    fields.markerBusAddr  = m_desc.markerBusAddr;
    fields.surfaceIndex   = GetRemoteSdiSurfaceIndex();
    fields.markerIndex    = GetRemoteSdiMarkerIndex();
    fields.markerVa       = GetBusAddrMarkerVa();
    const EscapeRoute route = ControlsEscapeRoute(*m_device, m_device->OsTransport());
    const Result result = Pal::Wddm::SetSdiRemoteBusAddress(SharedControlEscape(&route),
                                                            m_device->ChipProperties().gfxTriple.major,
                                                            surfaceBusAddr, markerBusAddr, &fields);
    m_desc.gpuVirtAddr    = fields.gpuVirtAddr;
    m_desc.surfaceBusAddr = fields.surfaceBusAddr;
    m_desc.markerBusAddr  = fields.markerBusAddr;
    SetRemoteSdiSurfaceIndex(fields.surfaceIndex);
    SetRemoteSdiMarkerIndex(fields.markerIndex);
    SetBusAddrMarkerVa(fields.markerVa);
    return result;
}

// Native slot 21 (3816c0). Its cached-CPU-map output is already reproduced lazily by UseCachedCpuMap() from the same
// inputs, so only the heap changes are applied. Desc().heaps keeps the client view, as in the original.
void GpuMemory::OsFinalizeHeaps()
{
    HeapFinalizeFields fields;
    fields.localOnly              = IsLocalOnly();
    fields.nonLocalOnly           = IsNonLocalOnly();
    fields.cpuVisible             = IsCpuVisible();
    fields.client                 = IsClient();
    fields.pageDirectory          = IsPageDirectory();
    fields.pageTableBlock         = IsPageTableBlock();
    fields.flippable              = IsFlippable();
    fields.tmzProtected           = IsTmzProtected();
    fields.busAddressable         = IsBusAddressable();
    fields.uswcHeapAllAllocations = m_pDevice->Settings().enableUswcHeapAllAllocations;
    fields.heapCount              = m_heapCount;
    for (uint32 index = 0; index < m_heapCount; ++index) { fields.heaps[index] = m_heaps[index]; }
    FinalizeHeaps(&fields);
    m_flags.localOnly = fields.localOnly ? 1 : 0;
    m_heapCount       = fields.heapCount;
    for (uint32 index = 0; index < m_heapCount; ++index) { m_heaps[index] = fields.heaps[index]; }
}
} // namespace Pal::Wddm
