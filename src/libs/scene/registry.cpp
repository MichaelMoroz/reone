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

#include <fstream>
#include <sstream>
#include <algorithm>

#include "reone/graphics/camera.h"
#include "reone/scene/node/camera.h"
#include "reone/scene/node/model.h"
#include "reone/scene/render/pass.h"
#include "reone/system/logutil.h"

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

static bool isInAnyFrustum(const SceneNode &node, const Frustum *frusta, size_t numFrusta) {
    for (size_t i = 0; i < numFrusta; ++i) {
        const auto &frustum = frusta[i];
        if (node.isPoint() ? frustum.isInFrustum(node.origin())
                           : frustum.isInFrustum(node.aabb() * node.absoluteTransform())) {
            return true;
        }
    }
    return false;
}

static bool isInAnyFrustum(const glm::vec3 &point, const Frustum *frusta, size_t numFrusta) {
    for (size_t i = 0; i < numFrusta; ++i) {
        const auto &frustum = frusta[i];
        if (frustum.isInFrustum(point)) {
            return true;
        }
    }
    return false;
}

static bool isCulled(ModelSceneNode &root, VisibilityPolicy visibility) {
    if (!root.isEnabled()) {
        return true;
    }
    if (!root.isCullingEnabled() || visibility.kind == VisibilityPolicyKind::None) {
        return false;
    }
    // Independent of which volume is tested below: a shadow pass culls against
    // the light but still drops what is too far away to be worth casting.
    if (visibility.drawDistanceCamera) {
        float distanceToCamera = root.getSquareDistanceTo(*visibility.drawDistanceCamera);
        float drawDistance = root.drawDistance() * root.drawDistance();
        if (distanceToCamera > drawDistance) {
            return true;
        }
    }
    switch (visibility.kind) {
    case VisibilityPolicyKind::ViewCamera:
        return visibility.drawDistanceCamera && !visibility.drawDistanceCamera->isInFrustum(root);
    case VisibilityPolicyKind::Frusta:
        return !isInAnyFrustum(root, visibility.lightFrusta, visibility.numLightFrusta);
    case VisibilityPolicyKind::None:
        return false;
    }
    return false;
}

static bool isCulled(const glm::vec3 &point, VisibilityPolicy visibility) {
    switch (visibility.kind) {
    case VisibilityPolicyKind::ViewCamera:
        return visibility.drawDistanceCamera &&
               !visibility.drawDistanceCamera->camera()->frustum().isInFrustum(point);
    case VisibilityPolicyKind::Frusta:
        return !isInAnyFrustum(point, visibility.lightFrusta, visibility.numLightFrusta);
    case VisibilityPolicyKind::None:
        return false;
    }
    return false;
}

void RenderRegistry::resetFrame() {
    // The vector is discarded immediately afterwards, but clear the
    // per-entry bookkeeping here as well so a future retained snapshot cannot
    // accidentally carry draw results into its next frame.
    for (auto &object : _objects) {
        std::visit([](auto &entry) {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (!std::is_same_v<T, RegisteredDebug>) {
                entry.drawnPasses = 0;
            }
        }, object);
    }
    _objects.clear();
    _traversalCount = 0;
    _registeredCounts = {};
    _drawnCounts = {};
    _drawnCountsByPass.clear();
}

void RenderRegistry::beginSceneTraversal() {
    ++_traversalCount;
}

void RenderRegistry::checkIdentityStability() {
    if (!Logger::instance.isChannelEnabled(LogChannel::Graphics)) {
        return;
    }

    std::vector<SceneNodeId> ids;
    ids.reserve(_objects.size());
    for (const auto &object : _objects) {
        std::visit(
            [&ids](const auto &entry) {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (!std::is_same_v<T, RegisteredDebug>) {
                    ids.push_back(entry.id);
                }
            },
            object);
    }
    std::sort(ids.begin(), ids.end(), [](SceneNodeId a, SceneNodeId b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });

    const bool unique = std::adjacent_find(ids.begin(), ids.end()) == ids.end();
    const bool sameAsPrevious = !_previousFrameIds.empty() && ids == _previousFrameIds;
    info("Scene registry ids snapshot=" + std::to_string(++_identitySnapshot) +
             ", entries=" + std::to_string(ids.size()) +
             ", unique=" + (unique ? "true" : "false") +
             ", same_as_previous=" + (sameAsPrevious ? "true" : "false"),
         LogChannel::Graphics);
    _previousFrameIds = std::move(ids);
}

void RenderRegistry::registerMesh(RenderCategories categories,
                             SceneNodeId id,
                             SceneNodeNameIds nameIds,
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
        RegisteredMesh {categories, id, nameIds, 0, mesh, material, transform, transformInv, prevTransform,
                        std::move(deformation), cullRoot});
}

void RenderRegistry::registerBillboard(RenderCategories categories,
                                  SceneNodeId id,
                                  SceneNodeNameIds nameIds,
                                  Texture &texture,
                                  const glm::vec4 &color,
                                  const glm::mat4 &transform,
                                  const glm::mat4 &transformInv,
                                  std::optional<float> size,
                                  ModelSceneNode *cullRoot) {
    ++_registeredCounts.entries;
    ++_registeredCounts.billboards;
    _objects.push_back(RegisteredBillboard {
        categories, id, nameIds, 0, texture, color, transform, transformInv, size, cullRoot});
}

void RenderRegistry::registerParticles(RenderCategories categories,
                                  SceneNodeId id,
                                  SceneNodeNameIds nameIds,
                                  const Material &material,
                                  const glm::ivec2 &gridSize,
                                  const std::vector<ParticleInstance> &instances,
                                  ModelSceneNode *cullRoot) {
    ++_registeredCounts.entries;
    ++_registeredCounts.particleEmitters;
    _registeredCounts.particles += instances.size();
    _objects.push_back(
        RegisteredParticles {categories, id, nameIds, 0, material, gridSize, instances, cullRoot});
}

void RenderRegistry::registerGrass(RenderCategories categories,
                              SceneNodeId id,
                              SceneNodeNameIds nameIds,
                              const Material &material,
                              float radius,
                              float quadSize,
                              const std::vector<GrassInstance> &instances) {
    ++_registeredCounts.entries;
    ++_registeredCounts.grassNodes;
    _registeredCounts.grassClusters += instances.size();
    _objects.push_back(
        RegisteredGrass {categories, id, nameIds, 0, material, radius, quadSize, instances});
}

void RenderRegistry::registerAABB(RenderCategories categories,
                             SceneNodeId id,
                             SceneNodeNameIds nameIds,
                             const std::vector<glm::vec4> &corners,
                             ModelSceneNode *cullRoot) {
    ++_registeredCounts.entries;
    ++_registeredCounts.aabbs;
    _objects.push_back(RegisteredAABB {categories, id, nameIds, 0, corners, cullRoot});
}

void RenderRegistry::addDebug(std::function<void()> execute) {
    ++_registeredCounts.entries;
    _objects.push_back(RegisteredDebug {renderCategory(RenderCategory::Debug), std::move(execute)});
}


bool RenderRegistry::CuratedMaterial::isDefault() const {
    return klass == TraceClass::Default &&
           albedoMul == glm::vec3(1.0f) &&
           roughnessMode == 0 && metallicMode == 0 && emissionMode == 0;
}

int RenderRegistry::curatedIndex(const std::string &model, const std::string &node) const {
    auto exact = _curatedIndexByKey.find(model + "/" + node);
    if (exact != _curatedIndexByKey.end()) {
        return exact->second;
    }
    auto wildcard = _curatedIndexByKey.find(model + "/*");
    if (wildcard != _curatedIndexByKey.end()) {
        return wildcard->second;
    }
    return -1;
}

const RenderRegistry::CuratedMaterial *RenderRegistry::curatedByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(_curatedMaterials.size())) {
        return nullptr;
    }
    return &_curatedMaterials[index];
}

RenderRegistry::CuratedMaterial RenderRegistry::curatedFor(const std::string &model,
                                                           const std::string &node) const {
    auto index = curatedIndex(model, node);
    return index >= 0 ? _curatedMaterials[index] : CuratedMaterial();
}

void RenderRegistry::setCurated(const std::string &model, const std::string &node,
                                CuratedMaterial curated) {
    auto key = model + "/" + node;
    auto found = _curatedIndexByKey.find(key);
    if (curated.isDefault()) {
        // Record slots stay put - materials hold indices for the rest of the
        // frame - only the key mapping goes, and the file line with it.
        if (found != _curatedIndexByKey.end()) {
            _curatedIndexByKey.erase(found);
        }
    } else if (found != _curatedIndexByKey.end()) {
        _curatedMaterials[found->second] = std::move(curated);
    } else {
        _curatedIndexByKey[key] = static_cast<int>(_curatedMaterials.size());
        _curatedMaterials.push_back(std::move(curated));
    }
    saveTraceClasses();
}

namespace {

const char *traceClassName(RenderRegistry::TraceClass klass) {
    switch (klass) {
    case RenderRegistry::TraceClass::Prelit:
        return "prelit";
    case RenderRegistry::TraceClass::Emissive:
        return "emissive";
    case RenderRegistry::TraceClass::None:
        return "none";
    default:
        return "default";
    }
}

RenderRegistry::TraceClass traceClassFromName(const std::string &name) {
    if (name == "prelit") return RenderRegistry::TraceClass::Prelit;
    if (name == "emissive") return RenderRegistry::TraceClass::Emissive;
    if (name == "none") return RenderRegistry::TraceClass::None;
    return RenderRegistry::TraceClass::Default;
}

std::vector<float> parseFloats(const std::string &text) {
    std::vector<float> values;
    std::istringstream in(text);
    float value;
    while (in >> value) {
        values.push_back(value);
    }
    return values;
}

void parseChannel(const std::string &value, int &mode, glm::vec4 &params, glm::vec3 &weights) {
    // "0.35"                        -> constant override
    // "curve base a b t [wr wg wb]" -> albedo-driven curve:
    //   lerp(base, smoothstep(a, b, dot(albedo, weights)), t)
    if (value.rfind("curve", 0) == 0) {
        auto numbers = parseFloats(value.substr(5));
        if (numbers.size() >= 4) {
            mode = 2;
            params = {numbers[0], numbers[1], numbers[2], numbers[3]};
            if (numbers.size() >= 7) {
                weights = {numbers[4], numbers[5], numbers[6]};
            }
        }
    } else {
        auto numbers = parseFloats(value);
        if (numbers.size() == 1) {
            mode = 1;
            params.x = numbers[0];
        }
    }
}

std::string formatChannel(int mode, const glm::vec4 &params, const glm::vec3 &weights) {
    std::ostringstream out;
    if (mode == 1) {
        out << params.x;
    } else {
        out << "curve " << params.x << " " << params.y << " " << params.z << " " << params.w;
        if (weights != glm::vec3(0.299f, 0.587f, 0.114f)) {
            out << " " << weights.x << " " << weights.y << " " << weights.z;
        }
    }
    return out.str();
}

} // namespace

void RenderRegistry::loadTraceClasses(const std::filesystem::path &path) {
    _traceClassesPath = path;
    _curatedIndexByKey.clear();
    _curatedMaterials.clear();
    std::ifstream in(path);
    if (!in) {
        return;
    }
    auto trim = [](std::string &text) {
        auto begin = text.find_first_not_of(" \t\r");
        auto end = text.find_last_not_of(" \t\r");
        text = begin == std::string::npos ? "" : text.substr(begin, end - begin + 1);
    };
    // INI-style sections keyed model/node, with the original flat
    // "model/node = class" lines still accepted for compatibility.
    std::string line;
    std::string section;
    CuratedMaterial current;
    auto flush = [&]() {
        if (!section.empty() && !current.isDefault()) {
            _curatedIndexByKey[section] = static_cast<int>(_curatedMaterials.size());
            _curatedMaterials.push_back(current);
        }
        current = CuratedMaterial();
    };
    while (std::getline(in, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            flush();
            section = line.substr(1, line.size() - 2);
            trim(section);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        bool multiply = eq > 0 && line[eq - 1] == '*';
        auto key = line.substr(0, multiply ? eq - 1 : eq);
        auto value = line.substr(eq + 1);
        trim(key);
        trim(value);
        if (section.empty()) {
            // Legacy flat line: "model/node = class".
            auto klass = traceClassFromName(value);
            if (klass != TraceClass::Default && !key.empty()) {
                CuratedMaterial legacy;
                legacy.klass = klass;
                _curatedIndexByKey[key] = static_cast<int>(_curatedMaterials.size());
                _curatedMaterials.push_back(legacy);
            }
            continue;
        }
        if (key == "class") {
            current.klass = traceClassFromName(value);
        } else if (key == "albedo" && multiply) {
            auto numbers = parseFloats(value);
            if (numbers.size() >= 3) {
                current.albedoMul = {numbers[0], numbers[1], numbers[2]};
            }
        } else if (key == "roughness") {
            parseChannel(value, current.roughnessMode, current.roughnessParams,
                         current.roughnessWeights);
        } else if (key == "metallic") {
            parseChannel(value, current.metallicMode, current.metallicParams,
                         current.metallicWeights);
        } else if (key == "emission") {
            auto numbers = parseFloats(value);
            if (numbers.size() >= 3) {
                current.emissionMode = multiply ? 1 : 2;
                current.emissionValue = {numbers[0], numbers[1], numbers[2]};
            } else if (numbers.size() == 1 && multiply) {
                current.emissionMode = 1;
                current.emissionValue = glm::vec3(numbers[0]);
            }
        }
    }
    flush();
}

void RenderRegistry::saveTraceClasses() const {
    if (_traceClassesPath.empty()) {
        return;
    }
    std::ofstream out(_traceClassesPath);
    for (const auto &[key, index] : _curatedIndexByKey) {
        const auto &curated = _curatedMaterials[index];
        out << "[" << key << "]" << "\n";
        if (curated.klass != TraceClass::Default) {
            out << "class = " << traceClassName(curated.klass) << "\n";
        }
        if (curated.albedoMul != glm::vec3(1.0f)) {
            out << "albedo *= " << curated.albedoMul.x << " " << curated.albedoMul.y << " "
                << curated.albedoMul.z << "\n";
        }
        if (curated.roughnessMode != 0) {
            out << "roughness = "
                << formatChannel(curated.roughnessMode, curated.roughnessParams,
                                 curated.roughnessWeights)
                << "\n";
        }
        if (curated.metallicMode != 0) {
            out << "metallic = "
                << formatChannel(curated.metallicMode, curated.metallicParams,
                                 curated.metallicWeights)
                << "\n";
        }
        if (curated.emissionMode == 1) {
            out << "emission *= " << curated.emissionValue.x << " " << curated.emissionValue.y
                << " " << curated.emissionValue.z << "\n";
        } else if (curated.emissionMode == 2) {
            out << "emission = " << curated.emissionValue.x << " " << curated.emissionValue.y
                << " " << curated.emissionValue.z << "\n";
        }
        out << "\n";
    }
}

void RenderRegistry::drawScene(IRenderPassExecutor &executor,
                               RenderFilter filter,
                               VisibilityPolicy visibility) {
    executor.beginPass(filter.pass);
    auto &passCounts = _drawnCountsByPass[filter.pass];
    auto category = renderCategory(filter.category);
    for (auto &object : _objects) {
        std::visit(
            [&](auto &entry) {
                if ((entry.categories & category) == 0) {
                    return;
                }
                using T = std::decay_t<decltype(entry)>;
                // The registry panel's per-object kill switch. Debug draws
                // carry no id and stay.
                if constexpr (!std::is_same_v<T, RegisteredDebug>) {
                    if (!isObjectEnabled(entry.id.index)) {
                        return;
                    }
                }
                if constexpr (std::is_same_v<T, RegisteredMesh>) {
                    if (entry.cullRoot && isCulled(*entry.cullRoot, visibility)) {
                        return;
                    }
                    ++_drawnCounts.entries;
                    ++passCounts.entries;
                    entry.drawnPasses |= renderPassFlag(filter.pass);
                    if (isCountedMesh(entry.material)) {
                        countMesh(_drawnCounts, entry.deformation);
                        countMesh(passCounts, entry.deformation);
                    }
                    bool shadowPass = filter.pass == RenderPassName::DirLightShadowsPass ||
                                      filter.pass == RenderPassName::PointLightShadows;
                    if (shadowPass) {
                        // Shadow shaders only have the plain POSITION vertex path.
                        executor.executeDraw(
                            entry.mesh, entry.material, entry.transform, entry.transformInv,
                            entry.prevTransform);
                    } else if (auto skin = std::get_if<RegisteredSkin>(&entry.deformation)) {
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
                    if (!entry.cullRoot || !isCulled(*entry.cullRoot, visibility)) {
                        ++_drawnCounts.entries;
                        ++passCounts.entries;
                        ++_drawnCounts.billboards;
                        ++passCounts.billboards;
                        entry.drawnPasses |= renderPassFlag(filter.pass);
                        executor.executeDrawBillboard(
                            entry.texture, entry.color, entry.transform, entry.transformInv, entry.size);
                    }
                } else if constexpr (std::is_same_v<T, RegisteredParticles>) {
                    if (entry.cullRoot && isCulled(*entry.cullRoot, visibility)) {
                        return;
                    }
                    std::vector<ParticleInstance> visible;
                    visible.reserve(entry.instances.size());
                    for (const auto &instance : entry.instances) {
                        if (!isCulled(instance.position, visibility)) {
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
                        entry.drawnPasses |= renderPassFlag(filter.pass);
                        // kMaxParticles is the uniform-block capacity, not a
                        // scene limit. Keep the complete registered list in
                        // lockstep with the tracer by issuing uniform-sized
                        // raster batches, exactly as grass does below.
                        for (size_t first = 0; first < visible.size(); first += kMaxParticles) {
                            auto last = std::min(first + kMaxParticles, visible.size());
                            std::vector<ParticleInstance> batch(
                                visible.begin() + first, visible.begin() + last);
                            executor.executeDrawParticles(entry.material, entry.gridSize, batch);
                        }
                    }
                } else if constexpr (std::is_same_v<T, RegisteredGrass>) {
                    std::vector<GrassInstance> visible;
                    visible.reserve(entry.instances.size());
                    for (const auto &instance : entry.instances) {
                        if (!isCulled(instance.position, visibility)) {
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
                        entry.drawnPasses |= renderPassFlag(filter.pass);
                        for (size_t first = 0; first < visible.size(); first += kMaxGrassClusters) {
                            auto last = std::min(first + kMaxGrassClusters, visible.size());
                            std::vector<GrassInstance> batch(
                                visible.begin() + first, visible.begin() + last);
                            executor.executeDrawGrass(
                                entry.radius, entry.quadSize, entry.material, batch);
                        }
                    }
                } else if constexpr (std::is_same_v<T, RegisteredAABB>) {
                    if (!entry.cullRoot || !isCulled(*entry.cullRoot, visibility)) {
                        ++_drawnCounts.entries;
                        ++passCounts.entries;
                        ++_drawnCounts.aabbs;
                        ++passCounts.aabbs;
                        entry.drawnPasses |= renderPassFlag(filter.pass);
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
