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

#include "reone/scene/registry.h"

#include "reone/graphics/camera.h"
#include "reone/scene/node/camera.h"
#include "reone/scene/node/model.h"
#include "reone/scene/render/pass.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

std::string formatRegistryCounts(const RegistryCounts &counts) {
    return "objects=" + std::to_string(counts.objects()) +
           ", rigid=" + std::to_string(counts.rigid) +
           ", skinned=" + std::to_string(counts.skinned) +
           ", dangly=" + std::to_string(counts.dangly) +
           ", saber=" + std::to_string(counts.saber) +
           ", particle_emitters=" + std::to_string(counts.particleEmitters) +
           ", particles=" + std::to_string(counts.particles) +
           ", grass_nodes=" + std::to_string(counts.grassNodes) +
           ", grass_clusters=" + std::to_string(counts.grassClusters) +
           ", billboards=" + std::to_string(counts.billboards) +
           ", aabbs=" + std::to_string(counts.aabbs);
}

std::string renderPassName(RenderPassName pass) {
    switch (pass) {
    case RenderPassName::DirLightShadowsPass:
        return "directional shadows";
    case RenderPassName::PointLightShadows:
        return "point shadows";
    case RenderPassName::OpaqueGeometry:
        return "opaque";
    case RenderPassName::TransparentGeometry:
        return "transparent";
    case RenderPassName::PostProcessing:
        return "post-processing";
    case RenderPassName::Debug:
        return "debug";
    default:
        return "none";
    }
}

static bool isCountedMesh(const Material &material) {
    return material.type == MaterialType::OpaqueModel ||
           material.type == MaterialType::TransparentModel;
}

static void countMesh(RegistryCounts &counts, const RegisteredDeformation &deformation) {
    if (std::holds_alternative<RegisteredSkin>(deformation)) {
        ++counts.skinned;
    } else if (std::holds_alternative<RegisteredDangly>(deformation)) {
        ++counts.dangly;
    } else if (std::holds_alternative<RegisteredSaber>(deformation)) {
        ++counts.saber;
    } else {
        ++counts.rigid;
    }
}

static bool isCulled(ModelSceneNode &root, const CameraSceneNode *camera) {
    if (!root.isEnabled()) {
        return true;
    }
    if (!root.isCullingEnabled() || !camera) {
        return false;
    }
    float distanceToCamera = root.getSquareDistanceTo(*camera);
    float drawDistance = root.drawDistance() * root.drawDistance();
    return (distanceToCamera > drawDistance) || !camera->isInFrustum(root);
}

void RenderRegistry::resetFrame() {
    _objects.clear();
    _traversalCount = 0;
    _registeredCounts = {};
    _drawnCounts = {};
    _drawnCountsByPass.clear();
}

void RenderRegistry::beginSceneTraversal() {
    ++_traversalCount;
}

void RenderRegistry::registerMesh(RenderCategories categories,
                             Mesh &mesh,
                             const Material &material,
                             const glm::mat4 &transform,
                             const glm::mat4 &transformInv,
                             const glm::mat4 &prevTransform,
                             RegisteredDeformation deformation,
                             ModelSceneNode *cullRoot) {
    ++_registeredCounts.entries;
    if (isCountedMesh(material)) {
        countMesh(_registeredCounts, deformation);
    }
    _objects.push_back(
        RegisteredMesh {categories, mesh, material, transform, transformInv, prevTransform,
                        std::move(deformation), cullRoot});
}

void RenderRegistry::registerBillboard(RenderCategories categories,
                                  Texture &texture,
                                  const glm::vec4 &color,
                                  const glm::mat4 &transform,
                                  const glm::mat4 &transformInv,
                                  std::optional<float> size,
                                  ModelSceneNode *cullRoot) {
    ++_registeredCounts.entries;
    ++_registeredCounts.billboards;
    _objects.push_back(RegisteredBillboard {
        categories, texture, color, transform, transformInv, size, cullRoot});
}

void RenderRegistry::registerParticles(RenderCategories categories,
                                  const Material &material,
                                  const glm::ivec2 &gridSize,
                                  const std::vector<ParticleInstance> &instances,
                                  ModelSceneNode *cullRoot) {
    ++_registeredCounts.entries;
    ++_registeredCounts.particleEmitters;
    _registeredCounts.particles += instances.size();
    _objects.push_back(
        RegisteredParticles {categories, material, gridSize, instances, cullRoot});
}

void RenderRegistry::registerGrass(RenderCategories categories,
                              const Material &material,
                              float radius,
                              float quadSize,
                              const std::vector<GrassInstance> &instances) {
    ++_registeredCounts.entries;
    ++_registeredCounts.grassNodes;
    _registeredCounts.grassClusters += instances.size();
    _objects.push_back(
        RegisteredGrass {categories, material, radius, quadSize, instances});
}

void RenderRegistry::registerAABB(RenderCategories categories,
                             const std::vector<glm::vec4> &corners,
                             ModelSceneNode *cullRoot) {
    ++_registeredCounts.entries;
    ++_registeredCounts.aabbs;
    _objects.push_back(RegisteredAABB {categories, corners, cullRoot});
}

void RenderRegistry::addDebug(std::function<void()> execute) {
    ++_registeredCounts.entries;
    _objects.push_back(RegisteredDebug {renderCategory(RenderCategory::Debug), std::move(execute)});
}

void RenderRegistry::drawScene(IRenderPassExecutor &executor,
                               RenderFilter filter,
                               const CameraSceneNode *camera) {
    auto &passCounts = _drawnCountsByPass[filter.pass];
    auto category = renderCategory(filter.category);
    for (auto &object : _objects) {
        std::visit(
            [&](auto &entry) {
                if ((entry.categories & category) == 0) {
                    return;
                }
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, RegisteredMesh>) {
                    if (entry.cullRoot && isCulled(*entry.cullRoot, camera)) {
                        return;
                    }
                    ++_drawnCounts.entries;
                    ++passCounts.entries;
                    if (isCountedMesh(entry.material)) {
                        countMesh(_drawnCounts, entry.deformation);
                        countMesh(passCounts, entry.deformation);
                    }
                    if (auto skin = std::get_if<RegisteredSkin>(&entry.deformation)) {
                        executor.executeDrawSkinned(
                            entry.mesh, entry.material, entry.transform, entry.transformInv,
                            entry.prevTransform, skin->bones, skin->prevBones);
                    } else if (auto dangly = std::get_if<RegisteredDangly>(&entry.deformation)) {
                        executor.executeDrawDangly(
                            entry.mesh, entry.material, entry.transform, entry.transformInv,
                            entry.prevTransform, dangly->positions);
                    } else if (auto saber = std::get_if<RegisteredSaber>(&entry.deformation)) {
                        executor.executeDrawSaber(
                            entry.mesh, entry.material, entry.transform, entry.transformInv,
                            entry.prevTransform, saber->displacement);
                    } else {
                        executor.executeDraw(
                            entry.mesh, entry.material, entry.transform, entry.transformInv,
                            entry.prevTransform);
                    }
                } else if constexpr (std::is_same_v<T, RegisteredBillboard>) {
                    if (!entry.cullRoot || !isCulled(*entry.cullRoot, camera)) {
                        ++_drawnCounts.entries;
                        ++passCounts.entries;
                        ++_drawnCounts.billboards;
                        ++passCounts.billboards;
                        executor.executeDrawBillboard(
                            entry.texture, entry.color, entry.transform, entry.transformInv, entry.size);
                    }
                } else if constexpr (std::is_same_v<T, RegisteredParticles>) {
                    if (entry.cullRoot && isCulled(*entry.cullRoot, camera)) {
                        return;
                    }
                    std::vector<ParticleInstance> visible;
                    visible.reserve(entry.instances.size());
                    for (const auto &instance : entry.instances) {
                        if (!camera || camera->camera()->isInFrustum(instance.position)) {
                            visible.push_back(instance);
                        }
                    }
                    if (!visible.empty()) {
                        ++_drawnCounts.entries;
                        ++passCounts.entries;
                        ++_drawnCounts.particleEmitters;
                        _drawnCounts.particles += visible.size();
                        ++passCounts.particleEmitters;
                        passCounts.particles += visible.size();
                        executor.executeDrawParticles(entry.material, entry.gridSize, visible);
                    }
                } else if constexpr (std::is_same_v<T, RegisteredGrass>) {
                    std::vector<GrassInstance> visible;
                    visible.reserve(entry.instances.size());
                    for (const auto &instance : entry.instances) {
                        if (!camera || camera->camera()->isInFrustum(instance.position)) {
                            visible.push_back(instance);
                        }
                    }
                    if (!visible.empty()) {
                        ++_drawnCounts.entries;
                        ++passCounts.entries;
                        ++_drawnCounts.grassNodes;
                        _drawnCounts.grassClusters += visible.size();
                        ++passCounts.grassNodes;
                        passCounts.grassClusters += visible.size();
                        for (size_t first = 0; first < visible.size(); first += kMaxGrassClusters) {
                            auto last = std::min(first + kMaxGrassClusters, visible.size());
                            std::vector<GrassInstance> batch(
                                visible.begin() + first, visible.begin() + last);
                            executor.executeDrawGrass(
                                entry.radius, entry.quadSize, entry.material, batch);
                        }
                    }
                } else if constexpr (std::is_same_v<T, RegisteredAABB>) {
                    if (!entry.cullRoot || !isCulled(*entry.cullRoot, camera)) {
                        ++_drawnCounts.entries;
                        ++passCounts.entries;
                        ++_drawnCounts.aabbs;
                        ++passCounts.aabbs;
                        executor.executeDrawAABB(entry.corners);
                    }
                } else if constexpr (std::is_same_v<T, RegisteredDebug>) {
                    ++_drawnCounts.entries;
                    ++passCounts.entries;
                    executor.executeDrawDebug(entry.execute);
                }
            },
            object);
    }
}

} // namespace scene

} // namespace reone
