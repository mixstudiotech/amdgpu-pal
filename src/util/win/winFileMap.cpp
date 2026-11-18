#include "palWin.h"
#include "palFileMap.h"
#include "palFile.h"
#include "palSysUtil.h"
#include <limits>

namespace Util
{
FileMapping::FileMapping() : m_memoryMapping(nullptr), m_fileHandle(nullptr), m_writeable(false), m_pFileName(nullptr), m_pSystemName(nullptr) {}
FileMapping::~FileMapping() { Close(); }
Result FileMapping::Create(const char* name, bool write, size_t size, const char* systemName)
{
    if (!name) { return Result::ErrorInvalidPointer; }
    if (m_fileHandle) { return Result::ErrorUnavailable; }
    HANDLE file = CreateFileA(name, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, write ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return GetLastError() == ERROR_FILE_NOT_FOUND ? Result::NotFound : Result::ErrorUnknown; }
    m_fileHandle = file;
    m_writeable = write;
    m_pFileName = name;
    m_pSystemName = systemName;
    const Result result = ReloadMap(size);
    if (result != Result::Success) { Close(); }
    return result;
}
Result FileMapping::CreateFromHandle(void* file, bool write, size_t size, const char* systemName)
{
    if (m_fileHandle) { return Result::ErrorUnavailable; }
    if (!file || file == INVALID_HANDLE_VALUE) { return Result::ErrorInvalidExternalHandle; }
    if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &m_fileHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
        { return Result::ErrorInvalidExternalHandle; }
    m_writeable = write;
    m_pSystemName = systemName;
    const Result result = ReloadMap(size);
    if (result != Result::Success) { Close(); }
    return result;
}
Result FileMapping::ReloadMap(size_t size)
{
    if (!m_fileHandle) { return Result::ErrorUnavailable; }
    if (m_memoryMapping) { CloseHandle(m_memoryMapping); m_memoryMapping = nullptr; }
    if (m_writeable && size)
    {
        LARGE_INTEGER length; length.QuadPart = size;
        if (!SetFilePointerEx(m_fileHandle, length, nullptr, FILE_BEGIN) || !SetEndOfFile(m_fileHandle)) { return Result::ErrorUnknown; }
    }
    m_memoryMapping = CreateFileMappingA(m_fileHandle, nullptr, m_writeable ? PAGE_READWRITE : PAGE_READONLY,
                                       static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), m_pSystemName);
    return m_memoryMapping ? Result::Success : Result::ErrorUnknown;
}
void FileMapping::Close()
{
    if (m_memoryMapping) { CloseHandle(m_memoryMapping); m_memoryMapping = nullptr; }
    if (m_fileHandle) { CloseHandle(m_fileHandle); m_fileHandle = nullptr; }
}
bool FileMapping::Flush() { return m_fileHandle && FlushFileBuffers(m_fileHandle); }
FileView::FileView() : m_pMappedMem(nullptr), m_offsetIntoView(0), m_requestedSize(0) {}
void* FileView::Map(const FileMapping& file, bool write, size_t offset, size_t size)
{
    if (m_pMappedMem || !file.IsValid() || !size) { return nullptr; }
    SYSTEM_INFO info; GetSystemInfo(&info);
    const size_t base = offset - (offset % info.dwAllocationGranularity);
    const size_t delta = offset - base;
    if (size > (std::numeric_limits<size_t>::max)() - delta) { return nullptr; }
    m_pMappedMem = MapViewOfFile(file.GetHandle(), write ? FILE_MAP_WRITE : FILE_MAP_READ,
                                static_cast<DWORD>(base >> 32), static_cast<DWORD>(base), size + delta);
    if (m_pMappedMem) { m_offsetIntoView = delta; m_requestedSize = size; }
    return m_pMappedMem ? Ptr() : nullptr;
}
void FileView::UnMap(bool flush)
{
    if (m_pMappedMem)
    {
        if (flush) { FlushViewOfFile(m_pMappedMem, m_requestedSize + m_offsetIntoView); }
        UnmapViewOfFile(m_pMappedMem);
    }
    m_pMappedMem = nullptr; m_offsetIntoView = 0; m_requestedSize = 0;
}
Result FileView::Flush(size_t bytes)
{
    if (!m_pMappedMem) { return Result::ErrorUnavailable; }
    if (bytes > m_requestedSize) { return Result::ErrorInvalidValue; }
    return FlushViewOfFile(m_pMappedMem, bytes ? bytes + m_offsetIntoView : 0) ? Result::Success : Result::ErrorUnknown;
}
}
