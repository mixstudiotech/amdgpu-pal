// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmDriverVersion.h"
#include "wddmTransport.h"
#include "wddmResult.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <array>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cerrno>

namespace Pal::Wddm
{
namespace
{
constexpr size_t RegistryOutputBytes = 1024;
constexpr size_t RegistryOutputOffset = offsetof(D3DDDI_QUERYREGISTRY_INFO, OutputString);
constexpr size_t RegistryPacketBytes = RegistryOutputOffset + RegistryOutputBytes;
static_assert(RegistryOutputOffset == 0x220 && RegistryPacketBytes == 1568,
              "Native389b80 uses a1568-byte adapter-key query with1024 output bytes");
void Put32(uint8_t* bytes, size_t offset, uint32_t value)
{ std::memcpy(bytes + offset, &value, sizeof(value)); }
uint32_t Get32(const uint8_t* bytes, size_t offset)
{ uint32_t value; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }

bool ReadVersionString(const Transport& transport, const wchar_t* name, std::array<char, 512>* value)
{
    alignas(D3DDDI_QUERYREGISTRY_INFO) std::array<uint8_t, RegistryPacketBytes> data = {};
    Put32(data.data(), offsetof(D3DDDI_QUERYREGISTRY_INFO, QueryType), D3DDDI_QUERYREGISTRY_ADAPTERKEY);
    Put32(data.data(), offsetof(D3DDDI_QUERYREGISTRY_INFO, ValueType), REG_SZ);
    // Zero flags/physical index match original scope0 on this single-adapter profile.
    const size_t nameChars = std::wcslen(name) + 1;
    if (nameChars > MAX_PATH) { return false; }
    std::memcpy(data.data() + offsetof(D3DDDI_QUERYREGISTRY_INFO, ValueName), name, nameChars * sizeof(wchar_t));
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = transport.AdapterHandle();
    query.Type = KMTQAITYPE_QUERYREGISTRY;
    query.pPrivateDriverData = data.data();
    query.PrivateDriverDataSize = static_cast<UINT>(data.size());
    if (ConvertNativeStatus(D3DKMTQueryAdapterInfo(&query)) != Result::Success ||
        Get32(data.data(), offsetof(D3DDDI_QUERYREGISTRY_INFO, Status)) != D3DDDI_QUERYREGISTRY_STATUS_SUCCESS ||
        Get32(data.data(), offsetof(D3DDDI_QUERYREGISTRY_INFO, OutputValueSize)) > RegistryOutputBytes)
    { return false; }
    // Preserve the native1024-byte limit. The extra local terminator bounds an
    // invalid nonterminated response; correctly typedREG_SZ responses are unchanged.
    std::array<wchar_t, RegistryOutputBytes / sizeof(wchar_t) + 1> wide = {};
    std::memcpy(wide.data(), data.data() + RegistryOutputOffset, RegistryOutputBytes);
    size_t converted = 0;
    const errno_t result = wcstombs_s(&converted, value->data(), value->size(), wide.data(), _TRUNCATE);
    return (result == 0 || result == STRUNCATE) && (*value)[0] != '\0';
}
Result QueryVersion(const Transport& transport, char* buffer, size_t length, bool software)
{
    // Native317840/317960 check environment before invalid output arguments.
    if (!transport.IsOpen()) { return Result::Unsupported; }
    if (buffer == nullptr || length == 0) { return Result::ErrorInvalidValue; }
    std::array<char, 512> value = {};
    const bool found = software
        ? (ReadVersionString(transport, L"CloudSoftwareVersion", &value) ||
           ReadVersionString(transport, L"RadeonSoftwareVersion", &value) ||
           ReadVersionString(transport, L"FireproSoftwareVersion", &value))
        : ReadVersionString(transport, L"ReleaseVersion", &value);
    if (!found) { return Result::NotFound; }
    // Original ValueType10 conversion calls2e8ab70 ->strncpy_s with_TRUNCATE
    // and ignores STRUNCATE80. Tiny buffers therefore still reportSuccess.
    static_cast<void>(strncpy_s(buffer, length, value.data(), _TRUNCATE));
    return Result::Success;
}
}

Result QueryDriverSoftwareVersion(const Transport& transport, char* buffer, size_t length)
{ return QueryVersion(transport, buffer, length, true); }
Result QueryDriverReleaseVersion(const Transport& transport, char* buffer, size_t length)
{ return QueryVersion(transport, buffer, length, false); }
}
