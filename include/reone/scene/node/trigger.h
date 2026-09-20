/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "reone/graphics/mesh.h"

#include "../node.h"

namespace reone {

namespace graphics {

struct GraphicsServices;

}

namespace scene {

class TriggerSceneNode : public SceneNode {
public:
    TriggerSceneNode(
        std::vector<glm::vec3> geometry,
        ISceneGraph &sceneGraph,
        graphics::GraphicsServices &graphicsSvc,
        audio::AudioServices &audioSvc,
        resource::ResourceServices &resourceSvc) :
        SceneNode(
            SceneNodeType::Trigger,
            sceneGraph,
            graphicsSvc,
            audioSvc,
            resourceSvc),
        _geometry(std::move(geometry)) {
        initGeometry();
    }

    bool isIn(const glm::vec2 &pt) const;

    /** The volume's geometry, for the trigger debug view. */
    const graphics::Mesh *mesh() const { return _mesh.get(); }

    /** The colour this volume draws in, which tracks the trigger's state. */
    void setDebugColor(glm::vec4 color) { _debugColor = color; }
    const glm::vec4 &debugColor() const { return _debugColor; }

private:
    std::vector<glm::vec3> _geometry;
    std::unique_ptr<graphics::Mesh> _mesh;
    glm::vec4 _debugColor {1.0f};

    void initGeometry();
};

} // namespace scene

} // namespace reone
