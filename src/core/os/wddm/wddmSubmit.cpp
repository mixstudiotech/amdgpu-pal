// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Queue creation flags and submission records recovered from amdvlk64.dll (SHA-256 36a9ef32...7a44); RVAs per function.
#include "core/os/wddm/wddmSubmit.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmPrivate.h"
#include "core/cmdStream.h"
#include "core/cmdStreamAllocation.h"
#include "core/gpuMemory.h"
#include "core/hw/gfxip/gfxDevice.h"
#include "palCmdBuffer.h"
#include <array>
#include <bit>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
constexpr size_t   HeaderBytes   = 8;
constexpr size_t   IbRecordBytes = 64;
constexpr size_t   Record4Bytes  = 72;
constexpr size_t   QueueRecordBytes = 24;
constexpr size_t   GangIbBytes   = 16;
constexpr uint32   RecordTypeIb    = 0;
constexpr uint32   RecordType4     = 4;
constexpr uint32   RecordTypeQueue = 6;
// GPU record offsets (Device+X == gpuRecord[X - 0x93e0]).
constexpr size_t   GpuTmzCaps            = 5540;  // Device+0xa984, bit1: TMZ supported.
constexpr size_t   GpuNoTimeoutNodes     = 16184; // Device+0xd318, qword node mask.
constexpr size_t   GpuFeatures16180      = 16180; // Device+0xd314.
constexpr size_t   GpuNativeFenceNodes   = 16828; // Device+0xd59c -> HwsInfo.nativeFenceNodeOrdinalMask (30db10).
constexpr size_t   GpuUserQueueBytes     = 16108; // Device+0xd2cc -> HwsInfo.gfx/compute/sdma.userQueueSize (30db10).
}

bool TmzQueuesSupported(const PhysicalProperties& physical, bool tmzEnabledSetting)
{
    return ((Private::Get32(physical.gpuRecord.data(), GpuTmzCaps) & 2) != 0) && tmzEnabledSetting;
}

uint32 QueueTmzLevel(bool tmzOnly)
{
    // Device+0x13c6 is set to 1 at 30b547 together with the TMZ bit; Device+0x13c5 has no writer (zero-initialized).
    return tmzOnly ? 1u : 0u;
}

bool DisableGpuTimeoutForNode(const PhysicalProperties& physical, bool longRunningSubmissions, uint32 node)
{
    // 37a980: _bittest64 on the qword, so nodes 0..63 only.
    return longRunningSubmissions ||
           ((node < 64) && (((Private::Get64(physical.gpuRecord.data(), GpuNoTimeoutNodes) >> node) & 1) != 0));
}

uint32 UserQueueBytes(const PhysicalProperties& physical, QueueType queueType)
{
    // 30db10 (HWS branch): Device+0x81f4 / +0x81fc / +0x8204 = Device+0xd2cc / +0xd2d0 / +0xd2d4.
    switch (queueType)
    {
    case QueueTypeUniversal: return Private::Get32(physical.gpuRecord.data(), GpuUserQueueBytes);
    case QueueTypeCompute:   return Private::Get32(physical.gpuRecord.data(), GpuUserQueueBytes + 4);
    default:                 return 0;
    }
}

bool NativeFenceSelected(const PhysicalProperties& physical, bool hwsEnabled)
{
    // 37b270: v11 = (adapter WDDM version >= 0x3200 && nativeFenceNodeOrdinalMask) ? (osHwsEnableFlags != 0) : false;
    // Queue+0x35f = v11 && setting && (setting == 1 || Device+0x8c3c). The setting is evaluated first here because it
    // is always 0; the adapter WDDM version (adapter object +0x8b8, filled by 3025d0) is not captured by this backend.
    if (EnableNativeFenceSetting == 0) { return false; }
    return (Private::Get64(physical.gpuRecord.data(), GpuNativeFenceNodes) != 0) && hwsEnabled;
}

// ---------------------------------------------------------------------------------------------------------------------
SubmitStream::SubmitStream() : m_data(HeaderBytes, 0) {}

void SubmitStream::NoteFirstIb(uint64 va, uint32 bytes)
{
    if (m_firstAddress == 0)
    {
        m_firstAddress = va;
        m_firstBytes   = bytes;
    }
}

void SubmitStream::AddIb(uint64 va, uint32 bytes, uint32 flags, uint32 ipType, uint32 contextSaveHandle)
{
    const size_t at = m_data.size();
    m_data.resize(at + IbRecordBytes, 0);
    Private::Put32(m_data.data(), at, RecordTypeIb);
    Private::Put32(m_data.data(), at + 4, IbRecordBytes);
    Private::Put32(m_data.data(), at + 8, bytes);
    Private::Put32(m_data.data(), at + 12, flags);
    Private::Put32(m_data.data(), at + 20, ipType);
    Private::Put64(m_data.data(), at + 24, va);
    Private::Put32(m_data.data(), at + 32, contextSaveHandle);
    ++m_entries;
    NoteFirstIb(va, bytes);
}

size_t SubmitStream::AddRecord4(uint32 subtype, uint32 low)
{
    const size_t at = m_data.size();
    m_data.resize(at + Record4Bytes, 0);
    Private::Put32(m_data.data(), at, RecordType4);
    Private::Put32(m_data.data(), at + 4, Record4Bytes);
    Private::Put32(m_data.data(), at + 8, ((subtype & 0xff) << 10) | (low & 0x3ff));
    ++m_entries;
    return at;
}

void SubmitStream::Put32(size_t at, uint32 value) { Private::Put32(m_data.data(), at, value); }
void SubmitStream::Put64(size_t at, uint64 value) { Private::Put64(m_data.data(), at, value); }

size_t SubmitStream::BeginQueue(uint32 word8, uint32 word12)
{
    const size_t at = m_data.size();
    m_data.resize(at + QueueRecordBytes, 0);
    Private::Put32(m_data.data(), at, RecordTypeQueue);
    Private::Put32(m_data.data(), at + 8, word8);
    Private::Put32(m_data.data(), at + 12, word12);
    ++m_entries;
    m_queueIbs = 0;
    return at;
}

void SubmitStream::AddGangIb(uint64 va, uint32 bytes, uint32 flags)
{
    const size_t at = m_data.size();
    m_data.resize(at + GangIbBytes, 0);
    Private::Put32(m_data.data(), at, bytes);
    Private::Put32(m_data.data(), at + 4, flags);
    Private::Put64(m_data.data(), at + 8, va);
    NoteFirstIb(va, bytes);
    ++m_queueIbs;
}

void SubmitStream::AddGangStream(const CmdStream& stream)
{
    const CmdStreamChunk* chunk = stream.GetFirstChunk();
    AddGangIb(chunk->GpuVirtAddr(), chunk->CmdDwordsToExecute() * uint32(sizeof(uint32)), IbRecordFlags(stream));
}

void SubmitStream::EndQueue(size_t header)
{
    // 37cae0 patches size and count only when at least one IB was added.
    if (m_queueIbs != 0)
    {
        Private::Put32(m_data.data(), header + 4, uint32(QueueRecordBytes + GangIbBytes * m_queueIbs));
        Private::Put32(m_data.data(), header + 16, m_queueIbs);
    }
}

void SubmitStream::Finalize(uint32 abiVersion)
{
    Private::Put32(m_data.data(), 0, abiVersion);
    Private::Put32(m_data.data(), 4, m_entries);
}

// ---------------------------------------------------------------------------------------------------------------------
uint32 IbRecordFlags(const CmdStream& stream)
{
    const CmdStreamUsage usage = stream.GetCmdStreamUsage();
    return ((stream.GetSubEngineType() == SubEngineType::ConstantEngine) ? 0x1u : 0u) |
           (stream.DropIfSameContext() ? 0x2u : 0u) |
           (stream.IsPreemptionEnabled() ? 0u : 0x4u) |
           ((usage == CmdStreamUsage::Preamble) ? 0x8u : 0u) |
           ((usage == CmdStreamUsage::Postamble) ? 0x200u : 0u);
}

bool IbRecordAceFlag(const PhysicalProperties& physical, const CmdStream& stream)
{
    return (stream.GetSubEngineType() == SubEngineType::AsyncCompute) &&
           ((Private::Get32(physical.gpuRecord.data(), GpuFeatures16180) & 4) != 0);
}

void AddCmdBufInfoRecords(SubmitStream* stream, const CmdBufInfo& info, uint64 flipOwner, uint32 gpuCaps14188,
                          AllocationHandleOf handleOf)
{
    const uint32 flags = info.u32All;
    const size_t at = stream->AddRecord4(RecordCmdBufInfo);
    // 4dbbc0: frameBegin, frameEnd, p2pCmd, disableDccRejected, rayTracingExecuted, noRenderPresent and the two
    // newer bits 29/30 (reserved in this interface) into DWORD+16; bits 29/30 also select size fields this interface
    // does not have.
    const uint32 bits = ((flags >> 1) & 1) | ((flags >> 3) & 2) | ((flags >> 3) & 4) | ((flags >> 10) & 8) |
                        ((flags >> 4) & 0x10) | ((flags >> 14) & 0x20) | ((flags >> 23) & 0x40) |
                        ((flags >> 23) & 0x80);
    stream->Put32(at + 16, bits);
    stream->Put64(at + 24, flipOwner);
    stream->Put64(at + 32, info.frameId);
    if (info.pPrimaryMemory != nullptr) { stream->Put32(at + 20, handleOf(*info.pPrimaryMemory)); }
    if (info.pMotionVectorMemory != nullptr) { stream->Put32(at + 44, handleOf(*info.pMotionVectorMemory)); }
    else if (info.motionVectorPropChanged) { stream->Put32(at + 44, UINT32_MAX); }
    if (info.pDepthMemory != nullptr) { stream->Put32(at + 40, handleOf(*info.pDepthMemory)); }
    else if (info.depthPropChanged) { stream->Put32(at + 40, UINT32_MAX); }
    if (info.pCameraMemory != nullptr) { stream->Put32(at + 48, handleOf(*info.pCameraMemory)); }
    else if (info.cameraPropChanged) { stream->Put32(at + 48, UINT32_MAX); }
    // Bit 27 (reserved in this interface) pairs with a memory object at original CmdBufInfo+0x50 that this interface
    // does not have; without it the original writes UINT32_MAX.
    if ((flags & 0x8000000) != 0) { stream->Put32(at + 52, UINT32_MAX); }

    if ((info.captureBegin != 0) || (info.captureEnd != 0) || (info.privateFlip != 0))
    {
        // 4dc4b0 DirectCapture record. A DirectCapture memory object (+0x130 id, +8 VA, +0x10 size) cannot be created
        // by this backend; the caller rejects that case before building the stream.
        const size_t capture = stream->AddRecord4(RecordDirectCapture);
        if ((info.pDirectCapMemory == nullptr) && (info.privateFlip != 0)) { stream->Put32(capture + 32, info.vidPnSourceId); }
        uint32 captureBits = 0;
        if ((gpuCaps14188 & 0x1000) != 0)
        {
            captureBits |= ((flags & 0x240) == 0x240) ? 0x2u : 0u;
            captureBits |= ((flags & 0x280) == 0x280) ? 0x4u : 0u;
            captureBits |= ((flags & 0x440) == 0x440) ? 0x10u : 0u;
            captureBits |= ((flags & 0x480) == 0x480) ? 0x20u : 0u;
            captureBits |= ((flags >> 8) & 0x8) | ((flags >> 8) & 0x40) | ((flags >> 8) & 0x780) |
                           ((flags >> 12) & 0x800) | ((flags >> 12) & 0x1000) | ((flags >> 12) & 0x2000) |
                           ((flags >> 12) & 0x4000) | ((flags >> 13) & 0x8000);
        }
        else
        {
            captureBits = (flags >> 7) & 1;
        }
        stream->Put32(capture + 36, captureBits);
        stream->Put64(capture + 48, info.frameIndex);
    }
}

void AddVpBltRecords(SubmitStream* stream, uint32 mask, const uint32* values)
{
    // 4dbf00 (engines without physical addressing): one record per nonzero mask byte; the byte and its group index
    // form the low 10 bits and the selected values follow at +16.
    uint32 consumed = 0;
    for (uint32 group = 0; mask != 0; ++group, mask >>= 8)
    {
        const uint32 byte = mask & 0xff;
        if (byte == 0) { continue; }
        const size_t at = stream->AddRecord4(RecordVpBlt, (byte | (group << 8)) & 0x3ff);
        const uint32 count = uint32(std::popcount(byte));
        for (uint32 i = 0; i < count; ++i) { stream->Put32(at + 16 + 4 * i, values[consumed + i]); }
        consumed += count;
    }
}

void AddTrapRecords(SubmitStream* stream, const TrapBinding (&bindings)[4], bool computeQueue)
{
    const uint32 count = computeQueue ? 2 : 4;
    constexpr uint32 Low[] = { 1, 0, 3, 2 };
    for (uint32 i = 0; i < count; ++i)
    {
        if (!bindings[i].bound) { continue; }
        stream->Put64(stream->AddRecord4(RecordTrap, Low[i]) + 16, bindings[i].address);
    }
}

void AddTrapRecords(SubmitStream* stream, const GfxDevice& gfxDevice, bool computeQueue)
{
    // 4db8e0 reads GfxDevice+0x28 compute handler, +0x40 compute buffer, +0x58 graphics handler, +0x70 graphics
    // buffer (bound by 34fa80 / 34fb00).
    const auto binding = [](const BoundGpuMemory& bound)
    {
        TrapBinding result = {};
        result.bound = (bound.Memory() != nullptr);
        result.address = result.bound ? (bound.Memory()->Desc().gpuVirtAddr + bound.Offset()) : 0;
        return result;
    };
    const TrapBinding bindings[4] =
    {
        binding(gfxDevice.TrapBuffer(PipelineBindPoint::Compute)),
        binding(gfxDevice.TrapHandler(PipelineBindPoint::Compute)),
        binding(gfxDevice.TrapBuffer(PipelineBindPoint::Graphics)),
        binding(gfxDevice.TrapHandler(PipelineBindPoint::Graphics)),
    };
    AddTrapRecords(stream, bindings, computeQueue);
}

void AddExternPhysRecord(SubmitStream* stream, uint64 surfaceIndex, uint64 markerIndex)
{
    stream->Put32(stream->AddRecord4(RecordExternPhys) + 16, uint32(surfaceIndex));
    stream->Put32(stream->AddRecord4(RecordExternPhys) + 16, uint32(markerIndex));
}

void AddExternPhysRecords(SubmitStream* stream, uint32 count, const IGpuMemory* const* memories)
{
    // 4dc220: GpuMemory+0xe0 / +0xe8 (remote SDI surface and marker indices).
    for (uint32 i = 0; i < count; ++i)
    {
        const auto* memory = static_cast<const Pal::GpuMemory*>(memories[i]);
        AddExternPhysRecord(stream, memory->GetRemoteSdiSurfaceIndex(), memory->GetRemoteSdiMarkerIndex());
    }
}

void AddPerfExperimentRecords(SubmitStream* stream, uint32 perfFlags, bool spmEscapeFailed, bool gfx103)
{
    if (((perfFlags & 2) != 0) && spmEscapeFailed) { stream->AddRecord4(RecordPerfExperiment, 1); }
    if (!gfx103) { return; }
    if ((perfFlags & 1) != 0) { stream->AddRecord4(RecordPerfExperiment, 3); }
    if ((perfFlags & 4) != 0) { stream->AddRecord4(RecordPerfExperiment, 2); }
}

Result SendSpmContextEscape(const EscapeRoute& route, uint32 perfFlags, uint32 hContext)
{
    std::array<uint8, 0x108> record = {};
    InitPrivateRecord(record.data(), record.size(), 0x03010103);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0x84, ((~perfFlags) & 2) | ((perfFlags >> 1) & 1));
    PutRecord32(record.data(), 0xc4, 0x40);
    return SharedEscape(route, record.data(), record.size(), false, hContext);
}
}
