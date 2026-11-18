// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/image.h"
#include "core/device.h"

namespace Pal::Wddm
{
struct PresentableImageState;
void RetainPresentableImageState(PresentableImageState* state);
void ReleasePresentableImageState(PresentableImageState* state);

class Image final : public Pal::Image
{
public:
    Image(Pal::Device* device, const ImageCreateInfo& info, const ImageInternalCreateInfo& internal)
        : Pal::Image(device, this + 1,
                     Util::VoidPtrInc(this + 1, device->GetGfxDevice()->GetImageSize(info)), info, internal) {}
    ~Image() override { ReleasePresentableImageState(m_presentableState); }
    void SetOptimalSharingLevel(MetadataSharingLevel level) override { m_sharingLevel = level; }
    MetadataSharingLevel GetOptimalSharingLevel() const override { return m_sharingLevel; }
    PresentableImageState* PresentationState() const { return m_presentableState; }
    void AttachPresentationState(PresentableImageState* state)
    {
        RetainPresentableImageState(state);
        ReleasePresentableImageState(m_presentableState);
        m_presentableState = state;
    }
    // Original Wddm::Image+0x270: cross-process ID of optimally shared metadata. The native allocation
    // writer (3802c0) publishes it at KMD metadata+180; external opens set it (3826a0, Init 381d80).
    void SetSharedResourceId(uint64 id) { m_sharedResourceId = id; }
    uint64 SharedResourceId() const { return m_sharedResourceId; }
private:
    MetadataSharingLevel m_sharingLevel = MetadataSharingLevel::FullExpand;
    PresentableImageState* m_presentableState = nullptr;
    uint64 m_sharedResourceId = 0;
};
}
