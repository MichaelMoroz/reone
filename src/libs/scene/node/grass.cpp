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

#include "reone/scene/node/grass.h"

#include "reone/graphics/di/services.h"
#include "reone/graphics/material.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/texture.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/textures.h"
#include "reone/scene/gpuscene.h"
#include "reone/scene/graph.h"
#include "reone/system/profiler.h"

using namespace reone::graphics;

namespace reone::scene {
namespace {

constexpr float kGrassDensityFactor = 0.5f;
constexpr uint32_t kGrassVariantCount = 4;

glm::vec3 transformPoint(const glm::mat4 &transform, const glm::vec3 &point) {
    return glm::vec3(transform * glm::vec4(point, 1.0f));
}

} // namespace

void GrassSceneNode::init() {
    setNameIds({0, _sceneGraph.internName(_aabbNode.name())});
    const auto mesh = _aabbNode.mesh()->mesh;
    const auto &faces = mesh->faces();
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto &face = faces[faceIndex];
        if (_properties.materials.count(face.material) == 0)
            continue;
        _grassFaces.push_back(static_cast<int>(faceIndex));
        if (!mesh->tryFaceUV2(face, glm::vec3(1.0f, 0.0f, 0.0f)))
            _hasLightmapUV = false;
    }
}

void GrassSceneNode::onAbsoluteTransformChanged() {
    _faceRecordsBuilt = false;
    _gpuSceneDirty = true;
}

void GrassSceneNode::rebuildFaceRecords() {
    R_PROFILE_ZONE("GrassSceneNode::face record refresh");
    _faceRecords.clear();
    _faceRecords.reserve(_grassFaces.size());

    const auto mesh = _aabbNode.mesh()->mesh;
    const auto &faces = mesh->faces();
    for (const auto faceIndex : _grassFaces) {
        const auto &face = faces[faceIndex];
        const int clusterBudget = getNumClustersInFace(face.area);
        if (clusterBudget <= 0)
            continue;

        const auto local = mesh->faceVertexCoords(face);
        const glm::vec3 v0 = transformPoint(_absTransform, local[0]);
        const glm::vec3 v1 = transformPoint(_absTransform, local[1]);
        const glm::vec3 v2 = transformPoint(_absTransform, local[2]);
        glm::vec2 uv0 {0.0f}, uv1 {0.0f}, uv2 {0.0f};
        if (_hasLightmapUV) {
            uv0 = *mesh->tryFaceUV2(face, glm::vec3(1.0f, 0.0f, 0.0f));
            uv1 = *mesh->tryFaceUV2(face, glm::vec3(0.0f, 1.0f, 0.0f));
            uv2 = *mesh->tryFaceUV2(face, glm::vec3(0.0f, 0.0f, 1.0f));
        }

        GrassFace record;
        record.vertex0Uv0x = glm::vec4(v0, uv0.x);
        record.vertex1Uv0y = glm::vec4(v1, uv0.y);
        record.vertex2Uv1x = glm::vec4(v2, uv1.x);
        record.uv1yUv2QuadSize = glm::vec4(uv1.y, uv2, _properties.quadSize);
        record.probabilities = _properties.probabilities;
        record.boundsMin = glm::vec4(glm::min(v0, glm::min(v1, v2)), 0.0f);
        record.boundsMax = glm::vec4(glm::max(v0, glm::max(v1, v2)), 0.0f);
        // materialIndex is filled after material interning by GpuScene::prepare.
        record.faceBudgetMaterialVariants = {
            static_cast<uint32_t>(faceIndex),
            static_cast<uint32_t>(clusterBudget),
            0u,
            kGrassVariantCount};
        _faceRecords.push_back(record);
    }

    _faceRecordsBuilt = true;
    if (++_faceGeneration == 0)
        ++_faceGeneration;
}

void GrassSceneNode::update(float dt) {
    const bool enabled = _enabled && _sceneGraph.grassEnabled();
    if (!enabled) {
        if (_wasGrassEnabled)
            _sceneGraph.gpuScene().unregisterObject(id());
        _wasGrassEnabled = false;
        _gpuSceneDirty = true;
        return;
    }
    _wasGrassEnabled = true;

    if (!_faceRecordsBuilt || _grassGeneration != _sceneGraph.grassGeneration()) {
        _grassGeneration = _sceneGraph.grassGeneration();
        rebuildFaceRecords();
        _gpuSceneDirty = true;
    }
    (void)dt;
}

void GrassSceneNode::collectInto(GpuScene &scene) {
    if (!_enabled || !_sceneGraph.grassEnabled()) {
        scene.unregisterObject(id());
        return;
    }
    if (!_faceRecordsBuilt)
        rebuildFaceRecords();

    std::optional<std::reference_wrapper<Texture>> lightmap;
    if (_hasLightmapUV && !_aabbNode.mesh()->lightmap.empty()) {
        lightmap = *_resourceSvc.textures.get(
            _aabbNode.mesh()->lightmap, TextureUsage::Lightmap);
    }
    Material material;
    material.type = MaterialType::Grass;
    material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)] =
        _properties.texture;
    if (lightmap) {
        material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)] =
            &lightmap->get();
    }
    material.faceCulling = FaceCullMode::None;
    scene.addGrass(renderCategory(RenderCategory::Opaque), id(), nameIds(), material,
                   _faceRecords, _faceGeneration);
    _gpuSceneDirty = false;
}

int GrassSceneNode::getNumClustersInFace(float area) const {
    // Budgets bake at the density CAP, not the live dial: the merge kernel
    // gates the active prefix by density/cap from a push constant, so the
    // slider is live without rebuilding face records. The cap matches the
    // editor slider's maximum.
    return static_cast<int>(glm::round(
        kGrassDensityFactor * kGrassDensityCap *
        _properties.density * area));
}

} // namespace reone::scene
