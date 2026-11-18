#include "palWin.h"
#include "palUuid.h"
#include <winsock2.h>
#include <iphlpapi.h>
#include <vector>
#include <bcrypt.h>
namespace Util::Uuid::Os
{
Node GetLocalNode()
{
    Node node = {};
    ULONG bytes = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &bytes);
    std::vector<uint8> storage(bytes);
    auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    if (bytes && GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &bytes) == NO_ERROR)
    {
        for (auto adapter = adapters; adapter; adapter = adapter->Next)
        {
            if (adapter->PhysicalAddressLength == sizeof(node.raw) && adapter->IfType != IF_TYPE_SOFTWARE_LOOPBACK)
            {
                memcpy(node.raw, adapter->PhysicalAddress, sizeof(node.raw));
                node.raw[0] |= 1;
                return node;
            }
        }
    }
    // A multicast node identifies a random/non-MAC node per RFC 4122.
    if (BCryptGenRandom(nullptr, node.raw, sizeof(node.raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    {
        LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
        const uint64 fallback = static_cast<uint64>(counter.QuadPart) ^ (static_cast<uint64>(GetCurrentProcessId()) << 32);
        memcpy(node.raw, &fallback, sizeof(node.raw));
    }
    node.raw[0] |= 1;
    return node;
}
Timestamp GetCurrentTimestamp()
{
    FILETIME now; GetSystemTimePreciseAsFileTime(&now);
    // FILETIME starts in 1601, UUID Gregorian time starts in 1582.
    return (static_cast<uint64>(now.dwHighDateTime) << 32 | now.dwLowDateTime) + 5748192000000000ULL;
}
uint64 GetFixedTimePoint() { return 138347712000000000ULL; } // 2021-03-01T00:00:00Z in UUID 100 ns units.
uint32_t GetSequenceStart()
{
    uint32 sequence = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&sequence), sizeof(sequence), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        { sequence = static_cast<uint32>(GetTickCount64()); }
    return sequence;
}
}
