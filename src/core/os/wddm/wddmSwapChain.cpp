// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmSwapChain.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmFullscreen.h"
#include "core/os/wddm/wddmImage.h"
#include "core/os/wddm/wddmPresent.h"
#include "core/os/wddm/wddmPresentScheduler.h"
#include "core/os/wddm/wddmResult.h"
#include "core/gpuMemory.h"
#include "palDeveloperHooks.h"
#include "palFormatInfo.h"
#include "palQueueSemaphore.h"
#include "palSysMemory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace Pal::Wddm
{
namespace
{

Result ConvertHresult(HRESULT status, const char* operation = "D3D12/DXGI")
{
    if (FAILED(status) && (std::getenv("PAL_WDDM_TRACE") != nullptr))
    {
        std::fprintf(stderr, "[pal-wddm-wsi] %s HRESULT=0x%08lx\n", operation, static_cast<unsigned long>(status));
    }
    if (SUCCEEDED(status)) { return Result::Success; }
    if ((status == DXGI_ERROR_DEVICE_REMOVED) || (status == DXGI_ERROR_DEVICE_RESET) ||
        (status == DXGI_ERROR_DEVICE_HUNG)) { return Result::ErrorDeviceLost; }
    if (status == E_OUTOFMEMORY) { return Result::ErrorOutOfMemory; }
    if ((status == E_INVALIDARG) || (status == DXGI_ERROR_INVALID_CALL)) { return Result::ErrorInvalidValue; }
    if ((status == E_NOINTERFACE) || (status == DXGI_ERROR_UNSUPPORTED)) { return Result::Unsupported; }
    return Result::ErrorUnknown;
}

DXGI_FORMAT NativeFormat(const SwizzledFormat& format)
{
    const auto& map = format.swizzle;
    const bool rgba = (map.r == ChannelSwizzle::X) && (map.g == ChannelSwizzle::Y) &&
                      (map.b == ChannelSwizzle::Z) && (map.a == ChannelSwizzle::W);
    const bool bgra = (map.r == ChannelSwizzle::Z) && (map.g == ChannelSwizzle::Y) &&
                      (map.b == ChannelSwizzle::X) && (map.a == ChannelSwizzle::W);
    switch (format.format)
    {
    case ChNumFormat::X8Y8Z8W8_Unorm:
    case ChNumFormat::X8Y8Z8W8_Srgb:
        return rgba ? DXGI_FORMAT_R8G8B8A8_UNORM : bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    case ChNumFormat::X10Y10Z10W2_Unorm:
        return rgba ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_UNKNOWN;
    case ChNumFormat::X16Y16Z16W16_Float:
        return rgba ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_UNKNOWN;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsWindowPlatform(WsiPlatform platform)
{
    return (platform == WsiPlatform::Win32) || (platform == WsiPlatform::Dxgi);
}

Result ValidateSwapChain(const SwapChainCreateInfo& info)
{
    // Every swap-chain mode and colour space reaches DXGI in the original (388190 maps the mode to the tearing flag
    // and the present interval, the colour space to SetColorSpace1).
    if (!IsWindowPlatform(info.wsiPlatform) || (info.imageArraySize > 1) ||
        info.flags.tmzProtected || info.flags.isDxgiStereo || info.flags.enablePresentThread ||
        ((info.preTransform != SurfaceTransformNone) && (info.preTransform != 0)) ||
        ((info.compositeAlpha != CompositeAlphaMode::Opaque) && (static_cast<uint32>(info.compositeAlpha) != 0)) ||
        (info.swapChainMode >= SwapChainMode::Count)) { return Result::Unsupported; }
    for (const IDevice* slave : info.pSlaveDevices)
    {
        if (slave != nullptr) { return Result::Unsupported; }
    }
    if ((info.imageCount < 2) || (info.imageCount > MaxSwapChainLength) ||
        (info.imageExtent.width == 0) || (info.imageExtent.height == 0) ||
        (info.imageExtent.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
        (info.imageExtent.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
        (NativeFormat(info.imageSwizzledFormat) == DXGI_FORMAT_UNKNOWN)) { return Result::ErrorInvalidValue; }
    if ((info.hWindow == nullptr) || !IsWindow(static_cast<HWND>(info.hWindow)))
    { return Result::ErrorInvalidValue; }
    return Result::Success;
}

ImageCreateInfo ImageInfo(const PresentableImageCreateInfo& info)
{
    ImageCreateInfo image = {};
    image.imageType = ImageType::Tex2d;
    image.tiling = ImageTiling::Linear;
    image.metadataMode = MetadataMode::Disabled;
    image.swizzledFormat = info.swizzledFormat;
    image.usageFlags = info.usage;
    image.extent = {info.extent.width, info.extent.height, 1};
    image.arraySize = 1;
    image.mipLevels = 1;
    image.samples = 1;
    image.fragments = 1;
    image.viewFormatCount = info.viewFormatCount;
    image.pViewFormats = info.pViewFormats;
    image.flags.presentable = 1;
    const uint32 pixelBytes = Formats::BytesPerPixel(info.swizzledFormat.format);
    image.rowPitch = Util::Pow2Align(info.extent.width * pixelBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    image.depthPitch = image.rowPitch * info.extent.height;
    return image;
}

Result ValidatePresentable(const PresentableImageCreateInfo& info)
{
    if (info.pSwapChain == nullptr) { return Result::ErrorInvalidPointer; }
    if (info.flags.fullscreen || info.flags.stereo || info.flags.tmzProtected)
    { return Result::Unsupported; }
    if ((info.extent.width == 0) || (info.extent.height == 0) ||
        (info.extent.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
        (info.extent.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
        (NativeFormat(info.swizzledFormat) == DXGI_FORMAT_UNKNOWN)) { return Result::ErrorInvalidValue; }
    return Result::Success;
}

std::chrono::nanoseconds Remaining(std::chrono::steady_clock::time_point start,
                                   std::chrono::nanoseconds timeout)
{
    if (timeout == std::chrono::nanoseconds::max()) { return timeout; }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return (elapsed < timeout) ? timeout - elapsed : std::chrono::nanoseconds::zero();
}

DWORD WaitMilliseconds(std::chrono::nanoseconds timeout)
{
    if (timeout == std::chrono::nanoseconds::max()) { return INFINITE; }
    const uint64 ns = static_cast<uint64>(std::max<int64_t>(timeout.count(), 0));
    const uint64 ms = (ns / 1000000) + ((ns % 1000000) != 0);
    return static_cast<DWORD>(std::min<uint64>(ms, INFINITE - 1));
}

} // namespace

struct PresentableImageState
{
    explicit PresentableImageState(Device* owner) : device(owner) {}
    ~PresentableImageState()
    {
        if (readySemaphore != nullptr) { readySemaphore->Destroy(); }
        PAL_SAFE_FREE(readySemaphoreMemory, device->GetPlatform());
    }
    std::atomic<uint32> references{1};
    Device* device;
    SwapChain* swapChain = nullptr;
    uint32 index = 0;
    bool acquired = false;
    uint64 lastCopy = 0;
    uint64 readySerial = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence> readyFence;
    IQueueSemaphore* readySemaphore = nullptr;
    void* readySemaphoreMemory = nullptr;
};

void RetainPresentableImageState(PresentableImageState* state)
{
    if (state != nullptr) { state->references.fetch_add(1, std::memory_order_relaxed); }
}

void ReleasePresentableImageState(PresentableImageState* state)
{
    if ((state != nullptr) && (state->references.fetch_sub(1, std::memory_order_acq_rel) == 1))
    { PAL_DELETE(state, state->device->GetPlatform()); }
}

struct SwapChain::Impl
{
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> copyFence;
    SyncObject copySync;
    HANDLE frameLatency = nullptr;
    uint64 copies = 0;
    uint64 acquiredAfterCopy = 0;
    bool allowTearing = false;               // 388190: DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING for Immediate chains
    PresentMode lastPresentMode = PresentMode::Unknown; // +0x3d8
    Util::Mutex mutex;
    IQueue* signalQueue = nullptr;
    void* signalQueueMemory = nullptr;
    PresentableImageState* images[MaxSwapChainLength] = {};
};

size_t SwapChain::GetSize(const Device& device, const SwapChainCreateInfo& info, Result* result)
{
    // Device slot 133 (313490): *pResult = Success, then the DXGI size (387900) or, for every other WSI platform,
    // the Win32 swap chain size (4dc710 + 4dd160 + 0x218).
    if (info.wsiPlatform != WsiPlatform::Dxgi)
    {
        if (result != nullptr) { *result = Result::Success; }
        return Win32SwapChain::GetSize(info, device);
    }
    const Result validation = ValidateSwapChain(info);
    if (result != nullptr) { *result = validation; }
    return (validation == Result::Success) ? sizeof(SwapChain) : 0;
}

Result SwapChain::Create(Device* device, const SwapChainCreateInfo& info,
                         void* placement, ISwapChain** swapChain)
{
    // Device slot 134 (3134f0): DXGI chain (385d20, 387ae0) or the Win32 chain (37e070).
    if ((device != nullptr) && (info.wsiPlatform != WsiPlatform::Dxgi))
    {
        return Win32SwapChain::Create(device, info, placement, swapChain);
    }
    if ((device == nullptr) || (placement == nullptr) || (swapChain == nullptr))
    { return Result::ErrorInvalidPointer; }
    *swapChain = nullptr;
    Result result = ValidateSwapChain(info);
    if (result == Result::Success)
    {
        auto* object = PAL_PLACEMENT_NEW(placement) SwapChain(device, info);
        result = object->InitNative();
        if (result == Result::Success) { *swapChain = object; }
        else { object->Destroy(); }
    }
    return result;
}

SwapChain::SwapChain(Device* device, const SwapChainCreateInfo& info)
    : Pal::SwapChain(info, device), m_impl(nullptr) {}

SwapChain::~SwapChain()
{
    if (m_impl != nullptr)
    {
        WaitIdle();
        if (m_impl->signalQueue != nullptr)
        {
            m_impl->signalQueue->WaitIdle();
            m_impl->signalQueue->Destroy();
        }
        PAL_SAFE_FREE(m_impl->signalQueueMemory, m_pDevice->GetPlatform());
        for (auto* image : m_impl->images) { ReleasePresentableImageState(image); }
        static_cast<Device*>(m_pDevice)->OsTransport().DestroySync(&m_impl->copySync);
        if (m_impl->frameLatency != nullptr) { CloseHandle(m_impl->frameLatency); }
        PAL_DELETE(m_impl, m_pDevice->GetPlatform());
    }
}

Result SwapChain::InitNative()
{
    auto* palDevice = static_cast<Device*>(m_pDevice);
    m_impl = PAL_NEW(Impl, m_pDevice->GetPlatform(), Util::AllocObject);
    if (m_impl == nullptr)
    {
        std::fprintf(stderr, "[pal-wddm-wsi] Allocating native swapchain state failed\n");
        return Result::ErrorOutOfMemory;
    }
    HRESULT status = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_impl->factory));
    if (FAILED(status)) { return ConvertHresult(status, "CreateDXGIFactory2"); }
    ComPtr<IDXGIAdapter1> adapter;
    const auto& identity = palDevice->Adapter();
    const LUID luid = {identity.luidLow, identity.luidHigh};
    status = m_impl->factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(status)) { return ConvertHresult(status, "EnumAdapterByLuid"); }
    status = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_impl->device));
    if (FAILED(status)) { return ConvertHresult(status, "D3D12CreateDevice"); }
    D3D12_COMMAND_QUEUE_DESC queueInfo = {};
    queueInfo.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    status = m_impl->device->CreateCommandQueue(&queueInfo, IID_PPV_ARGS(&m_impl->queue));
    if (FAILED(status)) { return ConvertHresult(status, "CreateCommandQueue"); }
    // 388190: tearing for Immediate chains only; the original does not query DXGI_FEATURE_PRESENT_ALLOW_TEARING.
    m_impl->allowTearing = (m_createInfo.swapChainMode == SwapChainMode::Immediate);

    DXGI_SWAP_CHAIN_DESC1 swapInfo = {};
    swapInfo.Width = m_createInfo.imageExtent.width;
    swapInfo.Height = m_createInfo.imageExtent.height;
    swapInfo.Format = NativeFormat(m_createInfo.imageSwizzledFormat);
    swapInfo.SampleDesc.Count = 1;
    swapInfo.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapInfo.BufferCount = m_createInfo.imageCount;
    swapInfo.SwapEffect = m_createInfo.flags.swapEffectDiscard ? DXGI_SWAP_EFFECT_FLIP_DISCARD
                                                             : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapInfo.Scaling = DXGI_SCALING_STRETCH;
    swapInfo.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapInfo.Flags = m_createInfo.flags.blockOnPresent ? 0 : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_impl->allowTearing) { swapInfo.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; }
    ComPtr<IDXGISwapChain1> swapChain;
    status = m_impl->factory->CreateSwapChainForHwnd(m_impl->queue.Get(), static_cast<HWND>(m_createInfo.hWindow),
                                                     &swapInfo, nullptr, nullptr, &swapChain);
    if (FAILED(status)) { return ConvertHresult(status, "CreateSwapChainForHwnd"); }
    status = swapChain.As(&m_impl->swapChain);
    if (FAILED(status)) { return ConvertHresult(status, "IDXGISwapChain3 query"); }
    // 3885a8: DXGI_MWA_NO_ALT_ENTER only.
    status = m_impl->factory->MakeWindowAssociation(static_cast<HWND>(m_createInfo.hWindow), DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(status)) { return ConvertHresult(status, "MakeWindowAssociation"); }
    // 3885c4..3885f6: the swap chain colour space from the create info (sRGB 0, scRGB 1, HDR10 12, else 0).
    Result result = DxgiStatusToResult(m_impl->swapChain->SetColorSpace1(
        static_cast<DXGI_COLOR_SPACE_TYPE>(HdrSwapChainColorSpace(m_createInfo.colorSpace))));
    if (result != Result::Success) { return result; }
    if (!m_createInfo.flags.blockOnPresent)
    {
        // 388603..3886a3: waitable object, then min(imageCount, frameLatency) without a default for 0, then one
        // two-second wait per frame of latency given up.
        m_impl->frameLatency = m_impl->swapChain->GetFrameLatencyWaitableObject();
        result = SetFrameLatency(std::min(m_createInfo.imageCount, m_createInfo.frameLatency));
        if (result != Result::Success) { return result; }
    }
    status = m_impl->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_impl->copyFence));
    if (FAILED(status)) { return ConvertHresult(status, "CreateFence(copy)"); }
    HANDLE sharedFence = nullptr;
    status = m_impl->device->CreateSharedHandle(m_impl->copyFence.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence);
    if (FAILED(status)) { return ConvertHresult(status, "CreateSharedHandle(copy fence)"); }
    result = ConvertStatus(palDevice->OsTransport().OpenSync(sharedFence, &m_impl->copySync));
    CloseHandle(sharedFence);
    if (result != Result::Success)
    {
        std::fprintf(stderr, "[pal-wddm-wsi] OpenSync(copy fence) PAL=%d\n", static_cast<int>(result));
        return result;
    }
    QueueCreateInfo signalInfo = {};
    signalInfo.queueType = QueueTypeUniversal;
    signalInfo.engineType = EngineTypeUniversal;
    signalInfo.priority = QueuePriority::Normal;
    const size_t queueSize = m_pDevice->GetQueueSize(signalInfo, &result);
    if (result != Result::Success) { return result; }
    m_impl->signalQueueMemory = PAL_MALLOC(queueSize, m_pDevice->GetPlatform(), Util::AllocInternal);
    if (m_impl->signalQueueMemory == nullptr) { return Result::ErrorOutOfMemory; }
    result = m_pDevice->CreateQueue(signalInfo, m_impl->signalQueueMemory, &m_impl->signalQueue);
    if (result != Result::Success) { return result; }
    return Pal::SwapChain::Init(nullptr, false);
}

void SwapChain::GetPresentableImageSizes(Device* device, const PresentableImageCreateInfo& info,
                                         size_t* imageSize, size_t* memorySize, Result* result)
{
    if ((device != nullptr) && IsWin32SwapChain(info.pSwapChain))
    {
        GetWin32PresentableImageSizes(device, info, imageSize, memorySize, result);
        return;
    }
    Result value = ((device == nullptr) || (imageSize == nullptr) || (memorySize == nullptr))
                 ? Result::ErrorInvalidPointer : ValidatePresentable(info);
    if (imageSize != nullptr) { *imageSize = 0; }
    if (memorySize != nullptr) { *memorySize = 0; }
    if (value == Result::Success)
    {
        const ImageCreateInfo imageInfo = ImageInfo(info);
        *imageSize = device->GetImageSize(imageInfo, &value);
        if (value == Result::Success) { *memorySize = device->GpuMemoryObjectSize(); }
    }
    if (result != nullptr) { *result = value; }
}

Result SwapChain::CreatePresentableImage(Device* device, const PresentableImageCreateInfo& info,
                                         void* imagePlacement, void* memoryPlacement,
                                         IImage** image, IGpuMemory** memory)
{
    if ((device != nullptr) && IsWin32SwapChain(info.pSwapChain))
    {
        return CreateWin32PresentableImage(device, info, imagePlacement, memoryPlacement, image, memory);
    }
    if ((device == nullptr) || (imagePlacement == nullptr) || (memoryPlacement == nullptr) ||
        (image == nullptr) || (memory == nullptr)) { return Result::ErrorInvalidPointer; }
    *image = nullptr;
    *memory = nullptr;
    Result result = ValidatePresentable(info);
    if (result != Result::Success) { return result; }
    auto* swapChain = static_cast<SwapChain*>(info.pSwapChain);
    if ((swapChain->m_pDevice != device) || (swapChain->m_impl == nullptr))
    { return Result::ErrorIncompatibleDevice; }
    if ((info.extent.width != swapChain->m_createInfo.imageExtent.width) ||
        (info.extent.height != swapChain->m_createInfo.imageExtent.height) ||
        !(info.swizzledFormat == swapChain->m_createInfo.imageSwizzledFormat))
    { return Result::ErrorInvalidValue; }
    Util::MutexAuto lock(&swapChain->m_impl->mutex);
    uint32 index = 0;
    while ((index < swapChain->m_createInfo.imageCount) &&
           (swapChain->m_impl->images[index] != nullptr) &&
           (swapChain->m_impl->images[index]->references.load(std::memory_order_acquire) > 1)) { ++index; }
    if (index == swapChain->m_createInfo.imageCount) { return Result::ErrorUnavailable; }
    if (swapChain->m_impl->images[index] != nullptr)
    {
        result = swapChain->WaitCopy(swapChain->m_impl->images[index]->lastCopy,
                                     std::chrono::nanoseconds::max());
        if (result != Result::Success) { return result; }
    }

    const ImageCreateInfo createInfo = ImageInfo(info);
    const ImageInternalCreateInfo internalInfo = {};
    Pal::Image* palImage = nullptr;
    result = device->CreateInternalImage(createInfo, internalInfo, imagePlacement, &palImage);
    TraceResult(result, "CreateInternalImage(present)");
    if (result != Result::Success) { return result; }
    auto* state = PAL_NEW(PresentableImageState, device->GetPlatform(), Util::AllocObject)(device);
    if (state == nullptr) { palImage->Destroy(); return Result::ErrorOutOfMemory; }
    state->swapChain = swapChain;
    state->index = index;
    D3D12_RESOURCE_DESC textureInfo = {};
    textureInfo.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureInfo.Width = info.extent.width;
    textureInfo.Height = info.extent.height;
    textureInfo.DepthOrArraySize = 1;
    textureInfo.MipLevels = 1;
    textureInfo.Format = NativeFormat(info.swizzledFormat);
    textureInfo.SampleDesc.Count = 1;
    UINT64 totalBytes = 0;
    auto* nativeDevice = swapChain->m_impl->device.Get();
    nativeDevice->GetCopyableFootprints(&textureInfo, 0, 1, 0, &state->footprint, nullptr, nullptr, &totalBytes);
    const SubResourceInfo* subresource = palImage->SubresourceInfo(0);
    if ((subresource->rowPitch != state->footprint.Footprint.RowPitch) || (subresource->offset != 0))
    {
        if (std::getenv("PAL_WDDM_TRACE") != nullptr)
        {
            std::fprintf(stderr, "[pal-wddm-wsi] Image footprint PAL=%llu offset=%llu D3D12=%u\n",
                         subresource->rowPitch, subresource->offset, state->footprint.Footprint.RowPitch);
        }
        result = Result::Unsupported;
    }
    GpuMemoryRequirements requirements = {};
    palImage->GetGpuMemoryRequirements(&requirements);
    const uint64 allocationSize = Util::Pow2Align(std::max<uint64>(requirements.size, totalBytes), uint64{65536});
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bufferInfo = {};
    bufferInfo.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferInfo.Width = allocationSize;
    bufferInfo.Height = 1;
    bufferInfo.DepthOrArraySize = 1;
    bufferInfo.MipLevels = 1;
    bufferInfo.SampleDesc.Count = 1;
    bufferInfo.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (result == Result::Success)
    {
        result = ConvertHresult(nativeDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &bufferInfo,
                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&state->resource)),
                                "CreateCommittedResource(present buffer)");
    }
    if (result == Result::Success)
    { result = ConvertHresult(nativeDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                   IID_PPV_ARGS(&state->allocator))); }
    if ((result == Result::Success) && (swapChain->m_impl->commandList == nullptr))
    {
        result = ConvertHresult(nativeDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                   state->allocator.Get(), nullptr, IID_PPV_ARGS(&swapChain->m_impl->commandList)));
        if (result == Result::Success) { result = ConvertHresult(swapChain->m_impl->commandList->Close()); }
    }
    if (result == Result::Success)
    { result = ConvertHresult(nativeDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state->readyFence))); }
    HANDLE shared = nullptr;
    if (result == Result::Success)
    { result = ConvertHresult(nativeDevice->CreateSharedHandle(state->readyFence.Get(), nullptr, GENERIC_ALL, nullptr, &shared)); }
    if (result == Result::Success)
    {
        ExternalQueueSemaphoreOpenInfo openInfo = {};
        openInfo.externalSemaphore = shared;
        openInfo.flags.sharedViaNtHandle = 1;
        openInfo.flags.isReference = 1;
        openInfo.flags.timeline = 1;
        const size_t semaphoreSize = device->GetExternalSharedQueueSemaphoreSize(openInfo, &result);
        if (result == Result::Success)
        {
            state->readySemaphoreMemory = PAL_MALLOC(semaphoreSize, device->GetPlatform(), Util::AllocObject);
            if (state->readySemaphoreMemory == nullptr) { result = Result::ErrorOutOfMemory; }
        }
        if (result == Result::Success)
        {
            result = device->OpenExternalSharedQueueSemaphore(openInfo, state->readySemaphoreMemory,
                                                               &state->readySemaphore);
        }
        CloseHandle(shared);
        shared = nullptr;
    }
    if (result == Result::Success)
    { result = ConvertHresult(nativeDevice->CreateSharedHandle(state->resource.Get(), nullptr, GENERIC_ALL, nullptr, &shared)); }
    Pal::GpuMemory* palMemory = nullptr;
    if (result == Result::Success)
    {
        GpuMemoryCreateInfo memoryInfo = {};
        memoryInfo.size = allocationSize;
        memoryInfo.alignment = std::max<uint64>(requirements.alignment, 65536);
        memoryInfo.priority = GpuMemPriority::High;
        memoryInfo.heapCount = 1;
        memoryInfo.heaps[0] = GpuHeapLocal;
        memoryInfo.flags.presentable = 1;
        memoryInfo.pImage = palImage;
        GpuMemoryInternalCreateInfo memoryInternal = {};
        memoryInternal.flags.isExternal = 1;
        memoryInternal.flags.isClient = 1;
        memoryInternal.hExternalResource = shared;
        result = device->CreateInternalGpuMemory(memoryInfo, memoryInternal, memoryPlacement, &palMemory);
        TraceResult(result, "CreateInternalGpuMemory(present import)");
        CloseHandle(shared);
        shared = nullptr;
    }
    if (result == Result::Success) { result = palImage->BindGpuMemory(palMemory, 0); }
    if (result == Result::Success)
    {
        ReleasePresentableImageState(swapChain->m_impl->images[index]);
        swapChain->m_impl->images[index] = state;
        static_cast<Image*>(palImage)->AttachPresentationState(state);
        *image = palImage;
        *memory = palMemory;
    }
    else
    {
        if (shared != nullptr) { CloseHandle(shared); }
        if (palMemory != nullptr) { palMemory->Destroy(); }
        palImage->Destroy();
        ReleasePresentableImageState(state);
    }
    return result;
}

Result SwapChain::WaitCopy(uint64 serial, std::chrono::nanoseconds timeout)
{
    if (serial == 0) { return Result::Success; }
    if (FAILED(m_impl->device->GetDeviceRemovedReason())) { return Result::ErrorDeviceLost; }
    const uint64 ns = (timeout == std::chrono::nanoseconds::max()) ? UINT64_MAX :
                      static_cast<uint64>(std::max<int64_t>(timeout.count(), 0));
    return ConvertStatus(static_cast<Device*>(m_pDevice)->OsTransport().WaitCpu(m_impl->copySync, serial, ns));
}

Result SwapChain::AcquireNextImage(const AcquireNextImageInfo& info, uint32* imageIndex)
{
    if (imageIndex == nullptr) { return Result::ErrorInvalidPointer; }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 863
    const std::chrono::nanoseconds timeout = info.timeout;
#else
    const std::chrono::nanoseconds timeout{std::min<uint64>(info.timeout, INT64_MAX)};
#endif
    if (timeout.count() < 0) { return Result::ErrorInvalidValue; }
    const auto start = std::chrono::steady_clock::now();
    const auto semaphoreTimeout = (timeout == std::chrono::nanoseconds::max())
                                  ? std::chrono::milliseconds::max()
                                  : std::chrono::milliseconds{WaitMilliseconds(timeout)};
    Result result = m_availableImageSemaphore.Wait(semaphoreTimeout);
    if (result != Result::Success) { return (timeout.count() == 0) ? Result::NotReady : result; }
    uint32 index = 0;
    {
        Util::MutexAuto lock(&m_unusedImageMutex);
        index = m_unusedImageQueue[0];
        for (uint32 i = 1; i < m_unusedImageCount; ++i) { m_unusedImageQueue[i - 1] = m_unusedImageQueue[i]; }
        --m_unusedImageCount;
    }
    {
        Util::MutexAuto lock(&m_impl->mutex);
        auto* state = m_impl->images[index];
        result = ((state != nullptr) && (state->references.load(std::memory_order_acquire) > 1))
                 ? WaitCopy(state->lastCopy, Remaining(start, timeout)) : Result::ErrorUnavailable;
        if ((result == Result::Success) && (m_impl->frameLatency != nullptr) &&
            (m_impl->copies > m_impl->acquiredAfterCopy))
        {
            const DWORD wait = WaitForSingleObject(m_impl->frameLatency, WaitMilliseconds(Remaining(start, timeout)));
            result = (wait == WAIT_OBJECT_0) ? Result::Success :
                     (wait == WAIT_TIMEOUT) ? Result::Timeout : Result::ErrorUnknown;
            if (result == Result::Success) { m_impl->acquiredAfterCopy = m_impl->copies; }
        }
        if ((result == Result::Success) && (info.pSemaphore != nullptr))
        { result = m_impl->signalQueue->SignalQueueSemaphore(info.pSemaphore, 0); }
        if ((result == Result::Success) && (info.pFence != nullptr))
        {
            const PerSubQueueSubmitInfo subQueue = {};
            IFence* fence = info.pFence;
            MultiSubmitInfo submit = {};
            submit.perSubQueueInfoCount = 1;
            submit.pPerSubQueueInfo = &subQueue;
            submit.fenceCount = 1;
            submit.ppFences = &fence;
            result = m_impl->signalQueue->Submit(submit);
        }
        if (result == Result::Success) { state->acquired = true; *imageIndex = index; }
    }
    if (result != Result::Success) { ReuseImage(index); }
    return ((result == Result::Timeout) && (timeout.count() == 0)) ? Result::NotReady : result;
}

Result SwapChain::Present(const PresentSwapChainInfo& info, IQueue* queue)
{
    // Native DXGI swap chain slot 16 (388be0) with the present 3865d0. This chain always copies the presentable image
    // into the current DXGI back buffer (the original's intermediate-copy mode, +0x3d0 bit 0), so the back-buffer
    // index check of 388bf1..388c08 always passes; the present mode is recorded, not checked.
    if ((queue == nullptr) || (info.pSrcImage == nullptr)) { return Result::ErrorInvalidPointer; }
    // The original reads the rectangles without a null check.
    if ((info.rectangleCount != 0) && (info.pRectangles == nullptr)) { return Result::ErrorInvalidPointer; }
    // 3865f2..386647: Fifo and FifoRelaxed present on the next vertical blank, the override wins; tearing only for
    // Immediate chains presenting immediately; temporary mono only on stereo chains. turboSyncEnabled is not read.
    uint32 interval = ((m_createInfo.swapChainMode == SwapChainMode::Fifo) ||
                       (m_createInfo.swapChainMode == SwapChainMode::FifoRelaxed)) ? 1 : 0;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 872
    if (info.flags.syncIntervalOverride) { interval = info.syncInterval; }
#else
    if (info.flags.syncInterval != 0) { interval = info.flags.syncInterval; }
#endif
    UINT presentFlags = ((m_createInfo.swapChainMode == SwapChainMode::Immediate) && (interval == 0))
                      ? DXGI_PRESENT_ALLOW_TEARING : 0;
    if (info.flags.isTemporaryMono && m_createInfo.flags.isDxgiStereo) { presentFlags |= DXGI_PRESENT_STEREO_TEMPORARY_MONO; }
    // 3866aa..386738: {x, y, x + width, y + height}, unvalidated; DXGI judges them.
    std::vector<RECT> rectangles;
    for (uint32 i = 0; i < info.rectangleCount; ++i)
    {
        const Rect& rectangle = info.pRectangles[i];
        rectangles.push_back({rectangle.offset.x, rectangle.offset.y,
                              rectangle.offset.x + static_cast<LONG>(rectangle.extent.width),
                              rectangle.offset.y + static_cast<LONG>(rectangle.extent.height)});
    }
    Result result = Result::Success;
    {
        Util::MutexAuto lock(&m_impl->mutex);
        auto* state = static_cast<Image*>(info.pSrcImage)->PresentationState();
        if ((state == nullptr) || (state != m_impl->images[info.imageIndex]) ||
            (state->swapChain != this) || !state->acquired) { return Result::ErrorInvalidValue; }
        // 388c25: the presenting queue signals the semaphore the copy queue waits on.
        const uint64 ready = state->readySerial + 1;
        result = queue->SignalQueueSemaphore(state->readySemaphore, ready);
        if (result == Result::Success)
        {
            state->readySerial = ready;
            result = ConvertHresult(m_impl->queue->Wait(state->readyFence.Get(), ready));
        }
        ComPtr<ID3D12Resource> backBuffer;
        if (result == Result::Success)
        { result = ConvertHresult(m_impl->swapChain->GetBuffer(m_impl->swapChain->GetCurrentBackBufferIndex(),
                                                               IID_PPV_ARGS(&backBuffer))); }
        if (result == Result::Success) { result = ConvertHresult(state->allocator->Reset()); }
        if (result == Result::Success)
        { result = ConvertHresult(m_impl->commandList->Reset(state->allocator.Get(), nullptr)); }
        if (result == Result::Success)
        {
            // 387280: the whole intermediate image goes to the back buffer (CopyResource in the original).
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backBuffer.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            m_impl->commandList->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = state->resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = state->footprint;
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = backBuffer.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            m_impl->commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            m_impl->commandList->ResourceBarrier(1, &barrier);
            result = ConvertHresult(m_impl->commandList->Close());
        }
        if (result == Result::Success)
        {
            ID3D12CommandList* lists[] = {m_impl->commandList.Get()};
            m_impl->queue->ExecuteCommandLists(1, lists);
            const uint64 serial = m_impl->copies + 1;
            result = ConvertHresult(m_impl->queue->Signal(m_impl->copyFence.Get(), serial));
            if (result == Result::Success) { m_impl->copies = serial; state->lastCopy = serial; }
        }
        if (result == Result::Success)
        {
            // 386738..3867b6: Present1 with the dirty rectangles, else Present; then GetLastPresentCount.
            HRESULT status = S_OK;
            if (!rectangles.empty())
            {
                DXGI_PRESENT_PARAMETERS parameters = {};
                parameters.DirtyRectsCount = static_cast<UINT>(rectangles.size());
                parameters.pDirtyRects = rectangles.data();
                status = m_impl->swapChain->Present1(interval, presentFlags, &parameters);
            }
            else
            {
                status = m_impl->swapChain->Present(interval, presentFlags);
            }
            result = DxgiStatusToResult(status);
            if (result == Result::Success)
            {
                UINT presentCount = 0;
                result = DxgiStatusToResult(m_impl->swapChain->GetLastPresentCount(&presentCount));
            }
        }
        state->acquired = false;
        m_impl->lastPresentMode = info.presentMode;
    }
    ReuseImage(info.imageIndex);
    // 388c5a..388cb6: PresentConcluded {Flip for fullscreen presents, else Blit}.
    ReportPresentConcluded(*static_cast<Device*>(m_pDevice),
                           static_cast<uint32>((info.presentMode == PresentMode::Fullscreen) ?
                                               Developer::PresentModeType::Flip : Developer::PresentModeType::Blit),
                           m_createInfo.hWindow);
    return result;
}

Result SwapChain::WaitIdle()
{
    if ((m_impl == nullptr) || (m_impl->device == nullptr)) { return Result::Success; }
    Util::MutexAuto lock(&m_impl->mutex);
    return WaitCopy(m_impl->copies, std::chrono::nanoseconds::max());
}

Result SwapChain::GetMaximumFrameLatency(uint32* latency)
{
    // Native DXGI slot 7 (388850).
    if (latency == nullptr) { return Result::ErrorInvalidPointer; }
    if (m_createInfo.flags.blockOnPresent) { return Result::Unsupported; }
    Util::MutexAuto lock(&m_impl->mutex);
    return DxgiStatusToResult(m_impl->swapChain->GetMaximumFrameLatency(latency));
}

Result SwapChain::SetMaximumFrameLatency(uint32 latency)
{
    // Native DXGI slot 8 (3888a0): no range check, 0 is passed to DXGI.
    if (m_createInfo.flags.blockOnPresent) { return Result::Unsupported; }
    return SetFrameLatency(latency);
}

Result SwapChain::SetFrameLatency(uint32 latency)
{
    // 3888d8..38896a: read the old latency, set the new one and consume one waitable-object signal (two-second
    // timeout each) for every frame of latency given up.
    UINT previous = 0;
    Result result = Result::Success;
    {
        Util::MutexAuto lock(&m_impl->mutex);
        result = DxgiStatusToResult(m_impl->swapChain->GetMaximumFrameLatency(&previous));
        if (result == Result::Success) { result = DxgiStatusToResult(m_impl->swapChain->SetMaximumFrameLatency(latency)); }
    }
    for (uint32 frame = latency; (result == Result::Success) && (frame < previous); ++frame)
    {
        const DWORD wait = WaitForSingleObjectEx(m_impl->frameLatency, 2000, FALSE);
        result = (wait == WAIT_TIMEOUT) ? Result::Timeout : ((wait != WAIT_OBJECT_0) ? Result::ErrorUnknown : Result::Success);
    }
    return result;
}

Result SwapChain::SetHdrMetaData(const ScreenColorConfig& colorConfig)
{
    // Native 388670: only the DXGI-platform swap chain implements this; the
    // Win32-platform class (656aed0 slot 6 = 26dde0) returns Unsupported.
    if (m_createInfo.wsiPlatform != WsiPlatform::Dxgi) { return Result::Unsupported; }
    static_assert(sizeof(Hdr10Metadata) == sizeof(DXGI_HDR_METADATA_HDR10));
    static_assert(offsetof(DXGI_HDR_METADATA_HDR10, MaxMasteringLuminance) == offsetof(Hdr10Metadata, maxMasteringLuminance));
    static_assert(offsetof(DXGI_HDR_METADATA_HDR10, MinMasteringLuminance) == offsetof(Hdr10Metadata, minMasteringLuminance));
    static_assert(offsetof(DXGI_HDR_METADATA_HDR10, MaxContentLightLevel) == offsetof(Hdr10Metadata, maxContentLightLevel));
    static_assert(DXGI_HDR_METADATA_TYPE_HDR10 == 1);
    Util::MutexAuto lock(&m_impl->mutex);
    // The native object holds IDXGISwapChain4 from creation (QueryInterface at 38834a).
    ComPtr<IDXGISwapChain4> swapChain;
    const HRESULT status = m_impl->swapChain.As(&swapChain);
    if (FAILED(status)) { return DxgiStatusToResult(status); }
    const HdrMetaDataCalls calls =
    {
        swapChain.Get(),
        [](void* context, uint32 colorSpace) -> int32
        {
            return static_cast<IDXGISwapChain4*>(context)->SetColorSpace1(static_cast<DXGI_COLOR_SPACE_TYPE>(colorSpace));
        },
        [](void* context, uint32 type, uint32 bytes, const void* data) -> int32
        {
            return static_cast<IDXGISwapChain4*>(context)->SetHDRMetaData(static_cast<DXGI_HDR_METADATA_TYPE>(type),
                                                                          bytes, const_cast<void*>(data));
        },
    };
    return ApplyHdrMetaData(calls, colorConfig);
}

Result SwapChain::Resize(uint32 width, uint32 height)
{
    if ((width == 0) || (height == 0) || (width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
        (height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)) { return Result::ErrorInvalidValue; }
    Util::MutexAuto lock(&m_impl->mutex);
    for (auto* image : m_impl->images)
    {
        if ((image != nullptr) && (image->references.load(std::memory_order_acquire) > 1))
        { return Result::ErrorInvalidValue; }
    }
    Result result = WaitCopy(m_impl->copies, std::chrono::nanoseconds::max());
    if (result == Result::Success)
    {
        const UINT flags = (m_createInfo.flags.blockOnPresent ? 0 : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) |
                           (m_impl->allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        result = ConvertHresult(m_impl->swapChain->ResizeBuffers(m_createInfo.imageCount, width, height,
                              NativeFormat(m_createInfo.imageSwizzledFormat), flags));
    }
    if (result == Result::Success)
    {
        for (auto*& image : m_impl->images) { ReleasePresentableImageState(image); image = nullptr; }
        m_createInfo.imageExtent = {width, height};
        Util::MutexAuto imageLock(&m_unusedImageMutex);
        const uint32 returned = m_createInfo.imageCount - m_unusedImageCount;
        m_unusedImageCount = m_createInfo.imageCount;
        for (uint32 i = 0; i < m_unusedImageCount; ++i) { m_unusedImageQueue[i] = i; }
        if (returned != 0) { m_availableImageSemaphore.Post(returned); }
    }
    return result;
}

Result SwapChain::QueryProperties(OsWindowHandle window, WsiPlatform platform,
                                  SwapChainProperties* properties)
{
    // Device slot 35 (3132b0): GetClientRect of the window for every WSI platform. The original writes through the
    // pointer without a check.
    if (properties == nullptr) { return Result::ErrorInvalidPointer; }
    RECT client = {};
    const bool valid = GetClientRect(static_cast<HWND>(window), &client) != FALSE;
    const int32 rect[4] = { client.left, client.top, client.right, client.bottom };
    return SwapChainPropertiesFromClientRect(valid, rect, platform, properties);
}

Result SwapChainPropertiesFromClientRect(bool clientRectValid, const int32 rect[4], WsiPlatform platform,
                                         SwapChainProperties* properties)
{
    // 3132b0: only these fields are written (the caller's other fields are kept). Opaque composite alpha is written
    // before the window is queried.
    properties->compositeAlphaMode = static_cast<uint32>(CompositeAlphaMode::Opaque);
    if (!clientRectValid) { return Result::ErrorUnknown; }
    const bool dxgi = (platform == WsiPlatform::Dxgi);
    const uint32 width = uint32(rect[2] - rect[0]);
    const uint32 height = uint32(rect[3] - rect[1]);
    properties->minImageCount = dxgi ? 2 : 1;
    properties->maxImageCount = MaxSwapChainLength;
    properties->currentExtent = { width, height };
    properties->minImageExtent = { width, height };
    properties->maxImageExtent = { width, height };
    properties->supportedTransforms = SurfaceTransformNone;
    properties->currentTransforms = SurfaceTransformNone;
    properties->maxImageArraySize = 1;
    properties->supportedUsageFlags.u32All = 0;
    properties->supportedUsageFlags.shaderRead = 1;
    properties->supportedUsageFlags.shaderWrite = 1;
    properties->supportedUsageFlags.colorTarget = 1;
    properties->imageFormatCount = 4;
    const ChannelMapping rgba = {{{ChannelSwizzle::X, ChannelSwizzle::Y, ChannelSwizzle::Z, ChannelSwizzle::W}}};
    const ChannelMapping bgra = {{{ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::W}}};
    properties->imageFormat[0] = {ChNumFormat::X8Y8Z8W8_Unorm, rgba};
    properties->imageFormat[1] = {ChNumFormat::X8Y8Z8W8_Unorm, bgra};
    if (dxgi)
    {
        properties->imageFormat[2] = {ChNumFormat::X16Y16Z16W16_Float, rgba};
        properties->imageFormat[3] = {ChNumFormat::X10Y10Z10W2_Unorm, rgba};
        properties->colorSpace[0] = static_cast<ScreenColorSpace>(CsSrgb | TfSrgb);
        properties->colorSpace[1] = static_cast<ScreenColorSpace>(CsScrgb | TfLinear0_125);
        properties->colorSpace[2] = static_cast<ScreenColorSpace>(CsBt2020 | TfPq2084);
        properties->colorSpaceCount = 3;
    }
    else
    {
        properties->imageFormat[2] = {ChNumFormat::X8Y8Z8W8_Srgb, rgba};
        properties->imageFormat[3] = {ChNumFormat::X8Y8Z8W8_Srgb, bgra};
        properties->colorSpaceCount = 0;
    }
    return Result::Success;
}

} // namespace Pal::Wddm
