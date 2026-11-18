// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmScreen.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmEscape.h"
#include "core/os/wddm/wddmFullscreen.h"
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmPlatform.h"
#include "core/os/wddm/wddmPresent.h"
#include "core/os/wddm/wddmQueueLimit.h"
#include "core/os/wddm/wddmResult.h"
#include "core/os/wddm/wddmScreenPolicy.h"
#include "core/image.h"
#include "palSysMemory.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace Pal::Wddm
{
namespace
{
bool SameLuid(const LUID& lhs, uint32 low, int32 high)
{
    return (lhs.LowPart == low) && (lhs.HighPart == high);
}

Result Win32Result(LONG status)
{
    if (status == ERROR_SUCCESS) { return Result::Success; }
    if ((status == ERROR_NOT_ENOUGH_MEMORY) || (status == ERROR_OUTOFMEMORY)) { return Result::ErrorOutOfMemory; }
    if (status == ERROR_ACCESS_DENIED) { return Result::ErrorUnavailable; }
    if (status == ERROR_NOT_SUPPORTED) { return Result::Unsupported; }
    if (status == ERROR_INSUFFICIENT_BUFFER) { return Result::ErrorIncompleteResults; }
    return Result::ErrorUnknown;
}

Result DxgiResult(HRESULT status)
{
    if (SUCCEEDED(status)) { return Result::Success; }
    if (status == E_OUTOFMEMORY) { return Result::ErrorOutOfMemory; }
    if (status == DXGI_ERROR_NOT_FOUND) { return Result::ErrorScreenRemoved; }
    if (status == DXGI_ERROR_MORE_DATA) { return Result::ErrorIncompleteResults; }
    if ((status == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) || (status == DXGI_ERROR_ACCESS_LOST))
        { return Result::ErrorUnavailable; }
    if ((status == E_NOINTERFACE) || (status == DXGI_ERROR_UNSUPPORTED)) { return Result::Unsupported; }
    return Result::ErrorUnknown;
}

class ScopedDisplayAdapter
{
public:
    ~ScopedDisplayAdapter() { if (m_handle != 0) { Close(); } }
    Result Open(const ScreenRecord& record)
    {
        D3DKMT_OPENADAPTERFROMLUID request = {};
        // KMT operates on the source adapter/source ID pair. A cross-adapter
        // target LUID is a different identity and must not be substituted.
        request.AdapterLuid = { record.sourceAdapterLow, record.sourceAdapterHigh };
        const Result result = ConvertNativeStatus(D3DKMTOpenAdapterFromLuid(&request));
        if (result == Result::Success)
        {
            if (request.hAdapter == 0) { return Result::ErrorUnknown; }
            m_handle = request.hAdapter;
        }
        return result;
    }
    Result Close()
    {
        if (m_handle == 0) { return Result::Success; }
        D3DKMT_CLOSEADAPTER request = { m_handle };
        const Result result = ConvertNativeStatus(D3DKMTCloseAdapter(&request));
        if (result == Result::Success) { m_handle = 0; }
        return result;
    }
    D3DKMT_HANDLE Handle() const { return m_handle; }
private:
    D3DKMT_HANDLE m_handle = 0;
};
static_assert(sizeof(D3DKMT_GETSCANLINE) == 16);
static_assert(offsetof(D3DKMT_GETSCANLINE, InVerticalBlank) == 8);
static_assert(offsetof(D3DKMT_GETSCANLINE, ScanLine) == 12);
static_assert(sizeof(D3DKMT_WAITFORVERTICALBLANKEVENT) == 12);

struct MonitorSearch
{
    const wchar_t* name;
    HMONITOR monitor = nullptr;
};

BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM argument)
{
    auto* search = reinterpret_cast<MonitorSearch*>(argument);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && (std::wcscmp(info.szDevice, search->name) == 0))
    {
        search->monitor = monitor;
        return FALSE;
    }
    return TRUE;
}

Result FindOutput(const ScreenRecord& record, ComPtr<IDXGIOutput1>* output)
{
    // Recreate the factory on every query so a stale factory never supplies
    // modes/color information after a topology or Advanced Color change.
    ComPtr<IDXGIFactory1> factory;
    HRESULT status = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(status)) { return DxgiResult(status); }
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        status = factory->EnumAdapters1(adapterIndex, &adapter);
        if (status == DXGI_ERROR_NOT_FOUND) { break; }
        if (FAILED(status)) { return DxgiResult(status); }
        DXGI_ADAPTER_DESC1 adapterDesc = {};
        status = adapter->GetDesc1(&adapterDesc);
        if (FAILED(status)) { return DxgiResult(status); }
        if (!SameLuid(adapterDesc.AdapterLuid, record.adapterLow, record.adapterHigh)) { continue; }
        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> candidate;
            status = adapter->EnumOutputs(outputIndex, &candidate);
            if (status == DXGI_ERROR_NOT_FOUND) { break; }
            if (FAILED(status)) { return DxgiResult(status); }
            DXGI_OUTPUT_DESC desc = {};
            status = candidate->GetDesc(&desc);
            if (FAILED(status)) { return DxgiResult(status); }
            if (desc.AttachedToDesktop && (std::wcscmp(desc.DeviceName, record.gdiName) == 0))
            {
                return DxgiResult(candidate.As(output));
            }
        }
    }
    return Result::ErrorScreenRemoved;
}

struct FormatPair
{
    DXGI_FORMAT dxgi;
    ChNumFormat pal;
    bool bgra;
};
// These are the desktop scan-out format families exposed by this bounded
// implementation. Each actual mode is still obtained from the output.
constexpr FormatPair Formats[] = {
    { DXGI_FORMAT_R8G8B8A8_UNORM, ChNumFormat::X8Y8Z8W8_Unorm, false },
    { DXGI_FORMAT_B8G8R8A8_UNORM, ChNumFormat::X8Y8Z8W8_Unorm, true },
    { DXGI_FORMAT_R10G10B10A2_UNORM, ChNumFormat::X10Y10Z10W2_Unorm, false },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, ChNumFormat::X16Y16Z16W16_Float, false },
    { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ChNumFormat::X8Y8Z8W8_Srgb, false },
    { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, ChNumFormat::X8Y8Z8W8_Srgb, true },
    { DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, ChNumFormat::X10Y10Z10W2Bias_Unorm, false },
};

SwizzledFormat PalFormat(const FormatPair& pair)
{
    SwizzledFormat format = {};
    format.format = pair.pal;
    format.swizzle.r = pair.bgra ? ChannelSwizzle::Z : ChannelSwizzle::X;
    format.swizzle.g = ChannelSwizzle::Y;
    format.swizzle.b = pair.bgra ? ChannelSwizzle::X : ChannelSwizzle::Z;
    format.swizzle.a = ChannelSwizzle::W;
    return format;
}

Result ReadModes(const ScreenRecord& record, std::vector<ScreenMode>* result)
{
    ComPtr<IDXGIOutput1> output;
    Result query = FindOutput(record, &output);
    if (query != Result::Success) { return query; }
    result->clear();
    // PAL ScreenMode has no interlace/scaling selector, so enumerate only
    // progressive modes, including scaling and enabled stereo variants.
    constexpr UINT flags = DXGI_ENUM_MODES_SCALING | DXGI_ENUM_MODES_STEREO;
    for (const auto& pair : Formats)
    {
        std::vector<DXGI_MODE_DESC1> modes;
        HRESULT status = DXGI_ERROR_MORE_DATA;
        for (uint32 attempt = 0; (attempt < 4) && (status == DXGI_ERROR_MORE_DATA); ++attempt)
        {
            UINT count = 0;
            status = output->GetDisplayModeList1(pair.dxgi, flags, &count, nullptr);
            if (FAILED(status)) { break; }
            if (count == 0) { modes.clear(); break; }
            modes.resize(count);
            status = output->GetDisplayModeList1(pair.dxgi, flags, &count, modes.data());
            if (SUCCEEDED(status)) { modes.resize(count); }
        }
        if (FAILED(status)) { return DxgiResult(status); }
        for (const auto& desc : modes)
        {
            if ((desc.Width == 0) || (desc.Height == 0) || (desc.RefreshRate.Denominator == 0)) { continue; }
            if ((desc.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST) ||
                (desc.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST)) { continue; }
            ScreenMode mode = {};
            mode.flags.stereo = (desc.Stereo != FALSE);
            mode.extent = { desc.Width, desc.Height };
            mode.format = PalFormat(pair);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 894
            mode.refreshRate = desc.RefreshRate.Numerator / desc.RefreshRate.Denominator;
#else
            mode.refreshRate = { desc.RefreshRate.Numerator, desc.RefreshRate.Denominator };
#endif
            // Scaling variants can map to the same representable PAL mode.
            const auto same = [&mode](const ScreenMode& other) {
                return (mode.flags.u32All == other.flags.u32All) && (mode.extent.width == other.extent.width) &&
                    (mode.extent.height == other.extent.height) && (mode.format == other.format) &&
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 894
                    (mode.refreshRate == other.refreshRate);
#else
                    (uint64(mode.refreshRate.numerator) * other.refreshRate.denominator ==
                     uint64(other.refreshRate.numerator) * mode.refreshRate.denominator);
#endif
            };
            if (std::none_of(result->begin(), result->end(), same)) { result->push_back(mode); }
        }
    }
    return Result::Success;
}

// 368c60: EnumDisplayMonitors callback; stops at the monitor whose rectangle
// equals the window client rectangle.
struct MonitorMatch
{
    const RECT* client;
    bool found;
};

BOOL CALLBACK MatchMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM argument)
{
    auto* match = reinterpret_cast<MonitorMatch*>(argument);
    MONITORINFOEXA info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoA(monitor, &info);
    if ((match->client->left == info.rcMonitor.left) && (match->client->right == info.rcMonitor.right) &&
        (match->client->top == info.rcMonitor.top) && (match->client->bottom == info.rcMonitor.bottom))
    {
        match->found = true;
    }
    return match->found ? FALSE : TRUE;
}

// 368ce0: borderless client area that exactly covers one monitor, without DPI virtualization.
bool CoversMonitor(HWND window)
{
    WINDOWINFO info = {}; // The original leaves cbSize zero.
    GetWindowInfo(window, &info);
    if ((info.rcClient.right != info.rcWindow.right) || (info.rcClient.bottom != info.rcWindow.bottom))
    {
        return false;
    }
    const LONG width  = info.rcClient.right - info.rcClient.left;
    const LONG height = info.rcClient.bottom - info.rcClient.top;
    POINT point = { width, height };
    // The original resolves this export lazily (Platform+0x2bb8) and skips the
    // check when the call fails.
    if ((LogicalToPhysicalPointForPerMonitorDPI(window, &point) != FALSE) &&
        ((point.x != width) || (point.y != height)))
    {
        return false;
    }
    MonitorMatch match = { &info.rcClient, false };
    EnumDisplayMonitors(nullptr, nullptr, MatchMonitor, reinterpret_cast<LPARAM>(&match));
    return match.found;
}

// 368e30: the window (or the foreground window it belongs to) covers a monitor
// and no other visible, activatable top-level window above it intersects it.
bool IsUnoccludedFullscreenWindow(HWND window)
{
    if (IsWindowVisible(window) == FALSE) { return false; }
    const HWND foreground = GetForegroundWindow();
    const bool covers = CoversMonitor(window);
    if ((foreground == window) && covers) { return true; }
    if ((IsChild(foreground, window) != FALSE) && CoversMonitor(foreground)) { return true; }
    if (!covers) { return false; }
    RECT bounds = {};
    GetWindowRect(window, &bounds);
    const HWND root = GetAncestor(window, GA_ROOT);
    for (HWND other = GetWindow((root != nullptr) ? root : window, GW_HWNDPREV); other != nullptr;
         other = GetWindow(other, GW_HWNDPREV))
    {
        if ((other == window) || (IsWindowVisible(other) == FALSE)) { continue; }
        WINDOWINFO info = {};
        GetWindowInfo(other, &info);
        const DWORD exStyle = info.dwExStyle & ~DWORD(WS_EX_NOREDIRECTIONBITMAP);
        if (((info.dwStyle & WS_VISIBLE) == 0) || ((info.dwExStyle & WS_EX_NOACTIVATE) != 0)) { continue; }
        // Extended-style combinations the original treats as non-occluding overlays.
        if ((exStyle == 0xe0000888) || (exStyle == 0xc0000888) || (exStyle == 0x20000888) ||
            (exStyle == 0xc0080888) || (exStyle == 0x20000880) || (exStyle == 0x000808a8) ||
            (exStyle == 0xe0008800)) { continue; }
        RECT overlap = {};
        if (IntersectRect(&overlap, &bounds, &info.rcWindow) != FALSE) { return false; }
    }
    return true;
}

// 36a087..36a0ad / 2b3f20: the root GPU memory bound to the image (the original
// follows GpuMemory+0x100 to the end, then reads its first KMT allocation).
const Pal::GpuMemory* RootBoundMemory(const Pal::Image& image)
{
    const Pal::GpuMemory* memory = image.GetBoundGpuMemory().Memory();
    while ((memory != nullptr) && (memory->OriginalGpuMem() != nullptr)) { memory = memory->OriginalGpuMem(); }
    return memory;
}

FullscreenNotifyState NotifyState(const Device& device)
{
    // Native adapter+0x10 is DWORD 8 of the 2400-byte KMD adapter record, the
    // header our 3008-byte basic record starts with.
    uint32 linkedAdapters = 0;
    std::memcpy(&linkedAdapters, device.Adapter().physical.basicRecord.data() + 8, sizeof(linkedAdapters));
    FullscreenNotifyState state = {};
    state.multiAdapter = linkedAdapters > 1;
    state.frameLock = device.FlglFrameLockEnabled();
    state.profiler = device.GetPlatform()->PlatformSettings().gpuProfilerMode != GpuProfilerDisabled;
    return state;
}

// Device-level steps of the ownership methods: 3687a0 on the screen's Device and
// 310510 on the image's Device.
struct OwnershipDevices
{
    Device* screen;
    Device* image;
};

Result NotifyHook(void* context, bool fullscreen)
{
    Device* device = static_cast<OwnershipDevices*>(context)->screen;
    return NotifyFullscreenState(DeviceEscapeRoute(device->OsTransport(), device->GetPlatform()), fullscreen,
                                 NotifyState(*device));
}

Result QueuedLimitHook(void* context)
{
    Device* device = static_cast<OwnershipDevices*>(context)->image;
    return ApplyQueuedPresentLimit(device->OsTransport(), device->MaxQueuedFrames());
}

ExclusiveModeLock& PlatformExclusiveMode(Device* device)
{
    return static_cast<Platform*>(device->GetPlatform())->ExclusiveMode();
}

void* PlatformAlloc(void* platform, size_t bytes)
{
    return (platform == nullptr) ? nullptr :
        PAL_MALLOC_ALIGNED(bytes, 1, static_cast<Pal::Platform*>(platform), Util::AllocInternal);
}

void PlatformFree(void* platform, void* memory)
{
    PAL_FREE(memory, static_cast<Pal::Platform*>(platform));
}

int32 DisplayConfigQuery(void* header)
{
    return DisplayConfigGetDeviceInfo(static_cast<DISPLAYCONFIG_DEVICE_INFO_HEADER*>(header));
}
}

Screen::Screen(const ScreenRecord& record, const IDevice* mainDevice)
    : m_record(record),
      m_mainDevice(mainDevice),
      m_platform((mainDevice != nullptr) ? static_cast<const Pal::Device*>(mainDevice)->GetPlatform() : nullptr)
{
}

Screen::~Screen()
{
    // 3688d9..3688f4: the display information returns to the Platform.
    // Fullscreen ownership is not released here, as in the original.
    ReleaseColorCache(&m_color, ColorContext(nullptr));
}

Device* Screen::MainDevice() const
{
    return (m_mainDevice == nullptr) ? nullptr :
        const_cast<Device*>(static_cast<const Device*>(static_cast<const Pal::Device*>(m_mainDevice)));
}

FullscreenTarget Screen::Target(Device* device) const
{
    // Screen+0x148 holds the main Device's adapter handle (adapter+0x970).
    return { device->OsTransport().AdapterHandle(), device->OsTransport().DeviceHandle(), m_record.sourceId };
}

ScreenColorContext Screen::ColorContext(const EscapeRoute* route) const
{
    return { route, m_platform, PlatformAlloc, PlatformFree, DisplayConfigQuery,
             m_record.sourceAdapterLow, m_record.sourceAdapterHigh, m_record.targetId };
}

Result QueryActiveScreenRecords(std::vector<ScreenRecord>* records, LONG* win32Status)
{
    if (win32Status != nullptr) { *win32Status = ERROR_SUCCESS; }
    if (records == nullptr) { return Result::ErrorInvalidPointer; }
    records->clear();
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (uint32 attempt = 0; (attempt < 4) && (status == ERROR_INSUFFICIENT_BUFFER); ++attempt)
    {
        UINT32 pathCount = 0, modeCount = 0;
        status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS)
        {
            if (win32Status != nullptr) { *win32Status = status; }
            return Win32Result(status);
        }
        if (pathCount == 0) { return Result::Success; }
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_SUCCESS) { paths.resize(pathCount); modes.resize(modeCount); }
    }
    if (status != ERROR_SUCCESS)
    {
        if (win32Status != nullptr) { *win32Status = status; }
        return Win32Result(status);
    }
    for (const auto& path : paths)
    {
        if (((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) || !path.targetInfo.targetAvailable) { continue; }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id };
        status = DisplayConfigGetDeviceInfo(&source.header);
        if (status != ERROR_SUCCESS) { records->clear(); return Win32Result(status); }
        const UINT32 sourceIndex = path.sourceInfo.modeInfoIdx;
        if ((sourceIndex >= modes.size()) || (modes[sourceIndex].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) ||
            (modes[sourceIndex].id != path.sourceInfo.id) ||
            !SameLuid(modes[sourceIndex].adapterId, path.sourceInfo.adapterId.LowPart, path.sourceInfo.adapterId.HighPart))
        {
            records->clear();
            return Result::ErrorUnavailable;
        }
        ScreenRecord record;
        record.adapterLow = path.targetInfo.adapterId.LowPart;
        record.adapterHigh = path.targetInfo.adapterId.HighPart;
        record.targetId = path.targetInfo.id;
        record.sourceId = path.sourceInfo.id;
        record.sourceAdapterLow = path.sourceInfo.adapterId.LowPart;
        record.sourceAdapterHigh = path.sourceInfo.adapterId.HighPart;
        std::wmemcpy(record.gdiName, source.viewGdiDeviceName, 32);
        record.gdiName[31] = L'\0';
        MonitorSearch search = { record.gdiName };
        EnumDisplayMonitors(nullptr, nullptr, FindMonitor, reinterpret_cast<LPARAM>(&search));
        if (search.monitor == nullptr) { records->clear(); return Result::ErrorUnavailable; }
        record.monitor = search.monitor;
        switch (path.targetInfo.rotation)
        {
        case DISPLAYCONFIG_ROTATION_IDENTITY: record.rotation = ScreenRotation::Rot0; break;
        case DISPLAYCONFIG_ROTATION_ROTATE90: record.rotation = ScreenRotation::Rot90; break;
        case DISPLAYCONFIG_ROTATION_ROTATE180: record.rotation = ScreenRotation::Rot180; break;
        case DISPLAYCONFIG_ROTATION_ROTATE270: record.rotation = ScreenRotation::Rot270; break;
        default: records->clear(); return Result::ErrorUnavailable;
        }
        const auto& sourceMode = modes[sourceIndex].sourceMode;
        record.desktop = { { sourceMode.position.x, sourceMode.position.y }, { sourceMode.width, sourceMode.height } };
        // The desktop/source image is rotated; PAL mode extents are unrotated.
        record.currentResolution = { sourceMode.width, sourceMode.height };
        if ((record.rotation == ScreenRotation::Rot90) || (record.rotation == ScreenRotation::Rot270))
            { std::swap(record.currentResolution.width, record.currentResolution.height); }
        DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred = {};
        preferred.header = { DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE, sizeof(preferred), path.targetInfo.adapterId, path.targetInfo.id };
        if (DisplayConfigGetDeviceInfo(&preferred.header) == ERROR_SUCCESS)
            { record.preferredResolution = { preferred.width, preferred.height }; }
        records->push_back(record);
    }
    return Result::Success;
}

Result Screen::Refresh(ScreenRecord* record) const
{
    std::vector<ScreenRecord> records;
    const Result result = QueryActiveScreenRecords(&records);
    if (result != Result::Success) { return result; }
    for (const auto& candidate : records)
    {
        if ((candidate.adapterLow == m_record.adapterLow) && (candidate.adapterHigh == m_record.adapterHigh) &&
            (candidate.targetId == m_record.targetId))
        {
            *record = candidate;
            return Result::Success;
        }
    }
    return Result::ErrorScreenRemoved;
}

Result Screen::GetProperties(ScreenProperties* info) const
{
    if (info == nullptr) { return Result::ErrorInvalidPointer; }
    ScreenRecord record;
    Result result = Refresh(&record);
    if (result != Result::Success) { return result; }
    ScreenProperties properties = {};
    properties.hDisplay = record.monitor;
    const int copied = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, record.gdiName, -1,
                                           properties.displayName, MaxDisplayName, nullptr, nullptr);
    if (copied == 0) { return Result::ErrorUnknown; }
    properties.screen = record.targetId;
    properties.vidPnSourceId = record.sourceId;
    properties.rotation = record.rotation;
    properties.desktopCoordinates = record.desktop;
    properties.pMainDevice = m_mainDevice;
    properties.physicalResolution = record.preferredResolution;
    SetScreenSelectedNativeProperties(&properties, WindowsScreenPolicyCalls());
    *info = properties;
    return Result::Success;
}

Result Screen::QueryCurrentDisplayMode(Extent2d* extent) const
{
    if (extent == nullptr) { return Result::ErrorInvalidPointer; }
    ScreenRecord record;
    const Result result = Refresh(&record);
    if (result == Result::Success) { *extent = record.currentResolution; }
    return result;
}

Result Screen::GetScreenModeList(uint32* count, ScreenMode* modes) const
{
    if (count == nullptr) { return Result::ErrorInvalidPointer; }
    ScreenRecord record;
    Result result = Refresh(&record);
    if (result != Result::Success) { return result; }
    std::vector<ScreenMode> available;
    result = ReadModes(record, &available);
    if (result != Result::Success) { return result; }
    const uint32 capacity = (modes != nullptr) ? *count : 0;
    *count = static_cast<uint32>(available.size());
    if (modes == nullptr) { return Result::Success; }
    const uint32 copied = std::min(capacity, static_cast<uint32>(available.size()));
    std::copy_n(available.begin(), copied, modes);
    *count = copied;
    return (capacity < available.size()) ? Result::ErrorInvalidMemorySize : Result::Success;
}

Result Screen::GetFormats(uint32* count, SwizzledFormat* formats)
{
    if (count == nullptr) { return Result::ErrorInvalidPointer; }
    ScreenRecord record;
    Result result = Refresh(&record);
    if (result != Result::Success) { return result; }
    std::vector<ScreenMode> modes;
    result = ReadModes(record, &modes);
    if (result != Result::Success) { return result; }
    std::vector<SwizzledFormat> available;
    for (const auto& mode : modes)
    {
        if (std::find(available.begin(), available.end(), mode.format) == available.end())
            { available.push_back(mode.format); }
    }
    const uint32 capacity = (formats != nullptr) ? *count : 0;
    *count = static_cast<uint32>(available.size());
    if (formats == nullptr) { return Result::Success; }
    const uint32 copied = std::min(capacity, static_cast<uint32>(available.size()));
    std::copy_n(available.begin(), copied, formats);
    *count = copied;
    return (capacity < available.size()) ? Result::ErrorIncompleteResults : Result::Success;
}

Result Screen::RegisterWindow(OsWindowHandle window)
{
    // Windows windowed presentation has registerWindowRequired=false. PAL
    // explicitly permits this call on such platforms without registration.
    return ((window == nullptr) || IsWindow(static_cast<HWND>(window))) ? Result::Success : Result::ErrorInvalidValue;
}

HdrDisplayMode Screen::GetFormatHdrMode(SwizzledFormat format) const
{
    // Native 36adc0: only the colour state of Screen+0x150 (refreshed at enumeration and by the colour methods)
    // and the channel format; the original's argument carries no swizzle.
    return static_cast<HdrDisplayMode>(FormatHdrMode(m_color.colorState, format.format));
}

Result Screen::RefreshColorStateForEnumeration()
{
    // Native 3015c3..3015e0: Screen+0x148 = adapter handle, then 36aba0. Enumeration only builds screens with a
    // Device, so the escape route always exists.
    Device* device = MainDevice();
    EscapeRoute route = {};
    if (device != nullptr) { route = DeviceEscapeRoute(device->OsTransport(), device->GetPlatform()); }
    return RefreshColorState(&m_color, ColorContext((device != nullptr) ? &route : nullptr));
}

Result Screen::GetScanLine(int32* scanLine) const
{
    if (scanLine == nullptr) { return Result::ErrorInvalidPointer; }
    if (ScreenTimingUnavailable(m_ownsFullscreen, WindowsScreenPolicyCalls())) { return Result::ErrorUnavailable; }
    ScreenRecord record;
    Result result = Refresh(&record);
    if (result != Result::Success) { return result; }
    ScopedDisplayAdapter adapter;
    result = adapter.Open(record);
    if (result != Result::Success) { return result; }
    D3DKMT_GETSCANLINE request = {};
    request.hAdapter = adapter.Handle();
    request.VidPnSourceId = record.sourceId;
    result = ConvertNativeStatus(D3DKMTGetScanLine(&request));
    const Result closed = adapter.Close();
    if (result == Result::Success) { result = closed; }
    if (result == Result::Success)
        { *scanLine = request.InVerticalBlank ? -1 : static_cast<int32>(request.ScanLine); }
    return result;
}

Result Screen::WaitForVerticalBlank() const
{
    if (ScreenTimingUnavailable(m_ownsFullscreen, WindowsScreenPolicyCalls())) { return Result::ErrorUnavailable; }
    ScreenRecord record;
    Result result = Refresh(&record);
    if (result != Result::Success) { return result; }
    ScopedDisplayAdapter adapter;
    result = adapter.Open(record);
    if (result != Result::Success) { return result; }
    D3DKMT_WAITFORVERTICALBLANKEVENT request = {};
    request.hAdapter = adapter.Handle();
    // hDevice is optional in the SDK and in the recovered original branch.
    request.VidPnSourceId = record.sourceId;
    result = ConvertNativeStatus(D3DKMTWaitForVerticalBlankEvent(&request));
    const Result closed = adapter.Close();
    return (result == Result::Success) ? closed : result;
}

Result Screen::IsImplicitFullscreenOwnershipSafe(OsDisplayHandle hDisplay, OsWindowHandle hWindow,
                                                 Extent2d imageExtent) const
{
    // Native 369000..3690fd. Only the caller's handles are inspected; this
    // screen's own identity is not compared.
    const HWND window = static_cast<HWND>(hWindow);
    static_cast<void>(GetForegroundWindow()); // 36902d: queried and discarded by the original.
    if (!IsUnoccludedFullscreenWindow(window)) { return Result::ErrorFullscreenUnavailable; }
    const HMONITOR monitor = static_cast<HMONITOR>(hDisplay);
    if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != monitor) { return Result::ErrorScreenRemoved; }
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoA(monitor, &info);
    if ((uint32(info.rcMonitor.right) - uint32(info.rcMonitor.left) != imageExtent.width) ||
        (uint32(info.rcMonitor.bottom) - uint32(info.rcMonitor.top) != imageExtent.height))
    {
        return Result::ErrorInvalidResolution;
    }
    RECT bounds = {};
    GetWindowRect(window, &bounds);
    if ((uint32(bounds.right) - uint32(bounds.left) != imageExtent.width) ||
        (uint32(bounds.bottom) - uint32(bounds.top) != imageExtent.height))
    {
        return Result::ErrorIncompatibleWindowSize;
    }
    return Result::Success;
}

Result Screen::TakeFullscreenOwnership(const IImage& image)
{
    // Native 3691a0 / 369ed0; the sequence is in wddmFullscreen.cpp.
    if (m_ownsFullscreen) { return Result::ErrorFullscreenUnavailable; }
    Device* device = MainDevice();
    // Enumeration only creates screens with a PAL Device; the original would
    // dereference the missing adapter object here.
    if (device == nullptr) { return Result::ErrorUnavailable; }
    const auto& palImage = static_cast<const Pal::Image&>(image);
    const Pal::GpuMemory* memory = RootBoundMemory(palImage);
    FullscreenImage facts = {};
    facts.width = image.GetImageCreateInfo().extent.width;
    facts.height = image.GetImageCreateInfo().extent.height;
    facts.hwRotation = palImage.GetImageInfo().internalCreateInfo.flags.hwRotationEnabled != 0;
    facts.hasMemory = memory != nullptr;
    facts.primaryAllocation = (memory != nullptr) ? static_cast<const GpuMemory*>(memory)->OsAllocation().handle : 0;
    OwnershipDevices devices = { device, static_cast<Device*>(palImage.GetDevice()) };
    const FullscreenHooks hooks = { &devices, NotifyHook, QueuedLimitHook };
    return Wddm::TakeFullscreenOwnership(WindowsFullscreenCalls(), Target(device), facts,
                                         PlatformExclusiveMode(device), hooks, &m_ownsFullscreen);
}

Result Screen::ReleaseFullscreenOwnership()
{
    // Native 369290.
    if (!m_ownsFullscreen) { return Result::ErrorFullscreenUnavailable; }
    Device* device = MainDevice(); // Ownership implies a Device.
    OwnershipDevices devices = { device, device };
    const FullscreenHooks hooks = { &devices, NotifyHook, QueuedLimitHook };
    return Wddm::ReleaseFullscreenOwnership(WindowsFullscreenCalls(), Target(device), PlatformExclusiveMode(device),
                                            hooks, &m_ownsFullscreen);
}

Result Screen::SetGammaRamp(const GammaRamp& gammaRamp)
{
    // Native 369330.
    if (!m_ownsFullscreen) { return Result::ErrorUnavailable; }
    return SetFullscreenGammaRamp(WindowsFullscreenCalls(), Target(MainDevice()), m_ownsFullscreen, gammaRamp);
}

Result Screen::GetColorCapabilities(ScreenColorCapabilities* caps)
{
    // Native 36a720. The original writes the output without a null check.
    if (caps == nullptr) { return Result::ErrorInvalidPointer; }
    Device* device = MainDevice();
    EscapeRoute route = {};
    if (device != nullptr) { route = DeviceEscapeRoute(device->OsTransport(), device->GetPlatform()); }
    return GetScreenColorCapabilities(&m_color, ColorContext((device != nullptr) ? &route : nullptr), caps);
}

Result Screen::SetColorConfiguration(const ScreenColorConfig* config)
{
    // Native 36a8f0. The original dereferences the configuration without a null check.
    if (config == nullptr) { return Result::ErrorInvalidPointer; }
    Device* device = MainDevice();
    EscapeRoute route = {};
    if (device != nullptr) { route = DeviceEscapeRoute(device->OsTransport(), device->GetPlatform()); }
    return SetScreenColorConfiguration(&m_color, ColorContext((device != nullptr) ? &route : nullptr), *config);
}
}