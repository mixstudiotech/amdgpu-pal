// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmWorkstation.h"
#include "wddmTransport.h"
#include "wddmResult.h"
#include "wddmApplicationProfile.h"
#include <cstring>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
namespace
{
uint32 Read32(const uint8_t* bytes, size_t offset)
{ uint32 value; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }
void Put32(uint8_t* bytes, size_t offset, uint32 value)
{ std::memcpy(bytes + offset, &value, sizeof(value)); }
int32 RawStereoEscape(void*, void* request)
{ return D3DKMTEscape(static_cast<D3DKMT_ESCAPE*>(request)); }
}
uint32 DecodeWorkstationCapabilities(const PhysicalProperties& properties)
{
    // Original 300a20 uses pointers after each 2400-byte private-record header.
    // Its SIMD sum is DWORD addition of (record[i] XOR i), with the checksum
    // DWORD first zeroed. Preserve that arithmetic without mutating our snapshot.
    const auto* gpu = properties.gpuRecord.data();
    const uint32 stored = Read32(gpu, 14208);
    uint32 checksum = 0;
    for (uint32 index = 0; index < 0xe80; ++index)
    {
        const size_t offset = 2400 + size_t(index) * 4;
        checksum += ((offset == 14208) ? 0 : Read32(gpu, offset)) ^ index;
    }
    if (((stored == 0) || (stored == checksum)) && ((Read32(gpu, 14176) & 0x10000) != 0))
    { return Read32(gpu, 14196) & 7; }
    return (Read32(properties.basicRecord.data(), 2900) >> 5) & 1;
}
Result QueryWorkstationCapabilities(const Transport& transport, WorkStationCaps* caps)
{
    if (caps == nullptr) { return Result::ErrorInvalidPointer; }
    caps->u32All = 0; // Native Device wrapper 313bc0 clears the complete output.
    if (!transport.IsOpen()) { return Result::ErrorUnavailable; }
    if (!Transport::IsVerifiedAdapter(transport.Identity())) { return Result::Unsupported; }
    PhysicalProperties current;
    const Result result = ConvertNativeStatus(QueryPhysicalProperties(transport.AdapterHandle(), &current));
    if (result == Result::Success) { caps->u32All = DecodeWorkstationCapabilities(current); }
    return result;
}
Result QueryWorkstationStereoModeRecord(const ApplicationProfileEscapeCalls& route, WorkstationStereoMode* mode)
{
    if (mode == nullptr) { return Result::ErrorInvalidPointer; }
    std::array<uint8, 264> record = {};
    Put32(record.data(), 0, 0x80);
    Put32(record.data(), 4, 0x10000);
    Put32(record.data(), 8, 0x03004103);
    Put32(record.data(), 0x80, 64);
    Put32(record.data(), 0xc4, 64);
    const Result result = EscapeApplicationProfileRecord(route, record.data(), record.size());
    if (result == Result::Success) { *mode = static_cast<WorkstationStereoMode>(Read32(record.data(), 0xd0)); }
    return result;
}
Result QueryWorkstationStereoMode(const Transport& transport, WorkstationStereoMode* mode)
{
    const ApplicationProfileEscapeCalls route = {nullptr, RawStereoEscape,
        transport.AdapterHandle(), transport.DeviceHandle(), 0, false};
    return QueryWorkstationStereoModeRecord(route, mode);
}
}
