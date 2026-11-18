// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"

namespace Pal::Wddm
{
class Transport;
Result ApplyQueuedPresentLimit(const Transport& transport, uint32 frames);
}
