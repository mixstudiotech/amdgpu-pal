/*
 * Copyright (c) 2026. SPDX-License-Identifier: MIT
 * AMD private query layouts derived from the Mesa WDDM2 reference.
 */
#include "wddmProperties.h"

#include <cstring>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
namespace
{

constexpr int32_t InvalidParameter = static_cast<int32_t>(0xc000000du);

template <typename T, size_t Size>
T Read(const std::array<uint8_t, Size>& record, size_t offset)
{
    T value;
    std::memcpy(&value, record.data() + offset, sizeof(value));
    return value;
}

template <size_t Size>
int32_t QueryRecord(uint32_t adapter, std::array<uint8_t, Size>* pRecord)
{
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter               = adapter;
    query.Type                   = KMTQAITYPE_UMDRIVERPRIVATE;
    query.pPrivateDriverData     = pRecord->data();
    query.PrivateDriverDataSize  = Size;
    return D3DKMTQueryAdapterInfo(&query);
}

} // namespace

int32_t QueryPhysicalProperties(uint32_t kmtAdapterHandle, PhysicalProperties* pProperties)
{
    if ((kmtAdapterHandle == 0) || (pProperties == nullptr))
    {
        return InvalidParameter;
    }

    PhysicalProperties properties;
    int32_t status = QueryRecord(kmtAdapterHandle, &properties.basicRecord);
    if (status < 0)
    {
        return status;
    }
    status = QueryRecord(kmtAdapterHandle, &properties.gpuRecord);
    if (status < 0)
    {
        return status;
    }

    const auto& basic = properties.basicRecord;
    const auto& gpu   = properties.gpuRecord;

    properties.deviceId           = Read<uint32_t>(gpu, 4960);
    properties.familyId           = Read<uint32_t>(gpu, 4964);
    properties.externalRevision   = Read<uint32_t>(gpu, 4968);
    properties.deviceData4972     = Read<uint32_t>(gpu, 4972);
    properties.gfxEngineId        = Read<uint32_t>(gpu, 4976);
    properties.subsystemId        = Read<uint32_t>(basic, 2456);
    properties.vramType           = Read<uint32_t>(basic, 2484);
    properties.vramBitWidth       = Read<uint32_t>(basic, 2488);
    properties.numRbPipes         = Read<uint32_t>(basic, 2992);
    properties.numEnabledRbPipes  = Read<uint32_t>(basic, 2996);
    properties.enabledRbMask      = Read<uint32_t>(basic, 3000) |
                                    (uint64_t{Read<uint32_t>(basic, 3004)} << 32);
    properties.numEngines         = Read<uint32_t>(gpu, 4600);
    std::memcpy(properties.engineTypes.data(), gpu.data() + 4604, properties.engineTypes.size());
    properties.meFirmwareVersion  = Read<uint32_t>(gpu, 4688);
    properties.cpFeatureVersion   = Read<uint32_t>(basic, 2920);
    properties.mecFirmwareVersion = Read<uint32_t>(gpu, 4692);
    properties.pfpFirmwareVersion = Read<uint32_t>(gpu, 4696);
    properties.crystalFrequencyHz = Read<uint32_t>(gpu, 4996);
    properties.fallbackMaxEngineClockMHz = Read<uint16_t>(gpu, 5000);
    properties.fallbackMaxMemoryClockMHz = Read<uint16_t>(gpu, 5002);
    properties.visibleVramSize    = Read<uint64_t>(gpu, 5008);
    properties.invisibleVramSize  = Read<uint64_t>(gpu, 5016);
    properties.gbAddrConfig       = Read<uint32_t>(gpu, 5472);
    properties.numShaderVisibleVgprs = Read<uint32_t>(gpu, 5560);
    properties.wavefrontSize         = Read<uint32_t>(gpu, 5564);
    properties.numShaderEngines      = Read<uint32_t>(gpu, 5568);
    properties.numActiveRbPerSe      = Read<uint32_t>(gpu, 5572);
    properties.numShaderArraysPerEngine = Read<uint32_t>(gpu, 5576);
    properties.numTccBlocks          = Read<uint32_t>(gpu, 5588);
    properties.maxGsWavesPerVgt       = Read<uint32_t>(gpu, 5592);
    properties.gsVgtTableDepth        = Read<uint32_t>(gpu, 5600);
    properties.gsPrimBufferDepth      = Read<uint32_t>(gpu, 5604);
    properties.numCuPerWgp            = Read<uint32_t>(gpu, 5700);
    std::memcpy(properties.wgpBitmap.data(), gpu.data() + 5732, sizeof(properties.wgpBitmap));
    std::memcpy(properties.alwaysOnWgpBitmap.data(), gpu.data() + 5756, sizeof(properties.alwaysOnWgpBitmap));
    properties.numSqcPerWgp           = Read<uint32_t>(gpu, 5820);
    properties.sqcInstructionCacheSize = Read<uint32_t>(gpu, 5824);
    properties.sqcDataCacheSize       = Read<uint32_t>(gpu, 5828);
    properties.gl1CacheSize           = Read<uint32_t>(gpu, 5832);
    properties.gl2CacheSize           = Read<uint32_t>(gpu, 5840);
    properties.mallSizeMb             = Read<uint32_t>(gpu, 5860);
    properties.gpuVaStart             = Read<uint64_t>(gpu, 13160);
    properties.gpuVaEnd               = Read<uint64_t>(gpu, 13168);
    properties.largePageEncoding      = Read<uint32_t>(gpu, 14200);
    for (size_t i = 0; i < properties.ibAlignments.size(); ++i)
    {
        properties.ibAlignments[i].start = Read<uint32_t>(gpu, 15084 + (i * 8));
        properties.ibAlignments[i].size  = Read<uint32_t>(gpu, 15088 + (i * 8));
    }

    if ((properties.deviceId != Read<uint32_t>(basic, 2444)) ||
        (properties.familyId != Read<uint32_t>(basic, 2448)) ||
        (properties.externalRevision != Read<uint32_t>(basic, 2452)) ||
        (properties.deviceId == 0) ||
        (properties.numEngines > properties.engineTypes.size()) ||
        (properties.numShaderEngines == 0) ||
        (properties.numShaderEngines > PhysicalProperties::MaxShaderEngines) ||
        (properties.numShaderArraysPerEngine == 0) ||
        (properties.numShaderArraysPerEngine > PhysicalProperties::MaxShaderArrays) ||
        (properties.gpuVaStart >= properties.gpuVaEnd))
    {
        return InvalidParameter;
    }

    *pProperties = properties;
    return status;
}

} // namespace Pal::Wddm
