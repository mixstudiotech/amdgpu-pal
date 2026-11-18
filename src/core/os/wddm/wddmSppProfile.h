// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "wddmApplicationProfile.h"

namespace Pal::Wddm
{
struct SppQueryReply
{
    Result result;
    const void* blob;
    uint32 bytes;
};
struct SppProfileStorage
{
    void* copiedBlob = nullptr;
    const uint8* selectedPayload = nullptr;
    size_t copiedBytes = 0; // Bounds the parser outside the native-hazard replica.
};
struct SppProfileCalls
{
    void* context;
    SppQueryReply (*query)(void*, const wchar_t*, const wchar_t*, ApplicationProfileClient);
    void* (*allocate)(void*, size_t, size_t, uint32);
    void (*free)(void*, void*);
    void* (*copy)(void*, void*, const void*, size_t);
    void (*zero)(void*, void*, size_t);
    void (*write)(void*, void*, size_t, uint32);
    Result (*escape)(void*, void*, size_t);
    // CPU fixtures set this to compare the native hazards (null writes after a
    // failed allocation, unbounded traversal, stale selected payload). Product
    // callers leave it false and get the safe deviations described below.
    bool replicateNativeHazards = false;
};

// Original behavior port (native 310270 and 3103c0). For well-formed profiles
// and successful allocations both modes are identical. Without
// replicateNativeHazards: a failed allocation returns ErrorOutOfMemory where the
// original writes through null; freeing the old copied profile also clears the
// selected payload the original keeps pointing into freed memory; traversal
// stops at the copied profile's end and when the native loop would stop advancing.
Result EnableSppProfile(uint32 capability, SppProfileStorage* state, const SppProfileCalls& calls,
                        const wchar_t* filename, const wchar_t* pathname);
Result SelectSppTable(uint32 capability, const SppProfileStorage& state, const SppProfileCalls& calls,
                     uint32 pixelCount, uint32 msaaRate);
}
