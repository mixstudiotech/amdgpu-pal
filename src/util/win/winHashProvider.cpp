#include "palWin.h"
#include "palHashProvider.h"
#include "palSysMemory.h"
#include <bcrypt.h>
#include <limits>

namespace Util
{
namespace
{
struct Provider
{
    BCRYPT_ALG_HANDLE handle = nullptr;
    ULONG objectBytes = 0;
    ULONG digestBytes = 0;
    explicit Provider(LPCWSTR algorithm)
    {
        if (BCryptOpenAlgorithmProvider(&handle, algorithm, nullptr, 0) >= 0)
        {
            ULONG bytes;
            if ((BCryptGetProperty(handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &bytes, 0) < 0) ||
                (BCryptGetProperty(handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &bytes, 0) < 0))
            {
                BCryptCloseAlgorithmProvider(handle, 0); handle = nullptr;
            }
        }
    }
    ~Provider() { if (handle) { BCryptCloseAlgorithmProvider(handle, 0); } }
};
Provider* GetProvider(HashAlgorithm algorithm)
{
    switch (algorithm)
    {
    case HashAlgorithm::Md5: { static Provider value(BCRYPT_MD5_ALGORITHM); return &value; }
    case HashAlgorithm::Sha1: { static Provider value(BCRYPT_SHA1_ALGORITHM); return &value; }
    case HashAlgorithm::Sha256: { static Provider value(BCRYPT_SHA256_ALGORITHM); return &value; }
    case HashAlgorithm::Sha384: { static Provider value(BCRYPT_SHA384_ALGORITHM); return &value; }
    case HashAlgorithm::Sha512: { static Provider value(BCRYPT_SHA512_ALGORITHM); return &value; }
    default: return nullptr;
    }
}
// SHA-224 uses SHA-256 compression with distinct initial state and a 224-bit digest (FIPS 180-4).
// CNG exposes no SHA-224 provider, so retain PAL's complete public algorithm set here.
class Sha224Context final : public IHashContext
{
public:
    Sha224Context() { Reset(); }
    Sha224Context(const Sha224Context& other) : IHashContext(), m_bytes(other.m_bytes), m_used(other.m_used)
    {
        memcpy(m_state, other.m_state, sizeof(m_state)); memcpy(m_block, other.m_block, sizeof(m_block));
    }
    Result AddData(const void* data, size_t bytes) override
    {
        if (bytes && !data) { return Result::ErrorInvalidPointer; }
        m_bytes += bytes;
        const auto* cursor = static_cast<const uint8*>(data);
        while (bytes)
        {
            const size_t count = (std::min)(bytes, sizeof(m_block) - m_used);
            memcpy(m_block + m_used, cursor, count); m_used += count; cursor += count; bytes -= count;
            if (m_used == sizeof(m_block)) { Compress(); m_used = 0; }
        }
        return Result::Success;
    }
    size_t GetOutputBufferSize() const override { return 28; }
    Result Finish(void* output) override
    {
        if (!output) { return Result::ErrorInvalidPointer; }
        Sha224Context final(*this);
        const uint64 bits = m_bytes * 8;
        const uint8 first = 0x80, zero = 0;
        final.AddData(&first, 1);
        while (final.m_used != 56) { final.AddData(&zero, 1); }
        uint8 length[8];
        for (size_t i = 0; i < sizeof(length); ++i) { length[i] = static_cast<uint8>(bits >> (56 - 8 * i)); }
        final.AddData(length, sizeof(length));
        auto* digest = static_cast<uint8*>(output);
        for (size_t i = 0; i < 28; ++i) { digest[i] = static_cast<uint8>(final.m_state[i / 4] >> (24 - 8 * (i % 4))); }
        return Result::Success;
    }
    Result Reset() override
    {
        const uint32 initial[8] = {0xc1059ed8,0x367cd507,0x3070dd17,0xf70e5939,0xffc00b31,0x68581511,0x64f98fa7,0xbefa4fa4};
        memcpy(m_state, initial, sizeof(initial)); memset(m_block, 0, sizeof(m_block)); m_bytes = 0; m_used = 0;
        return Result::Success;
    }
    size_t GetDuplicateObjectSize() const override { return sizeof(*this); }
    Result Duplicate(void* memory, IHashContext** output) const override
    {
        if (!memory || !output) { return Result::ErrorInvalidPointer; }
        *output = PAL_PLACEMENT_NEW(memory) Sha224Context(*this); return Result::Success;
    }
    void Destroy() override { this->~Sha224Context(); }
private:
    static uint32 Rotate(uint32 value, unsigned count) { return (value >> count) | (value << (32 - count)); }
    void Compress()
    {
        static constexpr uint32 Constants[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32 words[64];
        for (size_t i = 0; i < 16; ++i)
        {
            words[i] = (static_cast<uint32>(m_block[4*i]) << 24) | (static_cast<uint32>(m_block[4*i+1]) << 16) |
                       (static_cast<uint32>(m_block[4*i+2]) << 8) | m_block[4*i+3];
        }
        for (size_t i = 16; i < 64; ++i)
        {
            const uint32 x = words[i-15], y = words[i-2];
            words[i] = words[i-16] + (Rotate(x,7)^Rotate(x,18)^(x>>3)) + words[i-7] + (Rotate(y,17)^Rotate(y,19)^(y>>10));
        }
        uint32 a=m_state[0],b=m_state[1],c=m_state[2],d=m_state[3],e=m_state[4],f=m_state[5],g=m_state[6],h=m_state[7];
        for (size_t i = 0; i < 64; ++i)
        {
            const uint32 t1 = h + (Rotate(e,6)^Rotate(e,11)^Rotate(e,25)) + ((e&f)^((~e)&g)) + Constants[i] + words[i];
            const uint32 t2 = (Rotate(a,2)^Rotate(a,13)^Rotate(a,22)) + ((a&b)^(a&c)^(b&c));
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        m_state[0]+=a; m_state[1]+=b; m_state[2]+=c; m_state[3]+=d; m_state[4]+=e; m_state[5]+=f; m_state[6]+=g; m_state[7]+=h;
    }
    uint32 m_state[8];
    uint8 m_block[64];
    uint64 m_bytes;
    size_t m_used;
};

class HashContext final : public IHashContext
{
public:
    HashContext(Provider* provider, BCRYPT_HASH_HANDLE hash) : m_provider(provider), m_hash(hash) {}
    ~HashContext() override { if (m_hash) { BCryptDestroyHash(m_hash); } }
    Result AddData(const void* data, size_t bytes) override
    {
        if (bytes && !data) { return Result::ErrorInvalidPointer; }
        if (!m_provider) { return Result::Success; }
        const auto* cursor = static_cast<const uint8*>(data);
        while (bytes)
        {
            const ULONG chunk = static_cast<ULONG>((std::min)(bytes, static_cast<size_t>((std::numeric_limits<ULONG>::max)())));
            if (BCryptHashData(m_hash, const_cast<PUCHAR>(cursor), chunk, 0) < 0) { return Result::ErrorUnknown; }
            cursor += chunk; bytes -= chunk;
        }
        return Result::Success;
    }
    size_t GetOutputBufferSize() const override { return m_provider ? m_provider->digestBytes : 0; }
    Result Finish(void* output) override
    {
        if (!m_provider) { return Result::Success; }
        if (!output) { return Result::ErrorInvalidPointer; }
        return BCryptFinishHash(m_hash, static_cast<PUCHAR>(output), m_provider->digestBytes, 0) >= 0 ? Result::Success : Result::ErrorUnknown;
    }
    Result Reset() override
    {
        if (!m_provider) { return Result::Success; }
        if (m_hash) { BCryptDestroyHash(m_hash); m_hash = nullptr; }
        return BCryptCreateHash(m_provider->handle, &m_hash, reinterpret_cast<PUCHAR>(this + 1), m_provider->objectBytes, nullptr, 0, 0) >= 0
            ? Result::Success : Result::ErrorUnknown;
    }
    size_t GetDuplicateObjectSize() const override { return sizeof(*this) + (m_provider ? m_provider->objectBytes : 0); }
    Result Duplicate(void* memory, IHashContext** output) const override
    {
        if (!memory || !output) { return Result::ErrorInvalidPointer; }
        *output = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (m_provider && BCryptDuplicateHash(m_hash, &hash, static_cast<PUCHAR>(memory) + sizeof(HashContext), m_provider->objectBytes, 0) < 0)
            { return Result::ErrorUnknown; }
        *output = PAL_PLACEMENT_NEW(memory) HashContext(m_provider, hash);
        return Result::Success;
    }
    void Destroy() override { this->~HashContext(); }
private:
    Provider* m_provider;
    BCRYPT_HASH_HANDLE m_hash;
};
}
Result GetHashContextInfo(HashAlgorithm algorithm, HashContextInfo* info)
{
    if (!info) { return Result::ErrorInvalidPointer; }
    *info = {};
    if (algorithm == HashAlgorithm::Sha224)
    {
        info->contextObjectSize = sizeof(Sha224Context); info->contextObjectAlignment = alignof(Sha224Context); info->outputBufferSize = 28;
        return Result::Success;
    }
    Provider* provider = GetProvider(algorithm);
    if (algorithm != HashAlgorithm::NoOp && !provider) { return Result::Unsupported; }
    if (provider && !provider->handle) { return Result::ErrorUnavailable; }
    info->contextObjectSize = sizeof(HashContext) + (provider ? provider->objectBytes : 0);
    info->contextObjectAlignment = alignof(HashContext);
    info->outputBufferSize = provider ? provider->digestBytes : 0;
    return Result::Success;
}
Result CreateHashContext(HashAlgorithm algorithm, void* memory, IHashContext** output)
{
    if (!memory || !output) { return Result::ErrorInvalidPointer; }
    *output = nullptr;
    HashContextInfo info;
    const Result result = GetHashContextInfo(algorithm, &info);
    if (result != Result::Success) { return result; }
    if (algorithm == HashAlgorithm::Sha224) { *output = PAL_PLACEMENT_NEW(memory) Sha224Context(); return Result::Success; }
    Provider* provider = GetProvider(algorithm);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (provider && BCryptCreateHash(provider->handle, &hash, static_cast<PUCHAR>(memory) + sizeof(HashContext), provider->objectBytes, nullptr, 0, 0) < 0)
        { return Result::ErrorUnknown; }
    *output = PAL_PLACEMENT_NEW(memory) HashContext(provider, hash);
    return Result::Success;
}
}
