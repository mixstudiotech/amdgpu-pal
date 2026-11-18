// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
#include "pal.h"
#include <cstddef>

namespace Pal::Wddm
{
class Transport;
Result QueryDriverSoftwareVersion(const Transport& transport, char* buffer, size_t length);
Result QueryDriverReleaseVersion(const Transport& transport, char* buffer, size_t length);
}
