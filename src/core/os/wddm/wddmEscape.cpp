// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmEscape.h"
#include "wddmTransport.h"
#include "wddmResult.h"
#include "core/platform.h"
#include "palSysMemory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
constexpr size_t EnvelopeBytes = 0x4c;
constexpr size_t InlineBytes = 0x800;
}

EscapeRoute DeviceEscapeRoute(const Transport& transport, Pal::Platform* platform)
{
    EscapeRoute route = {};
    route.adapter = transport.AdapterHandle();
    route.device = transport.DeviceHandle();
    route.physicalAdapter = 0; // The verified profile has one physical adapter.
    route.platform = platform;
    return route;
}

void InitPrivateRecord(void* record, size_t bytes, uint32 command)
{
    std::memset(record, 0, bytes);
    PutRecord32(record, 0, 0x80);
    PutRecord32(record, 4, 0x10000);
    PutRecord32(record, 8, command);
}

Result SharedEscape(const EscapeRoute& route, void* record, size_t bytes, bool hardwareAccess, uint32 hContext)
{
    if (route.suppressed) { return Result::Unsupported; }
    const size_t total = bytes + EnvelopeBytes;
    alignas(16) uint8 stackPacket[InlineBytes];
    uint8* packet = stackPacket;
    if (total > InlineBytes)
    {
        if (route.platform == nullptr) { return Result::ErrorOutOfMemory; }
        packet = static_cast<uint8*>(PAL_MALLOC_ALIGNED(total, 1, route.platform, Util::AllocInternalTemp));
        if (packet == nullptr) { return Result::ErrorOutOfMemory; }
    }
    std::memset(packet, 0, total);
    PutRecord32(packet, 0, 2);
    PutRecord32(packet, 4, 0x10002);
    PutRecord32(packet, 0xc, route.physicalAdapter);
    PutRecord32(packet, 0x48, uint32(bytes));
    std::memcpy(packet + EnvelopeBytes, record, bytes);

    D3DKMT_ESCAPE escape = {};
    escape.hAdapter = route.adapter;
    escape.hDevice = route.device;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.Flags.HardwareAccess = hardwareAccess ? 1 : 0;
    escape.pPrivateDriverData = packet;
    escape.PrivateDriverDataSize = UINT(total);
    escape.hContext = hContext;
    const int32 status = (route.escape != nullptr) ? route.escape(route.context, &escape) : D3DKMTEscape(&escape);
    Result result = ConvertNativeStatus(status);
    if (result == Result::Success)
    {
        std::memcpy(record, packet + EnvelopeBytes, bytes);
        if (GetRecord32(record, 0x48) != 0) { result = Result::ErrorUnknown; }
    }
    if (packet != stackPacket) { PAL_FREE(packet, route.platform); }
    return result;
}
}
