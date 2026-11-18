// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmSppProfile.h"
#include <cstring>

namespace Pal::Wddm
{
namespace
{
uint32 Read(const void* data, size_t offset)
{ uint32 value; std::memcpy(&value, static_cast<const uint8*>(data) + offset, 4); return value; }
constexpr char SppBlobName[] = "VulkanBlob";
}

Result EnableSppProfile(uint32 capability, SppProfileStorage* state, const SppProfileCalls& calls,
                        const wchar_t* filename, const wchar_t* pathname)
{
    const bool safe = !calls.replicateNativeHazards;
    if ((capability & 3) != 1) { return Result::Unsupported; }
    const SppQueryReply query = calls.query(calls.context, filename, pathname, ApplicationProfileClient::Spp);
    if (query.result != Result::Success) { return query.result; }
    if (state->copiedBlob != nullptr)
    {
        calls.free(calls.context, state->copiedBlob);
        state->copiedBlob = nullptr;
        state->copiedBytes = 0;
        // Native 310270 keeps selectedPayload pointing into the freed buffer.
        if (safe) { state->selectedPayload = nullptr; }
    }
    state->copiedBlob = calls.allocate(calls.context, query.bytes, 8, 0x80000002);
    // Native copies into the null allocation and faults.
    if (safe && (state->copiedBlob == nullptr)) { return Result::ErrorOutOfMemory; }
    calls.copy(calls.context, state->copiedBlob, query.blob, query.bytes);
    state->copiedBytes = query.bytes;
    if (safe && (query.bytes < 8)) { return query.result; }
    const uint8* const begin = static_cast<const uint8*>(state->copiedBlob);
    const uint8* const end = begin + query.bytes;
    const uint32 budget = Read(state->copiedBlob, 0);
    const uint32 entryCount = Read(state->copiedBlob, 4);
    const uint8* entry = begin + 8;
    uint32 index = 0, consumed = 0;
    if (entryCount != 0)
    {
        while (true)
        {
            // An entry is a 16-byte name, a DWORD payload length and the payload.
            // The selected payload's first two DWORDs are read by Select.
            if (safe && ((entry < begin) || (size_t(end - entry) < 28))) { return query.result; }
            if (std::strcmp(reinterpret_cast<const char*>(entry), SppBlobName) == 0) { break; }
            // This order and 32-bit accumulation match the original body.
            if (consumed < budget)
            {
                ++index;
                const uint64 stride = (uint64(Read(entry, 16)) + 27) & ~uint64(7);
                if (safe && (stride > uint64(end - entry))) { return query.result; }
                entry += stride;
                consumed += uint32(stride);
            }
            // With an exhausted budget the native loop never advances again.
            else if (safe) { return query.result; }
            if (index >= entryCount) { return query.result; }
        }
        state->selectedPayload = entry + 20;
    }
    // A replacement profile without a match keeps the previous selection only in
    // the native replica; the safe path cleared it with the freed buffer above.
    return query.result;
}

Result SelectSppTable(uint32 capability, const SppProfileStorage& state, const SppProfileCalls& calls,
                     uint32 pixelCount, uint32 msaaRate)
{
    // The complete original target never reads either incoming argument.
    (void)pixelCount;
    (void)msaaRate;
    if ((capability & 3) != 1) { return Result::Unsupported; }
    if (state.selectedPayload == nullptr) { return Result::NotFound; }
    void* record = calls.allocate(calls.context, 248, 8, 0x80000002);
    // Native skips the memset and faults storing DWORD+12 into the null record.
    if (!calls.replicateNativeHazards && (record == nullptr)) { return Result::ErrorOutOfMemory; }
    if (record != nullptr) { calls.zero(calls.context, record, 248); }
    // Opaque write dependency preserves the native null+0xc store ordering.
    // A compiler must not use undefined null arithmetic in an inlined memcpy
    // to move/elide the original conditional memset across that fault.
    calls.write(calls.context, record, 12, 3);
    calls.write(calls.context, record, 4, 0x10000);
    calls.write(calls.context, record, 0, 128);
    calls.write(calls.context, record, 8, 0x03009b03);
    calls.write(calls.context, record, 128, 80);
    calls.write(calls.context, record, 132, Read(state.selectedPayload, 0));
    calls.write(calls.context, record, 136, Read(state.selectedPayload, 4));
    calls.write(calls.context, record, 196, 0);
    calls.write(calls.context, record, 200, 0);
    calls.write(calls.context, record, 212, 32);
    const Result result = calls.escape(calls.context, record, 248);
    calls.free(calls.context, record);
    return result;
}
}
