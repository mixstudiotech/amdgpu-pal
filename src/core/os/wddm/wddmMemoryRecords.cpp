// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmMemoryRecords.h"
#include "core/os/wddm/wddmPrivate.h"
#include "palFormatInfo.h"
#include <cstring>

namespace Pal::Wddm::Private
{
namespace
{
constexpr size_t AllocationHeaderBytes = 64;
constexpr size_t EntryPrefixBytes      = 196;
constexpr size_t NativeMetadataBytes   = 276;
constexpr size_t BoOffset              = 8;

// ImageCreateFlags / ImageUsageFlags bits read by the image section (same bit order in the original).
constexpr uint32_t ImagePresentable      = 1u << 3;
constexpr uint32_t ImageFlippable        = 1u << 4;
constexpr uint32_t ImageDxgiStereo       = 1u << 6;
constexpr uint32_t ImageCubemap          = 1u << 7;
constexpr uint32_t ImageOptimalShareable = 1u << 16;
constexpr uint32_t ImagePipSwapChain     = 1u << 20;
constexpr uint32_t UsageShaderRead       = 1u << 0;
constexpr uint32_t UsageShaderWrite      = 1u << 1;
constexpr uint32_t UsageColorTarget      = 1u << 4;
constexpr uint32_t UsageDepthStencil     = 1u << 5;
constexpr uint32_t UsageDepthAsZ24       = 1u << 8;
constexpr uint32_t FormatX8Uint          = 0x12;

struct KmdFormatEntry
{
    uint32_t format;
    uint32_t swizzle;
    uint32_t kmdFormat;
};
// Native table 5837c10: per ChNumFormat up to 3 {swizzle, KMD format} pairs (first match wins; rows without a
// non-zero KMD format omitted). Formats 0..205 only: the reference has four further formats this PAL lacks.
constexpr KmdFormatEntry ViewFormatTable[] =
{
    {0x01, 0x01000002, 402}, {0x06, 0x05020304,  26}, {0x08, 0x01020304,  23}, {0x0a, 0x05020304,  25}, {0x0e, 0x01000002, 397},
    {0x0f, 0x01000002, 397}, {0x10, 0x01000002, 397}, {0x11, 0x01000002, 397}, {0x12, 0x01000002, 397}, {0x13, 0x01000002, 397},
    {0x14, 0x01000002, 397}, {0x18, 0x01000302, 387}, {0x19, 0x01000302, 387}, {0x1a, 0x01000302, 387}, {0x1b, 0x01000302, 387},
    {0x1c, 0x01000302, 387}, {0x1d, 0x01000302, 387}, {0x1e, 0x01000302, 387}, {0x20, 0x05040302, 371}, {0x20, 0x05020304, 419},
    {0x20, 0x01020304, 421}, {0x21, 0x05040302, 371}, {0x21, 0x05020304, 419}, {0x21, 0x01020304, 421}, {0x22, 0x05040302, 371},
    {0x22, 0x05020304, 419}, {0x22, 0x01020304, 421}, {0x23, 0x05040302, 371}, {0x23, 0x05020304, 419}, {0x23, 0x01020304, 421},
    {0x24, 0x05040302, 371}, {0x24, 0x05020304, 419}, {0x24, 0x01020304, 421}, {0x25, 0x05040302, 371}, {0x25, 0x05020304, 419},
    {0x25, 0x01020304, 421}, {0x26, 0x05040302, 371}, {0x26, 0x05020304, 419}, {0x26, 0x01020304, 421}, {0x27, 0x01040302,  62},
    {0x29, 0x01040302, 370}, {0x2a, 0x05040302, 368}, {0x2a, 0x05020304,  35}, {0x2b, 0x05040302, 368}, {0x2b, 0x05020304,  35},
    {0x2c, 0x05040302, 368}, {0x2d, 0x05040302, 368}, {0x2d, 0x05020304,  35}, {0x2e, 0x05040302, 368}, {0x2f, 0x05040302, 368},
    {0x2f, 0x05020304,  35}, {0x30, 0x05040302, 119}, {0x32, 0x01000002, 392}, {0x32, 0x01010102, 392}, {0x33, 0x01000002, 392},
    {0x33, 0x01010102, 392}, {0x34, 0x01000002, 392}, {0x34, 0x01010102, 392}, {0x35, 0x01000002, 392}, {0x35, 0x01010102, 392},
    {0x36, 0x01000002, 392}, {0x36, 0x01010102, 392}, {0x37, 0x01000002, 392}, {0x37, 0x01010102, 392}, {0x38, 0x01000002, 392},
    {0x38, 0x01010102, 392}, {0x3a, 0x01000302, 376}, {0x3a, 0x01010302, 376}, {0x3b, 0x01000302, 376}, {0x3b, 0x01010302, 376},
    {0x3c, 0x01000302, 376}, {0x3c, 0x01010302, 376}, {0x3d, 0x01000302, 376}, {0x3d, 0x01010302, 376}, {0x3e, 0x01000302, 376},
    {0x3e, 0x01010302, 376}, {0x3f, 0x01000302, 376}, {0x3f, 0x01010302, 376}, {0x40, 0x01000302, 376}, {0x40, 0x01010302, 376},
    {0x41, 0x05040302, 357}, {0x42, 0x05040302, 357}, {0x43, 0x05040302, 357}, {0x44, 0x05040302, 357}, {0x45, 0x05040302, 357},
    {0x46, 0x05040302, 357}, {0x47, 0x05040302, 357}, {0x48, 0x01000002, 380}, {0x48, 0x01010102, 380}, {0x49, 0x01000002, 380},
    {0x49, 0x01010102, 380}, {0x4a, 0x01000002, 380}, {0x4a, 0x01010102, 380}, {0x4b, 0x01000302, 361}, {0x4b, 0x01010302, 361},
    {0x4c, 0x01000302, 361}, {0x4c, 0x01010302, 361}, {0x4d, 0x01000302, 361}, {0x4d, 0x01010302, 361}, {0x4e, 0x01040302, 353},
    {0x4f, 0x01040302, 353}, {0x50, 0x01040302, 353}, {0x51, 0x05040302, 350}, {0x52, 0x05040302, 350}, {0x53, 0x05040302, 350},
    {0x54, 0x01000302,  80}, {0x55, 0x01000302, 381}, {0x56, 0x01040302, 403}, {0x57, 0x05040302, 404}, {0x58, 0x05040302, 404},
    {0x59, 0x05040302, 407}, {0x5a, 0x05040302, 407}, {0x5b, 0x05040302, 410}, {0x5c, 0x05040302, 410}, {0x5d, 0x01000002, 413},
    {0x5e, 0x01000002, 413}, {0x5f, 0x01000302, 416}, {0x60, 0x01000302, 416}, {0x61, 0x01040302, 423}, {0x62, 0x01040302, 423},
    {0x63, 0x05040302, 426}, {0x64, 0x05040302, 426}, {0x99, 0x01040302, 1195525970}, {0x9a, 0x01040302, 1195525970}, {0x9b, 0x01040302, 1111970375},
    {0x9c, 0x01040302, 1111970375}, {0x9d, 0x05040302, 1448433985}, {0x9e, 0x01040302, 1498831189}, {0x9f, 0x01040302, 1498831189}, {0xa0, 0x01040302, 844715353},
    {0xa1, 0x01040302, 844715353}, {0xa2, 0x01040302, 842094169}, {0xa3, 0x01040302, 825316942}, {0xa4, 0x01040302, 842094158}, {0xa5, 0x01040302, 825382478},
    {0xa6, 0x01040302, 909193296}, {0xa7, 0x01040302, 808530000}, {0xa8, 0x01040302, 808530512}, {0xb1, 0x01040302, 942682704}, {0xba, 0x01040302, 909193817},
    {0xbb, 0x01040302, 808530521}, {0xbc, 0x05040302, 909194329}, {0xbd, 0x05040302, 808531033},
};
// Native table 5838fc0: per ChNumFormat up to 6 {swizzle, KMD format} pairs (first match wins; rows without a
// non-zero KMD format omitted). Formats 0..205 only: the reference has four further formats this PAL lacks.
constexpr KmdFormatEntry PlainFormatTable[] =
{
    {0x01, 0x01000002, 402}, {0x05, 0x03020202,  52}, {0x06, 0x05020304,  26}, {0x06, 0x05040302, 429}, {0x06, 0x02030405, 430},
    {0x06, 0x02050403, 431}, {0x06, 0x01040302, 434}, {0x06, 0x01020304,  30}, {0x08, 0x01020304,  23}, {0x08, 0x01040302, 432},
    {0x0a, 0x05020304,  25}, {0x0a, 0x01020304,  24}, {0x0a, 0x05040302, 435}, {0x0a, 0x01040302, 436}, {0x0a, 0x04030205, 437},
    {0x0a, 0x02030405, 438}, {0x0e, 0x01000002, 398}, {0x0e, 0x01010102, 398}, {0x0f, 0x01000002, 400}, {0x0f, 0x01010102, 400},
    {0x12, 0x01000002, 399}, {0x12, 0x01010102, 399}, {0x13, 0x01000002, 401}, {0x13, 0x01010102, 401}, {0x15, 0x02000000,  28},
    {0x16, 0x01020202,  50}, {0x17, 0x00020202,  41}, {0x18, 0x01000302, 388}, {0x18, 0x01010302, 388}, {0x19, 0x01000302, 390},
    {0x19, 0x01010302, 390}, {0x1c, 0x01000302, 389}, {0x1c, 0x01010302, 389}, {0x1d, 0x01000302, 391}, {0x1d, 0x01010302, 391},
    {0x1f, 0x03020202,  51}, {0x20, 0x05040302,  32}, {0x20, 0x01040302,  33}, {0x20, 0x05020304,  21}, {0x20, 0x01020304,  22},
    {0x20, 0x04030205, 439}, {0x20, 0x02030405, 440}, {0x21, 0x05040302, 374}, {0x21, 0x01040302, 441}, {0x21, 0x05020304, 433},
    {0x21, 0x01020304, 442}, {0x21, 0x04030205, 443}, {0x21, 0x02030405, 444}, {0x24, 0x05040302, 373}, {0x24, 0x01040302, 445},
    {0x24, 0x05020304, 446}, {0x24, 0x01020304, 447}, {0x24, 0x04030205, 448}, {0x24, 0x02030405, 449}, {0x25, 0x05040302, 375},
    {0x25, 0x01040302, 450}, {0x25, 0x05020304, 451}, {0x25, 0x01020304, 452}, {0x25, 0x04030205, 453}, {0x25, 0x02030405, 454},
    {0x26, 0x05040302, 372}, {0x26, 0x01040302, 455}, {0x26, 0x05020304, 420}, {0x26, 0x01020304, 422}, {0x27, 0x01040302,  62},
    {0x29, 0x01040302, 370}, {0x2a, 0x05040302,  31}, {0x2a, 0x01040302, 456}, {0x2a, 0x05020304,  35}, {0x2a, 0x01020304, 457},
    {0x2a, 0x04030205, 458}, {0x2a, 0x02030405, 459}, {0x2b, 0x05040302, 460}, {0x2b, 0x01040302, 461}, {0x2b, 0x05020304, 462},
    {0x2b, 0x01020304, 463}, {0x2b, 0x04030205, 464}, {0x2b, 0x02030405, 465}, {0x2e, 0x05040302, 369}, {0x2e, 0x01040302, 466},
    {0x2e, 0x05020304, 467}, {0x2e, 0x01020304, 468}, {0x2e, 0x04030205, 469}, {0x2e, 0x02030405, 470}, {0x2f, 0x05040302, 471},
    {0x2f, 0x01040302, 472}, {0x2f, 0x05020304, 473}, {0x2f, 0x01020304, 474}, {0x2f, 0x04030205, 475}, {0x2f, 0x02030405, 476},
    {0x30, 0x05040302, 119}, {0x31, 0x05040302,  67}, {0x32, 0x01000002, 393}, {0x32, 0x01010102, 393}, {0x33, 0x01000002, 395},
    {0x33, 0x01010102, 395}, {0x36, 0x01000002, 394}, {0x36, 0x01010102, 394}, {0x37, 0x01000002, 396}, {0x37, 0x01010102, 396},
    {0x38, 0x01000002, 111}, {0x38, 0x01010102, 111}, {0x39, 0x01020202,  81}, {0x3a, 0x01000302,  34}, {0x3a, 0x01010302,  34},
    {0x3b, 0x01000302, 378}, {0x3b, 0x01010302, 378}, {0x3e, 0x01000302, 377}, {0x3e, 0x01010302, 377}, {0x3f, 0x01000302, 377},
    {0x3f, 0x01010302, 377}, {0x40, 0x01000302, 112}, {0x40, 0x01010302, 112}, {0x41, 0x05040302,  36}, {0x41, 0x01040302, 477},
    {0x41, 0x05020304, 478}, {0x41, 0x01020304, 479}, {0x42, 0x05040302, 359}, {0x42, 0x01040302, 480}, {0x42, 0x05020304, 481},
    {0x42, 0x01020304, 482}, {0x45, 0x05040302, 358}, {0x45, 0x01040302, 483}, {0x45, 0x05020304, 484}, {0x45, 0x01020304, 485},
    {0x46, 0x05040302, 360}, {0x46, 0x01040302, 486}, {0x46, 0x05020304, 487}, {0x46, 0x01020304, 488}, {0x47, 0x05040302, 113},
    {0x47, 0x01040302, 489}, {0x47, 0x05020304, 490}, {0x47, 0x01020304, 491}, {0x48, 0x01000002, 382}, {0x48, 0x01010102, 382},
    {0x49, 0x01000002, 383}, {0x49, 0x01010102, 383}, {0x4a, 0x01000002, 114}, {0x4a, 0x01010102, 114}, {0x4b, 0x01000302, 362},
    {0x4b, 0x01010302, 362}, {0x4c, 0x01000302, 363}, {0x4c, 0x01010302, 363}, {0x4d, 0x01000302, 115}, {0x4d, 0x01010302, 115},
    {0x4e, 0x01040302, 355}, {0x4f, 0x01040302, 356}, {0x50, 0x01040302, 354}, {0x51, 0x05040302, 351}, {0x51, 0x01040302, 492},
    {0x51, 0x05020304, 493}, {0x51, 0x01020304, 494}, {0x52, 0x05040302, 352}, {0x52, 0x01040302, 495}, {0x52, 0x05020304, 496},
    {0x52, 0x01020304, 497}, {0x53, 0x05040302, 116}, {0x53, 0x01040302, 498}, {0x53, 0x05020304, 499}, {0x53, 0x01020304, 500},
    {0x54, 0x01000302,  80}, {0x55, 0x01000302, 381}, {0x56, 0x01040302, 403}, {0x57, 0x05040302, 405}, {0x57, 0x01040302, 405},
    {0x58, 0x05040302, 406}, {0x58, 0x01040302, 406}, {0x59, 0x05040302, 408}, {0x5a, 0x05040302, 409}, {0x5b, 0x05040302, 411},
    {0x5c, 0x05040302, 412}, {0x5d, 0x01000002, 414}, {0x5d, 0x01020202, 414}, {0x5e, 0x01000002, 415}, {0x5e, 0x01020202, 415},
    {0x5f, 0x01000302, 417}, {0x5f, 0x01010203, 843666497}, {0x5f, 0x03020202, 417}, {0x60, 0x01000302, 418}, {0x60, 0x03020202, 418},
    {0x61, 0x01040302, 424}, {0x62, 0x01040302, 425}, {0x63, 0x05040302, 427}, {0x64, 0x05040302, 428}, {0x99, 0x01040302, 1195525970},
    {0x9b, 0x01040302, 1111970375}, {0x9d, 0x05040302, 1448433985}, {0x9e, 0x01040302, 1498831189}, {0x9f, 0x01040302, 1498831189}, {0xa0, 0x01040302, 844715353},
    {0xa1, 0x01040302, 844715353}, {0xa2, 0x01040302, 842094169}, {0xa3, 0x01040302, 825316942}, {0xa4, 0x01040302, 842094158}, {0xa5, 0x01040302, 825382478},
    {0xa6, 0x01040302, 909193296}, {0xa7, 0x01040302, 808530000}, {0xa8, 0x01040302, 808530512}, {0xb1, 0x01040302, 942682704}, {0xba, 0x01040302, 909193817},
    {0xbb, 0x01040302, 808530521}, {0xbc, 0x05040302, 909194329}, {0xbd, 0x05040302, 808531033},
};

uint32_t LookUp(const KmdFormatEntry* table, size_t count, uint32_t format, uint32_t swizzle)
{
    for (size_t index = 0; index < count; ++index)
    {
        if ((table[index].format == format) && (table[index].swizzle == swizzle)) { return table[index].kmdFormat; }
    }
    return 0;
}

// Native format table 5835750 equals this PAL's FormatInfoTable for every format this PAL declares.
uint32_t NumericSupport(uint32_t format)
{
    return (format < uint32_t(ChNumFormat::Count))
        ? uint32_t(Formats::FormatInfoTable[format].numericSupport) : 0;
}
constexpr uint32_t NumericDepthStencil = uint32_t(Formats::NumericSupportFlags::DepthStencil);
constexpr uint32_t NumericYuv          = uint32_t(Formats::NumericSupportFlags::Yuv);
static_assert((NumericDepthStencil == 9) && (NumericYuv == 10), "380e00 compares numericSupport with 9 and 10");

// Native 380960 for allocation 'index' of the object. 'bo' points at entry+8 and is already zeroed.
int32_t FillBo(const NativeAllocationDesc& desc, uint32_t index, uint8_t* bo)
{
    Put32(bo, 0, 128);
    Put32(bo, 88, 0);
    Put32(bo, 16, uint32_t(desc.physicalAlignment));
    uint64_t size    = desc.size;
    uint64_t address = desc.gpuVirtAddr;
    if (desc.allocationCount > 1)
    {
        const uint64_t offset = uint64_t(index) << ChunkShift;
        address += offset;
        size = (index == desc.allocationCount - 1) ? (desc.size - offset) : ChunkBytes;
    }
    Put32(bo, 12, uint32_t(size)); // The original stores the low DWORD; +96/+112 carry the 64-bit size.
    Put64(bo, 96, size);
    Put64(bo, 112, size);
    Put64(bo, 104, address);

    uint32_t flags = 0;
    if (desc.cpuVisible) { flags |= 1u << 29; }
    if (desc.pinned || desc.kmdShareUmdSysMem) { flags |= 1u << 26; }
    if (desc.stereo) { flags |= 1u << 23; }
    if (desc.shareable || desc.interprocess) { flags |= 1u << 31; }
    if (!desc.virtualAlloc) { flags |= 1u << 13; }
    if (desc.boFlag200) { flags |= 0x200; }

    uint16_t word94 = 0x100;
    if (desc.privateScreen)      { word94 |= 0x10; }
    if (desc.accessedPhysically) { word94 |= 0x1; }
    if (desc.globallyCoherent)   { word94 |= 0x8; }
    if (desc.xdmaBuffer)         { word94 |= 0x400; }
    if (desc.peerWritable)       { word94 |= 0x40; }
    if (desc.tmzProtected)       { word94 |= 0x20; }
    // 380960: an image created with pipSwapChain marks the BO like a private primary.
    if (desc.privPrimary || ((desc.image != nullptr) && ((desc.image->createFlags & ImagePipSwapChain) != 0)))
    { word94 |= 0x1000; }
    std::memcpy(bo + 94, &word94, sizeof(word94));

    // Native table 584ba58 indexed by GpuMemPriority; the fine offset fills the low nibble.
    constexpr uint32_t CreationPriorities[] = { 1, 1, 2, 0, 3, 4 };
    const uint32_t priority = (desc.priority < 6) ? CreationPriorities[desc.priority] : 0;
    Put32(bo, 20, (((desc.priorityOffset & 0xf) | ((priority & 0xf) << 4)) << 7) | 5);

    const bool skipLocal = desc.cpuVisible && desc.shareable;
    uint32_t ids[4] = {};
    uint32_t idCount = 0;
    for (uint32_t i = 0; i < desc.heapCount; ++i)
    {
        const uint32_t heap = desc.heaps[i];
        if ((heap > 3) || (desc.heapSizes[heap] == 0)) { continue; }
        uint32_t id = 0;
        switch (heap)
        {
        case 0:
            if (desc.busAddressable) { flags |= 0x4001; }
            else if (skipLocal) { continue; }
            else { flags |= 1; }
            id = 1;
            break;
        case 1: flags |= 2; id = 2; break;
        case 2: flags |= 4; id = 3; break;
        default: flags |= 8; id = 4; break;
        }
        ids[idCount++] = id;
    }
    Put32(bo, 8, flags);
    if (idCount == 0) { return 10; }
    // Cross-domain (local/GART) preference pairs duplicate the first ID on non-integrated GPUs.
    uint32_t out = 0;
    for (uint32_t in = 0; out < 4; ++out)
    {
        if (in >= idCount) { bo[24 + out] = 0; continue; }
        if ((in == 1) && !desc.integrated &&
            ((((ids[1] - 3) <= 1) && ((ids[0] - 1) <= 1)) || (((ids[0] - 3) <= 1) && ((ids[1] - 1) <= 1))))
        {
            bo[24 + out] = bo[24];
            ++out;
            if (out >= 4) { break; }
        }
        bo[24 + out] = uint8_t(ids[in++]);
    }
    return 0;
}

// Native 380e00: SURF records of an image, one per (mip, slice) of plane 0 inside the given loop bounds, at
// surfaces + (mip * arraySize + slice) * surfaceBytes. 'surfaces' points at entry+196.
int32_t WriteImageSurfaces(const NativeAllocationDesc& desc, const NativeImageDesc& image, uint8_t* surfaces,
                           uint32_t mips, uint32_t slices)
{
    // The original's mip counter is a byte and its slice counter a WORD.
    if ((image.getSubresource == nullptr) || (mips > 0xff) || (slices > 0xffff)) { return -2; }
    const uint32_t sb = desc.surfaceBytes;
    const uint32_t type = image.imageType + 1;
    const uint32_t perPlane = image.mipLevels * image.arraySize;
    const uint32_t numeric = NumericSupport(image.format);
    const bool viewFormats = (image.viewFormatCount != 0) && ((image.createFlags & ImageFlippable) == 0) &&
                             !image.turbosync && !desc.privPrimary;
    const bool depthFormat = ((image.usageFlags & UsageDepthStencil) != 0) || (numeric == NumericDepthStencil);
    auto get = [&image](uint32_t index, NativeSubresource* out)
    {
        *out = {};
        return image.getSubresource(image.context, index, out);
    };
    for (uint32_t slice = 0; slice < slices; ++slice)
    {
        NativeSubresource base; // (plane 0, mip 0, slice)
        if (!get(slice, &base)) { return -2; }
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            const uint32_t index = mip * image.arraySize + slice;
            NativeSubresource sub;
            if (!get(index, &sub)) { return -2; }
            uint8_t* surf = surfaces + size_t(index) * sb;
            const bool depthPlane = depthFormat && (image.format != FormatX8Uint) && (sub.plane == 0);
            uint32_t w4 = Get32(surf, 4);
            Put32(surf, 0, sb);
            Put32(surf, 0x0c, type);
            w4 = (w4 & ~1u) | (desc.flippable ? 1u : 0u);
            Put32(surf, 4, w4);
            Put32(surf, 0x10, KmdSurfaceFormat(image.format, image.swizzle, depthPlane, viewFormats));
            Put32(surf, 0x14, sub.extentWidth);
            Put32(surf, 0x18, sub.extentHeight);
            Put32(surf, 0x1c, uint32_t(sub.rowPitch));
            Put32(surf, 0x20, sub.actualHeight);
            Put32(surf, 0x24, sub.extentDepth);
            Put32(surf, 0x28, uint32_t(sub.depthPitch));
            Put32(surf, 0x2c, uint32_t(sub.offset));
            Put64(surf, 0x40, sub.offset);
            w4 = (w4 & ~0x800u)  | (desc.restrictedContent ? 0x800u  : 0u);
            w4 = (w4 & ~0x1000u) | (desc.restrictedAccess  ? 0x1000u : 0u);
            w4 = (w4 & ~0x4000u) | (desc.crossAdapter      ? 0x4000u : 0u);
            Put32(surf, 4, w4);
            if ((numeric == NumericYuv) && (image.numPlanes == 2))
            {
                NativeSubresource chroma;
                if (!get((mip + image.mipLevels) * image.arraySize + slice, &chroma)) { return -2; }
                Put32(surf, 0x58, uint32_t(chroma.offset));
            }
            if (desc.flippable)
            {
                Put32(surf, 0x34, image.refreshNumerator);
                Put32(surf, 0x38, image.refreshDenominator);
                Put64(surf, 0xc0, image.mallCursorCacheOffset);
            }
            if (desc.stereo && (index == 0) && (image.arraySize == 1))
            {
                Put64(surf, 0x48, sub.stereoOffset);
                Put32(surf, 0x18, sub.extentHeight - sub.stereoLineOffset);
            }
            Put32(surf, 4, (Get32(surf, 4) & ~0x10000u) | 0x20000u);
            const uint32_t bytesPerTexel = sub.bitsPerTexel >> 3;
            Put32(surf, 0x08, sub.swizzleMode);
            if (desc.gfxLevelWord >= 0xc000000)
            {
                return -2; // GFX12 SURF tail (AddrMgr3 tile info, slot 11 DCC control) is not built by this PAL.
            }
            if (desc.stereo && (index == 0) && (image.arraySize > 1))
            {
                NativeSubresource right; // tile info of (plane 0, mip 0, slice 1)
                if (!get(1, &right)) { return -2; }
                Put64(surf, 0x48, uint64_t(uint32_t(right.pipeBankXor << 8)) | sub.size);
            }
            Put32(surf, 0x78, sub.ePitch + 1);
            Put32(surf, 0x3c, sub.pipeBankXor);
            Put64(surf, 0x50, sub.backingStoreOffset);
            Put32(surf, 0x60, mip);
            Put32(surf, 0x64, image.mipLevels - 1);
            Put32(surf, 0x68, slice);
            Put32(surf, 0x6c, base.actualWidth);
            Put32(surf, 0x70, base.actualHeight);
            Put32(surf, 0x74, base.actualDepth);
            // Port: the original divides without checks (#DE for a sub-byte texel or a zero pitch); 0 is stored.
            Put32(surf, 0x80, (bytesPerTexel != 0) ? uint32_t(sub.rowPitch / bytesPerTexel) : 0);
            Put32(surf, 0x88, (sub.rowPitch != 0) ? uint32_t(sub.depthPitch / sub.rowPitch) : 0);
            Put64(surf, 0x90, (mip == 0) ? sub.size : 0);
            if (image.hasDisplayDcc)
            {
                const uint32_t f = image.displayDccFlags;
                Put32(surf, 0xb0, image.displayDccPitch);
                Put64(surf, 0xa0, image.displayDccPrimaryOffset);
                Put64(surf, 0xa8, image.displayDccSecondaryOffset);
                uint32_t v = (Get32(surf, 0xb4) & 0xffffffc3u) | ((f & 0xf) << 2) | 2u;
                v = (v & ~1u) | ((f >> 4) & 1);
                const uint32_t blocks = ((f & 0x20) != 0) ? (((f >> 4) & 1) + 2) : 1;
                Put32(surf, 0xb4, (v & 0xffffff3fu) | (blocks << 6));
            }
            if (image.numPlanes > 1)
            {
                NativeSubresource second;
                if (!get(index + perPlane, &second)) { return -2; }
                const uint64_t secondBytes = second.bitsPerTexel >> 3;
                const uint64_t slicePitch = (second.rowPitch != 0) ? (second.depthPitch / second.rowPitch) : 0;
                Put32(surf, 0x84, (secondBytes != 0) ? uint32_t(second.rowPitch / secondBytes) : 0);
                Put32(surf, 0x8c, uint32_t(slicePitch));
                Put64(surf, 0x98, (mip == 0) ? second.size : 0);
                Put32(surf, 0x7c, second.ePitch);
                Put32(surf, 0xb8, uint32_t(second.rowPitch));
                Put32(surf, 0xbc, uint32_t(slicePitch));
                for (uint32_t plane = 1; plane < image.numPlanes; ++plane)
                {
                    NativeSubresource other;
                    if (((0xc8 + 4 * plane) > sb) || !get(plane * perPlane + index, &other)) { return -2; }
                    Put32(surf, 0xc8 + 4 * (plane - 1), other.pipeBankXor);
                }
            }
            Put32(surf, 0x30, image.hasScreen ? image.vidPnSourceId : 0xffffffffu);
            if (image.privateScreen) { Put32(surf, 4, Get32(surf, 4) | 0x10); }
        }
    }
    return 0;
}

// Native 3814c0: SURF records of a typed buffer, one per depth slice when depthIsSubres. Writes are clipped to
// 'available' bytes (entry SURF space + metadata): with one reserved SURF (object+344 bit3) and depthIsSubres the
// original writes the second SURF over the metadata, which the metadata fields then partly overwrite, and further
// SURFs past the block into its request records and heap (port: dropped, see analysis).
void WriteTypedSurfaces(const NativeAllocationDesc& desc, const NativeTypedBufferDesc& typed, uint8_t* surfaces,
                        size_t available)
{
    const uint32_t sb = desc.surfaceBytes;
    const uint32_t count = typed.depthIsSubres ? typed.depth : 1;
    const uint32_t depth = typed.depthIsSubres ? 1 : typed.depth;
    std::vector<uint8_t> surface(sb, 0);
    uint32_t offset = 0;
    size_t at = 0;
    for (uint32_t index = 0; (index < count) && (at < available); ++index)
    {
        const size_t bytes = ((available - at) < sb) ? (available - at) : sb;
        std::memset(surface.data(), 0, sb);
        std::memcpy(surface.data(), surfaces + at, bytes);
        uint8_t* surf = surface.data();
        Put32(surf, 0, sb);
        Put32(surf, 0x0c, typed.depthIsSubres ? 2 : 3);
        Put32(surf, 0x10, LookUp(PlainFormatTable, sizeof(PlainFormatTable) / sizeof(PlainFormatTable[0]),
                                 typed.format, typed.swizzle));
        Put32(surf, 0x14, typed.width);
        Put32(surf, 0x18, typed.height);
        Put32(surf, 0x1c, typed.rowPitch);
        // Port: the original divides by the row pitch without a check (#DE for 0); 0 is stored.
        Put32(surf, 0x20, (typed.rowPitch != 0) ? (typed.depthPitch / typed.rowPitch) : 0);
        Put32(surf, 0x24, depth);
        Put32(surf, 0x28, typed.depthPitch);
        Put64(surf, 0x40, offset);
        Put32(surf, 0x2c, offset);
        Put32(surf, 4, (Get32(surf, 4) & 0xfffebfffu) | 0x20000u | (desc.crossAdapter ? 0x4000u : 0u));
        Put32(surf, 0x08, ((desc.gfxLevelWord < 0xc000000) && desc.crossAdapter) ? 0x20 : 0);
        Put64(surf, 0x60, 0);
        Put32(surf, 0x68, 0);
        Put32(surf, 0x6c, typed.width);
        Put32(surf, 0x70, typed.height);
        Put32(surf, 0x74, depth);
        Put32(surf, 0x78, typed.rowPitch);
        Put32(surf, 0x3c, 0);
        Put64(surf, 0xc8, 0);
        Put64(surf, 0x90, typed.depthPitch);
        std::memcpy(surfaces + at, surf, bytes);
        offset += typed.depthPitch;
        at += sb;
    }
}
}

uint32_t KmdSurfaceFormat(uint32_t format, uint32_t swizzle, bool depthPlane, bool viewFormats)
{
    if (depthPlane)
    {
        switch (format)
        {
        case 0x4a: return viewFormats ? 0x17c : 0x17d; // X32_Float
        case 0x32: return viewFormats ? 0x188 : 0x50;  // X16_Unorm
        case 0x55: return 0x16d;                       // D32_Float_S8_Uint
        case 0x54: return 0x50;                        // D16_Unorm_S8_Uint
        default:   return 0;
        }
    }
    return viewFormats
        ? LookUp(ViewFormatTable, sizeof(ViewFormatTable) / sizeof(ViewFormatTable[0]), format, swizzle)
        : LookUp(PlainFormatTable, sizeof(PlainFormatTable) / sizeof(PlainFormatTable[0]), format, swizzle);
}

int32_t BuildNativeAllocationRequest(const NativeAllocationDesc& desc, NativeAllocationRequest* request)
{
    if ((request == nullptr) || (desc.allocationCount == 0) || (desc.heapCount > 4)) { return -2; }
    const NativeImageDesc* image = desc.image;
    const NativeTypedBufferDesc* typed = (image == nullptr) ? desc.typedBuffer : nullptr;
    const bool untypedSurface = (image == nullptr) && (typed == nullptr) && desc.client && desc.interprocess;
    // 37ef20: SURF bytes reserved in each entry.
    uint64_t surfaceBytes = 0;
    uint32_t imageMips = 1, imageSlices = 1;
    if (image != nullptr)
    {
        if (!desc.singleSurface) { imageMips = image->mipLevels; imageSlices = image->arraySize; }
        surfaceBytes = uint64_t(desc.surfaceBytes) * imageMips * imageSlices;
    }
    else if (typed != nullptr)
    {
        surfaceBytes = uint64_t(desc.surfaceBytes) *
                       ((!desc.singleSurface && typed->depthIsSubres) ? typed->depth : 1);
    }
    else if (untypedSurface)
    {
        surfaceBytes = desc.surfaceBytes;
    }
    // Port bound: the original computes the request buffer size in 32 bits.
    const uint64_t perAllocation = EntryPrefixBytes + surfaceBytes + NativeMetadataBytes + AllocationHeaderBytes + 40;
    if ((perAllocation * desc.allocationCount + 136) > UINT32_MAX) { return -2; }
    const size_t entryBytes = size_t(EntryPrefixBytes + surfaceBytes);
    const size_t entryAndMetadata = entryBytes + NativeMetadataBytes;
    const size_t blockBytes = AllocationHeaderBytes + entryAndMetadata;
    if (untypedSurface && (desc.surfaceBytes < 124)) { return -2; }
    if (((image != nullptr) || (typed != nullptr)) && (desc.surfaceBytes < 0xd8)) { return -2; }

    request->count = desc.allocationCount;
    request->blockBytes = uint32_t(blockBytes);
    request->blocks.assign(blockBytes * desc.allocationCount, 0);
    request->hostOffsets.assign(desc.allocationCount, 0);
    request->resourceHeader = {};
    request->vidPnSourceId = UINT32_MAX;
    request->infoFlags = 0;
    for (uint32_t index = 0; index < desc.allocationCount; ++index)
    {
        uint8_t* header = request->blocks.data() + size_t(index) * blockBytes;
        uint8_t* entry  = header + AllocationHeaderBytes;
        // 3802c0
        Put32(entry, 0, uint32_t(entryAndMetadata));
        Put32(entry, 176, uint32_t(entryBytes));
        Put32(entry, 4, 1);
        Put32(entry, 188, desc.abiVersion);
        const int32_t bo = FillBo(desc, index, entry + BoOffset);
        if (bo != 0) { return bo; }
        if (image != nullptr)
        {
            Put32(entry, 0x88, imageMips * imageSlices);
            Put32(entry, 0x9c, image->arraySize & 0xffff);
            Put32(entry, 0x98, image->mipLevels & 0xffff);
            const uint16_t samples = uint16_t(image->samples);
            const uint16_t allPlanes = 0xffff;
            std::memcpy(entry + 0x94, &samples, sizeof(samples));
            std::memcpy(entry + 0x96, &allPlanes, sizeof(allPlanes));
            const uint32_t create = image->createFlags;
            const uint32_t usage = image->usageFlags;
            uint32_t flags = Get32(entry, 0x90) & ~(0x20u | 0x400u | 0x10u | 0x2000u | 0x1u | 0x8u | 0x800000u | 0x1000000u);
            flags |= (((create & ImageCubemap) != 0)          ? 0x20u      : 0u) |
                     (((create & ImagePresentable) != 0)      ? 0x400u     : 0u) |
                     (((usage & UsageShaderRead) != 0)        ? 0x10u      : 0u) |
                     (((usage & UsageShaderWrite) != 0)       ? 0x2000u    : 0u) |
                     (((usage & UsageColorTarget) != 0)       ? 0x1u       : 0u) |
                     (((usage & UsageDepthStencil) != 0)      ? 0x8u       : 0u) | 0x100000u |
                     (((create & ImageOptimalShareable) != 0) ? 0x800000u  : 0u) |
                     (((usage & UsageDepthAsZ24) != 0)        ? 0x1000000u : 0u);
            Put32(entry, 0x90, flags);
            const int32_t written = WriteImageSurfaces(desc, *image, entry + EntryPrefixBytes, imageMips, imageSlices);
            if (written != 0) { return written; }
        }
        else if (typed != nullptr)
        {
            Put32(entry, 0x88, (desc.singleSurface || !typed->depthIsSubres) ? 1 : typed->depth);
            Put32(entry, 0x9c, typed->depthIsSubres ? typed->depth : 1);
            Put32(entry, 0x98, 1);
            Put32(entry, 0x94, 0xffff0001u);
            WriteTypedSurfaces(desc, *typed, entry + EntryPrefixBytes, entryAndMetadata - EntryPrefixBytes);
        }
        else if (untypedSurface)
        {
            // Untyped client+interprocess SURF.
            Put32(entry, 136, 1); Put32(entry, 156, 1); Put32(entry, 152, 1); Put32(entry, 148, 1);
            Put32(entry, 196, desc.surfaceBytes);
            Put64(entry, 208, 1);
            Put32(entry, 216, uint32_t(desc.clientSize));
            Put32(entry, 220, 1);
            Put32(entry, 228, 1);
            Put32(entry, 224, uint32_t(desc.size));
            Put32(entry, 232, 1);
            Put32(entry, 200, (Get32(entry, 200) & ~0x10000u) | 0x20000u);
            Put32(entry, 236, uint32_t(desc.size));
            Put32(entry, 204, (desc.gfxLevelWord < 0xc000000) ? 0x20 : 0);
            Put64(entry, 292, 0);
            Put32(entry, 300, 0);
            Put32(entry, 304, uint32_t(desc.clientSize));
            Put32(entry, 308, 1);
            Put32(entry, 312, 1);
            Put32(entry, 316, uint32_t(desc.size));
            Put32(entry, 256, 0);
            Put64(entry, 396, 0);
        }
        if ((image != nullptr) || (typed != nullptr) || untypedSurface) { Put32(entry, 4, Get32(entry, 4) | 0xc); }
        uint8_t* metadata = entry + entryBytes;
        Put32(metadata, 0, uint32_t(NativeMetadataBytes));
        Put64(metadata, 8, Get64(entry, BoOffset + 104));
        Put64(metadata, 16, Get64(entry, BoOffset + 96));
        Put32(metadata, 32, 0); // Device+0xd9cc, zeroed by the Device constructor and never written.
        Put32(metadata, 212, (desc.mallPolicy & 3) | (desc.mallRangeActive ? 4 : 0));
        Put32(metadata, 216, desc.mallStartPage);
        Put32(metadata, 220, desc.mallNumPages);
        constexpr uint32_t Mtypes[] = { 0, 1, 2, 4 };
        Put32(metadata, 204, Mtypes[desc.mtype & 3]);
        Put64(metadata, 180, desc.uniqueId);
        if ((image != nullptr) && ((image->createFlags & ImageOptimalShareable) != 0))
        {
            // Shared metadata 1.5: the image's cross-process ID and its GetSharedMetadataInfo offsets.
            Put32(metadata, 0x94, 0x10005);
            Put64(metadata, 0xb4, image->sharedResourceId);
            const uint32_t major = desc.gfxLevelWord >> 24;
            if (major == 12) { return -2; } // GFX12 hiZ/hiS fields are not produced by this PAL.
            if (major <= 11)
            {
                const uint32_t f = image->sharedFlags;
                Put64(metadata, 0x74, image->sharedDccOffset);
                Put64(metadata, 0x8c, image->sharedHtileOffset);
                Put64(metadata, 0x84, image->sharedFmaskOffset);
                const uint32_t flags = (Get32(metadata, 0x98) & 0xffffffb6u) | (f & 1) | ((f & 4) << 1) | ((f & 8) << 3);
                Put64(metadata, 0x9c, image->sharedFastClearOffset);
                Put64(metadata, 0xbc, image->sharedHisPretestOffset);
                Put64(metadata, 0xa4, image->sharedDccStateOffset);
                Put32(metadata, 0x98, flags);
                if (image->sharedFmaskOffset != 0)
                {
                    Put64(metadata, 0x8c, image->sharedFmaskXor);
                    Put32(metadata, 0xc4, image->sharedFmaskSwizzle);
                    Put32(metadata, 0x98, flags | 0x20);
                }
            }
        }
        uint32_t allocationFlags = Get32(metadata, 208) & ~(0x8u | 0x40u | 0x80u | 0x400u | 0x800u | 0x2000u);
        allocationFlags |= (desc.kmdShareUmdSysMem ? 0x8u : 0u) | (desc.deferCpuVaReservation ? 0x40u : 0u) |
                           (desc.dfSpmTraceBuffer ? 0x400u : 0u) | (desc.cpuInvisible ? 0x2000u : 0u);
        Put32(metadata, 208, allocationFlags);
        // 37ef20 allocation header (rewritten completely, also over SURF bytes a typed buffer spilled into it).
        std::memset(header, 0, AllocationHeaderBytes);
        Put32(header, 16, desc.adapterIndex);
        Put32(header, 4 * size_t(desc.adapterIndex), uint32_t(entryAndMetadata));
        Put32(header, 60, Get32(header, 60) | 1);
        Put32(header, 20, 0);
        Put32(header, 20, Checksum(header, AllocationHeaderBytes));
        if (desc.pinned) { request->hostOffsets[index] = uint64_t(index) << ChunkShift; }
    }
    if ((image != nullptr) && !image->turbosync && !desc.privPrimary)
    {
        // 37ef20 INFO: the fullscreen screen's VidPn source, Primary for flippable memory, Stereo for DXGI stereo.
        request->vidPnSourceId = image->hasScreen ? image->vidPnSourceId : UINT32_MAX;
        request->infoFlags = (desc.flippable ? 1u : 0u) | (((image->createFlags & ImageDxgiStereo) != 0) ? 2u : 0u);
    }
    uint8_t* resource = request->resourceHeader.data();
    Put32(resource, 8, 0x80);
    Put32(resource, 0, desc.adapterIndex);
    Put32(resource, 60, uint32_t(blockBytes));
    Put32(resource, 12, Checksum(resource, AllocationHeaderBytes));
    request->createFlags = desc.interprocess ? (3u | (desc.sharedViaNtHandle ? 0x40u : 0u)) : 0u;
    return 0;
}
}
