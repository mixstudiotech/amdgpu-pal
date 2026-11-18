// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmDmaUploadRing.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmQueue.h"

namespace Pal::Wddm
{
DmaUploadRing::DmaUploadRing(Device* device) : Pal::DmaUploadRing(device) {}
Result DmaUploadRing::WaitForPendingUpload(Pal::Queue* waiter, UploadFenceToken value)
{
    if (waiter == nullptr) { return Result::ErrorInvalidPointer; }
    if ((waiter->GetDevice() != m_pDevice) || (m_pDmaQueue == nullptr) || (value == 0))
        { return Result::ErrorInvalidValue; }
    const auto* context = static_cast<const SubmissionContext*>(m_pDmaQueue->GetSubmissionContext());
    if ((context == nullptr) || (value > context->NativeQueue().submitted))
        { return Result::ErrorInvalidValue; }
    const auto& queue = context->NativeQueue();
    // Called from the consuming queue's actual OsSubmit, after software batching.
    // This wait orders GPU consumers behind the upload's real legacy-context progress.
    return static_cast<Queue*>(waiter)->WaitNativeSync(
        { queue.progressFence, queue.progressCpu, queue.progressGpu }, value);
}
}
