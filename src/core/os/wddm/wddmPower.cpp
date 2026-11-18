// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmPower.h"
#include "wddmTransport.h"
#include "wddmResult.h"
#include "core/privateScreen.h"

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
constexpr size_t EnvelopeBytes = 0x4c;
constexpr size_t PowerRecordBytes = 0xe8;
using PowerRecord = std::array<uint8_t, PowerRecordBytes>;
void Put32(uint8_t* bytes, size_t offset, uint32 value)
{ std::memcpy(bytes + offset, &value, sizeof(value)); }
void Put64(uint8_t* bytes, size_t offset, uint64 value)
{ std::memcpy(bytes + offset, &value, sizeof(value)); }
uint32 Get32(const uint8_t* bytes, size_t offset)
{ uint32 value; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }
uint64 Get64(const uint8_t* bytes, size_t offset)
{ uint64 value; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }
PowerRecord MakePowerRecord(uint32 command)
{
    PowerRecord record = {};
    Put32(record.data(), 0, 0x80);
    Put32(record.data(), 4, 0x10000);
    Put32(record.data(), 8, command);
    Put32(record.data(), 0xc, 3);
    Put32(record.data(), 0x80, 0x40);
    Put32(record.data(), 0xc4, 0x20);
    return record;
}
}

Result ReplayNativePowerRecord(const Transport& transport, void* rawRecord, size_t bytes, PowerRequestTrace* trace)
{
    if (rawRecord == nullptr) { return Result::ErrorInvalidPointer; }
    if (bytes != PowerRecordBytes) { return Result::ErrorInvalidMemorySize; }
    auto* record = static_cast<uint8_t*>(rawRecord);
    if (trace != nullptr)
    {
        *trace = {};
        std::memcpy(trace->request.data(), record, bytes);
    }
    if (!transport.IsOpen()) { return Result::ErrorUnavailable; }
    if (!Transport::IsVerifiedAdapter(transport.Identity())) { return Result::Unsupported; }
    std::array<uint8_t, EnvelopeBytes + PowerRecordBytes> packet = {};
    Put32(packet.data(), 0, 2);
    Put32(packet.data(), 4, 0x10002);
    Put32(packet.data(), 0xc, 0); // Verified single physical adapter.
    Put32(packet.data(), 0x48, static_cast<uint32>(bytes));
    std::memcpy(packet.data() + EnvelopeBytes, record, bytes);
    D3DKMT_ESCAPE escape = {};
    escape.hAdapter = transport.AdapterHandle();
    escape.hDevice = transport.DeviceHandle();
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.pPrivateDriverData = packet.data();
    escape.PrivateDriverDataSize = static_cast<UINT>(packet.size());
    const Status status = D3DKMTEscape(&escape);
    if (trace != nullptr) { trace->ntStatus = status; }
    const Result result = ConvertNativeStatus(status);
    if (result != Result::Success) { return result; }
    std::memcpy(record, packet.data() + EnvelopeBytes, bytes);
    const uint32 privateResult = Get32(record, 0x48);
    if (trace != nullptr) { trace->privateResult = privateResult; trace->responseCopied = true; }
    return privateResult == 0 ? Result::Success : Result::ErrorUnknown;
}

Result ApplyPowerProfile(const Transport& transport, PowerProfile profile, CustomPowerProfile* info,
                         PowerRequestTrace* trace)
{
    // Native Device vslot135, RVA313940..313af4. Custom pointer/count guards
    // remain explicit deviations; unknown enum dispatch follows the original.
    if ((profile == PowerProfile::VrCustom) && ((info == nullptr) || (info->pScreen == nullptr)))
    { return Result::ErrorInvalidPointer; }
    if ((profile == PowerProfile::VrCustom) && (info->numSwitchInfo > MaxNumPowerSwitchInfo))
    { return Result::ErrorInvalidValue; }
    PowerRecord record = MakePowerRecord(0x03007203);
    std::array<std::array<uint32, 8>, MaxNumPowerSwitchInfo> switches = {};
    switch (profile)
    {
    case PowerProfile::Default: break;
    case PowerProfile::VrDefault:
        Put32(record.data(), 0x88, 1);
        break;
    case PowerProfile::Idle:
        Put32(record.data(), 0x84, 2);
        break;
    case PowerProfile::VrCustom:
        Put32(record.data(), 0x84, 1);
        Put32(record.data(), 0x8c, info->numSwitchInfo);
        for (uint32 i = 0; i < info->numSwitchInfo; ++i)
        {
            switches[i][0] = info->switchInfo[i].time;
            switches[i][1] = info->switchInfo[i].performance;
        }
        Put64(record.data(), 0x90, reinterpret_cast<uintptr_t>(switches.data()));
        // The native reads the screen object's DWORD+18, the current core's
        // PrivateScreen::Index member. It does not use properties.targetId.
        Put32(record.data(), 0x98, static_cast<PrivateScreen*>(info->pScreen)->Index());
        break;
    // Original313940 sends the zero/default record for every unrecognized
    // non-custom enum value, including Count and UINT32_MAX.
    default: break;
    }
    const Result result = ReplayNativePowerRecord(transport, record.data(), record.size(), trace);
    const auto* actual = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(Get64(record.data(), 0xc8)));
    // Native copies a returned custom schedule even if the private return code
    // is nonzero; preserve that policy. This is a borrowed pointer consumed
    // immediately, while the input staging array and outer response record live.
    if ((actual != nullptr) && (profile == PowerProfile::VrCustom))
    {
        for (uint32 i = 0; i < info->numSwitchInfo; ++i)
        {
            info->actualSwitchInfo[i].performance = Get32(actual, size_t(i) * 32 + 4);
            info->actualSwitchInfo[i].time = Get32(actual, size_t(i) * 32);
        }
    }
    return result;
}

Result ApplyMlPowerOptimization(const Transport& transport, bool enableOptimization, PowerRequestTrace* trace)
{
    // Native Device vslot136, RVA313b00..313bbe.
    PowerRecord record = MakePowerRecord(0x03006903);
    record[0x84] = !enableOptimization;
    Put32(record.data(), 0x88, 2);
    return ReplayNativePowerRecord(transport, record.data(), record.size(), trace);
}
}
