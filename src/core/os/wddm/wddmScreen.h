// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "palScreen.h"
#include "core/os/wddm/wddmFullscreen.h"
#include <vector>
#include <windows.h>

namespace Pal { class Platform; }
namespace Pal::Wddm
{
class Device;

// Public Windows desktop topology identity of one active CCD path.
struct ScreenRecord
{
    uint32 adapterLow = 0;
    int32 adapterHigh = 0;
    uint32 targetId = 0;
    uint32 sourceId = 0;
    uint32 sourceAdapterLow = 0;
    int32 sourceAdapterHigh = 0;
    wchar_t gdiName[32] = {};
    OsDisplayHandle monitor = nullptr;
    ScreenRotation rotation = ScreenRotation::Rot0;
    Rect desktop = {};
    Extent2d currentResolution = {};
    Extent2d preferredResolution = {};
};

// win32Status (optional) receives the failing GetDisplayConfigBufferSizes/QueryDisplayConfig status.
Result QueryActiveScreenRecords(std::vector<ScreenRecord>* records, LONG* win32Status = nullptr);

class Screen final : public IScreen
{
public:
    Screen(const ScreenRecord& record, const IDevice* mainDevice);
    ~Screen() override;
    void Destroy() override { this->~Screen(); }
    Result GetProperties(ScreenProperties* info) const override;
    Result GetScreenModeList(uint32* count, ScreenMode* modes) const override;
    Result QueryCurrentDisplayMode(Extent2d* extent) const override;
    Result GetFormats(uint32* count, SwizzledFormat* formats) override;
    Result RegisterWindow(OsWindowHandle window) override;
    HdrDisplayMode GetFormatHdrMode(SwizzledFormat format) const override;
    Result WaitForVerticalBlank() const override;
    Result GetScanLine(int32* scanLine) const override;

    // Native Screen slots 5, 7, 8, 9, 11, 12 (369000, 3691a0, 369290, 369330,
    // 36a720, 36a8f0); see wddmFullscreen.h for the shared private records.
    Result IsImplicitFullscreenOwnershipSafe(OsDisplayHandle hDisplay, OsWindowHandle hWindow,
                                             Extent2d imageExtent) const override;
    Result TakeFullscreenOwnership(const IImage& image) override;
    Result ReleaseFullscreenOwnership() override;
    Result SetGammaRamp(const GammaRamp& gammaRamp) override;
    Result GetColorCapabilities(ScreenColorCapabilities* caps) override;
    Result SetColorConfiguration(const ScreenColorConfig* config) override;

    // Native ReQueryScreens 301350 (3015d8): colour-state refresh 36aba0 of every enumerated screen.
    Result RefreshColorStateForEnumeration();

private:
    Result Refresh(ScreenRecord* record) const;
    Device* MainDevice() const;
    FullscreenTarget Target(Device* device) const;
    ScreenColorContext ColorContext(const EscapeRoute* route) const;

    ScreenRecord m_record;
    const IDevice* m_mainDevice;
    Pal::Platform* m_platform;     // Native +0x20; allocator of the display information.
    bool m_ownsFullscreen = false; // Native +0x14c bit0; set only by a complete TakeFullscreenOwnership.
    ScreenColorCache m_color;      // Native +0x38, +0x40, +0x14c bits 1..2, +0x150.
};
}
