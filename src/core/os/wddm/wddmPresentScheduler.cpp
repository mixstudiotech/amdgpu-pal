// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmPresentScheduler.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmPresent.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmResult.h"
#include "core/cmdBuffer.h"
#include "core/gpuMemory.h"
#include "core/image.h"
#include "core/queue.h"
#include "palFence.h"
#include "palQueue.h"
#include "palSysMemory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
namespace
{
// 4dd1b4..4dd21e / 4dd52f..4dd5c4: DMA when available, else the first non-exclusive compute engine, else universal.
QueueCreateInfo SignalQueueInfo(const Device& device)
{
    QueueCreateInfo info = {};
    info.submitOptMode = SubmitOptMode::Disabled;
    const auto& engines = device.EngineProperties().perEngine;
    if (engines[EngineTypeDma].numAvailable != 0)
    {
        info.queueType = QueueTypeDma;
        info.engineType = EngineTypeDma;
        return info;
    }
    for (uint32 index = 0; index < engines[EngineTypeCompute].numAvailable; ++index)
    {
        if (engines[EngineTypeCompute].capabilities[index].flags.exclusive == 0)
        {
            info.queueType = QueueTypeCompute;
            info.engineType = EngineTypeCompute;
            info.engineIndex = index;
            return info;
        }
    }
    // Universal (also the zeroed record when no universal engine exists).
    return info;
}

// 4dd5e4..4dd660: compute only when its direct present modes equal the universal queue's.
QueueCreateInfo PresentQueueInfo(const Device& device)
{
    QueueCreateInfo info = {};
    info.submitOptMode = SubmitOptMode::Disabled;
    const auto& queues = device.QueueProperties().perQueue;
    const auto& engines = device.EngineProperties().perEngine;
    if (queues[QueueTypeCompute].supportedDirectPresentModes == queues[QueueTypeUniversal].supportedDirectPresentModes)
    {
        for (uint32 index = 0; index < engines[EngineTypeCompute].numAvailable; ++index)
        {
            if (engines[EngineTypeCompute].capabilities[index].flags.exclusive == 0)
            {
                info.queueType = QueueTypeCompute;
                info.engineType = EngineTypeCompute;
                info.engineIndex = index;
                return info;
            }
        }
    }
    return info;
}

Pal::SwapChain* ChainOf(const PresentSwapChainInfo& info)
{
    return static_cast<Pal::SwapChain*>(info.pSwapChain);
}

// 381dd0, windowed presentable images: a presentable 2D image of the requested format and extent.
ImageCreateInfo PresentableImageInfo(const PresentableImageCreateInfo& info)
{
    ImageCreateInfo image = {};
    image.flags.presentable = 1;
    image.usageFlags = info.usage;
    image.imageType = ImageType::Tex2d;
    image.swizzledFormat = info.swizzledFormat;
    image.extent = { info.extent.width, info.extent.height, 1 };
    image.mipLevels = 1;
    image.arraySize = 1;
    image.samples = 1;
    image.fragments = 1;
    image.tiling = ImageTiling::Optimal;
    image.viewFormatCount = info.viewFormatCount;
    image.pViewFormats = info.pViewFormats;
    return image;
}

Result ValidateWin32Presentable(const PresentableImageCreateInfo& info)
{
    // Fullscreen (primary) and stereo presentable images need KMD primary allocations, which this backend does not
    // create; TMZ is refused as for every other allocation.
    if (info.flags.fullscreen || info.flags.stereo || info.flags.tmzProtected) { return Result::Unsupported; }
    if ((info.extent.width == 0) || (info.extent.height == 0)) { return Result::ErrorInvalidValue; }
    return Result::Success;
}
} // anonymous namespace

// =====================================================================================================================
WindowsPresentScheduler::WindowsPresentScheduler(Device* device, OsWindowHandle window)
    :
    Pal::PresentScheduler(device),
    m_wddmDevice(device),
    m_window(window)
{
}

WindowsPresentScheduler::~WindowsPresentScheduler()
{
    // The core destructor stops the worker and destroys the queues; the fence lives in the same placement.
    if (m_fence != nullptr)
    {
        m_fence->Destroy();
        m_fence = nullptr;
    }
}

size_t WindowsPresentScheduler::GetPlacementSize(const Device& device)
{
    // 4dd160: fence, signal queue, present queue (the presentId timeline semaphore of the original is omitted:
    // this interface version has no present ids).
    size_t size = device.GetFenceSize(nullptr);
    size += device.GetQueueSize(SignalQueueInfo(device), nullptr);
    size += device.GetQueueSize(PresentQueueInfo(device), nullptr);
    return size;
}

Result WindowsPresentScheduler::Init(IDevice* const slaveDevices[], void* placement)
{
    // 4dd500
    const FenceCreateInfo fenceInfo = {};
    Result result = m_wddmDevice->CreateFence(fenceInfo, placement, &m_fence);
    placement = Util::VoidPtrInc(placement, m_wddmDevice->GetFenceSize(nullptr));
    if (result == Result::Success)
    {
        const QueueCreateInfo signalInfo = SignalQueueInfo(*m_wddmDevice);
        result = m_wddmDevice->CreateQueue(signalInfo, placement, &m_pSignalQueue);
        placement = Util::VoidPtrInc(placement, m_wddmDevice->GetQueueSize(signalInfo, nullptr));
    }
    if (result == Result::Success)
    {
        result = m_wddmDevice->CreateQueue(PresentQueueInfo(*m_wddmDevice), placement, &m_pPresentQueues[0]);
    }
    // 4dd73b..4dd7b9: the two worker semaphores (Util::Semaphore in the core).
    if (result == Result::Success) { result = Pal::PresentScheduler::Init(slaveDevices, nullptr); }
    return result;
}

bool WindowsPresentScheduler::CanInlinePresent(const PresentSwapChainInfo& info, const IQueue& queue) const
{
    // 4dd7e0: only presents without vertical sync run on the caller's thread and queue.
    if ((ChainOf(info)->CreateInfo().swapChainMode != SwapChainMode::Immediate) &&
        ((info.flags.syncIntervalOverride == 0) || (info.syncInterval != 0)))
    {
        return false;
    }
    return static_cast<const Pal::Queue&>(queue).IsPresentModeSupported(info.presentMode);
}

Result WindowsPresentScheduler::ProcessPresent(const PresentSwapChainInfo& info, IQueue* queue, bool isInline)
{
    // 4dd810. The present-id signal of the original (PresentSwapChainInfo+0x30) has no field in this interface.
    bool vsync = (ChainOf(info)->CreateInfo().swapChainMode != SwapChainMode::Immediate);
    if (info.flags.syncIntervalOverride != 0) { vsync = (info.syncInterval > 0); }
    return (info.presentMode == PresentMode::Windowed) ? WindowedPresent(info, queue, vsync)
                                                       : FullscreenPresent(info, queue, vsync);
}

Result WindowsPresentScheduler::FailedToQueuePresentJob(const PresentSwapChainInfo& info, IQueue* queue)
{
    // 4dd8f0 -> 4dd040.
    return ChainOf(info)->PresentComplete(queue, info.imageIndex);
}

Result WindowsPresentScheduler::WindowedPresent(const PresentSwapChainInfo& info, IQueue* queue, bool vsync)
{
    // 4dd910
    PresentDirectInfo direct = {};
    direct.hWindow = m_window;
    direct.presentMode = PresentMode::Windowed;
    direct.pSrcImage = info.pSrcImage;
    bool waitAfterPresent = false;
    Result waited = Result::Success;
    if (vsync)
    {
        if (IsCompositionEnabled())
        {
            // The queue's windowed presenter +0x19: KMT presenters take the interval, GDI ones wait afterwards.
            const Presenter* presenter = static_cast<Queue*>(queue)->WindowedPresenter();
            if ((presenter != nullptr) && presenter->IsKmt())
            {
                direct.presentInterval = (info.flags.syncIntervalOverride != 0) ? info.syncInterval : 1;
            }
            else
            {
                waitAfterPresent = true;
            }
        }
        else
        {
            waited = WaitForVerticalBlank();
        }
    }
    Result result = Util::CollapseResults(waited, static_cast<Pal::Queue*>(queue)->PresentDirectInternal(direct, false));
    if (m_pendingSwapChain != nullptr)
    {
        result = Util::CollapseResults(result, m_pendingSwapChain->PresentComplete(queue, m_pendingIndex));
        m_pendingSwapChain = nullptr;
        m_pendingIndex = 0;
    }
    result = Util::CollapseResults(result, ChainOf(info)->PresentComplete(queue, info.imageIndex));
    if (!waitAfterPresent || (result != Result::Success)) { return result; }
    return WaitForVerticalBlank();
}

Result WindowsPresentScheduler::FullscreenPresent(const PresentSwapChainInfo& info, IQueue* queue, bool vsync)
{
    // 4dda80
    Pal::SwapChain* chain = ChainOf(info);
    if (chain->CreateInfo().swapChainMode == SwapChainMode::FifoRelaxed)
    {
        // 4ddaa0..4ddb0c: a missed refresh since the last flip turns vertical sync off for this flip.
        D3DKMT_GETDEVICESTATE state = {};
        state.hDevice = m_wddmDevice->OsTransport().DeviceHandle();
        state.StateType = D3DKMT_DEVICESTATE_PRESENT;
        if (ConvertNativeStatus(D3DKMTGetDeviceState(&state)) == Result::Success)
        {
            const uint32 refreshes = state.PresentState.PresentStats.SyncRefreshCount;
            const bool onTime = refreshes <= (m_syncRefreshCount + 1);
            m_syncRefreshCount = refreshes;
            vsync = onTime && vsync;
        }
    }
    PresentDirectInfo direct = {};
    direct.hWindow = m_window;
    direct.presentMode = PresentMode::Fullscreen;
    direct.presentInterval = (info.flags.syncIntervalOverride != 0) ? info.syncInterval : (vsync ? 1u : 0u);
    direct.pSrcImage = info.pSrcImage;
    Result result = static_cast<Pal::Queue*>(queue)->PresentDirectInternal(direct, false);
    if (vsync)
    {
        // 4ddb3d..4ddbd6: wait (two seconds) until the flip has been queued.
        Result waited = m_wddmDevice->ResetFences(1, &m_fence);
        if (waited == Result::Success)
        {
            const PerSubQueueSubmitInfo perSubQueue = {};
            MultiSubmitInfo submit = {};
            submit.perSubQueueInfoCount = 1;
            submit.pPerSubQueueInfo = &perSubQueue;
            submit.fenceCount = 1;
            submit.ppFences = &m_fence;
            waited = queue->Submit(submit);
        }
        if (waited == Result::Success)
        {
            waited = m_wddmDevice->WaitForFences(1, &m_fence, true, std::chrono::seconds{2});
        }
        result = Util::CollapseResults(result, waited);
    }
    // 4ddbe4..4ddce0: block later rendering to the scanned-out image (the previous one on hybrid discrete
    // adapters) unless the client does it.
    IImage* scanout = ((m_wddmDevice->AdapterPresentFlags() & AdapterHybridDiscrete) != 0) ? m_lastFlipImage
                                                                                           : info.pSrcImage;
    if ((scanout != nullptr) && (chain->CreateInfo().flags.clientBlockIfFlipping == 0))
    {
        const IGpuMemory* memory = static_cast<const Pal::Image*>(scanout)->GetBoundGpuMemory().Memory();
        ICmdBuffer* dummy = static_cast<Pal::Queue*>(queue)->DummyCmdBuffer();
        PerSubQueueSubmitInfo perSubQueue = {};
        perSubQueue.cmdBufferCount = 1;
        perSubQueue.ppCmdBuffers = &dummy;
        MultiSubmitInfo submit = {};
        submit.perSubQueueInfoCount = 1;
        submit.pPerSubQueueInfo = &perSubQueue;
        submit.blockIfFlippingCount = 1;
        submit.ppBlockIfFlipping = &memory;
        Result blocked = Result::Success;
        if (vsync)
        {
            submit.fenceCount = 1;
            submit.ppFences = &m_fence;
            blocked = m_wddmDevice->ResetFences(1, &m_fence);
            if (blocked == Result::Success) { blocked = queue->Submit(submit); }
            if (blocked == Result::Success)
            {
                blocked = m_wddmDevice->WaitForFences(1, &m_fence, true, std::chrono::seconds{2});
            }
        }
        else
        {
            blocked = queue->Submit(submit);
        }
        result = Util::CollapseResults(result, blocked);
    }
    if (m_pendingSwapChain != nullptr)
    {
        result = Util::CollapseResults(result, m_pendingSwapChain->PresentComplete(queue, m_pendingIndex));
    }
    // The flipped image stays on screen and is returned by the next present.
    m_pendingSwapChain = chain;
    m_pendingIndex = info.imageIndex;
    m_lastFlipImage = info.pSrcImage;
    return result;
}

Result WindowsPresentScheduler::WaitForVerticalBlank()
{
    // 4dde10: the window's monitor names the VidPn source; the wait uses the PAL Device's adapter.
    const HWND window = static_cast<HWND>(m_window);
    if (IsWindow(window) == FALSE) { return Result::ErrorUnknown; }
    HDC dc = nullptr;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (monitor != nullptr)
    {
        MONITORINFOEXA monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoA(monitor, &monitorInfo);
        if (monitorInfo.szDevice[0] != '\0') { dc = CreateDCA(nullptr, monitorInfo.szDevice, nullptr, nullptr); }
    }
    D3DKMT_OPENADAPTERFROMHDC open = {};
    open.hDc = dc;
    Result result = ConvertNativeStatus(D3DKMTOpenAdapterFromHdc(&open));
    if (result == Result::Success)
    {
        D3DKMT_CLOSEADAPTER close = {};
        close.hAdapter = open.hAdapter;
        result = ConvertNativeStatus(D3DKMTCloseAdapter(&close));
    }
    if (dc != nullptr) { DeleteDC(dc); }
    if (result != Result::Success) { return result; }
    D3DKMT_WAITFORVERTICALBLANKEVENT wait = {};
    wait.hAdapter = m_wddmDevice->OsTransport().AdapterHandle();
    wait.VidPnSourceId = open.VidPnSourceId;
    return ConvertNativeStatus(D3DKMTWaitForVerticalBlankEvent(&wait));
}

// =====================================================================================================================
Win32SwapChain::Win32SwapChain(const SwapChainCreateInfo& createInfo, Device* device)
    :
    Pal::SwapChain(createInfo, device)
{
}

size_t Win32SwapChain::GetSize(const SwapChainCreateInfo& createInfo, const Device& device)
{
    return sizeof(Win32SwapChain) + sizeof(WindowsPresentScheduler) + WindowsPresentScheduler::GetPlacementSize(device) +
           Pal::SwapChain::GetPlacementSize(createInfo, device, true);
}

Result Win32SwapChain::Create(Device* device, const SwapChainCreateInfo& createInfo, void* placement,
                              ISwapChain** swapChain)
{
    // 37e070
    if ((placement == nullptr) || (swapChain == nullptr)) { return Result::ErrorInvalidPointer; }
    // The core keeps one present-complete semaphore per image in a 16-entry array; the original writes past it.
    if (createInfo.imageCount > MaxSwapChainLength) { return Result::ErrorInvalidValue; }
    auto* object = PAL_PLACEMENT_NEW(placement) Win32SwapChain(createInfo, device);
    const Result result = object->InitWin32(Util::VoidPtrInc(placement, sizeof(Win32SwapChain)));
    if (result == Result::Success) { *swapChain = object; }
    else { object->Destroy(); }
    return result;
}

Result Win32SwapChain::InitWin32(void* placement)
{
    // 37e260: scheduler at +0x218, its queues after it, then the core semaphores (needPresentComplete = true).
    auto* device = static_cast<Device*>(m_pDevice);
    auto* scheduler = PAL_PLACEMENT_NEW(placement) WindowsPresentScheduler(device, m_createInfo.hWindow);
    void* schedulerPlacement = Util::VoidPtrInc(placement, sizeof(WindowsPresentScheduler));
    Result result = scheduler->Init(m_createInfo.pSlaveDevices, schedulerPlacement);
    if (result != Result::Success)
    {
        scheduler->Destroy();
        return result;
    }
    m_pScheduler = scheduler;
    return Pal::SwapChain::Init(Util::VoidPtrInc(schedulerPlacement, WindowsPresentScheduler::GetPlacementSize(*device)),
                                true);
}

// =====================================================================================================================
bool IsWin32SwapChain(const ISwapChain* swapChain)
{
    return (swapChain != nullptr) &&
           (static_cast<const Pal::SwapChain*>(swapChain)->CreateInfo().wsiPlatform != WsiPlatform::Dxgi);
}

void GetWin32PresentableImageSizes(Device* device, const PresentableImageCreateInfo& createInfo,
                                   size_t* imageSize, size_t* memorySize, Result* result)
{
    Result value = ((imageSize == nullptr) || (memorySize == nullptr)) ? Result::ErrorInvalidPointer
                                                                       : ValidateWin32Presentable(createInfo);
    if (imageSize != nullptr) { *imageSize = 0; }
    if (memorySize != nullptr) { *memorySize = 0; }
    if (value == Result::Success)
    {
        *imageSize = device->GetImageSize(PresentableImageInfo(createInfo), &value);
        if (value == Result::Success) { *memorySize = device->GpuMemoryObjectSize(); }
    }
    if (result != nullptr) { *result = value; }
}

Result CreateWin32PresentableImage(Device* device, const PresentableImageCreateInfo& createInfo,
                                   void* imagePlacement, void* memoryPlacement, IImage** image, IGpuMemory** memory)
{
    // 381dd0 -> 382420: image, memory of the image's requirements (priority VeryHigh, presentable), bind at 0.
    if ((imagePlacement == nullptr) || (memoryPlacement == nullptr) || (image == nullptr) || (memory == nullptr))
    {
        return Result::ErrorInvalidPointer;
    }
    *image = nullptr;
    *memory = nullptr;
    Result result = ValidateWin32Presentable(createInfo);
    if (result != Result::Success) { return result; }
    const ImageInternalCreateInfo internalInfo = {};
    Pal::Image* palImage = nullptr;
    result = device->CreateInternalImage(PresentableImageInfo(createInfo), internalInfo, imagePlacement, &palImage);
    if (result != Result::Success) { return result; }
    GpuMemoryRequirements requirements = {};
    palImage->GetGpuMemoryRequirements(&requirements);
    GpuMemoryCreateInfo memoryInfo = {};
    memoryInfo.size = requirements.size;
    memoryInfo.alignment = requirements.alignment;
    memoryInfo.priority = GpuMemPriority::VeryHigh;
    memoryInfo.heapCount = requirements.heapCount;
    for (uint32 heap = 0; heap < requirements.heapCount; ++heap) { memoryInfo.heaps[heap] = requirements.heaps[heap]; }
    memoryInfo.flags.presentable = 1;
    memoryInfo.pImage = palImage;
    const GpuMemoryInternalCreateInfo memoryInternal = {};
    Pal::GpuMemory* palMemory = nullptr;
    result = device->CreateInternalGpuMemory(memoryInfo, memoryInternal, memoryPlacement, &palMemory);
    if (result == Result::Success) { result = palImage->BindGpuMemory(palMemory, 0); }
    if (result != Result::Success)
    {
        if (palMemory != nullptr) { palMemory->Destroy(); }
        palImage->Destroy();
        return result;
    }
    *image = palImage;
    *memory = palMemory;
    if (std::getenv("PAL_WDDM_TRACE") != nullptr)
    {
        std::fprintf(stderr, "[pal-wddm] presentable image va=%llx size=%llx\n",
                     static_cast<unsigned long long>(palMemory->Desc().gpuVirtAddr),
                     static_cast<unsigned long long>(palMemory->Desc().size));
    }
    return result;
}
} // namespace Pal::Wddm
