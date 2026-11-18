// Windows named-pipe helpers reconstructed against amdvlk64.dll 9.2.10.416.
// SHA256 36a9ef32d75476aa154acab3c79c0d1b6ef8ffe36b378239f5a48eee851b7a44.
#pragma once
#include <ddPlatform.h>
#include <gpuopen.h>
#include <cstring>
namespace DevDriver
{
// amdvlk RVA 0x5e3710: the default pipe has no suffix; nonzero ports append "-%hu".
template<size_t Size> inline Result MakePipeName(char (&name)[Size], uint16 port)
{
    static constexpr const char* Base = R"(\\.\pipe\AMD-Developer-Service)";
    const int32 length = port ? Platform::Snprintf(name, Size, "%s-%hu", Base, port)
                              : Platform::Snprintf(name, Size, "%s", Base);
    if (length <= 0 || static_cast<size_t>(length) > Size)
    {
        name[0] = '\0';
        return Result::InvalidParameter;
    }
    return Result::Success;
}
inline bool IsValidPipeName(const char* name)
{
    return name && (strncmp(name, R"(\\.\pipe\)", 9) == 0) && name[9] != '\0';
}
inline void LogPipeError(DWORD error)
{
    DD_PRINT(LogLevel::Warn, "Named pipe operation failed: Win32 error %lu", error);
}
}
