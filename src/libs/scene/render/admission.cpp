/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/render/admission.h"

#include "reone/system/profiler.h"

#include <algorithm>
#include <limits>
#include <map>

#include "reone/graphics/mesh.h"
#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/scene/node/model.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::scene {
namespace {
using InstanceMaterial = GpuScene::InstanceMaterial;
using AdmissionKind = GpuScene::AdmissionKind;

void hashBytes(uint64_t &hash, const void *data, size_t size) {
    constexpr uint64_t kPrime = 1099511628211ull;
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
}

template <typename T>
void hashVector(uint64_t &hash, const std::vector<T> &values) {
    if (!values.empty())
        hashBytes(hash, values.data(), values.size() * sizeof(T));
}

uint64_t hashUpload(const GpuSceneUpload &upload) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto &object : upload.objects)
        hashBytes(hash, &object.data, sizeof(object.data));
    hashVector(hash, upload.materials);
    hashVector(hash, upload.bones);
    hashVector(hash, upload.danglyPositions);
    hashVector(hash, upload.proceduralQuads);
    hashBytes(hash, &upload.opaqueObjectCount, sizeof(upload.opaqueObjectCount));
    return hash;
}

void applyCategoryOverride(InstanceMaterial &material,
                           const GraphicsOptions &options,
                           uint32_t categoryIndex) {
    const auto &src = options.ptCategoryOverrides[std::min<uint32_t>(categoryIndex, 8u)];
    material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                       std::clamp(src.colorWeight, 0.0f, 1.0f));
    material.overrideParams = glm::vec4(src.roughness,
                                        std::max(0.0f, src.emissionScale),
                                        std::max(0.0f, src.envScale),
                                        std::max(0.0f, src.metallicScale));
    material.roughnessScale = std::max(0.0f, src.roughnessScale);
}

} // namespace

GpuSceneAdmission::GpuSceneAdmission(VulkanRenderer &renderer,
                                     GraphicsOptions &options,
                                     GpuScene &gpuScene) :
    _renderer(renderer), _options(options), _gpuScene(gpuScene) {}

std::optional<GpuScene::Classification> GpuSceneAdmission::classifyMesh(
    const RegisteredMesh &mesh, const ModelSceneNode *skyRoom) {
    // A selected sky shell is an environment source, never merged geometry.
    // This is unconditional: a failed bake uses the fallback cube.
    if (skyRoom && mesh.cullRoot == skyRoom) {
        ++_submission.sky;
        return std::nullopt;
    }
    const bool saber = std::holds_alternative<RegisteredSaber>(mesh.deformation);
    const bool dangly = std::holds_alternative<RegisteredDangly>(mesh.deformation);
    const auto *skinned = std::get_if<RegisteredSkin>(&mesh.deformation);
    if (!std::holds_alternative<std::monostate>(mesh.deformation) &&
        !skinned && !saber && !dangly) {
        ++_submission.deforming;
        return std::nullopt;
    }
    if (saber)
        ++_submission.sabers;
    if (dangly)
        ++_submission.dangly;
    if (mesh.id.index > 0x00ffffffu) {
        ++_submission.outOfRange;
        return std::nullopt;
    }
    if (skinned)
        ++_submission.skinned;

    InstanceMaterial material;
    material.selfIllumColor = glm::vec4(mesh.material.selfIllumColor, 0.0f);
    material.diffuseColor = glm::vec4(mesh.material.diffuseColor, 1.0f);
    material.uv0 = mesh.material.uv[0];
    material.uv1 = mesh.material.uv[1];
    material.uv2 = mesh.material.uv[2];
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(mesh.material));
    const uint32_t categoryIndex = mesh.cullRoot
                                       ? static_cast<uint32_t>(mesh.cullRoot->usage())
                                       : 8u;
    material.featureMask |= (categoryIndex & 0xFu) << 27;

    // Bit 24 remains reserved and wired through the shaders for V5. Selected
    // sky meshes return above, so no admitted object currently receives it.
    if (skyRoom && mesh.cullRoot == skyRoom)
        material.featureMask |= 1u << 24;

    const auto *curated = _gpuScene.traceMaterials().curatedByIndex(mesh.material.curatedIndex);
    if (dangly && (!curated || curated->klass != TraceClass::Emissive))
        material.selfIllumColor = glm::vec4(0.0f);
    if (curated) {
        if (curated->klass == TraceClass::None)
            material.selfIllumColor = glm::vec4(0.0f);
        material.curatedAlbedoMul = glm::vec4(curated->albedoMul, 0.0f);
        material.curatedRoughA = glm::vec4(static_cast<float>(curated->roughnessMode),
                                           curated->roughnessParams.x,
                                           curated->roughnessParams.y,
                                           curated->roughnessParams.z);
        material.curatedRoughB = glm::vec4(curated->roughnessParams.w,
                                           curated->roughnessWeights.x,
                                           curated->roughnessWeights.y,
                                           curated->roughnessWeights.z);
        material.curatedMetalA = glm::vec4(static_cast<float>(curated->metallicMode),
                                           curated->metallicParams.x,
                                           curated->metallicParams.y,
                                           curated->metallicParams.z);
        material.curatedMetalB = glm::vec4(curated->metallicParams.w,
                                           curated->metallicWeights.x,
                                           curated->metallicWeights.y,
                                           curated->metallicWeights.z);
        material.curatedEmission = glm::vec4(curated->emissionValue,
                                             static_cast<float>(curated->emissionMode));
    }

    AdmissionKind kind = AdmissionKind::Opaque;
    if (const auto *diffuse =
            mesh.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        if (diffuse->features().blending == Texture::Blending::Additive) {
            kind = AdmissionKind::AdditiveEmissive;
            ++_submission.additive;
        } else if (diffuse->features().blending == Texture::Blending::PunchThrough ||
                   mesh.material.type == MaterialType::TransparentModel) {
            kind = AdmissionKind::Cutout;
        }
    }
    if ((material.featureMask & (1u << 24)) != 0 ||
        (curated && curated->klass == TraceClass::Prelit)) {
        material.surfaceType = 2;
    }

    applyCategoryOverride(material, _options, categoryIndex);
    const float emissionScale = std::max(0.0f, material.overrideParams.y);
    // Additive emission reads overrideParams.y directly. Curated prelit still
    // takes the old baked scaling path because it terminates instead.
    if (kind != AdmissionKind::AdditiveEmissive || material.surfaceType == 2)
        material.selfIllumColor *= emissionScale;
    if (curated && curated->emissionMode != 0) {
        material.curatedEmission =
            glm::vec4(glm::vec3(material.curatedEmission) * emissionScale,
                      material.curatedEmission.w);
    }

    if (const auto *texture =
            mesh.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture =
            mesh.material.textures[static_cast<size_t>(MaterialTextureSlot::NormalMap)]) {
        material.normalMap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture =
            mesh.material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)]) {
        material.lightmap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture =
            mesh.material.textures[static_cast<size_t>(MaterialTextureSlot::BumpMapArray)]) {
        material.bumpMapArray = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        material.bumpMapFrame = mesh.material.bumpMapFrame;
        material.bumpMapScale = texture->features().bumpMapScaling;
    }

    _submission.dynamicTriangles +=
        (skinned || dangly || saber) ? static_cast<uint32_t>(mesh.mesh.get().faces().size()) : 0;
    if (!dangly && (!curated || curated->klass == TraceClass::Default) &&
        glm::any(glm::greaterThan(mesh.material.selfIllumColor, glm::vec3(0.0f)))) {
        ++_submission.emissive;
    }
    return {{material, kind, GpuScene::ResidencyClass::Dynamic, skinned}};
}

std::optional<GpuScene::Classification> GpuSceneAdmission::classifyProcedural(
    const RegisteredProcedural &procedural) {
    InstanceMaterial material;
    AdmissionKind kind = AdmissionKind::Opaque;
    switch (procedural.kind) {
    case ProceduralKind::Grass:
        _submission.grass += static_cast<uint32_t>(procedural.instances.size());
        material.diffuseColor = glm::vec4(procedural.material.diffuseColor, 1.0f);
        material.uv0 = procedural.material.uv[0];
        material.uv1 = procedural.material.uv[1];
        material.uv2 = procedural.material.uv[2];
        material.featureMask = static_cast<uint32_t>(materialFeatureMask(procedural.material)) |
                               UniformsFeatureFlags::hashedalphatest | (8u << 27);
        if (const auto *texture = procedural.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
            material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        }
        if (const auto *texture = procedural.material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)]) {
            material.lightmap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        }
        applyCategoryOverride(material, _options, 8);
        kind = AdmissionKind::Cutout;
        break;
    case ProceduralKind::Particles:
        _submission.particles += static_cast<uint32_t>(procedural.instances.size());
        material.diffuseColor = glm::vec4(procedural.material.diffuseColor, 1.0f);
        material.uv0 = procedural.material.uv[0];
        material.uv1 = procedural.material.uv[1];
        material.uv2 = procedural.material.uv[2];
        material.featureMask = static_cast<uint32_t>(materialFeatureMask(procedural.material));
        if (const auto *texture = procedural.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
            material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        }
        applyCategoryOverride(material, _options, 8);
        kind = procedural.material.blending == BlendMode::Lighten
                   ? AdmissionKind::AdditiveEmissive
                   : AdmissionKind::LitBlended;
        break;
    case ProceduralKind::Billboard:
        ++_submission.billboards;
        material.diffuseColor = procedural.instances.front().color;
        if (const auto *texture = procedural.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
            material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        }
        kind = AdmissionKind::AdditiveEmissive;
        break;
    }
    return {{material, kind, GpuScene::ResidencyClass::Dynamic, nullptr}};
}

GpuSceneAdmissionResult GpuSceneAdmission::prepare(const glm::mat4 &view) {
    R_PROFILE_ZONE("SceneAdmission::prepare");
    _submission = {};

    static constexpr float kSkyOverlapThreshold = 0.5f;
    glm::vec3 sceneMin(std::numeric_limits<float>::max());
    glm::vec3 sceneMax(std::numeric_limits<float>::lowest());
    struct SkyRoomCandidate {
        glm::vec3 boundsMin {std::numeric_limits<float>::max()};
        glm::vec3 boundsMax {std::numeric_limits<float>::lowest()};
    };
    std::map<const ModelSceneNode *, SkyRoomCandidate> sceneryRooms;
    for (const auto &object : _gpuScene.objects()) {
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (!mesh || !mesh->cullRoot || mesh->cullRoot->usage() != ModelUsage::Room)
            continue;
        if (!_gpuScene.isObjectEnabled(mesh->id.index))
            continue;
        if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                 renderCategory(RenderCategory::Transparent))) == 0) {
            continue;
        }
        const auto worldAabb = mesh->mesh.get().aabb() * mesh->transform;
        sceneMin = glm::min(sceneMin, worldAabb.min());
        sceneMax = glm::max(sceneMax, worldAabb.max());
        if (mesh->cullRoot->isBackgroundScenery()) {
            auto &candidate = sceneryRooms[mesh->cullRoot];
            candidate.boundsMin = glm::min(candidate.boundsMin, worldAabb.min());
            candidate.boundsMax = glm::max(candidate.boundsMax, worldAabb.max());
        }
    }

    GpuSceneAdmissionResult result;
    if (!sceneryRooms.empty()) {
        const glm::vec3 sceneExtent = glm::max(sceneMax - sceneMin, glm::vec3(1e-3f));
        float bestVolume = 0.0f;
        for (const auto &[room, candidate] : sceneryRooms) {
            const glm::vec3 extent = candidate.boundsMax - candidate.boundsMin;
            const glm::vec3 ratios = extent / sceneExtent;
            if (glm::min(ratios.x, glm::min(ratios.y, ratios.z)) < kSkyOverlapThreshold)
                continue;
            const float volume = extent.x * extent.y * extent.z;
            if (volume > bestVolume) {
                bestVolume = volume;
                result.skyRoom = room;
                result.skyOrigin = 0.5f * (candidate.boundsMin + candidate.boundsMax);
            }
        }
        if (result.skyRoom && _frameNumber == 0) {
            info("Vulkan: sky room is '" + result.skyRoom->model().name() + "'",
                 LogChannel::Graphics);
        }
    }

    _gpuScene.setSkyRoom(result.skyRoom);
    _submission.upload = _gpuScene.prepare(
        [this, skyRoom = result.skyRoom](const RegisteredMesh &mesh) {
            return classifyMesh(mesh, skyRoom);
        },
        [this](const RegisteredProcedural &procedural) {
            return classifyProcedural(procedural);
        },
        view);
    // The hash walks every uploaded byte; its one consumer is the
    // --dumptargets log line, so frames outside a dump run skip it.
    result.uploadHash = _options.hashUploads ? hashUpload(_submission.upload) : 0;
    result.submission = std::move(_submission);
    ++_frameNumber;
    return result;
}

} // namespace reone::scene
