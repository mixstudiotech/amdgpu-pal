// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "palQueue.h"

namespace Pal::Wddm
{
class Queue;
namespace Sparse
{
Result Remap(Queue& queue, uint32 count, const VirtualMemoryRemapRange* ranges, bool doNotWait);
Result Copy(Queue& queue, uint32 count, const VirtualMemoryCopyPageMappingsRange* ranges, bool doNotWait);
}
}
