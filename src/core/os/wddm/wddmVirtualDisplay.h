// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Windows virtual displays (KMD-emulated monitors that hot-plug into the desktop).
// Recovered from the reference ICD; evidence in .build/pal-wddm-logic/private-display/analysis.md.
#pragma once

#include "palDevice.h"
#include "core/os/wddm/wddmEscape.h"

namespace Pal::Wddm
{
// Original EarlyInit 30b987..30ba37 (escape 0x03009d03): the KMD's display count,
// or 1 when the escape fails; refresh range 45/1..240/1 Hz in every case.
VirtualDisplayCapabilities QueryVirtualDisplayCapabilities(const EscapeRoute& route);

// Original Device slot 151 (311330..31144a). Unsupported when caps report no displays.
Result CreateKmdVirtualDisplay(const EscapeRoute&                route,
                               const VirtualDisplayCapabilities& caps,
                               const VirtualDisplayInfo&         info,
                               uint32*                           pScreenTargetId);

// Original Device slot 152 (311450..311502).
Result DestroyKmdVirtualDisplay(const EscapeRoute& route, uint32 screenTargetId);

// Original Device slot 153 (311510..3115ce).
Result QueryKmdVirtualDisplayProperties(const EscapeRoute&        route,
                                        uint32                    screenTargetId,
                                        VirtualDisplayProperties* pProperties);
} // Pal::Wddm
