// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once

#include "palPlatform.h"
#include <array>

namespace Pal { class Platform; }
namespace Pal::Wddm
{
class Transport;
constexpr uint32 ApplicationProfileNameCharacters = 256;
using ApplicationProfileQueryRecord = std::array<uint8, 0x548>;
using ApplicationProfileReadRecord = std::array<uint8, 0x108>;

struct ApplicationProfileCopyCalls
{
    size_t (*boundedLength)(const wchar_t*, size_t);
    void* (*copy)(void*, const void*, size_t);
    int* (*errorNumber)();
    void (*invalidParameter)();
};
const ApplicationProfileCopyCalls& WindowsApplicationProfileCopyCalls();
int CopyApplicationProfileName(wchar_t* destination, size_t characters, const wchar_t* source,
                              const ApplicationProfileCopyCalls& calls);

// Layout-independent method state and observable native dependency boundary.
struct ApplicationProfileStorage
{
    void* blob = nullptr;
    uint32 bytes = 0;
};
struct ApplicationProfileCalls
{
    void* context;
    void* (*allocate)(void*, size_t, size_t, uint32);
    void (*free)(void*, void*);
    int (*copyName)(void*, wchar_t*, size_t, const wchar_t*);
    Result (*escape)(void*, void*, size_t);
};
void ClearApplicationProfile(ApplicationProfileStorage* state, const ApplicationProfileCalls& calls);
Result QueryApplicationProfile(ApplicationProfileStorage* state, const ApplicationProfileCalls& calls,
                               const wchar_t* filename, const wchar_t* pathname,
                               ApplicationProfileClient client, const char** output);

struct ApplicationProfileEscapeCalls
{
    void* context;
    int32 (*escape)(void*, void*); // The second argument is an SDK D3DKMT_ESCAPE.
    uint32 adapter;
    uint32 device;
    uint32 physicalAdapter;
    bool suppressed;
};
Result EscapeApplicationProfileRecord(const ApplicationProfileEscapeCalls& calls, void* record, size_t bytes);

Result BuildApplicationProfileQuery(const wchar_t* filename, const wchar_t* pathname,
                                   ApplicationProfileClient client, ApplicationProfileQueryRecord* record);
ApplicationProfileReadRecord BuildApplicationProfileRead(uint32 bytes, uint64 token, void* destination);

// The blob is opaque client-defined data, not necessarily a NUL-terminated
// string. Its ownership is retained until the next query or state destruction.
class ApplicationProfileState
{
public:
    explicit ApplicationProfileState(Pal::Platform* platform) : m_platform(platform) {}
    ~ApplicationProfileState();
    Result Query(const Transport& transport, const wchar_t* filename, const wchar_t* pathname,
                 ApplicationProfileClient client, const char** output);
    void Clear();
    uint32 Size() const { return m_state.bytes; }
    ApplicationProfileState(const ApplicationProfileState&) = delete;
    ApplicationProfileState& operator=(const ApplicationProfileState&) = delete;
private:
    Pal::Platform* m_platform;
    ApplicationProfileStorage m_state;
};
}
