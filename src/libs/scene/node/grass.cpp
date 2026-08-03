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

#include "reone/scene/node/grass.h"

#include "reone/graphics/barycentricutil.h"
#include "reone/graphics/di/services.h"
#include "reone/graphics/material.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/triangleutil.h"
#include "reone/graphics/uniforms.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/textures.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/grasscluster.h"
#include "reone/scene/render/pipeline.h"
#include "reone/system/profiler.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

static constexpr int kNumClustersInPool = 4096;
// The pool has to grow with the density dial or raising it does the opposite of
// what it says: the nearest faces consume a fixed pool, materialisation stops
// dead, and grass gains density in a shrinking radius with a hard empty edge
// beyond it. Capped so a careless drag cannot allocate scene nodes without end.
static constexpr int kMaxClustersInPool = 32768;
static constexpr float kGrassDensityFactor = 0.5f;

static constexpr float kMaxClusterDistance = 32.0f;
static constexpr float kMaxClusterDistance2 = kMaxClusterDistance * kMaxClusterDistance;

uint32_t grassHash(uint32_t faceIndex, uint32_t clusterIndex, uint32_t stream) {
    uint32_t value = faceIndex * 0x9e3779b9u ^ clusterIndex * 0x85ebca6bu ^ stream * 0xc2b2ae35u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float grassRandom01(int faceIndex, int clusterIndex, uint32_t stream) {
    // Keep 24 random mantissa bits, matching the useful precision of the old
    // randomFloat path while making placement independent of the shared RNG.
    return static_cast<float>(grassHash(static_cast<uint32_t>(faceIndex),
                                        static_cast<uint32_t>(clusterIndex), stream) >>
                              8) /
           16777216.0f;
}

void GrassSceneNode::init() {
    setNameIds({0, _sceneGraph.internName(_aabbNode.name())});
    // Compute grass faces
    auto faces = _aabbNode.mesh()->mesh->faces();
    for (size_t faceIdx = 0; faceIdx < faces.size(); ++faceIdx) {
        auto &face = faces[faceIdx];
        if (_properties.materials.count(face.material) == 0) {
            continue;
        }
        _grassFaces.push_back(static_cast<int>(faceIdx));
        if (!_aabbNode.mesh()->mesh->tryFaceUV2(face, glm::vec3(1.0f, 0.0f, 0.0f))) {
            _hasLightmapUV = false;
        }
    }

    // Pre-allocate grass clusters
    growClusterPool(kNumClustersInPool);
}

void GrassSceneNode::growClusterPool(int target) {
    target = std::min(target, kMaxClustersInPool);
    for (; _poolCapacity < target; ++_poolCapacity) {
        _clusterPool.push(_sceneGraph.newGrassCluster(*this).get());
    }
}

void GrassSceneNode::update(float dt) {
    R_PROFILE_ZONE("GrassSceneNode::materialisation");
    if (!_enabled || !_sceneGraph.grassEnabled()) {
        return;
    }
    // Density is a live dial, and clusters are materialised once per face and
    // then cached. Without dropping the cache a change would only affect faces
    // the camera has not reached yet, which reads as the slider half-working.
    if (_grassGeneration != _sceneGraph.grassGeneration()) {
        _grassGeneration = _sceneGraph.grassGeneration();
        // Returning a cluster to the pool is not enough: it has to leave
        // _children too, exactly as the out-of-distance sweep below does.
        // Without this the node keeps every cluster it has ever materialised
        // as a child, re-adds them on the next materialisation, and the child
        // list grows on every density change - so the frame cost stays high
        // afterwards and climbs with each further change.
        std::unordered_set<SceneNode *> returning;
        for (auto &entry : _materializedClusters) {
            for (auto *cluster : entry.second) {
                returning.insert(cluster);
                _clusterPool.push(cluster);
            }
        }
        _materializedClusters.clear();
        if (!returning.empty()) {
            _children.erase(
                std::remove_if(_children.begin(), _children.end(),
                               [&returning](auto *child) { return returning.count(child) > 0; }),
                _children.end());
        }
        growClusterPool(static_cast<int>(
            glm::round(kNumClustersInPool * _sceneGraph.grassDensityScale())));
    }
    auto camera = _sceneGraph.camera();
    if (!camera) {
        return;
    }
    auto mesh = _aabbNode.mesh()->mesh;
    auto &faces = mesh->faces();
    auto cameraPos = camera->get().origin();
    glm::vec3 meshSpaceCameraPos(_absTransformInv * glm::vec4(cameraPos, 1.0f));

    // Return grass clusters in out-of-distance faces, to the pool
    std::set<int> outOfDistance;
    for (auto &pair : _materializedClusters) {
        auto faceIdx = pair.first;
        auto &face = faces[faceIdx];
        float distance2 = glm::distance2(face.centroid, meshSpaceCameraPos);
        if (distance2 <= kMaxClusterDistance2) {
            continue;
        }
        outOfDistance.insert(faceIdx);
    }
    if (!outOfDistance.empty()) {
        // Collected first, then removed in one sweep. Children are held in
        // insertion order now, so erasing them one at a time would be
        // quadratic in the number of clusters on screen.
        std::unordered_set<SceneNode *> returning;
        for (auto &faceIdx : outOfDistance) {
            auto &clusters = _materializedClusters.find(faceIdx)->second;
            for (auto &cluster : clusters) {
                returning.insert(cluster);
                _clusterPool.push(cluster);
            }
            _materializedClusters.erase(faceIdx);
        }
        _children.erase(
            std::remove_if(_children.begin(), _children.end(),
                           [&returning](auto *child) { return returning.count(child) > 0; }),
            _children.end());
    }

    // Cannot materialize any more grass clusters
    if (_clusterPool.empty()) {
        return;
    }

    // Sort grass faces by distance to camera
    std::multimap<float, int> closestFaces;
    for (size_t faceIdx = 0; faceIdx < faces.size(); ++faceIdx) {
        auto &face = faces[faceIdx];
        if (_properties.materials.count(face.material) == 0) {
            continue;
        }
        float distance2 = glm::distance2(face.centroid, meshSpaceCameraPos);
        if (distance2 > kMaxClusterDistance2) {
            continue;
        }
        closestFaces.insert(std::make_pair(distance2, static_cast<int>(faceIdx)));
    }

    // Materialize grass clusters in closest faces, from the pool
    for (auto &pair : closestFaces) {
        auto faceIdx = pair.second;
        if (_materializedClusters.count(faceIdx) > 0) {
            continue;
        }
        auto &face = faces[faceIdx];
        auto verts = mesh->faceVertexCoords(face);
        for (int i = 0; i < getNumClustersInFace(face.area); ++i) {
            if (_clusterPool.empty()) {
                return;
            }
            const float r1sqrt = glm::sqrt(grassRandom01(faceIdx, i, 0));
            const float r2 = grassRandom01(faceIdx, i, 1);
            glm::vec3 baryPosition(1.0f - r1sqrt, r1sqrt * (1.0f - r2), r2 * r1sqrt);
            glm::vec3 position(barycentricToCartesian(verts[0], verts[1], verts[2], baryPosition));
            glm::vec2 lightmapUV {0.0f};
            if (_hasLightmapUV) {
                lightmapUV = *mesh->tryFaceUV2(face, baryPosition);
            }
            auto cluster = _clusterPool.top();
            _clusterPool.pop();
            cluster->setLocalTransform(glm::translate(position));
            cluster->setVariant(getGrassVariant(faceIdx, i));
            // Stream 3 extends the deterministic (face, cluster) placement
            // hash. This yaw is the sole pose input for both raster and trace.
            cluster->setYaw(glm::two_pi<float>() * grassRandom01(faceIdx, i, 3));
            cluster->setLightmapUV(std::move(lightmapUV));
            addChild(*cluster);
            _materializedClusters[faceIdx].push_back(cluster);
        }
    }
}

void GrassSceneNode::collectLeafs(GpuScene &scene, const std::vector<SceneNode *> &leafs) {
    if (leafs.empty()) {
        return;
    }
    std::optional<std::reference_wrapper<Texture>> lightmap;
    if (_hasLightmapUV && !_aabbNode.mesh()->lightmap.empty()) {
        lightmap = *_resourceSvc.textures.get(_aabbNode.mesh()->lightmap, TextureUsage::Lightmap);
    }
    auto instances = std::vector<GrassInstance>(leafs.size());
    for (size_t i = 0; i < leafs.size(); ++i) {
        const auto cluster = static_cast<GrassClusterSceneNode *>(leafs[i]);
        instances[i].position = cluster->origin();
        instances[i].variant = cluster->variant();
        instances[i].lightmapUV = cluster->lightmapUV();
        instances[i].yaw = cluster->yaw();
    }
    Material material;
    material.type = MaterialType::Grass;
    material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)] = _properties.texture;
    if (lightmap) {
        material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)] = &lightmap->get();
    }
    material.faceCulling = FaceCullMode::None;
    scene.addGrass(renderCategory(RenderCategory::Opaque),
                     id(),
                     nameIds(),
                     material,
                     kMaxClusterDistance,
                     _properties.quadSize,
                     instances);
}

int GrassSceneNode::getNumClustersInFace(float area) const {
    return static_cast<int>(glm::round(kGrassDensityFactor * _sceneGraph.grassDensityScale() *
                                       _properties.density * area));
}

int GrassSceneNode::getGrassVariant(int faceIndex, int clusterIndex) const {
    float sum = _properties.probabilities[0] + _properties.probabilities[1] + _properties.probabilities[2] + _properties.probabilities[3];
    float val = grassRandom01(faceIndex, clusterIndex, 2) * sum;
    float upper = 0.0f;
    for (int i = 0; i < 3; ++i) {
        upper += _properties.probabilities[i];
        if (val < upper) {
            return i;
        }
    }
    return 3;
}

} // namespace scene

} // namespace reone
