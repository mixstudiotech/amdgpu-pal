// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "core/os/wddm/wddmEventWait.h"
#include "core/os/wddm/wddmTransport.h"

namespace Pal::Wddm
{
struct CpuWaitCallbacks
{
    void* userData = nullptr;
    void* (*createEvent)(void*, const void* securityAttributes, uint32_t manualReset,
                         uint32_t initialState, const wchar_t* name) = nullptr;
    void (*closeEvent)(void*, void* event) = nullptr;
    Status (*registerWait)(void*, const void* nativeRequest) = nullptr;
    EventWaitCallbacks event;
};

// The optional callback observes the actual zeroed40-byte KMT request, not a
// second model of it. A null callback invokes the real native DDI.
Status RegisterCpuWaitRequest(uint32_t device, const uint32_t* handles, const uint64_t* values,
                              uint32_t count, bool waitAny, void* event,
                              const CpuWaitCallbacks* callbacks = nullptr);

// Single timeline payload fastpath from35e880. The reference requires a valid
// native CPU mapping; public object/type/null-map validation stays at the caller.
Result WaitCpuPayload(uint32_t device, uint32_t handle, const volatile uint64_t& completed,
                       uint64_t target, int64_t nanoseconds, bool infinite = false,
                       const CpuWaitCallbacks* callbacks = nullptr);

// Many-object3118b0 after the caller has constructed the handle array. There is
// no CPU-payload precheck. Object-array allocation/validation remains caller-owned.
Result WaitCpuPayloads(uint32_t device, const uint32_t* handles, const uint64_t* values,
                        uint32_t count, bool waitAny, int64_t nanoseconds, bool infinite = false,
                        const CpuWaitCallbacks* callbacks = nullptr);
}
