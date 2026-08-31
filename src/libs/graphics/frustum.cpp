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

#include "reone/graphics/frustum.h"

namespace reone {

namespace graphics {

Frustum::Frustum(const glm::mat4 &viewProjection) {
    for (int i = 0; i < 3; ++i) {
        _planes[0].normal[i] = viewProjection[i][3] + viewProjection[i][0];
        _planes[1].normal[i] = viewProjection[i][3] - viewProjection[i][0];
        _planes[2].normal[i] = viewProjection[i][3] + viewProjection[i][1];
        _planes[3].normal[i] = viewProjection[i][3] - viewProjection[i][1];
        _planes[4].normal[i] = viewProjection[i][2];
        _planes[5].normal[i] = viewProjection[i][3] - viewProjection[i][2];
    }
    _planes[0].distance = viewProjection[3][3] + viewProjection[3][0];
    _planes[1].distance = viewProjection[3][3] - viewProjection[3][0];
    _planes[2].distance = viewProjection[3][3] + viewProjection[3][1];
    _planes[3].distance = viewProjection[3][3] - viewProjection[3][1];
    _planes[4].distance = viewProjection[3][2];
    _planes[5].distance = viewProjection[3][3] - viewProjection[3][2];
    for (auto &plane : _planes) {
        float length = glm::length(plane.normal);
        plane.normal /= length;
        plane.distance /= length;
    }
}

bool Frustum::isInFrustum(const glm::vec3 &point) const {
    for (const auto &plane : _planes) {
        if (plane.distanceTo(point) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::isInFrustum(const AABB &aabb) const {
    for (const auto &plane : _planes) {
        auto codir = aabb.max();
        if (plane.normal.x < 0.0) {
            codir.x = aabb.min().x;
        }
        if (plane.normal.y < 0.0) {
            codir.y = aabb.min().y;
        }
        if (plane.normal.z < 0.0) {
            codir.z = aabb.min().z;
        }
        if (plane.distanceTo(codir) < 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace graphics

} // namespace reone
