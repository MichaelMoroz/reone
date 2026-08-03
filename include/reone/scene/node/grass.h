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

#include "reone/graphics/modelnode.h"
#include "reone/graphics/types.h"

#include "../grassproperties.h"
#include "../node.h"

#include "grasscluster.h"

namespace reone {

namespace graphics {

struct GraphicsServices;

}

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

    void collectLeafs(GpuScene &scene, const std::vector<SceneNode *> &leafs) override;
    void collectLeafsIfDirty(GpuScene &scene, const std::vector<SceneNode *> &leafs);

    int getNumClustersInFace(float area) const;
    void growClusterPool(int target);
    int getGrassVariant(int faceIndex, int clusterIndex) const;

private:
    struct ClusterPlacement {
        int faceIndex {0};
        int clusterIndex {0};
        glm::vec3 position {0.0f};
        glm::vec2 lightmapUV {0.0f};
        int variant {0};
        float yaw {0.0f};
        uint8_t sizeLevel {0};
        GrassClusterSceneNode *node {nullptr};
        bool queued {false};
    };

    struct FaceClusters {
        int faceIndex {0};
        std::vector<ClusterPlacement> clusters;
        glm::vec3 lastCameraPosition {0.0f};
        float nextUpdateDistance2 {0.0f};
        bool initialized {false};
    };

    GrassProperties _properties;
    graphics::ModelNode &_aabbNode;

    std::vector<int> _grassFaces;
    uint64_t _grassGeneration {0};
    int _poolCapacity {0};
    std::stack<GrassClusterSceneNode *> _clusterPool; /**< pre-allocated pool of clusters */
    std::vector<FaceClusters> _clusterFaces;
    std::vector<ClusterPlacement *> _pendingClusters;
    bool _clusterPlacementsBuilt {false};
    bool _hasLightmapUV {true};
    bool _gpuSceneDirty {true};

    void rebuildClusterPlacements();
    void returnAllClusters();
    void updateFaceClusters(FaceClusters &face, const glm::vec3 &cameraPosition,
                            std::unordered_set<SceneNode *> &returning);
    void admitPendingClusters(const glm::vec3 &cameraPosition);
    void collectLeafsImpl(GpuScene &scene, const std::vector<SceneNode *> &leafs,
                          bool force);
};

} // namespace scene

} // namespace reone
