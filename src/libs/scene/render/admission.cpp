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
#include "reone/graphics/pbrtextures.h"
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
    hashVector(hash, upload.grassFaces);
    hashVector(hash, upload.grassRanges);
    hashBytes(hash, &upload.cameraPosition, sizeof(upload.cameraPosition));
    hashBytes(hash, &upload.opaqueObjectCount, sizeof(upload.opaqueObjectCount));
    return hash;
}

std::string describeUploadDifference(const GpuSceneUpload &left,
                                     const GpuSceneUpload &right) {
    auto firstDifferent = [](const auto &a, const auto &b) -> size_t {
        const auto count = std::min(a.size(), b.size());
        for (size_t i = 0; i < count; ++i) {
            if (std::memcmp(&a[i], &b[i], sizeof(a[i])) != 0)
                return i;
        }
        return count;
    };
    const auto firstObject = firstDifferent(left.objects, right.objects);
    size_t objectByte = 0;
    uint32_t leftId = UINT32_MAX, rightId = UINT32_MAX;
    if (firstObject < left.objects.size() && firstObject < right.objects.size()) {
        leftId = left.objects[firstObject].objectIndex;
        rightId = right.objects[firstObject].objectIndex;
        const auto *a = reinterpret_cast<const unsigned char *>(
            &left.objects[firstObject].data);
        const auto *b = reinterpret_cast<const unsigned char *>(
            &right.objects[firstObject].data);
        while (objectByte < sizeof(SceneObject) && a[objectByte] == b[objectByte])
            ++objectByte;
    }
    std::string detail = " objects=" + std::to_string(left.objects.size()) + "/" +
                         std::to_string(right.objects.size()) +
                         " first_object=" + std::to_string(firstObject) +
                         " object_ids=" + std::to_string(leftId) + "/" +
                         std::to_string(rightId) +
                         " object_byte=" + std::to_string(objectByte) +
                         " materials=" + std::to_string(left.materials.size()) + "/" +
                         std::to_string(right.materials.size()) +
                         " first_material=" +
                         std::to_string(firstDifferent(left.materials, right.materials)) +
                         " bones=" + std::to_string(left.bones.size()) + "/" +
                         std::to_string(right.bones.size()) +
                         " first_bone=" +
                         std::to_string(firstDifferent(left.bones, right.bones)) +
                         " dangly=" + std::to_string(left.danglyPositions.size()) + "/" +
                         std::to_string(right.danglyPositions.size()) +
                         " first_dangly=" +
                         std::to_string(firstDifferent(left.danglyPositions,
                                                       right.danglyPositions)) +
                         " quads=" + std::to_string(left.proceduralQuads.size()) + "/" +
                         std::to_string(right.proceduralQuads.size()) +
                         " first_quad=" +
                         std::to_string(firstDifferent(left.proceduralQuads,
                                                       right.proceduralQuads)) +
                         " grass_faces=" + std::to_string(left.grassFaces.size()) + "/" +
                         std::to_string(right.grassFaces.size()) +
                         " first_grass_face=" +
                         std::to_string(firstDifferent(left.grassFaces,
                                                       right.grassFaces)) +
                         " grass_ranges=" + std::to_string(left.grassRanges.size()) + "/" +
                         std::to_string(right.grassRanges.size()) +
                         " first_grass_range=" +
                         std::to_string(firstDifferent(left.grassRanges,
                                                       right.grassRanges));
    return detail;
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

void populateMaterialResources(InstanceMaterial &dst,
                               const Material &src,
                               VulkanRenderer &renderer) {
    const auto textureAt = [&src](MaterialTextureSlot slot) {
        return src.textures[static_cast<size_t>(slot)];
    };
    const auto textureId = [&renderer](const Texture *texture) {
        return texture ? renderer.resources().textureId(*texture).value_or(UINT32_MAX)
                       : UINT32_MAX;
    };

    const auto *mainTex = textureAt(MaterialTextureSlot::MainTex);
    dst.mainTex = textureId(mainTex);
    dst.normalMap = textureId(textureAt(MaterialTextureSlot::NormalMap));
    dst.lightmap = textureId(textureAt(MaterialTextureSlot::Lightmap));

    if (const auto *bumpMap = textureAt(MaterialTextureSlot::BumpMapArray)) {
        dst.bumpMapArray = textureId(bumpMap);
        dst.bumpMapFrame = src.bumpMapFrame;
        dst.bumpMapScale = bumpMap->features().bumpMapScaling;
    }
    if (mainTex && mainTex->features().waterAlpha != -1.0f) {
        dst.waterAlpha = mainTex->features().waterAlpha;
    }

    auto *envMap = textureAt(MaterialTextureSlot::EnvMap);
    auto *envMapCube = textureAt(MaterialTextureSlot::EnvMapCube);
    dst.envMap = textureId(envMap);
    dst.envMapCube = textureId(envMapCube);
    if (auto *derivedSource = envMapCube ? envMapCube : envMap) {
        dst.envMapDerivedLayer =
            renderer.pbrTextures().requestEnvMapDerivedLayer(*derivedSource);
    }
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
    // The merged vertex stream has no per-object colour. Preserve the model's
    // alpha controller in the material record for forward compositing.
    material.diffuseColor =
        glm::vec4(mesh.material.diffuseColor, mesh.material.color.a);
    material.ambientColor = glm::vec4(mesh.material.ambientColor, 1.0f);
    material.uv0 = mesh.material.uv[0];
    material.uv1 = mesh.material.uv[1];
    material.uv2 = mesh.material.uv[2];
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(mesh.material));
    populateMaterialResources(material, mesh.material, _renderer);
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
            // TransparentModel stays a cutout. It is not a statement that the
            // surface has continuous coverage: MeshSceneNode::isTransparent
            // falls through to hasAlphaChannel, so any texture that merely
            // carries an alpha channel lands here - which is most foliage.
            // Routing those into the blended pass takes trees out of the
            // G-buffer and visibly changes geometry that was correct before.
            // Genuinely blended meshes need a narrower signal than this flag
            // before they can be separated from alpha-tested ones.
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
        material.diffuseColor = glm::vec4(procedural.material.diffuseColor, 1.0f);
        material.ambientColor = glm::vec4(procedural.material.ambientColor, 1.0f);
        material.uv0 = procedural.material.uv[0];
        material.uv1 = procedural.material.uv[1];
        material.uv2 = procedural.material.uv[2];
        material.featureMask = static_cast<uint32_t>(materialFeatureMask(procedural.material)) |
                               UniformsFeatureFlags::hashedalphatest | (8u << 27);
        applyCategoryOverride(material, _options, 8);
        kind = AdmissionKind::Cutout;
        break;
    case ProceduralKind::Particles:
        _submission.particles += static_cast<uint32_t>(procedural.instanceCount());
        material.diffuseColor = glm::vec4(procedural.material.diffuseColor, 1.0f);
        material.ambientColor = glm::vec4(procedural.material.ambientColor, 1.0f);
        material.uv0 = procedural.material.uv[0];
        material.uv1 = procedural.material.uv[1];
        material.uv2 = procedural.material.uv[2];
        material.featureMask = static_cast<uint32_t>(materialFeatureMask(procedural.material));
        applyCategoryOverride(material, _options, 8);
        kind = procedural.material.blending == BlendMode::Lighten
                   ? AdmissionKind::AdditiveEmissive
                   : AdmissionKind::LitBlended;
        break;
    case ProceduralKind::Billboard:
        ++_submission.billboards;
        // The lowered quad already carries the flare colour and alpha. Keep
        // material alpha neutral so forward blending applies it only once.
        material.diffuseColor =
            glm::vec4(glm::vec3(procedural.instances.front().color), 1.0f);
        material.ambientColor = glm::vec4(procedural.material.ambientColor, 1.0f);
        kind = AdmissionKind::AdditiveEmissive;
        break;
    }
    populateMaterialResources(material, procedural.material, _renderer);
    return {{material, kind, GpuScene::ResidencyClass::Dynamic, nullptr}};
}

void GpuSceneAdmission::rebuildSubmissionCounts(const ModelSceneNode *skyRoom) {
    auto upload = std::move(_submission.upload);
    _submission = {};
    _submission.upload = std::move(upload);
    const auto visibleCategories = renderCategory(RenderCategory::Opaque) |
                                   renderCategory(RenderCategory::Transparent);
    for (const auto &object : _gpuScene.objects()) {
        const auto id = std::visit([](const auto &entry) { return entry.id; }, object);
        if (!_gpuScene.isObjectActive(id) || !_gpuScene.isObjectEnabled(id.index))
            continue;
        if (const auto *mesh = std::get_if<RegisteredMesh>(&object)) {
            if ((mesh->categories & visibleCategories) == 0)
                continue;
            if (skyRoom && mesh->cullRoot == skyRoom) {
                ++_submission.sky;
                continue;
            }
            if (mesh->id.index > 0x00ffffffu) {
                ++_submission.outOfRange;
                continue;
            }
            const bool skinned = std::holds_alternative<RegisteredSkin>(mesh->deformation);
            const bool dangly = std::holds_alternative<RegisteredDangly>(mesh->deformation);
            const bool saber = std::holds_alternative<RegisteredSaber>(mesh->deformation);
            _submission.skinned += skinned ? 1u : 0u;
            _submission.dangly += dangly ? 1u : 0u;
            _submission.sabers += saber ? 1u : 0u;
            if (skinned || dangly || saber)
                _submission.dynamicTriangles +=
                    static_cast<uint32_t>(mesh->mesh.get().faces().size());
            if (const auto *diffuse = mesh->material.textures[
                    static_cast<size_t>(MaterialTextureSlot::MainTex)];
                diffuse && diffuse->features().blending == Texture::Blending::Additive)
                ++_submission.additive;
            const auto *curated =
                _gpuScene.traceMaterials().curatedByIndex(mesh->material.curatedIndex);
            if (!dangly && (!curated || curated->klass == TraceClass::Default) &&
                glm::any(glm::greaterThan(mesh->material.selfIllumColor,
                                          glm::vec3(0.0f))))
                ++_submission.emissive;
            continue;
        }
        const auto &procedural = std::get<RegisteredProcedural>(object);
        if ((procedural.categories & visibleCategories) == 0 ||
            procedural.instanceCount() == 0)
            continue;
        switch (procedural.kind) {
        case ProceduralKind::Grass:
            break;
        case ProceduralKind::Particles:
            _submission.particles += static_cast<uint32_t>(procedural.instanceCount());
            break;
        case ProceduralKind::Billboard:
            ++_submission.billboards;
            break;
        }
    }
    for (const auto &range : _submission.upload.grassRanges)
        _submission.grass += range.clusterCount;
}

GpuSceneAdmissionResult GpuSceneAdmission::prepare(
    const glm::mat4 &view,
    graphics::GpuSceneUpload reuse) {
    R_PROFILE_ZONE("SceneAdmission::prepare");
    _submission = {};

    uint64_t optionsFingerprint = 14695981039346656037ull;
    hashBytes(optionsFingerprint, _options.ptCategoryOverrides,
              sizeof(_options.ptCategoryOverrides));
    if (_optionsFingerprint != 0 && _optionsFingerprint != optionsFingerprint)
        ++_admissionGeneration;
    _optionsFingerprint = optionsFingerprint;

    GpuSceneAdmissionResult result;
    const auto sceneGeneration = _gpuScene.admissionGeneration();
    if (_skyCacheGeneration != sceneGeneration) {
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
                                     renderCategory(RenderCategory::Transparent))) == 0)
                continue;
            const auto worldAabb = mesh->mesh.get().aabb() * mesh->transform;
            sceneMin = glm::min(sceneMin, worldAabb.min());
            sceneMax = glm::max(sceneMax, worldAabb.max());
            if (mesh->cullRoot->isBackgroundScenery()) {
                auto &candidate = sceneryRooms[mesh->cullRoot];
                candidate.boundsMin = glm::min(candidate.boundsMin, worldAabb.min());
                candidate.boundsMax = glm::max(candidate.boundsMax, worldAabb.max());
            }
        }
        if (!sceneryRooms.empty()) {
            const glm::vec3 sceneExtent =
                glm::max(sceneMax - sceneMin, glm::vec3(1e-3f));
            float bestVolume = 0.0f;
            for (const auto &[room, candidate] : sceneryRooms) {
                const glm::vec3 extent = candidate.boundsMax - candidate.boundsMin;
                const glm::vec3 ratios = extent / sceneExtent;
                if (glm::min(ratios.x, glm::min(ratios.y, ratios.z)) <
                    kSkyOverlapThreshold)
                    continue;
                const float volume = extent.x * extent.y * extent.z;
                if (volume > bestVolume) {
                    bestVolume = volume;
                    result.skyRoom = room;
                    result.skyOrigin = 0.5f * (candidate.boundsMin + candidate.boundsMax);
                }
            }
        }
        _cachedSkyRoom = result.skyRoom;
        _cachedSkyOrigin = result.skyOrigin;
        _skyCacheGeneration = sceneGeneration;
        if (result.skyRoom && _frameNumber == 0) {
            info("Vulkan: sky room is '" + result.skyRoom->model().name() + "'",
                 LogChannel::Graphics);
        }
    } else {
        result.skyRoom = _cachedSkyRoom;
        result.skyOrigin = _cachedSkyOrigin;
    }

    if (_classifiedSkyRoom != result.skyRoom) {
        ++_admissionGeneration;
        _classifiedSkyRoom = result.skyRoom;
    }
    _gpuScene.setSkyRoom(result.skyRoom);
    _submission.upload = _gpuScene.prepare(
        [this, skyRoom = result.skyRoom](const RegisteredMesh &mesh) {
            return classifyMesh(mesh, skyRoom);
        },
        [this](const RegisteredProcedural &procedural) {
            return classifyProcedural(procedural);
        },
        view, _admissionGeneration,
        _options.admissionForceFull, std::move(reuse));
    // The live grass density rides to the merge kernel in cameraPosition.w as
    // density/cap; budgets are baked at the cap. Set before hashing so the
    // shadow path (below, same assignment) stays byte-comparable.
    _submission.upload.cameraPosition.w =
        std::clamp(_options.grassDensity / kGrassDensityCap, 0.0f, 1.0f);
    for (const auto &range : _submission.upload.grassRanges)
        _submission.grass += range.clusterCount;
    if (_options.admissionShadow && _gpuScene.shadowScene()) {
        auto savedSubmission = _submission;
        auto shadowUpload = _gpuScene.shadowScene()->prepare(
            [this, skyRoom = result.skyRoom](const RegisteredMesh &mesh) {
                return classifyMesh(mesh, skyRoom);
            },
            [this](const RegisteredProcedural &procedural) {
                return classifyProcedural(procedural);
            },
            view, _admissionGeneration, true);
        _submission = std::move(savedSubmission);
        shadowUpload.cameraPosition.w =
            std::clamp(_options.grassDensity / kGrassDensityCap, 0.0f, 1.0f);
        const auto incrementalHash = hashUpload(_submission.upload);
        const auto shadowHash = hashUpload(shadowUpload);
        if (_submission.upload.objects.empty() != shadowUpload.objects.empty()) {
            // Module loading can update deformers one engine tick before the
            // graph publishes its first complete render lists. Do not let that
            // partial tick seed different persistent intern histories.
            _gpuScene.resetAdmissionCache();
            _gpuScene.shadowScene()->resetAdmissionCache();
            info("GpuScene admission shadow warmup frame=" +
                     std::to_string(_frameNumber),
                 LogChannel::Graphics);
        } else if (incrementalHash != shadowHash) {
            error("GpuScene admission shadow MISMATCH frame=" +
                      std::to_string(_frameNumber) + ", incremental=" +
                      std::to_string(incrementalHash) + ", full=" +
                      std::to_string(shadowHash) +
                      describeUploadDifference(_submission.upload, shadowUpload),
                  LogChannel::Graphics);
        } else {
            info("GpuScene admission shadow match frame=" +
                     std::to_string(_frameNumber) + ", hash=" +
                     std::to_string(incrementalHash),
                 LogChannel::Graphics);
        }
    }
    if (_options.admissionShadow || _options.hashUploads ||
        Logger::instance.isChannelEnabled(LogChannel::Graphics))
        rebuildSubmissionCounts(result.skyRoom);
    // The hash walks every uploaded byte; its one consumer is the
    // --dumptargets log line, so frames outside a dump run skip it.
    result.uploadHash = _options.hashUploads ? hashUpload(_submission.upload) : 0;
    result.submission = std::move(_submission);
    ++_frameNumber;
    return result;
}

} // namespace reone::scene
