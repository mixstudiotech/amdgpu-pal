// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmDevice.h"
#include "core/platform.h"
#include "core/svmMgr.h"
#include "palSysMemory.h"

namespace Pal::Wddm
{
Result Device::InitSvmPool()
{
    if (m_svmMgr != nullptr) { return Result::ErrorUnavailable; }
    const gpusize bytes = GetPlatform()->GetMaxSizeOfSvm();
    if ((bytes == 0) || ((bytes & 65535) != 0)) { return Result::ErrorInvalidMemorySize; }
    // The recovered397fe0 manager uses the same four-GiB search and BestFit
    // suballocator as this public PAL source. Windows supplies real pool VA hooks.
    m_svmMgr = PAL_NEW(Pal::SvmMgr, GetPlatform(), Util::AllocInternal)(this);
    if (m_svmMgr == nullptr) { return Result::ErrorOutOfMemory; }
    auto& range = m_memoryProperties.vaRange[static_cast<uint32>(VaPartition::Svm)];
    Result result = m_svmMgr->Init(&range);
    // Do not expose an allocator if a complete CPU/GPU pool was never reserved.
    if ((result == Result::Success) && (!m_svmGpuReserved || !m_svmCpuReserved ||
        (range.baseVirtAddr != m_svmPoolBase) || (range.size != m_svmPoolBytes)))
    { result = Result::ErrorOutOfGpuMemory; }
    if (result == Result::Success)
    {
        m_memoryProperties.flags.svmSupport = 1;
        m_memoryProperties.flags.iommuv2Support = 0;
    }
    else
    {
        const Result cleanup = CleanupSvmPool();
        if (cleanup != Result::Success) { m_transport.Quarantine(); }
    }
    return result;
}

Result Device::ReserveGpuVirtualAddressSvm(gpusize base, gpusize size)
{
    if (m_svmGpuReserved || m_svmCpuReserved) { return Result::ErrorUnavailable; }
    uint64 address = 0;
    const Status status = m_transport.ReserveVa(base, 0, 0, size, false, &address);
    if (status != Success) { return ConvertStatus(status, "ReserveGpuVa(SVM pool)"); }
    if (address != base)
    {
        const Status released = m_transport.FreeVa(address, size);
        if (released != Success) { m_transport.Quarantine(); }
        return Result::ErrorOutOfGpuMemory;
    }
    // SvmMgr has already reserved this exact CPU range. These hooks own both
    // sides; Platform::SvmRangeStart stays zero to avoid the common CPU release.
    m_svmPoolBase = base;
    m_svmPoolBytes = size;
    m_svmGpuReserved = true;
    m_svmCpuReserved = true;
    return Result::Success;
}

Result Device::FreeGpuVirtualAddressSvm(gpusize base, gpusize size)
{
    if ((base != m_svmPoolBase) || (size != m_svmPoolBytes)) { return Result::ErrorInvalidValue; }
    if (m_svmGpuReserved)
    {
        const Status status = m_transport.FreeVa(base, size);
        if (status != Success) { return ConvertStatus(status, "FreeGpuVa(SVM pool)"); }
        m_svmGpuReserved = false;
    }
    if (m_svmCpuReserved)
    {
        const Result result = Util::VirtualRelease(reinterpret_cast<void*>(uintptr_t(base)), size_t(size));
        if (result != Result::Success) { return result; }
        m_svmCpuReserved = false;
    }
    return Result::Success;
}

Result Device::CleanupSvmPool()
{
    if (m_svmMgr == nullptr) { return Result::Success; }
    Result result = Result::Success;
    if (m_svmGpuReserved || m_svmCpuReserved) { result = m_svmMgr->Cleanup(); }
    if (result == Result::Success)
    {
        PAL_SAFE_DELETE(m_svmMgr, GetPlatform());
        m_svmPoolBase = m_svmPoolBytes = 0;
        m_memoryProperties.vaRange[static_cast<uint32>(VaPartition::Svm)] = {};
        m_memoryProperties.flags.svmSupport = 0;
    }
    return result;
}
}
