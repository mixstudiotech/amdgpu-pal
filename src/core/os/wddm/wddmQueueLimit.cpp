// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmQueueLimit.h"
#include "wddmTransport.h"
#include "wddmResult.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
Result ApplyQueuedPresentLimit(const Transport& transport, uint32 frames)
{
    // Native setter 310500 stores the requested DWORD before 310510 submits
    // this complete 16-byte public KMT record. Zero is passed through unchanged.
    if (!transport.IsOpen()) { return Result::ErrorUnavailable; }
    D3DKMT_SETQUEUEDLIMIT request = {};
    request.hDevice = transport.DeviceHandle();
    request.Type = D3DKMT_SET_QUEUEDLIMIT_PRESENT;
    request.QueuedPresentLimit = frames;
    const Status status = D3DKMTSetQueuedLimit(&request);
    // This native method intentionally overrides the common translator here.
    return status == InvalidParameter ? Result::ErrorInvalidValue : ConvertNativeStatus(status);
}
}
