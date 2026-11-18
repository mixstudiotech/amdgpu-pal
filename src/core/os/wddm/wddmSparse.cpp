// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmSparse.h"
#include "core/os/wddm/wddmQueue.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmGpuMemory.h"
#include "core/os/wddm/wddmResult.h"
#include "core/platform.h"
#include "palAutoBuffer.h"
#include <vector>

namespace Pal::Wddm::Sparse
{
namespace
{
bool IsAligned(gpusize value, gpusize alignment)
{
    return (alignment != 0) && ((value % alignment) == 0);
}
bool ValidRange(const Pal::GpuMemory& memory, gpusize offset, gpusize size, gpusize pageSize)
{
    const auto& desc = memory.Desc();
    return (size != 0) && IsAligned(offset, pageSize) && IsAligned(size, pageSize) &&
           (offset <= desc.size) && (size <= desc.size - offset) && (desc.gpuVirtAddr != 0) &&
           (offset <= UINT64_MAX - desc.gpuVirtAddr) &&
           (size <= UINT64_MAX - desc.gpuVirtAddr - offset);
}
Result CheckVirtual(const Pal::GpuMemory& memory, const Device& device,
                    gpusize offset, gpusize size, gpusize pageSize)
{
    if (memory.GetDevice() != &device) { return Result::ErrorIncompatibleDevice; }
    if (!memory.IsVirtual()) { return Result::ErrorInvalidObjectType; }
    return ValidRange(memory, offset, size, pageSize) ? Result::Success : Result::ErrorInvalidValue;
}
}

Result Remap(Queue& queue, uint32 count, const VirtualMemoryRemapRange* ranges, bool doNotWait)
{
    if (count == 0) { return Result::ErrorInvalidValue; }
    if (ranges == nullptr) { return Result::ErrorInvalidPointer; }
    auto& device = *static_cast<Device*>(queue.GetDevice());
    const auto& properties = device.MemoryProperties();
    // A remap into a split object (128 MiB kernel allocations mapped back to back) needs one map operation per
    // allocation it touches, so the operation list can be longer than the range list.
    std::vector<SparseOperation> operations;
    std::vector<size_t> firstOperation(size_t(count) + 1, 0);
    operations.reserve(count);

    // Validate the entire request before any kernel page-table mutation.
    for (uint32 i = 0; i < count; ++i)
    {
        firstOperation[i] = operations.size();
        const auto& range = ranges[i];
        if (range.pVirtualGpuMem == nullptr) { return Result::ErrorInvalidPointer; }
        const auto& virtualMemory = *static_cast<const Pal::GpuMemory*>(range.pVirtualGpuMem);
        Result result = CheckVirtual(virtualMemory, device, range.virtualStartOffset, range.size,
                                     properties.virtualMemPageSize);
        if (result != Result::Success) { return result; }
        SparseOperation operation = {};
        operation.virtualAddress = virtualMemory.Desc().gpuVirtAddr + range.virtualStartOffset;
        operation.size = range.size;
        if (range.pRealGpuMem != nullptr)
        {
            const auto& realMemory = *static_cast<const Pal::GpuMemory*>(range.pRealGpuMem);
            if (realMemory.GetDevice() != &device) { return Result::ErrorIncompatibleDevice; }
            if (realMemory.IsVirtual()) { return Result::ErrorInvalidObjectType; }
            if (!IsAligned(range.realStartOffset, properties.realMemAllocGranularity) ||
                !ValidRange(realMemory, range.realStartOffset, range.size, properties.virtualMemPageSize))
            { return Result::ErrorInvalidValue; }
            const auto& real = static_cast<const GpuMemory&>(realMemory);
            const Allocation* allocations = real.OsAllocations();
            gpusize allocationBase = 0;
            gpusize offset = range.realStartOffset;
            gpusize remaining = range.size;
            for (uint32 a = 0; (a < real.OsAllocationCount()) && (remaining != 0); ++a)
            {
                const gpusize allocationEnd = allocationBase + allocations[a].size;
                if (offset < allocationEnd)
                {
                    if (allocations[a].handle == 0) { return Result::ErrorUnavailable; }
                    const gpusize piece = Util::Min(remaining, allocationEnd - offset);
                    SparseOperation map = operation;
                    map.type = SparseOperationType::Map;
                    map.size = piece;
                    map.allocationHandle = allocations[a].handle;
                    map.allocationOffset = offset - allocationBase;
                    operations.push_back(map);
                    operation.virtualAddress += piece;
                    offset += piece;
                    remaining -= piece;
                }
                allocationBase = allocationEnd;
            }
            if (remaining != 0) { return Result::ErrorUnavailable; }
        }
        else
        {
            if (range.virtualAccessMode >= VirtualGpuMemAccessMode::Count) { return Result::ErrorInvalidValue; }
            operation.type = SparseOperationType::Unmap;
            operation.zero = (range.virtualAccessMode == VirtualGpuMemAccessMode::ReadZero);
            operation.noAccess = (range.virtualAccessMode == VirtualGpuMemAccessMode::NoAccess);
            operations.push_back(operation);
        }
    }
    firstOperation[count] = operations.size();

    // KMT map batches share a reservation. Preserve input order while grouping
    // consecutive ranges for the same destination, as the native PAL builder does.
    uint32 first = 0;
    while (first < count)
    {
        uint32 end = first + 1;
        while ((end < count) && (ranges[end].pVirtualGpuMem == ranges[first].pVirtualGpuMem)) { ++end; }
        const size_t begin = firstOperation[first];
        const size_t batch = firstOperation[end] - begin;
        const Result result = ConvertStatus(device.OsTransport().UpdateVa(
            &queue.NativeQueue(), &operations[begin], batch, doNotWait), "UpdateGpuVa(remap)");
        if (result != Result::Success) { return result; }
        first = end;
    }
    return Result::Success;
}

Result Copy(Queue& queue, uint32 count, const VirtualMemoryCopyPageMappingsRange* ranges, bool doNotWait)
{
    if (count == 0) { return Result::ErrorInvalidValue; }
    if (ranges == nullptr) { return Result::ErrorInvalidPointer; }
    auto& device = *static_cast<Device*>(queue.GetDevice());
    const gpusize pageSize = device.MemoryProperties().virtualMemPageSize;
    Util::AutoBuffer<SparseOperation, 64, Pal::Platform> operations(count, device.GetPlatform());
    if (operations.Capacity() < count) { return Result::ErrorOutOfMemory; }
    for (uint32 i = 0; i < count; ++i)
    {
        const auto& range = ranges[i];
        if ((range.pSrcGpuMem == nullptr) || (range.pDstGpuMem == nullptr)) { return Result::ErrorInvalidPointer; }
        const auto& source = *static_cast<const Pal::GpuMemory*>(range.pSrcGpuMem);
        const auto& destination = *static_cast<const Pal::GpuMemory*>(range.pDstGpuMem);
        Result result = CheckVirtual(source, device, range.srcStartOffset, range.size, pageSize);
        if (result == Result::Success)
        { result = CheckVirtual(destination, device, range.dstStartOffset, range.size, pageSize); }
        if (result != Result::Success) { return result; }
        operations[i] = {};
        operations[i].type = SparseOperationType::Copy;
        operations[i].sourceAddress = source.Desc().gpuVirtAddr + range.srcStartOffset;
        operations[i].virtualAddress = destination.Desc().gpuVirtAddr + range.dstStartOffset;
        operations[i].size = range.size;
    }
    // Every KMT batch has one source reservation and one destination reservation.
    // Keep each COPY operation whole so overlapping source mappings are retained.
    uint32 first = 0;
    while (first < count)
    {
        uint32 end = first + 1;
        while ((end < count) && (ranges[end].pSrcGpuMem == ranges[first].pSrcGpuMem) &&
               (ranges[end].pDstGpuMem == ranges[first].pDstGpuMem)) { ++end; }
        const Result result = ConvertStatus(device.OsTransport().UpdateVa(
            &queue.NativeQueue(), &operations[first], end - first, doNotWait), "UpdateGpuVa(copy)");
        if (result != Result::Success) { return result; }
        first = end;
    }
    return Result::Success;
}
}
