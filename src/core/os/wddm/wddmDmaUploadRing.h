// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/dmaUploadRing.h"

namespace Pal::Wddm
{
class Device;
class DmaUploadRing final : public Pal::DmaUploadRing
{
public:
    explicit DmaUploadRing(Device* device);
    Result WaitForPendingUpload(Pal::Queue* waiter, UploadFenceToken value) override;
};
}
