// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmFullscreen.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmResult.h"

#include <cstring>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <aclapi.h>

namespace Pal::Wddm
{
namespace
{
static_assert(sizeof(D3DKMT_CURRENTDISPLAYMODE) == 0x30);
static_assert(offsetof(D3DKMT_CURRENTDISPLAYMODE, DisplayMode.Width) == 4);
static_assert(offsetof(D3DKMT_CURRENTDISPLAYMODE, DisplayMode.DisplayOrientation) == 0x20);
static_assert(sizeof(D3DKMT_GETDEVICESTATE) == 0x38);
static_assert(offsetof(D3DKMT_SETVIDPNSOURCEOWNER1, Flags) == 0x20);
static_assert(sizeof(D3DKMT_SETDISPLAYMODE) == 0x1c);
static_assert(sizeof(D3DKMT_SETDISPLAYPRIVATEDRIVERFORMAT) == 12);
static_assert(offsetof(D3DKMT_SETGAMMARAMP, pGammaRampDXGI1) == 0x10);
static_assert(offsetof(D3DKMT_SETGAMMARAMP, Size) == 0x18);
// The public GammaRamp is handed to the KMT unchanged as D3DDDI_GAMMA_RAMP_DXGI_1.
static_assert((sizeof(GammaRamp) == sizeof(D3DDDI_GAMMA_RAMP_DXGI_1)) && (sizeof(GammaRamp) == 0x3024));
static_assert(offsetof(GammaRamp, offset) == offsetof(D3DDDI_GAMMA_RAMP_DXGI_1, Offset));
static_assert(offsetof(GammaRamp, gammaCurve) == offsetof(D3DDDI_GAMMA_RAMP_DXGI_1, GammaCurve));
static_assert(sizeof(ScreenColorCapabilities) == 0x3c);

constexpr int32 GraphicsPresentModeChanged        = int32(0xc01e0005); // STATUS_GRAPHICS_PRESENT_MODE_CHANGED
constexpr int32 GraphicsVidPnModalityUnsupported  = int32(0xc01e0306); // STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED
constexpr int32 GraphicsIncompatiblePrivateFormat = int32(0xc01e0355); // STATUS_GRAPHICS_INCOMPATIBLE_PRIVATE_FORMAT

// Native 300c00 open/ACL access: SYNCHRONIZE | MUTEX_MODIFY_STATE.
constexpr DWORD ExclusiveMutexAccess = 0x00100001;

HANDLE OpenOrCreateExclusiveMutex(const char* name)
{
    HANDLE mutex = OpenMutexA(ExclusiveMutexAccess, TRUE, name);
    if (mutex != nullptr) { return mutex; }
    SECURITY_DESCRIPTOR descriptor = {};
    if (InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) != 1) { return nullptr; }
    alignas(8) BYTE world[0x44] = {};
    DWORD worldBytes = sizeof(world);
    if (CreateWellKnownSid(WinWorldSid, nullptr, world, &worldBytes) == FALSE) { return nullptr; }
    EXPLICIT_ACCESS_A access = {};
    access.grfAccessPermissions = ExclusiveMutexAccess;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    access.Trustee.ptstrName = reinterpret_cast<LPSTR>(world);
    PACL acl = nullptr;
    if (SetEntriesInAclA(1, &access, nullptr, &acl) != ERROR_SUCCESS) { return nullptr; }
    if (SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) != FALSE)
    {
        SECURITY_ATTRIBUTES attributes = {};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        mutex = CreateMutexA(&attributes, FALSE, name);
        if (mutex == nullptr) { mutex = OpenMutexA(ExclusiveMutexAccess, TRUE, name); }
    }
    LocalFree(acl);
    return mutex;
}

constexpr uint32 DisplayEscape = 0x2000000;

uint32 Read32(const uint8* bytes, size_t offset)
{
    uint32 value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

// cvttsd2si r32: out-of-range conversions produce the integer indefinite value.
int32 TruncateToInt32(double value)
{
    return ((value > -2147483649.0) && (value < 2147483648.0)) ? static_cast<int32>(value) : INT32_MIN;
}

uint16 Chromaticity(uint32 value)
{
    // 3886a5..3886bc: cvtsi2ss of the zero-extended DWORD, cvtps2pd, /100000.0, *50000.0, cvttsd2si.
    const float single = static_cast<float>(static_cast<int64>(value));
    const volatile double units = static_cast<double>(single) / 100000.0;
    return static_cast<uint16>(TruncateToInt32(units * 50000.0));
}

struct AdvancedColorInfo2 // DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2, type 15
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;
};
static_assert(sizeof(AdvancedColorInfo2) == 0x24);

struct AdvancedColorInfo // DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO, type 9
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
};
static_assert(sizeof(AdvancedColorInfo) == 0x20);
} // namespace

// =====================================================================================================================
ExclusiveModeLock::~ExclusiveModeLock()
{
    if (m_check != nullptr) { CloseHandle(m_check); }
    if (m_exclusive != nullptr) { CloseHandle(m_exclusive); }
}

Result ExclusiveModeLock::Init(const char* checkName, const char* exclusiveName)
{
    if (m_check == nullptr) { m_check = OpenOrCreateExclusiveMutex(checkName); }
    if (m_check == nullptr) { return Result::ErrorUnknown; }
    if (m_exclusive == nullptr) { m_exclusive = OpenOrCreateExclusiveMutex(exclusiveName); }
    return (m_exclusive != nullptr) ? Result::Success : Result::ErrorUnknown;
}

bool ExclusiveModeLock::Acquire()
{
    DWORD wait = WAIT_FAILED;
    bool acquired = false;
    WaitForSingleObject(m_check, INFINITE);
    if ((m_count == 0) && !m_releaseFailed) { wait = WaitForSingleObject(m_exclusive, 0); }
    // WAIT_OBJECT_0 and WAIT_ABANDONED both grant the mutex (test esi, 0xffffff7f).
    if ((m_count != 0) || m_releaseFailed || ((wait & ~DWORD(0x80)) == 0))
    {
        ++m_count;
        m_releaseFailed = false;
        acquired = true;
    }
    ReleaseMutex(m_check);
    return acquired;
}

void ExclusiveModeLock::Release()
{
    WaitForSingleObject(m_check, INFINITE);
    if (m_count != 0)
    {
        --m_count;
        if (m_count == 0) { m_releaseFailed = (ReleaseMutex(m_exclusive) == FALSE); }
    }
    ReleaseMutex(m_check);
}

// =====================================================================================================================
const FullscreenCalls& WindowsFullscreenCalls()
{
    static const FullscreenCalls calls =
    {
        [](void* request) -> int32 { return D3DKMTQueryAdapterInfo(static_cast<D3DKMT_QUERYADAPTERINFO*>(request)); },
        [](void* request) -> int32 { return D3DKMTGetDeviceState(static_cast<D3DKMT_GETDEVICESTATE*>(request)); },
        [](const void* request) -> int32
            { return D3DKMTSetVidPnSourceOwner(static_cast<const D3DKMT_SETVIDPNSOURCEOWNER*>(request)); },
        [](const void* request) -> int32
            { return D3DKMTSetVidPnSourceOwner1(static_cast<const D3DKMT_SETVIDPNSOURCEOWNER1*>(request)); },
        [](void* request) -> int32 { return D3DKMTSetDisplayMode(static_cast<D3DKMT_SETDISPLAYMODE*>(request)); },
        [](const void* request) -> int32
        {
            return D3DKMTSetDisplayPrivateDriverFormat(static_cast<const D3DKMT_SETDISPLAYPRIVATEDRIVERFORMAT*>(request));
        },
        [](const void* request) -> int32 { return D3DKMTSetGammaRamp(static_cast<const D3DKMT_SETGAMMARAMP*>(request)); },
        [](int show) -> int { return ShowCursor(show); },
    };
    return calls;
}

Result TakeFullscreenOwnership(const FullscreenCalls& calls, const FullscreenTarget& target,
                               const FullscreenImage& image, ExclusiveModeLock& exclusiveMode,
                               const FullscreenHooks& hooks, bool* owned)
{
    // 3691a0..369280
    if (*owned) { return Result::ErrorFullscreenUnavailable; }
    D3DKMT_CURRENTDISPLAYMODE mode = {};
    mode.VidPnSourceId = target.sourceId;
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = target.adapter;
    query.Type = KMTQAITYPE_CURRENTDISPLAYMODE;
    query.pPrivateDriverData = &mode;
    query.PrivateDriverDataSize = sizeof(mode);
    Result result = ConvertNativeStatus(calls.queryAdapterInfo(&query));
    if (result != Result::Success) { return result; }
    if (!image.hwRotation && ((image.width != mode.DisplayMode.Width) || (image.height != mode.DisplayMode.Height) ||
                              (mode.DisplayMode.DisplayOrientation != D3DDDI_ROTATION_IDENTITY)))
    {
        return Result::ErrorInvalidResolution;
    }

    // 369ed0..36a23b
    D3DKMT_GETDEVICESTATE state = {};
    state.hDevice = target.device;
    state.StateType = D3DKMT_DEVICESTATE_RESET;
    if ((ConvertNativeStatus(calls.getDeviceState(&state)) != Result::Success) ||
        (state.ResetState.DesktopSwitched != 0))
    {
        return Result::ErrorFullscreenUnavailable;
    }
    if (!exclusiveMode.Acquire())
    {
        // 369fe5 -> 36a22e: another owner holds the exclusive-mode mutex. The
        // original returns the device-state result (Success) without ownership.
        return Result::Success;
    }

    const D3DDDI_VIDEO_PRESENT_SOURCE_ID sourceId = target.sourceId;
    D3DKMT_VIDPNSOURCEOWNER_TYPE ownerType = D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE;
    D3DKMT_SETVIDPNSOURCEOWNER1 owner = {};
    owner.Version0.hDevice = target.device;
    owner.Version0.pType = &ownerType;
    owner.Version0.pVidPnSourceId = &sourceId;
    owner.Version0.VidPnSourceCount = 1;
    owner.Flags.DisableDWMVirtualMode = 1;
    result = ConvertNativeStatus(calls.setVidPnSourceOwner1(&owner));
    if (result == Result::Success) { result = ConvertNativeStatus(calls.setVidPnSourceOwner(&owner.Version0)); }
    if ((result == Result::Success) && !image.hasMemory)
    {
        // The original dereferences the missing GPU memory here.
        result = Result::ErrorInvalidImage;
    }
    if (result == Result::Success)
    {
        D3DKMT_SETDISPLAYMODE displayMode = {};
        displayMode.hDevice = target.device;
        displayMode.hPrimaryAllocation = image.primaryAllocation;
        displayMode.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
        displayMode.DisplayOrientation = D3DDDI_ROTATION_IDENTITY;
        int32 status = calls.setDisplayMode(&displayMode);
        if (status == GraphicsIncompatiblePrivateFormat)
        {
            // 36a0de..36a12b: the KMD returns its attribute in the request; apply
            // it and retry, eleven SetDisplayMode retries at most.
            D3DKMT_SETDISPLAYPRIVATEDRIVERFORMAT format = {};
            format.hDevice = displayMode.hDevice;
            format.VidPnSourceId = sourceId;
            for (uint32 attempt = 0;; ++attempt)
            {
                format.PrivateDriverFormatAttribute = displayMode.PrivateDriverFormatAttribute;
                static_cast<void>(calls.setDisplayPrivateDriverFormat(&format));
                status = calls.setDisplayMode(&displayMode);
                if ((attempt >= 10) || (status != GraphicsIncompatiblePrivateFormat)) { break; }
            }
        }
        else if (status == GraphicsVidPnModalityUnsupported)
        {
            displayMode.ScanLineOrdering = D3DDDI_VSSLO_INTERLACED_UPPERFIELDFIRST;
            status = calls.setDisplayMode(&displayMode);
        }
        result = (status == GraphicsPresentModeChanged) ? Result::ErrorIncompatibleScreenMode
                                                        : ConvertNativeStatus(status);
    }
    if (result == Result::Success)
    {
        calls.showCursor(FALSE);
        calls.showCursor(TRUE);
        result = hooks.notify(hooks.context, true);
    }
    if (result == Result::Success) { result = hooks.applyQueuedLimit(hooks.context); }
    if (result == Result::Success)
    {
        *owned = true;
        return result;
    }
    // 36a15b..36a19b: give the source back and drop the exclusive-mode reference.
    owner.Flags.DisableDWMVirtualMode = 0;
    ownerType = D3DKMT_VIDPNSOURCEOWNER_UNOWNED;
    const Result released = ConvertNativeStatus(calls.setVidPnSourceOwner1(&owner));
    *owned = false;
    exclusiveMode.Release();
    return Util::CollapseResults(result, released);
}

Result ReleaseFullscreenOwnership(const FullscreenCalls& calls, const FullscreenTarget& target,
                                  ExclusiveModeLock& exclusiveMode, const FullscreenHooks& hooks, bool* owned)
{
    // 369290..36932b
    if (!*owned) { return Result::ErrorFullscreenUnavailable; }
    const D3DDDI_VIDEO_PRESENT_SOURCE_ID sourceId = target.sourceId;
    const D3DKMT_VIDPNSOURCEOWNER_TYPE ownerType = D3DKMT_VIDPNSOURCEOWNER_UNOWNED;
    D3DKMT_SETVIDPNSOURCEOWNER1 owner = {};
    owner.Version0.hDevice = target.device;
    owner.Version0.pType = &ownerType;
    owner.Version0.pVidPnSourceId = &sourceId;
    owner.Version0.VidPnSourceCount = 1;
    const Result result = ConvertNativeStatus(calls.setVidPnSourceOwner1(&owner));
    if (result != Result::Success) { return result; }
    *owned = false;
    exclusiveMode.Release();
    return hooks.notify(hooks.context, false);
}

Result SetFullscreenGammaRamp(const FullscreenCalls& calls, const FullscreenTarget& target, bool owned,
                              const GammaRamp& gammaRamp)
{
    // 369330..36939d
    if (!owned) { return Result::ErrorUnavailable; }
    D3DKMT_SETGAMMARAMP request = {};
    request.hDevice = target.device;
    request.VidPnSourceId = target.sourceId;
    request.Type = D3DDDI_GAMMARAMP_DXGI_1;
    request.pGammaRampDXGI1 =
        const_cast<D3DDDI_GAMMA_RAMP_DXGI_1*>(reinterpret_cast<const D3DDDI_GAMMA_RAMP_DXGI_1*>(&gammaRamp));
    request.Size = sizeof(D3DDDI_GAMMA_RAMP_DXGI_1);
    return ConvertNativeStatus(calls.setGammaRamp(&request));
}

uint32 FullscreenNotifyFlags(bool fullscreen, const FullscreenNotifyState& state)
{
    // 3687e9..368873, then bit 10 from the profiler setting.
    uint32 flags = 1;
    if (fullscreen) { flags |= 0x2; }
    if (state.multiAdapter) { flags |= 0x8 | 0x80 | 0x100; }
    if (state.frameLock) { flags |= 0x20; }
    if (state.frameLock && fullscreen) { flags |= 0x40; }
    if (state.profiler) { flags |= 0x400; }
    return flags;
}

Result NotifyFullscreenState(const EscapeRoute& route, bool fullscreen, const FullscreenNotifyState& state)
{
    uint8 record[0xbc];
    InitPrivateRecord(record, sizeof(record), 0x1001403);
    PutRecord32(record, 0x80, FullscreenNotifyFlags(fullscreen, state));
    return SharedEscape(route, record, sizeof(record), state.multiAdapter, 0);
}

// =====================================================================================================================
int32 QueryDisplayIndex(const EscapeRoute& route, uint32 targetId)
{
    uint8 record[0xac];
    InitPrivateRecord(record, sizeof(record), DisplayEscape);
    PutRecord32(record, 0x80, 0x18);
    PutRecord32(record, 0x84, 0x18);
    PutRecord32(record, 0x88, 0x150007);
    PutRecord32(record, 0x94, 8);
    PutRecord32(record, 0x98, targetId);
    PutRecord32(record, 0x9c, 0xc);
    PutRecord32(record, 0xa0, 0xc);
    if ((SharedEscape(route, record, sizeof(record)) != Result::Success) || (GetRecord32(record, 0x48) != 0))
    {
        return -1;
    }
    return static_cast<int32>(GetRecord32(record, 0xa8));
}

bool QueryDisplayInfo(const EscapeRoute& route, int32 displayIndex, uint8* info)
{
    uint8 record[0x244];
    InitPrivateRecord(record, sizeof(record), DisplayEscape);
    PutRecord32(record, 0x80, 0x10);
    PutRecord32(record, 0x84, 0x10);
    PutRecord32(record, 0x88, 0x130004);
    PutRecord32(record, 0x8c, static_cast<uint32>(displayIndex));
    PutRecord32(record, 0x94, uint32(DisplayInfoBytes));
    PutRecord32(record, 0x98, uint32(DisplayInfoBytes));
    if ((SharedEscape(route, record, sizeof(record)) != Result::Success) || (GetRecord32(record, 0x48) != 0))
    {
        return false;
    }
    std::memcpy(info, record + 0x98, DisplayInfoBytes);
    return Read32(info, 4) != 0;
}

Result QueryDisplayColorState(const EscapeRoute& route, int32 displayIndex, uint32* state)
{
    uint8 record[0xfc];
    InitPrivateRecord(record, sizeof(record), DisplayEscape);
    PutRecord32(record, 0x80, 0x10);
    PutRecord32(record, 0x84, 0x10);
    PutRecord32(record, 0x88, 0x150063);
    PutRecord32(record, 0x8c, static_cast<uint32>(displayIndex));
    PutRecord32(record, 0x94, 0x64);
    PutRecord32(record, 0x98, 0x64);
    PutRecord32(record, 0x9c, 2);
    Result result = SharedEscape(route, record, sizeof(record));
    if (result == Result::Success)
    {
        if (GetRecord32(record, 0x48) != 0) { result = Result::ErrorUnknown; }
        else { *state = DisplayColorStateToPal(GetRecord32(record, 0xa0), GetRecord32(record, 0xa4)); }
    }
    return result;
}

Result SetDisplayColorState(const EscapeRoute& route, int32 displayIndex, const ScreenColorConfig& config,
                            bool hasDisplayInfo)
{
    uint8 record[0xfc];
    InitPrivateRecord(record, sizeof(record), DisplayEscape);
    PutRecord32(record, 0x80, 0x74);
    PutRecord32(record, 0x84, 0x74);
    PutRecord32(record, 0x88, 0x150062);
    PutRecord32(record, 0x8c, static_cast<uint32>(displayIndex));
    PutRecord32(record, 0x94, 0x64);
    PutRecord32(record, 0x98, 2);
    PutRecord32(record, 0x9c, PalTransferToDisplay(config.colorSpace));
    PutRecord32(record, 0xa0, PalSpaceToDisplay(config.colorSpace));
    if (hasDisplayInfo)
    {
        // 36aa83..36aac8: primaries in green, blue, red, white order; avgLuminance is not sent.
        const ColorGamut& gamut = config.userDefinedColorGamut;
        PutRecord32(record, 0xa4, gamut.chromaticityGreenX);
        PutRecord32(record, 0xa8, gamut.chromaticityGreenY);
        PutRecord32(record, 0xac, gamut.chromaticityBlueX);
        PutRecord32(record, 0xb0, gamut.chromaticityBlueY);
        PutRecord32(record, 0xb4, gamut.chromaticityRedX);
        PutRecord32(record, 0xb8, gamut.chromaticityRedY);
        PutRecord32(record, 0xbc, gamut.chromaticityWhitePointX);
        PutRecord32(record, 0xc0, gamut.chromaticityWhitePointY);
        PutRecord32(record, 0xc4, gamut.minLuminance);
        PutRecord32(record, 0xc8, gamut.maxLuminance);
        PutRecord32(record, 0xcc, gamut.maxContentLightLevel);
        PutRecord32(record, 0xd0, gamut.maxFrameAverageLightLevel);
    }
    if (config.localDimmingDisable != 0) { PutRecord32(record, 0xd4, GetRecord32(record, 0xd4) | 1); }
    Result result = SharedEscape(route, record, sizeof(record));
    if ((result == Result::Success) && (GetRecord32(record, 0x48) != 0)) { result = Result::ErrorUnknown; }
    return result;
}

uint32 DisplayColorStateToPal(uint32 transfer, uint32 space)
{
    // 36ac76..36acbb and 36a7d8..36a830.
    uint32 bits = (space & 0x80) << 1;
    bits = ((bits | (space & 0x70)) << 1) | (space & 0x10c);
    bits = ((bits << 5) | (space & 0x200)) << 6;
    return bits | ((space & 1) << 12) | ((transfer >> 1) & 0x78) | (transfer & 7);
}

uint32 PalTransferToDisplay(uint32 colorSpace)
{
    // 36a997..36a9f4: first matching transfer function.
    if ((colorSpace & TfSrgb) != 0) { return 0x1; }
    if ((colorSpace & TfBt709) != 0) { return 0x2; }
    if ((colorSpace & TfPq2084) != 0) { return 0x4; }
    if ((colorSpace & TfLinear0_1) != 0) { return 0x10; }
    if ((colorSpace & TfLinear0_125) != 0) { return 0x20; }
    if ((colorSpace & TfDolbyVision) != 0) { return 0x40; }
    if ((colorSpace & TfGamma22) != 0) { return 0x80; }
    return 0;
}

uint32 PalSpaceToDisplay(uint32 colorSpace)
{
    // 36a9f7..36aa7a: first matching color space; Native is tested before UserDefined.
    if ((colorSpace & CsSrgb) != 0) { return 0x1; }
    if ((colorSpace & CsBt709) != 0) { return 0x4; }
    if ((colorSpace & CsBt2020) != 0) { return 0x8; }
    if ((colorSpace & CsDolbyVision) != 0) { return 0x200; }
    if ((colorSpace & CsAdobe) != 0) { return 0x10; }
    if ((colorSpace & CsDciP3) != 0) { return 0x20; }
    if ((colorSpace & CsScrgb) != 0) { return 0x40; }
    if ((colorSpace & CsNative) != 0) { return 0x80; }
    if ((colorSpace & CsUserDefined) != 0) { return 0x100; }
    return 0;
}

void FillDisplayColorCapabilities(const uint8* info, ScreenColorCapabilities* caps)
{
    // 36a76a..36a874
    ColorGamut& gamut = caps->nativeColorGamut;
    gamut.chromaticityRedX        = Read32(info, 0x171);
    gamut.chromaticityRedY        = Read32(info, 0x175);
    gamut.chromaticityGreenX      = Read32(info, 0x179);
    gamut.chromaticityGreenY      = Read32(info, 0x17d);
    gamut.chromaticityBlueX       = Read32(info, 0x181);
    gamut.chromaticityBlueY       = Read32(info, 0x185);
    gamut.chromaticityWhitePointX = Read32(info, 0x189);
    gamut.chromaticityWhitePointY = Read32(info, 0x18d);
    gamut.minLuminance            = Read32(info, 0x15d);
    gamut.avgLuminance            = Read32(info, 0x161);
    gamut.maxLuminance            = Read32(info, 0x165);
    caps->supportedColorSpaces = static_cast<ScreenColorSpace>(
        caps->supportedColorSpaces | DisplayColorStateToPal(Read32(info, 0x169), Read32(info, 0x16d)));
    if ((info[0x199] & 7) != 0)
    {
        if ((info[0x199] & 1) != 0) { caps->u32All |= 0x2; } // hdr10Supported
        if ((info[0x199] & 2) != 0) { caps->u32All |= 0x1; } // dolbyVisionSupported
        if ((info[0x199] & 4) != 0)
        {
            caps->u32All |= 0x4;                                 // freeSyncHdrSupported
            if ((info[0x19d] & 1) != 0) { caps->u32All |= 0x8; } // freeSyncBacklightSupport
        }
    }
}

uint32 MergeColorStateFlags(uint32 flags, uint32 colorState, uint32 advancedColor)
{
    // 36a885..36a8cf. isHdrEnabled: the KMD state is not sRGB/sRGB or the OS reports advanced color active.
    const uint32 notSrgb = ((colorState & (CsSrgb | TfSrgb)) != (CsSrgb | TfSrgb)) ? 4 : 0;
    flags = (flags & ~0x10u) | (((notSrgb | advancedColor) & 4) << 2);
    // hdr10Supported is also set when the OS reports HDR/advanced color support.
    return (flags & ~0x2u) | ((flags | advancedColor) & 2);
}

uint32 AdvancedColorFromInfo2(uint32 advancedColor, uint32 value)
{
    // 36ad02..36ad2d: bit1 = highDynamicRangeSupported; bit2 = advancedColorActive | highDynamicRangeUserEnabled.
    const uint32 supported = (advancedColor & ~0x2u) | ((value >> 3) & 2);
    return (supported & ~0x4u) | (((value << 1) | (value >> 3)) & 4);
}

uint32 AdvancedColorFromInfo(uint32 advancedColor, uint32 value)
{
    // 36ad75..36ad85: bit1 = advancedColorSupported; bit2 = advancedColorEnabled.
    return (advancedColor & ~0x6u) | ((value & 3) << 1);
}

int32 CachedDisplayIndex(ScreenColorCache* cache, const ScreenColorContext& context)
{
    // 36a620: queried until a nonnegative index is cached.
    if (cache->displayIndex < 0)
    {
        cache->displayIndex = (context.route != nullptr) ? QueryDisplayIndex(*context.route, context.targetId) : -1;
    }
    return cache->displayIndex;
}

void EnsureDisplayInfo(ScreenColorCache* cache, const ScreenColorContext& context)
{
    // 36a240: one Platform allocation (425 bytes, AllocInternal) kept once valid.
    if (cache->displayInfo != nullptr) { return; }
    auto* info = static_cast<uint8*>(context.alloc(context.allocatorContext, DisplayInfoBytes));
    if (info == nullptr) { return; }
    std::memset(info, 0, DisplayInfoBytes);
    // A cached index implies a Device; the route test only guards the impossible
    // combination on which the original would dereference a null Device.
    const int32 index = CachedDisplayIndex(cache, context);
    if ((index >= 0) && (context.route != nullptr) && QueryDisplayInfo(*context.route, index, info))
    {
        cache->displayInfo = info;
    }
    else
    {
        context.free(context.allocatorContext, info);
    }
}

Result RefreshColorState(ScreenColorCache* cache, const ScreenColorContext& context)
{
    // 36aba0: returns the escape result; DISPLAYCONFIG only updates the advanced-color bits.
    Result result = Result::ErrorUnavailable;
    if (context.route != nullptr)
    {
        const int32 index = CachedDisplayIndex(cache, context); // -1 is still sent, as in the original.
        uint32 state = 0;
        result = QueryDisplayColorState(*context.route, index, &state);
        if (result == Result::Success) { cache->colorState = state; }
    }
    AdvancedColorInfo2 info2 = {};
    info2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
    info2.header.size = sizeof(info2);
    info2.header.adapterId = { context.sourceAdapterLow, context.sourceAdapterHigh };
    info2.header.id = context.targetId;
    // The original's Win32 translator 368510 maps only ERROR_SUCCESS to Success.
    if (context.displayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS)
    {
        cache->advancedColor = AdvancedColorFromInfo2(cache->advancedColor, info2.value);
    }
    else
    {
        AdvancedColorInfo info = {};
        info.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(9);
        info.header.size = sizeof(info);
        info.header.adapterId = { context.sourceAdapterLow, context.sourceAdapterHigh };
        info.header.id = context.targetId;
        if (context.displayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS)
        {
            cache->advancedColor = AdvancedColorFromInfo(cache->advancedColor, info.value);
        }
    }
    return result;
}

Result GetScreenColorCapabilities(ScreenColorCache* cache, const ScreenColorContext& context,
                                  ScreenColorCapabilities* caps)
{
    // 36a720..36a8ee
    *caps = {};
    if (cache->displayInfo == nullptr) { EnsureDisplayInfo(cache, context); }
    if ((cache->displayInfo == nullptr) || (Read32(cache->displayInfo, 4) == 0)) { return Result::ErrorUnavailable; }
    FillDisplayColorCapabilities(cache->displayInfo, caps);
    const Result result = RefreshColorState(cache, context);
    if (result == Result::Success)
    {
        caps->u32All = MergeColorStateFlags(caps->u32All, cache->colorState, cache->advancedColor);
    }
    return result;
}

Result SetScreenColorConfiguration(ScreenColorCache* cache, const ScreenColorContext& context,
                                   const ScreenColorConfig& config)
{
    // 36a8f0..36ab98. Without a Device or display information no escape is sent.
    if (context.route == nullptr) { return Result::ErrorUnavailable; }
    EnsureDisplayInfo(cache, context);
    if (cache->displayInfo == nullptr) { return Result::ErrorUnavailable; }
    const Result result = SetDisplayColorState(*context.route, CachedDisplayIndex(cache, context), config, true);
    // The original also stores the color space in Device+0x9554, which has no
    // consumer in this backend, and emits a developer log entry.
    if (result == Result::Success) { cache->colorState = config.colorSpace; }
    return result;
}

void ReleaseColorCache(ScreenColorCache* cache, const ScreenColorContext& context)
{
    if (cache->displayInfo != nullptr)
    {
        context.free(context.allocatorContext, cache->displayInfo);
        cache->displayInfo = nullptr;
    }
}

// =====================================================================================================================
void BuildHdr10Metadata(const ScreenColorConfig& config, Hdr10Metadata* metadata)
{
    const ColorGamut& gamut = config.userDefinedColorGamut;
    metadata->redPrimary[0]   = Chromaticity(gamut.chromaticityRedX);
    metadata->redPrimary[1]   = Chromaticity(gamut.chromaticityRedY);
    metadata->greenPrimary[0] = Chromaticity(gamut.chromaticityGreenX);
    metadata->greenPrimary[1] = Chromaticity(gamut.chromaticityGreenY);
    metadata->bluePrimary[0]  = Chromaticity(gamut.chromaticityBlueX);
    metadata->bluePrimary[1]  = Chromaticity(gamut.chromaticityBlueY);
    metadata->whitePoint[0]   = Chromaticity(gamut.chromaticityWhitePointX);
    metadata->whitePoint[1]   = Chromaticity(gamut.chromaticityWhitePointY);
    // Luminances are copied unscaled (both use 0.0001 nit units); light levels are truncated to 16 bits.
    metadata->maxMasteringLuminance     = gamut.maxLuminance;
    metadata->minMasteringLuminance     = gamut.minLuminance;
    metadata->maxContentLightLevel      = static_cast<uint16>(gamut.maxContentLightLevel);
    metadata->maxFrameAverageLightLevel = static_cast<uint16>(gamut.maxFrameAverageLightLevel);
}

uint32 HdrSwapChainColorSpace(uint32 colorSpace)
{
    // 3887c1..3887ee: DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 (0), _G10_NONE_P709 (1), _G2084_NONE_P2020 (12).
    if (((colorSpace & CsSrgb) != 0) && ((colorSpace & TfSrgb) != 0)) { return 0; }
    if (((colorSpace & CsScrgb) != 0) && ((colorSpace & TfLinear0_125) != 0)) { return 1; }
    if (((colorSpace & CsBt2020) != 0) && ((colorSpace & TfPq2084) != 0)) { return 12; }
    return 0;
}

Result ApplyHdrMetaData(const HdrMetaDataCalls& calls, const ScreenColorConfig& config)
{
    // 388670: SetColorSpace1, then SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, 28 bytes).
    Hdr10Metadata metadata = {};
    BuildHdr10Metadata(config, &metadata);
    Result result = DxgiStatusToResult(calls.setColorSpace1(calls.context, HdrSwapChainColorSpace(config.colorSpace)));
    if (result == Result::Success)
    {
        result = DxgiStatusToResult(calls.setHdrMetaData(calls.context, 1, sizeof(metadata), &metadata));
    }
    return result;
}

Result DxgiStatusToResult(int32 status)
{
    switch (static_cast<uint32>(status))
    {
    case 0x00000000:
    case 0x00000001: return Result::Success;                       // S_OK, S_FALSE
    case 0x887a000b: return Result::Success;                       // DXGI_ERROR_FRAME_STATISTICS_DISJOINT
    case 0x8000000a: return Result::NotReady;                      // E_PENDING
    case 0x887a000a: return Result::NotReady;                      // DXGI_ERROR_WAS_STILL_DRAWING
    case 0x087a0008: return Result::NotReady;                      // DXGI_STATUS_MODE_CHANGE_IN_PROGRESS
    case 0x80004003: return Result::ErrorInvalidPointer;           // E_POINTER
    case 0x80004004: return Result::Aborted;                       // E_ABORT
    case 0x80004005: return Result::ErrorUnknown;                  // E_FAIL
    case 0xc0262111: return Result::ErrorUnknown;
    case 0x80070005: return Result::ErrorPermissionDenied;         // E_ACCESSDENIED
    case 0x8007000e: return Result::ErrorOutOfMemory;              // E_OUTOFMEMORY
    case 0x80070057: return Result::ErrorInvalidValue;             // E_INVALIDARG
    case 0x887a0001: return Result::ErrorInvalidValue;             // DXGI_ERROR_INVALID_CALL
    case 0x88760873: return Result::ErrorInvalidValue;
    case 0x8876017c: return Result::ErrorOutOfGpuMemory;           // D3DERR_OUTOFVIDEOMEMORY
    case 0x8876086a: return Result::ErrorUnavailable;              // D3DERR_NOTAVAILABLE
    case 0x887a0033: return Result::ErrorUnavailable;              // DXGI_ERROR_CACHE_CORRUPT
    case 0x88760870: return Result::ErrorDeviceLost;               // D3DERR_DEVICELOST
    case 0x887a0005:                                               // DXGI_ERROR_DEVICE_REMOVED
    case 0x887a0006:                                               // DXGI_ERROR_DEVICE_HUNG
    case 0x887a0007: return Result::ErrorDeviceLost;               // DXGI_ERROR_DEVICE_RESET
    case 0x8876087a: return Result::ErrorInvalidFormat;
    case 0x887a0002: return Result::NotFound;                      // DXGI_ERROR_NOT_FOUND
    case 0x887a0003: return Result::ErrorInvalidMemorySize;        // DXGI_ERROR_MORE_DATA
    case 0x887a0034: return Result::ErrorShaderCacheFull;          // DXGI_ERROR_CACHE_FULL
    case 0x887a0035: return Result::ErrorShaderCacheHashCollision; // DXGI_ERROR_CACHE_HASH_COLLISION
    case 0x887a0036: return Result::AlreadyExists;                 // DXGI_ERROR_ALREADY_EXISTS
    case 0xc0262109: return Result::ErrorGpuMemoryMapFailed;
    case 0x08760878:                                               // S_PRESENT_OCCLUDED
    case 0x087a0001: return Result::PresentOccluded;               // DXGI_STATUS_OCCLUDED
    case 0x087a0007: return Result::ErrorIncompatibleWindowSize;   // DXGI_STATUS_MODE_CHANGED
    default:         return Result::Unsupported;
    }
}
} // namespace Pal::Wddm
