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

#include <cmath>
#include <limits>

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
static constexpr float kClusterSizeRampWidth = 4.0f;
static constexpr int kClusterSizeLevels = 64;
static constexpr float kClusterSizeBandWidth =
    kClusterSizeRampWidth / static_cast<float>(kClusterSizeLevels);

uint8_t clusterSizeLevel(float distance) {
    if (distance >= kMaxClusterDistance) {
        return 0;
    }
    const float unquantized =
        (kMaxClusterDistance - distance) / kClusterSizeRampWidth;
    // Keep every admitted cluster non-zero, then round to the nearest 1/64.
    // This makes a stationary stream byte-identical while retaining the exact
    // 32-unit admission boundary.
    return static_cast<uint8_t>(std::clamp(
        static_cast<int>(glm::floor(unquantized * kClusterSizeLevels + 0.5f)),
        1, kClusterSizeLevels));
}

float distanceToSizeBandBoundary(float distance, uint8_t level) {
    if (level == 0) {
        return std::max(0.0f, distance - kMaxClusterDistance);
    }
    if (level == kClusterSizeLevels) {
        const float outerBoundary =
            kMaxClusterDistance -
            (static_cast<float>(kClusterSizeLevels) - 0.5f) * kClusterSizeBandWidth;
        return std::max(0.0f, outerBoundary - distance);
    }

    const float outerBoundary =
        level == 1
            ? kMaxClusterDistance
            : kMaxClusterDistance -
                  (static_cast<float>(level) - 0.5f) * kClusterSizeBandWidth;
    const float innerBoundary =
        kMaxClusterDistance -
        (static_cast<float>(level) + 0.5f) * kClusterSizeBandWidth;
    return std::max(0.0f,
                    std::min(outerBoundary - distance, distance - innerBoundary));
}

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

void GrassSceneNode::returnAllClusters() {
    std::unordered_set<SceneNode *> returning;
    for (auto &face : _clusterFaces) {
        for (auto &placement : face.clusters) {
            placement.queued = false;
            if (!placement.node) {
                continue;
            }
            returning.insert(placement.node);
            _clusterPool.push(placement.node);
            placement.node = nullptr;
        }
    }
    _pendingClusters.clear();
    if (returning.empty()) {
        return;
    }
    _children.erase(
        std::remove_if(_children.begin(), _children.end(),
                       [&returning](auto *child) { return returning.count(child) > 0; }),
        _children.end());
}

void GrassSceneNode::rebuildClusterPlacements() {
    _clusterFaces.clear();
    _clusterFaces.reserve(_grassFaces.size());
    _pendingClusters.clear();

    auto mesh = _aabbNode.mesh()->mesh;
    auto &faces = mesh->faces();
    for (auto faceIdx : _grassFaces) {
        auto &face = faces[faceIdx];
        auto verts = mesh->faceVertexCoords(face);
        FaceClusters faceClusters;
        faceClusters.faceIndex = faceIdx;
        const int count = getNumClustersInFace(face.area);
        faceClusters.clusters.reserve(count);
        for (int clusterIdx = 0; clusterIdx < count; ++clusterIdx) {
            const float r1sqrt = glm::sqrt(grassRandom01(faceIdx, clusterIdx, 0));
            const float r2 = grassRandom01(faceIdx, clusterIdx, 1);
            const glm::vec3 baryPosition(
                1.0f - r1sqrt, r1sqrt * (1.0f - r2), r2 * r1sqrt);

            ClusterPlacement placement;
            placement.faceIndex = faceIdx;
            placement.clusterIndex = clusterIdx;
            placement.position =
                barycentricToCartesian(verts[0], verts[1], verts[2], baryPosition);
            if (_hasLightmapUV) {
                placement.lightmapUV = *mesh->tryFaceUV2(face, baryPosition);
            }
            placement.variant = getGrassVariant(faceIdx, clusterIdx);
            placement.yaw =
                glm::two_pi<float>() * grassRandom01(faceIdx, clusterIdx, 3);
            faceClusters.clusters.push_back(placement);
        }
        _clusterFaces.push_back(std::move(faceClusters));
    }
    _clusterPlacementsBuilt = true;
}

void GrassSceneNode::updateFaceClusters(
    FaceClusters &face, const glm::vec3 &cameraPosition,
    std::unordered_set<SceneNode *> &returning) {
    float nextUpdateDistance = std::numeric_limits<float>::infinity();
    for (auto &placement : face.clusters) {
        const float distance = glm::distance(placement.position, cameraPosition);
        const uint8_t sizeLevel = clusterSizeLevel(distance);
        nextUpdateDistance = std::min(
            nextUpdateDistance, distanceToSizeBandBoundary(distance, sizeLevel));

        if (sizeLevel == placement.sizeLevel) {
            if (sizeLevel != 0 && !placement.node && !placement.queued) {
                placement.queued = true;
                _pendingClusters.push_back(&placement);
            }
            continue;
        }

        placement.sizeLevel = sizeLevel;
        if (placement.node) {
            if (sizeLevel == 0) {
                returning.insert(placement.node);
                _clusterPool.push(placement.node);
                placement.node = nullptr;
                _gpuSceneDirty = true;
            } else {
                placement.node->setSizeScale(
                    static_cast<float>(sizeLevel) / kClusterSizeLevels);
                _gpuSceneDirty = true;
            }
        } else if (sizeLevel != 0 && !placement.queued) {
            placement.queued = true;
            _pendingClusters.push_back(&placement);
        }
    }

    face.lastCameraPosition = cameraPosition;
    face.nextUpdateDistance2 = nextUpdateDistance * nextUpdateDistance;
    face.initialized = true;
}

void GrassSceneNode::admitPendingClusters(const glm::vec3 &cameraPosition) {
    if (_clusterPool.empty() || _pendingClusters.empty()) {
        return;
    }

    std::sort(_pendingClusters.begin(), _pendingClusters.end(),
              [&cameraPosition](const auto *left, const auto *right) {
                  const float leftDistance2 =
                      glm::distance2(left->position, cameraPosition);
                  const float rightDistance2 =
                      glm::distance2(right->position, cameraPosition);
                  if (leftDistance2 != rightDistance2) {
                      return leftDistance2 < rightDistance2;
                  }
                  if (left->faceIndex != right->faceIndex) {
                      return left->faceIndex < right->faceIndex;
                  }
                  return left->clusterIndex < right->clusterIndex;
              });

    size_t retained = 0;
    for (auto *placement : _pendingClusters) {
        if (!placement->queued) {
            continue;
        }
        if (placement->node || placement->sizeLevel == 0) {
            placement->queued = false;
            continue;
        }
        if (_clusterPool.empty()) {
            _pendingClusters[retained++] = placement;
            continue;
        }

        auto *cluster = _clusterPool.top();
        _clusterPool.pop();
        cluster->setLocalTransform(glm::translate(placement->position));
        cluster->setVariant(placement->variant);
        cluster->setYaw(placement->yaw);
        cluster->setLightmapUV(placement->lightmapUV);
        cluster->setSizeScale(
            static_cast<float>(placement->sizeLevel) / kClusterSizeLevels);
        addChild(*cluster);
        placement->node = cluster;
        placement->queued = false;
        _gpuSceneDirty = true;
    }
    _pendingClusters.resize(retained);
}

void GrassSceneNode::update(float dt) {
    R_PROFILE_ZONE("GrassSceneNode::materialisation");
    if (!_enabled || !_sceneGraph.grassEnabled()) {
        return;
    }
    // Density changes rebuild deterministic placement metadata as well as the
    // materialized nodes. Normal camera motion only visits a face after one of
    // its clusters can have crossed a quantized distance band.
    if (!_clusterPlacementsBuilt ||
        _grassGeneration != _sceneGraph.grassGeneration()) {
        _gpuSceneDirty = true;
        _grassGeneration = _sceneGraph.grassGeneration();
        returnAllClusters();
        growClusterPool(static_cast<int>(
            glm::round(kNumClustersInPool * _sceneGraph.grassDensityScale())));
        rebuildClusterPlacements();
    }
    auto camera = _sceneGraph.camera();
    if (!camera) {
        return;
    }
    auto cameraPos = camera->get().origin();
    glm::vec3 meshSpaceCameraPos(_absTransformInv * glm::vec4(cameraPos, 1.0f));

    std::unordered_set<SceneNode *> returning;
    for (auto &face : _clusterFaces) {
        if (face.initialized) {
            const float cameraMovement2 =
                glm::distance2(face.lastCameraPosition, meshSpaceCameraPos);
            if (cameraMovement2 == 0.0f ||
                cameraMovement2 < face.nextUpdateDistance2) {
                continue;
            }
        }
        updateFaceClusters(face, meshSpaceCameraPos, returning);
    }

    // Remove returned children before reusing their pooled nodes for new
    // placements; otherwise the batched erase could remove a just-added child.
    if (!returning.empty()) {
        _children.erase(
            std::remove_if(_children.begin(), _children.end(),
                           [&returning](auto *child) {
                               return returning.count(child) > 0;
                           }),
            _children.end());
    }
    admitPendingClusters(meshSpaceCameraPos);

    (void)dt;
}

void GrassSceneNode::collectLeafs(GpuScene &scene, const std::vector<SceneNode *> &leafs) {
    collectLeafsImpl(scene, leafs, true);
}

void GrassSceneNode::collectLeafsIfDirty(GpuScene &scene,
                                         const std::vector<SceneNode *> &leafs) {
    collectLeafsImpl(scene, leafs, false);
}

void GrassSceneNode::collectLeafsImpl(GpuScene &scene,
                                      const std::vector<SceneNode *> &leafs,
                                      bool force) {
    if (!force && !_gpuSceneDirty)
        return;
    _gpuSceneDirty = false;
    if (leafs.empty()) {
        scene.unregisterObject(id());
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
        instances[i].sizeScale = cluster->sizeScale();
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
