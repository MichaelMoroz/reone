/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "firstperson.h"

namespace reone::game {

/**
 * An unbound first-person camera used for scene inspection and capture.
 *
 * Looking is bound to holding the right mouse button rather than to the camera
 * being active. First person can hold the cursor for as long as it is active
 * because it is a play mode you leave deliberately; this one is toggled from
 * the debug menu, so it has to leave the cursor alone - otherwise the menu that
 * turned it on cannot be clicked to turn it off.
 */
class FreeCamera : public FirstPersonCamera {
public:
    using FirstPersonCamera::FirstPersonCamera;

    bool handle(const input::Event &event) override;

    /** Releases the cursor when the camera is switched away from mid-drag. */
    void endLook();

private:
    bool _looking {false};
};

} // namespace reone::game
