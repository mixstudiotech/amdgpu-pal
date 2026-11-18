// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmOglPresent.h"

#include <cstddef>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
static_assert(sizeof(OglIcdInfo) == sizeof(D3DKMT_OPENGLINFO));
static_assert(offsetof(OglAdapterQuery, hAdapter) == offsetof(D3DKMT_QUERYADAPTERINFO, hAdapter));
static_assert(offsetof(OglAdapterQuery, type) == offsetof(D3DKMT_QUERYADAPTERINFO, Type));
static_assert(offsetof(OglAdapterQuery, data) == offsetof(D3DKMT_QUERYADAPTERINFO, pPrivateDriverData));
static_assert(offsetof(OglAdapterQuery, size) == offsetof(D3DKMT_QUERYADAPTERINFO, PrivateDriverDataSize));
static_assert(sizeof(OglAdapterQuery) == sizeof(D3DKMT_QUERYADAPTERINFO));
static_assert(KmtQueryUmOpenGlInfo == KMTQAITYPE_UMOPENGLINFO);
static_assert(offsetof(OglPresenterState, blockV1) - offsetof(OglPresenterState, info) >= sizeof(PresentDirectInfo));
static_assert(offsetof(OglCallbackBlockV2, procs) == 0x18);
static_assert(offsetof(OglHwQueueBlt, hwQueues) == 0x30);
static_assert(offsetof(OglKmtBlt, rect) == 0x20);

void InitOglPresenterState(OglPresenterState* state, void* presenter, OglPresentCallbackFn callback)
{
    std::memset(state, 0, sizeof(*state));
    // 2d1390..2d13ba
    state->blockV1.presenter = presenter;
    state->blockV1.version = 1;
    state->blockV2.size = sizeof(OglCallbackBlockV2);
    state->blockV2.version = 2;
    state->blockV2.presenter = presenter;
    state->blockV2.callback = reinterpret_cast<void*>(callback);
}

void BuildOglHwQueueBlt(const OglPresentBuffers& data, const int32 rect[4], const uint32* hwQueue,
                        const uint32* source, const uint32* destination, OglHwQueueBlt* record)
{
    // 2b403b..2b40e3
    std::memset(record, 0, sizeof(*record));
    record->version = 3;
    record->surface = data.surface;
    record->presentToken = data.presentToken;
    std::memcpy(record->rect, rect, sizeof(record->rect));
    record->count = 1;
    record->hwQueues = hwQueue;
    record->sources = source;
    record->destinations = destination;
}

void BuildOglKmtBlt(const OglPresentBuffers& data, const int32 rect[4], uint32 context, uint32 source,
                    uint32 destination, OglKmtBlt* record)
{
    // 2b40f3..2b418a
    std::memset(record, 0, sizeof(*record));
    record->version = 3;
    record->context = context;
    record->source = source;
    record->destination = destination;
    record->surface = data.surface;
    record->presentToken = data.presentToken;
    std::memcpy(record->rect, rect, sizeof(record->rect));
}
} // namespace Pal::Wddm
