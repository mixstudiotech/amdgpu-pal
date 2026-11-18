/*
 * Copyright (c) 2026. SPDX-License-Identifier: MIT
 * AMD private query layouts derived from the Mesa WDDM2 reference.
 */
#pragma once

#include <array>
#include <cstdint>

namespace Pal::Wddm
{

struct IbAlignment
{
    uint32_t start = 0;
    uint32_t size  = 0;
};

// These properties belong to the versioned AMD UMD/KMD interface. The caller
// must verify the adapter/driver profile before issuing the private queries.
struct PhysicalProperties
{
    static constexpr uint32_t BasicRecordSize = 3008;
    static constexpr uint32_t GpuRecordSize   = 17248;
    static constexpr uint32_t MaxShaderEngines = 6;
    static constexpr uint32_t MaxShaderArrays  = 2;

    uint32_t deviceId         = 0;
    uint32_t familyId         = 0;
    uint32_t externalRevision = 0;
    // Mesa names this slot chip_rev. On the selected driver it contains the
    // subsystem identifier, so retain its raw position until its ABI is known.
    uint32_t deviceData4972   = 0;
    uint32_t gfxEngineId      = 0;
    uint32_t subsystemId      = 0;
    uint32_t vramType         = 0;
    uint32_t vramBitWidth     = 0;
    uint32_t numRbPipes       = 0;
    uint32_t numEnabledRbPipes = 0;
    uint64_t enabledRbMask    = 0;

    uint32_t numEngines = 0;
    std::array<uint8_t, 64> engineTypes = {};
    uint32_t meFirmwareVersion  = 0;
    uint32_t cpFeatureVersion   = 0;
    uint32_t mecFirmwareVersion = 0;
    uint32_t pfpFirmwareVersion = 0;
    uint32_t crystalFrequencyHz = 0;
    // Native30b822 fallback limits: GPU record5000/5002, unsigned16-bit MHz.
    // Native initialization first prefers a separate live clock-capability Escape.
    uint32_t fallbackMaxEngineClockMHz = 0;
    uint32_t fallbackMaxMemoryClockMHz = 0;
    uint64_t visibleVramSize    = 0;
    uint64_t invisibleVramSize  = 0;
    uint32_t gbAddrConfig       = 0;
    uint32_t numShaderVisibleVgprs = 0;
    uint32_t wavefrontSize         = 0;
    uint32_t numShaderEngines      = 0;
    uint32_t numActiveRbPerSe      = 0;
    uint32_t numShaderArraysPerEngine = 0;
    uint32_t numTccBlocks          = 0;
    uint32_t maxGsWavesPerVgt       = 0;
    uint32_t gsVgtTableDepth        = 0;
    uint32_t gsPrimBufferDepth      = 0;
    uint32_t numCuPerWgp            = 0;
    std::array<std::array<uint16_t, MaxShaderArrays>, MaxShaderEngines> wgpBitmap = {};
    std::array<std::array<uint16_t, MaxShaderArrays>, MaxShaderEngines> alwaysOnWgpBitmap = {};
    uint32_t numSqcPerWgp       = 0;
    uint32_t sqcInstructionCacheSize = 0;
    uint32_t sqcDataCacheSize   = 0;
    uint32_t gl1CacheSize       = 0;
    uint32_t gl2CacheSize       = 0;
    uint32_t mallSizeMb         = 0;
    uint64_t gpuVaStart         = 0;
    uint64_t gpuVaEnd           = 0;
    uint32_t largePageEncoding  = 0;
    std::array<IbAlignment, 8> ibAlignments = {};

    std::array<uint8_t, BasicRecordSize> basicRecord = {};
    std::array<uint8_t, GpuRecordSize> gpuRecord = {};
};

// Returns the original NTSTATUS from KMT, or STATUS_INVALID_PARAMETER when a
// successful response contradicts the known record layout. Output is committed
// only after both records and their common identification fields are validated.
int32_t QueryPhysicalProperties(uint32_t kmtAdapterHandle, PhysicalProperties* pProperties);

} // namespace Pal::Wddm
