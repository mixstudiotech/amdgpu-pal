// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Direct presentation of the original Windows PAL (amdvlk64.dll, image base 0x180000000): the per-queue-type
// presenter selection of Device::FinalizeQueueProperties (30d1b0), the queue presenters created by queue Init
// (4d84b0) and called by OsPresentDirect (HWS queue slot 37, 4d8f60), and GetSupportedSwapChainModes (313450).
// Notes: .build/pal-wddm-logic/present/analysis.txt.
#pragma once

#include "palQueue.h"
#include "palSwapChain.h"
#include "palFormat.h"

namespace Pal
{
class IQueueSemaphore;
class IImage;
class IGpuMemory;
class CmdBuffer;
struct PresentDirectInfo;
}

namespace Pal::Wddm
{
class Device;
class Queue;
class Platform;
struct AdapterInfo;

// =====================================================================================================================
// Presenter masks (native Device+0x95d0, one DWORD per queue type, written only by 30d1b0).
constexpr uint32 PresenterWindowed         = 0x1; // KMT windowed presenter (2d08d0)
constexpr uint32 PresenterWindowedPriorBlt = 0x2; // windowed prior-blit presents through the same presenter
constexpr uint32 PresenterFullscreen       = 0x4; // flip presenter (vtable 6576d50)
constexpr uint32 PresenterGdi              = 0x8; // GDI presenter (vtable 6576d70): CPU readback, not built here
constexpr uint32 PresenterMaskCount        = 8;   // 30d1b0 clears and fills eight queue types

// Native adapter-object flags (adapter+0x99c) that the selection reads.
constexpr uint32 AdapterHybridDiscrete = 0x1; // 3001b0: KMTQAITYPE_ADAPTERTYPE bit4 (Windows 8 and later)
constexpr uint32 AdapterAmdIntegrated  = 0x2; // 3001b0: vendor 0x1002/0x1022 and not hybrid discrete
constexpr uint32 AdapterActiveDisplay  = 0x8; // 301350: an active, available CCD path uses this source adapter

struct PresenterSelectionInputs
{
    uint32 physicalAdapter;        // Device+0xd9c8
    uint32 adapterFlags;           // adapter+0x99c (bits above)
    bool   noDisplayAccess;        // Platform+0x27bc: QueryDisplayConfig returned ERROR_ACCESS_DENIED
    bool   forcePresentViaCpuBlt;  // PalSettings+0xa62
    bool   compositionEnabled;     // Platform slot 35 (302b50)
    bool   redirectedBltSupported; // Platform slot 38 (302cb0) on the Device's adapter
    bool   allAdaptersAmdSingle;   // Platform helper 2ccd90
};

// 30d1b0 (30d1eb..30d359, 30d5ba..30d5fe).
void   ComputePresenterMasks(const PresenterSelectionInputs& inputs, uint32 masks[PresenterMaskCount]);
// 30d359..30d4c4: PresentModeSupport flags of one queue type. risState is PalPublicSettings+0xd0
// (Device+0x8be8); when it is 1 only queue types 0 and 1 report modes.
uint32 DirectPresentModes(const uint32 masks[PresenterMaskCount], uint32 queueType, bool forcePresentViaCpuBlt,
                          uint32 risState);
// Device slot 37, 313450. The WSI platform is not read.
uint32 SupportedSwapChainModes(uint32 universalDirectPresentModes, PresentMode mode);

// Screen slot 13 (36adc0): HdrDisplayMode from the cached colour state (Screen+0x150) and the channel format.
uint32 FormatHdrMode(uint32 colorState, ChNumFormat format);

// Platform slot 35 (302b50): Windows 8 or later, else DwmIsCompositionEnabled.
bool   IsCompositionEnabled();
// Platform slot 38 (302cb0): KMTQAITYPE_DRIVERVERSION against the adapter record DWORD at byte 2224.
bool   QueryRedirectedBltSupport(uint32 kmtAdapter, uint32 recordVersion);
bool   RedirectedBltFromVersions(uint32 recordVersion, uint32 driverVersion); // 302d04..302d30
// 3001b0 (3001c4..300297): adapter-object flag bits 0 and 1.
uint32 QueryAdapterPresentFlags(uint32 kmtAdapter, uint32 vendorId);
uint32 AdapterPresentFlagsFrom(bool adapterTypeKnown, uint32 adapterType, uint32 vendorId);
// 4d8f60: 1 selects the fullscreen presenter (+0x1d8), 0 the windowed one (+0x1d0).
uint32 PresenterSlotFor(PresentMode mode, bool forcePresentViaCpuBlt);
// 2ccd90: every adapter object of the Platform reports basic-record DWORD 1 == 1 and AMD-integrated.
bool   AllAdaptersAmdSingle(const Platform& platform);

// Device state behind the selection: native Device+0x95d0..0x95ef (30d1b0), Device+0x95f0 (30bd30) and the
// adapter-object flags adapter+0x99c (3001b0 bits 0..1, 301350 bit 3).
struct DevicePresentState
{
    uint32 masks[PresenterMaskCount] = {};
    bool   redirectedBltSupported = false;
    bool   adapterFlagsQueried = false;
    uint32 adapterFlags = 0;
};

// =====================================================================================================================
// Queue presenters (native objects at HWS queue +0x1d0 / +0x1d8).
class Presenter
{
public:
    virtual ~Presenter() {}
    virtual Result Present(const PresentDirectInfo& info) = 0; // native vtable slot 1
    bool IsKmt() const { return m_kmt; }                        // native presenter+0x19
    // Not in the original: waits (gfxTimeout seconds, 0 = no limit, health checked every 100 ms) until every present
    // this presenter queued has completed. Queue::OsWaitIdle calls it after the original's wait (4d8d80), which covers
    // only the queue's own submissions and not a KMD present queued on the HW queue by opengl32 or 2d1fa0.
    virtual Result WaitForPresents(uint32 gfxTimeoutSeconds) { return Result::Success; }

protected:
    Presenter(Device* device, Queue* queue, bool kmt);
    Device* m_device;    // +0x8
    Queue*  m_queue;     // +0x10
    bool    m_hwQueue;   // +0x18: queue slot 42 (HW queue handle) != 0
    bool    m_kmt;       // +0x19
};

// Native 4d84b0 for one queue. Windowed: KMT presenter when the mask has bit 0 or 1; the GDI presenter (bit 3) and
// the fallback presenter 51aee0 are not built (CPU presentation). Fullscreen: flip presenter when the mask has bit 2.
Result CreateQueuePresenters(Device* device, Queue* queue, QueueType queueType, EngineType engineType,
                             Presenter** windowed, Presenter** fullscreen);
void   DestroyQueuePresenters(Device* device, Presenter** windowed, Presenter** fullscreen);
// Native 4d8f60.
Result PresentDirectWithPresenters(const Device& device, const PresentDirectInfo& info,
                                   Presenter* windowed, Presenter* fullscreen);

// Native presenter developer callback (2d1ab0 tail, 51aca0 tail, 388be0 tail): PresentConcluded with
// {mode, hWindow}. Flip = 1, Blit = 3.
void   ReportPresentConcluded(const Device& device, uint32 presentModeType, OsWindowHandle window);

// Native D3DKMT_PRESENT flag words of the presenters.
constexpr uint32 KmtPresentBltFlags         = 0x20c1;  // Blt | DstRectValid | SrcRectValid | Rotate (2d1ab0, 51b8f0)
constexpr uint32 KmtPresentRedirectedFlags  = 0x10081; // Blt | SrcRectValid | RedirectedBlt (2d1fa0)
constexpr uint32 KmtPresentFlipFlag         = 0x4;     // Flip (51aca0), FlipDoNotWait 0x10 from flags bit 0
// DWM redirection: Model D3DKMT_PM_REDIRECTED_BLT and the DwmpDxGetWindowSharedSurface flag bits.
constexpr uint32 KmtRedirectedBltModel      = 3;
constexpr uint32 DwmSharedSurfaceWait       = 0x1;     // presentInterval != 0
constexpr uint32 DwmSharedSurfaceKmtPresent = 0x10;    // 2d1fa0 (redirected KMT blt)

// Pure record builders (CPU fixtures compare these against the native stack images).
struct KmtPresentRecord
{
    uint32 hHwQueue;     // SUBMITPRESENTTOHWQUEUE.hHwQueues target (0 for D3DKMTPresent)
    alignas(8) uint8 present[0x5d8]; // D3DKMT_PRESENT
};
struct KmtPresentSource
{
    uint32 context;       // queue's KMT context handle (*(queue+0x228))
    uint32 hwQueue;       // queue slot 42
    OsWindowHandle window;
    uint32 source;        // first allocation of the image's root bound memory
    uint32 width;
    uint32 height;
};
void BuildBltPresent(const KmtPresentSource& source, KmtPresentRecord* record);           // 2d1ab0 / 51b8f0
void BuildRedirectedBltPresent(const KmtPresentSource& source, uint32 destination, uint64 dwmSurface,
                               uint64 updateId, KmtPresentRecord* record);                // 2d1fa0
void BuildFlipPresent(const KmtPresentSource& source, uint32 flags, uint32 presentInterval,
                      KmtPresentRecord* record);                                          // 51aca0
// Client-relative rectangle of 2d1dfe..2d1e29 (window rectangle origin).
void ClientRectInWindow(const void* windowInfo, int32 rect[4]);

} // namespace Pal::Wddm
