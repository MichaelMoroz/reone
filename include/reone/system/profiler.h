/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifdef R_ENABLE_TRACY

#include <tracy/Tracy.hpp>

#define R_PROFILE_ZONE(name) ZoneScopedN(name)
#define R_PROFILE_FRAME_MARK() FrameMark
#define R_PROFILE_THREAD_NAME(name) tracy::SetThreadName(name)

#else

#define R_PROFILE_ZONE(name)
#define R_PROFILE_FRAME_MARK()
#define R_PROFILE_THREAD_NAME(name)

#endif
