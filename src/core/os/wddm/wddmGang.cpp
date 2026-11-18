// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Ganged multi-queue support recovered from amdvlk64.dll (SHA-256 36a9ef32...7a44); RVAs are cited per function.
#include "core/os/wddm/wddmGang.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmPrivate.h"
#include "core/cmdBuffer.h"
#include "core/cmdStream.h"
#include "core/cmdStreamAllocation.h"
#include "core/hw/gfxip/gfxCmdStream.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <array>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
// Device+X of the original equals gpuRecord[X - 0x93e0] for X in [0x9d40, 0xd740).
constexpr size_t GpuFlagsOffset      = 14176; // Device+0xcb40
constexpr size_t EngineBitsOffset    = 16148; // Device+0xd2f4
constexpr size_t GangFlagsOffset     = 16152; // Device+0xd2f8
constexpr size_t HwsFlagsOffset      = 16160; // Device+0xd300
constexpr size_t HwsNodeMaskOffset   = 16164; // Device+0xd304
constexpr uint32 GpuHwsSupported     = 0x1000000;
constexpr uint32 MaxEngineIds        = 62;    // 30c8b0 ignores engine ids >= 0x3e.
constexpr uint32 AbsentNode          = 63;    // 30c8b0 fills Device+0xdb94 with 63.
constexpr uint32 FollowerFlag        = 0x80000;
constexpr uint32 MaxRegisteredFollowers = 10; // 37b170 copies 4*n bytes to record+0x90 of a 0xb8-byte record.

uint64 NodeBit(uint32 node) { return uint64(1) << (node & 63); }

bool EngineMatches(const NodeInfo& node, EngineType engineType)
{
    switch (engineType)
    {
    case EngineTypeUniversal: return node.isGraphics;
    case EngineTypeCompute:   return node.isCompute;
    case EngineTypeDma:       return node.isDma;
    case EngineTypeTimer:     return node.isTimer;
    default:                  return false;
    }
}

bool AllGangEntriesAllowed(const AdapterInfo& adapter, uint32 count, const QueueCreateInfo* createInfo)
{
    // 3109c0/310ae0: once an entry fails, the HWS test is not evaluated again; queue types must be 0..2.
    const GangTopology topology = ReadGangTopology(adapter.physical);
    bool hws = true;
    bool types = true;
    for (uint32 i = 0; i < count; ++i)
    {
        if (hws) { hws = IsHwsEngine(adapter, topology, createInfo[i].engineType, AnyEngineInstance); }
        if ((createInfo[i].queueType != QueueTypeUniversal) && ((uint32(createInfo[i].queueType) - 1) >= 2))
            { types = false; }
    }
    return hws && types;
}
}

GangTopology ReadGangTopology(const PhysicalProperties& physical)
{
    const uint8* gpu = physical.gpuRecord.data();
    const uint32 gpuFlags = Private::Get32(gpu, GpuFlagsOffset);
    const uint32 hwsFlags = Private::Get32(gpu, HwsFlagsOffset);
    const uint64 hwsNodeMask = Private::Get64(gpu, HwsNodeMaskOffset);
    // 30c8b0 first loop: internal engine id (record+4604+node) -> node ordinal, in node order. Id 6 is not
    // recorded when the HWS gate holds and record+16160 bit2 is set; the gate uses the table built so far.
    std::array<uint32, MaxEngineIds> nodeOf;
    nodeOf.fill(AbsentNode);
    for (uint32 node = 0; node < physical.engineTypes.size(); ++node)
    {
        const uint32 id = physical.engineTypes[node];
        if (id >= MaxEngineIds) { continue; }
        if ((id == 6) && ((gpuFlags & GpuHwsSupported) != 0) &&
            (((NodeBit(nodeOf[5]) | NodeBit(nodeOf[4]) | NodeBit(nodeOf[0])) & hwsNodeMask) != 0) &&
            ((hwsFlags & 4) != 0)) { continue; }
        nodeOf[id] = node;
    }
    // 30db10: graphics (id 0), SDMA0 (id 4) and compute (id 5) nodes decide whether HWS is present.
    const uint64 coreNodes = NodeBit(nodeOf[5]) | NodeBit(nodeOf[0]) | NodeBit(nodeOf[4]);
    GangTopology topology = {};
    topology.hwsPresent = ((gpuFlags & GpuHwsSupported) != 0) && ((coreNodes & hwsNodeMask) != 0);
    if (topology.hwsPresent)
    {
        topology.gangSubmitEngineFlags = Private::Get32(gpu, GangFlagsOffset);
        if ((hwsFlags & 1) != 0)
        {
            topology.engineOrdinalMask = hwsNodeMask & coreNodes;
        }
        else
        {
            // Each set bit k of record+16148 is an engine id whose node joins the mask (tzcnt loop).
            const uint32 bits = Private::Get32(gpu, EngineBitsOffset);
            for (uint32 id = 0; id < 32; ++id)
            {
                if ((bits & (1u << id)) != 0) { topology.engineOrdinalMask |= NodeBit(nodeOf[id]); }
            }
        }
    }
    // Device+0x8248 is written unconditionally at the end of 30db10. Settings+0x3d9 (hash 1965135080) is
    // registered with default 1 by 4d0cb0; this backend has no override, so the default applies.
    topology.escapeRegisteredGangs = ((hwsFlags & 0xa) == 0xa);
    return topology;
}

uint32 GangEngineNode(const AdapterInfo& adapter, EngineType engineType, uint32 engineIndex)
{
    uint32 index = 0;
    for (const NodeInfo& node : adapter.nodes)
    {
        if (EngineMatches(node, engineType) && (index++ == engineIndex)) { return node.ordinal; }
    }
    return UINT32_MAX;
}

bool IsHwsEngine(const AdapterInfo& adapter, const GangTopology& topology, EngineType engineType, uint32 engineIndex)
{
    // 30da00: the GPU HWS flag and core-node test gate every non-video engine; then the instance's node must be in
    // HwsInfo.engineOrdinalMask. Index 12 tests every listed instance until the first missing one.
    if (!topology.hwsPresent) { return false; }
    const uint32 first = (engineIndex == AnyEngineInstance) ? 0 : engineIndex;
    const uint32 last = (engineIndex == AnyEngineInstance) ? MaxAvailableEngines : engineIndex + 1;
    for (uint32 index = first; index < last; ++index)
    {
        const uint32 node = GangEngineNode(adapter, engineType, index);
        if (node == UINT32_MAX) { break; }
        // The native tests only the low DWORDs ("test ebx, edx"), so nodes 32..63 never qualify.
        if ((node < 64) && ((uint32(topology.engineOrdinalMask) & uint32(NodeBit(node))) != 0)) { return true; }
    }
    return false;
}

size_t GangMultiQueueObjectSize(const AdapterInfo& adapter, uint32 queueCount, const QueueCreateInfo* createInfo,
                                size_t objectSize)
{
    // 3109c0 returns its object size (912) for a zero count without reading the create infos.
    if (queueCount == 0) { return objectSize; }
    if (createInfo == nullptr) { return 0; }
    return AllGangEntriesAllowed(adapter, queueCount, createInfo) ? objectSize : 0;
}

bool GangCanConstructMultiQueue(const AdapterInfo& adapter, uint32 queueCount, const QueueCreateInfo* createInfo)
{
    // 310ae0 constructs a zero-entry queue after reading createInfo[0], which may be null; that case is refused.
    if ((queueCount == 0) || (createInfo == nullptr)) { return false; }
    return AllGangEntriesAllowed(adapter, queueCount, createInfo) &&
           ((createInfo[0].queueType == QueueTypeUniversal) || ((uint32(createInfo[0].queueType) - 1) <= 1));
}

bool GangSupportsMultiQueue(const GangTopology& topology, EngineType engineType)
{
    // 30c370: inside the HWS gate, universal takes gang flag bit0, compute bit1; SDMA and others are cleared.
    if (!topology.hwsPresent) { return false; }
    switch (engineType)
    {
    case EngineTypeUniversal: return (topology.gangSubmitEngineFlags & 1) != 0;
    case EngineTypeCompute:   return (topology.gangSubmitEngineFlags & 2) != 0;
    default:                  return false;
    }
}

bool GangUsesEscapeRegistration(const GangTopology& topology, uint32 queueCount, const QueueCreateInfo* createInfo)
{
    // 37a180 stores Queue+0x35d: one universal entry, or exactly two compute entries.
    if (!topology.escapeRegisteredGangs || (createInfo == nullptr)) { return false; }
    if (queueCount == 1) { return createInfo[0].engineType == EngineTypeUniversal; }
    if (queueCount == 2)
    {
        return (createInfo[0].engineType == EngineTypeCompute) && (createInfo[1].engineType == EngineTypeCompute);
    }
    return false;
}

uint32 GangFollowerQueueFlags(const QueueCreateOptions& options)
{
    return Private::HwQueuePrivateFlags(options) | FollowerFlag;
}

Status CreateGangFollower(Transport& transport, uint32 node, QueueEngine engine, const QueueCreateOptions& options,
                          uint32 leaderQueueId, QueueState* follower)
{
    if (!transport.IsOpen()) { return DeviceRemoved; }
    if ((follower == nullptr) || (follower->handle != 0) || (follower->context != 0) || (options.priority >= 5))
        { return InvalidParameter; }
    if ((engine != QueueEngine::Graphics) && (engine != QueueEngine::Compute)) { return NotSupported; }
    // Same virtual context as the corresponding single queue (Transport::CreateQueue).
    auto data = Private::Context(engine == QueueEngine::Compute);
    D3DKMT_CREATECONTEXTVIRTUAL context = {};
    context.hDevice = transport.DeviceHandle();
    context.NodeOrdinal = node;
    context.EngineAffinity = 1;
    context.pPrivateDriverData = data.data();
    context.PrivateDriverDataSize = uint32_t(data.size());
    context.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
    context.Flags.HwQueueSupported = 1;
    context.Flags.NullRendering = options.nullRendering;
    Status status = D3DKMTCreateContextVirtual(&context);
    if (!Succeeded(status)) { return status; }
    // 37a980, leader=false: DWORD+16 |= 0x80000 and DWORD+24 = DWORD+20 returned for the leader.
    auto queueData = Private::QueueData(options);
    Private::Put32(queueData.data(), 16, Private::Get32(queueData.data(), 16) | FollowerFlag);
    Private::Put32(queueData.data(), 24, leaderQueueId);
    D3DKMT_CREATEHWQUEUE create = {};
    create.hHwContext = context.hContext;
    create.Flags.Value = options.disableGpuTimeout ? 0x1u : 0u;
    create.pPrivateDriverData = queueData.data();
    create.PrivateDriverDataSize = uint32_t(queueData.size());
    status = D3DKMTCreateHwQueue(&create);
    if (!Succeeded(status))
    {
        D3DKMT_DESTROYCONTEXT destroy = { context.hContext };
        D3DKMTDestroyContext(&destroy);
        return status;
    }
    *follower = {};
    follower->engine = engine;
    follower->hardwareQueue = true;
    follower->context = context.hContext;
    follower->handle = create.hHwQueue;
    follower->queueId = Private::Get32(queueData.data(), 20);
    follower->progressFence = create.hHwQueueProgressFence;
    follower->progressCpu = static_cast<const volatile uint64_t*>(create.HwQueueProgressFenceCPUVirtualAddress);
    follower->progressGpu = create.HwQueueProgressFenceGPUVirtualAddress;
    return Success;
}

Result RegisterGangFollowers(const Transport& transport, Pal::Platform* platform, uint32 leaderQueueId,
                             const uint32* followerFlags, uint32 followerCount)
{
    if ((followerFlags == nullptr) || (followerCount == 0) || (followerCount > MaxRegisteredFollowers))
        { return Result::ErrorInvalidValue; }
    std::array<uint8, 0xb8> record = {};
    InitPrivateRecord(record.data(), record.size(), 0x03010603);
    PutRecord32(record.data(), 0x0c, 3);
    PutRecord32(record.data(), 0x80, 0x10);
    PutRecord32(record.data(), 0x84, leaderQueueId);
    PutRecord32(record.data(), 0x88, 1);
    PutRecord32(record.data(), 0x8c, followerCount);
    PutRecord32(record.data(), 0x94, 0x20); // Stored before the flag copy, which may overwrite it.
    std::memcpy(record.data() + 0x90, followerFlags, sizeof(uint32) * followerCount);
    Result result = SharedEscape(DeviceEscapeRoute(transport, platform), record.data(), record.size(), false, 0);
    if ((result == Result::Success) && (GetRecord32(record.data(), 0x98) == 0)) { result = Result::ErrorUnknown; }
    return result;
}

void NotifyGangSubmission(const Transport& transport, Pal::Platform* platform, std::atomic<bool>* sent)
{
    // 309f40 sets its flag before the escape and 4d8c00 ignores the result.
    if ((sent == nullptr) || sent->exchange(true)) { return; }
    std::array<uint8, 0x188> record = {};
    InitPrivateRecord(record.data(), record.size(), 0x0300a603);
    PutRecord32(record.data(), 0x0c, 3);
    PutRecord32(record.data(), 0x80, 0x80);
    PutRecord32(record.data(), 0x84, 0xc);
    PutRecord32(record.data(), 0x88, 1);
    PutRecord32(record.data(), 0x104, 0x80);
    SharedEscape(DeviceEscapeRoute(transport, platform), record.data(), record.size(), false, 0);
}

uint32 GangIbFlags(const CmdStream& stream)
{
    return IbRecordFlags(stream);
}

uint32 GangChainableCount(uint32 count, ICmdBuffer*const* cmdBuffers)
{
    if ((count == 0) || (cmdBuffers == nullptr) || (cmdBuffers[0] == nullptr)) { return 0; }
    const auto* first = static_cast<const CmdBuffer*>(cmdBuffers[0]);
    // The native compares cmd-buffer DWORD+0x5d8 of each candidate with the first; the matching core state is
    // the TMZ build flag, which the Linux backend's batching rule also compares.
    uint32 batch = 1;
    while (batch < count)
    {
        const auto* previous = static_cast<const CmdBuffer*>(cmdBuffers[batch - 1]);
        const auto* current = static_cast<const CmdBuffer*>(cmdBuffers[batch]);
        if ((current == nullptr) || !previous->IsExclusiveSubmit() || (current->IsTmzEnabled() != first->IsTmzEnabled()))
            { break; }
        ++batch;
    }
    const uint32 streams = first->NumCmdStreams();
    for (uint32 streamIndex = 0; streamIndex < streams; ++streamIndex)
    {
        const CmdStream* previous = nullptr;
        uint32 index = 0;
        for (; index < batch; ++index)
        {
            const CmdStream* current = static_cast<const CmdBuffer*>(cmdBuffers[index])->GetCmdStream(streamIndex);
            if ((current == nullptr) || current->IsEmpty()) { continue; }
            if ((previous != nullptr) &&
                ((static_cast<const GfxCmdStream*>(previous)->m_pTailChainLocation == nullptr) ||
                 (previous->IsPreemptionEnabled() != current->IsPreemptionEnabled()))) { break; }
            previous = current;
        }
        batch = index;
    }
    return batch;
}

}
