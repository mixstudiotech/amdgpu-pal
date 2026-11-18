// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"
#include "palQueue.h"
#include "core/os/wddm/wddmTransport.h"
#include "core/os/wddm/wddmSubmit.h"
#include <atomic>
#include <cstddef>
#include <vector>

namespace Pal { class CmdStream; class ICmdBuffer; class Platform; }
namespace Pal::Wddm
{
// Gang (multi-queue) facts recovered from the KMD GPU record. The original copies the 14848-byte record body to
// Device+0x9d40 (30a060), so Device+X is gpuRecord[X - 0x93e0] below.
struct GangTopology
{
    bool   hwsPresent = false;            // 30db10 gate: record+14176 bit24 and a gfx/SDMA/compute node in record+16164
    uint64 engineOrdinalMask = 0;         // Device+0x8218 (HwsInfo.engineOrdinalMask)
    uint32 gangSubmitEngineFlags = 0;     // Device+0x8228 = record+16152; graphics bit0, compute bit1, dma bit2
    bool   escapeRegisteredGangs = false; // Device+0x8248 = (record+16160 & 0xa) == 0xa, ANDed with Settings+0x3d9
};
GangTopology ReadGangTopology(const PhysicalProperties& physical);

// Engine index 12 asks "any instance of this engine type" (original sentinel in 30da00/3109c0/310ae0).
inline constexpr uint32 AnyEngineInstance = 12;
// Node ordinal of the index-th engine of this type, enumerated the way Queue::Init and FinalizeQueueProperties do.
uint32 GangEngineNode(const AdapterInfo& adapter, EngineType engineType, uint32 engineIndex);
// Native 30da00 for PAL engine types (the video-only engine type 7 does not exist in this interface).
bool IsHwsEngine(const AdapterInfo& adapter, const GangTopology& topology, EngineType engineType, uint32 engineIndex);
// Native 3109c0: objectSize when every entry is HWS-capable and of queue type 0..2, else 0 (objectSize for count 0).
size_t GangMultiQueueObjectSize(const AdapterInfo& adapter, uint32 queueCount, const QueueCreateInfo* createInfo,
                                size_t objectSize);
// Native 310ae0 construction predicate (count 0 is refused: the native reads createInfo[0] even then).
bool GangCanConstructMultiQueue(const AdapterInfo& adapter, uint32 queueCount, const QueueCreateInfo* createInfo);
// Native 30c370 capability bit 2 (supportsMultiQueue) for every instance of an engine type.
bool GangSupportsMultiQueue(const GangTopology& topology, EngineType engineType);
// Native 37a180: the escape-registered follower mode is selected only for a two-entry compute+compute gang.
bool GangUsesEscapeRegistration(const GangTopology& topology, uint32 queueCount, const QueueCreateInfo* createInfo);

// Follower HW queue private DWORD+16 (37a980 non-leader branch, 37b070): the queue's record plus 0x80000.
uint32 GangFollowerQueueFlags(const QueueCreateOptions& options);
// 37a980 with leader=false: a separate virtual context plus HW queue whose private record carries 0x80000 at +16
// and the leader's queue id at +24. No progress/mapping object is created for the follower. NativeProgressFence is
// only ever requested for the leader.
Status CreateGangFollower(Transport& transport, uint32 node, QueueEngine engine, const QueueCreateOptions& options,
                          uint32 leaderQueueId, QueueState* follower);
// 37b170: escape 0x03010603 registering followers of the leader queue id with the KMD.
Result RegisterGangFollowers(const Transport& transport, Pal::Platform* platform, uint32 leaderQueueId,
                             const uint32* followerFlags, uint32 followerCount);
// 309f40: escape 0x0300a603, sent at most once (the original keys it on Platform+0x27d0).
void NotifyGangSubmission(const Transport& transport, Pal::Platform* platform, std::atomic<bool>* sent);

// Per-queue gang state owned by Wddm::Queue for queueCount > 1.
struct GangQueueState
{
    std::atomic<bool>* notified = nullptr; // Device-owned one-time flag for 309f40.
    QueueState* followers = nullptr;       // Sub-queues 1..n-1 (separate HW queues mode only).
    uint32 followerCount = 0;
    bool escapeRegistered = false;         // Queue+0x1e1: followers exist only inside the KMD (37b170 succeeded).
    bool active = false;                   // Queue+0x35c: the first ganged submit idled the queue; stays set.
};

// The gang path's private stream (37d120/37cae0/37df20) is a SubmitStream (wddmSubmit.h): an 8-byte {ABI, entries}
// header, then per sub-queue a 24-byte type-6 record {6, 24 + 16n, word8, word12, n, 0} followed by n 16-byte IB
// records, then the type-4 records of the submission.
// 37df20 IB flags: postamble 0x200, preamble 8, preemption disabled 4, drop-if-same-context 2, constant engine 1.
uint32 GangIbFlags(const CmdStream& stream);
// 4477b0: how many leading command buffers can be chained into one IB per stream.
uint32 GangChainableCount(uint32 count, ICmdBuffer*const* cmdBuffers);
}
