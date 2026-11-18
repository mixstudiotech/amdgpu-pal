// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "palScreen.h"
#include <windows.h>

namespace Pal::Wddm
{
struct ScreenRecord;
struct ScreenAdapterAssociation
{
    uint32 low;
    int32 high;
    const IDevice* device;
};
// count means no eligible PAL Device. A matching adapter without a Device
// terminates the search for that path, as in native 301480..3014d7.
uint32 FindScreenAdapter(const ScreenRecord& record, const ScreenAdapterAssociation* adapters, uint32 count);

// These dependencies preserve the recovered API call order and allow the
// original instruction fixture to supply controlled Windows replies. They do
// not represent the original class layout or virtual table.
struct ScreenPolicyCalls
{
    ULONGLONG (WINAPI* setConditionMask)(ULONGLONG, DWORD, BYTE);
    BOOL (WINAPI* verifyVersion)(LPOSVERSIONINFOEXW, DWORD, DWORDLONG);
    HRESULT (WINAPI* isCompositionEnabled)(BOOL*);
};

const ScreenPolicyCalls& WindowsScreenPolicyCalls();
bool ScreenWindowedTimingRestricted(const ScreenPolicyCalls& calls);
bool ScreenTimingUnavailable(bool ownsFullscreen, const ScreenPolicyCalls& calls);
void SetScreenSelectedNativeProperties(ScreenProperties* properties, const ScreenPolicyCalls& calls);
}
