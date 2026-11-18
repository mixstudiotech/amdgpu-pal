// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Windows private screens (AMD direct display). Recovered from the reference ICD
// (SHA-256 36a9ef32...7a44); per-method evidence is in
// .build/pal-wddm-logic/private-display/analysis.md.
#pragma once

#include "core/privateScreen.h"
#include "core/os/wddm/wddmEscape.h"

namespace Pal
{
class Device;
class Platform;
struct PrivateScreenNotifyInfo;
}

namespace Pal::Wddm
{
// Hot-plug notification state of the original Windows Device: auto-reset event
// (Device+0x9560), KMD registration flag (+0x9570) and thread-pool wait
// (+0x9578). The original's +0x9568 "do not close" byte is only ever zeroed
// (constructor 30a0d2), so the event is always owned and closed here.
class PrivateScreenNotifier
{
public:
    PrivateScreenNotifier() = default;
    ~PrivateScreenNotifier(); // Native destructor 30a70b..30a726.

    // Native Finalize 30e17b..30e1ef, run after Pal::Device::Finalize only when
    // DeviceFinalizeInfo::flags.supportPrivateScreens is set. pNotifyInfo is the
    // Device's m_finalizeInfo.privateScreenNotifyInfo and is read at callback time.
    Result Start(const PrivateScreenNotifyInfo* pNotifyInfo);

    // Native Cleanup 30a90d..30a95c. A non-Success result means the caller must
    // skip Pal::Device::Cleanup(), as the original does.
    Result Stop(const EscapeRoute& route);

    // Native EnumPrivateScreensInfo tail 311e6d..311e9f: registers the event
    // (possibly null) with the KMD once per successful registration.
    Result RegisterWithKmd(const EscapeRoute& route);

    void* Event() const { return m_event; }
    bool  RegisteredWithKmd() const { return m_kmdRegistered; }

private:
    void* m_event         = nullptr;
    void* m_wait          = nullptr;
    bool  m_kmdRegistered = false;

    PAL_DISALLOW_COPY_AND_ASSIGN(PrivateScreenNotifier);
};

// Original Device slot 199 (311ba0..311ecd). pInfos is Pal::Device::m_privateScreenInfo
// (MaxPrivateScreens entries); pPlatform allocates the KMD format scratch buffer.
Result EnumeratePrivateScreens(const EscapeRoute&       route,
                               Pal::Platform*           pPlatform,
                               PrivateScreenCreateInfo* pInfos,
                               PrivateScreenNotifier*   pNotifier,
                               uint32*                  pNumScreens);

// Views of the protected Pal::Device emulated-screen members.
struct EmulatedPrivateScreenSlots
{
    PrivateScreen** ppScreens;     // m_pEmulatedPrivateScreens[MaxPrivateScreens]
    uint32*         pCount;        // m_emulatedPrivateScreens
    uint32*         pNextTargetId; // m_emulatedTargetId (counts down from UINT_MAX)
};

// Original Device slot 129 (313650..313864). No KMD call is made.
Result AddEmulatedScreen(Pal::Device*                   pDevice,
                         const EmulatedPrivateScreenSlots& slots,
                         const PrivateScreenNotifyInfo& notifyInfo,
                         const PrivateScreenCreateInfo& createInfo,
                         uint32*                        pTargetId);

// Original Device slot 130 (313870..313936). Returns the removed screen's
// Disable() result, so a never-enabled screen yields ErrorPrivateScreenNotEnabled.
Result RemoveEmulatedScreen(Pal::Platform*                 pPlatform,
                            const EmulatedPrivateScreenSlots& slots,
                            const PrivateScreenNotifyInfo& notifyInfo,
                            uint32                         targetId);

// KMD private-screen status DWORD -> Result (table 6be4d00 plus three immediates).
Result PrivateScreenStatusToResult(uint32 status);

// D3DDDIFORMAT sent by the original present (4d0330): sRGB formats are first
// converted to UNORM, then (format, swizzle) is looked up in table 5838fc0.
uint32 PrivateScreenPresentFormat(SwizzledFormat format);

// Index into the eight-entry private-screen format list that the original
// converter 2cd040 produces for a KMD format code, or false when the code
// converts to a format outside that list.
bool PrivateScreenFormatIndex(uint32 kmdFormat, uint32* pIndex);
} // Pal::Wddm
