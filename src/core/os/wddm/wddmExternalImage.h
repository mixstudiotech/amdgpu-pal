// Copyright (c) 2026. SPDX-License-Identifier: MIT
// External shared image import (Device slots 39, 63, 64 of the original Windows PAL). Analysis and RVAs:
// .build/pal-wddm-logic/external-image/analysis.md.
#pragma once
#include "pal.h"
#include "palDevice.h"
#include "palImage.h"
#include "core/os/wddm/wddmSharedResource.h"
#include <vector>

namespace Pal { struct ImageInternalCreateInfo; }
namespace Pal::Wddm
{
class Device;

// The original's 80-byte query record (native 3120c0 fills it, 3124f0 releases it), by meaning.
struct ExternalResourceQuery
{
    OpenedSharedResource opened;     // KMT objects (+4, +40, +44, +48) and temporary private data (+64).
    uint32  allocation = 0;          // +0  selected allocation handle.
    uint32  flags = 0;               // +8  bit0: Dopp desktop info copied; bit1: creator VA from metadata.
    uint64  address = 0;             // +16 Dopp desktop VA or creator VA (metadata+8).
    uint32  addressInfo = 0;         // +24 Dopp vidPnSourceId or metadata+16.
    uint64  addressAux = 0;          // +32 entry DWORD 8 (creator-VA case only).
    const uint8* pEntry = nullptr;   // +56 selected AMD allocation entry.
    uint32  captureAllocation = 0;   // DirectCapture allocation created by 317dd0 (owned until adopted).
    size_t  entryBytes = 0;          // Readable bytes at pEntry (bound added by this port).
};

// Native 2cd040: KMD SURF format (D3DDDIFORMAT, DXGI-derived values 0x15e..0x1f4 and FourCCs) to PAL.
// *pAllViewFormats and *pDepthStencil receive the original's two output bytes.
SwizzledFormat TranslateKmdSurfaceFormat(uint32 kmdFormat, bool* pAllViewFormats, bool* pDepthStencil);

// Native 3120c0 entry selection. Picks the allocation whose private data belongs to this physical adapter
// and returns its AMD allocation entry (after the 64-byte header unless the entry's first DWORD equals the
// reported private size). False means the original's NotShareable outcome.
bool SelectExternalAllocationEntry(const OpenedSharedResource& opened, uint32 physicalAdapter,
                                   uint32* pAllocation, const uint8** ppEntry, size_t* pEntryBytes);

// Native 3833c0 (pure): ImageCreateInfo from the caller's open info and the selected entry. 'entryBytes'
// must cover ExternalImageEntryMinBytes. gfx12OrLater selects the GFX12 linear-swizzle rule.
constexpr size_t ExternalImageEntryMinBytes = 0x198;
void FillExternalImageCreateInfo(const ExternalImageOpenInfo& openInfo, const uint8* pEntry, bool gfx12OrLater,
                                 ImageCreateInfo* pCreateInfo);

// Native 3120c0 tail: a Dopp open copies DoppDesktopInfo into the query (flags bit0, +16 VA, +24 VidPn source).
void ApplyDoppDesktop(const ExternalResourceOpenInfo& openInfo, ExternalResourceQuery* pQuery);
// Native 313120 after the query: an entry whose metadata+0xd0 has bit1 carries the creator's VA (flags bit1, +16 =
// metadata+8, +24 = metadata+16, +32 = entry DWORD 8). False when the metadata offset lies outside the entry.
bool ApplyCreatorAddress(ExternalResourceQuery* pQuery);

// Native 317dd0 (pure): the DirectCapture allocation that a DirectCapture image open creates beside the opened
// resource. Its private data is a copy of the selected entry (entry[0] bytes) with the capture usage, the ABI version
// and the query's address fields patched in at the original's fixed offsets; the 64-byte resource header follows the
// native layout. False when the entry is shorter than its first DWORD claims (port bound).
bool BuildDirectCaptureAllocation(const uint8* pEntry, size_t entryBytes, uint32 usageFlags, uint64 address,
                                  uint32 addressInfo, uint32 abiVersion, uint32 adapterIndex,
                                  std::vector<uint8>* pAllocationData, uint8 (&resourceHeader)[64]);

// Device slot 39 (0x312bd0), slot 63 (0x312f60), slot 64 (0x313120).
Result DetermineExternalResourceType(Device* pDevice, const ExternalResourceOpenInfo& openInfo, bool* pIsImage);
Result GetExternalImageSizes(Device* pDevice, const ExternalImageOpenInfo& openInfo, size_t* pImageSize,
                             size_t* pGpuMemorySize, ImageCreateInfo* pImgCreateInfo);
Result OpenExternalImage(Device* pDevice, const ExternalImageOpenInfo& openInfo, void* pImagePlacementAddr,
                         void* pGpuMemoryPlacementAddr, GpuMemoryCreateInfo* pMemCreateInfo, IImage** ppImage,
                         IGpuMemory** ppGpuMemory);
}
