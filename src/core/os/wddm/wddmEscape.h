// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "pal.h"
#include <cstddef>
#include <cstdint>

namespace Pal { class Platform; }
namespace Pal::Wddm
{
class Transport;

// Route for the original shared AMD private Escape (native 30f860). The
// original reads the KMT adapter/device handles and the physical adapter
// index from its Device, and refuses before the DDI when Device+0x8bd0 is set.
struct EscapeRoute
{
    uint32 adapter = 0;
    uint32 device = 0;
    uint32 physicalAdapter = 0;
    bool suppressed = false;
    Pal::Platform* platform = nullptr; // Allocator for envelopes above 2048 bytes.
    // Optional replacement for D3DKMTEscape (CPU fixtures). The second argument
    // is an SDK D3DKMT_ESCAPE; the return value is an NTSTATUS.
    int32 (*escape)(void* context, void* request) = nullptr;
    void* context = nullptr;
};

EscapeRoute DeviceEscapeRoute(const Transport& transport, Pal::Platform* platform);

// Native 30f860. A 76-byte envelope {2, 0x10002, 0, physicalAdapter, ..., bytes}
// precedes the caller record. Envelopes up to 2048 bytes use stack storage and
// larger ones a temporary PAL allocation (ErrorOutOfMemory if it fails). The
// escape is DRIVERPRIVATE with Flags.HardwareAccess = hardwareAccess and the
// given hContext. Only NT success copies the response back over the record;
// a nonzero private result DWORD at record+0x48 then returns ErrorUnknown.
Result SharedEscape(const EscapeRoute& route, void* record, size_t bytes,
                    bool hardwareAccess = false, uint32 hContext = 0);

// Most private records start with {0x80 header bytes, version 0x10000, command}.
void InitPrivateRecord(void* record, size_t bytes, uint32 command);

inline void PutRecord32(void* record, size_t offset, uint32 value)
{
    auto* bytes = static_cast<uint8*>(record) + offset;
    for (uint32 i = 0; i < 4; ++i) { bytes[i] = uint8(value >> (8 * i)); }
}
inline uint32 GetRecord32(const void* record, size_t offset)
{
    const auto* bytes = static_cast<const uint8*>(record) + offset;
    return uint32(bytes[0]) | (uint32(bytes[1]) << 8) | (uint32(bytes[2]) << 16) | (uint32(bytes[3]) << 24);
}
inline void PutRecord64(void* record, size_t offset, uint64 value)
{
    PutRecord32(record, offset, uint32(value));
    PutRecord32(record, offset + 4, uint32(value >> 32));
}
inline uint64 GetRecord64(const void* record, size_t offset)
{
    return uint64(GetRecord32(record, offset)) | (uint64(GetRecord32(record, offset + 4)) << 32);
}
}
