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
