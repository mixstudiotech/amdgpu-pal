#include "palWin.h"
#include "palSysUtil.h"
#include <bcrypt.h>
#include "palVectorImpl.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>

namespace Util
{
namespace
{
Result WindowsError(DWORD error)
{
    switch (error)
    {
    case ERROR_SUCCESS: return Result::Success;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return Result::NotFound;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return Result::AlreadyExists;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: return Result::ErrorPermissionDenied;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return Result::ErrorOutOfMemory;
    case ERROR_DISK_FULL: return Result::ErrorDiskFull;
    case ERROR_INVALID_PARAMETER: case ERROR_INVALID_NAME: return Result::ErrorInvalidValue;
    default: return Result::ErrorUnknown;
    }
}
std::string Path(StringView<char> view) { return std::string(view.Data(), view.Length()); }
template<class Callback> Result Enumerate(StringView<char> path, Callback callback)
{
    if (path.IsEmpty()) { return Result::ErrorInvalidPointer; }
    const std::string pattern = Path(path) + "\\*";
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? Result::Success : WindowsError(error);
    }
    Result result = Result::Success;
    do
    {
        if ((strcmp(entry.cFileName, ".") == 0) || (strcmp(entry.cFileName, "..") == 0)) { continue; }
        result = callback(entry);
        if (result != Result::Success) { break; }
    } while (FindNextFileA(search, &entry));
    const DWORD error = GetLastError();
    FindClose(search);
    return (result != Result::Success) ? result : (error == ERROR_NO_MORE_FILES ? Result::Success : WindowsError(error));
}
SecondsSinceEpoch FileTime(FILETIME value)
{
    const uint64 ticks = (static_cast<uint64>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    return SecondsSinceEpoch{std::chrono::seconds{static_cast<int64>(ticks / 10000000ULL) - 11644473600LL}};
}
HMODULE ThisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}
}
Result QuerySystemInfo(SystemInfo* info)
{
    if (!info) { return Result::ErrorInvalidPointer; }
    *info = {};
    SYSTEM_INFO system; GetSystemInfo(&system);
    info->cpuLogicalCoreCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    std::vector<unsigned char> buffer(bytes);
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bytes))
    {
        for (DWORD offset = 0; offset < bytes;)
        {
            const auto record = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            ++info->cpuPhysicalCoreCount;
            offset += record->Size;
        }
    }
    MEMORYSTATUSEX memory = {}; memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) { info->totalSysMemSize = static_cast<uint32>(memory.ullTotalPhys >> 20); }
#if PAL_HAS_CPUID
    uint32 regs[4]; CpuId(regs, 0);
    memcpy(info->cpuVendorString, &regs[1], 4); memcpy(info->cpuVendorString + 4, &regs[3], 4); memcpy(info->cpuVendorString + 8, &regs[2], 4);
    CpuId(regs, 0x80000000);
    if (regs[0] >= 0x80000004)
    {
        for (uint32 index = 0; index < 3; ++index) { CpuId(regs, 0x80000002 + index); memcpy(info->cpuBrandString + index * 16, regs, 16); }
        info->cpuBrandString[47] = 0;
    }
    if (strcmp(info->cpuVendorString, "AuthenticAMD") == 0) { QueryAMDCpuType(info); }
    else if (strcmp(info->cpuVendorString, "GenuineIntel") == 0) { QueryIntelCpuType(info); }
    CpuId(regs, 0);
    if (regs[0] >= 0x16) { CpuId(regs, 0x16); info->cpuFrequency = regs[0] & 0xffff; }
#endif
    return Result::Success;
}
int64 GetPerfFrequency() { LARGE_INTEGER value; QueryPerformanceFrequency(&value); return value.QuadPart; }
int64 GetPerfCpuTime(bool) { LARGE_INTEGER value; QueryPerformanceCounter(&value); return value.QuadPart; }
bool IsProfileRestricted() { return false; } // Windows exposes profiling through driver/device capabilities, not /proc policy.
Result GetExecutableName(char* buffer, char** filename, size_t length)
{
    if (!buffer || !filename) { return Result::ErrorInvalidPointer; }
    if (!length || length > MAXDWORD) { return Result::ErrorInvalidValue; }
    const DWORD written = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(length));
    if (!written) { return WindowsError(GetLastError()); }
    if (written >= length) { buffer[length - 1] = 0; return Result::ErrorInvalidMemorySize; }
    char* slash = strrchr(buffer, '\\'); *filename = slash ? slash + 1 : buffer;
    return Result::Success;
}
Result GetExecutableName(wchar_t* buffer, wchar_t** filename, size_t length)
{
    if (!buffer || !filename) { return Result::ErrorInvalidPointer; }
    if (length < sizeof(wchar_t) || length / sizeof(wchar_t) > MAXDWORD) { return Result::ErrorInvalidValue; }
    length /= sizeof(wchar_t);
    const DWORD written = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(length));
    if (!written) { return WindowsError(GetLastError()); }
    if (written >= length) { buffer[length - 1] = 0; return Result::ErrorInvalidMemorySize; }
    wchar_t* slash = wcsrchr(buffer, L'\\'); *filename = slash ? slash + 1 : buffer;
    return Result::Success;
}
Result GetCurrentLibraryName(char* buffer, size_t length, char* extension, size_t extensionLength)
{
    if (!buffer) { return Result::ErrorInvalidPointer; }
    if (!length || length > MAXDWORD) { return Result::ErrorInvalidMemorySize; }
    char fullPath[32768];
    const DWORD count = GetModuleFileNameA(ThisModule(), fullPath, static_cast<DWORD>(sizeof(fullPath)));
    if (!count || count >= sizeof(fullPath)) { return Result::ErrorUnknown; }
    const char* separator = strrchr(fullPath, '\\');
    const char* name = separator ? separator + 1 : fullPath;
    const char* dot = strrchr(name, '.');
    const size_t nameLength = dot ? static_cast<size_t>(dot - name) : strlen(name);
    if (nameLength >= length) { return Result::ErrorInvalidMemorySize; }
    memcpy(buffer, name, nameLength); buffer[nameLength] = 0;
    if (extension)
    {
        const char* suffix = dot ? dot + 1 : "";
        if (strlen(suffix) >= extensionLength) { return Result::ErrorInvalidMemorySize; }
        Strncpy(extension, suffix, extensionLength);
    }
    return Result::Success;
}
bool GetCurrentLibraryBuildId(BuildId* id)
{
    if (!id) { return false; }
    // PE CodeView RSDS identifies the exact linked artifact, including stripped PDB builds.
    const auto base = reinterpret_cast<const uint8*>(ThisModule());
    if (!base) { return false; }
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return false; }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return false; }
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const auto debug = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    for (size_t index = 0; index < directory.Size / sizeof(*debug); ++index)
    {
        if ((debug[index].Type == IMAGE_DEBUG_TYPE_CODEVIEW) && (debug[index].SizeOfData >= 20))
        {
            const uint8* record = base + debug[index].AddressOfRawData;
            if (memcmp(record, "RSDS", 4) == 0) { memcpy(id->data, record + 4, sizeof(id->data)); return true; }
        }
    }
    static const BuildId fallback = []
    {
        BuildId value = {};
        if (BCryptGenRandom(nullptr, value.data, sizeof(value.data), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            const uint64 entropy = GetTickCount64() ^ reinterpret_cast<uintptr_t>(ThisModule());
            memcpy(value.data, &entropy, sizeof(entropy));
        }
        return value;
    }();
    *id = fallback;
    return false;
}
void SplitFilePath(const char* full, char* path, size_t pathLength, char* file, size_t fileLength)
{
    if (!full) { if (path && pathLength) { *path = 0; } if (file && fileLength) { *file = 0; } return; }
    const char* separator = nullptr;
    for (const char* p = full; *p; ++p) { if (*p == '/' || *p == '\\') { separator = p; } }
    if (path && pathLength)
    {
        const size_t length = separator ? static_cast<size_t>(separator - full + 1) : 0;
        const size_t copied = (std::min)(length, pathLength - 1); memcpy(path, full, copied); path[copied] = 0;
    }
    if (file && fileLength) { Strncpy(file, separator ? separator + 1 : full, fileLength); }
}
Result MkDir(const char* path)
{
    if (!path) { return Result::ErrorInvalidPointer; }
    return CreateDirectoryA(path, nullptr) ? Result::Success : WindowsError(GetLastError());
}
Result MkDirRecursively(const char* path)
{
    if (!path) { return Result::ErrorInvalidPointer; }
    std::error_code error;
    const bool created = std::filesystem::create_directories(path, error);
    return error ? WindowsError(static_cast<DWORD>(error.value())) : created ? Result::Success : Result::AlreadyExists;
}
Result CountFilesInDir(StringView<char> path, size_t* count, size_t* chars)
{
    if (!count || !chars) { return Result::ErrorInvalidPointer; }
    *count = 0; *chars = 0;
    return Enumerate(path, [&](const WIN32_FIND_DATAA& entry) { ++*count; *chars += strlen(entry.cFileName) + 1; return Result::Success; });
}
Result GetFileNamesInDir(StringView<char> path, Span<StringView<char>> names, Span<char> buffer)
{
    size_t index = 0, offset = 0;
    return Enumerate(path, [&](const WIN32_FIND_DATAA& entry)
    {
        const size_t bytes = strlen(entry.cFileName) + 1;
        if ((index >= names.NumElements()) || bytes > buffer.NumElements() - offset) { return Result::ErrorIncompleteResults; }
        memcpy(buffer.Data() + offset, entry.cFileName, bytes);
        names[index++] = StringView<char>(buffer.Data() + offset, static_cast<uint32>(bytes - 1));
        offset += bytes; return Result::Success;
    });
}
Result GetFileInfoInDir(StringView<char> path, Vector<StatName, 1, GenericAllocator>* files)
{
    if (!files) { return Result::ErrorInvalidPointer; }
    const std::string parent = Path(path);
    return Enumerate(path, [&](const WIN32_FIND_DATAA& entry)
    {
        const std::string name = parent + "\\" + entry.cFileName;
        if (name.size() >= MaxPathStrLen) { return Result::ErrorInvalidMemorySize; }
        StatName value = {}; Strncpy(value.name, name.c_str(), sizeof(value.name));
        Result result = File::GetStat(value.name, &value.stat);
        return result == Result::Success ? files->PushBack(value) : result;
    });
}
Result RemoveFilesOfDirOlderThan(const char* path, SecondsSinceEpoch threshold)
{
    if (!path) { return Result::ErrorInvalidPointer; }
    return Enumerate(StringView<char>(path), [&](const WIN32_FIND_DATAA& entry)
    {
        const std::string child = std::string(path) + "\\" + entry.cFileName;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { return Result::Success; }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { return RemoveFilesOfDirOlderThan(child.c_str(), threshold); }
        if ((std::max)(FileTime(entry.ftLastAccessTime), FileTime(entry.ftLastWriteTime)) >= threshold) { return Result::Success; }
        return DeleteFileA(child.c_str()) ? Result::Success : WindowsError(GetLastError());
    });
}
Result GetStatusOfDir(const char* path, uint64* total, SecondsSinceEpoch* oldest)
{
    if (!path || !total || !oldest) { return Result::ErrorInvalidPointer; }
    return Enumerate(StringView<char>(path), [&](const WIN32_FIND_DATAA& entry)
    {
        const std::string child = std::string(path) + "\\" + entry.cFileName;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { return Result::Success; }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { return GetStatusOfDir(child.c_str(), total, oldest); }
        *total += (static_cast<uint64>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
        const auto timestamp = (std::max)(FileTime(entry.ftLastAccessTime), FileTime(entry.ftLastWriteTime));
        *oldest = oldest->time_since_epoch().count() ? (std::min)(*oldest, timestamp) : timestamp;
        return Result::Success;
    });
}
Result Rename(const char* oldName, const char* newName)
{
    if (!oldName || !newName) { return Result::ErrorInvalidPointer; }
    return MoveFileExA(oldName, newName, MOVEFILE_REPLACE_EXISTING) ? Result::Success : WindowsError(GetLastError());
}
uint32 GetIdOfCurrentProcess() { return GetCurrentProcessId(); }
size_t DumpStackTrace(char* output, size_t bytes, uint32 skip)
{
    if (!output || !bytes) { return 0; }
    void* frames[64];
    const USHORT count = CaptureStackBackTrace(skip + 1, 64, frames, nullptr);
    size_t written = 0;
    for (USHORT index = 0; index < count && written < bytes - 1; ++index)
    {
        const int length = snprintf(output + written, bytes - written, "%p\n", frames[index]);
        if (length < 0 || static_cast<size_t>(length) >= bytes - written) { output[bytes - 1] = 0; return bytes - 1; }
        written += length;
    }
    output[written] = 0;
    return written;
}
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 870
void SleepMs(uint32 duration) { ::Sleep(duration); }
#endif
void BeepSound(uint32 frequency, uint32 duration) { ::Beep(frequency, duration); }
bool IsDebuggerAttached() { return IsDebuggerPresent() != FALSE; }
Result SetRwxFilePermissions(const char* name)
{
    if (!name) { return Result::ErrorInvalidPointer; }
    const DWORD attributes = GetFileAttributesA(name);
    if (attributes == INVALID_FILE_ATTRIBUTES) { return WindowsError(GetLastError()); }
    // PAL's Unix owner permission adjustment maps to clearing Windows' read-only attribute.
    return SetFileAttributesA(name, attributes & ~FILE_ATTRIBUTE_READONLY) ? Result::Success : WindowsError(GetLastError());
}
bool IsKeyPressed(KeyCode key, bool* previous)
{
    int vk = 0;
    if (key >= KeyCode::A && key <= KeyCode::Z) { vk = 'A' + static_cast<int>(key) - static_cast<int>(KeyCode::A); }
    else if (key >= KeyCode::Zero && key <= KeyCode::Nine) { vk = '0' + static_cast<int>(key) - static_cast<int>(KeyCode::Zero); }
    else if (key >= KeyCode::F1 && key <= KeyCode::F16) { vk = VK_F1 + static_cast<int>(key) - static_cast<int>(KeyCode::F1); }
    else if (key >= KeyCode::Num0 && key <= KeyCode::Num9) { vk = VK_NUMPAD0 + static_cast<int>(key) - static_cast<int>(KeyCode::Num0); }
    else
    {
        switch (key)
        {
        case KeyCode::Esc: vk = VK_ESCAPE; break;
        case KeyCode::Enter: case KeyCode::NumEnter: vk = VK_RETURN; break;
        case KeyCode::Space: vk = VK_SPACE; break;
        case KeyCode::Tab: vk = VK_TAB; break;
        case KeyCode::Backspace: vk = VK_BACK; break;
        case KeyCode::Shift: vk = VK_SHIFT; break;
        case KeyCode::LShift: vk = VK_LSHIFT; break;
        case KeyCode::RShift: vk = VK_RSHIFT; break;
        case KeyCode::Control: vk = VK_CONTROL; break;
        case KeyCode::LControl: vk = VK_LCONTROL; break;
        case KeyCode::RControl: vk = VK_RCONTROL; break;
        case KeyCode::Alt: vk = VK_MENU; break;
        case KeyCode::LAlt: vk = VK_LMENU; break;
        case KeyCode::RAlt: vk = VK_RMENU; break;
        case KeyCode::Shift_F10: vk = VK_F10; break;
        case KeyCode::Shift_F11: vk = VK_F11; break;
        case KeyCode::Insert: vk = VK_INSERT; break;
        case KeyCode::Delete: vk = VK_DELETE; break;
        case KeyCode::Home: vk = VK_HOME; break;
        case KeyCode::End: vk = VK_END; break;
        case KeyCode::PageUp: vk = VK_PRIOR; break;
        case KeyCode::PageDown: vk = VK_NEXT; break;
        case KeyCode::ArrowUp: vk = VK_UP; break;
        case KeyCode::ArrowDown: vk = VK_DOWN; break;
        case KeyCode::ArrowLeft: vk = VK_LEFT; break;
        case KeyCode::ArrowRight: vk = VK_RIGHT; break;
        case KeyCode::Capslock: vk = VK_CAPITAL; break;
        case KeyCode::Scroll: vk = VK_SCROLL; break;
        case KeyCode::Numlock: vk = VK_NUMLOCK; break;
        case KeyCode::NumSlash: vk = VK_DIVIDE; break;
        case KeyCode::NumAsterisk: vk = VK_MULTIPLY; break;
        case KeyCode::NumMinus: vk = VK_SUBTRACT; break;
        case KeyCode::NumPlus: vk = VK_ADD; break;
        case KeyCode::NumDot: vk = VK_DECIMAL; break;
        case KeyCode::Backtick: vk = VK_OEM_3; break;
        case KeyCode::Minus: vk = VK_OEM_MINUS; break;
        case KeyCode::Equal: vk = VK_OEM_PLUS; break;
        case KeyCode::LBrace: vk = VK_OEM_4; break;
        case KeyCode::RBrace: vk = VK_OEM_6; break;
        case KeyCode::Backslash: vk = VK_OEM_5; break;
        case KeyCode::Semicolon: vk = VK_OEM_1; break;
        case KeyCode::Apostrophe: vk = VK_OEM_7; break;
        case KeyCode::Comma: vk = VK_OEM_COMMA; break;
        case KeyCode::Dot: vk = VK_OEM_PERIOD; break;
        case KeyCode::Slash: vk = VK_OEM_2; break;
        default: return false;
        }
    }
    bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (key == KeyCode::Shift_F10 || key == KeyCode::Shift_F11) { pressed = pressed && ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0); }
    if (previous) { const bool was = *previous; *previous = pressed; pressed = pressed && !was; }
    return pressed;
}
}
