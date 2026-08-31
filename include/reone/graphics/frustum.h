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

#include "aabb.h"

namespace reone {

namespace graphics {

/** A set of clipping planes extracted from a projection-view matrix. */
class Frustum {
public:
    Frustum() = default;
    explicit Frustum(const glm::mat4 &viewProjection);

    bool isInFrustum(const glm::vec3 &point) const;
    bool isInFrustum(const AABB &aabb) const;

private:
    struct Plane {
        glm::vec3 normal {0.0f};
        float distance {0.0f};

        float distanceTo(const glm::vec3 &point) const {
            return glm::dot(normal, point) + distance;
        }
    };

    std::array<Plane, 6> _planes;
};

} // namespace graphics

} // namespace reone
