/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
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

    int getNumClustersInFace(float area) const;
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
    uint64_t _grassGeneration {0};
    uint64_t _faceGeneration {0};
    bool _hasLightmapUV {true};
    bool _faceRecordsBuilt {false};
    bool _gpuSceneDirty {true};
    bool _wasGrassEnabled {false};

    void rebuildFaceRecords();
};

} // namespace scene

} // namespace reone
