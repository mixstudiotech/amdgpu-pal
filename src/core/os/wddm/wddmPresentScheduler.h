// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Non-DXGI ("Win32") swap chain of the original Windows PAL: the core swap chain (4dc790..4dce00) with the Windows
// present scheduler (vtable 6575a78, base 6575ab8) embedded at +0x218, created by Device::CreateSwapChain (slot 134,
// 3134f0 -> 37e070) for every WSI platform other than DXGI. Presents reach the queue presenters through
// Queue::PresentDirectInternal. Notes: .build/pal-wddm-logic/present/analysis.txt.
#pragma once

#include "palDevice.h"
#include "core/presentScheduler.h"
#include "core/swapChain.h"

namespace Pal
{
class IFence;
struct PresentableImageCreateInfo;
}

namespace Pal::Wddm
{
class Device;

class WindowsPresentScheduler final : public Pal::PresentScheduler
{
public:
    // 4dd160 without the object itself: fence + signal queue + present queue placement.
    static size_t GetPlacementSize(const Device& device);
    WindowsPresentScheduler(Device* device, OsWindowHandle window);
    Result Init(IDevice* const slaveDevices[], void* placement) override; // 4dd500

protected:
    ~WindowsPresentScheduler() override;
    bool   CanInlinePresent(const PresentSwapChainInfo& info, const IQueue& queue) const override; // 4dd7e0
    Result ProcessPresent(const PresentSwapChainInfo& info, IQueue* queue, bool isInline) override; // 4dd810
    Result FailedToQueuePresentJob(const PresentSwapChainInfo& info, IQueue* queue) override;      // 4dd8f0

private:
    Result WindowedPresent(const PresentSwapChainInfo& info, IQueue* queue, bool vsync);   // 4dd910
    Result FullscreenPresent(const PresentSwapChainInfo& info, IQueue* queue, bool vsync); // 4dda80
    Result WaitForVerticalBlank();                                                          // 4dde10

    Device*         m_wddmDevice;
    OsWindowHandle  m_window;                     // +0xf0
    IFence*         m_fence = nullptr;            // +0xf8
    Pal::SwapChain* m_pendingSwapChain = nullptr; // +0x100: image held by the last fullscreen flip
    uint32          m_pendingIndex = 0;           // +0x108
    IImage*         m_lastFlipImage = nullptr;    // +0x110
    uint32          m_syncRefreshCount = 0;       // +0x118 (FifoRelaxed)
};

class Win32SwapChain final : public Pal::SwapChain
{
public:
    // Device slot 133 (313490): 4dc710 + 4dd160 + 0x218, nothing validated.
    static size_t GetSize(const SwapChainCreateInfo& createInfo, const Device& device);
    // 37e070
    static Result Create(Device* device, const SwapChainCreateInfo& createInfo, void* placement,
                         ISwapChain** swapChain);

private:
    Win32SwapChain(const SwapChainCreateInfo& createInfo, Device* device);
    ~Win32SwapChain() override {}
    Result InitWin32(void* placement); // 37e260

    PAL_DISALLOW_DEFAULT_CTOR(Win32SwapChain);
    PAL_DISALLOW_COPY_AND_ASSIGN(Win32SwapChain);
};

// Presentable images of non-DXGI swap chains (presentable image creation 381dd0, windowed images).
bool   IsWin32SwapChain(const ISwapChain* swapChain);
void   GetWin32PresentableImageSizes(Device* device, const PresentableImageCreateInfo& createInfo,
                                     size_t* imageSize, size_t* memorySize, Result* result);
Result CreateWin32PresentableImage(Device* device, const PresentableImageCreateInfo& createInfo,
                                   void* imagePlacement, void* memoryPlacement, IImage** image,
                                   IGpuMemory** memory);
} // namespace Pal::Wddm
