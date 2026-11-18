// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Windowed presentation through the AMD OpenGL runtime, the original's default (PalSettings presentViaOglRuntime,
// hash 2466363770, default true). Reference: amdvlk64.dll 9.2.10.416, image base 0x180000000.
//   Platform slot 39 (301770) loads the OpenGL ICD named by KMTQAITYPE_UMOPENGLINFO (atig6pxx.dll on mix) and its
//     AmdGetCallbackProcsV2 / AmdGetCallbackProcs export; slots 40/41 return the callback table (Platform+0x2b90).
//   The KMT windowed presenter's 2d21e0 passes each present to opengl32's PresentBuffers callback, which the ICD hands
//     to PAL on the first present into a window (AmdGetCallbackProcsV2 sets the window's pixel format so that
//     opengl32 loads the ICD and calls its DrvSetCallbackProcs, then copies opengl32's procs into the V2 block).
//   opengl32 gets the window's DWM redirection surface and calls the ICD's DrvPresentBuffers, which calls 2b41e0/
//     2b3f40 through the V2 block: PAL opens the surface and asks opengl32 (procs 7/8) to submit a KMD redirected
//     blit from the presentable image into it. Pixels never pass through the CPU on any of these paths.
// Notes: .build/pal-wddm-logic/icd-gaps/analysis.txt.
#pragma once

#include "pal.h"
#include "palQueue.h"
#include "core/os/wddm/wddmResult.h"

#include <cstring>

namespace Pal
{
class IImage;
}

namespace Pal::Wddm
{
// Native Platform+0x2b90 (Platform slots 40 and 41 return its address).
struct OglRuntimeCallbacks
{
    void* getCallbackProcs;   // +0x00 ICD export AmdGetCallbackProcs
    void* getCallbackProcsV2; // +0x08 ICD export AmdGetCallbackProcsV2
    void* presentBuffers;     // +0x10 opengl32 PresentBuffers (V2 block +0x38, opengl32 proc 4)
    void* kmtPresent;         // +0x18 opengl32 legacy-context redirected blit (V2 block +0x50, proc 7)
    void* hwQueuePresent;     // +0x20 opengl32 HW-queue redirected blit (V2 block +0x58, proc 8)
};
static_assert(sizeof(OglRuntimeCallbacks) == 0x28);

// Native Platform+0x2800 (module) and +0x2b90 (callbacks). The Platform destructor (300d90) clears the module
// handle without FreeLibrary: the ICD stays loaded for opengl32, which holds its own reference.
struct OglRuntimeState
{
    void*               module;
    OglRuntimeCallbacks callbacks;
};

// D3DKMT_OPENGLINFO (KMTQAITYPE_UMOPENGLINFO), 528 bytes zeroed at 301799.
struct OglIcdInfo
{
    wchar_t fileName[260];
    uint32  version;
    uint32  flags;
};
static_assert(sizeof(OglIcdInfo) == 528);

// D3DKMT_QUERYADAPTERINFO as 301770 fills it (the .cpp checks the layout against d3dkmthk.h).
struct OglAdapterQuery
{
    uint32 hAdapter;
    uint32 type;
    void*  data;
    uint32 size;
};
constexpr uint32 KmtQueryUmOpenGlInfo = 2; // KMTQAITYPE_UMOPENGLINFO

// Native presenter +0x310: block passed to AmdGetCallbackProcs.
struct OglCallbackBlockV1
{
    void*  presenter;
    uint32 version; // 1
    uint32 reserved;
};
static_assert(sizeof(OglCallbackBlockV1) == 0x10);

// Native presenter +0x320 (248 bytes): block passed to AmdGetCallbackProcsV2 and, as PRESENTBUFFERSCB.pPrivData, to
// opengl32. The ICD checks size >= 0xf8 and version >= 2 and copies up to 16 opengl32 callback procs to +0x18.
struct OglCallbackBlockV2
{
    uint32 size;           // 248
    uint32 version;        // 2
    void*  presenter;      // +0x08, read back by 2b41e0
    void*  callback;       // +0x10, 2b41e0; the ICD's DrvPresentBuffers calls it with the PRESENTBUFFERS record
    void*  procs[16];      // +0x18
    uint8  reserved[0x60]; // +0x98
};
static_assert(sizeof(OglCallbackBlockV2) == 248);
constexpr uint32 OglProcPresentBuffers  = 4; // +0x38
constexpr uint32 OglProcKmtPresent      = 7; // +0x50
constexpr uint32 OglProcHwQueuePresent  = 8; // +0x58

// PRESENTBUFFERSCB (gldrv.h) as 2d21e0 builds it on the stack: version 3.
struct OglPresentBuffersCb
{
    uint32 version;     // 3
    uint32 syncType;    // presentInterval != 0 (unsigned)
    uint64 adapterLuid; // adapter+0x968
    void*  privateData; // the V2 block, else the V1 block
    int32  updateRect[4];
};
static_assert(sizeof(OglPresentBuffersCb) == 0x28);

// PRESENTBUFFERS (gldrv.h): what opengl32 passes to the ICD's DrvPresentBuffers and the ICD to 2b41e0.
struct OglPresentBuffers
{
    uint64 surface;      // DWM redirection surface (0: nothing to open)
    uint64 adapterLuid;
    uint64 presentToken; // DWM update id
    void*  privateData;  // the V2 block
};

// Record of opengl32 proc 8 (D3DKMTSubmitPresentToHwQueue), built at 2b403b..2b40e3.
struct OglHwQueueBlt
{
    uint64        version;      // 3
    uint64        surface;      // +0x08
    uint64        presentToken; // +0x10
    int32         rect[4];      // +0x18
    uint32        count;        // +0x28 1
    uint32        reserved0;    // +0x2c
    const uint32* hwQueues;     // +0x30
    const uint32* sources;      // +0x38
    const uint32* destinations; // +0x40
    uint8         reserved[16]; // +0x48
};
static_assert(sizeof(OglHwQueueBlt) == 0x58);

// Record of opengl32 proc 7 (D3DKMTPresent on the queue's context), built at 2b40ed..2b418a.
struct OglKmtBlt
{
    uint32 version;        // 3
    uint32 context;        // +0x04
    uint32 source;         // +0x08
    uint32 destination;    // +0x0c
    uint64 surface;        // +0x10
    uint64 presentToken;   // +0x18
    int32  rect[4];        // +0x20
    uint8  reserved[0x310];
};
static_assert(sizeof(OglKmtBlt) == 0x340);

using OglGetCallbackProcsFn = void (*)(void* block, void* window);
using OglPresentBuffersFn   = int (*)(void* dc, OglPresentBuffersCb* data);
using OglBltFn              = int (*)(void* dc, void* record);
using OglPresentCallbackFn  = bool (*)(const OglPresentBuffers* data);

// Native presenter +0x2e0..+0x428.
struct OglPresenterState
{
    PresentDirectInfo  info;    // +0x2e0 the present being handed to opengl32 (the original copies 0x30 bytes)
    OglCallbackBlockV1 blockV1; // +0x310
    OglCallbackBlockV2 blockV2; // +0x320
    void*              window;  // +0x418 last window handed to the ICD; also the DWM surface cache key
    void*              dc;      // +0x420 window DC while opengl32 presents (stale afterwards, as in the original)
};

// 2d08d0 clears +0x2e0..+0x41f; 2d1390 fills the two blocks.
void InitOglPresenterState(OglPresenterState* state, void* presenter, OglPresentCallbackFn callback);

// Record builders of 2b3f40.
void BuildOglHwQueueBlt(const OglPresentBuffers& data, const int32 rect[4], const uint32* hwQueue,
                        const uint32* source, const uint32* destination, OglHwQueueBlt* record);
void BuildOglKmtBlt(const OglPresentBuffers& data, const int32 rect[4], uint32 context, uint32 source,
                    uint32 destination, OglKmtBlt* record);

// =====================================================================================================================
// 54c570 (Util library load) with flags 0: a module that is already loaded gains a reference; otherwise
// LoadLibraryExA(name, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS). Failure: ErrorUnavailable (-2).
template <typename Os>
Result LoadLibraryByName(Os& os, void** module, const char* name)
{
    if (os.ModuleHandle(name, module))
    {
        return Result::Success;
    }
    *module = os.LoadModule(name, 0x1000);
    return (*module != nullptr) ? Result::Success : Result::ErrorUnavailable;
}

// Platform slot 39 (301770). Runs on every OpenGL-runtime present; it does nothing once either export is known, and
// tries again on the next present while both are missing (no ICD, a failed load or neither export).
template <typename Os>
void LoadOglRuntime(Os& os, OglRuntimeState* state, uint32 kmtAdapter)
{
    if ((state->callbacks.getCallbackProcs != nullptr) || (state->callbacks.getCallbackProcsV2 != nullptr))
    {
        return;
    }
    OglIcdInfo info = {};
    OglAdapterQuery query = { kmtAdapter, KmtQueryUmOpenGlInfo, &info, uint32(sizeof(info)) };
    if (ConvertNativeStatus(os.QueryAdapterInfo(&query)) != Result::Success)
    {
        return;
    }
    char name[512];
    os.WideToMultiByte(name, info.fileName, sizeof(name)); // 2e99b38 (CRT wcstombs); its result is not checked
    if (LoadLibraryByName(os, &state->module, name) == Result::Success)
    {
        state->callbacks.getCallbackProcsV2 = os.GetProcAddress(state->module, "AmdGetCallbackProcsV2");
        if (state->callbacks.getCallbackProcsV2 == nullptr)
        {
            state->callbacks.getCallbackProcs = os.GetProcAddress(state->module, "AmdGetCallbackProcs");
        }
    }
}

// 2d21e0: the presenter's OpenGL-runtime present. Os supplies IsWindow, Platform slots 39/41, GetDC/ReleaseDC and
// the presenter's own fallbacks (2d1fa0 on a HW queue, else 2d1d30) for a missing opengl32 PresentBuffers callback.
template <typename Os>
Result PresentViaOglRuntime(Os& os, OglPresenterState* state, const PresentDirectInfo& info, uint64 adapterLuid,
                            bool hwQueue)
{
    if (!os.IsWindow(info.hWindow))
    {
        return Result::ErrorUnknown;
    }
    os.LoadRuntime();
    OglRuntimeCallbacks* callbacks = os.Callbacks();
    if (info.hWindow != state->window)
    {
        // 2d2261..2d22c2: the ICD learns each new window once; the V2 export reports opengl32's procs through the
        // block, and only the non-null ones replace the Platform's.
        if (callbacks->getCallbackProcsV2 != nullptr)
        {
            reinterpret_cast<OglGetCallbackProcsFn>(callbacks->getCallbackProcsV2)(&state->blockV2, info.hWindow);
            if (state->blockV2.procs[OglProcPresentBuffers] != nullptr)
            {
                callbacks->presentBuffers = state->blockV2.procs[OglProcPresentBuffers];
            }
            if (state->blockV2.procs[OglProcKmtPresent] != nullptr)
            {
                callbacks->kmtPresent = state->blockV2.procs[OglProcKmtPresent];
            }
            if (state->blockV2.procs[OglProcHwQueuePresent] != nullptr)
            {
                callbacks->hwQueuePresent = state->blockV2.procs[OglProcHwQueuePresent];
            }
        }
        else if (callbacks->getCallbackProcs != nullptr)
        {
            reinterpret_cast<OglGetCallbackProcsFn>(callbacks->getCallbackProcs)(&state->blockV1, info.hWindow);
        }
        state->window = info.hWindow;
    }
    if (callbacks->presentBuffers == nullptr)
    {
        return os.Fallback(hwQueue);
    }
    state->info = info;
    OglPresentBuffersCb data = {};
    data.version = 3;
    data.syncType = (info.presentInterval != 0) ? 1u : 0u;
    data.adapterLuid = adapterLuid;
    if (callbacks->getCallbackProcsV2 != nullptr)
    {
        data.privateData = &state->blockV2;
    }
    else if (callbacks->getCallbackProcs != nullptr)
    {
        data.privateData = &state->blockV1;
    }
    else
    {
        return Result::ErrorUnknown;
    }
    state->dc = os.GetDc(state->window);
    if (state->dc == nullptr)
    {
        return Result::ErrorUnknown;
    }
    const bool presented =
        reinterpret_cast<OglPresentBuffersFn>(callbacks->presentBuffers)(state->dc, &data) != 0;
    os.ReleaseDc(state->window, state->dc);
    return presented ? Result::Success : Result::ErrorUnknown;
}

// 2b3f40, reached through 2b41e0 when opengl32 calls the ICD's DrvPresentBuffers during PresentViaOglRuntime. Opens
// the DWM surface (2d23e0, cache key = state->window) and has opengl32 submit the redirected blit on this queue.
// Returns true when there was nothing to open or the open succeeded; the blit callback's result is not used.
template <typename Os>
bool OglPresentCallback(Os& os, OglPresenterState* state, const OglPresentBuffers& data)
{
    if (data.surface != 0)
    {
        if (os.OpenSurface(uint32(data.surface)) != Result::Success)
        {
            return false;
        }
    }
    const uint32 hwQueue = os.HwQueueHandle(); // queue slot 42
    int32 rect[4] = {};
    if (!os.GetClientRect(state->info.hWindow, rect))
    {
        // 2b3ffc..2b4016: the source image's extent.
        uint32 width = 0;
        uint32 height = 0;
        os.ImageExtent(state->info.pSrcImage, &width, &height);
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = int32(width);
        rect[3] = int32(height);
    }
    OglRuntimeCallbacks* callbacks = os.Callbacks(); // Platform slot 40
    const uint32 source = os.SourceAllocation(state->info.pSrcImage);
    const uint32 destination = (data.surface != 0) ? os.DestinationAllocation() : 0;
    if (hwQueue != 0)
    {
        OglHwQueueBlt record;
        BuildOglHwQueueBlt(data, rect, &hwQueue, &source, &destination, &record);
        // The original calls through the table entry without a check.
        if (callbacks->hwQueuePresent != nullptr)
        {
            reinterpret_cast<OglBltFn>(callbacks->hwQueuePresent)(state->dc, &record);
        }
    }
    else
    {
        OglKmtBlt record;
        BuildOglKmtBlt(data, rect, os.Context(), source, destination, &record);
        if (callbacks->kmtPresent != nullptr)
        {
            reinterpret_cast<OglBltFn>(callbacks->kmtPresent)(state->dc, &record);
        }
    }
    return true;
}
} // namespace Pal::Wddm
