// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "palMutex.h"
#include <cstdint>

namespace Pal::Wddm
{
// Native37f9f0/37faf0 state: a per-object critical section, outstanding map
// count, and the optional pointer retained by native cached-map flag344.6.
// The callbacks are the actual per-call KMT operations, not a lock cache.
class CpuMapState
{
  public:
    template<class Lock>
    Util::Result Map(bool cached, Lock&& lock, void** output)
    {
        Util::MutexAuto guard(&m_mutex);
        Util::Result result = Util::Result::Success;
        void* address = m_cachedAddress;
        if (address == nullptr)
        {
            void* locked = nullptr;
            result = lock(&locked);
            if (result == Util::Result::Success)
            {
                address = locked;
                if (cached) { m_cachedAddress = address; }
            }
        }
        if (result == Util::Result::Success) { ++m_count; }
        *output = address;
        return result;
    }

    template<class Unlock>
    Util::Result Unmap(bool cached, Unlock&& unlock)
    {
        Util::MutexAuto guard(&m_mutex);
        Util::Result result = Util::Result::ErrorGpuMemoryUnmapFailed;
        if (m_count != 0)
        {
            result = cached ? Util::Result::Success : unlock();
            if (result == Util::Result::Success) { --m_count; }
        }
        return result;
    }

  private:
    Util::Mutex m_mutex;
    void* m_cachedAddress = nullptr;
    uint32_t m_count = 0;
};
}
