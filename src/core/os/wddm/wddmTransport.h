// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "wddmProperties.h"
#include "wddmSharedMemory.h"

namespace Pal::Wddm
{
struct GpuVaMapPacket;
namespace Private { struct AllocationData; struct NativeAllocationRequest; }
using Status = int32_t;
inline constexpr Status Success = 0;
inline constexpr Status Timeout = 0x102;
inline constexpr Status InvalidParameter = static_cast<Status>(0xc000000du);
inline constexpr Status NotSupported = static_cast<Status>(0xc00000bbu);
inline constexpr Status NoMemory = static_cast<Status>(0xc0000017u);
inline constexpr Status DeviceRemoved = static_cast<Status>(0xc00002b6u);
constexpr bool Succeeded(Status status) { return status >= 0; }

// Raw request boundaries also used by the native-byte CPU differential fixture.
Status LockCpuPerCallRequest(uint32_t device, uint32_t allocation, void** cpuAddress);
Status UnlockCpuPerCallRequest(uint32_t device, uint32_t allocation);
// Native ordinary allocation boundary: legacy CREATE72/INFO40. Carriers and
// caller resource input are preserved; output handles publish only on raw0.
Status CreateAllocationRequest(uint32_t device, const void* headerData, uint32_t headerBytes,
                               const void* allocationData, uint32_t allocationBytes, void* hostPtr,
                               uint32_t createFlags, uint32_t* allocationOut, uint32_t* resourceInOut);
Status DestroyAllocationRequest(uint32_t device, uint32_t resource, uint32_t count,
                                const uint32_t* allocations, bool nativeClearFlags);
// Native 37ef20 request for one memory object: CREATE72 with 'count' INFO40 records, one private block per
// allocation (stride blockBytes) and a 64-byte resource header. Outputs publish only on raw STATUS_SUCCESS.
Status CreateAllocationsRequest(uint32_t device, const void* headerData, uint32_t headerBytes,
                                const uint8_t* blocks, uint32_t blockBytes, uint32_t count, void* const* hostPtrs,
                                uint32_t createFlags, uint32_t* allocationsOut, uint32_t* resourceInOut,
                                uint32_t* globalShareOut, uint32_t vidPnSourceId = UINT32_MAX,
                                uint32_t infoFlags = 0);
// Native 318150: D3DKMTEscape(DRIVERPRIVATE, DriverKnownEscape) TRANSLATEALLOCATIONHANDLE, in place.
Status TranslateAllocationHandleRequest(uint32_t adapter, uint32_t device, uint32_t* handle);
// Raw104-byte request only; preserves NTSTATUS and every callback-written byte.
Status MapGpuVaRequest(GpuVaMapPacket* packet);
// Native DMA postamble: increment the live serial before the zero-flags GPU2
// signal, retaining that increment on every native result, including wrap0.
Status SignalLegacyProgressRequest(uint32_t context, uint32_t fence, uint64_t* lastSubmitted);

struct NodeInfo
{
    uint32_t ordinal = 0;
    uint32_t engineType = 0;
    bool isCompute = false;
    bool isGraphics = false;
    bool isDma = false;
    bool isTimer = false;
    std::string name;
};
struct AdapterInfo
{
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t revisionId = 0;
    uint64_t driverVersion = 0;
    uint32_t luidLow = 0;
    int32_t luidHigh = 0;
    uint32_t virtualAddressBits = 0;
    uint64_t dedicatedVideoMemory = 0;
    uint64_t dedicatedSystemMemory = 0;
    uint64_t sharedSystemMemory = 0;
    std::vector<NodeInfo> nodes;
    PhysicalProperties physical;
};

// Non-owning OS object records. Transport and its PAL owner provide lifetime management:
// never copy a live record and destroy both copies. All GPU users must retire before destruction.
struct Allocation
{
    uint32_t handle = 0;
    uint32_t resource = 0;
    uint64_t gpuAddress = 0;
    uint64_t size = 0;
    void* cpuAddress = nullptr;
    void* originalCpuAddress = nullptr;
    bool locked = false;
    bool hostImported = false;
    bool ownsVa = false;
    bool resident = false;
    bool zeroOnUnmap = false; // Preserve a borrowed virtual owner's read-zero reservation.
    bool ordinaryLegacy = false; // Authenticated ordinary GART create/destroy protocol.
};
enum class QueueEngine { Graphics, Compute, Dma, Timer };
// Per-queue options of the KMD context and HW-queue records (native 37a980, 37b070, 383b20, 38b140). The defaults
// reproduce the records the backend used before these options existed.
struct QueueCreateOptions
{
    uint32_t priority            = 0;     // QueuePriority; HW-queue DWORD+16 bits 12..14 through table 6be48d0.
    bool     tmzOnly             = false; // HW-queue DWORD+16 bit16 / legacy context +56 bit19.
    uint32_t tmzLevel            = 0;     // Pal::Queue+0x9c: DWORD+16 bits 20..23 / legacy context +56 bits 23..26.
    bool     dispatchTunneling   = false; // HW-queue DWORD+16 bit15.
    uint32_t numReservedCu       = 0;     // HW-queue DWORD+16 bits 4..11.
    bool     disableGpuTimeout   = false; // CREATEHWQUEUE (HWS) or context (legacy, timer) Flags.DisableGpuTimeout.
    bool     nativeProgressFence = false; // CREATEHWQUEUE Flags.NativeProgressFence.
    bool     nullRendering       = false; // Context Flags.NullRendering (IFH implemented by the kernel).
};
struct QueueState
{
    uint32_t context = 0;
    uint32_t handle = 0;
    uint32_t queueId = 0;
    uint32_t progressFence = 0;
    const volatile uint64_t* progressCpu = nullptr;
    uint64_t progressGpu = 0;
    uint64_t submitted = 0;
    uint32_t mappingFence = 0;
    const volatile uint64_t* mappingCpu = nullptr;
    uint64_t mappingGpu = 0;
    uint64_t mappingSerial = 0;
    QueueEngine engine = QueueEngine::Graphics;
    bool hardwareQueue = true;
    bool ownsProgressFence = false; // Legacy queues own a separately created monitored fence.
    // Native queue+0x18c/+0x190: the windowed presenter's copy into a DWM redirection surface (2d1eeb) marks the
    // next legacy-context submission PresentRedirected with the DWM update id (384670, 3847e4..384802).
    bool redirectedPresentPending = false;
    uint64_t redirectedPresentToken = 0;
};
struct SyncObject
{
    uint32_t handle = 0;
    const volatile uint64_t* cpuAddress = nullptr;
    uint64_t gpuAddress = 0;
    uint32_t sharedHandle = 0; // Global KMT share ID, not an NT handle.
    uint32_t nativeFlags = 0;  // Public KMT flags returned by successful creation/open.
    bool ownsHandle = true;
    bool nativeFence = false;
    bool gpuOnly = false;
    uint64_t monitoredGpuAddress = 0;
};
struct SyncCreateFlags
{
    bool shareable = false;
    bool sharedViaNtHandle = false;
    bool noSignalOnDeviceLost = false;
};
enum class SparseOperationType { Map, Unmap, Copy };
struct SparseOperation
{
    SparseOperationType type = SparseOperationType::Map;
    uint64_t virtualAddress = 0;
    uint64_t size = 0;
    uint32_t allocationHandle = 0;
    uint64_t allocationOffset = 0;
    uint64_t sourceAddress = 0;
    bool zero = false;
    bool noAccess = false;
};
struct IbEntry
{
    uint64_t address;
    uint32_t bytes;
    uint32_t flags; // Observed AMD record flags: main=0, preamble=0xc, postamble=0x104.
};
enum class CacheMode { Default, Gl2Bypass };
enum class MemoryHeap { GartCacheable, GartUswc, Local, Invisible };

class Transport final
{
public:
    Transport();
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    static Status Enumerate(std::vector<AdapterInfo>* adapters);
    static bool IsVerifiedAdapter(const AdapterInfo& adapter);
    Status Open(const AdapterInfo& adapter);
    void Close();
    const AdapterInfo& Identity() const;
    uint32_t DeviceHandle() const;
    uint32_t AdapterHandle() const;
    bool IsOpen() const;
    void Quarantine();

    Status ReserveVa(uint64_t base, uint64_t minimum, uint64_t maximum, uint64_t size,
                     bool zero, uint64_t* address);
    Status FreeVa(uint64_t address, uint64_t size);
    // Restore an existing reservation without freeing any part of its owning VA range.
    Status ResetVa(uint64_t address, uint64_t size, bool zero);
    // size is rounded to 64 KiB for owned allocations and 4 KiB for imported host memory.
    // hostPtr must already be page aligned and remain alive until DestroyAllocation succeeds.
    // requestedGpuVa must be zero or lie inside a reservation held by the caller.
    Status Allocate(uint64_t size, uint64_t alignment, bool cpuVisible, bool executable,
                    void* hostPtr, uint64_t requestedGpuVa, Allocation* allocation,
                    uint64_t minimum = 0, uint64_t maximum = 0,
                    CacheMode cache = CacheMode::Default,
                    MemoryHeap heap = MemoryHeap::GartCacheable,
                    uint32_t priority = 3, uint32_t priorityOffset = 0,
                    bool lockCpuOnCreate = true,
                    const Private::AllocationData* ordinaryGartRecords = nullptr);
    Status DestroyAllocation(Allocation* allocation);
    // Native 37ef20 + 381870 for the allocations of one memory object: one legacy create, then each
    // allocation is made resident and mapped at base + i * 128 MiB inside the caller's reservation with the
    // native bounds. hostBase + request.hostOffsets[i] is the pinned system memory of allocation i. On any
    // failure every created allocation is destroyed and nothing is published.
    Status CreateNativeAllocations(const Private::NativeAllocationRequest& request, void* hostBase, uint64_t base,
                                   uint64_t minimum, uint64_t maximum, Allocation* allocations,
                                   uint32_t* resource, uint32_t* globalShare);
    // Native 309d20 with the object's handle list: each mapping is first restored inside the caller's
    // reservation, then one DestroyAllocation2 releases all of them.
    Status DestroyAllocations(Allocation* allocations, uint32_t count);
    Status MapAllocation(Allocation* allocation, uint64_t requestedGpuVa, bool executable,
                         uint64_t minimum = 0, uint64_t maximum = 0);
    Status UnmapAllocation(Allocation* allocation);
    // Exact per-call KMT boundary. The PAL object owns its map count/cache policy.
    Status LockCpuPerCall(const Allocation& allocation, void** cpuAddress);
    Status UnlockCpuPerCall(const Allocation& allocation);
    Status LockAllocation(Allocation* allocation);
    Status UnlockAllocation(Allocation* allocation);
    Status MakeResident(Allocation* allocation);
    Status Evict(Allocation* allocation);
    Status SetAllocationPriority(const Allocation& allocation, uint32_t priority);
    // Native 37f960 for a multi-allocation object (one request for all handles).
    Status SetAllocationPriority(const Allocation* allocations, uint32_t count, uint32_t priority);
    // Native 318150.
    Status TranslateAllocationHandle(uint32_t handle, uint32_t* translated);
    // KMTQAITYPE_DRIVERVERSION (KMT_DRIVERVERSION_WDDM_x_y).
    Status QueryWddmVersion(uint32_t* version) const;
    // KMTQAITYPE_ADAPTERTYPE HybridDiscrete bit (native adapter+0x99c bit0, 3001b0).
    Status QueryHybridDiscrete(bool* value) const;

    Status CreateQueue(uint32_t node, bool compute, QueueState* queue, uint32_t priority = 0);
    Status CreateQueue(uint32_t node, QueueEngine engine, QueueState* queue, uint32_t priority = 0);
    Status CreateQueue(uint32_t node, QueueEngine engine, QueueState* queue, const QueueCreateOptions& options);
    Status DestroyQueue(QueueState* queue);
    Status Submit(QueueState* queue, const IbEntry* entries, size_t count, uint64_t* serial);
    // Native 37b9c0 (HWS) / 384670 (legacy) KMT boundary for a prebuilt private stream. primaries are the KMT
    // allocation handles of MultiSubmitInfo::ppBlockIfFlipping (WrittenPrimaries, at most 16).
    Status SubmitPrivate(QueueState* queue, uint64_t commandBuffer, uint32_t commandLength, const uint8_t* data,
                         size_t bytes, const uint32_t* primaries, uint32_t primaryCount, uint64_t* serial);
    // Native 30ee80: a GPU wait for the device paging fence, issued only when its CPU value is below the target.
    Status WaitPagingFenceOnQueue(const QueueState& queue, uint64_t value);
    Status Delay(QueueState* queue, float microseconds);
    // Native 38b5b0 with a private screen: vsync-relative delay on the screen's VidPN source.
    Status Delay(QueueState* queue, float microseconds, uint32_t sourceId, bool afterVsync);
    Status Wait(const QueueState& queue, uint64_t serial, uint64_t timeoutNs);
    uint64_t Completed(const QueueState& queue) const;
    // Register the caller-owned event for the last submitted queue serial. This does not
    // wait or transfer event ownership. The caller serializes access to queue submissions.
    Status RegisterQueueEvent(const QueueState& queue, void* event);
    Status UpdateVa(QueueState* queue, const SparseOperation* operations, size_t count, bool doNotWait);
    Status WaitMapping(const QueueState& queue, uint64_t timeoutNs);

    Status CreateSync(uint64_t initialValue, bool shareable, SyncObject* object);
    Status CreateSync(uint64_t initialValue, const SyncCreateFlags& flags, SyncObject* object);
    Status QueryNativeFenceSupport(bool* supported) const;
    Status CreateNativeSync(uint64_t initialValue, bool gpuOnly,
                            const SyncCreateFlags& flags, SyncObject* object);
    Status OpenNativeSync(void* ntHandle, SyncObject* object);
    Status CreateCountingSync(uint32_t initialCount, uint32_t maxCount, bool shareable, SyncObject* object);
    Status CreateCountingSync(uint32_t initialCount, uint32_t maxCount,
                              const SyncCreateFlags& flags, SyncObject* object);
    Status DestroySync(SyncObject* object);
    Status WaitCpu(const SyncObject& object, uint64_t value, uint64_t timeoutNs);
    Status WaitCpuMany(const SyncObject* objects, const uint64_t* values, size_t count,
                       bool waitAll, uint64_t timeoutNs);
    Status SignalCpu(const SyncObject& object, uint64_t value);
    Status WaitQueue(const QueueState& queue, const SyncObject& object, uint64_t value);
    Status SignalQueue(const QueueState& queue, const SyncObject& object, uint64_t value);
    Status OpenSync(void* ntHandle, SyncObject* object);
    Status OpenCountingSync(void* ntHandle, SyncObject* object);
    Status OpenGlobalSync(uint32_t globalHandle, SyncObject* object);
    // References a same-process external local KMT handle; its owner retains lifetime.
    Status BorrowSync(uint32_t localHandle, SyncObject* object);
    // The caller owns the returned NT handle and must close it with CloseHandle.
    Status ExportSync(const SyncObject& object, void** ntHandle);
    // NT export transfers a CloseHandle handle; global export returns a non-owning KMT ID.
    Status ExportSync(const SyncObject& object, bool sharedViaNtHandle, void** handle);
    Status OpenSharedAllocation(void* ntHandle, bool executable, Allocation* allocation,
                                SharedAllocationProperties* properties = nullptr,
                                bool* resourceRecognized = nullptr, bool dataBufferOnly = false);

private:
    struct Impl;
    Impl* m_impl;
};
}
