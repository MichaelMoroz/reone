/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "reone/game/object/camera/free.h"

#include "reone/game/game.h"

namespace reone::game {

bool FreeCamera::handle(const input::Event &event) {
    switch (event.type) {
    case input::EventType::MouseButtonDown:
        if (event.button.button == input::MouseButton::Right) {
            _looking = true;
            _game.setRelativeMouseMode(true);
            return true;
        }
        return false;
    case input::EventType::MouseButtonUp:
        if (event.button.button == input::MouseButton::Right) {
            endLook();
            return true;
        }
        return false;
    case input::EventType::MouseMotion:
        // Motion outside a drag belongs to whatever the cursor is over.
        if (!_looking) {
            return false;
        }
        break;
    default:
        break;
    }
    return FirstPersonCamera::handle(event);
}

void FreeCamera::endLook() {
    if (!_looking) {
        return;
    }
    _looking = false;
    _game.setRelativeMouseMode(false);
}

} // namespace reone::game
