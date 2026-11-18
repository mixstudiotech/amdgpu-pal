// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "core/swapChain.h"

namespace Pal::Wddm
{
class Device;
class Image;
struct PresentableImageState;

void RetainPresentableImageState(PresentableImageState* state);
void ReleasePresentableImageState(PresentableImageState* state);
// Device slot 35 (3132b0) once GetClientRect has run (rect = left, top, right, bottom).
Result SwapChainPropertiesFromClientRect(bool clientRectValid, const int32 rect[4], WsiPlatform platform,
                                         SwapChainProperties* properties);

class SwapChain final : public Pal::SwapChain
{
public:
    static size_t GetSize(const Device& device, const SwapChainCreateInfo& info, Result* result);
    static Result Create(Device* device, const SwapChainCreateInfo& info,
                         void* placement, ISwapChain** swapChain);
    static void GetPresentableImageSizes(Device* device, const PresentableImageCreateInfo& info,
                                         size_t* imageSize, size_t* memorySize, Result* result);
    static Result CreatePresentableImage(Device* device, const PresentableImageCreateInfo& info,
                                         void* imagePlacement, void* memoryPlacement,
                                         IImage** image, IGpuMemory** memory);
    static Result QueryProperties(OsWindowHandle window, WsiPlatform platform,
                                  SwapChainProperties* properties);

    Result AcquireNextImage(const AcquireNextImageInfo& info, uint32* imageIndex) override;
    Result Present(const PresentSwapChainInfo& info, IQueue* queue) override;
    Result WaitIdle() override;
    void Destroy() override { this->~SwapChain(); }
    Result GetMaximumFrameLatency(uint32* latency) override;
    Result SetMaximumFrameLatency(uint32 latency) override;
    Result Resize(uint32 width, uint32 height) override;
    // Native DXGI swap chain 656b2c8 slot 6 (388670); the Win32 swap chain
    // 656aed0 keeps the base Unsupported. WaitForImageIdle and
    // ReclaimUnusedImages keep the base hooks, as both native classes do.
    Result SetHdrMetaData(const ScreenColorConfig& colorConfig) override;

private:
    SwapChain(Device* device, const SwapChainCreateInfo& info);
    ~SwapChain() override;
    Result InitNative();
    Result WaitCopy(uint64 serial, std::chrono::nanoseconds timeout);
    Result SetFrameLatency(uint32 latency);

    struct Impl;
    Impl* m_impl;
};
} // namespace Pal::Wddm
