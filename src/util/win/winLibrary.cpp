#include "palLibrary.h"
namespace Util
{
Result Library::Load(const char* name)
{
    if (!name) { return Result::ErrorInvalidPointer; }
    if (m_hLib) { return Result::ErrorUnavailable; }
    m_hLib = LoadLibraryExA(name, nullptr, 0);
    return m_hLib ? Result::Success : Result::ErrorUnavailable;
}
void Library::Close() { if (m_hLib) { FreeLibrary(m_hLib); m_hLib = nullptr; } }
void Library::ReleaseWithoutClosing() { m_hLib = nullptr; }
void* Library::GetFunctionHelper(const char* name) const
{
    return m_hLib ? reinterpret_cast<void*>(GetProcAddress(m_hLib, name)) : nullptr;
}
}
