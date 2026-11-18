// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/platform.h"
#include "core/os/wddm/wddmTransport.h"
#include "core/os/wddm/wddmFullscreen.h"
#include "core/os/wddm/wddmOglPresent.h"

namespace Pal::Wddm
{
class Device;

class Platform final : public Pal::Platform
{
  public:
    Platform(const PlatformCreateInfo &info, const Util::AllocCallbacks &callbacks) : Pal::Platform(info, callbacks)
    {
    }
    void Destroy() override
    {
        TearDownDevices();
        this->~Platform();
    }
    size_t GetScreenObjectSize() const override;
    // The reference Platform table 6565988 has no primary-layout entry: PlatformSettings (slot 26) is followed
    // directly by TurboSyncControl (slot 27), and no other slot issues a layout query.
    Result GetPrimaryLayout(uint32, GetPrimaryLayoutOutput *) override
    {
        return Result::Unsupported;
    }
    Result TurboSyncControl(const TurboSyncControlInput &input) override;
    // Native Platform+0x27d8..0x27ec exclusive-mode mutexes (see wddmFullscreen.h).
    ExclusiveModeLock &ExclusiveMode()
    {
        return m_exclusiveMode;
    }
    // Native Platform+0x27bc: ReQueryScreens saw ERROR_ACCESS_DENIED from the display configuration (sticky).
    bool NoDisplayAccess() const
    {
        return m_noDisplayAccess;
    }
    // Native Platform slot 39 (301770): load the OpenGL ICD of the Device's adapter (see wddmOglPresent.h).
    void LoadOglRuntime(Device &device);
    // Native Platform slots 40/41 (300b90): the OpenGL runtime callback table, Platform+0x2b90.
    OglRuntimeCallbacks *OglCallbacks()
    {
        return &m_oglRuntime.callbacks;
    }

  private:
    Result ConnectToOsInterface() override;
    Result ReQueryDevices() override;
    Result ReQueryScreens(uint32 *count, void *storage[MaxScreens], IScreen *screens[MaxScreens]) override;
    ExclusiveModeLock m_exclusiveMode;
    bool m_noDisplayAccess = false;
    OglRuntimeState m_oglRuntime = {}; // Native Platform+0x2800 / +0x2b90; the module is never freed (300d90).
};
} // namespace Pal::Wddm
