// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmClockMode.h"
#include "wddmTransport.h"
#include "wddmResult.h"

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
constexpr size_t ModeRecordBytes = 0xc8;
using ModeRecord = std::array<uint8_t, ModeRecordBytes>;
void Put32(uint8_t* data, size_t offset, uint32 value)
{ std::memcpy(data + offset, &value, sizeof(value)); }
uint32 Get32(const uint8_t* data, size_t offset)
{
    uint32 value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}
ModeRecord MakeModeRecord(uint32 command, uint32 mode)
{
    ModeRecord record = {};
    Put32(record.data(), 0, 0x80);
    Put32(record.data(), 4, 0x10000);
    Put32(record.data(), 8, command);
    Put32(record.data(), 0xc, 3);
    Put32(record.data(), 0x80, 0x20);
    Put32(record.data(), 0x84, mode);
    Put32(record.data(), 0xa4, 0x20);
    return record;
}
template<size_t RecordBytes>
Result ModeEscape(const Transport& transport, std::array<uint8_t, RecordBytes>* record, bool hardwareAccess)
{
    // Original envelope helper RVA30f860; this selected profile has physical adapter0.
    std::array<uint8_t, EnvelopeBytes + RecordBytes> packet = {};
    Put32(packet.data(), 0, 2);
    Put32(packet.data(), 4, 0x10002);
    Put32(packet.data(), 0xc, 0);
    Put32(packet.data(), 0x48, static_cast<uint32>(RecordBytes));
    std::memcpy(packet.data() + EnvelopeBytes, record->data(), record->size());
    D3DKMT_ESCAPE escape = {};
    escape.hAdapter = transport.AdapterHandle();
    escape.hDevice = transport.DeviceHandle();
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.Flags.HardwareAccess = hardwareAccess;
    escape.pPrivateDriverData = packet.data();
    escape.PrivateDriverDataSize = static_cast<UINT>(packet.size());
    const Result result = ConvertNativeStatus(D3DKMTEscape(&escape));
    if (result != Result::Success) { return result; }
    std::memcpy(record->data(), packet.data() + EnvelopeBytes, record->size());
    return (Get32(record->data(), 0x48) == 0) ? Result::Success : Result::ErrorUnknown;
}
}

Result QueryClockLimits(const Transport& transport, GpuType gpuType, SetClockModeOutput* output)
{
    if (output == nullptr) { return Result::ErrorInvalidPointer; }
    if (!transport.IsOpen()) { return Result::ErrorUnavailable; }
    if (!Transport::IsVerifiedAdapter(transport.Identity())) { return Result::Unsupported; }
    const auto& physical = transport.Identity().physical;
    output->memoryClockFrequency = physical.fallbackMaxMemoryClockMHz;
    output->engineClockFrequency = physical.fallbackMaxEngineClockMHz;
    // Native30b79f..30b840 prefers this live capability record; only an Escape
    // failure selects the two GPU-record WORDs already installed above.
    std::array<uint8_t, 0x398> record = {};
    Put32(record.data(), 0, 0x80);
    Put32(record.data(), 4, 0x10000);
    Put32(record.data(), 8, 0x03000000);
    Put32(record.data(), 0x4c, 0xbadcaffe);
    Put32(record.data(), 0x80, 0x10);
    Put32(record.data(), 0x84, 0x10);
    Put32(record.data(), 0x88, 0xc0009b);
    Put32(record.data(), 0x94, 0x300);
    const Result result = ModeEscape(transport, &record, false);
    if (result == Result::Success)
    {
        output->engineClockFrequency = Get32(record.data(), 0x98);
        output->memoryClockFrequency = Get32(record.data(), gpuType == GpuType::Integrated ? 0xf8 : 0xc0);
    }
    return result;
}

SetClockModeOutput DecodeClockModeReply(uint32 memoryRatio, uint32 engineRatio,
                                       uint32 absoluteMemoryMHz, uint32 absoluteEngineMHz,
                                       uint32 maxMemoryMHz, uint32 maxEngineMHz)
{
    SetClockModeOutput output = {};
    if ((absoluteMemoryMHz != 0) && (absoluteEngineMHz != 0))
    {
        output.memoryClockFrequency = absoluteMemoryMHz;
        output.engineClockFrequency = absoluteEngineMHz;
    }
    else
    {
        // Native314450..3144bd: float32 Q1.31, then truncate to64 bits and
        // retain its low DWORD. All uint32 inputs remain in the64-bit range.
        constexpr float RatioScale = 0x1p-31f;
        const float memoryScale = static_cast<float>(memoryRatio) * RatioScale;
        const float engineScale = static_cast<float>(engineRatio) * RatioScale;
        output.memoryClockFrequency = static_cast<uint32>(static_cast<uint64>(
            static_cast<float>(maxMemoryMHz) * memoryScale));
        output.engineClockFrequency = static_cast<uint32>(static_cast<uint64>(
            static_cast<float>(maxEngineMHz) * engineScale));
    }
    return output;
}

Result ApplyClockMode(const Transport& transport, const SetClockModeInput& input,
                     SetClockModeOutput* output, ClockModePolicy policy,
                     uint32 maxMemoryClockMHz, uint32 maxEngineClockMHz)
{
    // Exact native branch policy at314371..3143a6; IgnoreAll is an intentional
    // native setting bypass, including preservation of the optional output.
    if (policy == ClockModePolicy::IgnoreAll) { return Result::Success; }
    const bool query = (input.clockMode == DeviceClockMode::Query) ||
                       (input.clockMode == DeviceClockMode::QueryProfiling) ||
                       (input.clockMode == DeviceClockMode::QueryPeak);
    if (!query && (policy == ClockModePolicy::QueryOnly)) { return Result::ErrorUnavailable; }
    if (!transport.IsOpen()) { return Result::ErrorUnavailable; }
    if (!Transport::IsVerifiedAdapter(transport.Identity())) { return Result::Unsupported; }

    ModeRecord record = MakeModeRecord(0x03007a03, static_cast<uint32>(input.clockMode));
    const Result result = ModeEscape(transport, &record, false);
    if (result != Result::Success) { return result; }
    if (output != nullptr)
    {
        *output = DecodeClockModeReply(Get32(record.data(), 0xa8), Get32(record.data(), 0xac),
                                       Get32(record.data(), 0xb0), Get32(record.data(), 0xb4),
                                       maxMemoryClockMHz, maxEngineClockMHz);
    }
    if (!query)
    {
        record = MakeModeRecord(0x0300fc03, input.clockMode == DeviceClockMode::Default);
        // Native31452a deliberately ignores this second request's result after
        // committing the first request's output. Preserve that selected policy.
        static_cast<void>(ModeEscape(transport, &record, true));
    }
    return Result::Success;
}
}
