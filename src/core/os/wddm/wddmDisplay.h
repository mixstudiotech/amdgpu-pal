// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Display-related Device/Platform queries recovered from the reference AMD ICD
// (SHA-256 36a9ef32...7a44). RVAs in comments are original function bodies.
#pragma once

#include "palDevice.h"
#include "palPlatform.h"
#include "palSysMemory.h"
#include "core/os/wddm/wddmEscape.h"
#include <array>

namespace Pal::Wddm
{
struct PhysicalProperties;

// ---------------------------------------------------------------------------------------------------------------------
// Cached KMD record gates. The original copies the BASIC record body to Device+9ae0 and the GPU record body to
// Device+9d40 (constructor 30a1aa..30a24d); both bodies start at record offset 2400.

// Device+9cd4 = BASIC DWORD 2900: GetStereoDisplayModes requires bit 19 set and bit 23 clear.
bool StereoDisplayModesSupported(const PhysicalProperties& records);
// Device+cb40 = GPU DWORD 14176: bit 17 gates the frame-metadata shared buffer.
bool FrameMetadataSupported(const PhysicalProperties& records);

// ---------------------------------------------------------------------------------------------------------------------
// GetPrimaryInfo: public wrapper 314220 around helper 313fb0 (command 0x01001003, 256-byte record).
constexpr uint32 PrimaryInfoCommand = 0x01001003;
using PrimaryInfoRecord = std::array<uint8, 0x100>;

// The helper fills a 20-byte internal output; the public wrapper copies only its first 16 bytes. The fifth DWORD is
// also consumed by the presentable-image path (381f8d).
struct PrimaryInfoReply
{
    GetPrimaryInfoOutput output;
    uint32 extendedFlags;
};

// Record +0x90: FormatInfoTable bitsPerPixel, or bitCount[0] for YUV-planar formats.
uint32 PrimarySurfaceBitsPerPixel(ChNumFormat format);
// Record +0x94: first row match in the original (format, swizzle) -> D3DDDIFORMAT table 5838fc0, with the AMD codes
// 0x1ad..0x1b1 and unmatched swizzles reported as 0.
uint32 PrimarySurfaceDdiFormat(SwizzledFormat format);

PrimaryInfoRecord BuildPrimaryInfoRecord(const GetPrimaryInfoInput& input);
// Merges the reply into *reply exactly as 313fb0 does (read-modify-write of the flag DWORDs). gpuSpoofed is
// Device+202c bit 0, i.e. HwIpLevelFlags::isSpoofed.
void DecodePrimaryInfoRecord(const PrimaryInfoRecord& record, bool gpuSpoofed, PrimaryInfoReply* reply);
Result QueryPrimaryInfo(const EscapeRoute& route, bool gpuSpoofed, const GetPrimaryInfoInput& input,
                        PrimaryInfoReply* reply);
Result GetPrimaryInfo(const EscapeRoute& route, bool gpuSpoofed, const GetPrimaryInfoInput& input,
                      GetPrimaryInfoOutput* output);

// ---------------------------------------------------------------------------------------------------------------------
// GetStereoDisplayModes 314650 (command 0x03002e03). Count-only queries send a fixed 264-byte record; list queries
// allocate count*64+264 bytes (Platform allocator, alignment 8, AllocInternalTemp) and page through the KMD list 256
// rows at a time.
constexpr uint32 StereoModeCommand = 0x03002e03;
// Only D3DDDIFMT_X8R8G8B8, R5G6B5, X1R5G5B5 and A2R10G10B10 rows are reported (314870..314890).
bool IsReportedStereoModeFormat(uint32 ddiFormat);
// The four conversions of 2cd040 reachable from 314650.
SwizzledFormat StereoModeFormat(uint32 ddiFormat);
Result GetStereoDisplayModes(const EscapeRoute& route, const Util::IndirectAllocator& allocator, bool supported,
                             uint32* count, StereoDisplayModeOutput* list);

// ---------------------------------------------------------------------------------------------------------------------
// GetActive10BitPackedPixelMode 314980 (command 0x03003f03) and RequestKmdReinterpretAs10Bit 314b00 (0x03002f03).
constexpr uint32 Active10BitModeCommand = 0x03003f03;
constexpr uint32 Reinterpret10BitCommand = 0x03002f03;
// enabled is reply +0xcc, packing is reply +0xd0. The caller's isInWs10BitMode byte is read when enabled != 1.
void DecodeActive10BitPackedPixelMode(uint32 enabled, uint32 packing, Active10BitPackedPixelModeOutput* mode);
Result GetActive10BitPackedPixelMode(const EscapeRoute& route, Active10BitPackedPixelModeOutput* mode);

// Native 2b3f20: KMT handle of the root allocation of a WDDM GPU memory object.
uint32 KmtAllocationHandle(const IGpuMemory& memory);
using AllocationHandleQuery = uint32 (*)(const IGpuMemory& memory);
// State-changing: asks the KMD to treat the allocation as 10 bits per channel for KMD-initiated BLTs.
Result RequestKmdReinterpretAs10Bit(const EscapeRoute& route, const IGpuMemory* memory,
                                    AllocationHandleQuery handleOf = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// KMD/UMD shared buffers set up by Device::Finalize (30dfe0 calls 30d620 after the base finalize). Each buffer is a
// KMD-provided user address returned by command 0x03007703 at reply +0xc8. Defaults follow the constructor 30a060.
constexpr uint32 SharedBufferCommand = 0x03007703;
struct KmdSharedBuffers
{
    uint64 flipStatus = 0;            // Device+9588, type 3 (requireFlipStatus)
    uint64 rsFeatureChange = 0;       // Device+9590, type 5
    uint64 rsFeatureSettings = 0;     // Device+9598, type 9
    uint64 chillStatus = 0;           // Device+95a0, type 7
    uint32 rsFeatureCache[8] = {};    // Device+95a8..95c7, read by the RS feature methods 316260/316460
    uint64 frameMetadata = 0;         // Device+95c8, type 4 (requireFrameMetadata and GPU caps bit 17)
    uint64 flipOwner = ~uint64(0);    // Device+d9d8, type 3 reply +0xd8
};
// Native 30d620 field remap of DeviceFinalizeInfo::supportedFullScreenFrameMetadata into the type-4 parameters.
uint32 EncodeFrameMetadataCaps(uint32 palFlags);
// Native 314de0: one 264-byte request; on success writes reply +0xc8/+0xd0/+0xd8 to the non-null outputs.
Result QueryKmdSharedBuffer(const EscapeRoute& route, uint32 type, const uint8* parameters48,
                            uint64* buffer, uint64* second, uint64* third);
// Native 30d620. Every escape failure is ignored, as in the original.
void InitKmdSharedBuffers(const EscapeRoute& route, const DeviceFinalizeInfo& finalizeInfo,
                          const PhysicalProperties& records, KmdSharedBuffers* buffers);

// PollFullScreenFrameMetadataControl 314bd0 and GetFlipStatus 314d50 read the shared buffers: DWORD +4 is the source
// count and source N uses the 64-byte entry at (N+1)*64.
uint32 DecodeFrameMetadataFlags(uint32 kmdFlags, uint32 previousFlags);
Result PollFullScreenFrameMetadataControl(const KmdSharedBuffers& buffers, uint32 vidPnSrcId,
                                          PerSourceFrameMetadataControl* control);
Result GetFlipStatus(const KmdSharedBuffers& buffers, uint32 vidPnSrcId, FlipStatusFlags* flags, bool* isFlipOwner);

// ---------------------------------------------------------------------------------------------------------------------
// Platform::TurboSyncControl 2cce40 (command 0x03008203). The original escapes through the first adapter's master
// Device; every device with a primary surface must belong to that adapter.
constexpr uint32 TurboSyncCommand = 0x03008203;
struct TurboSyncDeviceSlot
{
    bool present = false;           // Platform device pointer is non-null
    bool onPrimaryAdapter = false;  // Device+d9c0 equals the first adapter object
    uint32 physicalAdapter = 0;     // Device+d9c8
};
// State-changing for every valid mode.
Result TurboSyncControl(const EscapeRoute& route, const TurboSyncControlInput& input,
                        const TurboSyncDeviceSlot (&devices)[MaxDevices], AllocationHandleQuery handleOf = nullptr);
} // namespace Pal::Wddm
