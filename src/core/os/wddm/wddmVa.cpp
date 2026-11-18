// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmPlatform.h"
#include "palSettingsFileMgrImpl.h"

namespace Pal::Wddm
{
Device::~Device()
{
    // Native destructor 30a615..30a661 frees the raw profile, then the copied SPP profile.
    m_applicationProfile.Clear();
    if (m_spp.copiedBlob != nullptr)
    {
        PAL_FREE(m_spp.copiedBlob, GetPlatform());
    }
    m_spp = {};
    CleanupSvmPool();
    for (const auto &reservation : m_alignedReservations)
    {
        m_transport.FreeVa(reservation.second.base, reservation.second.size);
    }
}

Result Device::ProbeGpuVaRange(gpusize base, gpusize size, VaPartition partition) const
{
    auto *device = const_cast<Device *>(this);
    uint64 address = 0;
    Status status = device->m_transport.ReserveVa(base, 0, 0, size, false, &address);
    if (status != Success)
    {
        return ConvertStatus(status, "ProbeGpuVaRange reserve");
    }
    const Status released = device->m_transport.FreeVa(address, size);
    if (released != Success)
    {
        device->m_transport.Quarantine();
        return ConvertStatus(released);
    }
    return (address == base) ? Result::Success : Result::ErrorUnavailable;
}

Result Device::ReserveVa(VaPartition partition, gpusize base, gpusize size, bool isVirtual,
                         VirtualGpuMemAccessMode mode, gpusize *address, gpusize alignment)
{
    if (!m_transport.IsOpen())
    {
        return Result::ErrorDeviceLost;
    }
    if ((address == nullptr) || (size == 0) || ((size & 65535) != 0) || ((base & 65535) != 0))
    {
        return Result::ErrorInvalidValue;
    }
    const auto &range = MemoryProperties().vaRange[static_cast<uint32>(partition)];
    const uint64 minimum = (range.size > 0) ? range.baseVirtAddr : 65536;
    const uint64 maximum = (range.size > 0) ? minimum + range.size - 1 : MemoryProperties().vaEnd;
    const bool zero = isVirtual && (mode == VirtualGpuMemAccessMode::ReadZero);
    if ((base == 0) && (alignment > 65536))
    {
        if (size > UINT64_MAX - alignment + 65536)
        {
            return Result::ErrorInvalidValue;
        }
        const uint64 padded = size + alignment - 65536;
        uint64 original = 0;
        Status status = m_transport.ReserveVa(0, minimum, maximum, padded, zero, &original);
        if (status != Success)
        {
            return ConvertStatus(status);
        }
        *address = Util::Pow2Align(original, alignment);
        // A prefix FreeVa releases the whole reservation on the verified KMD.
        // Keep padding reserved until this allocation is destroyed.
        Util::MutexAuto lock(&m_vaLock);
        m_alignedReservations.emplace(*address, AlignedReservation{original, padded, size});
        return Result::Success;
    }
    // KMT reserves each allocation atomically within the selected PAL high32
    // interval. Do not retain a whole4GiB descriptor partition: other Windows
    // UMDs, including D3D12/DXGI, must be able to allocate in the remaining holes.
    return ConvertStatus(m_transport.ReserveVa(base, minimum, maximum, size, zero, address));
}

Result Device::FreeGpuVirtualAddress(gpusize base, gpusize size)
{
    if (!m_transport.IsOpen())
    {
        return Result::ErrorDeviceLost;
    }
    Util::MutexAuto lock(&m_vaLock);
    auto aligned = m_alignedReservations.find(base);
    if (aligned != m_alignedReservations.end())
    {
        if (aligned->second.mappedSize != size)
        {
            return Result::ErrorInvalidValue;
        }
        Status status = m_transport.FreeVa(aligned->second.base, aligned->second.size);
        if (status == Success)
        {
            m_alignedReservations.erase(aligned);
        }
        return ConvertStatus(status);
    }
    return ConvertStatus(m_transport.FreeVa(base, size));
}
} // namespace Pal::Wddm
