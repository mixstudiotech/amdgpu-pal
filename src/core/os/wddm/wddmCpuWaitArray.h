// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/os/wddm/wddmCpuWait.h"
#include "palAutoBuffer.h"

namespace Pal::Wddm
{
// Native3118b0 array policy. The supplied allocator is PAL's Platform (or its
// ForwardAllocator callback boundary). Object/type guards outside this function
// remain caller-owned. Dynamic storage outlives event close in WaitCpuPayloads.
template<typename Allocator, typename Object, typename HandleGetter>
Result WaitCpuPayloadObjects(uint32_t device, uint32_t count, const Object*const* objects,
                             const uint64_t* values, bool waitAny, int64_t nanoseconds,
                             Allocator* allocator, HandleGetter getHandle, bool infinite = false,
                             const CpuWaitCallbacks* callbacks = nullptr,
                             Result (*validateObject)(const Object*) = nullptr)
{
    Util::AutoBuffer<uint32_t, 16, Allocator> handles(count, allocator);
    if (count == 0) { return Result::ErrorInvalidValue; }
    if (handles.Capacity() < count) { return Result::ErrorOutOfMemory; }
    if ((objects == nullptr) || (values == nullptr)) { return Result::ErrorInvalidPointer; }
    for (uint32_t i = 0; i < count; ++i)
    {
        if (objects[i] == nullptr) { return Result::ErrorInvalidPointer; }
        // Optional public type/capability guards are separate from the native
        // array policy and must not overtake allocation/capacity/null checks.
        if (validateObject != nullptr)
        {
            const Result result = validateObject(objects[i]);
            if (result != Result::Success) { return result; }
        }
        handles[i] = getHandle(objects[i]);
    }
    return WaitCpuPayloads(device, handles.Data(), values, count, waitAny, nanoseconds, infinite, callbacks);
}
}
