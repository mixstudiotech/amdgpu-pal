// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "wddmTransport.h"
#include <array>
#include <cstring>
#include <limits>

namespace Pal::Wddm::Private
{
// Reconstructed AMD private protocol, not a public AMD API. Provenance:
// AnyPS5 driver/runtime/src/backend/wddm/private.rs and mesa-wddm HEAD
// 801c9763c6043f0de8408e905a5324eea06d81d7, radv_wddm2_{bo,cs}.c.
// AMDVLK artifact SHA256: 36a9ef32d75476aa154acab3c79c0d1b6ef8ffe36b378239f5a48eee851b7a44.
// Neutral names are intentional where the meanings of private flag bits are unknown.
inline constexpr uint32_t AbiVersion = 9;
inline constexpr uint32_t VendorId = 0x1002;
inline constexpr uint32_t DeviceId = 0x1586;
inline constexpr uint32_t RevisionId = 0xc1;
inline constexpr uint64_t DriverVersion = (uint64_t(32) << 48) | (uint64_t(32015) << 16) | 7029;
inline constexpr size_t ContextBytes = 424;
inline constexpr size_t QueueBytes = 64;
inline constexpr size_t AllocationPrivateBytes = 64;
inline constexpr size_t HeaderBytes = 64;
inline constexpr size_t EntryBytes = 196;
inline constexpr size_t MetadataBytes = 276;
inline constexpr size_t RecordsBytes = HeaderBytes + EntryBytes + MetadataBytes;
inline constexpr size_t MaxIbs = 64;

inline void Put32(uint8_t* p, size_t at, uint32_t value) { std::memcpy(p + at, &value, 4); }
inline void Put64(uint8_t* p, size_t at, uint64_t value) { std::memcpy(p + at, &value, 8); }
inline uint32_t Get32(const uint8_t* p, size_t at)
{ uint32_t value; std::memcpy(&value, p + at, 4); return value; }
inline uint64_t Get64(const uint8_t* p, size_t at)
{ uint64_t value; std::memcpy(&value, p + at, 8); return value; }
inline uint32_t Checksum(const uint8_t* p, size_t bytes)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < bytes / 4; ++i) { sum += uint32_t(i) ^ Get32(p, i * 4); }
    return sum;
}
inline std::array<uint8_t, ContextBytes> Context(bool compute)
{
    std::array<uint8_t, ContextBytes> data = {};
    Put32(data.data(), 0, 0x40);
    Put32(data.data(), 4, compute ? 0x100000 : 0x140000);
    Put32(data.data(), 52, compute ? 0 : AbiVersion);
    Put32(data.data(), 56, 0x200000);
    Put32(data.data(), 64, 0x168);
    return data;
}
inline std::array<uint8_t, QueueBytes> QueueData(uint32_t priority = 0)
{
    // Native RVA37a980 (chained ranges through37b064), table6be48d0:
    // Normal, Idle, Medium, High, Realtime map to1,0,2,3,4 respectively.
    // Priority occupies bits12..14 of the AMD HW-queue private DWORD+16.
    constexpr uint32_t Priorities[] = { 1, 0, 2, 3, 4 };
    std::array<uint8_t, QueueBytes> data = {};
    Put32(data.data(), 0, QueueBytes);
    Put32(data.data(), 16, 0x20000 | (Priorities[priority] << 12));
    return data;
}
// Native 37a980 HW-queue DWORD+16 for the creation options (37b070 builds the same value | 0x80000 for an escape-
// registered follower). Bit17 is Device+0x2380 bit40, set on the gated profile as in QueueData. The low nibble is 2
// only when Device+0x1140 bit12 and the engine flag bit17 are both set, which this backend does not set.
inline uint32_t HwQueuePrivateFlags(const QueueCreateOptions& options)
{
    constexpr uint32_t Priorities[] = { 1, 0, 2, 3, 4 };
    const uint32_t priority = (options.priority < 5) ? Priorities[options.priority] : 0;
    return ((options.numReservedCu & 0xff) << 4) | ((priority & 7) << 12) |
           (options.dispatchTunneling ? 0x8000u : 0u) | (options.tmzOnly ? 0x10000u : 0u) | 0x20000u |
           ((options.tmzLevel & 0xf) << 20);
}
inline std::array<uint8_t, QueueBytes> QueueData(const QueueCreateOptions& options)
{
    std::array<uint8_t, QueueBytes> data = {};
    Put32(data.data(), 0, QueueBytes);
    Put32(data.data(), 16, HwQueuePrivateFlags(options));
    return data;
}
struct AllocationData
{
    std::array<uint8_t, AllocationPrivateBytes> privateData = {};
    std::array<uint8_t, RecordsBytes> records = {};
};
inline AllocationData AllocationRecords(uint32_t size, uint32_t alignment, bool host,
                                        uint64_t address, bool uncached,
                                        MemoryHeap heap = MemoryHeap::GartCacheable,
                                        uint32_t priority = 3, uint32_t priorityOffset = 0)
{
    AllocationData data;
    auto* r = data.records.data();
    Put32(r, 0, EntryBytes + MetadataBytes);
    Put32(r, 60, 1);
    Put32(r, 20, Checksum(r, HeaderBytes));
    constexpr size_t entry = HeaderBytes;
    Put32(r, entry, EntryBytes + MetadataBytes);
    Put32(r, entry + 4, 1);
    constexpr size_t bo = entry + 8;
    Put32(r, bo, 128);
    // RADV fill_alloc_heaps: heap IDs are index+1 and every allowed heap sets its index bit.
    // The first heap is preferred; the second is the observed GART-cacheable -> USWC fallback.
    uint32_t boFlags = 0x2000; // ALLOC_FLAG_NOT_VIRTUAL
    if (heap != MemoryHeap::Invisible) { boFlags |= 0x20000000; } // CPU_VISIBLE
    if (host) { boFlags |= 0x04000000; } // HOST_ALLOCATED
    switch (heap)
    {
    case MemoryHeap::Local: r[bo + 24] = 1; boFlags |= 1u << 0; break;
    case MemoryHeap::Invisible: r[bo + 24] = 2; boFlags |= 1u << 1; break;
    case MemoryHeap::GartUswc: r[bo + 24] = 3; boFlags |= 1u << 2; break;
    case MemoryHeap::GartCacheable:
        r[bo + 24] = 4;
        boFlags |= 1u << 3;
        if (!host) { r[bo + 25] = 3; boFlags |= 1u << 2; }
        break;
    }
    Put32(r, bo + 8, boFlags);
    Put32(r, bo + 12, size);
    Put32(r, bo + 16, alignment);
    // Original BO record filler380960 uses a distinct creation-hint table at
    //584ba58; it is not the Windows SetAllocationPriority table at584ba70.
    constexpr uint32_t CreationPriorities[] = { 1, 1, 2, 0, 3, 4 };
    const uint32_t priorityBits = ((CreationPriorities[priority] << 4) | priorityOffset) << 7;
    // The former host0x800 default is the Unused-priority encoding. It is
    // overwritten by the native hint field, not an independent host flag.
    Put32(r, bo + 20, 5 | priorityBits);
    Put32(r, bo + 92, 0x1000000);
    Put64(r, bo + 96, size);
    Put64(r, bo + 104, address);
    Put64(r, bo + 112, size);
    Put32(r, entry + 176, EntryBytes);
    Put32(r, entry + 188, AbiVersion);
    constexpr size_t metadata = entry + EntryBytes;
    Put32(r, metadata, MetadataBytes);
    Put64(r, metadata + 8, address);
    Put64(r, metadata + 16, size);
    Put32(r, metadata + 204, uncached ? 4 : 0);
    Put32(r, metadata + 208, (heap == MemoryHeap::Invisible) ? 0x2000 : 0); // no CPU access
    // Original AMD/Mesa policy: default keeps MALL enabled; bypass explicitly disables it.
    // Do not inherit AnyPS5's title-specific D56 MALL-never workaround into generic PAL.
    Put32(r, metadata + 212, uncached ? 1 : 0);
    auto* p = data.privateData.data();
    Put32(p, 8, 0x80);
    Put32(p, 60, RecordsBytes);
    Put32(p, 12, Checksum(p, AllocationPrivateBytes));
    return data;
}
// Exact ordinary-client GART carrier recovered from complete native380960,
//3802c0 and37ef20 instruction captures. Other policies retain AllocationRecords.
struct OrdinaryGartAllocationInfo
{
    uint32_t size = 0;
    uint32_t physicalAlignment = 0;
    uint64_t address = 0;
    uint64_t uniqueId = 0;
    uint32_t heapCount = 0;
    std::array<uint8_t, 4> heapIds = {}; // Native IDs3=USWC,4=Cacheable, actual translated order.
    uint32_t priority = 3;
    uint32_t priorityOffset = 0;
    uint32_t mtype = 0;
    uint32_t mallPolicy = 0;
};
inline Status OrdinaryGartAllocationRecords(const OrdinaryGartAllocationInfo& info, AllocationData* output)
{
    if ((output == nullptr) || (info.size == 0) || ((info.size & 65535) != 0) ||
        (info.physicalAlignment < 65536) ||
        ((info.physicalAlignment & (info.physicalAlignment - 1)) != 0) ||
        (info.address == 0) || ((info.address & 65535) != 0) || (info.address > UINT64_MAX - info.size) ||
        (info.heapCount == 0) || (info.heapCount > info.heapIds.size()) ||
        (info.priority >= 6) || (info.priorityOffset >= 8) || (info.mtype >= 4) || (info.mallPolicy >= 3))
    { return InvalidParameter; }
    uint32_t heapMask = 0;
    for (uint32_t index = 0; index < info.heapCount; ++index)
    {
        const uint8_t id = info.heapIds[index];
        if ((id != 3) && (id != 4)) { return InvalidParameter; }
        heapMask |= 1u << (id - 1);
    }
    auto data = AllocationRecords(info.size, info.physicalAlignment, false, info.address, false,
                                  MemoryHeap::GartUswc, info.priority, info.priorityOffset);
    auto* entry = data.records.data() + HeaderBytes;
    constexpr size_t bo = 8;
    Put32(entry, bo + 8, 0x20002000 | heapMask); // CPU-visible, real, exact allowed GART mask.
    std::memset(entry + bo + 24, 0, 4);
    std::memcpy(entry + bo + 24, info.heapIds.data(), info.heapCount);
    auto* metadata = entry + EntryBytes;
    Put64(metadata, 180, info.uniqueId);
    constexpr uint32_t Mtypes[] = {0, 1, 2, 4};
    Put32(metadata, 204, Mtypes[info.mtype]);
    Put32(metadata, 212, info.mallPolicy); // Independent of MType/gl2Uncached.
    *output = data;
    return Success;
}
inline Status Submission(const IbEntry* entries, size_t count, std::vector<uint8_t>* output,
                         uint32_t ipType = 1)
{
    if ((entries == nullptr) || (output == nullptr) || (count == 0) || (count > MaxIbs) || (ipType > 1))
        { return InvalidParameter; }
    for (size_t i = 0; i < count; ++i)
    {
        if ((entries[i].address == 0) || ((entries[i].address & 3) != 0) ||
            (entries[i].bytes == 0) || ((entries[i].bytes & 3) != 0) ||
            ((entries[i].flags != 0) && (entries[i].flags != 0xc) && (entries[i].flags != 0x104)))
            { return InvalidParameter; }
    }
    output->assign(8 + count * 64, 0);
    auto* p = output->data();
    Put32(p, 0, AbiVersion);
    Put32(p, 4, uint32_t(count));
    for (size_t i = 0; i < count; ++i)
    {
        const size_t at = 8 + i * 64;
        Put32(p, at, 0);
        Put32(p, at + 4, 64);
        Put32(p, at + 8, entries[i].bytes);
        Put32(p, at + 12, entries[i].flags);
        Put32(p, at + 20, ipType); // AMD private IP discriminator: SDMA0, graphics/compute1.
        Put64(p, at + 24, entries[i].address);
    }
    return Success;
}
inline bool SharedSize(const uint8_t* p, size_t bytes, uint64_t* size)
{
    if ((p == nullptr) || (size == nullptr)) { return false; }
    for (size_t at = 0; at + HeaderBytes + EntryBytes <= bytes; at += 4)
    {
        const uint8_t* header = p + at;
        const size_t entryBytes = Get32(header, 0);
        if ((Get32(header, 60) == 0) || (Get32(header, 60) > 4) ||
            (entryBytes < EntryBytes) || (entryBytes > bytes - at - HeaderBytes)) { continue; }
        const uint8_t* entry = header + HeaderBytes;
        const uint32_t version = Get32(entry, 188);
        if ((Get32(entry, 0) != entryBytes) || ((version != 8) && (version != 9)) ||
            (Get32(entry, 8) != 128)) { continue; }
        *size = Get64(entry, 8 + 96);
        return (*size != 0) && ((*size & 4095) == 0) && (*size <= UINT32_MAX);
    }
    return false;
}
}
