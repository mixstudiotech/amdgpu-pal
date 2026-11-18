// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmScreenPolicy.h"
#include "core/os/wddm/wddmScreen.h"
#include <dwmapi.h>

namespace Pal::Wddm
{
uint32 FindScreenAdapter(const ScreenRecord& record, const ScreenAdapterAssociation* adapters, uint32 count)
{
    for (uint32 index = 0; index < count; ++index)
    {
        if ((adapters[index].low == record.sourceAdapterLow) && (adapters[index].high == record.sourceAdapterHigh))
        {
            return (adapters[index].device != nullptr) ? index : count;
        }
    }
    return count;
}

const ScreenPolicyCalls& WindowsScreenPolicyCalls()
{
    static const ScreenPolicyCalls calls = { VerSetConditionMask, VerifyVersionInfoW, DwmIsCompositionEnabled };
    return calls;
}

bool ScreenWindowedTimingRestricted(const ScreenPolicyCalls& calls)
{
    // Native 2ffe40(this, 2) tests Windows 6.2 or later. VerifyVersionInfoW's
    // result is used as a Boolean, including FALSE/error. Do not replace it
    // with an inferred OS version or a modern version-query fallback.
    OSVERSIONINFOEXW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    version.dwMajorVersion = 6;
    version.dwMinorVersion = 2;
    ULONGLONG mask = calls.setConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = calls.setConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    mask = calls.setConditionMask(mask, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    BOOL restricted = calls.verifyVersion(&version, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, mask);
    if (restricted == FALSE)
    {
        // Native 302b50 initializes this value to FALSE and ignores HRESULT.
        // The callee's output remains authoritative even when it reports an
        // error. This deliberately preserves that recovered behavior.
        calls.isCompositionEnabled(&restricted);
    }
    return restricted != FALSE;
}

bool ScreenTimingUnavailable(bool ownsFullscreen, const ScreenPolicyCalls& calls)
{
    // Native 3693a0/369450 skip all version/DWM calls for an owned screen.
    return !ownsFullscreen && ScreenWindowedTimingRestricted(calls);
}

void SetScreenSelectedNativeProperties(ScreenProperties* properties, const ScreenPolicyCalls& calls)
{
    constexpr uint32 count = 1024;
    static_assert(MaxGammaRampControlPoints >= count);
    properties->gammaRampCaps.supportScaleAndOffset = false;
    properties->gammaRampCaps.minConvertedValue = 0.0f;
    properties->gammaRampCaps.maxConvertedValue = 1.0f;
    properties->gammaRampCaps.controlPointCount = count;
    for (uint32 index = 0; index < count; ++index)
    {
        // Native emits DIVSS. Keep the original rounded curve under fast-math.
        volatile float divisor = float(count - 1);
        properties->gammaRampCaps.controlPointPositions[index] = float(index) / divisor;
    }
    const bool windowed = !ScreenWindowedTimingRestricted(calls);
    properties->supportWindowedWaitForVerticalBlank = windowed;
    properties->supportWindowedGetScanLine = windowed;
}
}
