// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmSharedMemory.h"
#include <cstring>
#include <limits>

namespace Pal::Wddm
{
namespace
{
uint32_t Read32(const uint8_t* bytes, size_t offset)
{
    uint32_t value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
uint64_t Read64(const uint8_t* bytes, size_t offset)
{
    uint64_t value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
bool ParseEntry(const uint8_t* entry, size_t entrySize, SharedAllocationProperties* properties)
{
    constexpr size_t EntryBytes = 196;
    if (entrySize < EntryBytes) { return false; }
    const uint32_t version = Read32(entry, 188);
    // Version2 is the captured D3D11 legacy resource carrier;8/9 are the
    // authenticated D3D12/Vulkan carriers with the same selected fields.
    if ((Read32(entry, 0) != entrySize) || ((version != 2) && (version != 8) && (version != 9)) ||
        (Read32(entry, 8) != 128)) { return false; }
    SharedAllocationProperties result;
    result.size = Read64(entry, 104);
    result.clientSize = result.size;
    result.alignment = Read32(entry, 24);
    if ((result.size == 0) || ((result.size & 4095) != 0) ||
        (result.size > std::numeric_limits<uint32_t>::max()) ||
        (result.alignment == 0) || ((result.alignment & (result.alignment - 1)) != 0)) { return false; }
    bool validHeaps = true;
    for (size_t heap = 0; heap < 4; ++heap)
    {
        const uint8_t id = entry[32 + heap];
        if (id > 4) { validHeaps = false; break; }
        if (id != 0) { result.heapIds[result.heapCount++] = id; }
    }
    if (!validHeaps || (result.heapCount == 0)) { return false; }
    
    // Native312620 preserves the creator's ID from metadata+180. Validate
    // both the entry-relative offset and the metadata section's own bounds.
    const size_t metadataOffset = Read32(entry, 176);
    if (metadataOffset != 0)
    {
        constexpr size_t UniqueIdEnd = 180 + sizeof(uint64_t);
        if ((metadataOffset < EntryBytes) || (metadataOffset > entrySize) ||
            (UniqueIdEnd > entrySize - metadataOffset)) { return false; }
        const auto* metadata = entry + metadataOffset;
        const size_t metadataSize = Read32(metadata, 0);
        if ((metadataSize < UniqueIdEnd) || (metadataSize > entrySize - metadataOffset)) { return false; }
        result.uniqueId = Read64(metadata, 180);
    }
    
    // Native untyped external-memory open returns the SURF byte width as
    // client size when a surface is present, while retaining the physical
    // allocation size in the memory object. A buffer width is measured in
    // bytes. No image format/layout is inferred from this field.
    if (Read32(entry, 136) != 0)
    {
        constexpr size_t SurfaceBytesOffset = 216;
        if (entrySize < SurfaceBytesOffset + sizeof(uint32_t)) { return false; }
        const uint32_t clientSize = Read32(entry, SurfaceBytesOffset);
        if ((clientSize == 0) || (clientSize > result.size)) { return false; }
        result.clientSize = clientSize;
        // Native3802c0's non-image linear SURF has this exact tuple on
        // GFX11. A surface count alone also occurs for textures and is
        // insufficient to authenticate a data-buffer import.
        if (entrySize >= 236)
        {
            const uint32_t surfaceBytes = Read32(entry, 196);
            result.linearBuffer = (surfaceBytes >= 40) && (surfaceBytes <= entrySize - 196) &&
                (Read32(entry, 204) == 0x20) && (Read32(entry, 208) == 1) &&
                (Read32(entry, 212) == 0) && (Read32(entry, 220) == 1) && (Read32(entry, 232) == 1);
        }
    }
    *properties = result;
    return true;
}
}
bool ParseSharedAllocationProperties(const uint8_t* records, size_t bytes,
                                     SharedAllocationProperties* properties)
{
    constexpr size_t HeaderBytes = 64;
    constexpr size_t EntryBytes = 196;
    if ((records == nullptr) || (properties == nullptr) || (bytes < EntryBytes)) { return false; }
    // Native3120c0 accepts a direct entry when its first DWORD equals the
    // private-data size, or the same entry after a64-byte allocation header.
    if ((Read32(records, 0) == bytes) && ParseEntry(records, bytes, properties)) { return true; }
    if (bytes < HeaderBytes + EntryBytes) { return false; }
    for (size_t offset = 0; offset <= bytes - HeaderBytes - EntryBytes; offset += 4)
    {
        const auto* header = records + offset;
        const size_t entrySize = Read32(header, 0);
        const uint32_t entries = Read32(header, 60);
        if ((entries == 0) || (entries > 4) || (entrySize < EntryBytes) ||
            (entrySize > bytes - offset - HeaderBytes)) { continue; }
        if (ParseEntry(header + HeaderBytes, entrySize, properties)) { return true; }
    }
    return false;
}
}

