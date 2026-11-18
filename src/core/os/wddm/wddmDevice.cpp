/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#if PAL_WDDM_BUILD

#include "core/hw/amdgpu_asic.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmFence.h"
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmPlatform.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmImage.h"
#include "core/os/wddm/wddmSwapChain.h"
#include "core/os/wddm/wddmClock.h"
#include "core/os/wddm/wddmClockMode.h"
#include "core/os/wddm/wddmPower.h"
#include "core/os/wddm/wddmDriverVersion.h"
#include "core/os/wddm/wddmQueueLimit.h"
#include "core/os/wddm/wddmWorkstation.h"
#include "core/os/wddm/wddmStaticVmid.h"
#include "core/os/wddm/wddmDmaUploadRing.h"
#include "core/os/wddm/wddmCpuWaitArray.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmExternalImage.h"
#include "core/os/wddm/wddmSharedResource.h"
#include "core/queueSemaphore.h"
#include "palFormatInfo.h"
#include "palGpuUtil.h"
#include "palSettingsFileMgrImpl.h"
#include "palSysMemory.h"
#include "palInlineFuncs.h"

#include <windows.h>
#include <VersionHelpers.h>
#include <limits.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Util;

namespace Pal
{
namespace Wddm
{

// =====================================================================================================================
Device::Device(Platform *pPlatform, const char *pSettingsPath, const GpuInfo &gpuInfo,
               const HwIpDeviceSizes &hwDeviceSizes, const AdapterInfo &adapter)
    : Pal::Device(pPlatform,
                  0, // deviceIndex, we only have one
                  0, // Refreshed from exact-LUID desktop topology during screen queries.
                  sizeof(Device), hwDeviceSizes,
                  UINT_MAX), // max semaphore count
      m_gpuInfo(gpuInfo), m_adapter(adapter), m_applicationProfile(pPlatform),
      m_flgl(FlglRoute{}, static_cast<Pal::Platform *>(pPlatform)), m_settingFileMgr(SettingsFileName, pPlatform),
      m_pSettingsPath(pSettingsPath)
{
    Strncpy(&m_gpuName[0], gpuInfo.pGpuName, sizeof(m_gpuName));
}

// =====================================================================================================================
// Factory function for the reconstructed WDDM device on a verified adapter profile.
Result Device::Create(Platform *pPlatform, const char *pSettingsPath, Device **ppDeviceOut, const AdapterInfo &adapter)
{
    Result result = Result::ErrorInitializationFailed;

    // Determine if the GPU is supported by PAL, and if so, what its hardware IP levels are.
    GpuInfo gpuInfo = {};
    gpuInfo.asicRev = AsicRevision::StrixHalo;
    gpuInfo.familyId = adapter.physical.familyId;
    gpuInfo.eRevId = adapter.physical.externalRevision;
    gpuInfo.revisionId = adapter.revisionId;
    gpuInfo.gfxEngineId = adapter.physical.gfxEngineId;
    gpuInfo.deviceId = adapter.deviceId;
    gpuInfo.pGpuName = "AMD Radeon 8060S";
    HwIpLevels ipLevels = {};
    if (Transport::IsVerifiedAdapter(adapter) &&
        Pal::Device::DetermineGpuIpLevels(gpuInfo.familyId, gpuInfo.eRevId, adapter.physical.cpFeatureVersion,
                                          pPlatform, &ipLevels))
    {
        const size_t deviceSize = sizeof(Device);

        size_t addrMgrSize = 0;
        HwIpDeviceSizes hwDeviceSizes = {};
        GetHwIpDeviceSizes(ipLevels, &hwDeviceSizes, &addrMgrSize);
        const size_t neededMemSize = deviceSize + hwDeviceSizes.gfx + addrMgrSize;
        // GfxDevice contains 32-byte-aligned state immediately after this object.
        void *pMemory = PAL_MALLOC_ALIGNED(neededMemSize, alignof(Device), pPlatform, Util::AllocInternal);

        if (pMemory != nullptr)
        {
            (*ppDeviceOut) =
                PAL_PLACEMENT_NEW(pMemory) Device(pPlatform, pSettingsPath, gpuInfo, hwDeviceSizes, adapter);

            result = ConvertStatus((*ppDeviceOut)->m_transport.Open(adapter));
            TraceResult(result, "Transport::Open");
            if (result == Result::Success)
            {
                result = (*ppDeviceOut)->EarlyInit(ipLevels);
            }
            TraceResult(result, "Device::EarlyInit");
            if (result != Result::Success)
            {
                (*ppDeviceOut)->Cleanup(); // Ignore result; we've already failed.
                (*ppDeviceOut)->~Device();
                PAL_SAFE_FREE(pMemory, pPlatform);
                *ppDeviceOut = nullptr;
            }
        }
        else
        {
            result = Result::ErrorOutOfMemory;
        }
    }

    return result;
}

// =====================================================================================================================
// Original Device slot 129 (313650).
Result Device::AddEmulatedPrivateScreen(const PrivateScreenCreateInfo &createInfo, uint32 *pTargetId)
{
    return AddEmulatedScreen(this, {m_pEmulatedPrivateScreens, &m_emulatedPrivateScreens, &m_emulatedTargetId},
                             m_finalizeInfo.privateScreenNotifyInfo, createInfo, pTargetId);
}

// =====================================================================================================================
Result Device::AssignVirtualAddress(const Pal::GpuMemory &gpuMemory, gpusize *pGpuVirtAddr, VaPartition vaPartition,
                                    gpusize base, VirtualGpuMemAccessMode mode)
{
    return ReserveVa(vaPartition, base, Util::Pow2Align(gpuMemory.Desc().size, gpusize(65536)), gpuMemory.IsVirtual(),
                     mode, pGpuVirtAddr, Util::Max(gpuMemory.Desc().alignment, gpusize(65536)));
}

// =====================================================================================================================
// Captures a GPU timestamp with the corresponding CPU timestamps, allowing tighter CPU/GPU timeline synchronization.
Result Device::GetCalibratedTimestamps(CalibratedTimestamps *pCalibratedTimestamps) const
{
    return QueryCalibratedTimestamps(m_transport, pCalibratedTimestamps);
}

Result Device::GetProperties(DeviceProperties* properties) const
{
    if (properties == nullptr) { return Result::ErrorInvalidPointer; }
    const Result result = Pal::Device::GetProperties(properties);
    if (result == Result::Success)
    {
        // Original 30fa64..30fa8f: DeviceProperties+0x1a90 = adapter LUID HighPart, +0x1a94 = LowPart.
        properties->osProperties.luidHighPart = static_cast<uint32>(m_adapter.luidHigh);
        properties->osProperties.luidLowPart = m_adapter.luidLow;
        // Original 30fa95..30faaf: +0x1a98 = 2ffe40, VerifyVersionInfoW(6.2 SP0, major|minor|SP major >=), i.e.
        // IsWindows8OrGreater(); the Vulkan client gates its NT handle types on it.
        properties->osProperties.supportNtSharedHandles = IsWindows8OrGreater();
        properties->osProperties.timelineSemaphore.support = 1;
        properties->osProperties.timelineSemaphore.supportHostQuery = 1;
        properties->osProperties.timelineSemaphore.supportHostWait = 1;
        properties->osProperties.timelineSemaphore.supportHostSignal = 1;
        properties->osProperties.timelineSemaphore.supportWaitBeforeSignal = 1;
        // Original 30fcda writes 0x01010101 at +0x1aa0: supportQueuePriority, supportDynamicQueuePriority,
        // supportMemoryBudgetQuery (AMDGPU-only in this interface) and a newer bool. SetExecutionPriority (31abb0) is
        // the software store of Pal::Queue.
        properties->osProperties.supportQueuePriority = true;
        properties->osProperties.supportDynamicQueuePriority = true;
        properties->osProperties.timeDomains.supportDevice = 1;
        properties->osProperties.timeDomains.supportQueryPerformanceCounter = 1;
        // Original 30fc29..30fc3d copies the EarlyInit caps.
        properties->osProperties.virtualDisplayCaps = m_virtualDisplayCaps;

        // Original 30fa9a..30fcfb. Device+0xcb4c/+0xcb5c/+0xd314/+0xd324 are GPU record DWORDs 14188, 14204, 16180,
        // 16196 (copied to Device+0x9d40 from record offset 2400); Device+0x9cd4 is BASIC record DWORD 2900.
        const uint8 *gpu = m_adapter.physical.gpuRecord.data();
        const uint32 display = GetRecord32(gpu, 14188);
        const uint32 caps = GetRecord32(gpu, 16180);
        const uint32 basic = GetRecord32(m_adapter.physical.basicRecord.data(), 2900);
        auto &flags = properties->osProperties.flags;
        flags.supportTurboSync = (display >> 2) & 1;
        flags.enableUmdFpsCap = (display >> 5) & 1;
        flags.isCwgSupported = m_kmdFeatureFlags & 1;
        flags.isGamingDriver = (m_kmdFeatureFlags >> 1) & 1;
        flags.placeholder0 = 1;
        flags.ifhModeEnabled = (Settings().ifh != IfhModeDisabled) ? 1 : 0;
        flags.requireFrameEnd = (caps >> 3) & 1;
        flags.supportNativeHdrWindowing = 1;
        flags.flipQueueSupportsDecodeDst = (display >> 14) & 1;
        flags.supportFreeMux = (basic >> 25) & 1;
        flags.u32All = (flags.u32All & ~(1u << 12)) | (((caps >> 6) & 1) << 12); // Bit 12 of the newer interface.
        flags.forceAlignmentSupported = (caps >> 12) & 1;
        properties->osProperties.umdFpsCapFrameRate = GetRecord32(gpu, 14204);
        auto &capture = properties->osProperties.directCapture;
        capture.supportPostflip = (display >> 11) & 1;
        capture.supportPreflip = (display >> 12) & 1;
        capture.supportRSync = (display >> 13) & 1;
        capture.maxFrameGenRatio = GetRecord32(gpu, 16196) & 0xf;
        capture.supportNonPrimary = (display >> 18) & 1;
        capture.u32All = (capture.u32All & ~(1u << 8)) | (((display >> 19) & 1) << 8); // Bit 8 of the newer interface.
    }
    return result;
}

Result Device::SetClockMode(const SetClockModeInput& input, SetClockModeOutput* output)
{
    return ApplyClockMode(m_transport, input, output,
        Settings().neverChangeClockMode ? ClockModePolicy::QueryOnly : ClockModePolicy::AllowChanges,
        m_chipProperties.maxMemoryClock, m_chipProperties.maxEngineClock);
}

Result Device::OsSetStaticVmidMode(bool enable)
{
    return ApplyStaticVmid(m_transport, enable);
}

Result Device::QueryRadeonSoftwareVersion(char* buffer, size_t length) const
{
    return QueryDriverSoftwareVersion(m_transport, buffer, length);
}

Result Device::QueryReleaseVersion(char* buffer, size_t length) const
{
    return QueryDriverReleaseVersion(m_transport, buffer, length);
}

// =====================================================================================================================
Result Device::Cleanup()
{
    // Original slot 10 (30a750; 30a90d..30a95c) stops private-screen notifications first and skips the base
    // cleanup when unregistering the wait fails.
    Result base = m_privateScreenNotifier.Stop(KmdEscapeRoute());
    if (base == Result::Success)
    {
        base = Pal::Device::Cleanup();
    }
    const Result svm = CleanupSvmPool();
    return (base == Result::Success) ? svm : base;
}

// =====================================================================================================================
Pal::GpuMemory *Device::ConstructGpuMemoryObject(void *pPlacementAddr)
{
    return PAL_PLACEMENT_NEW(pPlacementAddr) GpuMemory(this);
}

// =====================================================================================================================
Pal::Queue *Device::ConstructQueueObject(const QueueCreateInfo &createInfo, void *pPlacementAddr)
{
    // 4d8c00 sends the one-time gang escape for implicit ACE gangs of ordinary queues too.
    Pal::Queue *pQueue = PAL_PLACEMENT_NEW(pPlacementAddr) Queue(1, this, &createInfo, &m_gangSubmitNotified);

    return pQueue;
}

// =====================================================================================================================
// Native Device slot 195 (RVA 3109c0): the queue object size when every entry has a hardware-scheduled instance of its
// engine type and a queue type of 0..2, otherwise 0.
size_t Device::MultiQueueObjectSize(uint32 queueCount, const QueueCreateInfo *pCreateInfo) const
{
    return GangMultiQueueObjectSize(m_adapter, queueCount, pCreateInfo, sizeof(Queue));
}

// =====================================================================================================================
// Native Device slot 197 (RVA 310ae0): constructs the HWS queue with queueCount sub-queues.
Pal::Queue *Device::ConstructMultiQueueObject(uint32 queueCount, const QueueCreateInfo *pCreateInfo,
                                              void *pPlacementAddr)
{
    if ((pPlacementAddr == nullptr) || !GangCanConstructMultiQueue(m_adapter, queueCount, pCreateInfo))
    {
        return nullptr;
    }
    return PAL_PLACEMENT_NEW(pPlacementAddr) Queue(queueCount, this, pCreateInfo, &m_gangSubmitNotified);
}

// =====================================================================================================================
// Creates and initializes a new Image object.
Result Device::CreateImage(const ImageCreateInfo &createInfo, void *pPlacementAddr, IImage **ppImage)
{
    if ((pPlacementAddr == nullptr) || (ppImage == nullptr))
    {
        return Result::ErrorInvalidPointer;
    }
    ImageInternalCreateInfo internal = {};
    Pal::Image *image = nullptr;
    Result result = CreateInternalImage(createInfo, internal, pPlacementAddr, &image);
    *ppImage = image;
    return result;
}

// =====================================================================================================================
// Creates and initializes a new Image object.
Result Device::CreateInternalImage(const ImageCreateInfo &createInfo, const ImageInternalCreateInfo &internalCreateInfo,
                                   void *pPlacementAddr, Pal::Image **ppImage)
{
    auto* image = PAL_PLACEMENT_NEW(pPlacementAddr) Image(this, createInfo, internalCreateInfo);
    *ppImage = image;
    // 3111f0: an optimally shareable image gets a new cross-process ID (published by 3802c0 at KMD metadata+0xb4)
    // unless it adopts shared metadata, whose ID Wddm::Image::Init (381d80) takes instead.
    if (createInfo.flags.optimalShareable != 0)
    {
        image->SetSharedResourceId((internalCreateInfo.flags.useSharedMetadata == 0)
                                       ? GpuUtil::GenerateGpuMemoryUniqueId(true)
                                       : internalCreateInfo.sharedMetadata.resourceId);
    }
    Result result = (*ppImage)->Init();
    if (result != Result::Success)
    {
        (*ppImage)->Destroy();
        *ppImage = nullptr;
    }
    return result;
}

// =====================================================================================================================
Result Device::CreatePresentableImage(const PresentableImageCreateInfo &createInfo, void *pImagePlacementAddr,
                                      void *pGpuMemoryPlacementAddr, IImage **ppImage, IGpuMemory **ppGpuMemory)
{
    return SwapChain::CreatePresentableImage(this, createInfo, pImagePlacementAddr, pGpuMemoryPlacementAddr, ppImage,
                                             ppGpuMemory);
}

// =====================================================================================================================
// Determines the size in bytes of a Fence object.
size_t Device::GetFenceSize(Result *pResult) const
{
    if (pResult != nullptr)
    {
        (*pResult) = Result::Success;
    }

    return sizeof(Fence);
}

// =====================================================================================================================
// Creates a new Fence object in preallocated memory provided by the caller.
Result Device::CreateFence(const FenceCreateInfo &createInfo, void *pPlacementAddr, IFence **ppFence) const
{
    if (ppFence == nullptr) { return Result::ErrorInvalidPointer; }
    *ppFence = nullptr;
    if (pPlacementAddr == nullptr) { return Result::ErrorInvalidPointer; }

    Fence *pFence = PAL_PLACEMENT_NEW(pPlacementAddr) Fence(const_cast<Device *>(this));

    // Set needsEvent argument to true - all client-created fences require event objects to support the
    // IDevice::WaitForFences interface.
    Result result = pFence->Init(createInfo);

    if (result != Result::Success)
    {
        pFence->Destroy();
        pFence = nullptr;
    }

    (*ppFence) = pFence;

    return result;
}

// =====================================================================================================================
// Open/Reconstruct the pFence from a handle or a name.
Result Device::OpenFence(const FenceOpenInfo &openInfo, void *pPlacementAddr, IFence **ppFence) const
{
    if (ppFence == nullptr) { return Result::ErrorInvalidPointer; }
    *ppFence = nullptr;
    if (pPlacementAddr == nullptr) { return Result::ErrorInvalidPointer; }

    Fence *pFence = PAL_PLACEMENT_NEW(pPlacementAddr) Fence(const_cast<Device *>(this));

    Result result = pFence->OpenHandle(openInfo);

    if (result != Result::Success)
    {
        pFence->Destroy();
        pFence = nullptr;
    }

    (*ppFence) = pFence;

    return result;
}

// =====================================================================================================================
// Native slot 111 (317700): an NT handle to a named shared resource (type 0) or synchronization object (type 1).
Result Device::OpenExternalHandleByName(const ExternalHandleNameInfo &info, OsExternalHandle *pHandle)
{
    return OpenNamedNtHandle(uint32(info.type), info.pSecurityAttributes, info.pName, info.access, pHandle);
}

// =====================================================================================================================
// Call to wait for multiple semaphores
Result Device::WaitForSemaphores(uint32 semaphoreCount, const IQueueSemaphore *const *ppSemaphores,
                                 const uint64 *pValues, uint32 flags, std::chrono::nanoseconds timeout) const
{
    return WaitCpuPayloadObjects(m_transport.DeviceHandle(), semaphoreCount, ppSemaphores, pValues,
        (flags & HostWaitAny) != 0, timeout.count(), GetPlatform(),
        [](const IQueueSemaphore* item)
        {
            return static_cast<const Pal::QueueSemaphore*>(item)->NativeSync().handle;
        }, timeout == std::chrono::nanoseconds::max(), nullptr,
        +[](const IQueueSemaphore* item)
        {
            // These public object/type restrictions are outside the native
            // valid-timeline subset. Native array allocation precedes them.
            const auto* semaphore = static_cast<const Pal::QueueSemaphore*>(item);
            if (!semaphore->IsTimeline()) { return Result::ErrorInvalidObjectType; }
            return semaphore->NativeSync().gpuOnly ? Result::Unsupported : Result::Success;
        });
}

// =====================================================================================================================
// Create swap chain based on local window system.
Result Device::CreateSwapChain(const SwapChainCreateInfo &createInfo, void *pPlacementAddr, ISwapChain **ppSwapChain)
{
    return SwapChain::Create(this, createInfo, pPlacementAddr, ppSwapChain);
}

// =====================================================================================================================
// Original Device slot 39 (0x312bd0).
Result Device::DetermineExternalSharedResourceType(const ExternalResourceOpenInfo &openInfo, bool *pIsImage) const
{
    return Wddm::DetermineExternalResourceType(const_cast<Device *>(this), openInfo, pIsImage);
}

// =====================================================================================================================
// Physical properties are queried from the versioned AMD KMD records. Hardware defaults
// come from the existing PAL GFX layer; fuse masks and firmware never come from nullDevice.
void Device::InitGfx9ChipProperties()
{
    const auto &physical = m_adapter.physical;
    // Basic query byte2920 is the vendor's CP Feature field; firmware versions
    // come from separate GPU-query fields. See the binary reader evidence.
    m_chipProperties.cpUcodeVersion = physical.cpFeatureVersion;
    m_chipProperties.meUcodeVersion = physical.meFirmwareVersion;
    m_chipProperties.mecUcodeVersion = physical.mecFirmwareVersion;
    m_chipProperties.pfpUcodeVersion = physical.pfpFirmwareVersion;
    m_chipProperties.gpuCounterFrequency = physical.crystalFrequencyHz;
    m_chipProperties.gpuType = GpuType::Integrated;
    SetClockModeOutput limits = {};
    const Result clockLimits = QueryClockLimits(m_transport, m_chipProperties.gpuType, &limits);
    // The native initializer retains raw GPU-record WORD limits on Escape failure.
    TraceResult(clockLimits, "QueryClockLimits");
    m_chipProperties.maxEngineClock = limits.engineClockFrequency;
    m_chipProperties.maxMemoryClock = limits.memoryClockFrequency;
    Gfx9::InitializeGpuChipProperties(GetPlatform(), m_chipProperties.cpUcodeVersion, &m_chipProperties);
    // The gated profile accepted the recovered native static-VMID enable and
    // disable requests, including the response's operation-supported bit.
    m_chipProperties.gfxip.supportStaticVmid = 1;
    auto &chip = m_chipProperties.gfx9;
    chip.gbAddrConfig = physical.gbAddrConfig;
    chip.numShaderEngines = physical.numShaderEngines;
    chip.numShaderArrays = physical.numShaderArraysPerEngine;
    chip.maxNumRbPerSe = physical.numRbPipes / physical.numShaderEngines;
    chip.numPhysicalVgprsPerSimd = physical.numShaderVisibleVgprs;
    chip.nativeWavefrontSize = physical.wavefrontSize;
    chip.numTccBlocks = physical.numTccBlocks;
    chip.gsVgtTableDepth = physical.gsVgtTableDepth;
    chip.gsPrimBufferDepth = physical.gsPrimBufferDepth;
    chip.maxGsWavesPerVgt = physical.maxGsWavesPerVgt;
    chip.numCuPerSh = 0;
    chip.maxNumCuPerSh = 0;
    for (uint32 se = 0; se < chip.numShaderEngines; ++se)
    {
        for (uint32 sa = 0; sa < chip.numShaderArrays; ++sa)
        {
            const uint16 active = physical.wgpBitmap[se][sa];
            // GFX11 has no independently switchable always-on CUs; Linux PAL likewise
            // uses the active bitmap for its always-on bitmap on GFX11.
            chip.gfx10.activeWgpMask[se][sa] = active;
            chip.gfx10.alwaysOnWgpMask[se][sa] = active;
            uint32 cuMask = 0;
            for (uint32 wgp = 0; wgp < 16; ++wgp)
            {
                if (active & (1u << wgp))
                {
                    cuMask |= 3u << (wgp * 2);
                }
            }
            chip.activeCuMask[se][sa] = cuMask;
            chip.alwaysOnCuMask[se][sa] = cuMask;
            chip.numCuPerSh = Util::Max(chip.numCuPerSh, Util::CountSetBits(cuMask));
            chip.maxNumCuPerSh =
                Util::Max(chip.maxNumCuPerSh, (active == 0) ? 0u : 2u * (Util::Log2(static_cast<uint32>(active)) + 1));
        }
    }
    chip.numActiveRbs = physical.numEnabledRbPipes;
    chip.backendDisableMask = static_cast<uint32>(~physical.enabledRbMask) & ((1u << physical.numRbPipes) - 1);
    Gfx9::FinalizeGpuChipProperties(*this, &m_chipProperties);
    // Buffer-only sparse mapping has direct GPU readback/order/lifecycle proof.
    // Sparse image metadata and shader residency features have separate gates.
    m_chipProperties.imageProperties.prtFeatures = PrtFeatureBuffer;
    // Apply harvested RBs after deriving the total from the ASIC topology.
    chip.numActiveRbs = physical.numEnabledRbPipes;
    chip.backendDisableMask = static_cast<uint32>(~physical.enabledRbMask) & ((1u << chip.numTotalRbs) - 1);
}

// =====================================================================================================================
Result Device::EarlyInit(const HwIpLevels &ipLevels)
{
    Result result = Result::Success;

    // The FLGL/connector escapes use the open KMT adapter/device; the native
    // constructor already zeroed the 12-byte FLGL cache (2f12d7..2f12e3).
    m_flgl.SetRoute({m_transport.AdapterHandle(), m_transport.DeviceHandle(), 0, false});

    m_chipProperties.familyId = m_gpuInfo.familyId;
    m_chipProperties.deviceId = m_gpuInfo.deviceId;
    m_chipProperties.eRevId = m_gpuInfo.eRevId;
    m_chipProperties.revisionId = m_gpuInfo.revisionId;
    m_chipProperties.gfxEngineId = m_gpuInfo.gfxEngineId;
    m_chipProperties.gpuIndex = 0;

    m_chipProperties.gfxLevel = IpTripleToGfxLevel(ipLevels.gfx);
    m_chipProperties.gfxTriple = ipLevels.gfx;
    m_chipProperties.vcnLevel = ipLevels.vcn;
    m_chipProperties.hwIpFlags.u32All = ipLevels.flags.u32All;

    for (uint32 i = 0; i < EngineTypeCount; i++)
    {
        m_engineProperties.perEngine[i].preferredCmdAllocHeaps[CommandDataAlloc] = GpuHeapGartUswc;
        m_engineProperties.perEngine[i].preferredCmdAllocHeaps[EmbeddedDataAlloc] = GpuHeapGartUswc;
        m_engineProperties.perEngine[i].preferredCmdAllocHeaps[LargeEmbeddedDataAlloc] = GpuHeapGartUswc;
        m_engineProperties.perEngine[i].preferredCmdAllocHeaps[GpuScratchMemAlloc] = GpuHeapInvisible;
    }

    for (uint32 i = 0; i < EngineTypeCount; i++)
    {
        switch (i)
        {
        case EngineTypeUniversal:
        case EngineTypeCompute:
        case EngineTypeDma:
            m_engineProperties.perEngine[i].flags.supportsTrackBusyChunks = 1;
            break;
        default:
            m_engineProperties.perEngine[i].flags.supportsTrackBusyChunks = 0;
            break;
        }
    }

    switch (m_chipProperties.gfxTriple.major)
    {
    case 10:
    case 11:
        m_pFormatPropertiesTable =
            Gfx9::GetFormatPropertiesTable(m_chipProperties.gfxLevel, GetPlatform()->PlatformSettings());

        InitGfx9ChipProperties();
        Gfx9::InitializeGpuEngineProperties(m_chipProperties, &m_engineProperties);
        break;

#if PAL_BUILD_GFX12
    case 12:
        m_pFormatPropertiesTable = Gfx12::GetFormatPropertiesTable(m_chipProperties.gfxLevel);

        InitGfx12ChipProperties();
        Gfx12::InitializeGpuEngineProperties(m_chipProperties, &m_engineProperties);
        break;
#endif

    case 0:
        // No Graphics IP block found or recognized!
    default:
        break;
    }

    if (result == Result::Success)
    {
        result = InitMemoryProperties();
        TraceResult(result, "InitMemoryProperties");

        if (result == Result::Success)
        {
            InitMemoryHeapProperties();
            // General shared creation remains unadvertised until the original
            // allocation carrier and borrowed KMT ownership path match.
            for (auto& heap : m_heapProperties)
            {
                heap.flags.shareable = 0;
            }
        }
    }

    // Init paths
    InitOutputPaths();

    if (result == Result::Success)
    {
        result = m_settingFileMgr.Init(m_pSettingsPath);
        TraceResult(result, "SettingsFileMgr::Init");

        if (result == Result::ErrorUnavailable)
        {
            // Unavailable means that the file was not found, which is an acceptable failure.
            PAL_DPINFO("No settings file loaded.");
            result = Result::Success;
        }
    }

    if (result == Result::Success)
    {
        result = InitSettings();
        TraceResult(result, "InitSettings");
    }

    if ((result == Result::Success) && GetPlatform()->SvmModeEnabled())
    {
        result = InitSvmPool();
        TraceResult(result, "InitSvmPool");
    }

    if (result == Result::Success)
    {
        // Original 30b987..30ba37: KMD count (1 if the query fails), refresh range 45..240 Hz.
        m_virtualDisplayCaps = QueryVirtualDisplayCapabilities(KmdEscapeRoute());

        // Reserve an explicitly requested SVM pool before internal GPU objects
        // consume its common CPU/GPU address interval.
        result = Pal::Device::EarlyInit(ipLevels);
        TraceResult(result, "Pal::Device::EarlyInit");
    }

    return result;
}

// =====================================================================================================================
// Native Device slots 147..149 (316260, 316460, 316b40) over the Finalize-mapped KMD pages.
Result Device::DidRsFeatureSettingsChange(uint32 rsFeatures, uint32 *pRsFeaturesChanged)
{
    return DeviceDidRsFeatureSettingsChange(m_adapter.physical, m_kmdShared, rsFeatures, pRsFeaturesChanged);
}

Result Device::GetRsFeatureGlobalSettings(RsFeatureType rsFeature, RsFeatureInfo *pRsFeatureInfo)
{
    return DeviceGetRsFeatureGlobalSettings(*this, m_transport, m_adapter.physical, m_kmdShared, rsFeature,
                                            pRsFeatureInfo);
}

Result Device::UpdateChillStatus(uint64 lastChillActiveTimeStampUs)
{
    return DeviceUpdateChillStatus(m_adapter.physical, m_kmdShared, lastChillActiveTimeStampUs);
}

// =====================================================================================================================
// Native Device slot 150 (30e340).
Result Device::InitBusAddressableGpuMemory(IQueue *pQueue, uint32 gpuMemCount, IGpuMemory *const *ppGpuMemList)
{
    return DeviceInitBusAddressableGpuMemory(*this, m_transport, m_deviceControls, pQueue, gpuMemCount, ppGpuMemList);
}

// =====================================================================================================================
// Native Device slots 159/160 (3181f0, 318450).
Result Device::RegisterHipRuntimeState(const HipRuntimeSetup &runtimeState) const
{
    return DeviceRegisterHipRuntimeState(*this, m_transport, runtimeState);
}

Result Device::SetHipTrapHandler(const IGpuMemory *pTrapHandlerCode, gpusize codeOffset,
                                 const IGpuMemory *pTrapHandlerMemory, gpusize memoryOffset) const
{
    return DeviceSetHipTrapHandler(*this, m_transport, pTrapHandlerCode, codeOffset, pTrapHandlerMemory, memoryOffset);
}

// =====================================================================================================================
// Native Device slot 180 (30e330): original PalSettings+0x2e0 is commandBufferCombineDePreambles.
void Device::OverrideDefaultSettings(PalSettings *pSettings) const
{
    pSettings->commandBufferCombineDePreambles = true;
}

// =====================================================================================================================
// Native Device slot 190 (30bac0): two RS feature queries and the submit flag, after the settings are final.
void Device::OsFinalizeSettings()
{
    // The settings are final here; the FLGL route reads the same disableEscapeCall setting as every other escape.
    m_flgl.SetRoute({m_transport.AdapterHandle(), m_transport.DeviceHandle(), 0, GetPublicSettings()->disableEscapeCall});
    DeviceOsFinalizeSettings(*this, m_transport, m_adapter.physical, m_deviceControls);
}

// =====================================================================================================================
// Original Device slot 199 (311ba0).
Result Device::EnumPrivateScreensInfo(uint32 *pNumScreens)
{
    return EnumeratePrivateScreens(KmdEscapeRoute(), GetPlatform(), m_privateScreenInfo,
                                   &m_privateScreenNotifier, pNumScreens);
}

// =====================================================================================================================
// Peforms extra initialization which needs to be done when the client is ready to start using the device.
Result Device::Finalize(const DeviceFinalizeInfo &finalizeInfo)
{
    // The original (30dfe0) first creates its KMT paging queue; Transport::Open already owns this backend's queue.
    // Its optional Finalize-time SVM range manager (platform SVM flag, 30e258..30e2d6) corresponds to the SVM pool
    // this backend reserves in EarlyInit.
    Result result = Pal::Device::Finalize(finalizeInfo);
    if (result == Result::Success)
    {
        const EscapeRoute route = KmdEscapeRoute();
        // 30e10f..30e175: KMD feature query; reply +0xc8 feeds the CWG and gaming-driver property flags.
        uint8 record[0x108] = {};
        InitPrivateRecord(record, sizeof(record), 0x03008705);
        PutRecord32(record, 0x80, 0x40);
        PutRecord32(record, 0xc4, 0x40);
        if (SharedEscape(route, record, sizeof(record)) == Result::Success)
        {
            m_kmdFeatureFlags = GetRecord32(record, 0xc8);
        }
        // 30e17b..30e1ef: private-screen hot-plug event and wait.
        if ((m_finalizeInfo.flags.supportPrivateScreens != 0) &&
            (m_privateScreenNotifier.Start(&m_finalizeInfo.privateScreenNotifyInfo) != Result::Success))
        {
            return Result::ErrorInitializationFailed;
        }
        // 30e1f7 (30d620): KMD/UMD shared buffers; every escape failure is ignored.
        InitKmdSharedBuffers(route, finalizeInfo, m_adapter.physical, &m_kmdShared);
        // 30e1fc..30e258: FLGL state from the workstation capabilities; the result is the Finalize result.
        WorkStationCaps caps = {};
        const Result capsResult = QueryWorkstationCapabilities(m_transport, &caps);
        result = m_flgl.InitializeFromCapabilities(capsResult, caps.u32All);
    }
    return result;
}

// =====================================================================================================================
// Helper method which finalizes some of the Queue properties which cannot be determined until the settings are read.
void Device::FinalizeQueueProperties()
{
    m_queueProperties.maxNumCmdStreamsPerSubmit = 64;
    m_engineProperties.maxInternalRefsPerSubmission = InternalMemMgrAllocLimit;
    m_engineProperties.maxUserMemRefsPerSubmission = CmdBufMemReferenceLimit;
    // Native 30d1ba/30d1d4/30d1e1 unconditionally set engine flag bit 15 (supportVirtualMemoryRemap) for the
    // universal (Device+0x1460), compute (+0x15b0) and DMA (+0x1700) engines; clients expose sparse binding on them.
    m_engineProperties.perEngine[EngineTypeUniversal].flags.supportVirtualMemoryRemap = 1;
    m_engineProperties.perEngine[EngineTypeCompute].flags.supportVirtualMemoryRemap = 1;
    m_engineProperties.perEngine[EngineTypeDma].flags.supportVirtualMemoryRemap = 1;
    // Native engine enumeration 30c8b0 -> 30c370 sets capability bit 2 (supportsMultiQueue) per engine instance.
    const GangTopology gangTopology = ReadGangTopology(m_adapter.physical);
    for (uint32 index = 0; index < EngineTypeCount; ++index)
    {
        auto &engine = m_engineProperties.perEngine[index];
        engine.numAvailable = 0;
        engine.sizeAlignInDwords = 8;
        const auto &alignment = m_adapter.physical.ibAlignments[
            (index == EngineTypeDma) ? 2 : (index == EngineTypeCompute) ? 1 : 0];
        if (alignment.start != 0)
        {
            engine.startAlign = alignment.start;
        }
        if (alignment.size != 0)
        {
            engine.sizeAlignInDwords = alignment.size / sizeof(uint32);
        }
        for (const auto &node : m_adapter.nodes)
        {
            if (((index == EngineTypeUniversal) && node.isGraphics) ||
                ((index == EngineTypeCompute) && node.isCompute) || ((index == EngineTypeDma) && node.isDma) ||
                ((index == EngineTypeTimer) && node.isTimer))
            {
                // Recovered private priority encoding, independently exercised
                // on these two nodes of the strictly gated8060S/7029 profile.
                // Other engine instances retain the basic Normal capability.
                const uint32 prioritySupport = node.ordinal == 0 ?
                    (SupportQueuePriorityNormal | SupportQueuePriorityHigh | SupportQueuePriorityRealtime) :
                    node.ordinal == 2 ?
                    (SupportQueuePriorityNormal | SupportQueuePriorityMedium | SupportQueuePriorityHigh) :
                    SupportQueuePriorityNormal;
                engine.capabilities[engine.numAvailable].queuePrioritySupport = prioritySupport;
                engine.capabilities[engine.numAvailable].flags.supportsMultiQueue =
                    GangSupportsMultiQueue(gangTopology, static_cast<EngineType>(index)) ? 1 : 0;
                ++engine.numAvailable;
            }
        }
    }
    // Native 30d1b0 (30d1b0..30d4c4, 30d507..30d517): swap-chain presents on universal and compute queues, the
    // presenter masks and every queue type's direct present modes.
    m_queueProperties.perQueue[QueueTypeUniversal].flags.supportsSwapChainPresents = 1;
    m_queueProperties.perQueue[QueueTypeCompute].flags.supportsSwapChainPresents = 1;
    const auto *platform = static_cast<const Platform *>(GetPlatform());
    m_present.redirectedBltSupported =
        QueryRedirectedBltSupport(m_transport.AdapterHandle(), GetRecord32(m_adapter.physical.basicRecord.data(), 2224));
    PresenterSelectionInputs inputs = {};
    inputs.physicalAdapter = DeviceEscapeRoute(m_transport, GetPlatform()).physicalAdapter;
    inputs.adapterFlags = AdapterPresentFlags();
    inputs.noDisplayAccess = platform->NoDisplayAccess();
    inputs.forcePresentViaCpuBlt = Settings().forcePresentViaCpuBlt;
    inputs.compositionEnabled = IsCompositionEnabled();
    inputs.redirectedBltSupported = m_present.redirectedBltSupported;
    inputs.allAdaptersAmdSingle = AllAdaptersAmdSingle(*platform);
    ComputePresenterMasks(inputs, m_present.masks);
    for (uint32 type = 0; type < QueueTypeCount; ++type)
    {
        m_queueProperties.perQueue[type].supportedDirectPresentModes =
            DirectPresentModes(m_present.masks, type, inputs.forcePresentViaCpuBlt,
                               m_deviceControls.finalized.risState);
    }
    if (std::getenv("PAL_WDDM_TRACE") != nullptr)
    {
        std::fprintf(stderr, "[pal-wddm] present selection phys=%u adapterFlags=%x basic1=%u noDisplay=%u force=%u "
                     "oglRuntime=%u composition=%u redirectedBlt=%u amdSingle=%u ris=%u masks=%x,%x,%x,%x modes=%x,%x,%x,%x\n",
                     inputs.physicalAdapter, inputs.adapterFlags,
                     GetRecord32(m_adapter.physical.basicRecord.data(), 4), inputs.noDisplayAccess,
                     inputs.forcePresentViaCpuBlt, Settings().presentViaOglRuntime, inputs.compositionEnabled,
                     inputs.redirectedBltSupported, inputs.allAdaptersAmdSingle, m_deviceControls.finalized.risState,
                     m_present.masks[0], m_present.masks[1], m_present.masks[2], m_present.masks[3],
                     m_queueProperties.perQueue[0].supportedDirectPresentModes,
                     m_queueProperties.perQueue[1].supportedDirectPresentModes,
                     m_queueProperties.perQueue[2].supportedDirectPresentModes,
                     m_queueProperties.perQueue[3].supportedDirectPresentModes);
    }
}

// =====================================================================================================================
uint32 Device::AdapterPresentFlags() const
{
    if (!m_present.adapterFlagsQueried)
    {
        m_present.adapterFlags |= QueryAdapterPresentFlags(m_transport.AdapterHandle(), m_adapter.vendorId);
        m_present.adapterFlagsQueried = true;
    }
    return m_present.adapterFlags;
}

// =====================================================================================================================
// Original Device slot 63 (0x312f60): image size, memory object size and create info of an external shared image.
Result Device::GetExternalSharedImageSizes(const ExternalImageOpenInfo &openInfo, size_t *pImageSize,
                                           size_t *pGpuMemorySize, ImageCreateInfo *pImgCreateInfo) const
{
    return Wddm::GetExternalImageSizes(const_cast<Device *>(this), openInfo, pImageSize, pGpuMemorySize,
                                       pImgCreateInfo);
}

// =====================================================================================================================
// Determines the size in bytes of a Image object.
size_t Device::GetImageSize(const ImageCreateInfo &createInfo, Result *pResult) const
{
    const ImageInternalCreateInfo internal = {};
    Result result = Pal::Image::ValidateCreateInfo(this, createInfo, internal);
    if (pResult != nullptr)
    {
        *pResult = result;
    }
    return sizeof(Image) + GetGfxDevice()->GetImageSize(createInfo) +
           Pal::Image::GetTotalSubresourceSize(*this, createInfo);
}

// =====================================================================================================================
// Compares this device against another device to determine how compatible they are for multi-GPU operations.
// NOTE: Part of the IDevice public interface.
Result Device::GetMultiGpuCompatibility(const IDevice &otherDevice, GpuCompatibilityInfo *pInfo) const
{
    if (pInfo == nullptr) { return Result::ErrorInvalidPointer; }
    *pInfo = {};
    // Original 30fe80: only devices sharing the adapter-state object (Device+0xd9c0, here the Transport that owns
    // the KMT adapter and device) are compatible. That object holds the first 2400 bytes of the BASIC record at +8.
    const auto& other = static_cast<const Device&>(otherDevice);
    if (Settings().mgpuCompatibilityEnabled && (&other.m_transport == &m_transport))
    {
        const uint8* basic = m_adapter.physical.basicRecord.data();
        constexpr uint32 PhysicalIndex = 0; // Device+0xd9c8 of both devices
        uint32 flags = 0x30; // sharedMemory, sharedSync
        if (Settings().peerMemoryEnabled && ((basic[2160 + PhysicalIndex] & (1u << PhysicalIndex)) != 0))
        { flags |= 0x4; } // peerTransferWrite: adapter+0x878 mask
        if (Settings().peerMemoryEnabled && ((basic[2228 + PhysicalIndex] & (1u << PhysicalIndex)) != 0))
        { flags |= 0x8; } // peerTransferRead: adapter+0x8bc mask
        // adapter+0x99c bit0: KMTQAITYPE_ADAPTERTYPE HybridDiscrete (3001b0).
        bool hybridDiscrete = false;
        m_transport.QueryHybridDiscrete(&hybridDiscrete);
        if ((ChipProperties().gfxTriple.major == other.ChipProperties().gfxTriple.major) &&
            (ChipProperties().gfxTriple.minor == other.ChipProperties().gfxTriple.minor) && !hybridDiscrete)
        {
            flags |= 0x2; // iqMatch
            if (ChipProperties().deviceId == other.ChipProperties().deviceId) { flags |= 0x1; } // gpuFeatures
        }
        // Device+0x81e4 bits 0..3 = BASIC DWORD 2140 bits 12..15 (EarlyInit 30abd3..30ac93), required on both.
        const uint32 screens = (GetRecord32(basic, 2140) >> 12) & 0xf;
        flags |= (screens & (GetRecord32(other.m_adapter.physical.basicRecord.data(), 2140) >> 12)) << 6;
        pInfo->flags.u32All = flags;
    }
    return Result::Success;
}

// =====================================================================================================================
void Device::GetPresentableImageSizes(const PresentableImageCreateInfo &createInfo, size_t *pImageSize,
                                      size_t *pGpuMemorySize, Result *pResult) const
{
    SwapChain::GetPresentableImageSizes(const_cast<Device *>(this), createInfo, pImageSize, pGpuMemorySize, pResult);
}

// =====================================================================================================================
// Primary surface capabilities of a display mode, as reported by the KMD.
Result Device::GetPrimaryInfo(const GetPrimaryInfoInput &primaryInfoInput,
                              GetPrimaryInfoOutput *pPrimaryInfoOutput) const
{
    // Native Device slot 20 (314220 -> 313fb0).
    return Wddm::GetPrimaryInfo(KmdEscapeRoute(), IsSpoofed(), primaryInfoInput,
                                pPrimaryInfoOutput);
}

// =====================================================================================================================
// Stereo display modes reported by the KMD.
Result Device::GetStereoDisplayModes(uint32 *pStereoModeCount, StereoDisplayModeOutput *pStereoModeList) const
{
    // Native Device slot 21 (314650); Device+9cd4 is the cached BASIC record DWORD 2900.
    return Wddm::GetStereoDisplayModes(KmdEscapeRoute(), GetPlatform(),
                                       StereoDisplayModesSupported(m_adapter.physical), pStereoModeCount,
                                       pStereoModeList);
}

// =====================================================================================================================
// Active workstation 10-bit / packed-pixel display mode.
Result Device::GetActive10BitPackedPixelMode(Active10BitPackedPixelModeOutput *pMode) const
{
    // Native Device slot 23 (314980).
    return Wddm::GetActive10BitPackedPixelMode(KmdEscapeRoute(), pMode);
}

// =====================================================================================================================
// Inform KMD that the present blt dst surface must be reinterpreted as 10-bits per channel.
Result Device::RequestKmdReinterpretAs10Bit(const IGpuMemory *pGpuMemory) const
{
    // Native Device slot 24 (314b00). Changes KMD state for the given allocation.
    return Wddm::RequestKmdReinterpretAs10Bit(KmdEscapeRoute(), pGpuMemory);
}

// =====================================================================================================================
// Get swap chain information for swap chain creation.
Result Device::GetSwapChainInfo(OsDisplayHandle hDisplay, OsWindowHandle hWindow, WsiPlatform wsiPlatform,
                                SwapChainProperties *pSwapChainProperties)
{
    return SwapChain::QueryProperties(hWindow, wsiPlatform, pSwapChainProperties);
}

// =====================================================================================================================
// Return placement requirements for the supported windowed swap chain.
size_t Device::GetSwapChainSize(const SwapChainCreateInfo &createInfo, Result *pResult) const
{
    return SwapChain::GetSize(*this, createInfo, pResult);
}

// =====================================================================================================================
// Helper function to Call KMD to get XDMA cache buffer info.
Result Device::GetXdmaInfo(uint32 vidPnSrcId, const IGpuMemory &gpuMemory, GetXdmaInfoOutput *pGetXdmaInfoOutput) const
{
    return Result::Unsupported;
}

// =====================================================================================================================
size_t Device::GpuMemoryObjectSize() const
{
    return sizeof(GpuMemory);
}

// =====================================================================================================================
// Helper method which initializes the GPU memory properties.
Result Device::InitMemoryProperties()
{
    m_memoryProperties.vaStart = 65536;
    m_memoryProperties.vaEnd = m_adapter.physical.gpuVaEnd;
    m_memoryProperties.vaInitialEnd = m_memoryProperties.vaEnd;
    m_memoryProperties.vaUsableEnd = m_memoryProperties.vaEnd;
    m_memoryProperties.realMemAllocGranularity = 65536;
    m_memoryProperties.virtualMemAllocGranularity = 65536;
    m_memoryProperties.virtualMemPageSize = 4096;
    m_memoryProperties.fragmentSize = 65536;
    m_memoryProperties.pdeSize = m_memoryProperties.pteSize = 8;
    m_memoryProperties.spaceMappedPerPde = m_memoryProperties.vaEnd + 1;
    m_memoryProperties.numPtbsPerGroup = 1;
    m_memoryProperties.flags.pinningSupport = 1;
    m_memoryProperties.flags.virtualRemappingSupport = 1;
    m_memoryProperties.flags.supportPageFaultInfo = 1;
    m_memoryProperties.localMemoryType = LocalMemoryType::Unknown;
    m_memoryProperties.memOpsPerClock = MemoryOpsPerClock(m_memoryProperties.localMemoryType);
    m_heapProperties[GpuHeapLocal].logicalSize = m_adapter.physical.visibleVramSize;
    m_heapProperties[GpuHeapLocal].physicalSize = m_adapter.physical.visibleVramSize;
    m_heapProperties[GpuHeapInvisible].logicalSize = m_adapter.physical.invisibleVramSize;
    m_heapProperties[GpuHeapInvisible].physicalSize = m_adapter.physical.invisibleVramSize;
    m_memoryProperties.barSize = m_adapter.physical.visibleVramSize;
    m_memoryProperties.vramBusBitWidth = m_adapter.physical.vramBitWidth;
    m_memoryProperties.nonLocalHeapSize = m_adapter.sharedSystemMemory;
    return FixupUsableGpuVirtualAddressRange(m_force32BitVaSpace ? VaRangeLimitTo32bits : m_adapter.virtualAddressBits);
}

// =====================================================================================================================
// General external-memory creation requires additional private resource metadata.
Result Device::OpenExternalSharedGpuMemory(const ExternalGpuMemoryOpenInfo &openInfo, void *pPlacementAddr,
                                           GpuMemoryCreateInfo *pMemCreateInfo, IGpuMemory **ppGpuMemory)
{
    return GpuMemory::OpenExternal(this, openInfo, pPlacementAddr, pMemCreateInfo, ppGpuMemory);
}

Result Device::OpenSharedGpuMemory(const GpuMemoryOpenInfo& openInfo, void* placement, IGpuMemory** memory)
{
    if (memory != nullptr) { *memory = nullptr; }
    if ((placement == nullptr) || (memory == nullptr)) { return Result::ErrorInvalidPointer; }
    const Result validation = Pal::GpuMemory::ValidateOpenInfo(this, openInfo);
    if (validation != Result::Success) { return validation; }
    GpuCompatibilityInfo compatibility = {};
    const auto* original = static_cast<const Pal::GpuMemory*>(openInfo.pSharedMem);
    const Result query = GetMultiGpuCompatibility(*original->GetDevice(), &compatibility);
    if (query != Result::Success) { return query; }
    if (!compatibility.flags.sharedMemory) { return Result::ErrorIncompatibleDevice; }
    return Pal::Device::OpenSharedGpuMemory(openInfo, placement, memory);
}

// =====================================================================================================================
// Original Device slot 64 (0x313120): opens a shared image from anyone but a PAL device of the same LDA chain.
Result Device::OpenExternalSharedImage(const ExternalImageOpenInfo &openInfo, void *pImagePlacementAddr,
                                       void *pGpuMemoryPlacementAddr, GpuMemoryCreateInfo *pMemCreateInfo,
                                       IImage **ppImage, IGpuMemory **ppGpuMemory)
{
    return Wddm::OpenExternalImage(this, openInfo, pImagePlacementAddr, pGpuMemoryPlacementAddr, pMemCreateInfo,
                                   ppImage, ppGpuMemory);
}

// =====================================================================================================================
// Gets current fullscreen frame metadata control flags from the shared memory buffer.
Result Device::PollFullScreenFrameMetadataControl(uint32 vidPnSrcId,
                                                  PerSourceFrameMetadataControl *pFrameMetadataControl) const
{
    // Native Device slot 27 (314bd0).
    return Wddm::PollFullScreenFrameMetadataControl(m_kmdShared, vidPnSrcId, pFrameMetadataControl);
}

// =====================================================================================================================
// Requery the versioned KMD records and apply the native checksum/fallback policy.
Result Device::QueryWorkStationCaps(WorkStationCaps *pCaps) const
{
    return QueryWorkstationCapabilities(m_transport, pCaps);
}
Result Device::GetWsStereoMode(WorkstationStereoMode* mode) const
{
    return QueryWorkstationStereoMode(m_transport, mode);
}

// =====================================================================================================================
// The platform forwards its opaque, client-specific profile query to Device0.
Result Device::QueryRawApplicationProfile(const wchar_t *pFilename,
                                          const wchar_t *pPathname, // This parameter is optional and may be null
                                          ApplicationProfileClient client, const char **pOut)
{
    return m_applicationProfile.Query(m_transport, pFilename, pPathname, client, pOut);
}

// =====================================================================================================================
// Native Device slot 168 (310270). The capability is the raw GPU record DWORD
// at byte 16180 that the native initializer copies into Device+0xd314.
uint32 Device::SppCapability() const
{
    uint32 value = 0;
    std::memcpy(&value, m_adapter.physical.gpuRecord.data() + 16180, sizeof(value));
    return value;
}

SppProfileCalls Device::SppCalls() const
{
    SppProfileCalls calls = {};
    calls.context = const_cast<Device *>(this);
    calls.query = [](void *context, const wchar_t *file, const wchar_t *path, ApplicationProfileClient client)
    {
        auto *device = static_cast<Device *>(context);
        const char *blob = nullptr;
        const Result result = device->m_applicationProfile.Query(device->m_transport, file, path, client, &blob);
        return SppQueryReply{result, blob, device->m_applicationProfile.Size()};
    };
    calls.allocate = [](void *context, size_t bytes, size_t alignment, uint32 type) -> void *
    {
        return PAL_MALLOC_ALIGNED(bytes, alignment, static_cast<Device *>(context)->GetPlatform(),
                                  static_cast<Util::SystemAllocType>(type));
    };
    calls.free = [](void *context, void *memory) { PAL_FREE(memory, static_cast<Device *>(context)->GetPlatform()); };
    calls.copy = [](void *, void *destination, const void *source, size_t bytes)
    { return std::memcpy(destination, source, bytes); };
    calls.zero = [](void *, void *destination, size_t bytes) { std::memset(destination, 0, bytes); };
    calls.write = [](void *, void *record, size_t offset, uint32 value)
    { std::memcpy(static_cast<uint8 *>(record) + offset, &value, sizeof(value)); };
    calls.escape = [](void *context, void *record, size_t bytes)
    {
        auto *device = static_cast<Device *>(context);
        return SharedEscape(DeviceEscapeRoute(device->m_transport, device->GetPlatform()), record, bytes);
    };
    return calls;
}

Result Device::EnableSppProfile(const wchar_t *pFilename, const wchar_t *pPathname)
{
    return Wddm::EnableSppProfile(SppCapability(), &m_spp, SppCalls(), pFilename, pPathname);
}

// Native Device slot 169 (3103c0): both arguments are ignored by the original.
Result Device::SelectSppTable(uint32 pixelCount, uint32 msaaRate) const
{
    return Wddm::SelectSppTable(SppCapability(), m_spp, SppCalls(), pixelCount, msaaRate);
}

// =====================================================================================================================
// Native Device slot 138 (313be0). The original writes the count only after a
// successful escape and would fault on a null count there; reject it first.
Result Device::QueryDisplayConnectors(uint32 *pConnectorCount, DisplayConnectorProperties *pConnectors)
{
    if (pConnectorCount == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }
    return Wddm::QueryDisplayConnectors(m_flgl.Route(), pConnectorCount, pConnectors);
}

// =====================================================================================================================
size_t Device::QueueObjectSize(const QueueCreateInfo &createInfo) const
{
    return sizeof(Queue);
}

// =====================================================================================================================
// Read the configured PAL settings file.

bool Device::ReadSetting(const char *pSettingName, Util::ValueType valueType, void *pValue,
                         InternalSettingScope settingType, size_t bufferSz) const
{
    return m_settingFileMgr.GetValue(pSettingName, valueType, pValue, bufferSz);
}

// =====================================================================================================================
// Original Device slot 130 (313870). Returns the removed screen's Disable() result.
Result Device::RemoveEmulatedPrivateScreen(uint32 targetId)
{
    return RemoveEmulatedScreen(GetPlatform(),
                                {m_pEmulatedPrivateScreens, &m_emulatedPrivateScreens, &m_emulatedTargetId},
                                m_finalizeInfo.privateScreenNotifyInfo, targetId);
}
// =====================================================================================================================
// Multi-GPU compositing is not implemented.
Result Device::SetMgpuMode(const SetMgpuModeInput &setMgpuModeInput) const
{
    return Result::Unsupported;
}

// =====================================================================================================================
// Adjust the KMT device-wide present limit, independent of DXGI swap-chain latency.

Result Device::SetMaxQueuedFrames(uint32 maxFrames)
{
    m_maxQueuedFrames = maxFrames;
    return ApplyQueuedPresentLimit(m_transport, m_maxQueuedFrames);
}

Result Device::CreateDmaUploadRing()
{
    Util::MutexAuto lock(&m_dmaUploadRingLock);
    Result result = Result::Success;
    if (m_pDmaUploadRing == nullptr)
    {
        m_pDmaUploadRing = PAL_NEW(DmaUploadRing, GetPlatform(), Util::AllocInternal)(this);
        if (m_pDmaUploadRing == nullptr) { return Result::ErrorOutOfMemory; }
        result = m_pDmaUploadRing->Init();
        if (result != Result::Success) { PAL_SAFE_DELETE(m_pDmaUploadRing, m_pPlatform); }
    }
    return result;
}

// =====================================================================================================================
// Vendor power-policy controls are not implemented.
Result Device::SetPowerProfile(PowerProfile profile, CustomPowerProfile *pInfo)
{
    return ApplyPowerProfile(m_transport, profile, pInfo);
}

// =====================================================================================================================
// Vendor power-policy controls are not implemented.
Result Device::SetMlPowerOptimization(bool enableOptimization) const
{
    return ApplyMlPowerOptimization(m_transport, enableOptimization);
}

// Original Device slot 37 (313450): the WSI platform is not read.
uint32 Device::SwapChainModes(WsiPlatform platform, PresentMode mode) const
{
    return SupportedSwapChainModes(m_queueProperties.perQueue[QueueTypeUniversal].supportedDirectPresentModes, mode);
}

} // namespace Wddm
} // namespace Pal

#endif
