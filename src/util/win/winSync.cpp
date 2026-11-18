// Native Windows synchronization primitives. See the contracts in pal/inc/util.
#include "palWin.h"
#include "palMutex.h"
#include "palConditionVariable.h"
#include "palEvent.h"
#include "palSemaphore.h"
#include <cmath>
#include <limits>

namespace Util
{
namespace
{
DWORD Milliseconds(std::chrono::milliseconds duration)
{
    if (duration == (std::chrono::milliseconds::max)()) { return INFINITE; }
    if (duration.count() <= 0) { return 0; }
    return static_cast<DWORD>((std::min)(duration.count(), static_cast<decltype(duration.count())>(INFINITE - 1)));
}
Result WaitResult(DWORD result)
{
    return result == WAIT_OBJECT_0 ? Result::Success : result == WAIT_TIMEOUT ? Result::Timeout : Result::ErrorUnknown;
}
}
void Mutex::Lock() { EnterCriticalSection(&m_osMutex); }
bool Mutex::TryLock() { return TryEnterCriticalSection(&m_osMutex) != 0; }
void Mutex::Unlock() { LeaveCriticalSection(&m_osMutex); }
void RWLock::LockForRead() { AcquireSRWLockShared(&m_osRWLock); }
void RWLock::LockForWrite() { AcquireSRWLockExclusive(&m_osRWLock); }
bool RWLock::TryLockForRead() { return TryAcquireSRWLockShared(&m_osRWLock) != 0; }
bool RWLock::TryLockForWrite() { return TryAcquireSRWLockExclusive(&m_osRWLock) != 0; }
void RWLock::UnlockForRead() { ReleaseSRWLockShared(&m_osRWLock); }
void RWLock::UnlockForWrite() { ReleaseSRWLockExclusive(&m_osRWLock); }
bool ConditionVariable::Wait(Mutex* pMutex, std::chrono::milliseconds timeout)
{
    return SleepConditionVariableCS(&m_osCondVariable, pMutex->GetMutexData(), Milliseconds(timeout)) != 0;
}
void ConditionVariable::WakeOne() { WakeConditionVariable(&m_osCondVariable); }
void ConditionVariable::WakeAll() { WakeAllConditionVariable(&m_osCondVariable); }
const Event::EventHandle Event::InvalidEvent = nullptr;
Event::Event() : m_hEvent(InvalidEvent), m_isReference(false) {}
Event::~Event() { if ((m_hEvent != InvalidEvent) && !m_isReference) { CloseHandle(m_hEvent); } }
Result Event::Init(const EventCreateFlags& flags)
{
    if (m_hEvent != InvalidEvent) { return Result::ErrorUnavailable; }
    if (flags.reserved || flags.semaphore) { return Result::ErrorInvalidFlags; }
    m_hEvent = CreateEventW(nullptr, flags.manualReset, flags.initiallySignaled, nullptr);
    return m_hEvent ? Result::Success : Result::ErrorInitializationFailed;
}
Result Event::Set() const { return !m_hEvent ? Result::ErrorUnavailable : SetEvent(m_hEvent) ? Result::Success : Result::ErrorUnknown; }
Result Event::Reset() const { return !m_hEvent ? Result::ErrorUnavailable : ResetEvent(m_hEvent) ? Result::Success : Result::ErrorUnknown; }
Result Event::Wait(fseconds timeout) const
{
    if (!m_hEvent) { return Result::ErrorUnavailable; }
    if (std::isnan(timeout.count()) || timeout.count() < 0) { return Result::ErrorInvalidValue; }
    const double millis = std::ceil(static_cast<double>(timeout.count()) * 1000.0);
    const DWORD wait = (!std::isfinite(millis) || millis >= INFINITE) ? INFINITE : static_cast<DWORD>(millis);
    return WaitResult(WaitForSingleObject(m_hEvent, wait));
}
Result Event::Open(EventHandle handle, bool isReference)
{
    if (m_hEvent) { return Result::ErrorUnavailable; }
    if ((handle == nullptr) || (handle == INVALID_HANDLE_VALUE)) { return Result::ErrorInvalidExternalHandle; }
    if (isReference) { m_hEvent = handle; m_isReference = true; return Result::Success; }
    return DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &m_hEvent, 0, FALSE, DUPLICATE_SAME_ACCESS)
        ? Result::Success : Result::ErrorInvalidExternalHandle;
}
Semaphore::~Semaphore() { if (m_osSemaphore) { CloseHandle(m_osSemaphore); } }
Result Semaphore::Init(uint32 maximumCount, uint32 initialCount)
{
    if (m_osSemaphore) { return Result::ErrorUnavailable; }
    if ((maximumCount == 0) || (maximumCount > MaximumCountLimit) || (initialCount > maximumCount)) { return Result::ErrorInvalidValue; }
    m_osSemaphore = CreateSemaphoreW(nullptr, initialCount, maximumCount, nullptr);
    return m_osSemaphore ? Result::Success : Result::ErrorInitializationFailed;
}
Result Semaphore::Wait(std::chrono::milliseconds waitTime)
{
    return m_osSemaphore ? WaitResult(WaitForSingleObject(m_osSemaphore, Milliseconds(waitTime))) : Result::ErrorUnavailable;
}
void Semaphore::Post(uint32 postCount)
{
    const BOOL result = ReleaseSemaphore(m_osSemaphore, static_cast<LONG>(postCount), nullptr);
    PAL_ASSERT(result != FALSE);
}

void YieldThread() { SwitchToThread(); }
void AtomicWriteRelaxed64(volatile uint64* target, uint64 value)
{
    PAL_ASSERT((reinterpret_cast<uintptr_t>(target) & (alignof(uint64) - 1)) == 0);
#if defined(__clang__)
    __atomic_store_n(target, value, __ATOMIC_RELAXED);
#else
    static_assert(sizeof(void*) == 8, "Aligned 64-bit relaxed accesses require a 64-bit Windows target");
    *target = value;
#endif
}
uint64 AtomicReadRelaxed64(const volatile uint64* target)
{
    PAL_ASSERT((reinterpret_cast<uintptr_t>(target) & (alignof(uint64) - 1)) == 0);
#if defined(__clang__)
    return __atomic_load_n(target, __ATOMIC_RELAXED);
#else
    static_assert(sizeof(void*) == 8, "Aligned 64-bit relaxed accesses require a 64-bit Windows target");
    return *target;
#endif
}
uint32 AtomicIncrement(volatile uint32* value)
{ return static_cast<uint32>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(value))); }
uint64 AtomicIncrement64(volatile uint64* value)
{ return static_cast<uint64>(InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(value))); }
uint32 AtomicDecrement(volatile uint32* value)
{ return static_cast<uint32>(InterlockedDecrement(reinterpret_cast<volatile LONG*>(value))); }
uint32 AtomicDecrement64(volatile uint64* value)
{
    // Preserve PAL's published uint32 return type for this 64-bit operation.
    return static_cast<uint32>(InterlockedDecrement64(reinterpret_cast<volatile LONG64*>(value)));
}
uint32 AtomicCompareAndSwap(volatile uint32* target, uint32 oldValue, uint32 newValue)
{
    return static_cast<uint32>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(target),
                                                         static_cast<LONG>(newValue), static_cast<LONG>(oldValue)));
}
uint32 AtomicExchange(volatile uint32* target, uint32 value)
{ return static_cast<uint32>(InterlockedExchange(reinterpret_cast<volatile LONG*>(target), static_cast<LONG>(value))); }
uint64 AtomicExchange64(volatile uint64* target, uint64 value)
{ return static_cast<uint64>(InterlockedExchange64(reinterpret_cast<volatile LONG64*>(target), static_cast<LONG64>(value))); }
void* AtomicExchangePointer(void*volatile* target, void* value)
{ return InterlockedExchangePointer(target, value); }
void* AtomicCompareExchangePointer(void*volatile* target, void* oldValue, void* newValue)
{ return InterlockedCompareExchangePointer(target, newValue, oldValue); }
uint32 AtomicAdd(volatile uint32* target, uint32 value)
{
    return static_cast<uint32>(InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(target), static_cast<LONG>(value))) + value;
}
uint64 AtomicAdd64(volatile uint64* target, uint64 value)
{
    return static_cast<uint64>(InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(target), static_cast<LONG64>(value))) + value;
}
uint32 AtomicOr(volatile uint32* target, uint32 value)
{ return static_cast<uint32>(InterlockedOr(reinterpret_cast<volatile LONG*>(target), static_cast<LONG>(value))); }
uint64 AtomicOr64(volatile uint64* target, uint64 value)
{ return static_cast<uint64>(InterlockedOr64(reinterpret_cast<volatile LONG64*>(target), static_cast<LONG64>(value))); }
uint32 AtomicAnd(volatile uint32* target, uint32 value)
{ return static_cast<uint32>(InterlockedAnd(reinterpret_cast<volatile LONG*>(target), static_cast<LONG>(value))); }
uint64 AtomicAnd64(volatile uint64* target, uint64 value)
{ return static_cast<uint64>(InterlockedAnd64(reinterpret_cast<volatile LONG64*>(target), static_cast<LONG64>(value))); }

}
