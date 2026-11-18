// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmClock.h"
#include "wddmTransport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <array>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
// Exact DLL SHA256 36a9ef32d75476aa154acab3c79c0d1b6ef8ffe36b378239f5a48eee851b7a44:
// Device vslot32, RVA3105a0 builds this record; RVA30f860 adds the escape envelope.
// D3DKMTEscape is resolved at RVA301cb1 into dispatch slot2930, called at30f9ad.
constexpr size_t EnvelopeBytes = 0x4c;
constexpr size_t ClockRecordBytes = 0x108;

void Put32(uint8_t* bytes, size_t offset, uint32_t value)
{
    std::memcpy(bytes + offset, &value, sizeof(value));
}
uint32_t Get32(const uint8_t* bytes, size_t offset)
{
    uint32_t value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
uint64_t Get64(const uint8_t* bytes, size_t offset)
{
    uint64_t value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
}

Result QueryCalibratedTimestamps(const Transport& transport, CalibratedTimestamps* timestamps)
{
    if (timestamps == nullptr) { return Result::ErrorInvalidPointer; }
    if (!transport.IsOpen()) { return Result::ErrorUnavailable; }
    if (!Transport::IsVerifiedAdapter(transport.Identity())) { return Result::Unsupported; }

    // This profile contains one physical adapter. The original places its
    // physical-adapter ordinal in envelope+0xc; no LDA profile is enabled here.
    std::array<uint8_t, EnvelopeBytes + ClockRecordBytes> packet = {};
    Put32(packet.data(), 0, 2);
    Put32(packet.data(), 4, 0x10002);
    Put32(packet.data(), 0x0c, 0);
    Put32(packet.data(), 0x48, ClockRecordBytes);
    uint8_t* record = packet.data() + EnvelopeBytes;
    Put32(record, 0, 0x80);
    Put32(record, 4, 0x10000);
    Put32(record, 8, 0x03005003);
    Put32(record, 0x0c, 3);
    Put32(record, 0x80, 0x40);
    Put32(record, 0x84, 0x40);
    Put32(record, 0xc4, 0x40);
    Put32(record, 0xc8, 0x40);

    D3DKMT_ESCAPE request = {};
    request.hAdapter = transport.AdapterHandle();
    request.hDevice = transport.DeviceHandle();
    request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    request.pPrivateDriverData = packet.data();
    request.PrivateDriverDataSize = static_cast<UINT>(packet.size());
    // Native query: HardwareAccess0, context0, and private return-code0 required.
    if ((D3DKMTEscape(&request) != 0) || (Get32(record, 0x48) != 0))
    {
        return Result::ErrorUnavailable;
    }

    const uint64_t cpuBefore = Get64(record, 0xd0);
    timestamps->cpuQueryPerfCounterTimestamp = cpuBefore;
    // Preserve the recovered subtraction. The native method does not perform a
    // QPC-frequency conversion here; see the clock evidence before changing units.
    timestamps->maxDeviation = Get64(record, 0xe0) - cpuBefore;
    timestamps->gpuTimestamp = Get64(record, 0xd8);
    return Result::Success;
}
}
