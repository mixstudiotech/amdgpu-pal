// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmFullscreen.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmDisplay.h"
#include "core/os/wddm/wddmFence.h"
#include "core/os/wddm/wddmSparse.h"
#include "core/os/wddm/wddmResult.h"
#include "core/os/wddm/wddmQueueIdle.h"
#include "core/os/wddm/wddmPrivate.h"
#include "core/cmdBuffer.h"
#include "core/cmdStream.h"
#include "core/cmdStreamAllocation.h"
#include "core/platform.h"
#include "core/privateScreen.h"
#include "core/hw/gfxip/gfxCmdBuffer.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace Pal::Wddm
{
namespace
{
// D3DKMTSubmitCommandToHwQueue result after which 37b9c0 queries the execution state (37bbc7).
constexpr Status GpuExceptionOnDevice = static_cast<Status>(0xc01e0200u);
// GPU record offsets (Device+X == gpuRecord[X - 0x93e0]).
constexpr size_t GpuCaps14188  = 14188; // Device+0xcb4c: DirectCapture record bits (4dc4b0).
constexpr size_t GpuVpBltMask  = 16176; // Device+0xd310: 4dbf00 record mask.

const CmdBuffer* AsCmdBuffer(const ICmdBuffer* buffer) { return static_cast<const CmdBuffer*>(buffer); }

uint32 PerfFlagsOf(const ICmdBuffer* buffer)
{
    // GfxCmdBuffer::PerfTracesEnabled (native cmd-buffer DWORD+0x758).
    return static_cast<const GfxCmdBuffer*>(buffer)->PerfTracesEnabled().u32All;
}

// Debug-only capture of the CPU-mapped IB submitted to KMT (PAL_WDDM_DUMP_DIR). Failures never alter submission.
void DumpIb(const CmdStreamChunk& chunk, uint32 queueHandle, uint64 serial, uint32 entry, uint32 flags)
{
    static const char* directory = std::getenv("PAL_WDDM_DUMP_DIR");
    static std::atomic<uint32> count = 0;
    if ((directory == nullptr) || (directory[0] == '\0')) { return; }
    uint32 index = count.load(std::memory_order_relaxed);
    while ((index < 32) && !count.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {}
    if (index >= 32) { return; }
    char path[4096] = {};
    const int pathBytes = std::snprintf(path, sizeof(path),
        "%s\\ib_%02u_queue_%08x_submit_%llu_entry_%02u_flags_%08x_va_%016llx.bin",
        directory, index, queueHandle, serial, entry, flags, chunk.GpuVirtAddr());
    const size_t bytes = size_t(chunk.CmdDwordsToExecute()) * sizeof(uint32);
    size_t written = 0;
    if ((pathBytes > 0) && (size_t(pathBytes) < sizeof(path)) && (chunk.CpuAddr() != nullptr))
    {
        if (FILE* file = std::fopen(path, "wb"))
        {
            written = std::fwrite(chunk.CpuAddr(), 1, bytes, file);
            std::fclose(file);
        }
    }
    std::fprintf(stderr, "[pal-wddm] IB_DUMP index=%u queue=%08x bytes=%zu written=%zu path=%s\n",
                 index, queueHandle, bytes, written, path);
    std::fflush(stderr);
}
}

SubmissionContext::SubmissionContext(Device* device)
    : Pal::SubmissionContext(device->GetPlatform()), m_device(device), m_queue{} {}
SubmissionContext::~SubmissionContext() { m_device->OsTransport().DestroyQueue(&m_queue); }
bool SubmissionContext::IsTimestampRetired(uint64 timestamp) const
{
    if (m_queue.progressCpu == nullptr) { return false; }
    const uint32 completedLow = *reinterpret_cast<const volatile uint32*>(m_queue.progressCpu);
    return NativeTimestampRetired(LastTimestamp(), completedLow, timestamp);
}

Queue::Queue(uint32 count, Device* device, const QueueCreateInfo* info)
    : Pal::Queue(count, OsQueueMode::Kernel, device, info) {}
Queue::Queue(uint32 count, Device* device, const QueueCreateInfo* info, std::atomic<bool>* gangNotified)
    : Pal::Queue(count, OsQueueMode::Kernel, device, info)
{
    m_gang.notified = gangNotified;
}
Queue::~Queue()
{
    DestroyQueuePresenters(static_cast<Device*>(m_pDevice), &m_windowedPresenter, &m_fullscreenPresenter);
    DestroyGangMembers();
    // Initialization failures may destroy the placement object before the ordinary Destroy path.
    if (m_pSubmissionContext != nullptr)
    {
        m_pSubmissionContext->ReleaseReference();
        m_pSubmissionContext = nullptr;
    }
}

// Options of the KMD context / HW-queue records of one sub-queue (37a980, 383b20, 38b140).
QueueCreateOptions Queue::CreateOptions(const QueueCreateInfo& info, uint32 node, bool leader) const
{
    auto* device = static_cast<Device*>(m_pDevice);
    const PhysicalProperties& physical = device->Adapter().physical;
    QueueCreateOptions options = {};
    options.priority = static_cast<uint32>(info.priority);
    options.tmzOnly = (info.tmzOnly != 0);
    options.tmzLevel = QueueTmzLevel(info.tmzOnly != 0);
    options.dispatchTunneling = (info.dispatchTunneling != 0);
    // Pal::Queue::Init 31bc70 clears numReservedCu below Realtime priority.
    options.numReservedCu = (info.priority == QueuePriority::Realtime) ? info.numReservedCu : 0;
    options.disableGpuTimeout =
        DisableGpuTimeoutForNode(physical, device->GetPublicSettings()->longRunningSubmissions, node);
    options.nativeProgressFence = leader && NativeFenceSelected(physical, ReadGangTopology(physical).hwsPresent);
    // 37a980 / 383dd4: IFH in the kernel (Queue+0x30 == 2) or create-info flag bit6.
    options.nullRendering = (m_ifhMode == IfhModeKmd) || (info.placeholder3 != 0);
    return options;
}

Result Queue::Init(const QueueCreateInfo* info, void* contextPlacement)
{
    if (m_queueCount > 1) { return InitGang(info, contextPlacement); }
    if ((info == nullptr) || (contextPlacement == nullptr)) { return Result::ErrorInvalidPointer; }
    if (info->priority >= QueuePriority::Count) { return Result::ErrorInvalidValue; }
    if ((m_queueCount != 1) ||
        ((info->queueType != QueueTypeUniversal) && (info->queueType != QueueTypeCompute) &&
         (info->queueType != QueueTypeDma) && (info->queueType != QueueTypeTimer)))
        { return Result::Unsupported; }
    const EngineType expectedEngine = (info->queueType == QueueTypeTimer) ? EngineTypeTimer :
        (info->queueType == QueueTypeDma) ? EngineTypeDma :
        (info->queueType == QueueTypeCompute) ? EngineTypeCompute : EngineTypeUniversal;
    if (info->engineType != expectedEngine) { return Result::ErrorInvalidValue; }
    auto* device = static_cast<Device*>(m_pDevice);
    const bool compute = info->queueType == QueueTypeCompute;
    const bool dma = info->queueType == QueueTypeDma;
    const bool timer = info->queueType == QueueTypeTimer;
    const bool hws = !dma && !timer;
    uint32 nodeOrdinal = UINT32_MAX;
    uint32 index = 0;
    for (const NodeInfo& node : device->Adapter().nodes)
    {
        if (timer ? node.isTimer : dma ? node.isDma : compute ? node.isCompute : node.isGraphics)
        {
            if (index++ == info->engineIndex) { nodeOrdinal = node.ordinal; break; }
        }
    }
    if (nodeOrdinal == UINT32_MAX) { return Result::ErrorInvalidValue; }
    // HWS Init 37b270 refuses IFH implemented in PAL before Pal::Queue::Init.
    if (hws && (m_ifhMode == IfhModePal)) { return Result::Unsupported; }
    // Pal::Queue::Init 31c721 (reached by the HWS, legacy and timer Init alike): a TMZ queue needs Device+0x13c0 bit1.
    if ((info->tmzOnly != 0) && !TmzQueuesSupported(device->Adapter().physical, device->Settings().tmzEnabled))
        { return Result::ErrorUnavailable; }
    // 37b270: physical-addressing engines and windowed-prior-blit queues are refused by the HWS queue.
    if (hws && ((info->windowedPriorBlit != 0) ||
                (device->EngineProperties().perEngine[info->engineType].flags.physicalAddressingMode != 0)))
        { return Result::Unsupported; }
    // 4d8010 would allocate a user-queue (context-save) area here; that allocation's internal create flags are not
    // reproduced, so such a profile is refused. The gated profile reports zero bytes.
    if (hws && (UserQueueBytes(device->Adapter().physical, info->queueType) != 0) &&
        IsHwsEngine(device->Adapter(), ReadGangTopology(device->Adapter().physical), info->engineType, info->engineIndex))
        { return Result::Unsupported; }
    // 4d7f30: Queue+0x1e0 marks a universal or compute sub-queue (trap records on every submission).
    m_hasGfxSubQueue = hws;
    // 37a180: an ordinary universal queue registers its implicit ACE through the KMD escape when Device+0x8248 and
    // Settings+0x3d9 allow it.
    m_aceEscapeMode = hws && GangUsesEscapeRegistration(ReadGangTopology(device->Adapter().physical), 1, info);
    auto* context = PAL_NEW(SubmissionContext, device->GetPlatform(), Util::AllocInternal)(device);
    if (context == nullptr) { return Result::ErrorOutOfMemory; }
    m_pSubmissionContext = context;
    Result result = ConvertStatus(device->OsTransport().CreateQueue(
        nodeOrdinal, timer ? QueueEngine::Timer : dma ? QueueEngine::Dma : compute ? QueueEngine::Compute : QueueEngine::Graphics,
        &context->NativeQueue(), CreateOptions(*info, nodeOrdinal, true)));
    if ((result == Result::Success) && !timer)
    {
        // HWS Init 37b270 and legacy Init 383b20 query the kernel context (4d9f70) and ignore the result;
        // the timer Init 38b140 does not, so timer queues report ErrorUnavailable.
        const EscapeRoute route = device->KmdEscapeRoute();
        RefreshKernelContextInfo(SharedControlEscape(&route), context->NativeQueue().context, &m_kernelContext);
    }
    if (result == Result::Success) { result = Pal::Queue::Init(info, contextPlacement); }
    // HWS Init 37b8e1 and legacy Init 383fb5: the presenters of this queue type (4d84b0); failure fails Init.
    if (result == Result::Success)
    {
        result = CreateQueuePresenters(device, this, info->queueType, info->engineType,
                                       &m_windowedPresenter, &m_fullscreenPresenter);
    }
    return result;
}
QueueState& Queue::NativeQueue()
{ return static_cast<SubmissionContext*>(m_pSubmissionContext)->NativeQueue(); }
const QueueState& Queue::NativeQueue() const
{ return static_cast<const SubmissionContext*>(m_pSubmissionContext)->NativeQueue(); }
Result Queue::SignalNativeSync(const SyncObject& object, uint64 value)
{ return ConvertStatus(static_cast<Device*>(m_pDevice)->OsTransport().SignalQueue(NativeQueue(), object, value)); }
Result Queue::WaitNativeSync(const SyncObject& object, uint64 value)
{ return ConvertStatus(static_cast<Device*>(m_pDevice)->OsTransport().WaitQueue(NativeQueue(), object, value)); }
void Queue::OsDestroy()
{
    if ((m_pSubmissionContext != nullptr) && (NativeQueue().context != 0)) { OsWaitIdle(); }
    DestroyQueuePresenters(static_cast<Device*>(m_pDevice), &m_windowedPresenter, &m_fullscreenPresenter);
    // Native 37a480 releases the follower queues and the implicit ACE here; the leader belongs to the submission context.
    DestroyGangMembers();
}

// =====================================================================================================================
// Native 4d8c00 -> 37c730 (HWS) / 4d8b20 (legacy).
Result Queue::OsSubmit(const MultiSubmitInfo& submit, const InternalSubmitInfo* internal)
{
    // Timer vtable slot 34 is 2ee740.
    if (Type() == QueueTypeTimer) { return Result::ErrorUnavailable; }
    if ((internal == nullptr) || (m_pSubmissionContext == nullptr) || (submit.perSubQueueInfoCount > m_queueCount))
        { return Result::ErrorInvalidValue; }
    if ((submit.perSubQueueInfoCount > 0) && (submit.pPerSubQueueInfo == nullptr)) { return Result::ErrorInvalidPointer; }
    // DOPP records (4dc350) carry GpuMemory+0x138 of a DOPP desktop allocation; this backend cannot open one.
    if (submit.doppRefCount != 0) { return Result::Unsupported; }
    auto* device = static_cast<Device*>(m_pDevice);
    if (m_gang.escapeRegistered || (submit.perSubQueueInfoCount > 1) || (internal->implicitGangedSubQueues != 0))
        { NotifyGangSubmission(device->OsTransport(), device->GetPlatform(), m_gang.notified); }
    if (Type() == QueueTypeDma) { return OsSubmitDma(submit, *internal); }
    // 37c730: the ganged stream is used for multi-sub-queue submissions, always once followers were registered through
    // the escape, and for every submission after the first ganged or implicitly ganged one.
    const bool gang = (m_gang.escapeRegistered && (m_queueCount > 1)) || (submit.perSubQueueInfoCount > 1);
    if (!gang && (internal->implicitGangedSubQueues == 0))
    {
        return m_gang.active ? OsSubmitGang(submit, internal) : OsSubmitSingle(submit, *internal);
    }
    if (!m_gang.active)
    {
        // The first ganged submission waits for the queue to drain; the switch is kept even if the wait fails.
        const Result idle = OsWaitIdle();
        m_gang.active = true;
        if (idle != Result::Success) { return idle; }
    }
    return gang ? OsSubmitGang(submit, internal) : OsSubmitImplicitGang(submit, *internal);
}

// 4dafd0: preambles, then for each stream index the batch's streams chained into one IB, then postambles.
Result Queue::AddChainedIbs(SubmitStream* stream, const InternalSubmitInfo& internal, uint32 count,
                            ICmdBuffer*const* cmdBuffers, uint32* consumed)
{
    auto* device = static_cast<Device*>(m_pDevice);
    const PhysicalProperties& physical = device->Adapter().physical;
    // 4477b0 returns at least one buffer, also for a zero chain limit.
    const uint32 limit = Util::Max(1u, Util::Min(count, device->GetPublicSettings()->cmdBufBatchedSubmitChainLimit));
    const uint32 batch = GangChainableCount(limit, cmdBuffers);
    const auto add = [&](const CmdStream& cmdStream)
    {
        // 4d9940: an async-compute stream gets 0x400 when GPU record 16180 bit2 is set, and sends 309f40.
        uint32 flags = IbRecordFlags(cmdStream);
        if (IbRecordAceFlag(physical, cmdStream))
        {
            flags |= 0x400;
            NotifyGangSubmission(device->OsTransport(), device->GetPlatform(), m_gang.notified);
        }
        const CmdStreamChunk* chunk = cmdStream.GetFirstChunk();
        DumpIb(*chunk, NativeQueue().handle, NativeQueue().submitted + 1, stream->Entries(), flags);
        stream->AddIb(chunk->GpuVirtAddr(), chunk->CmdDwordsToExecute() * uint32(sizeof(uint32)), flags, 1, 0);
    };
    // Preamble and postamble streams are added without the emptiness test of the command streams; an empty one is
    // skipped here because the original dereferences its missing first chunk.
    for (uint32 i = 0; i < internal.numPreambleCmdStreams; ++i)
    {
        const CmdStream* preamble = internal.pPreambleCmdStream[i];
        if ((preamble != nullptr) && (preamble->GetNumChunks() != 0)) { add(*preamble); }
    }
    const uint32 streams = AsCmdBuffer(cmdBuffers[0])->NumCmdStreams();
    for (uint32 s = 0; s < streams; ++s)
    {
        const CmdBuffer* last = nullptr;
        const CmdStream* previous = nullptr;
        for (uint32 b = 0; b < batch; ++b)
        {
            const CmdStream* current = AsCmdBuffer(cmdBuffers[b])->GetCmdStream(s);
            if ((current == nullptr) || current->IsEmpty()) { continue; }
            if (previous != nullptr) { previous->PatchTailChain(current); }
            else { add(*current); }
            last = AsCmdBuffer(cmdBuffers[b]);
            previous = current;
        }
        if ((last != nullptr) && last->IsExclusiveSubmit() && (previous != nullptr)) { previous->PatchTailChain(nullptr); }
    }
    for (uint32 i = 0; i < internal.numPostambleCmdStreams; ++i)
    {
        const CmdStream* postamble = internal.pPostambleCmdStream[i];
        if ((postamble != nullptr) && (postamble->GetNumChunks() != 0)) { add(*postamble); }
    }
    *consumed = batch;
    return (batch == 0) ? Result::ErrorInvalidValue : Result::Success;
}

// 4da8a0 (universal/compute, the non-batching branch): chained batches, each followed by its records and submitted.
Result Queue::OsSubmitSingle(const MultiSubmitInfo& submit, const InternalSubmitInfo& internal)
{
    auto* device = static_cast<Device*>(m_pDevice);
    const PhysicalProperties& physical = device->Adapter().physical;
    SubmitStream stream;
    const bool hasBuffers = (submit.perSubQueueInfoCount != 0) && (submit.pPerSubQueueInfo[0].cmdBufferCount != 0);
    if (!hasBuffers)
    {
        // 37c730: no command buffers, so no IB records; only the finishing records are submitted.
        m_pagingFence = internal.pagingFence;
        return FinishSubmit(&stream, submit, true);
    }
    const PerSubQueueSubmitInfo& perQueue = submit.pPerSubQueueInfo[0];
    if (perQueue.ppCmdBuffers == nullptr) { return Result::ErrorInvalidPointer; }
    for (uint32 i = 0; i < perQueue.cmdBufferCount; ++i)
    {
        if (perQueue.ppCmdBuffers[i] == nullptr) { return Result::ErrorInvalidPointer; }
        // 4dc4b0 reads a DirectCapture memory object (+0x130 source id, VA, size) this backend cannot create.
        if ((perQueue.pCmdBufInfoList != nullptr) && perQueue.pCmdBufInfoList[i].isValid &&
            (perQueue.pCmdBufInfoList[i].pDirectCapMemory != nullptr)) { return Result::Unsupported; }
    }
    // Perf experiments: the per-context SPM escape when the OR of the command buffers' flags changes (4daee0), then
    // the 4db6d0 records ahead of the first batch's IBs.
    uint32 perfFlags = 0;
    for (uint32 i = 0; i < perQueue.cmdBufferCount; ++i) { perfFlags |= PerfFlagsOf(perQueue.ppCmdBuffers[i]); }
    if (m_perfFlags != perfFlags)
    {
        if (!m_spmEscapeFailed &&
            (SendSpmContextEscape(device->KmdEscapeRoute(), perfFlags, NativeQueue().context) != Result::Success))
            { m_spmEscapeFailed = true; }
        m_perfFlags = perfFlags;
    }
    const auto& gfxIp = device->ChipProperties().gfxTriple;
    if (perfFlags != 0)
    {
        AddPerfExperimentRecords(&stream, perfFlags, m_spmEscapeFailed, (gfxIp.major == 10) && (gfxIp.minor == 3));
    }
    m_pagingFence = internal.pagingFence;
    Result result = GfxIpWaitPipelineUploading(submit); // 31c7c0
    ICmdBuffer*const* buffers = perQueue.ppCmdBuffers;
    const CmdBufInfo* infos = perQueue.pCmdBufInfoList;
    uint32 remaining = perQueue.cmdBufferCount;
    const uint32 vpBltMask = (GetEngineType() == EngineTypeUniversal) ?
        (0x10u & Private::Get32(physical.gpuRecord.data(), GpuVpBltMask)) : 0u;
    const uint32 vpBltValues[] = { 1 };
    while ((result == Result::Success) && (remaining != 0))
    {
        uint32 consumed = 0;
        result = AddChainedIbs(&stream, internal, remaining, buffers, &consumed);
        if (result != Result::Success) { break; }
        for (uint32 i = 0; i < consumed; ++i)
        {
            m_pagingFence = Util::Max(m_pagingFence, AsCmdBuffer(buffers[i])->LastPagingFence());
            if (infos == nullptr) { continue; }
            if (infos[i].isValid)
            {
                AddCmdBufInfoRecords(&stream, infos[i], device->KmdShared().flipOwner,
                                     Private::Get32(physical.gpuRecord.data(), GpuCaps14188), KmtAllocationHandle);
            }
            // The vpBltExecuted record does not depend on isValid in the original.
            if (infos[i].vpBltExecuted) { AddVpBltRecords(&stream, vpBltMask, vpBltValues); }
        }
        if (AsCmdBuffer(buffers[0])->IsTmzEnabled()) { stream.AddRecord4(RecordTmz); }
        AddExternPhysRecords(&stream, submit.externPhysMemCount, submit.ppExternPhysMem);
        buffers += consumed;
        remaining -= consumed;
        if (infos != nullptr) { infos += consumed; }
        result = FinishSubmit(&stream, submit, remaining == 0);
        stream = SubmitStream();
    }
    return result;
}

// 4da260 (DMA and the other legacy engines): one KMD submission per command chunk.
Result Queue::OsSubmitDma(const MultiSubmitInfo& submit, const InternalSubmitInfo& internal)
{
    auto* device = static_cast<Device*>(m_pDevice);
    const PhysicalProperties& physical = device->Adapter().physical;
    SubmitStream stream;
    const bool hasBuffers = (submit.perSubQueueInfoCount != 0) && (submit.pPerSubQueueInfo[0].cmdBufferCount != 0);
    if (!hasBuffers)
    {
        m_pagingFence = internal.pagingFence;
        return FinishSubmit(&stream, submit, true);
    }
    const PerSubQueueSubmitInfo& perQueue = submit.pPerSubQueueInfo[0];
    if (perQueue.ppCmdBuffers == nullptr) { return Result::ErrorInvalidPointer; }
    for (uint32 i = 0; i < perQueue.cmdBufferCount; ++i)
    {
        if (perQueue.ppCmdBuffers[i] == nullptr) { return Result::ErrorInvalidPointer; }
        if ((perQueue.pCmdBufInfoList != nullptr) && perQueue.pCmdBufInfoList[i].isValid &&
            (perQueue.pCmdBufInfoList[i].pDirectCapMemory != nullptr)) { return Result::Unsupported; }
    }
    Result result = Result::Success;
    bool finished = false;
    for (uint32 b = 0; (result == Result::Success) && (b < perQueue.cmdBufferCount); ++b)
    {
        const CmdBuffer* buffer = AsCmdBuffer(perQueue.ppCmdBuffers[b]);
        // 4da260 takes the buffer's paging fence against the submission's, not a running maximum.
        m_pagingFence = Util::Max(buffer->LastPagingFence(), internal.pagingFence);
        const CmdStream* cmdStream = buffer->GetCmdStream(0);
        if ((perQueue.pCmdBufInfoList != nullptr) && perQueue.pCmdBufInfoList[b].isValid)
        {
            AddCmdBufInfoRecords(&stream, perQueue.pCmdBufInfoList[b], device->KmdShared().flipOwner,
                                 Private::Get32(physical.gpuRecord.data(), GpuCaps14188), KmtAllocationHandle);
        }
        AddExternPhysRecords(&stream, submit.externPhysMemCount, submit.ppExternPhysMem);
        // A buffer without chunks never advances the original's loop; it is skipped here.
        if ((cmdStream == nullptr) || (cmdStream->GetNumChunks() == 0)) { continue; }
        for (auto cursor = cmdStream->GetFwdIterator(); (result == Result::Success) && cursor.IsValid(); )
        {
            const CmdStreamChunk* chunk = cursor.Get();
            cursor.Next();
            DumpIb(*chunk, NativeQueue().handle, NativeQueue().submitted + 1, stream.Entries(), 0);
            stream.AddIb(chunk->GpuVirtAddr(), chunk->CmdDwordsToExecute() * uint32(sizeof(uint32)), 0, 0, 0);
            const bool last = (b + 1 == perQueue.cmdBufferCount) && !cursor.IsValid();
            result = FinishSubmit(&stream, submit, last);
            finished = finished || last;
            stream = SubmitStream();
        }
    }
    // Trailing buffers without chunks: the fences still need their final submission.
    if ((result == Result::Success) && !finished) { result = FinishSubmit(&stream, submit, true); }
    return result;
}

// 37ca10: the paging fence maximum of a sub-queue and its CmdBufInfo records.
void Queue::AccumulatePagingFence(uint32 subQueue, uint32 count, ICmdBuffer*const* cmdBuffers,
                                  const CmdBufInfo* infos, const InternalSubmitInfo& internalInfo, SubmitStream* stream)
{
    auto* device = static_cast<Device*>(m_pDevice);
    m_pagingFence = Util::Max(m_pagingFence, internalInfo.pagingFence);
    for (uint32 b = 0; b < count; ++b)
    {
        m_pagingFence = Util::Max(m_pagingFence, AsCmdBuffer(cmdBuffers[b])->LastPagingFence());
        if ((infos != nullptr) && infos[b].isValid)
        {
            AddCmdBufInfoRecords(stream, infos[b], device->KmdShared().flipOwner,
                                 Private::Get32(device->Adapter().physical.gpuRecord.data(), GpuCaps14188),
                                 KmtAllocationHandle);
        }
    }
}

// 37cae0 + 37dcc0: the sub-queue's type-6 record and, when the submission carries implicit ganged work, the ACE
// record. allEngines is the gang path's argument (no engine filtering of the main record).
Result Queue::AddGangSubQueue(SubmitStream* stream, bool allEngines, uint32 index, uint32 count,
                              ICmdBuffer*const* cmdBuffers, const InternalSubmitInfo& internalInfo,
                              bool* tmzSeen, uint32* perfFlags)
{
    for (uint32 b = 0; b < count; ++b) { *perfFlags |= PerfFlagsOf(cmdBuffers[b]); }
    if (!*tmzSeen && AsCmdBuffer(cmdBuffers[0])->IsTmzEnabled()) { *tmzSeen = true; }
    const InternalSubmitInfo& sub = (&internalInfo)[index];
    const uint32 mainQueueId = (index == 0) ? NativeQueue().queueId : m_gang.followers[index - 1].queueId;
    const size_t header = m_gang.escapeRegistered ? stream->BeginQueue(2, (index == 0) ? 1u : 2u)
                                                  : stream->BeginQueue(1, mainQueueId);
    if (GangChainableCount(count, cmdBuffers) < count) { return Result::ErrorUnavailable; }
    const EngineType mainEngine = m_pQueueInfos[index].createInfo.engineType;
    const auto addRecord = [&](int32 subQueueIndex, bool engineFilter, EngineType engine)
    {
        for (uint32 p = 0; p < sub.numPreambleCmdStreams; ++p)
        {
            const CmdStream* preamble = sub.pPreambleCmdStream[p];
            if ((preamble != nullptr) && (preamble->GetNumChunks() != 0) &&
                (!engineFilter || (preamble->GetEngineType() == engine))) { stream->AddGangStream(*preamble); }
        }
        const CmdBuffer* last = nullptr;
        const CmdStream* previous = nullptr;
        for (uint32 b = 0; b < count; ++b)
        {
            const CmdBuffer* buffer = AsCmdBuffer(cmdBuffers[b]);
            const CmdStream* current = buffer->GetCmdStreamInSubQueue(subQueueIndex);
            if ((current == nullptr) || current->IsEmpty() || (engineFilter && (current->GetEngineType() != engine)))
                { continue; }
            if (previous != nullptr) { previous->PatchTailChain(current); }
            else { stream->AddGangStream(*current); }
            last = buffer;
            previous = current;
        }
        if ((last != nullptr) && last->IsExclusiveSubmit() && (previous != nullptr)) { previous->PatchTailChain(nullptr); }
        for (uint32 p = 0; p < sub.numPostambleCmdStreams; ++p)
        {
            const CmdStream* postamble = sub.pPostambleCmdStream[p];
            if ((postamble != nullptr) && (postamble->GetNumChunks() != 0) &&
                (!engineFilter || (postamble->GetEngineType() == engine))) { stream->AddGangStream(*postamble); }
        }
    };
    addRecord(CmdBuffer::MainSubQueueIdx, !allEngines, mainEngine);
    stream->EndQueue(header);
    // 37cae0 tests the first internal info (Pal::Queue::SubmitConfig fills only that one).
    if (internalInfo.implicitGangedSubQueues == 0) { return Result::Success; }
    const size_t ace = m_gang.escapeRegistered ? stream->BeginQueue(2, 2) : stream->BeginQueue(1, m_ace.queueId);
    addRecord(0, true, EngineTypeCompute);
    stream->EndQueue(ace);
    return Result::Success;
}

// 37cfb0: the ACE of an implicit gang, created on the first submission that needs it.
Result Queue::CreateImplicitAce()
{
    if (m_ace.created) { return Result::Success; }
    auto* device = static_cast<Device*>(m_pDevice);
    auto& transport = device->OsTransport();
    QueueCreateInfo aceInfo = m_pQueueInfos[0].createInfo;
    aceInfo.queueType = QueueTypeCompute;
    aceInfo.engineType = EngineTypeCompute;
    Result result;
    if (!m_aceEscapeMode)
    {
        // Highest compute instance whose capabilities have supportsMultiQueue (Device+0x15b4 + 16i, bit2).
        int32 aceIndex = -1;
        const auto& capabilities = device->EngineProperties().perEngine[EngineTypeCompute].capabilities;
        for (int32 i = int32(MaxAvailableEngines) - 1; i >= 0; --i)
        {
            if (capabilities[i].flags.supportsMultiQueue != 0) { aceIndex = i; break; }
        }
        if (aceIndex < 0) { return Result::ErrorUnavailable; }
        aceInfo.engineIndex = uint32(aceIndex);
        const uint32 node = GangEngineNode(device->Adapter(), EngineTypeCompute, aceInfo.engineIndex);
        if (node == UINT32_MAX) { return Result::ErrorInvalidValue; }
        result = ConvertStatus(CreateGangFollower(transport, node, QueueEngine::Compute,
                                                  CreateOptions(aceInfo, node, false), NativeQueue().queueId,
                                                  &m_ace.follower), "CreateImplicitAce");
        if (result == Result::Success)
        {
            m_ace.queueId = m_ace.follower.queueId;
            m_ace.created = true;
        }
    }
    else
    {
        // 37b070 + 37b170: the follower exists only inside the KMD, addressed through the leader's queue id.
        const uint32 flags = GangFollowerQueueFlags(CreateOptions(aceInfo, UINT32_MAX, false));
        const uint32 leaderId = NativeQueue().queueId;
        result = RegisterGangFollowers(transport, device->GetPlatform(), leaderId, &flags, 1);
        if (result == Result::Success)
        {
            m_gang.escapeRegistered = true;
            m_ace.queueId = leaderId;
            m_ace.created = true;
        }
    }
    return result;
}

// 37c730 tail for an ordinary queue whose submission carries implicit ganged (ACE) work.
Result Queue::OsSubmitImplicitGang(const MultiSubmitInfo& submit, const InternalSubmitInfo& internal)
{
    auto* device = static_cast<Device*>(m_pDevice);
    m_pagingFence = 0;
    // The original reads pPerSubQueueInfo[0] unconditionally; an empty submission counts as zero buffers here.
    const uint32 count = (submit.perSubQueueInfoCount != 0) ? submit.pPerSubQueueInfo[0].cmdBufferCount : 0;
    Result result = CreateImplicitAce();
    if (result != Result::Success) { return result; }
    SubmitStream stream;
    if (count == 0)
    {
        m_pagingFence = internal.pagingFence;
        return FinishSubmit(&stream, submit, true);
    }
    const PerSubQueueSubmitInfo& perQueue = submit.pPerSubQueueInfo[0];
    if (perQueue.ppCmdBuffers == nullptr) { return Result::ErrorInvalidPointer; }
    for (uint32 i = 0; i < count; ++i)
    {
        if (perQueue.ppCmdBuffers[i] == nullptr) { return Result::ErrorInvalidPointer; }
        if ((perQueue.pCmdBufInfoList != nullptr) && perQueue.pCmdBufInfoList[i].isValid &&
            (perQueue.pCmdBufInfoList[i].pDirectCapMemory != nullptr)) { return Result::Unsupported; }
    }
    AccumulatePagingFence(0, count, perQueue.ppCmdBuffers, perQueue.pCmdBufInfoList, internal, &stream);
    bool tmz = false;
    uint32 perfFlags = 0;
    result = AddGangSubQueue(&stream, false, 0, count, perQueue.ppCmdBuffers, internal, &tmz, &perfFlags);
    if (result == Result::Success) { result = GfxIpWaitPipelineUploading(submit); }
    if (result != Result::Success) { return result; }
    const auto& gfxIp = device->ChipProperties().gfxTriple;
    if (perfFlags != 0)
    {
        AddPerfExperimentRecords(&stream, perfFlags, m_spmEscapeFailed, (gfxIp.major == 10) && (gfxIp.minor == 3));
    }
    if (tmz) { stream.AddRecord4(RecordTmz); }
    AddExternPhysRecords(&stream, submit.externPhysMemCount, submit.ppExternPhysMem);
    return FinishSubmit(&stream, submit, true);
}

// 37bd90 + 37b9c0 (HWS) / 384df0 + 384670 + 384a30 (legacy): finishing records, header, paging-fence wait, the KMD
// submission and, after the last batch, the fence registrations.
Result Queue::FinishSubmit(SubmitStream* stream, const MultiSubmitInfo& submit, bool lastBatch)
{
    auto* device = static_cast<Device*>(m_pDevice);
    auto& transport = device->OsTransport();
    // MultiSubmitInfo::ppBlockIfFlipping become WrittenPrimaries (core validation caps them at 16).
    uint32 primaries[MaxBlockIfFlippingCount] = {};
    const uint32 primaryCount = Util::Min(submit.blockIfFlippingCount, MaxBlockIfFlippingCount);
    for (uint32 i = 0; i < primaryCount; ++i)
    {
        if (submit.ppBlockIfFlipping[i] == nullptr) { return Result::ErrorInvalidPointer; }
        primaries[i] = KmtAllocationHandle(*submit.ppBlockIfFlipping[i]);
    }
    if (m_hasGfxSubQueue) { AddTrapRecords(stream, *device->GetGfxDevice(), Type() == QueueTypeCompute); }
    stream->Finalize(Private::AbiVersion);
    Result result = Result::Success;
    if (m_pagingFence != 0)
    {
        result = ConvertStatus(transport.WaitPagingFenceOnQueue(NativeQueue(), m_pagingFence), "WaitPagingFence");
    }
    if (result == Result::Success)
    {
        uint64 serial = 0;
        const Status status = transport.SubmitPrivate(&NativeQueue(), stream->FirstIbAddress(), stream->FirstIbBytes(),
                                                      stream->Data().data(), stream->Data().size(), primaries,
                                                      primaryCount, &serial);
        *m_pSubmissionContext->LastTimestampPtr() = NativeQueue().submitted;
        result = (status == GpuExceptionOnDevice) ? CheckQueueExecutionRequest(transport.DeviceHandle())
                                                  : ConvertStatus(status, "Submit");
    }
    for (uint32 i = 0; lastBatch && (result == Result::Success) && (i < submit.fenceCount); ++i)
        { result = static_cast<Fence*>(submit.ppFences[i])->Signal(*this); }
    return result;
}

Result Queue::OsWaitIdle()
{
    // A queue whose Init failed before Pal::Queue::Init (core CreateMultiQueue destroys it then) has no queue
    // infos and never submitted anything.
    if ((m_pQueueInfos == nullptr) || (m_pSubmissionContext == nullptr)) { return Result::Success; }
    // Original Timer vtable+0x118 is the immediate-success leaf 123270.
    if (Type() == QueueTypeTimer) { return Result::Success; }
    QueueIdleCallbacks callbacks = {};
    callbacks.userData = this;
    callbacks.lastTimestamp = m_pSubmissionContext == nullptr ? nullptr : m_pSubmissionContext->LastTimestampPtr();
    callbacks.isRetired = [](void* userData, uint64_t timestamp)
    {
        return static_cast<Queue*>(userData)->m_pSubmissionContext->IsTimestampRetired(timestamp);
    };
    callbacks.checkExecution = [](void* userData)
    {
        const auto* queue = static_cast<Queue*>(userData);
        return CheckQueueExecutionRequest(static_cast<Device*>(queue->m_pDevice)->OsTransport().DeviceHandle());
    };
    Result result = WaitQueueIdle(callbacks, m_pDevice->Settings().gfxTimeout);
    // Not in the original (4d8d80 waits for the queue's own submissions only): a KMD present that opengl32 or 2d1fa0
    // queued on this HW queue is not one of them, and swap-chain teardown frees the presented images after this
    // wait. Wait for the windowed presenter's last present as well, with the same timeout and health checks.
    if ((result == Result::Success) && (m_windowedPresenter != nullptr))
    {
        result = m_windowedPresenter->WaitForPresents(m_pDevice->Settings().gfxTimeout);
    }
    return result;
}
Result Queue::DoAssociateFenceWithLastSubmit(Pal::Fence* fence)
{
    if (fence == nullptr) { return Result::ErrorInvalidPointer; }
    // The shared wrapper has already called AssociateWithContext, exactly as
    // original31f0a0 does before Timer's vtable+0x140 unavailable leaf2ee740.
    return Type() == QueueTypeTimer ? Result::ErrorUnavailable : static_cast<Fence*>(fence)->Signal(*this);
}
// Timer Delay 38b5b0. A private screen selects the vsync-relative delay on the screen's index; a screen that is not
// enabled fails with ErrorPrivateScreenNotEnabled before anything is submitted (38b609).
Result Queue::OsDelay(Util::fmicroseconds delay, const IPrivateScreen* screen)
{
    uint32 source = 0;
    bool afterVsync = false;
    if (screen != nullptr)
    {
        const auto* privateScreen = static_cast<const PrivateScreen*>(screen);
        if (!privateScreen->Enabled()) { return Result::ErrorPrivateScreenNotEnabled; }
        source = privateScreen->Index();
        afterVsync = true;
    }
    return ConvertStatus(static_cast<Device*>(m_pDevice)->OsTransport().Delay(&NativeQueue(), delay.count(), source,
                                                                               afterVsync));
}
Result Queue::OsPresentDirect(const PresentDirectInfo& info)
{
    // Native HWS-queue slot 37 (4d8f60); the gate (31e8b0) is Pal::Queue::PresentDirectInternal and
    // IsPresentModeSupported keeps the base body (slot 33, 3200d0).
    return PresentDirectWithPresenters(*static_cast<Device*>(m_pDevice), info, m_windowedPresenter,
                                       m_fullscreenPresenter);
}
Result Queue::OsRemapVirtualMemoryPages(uint32 count, const VirtualMemoryRemapRange* ranges,
                                      bool doNotWait, IFence* fence)
{
    if (Type() == QueueTypeTimer) { return Result::ErrorUnavailable; }
    const Result result = Sparse::Remap(*this, count, ranges, doNotWait);
    return ((result == Result::Success) && (fence != nullptr)) ? SubmitFence(fence) : result;
}
Result Queue::OsCopyVirtualMemoryPageMappings(uint32 count, const VirtualMemoryCopyPageMappingsRange* ranges,
                                            bool doNotWait)
{
    if (Type() == QueueTypeTimer) { return Result::ErrorUnavailable; }
    return Sparse::Copy(*this, count, ranges, doNotWait);
}

// Native HWS queue Init 37b270 for queueCount > 1: sub-queue 0 is created exactly like a single queue; the others
// become followers of its HW queue, either as separate HW queues (37a980, leader=false) or, for the escape mode,
// as KMD-side followers registered by escape 0x03010603 (37b070/37b170).
Result Queue::InitGang(const QueueCreateInfo* info, void* contextPlacement)
{
    if ((info == nullptr) || (contextPlacement == nullptr)) { return Result::ErrorInvalidPointer; }
    auto* device = static_cast<Device*>(m_pDevice);
    const AdapterInfo& adapter = device->Adapter();
    const GangTopology topology = ReadGangTopology(adapter.physical);
    // 37b270 returns Unsupported unless HwsInfo.osHwsEnableFlags (Device+0x81f0) is nonzero, or with IFH in PAL.
    if (!topology.hwsPresent || (m_ifhMode == IfhModePal)) { return Result::Unsupported; }
    std::vector<uint32> nodes(m_queueCount, UINT32_MAX);
    for (uint32 i = 0; i < m_queueCount; ++i)
    {
        const QueueCreateInfo& sub = info[i];
        // Per-entry rules of the single-queue path. HWS SDMA queues do not exist on the gated profile (SDMA is not
        // in HwsInfo.engineOrdinalMask), so MultiQueueObjectSize already refuses them.
        if (sub.priority >= QueuePriority::Count) { return Result::ErrorInvalidValue; }
        if ((sub.queueType != QueueTypeUniversal) && (sub.queueType != QueueTypeCompute)) { return Result::Unsupported; }
        const EngineType expectedEngine = (sub.queueType == QueueTypeCompute) ? EngineTypeCompute : EngineTypeUniversal;
        if (sub.engineType != expectedEngine) { return Result::ErrorInvalidValue; }
        nodes[i] = GangEngineNode(adapter, sub.engineType, sub.engineIndex);
        if (nodes[i] == UINT32_MAX) { return Result::ErrorInvalidValue; }
        // 4d8010 user-queue area: not reproduced (zero bytes on the gated profile), see Init.
        if ((UserQueueBytes(adapter.physical, sub.queueType) != 0) &&
            IsHwsEngine(adapter, topology, sub.engineType, sub.engineIndex)) { return Result::Unsupported; }
    }
    // Pal::Queue::Init 31c721 checks the TMZ capability of the first create info.
    if ((info[0].tmzOnly != 0) && !TmzQueuesSupported(adapter.physical, device->Settings().tmzEnabled))
        { return Result::ErrorUnavailable; }
    for (uint32 i = 0; i < m_queueCount; ++i)
    {
        // 37b270: physical-addressing engines and windowed-prior-blit queues are refused with Unsupported.
        if ((info[i].windowedPriorBlit != 0) ||
            (device->EngineProperties().perEngine[info[i].engineType].flags.physicalAddressingMode != 0))
            { return Result::Unsupported; }
    }
    m_hasGfxSubQueue = true;
    auto* context = PAL_NEW(SubmissionContext, device->GetPlatform(), Util::AllocInternal)(device);
    if (context == nullptr) { return Result::ErrorOutOfMemory; }
    m_pSubmissionContext = context;
    auto& transport = device->OsTransport();
    QueueState& leader = context->NativeQueue();
    const auto engineOf = [](const QueueCreateInfo& sub)
        { return (sub.queueType == QueueTypeCompute) ? QueueEngine::Compute : QueueEngine::Graphics; };
    Result result = ConvertStatus(transport.CreateQueue(nodes[0], engineOf(info[0]), &leader,
                                                        CreateOptions(info[0], nodes[0], true)));
    const uint32 followerCount = m_queueCount - 1;
    if ((result == Result::Success) && GangUsesEscapeRegistration(topology, m_queueCount, info))
    {
        std::vector<uint32> flags(followerCount);
        for (uint32 i = 1; i < m_queueCount; ++i)
            { flags[i - 1] = GangFollowerQueueFlags(CreateOptions(info[i], nodes[i], false)); }
        result = RegisterGangFollowers(transport, device->GetPlatform(), leader.queueId, flags.data(), followerCount);
        m_gang.escapeRegistered = (result == Result::Success);
    }
    else if (result == Result::Success)
    {
        m_gang.followers = PAL_NEW_ARRAY(QueueState, followerCount, device->GetPlatform(), Util::AllocInternal);
        if (m_gang.followers == nullptr) { result = Result::ErrorOutOfMemory; }
        else { m_gang.followerCount = followerCount; }
        for (uint32 i = 1; (result == Result::Success) && (i < m_queueCount); ++i)
        {
            result = ConvertStatus(CreateGangFollower(transport, nodes[i], engineOf(info[i]),
                                                      CreateOptions(info[i], nodes[i], false), leader.queueId,
                                                      &m_gang.followers[i - 1]), "CreateGangFollower");
        }
    }
    if (result == Result::Success) { result = Pal::Queue::Init(info, contextPlacement); }
    if (result == Result::Success)
    {
        result = CreateQueuePresenters(device, this, info[0].queueType, info[0].engineType,
                                       &m_windowedPresenter, &m_fullscreenPresenter);
    }
    return result;
}

// Native 37a480: without escape registration, DestroyHwQueue then DestroyContext for every follower and the implicit
// ACE; KMD-registered followers have nothing to destroy.
void Queue::DestroyGangMembers()
{
    auto& transport = static_cast<Device*>(m_pDevice)->OsTransport();
    if (m_gang.followers != nullptr)
    {
        for (uint32 i = 0; i < m_gang.followerCount; ++i)
        {
            if ((m_gang.followers[i].context != 0) || (m_gang.followers[i].handle != 0))
                { transport.DestroyQueue(&m_gang.followers[i]); }
        }
        PAL_SAFE_DELETE_ARRAY(m_gang.followers, m_pDevice->GetPlatform());
        m_gang.followerCount = 0;
    }
    if ((m_ace.follower.context != 0) || (m_ace.follower.handle != 0)) { transport.DestroyQueue(&m_ace.follower); }
    m_ace.created = false;
}

// Native 4d8c00 -> 37c730 -> 37d120: one KMD submission on the leader whose private stream carries one type-6 record
// per sub-queue that has command buffers (plus the implicit ACE record), followed by the submission's type-4 records.
Result Queue::OsSubmitGang(const MultiSubmitInfo& submit, const InternalSubmitInfo* internal)
{
    auto* device = static_cast<Device*>(m_pDevice);
    SubmitStream stream;
    m_pagingFence = 0;
    bool anyWork = false;
    bool gfxSeen = false;
    bool computeOrAceSeen = false;
    bool tmz = false;
    uint32 perfFlags = 0;
    Result result = Result::Success;
    for (uint32 i = 0; (result == Result::Success) && (i < submit.perSubQueueInfoCount); ++i)
    {
        const PerSubQueueSubmitInfo& perQueue = submit.pPerSubQueueInfo[i];
        if (perQueue.cmdBufferCount == 0) { continue; }
        if (perQueue.ppCmdBuffers == nullptr) { return Result::ErrorInvalidPointer; }
        for (uint32 b = 0; b < perQueue.cmdBufferCount; ++b)
        {
            if (perQueue.ppCmdBuffers[b] == nullptr) { return Result::ErrorInvalidPointer; }
            if ((perQueue.pCmdBufInfoList != nullptr) && perQueue.pCmdBufInfoList[b].isValid &&
                (perQueue.pCmdBufInfoList[b].pDirectCapMemory != nullptr)) { return Result::Unsupported; }
        }
        anyWork = true;
        AccumulatePagingFence(i, perQueue.cmdBufferCount, perQueue.ppCmdBuffers, perQueue.pCmdBufInfoList,
                              internal[i], &stream);
        const QueueType type = m_pQueueInfos[i].createInfo.queueType;
        if ((type != QueueTypeUniversal) && (type != QueueTypeCompute))
        {
            // 37d120 builds an SDMA record from the first buffer's stream for HWS SDMA sub-queues; InitGang never
            // creates one on the gated profile.
            return Result::Unsupported;
        }
        gfxSeen = true;
        computeOrAceSeen = computeOrAceSeen || (type == QueueTypeCompute);
        if (internal[0].implicitGangedSubQueues != 0)
        {
            // An implicit ACE cannot be ganged next to a compute sub-queue.
            if (computeOrAceSeen) { return Result::Unsupported; }
            result = CreateImplicitAce();
            computeOrAceSeen = true;
            if (result != Result::Success) { break; }
        }
        result = AddGangSubQueue(&stream, true, i, perQueue.cmdBufferCount, perQueue.ppCmdBuffers, *internal,
                                 &tmz, &perfFlags);
    }
    if (result != Result::Success) { return result; }
    if (!anyWork)
    {
        m_pagingFence = internal[0].pagingFence;
        return FinishSubmit(&stream, submit, true);
    }
    if (gfxSeen) { result = GfxIpWaitPipelineUploading(submit); }
    if (result != Result::Success) { return result; }
    const auto& gfxIp = device->ChipProperties().gfxTriple;
    if (perfFlags != 0)
    {
        AddPerfExperimentRecords(&stream, perfFlags, m_spmEscapeFailed, (gfxIp.major == 10) && (gfxIp.minor == 3));
    }
    if (tmz) { stream.AddRecord4(RecordTmz); }
    AddExternPhysRecords(&stream, submit.externPhysMemCount, submit.ppExternPhysMem);
    static const bool trace = std::getenv("PAL_WDDM_TRACE") != nullptr;
    if (trace)
    {
        std::fprintf(stderr, "[pal-wddm] gang submit queue=%x serial=%llu entries=%u bytes=%zu first=%llx/%u\n",
                     NativeQueue().handle, NativeQueue().submitted + 1, stream.Entries(), stream.Data().size(),
                     stream.FirstIbAddress(), stream.FirstIbBytes());
    }
    return FinishSubmit(&stream, submit, true);
}
Result Queue::QueryKernelContextInfo(KernelContextInfo* pKernelContextInfo) const
{
    return CopyKernelContextInfo(m_kernelContext, pKernelContextInfo);
}
Result Queue::UpdateAppPowerProfile(const wchar_t* pFileName, const wchar_t* pPathName)
{
    auto* const device = static_cast<Device*>(m_pDevice);
    const EscapeRoute route = device->KmdEscapeRoute();
    const uint32 context = (m_pSubmissionContext != nullptr) ? NativeQueue().context : 0;
    return SendAppPowerProfile(SharedControlEscape(&route), context, pFileName, pPathName);
}
}
