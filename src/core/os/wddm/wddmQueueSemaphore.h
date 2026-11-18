// Copyright (c) 2026. SPDX-License-Identifier: MIT
#pragma once
// Windows shares PAL's master/opened semaphore model; this backend implements the
// OS operations in the existing base instead of duplicating its batching logic.
#include "core/queueSemaphore.h"
#include "core/masterQueueSemaphore.h"
#include "core/openedQueueSemaphore.h"
