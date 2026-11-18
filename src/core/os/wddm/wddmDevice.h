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

#pragma once

#if PAL_WDDM_BUILD

#include "core/device.h"
#include "core/os/wddm/wddmTransport.h"
#include "core/os/wddm/wddmApplicationProfile.h"
#include "core/os/wddm/wddmFlgl.h"
#include "core/os/wddm/wddmSppProfile.h"
#include "core/os/wddm/wddmDisplay.h"
#include "core/os/wddm/wddmPrivateScreen.h"
#include "core/os/wddm/wddmPresent.h"
#include "core/os/wddm/wddmVirtualDisplay.h"
#include "core/os/wddm/wddmDeviceControls.h"
#include "palSettingsFileMgr.h"
#include "palPlatform.h"
#include "palSwapChain.h"
#include "core/os/wddm/wddmResult.h"
#include <array>
#include <atomic>
#include <map>

namespace Pal
{
class SvmMgr;
namespace Wddm
{

class Platform;

// =====================================================================================================================
// Native AMD WDDM device. Hardware facts come from the KMD property records.
class alignas(32) Device final : public Pal::Device
{
  public:
    ~Device() override;
    static Result Create(Platform *pPlatform, const char *pSettingsPath, Device **ppDeviceOut,
                         const AdapterInfo &adapter);

    virtual Result AddEmulatedPrivateScreen(const PrivateScreenCreateInfo &createInfo, uint32 *pTargetId) override;

    virtual Result ReserveGpuVirtualAddress(VaPartition vaPartition, gpusize baseVirtAddr, gpusize size, bool isVirtual,
                                            VirtualGpuMemAccessMode virtualAccessMode, gpusize *pGpuVirtAddr) override
    {
        return ReserveVa(vaPartition, baseVirtAddr, size, isVirtual, virtualAccessMode, pGpuVirtAddr);
    }
    virtual Result FreeGpuVirtualAddress(gpusize vaStartAddress, gpusize vaSize) override;
    Result ReserveGpuVirtualAddressSvm(gpusize base, gpusize size) override;
    Result FreeGpuVirtualAddressSvm(gpusize base, gpusize size) override;
    Pal::SvmMgr* GetSvmMgr() const { return m_svmMgr; }
    void UpdateAttachedScreenCount(uint32 count) { m_attachedScreenCount = count; }

    Result AssignVirtualAddress(const Pal::GpuMemory &gpuMemory, gpusize *pGpuVirtAddr, VaPartition vaPartition,
                                gpusize base = 0, VirtualGpuMemAccessMode mode = VirtualGpuMemAccessMode::Undefined);
    Result CheckExecutionState(PageFaultStatus *status) override;

    // NOTE: Part of the public IDevice interface.
    virtual Result GetCalibratedTimestamps(CalibratedTimestamps *pCalibratedTimestamps) const override;
    Result GetProperties(DeviceProperties* properties) const override;

    virtual Result Cleanup() override;

    virtual Result CreateImage(const ImageCreateInfo &createInfo, void *pPlacementAddr, IImage **ppImage) override;

    virtual Result CreateInternalImage(const ImageCreateInfo &createInfo,
                                       const ImageInternalCreateInfo &internalCreateInfo, void *pPlacementAddr,
                                       Pal::Image **ppImage) override;

    virtual Result CreatePresentableImage(const PresentableImageCreateInfo &createInfo, void *pImagePlacementAddr,
                                          void *pGpuMemoryPlacementAddr, IImage **ppImage,
                                          IGpuMemory **ppGpuMemory) override;

    virtual size_t GetFenceSize(Result *pResult) const override;

    // NOTE: Part of the public IDevice interface.
    virtual Result CreateFence(const FenceCreateInfo &createInfo, void *pPlacementAddr,
                               IFence **ppFence) const override;

    // NOTE: Part of the public IDevice interface.
    virtual Result OpenFence(const FenceOpenInfo &openInfo, void *pPlacementAddr, IFence **ppFence) const override;

    // NOTE: Part of the public IDevice interface. Native slot 111 (317700).
    virtual Result OpenExternalHandleByName(const ExternalHandleNameInfo &info, OsExternalHandle *pHandle) override;

    // NOTE: Part of the public IDevice interface.
    virtual Result WaitForSemaphores(uint32 semaphoreCount, const IQueueSemaphore *const *ppSemaphores,
                                     const uint64 *pValues, uint32 flags,
                                     std::chrono::nanoseconds timeout) const override;

    bool IsNativeFenceSupported() const override
    {
        return false;
    }

    virtual Result CreateSwapChain(const SwapChainCreateInfo &createInfo, void *pPlacementAddr,
                                   ISwapChain **ppSwapChain) override;

    // Original Device slot 36 is the shared `xor eax,eax; ret` (123270): Success for every platform and display.
    virtual Result DeterminePresentationSupported(OsDisplayHandle hDisplay, WsiPlatform wsiPlatform,
                                                  int64 visualId) override
    {
        return Result::Success;
    }

    virtual uint32 GetSupportedSwapChainModes(WsiPlatform wsiPlatform, PresentMode mode) const override
    {
        return SwapChainModes(wsiPlatform, mode);
    }

    virtual Result DetermineExternalSharedResourceType(const ExternalResourceOpenInfo &openInfo,
                                                       bool *pIsImage) const override;

    virtual Result EnumPrivateScreensInfo(uint32 *pNumScreens) override;

    virtual Result Finalize(const DeviceFinalizeInfo &finalizeInfo) override;

    virtual Result GetExternalSharedImageSizes(const ExternalImageOpenInfo &openInfo, size_t *pImageSize,
                                               size_t *pGpuMemorySize, ImageCreateInfo *pImgCreateInfo) const override;

    // Native Device slot 28 (314d50).
    virtual Result GetFlipStatus(uint32 vidPnSrcId, FlipStatusFlags *pFlipFlags, bool *pIsFlipOwner) const override
    {
        return Wddm::GetFlipStatus(m_kmdShared, vidPnSrcId, pFlipFlags, pIsFlipOwner);
    }

    // NOTE: Part of the public IDevice interface.
    virtual size_t GetImageSize(const ImageCreateInfo &createInfo, Result *pResult) const override;

    virtual Result GetMultiGpuCompatibility(const IDevice &otherDevice, GpuCompatibilityInfo *pInfo) const override;

    virtual void GetPresentableImageSizes(const PresentableImageCreateInfo &createInfo, size_t *pImageSize,
                                          size_t *pGpuMemorySize, Result *pResult) const override;

    virtual Result GetPrimaryInfo(const GetPrimaryInfoInput &primaryInfoInput,
                                  GetPrimaryInfoOutput *pPrimaryInfoOutput) const override;

    virtual Result GetStereoDisplayModes(uint32 *pStereoModeCount,
                                         StereoDisplayModeOutput *pStereoModeList) const override;

    virtual Result GetWsStereoMode(WorkstationStereoMode *pWsStereoMode) const override;

    virtual Result GetActive10BitPackedPixelMode(Active10BitPackedPixelModeOutput *pMode) const override;

    virtual Result RequestKmdReinterpretAs10Bit(const IGpuMemory *pGpuMemory) const override;

    virtual size_t GetSwapChainSize(const SwapChainCreateInfo &createInfo, Result *pResult) const override;

    virtual Result GetXdmaInfo(uint32 vidPnSrcId, const IGpuMemory &gpuMemory,
                               GetXdmaInfoOutput *pGetXdmaInfoOutput) const override;

    virtual size_t GpuMemoryObjectSize() const override;

    virtual bool IsMasterGpu() const override
    {
        return true;
    } // only one gpu, it's going to be the master

    virtual bool IsNull() const override
    {
        return false;
    }

    virtual Result OpenExternalSharedGpuMemory(const ExternalGpuMemoryOpenInfo &openInfo, void *pPlacementAddr,
                                               GpuMemoryCreateInfo *pMemCreateInfo, IGpuMemory **ppGpuMemory) override;
    Result OpenSharedGpuMemory(const GpuMemoryOpenInfo& openInfo, void* placement, IGpuMemory** memory) override;

    virtual Result OpenExternalSharedImage(const ExternalImageOpenInfo &openInfo, void *pImagePlacementAddr,
                                           void *pGpuMemoryPlacementAddr, GpuMemoryCreateInfo *pMemCreateInfo,
                                           IImage **ppImage, IGpuMemory **ppGpuMemory) override;

    virtual Result PollFullScreenFrameMetadataControl(
        uint32 vidPnSrcId, PerSourceFrameMetadataControl *pFrameMetadataControl) const override;

    virtual Result QueryDisplayConnectors(uint32 *pConnectorCount, DisplayConnectorProperties *pConnectors) override;

    virtual Result QueryWorkStationCaps(WorkStationCaps *pCaps) const override;

    virtual Result RemoveEmulatedPrivateScreen(uint32 targetId) override;

    virtual Result SetClockMode(const SetClockModeInput &setClockModeInput,
                                SetClockModeOutput *pSetClockModeOutput) override;

    Result OsSetStaticVmidMode(bool enable) override;

    virtual Result SetMaxQueuedFrames(uint32 maxFrames) override;

    virtual Result SetMgpuMode(const SetMgpuModeInput &setMgpuModeInput) const override;

    virtual Result SetPowerProfile(PowerProfile profile, CustomPowerProfile *pInfo) override;

    virtual Result SetMlPowerOptimization(bool enableOptimization) const override;

    // Native Device slots 139..146 (38a910..38b020); see wddmFlgl.cpp.
    virtual Result FlglQueryState(Pal::FlglState *pState) override
    {
        return m_flgl.QueryState(pState);
    }

    virtual Result FlglSetSyncConfiguration(const GlSyncConfig &glSyncConfig) override
    {
        return m_flgl.SetSyncConfiguration(glSyncConfig);
    }

    virtual Result FlglGetSyncConfiguration(GlSyncConfig *pGlSyncConfig) const override
    {
        return m_flgl.GetSyncConfiguration(pGlSyncConfig);
    }

    virtual Result FlglSetFrameLock(bool enable) override
    {
        return m_flgl.SetFrameLock(enable);
    }

    virtual Result FlglSetGenLock(bool enable) override
    {
        return m_flgl.SetGenLock(enable);
    }

    virtual Result FlglResetFrameCounter() const override
    {
        return m_flgl.ResetFrameCounter();
    }

    virtual Result FlglGetFrameCounter(uint64 *pValue, bool *pReset) const override
    {
        return m_flgl.GetFrameCounter(pValue, pReset);
    }

    virtual Result FlglGetFrameCounterResetStatus(bool *pReset) const override
    {
        return m_flgl.GetFrameCounterResetStatus(pReset);
    }

    virtual Result DidRsFeatureSettingsChange(uint32 rsFeatures, uint32 *pRsFeaturesChanged) override;
    virtual Result GetRsFeatureGlobalSettings(RsFeatureType rsFeature, RsFeatureInfo *pRsFeatureInfo) override;
    virtual Result UpdateChillStatus(uint64 lastChillActiveTimeStampUs) override;
    virtual Result InitBusAddressableGpuMemory(IQueue *pQueue, uint32 gpuMemCount,
                                               IGpuMemory *const *ppGpuMemList) override;
    virtual Result RegisterHipRuntimeState(const HipRuntimeSetup &runtimeState) const override;
    virtual Result SetHipTrapHandler(const IGpuMemory *pTrapHandlerCode, gpusize codeOffset,
                                     const IGpuMemory *pTrapHandlerMemory, gpusize memoryOffset) const override;
    // Native Device slot 180 (30e330).
    virtual void OverrideDefaultSettings(PalSettings *pSettings) const override;

    // Native Device slots 151..153 (311330, 311450, 311510).
    virtual Result CreateVirtualDisplay(const VirtualDisplayInfo &virtualDisplayInfo, uint32 *pScreenTargetId) override
    {
        return CreateKmdVirtualDisplay(KmdEscapeRoute(), m_virtualDisplayCaps,
                                       virtualDisplayInfo, pScreenTargetId);
    }

    virtual Result DestroyVirtualDisplay(uint32 screenTargetId) override
    {
        return DestroyKmdVirtualDisplay(KmdEscapeRoute(), screenTargetId);
    }

    virtual Result GetVirtualDisplayProperties(uint32 screenTargetId, VirtualDisplayProperties *pProperties) override
    {
        return QueryKmdVirtualDisplayProperties(KmdEscapeRoute(), screenTargetId,
                                                pProperties);
    }

    virtual Result QueryRadeonSoftwareVersion(char *pBuffer, size_t bufferLength) const override;

    virtual Result QueryReleaseVersion(char *pBuffer, size_t bufferLength) const override;

    Result CreateDmaUploadRing() override;

    // Native Device+0xd314 SPP gate: raw GPU record DWORD at byte 16180.
    uint32 SppCapability() const;

    // Route of the shared private escape (native 30f860). Device+0x8bd0, which refuses every escape before the
    // DDI, is PalPublicSettings::disableEscapeCall (public settings at Device+0x8b18, GetPublicSettings 2f8110).
    EscapeRoute KmdEscapeRoute() const
    {
        EscapeRoute route = DeviceEscapeRoute(m_transport, GetPlatform());
        route.suppressed = GetPublicSettings()->disableEscapeCall;
        return route;
    }

    // KMD/UMD shared buffers (Device+0x9588.., +0xd9d8); CmdBufInfo records carry flipOwner.
    const KmdSharedBuffers &KmdShared() const { return m_kmdShared; }
    Transport &OsTransport()
    {
        return m_transport;
    }
    const AdapterInfo &Adapter() const
    {
        return m_adapter;
    }
    // Native Device+0x1054 FLGL cache flags bit1, read by the fullscreen notification 3687a0.
    bool FlglFrameLockEnabled() const
    {
        return m_flgl.CachedState().frameLockEnabled != 0;
    }
    // Native Device+0xd9d0: SetMaxQueuedFrames 310500 stores it, 310510 re-applies it.
    uint32 MaxQueuedFrames() const
    {
        return m_maxQueuedFrames;
    }
    // Native Device+0x95d0[queueType] presenter mask (FinalizeQueueProperties 30d1b0), read by queue Init 4d84b0.
    uint32 PresenterMask(QueueType queueType) const
    {
        return (queueType < PresenterMaskCount) ? m_present.masks[queueType] : 0;
    }
    // Native Device+0x95f0 (30bd30): Platform slot 38 on this adapter.
    bool RedirectedBltSupported() const
    {
        return m_present.redirectedBltSupported;
    }
    // Native adapter+0x99c: bits 0..1 queried once (3001b0), bit 3 set by screen enumeration (301350).
    uint32 AdapterPresentFlags() const;
    void MarkActiveDisplay()
    {
        m_present.adapterFlags |= AdapterActiveDisplay;
    }

  protected:
    Device(Platform *pPlatform, const char *pSettingsPath, const GpuInfo &gpuInfo, const HwIpDeviceSizes &hwDeviceSizes,
           const AdapterInfo &adapter);

    virtual Pal::GpuMemory *ConstructGpuMemoryObject(void *pPlacementAddr) override;

    virtual Pal::Queue *ConstructQueueObject(const QueueCreateInfo &createInfo, void *pPlacementAddr) override;

    virtual void FinalizeQueueProperties() override;

    virtual size_t QueueObjectSize(const QueueCreateInfo &createInfo) const override;

    virtual bool ValidatePipelineUploadHeap(const GpuHeap &preferredHeap) const override
    {
        return (preferredHeap == GpuHeapGartCacheable) || (preferredHeap == GpuHeapGartUswc);
    }

    virtual Pal::Queue *ConstructMultiQueueObject(uint32 queueCount, const QueueCreateInfo *pCreateInfo,
                                                  void *pPlacementAddr) override;

    virtual size_t MultiQueueObjectSize(uint32 queueCount, const QueueCreateInfo *pCreateInfo) const override;

  private:
    virtual Result EarlyInit(const HwIpLevels &ipLevels) override;
    // Native Device slot 190 (30bac0).
    virtual void OsFinalizeSettings() override;

    virtual Result GetSwapChainInfo(OsDisplayHandle hDisplay, OsWindowHandle hWindow, Pal::WsiPlatform wsiPlatform,
                                    SwapChainProperties *pSwapChainProperties) override;

    uint32 SwapChainModes(WsiPlatform platform, PresentMode mode) const;
    Result InitMemoryProperties();
    Result ProbeGpuVaRange(gpusize base, gpusize size, VaPartition partition) const override;
    Util::Mutex m_vaLock;
    struct AlignedReservation
    {
        gpusize base;
        gpusize size;
        gpusize mappedSize;
    };
    std::map<gpusize, AlignedReservation> m_alignedReservations;
    Result ReserveVa(VaPartition partition, gpusize base, gpusize size, bool isVirtual, VirtualGpuMemAccessMode mode,
                     gpusize *address, gpusize alignment = 65536);

    virtual Result QueryRawApplicationProfile(const wchar_t *pFilename, const wchar_t *pPathname,
                                              ApplicationProfileClient client, const char **pOut) override;

    virtual Result EnableSppProfile(const wchar_t *pFilename, const wchar_t *pPathname) override;

    virtual Result SelectSppTable(uint32 pixelCount, uint32 msaaRate) const override;
    SppProfileCalls SppCalls() const;

    virtual bool ReadSetting(const char *pSettingName, Util::ValueType valueType, void *pValue,
                             InternalSettingScope settingType, size_t bufferSz = 0) const override;

    void InitGfx9ChipProperties();
    Result InitSvmPool();
    Result CleanupSvmPool();

    Pal::SvmMgr* m_svmMgr = nullptr;
    gpusize m_svmPoolBase = 0;
    gpusize m_svmPoolBytes = 0;
    bool m_svmGpuReserved = false;
    bool m_svmCpuReserved = false;
    uint32 m_maxQueuedFrames = 0;
    // Native Platform+0x27d0 one-time flag of the gang notification escape (309f40), kept per Device.
    std::atomic<bool> m_gangSubmitNotified{false};
    KmdSharedBuffers m_kmdShared; // Native Device+9588..95cf and +d9d8, filled by Finalize.
    DeviceControlsState m_deviceControls; // Native Device+0x8be8/+0x8bf0/+0x950c results and the +0xdcd0 lock.
    PrivateScreenNotifier m_privateScreenNotifier; // Native Device+0x9560/+0x9570/+0x9578 hot-plug state.
    VirtualDisplayCapabilities m_virtualDisplayCaps = {}; // Native Device+0x8afc, set by EarlyInit.
    uint32 m_kmdFeatureFlags = 0; // Native Device+0xd9e0: Finalize query 0x03008705 reply +0xc8.
    mutable DevicePresentState m_present; // Native Device+0x95d0..0x95f0 and adapter+0x99c presenter selection.

    GpuInfo m_gpuInfo;
    AdapterInfo m_adapter;
    Transport m_transport;
    ApplicationProfileState m_applicationProfile;
    SppProfileStorage m_spp;  // Native Device copied SPP profile and selected payload.
    Flgl m_flgl;              // Native Device+0x1054 FLGL cache and escape route.
    Util::SettingsFileMgr<Platform> m_settingFileMgr; // File Mangager.
    const char *m_pSettingsPath;

    PAL_DISALLOW_DEFAULT_CTOR(Device);
    PAL_DISALLOW_COPY_AND_ASSIGN(Device);
};

} // namespace Wddm
} // namespace Pal

#endif
