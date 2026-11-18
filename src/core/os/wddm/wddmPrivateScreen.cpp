// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Windows private screens: Device-level enumeration/emulation and the OS half of
// Pal::PrivateScreen. The core half (state checks, removed/enabled bookkeeping)
// is pal/src/core/privateScreen.cpp, which the original inlines unchanged.
// Every record below is {0x80, 0x10000, command, 3, ..., inputBytes @+0x80,
// input @+0x84, outputBytes, output} sent through the shared escape 30f860.
#include "core/os/wddm/wddmPrivateScreen.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmFence.h"
#include "core/device.h"
#include "core/image.h"
#include "core/platform.h"
#include "palAutoBuffer.h"
#include "palFormatInfo.h"
#include "palInlineFuncs.h"
#include "palMetroHash.h"
#include "palSysMemory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
// Native layouts this file reads and writes by offset.
static_assert(sizeof(PrivateScreenCreateInfo) == 0x248, "Original m_privateScreenInfo stride is 0x248");
static_assert(offsetof(PrivateScreenCreateInfo, props) + offsetof(PrivateScreenProperties, edid) == 0x2c);
static_assert(sizeof(PrivateScreen) == 0x360, "Original PrivateScreen is 0x360 bytes before its format list");
static_assert(sizeof(PrivateDisplayMode) == 0x58);
static_assert(offsetof(PrivateDisplayMode, verticalTiming) == 0x24 && offsetof(PrivateDisplayMode, flags) == 0x54);
static_assert(sizeof(PrivateScreenConnectorProperties) == 16 && sizeof(PrivateScreenBridge) == 20);
static_assert(sizeof(SwizzledFormat) == 8 && sizeof(ChannelMapping) == 4);
static_assert(static_cast<uint32>(ChNumFormat::X8Y8Z8W8_Unorm) == 0x20 &&
              static_cast<uint32>(ChNumFormat::X16Y16Z16W16_Float) == 0x47,
              "The format tables below are keyed by the original ChNumFormat values");

constexpr uint32 Version3 = 3;

// Commands (record+8).
constexpr uint32 CmdListScreens        = 0x03005803;
constexpr uint32 CmdScreenInfo         = 0x03005903;
constexpr uint32 CmdEnable             = 0x03005a03;
constexpr uint32 CmdDisable            = 0x03005b03;
constexpr uint32 CmdPresent            = 0x03005c03;
constexpr uint32 CmdPresentStats       = 0x03005d03;
constexpr uint32 CmdBlank              = 0x03005e03;
constexpr uint32 CmdScanLine           = 0x03006203;
constexpr uint32 CmdConnector          = 0x03006b03;
constexpr uint32 CmdSetDisplayMode     = 0x03006c03;
constexpr uint32 CmdSetGammaRamp       = 0x03006d03;
constexpr uint32 CmdSetColorMatrix     = 0x03006e03;
constexpr uint32 CmdSetPowerMode       = 0x03006f03;
constexpr uint32 CmdEventAfterVsync    = 0x03007303;
constexpr uint32 CmdHdcpStatus         = 0x03007603;
constexpr uint32 CmdEnableAudio        = 0x03007f03;
constexpr uint32 CmdRegisterEvent      = 0x01002603; // 311ed0
constexpr uint32 CmdUnregisterEvent    = 0x01003e03; // 311fe0
constexpr uint32 PrivateScreenEventType = 4;         // Hot-plug event class used by 311ba0 and Cleanup.

// Original GetPrivateScreens/AddEmulatedPrivateScreen allocation: object plus eight formats.
constexpr size_t ScreenAllocationBytes = sizeof(PrivateScreen) + MaxPrivateScreenFormats * sizeof(SwizzledFormat);
static_assert(ScreenAllocationBytes == 0x3a0);

// Copy of the original format list at .rdata 5849570 (same order as core AllPrivateScreenFormats).
const SwizzledFormat PrivateFormats[MaxPrivateScreenFormats] =
{
    { ChNumFormat::X5Y6Z5_Unorm,       { { { ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::One } } } },
    { ChNumFormat::X8Y8Z8W8_Unorm,     { { { ChannelSwizzle::X, ChannelSwizzle::Y, ChannelSwizzle::Z, ChannelSwizzle::W   } } } },
    { ChNumFormat::X8Y8Z8W8_Srgb,      { { { ChannelSwizzle::X, ChannelSwizzle::Y, ChannelSwizzle::Z, ChannelSwizzle::W   } } } },
    { ChNumFormat::X8Y8Z8W8_Unorm,     { { { ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::W   } } } },
    { ChNumFormat::X8Y8Z8W8_Srgb,      { { { ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::W   } } } },
    { ChNumFormat::X10Y10Z10W2_Unorm,  { { { ChannelSwizzle::X, ChannelSwizzle::Y, ChannelSwizzle::Z, ChannelSwizzle::W   } } } },
    { ChNumFormat::X10Y10Z10W2_Unorm,  { { { ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::W   } } } },
    { ChNumFormat::X16Y16Z16W16_Float, { { { ChannelSwizzle::X, ChannelSwizzle::Y, ChannelSwizzle::Z, ChannelSwizzle::W   } } } },
};

// Non-zero entries of the original (ChNumFormat, swizzleValue) -> D3DDDIFORMAT table
// at .rdata 5838fc0 (six slots per format, first swizzle match wins, no duplicate
// swizzles in a row). Rows 0x00..0xCD; the original's rows 0xCE..0xD1 are all zero
// and those formats do not exist in this PAL interface.
struct KmdFormatEntry
{
    uint8  format;
    uint32 swizzle;
    uint32 kmdFormat;
};
constexpr KmdFormatEntry KmdFormatTable[] =
{
    {0x01, 0x01000002, 402}, {0x05, 0x03020202, 52}, {0x06, 0x05020304, 26}, {0x06, 0x05040302, 429},
    {0x06, 0x02030405, 430}, {0x06, 0x02050403, 431}, {0x06, 0x01040302, 434}, {0x06, 0x01020304, 30},
    {0x08, 0x01020304, 23}, {0x08, 0x01040302, 432}, {0x0A, 0x05020304, 25}, {0x0A, 0x01020304, 24},
    {0x0A, 0x05040302, 435}, {0x0A, 0x01040302, 436}, {0x0A, 0x04030205, 437}, {0x0A, 0x02030405, 438},
    {0x0E, 0x01000002, 398}, {0x0E, 0x01010102, 398}, {0x0F, 0x01000002, 400}, {0x0F, 0x01010102, 400},
    {0x12, 0x01000002, 399}, {0x12, 0x01010102, 399}, {0x13, 0x01000002, 401}, {0x13, 0x01010102, 401},
    {0x15, 0x02000000, 28}, {0x16, 0x01020202, 50}, {0x17, 0x00020202, 41}, {0x18, 0x01000302, 388},
    {0x18, 0x01010302, 388}, {0x19, 0x01000302, 390}, {0x19, 0x01010302, 390}, {0x1C, 0x01000302, 389},
    {0x1C, 0x01010302, 389}, {0x1D, 0x01000302, 391}, {0x1D, 0x01010302, 391}, {0x1F, 0x03020202, 51},
    {0x20, 0x05040302, 32}, {0x20, 0x01040302, 33}, {0x20, 0x05020304, 21}, {0x20, 0x01020304, 22},
    {0x20, 0x04030205, 439}, {0x20, 0x02030405, 440}, {0x21, 0x05040302, 374}, {0x21, 0x01040302, 441},
    {0x21, 0x05020304, 433}, {0x21, 0x01020304, 442}, {0x21, 0x04030205, 443}, {0x21, 0x02030405, 444},
    {0x24, 0x05040302, 373}, {0x24, 0x01040302, 445}, {0x24, 0x05020304, 446}, {0x24, 0x01020304, 447},
    {0x24, 0x04030205, 448}, {0x24, 0x02030405, 449}, {0x25, 0x05040302, 375}, {0x25, 0x01040302, 450},
    {0x25, 0x05020304, 451}, {0x25, 0x01020304, 452}, {0x25, 0x04030205, 453}, {0x25, 0x02030405, 454},
    {0x26, 0x05040302, 372}, {0x26, 0x01040302, 455}, {0x26, 0x05020304, 420}, {0x26, 0x01020304, 422},
    {0x27, 0x01040302, 62}, {0x29, 0x01040302, 370}, {0x2A, 0x05040302, 31}, {0x2A, 0x01040302, 456},
    {0x2A, 0x05020304, 35}, {0x2A, 0x01020304, 457}, {0x2A, 0x04030205, 458}, {0x2A, 0x02030405, 459},
    {0x2B, 0x05040302, 460}, {0x2B, 0x01040302, 461}, {0x2B, 0x05020304, 462}, {0x2B, 0x01020304, 463},
    {0x2B, 0x04030205, 464}, {0x2B, 0x02030405, 465}, {0x2E, 0x05040302, 369}, {0x2E, 0x01040302, 466},
    {0x2E, 0x05020304, 467}, {0x2E, 0x01020304, 468}, {0x2E, 0x04030205, 469}, {0x2E, 0x02030405, 470},
    {0x2F, 0x05040302, 471}, {0x2F, 0x01040302, 472}, {0x2F, 0x05020304, 473}, {0x2F, 0x01020304, 474},
    {0x2F, 0x04030205, 475}, {0x2F, 0x02030405, 476}, {0x30, 0x05040302, 119}, {0x31, 0x05040302, 67},
    {0x32, 0x01000002, 393}, {0x32, 0x01010102, 393}, {0x33, 0x01000002, 395}, {0x33, 0x01010102, 395},
    {0x36, 0x01000002, 394}, {0x36, 0x01010102, 394}, {0x37, 0x01000002, 396}, {0x37, 0x01010102, 396},
    {0x38, 0x01000002, 111}, {0x38, 0x01010102, 111}, {0x39, 0x01020202, 81}, {0x3A, 0x01000302, 34},
    {0x3A, 0x01010302, 34}, {0x3B, 0x01000302, 378}, {0x3B, 0x01010302, 378}, {0x3E, 0x01000302, 377},
    {0x3E, 0x01010302, 377}, {0x3F, 0x01000302, 377}, {0x3F, 0x01010302, 377}, {0x40, 0x01000302, 112},
    {0x40, 0x01010302, 112}, {0x41, 0x05040302, 36}, {0x41, 0x01040302, 477}, {0x41, 0x05020304, 478},
    {0x41, 0x01020304, 479}, {0x42, 0x05040302, 359}, {0x42, 0x01040302, 480}, {0x42, 0x05020304, 481},
    {0x42, 0x01020304, 482}, {0x45, 0x05040302, 358}, {0x45, 0x01040302, 483}, {0x45, 0x05020304, 484},
    {0x45, 0x01020304, 485}, {0x46, 0x05040302, 360}, {0x46, 0x01040302, 486}, {0x46, 0x05020304, 487},
    {0x46, 0x01020304, 488}, {0x47, 0x05040302, 113}, {0x47, 0x01040302, 489}, {0x47, 0x05020304, 490},
    {0x47, 0x01020304, 491}, {0x48, 0x01000002, 382}, {0x48, 0x01010102, 382}, {0x49, 0x01000002, 383},
    {0x49, 0x01010102, 383}, {0x4A, 0x01000002, 114}, {0x4A, 0x01010102, 114}, {0x4B, 0x01000302, 362},
    {0x4B, 0x01010302, 362}, {0x4C, 0x01000302, 363}, {0x4C, 0x01010302, 363}, {0x4D, 0x01000302, 115},
    {0x4D, 0x01010302, 115}, {0x4E, 0x01040302, 355}, {0x4F, 0x01040302, 356}, {0x50, 0x01040302, 354},
    {0x51, 0x05040302, 351}, {0x51, 0x01040302, 492}, {0x51, 0x05020304, 493}, {0x51, 0x01020304, 494},
    {0x52, 0x05040302, 352}, {0x52, 0x01040302, 495}, {0x52, 0x05020304, 496}, {0x52, 0x01020304, 497},
    {0x53, 0x05040302, 116}, {0x53, 0x01040302, 498}, {0x53, 0x05020304, 499}, {0x53, 0x01020304, 500},
    {0x54, 0x01000302, 80}, {0x55, 0x01000302, 381}, {0x56, 0x01040302, 403}, {0x57, 0x05040302, 405},
    {0x57, 0x01040302, 405}, {0x58, 0x05040302, 406}, {0x58, 0x01040302, 406}, {0x59, 0x05040302, 408},
    {0x5A, 0x05040302, 409}, {0x5B, 0x05040302, 411}, {0x5C, 0x05040302, 412}, {0x5D, 0x01000002, 414},
    {0x5D, 0x01020202, 414}, {0x5E, 0x01000002, 415}, {0x5E, 0x01020202, 415}, {0x5F, 0x01000302, 417},
    {0x5F, 0x01010203, 843666497}, {0x5F, 0x03020202, 417}, {0x60, 0x01000302, 418}, {0x60, 0x03020202, 418},
    {0x61, 0x01040302, 424}, {0x62, 0x01040302, 425}, {0x63, 0x05040302, 427}, {0x64, 0x05040302, 428},
    {0x99, 0x01040302, 1195525970}, {0x9B, 0x01040302, 1111970375}, {0x9D, 0x05040302, 1448433985},
    {0x9E, 0x01040302, 1498831189}, {0x9F, 0x01040302, 1498831189}, {0xA0, 0x01040302, 844715353},
    {0xA1, 0x01040302, 844715353}, {0xA2, 0x01040302, 842094169}, {0xA3, 0x01040302, 825316942},
    {0xA4, 0x01040302, 842094158}, {0xA5, 0x01040302, 825382478}, {0xA6, 0x01040302, 909193296},
    {0xA7, 0x01040302, 808530000}, {0xA8, 0x01040302, 808530512}, {0xB1, 0x01040302, 942682704},
    {0xBA, 0x01040302, 909193817}, {0xBB, 0x01040302, 808530521}, {0xBC, 0x05040302, 909194329},
    {0xBD, 0x05040302, 808531033},
};

void InitScreenRecord(void* pRecord, size_t bytes, uint32 command, uint32 inputBytes, uint32 outputBytes)
{
    InitPrivateRecord(pRecord, bytes, command);
    PutRecord32(pRecord, 0xc, Version3);
    PutRecord32(pRecord, 0x80, inputBytes);
    PutRecord32(pRecord, 0x84 + inputBytes, outputBytes);
}

EscapeRoute ScreenRoute(Pal::Device* pDevice)
{
    return DeviceEscapeRoute(static_cast<Device*>(pDevice)->OsTransport(), pDevice->GetPlatform());
}

// MetroHash64 (seed 0) of EDID bytes then the 4-byte display index (2e52c30/2e53120/2e52c60).
uint64 ScreenHash(const uint8* pEdid, size_t edidBytes, const uint32& index)
{
    uint64 hash = 0;
    Util::MetroHash64 hasher;
    hasher.Update(pEdid, edidBytes);
    hasher.Update(reinterpret_cast<const uint8*>(&index), sizeof(index));
    hasher.Finalize(reinterpret_cast<uint8*>(&hash));
    return hash;
}

// KMD display timing block (0xe0 bytes): filled through a user pointer by the
// enable command (decoded at 4d0212..4d02f6) and embedded at SetDisplayMode
// record+0x88 (encoded at 356d03..356ddb).
constexpr size_t TimingBytes = 0xe0;

void DecodeTiming(const uint8* pTiming, PrivateDisplayMode* pMode)
{
    pMode->sourceSize.width                      = GetRecord32(pTiming, 0x00);
    pMode->sourceSize.height                     = GetRecord32(pTiming, 0x04);
    pMode->pixelClockInKhz                       = GetRecord32(pTiming, 0x20);
    const uint32 polarity                        = GetRecord32(pTiming, 0x24);
    pMode->horizontalTiming.active               = GetRecord32(pTiming, 0x28);
    pMode->horizontalTiming.blank                = GetRecord32(pTiming, 0x2c);
    pMode->horizontalTiming.syncWidth            = GetRecord32(pTiming, 0x30);
    pMode->horizontalTiming.syncOffset           = GetRecord32(pTiming, 0x34);
    pMode->horizontalTiming.positiveSyncPolarity = ((polarity >> 1) & 1) != 0;
    pMode->verticalTiming.active                 = GetRecord32(pTiming, 0x48);
    pMode->verticalTiming.blank                  = GetRecord32(pTiming, 0x4c);
    pMode->verticalTiming.syncWidth              = GetRecord32(pTiming, 0x50);
    pMode->verticalTiming.syncOffset             = GetRecord32(pTiming, 0x54);
    pMode->verticalTiming.positiveSyncPolarity   = (polarity & 1) != 0;
    pMode->colorDepth                            = static_cast<PrivateDisplayColorDepth>(GetRecord32(pTiming, 0x68));
    pMode->pixelEncoding                         = static_cast<PrivateDisplayPixelEncoding>(GetRecord32(pTiming, 0x6c));
    pMode->scalingEnabled                        = (pTiming[0x80] == 1);
    pMode->destinationSize.width                 = GetRecord32(pTiming, 0x84);
    pMode->destinationSize.height                = GetRecord32(pTiming, 0x88);
    pMode->offset.y                              = static_cast<int32>(GetRecord32(pTiming, 0xa4));
    pMode->offset.x                              = static_cast<int32>(GetRecord32(pTiming, 0xa8));
    pMode->flags.slsTiledLayout                  = GetRecord32(pTiming, 0xc0) & 1;
}

void EncodeTiming(const PrivateDisplayMode& mode, uint8* pTiming)
{
    PutRecord32(pTiming, 0x00, mode.sourceSize.width);
    PutRecord32(pTiming, 0x04, mode.sourceSize.height);
    PutRecord32(pTiming, 0x20, static_cast<uint32>(mode.pixelClockInKhz));
    PutRecord32(pTiming, 0x24, ((mode.horizontalTiming.positiveSyncPolarity ? 1u : 0u) << 1) |
                               (mode.verticalTiming.positiveSyncPolarity ? 1u : 0u));
    PutRecord32(pTiming, 0x28, mode.horizontalTiming.active);
    PutRecord32(pTiming, 0x2c, mode.horizontalTiming.blank);
    PutRecord32(pTiming, 0x30, mode.horizontalTiming.syncWidth);
    PutRecord32(pTiming, 0x34, mode.horizontalTiming.syncOffset);
    PutRecord32(pTiming, 0x48, mode.verticalTiming.active);
    PutRecord32(pTiming, 0x4c, mode.verticalTiming.blank);
    PutRecord32(pTiming, 0x50, mode.verticalTiming.syncWidth);
    PutRecord32(pTiming, 0x54, mode.verticalTiming.syncOffset);
    PutRecord32(pTiming, 0x68, static_cast<uint32>(mode.colorDepth));
    PutRecord32(pTiming, 0x6c, static_cast<uint32>(mode.pixelEncoding));
    pTiming[0x80] = mode.scalingEnabled ? 1 : 0;
    PutRecord32(pTiming, 0x84, mode.destinationSize.width);
    PutRecord32(pTiming, 0x88, mode.destinationSize.height);
    PutRecord32(pTiming, 0xa4, static_cast<uint32>(mode.offset.y));
    PutRecord32(pTiming, 0xa8, static_cast<uint32>(mode.offset.x));
    PutRecord32(pTiming, 0xc0, mode.flags.u32All & 1);
}

// cvttss2si r64 on value*100000.0f, low dword kept (3570f0). Out-of-range and NaN
// produce the integer indefinite 0x8000000000000000, whose low dword is zero.
uint32 ColorFixedPoint(float value)
{
    const float scaled = value * 100000.0f;
    int64 converted = INT64_MIN;
    if ((scaled >= -9223372036854775808.0f) && (scaled < 9223372036854775808.0f))
    {
        converted = static_cast<int64>(scaled);
    }
    return static_cast<uint32>(static_cast<uint64>(converted));
}

// Pal::Fence keeps its state protected; the original ORs bit 2 (privateScreenPresentUsed)
// into Fence+0x10 directly.
struct FenceStateAccess : Pal::Fence
{
    static void MarkPrivateScreenPresentUsed(Pal::Fence* pFence)
    {
        auto state = &FenceStateAccess::m_fenceState;
        (pFence->*state).privateScreenPresentUsed = 1;
    }
};

VOID NTAPI TopologyChanged(PVOID pContext, BOOLEAN timerOrWaitFired)
{
    // Native 314f00. Deviation: the original calls pfnOnTopology without a null check.
    const auto* pNotifyInfo = static_cast<const PrivateScreenNotifyInfo*>(pContext);
    if ((timerOrWaitFired == FALSE) && (pNotifyInfo->pfnOnTopology != nullptr))
    {
        pNotifyInfo->pfnOnTopology(pNotifyInfo->pClient);
    }
}

void NotifyTopology(const PrivateScreenNotifyInfo& notifyInfo)
{
    // Deviation: 313650/313870 call pfnOnTopology without a null check.
    if (notifyInfo.pfnOnTopology != nullptr)
    {
        notifyInfo.pfnOnTopology(notifyInfo.pClient);
    }
}

// 311ed0 for the hot-plug event class; the 0x20-class side effect (Device+0x9698) does not apply.
Result RegisterKmdEvent(const EscapeRoute& route, uint32 type, void* hEvent)
{
    std::array<uint8, 0xe8> record;
    InitScreenRecord(record.data(), record.size(), CmdRegisterEvent, 0x40, 0x20);
    PutRecord32(record.data(), 0x84, type);
    PutRecord64(record.data(), 0x88, reinterpret_cast<uint64>(hEvent));
    PutRecord32(record.data(), 0x90, 0);
    PutRecord32(record.data(), 0x94, 0);
    Result result = SharedEscape(route, record.data(), record.size());
    if ((result == Result::Success) && (GetRecord32(record.data(), 0xc8) != 0))
    {
        result = Result::ErrorUnknown;
    }
    return result;
}

// 311fe0.
Result UnregisterKmdEvent(const EscapeRoute& route, uint32 type, void* hEvent)
{
    std::array<uint8, 0xe8> record;
    InitScreenRecord(record.data(), record.size(), CmdUnregisterEvent, 0x40, 0x20);
    PutRecord32(record.data(), 0x84, type);
    PutRecord32(record.data(), 0x88, 0);
    PutRecord64(record.data(), 0x8c, reinterpret_cast<uint64>(hEvent));
    Result result = SharedEscape(route, record.data(), record.size());
    if ((result == Result::Success) && (GetRecord32(record.data(), 0xc8) != 0))
    {
        result = Result::ErrorUnknown;
    }
    return result;
}
} // anonymous namespace

// =====================================================================================================================
Result PrivateScreenStatusToResult(uint32 status)
{
    switch (status)
    {
    case 0:  return Result::Success;
    case 1:  return Result::ErrorPrivateScreenRemoved;
    case 2:  return Result::OutOfSpec;
    case 3:  return Result::ErrorPrivateScreenInvalidTiming;
    case 4:  return Result::ErrorPrivateScreenInvalidFormat;
    case 5:  return Result::ErrorPrivateScreenInvalidResolution;
    case 6:  return Result::ErrorPrivateScreenInvalidScaling;
    // Deviation: the original indexes its seven-entry stack table without a bound.
    default: return Result::ErrorUnknown;
    }
}

// =====================================================================================================================
uint32 PrivateScreenPresentFormat(SwizzledFormat format)
{
    if (Formats::IsSrgb(format.format))
    {
        format.format = Formats::ConvertToUnorm(format.format);
    }
    for (const KmdFormatEntry& entry : KmdFormatTable)
    {
        if ((entry.format == static_cast<uint32>(format.format)) && (entry.swizzle == format.swizzle.swizzleValue))
        {
            return entry.kmdFormat;
        }
    }
    return 0;
}

// =====================================================================================================================
// Emulating 2cd040 over every code it distinguishes (0..0x3ff and its FOURCC cases) shows these are the only codes
// that convert to one of the eight private-screen formats.
bool PrivateScreenFormatIndex(uint32 kmdFormat, uint32* pIndex)
{
    switch (kmdFormat)
    {
    case 23:  *pIndex = 0; return true; // R5G6B5
    case 32:  *pIndex = 1; return true; // A8B8G8R8
    case 372: *pIndex = 2; return true;
    case 21:  *pIndex = 3; return true; // A8R8G8B8
    case 419:
    case 420: *pIndex = 4; return true;
    case 31:  *pIndex = 5; return true; // A2B10G10R10
    case 35:  *pIndex = 6; return true; // A2R10G10B10
    case 113: *pIndex = 7; return true; // A16B16G16R16F
    default:  return false;
    }
}

// =====================================================================================================================
PrivateScreenNotifier::~PrivateScreenNotifier()
{
    if (m_event != nullptr)
    {
        CloseHandle(m_event);
        m_event = nullptr;
    }
}

// =====================================================================================================================
Result PrivateScreenNotifier::Start(const PrivateScreenNotifyInfo* pNotifyInfo)
{
    if (m_event == nullptr)
    {
        // The original passes a zeroed SECURITY_ATTRIBUTES (not null): auto-reset, unsignaled, unnamed.
        SECURITY_ATTRIBUTES attributes = {};
        m_event = CreateEventW(&attributes, FALSE, FALSE, nullptr);
        if (m_event == nullptr)
        {
            return Result::ErrorInitializationFailed;
        }
    }
    // Deviation: a second Finalize without Cleanup would make the original register a second wait on the same
    // event and leak it past the event's CloseHandle; keep the existing registration instead.
    if (m_wait != nullptr)
    {
        return Result::Success;
    }
    HANDLE wait = nullptr;
    if (RegisterWaitForSingleObject(&wait, m_event, &TopologyChanged, const_cast<PrivateScreenNotifyInfo*>(pNotifyInfo),
                                    INFINITE, WT_EXECUTEDEFAULT) == FALSE)
    {
        return Result::ErrorInitializationFailed;
    }
    m_wait = wait;
    return Result::Success;
}

// =====================================================================================================================
Result PrivateScreenNotifier::Stop(const EscapeRoute& route)
{
    Result result = Result::Success;
    if (m_wait != nullptr)
    {
        // Deviation: the original's non-blocking UnregisterWait fails (ERROR_IO_PENDING) while the callback runs and
        // lets it touch a device that is about to be destroyed; wait for it instead.
        const BOOL unregistered = UnregisterWaitEx(m_wait, INVALID_HANDLE_VALUE);
        m_wait = nullptr;
        if (unregistered == FALSE)
        {
            result = Result::ErrorUnknown;
        }
    }
    if ((result == Result::Success) && m_kmdRegistered)
    {
        // The original discards this result.
        UnregisterKmdEvent(route, PrivateScreenEventType, m_event);
        m_kmdRegistered = false;
    }
    return result;
}

// =====================================================================================================================
Result PrivateScreenNotifier::RegisterWithKmd(const EscapeRoute& route)
{
    Result result = Result::Success;
    if (m_kmdRegistered == false)
    {
        result = RegisterKmdEvent(route, PrivateScreenEventType, m_event);
        m_kmdRegistered = (result == Result::Success);
    }
    return result;
}

// =====================================================================================================================
Result EnumeratePrivateScreens(
    const EscapeRoute&       route,
    Pal::Platform*           pPlatform,
    PrivateScreenCreateInfo* pInfos,
    PrivateScreenNotifier*   pNotifier,
    uint32*                  pNumScreens)
{
    // Never null from Pal::Device::GetPrivateScreens; the original would fault after the first escape.
    if ((pNumScreens == nullptr) || (pInfos == nullptr) || (pNotifier == nullptr))
    {
        return Result::ErrorInvalidPointer;
    }

    std::array<uint8, 0x188> list;
    InitScreenRecord(list.data(), list.size(), CmdListScreens, 0x20, 0xe0);
    Result result = SharedEscape(route, list.data(), list.size());
    if (result != Result::Success)
    {
        return result;
    }

    // Deviation: the reply holds six 32-byte entries; the original trusts any count and overruns both the reply and
    // m_privateScreenInfo (and GetPrivateScreens' per-screen arrays) beyond MaxPrivateScreens.
    const uint32 count = Util::Min(GetRecord32(list.data(), 0xac), MaxPrivateScreens);
    *pNumScreens = count;

    for (uint32 i = 0; i < count; ++i)
    {
        const uint8* pEntry = list.data() + 0xb0 + (0x20 * i);
        PrivateScreenCreateInfo& info = pInfos[i];
        info.index            = GetRecord32(pEntry, 0x0);
        info.props.numFormats = GetRecord32(pEntry, 0x4);
        info.props.targetId   = GetRecord32(pEntry, 0x8);
        info.props.type       = (pEntry[0xc] == 0) ? PrivateScreenType::Temporary : PrivateScreenType::Permanent;

        // The KMD fills this list too, but only OsInitProperties consumes formats.
        Util::AutoBuffer<uint32, 16, Pal::Platform> formats(info.props.numFormats, pPlatform);

        std::array<uint8, 0x328> query;
        InitScreenRecord(query.data(), query.size(), CmdScreenInfo, 0x20, 0x280);
        PutRecord32(query.data(), 0x84, info.index);
        PutRecord64(query.data(), 0xb4, reinterpret_cast<uint64>(formats.Data()));
        // Only the last screen's escape result survives the loop, as in the original.
        result = SharedEscape(route, query.data(), query.size());
        if (result == Result::Success)
        {
            constexpr size_t EdidOffset = 0xc4;
            const uint32 edidSize = GetRecord32(query.data(), 0x2c4);
            // Deviation: the original hashes edidSize bytes even past the end of its reply record.
            info.props.hash = ScreenHash(query.data() + EdidOffset,
                                         Util::Min<size_t>(edidSize, query.size() - EdidOffset), info.index);
            info.props.refreshRate.numerator   = GetRecord32(query.data(), 0xbc);
            info.props.refreshRate.denominator = GetRecord32(query.data(), 0xc0);
            info.props.extent.width            = GetRecord32(query.data(), 0xac);
            info.props.extent.height           = GetRecord32(query.data(), 0xb0);
            info.props.edidSize                = edidSize;
            info.props.maxNumPowerSwitches     = GetRecord32(query.data(), 0x2c8);
            info.props.powerSwitchLatency      = GetRecord32(query.data(), 0x2cc);
            info.props.caps.hasAudio           = GetRecord32(query.data(), 0x2d0) & 1;
            std::memcpy(info.props.edid, query.data() + EdidOffset, Util::Min<size_t>(edidSize, MaxEdidSize));
        }
    }

    if (result == Result::Success)
    {
        result = pNotifier->RegisterWithKmd(route);
    }
    return result;
}

// =====================================================================================================================
Result AddEmulatedScreen(
    Pal::Device*                      pDevice,
    const EmulatedPrivateScreenSlots& slots,
    const PrivateScreenNotifyInfo&    notifyInfo,
    const PrivateScreenCreateInfo&    createInfo,
    uint32*                           pTargetId)
{
    // The original fills the slot indexed by the current count, not the first free slot.
    const uint32 slot = *slots.pCount;
    if (slot >= MaxPrivateScreens)
    {
        return Result::ErrorTooManyPrivateScreens;
    }
    // Deviation: the original writes *pTargetId unconditionally after creating the screen.
    if (pTargetId == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }

    PrivateScreenCreateInfo info;
    std::memcpy(&info, &createInfo, sizeof(info));
    info.index = *slots.pNextTargetId;
    constexpr size_t EdidOffset = offsetof(PrivateScreenCreateInfo, props) + offsetof(PrivateScreenProperties, edid);
    // Deviation: the original hashes the caller's edidSize bytes even past the end of its local copy.
    info.props.hash = ScreenHash(reinterpret_cast<const uint8*>(&info) + EdidOffset,
                                 Util::Min<size_t>(info.props.edidSize, sizeof(info) - EdidOffset), info.index);
    info.props.targetId   = info.index;
    info.props.type       = PrivateScreenType::Emulated;
    info.props.numFormats = MaxPrivateScreenFormats;

    void* pMemory = PAL_MALLOC_ALIGNED(ScreenAllocationBytes, 8, pDevice->GetPlatform(), Util::AllocInternal);
    if (pMemory == nullptr)
    {
        return Result::ErrorOutOfMemory;
    }
    info.props.pFormats = reinterpret_cast<SwizzledFormat*>(static_cast<uint8*>(pMemory) + sizeof(PrivateScreen));
    PrivateScreen* pScreen = PAL_PLACEMENT_NEW(pMemory) PrivateScreen(pDevice, info);
    slots.ppScreens[slot] = pScreen;
    pScreen->InitEmulated(); // Inlined by the original; always Success.

    *pTargetId = info.index;
    ++(*slots.pCount);
    --(*slots.pNextTargetId);
    NotifyTopology(notifyInfo);
    return Result::Success;
}

// =====================================================================================================================
Result RemoveEmulatedScreen(
    Pal::Platform*                    pPlatform,
    const EmulatedPrivateScreenSlots& slots,
    const PrivateScreenNotifyInfo&    notifyInfo,
    uint32                            targetId)
{
    uint32 slot = 0;
    for (; slot < MaxPrivateScreens; ++slot)
    {
        // Deviation: the original dereferences empty slots while searching.
        if ((slots.ppScreens[slot] != nullptr) && (slots.ppScreens[slot]->Index() == targetId))
        {
            break;
        }
    }
    if (slot == MaxPrivateScreens)
    {
        return Result::ErrorUnavailable;
    }

    --(*slots.pCount);
    PrivateScreen* pScreen = slots.ppScreens[slot];
    const Result result = pScreen->Disable();
    pScreen->~PrivateScreen();
    PAL_FREE(pScreen, pPlatform);
    slots.ppScreens[slot] = nullptr;
    NotifyTopology(notifyInfo);
    return result;
}
} // Pal::Wddm

namespace Pal
{
using namespace Wddm;

// =====================================================================================================================
// Native 4cfe80 (InitPhysical with this inlined). Formats come from the KMD as D3DDDIFORMAT codes.
Result PrivateScreen::OsInitProperties()
{
    Pal::Platform* pPlatform = m_pDevice->GetPlatform();
    const uint32 kmdFormats = m_properties.numFormats;
    auto* pCodes = static_cast<uint32*>(PAL_MALLOC_ALIGNED(sizeof(uint32) * kmdFormats, alignof(uint32), pPlatform,
                                                           Util::AllocInternalTemp));
    if (pCodes == nullptr)
    {
        return Result::ErrorOutOfMemory;
    }

    std::array<uint8, 0x328> record;
    InitScreenRecord(record.data(), record.size(), CmdScreenInfo, 0x20, 0x280);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord64(record.data(), 0xb4, reinterpret_cast<uint64>(pCodes));
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    if (result == Result::Success)
    {
        for (uint32 i = 0; i < kmdFormats; ++i)
        {
            const uint32 code = pCodes[i];
            uint32 index = 0;
            if ((code == 0x77) || (code == 0x21) || (code == 0x29) || (PrivateScreenFormatIndex(code, &index) == false))
            {
                continue;
            }
            // UpdateSupportedFormats, inlined by the original. The sRGB companion is appended even when an earlier
            // sRGB code already added it (a duplicate entry), as the original does.
            // Deviation: the original keeps appending past the eight-entry format storage when duplicates push the
            // count beyond it; entries that do not fit are dropped.
            const auto append = [this](uint32 formatIndex)
            {
                if (m_reportedFormats < MaxPrivateScreenFormats)
                {
                    m_properties.pFormats[m_reportedFormats++] = PrivateFormats[formatIndex];
                }
            };
            const uint32 bit = 1u << index;
            if ((m_formatCaps & bit) == 0)
            {
                m_formatCaps |= bit;
                append(index);
                if (PrivateFormats[index].format == ChNumFormat::X8Y8Z8W8_Unorm)
                {
                    m_formatCaps |= bit << 1;
                    append(index + 1);
                }
            }
        }
        FinalizeSupportedFormats();
    }
    PAL_FREE(pCodes, pPlatform);
    return result;
}

// =====================================================================================================================
// Inlined in 356610.
Result PrivateScreen::OsGetPresentStats(
    PrivateScreenPresentStats* pStats)
{
    std::array<uint8, 0x108> record;
    InitScreenRecord(record.data(), record.size(), CmdPresentStats, 0x20, 0x60);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord64(record.data(), 0x88, m_dummyHandle);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xd0));
    }
    if (result == Result::Success)
    {
        pStats->presentCount          = GetRecord64(record.data(), 0xa8);
        pStats->presentCpuTime        = GetRecord64(record.data(), 0xb0);
        pStats->vsyncCount            = GetRecord64(record.data(), 0xb8);
        pStats->vsyncCpuTime          = GetRecord64(record.data(), 0xc0);
        pStats->displayLatchedCpuTime = GetRecord64(record.data(), 0xc8);
        pStats->vsyncGpuTime          = GetRecord64(record.data(), 0xd8);
    }
    return result;
}

// =====================================================================================================================
// Native 4d00f0. Only flags bit 0 (vsyncAlwaysOn) is sent; disablePowerManagement is not.
Result PrivateScreen::OsEnable(
    const PrivateScreenEnableInfo& enableInfo)
{
    std::array<uint8, 0xc8> record;
    InitScreenRecord(record.data(), record.size(), CmdEnable, 0x20, 0x20);
    alignas(8) uint8 timing[TimingBytes] = {};
    const bool emulated = (m_properties.type == PrivateScreenType::Emulated);
    uint32 flags = enableInfo.flags.u32All & 1;
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord32(record.data(), 0x88, 0);
    if (emulated)
    {
        // Deviation: the original divides by the refresh numerator without a check (#DE on zero).
        if (m_properties.refreshRate.numerator == 0)
        {
            return Result::ErrorInvalidValue;
        }
        flags |= 2;
        // Frame period in microseconds, with the original's 32-bit product.
        PutRecord32(record.data(), 0x90,
                    static_cast<uint32>(m_properties.refreshRate.denominator * 1000000u) /
                    m_properties.refreshRate.numerator);
    }
    else
    {
        PutRecord64(record.data(), 0xac, reinterpret_cast<uint64>(&timing[0]));
    }
    PutRecord32(record.data(), 0x8c, flags);

    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    if (result == Result::Success)
    {
        if (emulated)
        {
            m_dummyHandle = GetRecord64(record.data(), 0xac);
        }
        else
        {
            DecodeTiming(timing, &m_displayMode);
        }
    }
    return result;
}

// =====================================================================================================================
// Inlined in 356800. The present format and dummy handle are reset whatever the KMD answers.
Result PrivateScreen::OsDisable()
{
    std::array<uint8, 0xc8> record;
    InitScreenRecord(record.data(), record.size(), CmdDisable, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord64(record.data(), 0x88, m_dummyHandle);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    m_lastPresentFormat = 0;
    m_dummyHandle       = 0;
    return result;
}

// =====================================================================================================================
// Inlined in 356940.
Result PrivateScreen::OsBlank()
{
    std::array<uint8, 0xc8> record;
    InitScreenRecord(record.data(), record.size(), CmdBlank, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    return result;
}

// =====================================================================================================================
// Native 4d0330. The KMD already knows each image id's flip address; the present
// names the id, the D3DDDIFORMAT when it changed, and the fence event to signal.
Result PrivateScreen::OsPresent(
    const PrivateScreenPresentInfo& presentInfo)
{
    const auto* pImage = static_cast<const Pal::Image*>(presentInfo.pSrcImg);
    std::array<uint8, 0xe8> record;
    InitScreenRecord(record.data(), record.size(), CmdPresent, 0x40, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord64(record.data(), 0x9c, m_dummyHandle);
    const uint32 vsync = presentInfo.vsync ? 1 : 0;
    PutRecord32(record.data(), 0x94, vsync);
    PutRecord32(record.data(), 0x88, pImage->GetPrivateScreenImageId());

    const uint32 kmdFormat = PrivateScreenPresentFormat(pImage->GetImageCreateInfo().swizzledFormat);
    const bool formatChanged = (static_cast<uint32>(m_lastPresentFormat) != kmdFormat);
    if (formatChanged)
    {
        PutRecord32(record.data(), 0x98, kmdFormat);
        PutRecord32(record.data(), 0x94, vsync + 2);
    }
    if (presentInfo.pPresentDoneFence != nullptr)
    {
        auto* pFence = static_cast<Pal::Fence*>(presentInfo.pPresentDoneFence);
        FenceStateAccess::MarkPrivateScreenPresentUsed(pFence);
        PutRecord64(record.data(), 0x8c, reinterpret_cast<uint64>(static_cast<Wddm::Fence*>(pFence)->NativeEvent()));
    }

    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xc8));
    }
    if (formatChanged && (result == Result::Success))
    {
        m_lastPresentFormat = static_cast<int32>(kmdFormat);
    }
    return result;
}

// =====================================================================================================================
// Inlined in 356ad0. A nonzero in-vblank byte reports -1.
Result PrivateScreen::OsGetScanLine(
    int32* pScanLine)
{
    // Deviation: the original writes through pScanLine without a check (palPrivateScreen.h documents this error).
    if (pScanLine == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }
    std::array<uint8, 0xc8> record;
    InitScreenRecord(record.data(), record.size(), CmdScanLine, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xb0));
    }
    if (result == Result::Success)
    {
        *pScanLine = (record[0xac] != 0) ? -1 : static_cast<int32>(GetRecord32(record.data(), 0xa8));
    }
    return result;
}

// =====================================================================================================================
// Native 4d05c0. The bridge capacity is the count reported by the previous call; a
// null pConnectorProperties resets it. connectorType is never written.
Result PrivateScreen::OsGetConnectorProperties(
    uint32*                           pDataSize,
    PrivateScreenConnectorProperties* pConnectorProperties)
{
    // Deviation: the original writes *pDataSize without a check.
    if (pDataSize == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }
    if (pConnectorProperties == nullptr)
    {
        m_bridgeCount = 0;
    }
    const uint32 capacity = m_bridgeCount;
    const uint64 slots = Util::Max(capacity, 1u);
    const uint64 recordBytes = 0xc8 + (0x20 * slots);
    // Deviation: the original computes these sizes in 32 bits; a KMD count that would wrap them is refused.
    if ((recordBytes + 0x4c) > UINT32_MAX)
    {
        return Result::ErrorOutOfMemory;
    }

    Pal::Platform* pPlatform = m_pDevice->GetPlatform();
    auto* pRecord = static_cast<uint8*>(PAL_MALLOC_ALIGNED(static_cast<size_t>(recordBytes), 8, pPlatform,
                                                           Util::AllocInternalTemp));
    if (pRecord == nullptr)
    {
        return Result::ErrorOutOfMemory;
    }
    // Deviation: the original leaves the rest of this heap record uninitialized; it is zeroed here.
    InitScreenRecord(pRecord, static_cast<size_t>(recordBytes), CmdConnector, 0x20, static_cast<uint32>(0x20 + (0x20 * slots)));
    PutRecord32(pRecord, 0x84, m_index);
    PutRecord32(pRecord, 0x88, capacity);

    Result result = SharedEscape(ScreenRoute(m_pDevice), pRecord, static_cast<size_t>(recordBytes));
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(pRecord, 0xac));
    }
    if (result == Result::Success)
    {
        if (m_bridgeCount != 0)
        {
            // The caller's buffer was sized from the previous *pDataSize: the bridges follow the structure.
            auto* pBridges = reinterpret_cast<PrivateScreenBridge*>(pConnectorProperties + 1);
            pConnectorProperties->pBridge = pBridges;
            for (uint32 i = 0; i < m_bridgeCount; ++i)
            {
                pBridges[i].bridgeType = GetRecord32(pRecord, 0xc8 + (0x20 * i));
                std::memcpy(pBridges[i].id, pRecord + 0xcc + (0x20 * i), MaxDisplayBridgeIdSize);
            }
        }
        const uint32 bridgeCount = GetRecord32(pRecord, 0xa8);
        if (pConnectorProperties != nullptr)
        {
            pConnectorProperties->bridgeCount = bridgeCount;
        }
        m_bridgeCount = bridgeCount;
        *pDataSize    = (bridgeCount * static_cast<uint32>(sizeof(PrivateScreenBridge))) +
                        static_cast<uint32>(sizeof(PrivateScreenConnectorProperties));
    }
    PAL_FREE(pRecord, pPlatform);
    return result;
}

// =====================================================================================================================
// Inlined in 356eb0. The KMD reads the caller's ramp through this pointer; null restores its default.
Result PrivateScreen::OsSetGammaRamp(
    const GammaRamp* pGammaRamp)
{
    std::array<uint8, 0xc8> record;
    InitScreenRecord(record.data(), record.size(), CmdSetGammaRamp, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord64(record.data(), 0x88, reinterpret_cast<uint64>(pGammaRamp));
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    return result;
}

// =====================================================================================================================
// Inlined in 3570f0: a 3x3 matrix then three offsets, each a fraction n/100000.
Result PrivateScreen::OsSetColorMatrix(
    const ColorTransform& matrix)
{
    std::array<uint8, 0x128> record;
    InitScreenRecord(record.data(), record.size(), CmdSetColorMatrix, 0x80, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    // matrix[] index -> numerator offset; every denominator follows at +4.
    constexpr uint32 Offsets[12] = { 0x88, 0x90, 0x98, 0xd0, 0xa0, 0xa8, 0xb0, 0xd8, 0xb8, 0xc0, 0xc8, 0xe0 };
    for (uint32 i = 0; i < 12; ++i)
    {
        PutRecord32(record.data(), Offsets[i], ColorFixedPoint(matrix.matrix[i]));
        PutRecord32(record.data(), Offsets[i] + 4, 100000);
    }
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0x108));
    }
    return result;
}

// =====================================================================================================================
// Inlined in 356fd0.
Result PrivateScreen::OsSetPowerMode(
    PrivateDisplayPowerState powerMode)
{
    std::array<uint8, 0xc8> record;
    InitScreenRecord(record.data(), record.size(), CmdSetPowerMode, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord32(record.data(), 0x88, static_cast<uint32>(powerMode));
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    return result;
}

// =====================================================================================================================
// Inlined in 356ca0. The core caches the mode only on Success (OutOfSpec is not cached).
Result PrivateScreen::OsSetDisplayMode(
    const PrivateDisplayMode& displayMode)
{
    std::array<uint8, 0x1a8> record;
    InitScreenRecord(record.data(), record.size(), CmdSetDisplayMode, 0x100, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    EncodeTiming(displayMode, record.data() + 0x88);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0x188));
    }
    return result;
}

// =====================================================================================================================
// Inlined in 357340.
Result PrivateScreen::OsSetEventAfterVsync(
    OsExternalHandle hEvent,
    uint32           delayInUs,
    bool             repeated)
{
    std::array<uint8, 0xe8> record;
    InitScreenRecord(record.data(), record.size(), CmdEventAfterVsync, 0x40, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord32(record.data(), 0x88, 0);
    PutRecord64(record.data(), 0x8c, reinterpret_cast<uint64>(hEvent));
    PutRecord32(record.data(), 0x94, repeated ? 1 : 0);
    PutRecord32(record.data(), 0x98, delayInUs);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xc8));
    }
    return result;
}

// =====================================================================================================================
// Inlined in 357480. Flags (low four bits) are written only while HDCP is active.
Result PrivateScreen::OsGetHdcpStatus(
    HdcpStatus* pStatus)
{
    // Deviation: the original writes through pStatus without a check.
    if (pStatus == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }
    std::array<uint8, 0xe8> record;
    InitScreenRecord(record.data(), record.size(), CmdHdcpStatus, 0x20, 0x40);
    PutRecord32(record.data(), 0x84, m_index);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    if (result == Result::Success)
    {
        pStatus->active = (GetRecord32(record.data(), 0xb0) == 1);
        if (pStatus->active)
        {
            pStatus->flags.u32All = (pStatus->flags.u32All & ~0xfu) | (GetRecord32(record.data(), 0xac) & 0xf);
        }
    }
    return result;
}

// =====================================================================================================================
// Inlined in 3575c0.
Result PrivateScreen::OsEnableAudio(
    bool enable)
{
    std::array<uint8, 0xc8> record;
    InitScreenRecord(record.data(), record.size(), CmdEnableAudio, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, m_index);
    PutRecord32(record.data(), 0x88, enable ? 1 : 0);
    Result result = SharedEscape(ScreenRoute(m_pDevice), record.data(), record.size());
    if (result == Result::Success)
    {
        result = PrivateScreenStatusToResult(GetRecord32(record.data(), 0xa8));
    }
    return result;
}

} // Pal
