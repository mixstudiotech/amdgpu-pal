// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmStaticVmid.h"
#include "wddmTransport.h"
#include "wddmResult.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
void Put32(uint8_t* bytes, size_t offset, uint32_t value) { std::memcpy(bytes + offset, &value, 4); }
uint32_t Get32(const uint8_t* bytes, size_t offset) { uint32_t value; std::memcpy(&value, bytes + offset, 4); return value; }
}
StaticVmidRecord MakeStaticVmidRecord(bool enable)
{
    // Complete original OS method 314570..314649.
    StaticVmidRecord record = {};
    Put32(record.data(), 0, 128);
    Put32(record.data(), 4, 0x10000);
    Put32(record.data(), 8, 0x03008603);
    Put32(record.data(), 12, 3);
    Put32(record.data(), 0x80, 64);
    Put32(record.data(), 0x84, enable ? 1 : 2);
    Put32(record.data(), 0xc4, 64);
    return record;
}
Result DecodeStaticVmidResult(Result escapeResult, const StaticVmidRecord& response)
{
    return (escapeResult == Result::Success && (response[0xd0] & 4) != 0)
        ? Result::Success : Result::ErrorStaticVmidOpFailed;
}
Result ReplayStaticVmidRecord(const Transport& transport, StaticVmidRecord* record)
{
    if (record == nullptr) { return Result::ErrorInvalidPointer; }
    if (!transport.IsOpen()) { return Result::ErrorUnavailable; }
    if (!Transport::IsVerifiedAdapter(transport.Identity())) { return Result::Unsupported; }
    std::array<uint8_t, 0x4c + 264> packet = {};
    Put32(packet.data(), 0, 2); Put32(packet.data(), 4, 0x10002);
    Put32(packet.data(), 0x48, uint32_t(record->size()));
    std::memcpy(packet.data() + 0x4c, record->data(), record->size());
    D3DKMT_ESCAPE escape = {};
    escape.hAdapter = transport.AdapterHandle(); escape.hDevice = transport.DeviceHandle();
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.pPrivateDriverData = packet.data(); escape.PrivateDriverDataSize = uint32_t(packet.size());
    const Result result = ConvertNativeStatus(D3DKMTEscape(&escape));
    if (result != Result::Success) { return result; }
    std::memcpy(record->data(), packet.data() + 0x4c, record->size());
    return Get32(record->data(), 0x48) == 0 ? Result::Success : Result::ErrorUnknown;
}
Result ApplyStaticVmid(const Transport& transport, bool enable)
{
    auto record = MakeStaticVmidRecord(enable);
    return DecodeStaticVmidResult(ReplayStaticVmidRecord(transport, &record), record);
}
}
