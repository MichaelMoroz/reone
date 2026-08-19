/*
 * Copyright (c) 2020-2026 The reone project contributors
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

#include "reone/graphics/rendering/gpuscene.h"
#include "reone/graphics/modelnode.h"

#include "../grassproperties.h"
#include "../node.h"

namespace reone {

namespace scene {

class GrassSceneNode : public SceneNode {
public:
    GrassSceneNode(
        GrassProperties properties,
        graphics::ModelNode &aabbNode,
        ISceneGraph &sceneGraph,
        graphics::GraphicsServices &graphicsSvc,
        audio::AudioServices &audioSvc,
        resource::ResourceServices &resourceSvc) :
        SceneNode(
            SceneNodeType::Grass,
            sceneGraph,
            graphicsSvc,
            audioSvc,
            resourceSvc),
        _properties(std::move(properties)),
        _aabbNode(aabbNode) {
    }

    void init();
    void update(float dt) override;
    void collectInto(GpuScene &scene);
    void collectIntoIfDirty(GpuScene &scene) {
        if (_gpuSceneDirty)
            collectInto(scene);
    }

    /** Exact, unrounded: the caller carries the remainder between pieces of one face. */
    float getNumClustersInFace(float area) const;
    const std::vector<graphics::GrassFace> &faceRecords() const {
        return _faceRecords;
    }

protected:
    void onAbsoluteTransformChanged() override;

private:
    GrassProperties _properties;
    graphics::ModelNode &_aabbNode;
    std::vector<int> _grassFaces;
    std::vector<graphics::GrassFace> _faceRecords;
    /**
     * A piece of an admitted walkmesh face, after it has been divided finely
     * enough to follow the ground that is actually drawn.
     *
     * A walkmesh face is coarse - on Dantooine's estate its median plan area
     * is 34 world units and a planter bed is four triangles - so lifting only
     * its corners onto the drawn surface leaves a mound's interior behind.
     * Dividing until the surface is within tolerance of the corners' own plane
     * refines only where the two disagree, which is why flat ground keeps its
     * original face count.
     */
    struct SupportFace {
        int face {0};
        float areaFraction {1.0f};
        std::array<glm::vec3, 3> barycentric {};
    };
    std::vector<SupportFace> _supportFaces;
    /**
     * World positions of the support faces' vertices, lifted onto the drawn
     * ground. Three per support face, in the order that face lists them.
     *
     * Kept because it survives every dial: what the ground is doing does not
     * depend on blade length or density, so it is recomputed only when the
     * room moves, not when the settings change.
     */
    std::vector<glm::vec3> _supportVertices;
    uint64_t _grassGeneration {0};
    uint64_t _faceGeneration {0};
    bool _hasLightmapUV {true};
    bool _faceRecordsBuilt {false};
    bool _supportVerticesBuilt {false};
    bool _gpuSceneDirty {true};
    bool _wasGrassEnabled {false};

    void rebuildFaceRecords();
    void rebuildSupportVertices();
};

} // namespace scene

} // namespace reone
