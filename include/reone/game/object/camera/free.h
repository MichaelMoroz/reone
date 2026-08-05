/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
