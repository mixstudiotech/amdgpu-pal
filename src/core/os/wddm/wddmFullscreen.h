// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "palQueue.h"
#include "palScreen.h"
#include <cstddef>

namespace Pal::Wddm
{
struct EscapeRoute;

// Native Platform+0x27d8..0x27ec: the two DirectDraw exclusive-mode mutexes
// shared with the DirectX runtime. Opened or created by the Platform OS setup
// 300fd7..301041 and closed by the Platform destructor 300d90.
class ExclusiveModeLock
{
public:
    ExclusiveModeLock() = default;
    ~ExclusiveModeLock();
    ExclusiveModeLock(const ExclusiveModeLock&) = delete;
    ExclusiveModeLock& operator=(const ExclusiveModeLock&) = delete;

    // 300c00 for "Local\__DDrawCheckExclMode__" then "Local\__DDrawExclMode__".
    // ErrorUnknown when either mutex can be neither opened nor created.
    Result Init() { return Init("Local\\__DDrawCheckExclMode__", "Local\\__DDrawExclMode__"); }
    Result Init(const char* checkName, const char* exclusiveName); // Fixtures use private names.
    // 369f59..369fe5: take the exclusive mode. False when another owner holds
    // the exclusive mutex.
    bool Acquire();
    // 302c50: drop one reference; the last one releases the exclusive mutex.
    void Release();
    uint32 ReferenceCount() const { return m_count; }
    bool ReleaseFailed() const { return m_releaseFailed; }

private:
    void*  m_check         = nullptr; // +27d8
    void*  m_exclusive     = nullptr; // +27e0
    uint32 m_count         = 0;       // +27e8
    bool   m_releaseFailed = false;   // +27ec: the last ReleaseMutex of +27e0 returned FALSE
};

// ---------------------------------------------------------------------------------------------------------------------
// Fullscreen ownership and gamma (native Screen slots 7, 8, 9 = 3691a0, 369290, 369330).

// The KMT/USER32 entry points the ownership methods call. Each request argument
// is the SDK structure named in the member; WindowsFullscreenCalls() binds the
// gdi32/user32 exports. The original reaches the same exports through its
// Platform function table (+0x2908, +0x2938, +0x2940, +0x2980, +0x2988, +0x28d0, +0x28d8).
struct FullscreenCalls
{
    int32 (*queryAdapterInfo)(void* request);             // D3DKMT_QUERYADAPTERINFO
    int32 (*getDeviceState)(void* request);               // D3DKMT_GETDEVICESTATE
    int32 (*setVidPnSourceOwner)(const void* request);    // D3DKMT_SETVIDPNSOURCEOWNER
    int32 (*setVidPnSourceOwner1)(const void* request);   // D3DKMT_SETVIDPNSOURCEOWNER1
    int32 (*setDisplayMode)(void* request);               // D3DKMT_SETDISPLAYMODE (in/out)
    int32 (*setDisplayPrivateDriverFormat)(const void* request); // D3DKMT_SETDISPLAYPRIVATEDRIVERFORMAT
    int32 (*setGammaRamp)(const void* request);           // D3DKMT_SETGAMMARAMP
    int   (*showCursor)(int show);
};
const FullscreenCalls& WindowsFullscreenCalls();

struct FullscreenTarget
{
    uint32 adapter;  // Screen+0x148 = main Device KMT adapter handle (adapter+0x970)
    uint32 device;   // adapter+0x974: main Device KMT device handle
    uint32 sourceId; // Screen+0x30: VidPn source ID
};

// The facts TakeFullscreenOwnership reads from the presentable image.
struct FullscreenImage
{
    uint32 width;             // image+0x1c: create-info extent
    uint32 height;            // image+0x20
    bool   hwRotation;        // image+0xe0 bit0: internal flags.hwRotationEnabled
    bool   hasMemory;         // image+0x190: bound GPU memory present
    uint32 primaryAllocation; // first KMT allocation of the root bound GPU memory
};

// Device-level steps of a successful acquire / any release.
struct FullscreenHooks
{
    void*  context;
    Result (*notify)(void* context, bool fullscreen); // 3687a0 on the screen's Device
    Result (*applyQueuedLimit)(void* context);        // 310510 on the image's Device
};

Result TakeFullscreenOwnership(const FullscreenCalls& calls, const FullscreenTarget& target,
                               const FullscreenImage& image, ExclusiveModeLock& exclusiveMode,
                               const FullscreenHooks& hooks, bool* owned);
Result ReleaseFullscreenOwnership(const FullscreenCalls& calls, const FullscreenTarget& target,
                                  ExclusiveModeLock& exclusiveMode, const FullscreenHooks& hooks, bool* owned);
Result SetFullscreenGammaRamp(const FullscreenCalls& calls, const FullscreenTarget& target, bool owned,
                              const GammaRamp& gammaRamp);

// Inputs of the fullscreen notification 3687a0 taken from Device and Platform state.
struct FullscreenNotifyState
{
    bool multiAdapter; // KMD adapter record DWORD 8 (adapter+0x10) > 1; also the escape's HardwareAccess
    bool frameLock;    // Device+0x1054 FLGL cache bit1 (FlglState::frameLockEnabled)
    bool profiler;     // Platform settings +0x660 (gpuProfilerMode) != 0
};
uint32 FullscreenNotifyFlags(bool fullscreen, const FullscreenNotifyState& state);
// Escape 0x1001403, 188-byte record, flags DWORD at +0x80.
Result NotifyFullscreenState(const EscapeRoute& route, bool fullscreen, const FullscreenNotifyState& state);

// ---------------------------------------------------------------------------------------------------------------------
// Screen color (native Screen slots 11, 12 = 36a720, 36a8f0) through AMD display ("DAL") private escapes.
// Every record starts {0x80, 0x10000, 0x2000000} and carries the operation at +0x88.

constexpr size_t DisplayInfoBytes = 0x1a9; // 425-byte reply cached at Screen+0x40

// Per-screen cache: native Screen+0x38, +0x40, +0x14c bits 1..2 and +0x150.
struct ScreenColorCache
{
    int32  displayIndex  = -1;
    uint8* displayInfo   = nullptr;
    uint32 advancedColor = 0; // bit1 = HDR/advanced color supported, bit2 = active
    uint32 colorState    = 0; // ScreenColorSpace bits last read from or sent to the KMD
};

struct ScreenColorContext
{
    const EscapeRoute* route;   // Null when the screen has no PAL Device (Screen+0x18 null).
    void* allocatorContext;     // Platform allocator for the display information (+0x20).
    void* (*alloc)(void* context, size_t bytes);
    void  (*free)(void* context, void* memory);
    int32 (*displayConfigGetDeviceInfo)(void* header); // DISPLAYCONFIG_DEVICE_INFO_HEADER
    uint32 sourceAdapterLow;    // Screen+0x28: source LUID
    int32  sourceAdapterHigh;
    uint32 targetId;            // Screen+0x34
};

int32  CachedDisplayIndex(ScreenColorCache* cache, const ScreenColorContext& context);  // 36a620
void   EnsureDisplayInfo(ScreenColorCache* cache, const ScreenColorContext& context);   // 36a240
Result RefreshColorState(ScreenColorCache* cache, const ScreenColorContext& context);   // 36aba0
Result GetScreenColorCapabilities(ScreenColorCache* cache, const ScreenColorContext& context,
                                  ScreenColorCapabilities* caps);                        // 36a720
Result SetScreenColorConfiguration(ScreenColorCache* cache, const ScreenColorContext& context,
                                   const ScreenColorConfig& config);                     // 36a8f0
void   ReleaseColorCache(ScreenColorCache* cache, const ScreenColorContext& context);   // 3688d9..3688f4

// Record builders behind the methods above.
int32  QueryDisplayIndex(const EscapeRoute& route, uint32 targetId);                     // escape 0x150007
bool   QueryDisplayInfo(const EscapeRoute& route, int32 displayIndex, uint8* info);      // escape 0x130004
Result QueryDisplayColorState(const EscapeRoute& route, int32 displayIndex, uint32* state); // escape 0x150063
Result SetDisplayColorState(const EscapeRoute& route, int32 displayIndex, const ScreenColorConfig& config,
                            bool hasDisplayInfo);                                        // escape 0x150062

uint32 DisplayColorStateToPal(uint32 transfer, uint32 space); // KMD -> ScreenColorSpace
uint32 PalTransferToDisplay(uint32 colorSpace);                // ScreenColorSpace TF -> KMD (first match)
uint32 PalSpaceToDisplay(uint32 colorSpace);                   // ScreenColorSpace CS -> KMD (first match)
void   FillDisplayColorCapabilities(const uint8* info, ScreenColorCapabilities* caps);
uint32 MergeColorStateFlags(uint32 flags, uint32 colorState, uint32 advancedColor);
uint32 AdvancedColorFromInfo2(uint32 advancedColor, uint32 value);
uint32 AdvancedColorFromInfo(uint32 advancedColor, uint32 value);

// ---------------------------------------------------------------------------------------------------------------------
// DXGI swap chain HDR metadata (native 656b2c8 slot 6 = 388670).

struct Hdr10Metadata // DXGI_HDR_METADATA_HDR10
{
    uint16 redPrimary[2];
    uint16 greenPrimary[2];
    uint16 bluePrimary[2];
    uint16 whitePoint[2];
    uint32 maxMasteringLuminance;
    uint32 minMasteringLuminance;
    uint16 maxContentLightLevel;
    uint16 maxFrameAverageLightLevel;
};
static_assert(sizeof(Hdr10Metadata) == 28);

struct HdrMetaDataCalls
{
    void* context;
    int32 (*setColorSpace1)(void* context, uint32 colorSpace);                       // IDXGISwapChain3 slot 38
    int32 (*setHdrMetaData)(void* context, uint32 type, uint32 bytes, const void* data); // IDXGISwapChain4 slot 40
};
Result ApplyHdrMetaData(const HdrMetaDataCalls& calls, const ScreenColorConfig& config);
void   BuildHdr10Metadata(const ScreenColorConfig& config, Hdr10Metadata* metadata);
uint32 HdrSwapChainColorSpace(uint32 colorSpace); // DXGI_COLOR_SPACE_TYPE for SetColorSpace1
Result DxgiStatusToResult(int32 status);           // native HRESULT translator 54c1a0
} // namespace Pal::Wddm
