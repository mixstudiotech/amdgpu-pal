// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/fence.h"

namespace Pal::Wddm
{
class Device;
class Queue;
class Fence final : public Pal::Fence
{
public:
    explicit Fence(Device* device);
    ~Fence() override;
    Result Init(const FenceCreateInfo& info) override;
    Result GetStatus() const override;
    Result OpenHandle(const FenceOpenInfo& info) override;
    OsExternalHandle ExportExternalHandle(const FenceExportInfo& info) const override;
    void AssociateWithContext(Pal::SubmissionContext* context) override;
    Result Reset() override;
    Result WaitForFences(const Pal::Device& device, uint32 count, const Pal::Fence*const* fences,
                         bool waitAll, std::chrono::nanoseconds timeout) const override;
    Result Signal(Queue& queue);
    OsExternalHandle NativeEvent() const { return m_event; }
private:
    OsExternalHandle m_event;
    bool m_isReference;
};
static_assert(sizeof(Pal::Fence) == 24, "Native fence base layout is specific to the verified x64 PAL ABI");
static_assert(sizeof(Fence) == 40, "Native Windows fence stores an event and a borrowed-handle flag");
}
