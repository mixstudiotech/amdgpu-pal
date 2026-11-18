// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Queue creation flags and submission records of the original Windows PAL queues, recovered from amdvlk64.dll
// (SHA-256 36a9ef32...7a44). Per-function evidence: .build/pal-wddm-logic/submit/analysis.txt.
#pragma once
#include "pal.h"
#include "palQueue.h"
#include "core/os/wddm/wddmTransport.h"
#include <vector>

namespace Pal
{
class CmdStream;
class GfxDevice;
class IGpuMemory;
struct CmdBufInfo;
}

namespace Pal::Wddm
{
// ---------------------------------------------------------------------------------------------------------------------
// Queue creation.

// EarlyInit 30b529..30b5f4 sets Device+0x13c0 bit1 (TMZ queues) when GPU record 5540 bit1 is set, and clears it
// again when the TmzEnabled setting (Settings+0xa84, hash 2606194033, default true) is off.
bool TmzQueuesSupported(const PhysicalProperties& physical, bool tmzEnabledSetting);
// Pal::Queue::Init 31c721..31c754: a queue whose tmzOnly bit is not in Device+0x13c0 fails with ErrorUnavailable;
// otherwise Queue+0x9c = Device+0x13c5[tmzOnly] (0 for ordinary queues, 1 for TMZ queues: 30b547).
uint32 QueueTmzLevel(bool tmzOnly);
// 37a980 (HW queue), 383daf (legacy context) and 38b2a4/38b380 (timer context): GPU timeouts are disabled when
// PalPublicSettings::longRunningSubmissions (Device+0x8bd1) is set or the node's bit is in Device+0xd318
// (GPU record 16184).
bool DisableGpuTimeoutForNode(const PhysicalProperties& physical, bool longRunningSubmissions, uint32 node);
// Native fences are selected only by the EnableNativeFence setting (Settings+0x3dc, hash 3608331410, default 0 at
// 4d120a); this backend has no registry override, so the setting is always 0.
constexpr uint32 EnableNativeFenceSetting = 0;
// 37a980 -> 4d8010: a HW queue of an engine whose node is in HwsInfo.engineOrdinalMask gets a user-queue
// (context-save-area) allocation of HwsInfo.<engine>.userQueueSize bytes (GPU record 16108 universal, 16112 compute,
// 30db10), passed in HW-queue private bytes +4 (translated handle) / +8 (VA). Returns that size; zero means no
// allocation, which is the case on the gated profile.
uint32 UserQueueBytes(const PhysicalProperties& physical, QueueType queueType);
// Native-fence selector, identical in 311840 (semaphore creation) and 37b270 (Queue+0x35f, the NativeProgressFence
// HW-queue flag): WDDM >= 3.2 adapter, nonzero native-fence node mask (Device+0x8238 = GPU record 16828), HWS
// enabled and the EnableNativeFence setting (1, or 2 with Device+0x8c3c). The original never queries
// KMTQAITYPE_WDDM_3_1_CAPS.
bool NativeFenceSelected(const PhysicalProperties& physical, bool hwsEnabled);

// QueueCreateOptions (wddmTransport.h) carries these values into the KMD records; Private::HwQueuePrivateFlags
// (wddmPrivate.h) builds the 37a980 HW-queue DWORD+16 from it.

// ---------------------------------------------------------------------------------------------------------------------
// Submission private data (native Queue+0x240 buffer, grown by 4db570): an 8-byte header {ABI version, record count}
// followed by records. Type 0 is the 64-byte IB record of the ordinary path (4dbaf0), type 6 the per-sub-queue record
// of the ganged path (37cae0) followed by 16-byte IB records (37df20), type 4 a 72-byte KMD record and type 8 a
// 32-byte record.
class SubmitStream
{
public:
    SubmitStream();

    // 4d9940 + 4dbaf0: {0, 64, bytes, flags, 0, ipType, va, +32 translated context-save-area handle}.
    void AddIb(uint64 va, uint32 bytes, uint32 flags, uint32 ipType, uint32 contextSaveHandle);
    // 72-byte type-4 record with DWORD+8 = (subtype << 10) | low; returns the record offset.
    size_t AddRecord4(uint32 subtype, uint32 low = 0);
    void Put32(size_t at, uint32 value);
    void Put64(size_t at, uint64 value);

    // 37cae0/37d120: 24-byte type-6 record {6, 24 + 16n, word8, word12, n, 0}, then 16-byte IB records.
    size_t BeginQueue(uint32 word8, uint32 word12);
    void AddGangIb(uint64 va, uint32 bytes, uint32 flags);
    void AddGangStream(const CmdStream& stream);
    void EndQueue(size_t header);

    // 37b9c0/384670: {Platform+0x27c8 ABI version, Queue+0x258 record count}.
    void Finalize(uint32 abiVersion);

    const std::vector<uint8>& Data() const { return m_data; }
    uint32 Entries() const { return m_entries; }
    uint64 FirstIbAddress() const { return m_firstAddress; }
    uint32 FirstIbBytes() const { return m_firstBytes; }

private:
    void NoteFirstIb(uint64 va, uint32 bytes);

    std::vector<uint8> m_data;
    uint32 m_entries  = 0;
    uint32 m_queueIbs = 0;
    uint64 m_firstAddress = 0; // Queue+0x230: the first IB record's address and size become CommandBuffer/Length.
    uint32 m_firstBytes   = 0;
};

// 4d9940 / 37df20 IB flags: constant engine 1, drop-if-same-context 2, preemption disabled 4, preamble 8,
// postamble 0x200; 4d9940 adds 0x400 for an async-compute stream when Device+0xd314 (GPU record 16180) bit2 is set.
uint32 IbRecordFlags(const CmdStream& stream);
bool IbRecordAceFlag(const PhysicalProperties& physical, const CmdStream& stream);

// Type-4 subtypes written by the original.
inline constexpr uint32 RecordTrap          = 186; // 4db8e0 (0x2e800): low = 0 compute handler, 1 compute buffer,
                                                   //   2 graphics handler, 3 graphics buffer; +16 = GPU VA.
inline constexpr uint32 RecordPerfExperiment = 190; // 4db6d0 (0x2f800): low = 1 SPM fallback, 3 perf counters, 2 SQTT.
inline constexpr uint32 RecordExternPhys    = 179; // 4dc220 (0x2cc00): +16 = remote SDI surface / marker index.
inline constexpr uint32 RecordDoppCurrent   = 187; // 37b9c0/384670 via 4dc350: DOPP of the onscreen primary,
inline constexpr uint32 RecordDoppLastPfpa  = 205; //   last pre-flip-primary access of the frame (last batch),
inline constexpr uint32 RecordDoppPfpa      = 206; //   pre-flip-primary access.
inline constexpr uint32 RecordTmz           = 201; // 4dc410(201): the batch's first command buffer is TMZ.
inline constexpr uint32 RecordCmdBufInfo    = 202; // 4dbbc0 (0x32800).
inline constexpr uint32 RecordVpBlt         = 223; // 4dbf00 (0x37c00): low = byte mask << 0 | group << 8.
inline constexpr uint32 RecordDirectCapture = 224; // 4dc4b0 (0x38000).

using AllocationHandleOf = uint32 (*)(const IGpuMemory& memory); // Native 2b3f20 (KmtAllocationHandle).

// 4dbbc0 (+ 4dc4b0 when frameBegin/privateFlip bits 6, 7 or 11 are set): CmdBufInfo records of one command buffer.
// flipOwner is Device+0xd9d8 (KmdSharedBuffers::flipOwner); gpuCaps14188 is Device+0xcb4c (GPU record 14188).
void AddCmdBufInfoRecords(SubmitStream* stream, const CmdBufInfo& info, uint64 flipOwner, uint32 gpuCaps14188,
                          AllocationHandleOf handleOf);
// 4dbf00 with the vpBltExecuted bit: universal queues send mask 0x10 & Device+0xd310 (GPU record 16176).
void AddVpBltRecords(SubmitStream* stream, uint32 mask, const uint32* values);
// 4db8e0: trap buffer then trap handler for compute, then (queues other than compute) for graphics, each only when
// GfxDevice::BindTrapBuffer / BindTrapHandler bound memory. Bindings: compute buffer, compute handler, graphics
// buffer, graphics handler; address = bound memory VA + offset.
struct TrapBinding
{
    bool   bound   = false;
    uint64 address = 0;
};
void AddTrapRecords(SubmitStream* stream, const TrapBinding (&bindings)[4], bool computeQueue);
void AddTrapRecords(SubmitStream* stream, const GfxDevice& gfxDevice, bool computeQueue);
// 4dc220: two records per external physical allocation (remote SDI surface and marker indices, low DWORDs).
void AddExternPhysRecord(SubmitStream* stream, uint64 surfaceIndex, uint64 markerIndex);
void AddExternPhysRecords(SubmitStream* stream, uint32 count, const IGpuMemory* const* memories);
// 4db6d0 for the OR of GfxCmdBuffer::PerfTracesEnabled() (bit0 perf counters, bit1 SPM, bit2 SQTT). spmEscapeFailed is
// Queue+0x2b0; gfx103 is (Device+0x2020 & 0xffff0000) == 0x0a030000.
void AddPerfExperimentRecords(SubmitStream* stream, uint32 perfFlags, bool spmEscapeFailed, bool gfx103);
// 4daee0: per-context SPM escape 0x03010103, sent when the queue's perf flags change. Returns the escape result.
Result SendSpmContextEscape(const struct EscapeRoute& route, uint32 perfFlags, uint32 hContext);
}
