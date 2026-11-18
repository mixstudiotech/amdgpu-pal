// Copyright (c) 2026. SPDX-License-Identifier: MIT
// External shared images (original Device slots 39/63/64: 312bd0, 312f60, 313120 and helpers 3120c0,
// 3124f0, 3833c0, 3826a0, 312620, 2cd040). See .build/pal-wddm-logic/external-image/analysis.md.
#include "core/os/wddm/wddmExternalImage.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmImage.h"
#include "core/os/wddm/wddmResult.h"
#include "core/os/wddm/wddmPrivate.h"
#include "core/hw/gfxip/gfxDevice.h"
#include "core/hw/gfxip/gfxImage.h"
#include "core/image.h"
#include "core/privateScreen.h"
#include "palFormatInfo.h"
#include "palGpuUtil.h"
#include <cstring>

namespace Pal::Wddm
{
namespace
{
uint16 Read16(const uint8* bytes, size_t offset) { uint16 v; std::memcpy(&v, bytes + offset, sizeof(v)); return v; }
uint32 Read32(const uint8* bytes, size_t offset) { uint32 v; std::memcpy(&v, bytes + offset, sizeof(v)); return v; }
uint64 Read64(const uint8* bytes, size_t offset) { uint64 v; std::memcpy(&v, bytes + offset, sizeof(v)); return v; }

// AMD allocation entry (follows the 64-byte allocation header). The buffer-object record starts at +0x08 and
// the first SURF record at +0xc4; native offsets are kept so they can be checked against the disassembly.
namespace Entry
{
constexpr size_t Bytes             = 0x00;
constexpr size_t BoFlags           = 0x10;
constexpr size_t BoAlignment       = 0x18;
constexpr size_t BoHeapIdCount     = 0x1c; // Low three bits: number of heap-id bytes.
constexpr size_t BoHeapIds         = 0x20;
constexpr size_t BoAttributes      = 0x66; // High WORD of BO DWORD 0x64.
constexpr size_t BoPhysicalSize    = 0x68;
constexpr size_t SurfaceCount      = 0x88;
constexpr size_t ResourceFlags     = 0x90;
constexpr size_t Samples           = 0x94; // WORD
constexpr size_t MipLevels         = 0x98;
constexpr size_t ArraySize         = 0x9c;
constexpr size_t MetadataOffset    = 0xb0;
constexpr size_t SurfaceFlags      = 0xc8;
constexpr size_t SwizzleMode       = 0xcc;
constexpr size_t ResourceType      = 0xd0;
constexpr size_t Format            = 0xd4;
constexpr size_t Width             = 0xd8;
constexpr size_t Height            = 0xdc;
constexpr size_t RowPitch          = 0xe0;
constexpr size_t AlternateHeight   = 0xe4; // Used when SurfaceFlags bit14 (cross-adapter) is set.
constexpr size_t Depth             = 0xe8;
constexpr size_t SliceBytes        = 0xec;
constexpr size_t PipeBankXor0      = 0x100;
constexpr size_t DisplayDccPrimary = 0x164;
constexpr size_t DisplayDccSecond  = 0x16c;
constexpr size_t DisplayDccPitch   = 0x174;
constexpr size_t DisplayDccFlags   = 0x178;
constexpr size_t PipeBankXor1      = 0x18c;
constexpr size_t PipeBankXor2      = 0x190;
constexpr size_t TypeMinBytes      = Height + sizeof(uint32); // Reads of the type query (312bd0).
static_assert(PipeBankXor2 + 8 == ExternalImageEntryMinBytes, "3833c0 reads up to entry+0x197");
}
// Shared metadata section at entry + Entry::MetadataOffset.
namespace Metadata
{
constexpr size_t Address        = 0x08;
constexpr size_t Size           = 0x10;
constexpr size_t DccOffset      = 0x74;
constexpr size_t CmaskOffset    = 0x84;
constexpr size_t HtileOffset    = 0x8c;
constexpr size_t Version        = 0x94; // DWORD minor | major << 16.
constexpr size_t VersionMajor   = 0x96;
constexpr size_t Flags          = 0x98;
constexpr size_t FastClear      = 0x9c;
constexpr size_t DccState       = 0xa4;
constexpr size_t ResourceId     = 0xb4;
constexpr size_t FmaskSwizzle   = 0xc4;
constexpr size_t AllocFlags     = 0xd0;
constexpr size_t Mall           = 0xd4;
constexpr size_t MallStartPage  = 0xd8;
constexpr size_t MallPages      = 0xdc;
constexpr size_t MinBytes       = MallPages + sizeof(uint32);
}
// Original Device+0xd9c8. The verified profile has one physical adapter (see wddmEscape.cpp).
constexpr uint32 PhysicalAdapterIndex = 0;
// Device+0xcb40 is GPU query record DWORD 14176 (copy base proven by the gpu-map/queue-types families).
// Bit 3 gates the shared-metadata translation in 3826a0. Bit 31 is copied to the original's memory-property
// flag bit 14 (30c196), which gates the MALL policy copy in 312620; this backend does not report that flag.
constexpr size_t GpuCapsOffset = 14176;
constexpr uint32 SharedMetadataGate = 1u << 3;
constexpr uint32 MallPolicyGate = 1u << 31;
uint32 GpuCaps(const Device& device) { return Read32(device.Adapter().physical.gpuRecord.data(), GpuCapsOffset); }

constexpr uint32 FourCc(char a, char b, char c, char d)
{
    return uint32(uint8(a)) | (uint32(uint8(b)) << 8) | (uint32(uint8(c)) << 16) | (uint32(uint8(d)) << 24);
}
namespace Channel
{
constexpr ChannelSwizzle Zero = ChannelSwizzle::Zero;
constexpr ChannelSwizzle One  = ChannelSwizzle::One;
constexpr ChannelSwizzle X    = ChannelSwizzle::X;
constexpr ChannelSwizzle Y    = ChannelSwizzle::Y;
constexpr ChannelSwizzle Z    = ChannelSwizzle::Z;
constexpr ChannelSwizzle W    = ChannelSwizzle::W;
}
constexpr uint32 Swz(ChannelSwizzle r, ChannelSwizzle g, ChannelSwizzle b, ChannelSwizzle a)
{
    return uint32(r) | (uint32(g) << 8) | (uint32(b) << 16) | (uint32(a) << 24);
}
struct KmdFormat
{
    uint32      kmdFormat;
    ChNumFormat format;
    uint32      swizzle;
    bool        allViewFormats;
    bool        depthStencil;
};
// Exact output of native 2cd040 for every input that differs from its default {Undefined, X001}. Generated by
// emulating the original function (external-image/emulate_kmd_format.py, kmd-format-table.json).
using namespace Channel;
constexpr KmdFormat KmdFormats[] =
{
    { 0x015, ChNumFormat::X8Y8Z8W8_Unorm, Swz(Z, Y, X, W), false, false },
    { 0x016, ChNumFormat::X8Y8Z8W8_Unorm, Swz(Z, Y, X, One), false, false },
    { 0x017, ChNumFormat::X5Y6Z5_Unorm, Swz(Z, Y, X, One), false, false },
    { 0x018, ChNumFormat::X5Y5Z5W1_Unorm, Swz(Z, Y, X, One), false, false },
    { 0x019, ChNumFormat::X5Y5Z5W1_Unorm, Swz(Z, Y, X, W), false, false },
    { 0x01a, ChNumFormat::X4Y4Z4W4_Unorm, Swz(Z, Y, X, W), false, false },
    { 0x01c, ChNumFormat::A8_Unorm, Swz(Zero, Zero, Zero, W), false, false },
    { 0x01f, ChNumFormat::X10Y10Z10W2_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x020, ChNumFormat::X8Y8Z8W8_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x021, ChNumFormat::X8Y8Z8W8_Unorm, Swz(X, Y, Z, One), false, false },
    { 0x022, ChNumFormat::X16Y16_Unorm, Swz(X, Y, Zero, One), false, false },
    { 0x023, ChNumFormat::X10Y10Z10W2_Unorm, Swz(Z, Y, X, W), false, false },
    { 0x024, ChNumFormat::X16Y16Z16W16_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x029, ChNumFormat::P8_Unorm, Swz(X, X, X, Zero), false, false },
    { 0x032, ChNumFormat::L8_Unorm, Swz(X, X, X, One), false, false },
    { 0x033, ChNumFormat::L8A8_Unorm, Swz(X, X, X, Y), false, false },
    { 0x034, ChNumFormat::L4A4_Unorm, Swz(X, X, X, Y), false, false },
    { 0x03e, ChNumFormat::U8V8_Snorm_L8W8_Unorm, Swz(X, Y, Z, One), false, false },
    { 0x043, ChNumFormat::U10V10W10_Snorm_A2_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x050, ChNumFormat::X16_Unorm, Swz(X, Zero, Zero, One), false, true },
    { 0x051, ChNumFormat::L16_Unorm, Swz(X, X, X, One), false, false },
    { 0x06f, ChNumFormat::X16_Float, Swz(X, Zero, Zero, One), false, false },
    { 0x070, ChNumFormat::X16Y16_Float, Swz(X, Y, Zero, One), false, false },
    { 0x071, ChNumFormat::X16Y16Z16W16_Float, Swz(X, Y, Z, W), false, false },
    { 0x072, ChNumFormat::X32_Float, Swz(X, Zero, Zero, One), false, false },
    { 0x073, ChNumFormat::X32Y32_Float, Swz(X, Y, Zero, One), false, false },
    { 0x074, ChNumFormat::X32Y32Z32W32_Float, Swz(X, Y, Z, W), false, false },
    { 0x077, ChNumFormat::X10Y10Z10W2Bias_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x15e, ChNumFormat::X32Y32Z32W32_Uint, Swz(X, Y, Z, W), true, false },
    { 0x15f, ChNumFormat::X32Y32Z32W32_Uint, Swz(X, Y, Z, W), false, false },
    { 0x160, ChNumFormat::X32Y32Z32W32_Sint, Swz(X, Y, Z, W), false, false },
    { 0x161, ChNumFormat::X32Y32Z32_Uint, Swz(X, Y, Z, One), true, false },
    { 0x162, ChNumFormat::X32Y32Z32_Float, Swz(X, Y, Z, One), false, false },
    { 0x163, ChNumFormat::X32Y32Z32_Uint, Swz(X, Y, Z, One), false, false },
    { 0x164, ChNumFormat::X32Y32Z32_Sint, Swz(X, Y, Z, One), false, false },
    { 0x165, ChNumFormat::X16Y16Z16W16_Uint, Swz(X, Y, Z, W), true, false },
    { 0x166, ChNumFormat::X16Y16Z16W16_Uint, Swz(X, Y, Z, W), false, false },
    { 0x167, ChNumFormat::X16Y16Z16W16_Snorm, Swz(X, Y, Z, W), false, false },
    { 0x168, ChNumFormat::X16Y16Z16W16_Sint, Swz(X, Y, Z, W), false, false },
    { 0x169, ChNumFormat::X32Y32_Uint, Swz(X, Y, Zero, One), true, false },
    { 0x16a, ChNumFormat::X32Y32_Uint, Swz(X, Y, Zero, One), false, false },
    { 0x16b, ChNumFormat::X32Y32_Sint, Swz(X, Y, Zero, One), false, false },
    { 0x16c, ChNumFormat::Undefined, Swz(X, Zero, Zero, One), true, false },
    { 0x16d, ChNumFormat::D32_Float_S8_Uint, Swz(X, Y, Zero, One), false, false },
    { 0x16e, ChNumFormat::Undefined, Swz(X, Zero, Zero, One), true, false },
    { 0x16f, ChNumFormat::Undefined, Swz(X, Zero, Zero, One), true, false },
    { 0x170, ChNumFormat::X10Y10Z10W2_Uint, Swz(X, Y, Z, W), true, false },
    { 0x171, ChNumFormat::X10Y10Z10W2_Uint, Swz(X, Y, Z, W), false, false },
    { 0x172, ChNumFormat::X11Y11Z10_Float, Swz(X, Y, Z, One), false, false },
    { 0x173, ChNumFormat::X8Y8Z8W8_Uint, Swz(X, Y, Z, W), true, false },
    { 0x174, ChNumFormat::X8Y8Z8W8_Srgb, Swz(X, Y, Z, W), false, false },
    { 0x175, ChNumFormat::X8Y8Z8W8_Uint, Swz(X, Y, Z, W), false, false },
    { 0x176, ChNumFormat::X8Y8Z8W8_Snorm, Swz(X, Y, Z, W), false, false },
    { 0x177, ChNumFormat::X8Y8Z8W8_Sint, Swz(X, Y, Z, W), false, false },
    { 0x178, ChNumFormat::X16Y16_Uint, Swz(X, Y, Zero, One), true, false },
    { 0x179, ChNumFormat::X16Y16_Uint, Swz(X, Y, Zero, One), false, false },
    { 0x17a, ChNumFormat::X16Y16_Snorm, Swz(X, Y, Zero, One), false, false },
    { 0x17b, ChNumFormat::X16Y16_Sint, Swz(X, Y, Zero, One), false, false },
    { 0x17c, ChNumFormat::X32_Uint, Swz(X, Zero, Zero, One), true, false },
    { 0x17d, ChNumFormat::X32_Float, Swz(X, Zero, Zero, One), false, true },
    { 0x17e, ChNumFormat::X32_Uint, Swz(X, Zero, Zero, One), false, false },
    { 0x17f, ChNumFormat::X32_Sint, Swz(X, Zero, Zero, One), false, false },
    { 0x180, ChNumFormat::Undefined, Swz(X, Zero, Zero, One), true, false },
    { 0x181, ChNumFormat::Undefined, Swz(X, Zero, Zero, One), true, false },
    { 0x182, ChNumFormat::Undefined, Swz(X, Zero, Zero, One), true, false },
    { 0x183, ChNumFormat::X8Y8_Uint, Swz(X, Y, Zero, One), true, false },
    { 0x184, ChNumFormat::X8Y8_Unorm, Swz(X, Y, Zero, One), false, false },
    { 0x185, ChNumFormat::X8Y8_Uint, Swz(X, Y, Zero, One), false, false },
    { 0x186, ChNumFormat::X8Y8_Snorm, Swz(X, Y, Zero, One), false, false },
    { 0x187, ChNumFormat::X8Y8_Sint, Swz(X, Y, Zero, One), false, false },
    { 0x188, ChNumFormat::X16_Uint, Swz(X, Zero, Zero, One), true, false },
    { 0x189, ChNumFormat::X16_Unorm, Swz(X, Zero, Zero, One), false, false },
    { 0x18a, ChNumFormat::X16_Uint, Swz(X, Zero, Zero, One), false, false },
    { 0x18b, ChNumFormat::X16_Snorm, Swz(X, Zero, Zero, One), false, false },
    { 0x18c, ChNumFormat::X16_Sint, Swz(X, Zero, Zero, One), false, false },
    { 0x18d, ChNumFormat::X8_Uint, Swz(X, Zero, Zero, One), true, false },
    { 0x18e, ChNumFormat::X8_Unorm, Swz(X, Zero, Zero, One), false, false },
    { 0x18f, ChNumFormat::X8_Uint, Swz(X, Zero, Zero, One), false, false },
    { 0x190, ChNumFormat::X8_Snorm, Swz(X, Zero, Zero, One), false, false },
    { 0x191, ChNumFormat::X8_Sint, Swz(X, Zero, Zero, One), false, false },
    { 0x192, ChNumFormat::X1_Unorm, Swz(X, Zero, Zero, One), false, false },
    { 0x193, ChNumFormat::X9Y9Z9E5_Float, Swz(X, Y, Z, One), false, false },
    { 0x194, ChNumFormat::Bc1_Unorm, Swz(X, Y, Z, W), true, false },
    { 0x195, ChNumFormat::Bc1_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x196, ChNumFormat::Bc1_Srgb, Swz(X, Y, Z, W), false, false },
    { 0x197, ChNumFormat::Bc2_Unorm, Swz(X, Y, Z, W), true, false },
    { 0x198, ChNumFormat::Bc2_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x199, ChNumFormat::Bc2_Srgb, Swz(X, Y, Z, W), false, false },
    { 0x19a, ChNumFormat::Bc3_Unorm, Swz(X, Y, Z, W), true, false },
    { 0x19b, ChNumFormat::Bc3_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x19c, ChNumFormat::Bc3_Srgb, Swz(X, Y, Z, W), false, false },
    { 0x19d, ChNumFormat::Bc4_Unorm, Swz(X, Zero, Zero, One), true, false },
    { 0x19e, ChNumFormat::Bc4_Unorm, Swz(X, Zero, Zero, One), false, false },
    { 0x19f, ChNumFormat::Bc4_Snorm, Swz(X, Zero, Zero, One), false, false },
    { 0x1a0, ChNumFormat::Bc5_Unorm, Swz(X, Y, Zero, One), true, false },
    { 0x1a1, ChNumFormat::Bc5_Unorm, Swz(X, Y, Zero, One), false, false },
    { 0x1a2, ChNumFormat::Bc5_Snorm, Swz(X, Y, Zero, One), true, false },
    { 0x1a3, ChNumFormat::X8Y8Z8W8_Srgb, Swz(Z, Y, X, W), true, false },
    { 0x1a4, ChNumFormat::X8Y8Z8W8_Srgb, Swz(Z, Y, X, W), false, false },
    { 0x1a5, ChNumFormat::X8Y8Z8W8_Srgb, Swz(Z, Y, X, One), true, false },
    { 0x1a6, ChNumFormat::X8Y8Z8W8_Srgb, Swz(Z, Y, X, One), false, false },
    { 0x1a7, ChNumFormat::Bc6_Ufloat, Swz(X, Y, Z, One), true, false },
    { 0x1a8, ChNumFormat::Bc6_Ufloat, Swz(X, Y, Z, One), false, false },
    { 0x1a9, ChNumFormat::Bc6_Sfloat, Swz(X, Y, Z, One), false, false },
    { 0x1aa, ChNumFormat::Bc7_Unorm, Swz(X, Y, Z, W), true, false },
    { 0x1ab, ChNumFormat::Bc7_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x1ac, ChNumFormat::Bc7_Srgb, Swz(X, Y, Z, W), false, false },
    { 0x1ad, ChNumFormat::X4Y4Z4W4_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x1ae, ChNumFormat::X4Y4Z4W4_Unorm, Swz(W, Z, Y, X), false, false },
    { 0x1af, ChNumFormat::X4Y4Z4W4_Unorm, Swz(W, X, Y, Z), false, false },
    { 0x1b0, ChNumFormat::X5Y6Z5_Unorm, Swz(X, Y, Z, One), false, false },
    { 0x1b1, ChNumFormat::X8Y8Z8W8_Snorm, Swz(Z, Y, X, W), false, false },
    { 0x1b2, ChNumFormat::X4Y4Z4W4_Unorm, Swz(X, Y, Z, One), false, false },
    { 0x1b3, ChNumFormat::X5Y5Z5W1_Unorm, Swz(X, Y, Z, W), false, false },
    { 0x1b4, ChNumFormat::X5Y5Z5W1_Unorm, Swz(X, Y, Z, One), false, false },
    { 0x1b5, ChNumFormat::X5Y5Z5W1_Unorm, Swz(W, X, Y, Z), false, false },
    { 0x1b6, ChNumFormat::X5Y5Z5W1_Unorm, Swz(W, Z, Y, X), false, false },
    { 0x1b7, ChNumFormat::X8Y8Z8W8_Unorm, Swz(W, X, Y, Z), false, false },
    { 0x1b8, ChNumFormat::X8Y8Z8W8_Unorm, Swz(W, Z, Y, X), false, false },
    { 0x1b9, ChNumFormat::X8Y8Z8W8_Snorm, Swz(X, Y, Z, One), false, false },
    { 0x1ba, ChNumFormat::X8Y8Z8W8_Snorm, Swz(Z, Y, X, One), false, false },
    { 0x1bb, ChNumFormat::X8Y8Z8W8_Snorm, Swz(W, X, Y, Z), false, false },
    { 0x1bc, ChNumFormat::X8Y8Z8W8_Snorm, Swz(W, Z, Y, X), false, false },
    { 0x1bd, ChNumFormat::X8Y8Z8W8_Uint, Swz(X, Y, Z, One), false, false },
    { 0x1be, ChNumFormat::X8Y8Z8W8_Uint, Swz(Z, Y, X, W), false, false },
    { 0x1bf, ChNumFormat::X8Y8Z8W8_Uint, Swz(Z, Y, X, One), false, false },
    { 0x1c0, ChNumFormat::X8Y8Z8W8_Uint, Swz(W, X, Y, Z), false, false },
    { 0x1c1, ChNumFormat::X8Y8Z8W8_Uint, Swz(W, Z, Y, X), false, false },
    { 0x1c2, ChNumFormat::X8Y8Z8W8_Sint, Swz(X, Y, Z, One), false, false },
    { 0x1c3, ChNumFormat::X8Y8Z8W8_Sint, Swz(Z, Y, X, W), false, false },
    { 0x1c4, ChNumFormat::X8Y8Z8W8_Sint, Swz(Z, Y, X, One), false, false },
    { 0x1c5, ChNumFormat::X8Y8Z8W8_Sint, Swz(W, X, Y, Z), false, false },
    { 0x1c6, ChNumFormat::X8Y8Z8W8_Sint, Swz(W, Z, Y, X), false, false },
    { 0x1c7, ChNumFormat::X8Y8Z8W8_Srgb, Swz(X, Y, Z, One), false, false },
    { 0x1c8, ChNumFormat::X10Y10Z10W2_Unorm, Swz(X, Y, Z, One), false, false },
    { 0x1c9, ChNumFormat::X10Y10Z10W2_Unorm, Swz(Z, Y, X, One), false, false },
    { 0x1ca, ChNumFormat::X10Y10Z10W2_Unorm, Swz(W, X, Y, Z), false, false },
    { 0x1cb, ChNumFormat::X10Y10Z10W2_Unorm, Swz(W, Z, Y, X), false, false },
    { 0x1cc, ChNumFormat::X10Y10Z10W2_Snorm, Swz(X, Y, Z, W), false, false },
    { 0x1cd, ChNumFormat::X10Y10Z10W2_Snorm, Swz(X, Y, Z, One), false, false },
    { 0x1ce, ChNumFormat::X10Y10Z10W2_Snorm, Swz(Z, Y, X, W), false, false },
    { 0x1cf, ChNumFormat::X10Y10Z10W2_Snorm, Swz(Z, Y, X, One), false, false },
    { 0x1d0, ChNumFormat::X10Y10Z10W2_Snorm, Swz(W, X, Y, Z), false, false },
    { 0x1d1, ChNumFormat::X10Y10Z10W2_Snorm, Swz(W, Z, Y, X), false, false },
    { 0x1d2, ChNumFormat::X10Y10Z10W2_Uint, Swz(X, Y, Z, One), false, false },
    { 0x1d3, ChNumFormat::X10Y10Z10W2_Uint, Swz(Z, Y, X, W), false, false },
    { 0x1d4, ChNumFormat::X10Y10Z10W2_Uint, Swz(Z, Y, X, One), false, false },
    { 0x1d5, ChNumFormat::X10Y10Z10W2_Uint, Swz(W, X, Y, Z), false, false },
    { 0x1d6, ChNumFormat::X10Y10Z10W2_Uint, Swz(W, Z, Y, X), false, false },
    { 0x1d7, ChNumFormat::X10Y10Z10W2_Sint, Swz(X, Y, Z, W), false, false },
    { 0x1d8, ChNumFormat::X10Y10Z10W2_Sint, Swz(X, Y, Z, One), false, false },
    { 0x1d9, ChNumFormat::X10Y10Z10W2_Sint, Swz(Z, Y, X, W), false, false },
    { 0x1da, ChNumFormat::X10Y10Z10W2_Sint, Swz(Z, Y, X, One), false, false },
    { 0x1db, ChNumFormat::X10Y10Z10W2_Sint, Swz(W, X, Y, Z), false, false },
    { 0x1dc, ChNumFormat::X10Y10Z10W2_Sint, Swz(W, Z, Y, X), false, false },
    { 0x1dd, ChNumFormat::X16Y16Z16W16_Unorm, Swz(X, Y, Z, One), false, false },
    { 0x1de, ChNumFormat::X16Y16Z16W16_Unorm, Swz(Z, Y, X, W), false, false },
    { 0x1df, ChNumFormat::X16Y16Z16W16_Unorm, Swz(Z, Y, X, One), false, false },
    { 0x1e0, ChNumFormat::X16Y16Z16W16_Snorm, Swz(X, Y, Z, One), false, false },
    { 0x1e1, ChNumFormat::X16Y16Z16W16_Snorm, Swz(Z, Y, X, W), false, false },
    { 0x1e2, ChNumFormat::X16Y16Z16W16_Snorm, Swz(Z, Y, X, One), false, false },
    { 0x1e3, ChNumFormat::X16Y16Z16W16_Uint, Swz(X, Y, Z, One), false, false },
    { 0x1e4, ChNumFormat::X16Y16Z16W16_Uint, Swz(Z, Y, X, W), false, false },
    { 0x1e5, ChNumFormat::X16Y16Z16W16_Uint, Swz(Z, Y, X, One), false, false },
    { 0x1e6, ChNumFormat::X16Y16Z16W16_Sint, Swz(X, Y, Z, One), false, false },
    { 0x1e7, ChNumFormat::X16Y16Z16W16_Sint, Swz(Z, Y, X, W), false, false },
    { 0x1e8, ChNumFormat::X16Y16Z16W16_Sint, Swz(Z, Y, X, One), false, false },
    { 0x1e9, ChNumFormat::X16Y16Z16W16_Float, Swz(X, Y, Z, One), false, false },
    { 0x1ea, ChNumFormat::X16Y16Z16W16_Float, Swz(Z, Y, X, W), false, false },
    { 0x1eb, ChNumFormat::X16Y16Z16W16_Float, Swz(Z, Y, X, One), false, false },
    { 0x1ec, ChNumFormat::X32Y32Z32W32_Uint, Swz(X, Y, Z, One), false, false },
    { 0x1ed, ChNumFormat::X32Y32Z32W32_Uint, Swz(Z, Y, X, W), false, false },
    { 0x1ee, ChNumFormat::X32Y32Z32W32_Uint, Swz(Z, Y, X, One), false, false },
    { 0x1ef, ChNumFormat::X32Y32Z32W32_Sint, Swz(X, Y, Z, One), false, false },
    { 0x1f0, ChNumFormat::X32Y32Z32W32_Sint, Swz(Z, Y, X, W), false, false },
    { 0x1f1, ChNumFormat::X32Y32Z32W32_Sint, Swz(Z, Y, X, One), false, false },
    { 0x1f2, ChNumFormat::X32Y32Z32W32_Float, Swz(X, Y, Z, One), false, false },
    { 0x1f3, ChNumFormat::X32Y32Z32W32_Float, Swz(Z, Y, X, W), false, false },
    { 0x1f4, ChNumFormat::X32Y32Z32W32_Float, Swz(Z, Y, X, One), false, false },
    { FourCc('P', '0', '1', '0'), ChNumFormat::P010, Swz(X, Y, Z, One), false, false },
    { FourCc('Y', '2', '1', '0'), ChNumFormat::Y210, Swz(X, Y, Z, One), false, false },
    { FourCc('Y', '4', '1', '0'), ChNumFormat::Y410, Swz(X, Y, Z, W), false, false },
    { FourCc('N', 'V', '1', '1'), ChNumFormat::NV11, Swz(X, Y, Z, One), false, false },
    { FourCc('N', 'V', '2', '1'), ChNumFormat::NV21, Swz(X, Y, Z, One), false, false },
    { FourCc('D', 'X', 'T', '1'), ChNumFormat::Bc1_Unorm, Swz(X, Y, Z, W), false, false },
    { FourCc('N', 'V', '1', '2'), ChNumFormat::NV12, Swz(X, Y, Z, One), false, false },
    { FourCc('Y', 'V', '1', '2'), ChNumFormat::YV12, Swz(X, Y, Z, One), false, false },
    { FourCc('A', 'T', 'I', '2'), ChNumFormat::Bc5_Unorm, Swz(Y, X, One, One), false, false },
    { FourCc('D', 'X', 'T', '2'), ChNumFormat::Bc2_Unorm, Swz(X, Y, Z, W), false, false },
    { FourCc('Y', 'U', 'Y', '2'), ChNumFormat::YUY2, Swz(X, Y, Z, One), false, false },
    { FourCc('D', 'X', 'T', '3'), ChNumFormat::Bc2_Unorm, Swz(X, Y, Z, W), false, false },
    { FourCc('D', 'X', 'T', '4'), ChNumFormat::Bc3_Unorm, Swz(X, Y, Z, W), false, false },
    { FourCc('D', 'X', 'T', '5'), ChNumFormat::Bc3_Unorm, Swz(X, Y, Z, W), false, false },
    { FourCc('P', '0', '1', '6'), ChNumFormat::P016, Swz(X, Y, Z, One), false, false },
    { FourCc('Y', '2', '1', '6'), ChNumFormat::Y216, Swz(X, Y, Z, One), false, false },
    { FourCc('Y', '4', '1', '6'), ChNumFormat::Y416, Swz(X, Y, Z, W), false, false },
    { FourCc('G', 'R', 'G', 'B'), ChNumFormat::Y8X8_Y8Z8_Unorm, Swz(X, Y, Z, One), false, false },
    { FourCc('R', 'G', 'B', 'G'), ChNumFormat::X8Y8_Z8Y8_Unorm, Swz(X, Y, Z, One), false, false },
    { FourCc('A', 'Y', 'U', 'V'), ChNumFormat::AYUV, Swz(X, Y, Z, W), false, false },
    { FourCc('U', 'Y', 'V', 'Y'), ChNumFormat::UYVY, Swz(X, Y, Z, One), false, false },
};

bool Gfx12OrLater(const Device& device)
{
    return uint32(device.ChipProperties().gfxLevel) > uint32(GfxIpLevel::GfxIp11_5);
}

// Native 3120c0: open (no residency) and select the entry. Any failure is NotShareable, as the original
// returns -32 whenever no entry was selected, whatever request failed.
Result QueryExternalResource(Device* pDevice, const ExternalResourceOpenInfo& openInfo, size_t minEntryBytes,
                             ExternalResourceQuery* pQuery)
{
    const Status status = OpenSharedResource(pDevice->OsTransport(), pDevice->GetPlatform(), openInfo.hExternalResource,
                                             openInfo.flags.ntHandle != 0, &pQuery->opened);
    TraceResult(ConvertNativeStatus(status), "OpenExternalSharedResource");
    if ((status == Success) &&
        SelectExternalAllocationEntry(pQuery->opened, PhysicalAdapterIndex, &pQuery->allocation, &pQuery->pEntry,
                                      &pQuery->entryBytes) &&
        (pQuery->entryBytes < minEntryBytes))
    {
        // Port bound: the original reads these fields without checking the reported size.
        pQuery->pEntry = nullptr;
    }
    if (pQuery->pEntry == nullptr) { return Result::ErrorNotShareable; }
    ApplyDoppDesktop(openInfo, pQuery);
    return Result::Success;
}

// Native 3124f0: Determine/GetSizes always destroy; Open destroys only on failure.
Result ReleaseExternalResource(Device* pDevice, ExternalResourceQuery* pQuery, bool destroy)
{
    return ConvertNativeStatus(ReleaseSharedResource(pDevice->OsTransport(), pDevice->GetPlatform(),
                                                     &pQuery->opened, destroy));
}

// Native 3826a0 before the image construction: internal create info and the shared-metadata policy that also
// adjusts the create info. GFX9-11 layout only (the GFX12 union member is not part of this build).
void FillExternalImageInternalCreateInfo(const Device& device, const ExternalImageOpenInfo& openInfo,
                                         const uint8* pEntry, ImageCreateInfo* pCreateInfo,
                                         ImageInternalCreateInfo* pInternal)
{
    const GpuChipProperties& chip = device.ChipProperties();
    const uint32 surfaceFlags = Read32(pEntry, Entry::SurfaceFlags);
    InternalImageFlags& flags = pInternal->flags;
    if (chip.hwIpFlags.isSpoofed == 0)
    {
        const uint32 swizzle = Read32(pEntry, Entry::SwizzleMode);
        pInternal->gfx9.sharedSwizzleMode =
            static_cast<AddrSwizzleMode>((swizzle == ADDR_SW_LINEAR_GENERAL) ? ADDR_SW_LINEAR : swizzle);
        if (Read64(pEntry, Entry::DisplayDccPrimary) != 0)
        {
            DccState& dcc = pInternal->gfx9.sharedDccState;
            dcc.primaryOffset = Read64(pEntry, Entry::DisplayDccPrimary);
            dcc.secondaryOffset = Read64(pEntry, Entry::DisplayDccSecond);
            dcc.pitch = Read32(pEntry, Entry::DisplayDccPitch);
            const uint32 dccFlags = Read32(pEntry, Entry::DisplayDccFlags);
            dcc.maxUncompressedBlockSize = (dccFlags >> 2) & 3;
            dcc.maxCompressedBlockSize = (dccFlags >> 4) & 3;
            dcc.independentBlk64B = dccFlags & 1;
            dcc.independentBlk128B = (dccFlags >> 7) & 1; // 0x80/0xc0 set it, 0x40 clears it, 0 keeps zero.
            flags.useSharedDccState = 1;
            pInternal->displayDcc.enabled = 1;
        }
    }
    else
    {
        pInternal->gfx9.sharedSwizzleMode = ADDR_SW_LINEAR;
    }
    pInternal->sharedPipeBankXor[0] = Read32(pEntry, Entry::PipeBankXor0);
    pInternal->sharedPipeBankXor[1] = Read32(pEntry, Entry::PipeBankXor1);
    pInternal->sharedPipeBankXor[2] = Read32(pEntry, Entry::PipeBankXor2);
    flags.crossAdapter = (surfaceFlags >> 14) & 1;
    flags.privateScreenPresent = (openInfo.pScreen != nullptr);
    flags.useSharedTilingOverrides = 1;
    // The original also sets its InternalImageFlags bit 12 from SURF flag bit 5; this PAL version has no such
    // flag (bit 12 is reserved), so it is not represented.

    if ((GpuCaps(device) & SharedMetadataGate) == 0) { return; }
    const uint8* pMetadata = pEntry + Read32(pEntry, Entry::MetadataOffset);
    const uint16 major = Read16(pMetadata, Metadata::VersionMajor);
    const bool preflipCapture = (openInfo.resourceInfo.flags.isDirectCapture != 0) &&
                                (openInfo.resourceInfo.directCaptureInfo.usageFlags.preflip != 0);
    if ((major == 0) || preflipCapture)
    {
        pCreateInfo->flags.optimalShareable = 0;
        pCreateInfo->metadataMode = MetadataMode::Disabled;
        pCreateInfo->metadataTcCompatMode = MetadataTcCompatMode::Disabled;
        return;
    }
    if (major == 1)
    {
        flags.useSharedMetadata = 1;
        SharedMetadataInfo& shared = pInternal->sharedMetadata;
        shared.resourceId = Read64(pMetadata, Metadata::ResourceId);
        // The original's shared-metadata record has single offsets; this PAL describes them as plane 0 of 1.
        shared.numPlanes = 1;
        shared.dccOffset[0] = Read64(pMetadata, Metadata::DccOffset);
        shared.cmaskOffset = Read64(pMetadata, Metadata::CmaskOffset);
        shared.htileOffset = Read64(pMetadata, Metadata::HtileOffset);
        const uint32 metadataFlags = Read32(pMetadata, Metadata::Flags);
        shared.flags.shaderFetchable = metadataFlags & 1;
        shared.flags.hasEqGpuAccess = (metadataFlags >> 3) & 1;
        shared.flags.htileHasDsMetadata = (Read32(pMetadata, Metadata::Version) < 0x10002)
                                              ? (shared.htileOffset != 0) : ((metadataFlags >> 6) & 1);
        shared.fastClearMetaDataOffset[0] = Read64(pMetadata, Metadata::FastClear);
        shared.hisPretestMetaDataOffset = 0;
        if (shared.dccOffset[0] != 0) { shared.dccStateMetaDataOffset[0] = Read64(pMetadata, Metadata::DccState); }
        if (((metadataFlags & 0x20) != 0) && IsGfx10(chip.gfxLevel))
        {
            // htile_as_fmask_xor: the HTILE slot carries the FMASK pipe-bank xor.
            pInternal->gfx9.sharedPipeBankXorFmask = uint32(shared.htileOffset);
            shared.htileOffset = 0;
        }
        shared.fmaskSwizzleMode.v2 = static_cast<AddrSwizzleMode>(Read32(pMetadata, Metadata::FmaskSwizzle));
    }
    pCreateInfo->flags.optimalShareable = 1;
}

// Native 312620 for an image: GPU memory create info from the BO, SURF and metadata records.
bool FillExternalImageMemoryCreateInfo(const Device& device, const ExternalImageOpenInfo& openInfo,
                                       const uint8* pEntry, const Pal::Image& image, GpuMemoryCreateInfo* pInfo,
                                       uint64* pUniqueId)
{
    GpuMemoryCreateInfo& info = *pInfo;
    info.flags.interprocess = 1;
    info.flags.sharedViaNtHandle = openInfo.resourceInfo.flags.ntHandle;
    info.flags.globalGpuVa = openInfo.resourceInfo.flags.globalGpuVa;
    info.size = Read64(pEntry, Entry::BoPhysicalSize);
    info.alignment = Read32(pEntry, Entry::BoAlignment);
    info.priority = GpuMemPriority::High;
    const uint32 heapIdBytes = Read32(pEntry, Entry::BoHeapIdCount) & 7;
    for (uint32 index = 0; index < heapIdBytes; ++index)
    {
        const uint8 id = pEntry[Entry::BoHeapIds + index];
        if (id == 0) { continue; }
        if (info.heapCount == 4) { break; }
        // Port bound: an id beyond the PAL heap enumeration would index PAL's heap tables out of range.
        if (id > GpuHeapCount) { return false; }
        info.heaps[info.heapCount++] = static_cast<GpuHeap>(id - 1);
    }
    info.flags.stereo = (Read32(pEntry, Entry::BoFlags) >> 23) & 1;
    const uint32 attributes = Read16(pEntry, Entry::BoAttributes);
    info.flags.privateScreen = (attributes >> 4) & 1;
    info.flags.globallyCoherent = (attributes >> 3) & 1;
    info.flags.xdmaBuffer = (attributes >> 10) & 1;
    info.flags.peerWritable = (attributes >> 6) & 1;
    // The original additionally consults a device TMZ type table (Device+0x13c4) before setting this flag.
    info.flags.tmzProtected = (attributes >> 5) & 1;
    // Attribute bit 9 maps to an original create flag that this PAL version does not have.
    if (Read32(pEntry, Entry::SurfaceCount) != 0)
    {
        const uint32 surfaceFlags = Read32(pEntry, Entry::SurfaceFlags);
        info.flags.flippable = surfaceFlags & 1;
        info.flags.restrictedContent = (surfaceFlags >> 11) & 1;
        info.flags.restrictedAccess = (surfaceFlags >> 12) & 1;
        info.flags.crossAdapter = (surfaceFlags >> 14) & 1;
    }
    const ImageCreateFlags imageFlags = image.GetImageCreateInfo().flags;
    info.pImage = const_cast<Pal::Image*>(&image);
    info.flags.flippable = imageFlags.flippable;
    info.flags.presentable = imageFlags.presentable;
    info.flags.privateScreen = (image.GetPrivateScreen() != nullptr);
    *pUniqueId = 0;
    const uint32 metadataOffset = Read32(pEntry, Entry::MetadataOffset);
    if (metadataOffset != 0)
    {
        const uint8* pMetadata = pEntry + metadataOffset;
        const uint32 mall = Read32(pMetadata, Metadata::Mall);
        if ((GpuCaps(device) & MallPolicyGate) != 0) { info.mallPolicy = static_cast<GpuMemMallPolicy>(mall & 3); }
        if ((mall & 4) != 0)
        {
            info.flags.mallRangeActive = 1;
            info.mallRange.startPage = Read32(pMetadata, Metadata::MallStartPage);
            info.mallRange.numPages = Read32(pMetadata, Metadata::MallPages);
        }
        // The GFX12 compression tri-state (metadata AllocFlags bit 7) has no field in this build.
        const uint32 allocationFlags = Read32(pMetadata, Metadata::AllocFlags);
        info.flags.kmdShareUmdSysMem = (allocationFlags >> 3) & 1;
        info.flags.deferCpuVaReservation = (allocationFlags >> 6) & 1;
        *pUniqueId = Read64(pMetadata, Metadata::ResourceId);
    }
    return true;
}

// Port addition: structural limits the original never checks on KMD data before PAL indexes per-subresource
// arrays with them. A real resource always passes.
Result CheckExternalImageShape(const ImageCreateInfo& info)
{
    if ((uint32(info.imageType) >= uint32(ImageType::Count)) || (info.mipLevels == 0) || (info.arraySize == 0) ||
        (info.extent.width == 0) || (info.extent.height == 0) || (info.extent.depth == 0))
    {
        return Result::ErrorInvalidValue;
    }
    return Formats::IsUndefined(info.swizzledFormat.format) ? Result::ErrorInvalidFormat : Result::Success;
}

// Native 317dd0: create the DirectCapture allocation (hResource 0, one INFO with VidPnSourceId 0 and no flags) and
// let it replace the selected allocation handle.
Result CreateDirectCaptureAllocation(Device* pDevice, const ExternalImageOpenInfo& openInfo,
                                     ExternalResourceQuery* pQuery)
{
    std::vector<uint8> data;
    uint8 header[64];
    uint32 abiVersion = Private::AbiVersion;
    if (!BuildDirectCaptureAllocation(pQuery->pEntry, pQuery->entryBytes,
                                      openInfo.resourceInfo.directCaptureInfo.usageFlags.u32All, pQuery->address,
                                      pQuery->addressInfo, abiVersion, PhysicalAdapterIndex, &data, header))
    {
        return Result::ErrorNotShareable;
    }
    uint32 handle = 0, resource = 0, share = 0;
    const Status status = CreateAllocationsRequest(pDevice->OsTransport().DeviceHandle(), header, sizeof(header),
                                                   data.data(), uint32(data.size()), 1, nullptr, 0, &handle,
                                                   &resource, &share, 0, 0);
    const Result result = ConvertNativeStatus(status);
    if (result == Result::Success)
    {
        pQuery->allocation = handle;
        pQuery->captureAllocation = handle;
    }
    return result;
}

// Native 3826a0 from the image construction on.
Result CreateExternalImageObjects(Device* pDevice, const ExternalImageOpenInfo& openInfo,
                                  ExternalResourceQuery* pQuery, void* pImagePlacementAddr,
                                  void* pGpuMemoryPlacementAddr, GpuMemoryCreateInfo* pMemCreateInfo,
                                  IImage** ppImage, IGpuMemory** ppGpuMemory)
{
    const uint8* pEntry = pQuery->pEntry;
    ImageCreateInfo createInfo = {};
    FillExternalImageCreateInfo(openInfo, pEntry, Gfx12OrLater(*pDevice), &createInfo);
    ImageInternalCreateInfo internalInfo = {};
    FillExternalImageInternalCreateInfo(*pDevice, openInfo, pEntry, &createInfo, &internalInfo);
    Result result = CheckExternalImageShape(createInfo);
    if (result != Result::Success) { return result; }

    auto* pImage = PAL_PLACEMENT_NEW(pImagePlacementAddr) Image(pDevice, createInfo, internalInfo);
    if ((createInfo.flags.optimalShareable != 0) && (internalInfo.flags.useSharedMetadata == 0))
    {
        pImage->SetSharedResourceId(GpuUtil::GenerateGpuMemoryUniqueId(true));
    }
    // Original Wddm::Image::Init (381d80) adopts the creator's ID for shared metadata before Pal::Image::Init.
    if ((createInfo.flags.optimalShareable != 0) && (internalInfo.flags.useSharedMetadata != 0))
    {
        pImage->SetSharedResourceId(internalInfo.sharedMetadata.resourceId);
    }
    result = pImage->Init();
    if (result != Result::Success)
    {
        pImage->Destroy();
        return result;
    }

    auto* pScreen = static_cast<PrivateScreen*>(openInfo.pScreen);
    uint32 imageId = 0;
    if (pScreen != nullptr)
    {
        result = pScreen->FormatSupported(createInfo.swizzledFormat) ? pScreen->ObtainImageId(&imageId)
                                                                     : Result::ErrorPrivateScreenInvalidFormat;
        if (result == Result::Success)
        {
            pImage->SetPrivateScreen(pScreen);
            pImage->SetPrivateScreenImageId(imageId);
        }
    }

    GpuMemoryCreateInfo memoryInfo = {};
    uint64 uniqueId = 0;
    IGpuMemory* pMemory = nullptr;
    if ((result == Result::Success) &&
        !FillExternalImageMemoryCreateInfo(*pDevice, openInfo, pEntry, *pImage, &memoryInfo, &uniqueId))
    {
        result = Result::ErrorNotShareable;
    }
    if (result == Result::Success)
    {
        Allocation opened = {};
        opened.handle = pQuery->allocation;
        opened.resource = pQuery->opened.resource;
        opened.size = memoryInfo.size;
        // The memory object owns the KMT resource (and a DirectCapture allocation) from here on, on failure too.
        pQuery->opened.resource = 0;
        const uint32 standalone = pQuery->captureAllocation;
        pQuery->captureAllocation = 0;
        // 312620: flags bit0 (Dopp) or bit1 (creator VA) keep query+16 as the VA; only the creator-VA branch also
        // presets the size from query+32.
        ExternalAddress kmdAddress;
        kmdAddress.address = pQuery->address;
        kmdAddress.sizeHint = ((pQuery->flags & 1) == 0) ? pQuery->addressAux : 0;
        result = GpuMemory::AdoptExternalImageMemory(pDevice, opened, uniqueId, openInfo.resourceInfo.hExternalResource,
                                                     memoryInfo, pGpuMemoryPlacementAddr, &pMemory,
                                                     ((pQuery->flags & 3) != 0) ? &kmdAddress : nullptr, standalone);
    }
    if (result == Result::Success)
    {
        // The original binds at offset 0; ExternalImageOpenInfo::gpuMemOffset is not consulted.
        result = pImage->BindGpuMemory(pMemory, 0);
        if (result != Result::Success)
        {
            // The original leaks the memory object here; it owns the resource in this port, so destroy it.
            pMemory->Destroy();
        }
    }
    if (result != Result::Success)
    {
        pImage->Destroy();
        return result;
    }
    if (pScreen != nullptr) { pScreen->SetImageSlot(imageId, pImage); }
    *ppImage = pImage;
    *ppGpuMemory = pMemory;
    if (pMemCreateInfo != nullptr) { *pMemCreateInfo = memoryInfo; }
    return Result::Success;
}
} // anonymous namespace

// =====================================================================================================================
SwizzledFormat TranslateKmdSurfaceFormat(uint32 kmdFormat, bool* pAllViewFormats, bool* pDepthStencil)
{
    SwizzledFormat format = {};
    format.format = ChNumFormat::Undefined;
    format.swizzle.swizzleValue = Swz(Channel::X, Channel::Zero, Channel::Zero, Channel::One);
    bool allViews = false;
    bool depth = false;
    for (const KmdFormat& entry : KmdFormats)
    {
        if (entry.kmdFormat == kmdFormat)
        {
            format.format = entry.format;
            format.swizzle.swizzleValue = entry.swizzle;
            allViews = entry.allViewFormats;
            depth = entry.depthStencil;
            break;
        }
    }
    if (pAllViewFormats != nullptr) { *pAllViewFormats = allViews; }
    if (pDepthStencil != nullptr) { *pDepthStencil = depth; }
    return format;
}

// =====================================================================================================================
bool SelectExternalAllocationEntry(const OpenedSharedResource& opened, uint32 physicalAdapter, uint32* pAllocation,
                                   const uint8** ppEntry, size_t* pEntryBytes)
{
    constexpr uint32 HeaderBytes = 64;
    *ppEntry = nullptr;
    *pEntryBytes = 0;
    // Resource private data ends with a 64-byte adapter trailer: DWORD adapter index at size-64 and, in the
    // byte at size-56, bit 7 for a linked (multi-adapter) resource. Port: shorter data counts as no trailer.
    bool sameAdapter = true;
    if ((opened.resourceData != nullptr) && (opened.resourceBytes >= HeaderBytes))
    {
        const uint8* pTrailer = opened.resourceData + opened.resourceBytes - HeaderBytes;
        if ((pTrailer[8] & 0x80) != 0)
        {
            for (uint32 index = 0; index < opened.allocationCount; ++index)
            {
                uint32 handle = 0;
                const uint8* pData = nullptr;
                uint32 bytes = 0;
                if (SharedResourceAllocation(opened, index, &handle, &pData, &bytes) &&
                    (bytes >= HeaderBytes) && (Read32(pData, 16) == physicalAdapter))
                {
                    *pAllocation = handle;
                    *ppEntry = pData + HeaderBytes;
                    *pEntryBytes = bytes - HeaderBytes;
                    return true;
                }
            }
            return false;
        }
        sameAdapter = (Read32(pTrailer, 0) == physicalAdapter);
    }
    uint32 handle = 0;
    const uint8* pData = nullptr;
    uint32 bytes = 0;
    if (!sameAdapter || !SharedResourceAllocation(opened, 0, &handle, &pData, &bytes) || (bytes < sizeof(uint32)))
    {
        return false;
    }
    if (Read32(pData, 0) == bytes)
    {
        *ppEntry = pData;
        *pEntryBytes = bytes;
    }
    else if (bytes >= HeaderBytes)
    {
        *ppEntry = pData + HeaderBytes;
        *pEntryBytes = bytes - HeaderBytes;
    }
    else
    {
        return false;
    }
    *pAllocation = handle;
    return true;
}

// =====================================================================================================================
void FillExternalImageCreateInfo(const ExternalImageOpenInfo& openInfo, const uint8* pEntry, bool gfx12OrLater,
                                 ImageCreateInfo* pCreateInfo)
{
    ImageCreateInfo& info = *pCreateInfo;
    info.flags.u32All = openInfo.flags.u32All;
    info.usageFlags.u32All = openInfo.usage.u32All;
    info.flags.tmzProtected = (Read16(pEntry, Entry::BoAttributes) >> 5) & 1;
    const uint32 surfaceFlags = Read32(pEntry, Entry::SurfaceFlags);
    info.extent.height = Read32(pEntry, ((surfaceFlags & 0x4000) != 0) ? Entry::AlternateHeight : Entry::Height);
    info.extent.width = Read32(pEntry, Entry::Width);
    info.extent.depth = Read32(pEntry, Entry::Depth);
    info.imageType = static_cast<ImageType>(Read32(pEntry, Entry::ResourceType) - 1);
    if (openInfo.swizzledFormat.format == ChNumFormat::Undefined)
    {
        bool allViews = false;
        bool depth = false;
        info.swizzledFormat = TranslateKmdSurfaceFormat(Read32(pEntry, Entry::Format), &allViews, &depth);
        if (allViews) { info.viewFormatCount = AllCompatibleFormats; }
        info.usageFlags.depthStencil = depth;
    }
    else
    {
        info.swizzledFormat = openInfo.swizzledFormat;
    }
    const uint32 swizzle = Read32(pEntry, Entry::SwizzleMode);
    const bool linear = gfx12OrLater ? (swizzle == 0) : ((swizzle & ~uint32(ADDR_SW_LINEAR_GENERAL)) == 0);
    if (linear && !Formats::IsYuv(info.swizzledFormat.format))
    {
        info.rowPitch = Read32(pEntry, Entry::RowPitch);
        const uint32 slice = Read32(pEntry, Entry::SliceBytes);
        info.depthPitch = (slice != 0) ? slice : (info.extent.height * info.rowPitch);
    }
    info.tiling = linear ? ImageTiling::Linear : ImageTiling::Optimal;
    if ((Read32(pEntry, 4) & 4) != 0)
    {
        // Resource-information section present.
        const uint32 resourceFlags = Read32(pEntry, Entry::ResourceFlags);
        if ((resourceFlags & 0x480) == 0x480)
        {
            info.mipLevels = 1;
            info.arraySize = 1;
        }
        else
        {
            info.mipLevels = Read32(pEntry, Entry::MipLevels);
            info.arraySize = Read32(pEntry, Entry::ArraySize);
        }
        info.samples = Util::Max(1u, uint32(Read16(pEntry, Entry::Samples)));
        info.flags.cubemap = (resourceFlags >> 5) & 1;
        info.flags.presentable = (resourceFlags >> 10) & 1;
        info.usageFlags.shaderRead |= (resourceFlags >> 4) & 1;
        info.usageFlags.shaderWrite |= (resourceFlags >> 13) & 1;
        info.usageFlags.colorTarget |= resourceFlags & 1;
        info.usageFlags.depthStencil |= (resourceFlags >> 3) & 1;
        info.usageFlags.depthAsZ24 |= (resourceFlags >> 24) & 1;
        info.flags.optimalShareable = (resourceFlags >> 23) & 1;
    }
    else
    {
        info.mipLevels = Read32(pEntry, Entry::SurfaceCount);
        info.arraySize = 1;
        info.samples = 1;
        info.usageFlags.shaderRead = 1;
        info.usageFlags.shaderWrite = 1;
        info.usageFlags.colorTarget = (info.usageFlags.depthStencil == 0);
        info.flags.presentable = (surfaceFlags >> 6) & 1;
    }
    if (info.usageFlags.depthStencil != 0)
    {
        ChNumFormat& format = info.swizzledFormat.format;
        if (format == ChNumFormat::X32_Uint) { format = ChNumFormat::X32_Float; }
        else if (format == ChNumFormat::X16_Uint) { format = ChNumFormat::X16_Unorm; }
    }
    info.flags.shareable = 1;
    info.flags.flippable = surfaceFlags & 1;
    if (info.flags.optimalShareable == 0)
    {
        info.viewFormatCount = AllCompatibleFormats;
        info.metadataMode = MetadataMode::Disabled;
        info.metadataTcCompatMode = MetadataTcCompatMode::Disabled;
    }
    // The original's create info has no fragment count; this PAL needs one equal to the sample count.
    info.fragments = info.samples;
    // Its GFX12 compressionMode (entry+0x194) has no field in this build.
}

// =====================================================================================================================
Result DetermineExternalResourceType(Device* pDevice, const ExternalResourceOpenInfo& openInfo, bool* pIsImage)
{
    if (pIsImage == nullptr) { return Result::ErrorInvalidPointer; }
    if (openInfo.flags.isDirectCapture != 0)
    {
        *pIsImage = true;
        return Result::Success;
    }
    ExternalResourceQuery query = {};
    const Result result = QueryExternalResource(pDevice, openInfo, Entry::TypeMinBytes, &query);
    if (result == Result::Success)
    {
        // A surface-less resource, or format 0 with height 1 (a linear buffer SURF), is not an image.
        const uint8* pEntry = query.pEntry;
        *pIsImage = (Read32(pEntry, Entry::SurfaceCount) != 0) &&
                    ((Read32(pEntry, Entry::Format) != 0) || (Read32(pEntry, Entry::Height) != 1));
    }
    return CollapseResults(result, ReleaseExternalResource(pDevice, &query, true));
}

// =====================================================================================================================
Result GetExternalImageSizes(Device* pDevice, const ExternalImageOpenInfo& openInfo, size_t* pImageSize,
                             size_t* pGpuMemorySize, ImageCreateInfo* pImgCreateInfo)
{
    if ((pImageSize == nullptr) || (pGpuMemorySize == nullptr)) { return Result::ErrorInvalidPointer; }
    ExternalResourceQuery query = {};
    const Result result = QueryExternalResource(pDevice, openInfo.resourceInfo, ExternalImageEntryMinBytes, &query);
    if (result == Result::Success)
    {
        ImageCreateInfo createInfo = {};
        FillExternalImageCreateInfo(openInfo, query.pEntry, Gfx12OrLater(*pDevice), &createInfo);
        // The original's GetImageSize body without its validation (311080).
        size_t imageSize = sizeof(Image) + Pal::Image::GetTotalSubresourceSize(*pDevice, createInfo);
        if (pDevice->GetGfxDevice() != nullptr) { imageSize += pDevice->GetGfxDevice()->GetImageSize(createInfo); }
        *pImageSize = imageSize;
        *pGpuMemorySize = pDevice->GpuMemoryObjectSize();
        if (pImgCreateInfo != nullptr) { *pImgCreateInfo = createInfo; }
    }
    return CollapseResults(result, ReleaseExternalResource(pDevice, &query, true));
}

// =====================================================================================================================
Result OpenExternalImage(Device* pDevice, const ExternalImageOpenInfo& openInfo, void* pImagePlacementAddr,
                         void* pGpuMemoryPlacementAddr, GpuMemoryCreateInfo* pMemCreateInfo, IImage** ppImage,
                         IGpuMemory** ppGpuMemory)
{
    if ((pImagePlacementAddr == nullptr) || (pGpuMemoryPlacementAddr == nullptr) || (ppImage == nullptr) ||
        (ppGpuMemory == nullptr))
    {
        return Result::ErrorInvalidPointer;
    }
    ExternalResourceQuery query = {};
    Result result = QueryExternalResource(pDevice, openInfo.resourceInfo, ExternalImageEntryMinBytes, &query);
    const uint8* pEntry = query.pEntry;
    const uint32 metadataOffset = (result == Result::Success) ? Read32(pEntry, Entry::MetadataOffset) : 0;
    if ((result == Result::Success) && !ApplyCreatorAddress(&query)) { result = Result::ErrorNotShareable; }
    if ((result == Result::Success) && (openInfo.resourceInfo.flags.isDirectCapture != 0) &&
        (Read32(pEntry, Entry::Bytes) != 0) && (metadataOffset != 0))
    {
        // Native 317dd0: a further allocation from a patched copy of the entry; it replaces the selected handle.
        result = CreateDirectCaptureAllocation(pDevice, openInfo, &query);
    }
    if ((result == Result::Success) && Gfx12OrLater(*pDevice))
    {
        // The GFX12 internal create info is not part of this build.
        result = Result::Unsupported;
    }
    if (result == Result::Success)
    {
        result = CreateExternalImageObjects(pDevice, openInfo, &query, pImagePlacementAddr, pGpuMemoryPlacementAddr,
                                            pMemCreateInfo, ppImage, ppGpuMemory);
    }
    if (result == Result::Success)
    {
        // 312620 (through 3826a0): the memory object takes the KMT open's global share handle and the NT open's
        // keyed mutex and synchronization object; the keyed mutex slots of the memory object use them.
        static_cast<GpuMemory*>(*ppGpuMemory)->AdoptKeyedMutex(query.opened.globalHandle, query.opened.keyedMutex,
                                                               query.opened.syncObject);
        query.opened.keyedMutex = 0;
        query.opened.syncObject = 0;
    }
    if (query.captureAllocation != 0)
    {
        // Created by 317dd0 but not adopted (a later step failed): released with the opened resource.
        Allocation capture = {};
        capture.handle = query.captureAllocation;
        capture.ordinaryLegacy = true;
        pDevice->OsTransport().DestroyAllocation(&capture);
    }
    return CollapseResults(result, ReleaseExternalResource(pDevice, &query, result != Result::Success));
}

// =====================================================================================================================
void ApplyDoppDesktop(const ExternalResourceOpenInfo& openInfo, ExternalResourceQuery* pQuery)
{
    if (openInfo.flags.isDopp)
    {
        pQuery->flags |= 1;
        pQuery->address = openInfo.doppDesktopInfo.gpuVirtAddr;
        pQuery->addressInfo = openInfo.doppDesktopInfo.vidPnSourceId;
    }
}

// =====================================================================================================================
bool ApplyCreatorAddress(ExternalResourceQuery* pQuery)
{
    const uint8* pEntry = pQuery->pEntry;
    const uint32 metadataOffset = Read32(pEntry, Entry::MetadataOffset);
    if (metadataOffset == 0) { return true; }
    if ((metadataOffset > pQuery->entryBytes) || ((pQuery->entryBytes - metadataOffset) < Metadata::MinBytes))
    {
        return false; // Port bound: the original follows the offset without a size check.
    }
    if ((Read32(pEntry + metadataOffset, Metadata::AllocFlags) & 2) != 0)
    {
        pQuery->flags |= 2;
        pQuery->address = Read64(pEntry + metadataOffset, Metadata::Address);
        pQuery->addressInfo = Read32(pEntry + metadataOffset, Metadata::Size);
        pQuery->addressAux = Read32(pEntry, 8);
    }
    return true;
}

// =====================================================================================================================
bool BuildDirectCaptureAllocation(const uint8* pEntry, size_t entryBytes, uint32 usageFlags, uint64 address,
                                  uint32 addressInfo, uint32 abiVersion, uint32 adapterIndex,
                                  std::vector<uint8>* pAllocationData, uint8 (&resourceHeader)[64])
{
    const uint32 bytes = Read32(pEntry, Entry::Bytes);
    if ((bytes > entryBytes) || (adapterIndex >= 16)) { return false; }
    // The original's zeroed buffer of bytes + 240: allocation header, entry copy, resource header, INFO, CREATE.
    // Its patches use fixed buffer offsets up to 0x2f8; for a short entry they run past the buffer (heap overflow),
    // so this emulation adds slack. What the KMD receives is the entry copy and the resource header.
    std::vector<uint8> buffer(Util::Max<size_t>(size_t(bytes) + 240, 0x300), 0);
    uint8* b = buffer.data();
    auto get32 = [b](size_t at) { uint32 value; std::memcpy(&value, b + at, 4); return value; };
    auto put32 = [b](size_t at, uint32 value) { std::memcpy(b + at, &value, 4); };
    std::memcpy(b + 64, pEntry, bytes);
    put32(208, get32(208) | 0x10);                                         // entry+0x90 resource flags
    put32(252, abiVersion);                                                // entry+0xbc
    put32(80, (get32(80) & 0x7fffffff) | ((usageFlags & 0xfffffff8u) << 28)); // BO flags: shared = usage.shared
    put32(756, get32(756) | (((usageFlags & 0x2e000) != 0) ? 0x1000u : 2u)); // metadata+208 (single-SURF layout)
    std::memcpy(b + 556, &address, sizeof(address));                       // metadata+8
    put32(564, addressInfo);                                               // metadata+16
    // Allocation header (computed by the original although the INFO points past it).
    put32(60, get32(60) | 1);
    put32(16, adapterIndex);
    put32(4 * size_t(adapterIndex), bytes);
    put32(20, 0);
    put32(20, Private::Checksum(b, 64));
    // Resource header after the entry copy.
    const size_t r = size_t(bytes) + 64;
    put32(r + 8, get32(r + 8) | 0x80);
    put32(r + 0, adapterIndex);
    put32(r + 60, bytes + 64);
    put32(r + 12, 0);
    put32(r + 12, Private::Checksum(b + r, 64));
    pAllocationData->assign(b + 64, b + 64 + bytes);
    std::memcpy(resourceHeader, b + r, sizeof(resourceHeader));
    return true;
}
} // namespace Pal::Wddm
