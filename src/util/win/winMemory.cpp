#include "palWin.h"
#include "palSysMemory.h"
#include <malloc.h>
namespace Util
{
namespace
{
void* PAL_STDCALL Allocate(void*, size_t size, size_t alignment, SystemAllocType)
{
    if (!IsPowerOfTwo(alignment)) { return nullptr; }
    return _aligned_malloc(size, (std::max)(alignment, sizeof(void*)));
}
void PAL_STDCALL Free(void*, void* memory) { _aligned_free(memory); }
}
Result OsInitDefaultAllocCallbacks(AllocCallbacks* callbacks)
{
    if (!callbacks) { return Result::ErrorInvalidPointer; }
    callbacks->pfnAlloc = Allocate; callbacks->pfnFree = Free;
    return Result::Success;
}
size_t VirtualPageSize() { SYSTEM_INFO info; GetSystemInfo(&info); return info.dwPageSize; }
Result VirtualReserve(size_t bytes, void** output, void* requested, size_t alignment)
{
    if (!output) { return Result::ErrorInvalidPointer; }
    *output = nullptr;
    if (!bytes) { return Result::ErrorInvalidValue; }
    SYSTEM_INFO info; GetSystemInfo(&info);
    if ((alignment != 0) && (!IsPowerOfTwo(alignment) || (alignment > info.dwAllocationGranularity)))
    {
        // VirtualAlloc2 supports explicit larger alignment without a release/re-reserve race.
        using VirtualAlloc2Fn = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
        const auto allocate = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
        if (!allocate || !IsPowerOfTwo(alignment)) { return Result::ErrorInvalidAlignment; }
        MEM_ADDRESS_REQUIREMENTS requirements = {};
        requirements.Alignment = alignment;
        MEM_EXTENDED_PARAMETER parameter = {};
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        *output = allocate(GetCurrentProcess(), requested, bytes, MEM_RESERVE, PAGE_NOACCESS, &parameter, 1);
    }
    else
    {
        *output = VirtualAlloc(requested, bytes, MEM_RESERVE, PAGE_NOACCESS);
    }
    return *output ? Result::Success : Result::ErrorOutOfMemory;
}
Result VirtualCommit(void* memory, size_t bytes, bool executable)
{
    if (!memory) { return Result::ErrorInvalidPointer; }
    if (!bytes) { return Result::ErrorInvalidValue; }
    return VirtualAlloc(memory, bytes, MEM_COMMIT, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE)
        ? Result::Success : Result::ErrorOutOfMemory;
}
Result VirtualDecommit(void* memory, size_t bytes)
{
    if (!memory) { return Result::ErrorInvalidPointer; }
    if (!bytes) { return Result::ErrorInvalidValue; }
    return VirtualFree(memory, bytes, MEM_DECOMMIT) ? Result::Success : Result::ErrorUnknown;
}
Result VirtualRelease(void* memory, size_t bytes)
{
    if (!memory) { return Result::ErrorInvalidPointer; }
    if (!bytes) { return Result::ErrorInvalidValue; }
    return VirtualFree(memory, 0, MEM_RELEASE) ? Result::Success : Result::ErrorUnknown;
}
void* GenericAllocator::Alloc(const AllocInfo& info)
{
    void* memory = Allocate(nullptr, info.bytes, info.alignment, info.allocType);
    if (memory && info.zeroMem) { memset(memory, 0, info.bytes); }
    return memory;
}
void GenericAllocator::Free(const FreeInfo& info) { _aligned_free(info.pClientMem); }
}
