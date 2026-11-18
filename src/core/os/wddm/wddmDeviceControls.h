// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Device controls recovered from the AMD Windows PAL (amdvlk64.dll SHA-256 36a9ef32...7a44): Radeon Settings
// feature queries, Chill status, HIP debugger registration, bus-addressable/SDI memory, settings finalization and
// the queue kernel-context/app-power records. Notes: .build/pal-wddm-logic/device-controls/analysis.txt.
#pragma once

#include "pal.h"
#include "palDevice.h"
#include "palQueue.h"
#include "palMutex.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmDisplay.h"
#include <array>

namespace Pal { class Device; class IGpuMemory; }
namespace Pal::Wddm
{
class Transport;
struct PhysicalProperties;

// Shared Escape route of a PAL device, including the public disableEscapeCall flag that the original shared Escape
// reads at Device+0x8bd0 (PalPublicSettings+0xb8).
EscapeRoute ControlsEscapeRoute(const Pal::Device& device, const Transport& transport);

// The original shared Escape boundary (30f860): Result(record, bytes, hardwareAccess, hContext). Device glue wraps
// SharedEscape; CPU fixtures substitute the KMD at exactly this boundary.
struct ControlEscape
{
    void* context = nullptr;
    Result (*send)(void* context, void* record, size_t bytes, bool hardwareAccess, uint32 hContext) = nullptr;
    Result operator()(void* record, size_t bytes, bool hardwareAccess = false, uint32 hContext = 0) const
    { return send(context, record, bytes, hardwareAccess, hContext); }
};
// The route must outlive the returned object.
ControlEscape SharedControlEscape(const EscapeRoute* route);

// Native EarlyInit 30a9d0 copies GPU-record DWORDs 14856/14860 to Device+0x8af0/+0x8af4.
struct RsInterfaceVersion
{
    uint32 major = 0;
    uint32 minor = 0;
};
RsInterfaceVersion ReadRsInterfaceVersion(const PhysicalProperties& physical);
// Native threshold of 316260/316460: from 2019.1 on, features are queried by Escape.
constexpr bool RsUsesEscapeQueries(RsInterfaceVersion version)
{ return (version.major > 2019) || ((version.major == 2019) && (version.minor >= 1)); }

// =====================================================================================================================
// Radeon Settings features. The KMD pages and the last-seen counters live in the display family's KmdSharedBuffers
// (filled by InitKmdSharedBuffers, the original 30d620). rsFeatureCache[i] is Device+0x95a8+4*i.
constexpr uint32 RsFeatureCommand = 0x0300a503;
using RsFeatureRecord = std::array<uint8, 0x188>;
RsFeatureRecord BuildRsFeatureRecord(uint32 subOp);

// Native Device slot 147 (316260).
Result DidRsFeatureSettingsChange(RsInterfaceVersion version, KmdSharedBuffers* buffers,
                                  uint32 rsFeatures, uint32* pRsFeaturesChanged);
// TurboSync is the native inline ReadSetting("TurboSync", Boolean, PublicPalKey, 1 byte).
struct RsTurboSyncReader
{
    const void* context = nullptr;
    bool (*read)(const void* context, bool* pEnabled) = nullptr;
};
// Bits of the original PAL setting at settings+0x6c4 (default 0.8f), returned by the pre-2019.1 RS feature 8 query.
constexpr uint32 DefaultRisSharpnessBits = 0x3f4ccccd;
// Native Device slot 148 (316460).
Result GetRsFeatureGlobalSettings(const ControlEscape& escape, RsInterfaceVersion version,
                                  const KmdSharedBuffers& buffers, const RsTurboSyncReader& turboSync,
                                  uint32 risSharpnessBits, RsFeatureType rsFeature, RsFeatureInfo* pRsFeatureInfo);
// Native Device slot 149 (316b40).
Result UpdateChillStatus(RsInterfaceVersion version, const KmdSharedBuffers& buffers,
                         uint64 lastChillActiveTimeStampUs);

// =====================================================================================================================
// Settings finalization (native Device slot 190, 30bac0).
struct FinalizedControlSettings
{
    uint32 risState       = 0;     // Original PalPublicSettings+0xd0 (Device+0x8be8): 0 Default, 1 Enabled, 2 Disabled
    uint32 feature40State = 0;     // Original PalPublicSettings+0xd8 (Device+0x8bf0), RS feature bit 6
    bool   submitFlag     = false; // Device+0x950c, read by the original queue submit paths
};
// gpuCapabilities is GPU-record DWORD 16180 (Device+0xd314). settingOverride is the original enum setting at
// settings+0x44 (not exposed by this tree's PalSettings; original default 0).
void FinalizeControlSettings(const ControlEscape& escape, uint32 gpuCapabilities, uint32 settingOverride,
                             FinalizedControlSettings* state);

// =====================================================================================================================
// HIP runtime (native Device slots 159 and 160, Escape 0x0300e503).
constexpr uint32 HipRuntimeCommand = 0x0300e503;
using HipRecord = std::array<uint8, 0x1208>;
struct HipCalls
{
    void* userData = nullptr;
    uint32 (*processId)(void* userData) = nullptr;
    void*  (*createEvent)(void* userData) = nullptr; // CreateEventW(zeroed SA, FALSE, FALSE, L"HipRuntimeInit")
    Result (*wait)(void* userData, void* event, float seconds) = nullptr; // Original Event::Wait 54e690
    void   (*closeEvent)(void* userData, void* event) = nullptr;
};
HipRecord BuildHipRecord(uint32 processId, uint32 subOp);
Result RegisterHipRuntimeState(const ControlEscape& escape, uint32 gfxTimeoutSeconds,
                               const HipRuntimeSetup& runtimeState, const HipCalls* calls = nullptr);
// The original interface (PAL >= 974) passes the two addresses directly.
Result SetHipTrapHandler(const ControlEscape& escape, gpusize trapHandlerAddress, gpusize trapMemoryAddress,
                         const HipCalls* calls = nullptr);
// Adapter for this tree's (IGpuMemory*, offset) signature: a null object means "none" (address 0).
gpusize HipTrapAddress(const IGpuMemory* pGpuMemory, gpusize offset);

// =====================================================================================================================
// Bus-addressable and SDI external-physical memory (native 315fa0, 3160b0 and Device slot 150 30e340).
struct BusAddressReply
{
    uint64 value[3] = {};
};
using BusAddressRecord = std::array<uint8, 0x120>;
using SdiSurfaceRecord = std::array<uint8, 0x148>;
BusAddressRecord BuildBusAddressRecord(uint32 gfxMajor, uint32 operation, uint64 argument);
SdiSurfaceRecord BuildSdiSurfaceRecord(uint32 gfxMajor, gpusize size, gpusize surfaceBusAddr, gpusize markerBusAddr);
Result QueryBusAddress(const ControlEscape& escape, uint32 gfxMajor, uint32 operation, uint64 argument,
                       BusAddressReply* pReply);

// The GpuMemory state these methods read and write, copied in and out by the object.
struct SdiMemoryFields
{
    bool    externPhys     = false; // desc.flags.isExternPhys
    gpusize size           = 0;     // desc.size
    gpusize gpuVirtAddr    = 0;     // desc.gpuVirtAddr
    gpusize surfaceBusAddr = 0;     // desc.surfaceBusAddr
    gpusize markerBusAddr  = 0;     // desc.markerBusAddr
    gpusize surfaceIndex   = 0;     // remote SDI surface PTE index
    gpusize markerIndex    = 0;     // remote SDI marker PTE index
    gpusize markerVa       = 0;     // marker GPU VA
};
// Native 3160b0: register the remote surface, then translate the marker and surface bus addresses.
Result RegisterSdiRemoteSurface(const ControlEscape& escape, uint32 gfxMajor, SdiMemoryFields* fields);
// Native GpuMemory slot 6 (381d00).
Result SetSdiRemoteBusAddress(const ControlEscape& escape, uint32 gfxMajor, gpusize surfaceBusAddr,
                              gpusize markerBusAddr, SdiMemoryFields* fields);

struct BusAddressableFields
{
    bool    busAddressable   = false;
    uint32  allocationHandle = 0;   // first KMT allocation handle
    gpusize gpuVirtAddr      = 0;
    gpusize surfaceBusAddr   = 0;
    gpusize markerBusAddr    = 0;
    gpusize markerVa         = 0;
};
// One iteration of native 30e340: ErrorOutOfGpuMemory unless bus addressable, otherwise the 315fa0 operation 0.
Result InitBusAddressableStep(const ControlEscape& escape, uint32 gfxMajor, BusAddressableFields* fields);
// Native Device slot 150 (30e340) over PAL GPU memory objects.
Result InitBusAddressableGpuMemory(const ControlEscape& escape, uint32 gfxMajor, Util::Mutex* pLock,
                                   uint32 gpuMemCount, IGpuMemory* const* ppGpuMemList);

// =====================================================================================================================
// GpuMemory heap finalization (native GpuMemory slot 21, 3816c0) on an explicit view of the object's state.
struct HeapFinalizeFields
{
    bool    localOnly              = false;
    bool    nonLocalOnly           = false;
    bool    cpuVisible             = false;
    bool    client                 = false;
    bool    pageDirectory          = false;
    bool    pageTableBlock         = false;
    bool    flippable              = false;
    bool    tmzProtected           = false;
    bool    busAddressable         = false;
    bool    uswcHeapAllAllocations = false; // PalSettings::enableUswcHeapAllAllocations
    bool    cachedCpuMapSupported  = false; // Device slot 186: GPU-record DWORD 16180 bit 8
    uint32  heapCount              = 0;
    GpuHeap heaps[GpuHeapCount]    = {};
    bool    cachedCpuMap           = false; // Output: the native GpuMemory+0x158 bit 6
};
void FinalizeHeaps(HeapFinalizeFields* fields);

// =====================================================================================================================
// Queue records (native HWS/legacy/timer queue slots 9 and 21, kernel-context query 4d9f70).
struct KernelContextState
{
    KernelContextInfo info  = {};
    bool              valid = false; // Queue+0x150
};
using KernelContextRecord   = std::array<uint8, 0x108>;
using AppPowerProfileRecord = std::array<uint8, 0x4c8>;
KernelContextRecord BuildKernelContextRecord();
Result RefreshKernelContextInfo(const ControlEscape& escape, uint32 hContext, KernelContextState* state);
Result CopyKernelContextInfo(const KernelContextState& state, KernelContextInfo* pKernelContextInfo);
void BuildAppPowerProfileRecord(const wchar_t* pFileName, const wchar_t* pPathName, AppPowerProfileRecord* record);
Result SendAppPowerProfile(const ControlEscape& escape, uint32 hContext, const wchar_t* pFileName,
                           const wchar_t* pPathName);

// =====================================================================================================================
// Per-device state of this family: one Wddm::Device member (see WIRING.txt).
struct DeviceControlsState
{
    FinalizedControlSettings finalized;
    Util::Mutex              busAddressableLock; // Device+0xdcd0
};

// Device-level entry points used by Wddm::Device. They take explicit dependencies so const and non-const Device
// methods can forward without new accessors.
Result DeviceDidRsFeatureSettingsChange(const PhysicalProperties& physical, KmdSharedBuffers& buffers,
                                        uint32 rsFeatures, uint32* pRsFeaturesChanged);
Result DeviceGetRsFeatureGlobalSettings(const Pal::Device& device, const Transport& transport,
                                        const PhysicalProperties& physical, const KmdSharedBuffers& buffers,
                                        RsFeatureType rsFeature, RsFeatureInfo* pRsFeatureInfo);
Result DeviceUpdateChillStatus(const PhysicalProperties& physical, const KmdSharedBuffers& buffers,
                               uint64 lastChillActiveTimeStampUs);
Result DeviceInitBusAddressableGpuMemory(const Pal::Device& device, const Transport& transport,
                                         DeviceControlsState& state, IQueue* pQueue, uint32 gpuMemCount,
                                         IGpuMemory* const* ppGpuMemList);
Result DeviceRegisterHipRuntimeState(const Pal::Device& device, const Transport& transport,
                                     const HipRuntimeSetup& runtimeState);
Result DeviceSetHipTrapHandler(const Pal::Device& device, const Transport& transport,
                               const IGpuMemory* pTrapHandlerCode, gpusize codeOffset,
                               const IGpuMemory* pTrapHandlerMemory, gpusize memoryOffset);
void DeviceOsFinalizeSettings(const Pal::Device& device, const Transport& transport,
                              const PhysicalProperties& physical, DeviceControlsState& state);
}
