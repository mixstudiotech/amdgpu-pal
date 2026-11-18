// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "palDevice.h"
#include "palSysMemory.h"
#include <cstddef>
#include <cstdint>

namespace Pal::Wddm
{
struct FlglRoute
{
    uint32 adapter=0;
    uint32 device=0;
    uint32 physicalAdapterIndex=0;
    bool nativeEscapeDisabled=false; // Original Device+35792 guard.
};
struct FlglCallbacks
{
    void* userData=nullptr;
    int32_t (*escape)(void*, const void* nativeRequest)=nullptr;
    Result (*workstationCaps)(void*, uint32* caps)=nullptr;
};

// Native30f860 envelope/result boundary for this family's bounded inline records.
Result ReplayFlglRecord(const FlglRoute& route, void* record, size_t bytes,
                        const FlglCallbacks* callbacks=nullptr);
Result QueryDisplayConnectors(const FlglRoute& route, uint32* count,
                               DisplayConnectorProperties* connectors,
                               const FlglCallbacks* callbacks=nullptr);

class Flgl
{
public:
    Flgl(FlglRoute route, Util::IndirectAllocator allocator, FlglCallbacks callbacks={}, FlglState initialState={})
        : m_route(route),m_allocator(allocator),m_callbacks(callbacks),m_state(initialState) {}
    void SetRoute(FlglRoute route) { m_route=route; }
    const FlglRoute& Route() const { return m_route; }
    const FlglState& CachedState() const { return m_state; }
    Result QueryState(FlglState* state);
    Result SetSyncConfiguration(const GlSyncConfig& config);
    Result GetSyncConfiguration(GlSyncConfig* config) const;
    Result SetFrameLock(bool enable);
    Result SetGenLock(bool enable);
    Result ResetFrameCounter() const;
    Result GetFrameCounter(uint64* value, bool* reset) const;
    Result GetFrameCounterResetStatus(bool* reset) const;
    // Native Device::Finalize 30e208..30e258: clears the cached support, then for capability bit0
    // refreshes it and, only when Available, the gen-lock flags. Returns the capability query result
    // or the gen-lock refresh result, which the original propagates as the Finalize result.
    Result InitializeFromCapabilities(Result capabilityResult, uint32 caps);
private:
    Result Replay(void* record,size_t bytes) const { return ReplayFlglRecord(m_route,record,bytes,&m_callbacks); }
    uint32 ControllerCount() const;
    void UpdateSupport();
    Result UpdateGenLock();
    Result Control(uint32 control) const;
    Result FetchCounter(uint64* value,bool* reset) const;
    Result GetConfig(uint32 mask,uint8* config36) const;
    bool FrameLockEnabled() const;
    Result EnumerateRoutes(uint8* routes,uint32* count) const;
    FlglRoute m_route;
    Util::IndirectAllocator m_allocator;
    FlglCallbacks m_callbacks;
    FlglState m_state;
};
}
