// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmPresent.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmOglPresent.h"
#include "core/os/wddm/wddmPlatform.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmQueueIdle.h"
#include "core/os/wddm/wddmResult.h"
#include "core/cmdBuffer.h"
#include "core/gpuMemory.h"
#include "core/image.h"
#include "core/hw/gfxip/gfxCmdBuffer.h"
#include "palDeveloperHooks.h"
#include "palDevice.h"
#include "palScreen.h"
#include "palQueueSemaphore.h"
#include "palSysMemory.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgiformat.h>

namespace Pal::Wddm
{
namespace
{
static_assert(offsetof(D3DKMT_PRESENT, hSource) == 0x14);
static_assert(offsetof(D3DKMT_PRESENT, hDestination) == 0x18);
static_assert(offsetof(D3DKMT_PRESENT, DstRect) == 0x20);
static_assert(offsetof(D3DKMT_PRESENT, SrcRect) == 0x30);
static_assert(offsetof(D3DKMT_PRESENT, SubRectCnt) == 0x40);
static_assert(offsetof(D3DKMT_PRESENT, pSrcSubRects) == 0x48);
static_assert(offsetof(D3DKMT_PRESENT, FlipInterval) == 0x54);
static_assert(offsetof(D3DKMT_PRESENT, Flags) == 0x58);
static_assert(offsetof(D3DKMT_PRESENT, PresentHistoryToken) == 0x168);
static_assert(offsetof(D3DKMT_PRESENT, PresentHistoryToken.Token.Blt.hPhysicalSurface) == 0x180);
static_assert(offsetof(D3DKMT_PRESENT, PresentHistoryToken.Token.Blt.EventId) == 0x188);
static_assert(offsetof(D3DKMT_PRESENT, bOptimizeForComposition) == 0x5d0);
static_assert(sizeof(D3DKMT_PRESENT) == 0x5d8);
static_assert(offsetof(D3DKMT_SUBMITPRESENTTOHWQUEUE, PrivatePresentData) == 8);
static_assert(sizeof(KmtPresentRecord::present) == sizeof(D3DKMT_PRESENT));
static_assert(offsetof(KmtPresentRecord, present) % alignof(D3DKMT_PRESENT) == 0);

constexpr HRESULT DwmGdiRedirectionSurface = 0x00263005; // DWM_S_GDI_REDIRECTION_SURFACE

bool Tracing()
{
    static const bool trace = std::getenv("PAL_WDDM_TRACE") != nullptr;
    return trace;
}

// Native Platform+0x2b68..+0x2b80, loaded from dwmapi.dll by the Platform OS setup (300ea0..300f7d).
using DwmIsCompositionEnabledFn = HRESULT (WINAPI*)(BOOL*);
using DwmGetSharedSurfaceFn     = HRESULT (WINAPI*)(HWND, LUID, HMONITOR, DWORD, DXGI_FORMAT*, HANDLE*, UINT64*);
using DwmUpdateSharedSurfaceFn  = HRESULT (WINAPI*)(HWND, UINT64, DWORD, HMONITOR, const RECT*);

struct DwmEntryPoints
{
    DwmIsCompositionEnabledFn isCompositionEnabled = nullptr; // +0x2b68
    DwmGetSharedSurfaceFn     getSharedSurface     = nullptr; // +0x2b70, ordinal 100
    DwmUpdateSharedSurfaceFn  updateSharedSurface  = nullptr; // +0x2b78, ordinal 101
};

const DwmEntryPoints& Dwm()
{
    static const DwmEntryPoints entryPoints = []
    {
        DwmEntryPoints calls = {};
        HMODULE module = LoadLibraryExA("dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module != nullptr)
        {
            calls.isCompositionEnabled =
                reinterpret_cast<DwmIsCompositionEnabledFn>(GetProcAddress(module, "DwmIsCompositionEnabled"));
            calls.getSharedSurface =
                reinterpret_cast<DwmGetSharedSurfaceFn>(GetProcAddress(module, MAKEINTRESOURCEA(100)));
            calls.updateSharedSurface =
                reinterpret_cast<DwmUpdateSharedSurfaceFn>(GetProcAddress(module, MAKEINTRESOURCEA(101)));
        }
        return calls;
    }();
    return entryPoints;
}

// 2ffe40(platform, minor): VerifyVersionInfoW(major 6, minor, service pack 0), all VER_GREATER_EQUAL.
bool IsWindows6Point(uint16 minor)
{
    OSVERSIONINFOEXW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    info.dwMajorVersion = 6;
    info.dwMinorVersion = minor;
    ULONGLONG mask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    return VerifyVersionInfoW(&info, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, mask) != FALSE;
}

uint32 Read32(const uint8* bytes, size_t offset)
{
    uint32 value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

// 2d20b4..2d20d7: first KMT allocation of the root GPU memory bound to the image (memory+0x100 chain, +0x120).
const GpuMemory* RootMemory(const IImage* image)
{
    if (image == nullptr) { return nullptr; }
    const Pal::GpuMemory* memory = static_cast<const Pal::Image*>(image)->GetBoundGpuMemory().Memory();
    while ((memory != nullptr) && (memory->OriginalGpuMem() != nullptr)) { memory = memory->OriginalGpuMem(); }
    return static_cast<const GpuMemory*>(memory);
}

uint32 AllocationOf(const IGpuMemory* memory)
{
    const Pal::GpuMemory* root = static_cast<const Pal::GpuMemory*>(memory);
    while ((root != nullptr) && (root->OriginalGpuMem() != nullptr)) { root = root->OriginalGpuMem(); }
    return (root != nullptr) ? static_cast<const GpuMemory*>(root)->OsAllocation().handle : 0;
}

D3DKMT_PRESENT* PresentOf(KmtPresentRecord* record)
{
    return reinterpret_cast<D3DKMT_PRESENT*>(record->present);
}

void FillCommonPresent(const KmtPresentSource& source, KmtPresentRecord* record)
{
    std::memset(record, 0, sizeof(*record));
    record->hHwQueue = source.hwQueue;
    D3DKMT_PRESENT* present = PresentOf(record);
    present->hContext = source.context;
    present->hWindow = static_cast<HWND>(source.window);
    present->hSource = source.source;
}

// Native Platform KMT table +0x2b48 (gdi32 D3DKMTSubmitPresentToHwQueue, Windows 10 1709 and later; resolved at run
// time like the original's table) and +0x28e8 (D3DKMTPresent).
using SubmitPresentToHwQueueFn = NTSTATUS (APIENTRY*)(D3DKMT_SUBMITPRESENTTOHWQUEUE*);

NTSTATUS SubmitKmtPresent(bool hwQueue, KmtPresentRecord* record)
{
    D3DKMT_PRESENT* present = PresentOf(record);
    if (hwQueue)
    {
        static const auto submit = reinterpret_cast<SubmitPresentToHwQueueFn>(
            GetProcAddress(GetModuleHandleA("gdi32.dll"), "D3DKMTSubmitPresentToHwQueue"));
        // The original calls through a null table entry; report the missing export instead.
        if (submit == nullptr) { return static_cast<NTSTATUS>(0xc000007a); } // STATUS_PROCEDURE_NOT_FOUND
        // 2d21a4 / 2d1c63: SUBMITPRESENTTOHWQUEUE {&hwQueue, present}. The sub-rectangle pointer still refers to the
        // caller's record.
        D3DKMT_SUBMITPRESENTTOHWQUEUE request = {};
        request.hHwQueues = &record->hHwQueue;
        request.PrivatePresentData = *present;
        return submit(&request);
    }
    return D3DKMTPresent(present);
}

LUID AdapterLuid(const Device& device)
{
    LUID luid = {};
    luid.LowPart = device.Adapter().luidLow;
    luid.HighPart = device.Adapter().luidHigh;
    return luid;
}

// Platform slot 37 (302bd0).
Result GetWindowSharedSurface(const Device& device, HWND window, bool kmtPresent, bool wait, uint64* surface,
                              uint64* updateId)
{
    const auto& dwm = Dwm();
    if (dwm.getSharedSurface == nullptr) { return Result::ErrorUnavailable; } // The original calls through null.
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HANDLE handle = nullptr;
    const DWORD flags = (kmtPresent ? DwmSharedSurfaceKmtPresent : 0u) | (wait ? DwmSharedSurfaceWait : 0u);
    const HRESULT status = dwm.getSharedSurface(window, AdapterLuid(device), nullptr, flags, &format, &handle,
                                                updateId);
    *surface = reinterpret_cast<uint64>(handle);
    if (Tracing())
    {
        std::fprintf(stderr, "[pal-wddm] dwm shared surface hr=%08lx flags=%lx format=%u handle=%llx update=%llx\n",
                     static_cast<unsigned long>(status), static_cast<unsigned long>(flags), uint32(format),
                     static_cast<unsigned long long>(*surface), static_cast<unsigned long long>(*updateId));
    }
    return (((status == S_OK) || (status == DwmGdiRedirectionSurface)) && (*updateId != 0)) ? Result::Success
                                                                                            : Result::ErrorUnknown;
}

// Platform slot 36 (302b90).
Result UpdateWindowSharedSurface(HWND window, uint64 updateId, const RECT& rect)
{
    const auto& dwm = Dwm();
    if (dwm.updateSharedSurface == nullptr) { return Result::ErrorUnavailable; }
    const HRESULT status = dwm.updateSharedSurface(window, updateId, 0, nullptr, &rect);
    if (Tracing())
    {
        std::fprintf(stderr, "[pal-wddm] dwm update hr=%08lx update=%llx rect=%ld,%ld,%ld,%ld\n",
                     static_cast<unsigned long>(status), static_cast<unsigned long long>(updateId), rect.left, rect.top,
                     rect.right, rect.bottom);
    }
    return (status == S_OK) ? Result::Success : Result::ErrorUnknown;
}

int64 QueryCounter()
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

// =====================================================================================================================
// KMT windowed presenter: vtable 6562870 (dtor 2d1060, Present 2d1ab0), 1064 bytes, created by 2d08d0.
class WindowedPresenter final : public Presenter
{
public:
    WindowedPresenter(Device* device, Queue* queue)
        :
        Presenter(device, queue, true),
        m_redirectedBltSupported(device->RedirectedBltSupported()),
        m_presentViaOglRuntime(device->Settings().presentViaOglRuntime)
    {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        m_frequency = frequency.QuadPart;
        InitOglPresenterState(&m_ogl, this, &OglPresentThunk);
    }
    ~WindowedPresenter() override;
    Result Init(QueueType queueType, EngineType engineType);
    Result Present(const PresentDirectInfo& info) override;
    Result WaitForPresents(uint32 gfxTimeoutSeconds) override;

private:
    // One DWM surface opened as a PAL image (40-byte value of the map at +0x58 and of the deque at +0x290).
    struct Surface
    {
        uint32      handle  = 0;
        IImage*     image   = nullptr;
        IGpuMemory* memory  = nullptr;
        uint64      value   = 0; // presenter semaphore value of the last present that used it
        int64       stamp   = 0; // QueryPerformanceCounter of the last use
        void*       storage = nullptr;
    };
    struct CacheEntry
    {
        uint64  key;
        Surface surface;
    };

    void   Retire();                                         // 2d2c50
    void   DestroySurface(const Surface& surface);           // 2d2c50 body
    void   Release(const Surface& surface);                  // 2c00c0 onto +0x290
    Result OpenSharedSurface(uint32 handle);                 // 2d23e0
    Result OpenSurfaceObjects(uint32 handle, Surface* surface);
    Result PresentThroughOglRuntime(const PresentDirectInfo& info); // 2d21e0
    static bool OglPresentThunk(const OglPresentBuffers* data);    // 2b41e0
    struct OglPresentOs;
    struct OglCallbackOs;
    Result PresentCopy(const PresentDirectInfo& info);              // 2d1d30
    Result PresentRedirected(const PresentDirectInfo& info);        // 2d1fa0
    Result PresentBlt(const PresentDirectInfo& info);               // 2d1ab0 without composition
    KmtPresentSource Source(const PresentDirectInfo& info) const;

    uint32      m_lastHandle   = 0;       // +0x30
    IImage*     m_image        = nullptr; // +0x20
    IGpuMemory* m_memory       = nullptr; // +0x28
    uint64      m_updateId     = 0;       // +0x38
    IQueueSemaphore* m_semaphore = nullptr; // +0x40, timeline
    void*       m_semaphoreMemory = nullptr;
    uint64      m_semaphoreValue = 0;     // +0x48
    uint64      m_signaledValue = 0;      // not in the original: last +0x48 value whose signal was queued successfully
    int64       m_frequency    = 1;       // +0x50
    std::vector<CacheEntry> m_cache;      // +0x58 hash map keyed by +0x418 (m_ogl.window)
    std::vector<Surface> m_retired;       // +0x290 deque
    CmdBuffer*  m_cmdBuffer    = nullptr; // +0x2d0
    bool        m_redirectedBltSupported; // +0x2d8 = Device+0x95f0
    bool        m_presentViaOglRuntime;   // +0x2d9 = PalSettings+0xa63
    OglPresenterState m_ogl;              // +0x2e0..+0x428
};

// =====================================================================================================================
// Fullscreen flip presenter: vtable 6576d50 (dtor 2c8cf0, Present 51aca0), 32 bytes.
class FlipPresenter final : public Presenter
{
public:
    FlipPresenter(Device* device, Queue* queue) : Presenter(device, queue, true) {}
    Result Present(const PresentDirectInfo& info) override;
};

// 51acd0..51acec: the image's fullscreen screen (image+0x268, set from PresentableImageCreateInfo::pScreen by
// presentable creation 381dd0) must belong to this presenter's Device. This backend's presentable images are
// never fullscreen (CreatePresentableImage refuses flags.fullscreen), so no image has an owning screen.
const Device* FullscreenOwner(const IImage* image)
{
    static_cast<void>(image);
    return nullptr;
}
} // anonymous namespace

// =====================================================================================================================
void ComputePresenterMasks(const PresenterSelectionInputs& in, uint32 masks[PresenterMaskCount])
{
    for (uint32 type = 0; type < PresenterMaskCount; ++type) { masks[type] = 0; }
    const bool firstAdapter = (in.physicalAdapter == 0);
    const auto gdi = [masks]
    {
        // 30d30d
        masks[0] |= PresenterGdi;
        masks[1] = PresenterGdi;
    };
    if (firstAdapter && (((in.adapterFlags & AdapterActiveDisplay) != 0) || in.noDisplayAccess) &&
        !in.forcePresentViaCpuBlt)
    {
        // 30d23f..30d277
        masks[0] |= PresenterWindowed | PresenterWindowedPriorBlt;
        masks[1] |= PresenterWindowedPriorBlt;
        masks[2] |= PresenterWindowedPriorBlt;
        if (in.compositionEnabled && !in.redirectedBltSupported) { masks[1] |= PresenterWindowed; } // 30d5db
    }
    else if (((in.adapterFlags & AdapterHybridDiscrete) != 0) || in.allAdaptersAmdSingle)
    {
        // 30d5ba
        if (in.forcePresentViaCpuBlt) { gdi(); }
        else
        {
            masks[0] |= PresenterWindowed;
            if (!in.redirectedBltSupported) { masks[1] |= PresenterWindowed; }
        }
    }
    else
    {
        gdi();
    }
    // 30d31e
    if (!in.forcePresentViaCpuBlt && !in.noDisplayAccess && firstAdapter)
    {
        masks[0] |= PresenterFullscreen;
        masks[1] |= PresenterFullscreen;
        masks[2] |= PresenterFullscreen;
    }
}

uint32 DirectPresentModes(const uint32 masks[PresenterMaskCount], uint32 queueType, bool forcePresentViaCpuBlt,
                          uint32 risState)
{
    // 30d370..30d4c4: with Device+0x8be8 == 1 only queue types 0 and 1 are filled.
    if ((queueType >= PresenterMaskCount) || ((risState == 1) && (queueType > 1))) { return 0; }
    const uint32 mask = masks[queueType];
    uint32 modes = 0;
    if ((mask & (PresenterWindowed | PresenterGdi)) != 0) { modes = SupportWindowedPresent; }
    if ((mask & PresenterWindowedPriorBlt) != 0) { modes |= SupportWindowedPriorBlitPresent; }
    if (((mask & PresenterFullscreen) != 0) || forcePresentViaCpuBlt) { modes |= SupportFullscreenPresent; }
    return modes;
}

uint32 SupportedSwapChainModes(uint32 universalDirectPresentModes, PresentMode mode)
{
    // 313450: windowed -> Immediate|Fifo when windowed presents are supported, anything else -> Immediate|Fifo|
    // FifoRelaxed when fullscreen presents are supported.
    if (mode == PresentMode::Windowed)
    {
        return ((universalDirectPresentModes & SupportWindowedPresent) != 0)
               ? (SupportImmediateSwapChain | SupportFifoSwapChain) : 0;
    }
    return ((universalDirectPresentModes & SupportFullscreenPresent) != 0)
           ? (SupportImmediateSwapChain | SupportFifoSwapChain | SupportFifoRelaxedSwapChain) : 0;
}

uint32 FormatHdrMode(uint32 colorState, ChNumFormat format)
{
    // 36adc0..36ae1d
    if (((colorState & (CsBt2020 | TfPq2084)) == (CsBt2020 | TfPq2084)) && (format == ChNumFormat::X10Y10Z10W2_Unorm))
    {
        return static_cast<uint32>(HdrDisplayMode::Hdr10);
    }
    if (((colorState & (CsScrgb | TfLinear0_125)) == (CsScrgb | TfLinear0_125)) &&
        (format == ChNumFormat::X16Y16Z16W16_Float))
    {
        return static_cast<uint32>(HdrDisplayMode::ScRgb);
    }
    if (((colorState & (CsNative | TfGamma22)) == (CsNative | TfGamma22)) && (format == ChNumFormat::X10Y10Z10W2_Unorm))
    {
        return static_cast<uint32>(HdrDisplayMode::FreeSync2);
    }
    return static_cast<uint32>(HdrDisplayMode::Sdr);
}

bool IsCompositionEnabled()
{
    if (IsWindows6Point(2)) { return true; }
    BOOL enabled = FALSE;
    if (Dwm().isCompositionEnabled != nullptr) { Dwm().isCompositionEnabled(&enabled); } // HRESULT ignored (302b7a)
    return enabled != FALSE;
}

bool QueryRedirectedBltSupport(uint32 kmtAdapter, uint32 recordVersion)
{
    // 302cb0: QueryAdapterInfo {adapter, KMTQAITYPE_DRIVERVERSION, 4 bytes}; signed comparisons.
    D3DKMT_DRIVERVERSION version = {};
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = kmtAdapter;
    query.Type = KMTQAITYPE_DRIVERVERSION;
    query.pPrivateDriverData = &version;
    query.PrivateDriverDataSize = sizeof(version);
    if (ConvertNativeStatus(D3DKMTQueryAdapterInfo(&query)) != Result::Success) { return false; }
    return RedirectedBltFromVersions(recordVersion, static_cast<uint32>(version));
}

bool RedirectedBltFromVersions(uint32 recordVersion, uint32 driverVersion)
{
    const int32 driver = static_cast<int32>(driverVersion);
    if ((static_cast<int32>(recordVersion) >= 0x2200) && (driver >= 0x4b0)) { return true; }
    return driver >= 0x834;
}

uint32 QueryAdapterPresentFlags(uint32 kmtAdapter, uint32 vendorId)
{
    bool known = false;
    D3DKMT_ADAPTERTYPE type = {};
    if (IsWindows6Point(2))
    {
        // 3001ce..300212: KMTQAITYPE_ADAPTERTYPE (Windows 8 and later). The original fails adapter creation when the
        // query fails; the adapter already exists here, so the flag stays clear instead.
        D3DKMT_QUERYADAPTERINFO query = {};
        query.hAdapter = kmtAdapter;
        query.Type = KMTQAITYPE_ADAPTERTYPE;
        query.pPrivateDriverData = &type;
        query.PrivateDriverDataSize = sizeof(type);
        known = (ConvertNativeStatus(D3DKMTQueryAdapterInfo(&query)) == Result::Success);
    }
    return AdapterPresentFlagsFrom(known, type.Value, vendorId);
}

uint32 AdapterPresentFlagsFrom(bool adapterTypeKnown, uint32 adapterType, uint32 vendorId)
{
    // 300206: bit0 = HybridDiscrete (bit 4 of D3DKMT_ADAPTERTYPE); 300219..300234: bit1 for vendor 0x1002/0x1022
    // unless hybrid discrete.
    uint32 flags = adapterTypeKnown ? ((adapterType >> 4) & AdapterHybridDiscrete) : 0;
    if (((flags & AdapterHybridDiscrete) == 0) && (((vendorId - 0x1002u) & 0xffdfu) == 0))
    {
        flags |= AdapterAmdIntegrated;
    }
    return flags;
}

uint32 PresenterSlotFor(PresentMode mode, bool forcePresentViaCpuBlt)
{
    return ((mode == PresentMode::Fullscreen) && !forcePresentViaCpuBlt) ? 1 : 0;
}

bool AllAdaptersAmdSingle(const Platform& platform)
{
    // 2ccd90: Platform adapter objects +0x2738[+0x27b8]; record DWORD 1 (adapter+0xc) == 1 and flag bit1.
    for (uint32 index = 0; index < platform.GetDeviceCount(); ++index)
    {
        const auto* device = static_cast<const Device*>(platform.GetDevice(index));
        if ((Read32(device->Adapter().physical.basicRecord.data(), 4) != 1) ||
            ((device->AdapterPresentFlags() & AdapterAmdIntegrated) == 0))
        {
            return false;
        }
    }
    return true;
}

void ReportPresentConcluded(const Device& device, uint32 presentModeType, OsWindowHandle window)
{
    Developer::PresentationModeData data = {};
    data.presentationMode = static_cast<Developer::PresentModeType>(presentModeType);
    data.presentKey = PresentKeyFromOsWindowHandle(window);
    device.DeveloperCb(Developer::CallbackType::PresentConcluded, &data);
}

void ClientRectInWindow(const void* windowInfo, int32 rect[4])
{
    // 2d1e02..2d1e29: client rectangle relative to the window rectangle's origin.
    const auto* info = static_cast<const WINDOWINFO*>(windowInfo);
    rect[0] = info->rcClient.left - info->rcWindow.left;
    rect[1] = info->rcClient.top - info->rcWindow.top;
    rect[2] = info->rcClient.right - info->rcWindow.left;
    rect[3] = info->rcClient.bottom - info->rcWindow.top;
}

void BuildBltPresent(const KmtPresentSource& source, KmtPresentRecord* record)
{
    // 2d1b86..2d1c93 (and the fallback presenter 51b8f0): blit the whole image to the window.
    FillCommonPresent(source, record);
    D3DKMT_PRESENT* present = PresentOf(record);
    const RECT rect = { 0, 0, static_cast<LONG>(source.width), static_cast<LONG>(source.height) };
    present->DstRect = rect;
    present->SrcRect = rect;
    present->SubRectCnt = 1;
    present->pSrcSubRects = &present->SrcRect; // a stack copy of the same rectangle in the original
    present->Flags.Value |= KmtPresentBltFlags;
}

void BuildRedirectedBltPresent(const KmtPresentSource& source, uint32 destination, uint64 dwmSurface,
                               uint64 updateId, KmtPresentRecord* record)
{
    // 2d2058..2d217b
    FillCommonPresent(source, record);
    D3DKMT_PRESENT* present = PresentOf(record);
    present->hDestination = destination;
    present->SrcRect = { 0, 0, static_cast<LONG>(source.width), static_cast<LONG>(source.height) };
    present->SubRectCnt = 1;
    present->pSrcSubRects = &present->SrcRect;
    present->Flags.Value |= KmtPresentRedirectedFlags;
    present->PresentHistoryToken.Model = static_cast<D3DKMT_PRESENT_MODEL>(KmtRedirectedBltModel);
    present->PresentHistoryToken.Token.Blt.hPhysicalSurface = dwmSurface;
    present->PresentHistoryToken.Token.Blt.EventId = updateId;
    present->bOptimizeForComposition = TRUE;
}

void BuildFlipPresent(const KmtPresentSource& source, uint32 flags, uint32 presentInterval, KmtPresentRecord* record)
{
    // 51ace6..51ad8c
    FillCommonPresent(source, record);
    D3DKMT_PRESENT* present = PresentOf(record);
    present->Flags.Value = KmtPresentFlipFlag | ((flags << 4) & 0x10);
    const int32 interval = static_cast<int32>(presentInterval);
    present->FlipInterval = static_cast<D3DDDI_FLIPINTERVAL_TYPE>((interval > 4) ? 4 : interval);
}

// =====================================================================================================================
Presenter::Presenter(Device* device, Queue* queue, bool kmt)
    :
    m_device(device),
    m_queue(queue),
    m_hwQueue(queue->NativeQueue().handle != 0), // queue slot 42 (37a020) != 0
    m_kmt(kmt)
{
}

KmtPresentSource WindowedPresenter::Source(const PresentDirectInfo& info) const
{
    KmtPresentSource source = {};
    source.context = m_queue->NativeQueue().context;
    source.hwQueue = m_hwQueue ? m_queue->NativeQueue().handle : 0;
    source.window = info.hWindow;
    const GpuMemory* memory = RootMemory(info.pSrcImage);
    source.source = (memory != nullptr) ? memory->OsAllocation().handle : 0;
    if (info.pSrcImage != nullptr)
    {
        source.width = info.pSrcImage->GetImageCreateInfo().extent.width;
        source.height = info.pSrcImage->GetImageCreateInfo().extent.height;
    }
    return source;
}

Result WindowedPresenter::Init(QueueType queueType, EngineType engineType)
{
    // 2d1390: an internal command buffer for the queue's engine (+0x2d0) and a timeline semaphore (+0x40).
    CmdBufferCreateInfo createInfo = {};
    createInfo.pCmdAllocator = m_device->InternalUntrackedCmdAllocator();
    createInfo.queueType = queueType;
    createInfo.engineType = engineType;
    CmdBufferInternalCreateInfo internalInfo = {};
    internalInfo.flags.isInternal = 1;
    Result result = (createInfo.pCmdAllocator != nullptr)
                  ? m_device->CreateInternalCmdBuffer(createInfo, internalInfo, &m_cmdBuffer)
                  : Result::ErrorInitializationFailed;
    if (result == Result::Success)
    {
        QueueSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.flags.timeline = 1;
        const size_t size = m_device->GetQueueSemaphoreSize(semaphoreInfo, &result);
        if (result == Result::Success)
        {
            m_semaphoreMemory = PAL_MALLOC(size, m_device->GetPlatform(), Util::AllocInternal);
            result = (m_semaphoreMemory != nullptr)
                   ? m_device->CreateQueueSemaphore(semaphoreInfo, m_semaphoreMemory, &m_semaphore)
                   : Result::ErrorOutOfMemory;
        }
    }
    return result;
}

WindowedPresenter::~WindowedPresenter()
{
    // 2d1090: the internal command buffer; every cached surface moves to the retire list (2c00c0) and the current
    // pointers are cleared; 2d2c50 frees the retired surfaces whose last present completed and stops at the first one
    // that has not; the semaphore is destroyed. A surface a queued present can still write is never destroyed or
    // unmapped: it is left to the device's destruction, as in the original. Queue::OsDestroy has waited for this
    // presenter's presents first (WaitForPresents), so normally every surface is freed here.
    if (m_cmdBuffer != nullptr)
    {
        m_cmdBuffer->DestroyInternal();
        m_cmdBuffer = nullptr;
    }
    for (const CacheEntry& entry : m_cache) { m_retired.push_back(entry.surface); }
    m_cache.clear();
    m_image = nullptr;
    m_memory = nullptr;
    if (!m_retired.empty()) { Retire(); }
    if (Tracing() && !m_retired.empty())
    {
        std::fprintf(stderr, "[pal-wddm] presenter destroyed with %zu surface(s) still used by queued presents (kept)\n",
                     m_retired.size());
    }
    m_retired.clear();
    if (m_semaphore != nullptr) { m_semaphore->Destroy(); }
    PAL_SAFE_FREE(m_semaphoreMemory, m_device->GetPlatform());
}

void WindowedPresenter::DestroySurface(const Surface& surface)
{
    // 2d2cb6..2d2cf8: residency reference, memory, then the image and the block that holds both.
    if (Tracing())
    {
        std::fprintf(stderr, "[pal-wddm] dwm surface destroy handle=%x va=%llx value=%llu\n", surface.handle,
                     static_cast<unsigned long long>((surface.memory != nullptr) ? surface.memory->Desc().gpuVirtAddr : 0),
                     static_cast<unsigned long long>(surface.value));
    }
    if (surface.memory != nullptr)
    {
        IGpuMemory* memory = surface.memory;
        m_device->RemoveGpuMemoryReferences(1, &memory, m_queue);
        memory->Destroy();
    }
    if (surface.image != nullptr) { surface.image->Destroy(); }
    PAL_FREE(surface.storage, m_device->GetPlatform());
}

void WindowedPresenter::Release(const Surface& surface)
{
    // Deviation: the original keeps +0x20/+0x28 pointing at an entry it has just retired when that entry
    // was evicted for age; drop the stale pointers instead.
    if ((surface.image != nullptr) && (surface.image == m_image)) { m_image = nullptr; }
    if ((surface.memory != nullptr) && (surface.memory == m_memory)) { m_memory = nullptr; }
    m_retired.push_back(surface);
}

void WindowedPresenter::Retire()
{
    // 2d2c50: free retired surfaces whose last present completed, in order, stopping at the first that has not.
    // An entry's value is +0x48 when a present last used it (2d25ee, 2d2bea), and that present signals value + 1
    // after it (2d1b6f). The original frees at completed >= value, i.e. possibly before that present completed; a
    // queued present could then write a freed and unmapped surface, so this port requires completed > value.
    if (m_retired.empty()) { return; }
    uint64 completed = 0;
    m_semaphore->QuerySemaphoreValue(&completed);
    size_t count = 0;
    while ((count < m_retired.size()) && (completed > m_retired[count].value))
    {
        DestroySurface(m_retired[count]);
        ++count;
    }
    m_retired.erase(m_retired.begin(), m_retired.begin() + count);
}

Result WindowedPresenter::OpenSurfaceObjects(uint32 handle, Surface* surface)
{
    // 2d26a0..2d2c00: query (3120c0), create info (3833c0), sizes, one Platform block, open (3826a0).
    ExternalImageOpenInfo openInfo = {};
    openInfo.resourceInfo.hExternalResource = reinterpret_cast<OsExternalHandle>(static_cast<uintptr_t>(handle));
    openInfo.flags.flippable = 1;
    size_t imageSize = 0;
    size_t memorySize = 0;
    Result result = m_device->GetExternalSharedImageSizes(openInfo, &imageSize, &memorySize, nullptr);
    void* storage = nullptr;
    if (result == Result::Success)
    {
        storage = PAL_MALLOC(imageSize + memorySize, m_device->GetPlatform(), Util::AllocInternal);
        result = (storage != nullptr) ? Result::Success : Result::ErrorOutOfMemory;
    }
    IImage* image = nullptr;
    IGpuMemory* memory = nullptr;
    if (result == Result::Success)
    {
        result = m_device->OpenExternalSharedImage(openInfo, storage, Util::VoidPtrInc(storage, imageSize), nullptr,
                                                   &image, &memory);
    }
    if (Tracing())
    {
        ImageCreateInfo info = {};
        if (image != nullptr) { info = image->GetImageCreateInfo(); }
        std::fprintf(stderr, "[pal-wddm] dwm surface open handle=%x result=%d format=%u extent=%ux%u tiling=%u flags=%x "
                     "usage=%x metadata=%u memory=%llx va=%llx\n", handle, int(result),
                     uint32(info.swizzledFormat.format), info.extent.width, info.extent.height, uint32(info.tiling),
                     info.flags.u32All, info.usageFlags.u32All, uint32(info.metadataMode),
                     static_cast<unsigned long long>((memory != nullptr) ? memory->Desc().size : 0),
                     static_cast<unsigned long long>((memory != nullptr) ? memory->Desc().gpuVirtAddr : 0));
    }
    if (result != Result::Success)
    {
        PAL_FREE(storage, m_device->GetPlatform());
        return result;
    }
    surface->handle = handle;
    surface->image = image;
    surface->memory = memory;
    surface->storage = storage;
    return result;
}

Result WindowedPresenter::OpenSharedSurface(uint32 handle)
{
    // 2d23e0
    if (handle == m_lastHandle)
    {
        // 2d2411..2d2414 returns at once and leaves the entry's value and stamp at an earlier use; a later
        // retirement (handle change, age) could then free a surface a queued present still writes. This port
        // records the present on the current entry first (value and stamp as 2d25ee..2d261b would).
        for (CacheEntry& entry : m_cache)
        {
            if ((entry.key == reinterpret_cast<uint64>(m_ogl.window)) && (entry.surface.handle == handle))
            {
                entry.surface.value = m_semaphoreValue;
                entry.surface.stamp = QueryCounter();
                break;
            }
        }
        return Result::Success;
    }
    const int64 now = QueryCounter();
    // 2d24a0..2d2528: entries with a non-zero key idle for ten seconds are retired.
    for (size_t index = 0; index < m_cache.size();)
    {
        if ((m_cache[index].key != 0) && ((now - m_cache[index].surface.stamp) >= (10 * m_frequency)))
        {
            Release(m_cache[index].surface);
            m_cache.erase(m_cache.begin() + index);
        }
        else { ++index; }
    }
    CacheEntry* entry = nullptr;
    bool found = false;
    for (CacheEntry& candidate : m_cache)
    {
        if (candidate.key == reinterpret_cast<uint64>(m_ogl.window)) { entry = &candidate; found = true; break; }
    }
    if (entry == nullptr)
    {
        m_cache.push_back({ reinterpret_cast<uint64>(m_ogl.window), {} });
        entry = &m_cache.back();
    }
    Result result = Result::Success;
    if (found)
    {
        if (handle == entry->surface.handle)
        {
            // 2d25ee..2d261b
            entry->surface.value = m_semaphoreValue;
            entry->surface.stamp = now;
            m_lastHandle = handle;
            m_memory = entry->surface.memory;
            m_image = entry->surface.image;
        }
        else
        {
            Release(entry->surface);
            m_lastHandle = 0;
            m_image = nullptr;
            m_memory = nullptr;
            entry->surface = {};
        }
        if (m_lastHandle != 0) { return result; }
    }
    if (handle != 0)
    {
        Surface surface = {};
        result = OpenSurfaceObjects(handle, &surface);
        if (result == Result::Success)
        {
            surface.value = m_semaphoreValue;
            surface.stamp = now;
            entry->surface = surface;
            m_image = surface.image;
            m_memory = surface.memory;
            // 2d2c0b: AddGpuMemoryReferences(1, {memory}, queue, CantTrim).
            GpuMemoryRef reference = {};
            reference.pGpuMemory = surface.memory;
            result = m_device->AddGpuMemoryReferences(1, &reference, m_queue, GpuMemoryRefCantTrim);
        }
    }
    m_lastHandle = handle; // 2d2c32: also after a failed open
    return result;
}

Result WindowedPresenter::PresentCopy(const PresentDirectInfo& info)
{
    // 2d1d30: copy into the window's DWM redirection surface, then publish it.
    const HWND window = static_cast<HWND>(info.hWindow);
    uint64 surface = 0;
    Result result = GetWindowSharedSurface(*m_device, window, false, info.presentInterval > 0, &surface, &m_updateId);
    if (result != Result::Success) { return result; }
    WINDOWINFO windowInfo = {}; // cbSize stays 0, as in the original.
    int32 rect[4] = {};
    if (GetWindowInfo(window, &windowInfo) != FALSE) { ClientRectInWindow(&windowInfo, rect); }
    result = OpenSharedSurface(static_cast<uint32>(surface));
    if (m_image == nullptr) { return Result::ErrorUnknown; }
    if (result != Result::Success) { return result; }
    // The original records the copy without a source check; a null source would fault.
    if (info.pSrcImage == nullptr) { return Result::ErrorInvalidPointer; }
    // GfxCmdBuffer slot 169 (CmdPresentBlt); the DMA command buffer's slot is not reconstructed.
    if ((m_queue->GetEngineType() != EngineTypeUniversal) && (m_queue->GetEngineType() != EngineTypeCompute))
    {
        return Result::Unsupported;
    }
    CmdBufferBuildInfo buildInfo = {};
    buildInfo.flags.optimizeOneTimeSubmit = 1;
    result = m_cmdBuffer->Begin(buildInfo);
    if (result != Result::Success) { return result; }
    const Offset3d offset = { rect[0], rect[1], 0 };
    static_cast<GfxCmdBuffer*>(m_cmdBuffer)->CmdPresentBlt(*info.pSrcImage, *m_image, offset);
    result = m_cmdBuffer->End();
    if (result != Result::Success) { return result; }
    // 2d1eeb: queue+0x190 / +0x18c, consumed by a legacy-context submit as PresentRedirected + history token.
    m_queue->NativeQueue().redirectedPresentToken = m_updateId;
    m_queue->NativeQueue().redirectedPresentPending = true;
    ICmdBuffer* cmdBuffer = m_cmdBuffer;
    PerSubQueueSubmitInfo perSubQueue = {};
    perSubQueue.cmdBufferCount = 1;
    perSubQueue.ppCmdBuffers = &cmdBuffer;
    MultiSubmitInfo submitInfo = {};
    submitInfo.perSubQueueInfoCount = 1;
    submitInfo.pPerSubQueueInfo = &perSubQueue;
    result = m_queue->SubmitInternal(submitInfo, true);
    if (result != Result::Success) { return result; }
    const RECT client = { rect[0], rect[1], rect[2], rect[3] };
    return UpdateWindowSharedSurface(window, m_updateId, client);
}

Result WindowedPresenter::PresentRedirected(const PresentDirectInfo& info)
{
    // 2d1fa0: let the KMD blit into the window's DWM redirection surface.
    uint64 surface = 0;
    Result result = GetWindowSharedSurface(*m_device, static_cast<HWND>(info.hWindow), true,
                                           info.presentInterval > 0, &surface, &m_updateId);
    if (result == Result::Success) { result = OpenSharedSurface(static_cast<uint32>(surface)); }
    if (result != Result::Success) { return result; }
    // The original dereferences the source image and the opened surface memory without checks.
    if (info.pSrcImage == nullptr) { return Result::ErrorInvalidPointer; }
    if (m_memory == nullptr) { return Result::ErrorUnknown; }
    KmtPresentRecord record;
    BuildRedirectedBltPresent(Source(info), AllocationOf(m_memory), surface, m_updateId, &record);
    return ConvertNativeStatus(SubmitKmtPresent(m_hwQueue, &record));
}

// What 2d21e0 reaches outside the shared logic of wddmOglPresent.h.
struct WindowedPresenter::OglPresentOs
{
    WindowedPresenter&       presenter;
    const PresentDirectInfo& info;
    Platform*                platform;

    bool IsWindow(void* window) const { return ::IsWindow(static_cast<HWND>(window)) != FALSE; }
    void LoadRuntime() { platform->LoadOglRuntime(*presenter.m_device); }      // Platform slot 39
    OglRuntimeCallbacks* Callbacks() { return platform->OglCallbacks(); }      // Platform slot 41
    void* GetDc(void* window) const { return ::GetDC(static_cast<HWND>(window)); }
    void ReleaseDc(void* window, void* dc) const { ::ReleaseDC(static_cast<HWND>(window), static_cast<HDC>(dc)); }
    // 2d22d2..2d22ef: no opengl32 PresentBuffers callback.
    Result Fallback(bool hwQueue) { return hwQueue ? presenter.PresentRedirected(info) : presenter.PresentCopy(info); }
};

// What 2b3f40 reaches outside the shared logic of wddmOglPresent.h.
struct WindowedPresenter::OglCallbackOs
{
    WindowedPresenter& presenter;
    Platform*          platform;

    Result OpenSurface(uint32 handle) { return presenter.OpenSharedSurface(handle); }          // 2d23e0
    uint32 HwQueueHandle() const { return presenter.m_queue->NativeQueue().handle; }          // queue slot 42
    uint32 Context() const { return presenter.m_queue->NativeQueue().context; }               // *(queue+0x228)
    bool GetClientRect(void* window, int32 rect[4]) const
    {
        RECT client = {};
        const bool ok = ::GetClientRect(static_cast<HWND>(window), &client) != FALSE;
        rect[0] = client.left;
        rect[1] = client.top;
        rect[2] = client.right;
        rect[3] = client.bottom;
        return ok;
    }
    void ImageExtent(const IImage* image, uint32* width, uint32* height) const
    {
        // image+0x1c/+0x20; the original reads them without a null check.
        if (image != nullptr)
        {
            *width = image->GetImageCreateInfo().extent.width;
            *height = image->GetImageCreateInfo().extent.height;
        }
    }
    OglRuntimeCallbacks* Callbacks() { return platform->OglCallbacks(); }                     // Platform slot 40
    // image+0x190 -> root memory (+0x100 chain) -> first allocation (+0x120); the original dereferences the image.
    uint32 SourceAllocation(const IImage* image) const
    {
        const GpuMemory* memory = RootMemory(image);
        return (memory != nullptr) ? memory->OsAllocation().handle : 0;
    }
    uint32 DestinationAllocation() const { return AllocationOf(presenter.m_memory); }         // presenter+0x28
};

Result WindowedPresenter::PresentThroughOglRuntime(const PresentDirectInfo& info)
{
    // 2d21e0
    OglPresentOs os = { *this, info, static_cast<Platform*>(m_device->GetPlatform()) };
    const uint64 luid = (uint64(uint32(m_device->Adapter().luidHigh)) << 32) | m_device->Adapter().luidLow;
    const Result result = PresentViaOglRuntime(os, &m_ogl, info, luid, m_hwQueue);
    if (Tracing())
    {
        const OglRuntimeCallbacks* callbacks = os.Callbacks();
        std::fprintf(stderr, "[pal-wddm] ogl present window=%p icd=%u v2=%u presentBuffers=%u result=%d\n",
                     info.hWindow, (callbacks->getCallbackProcs != nullptr) || (callbacks->getCallbackProcsV2 != nullptr),
                     callbacks->getCallbackProcsV2 != nullptr, callbacks->presentBuffers != nullptr, int(result));
    }
    return result;
}

bool WindowedPresenter::OglPresentThunk(const OglPresentBuffers* data)
{
    // 2b41e0: the presenter is the V2 block's +0x08. The original reads through a null record.
    if ((data == nullptr) || (data->privateData == nullptr)) { return false; }
    auto* presenter = static_cast<WindowedPresenter*>(static_cast<const OglCallbackBlockV2*>(data->privateData)->presenter);
    OglCallbackOs os = { *presenter, static_cast<Platform*>(presenter->m_device->GetPlatform()) };
    const bool opened = OglPresentCallback(os, &presenter->m_ogl, *data);
    if (Tracing())
    {
        std::fprintf(stderr, "[pal-wddm] ogl callback surface=%llx token=%llx hwQueue=%x opened=%u\n",
                     static_cast<unsigned long long>(data->surface), static_cast<unsigned long long>(data->presentToken),
                     os.HwQueueHandle(), opened);
    }
    return opened;
}

Result WindowedPresenter::PresentBlt(const PresentDirectInfo& info)
{
    // 2d1b86..2d1c9a: no composition, D3DKMT Blt straight to the window.
    if (info.pSrcImage == nullptr) { return Result::ErrorInvalidPointer; } // the original dereferences it
    KmtPresentRecord record;
    BuildBltPresent(Source(info), &record);
    return ConvertNativeStatus(SubmitKmtPresent(m_hwQueue, &record));
}

Result WindowedPresenter::Present(const PresentDirectInfo& info)
{
    // 2d1ab0
    Result result = Result::Success;
    if (IsCompositionEnabled())
    {
        Retire();
        if (m_presentViaOglRuntime)
        {
            result = PresentThroughOglRuntime(info);
        }
        else if (!m_redirectedBltSupported || ((m_device->AdapterPresentFlags() & AdapterHybridDiscrete) != 0) ||
                 AllAdaptersAmdSingle(*static_cast<const Platform*>(m_device->GetPlatform())))
        {
            result = PresentCopy(info);
        }
        else
        {
            result = PresentRedirected(info);
        }
        // 2d1b56: the presenter semaphore advances after every present; its result is not checked by the original.
        // The value is kept for WaitForPresents only when the signal was queued.
        const uint64 value = ++m_semaphoreValue;
        if (m_queue->SignalQueueSemaphore(m_semaphore, value) == Result::Success) { m_signaledValue = value; }
    }
    else
    {
        result = PresentBlt(info);
    }
    ReportPresentConcluded(*m_device, static_cast<uint32>(Developer::PresentModeType::Blit), info.hWindow);
    return result;
}

Result WindowedPresenter::WaitForPresents(uint32 gfxTimeoutSeconds)
{
    // Every present signals the presenter semaphore after it on the same queue (2d1b6f), and the original's surface
    // retirement relies on that order. Waiting for the last signaled value therefore covers every KMD present this
    // presenter queued: the source image and the DWM surface stay mapped until then.
    QueueIdleCallbacks callbacks = {};
    callbacks.userData = this;
    callbacks.lastTimestamp = &m_signaledValue;
    callbacks.isRetired = [](void* userData, uint64_t value)
    {
        uint64 completed = 0;
        const Result query = static_cast<WindowedPresenter*>(userData)->m_semaphore->QuerySemaphoreValue(&completed);
        // A failed query means a lost device (the fence reads UINT64_MAX); like the queue's own fence in 4d8d80 that
        // ends the wait as retired.
        return (query != Result::Success) || (completed >= value);
    };
    callbacks.checkExecution = [](void* userData)
    {
        return CheckQueueExecutionRequest(
            static_cast<WindowedPresenter*>(userData)->m_device->OsTransport().DeviceHandle());
    };
    const Result result = (m_semaphore != nullptr) ? WaitQueueIdle(callbacks, gfxTimeoutSeconds) : Result::Success;
    if (Tracing() && (m_signaledValue != 0))
    {
        uint64 completed = 0;
        if (m_semaphore != nullptr) { m_semaphore->QuerySemaphoreValue(&completed); }
        std::fprintf(stderr, "[pal-wddm] presenter wait signaled=%llu completed=%llu result=%d\n",
                     static_cast<unsigned long long>(m_signaledValue), static_cast<unsigned long long>(completed),
                     int(result));
    }
    return result;
}

Result FlipPresenter::Present(const PresentDirectInfo& info)
{
    // 51aca0
    Result result = Result::ErrorFullscreenUnavailable;
    if ((info.pSrcImage != nullptr) && (FullscreenOwner(info.pSrcImage) == m_device))
    {
        KmtPresentSource source = {};
        source.context = m_queue->NativeQueue().context;
        source.hwQueue = m_hwQueue ? m_queue->NativeQueue().handle : 0;
        source.window = info.hWindow;
        const GpuMemory* memory = RootMemory(info.pSrcImage);
        source.source = (memory != nullptr) ? memory->OsAllocation().handle : 0;
        KmtPresentRecord record;
        BuildFlipPresent(source, info.flags.u32All, info.presentInterval, &record);
        // Flags bit 4 of the original PresentDirectInfo adds a 104-byte private record; this interface has no
        // such flag.
        result = ConvertNativeStatus(SubmitKmtPresent(m_hwQueue, &record));
    }
    ReportPresentConcluded(*m_device, static_cast<uint32>(Developer::PresentModeType::Flip), info.hWindow);
    return result;
}

// =====================================================================================================================
Result CreateQueuePresenters(Device* device, Queue* queue, QueueType queueType, EngineType engineType,
                             Presenter** windowed, Presenter** fullscreen)
{
    // 4d84b0
    const uint32 mask = device->PresenterMask(queueType);
    Result result = Result::Success;
    if ((mask & (PresenterWindowed | PresenterWindowedPriorBlt)) != 0)
    {
        auto* presenter = PAL_NEW(WindowedPresenter, device->GetPlatform(), Util::AllocInternal)(device, queue);
        result = (presenter != nullptr) ? presenter->Init(queueType, engineType) : Result::ErrorOutOfMemory;
        if (result == Result::Success) { *windowed = presenter; }
        else if (presenter != nullptr) { PAL_DELETE(presenter, device->GetPlatform()); }
    }
    // mask bit 3 selects the GDI presenter (CPU readback and SetDIBitsToDevice); it is not built, so such queues
    // have no windowed presenter.
    if ((result == Result::Success) && ((mask & PresenterFullscreen) != 0))
    {
        auto* presenter = PAL_NEW(FlipPresenter, device->GetPlatform(), Util::AllocInternal)(device, queue);
        if (presenter != nullptr) { *fullscreen = presenter; }
        else { result = Result::ErrorOutOfMemory; }
    }
    // On failure with KMD basic DWORD 2900 bit 4 the original builds the fallback presenter 51aee0 (vtable
    // 6576d60) in the windowed slot instead; not reconstructed, the failure is returned.
    return result;
}

void DestroyQueuePresenters(Device* device, Presenter** windowed, Presenter** fullscreen)
{
    if (*windowed != nullptr) { PAL_DELETE(*windowed, device->GetPlatform()); *windowed = nullptr; }
    if (*fullscreen != nullptr) { PAL_DELETE(*fullscreen, device->GetPlatform()); *fullscreen = nullptr; }
}

Result PresentDirectWithPresenters(const Device& device, const PresentDirectInfo& info,
                                   Presenter* windowed, Presenter* fullscreen)
{
    // 4d8f60: fullscreen presents use +0x1d8 unless forcePresentViaCpuBlt; everything else uses +0x1d0.
    Presenter* presenter = (PresenterSlotFor(info.presentMode, device.Settings().forcePresentViaCpuBlt) != 0)
                         ? fullscreen : windowed;
    // The original calls through a null presenter; a queue without a GPU presenter reports ErrorUnavailable, the
    // core gate's answer for a present mode without support.
    return (presenter != nullptr) ? presenter->Present(info) : Result::ErrorUnavailable;
}
} // namespace Pal::Wddm
