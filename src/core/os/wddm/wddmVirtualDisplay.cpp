// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Records are {0x80, 0x10000, command, 3, ..., inputBytes @+0x80, input @+0x84,
// outputBytes, output} through the shared escape 30f860. None of these replies
// carries a private status that the original checks.
#include "core/os/wddm/wddmVirtualDisplay.h"
#include "core/privateScreen.h"

#include <array>
#include <cstddef>

namespace Pal::Wddm
{
namespace
{
static_assert(offsetof(VirtualDisplayInfo, vsyncMode) == 0x10 && offsetof(VirtualDisplayInfo, pPrivateScreen) == 0x18);

constexpr uint32 CmdCreateVirtualDisplay  = 0x03008a03;
constexpr uint32 CmdDestroyVirtualDisplay = 0x03008b03;
constexpr uint32 CmdVirtualDisplayInfo    = 0x03008c03;
constexpr uint32 CmdVirtualDisplayCaps    = 0x03009d03;

void InitDisplayRecord(void* pRecord, size_t bytes, uint32 command, uint32 inputBytes, uint32 outputBytes)
{
    InitPrivateRecord(pRecord, bytes, command);
    PutRecord32(pRecord, 0xc, 3);
    PutRecord32(pRecord, 0x80, inputBytes);
    PutRecord32(pRecord, 0x84 + inputBytes, outputBytes);
}
} // anonymous namespace

// =====================================================================================================================
VirtualDisplayCapabilities QueryVirtualDisplayCapabilities(
    const EscapeRoute& route)
{
    std::array<uint8, 0xc8> record;
    InitDisplayRecord(record.data(), record.size(), CmdVirtualDisplayCaps, 0x20, 0x20);
    const Result result = SharedEscape(route, record.data(), record.size());

    VirtualDisplayCapabilities caps = {};
    caps.maxVirtualDisplays         = (result == Result::Success) ? GetRecord32(record.data(), 0xa8) : 1;
    caps.minRefreshRate.numerator   = 45;
    caps.minRefreshRate.denominator = 1;
    caps.maxRefreshRate.numerator   = 240;
    caps.maxRefreshRate.denominator = 1;
    return caps;
}

// =====================================================================================================================
Result CreateKmdVirtualDisplay(
    const EscapeRoute&                route,
    const VirtualDisplayCapabilities& caps,
    const VirtualDisplayInfo&         info,
    uint32*                           pScreenTargetId)
{
    if (caps.maxVirtualDisplays == 0)
    {
        return Result::Unsupported;
    }
    // Deviation: the original dereferences pPrivateScreen (HMD vsync) before, and pScreenTargetId after, the escape
    // without checks; a null pointer is refused before any display is created.
    if ((pScreenTargetId == nullptr) ||
        ((info.vsyncMode == VirtualDisplayVSyncMode::HMD) && (info.pPrivateScreen == nullptr)))
    {
        return Result::ErrorInvalidPointer;
    }

    std::array<uint8, 0x108> record;
    InitDisplayRecord(record.data(), record.size(), CmdCreateVirtualDisplay, 0x60, 0x20);
    PutRecord32(record.data(), 0x84, info.width);
    PutRecord32(record.data(), 0x88, info.height);
    PutRecord32(record.data(), 0x8c, info.refreshRate.numerator);
    PutRecord32(record.data(), 0x90, info.refreshRate.denominator);
    PutRecord32(record.data(), 0x94, static_cast<uint32>(info.vsyncMode));
    PutRecord32(record.data(), 0x98, info.vsyncOffset);
    if (info.vsyncMode == VirtualDisplayVSyncMode::HMD)
    {
        // The HMD is named by its display index (PrivateScreen+0x18), not its target id.
        PutRecord32(record.data(), 0x9c, static_cast<const PrivateScreen*>(info.pPrivateScreen)->Index());
    }

    const Result result = SharedEscape(route, record.data(), record.size());
    if (result == Result::Success)
    {
        *pScreenTargetId = GetRecord32(record.data(), 0xe8);
    }
    return result;
}

// =====================================================================================================================
Result DestroyKmdVirtualDisplay(
    const EscapeRoute& route,
    uint32             screenTargetId)
{
    std::array<uint8, 0xc8> record;
    InitDisplayRecord(record.data(), record.size(), CmdDestroyVirtualDisplay, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, screenTargetId);
    return SharedEscape(route, record.data(), record.size());
}

// =====================================================================================================================
Result QueryKmdVirtualDisplayProperties(
    const EscapeRoute&        route,
    uint32                    screenTargetId,
    VirtualDisplayProperties* pProperties)
{
    // Deviation: the original writes through pProperties after the escape without a check.
    if (pProperties == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }
    std::array<uint8, 0xc8> record;
    InitDisplayRecord(record.data(), record.size(), CmdVirtualDisplayInfo, 0x20, 0x20);
    PutRecord32(record.data(), 0x84, screenTargetId);
    const Result result = SharedEscape(route, record.data(), record.size());
    if (result == Result::Success)
    {
        pProperties->isVirtualDisplay = (record[0xa8] == 1);
    }
    return result;
}
} // Pal::Wddm
