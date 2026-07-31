/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "firstperson.h"

namespace reone::game {

/** An unbound first-person camera used for scene inspection and capture. */
class FreeCamera : public FirstPersonCamera {
public:
    using FirstPersonCamera::FirstPersonCamera;
};

} // namespace reone::game
