// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/os/wddm/wddmDeviceControls.h"
#include "core/queue.h"
#include "core/os/wddm/wddmTransport.h"
#include "core/os/wddm/wddmGang.h"
#include "core/os/wddm/wddmPresent.h"
#include "core/os/wddm/wddmSubmit.h"
#include <atomic>

namespace Pal::Wddm
{
class Device;
class SubmissionContext final : public Pal::SubmissionContext
{
public:
    explicit SubmissionContext(Device* device);
    ~SubmissionContext() override;
    bool IsTimestampRetired(uint64 timestamp) const override;
    QueueState& NativeQueue() { return m_queue; }
    const QueueState& NativeQueue() const { return m_queue; }
    Device* GetDevice() const { return m_device; }
private:
    Device* m_device;
    QueueState m_queue;
};

// Implicitly ganged ACE sub-queue of a single queue (native Queue+0x2c0..0x308, created by 37cfb0).
struct ImplicitAceState
{
    bool       created = false; // Queue+0x308.
    uint32     queueId = 0;     // Queue+0x2c4: the ACE HW queue id, or the leader's id when registered by escape.
    QueueState follower;        // Separate-HW-queue mode only.
};

class Queue final : public Pal::Queue
{
public:
    Queue(uint32 count, Device* device, const QueueCreateInfo* createInfo);
    // gangNotified is the Device-owned one-time flag of escape 309f40 (native 4d8c00 sends it for multi-queue
    // submissions and for implicit ACE gangs of ordinary queues alike).
    Queue(uint32 count, Device* device, const QueueCreateInfo* createInfo, std::atomic<bool>* gangNotified);
    ~Queue() override;
    Result Init(const QueueCreateInfo* createInfo, void* contextPlacement) override;
    // Native queue slots 21 (4d89e0) and 9 (4d8a10), shared by the HWS, legacy and timer vtables.
    Result QueryKernelContextInfo(KernelContextInfo* pKernelContextInfo) const override;
    Result UpdateAppPowerProfile(const wchar_t* pFileName, const wchar_t* pPathName) override;
    void OsDestroy() override;
    Result OsSubmit(const MultiSubmitInfo& submitInfo, const InternalSubmitInfo* internalInfo) override;
    QueueState& NativeQueue();
    const QueueState& NativeQueue() const;
    Result SignalNativeSync(const SyncObject& object, uint64 value);
    // Native queue+0x1d0: the windowed presenter (read by the Windows present scheduler 4dd910).
    const Presenter* WindowedPresenter() const { return m_windowedPresenter; }
    Result WaitNativeSync(const SyncObject& object, uint64 value);

protected:
    Result OsWaitIdle() override;
    Result OsDelay(Util::fmicroseconds delay, const IPrivateScreen* screen) override;
    Result OsPresentDirect(const PresentDirectInfo& info) override;
    Result OsRemapVirtualMemoryPages(uint32 count, const VirtualMemoryRemapRange* ranges,
                                    bool doNotWait, IFence* fence) override;
    Result OsCopyVirtualMemoryPageMappings(uint32 count, const VirtualMemoryCopyPageMappingsRange* ranges,
                                          bool doNotWait) override;
    Result DoAssociateFenceWithLastSubmit(Pal::Fence* fence) override;

private:
    Result InitGang(const QueueCreateInfo* createInfo, void* contextPlacement);
    QueueCreateOptions CreateOptions(const QueueCreateInfo& createInfo, uint32 node, bool leader) const;
    Result OsSubmitSingle(const MultiSubmitInfo& submitInfo, const InternalSubmitInfo& internalInfo);
    Result OsSubmitDma(const MultiSubmitInfo& submitInfo, const InternalSubmitInfo& internalInfo);
    Result OsSubmitGang(const MultiSubmitInfo& submitInfo, const InternalSubmitInfo* internalInfo);
    Result OsSubmitImplicitGang(const MultiSubmitInfo& submitInfo, const InternalSubmitInfo& internalInfo);
    Result AddChainedIbs(SubmitStream* stream, const InternalSubmitInfo& internalInfo, uint32 count,
                         ICmdBuffer*const* cmdBuffers, uint32* consumed);
    Result AddGangSubQueue(SubmitStream* stream, bool allEngines, uint32 index, uint32 count,
                           ICmdBuffer*const* cmdBuffers, const InternalSubmitInfo& internalInfo,
                           bool* tmzSeen, uint32* perfFlags);
    void AccumulatePagingFence(uint32 subQueue, uint32 count, ICmdBuffer*const* cmdBuffers,
                               const CmdBufInfo* infos, const InternalSubmitInfo& internalInfo,
                               SubmitStream* stream);
    Result CreateImplicitAce();
    Result FinishSubmit(SubmitStream* stream, const MultiSubmitInfo& submitInfo, bool lastBatch);
    void DestroyGangMembers();

    GangQueueState     m_gang;
    ImplicitAceState   m_ace;
    bool               m_aceEscapeMode = false;  // Queue+0x35d for an ordinary universal queue (37a180).
    bool               m_hasGfxSubQueue = false; // Queue+0x1e0: a universal or compute sub-queue exists (4d7f30).
    uint32             m_perfFlags = 0;          // Queue+0x2ac: last OR of GfxCmdBuffer::PerfTracesEnabled().
    bool               m_spmEscapeFailed = false;// Queue+0x2b0: escape 0x03010103 failed once; records are used.
    uint64             m_pagingFence = 0;        // Queue+0x198: paging fence awaited before the KMD submission.
    KernelContextState m_kernelContext;          // Native Queue+0x140..+0x150.
    Presenter* m_windowedPresenter = nullptr;   // Native queue+0x1d0 (4d84b0).
    Presenter* m_fullscreenPresenter = nullptr; // Native queue+0x1d8.
};
}
