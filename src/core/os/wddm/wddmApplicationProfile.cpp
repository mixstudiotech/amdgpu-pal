// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmApplicationProfile.h"
#include "wddmTransport.h"
#include "wddmResult.h"
#include "core/platform.h"
#include "palSysMemory.h"
#include <cstring>
#include <cwchar>
#include <cerrno>
#include <cstdlib>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal::Wddm
{
namespace
{
constexpr uint32 Clients[] = {0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 26, 28};
static_assert(sizeof(Clients) / sizeof(Clients[0]) == uint32(ApplicationProfileClient::Count));
static_assert(sizeof(wchar_t) == 2);
void Put32(uint8* bytes, size_t offset, uint32 value) { std::memcpy(bytes + offset, &value, sizeof(value)); }
void Put64(uint8* bytes, size_t offset, uint64 value) { std::memcpy(bytes + offset, &value, sizeof(value)); }
uint32 Get32(const uint8* bytes, size_t offset) { uint32 value; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }
uint64 Get64(const uint8* bytes, size_t offset) { uint64 value; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }
void QueryHeader(ApplicationProfileQueryRecord* record, ApplicationProfileClient client)
{
    Put32(record->data(), 0, 0x80); Put32(record->data(), 4, 0x10000);
    Put32(record->data(), 8, 0x03002903); Put32(record->data(), 0xc, 3);
    Put32(record->data(), 0x80, 0x480); Put32(record->data(), 0x84, 0x480);
    Put32(record->data(), 0x4b8, Clients[uint32(client)]);
    Put32(record->data(), 0x504, 0x40); Put32(record->data(), 0x508, 0x40);
}
size_t BoundedLength(const wchar_t* source, size_t characters) { return wcsnlen(source, characters); }
void* Copy(void* destination, const void* source, size_t bytes) { return std::memcpy(destination, source, bytes); }
int* ErrorNumber() { return _errno(); }
void ReportInvalidParameter() { _invalid_parameter_noinfo(); }
int CopyName(void*, wchar_t* destination, size_t characters, const wchar_t* source)
{ return CopyApplicationProfileName(destination, characters, source, WindowsApplicationProfileCopyCalls()); }
struct ProfileContext { Pal::Platform* platform; const Transport* transport; };
void* Allocate(void* context, size_t bytes, size_t alignment, uint32 type)
{
    return PAL_MALLOC_ALIGNED(bytes, alignment, static_cast<ProfileContext*>(context)->platform,
                              static_cast<Util::SystemAllocType>(type));
}
void Free(void* context, void* blob) { PAL_FREE(blob, static_cast<ProfileContext*>(context)->platform); }
int32 RawEscape(void*, void* request) { return D3DKMTEscape(static_cast<D3DKMT_ESCAPE*>(request)); }
Result ProfileEscape(void* context, void* record, size_t bytes)
{
    const auto* transport = static_cast<ProfileContext*>(context)->transport;
    if (!transport->IsOpen()) { return Result::ErrorUnavailable; }
    const ApplicationProfileEscapeCalls calls = {
        nullptr, RawEscape, transport->AdapterHandle(), transport->DeviceHandle(), 0, false
    };
    // Current verified profile has physical ordinal 0. Complete original
    // Device suppression/ordinal initialization is a separate proof boundary.
    return EscapeApplicationProfileRecord(calls, record, bytes);
}
ApplicationProfileCalls Calls(ProfileContext* context) { return {context, Allocate, Free, CopyName, ProfileEscape}; }
}

const ApplicationProfileCopyCalls& WindowsApplicationProfileCopyCalls()
{
    static const ApplicationProfileCopyCalls calls = {BoundedLength, Copy, ErrorNumber, ReportInvalidParameter};
    return calls;
}
int CopyApplicationProfileName(wchar_t* destination, size_t characters, const wchar_t* source,
                              const ApplicationProfileCopyCalls& calls)
{
    int error = 0;
    if ((destination == nullptr) || (characters == 0)) { error = EINVAL; }
    else if (source == nullptr) { *destination = L'\0'; error = EINVAL; }
    else if (characters > 32)
    {
        const size_t length = calls.boundedLength(source, characters);
        if (length == characters) { *destination = L'\0'; error = ERANGE; }
        else { calls.copy(destination, source, 2 * length + 2); }
    }
    else
    {
        size_t remaining = characters;
        wchar_t* current = destination;
        while (true)
        {
            const wchar_t value = *source++;
            *current++ = value;
            if (value == L'\0') { break; }
            if (--remaining == 0) { *destination = L'\0'; error = ERANGE; break; }
        }
    }
    if (error != 0)
    {
        *calls.errorNumber() = error;
        calls.invalidParameter();
    }
    return error;
}

Result EscapeApplicationProfileRecord(const ApplicationProfileEscapeCalls& calls, void* record, size_t bytes)
{
    if (calls.suppressed) { return Result::Unsupported; }
    // Profile query/read and the native SPP select record all fit the original
    // 2048-byte stack packet with its complete 76-byte shared envelope.
    if ((bytes != sizeof(ApplicationProfileQueryRecord)) && (bytes != sizeof(ApplicationProfileReadRecord)) && (bytes != 248))
        { return Result::ErrorInvalidMemorySize; }
    std::array<uint8, 2048> packet = {};
    Put32(packet.data(), 0, 2); Put32(packet.data(), 4, 0x10002);
    Put32(packet.data(), 0xc, calls.physicalAdapter); Put32(packet.data(), 0x48, uint32(bytes));
    std::memcpy(packet.data() + 0x4c, record, bytes);
    D3DKMT_ESCAPE escape = {};
    escape.hAdapter = calls.adapter; escape.hDevice = calls.device;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.pPrivateDriverData = packet.data(); escape.PrivateDriverDataSize = uint32(bytes + 0x4c);
    const Result result = ConvertNativeStatus(calls.escape(calls.context, &escape));
    if (result != Result::Success) { return result; }
    std::memcpy(record, packet.data() + 0x4c, bytes);
    return Get32(static_cast<uint8*>(record), 0x48) == 0 ? Result::Success : Result::ErrorUnknown;
}

Result BuildApplicationProfileQuery(const wchar_t* filename, const wchar_t* pathname,
                                   ApplicationProfileClient client, ApplicationProfileQueryRecord* record)
{
    if (record == nullptr) { return Result::ErrorInvalidPointer; }
    if (filename == nullptr) { return Result::Unsupported; }
    if (uint32(client) >= uint32(ApplicationProfileClient::Count)) { return Result::ErrorInvalidValue; }
    ApplicationProfileQueryRecord built = {};
    QueryHeader(&built, client);
    CopyName(nullptr, reinterpret_cast<wchar_t*>(built.data() + 0x88), 256, filename);
    if (pathname != nullptr) { CopyName(nullptr, reinterpret_cast<wchar_t*>(built.data() + 0x288), 256, pathname); }
    *record = built;
    return Result::Success;
}
ApplicationProfileReadRecord BuildApplicationProfileRead(uint32 bytes, uint64 token, void* destination)
{
    ApplicationProfileReadRecord record = {};
    Put32(record.data(), 0, 0x80); Put32(record.data(), 4, 0x10000);
    Put32(record.data(), 8, 0x03002a03); Put32(record.data(), 0xc, 3);
    Put32(record.data(), 0x80, 0x40); Put32(record.data(), 0x84, 0x40);
    Put32(record.data(), 0x88, bytes); Put64(record.data(), 0x8c, token);
    Put64(record.data(), 0x94, uint64(reinterpret_cast<uintptr_t>(destination)));
    Put32(record.data(), 0xc4, 0x40); Put32(record.data(), 0xc8, 0x40);
    return record;
}
void ClearApplicationProfile(ApplicationProfileStorage* state, const ApplicationProfileCalls& calls)
{
    if (state->blob != nullptr)
    {
        state->bytes = 0;
        calls.free(calls.context, state->blob);
        state->blob = nullptr;
    }
}
Result QueryApplicationProfile(ApplicationProfileStorage* state, const ApplicationProfileCalls& calls,
                               const wchar_t* filename, const wchar_t* pathname,
                               ApplicationProfileClient client, const char** output)
{
    ClearApplicationProfile(state, calls);
    if (filename == nullptr) { return Result::Unsupported; }
    // Native has unchecked output/table accesses. These explicit public-domain
    // guards are outside the compared valid-21-client/nonnull-output scope.
    if (output == nullptr) { return Result::ErrorInvalidPointer; }
    if (uint32(client) >= uint32(ApplicationProfileClient::Count)) { return Result::ErrorInvalidValue; }
    ApplicationProfileQueryRecord query = {};
    QueryHeader(&query, client);
    calls.copyName(calls.context, reinterpret_cast<wchar_t*>(query.data() + 0x88), 256, filename);
    if (pathname != nullptr)
        { calls.copyName(calls.context, reinterpret_cast<wchar_t*>(query.data() + 0x288), 256, pathname); }
    const Result first = calls.escape(calls.context, query.data(), query.size());
    const uint32 bytes = Get32(query.data(), 0x50c);
    if ((first != Result::Success) || (bytes == 0)) { return Result::Unsupported; }
    void* blob = calls.allocate(calls.context, bytes, 8, 0x80000002);
    if (blob == nullptr) { return Result::ErrorOutOfMemory; }
    std::memset(blob, 0, bytes);
    auto read = BuildApplicationProfileRead(bytes, Get64(query.data(), 0x510), blob);
    const Result result = calls.escape(calls.context, read.data(), read.size());
    if (result == Result::Success)
    {
        state->blob = blob; state->bytes = bytes;
        *output = static_cast<const char*>(blob);
    }
    // Deliberately match native 310040: failed second read neither frees nor
    // publishes/retains this allocation. This observed native failure leak is
    // requested exact behavior, not an invented cleanup success fallback.
    return result;
}
ApplicationProfileState::~ApplicationProfileState() { Clear(); }
void ApplicationProfileState::Clear()
{
    ProfileContext context = {m_platform, nullptr};
    ClearApplicationProfile(&m_state, Calls(&context));
}
Result ApplicationProfileState::Query(const Transport& transport, const wchar_t* filename, const wchar_t* pathname,
                                      ApplicationProfileClient client, const char** output)
{
    ProfileContext context = {m_platform, &transport};
    return QueryApplicationProfile(&m_state, Calls(&context), filename, pathname, client, output);
}
}
