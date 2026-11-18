// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Native allocation request builder: BO filler 380960, entry/metadata builder 3802c0 and the request
// packaging of 37ef20 in the reference amdvlk64.dll (see .build/pal-wddm-logic/memory/analysis.txt).
// Inputs are the original GpuMemory/Device fields by meaning; the output is the exact private data the
// original hands to D3DKMTCreateAllocation for N allocations of one memory object.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Pal::Wddm::Private
{
// Native 37e870/37eb10/37ebc0: allocations of at least 256 MiB may be backed by 128 MiB KMT allocations.
inline constexpr uint64_t ChunkShift = 27;
inline constexpr uint64_t ChunkBytes = uint64_t(1) << ChunkShift;
inline constexpr uint64_t ChunkThreshold = 0x10000000;
inline constexpr uint32_t ChunkCount(uint64_t size)
{
    const uint64_t count = (size + (ChunkBytes - 1)) >> ChunkShift;
    return (count > 1) ? uint32_t(count) : 1;
}

// One subresource as 380e00 reads it: SubResourceInfo (184 bytes, the same layout in the original and this PAL),
// its AddrMgr2 TileInfo, and GfxImage::GetSwTileMode (original Gfx9::Image vtable 6574840 slot 18).
struct NativeSubresource
{
    uint32_t plane              = 0; // SubResourceInfo+0x00 (subresId.plane)
    uint32_t bitsPerTexel       = 0; // +0x0c
    uint32_t extentWidth        = 0; // +0x18 extentTexels
    uint32_t extentHeight       = 0; // +0x1c
    uint32_t extentDepth        = 0; // +0x20
    uint32_t actualWidth        = 0; // +0x30 actualExtentTexels
    uint32_t actualHeight       = 0; // +0x34
    uint32_t actualDepth        = 0; // +0x38
    uint64_t size               = 0; // +0x50
    uint64_t offset             = 0; // +0x58
    uint64_t rowPitch           = 0; // +0x70
    uint64_t depthPitch         = 0; // +0x78
    uint32_t stereoOffset       = 0; // +0x88 (low DWORD)
    uint32_t stereoLineOffset   = 0; // +0x90
    uint32_t swizzleMode        = 0; // GetSwTileMode
    uint64_t backingStoreOffset = 0; // TileInfo+0x00
    uint32_t pipeBankXor        = 0; // TileInfo+0x08
    uint32_t ePitch             = 0; // TileInfo+0x0c
};

// Image fields read by 37ef20, 380960, 3802c0 and 380e00 (original Wddm::Image offsets).
struct NativeImageDesc
{
    uint32_t createFlags        = 0; // +0x08 ImageCreateFlags
    uint32_t usageFlags         = 0; // +0x0c ImageUsageFlags
    uint32_t imageType          = 0; // +0x10
    uint32_t format             = 0; // +0x14 ChNumFormat
    uint32_t swizzle            = 0; // +0x18 ChannelMapping
    uint32_t mipLevels          = 1; // +0x28
    uint32_t arraySize          = 1; // +0x2c
    uint32_t samples            = 1; // +0x30
    uint32_t refreshNumerator   = 0; // +0x70 refreshRate
    uint32_t refreshDenominator = 0; // +0x74
    uint32_t viewFormatCount    = 0; // +0x78
    bool     turbosync          = false; // +0xe0 bit5 (internal create flags)
    uint32_t numPlanes          = 1; // +0x178
    bool     privateScreen      = false; // +0x250 != null
    bool     hasScreen          = false; // +0x268 != null (fullscreen screen of a presentable image)
    uint32_t vidPnSourceId      = 0; // screen+0x30
    uint64_t sharedResourceId   = 0; // +0x270
    // GfxImage (+0x1a8) virtual results: slot 2 HasDisplayDccData, slot 9 GetDisplayDccState (DccState fields),
    // slot 14 GetMallCursorCacheOffset.
    bool     hasDisplayDcc      = false;
    uint64_t displayDccPrimaryOffset   = 0;
    uint64_t displayDccSecondaryOffset = 0;
    uint32_t displayDccPitch    = 0;
    uint32_t displayDccFlags    = 0; // DccState bitfield DWORD
    uint64_t mallCursorCacheOffset = 0;
    // Slot 8 GetSharedMetadataInfo (read for optimalShareable images only). The original's record has single
    // offsets; flags in its bit order: 0 shaderFetchable, 2 hasEqGpuAccess, 3 htileHasDsMetadata.
    uint32_t sharedFlags        = 0;
    uint64_t sharedDccOffset    = 0;
    uint64_t sharedFmaskOffset  = 0;
    uint64_t sharedFmaskXor     = 0;
    uint64_t sharedHtileOffset  = 0;
    uint64_t sharedDccStateOffset   = 0;
    uint64_t sharedFastClearOffset  = 0;
    uint64_t sharedHisPretestOffset = 0;
    uint32_t sharedFmaskSwizzle = 0;
    // Subresource 'index' = (plane * mipLevels + mip) * arraySize + slice; false when it does not exist.
    bool (*getSubresource)(const void* context, uint32_t index, NativeSubresource* out) = nullptr;
    const void* context = nullptr;
};

// TypedBufferCreateInfo as 3814c0 reads it (object+0xc0..+0xdc).
struct NativeTypedBufferDesc
{
    uint32_t format        = 0;
    uint32_t swizzle       = 0;
    uint32_t width         = 0;
    uint32_t height        = 0;
    uint32_t depth         = 0;
    uint32_t rowPitch      = 0;
    uint32_t depthPitch    = 0;
    bool     depthIsSubres = false;
};

// PAL GpuHeap order: Local, Invisible, GartUswc, GartCacheable.
struct NativeAllocationDesc
{
    uint64_t gpuVirtAddr        = 0; // object+8 when the request is built (0 without a reservation)
    uint64_t size               = 0; // object+16
    uint64_t clientSize         = 0; // object+24 (untyped interprocess SURF width)
    uint64_t physicalAlignment  = 0; // 353250
    uint64_t uniqueId           = 0; // object+88
    uint32_t allocationCount    = 1; // object+296
    uint32_t adapterIndex       = 0; // Device+0xd9c8
    uint32_t abiVersion         = 9; // Platform+0x27c8
    uint32_t surfaceBytes       = 288; // Device+0xdc8c
    uint32_t gfxLevelWord       = 0; // Device+0x2020 (SURF linear swizzle below 0xc000000)
    bool     integrated         = true; // Device+0x201c == 1: no cross-domain heap duplication
    bool     boFlag200          = false; // Device+0x81e1
    uint32_t heapCount          = 0;
    uint32_t heaps[4]           = {};
    uint64_t heapSizes[4]       = {}; // Device+35000+24*heap; zero-size heaps are skipped
    uint32_t priority           = 0;
    uint32_t priorityOffset     = 0;
    uint32_t mtype              = 0; // MType enum 0..3
    uint32_t mallPolicy         = 0;
    uint32_t mallStartPage      = 0;
    uint32_t mallNumPages       = 0;
    // GpuMemory flags read by the builders.
    bool virtualAlloc       = false;
    bool pinned             = false;
    bool kmdShareUmdSysMem  = false;
    bool client             = false;
    bool shareable          = false;
    bool interprocess       = false;
    bool sharedViaNtHandle  = false;
    bool cpuVisible         = false;
    bool cpuInvisible       = false; // native TranslateHeapInfo result (metadata+208 bit13)
    bool stereo             = false;
    bool privateScreen      = false;
    bool globallyCoherent   = false;
    bool xdmaBuffer         = false;
    bool peerWritable       = false;
    bool tmzProtected       = false;
    bool busAddressable     = false;
    bool accessedPhysically = false;
    bool privPrimary        = false;
    bool mallRangeActive    = false;
    bool flippable          = false;
    bool restrictedContent  = false;
    bool restrictedAccess   = false;
    bool crossAdapter       = false;
    bool deferCpuVaReservation = false; // metadata+208 bit6
    bool dfSpmTraceBuffer   = false; // metadata+208 bit10
    // object+344 bit3: set by the GpuMemory constructor (309c00) from Device+0xcb40 bit23 (GPU record DWORD 14176):
    // one SURF record describes the base subresource of an image (and a typed buffer reserves one).
    bool singleSurface      = false;
    const NativeImageDesc*       image       = nullptr; // object+176
    const NativeTypedBufferDesc* typedBuffer = nullptr; // typedBuffer flag (ignored when an image is set)
};

struct NativeAllocationRequest
{
    uint32_t count       = 0;
    uint32_t blockBytes  = 0; // 64-byte allocation header + entry + metadata
    uint32_t createFlags = 0; // D3DKMT_CREATEALLOCATIONFLAGS bits
    std::array<uint8_t, 64> resourceHeader = {};
    std::vector<uint8_t> blocks;   // count * blockBytes
    std::vector<uint64_t> hostOffsets; // pSystemMem offset from the pinned base per allocation
    uint32_t vidPnSourceId = UINT32_MAX; // INFO+0x1c for every allocation
    uint32_t infoFlags     = 0;          // INFO+0x20 (D3DDDI_ALLOCATIONINFO2 Flags: bit0 Primary, bit1 Stereo)
};

// Native 2cf440: KMD surface format of a PAL format for the image/typed-buffer SURF records.
uint32_t KmdSurfaceFormat(uint32_t format, uint32_t swizzle, bool depthPlane, bool viewFormats);

// Returns 0 (Success), 10 (the native NotFound when no requested heap exists), or -2 for inputs this builder does
// not model (user queues, GFX12 images, sizes the 32-bit native arithmetic cannot express).
int32_t BuildNativeAllocationRequest(const NativeAllocationDesc& desc, NativeAllocationRequest* request);
}
