// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmDeviceControls.h"
#include "core/os/wddm/wddmApplicationProfile.h"
#include "core/os/wddm/wddmEventWait.h"
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmPrivate.h"
#include "core/os/wddm/wddmProperties.h"
#include "core/os/wddm/wddmTransport.h"
#include "core/device.h"
#include "core/settingsLoader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfloat>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
constexpr uint32 KernelContextCommand   = 0x03008603;
constexpr uint32 AppPowerProfileCommand = 0x03007503;

// RS feature output DWORD n of a 0x0300a503 reply (output block at record+0x108).
uint32 RsOutput(const RsFeatureRecord& record, uint32 index) { return GetRecord32(record.data(), 0x108 + 4 * index); }

void PutByte(RsFeatureInfo* info, size_t offset, bool value) { reinterpret_cast<uint8*>(info)[offset] = value ? 1 : 0; }
void PutDword(RsFeatureInfo* info, size_t offset, uint32 value) { PutRecord32(info, offset, value); }

uint32 PageDword(uint64 page, size_t offset)
{ return *reinterpret_cast<const volatile uint32*>(uintptr_t(page) + offset); }
// RS feature change counters: uint16 words at rsFeatureSettings+4.
uint32 Counter(uint64 page, uint32 index)
{ return *reinterpret_cast<const volatile uint16*>(uintptr_t(page) + 4 + 2 * index); }

uint32 NativeProcessId(void*) { return GetCurrentProcessId(); }
void* NativeCreateHipEvent(void*)
{
    // 31835d..318385: a zeroed SECURITY_ATTRIBUTES (nLength 0), auto-reset, initially non-signaled, named.
    SECURITY_ATTRIBUTES attributes = {};
    return CreateEventW(&attributes, FALSE, FALSE, L"HipRuntimeInit");
}
Result NativeWaitHipEvent(void*, void* event, float seconds) { return WaitEvent(event, seconds); }
void NativeCloseHipEvent(void*, void* event) { CloseHandle(event); }
const HipCalls& NativeHipCalls()
{
    static const HipCalls calls = {nullptr, NativeProcessId, NativeCreateHipEvent, NativeWaitHipEvent,
                                   NativeCloseHipEvent};
    return calls;
}

uint32 GpuCapabilities(const PhysicalProperties& physical) { return GetRecord32(physical.gpuRecord.data(), 16180); }
} // namespace

// =====================================================================================================================
EscapeRoute ControlsEscapeRoute(const Pal::Device& device, const Transport& transport)
{
    EscapeRoute route = DeviceEscapeRoute(transport, device.GetPlatform());
    route.suppressed = device.GetPublicSettings()->disableEscapeCall;
    return route;
}

ControlEscape SharedControlEscape(const EscapeRoute* route)
{
    ControlEscape escape;
    escape.context = const_cast<EscapeRoute*>(route);
    escape.send = [](void* context, void* record, size_t bytes, bool hardwareAccess, uint32 hContext)
    { return SharedEscape(*static_cast<const EscapeRoute*>(context), record, bytes, hardwareAccess, hContext); };
    return escape;
}

RsInterfaceVersion ReadRsInterfaceVersion(const PhysicalProperties& physical)
{
    RsInterfaceVersion version;
    version.major = GetRecord32(physical.gpuRecord.data(), 14856);
    version.minor = GetRecord32(physical.gpuRecord.data(), 14860);
    return version;
}

// =====================================================================================================================
RsFeatureRecord BuildRsFeatureRecord(uint32 subOp)
{
    RsFeatureRecord record = {};
    InitPrivateRecord(record.data(), record.size(), RsFeatureCommand);
    PutRecord32(record.data(), 0xc, 3);
    PutRecord32(record.data(), 0x80, 0x80);
    PutRecord32(record.data(), 0x84, subOp);
    PutRecord32(record.data(), 0x104, 0x80);
    return record;
}

// =====================================================================================================================
// Native 316260. The original has no pointer validation and always succeeds; a null output pointer, which the
// original dereferences, is rejected here before any cache update.
Result DidRsFeatureSettingsChange(RsInterfaceVersion version, KmdSharedBuffers* buffers, uint32 rsFeatures,
                                  uint32* pRsFeaturesChanged)
{
    if ((buffers == nullptr) || (pRsFeaturesChanged == nullptr)) { return Result::ErrorInvalidPointer; }
    uint32* const cache = buffers->rsFeatureCache;
    const bool escapeQueries = RsUsesEscapeQueries(version);
    const uint64 counters = buffers->rsFeatureSettings;
    uint32 changed = 0;
    // Each counter is read once for the comparison and again for the stored value, except Chill and bit 3.
    if (((rsFeatures & RsFeatureTypeTurboSync) != 0) && (counters != 0))
    {
        changed = (cache[1] != Counter(counters, 0)) ? uint32(RsFeatureTypeTurboSync) : 0;
        cache[1] = Counter(counters, 0);
    }
    if ((rsFeatures & RsFeatureTypeChill) != 0)
    {
        const uint32 value = (escapeQueries && (counters != 0)) ? Counter(counters, 4)
                           : (buffers->rsFeatureChange != 0)    ? PageDword(buffers->rsFeatureChange, 0)
                                                                : cache[0];
        if (cache[0] != value) { changed |= RsFeatureTypeChill; }
        cache[0] = value;
    }
    const struct { uint32 bit; uint32 counter; uint32 slot; } words[] = {
        {RsFeatureTypeDelag, 1, 2}, {RsFeatureTypeBoost, 2, 3}, {RsFeatureTypeProVsr, 5, 4}, {0x40, 6, 5}};
    for (const auto& word : words)
    {
        if (((rsFeatures & word.bit) != 0) && (counters != 0))
        {
            if (cache[word.slot] != Counter(counters, word.counter)) { changed |= word.bit; }
            cache[word.slot] = Counter(counters, word.counter);
        }
    }
    if ((rsFeatures & 0x8) != 0)
    {
        const uint32 value = (escapeQueries && (counters != 0)) ? Counter(counters, 3) : 0;
        if (cache[6] != value) { changed |= 0x8; }
        cache[6] = value;
    }
    *pRsFeaturesChanged = changed;
    return Result::Success;
}

// =====================================================================================================================
// Native 316460. The 20-byte output is cleared first. Feature bits 3, 6 and 7 are not named by this tree's
// RsFeatureType; they keep the original byte layout (bit 3: bool at +0, float at +4; bit 6: bool at +0).
Result GetRsFeatureGlobalSettings(const ControlEscape& escape, RsInterfaceVersion version,
                                  const KmdSharedBuffers& buffers, const RsTurboSyncReader& turboSync,
                                  uint32 risSharpnessBits, RsFeatureType rsFeature, RsFeatureInfo* pRsFeatureInfo)
{
    static_assert(sizeof(RsFeatureInfo) == 20, "The original clears and fills 20 bytes");
    if (pRsFeatureInfo == nullptr) { return Result::ErrorInvalidPointer; }
    std::memset(pRsFeatureInfo, 0, sizeof(*pRsFeatureInfo));
    const uint32 feature = uint32(rsFeature);
    if (feature == RsFeatureTypeTurboSync)
    {
        bool enabled = false;
        if (turboSync.read != nullptr) { turboSync.read(turboSync.context, &enabled); }
        PutByte(pRsFeatureInfo, 0, enabled);
        return Result::Success;
    }
    uint32 subOp = 0;
    if (RsUsesEscapeQueries(version))
    {
        switch (feature)
        {
        case RsFeatureTypeChill:  subOp = 5;   break;
        case RsFeatureTypeDelag:  subOp = 1;   break;
        case RsFeatureTypeBoost:  subOp = 2;   break;
        case RsFeatureTypeProVsr: subOp = 6;   break;
        case 0x40:                subOp = 0xa; break;
        case 0x8:                 subOp = 3;   break;
        case 0x80:                subOp = 9;   break;
        default:                  return Result::Success;
        }
        RsFeatureRecord record = BuildRsFeatureRecord(subOp);
        const Result result = escape(record.data(), record.size());
        if (result != Result::Success) { return result; }
        switch (feature)
        {
        case RsFeatureTypeChill:
            PutByte(pRsFeatureInfo, 0, RsOutput(record, 0) != 0);
            pRsFeatureInfo->chill.hotkey = RsOutput(record, 1);
            pRsFeatureInfo->chill.minFps = RsOutput(record, 2);
            pRsFeatureInfo->chill.maxFps = RsOutput(record, 3);
            break;
        case RsFeatureTypeDelag:
            PutByte(pRsFeatureInfo, 0, RsOutput(record, 1) != 0);
            pRsFeatureInfo->delag.hotkey    = RsOutput(record, 0);
            pRsFeatureInfo->delag.limitFps  = RsOutput(record, 2);
            pRsFeatureInfo->delag.level     = RsOutput(record, 3);
            pRsFeatureInfo->delag.hotkeyInd = RsOutput(record, 4);
            break;
        case RsFeatureTypeBoost:
            PutByte(pRsFeatureInfo, 0, RsOutput(record, 1) != 0);
            pRsFeatureInfo->boost.hotkey    = RsOutput(record, 0);
            pRsFeatureInfo->boost.minRes    = RsOutput(record, 2);
            pRsFeatureInfo->boost.hotkeyInd = RsOutput(record, 4);
            PutByte(pRsFeatureInfo, 0x10, RsOutput(record, 5) != 0);
            break;
        case RsFeatureTypeProVsr:
            PutByte(pRsFeatureInfo, 0, RsOutput(record, 1) != 0);
            pRsFeatureInfo->proVsr.hotkey = RsOutput(record, 0);
            break;
        case 0x40:
            PutByte(pRsFeatureInfo, 0, RsOutput(record, 0) != 0);
            break;
        case 0x8:
            PutDword(pRsFeatureInfo, 4, RsOutput(record, 1));
            PutByte(pRsFeatureInfo, 0, RsOutput(record, 0) != 0);
            break;
        default:
            break;
        }
        return Result::Success;
    }
    // Interfaces before 2019.1 read the KMD pages for Chill, Delag and Boost.
    switch (feature)
    {
    case RsFeatureTypeChill:
        PutByte(pRsFeatureInfo, 0,
                (buffers.rsFeatureChange != 0) && ((PageDword(buffers.rsFeatureChange, 4) & 1) != 0));
        return Result::Success;
    case RsFeatureTypeDelag:
    case RsFeatureTypeBoost:
    {
        const uint32 value = (buffers.rsFeatureSettings == 0) ? 0
            : PageDword(buffers.rsFeatureSettings, (feature == RsFeatureTypeDelag) ? 0x48 : 0x44);
        PutDword(pRsFeatureInfo, 4, value);
        PutByte(pRsFeatureInfo, 0, value != 0);
        return Result::Success;
    }
    case 0x40: subOp = 0xa; break;
    case 0x8:  subOp = 3;   break;
    default:   return Result::Success;
    }
    RsFeatureRecord record = BuildRsFeatureRecord(subOp);
    const Result result = escape(record.data(), record.size());
    if (result == Result::Success)
    {
        PutByte(pRsFeatureInfo, 0, RsOutput(record, 0) != 0);
        if (feature == 0x8) { PutDword(pRsFeatureInfo, 4, risSharpnessBits); }
    }
    return result;
}

// =====================================================================================================================
// Native 316b40: interfaces before 2018 report ErrorUnavailable; otherwise the timestamp is stored into the KMD
// page when one was mapped, and the call succeeds either way.
Result UpdateChillStatus(RsInterfaceVersion version, const KmdSharedBuffers& buffers,
                         uint64 lastChillActiveTimeStampUs)
{
    if (version.major < 2018) { return Result::ErrorUnavailable; }
    if (buffers.chillStatus != 0)
    {
        *reinterpret_cast<volatile uint64*>(uintptr_t(buffers.chillStatus)) = lastChillActiveTimeStampUs;
    }
    return Result::Success;
}

// =====================================================================================================================
// Native 30bac0. Each tri-state still at Default is resolved from the KMD: a nonzero first output DWORD selects
// Enabled (1); a zero DWORD or any Escape failure selects Disabled (2).
void FinalizeControlSettings(const ControlEscape& escape, uint32 gpuCapabilities, uint32 settingOverride,
                             FinalizedControlSettings* state)
{
    const auto resolve = [&escape](uint32 subOp)
    {
        RsFeatureRecord record = BuildRsFeatureRecord(subOp);
        bool disabled = true;
        if (escape(record.data(), record.size()) == Result::Success)
        {
            disabled = (RsOutput(record, 0) == 0);
        }
        return disabled ? 2u : 1u;
    };
    if (state->feature40State == 0) { state->feature40State = resolve(0xa); }
    if (state->risState == 0) { state->risState = resolve(3); }
    switch (settingOverride)
    {
    case 1:  state->submitFlag = true;  break;
    case 2:  state->submitFlag = false; break;
    default: state->submitFlag = ((gpuCapabilities >> 13) & 1) != 0; break;
    }
}

// =====================================================================================================================
HipRecord BuildHipRecord(uint32 processId, uint32 subOp)
{
    HipRecord record = {};
    InitPrivateRecord(record.data(), record.size(), HipRuntimeCommand);
    PutRecord32(record.data(), 0xc, 3);
    PutRecord32(record.data(), 0x80, 0x100);
    PutRecord32(record.data(), 0x84, processId);
    PutRecord32(record.data(), 0x88, subOp);
    PutRecord32(record.data(), 0x184, 0x1080);
    return record;
}

// Native 3181f0: version query (sub-op 0xd), then registration (sub-op 1) carrying an event the KMD signals, then a
// gfxTimeout-second wait on that event (0 waits forever).
Result RegisterHipRuntimeState(const ControlEscape& escape, uint32 gfxTimeoutSeconds,
                               const HipRuntimeSetup& runtimeState, const HipCalls* calls)
{
    const HipCalls& native = (calls != nullptr) ? *calls : NativeHipCalls();
    HipRecord record = BuildHipRecord(native.processId(native.userData), 0xd);
    Result result = escape(record.data(), record.size());
    if (result != Result::Success) { return result; }
    const uint32 major = GetRecord32(record.data(), 0x1c8);
    const uint32 minor = GetRecord32(record.data(), 0x1cc);
    if ((major > 2) || ((major == 1) && (minor < 2))) { return Result::Unsupported; }

    record = BuildHipRecord(native.processId(native.userData), 1);
    PutRecord64(record.data(), 0xc4, uint64(uintptr_t(runtimeState.pRdebug)));
    PutRecord32(record.data(), 0xcc, runtimeState.runtimeState);
    PutRecord32(record.data(), 0xd0, runtimeState.ttmpSetupHint);
    void* const event = native.createEvent(native.userData);
    if (event == nullptr) { return Result::ErrorInitializationFailed; }
    PutRecord64(record.data(), 0xd4, uint64(uintptr_t(event)));
    result = escape(record.data(), record.size());
    if (result == Result::Success)
    {
        const float seconds = (gfxTimeoutSeconds == 0) ? FLT_MAX : float(int64(gfxTimeoutSeconds));
        result = native.wait(native.userData, event, seconds);
    }
    native.closeEvent(native.userData, event);
    return result;
}

// Native 318450 (sub-op 0x14).
Result SetHipTrapHandler(const ControlEscape& escape, gpusize trapHandlerAddress, gpusize trapMemoryAddress,
                         const HipCalls* calls)
{
    const HipCalls& native = (calls != nullptr) ? *calls : NativeHipCalls();
    HipRecord record = BuildHipRecord(native.processId(native.userData), 0x14);
    PutRecord64(record.data(), 0xc4, trapHandlerAddress);
    PutRecord64(record.data(), 0xcc, trapMemoryAddress);
    return escape(record.data(), record.size());
}

gpusize HipTrapAddress(const IGpuMemory* pGpuMemory, gpusize offset)
{
    return (pGpuMemory != nullptr) ? (pGpuMemory->Desc().gpuVirtAddr + offset) : 0;
}

// =====================================================================================================================
BusAddressRecord BuildBusAddressRecord(uint32 gfxMajor, uint32 operation, uint64 argument)
{
    BusAddressRecord record = {};
    InitPrivateRecord(record.data(), record.size(), (gfxMajor == 11) ? 0x0300a903 : 0x03003b03);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0x84, 0x40);
    PutRecord32(record.data(), 0x88, operation);
    if (operation == 0) { PutRecord32(record.data(), 0x8c, uint32(argument)); }
    else if (operation == 1) { PutRecord64(record.data(), 0x8c, argument); }
    PutRecord32(record.data(), 0xc4, 0x58);
    PutRecord32(record.data(), 0xc8, 0x58);
    return record;
}

SdiSurfaceRecord BuildSdiSurfaceRecord(uint32 gfxMajor, gpusize size, gpusize surfaceBusAddr, gpusize markerBusAddr)
{
    SdiSurfaceRecord record = {};
    InitPrivateRecord(record.data(), record.size(), (gfxMajor == 11) ? 0x0300a803 : 0x03003c03);
    PutRecord32(record.data(), 0x80, 0x80);
    PutRecord32(record.data(), 0x84, 0x80);
    PutRecord32(record.data(), 0x88, 1);
    PutRecord64(record.data(), 0xc8, size);
    PutRecord64(record.data(), 0xd0, surfaceBusAddr);
    PutRecord64(record.data(), 0xd8, markerBusAddr);
    PutRecord32(record.data(), 0xe0, 1);
    PutRecord32(record.data(), 0x104, 0x40);
    PutRecord32(record.data(), 0x108, 0x40);
    return record;
}

// Native 315fa0: the 24-byte reply at record+0x108 is published only on success.
Result QueryBusAddress(const ControlEscape& escape, uint32 gfxMajor, uint32 operation, uint64 argument,
                       BusAddressReply* pReply)
{
    BusAddressRecord record = BuildBusAddressRecord(gfxMajor, operation, argument);
    const Result result = escape(record.data(), record.size());
    if (result == Result::Success)
    {
        for (uint32 index = 0; index < 3; ++index)
        {
            pReply->value[index] = GetRecord64(record.data(), 0x108 + 8 * index);
        }
    }
    return result;
}

// Native 3160b0: register the remote surface, then translate the marker and surface bus addresses. Each index is the
// sign-extended low DWORD of reply qword 1; reply qword 2 is the GPU VA. Marker fields survive a later surface failure.
Result RegisterSdiRemoteSurface(const ControlEscape& escape, uint32 gfxMajor, SdiMemoryFields* fields)
{
    SdiSurfaceRecord record = BuildSdiSurfaceRecord(gfxMajor, fields->size, fields->surfaceBusAddr,
                                                    fields->markerBusAddr);
    Result result = escape(record.data(), record.size());
    BusAddressReply reply;
    if (result == Result::Success) { result = QueryBusAddress(escape, gfxMajor, 1, fields->markerBusAddr, &reply); }
    if (result == Result::Success)
    {
        fields->markerIndex = gpusize(int64(int32(uint32(reply.value[1]))));
        fields->markerVa    = reply.value[2];
        result = QueryBusAddress(escape, gfxMajor, 1, fields->surfaceBusAddr, &reply);
    }
    if (result == Result::Success)
    {
        fields->surfaceIndex = gpusize(int64(int32(uint32(reply.value[1]))));
        fields->gpuVirtAddr  = reply.value[2];
    }
    return result;
}

// Native 381d00. Only an SDI external-physical object without a GPU VA accepts the addresses; both zero is
// ErrorInvalidValue. The addresses stay recorded even if the registration fails.
Result SetSdiRemoteBusAddress(const ControlEscape& escape, uint32 gfxMajor, gpusize surfaceBusAddr,
                              gpusize markerBusAddr, SdiMemoryFields* fields)
{
    if ((fields->externPhys == false) || (fields->gpuVirtAddr != 0)) { return Result::ErrorUnavailable; }
    if ((surfaceBusAddr == 0) && (markerBusAddr == 0)) { return Result::ErrorInvalidValue; }
    fields->surfaceBusAddr = surfaceBusAddr;
    fields->markerBusAddr  = markerBusAddr;
    return RegisterSdiRemoteSurface(escape, gfxMajor, fields);
}

Result InitBusAddressableStep(const ControlEscape& escape, uint32 gfxMajor, BusAddressableFields* fields)
{
    if (fields->busAddressable == false) { return Result::ErrorOutOfGpuMemory; }
    BusAddressReply reply;
    const Result result = QueryBusAddress(escape, gfxMajor, 0, fields->allocationHandle, &reply);
    if (result == Result::Success)
    {
        fields->surfaceBusAddr = reply.value[1];
        fields->markerBusAddr  = reply.value[2];
        fields->markerVa       = reply.value[2] + fields->gpuVirtAddr - reply.value[1];
    }
    return result;
}

// Native 30e340. The queue is unused. Each memory must be bus addressable, otherwise ErrorOutOfGpuMemory ends the
// walk. The original first waits for a pending bus-addressable paging fence (Device+0xdcf8); AddGpuMemoryReferences
// records one only for bus-addressable memory, which this backend cannot create, so there is never one to wait for.
Result InitBusAddressableGpuMemory(const ControlEscape& escape, uint32 gfxMajor, Util::Mutex* pLock,
                                   uint32 gpuMemCount, IGpuMemory* const* ppGpuMemList)
{
    Util::MutexAuto lock(pLock);
    Result result = Result::Success;
    for (uint32 index = 0; (index < gpuMemCount) && (result == Result::Success); ++index)
    {
        // The original dereferences the list and each entry without checks.
        if ((ppGpuMemList == nullptr) || (ppGpuMemList[index] == nullptr)) { return Result::ErrorInvalidPointer; }
        auto* const memory = static_cast<GpuMemory*>(static_cast<Pal::GpuMemory*>(ppGpuMemList[index]));
        BusAddressableFields fields;
        fields.busAddressable   = memory->IsBusAddressable();
        fields.allocationHandle = memory->OsAllocation().handle;
        fields.gpuVirtAddr      = memory->Desc().gpuVirtAddr;
        fields.surfaceBusAddr   = memory->Desc().surfaceBusAddr;
        fields.markerBusAddr    = memory->Desc().markerBusAddr;
        fields.markerVa         = memory->GetBusAddrMarkerVa();
        result = InitBusAddressableStep(escape, gfxMajor, &fields);
        memory->SetSurfaceBusAddr(fields.surfaceBusAddr);
        memory->SetMarkerBusAddr(fields.markerBusAddr);
        memory->SetBusAddrMarkerVa(fields.markerVa);
    }
    return result;
}

// =====================================================================================================================
// Native 3816c0. The cached-CPU-map flag needs CPU visibility, no client non-local-only placement and the device
// capability. A local-only allocation that may live in GART gets a GartUswc backup heap, and later GartCacheable
// preferences become GartUswc. The original appends without a bound; a four-entry local-only list would overwrite
// the member after the heap array, so that case keeps its list.
void FinalizeHeaps(HeapFinalizeFields* fields)
{
    fields->cachedCpuMap = fields->cpuVisible && !(fields->client && fields->nonLocalOnly) &&
                           fields->cachedCpuMapSupported;
    if (fields->localOnly && (fields->cpuVisible || fields->uswcHeapAllAllocations) && !fields->pageDirectory &&
        !fields->pageTableBlock && !fields->flippable && !fields->tmzProtected && !fields->busAddressable &&
        (fields->heapCount < GpuHeapCount))
    {
        fields->localOnly = false;
        fields->heaps[fields->heapCount++] = GpuHeapGartUswc;
    }
    for (uint32 index = 1; index < fields->heapCount; ++index)
    {
        if (fields->heaps[index] == GpuHeapGartCacheable) { fields->heaps[index] = GpuHeapGartUswc; }
    }
}

// =====================================================================================================================
KernelContextRecord BuildKernelContextRecord()
{
    KernelContextRecord record = {};
    InitPrivateRecord(record.data(), record.size(), KernelContextCommand);
    PutRecord32(record.data(), 0xc, 3);
    PutRecord32(record.data(), 0x80, 0x40);
    PutRecord32(record.data(), 0xc4, 0x40);
    return record;
}

// Native 4d9f70, run after the KMT context exists (HWS Init 37b270, legacy Init 383b20); callers ignore the result.
// Reply +0xc8 is the context identifier; reply +0xd0 bit 1 -> hasDebugVmid, bit 0 -> hasHighPriorityVmid. Other
// flag bits keep their previous value.
Result RefreshKernelContextInfo(const ControlEscape& escape, uint32 hContext, KernelContextState* state)
{
    KernelContextRecord record = BuildKernelContextRecord();
    const Result result = escape(record.data(), record.size(), false, hContext);
    if (result == Result::Success)
    {
        const uint32 kmdFlags = GetRecord32(record.data(), 0xd0);
        state->info.contextIdentifier = GetRecord64(record.data(), 0xc8);
        state->info.flags.u32All = (state->info.flags.u32All & ~3u) | ((kmdFlags >> 1) & 1) | ((kmdFlags & 1) << 1);
        state->valid = true;
    }
    return result;
}

// Native queue slot 21 (4d89e0).
Result CopyKernelContextInfo(const KernelContextState& state, KernelContextInfo* pKernelContextInfo)
{
    if (pKernelContextInfo == nullptr) { return Result::ErrorInvalidPointer; }
    if (state.valid == false) { return Result::ErrorUnavailable; }
    static_assert(sizeof(KernelContextInfo) == 16, "The original copies 16 bytes");
    std::memcpy(pKernelContextInfo, &state.info, sizeof(KernelContextInfo));
    return Result::Success;
}

// Native queue slot 9 (4d8a10) record. Names are copied with the original wcscpy_s (2e9bbd0) semantics: the path is
// always copied, the file name only when present. +0x484 is Platform+0x27c8, the constant 9 the platform
// constructor 2c9c60 writes (Private::AbiVersion in this backend).
void BuildAppPowerProfileRecord(const wchar_t* pFileName, const wchar_t* pPathName, AppPowerProfileRecord* record)
{
    InitPrivateRecord(record->data(), record->size(), AppPowerProfileCommand);
    PutRecord32(record->data(), 0x484, Private::AbiVersion);
    CopyApplicationProfileName(reinterpret_cast<wchar_t*>(record->data() + 0x284), 256, pPathName,
                               WindowsApplicationProfileCopyCalls());
    if (pFileName != nullptr)
    {
        CopyApplicationProfileName(reinterpret_cast<wchar_t*>(record->data() + 0x84), 256, pFileName,
                                   WindowsApplicationProfileCopyCalls());
    }
    PutRecord32(record->data(), 0x80, 0x420);
    PutRecord32(record->data(), 0x4a4, 0x20);
}

Result SendAppPowerProfile(const ControlEscape& escape, uint32 hContext, const wchar_t* pFileName,
                           const wchar_t* pPathName)
{
    if (hContext == 0) { return Result::ErrorUnavailable; }
    AppPowerProfileRecord record;
    BuildAppPowerProfileRecord(pFileName, pPathName, &record);
    return escape(record.data(), record.size(), false, hContext);
}

// =====================================================================================================================
Result DeviceDidRsFeatureSettingsChange(const PhysicalProperties& physical, KmdSharedBuffers& buffers,
                                        uint32 rsFeatures, uint32* pRsFeaturesChanged)
{
    return DidRsFeatureSettingsChange(ReadRsInterfaceVersion(physical), &buffers, rsFeatures, pRsFeaturesChanged);
}

Result DeviceGetRsFeatureGlobalSettings(const Pal::Device& device, const Transport& transport,
                                        const PhysicalProperties& physical, const KmdSharedBuffers& buffers,
                                        RsFeatureType rsFeature, RsFeatureInfo* pRsFeatureInfo)
{
    RsTurboSyncReader turboSync;
    turboSync.context = &device;
    turboSync.read = [](const void* context, bool* pEnabled)
    {
        return static_cast<const Pal::Device*>(context)->ReadSetting("TurboSync", Util::ValueType::Boolean,
                                                                      pEnabled, PublicPalKey, 1);
    };
    // The pre-2019.1 RIS sharpness setting is not part of this tree's PalSettings: use its original default.
    const EscapeRoute route = ControlsEscapeRoute(device, transport);
    return GetRsFeatureGlobalSettings(SharedControlEscape(&route), ReadRsInterfaceVersion(physical), buffers,
                                      turboSync, DefaultRisSharpnessBits, rsFeature, pRsFeatureInfo);
}

Result DeviceUpdateChillStatus(const PhysicalProperties& physical, const KmdSharedBuffers& buffers,
                               uint64 lastChillActiveTimeStampUs)
{
    return UpdateChillStatus(ReadRsInterfaceVersion(physical), buffers, lastChillActiveTimeStampUs);
}

Result DeviceInitBusAddressableGpuMemory(const Pal::Device& device, const Transport& transport,
                                         DeviceControlsState& state, IQueue* pQueue, uint32 gpuMemCount,
                                         IGpuMemory* const* ppGpuMemList)
{
    static_cast<void>(pQueue);
    const EscapeRoute route = ControlsEscapeRoute(device, transport);
    return InitBusAddressableGpuMemory(SharedControlEscape(&route), device.ChipProperties().gfxTriple.major,
                                       &state.busAddressableLock, gpuMemCount, ppGpuMemList);
}

Result DeviceRegisterHipRuntimeState(const Pal::Device& device, const Transport& transport,
                                     const HipRuntimeSetup& runtimeState)
{
    const EscapeRoute route = ControlsEscapeRoute(device, transport);
    return RegisterHipRuntimeState(SharedControlEscape(&route), device.Settings().gfxTimeout, runtimeState);
}

Result DeviceSetHipTrapHandler(const Pal::Device& device, const Transport& transport,
                               const IGpuMemory* pTrapHandlerCode, gpusize codeOffset,
                               const IGpuMemory* pTrapHandlerMemory, gpusize memoryOffset)
{
    const EscapeRoute route = ControlsEscapeRoute(device, transport);
    return SetHipTrapHandler(SharedControlEscape(&route), HipTrapAddress(pTrapHandlerCode, codeOffset),
                             HipTrapAddress(pTrapHandlerMemory, memoryOffset));
}

void DeviceOsFinalizeSettings(const Pal::Device& device, const Transport& transport,
                              const PhysicalProperties& physical, DeviceControlsState& state)
{
    const EscapeRoute route = ControlsEscapeRoute(device, transport);
    FinalizeControlSettings(SharedControlEscape(&route), GpuCapabilities(physical), 0, &state.finalized);
}
} // namespace Pal::Wddm
