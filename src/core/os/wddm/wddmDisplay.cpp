// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmDisplay.h"
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmProperties.h"
#include "palFormatInfo.h"
#include "palInlineFuncs.h"
#include "palSysMemory.h"
#include <cstring>

namespace Pal::Wddm
{
namespace
{
constexpr size_t SmallRecordBytes = 0x108;
constexpr size_t EnvelopeBytes = 0x4c;
constexpr size_t StereoRowBase = 0x108;
constexpr size_t StereoRowBytes = 64;
constexpr uint32 StereoPageRows = 256;
// Largest list whose record plus the shared envelope still fits D3DKMT_ESCAPE::PrivateDriverDataSize.
constexpr uint32 MaxStereoRows = uint32((0xffffffffull - SmallRecordBytes - EnvelopeBytes) / StereoRowBytes);
constexpr uint32 SwizzleBgr1 = 0x01020304; // {Z, Y, X, One}
constexpr uint32 SwizzleBgra = 0x05020304; // {Z, Y, X, W}

// Effective entries of the original (format, swizzle) -> D3DDDIFORMAT table at 5838fc0 (six pairs per format).
// The first matching pair wins; pairs whose result is 0 or 0x1ad..0x1b1 behave like no match and are omitted.
struct PrimaryDdiFormat
{
    ChNumFormat format;
    uint32 swizzle;
    uint32 ddiFormat;
};
constexpr PrimaryDdiFormat PrimaryDdiFormats[] =
{
    {ChNumFormat::X1_Unorm, 0x01000002, 0x192},
    {ChNumFormat::L4A4_Unorm, 0x03020202, 0x034},
    {ChNumFormat::X4Y4Z4W4_Unorm, 0x05020304, 0x01a},
    {ChNumFormat::X4Y4Z4W4_Unorm, 0x01040302, 0x1b2},
    {ChNumFormat::X4Y4Z4W4_Unorm, 0x01020304, 0x01e},
    {ChNumFormat::X5Y6Z5_Unorm, 0x01020304, 0x017},
    {ChNumFormat::X5Y5Z5W1_Unorm, 0x05020304, 0x019},
    {ChNumFormat::X5Y5Z5W1_Unorm, 0x01020304, 0x018},
    {ChNumFormat::X5Y5Z5W1_Unorm, 0x05040302, 0x1b3},
    {ChNumFormat::X5Y5Z5W1_Unorm, 0x01040302, 0x1b4},
    {ChNumFormat::X5Y5Z5W1_Unorm, 0x04030205, 0x1b5},
    {ChNumFormat::X5Y5Z5W1_Unorm, 0x02030405, 0x1b6},
    {ChNumFormat::X8_Unorm, 0x01000002, 0x18e},
    {ChNumFormat::X8_Unorm, 0x01010102, 0x18e},
    {ChNumFormat::X8_Snorm, 0x01000002, 0x190},
    {ChNumFormat::X8_Snorm, 0x01010102, 0x190},
    {ChNumFormat::X8_Uint, 0x01000002, 0x18f},
    {ChNumFormat::X8_Uint, 0x01010102, 0x18f},
    {ChNumFormat::X8_Sint, 0x01000002, 0x191},
    {ChNumFormat::X8_Sint, 0x01010102, 0x191},
    {ChNumFormat::A8_Unorm, 0x02000000, 0x01c},
    {ChNumFormat::L8_Unorm, 0x01020202, 0x032},
    {ChNumFormat::P8_Unorm, 0x00020202, 0x029},
    {ChNumFormat::X8Y8_Unorm, 0x01000302, 0x184},
    {ChNumFormat::X8Y8_Unorm, 0x01010302, 0x184},
    {ChNumFormat::X8Y8_Snorm, 0x01000302, 0x186},
    {ChNumFormat::X8Y8_Snorm, 0x01010302, 0x186},
    {ChNumFormat::X8Y8_Uint, 0x01000302, 0x185},
    {ChNumFormat::X8Y8_Uint, 0x01010302, 0x185},
    {ChNumFormat::X8Y8_Sint, 0x01000302, 0x187},
    {ChNumFormat::X8Y8_Sint, 0x01010302, 0x187},
    {ChNumFormat::L8A8_Unorm, 0x03020202, 0x033},
    {ChNumFormat::X8Y8Z8W8_Unorm, 0x05040302, 0x020},
    {ChNumFormat::X8Y8Z8W8_Unorm, 0x01040302, 0x021},
    {ChNumFormat::X8Y8Z8W8_Unorm, 0x05020304, 0x015},
    {ChNumFormat::X8Y8Z8W8_Unorm, 0x01020304, 0x016},
    {ChNumFormat::X8Y8Z8W8_Unorm, 0x04030205, 0x1b7},
    {ChNumFormat::X8Y8Z8W8_Unorm, 0x02030405, 0x1b8},
    {ChNumFormat::X8Y8Z8W8_Snorm, 0x05040302, 0x176},
    {ChNumFormat::X8Y8Z8W8_Snorm, 0x01040302, 0x1b9},
    {ChNumFormat::X8Y8Z8W8_Snorm, 0x01020304, 0x1ba},
    {ChNumFormat::X8Y8Z8W8_Snorm, 0x04030205, 0x1bb},
    {ChNumFormat::X8Y8Z8W8_Snorm, 0x02030405, 0x1bc},
    {ChNumFormat::X8Y8Z8W8_Uint, 0x05040302, 0x175},
    {ChNumFormat::X8Y8Z8W8_Uint, 0x01040302, 0x1bd},
    {ChNumFormat::X8Y8Z8W8_Uint, 0x05020304, 0x1be},
    {ChNumFormat::X8Y8Z8W8_Uint, 0x01020304, 0x1bf},
    {ChNumFormat::X8Y8Z8W8_Uint, 0x04030205, 0x1c0},
    {ChNumFormat::X8Y8Z8W8_Uint, 0x02030405, 0x1c1},
    {ChNumFormat::X8Y8Z8W8_Sint, 0x05040302, 0x177},
    {ChNumFormat::X8Y8Z8W8_Sint, 0x01040302, 0x1c2},
    {ChNumFormat::X8Y8Z8W8_Sint, 0x05020304, 0x1c3},
    {ChNumFormat::X8Y8Z8W8_Sint, 0x01020304, 0x1c4},
    {ChNumFormat::X8Y8Z8W8_Sint, 0x04030205, 0x1c5},
    {ChNumFormat::X8Y8Z8W8_Sint, 0x02030405, 0x1c6},
    {ChNumFormat::X8Y8Z8W8_Srgb, 0x05040302, 0x174},
    {ChNumFormat::X8Y8Z8W8_Srgb, 0x01040302, 0x1c7},
    {ChNumFormat::X8Y8Z8W8_Srgb, 0x05020304, 0x1a4},
    {ChNumFormat::X8Y8Z8W8_Srgb, 0x01020304, 0x1a6},
    {ChNumFormat::U8V8_Snorm_L8W8_Unorm, 0x01040302, 0x03e},
    {ChNumFormat::X11Y11Z10_Float, 0x01040302, 0x172},
    {ChNumFormat::X10Y10Z10W2_Unorm, 0x05040302, 0x01f},
    {ChNumFormat::X10Y10Z10W2_Unorm, 0x01040302, 0x1c8},
    {ChNumFormat::X10Y10Z10W2_Unorm, 0x05020304, 0x023},
    {ChNumFormat::X10Y10Z10W2_Unorm, 0x01020304, 0x1c9},
    {ChNumFormat::X10Y10Z10W2_Unorm, 0x04030205, 0x1ca},
    {ChNumFormat::X10Y10Z10W2_Unorm, 0x02030405, 0x1cb},
    {ChNumFormat::X10Y10Z10W2_Snorm, 0x05040302, 0x1cc},
    {ChNumFormat::X10Y10Z10W2_Snorm, 0x01040302, 0x1cd},
    {ChNumFormat::X10Y10Z10W2_Snorm, 0x05020304, 0x1ce},
    {ChNumFormat::X10Y10Z10W2_Snorm, 0x01020304, 0x1cf},
    {ChNumFormat::X10Y10Z10W2_Snorm, 0x04030205, 0x1d0},
    {ChNumFormat::X10Y10Z10W2_Snorm, 0x02030405, 0x1d1},
    {ChNumFormat::X10Y10Z10W2_Uint, 0x05040302, 0x171},
    {ChNumFormat::X10Y10Z10W2_Uint, 0x01040302, 0x1d2},
    {ChNumFormat::X10Y10Z10W2_Uint, 0x05020304, 0x1d3},
    {ChNumFormat::X10Y10Z10W2_Uint, 0x01020304, 0x1d4},
    {ChNumFormat::X10Y10Z10W2_Uint, 0x04030205, 0x1d5},
    {ChNumFormat::X10Y10Z10W2_Uint, 0x02030405, 0x1d6},
    {ChNumFormat::X10Y10Z10W2_Sint, 0x05040302, 0x1d7},
    {ChNumFormat::X10Y10Z10W2_Sint, 0x01040302, 0x1d8},
    {ChNumFormat::X10Y10Z10W2_Sint, 0x05020304, 0x1d9},
    {ChNumFormat::X10Y10Z10W2_Sint, 0x01020304, 0x1da},
    {ChNumFormat::X10Y10Z10W2_Sint, 0x04030205, 0x1db},
    {ChNumFormat::X10Y10Z10W2_Sint, 0x02030405, 0x1dc},
    {ChNumFormat::X10Y10Z10W2Bias_Unorm, 0x05040302, 0x077},
    {ChNumFormat::U10V10W10_Snorm_A2_Unorm, 0x05040302, 0x043},
    {ChNumFormat::X16_Unorm, 0x01000002, 0x189},
    {ChNumFormat::X16_Unorm, 0x01010102, 0x189},
    {ChNumFormat::X16_Snorm, 0x01000002, 0x18b},
    {ChNumFormat::X16_Snorm, 0x01010102, 0x18b},
    {ChNumFormat::X16_Uint, 0x01000002, 0x18a},
    {ChNumFormat::X16_Uint, 0x01010102, 0x18a},
    {ChNumFormat::X16_Sint, 0x01000002, 0x18c},
    {ChNumFormat::X16_Sint, 0x01010102, 0x18c},
    {ChNumFormat::X16_Float, 0x01000002, 0x06f},
    {ChNumFormat::X16_Float, 0x01010102, 0x06f},
    {ChNumFormat::L16_Unorm, 0x01020202, 0x051},
    {ChNumFormat::X16Y16_Unorm, 0x01000302, 0x022},
    {ChNumFormat::X16Y16_Unorm, 0x01010302, 0x022},
    {ChNumFormat::X16Y16_Snorm, 0x01000302, 0x17a},
    {ChNumFormat::X16Y16_Snorm, 0x01010302, 0x17a},
    {ChNumFormat::X16Y16_Uint, 0x01000302, 0x179},
    {ChNumFormat::X16Y16_Uint, 0x01010302, 0x179},
    {ChNumFormat::X16Y16_Sint, 0x01000302, 0x179},
    {ChNumFormat::X16Y16_Sint, 0x01010302, 0x179},
    {ChNumFormat::X16Y16_Float, 0x01000302, 0x070},
    {ChNumFormat::X16Y16_Float, 0x01010302, 0x070},
    {ChNumFormat::X16Y16Z16W16_Unorm, 0x05040302, 0x024},
    {ChNumFormat::X16Y16Z16W16_Unorm, 0x01040302, 0x1dd},
    {ChNumFormat::X16Y16Z16W16_Unorm, 0x05020304, 0x1de},
    {ChNumFormat::X16Y16Z16W16_Unorm, 0x01020304, 0x1df},
    {ChNumFormat::X16Y16Z16W16_Snorm, 0x05040302, 0x167},
    {ChNumFormat::X16Y16Z16W16_Snorm, 0x01040302, 0x1e0},
    {ChNumFormat::X16Y16Z16W16_Snorm, 0x05020304, 0x1e1},
    {ChNumFormat::X16Y16Z16W16_Snorm, 0x01020304, 0x1e2},
    {ChNumFormat::X16Y16Z16W16_Uint, 0x05040302, 0x166},
    {ChNumFormat::X16Y16Z16W16_Uint, 0x01040302, 0x1e3},
    {ChNumFormat::X16Y16Z16W16_Uint, 0x05020304, 0x1e4},
    {ChNumFormat::X16Y16Z16W16_Uint, 0x01020304, 0x1e5},
    {ChNumFormat::X16Y16Z16W16_Sint, 0x05040302, 0x168},
    {ChNumFormat::X16Y16Z16W16_Sint, 0x01040302, 0x1e6},
    {ChNumFormat::X16Y16Z16W16_Sint, 0x05020304, 0x1e7},
    {ChNumFormat::X16Y16Z16W16_Sint, 0x01020304, 0x1e8},
    {ChNumFormat::X16Y16Z16W16_Float, 0x05040302, 0x071},
    {ChNumFormat::X16Y16Z16W16_Float, 0x01040302, 0x1e9},
    {ChNumFormat::X16Y16Z16W16_Float, 0x05020304, 0x1ea},
    {ChNumFormat::X16Y16Z16W16_Float, 0x01020304, 0x1eb},
    {ChNumFormat::X32_Uint, 0x01000002, 0x17e},
    {ChNumFormat::X32_Uint, 0x01010102, 0x17e},
    {ChNumFormat::X32_Sint, 0x01000002, 0x17f},
    {ChNumFormat::X32_Sint, 0x01010102, 0x17f},
    {ChNumFormat::X32_Float, 0x01000002, 0x072},
    {ChNumFormat::X32_Float, 0x01010102, 0x072},
    {ChNumFormat::X32Y32_Uint, 0x01000302, 0x16a},
    {ChNumFormat::X32Y32_Uint, 0x01010302, 0x16a},
    {ChNumFormat::X32Y32_Sint, 0x01000302, 0x16b},
    {ChNumFormat::X32Y32_Sint, 0x01010302, 0x16b},
    {ChNumFormat::X32Y32_Float, 0x01000302, 0x073},
    {ChNumFormat::X32Y32_Float, 0x01010302, 0x073},
    {ChNumFormat::X32Y32Z32_Uint, 0x01040302, 0x163},
    {ChNumFormat::X32Y32Z32_Sint, 0x01040302, 0x164},
    {ChNumFormat::X32Y32Z32_Float, 0x01040302, 0x162},
    {ChNumFormat::X32Y32Z32W32_Uint, 0x05040302, 0x15f},
    {ChNumFormat::X32Y32Z32W32_Uint, 0x01040302, 0x1ec},
    {ChNumFormat::X32Y32Z32W32_Uint, 0x05020304, 0x1ed},
    {ChNumFormat::X32Y32Z32W32_Uint, 0x01020304, 0x1ee},
    {ChNumFormat::X32Y32Z32W32_Sint, 0x05040302, 0x160},
    {ChNumFormat::X32Y32Z32W32_Sint, 0x01040302, 0x1ef},
    {ChNumFormat::X32Y32Z32W32_Sint, 0x05020304, 0x1f0},
    {ChNumFormat::X32Y32Z32W32_Sint, 0x01020304, 0x1f1},
    {ChNumFormat::X32Y32Z32W32_Float, 0x05040302, 0x074},
    {ChNumFormat::X32Y32Z32W32_Float, 0x01040302, 0x1f2},
    {ChNumFormat::X32Y32Z32W32_Float, 0x05020304, 0x1f3},
    {ChNumFormat::X32Y32Z32W32_Float, 0x01020304, 0x1f4},
    {ChNumFormat::D16_Unorm_S8_Uint, 0x01000302, 0x050},
    {ChNumFormat::D32_Float_S8_Uint, 0x01000302, 0x17d},
    {ChNumFormat::X9Y9Z9E5_Float, 0x01040302, 0x193},
    {ChNumFormat::Bc1_Unorm, 0x05040302, 0x195},
    {ChNumFormat::Bc1_Unorm, 0x01040302, 0x195},
    {ChNumFormat::Bc1_Srgb, 0x05040302, 0x196},
    {ChNumFormat::Bc1_Srgb, 0x01040302, 0x196},
    {ChNumFormat::Bc2_Unorm, 0x05040302, 0x198},
    {ChNumFormat::Bc2_Srgb, 0x05040302, 0x199},
    {ChNumFormat::Bc3_Unorm, 0x05040302, 0x19b},
    {ChNumFormat::Bc3_Srgb, 0x05040302, 0x19c},
    {ChNumFormat::Bc4_Unorm, 0x01000002, 0x19e},
    {ChNumFormat::Bc4_Unorm, 0x01020202, 0x19e},
    {ChNumFormat::Bc4_Snorm, 0x01000002, 0x19f},
    {ChNumFormat::Bc4_Snorm, 0x01020202, 0x19f},
    {ChNumFormat::Bc5_Unorm, 0x01000302, 0x1a1},
    {ChNumFormat::Bc5_Unorm, 0x01010203, 0x32495441},
    {ChNumFormat::Bc5_Unorm, 0x03020202, 0x1a1},
    {ChNumFormat::Bc5_Snorm, 0x01000302, 0x1a2},
    {ChNumFormat::Bc5_Snorm, 0x03020202, 0x1a2},
    {ChNumFormat::Bc6_Ufloat, 0x01040302, 0x1a8},
    {ChNumFormat::Bc6_Sfloat, 0x01040302, 0x1a9},
    {ChNumFormat::Bc7_Unorm, 0x05040302, 0x1ab},
    {ChNumFormat::Bc7_Srgb, 0x05040302, 0x1ac},
    {ChNumFormat::X8Y8_Z8Y8_Unorm, 0x01040302, 0x47424752},
    {ChNumFormat::Y8X8_Y8Z8_Unorm, 0x01040302, 0x42475247},
    {ChNumFormat::AYUV, 0x05040302, 0x56555941},
    {ChNumFormat::UYVY, 0x01040302, 0x59565955},
    {ChNumFormat::VYUY, 0x01040302, 0x59565955},
    {ChNumFormat::YUY2, 0x01040302, 0x32595559},
    {ChNumFormat::YVY2, 0x01040302, 0x32595559},
    {ChNumFormat::YV12, 0x01040302, 0x32315659},
    {ChNumFormat::NV11, 0x01040302, 0x3131564e},
    {ChNumFormat::NV12, 0x01040302, 0x3231564e},
    {ChNumFormat::NV21, 0x01040302, 0x3132564e},
    {ChNumFormat::P016, 0x01040302, 0x36313050},
    {ChNumFormat::P010, 0x01040302, 0x30313050},
    {ChNumFormat::P210, 0x01040302, 0x30313250},
    {ChNumFormat::P208, 0x01040302, 0x38303250},
    {ChNumFormat::Y216, 0x01040302, 0x36313259},
    {ChNumFormat::Y210, 0x01040302, 0x30313259},
    {ChNumFormat::Y416, 0x05040302, 0x36313459},
    {ChNumFormat::Y410, 0x05040302, 0x30313459},
};

uint32 SharedRead32(uint64 address, size_t offset)
{
    return *reinterpret_cast<const volatile uint32*>(static_cast<uintptr_t>(address + offset));
}
uint64 SharedRead64(uint64 address, size_t offset)
{
    return *reinterpret_cast<const volatile uint64*>(static_cast<uintptr_t>(address + offset));
}
uint32 HandleOf(const IGpuMemory& memory, AllocationHandleQuery query)
{
    return (query != nullptr) ? query(memory) : KmtAllocationHandle(memory);
}
} // anonymous namespace

// =====================================================================================================================
bool StereoDisplayModesSupported(const PhysicalProperties& records)
{
    return (GetRecord32(records.basicRecord.data(), 2900) & 0x880000) == 0x80000;
}

bool FrameMetadataSupported(const PhysicalProperties& records)
{
    return (GetRecord32(records.gpuRecord.data(), 14176) & 0x20000) != 0;
}

// =====================================================================================================================
uint32 PrimarySurfaceBitsPerPixel(ChNumFormat format)
{
    // The original indexes FormatInfoTable (identical to PAL's for all 206 formats) without a bound.
    if (uint32(format) >= uint32(ChNumFormat::Count)) { return 0; }
    const Formats::FormatInfo& info = Formats::FormatInfoTable[uint32(format)];
    return ((info.properties & Formats::YuvPlanar) != 0) ? info.bitCount[0] : info.bitsPerPixel;
}

uint32 PrimarySurfaceDdiFormat(SwizzledFormat format)
{
    for (const PrimaryDdiFormat& row : PrimaryDdiFormats)
    {
        if ((row.format == format.format) && (row.swizzle == format.swizzle.swizzleValue)) { return row.ddiFormat; }
    }
    return 0;
}

PrimaryInfoRecord BuildPrimaryInfoRecord(const GetPrimaryInfoInput& input)
{
    PrimaryInfoRecord record;
    InitPrivateRecord(record.data(), record.size(), PrimaryInfoCommand);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0x84, input.vidPnSrcId);
    PutRecord32(record.data(), 0x88, input.width);
    PutRecord32(record.data(), 0x8c, input.height);
    PutRecord32(record.data(), 0x90, PrimarySurfaceBitsPerPixel(input.swizzledFormat.format));
    PutRecord32(record.data(), 0x94, PrimarySurfaceDdiFormat(input.swizzledFormat));
    // The refresh rate is sent even when refreshRateValid is clear.
    PutRecord32(record.data(), 0x98, input.refreshRate.numerator);
    PutRecord32(record.data(), 0x9c, input.refreshRate.denominator);
    // Request bits 0/1 keep their position, bits 2/3 move to 3/4 (3140ba..3140e6); bits 4+ are not sent.
    const uint32 flags = input.flags.u32All;
    PutRecord32(record.data(), 0xa0, (flags & 0x3) | ((flags & 0xc) << 1));
    PutRecord32(record.data(), 0xc0, 0x40);
    return record;
}

void DecodePrimaryInfoRecord(const PrimaryInfoRecord& record, bool gpuSpoofed, PrimaryInfoReply* reply)
{
    const uint32 caps = GetRecord32(record.data(), 0xd4);
    reply->output.tilingCaps = gpuSpoofed ? 0 : GetRecord32(record.data(), 0xc4);
    reply->output.stereoMode = gpuSpoofed ? StereoModeNotSupported : StereoMode((caps >> 3) & 7);
    reply->output.mallCursorCacheSize = GetRecord32(record.data(), 0xec);
    uint32 flags = reply->output.flags.u32All;
    flags = (flags & ~0x1u) | ((caps >> 11) & 0x1); // dvoHwMode
    flags = (flags & ~0x2u) | ((caps >> 7) & 0x2);  // xdmaHwMode
    flags = (flags & ~0x4u) | ((caps >> 8) & 0x4);  // swMode
    reply->output.flags.u32All = flags;
    if (!gpuSpoofed)
    {
        const uint32 display = GetRecord32(record.data(), 0xe8);
        uint32 extended = (reply->extendedFlags & ~0x1ffeu) | ((display >> 1) & 0x1ffe);
        if ((display & 0x7f0) != 0) { extended |= 1; }
        reply->extendedFlags = extended;
    }
}

Result QueryPrimaryInfo(const EscapeRoute& route, bool gpuSpoofed, const GetPrimaryInfoInput& input,
                        PrimaryInfoReply* reply)
{
    PrimaryInfoRecord record = BuildPrimaryInfoRecord(input);
    const Result result = SharedEscape(route, record.data(), record.size());
    if ((result == Result::Success) && (reply != nullptr)) { DecodePrimaryInfoRecord(record, gpuSpoofed, reply); }
    return result;
}

Result GetPrimaryInfo(const EscapeRoute& route, bool gpuSpoofed, const GetPrimaryInfoInput& input,
                      GetPrimaryInfoOutput* output)
{
    PrimaryInfoReply reply = {};
    const Result result = QueryPrimaryInfo(route, gpuSpoofed, input, &reply);
    // 314255 stores the first 16 bytes unconditionally (zeros after a failed query); a null output faults there.
    if (output == nullptr) { return Result::ErrorInvalidPointer; }
    *output = reply.output;
    return result;
}

// =====================================================================================================================
bool IsReportedStereoModeFormat(uint32 ddiFormat)
{
    return (ddiFormat == 22) || (ddiFormat == 23) || (ddiFormat == 24) || (ddiFormat == 35);
}

SwizzledFormat StereoModeFormat(uint32 ddiFormat)
{
    SwizzledFormat format = UndefinedSwizzledFormat;
    switch (ddiFormat)
    {
    case 22: // D3DDDIFMT_X8R8G8B8
        format.format = ChNumFormat::X8Y8Z8W8_Unorm;
        format.swizzle.swizzleValue = SwizzleBgr1;
        break;
    case 23: // D3DDDIFMT_R5G6B5
        format.format = ChNumFormat::X5Y6Z5_Unorm;
        format.swizzle.swizzleValue = SwizzleBgr1;
        break;
    case 24: // D3DDDIFMT_X1R5G5B5
        format.format = ChNumFormat::X5Y5Z5W1_Unorm;
        format.swizzle.swizzleValue = SwizzleBgr1;
        break;
    case 35: // D3DDDIFMT_A2R10G10B10
        format.format = ChNumFormat::X10Y10Z10W2_Unorm;
        format.swizzle.swizzleValue = SwizzleBgra;
        break;
    default:
        break;
    }
    return format;
}

Result GetStereoDisplayModes(const EscapeRoute& route, const Util::IndirectAllocator& allocator, bool supported,
                             uint32* count, StereoDisplayModeOutput* list)
{
    if (count == nullptr) { return Result::ErrorInvalidPointer; }
    if (!supported) { return Result::Unsupported; }
    if (list == nullptr)
    {
        // 3146b9: a 328-byte local record of which only the first 264 bytes are sent.
        std::array<uint8, 0x148> record;
        InitPrivateRecord(record.data(), record.size(), StereoModeCommand);
        PutRecord32(record.data(), 0x80, 0x40);
        PutRecord32(record.data(), 0x84, 0x40);
        PutRecord32(record.data(), 0xc4, 0x40);
        PutRecord32(record.data(), 0xc8, 0x40);
        const Result result = SharedEscape(route, record.data(), SmallRecordBytes);
        *count = GetRecord32(record.data(), 0xcc); // Stored whatever the result (31473f).
        return result;
    }
    const uint32 capacity = *count;
    // The original sizes the record as (count-1)*64+0x148 in 32 bits; sizes it would wrap are refused here.
    if (capacity > MaxStereoRows) { return Result::ErrorOutOfMemory; }
    const size_t bytes = (size_t(capacity) * StereoRowBytes) + SmallRecordBytes;
    auto* record = static_cast<uint8*>(PAL_MALLOC_ALIGNED(bytes, 8, &allocator, Util::AllocInternalTemp));
    if (record == nullptr) { return Result::ErrorOutOfMemory; }
    InitPrivateRecord(record, bytes, StereoModeCommand);
    PutRecord32(record, 0x80, 0x40);
    PutRecord32(record, 0x84, 0x40);
    PutRecord32(record, 0xc4, (capacity * 64) + 0x40);
    PutRecord32(record, 0xc8, (capacity * 64) + 0x40);
    Result result = Result::ErrorInvalidValue; // Returned unchanged when the capacity is 0 and nothing is sent.
    uint32 written = 0;
    for (uint32 start = 0; start < capacity; start += StereoPageRows)
    {
        PutRecord32(record, 0x8c, start);
        PutRecord32(record, 0x88, Util::Min(capacity - start, StereoPageRows));
        // Every page sends the whole record; a failed page leaves the previous reply rows in place, as in the
        // original, and only the last page's result is returned.
        result = SharedEscape(route, record, bytes);
        // The original trusts the reply row count; rows past the allocation or the caller's list are ignored.
        const uint32 rows = Util::Min(GetRecord32(record, 0xcc), capacity);
        for (uint32 row = 0; row < rows; ++row)
        {
            const uint8* entry = record + StereoRowBase + (size_t(row) * StereoRowBytes);
            const uint32 ddiFormat = GetRecord32(entry, 0x10);
            if (!IsReportedStereoModeFormat(ddiFormat) || (written == capacity)) { continue; }
            StereoDisplayModeOutput& mode = list[written++];
            mode.extent.width = GetRecord32(entry, 0x4);
            mode.extent.height = GetRecord32(entry, 0x8);
            mode.format = StereoModeFormat(ddiFormat);
            mode.refreshRate.numerator = GetRecord32(entry, 0x14);
            mode.refreshRate.denominator = GetRecord32(entry, 0x18);
        }
    }
    *count = written;
    PAL_FREE(record, &allocator);
    return result;
}

// =====================================================================================================================
void DecodeActive10BitPackedPixelMode(uint32 enabled, uint32 packing, Active10BitPackedPixelModeOutput* mode)
{
    if (enabled == 1)
    {
        mode->isInWs10BitMode = true;
        mode->notifyKmd10bitsPresent = true;
    }
    else
    {
        // 314a16 tests the caller's existing byte; the method does not write it on this path.
        uint8 inMode = 0;
        std::memcpy(&inMode, &mode->isInWs10BitMode, sizeof(inMode));
        if (inMode == 0) { return; }
    }
    struct Packing
    {
        PackedPixelType type;
        uint32 ratio;
    };
    // Jump table 314adc; other values leave both fields unchanged.
    constexpr Packing Packings[] =
    {
        {PackedPixelType::Native, 1},
        {PackedPixelType::SplitG70B54_R70B10, 2},
        {PackedPixelType::R70G76, 1},
        {PackedPixelType::SplitB70G10_R70G76, 2},
        {PackedPixelType::G70B54_R70B10, 2},
        {PackedPixelType::G70B54, 1},
        {PackedPixelType::B70R30_G70R74, 2},
        {PackedPixelType::B70_G70_R70, 3},
        {PackedPixelType::B70R32_G70R76, 2},
    };
    if (packing < Util::ArrayLen32(Packings))
    {
        mode->packedPixelType = Packings[packing].type;
        mode->pixelPackRatio = Packings[packing].ratio;
    }
}

Result GetActive10BitPackedPixelMode(const EscapeRoute& route, Active10BitPackedPixelModeOutput* mode)
{
    std::array<uint8, SmallRecordBytes> record;
    InitPrivateRecord(record.data(), record.size(), Active10BitModeCommand);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0xc4, 0x40);
    const Result result = SharedEscape(route, record.data(), record.size());
    // The reply is decoded without checking the escape result (314a03); a null output faults there.
    if (mode == nullptr) { return Result::ErrorInvalidPointer; }
    DecodeActive10BitPackedPixelMode(GetRecord32(record.data(), 0xcc), GetRecord32(record.data(), 0xd0), mode);
    return result;
}

uint32 KmtAllocationHandle(const IGpuMemory& memory)
{
    // 2b3f20 follows GpuMemory+100 to the root object and returns its first KMT allocation handle. The only such
    // link this backend can form is the peer original.
    const auto* gpuMemory = static_cast<const Pal::GpuMemory*>(&memory);
    while (gpuMemory->OriginalGpuMem() != nullptr) { gpuMemory = gpuMemory->OriginalGpuMem(); }
    return static_cast<const GpuMemory*>(gpuMemory)->OsAllocation().handle;
}

Result RequestKmdReinterpretAs10Bit(const EscapeRoute& route, const IGpuMemory* memory, AllocationHandleQuery handleOf)
{
    // 314b37 reads the object before the escape; a null pointer faults there.
    if (memory == nullptr) { return Result::ErrorInvalidPointer; }
    std::array<uint8, SmallRecordBytes> record;
    InitPrivateRecord(record.data(), record.size(), Reinterpret10BitCommand);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0x84, HandleOf(*memory, handleOf));
    PutRecord32(record.data(), 0x8c, GetRecord32(record.data(), 0x8c) | 1);
    PutRecord32(record.data(), 0xc4, 0x40);
    return SharedEscape(route, record.data(), record.size());
}

// =====================================================================================================================
uint32 EncodeFrameMetadataCaps(uint32 palFlags)
{
    return (palFlags & 0x3f) | ((palFlags & 0x40) << 1) | ((palFlags & 0x3800) << 2) |
           ((palFlags & 0x7c000) << 3) | ((palFlags & 0x80000) << 3);
}

Result QueryKmdSharedBuffer(const EscapeRoute& route, uint32 type, const uint8* parameters48,
                            uint64* buffer, uint64* second, uint64* third)
{
    std::array<uint8, SmallRecordBytes> record;
    InitPrivateRecord(record.data(), record.size(), SharedBufferCommand);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0x84, type);
    if (parameters48 != nullptr) { std::memcpy(record.data() + 0x94, parameters48, 48); }
    PutRecord32(record.data(), 0xc4, 0x40);
    const Result result = SharedEscape(route, record.data(), record.size());
    if (result == Result::Success)
    {
        if (buffer != nullptr) { *buffer = GetRecord64(record.data(), 0xc8); }
        if (second != nullptr) { *second = GetRecord64(record.data(), 0xd0); }
        if (third != nullptr) { *third = GetRecord64(record.data(), 0xd8); }
    }
    return result;
}

void InitKmdSharedBuffers(const EscapeRoute& route, const DeviceFinalizeInfo& finalizeInfo,
                          const PhysicalProperties& records, KmdSharedBuffers* buffers)
{
    if (finalizeInfo.flags.requireFlipStatus != 0)
    {
        uint64 unused = 0;
        QueryKmdSharedBuffer(route, 3, nullptr, &buffers->flipStatus, &unused, &buffers->flipOwner);
    }
    if ((finalizeInfo.flags.requireFrameMetadata != 0) && FrameMetadataSupported(records))
    {
        std::array<uint8, 48> parameters = {};
        PutRecord32(parameters.data(), 0,
                    EncodeFrameMetadataCaps(finalizeInfo.supportedFullScreenFrameMetadata.u32All));
        QueryKmdSharedBuffer(route, 4, parameters.data(), &buffers->frameMetadata, nullptr, nullptr);
    }
    if (QueryKmdSharedBuffer(route, 5, nullptr, &buffers->rsFeatureChange, nullptr, nullptr) == Result::Success)
    {
        buffers->rsFeatureCache[0] = UINT32_MAX;
    }
    // Device+8af0/8af4 are copied from GPU record DWORDs 14856/14860 by EarlyInit (30ac9a..30acaf).
    const uint32 version = GetRecord32(records.gpuRecord.data(), 14856);
    const uint32 revision = GetRecord32(records.gpuRecord.data(), 14860);
    if (version >= 0x7e2)
    {
        QueryKmdSharedBuffer(route, 7, nullptr, &buffers->chillStatus, nullptr, nullptr);
        if ((version > 0x7e2) || (revision != 0))
        {
            if (QueryKmdSharedBuffer(route, 9, nullptr, &buffers->rsFeatureSettings, nullptr, nullptr) ==
                Result::Success)
            {
                for (uint32& value : buffers->rsFeatureCache) { value = UINT32_MAX; }
            }
        }
    }
}

uint32 DecodeFrameMetadataFlags(uint32 kmdFlags, uint32 previousFlags)
{
    // 314c10..314d19: bits 0..19 are replaced, bits 20..31 keep the caller's value.
    const uint32 flags = (kmdFlags & 0x3f) |            // timerNodeSubmission .. forceSwCfMode
                         ((kmdFlags >> 1) & 0xc0) |     // postFrameTimerSubmission, useHp3dForDwm <- 7, 8
                         ((kmdFlags >> 2) & 0x100) |    // expandDcc <- 10
                         (kmdFlags & 0x200) |           // enableTurboSyncForDwm <- 9
                         ((kmdFlags << 4) & 0x400) |    // enableDwmFrameMetadata <- 6
                         ((kmdFlags >> 2) & 0x3800) |   // flipIntervalOverride <- 13..15
                         ((kmdFlags >> 3) & 0xfc000);   // disableFreeMux .. sendCameraMatrix <- 17..22
    return (previousFlags & 0xfff00000) | flags;
}

Result PollFullScreenFrameMetadataControl(const KmdSharedBuffers& buffers, uint32 vidPnSrcId,
                                          PerSourceFrameMetadataControl* control)
{
    if (buffers.frameMetadata == 0) { return Result::ErrorUnavailable; }
    if (vidPnSrcId >= SharedRead32(buffers.frameMetadata, 4)) { return Result::ErrorInvalidValue; }
    if (control == nullptr) { return Result::Success; }
    const uint64 entry = buffers.frameMetadata + ((uint64(vidPnSrcId) + 1) * 64);
    control->flags.u32All = DecodeFrameMetadataFlags(SharedRead32(entry, 0), control->flags.u32All);
    control->timerNodeSubmissionMode =
        (SharedRead32(entry, 4) == 1) ? TimerNodeMode::ForceFlipHold : TimerNodeMode::Unspecified;
    return Result::Success;
}

Result GetFlipStatus(const KmdSharedBuffers& buffers, uint32 vidPnSrcId, FlipStatusFlags* flags, bool* isFlipOwner)
{
    if (buffers.flipStatus == 0) { return Result::ErrorInitializationFailed; }
    if (vidPnSrcId >= SharedRead32(buffers.flipStatus, 4)) { return Result::ErrorInvalidValue; }
    // 314d8b and 314d8e store through both pointers unconditionally.
    if ((flags == nullptr) || (isFlipOwner == nullptr)) { return Result::ErrorInvalidPointer; }
    const uint64 entry = buffers.flipStatus + ((uint64(vidPnSrcId) + 1) * 64);
    *isFlipOwner = (SharedRead64(entry, 0) == buffers.flipOwner);
    flags->u32All = (flags->u32All & ~0x7u) | (SharedRead32(entry, 8) & 0x7);
    return Result::Success;
}

// =====================================================================================================================
Result TurboSyncControl(const EscapeRoute& route, const TurboSyncControlInput& input,
                        const TurboSyncDeviceSlot (&devices)[MaxDevices], AllocationHandleQuery handleOf)
{
    std::array<uint8, SmallRecordBytes> record;
    InitPrivateRecord(record.data(), record.size(), TurboSyncCommand);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0x84, input.vidPnSourceId);
    PutRecord32(record.data(), 0xc4, 0x40);
    bool withSurfaces = false;
    switch (input.mode)
    {
    case TurboSyncControlMode::Disable:
        PutRecord32(record.data(), 0x88, 0);
        break;
    case TurboSyncControlMode::Enable:
        PutRecord32(record.data(), 0x88, 1);
        withSurfaces = true;
        break;
    case TurboSyncControlMode::UpdateAllocations:
        PutRecord32(record.data(), 0x88, 2);
        withSurfaces = true;
        break;
    case TurboSyncControlMode::Register:
        PutRecord32(record.data(), 0x88, 1); // Same KMD code as Enable, without surfaces.
        break;
    default:
        return Result::ErrorInvalidValue;
    }
    if (withSurfaces)
    {
        Result error = Result::Success;
        uint32 highestAdapter = 0;
        for (uint32 gpu = 0; (gpu < MaxDevices) && (error == Result::Success); ++gpu)
        {
            if (input.pPrimaryMemoryArray[gpu][0] == nullptr) { continue; }
            if (!devices[gpu].present || !devices[gpu].onPrimaryAdapter) { return Result::ErrorInvalidValue; }
            const uint32 adapter = devices[gpu].physicalAdapter;
            for (uint32 surface = 0; surface < TurboSyncMaxSurfaces; ++surface)
            {
                const IGpuMemory* memory = input.pPrimaryMemoryArray[gpu][surface];
                if (memory == nullptr)
                {
                    error = Result::ErrorInvalidValue;
                    break;
                }
                // Handles go to +0x8c[adapter][surface]; the original does not bound the adapter index.
                const size_t offset = 0x8c + (((size_t(adapter) * TurboSyncMaxSurfaces) + surface) * 4);
                if ((offset + 4) > record.size()) { return Result::ErrorInvalidValue; }
                PutRecord32(record.data(), offset, HandleOf(*memory, handleOf));
            }
            highestAdapter = Util::Max(highestAdapter, adapter);
        }
        PutRecord32(record.data(), 0xac, highestAdapter + 1);
        if (error != Result::Success) { return error; }
    }
    return SharedEscape(route, record.data(), record.size());
}
} // namespace Pal::Wddm
