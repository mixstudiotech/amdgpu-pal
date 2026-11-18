#include "palWin.h"
#include "palThread.h"
#include "palAssert.h"
#include <process.h>
#include <string>

namespace Util
{
ThreadId Thread::GetCurrentThreadId() { return ::GetCurrentThreadId(); }
Thread::Thread() : m_pfnStartFunction(nullptr), m_pStartParameter(nullptr), m_threadHandle(nullptr), m_threadId(0), m_ownedThread(false) {}
Thread::~Thread() { if (m_threadHandle) { CloseHandle(m_threadHandle); } }
unsigned PAL_STDCALL Thread::StartThread(void* object)
{
    Thread* thread = static_cast<Thread*>(object);
    // Copy before invoking: the callback may detach/delete its wrapper.
    const StartFunction function = thread->m_pfnStartFunction;
    void* parameter = thread->m_pStartParameter;
    function(parameter);
    return 0;
}
Result Thread::Begin(StartFunction function, void* parameter, uint32 priority)
{
    if (m_threadHandle) { return Result::ErrorUnavailable; }
    if (!function) { return Result::ErrorInvalidPointer; }
    m_pfnStartFunction = function;
    m_pStartParameter = parameter;
    unsigned id = 0;
    m_threadHandle = reinterpret_cast<void*>(_beginthreadex(nullptr, 0, StartThread, this, CREATE_SUSPENDED, &id));
    if (!m_threadHandle) { return Result::ErrorUnknown; }
    m_threadId = id;
    m_ownedThread = true;
    if ((priority != 0) && !SetThreadPriority(m_threadHandle, static_cast<int>(priority)))
    {
        // Never leave a suspended thread alive on a failed initialization.
        TerminateThread(m_threadHandle, 1);
        CloseHandle(m_threadHandle);
        m_threadHandle = nullptr;
        m_ownedThread = false;
        return Result::ErrorInvalidValue;
    }
    if (ResumeThread(m_threadHandle) == static_cast<DWORD>(-1))
    {
        TerminateThread(m_threadHandle, 1);
        CloseHandle(m_threadHandle);
        m_threadHandle = nullptr;
        m_ownedThread = false;
        return Result::ErrorUnknown;
    }
    return Result::Success;
}
Result Thread::SetToSelf()
{
    if (m_threadHandle) { return Result::ErrorUnavailable; }
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_threadHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
        { return Result::ErrorUnknown; }
    m_threadId = ::GetCurrentThreadId();
    m_ownedThread = false;
    return Result::Success;
}
void Thread::Join()
{
    PAL_ASSERT(IsNotCurrentThread());
    if (m_threadHandle && m_ownedThread)
    {
        const DWORD result = WaitForSingleObject(m_threadHandle, INFINITE);
        PAL_ASSERT(result == WAIT_OBJECT_0);
        CloseHandle(m_threadHandle);
        m_threadHandle = nullptr;
        m_threadId = 0;
        m_ownedThread = false;
    }
}
[[noreturn]] void Thread::End() { PAL_ASSERT(IsCurrentThread()); _endthreadex(0); __assume(0); }
bool Thread::IsCurrentThread() const { return m_threadHandle && (m_threadId == ::GetCurrentThreadId()); }
bool Thread::IsCreated() const { return m_threadHandle && m_ownedThread; }
Result Thread::SetThreadName(const char* name) const
{
    if (!name) { return Result::ErrorInvalidPointer; }
    if (!m_threadHandle) { return Result::ErrorUnavailable; }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
    if (length == 0) { return Result::ErrorInvalidValue; }
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, text.data(), length);
    return SUCCEEDED(SetThreadDescription(m_threadHandle, text.c_str())) ? Result::Success : Result::ErrorUnknown;
}
Result CreateThreadLocalKey(ThreadLocalKey* key, ThreadLocalDestructor destructor)
{
    if (!key) { return Result::ErrorInvalidPointer; }
    *key = FlsAlloc(reinterpret_cast<PFLS_CALLBACK_FUNCTION>(destructor));
    return *key == FLS_OUT_OF_INDEXES ? Result::ErrorUnavailable : Result::Success;
}
Result DeleteThreadLocalKey(ThreadLocalKey key) { return FlsFree(key) ? Result::Success : Result::ErrorUnknown; }
void* GetThreadLocalValue(ThreadLocalKey key) { return FlsGetValue(key); }
Result SetThreadLocalValue(ThreadLocalKey key, void* value) { return FlsSetValue(key, value) ? Result::Success : Result::ErrorUnknown; }
}
