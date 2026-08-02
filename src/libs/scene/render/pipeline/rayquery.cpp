/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/render/pipeline/rayquery.h"

#include <algorithm>
#include <limits>
#include <map>

#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/rayquery.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/scenepipeline.h"
#include "reone/scene/node/model.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::scene {
namespace {
using InstanceMaterial = GpuScene::InstanceMaterial;
}

RayQueryPipeline::RayQueryPipeline(VulkanRenderer &renderer,
                                   glm::ivec2 extent,
                                   GraphicsOptions &options,
                                   GpuScene &gpuScene,
                                   VulkanGpuScene &deviceGpuScene,
                                   bool primaryRayMode) :
    _renderer(renderer), _extent(extent), _options(options), _gpuScene(gpuScene),
    _deviceGpuScene(deviceGpuScene), _primaryRayMode(primaryRayMode) {}

RayQueryPipeline::~RayQueryPipeline() {
    deinit();
}

void RayQueryPipeline::init() {
    if (!_primaryRayMode || _native)
        return;
    _native = std::make_unique<VulkanRayQuery>(_renderer, _extent, _options);
    _native->init();
}

void RayQueryPipeline::deinit() {
    _native.reset();
}

void RayQueryPipeline::restartTemporalHistory() {
    if (_native)
        _native->restartTemporalHistory();
}

VulkanRayQuery &RayQueryPipeline::native() {
    return *_native;
}

const VulkanRayQuery &RayQueryPipeline::native() const {
    return *_native;
}
std::optional<GpuScene::Classification> RayQueryPipeline::classifyMesh(const RegisteredMesh &registeredMesh,
                                                                  const ModelSceneNode *skyRoom,
                                                                  bool skyBaked) {
    const auto *mesh = &registeredMesh;
    // The cubemap is ready only after the room's complete textured raster
    // bake. Until then preserve the old geometry path exactly, including
    // its candidate rejection for shadow rays.
    if (skyBaked && mesh->cullRoot == skyRoom) {
        ++_submission.sky;
        return std::nullopt;
    }
    const bool saber = std::holds_alternative<RegisteredSaber>(mesh->deformation);
    const bool dangly = std::holds_alternative<RegisteredDangly>(mesh->deformation);
    const auto *skinned = std::get_if<RegisteredSkin>(&mesh->deformation);
    if (!std::holds_alternative<std::monostate>(mesh->deformation) && !skinned && !saber && !dangly) {
        ++_submission.deforming;
        return std::nullopt;
    }
    if (saber)
        ++_submission.sabers;
    if (dangly)
        ++_submission.dangly;
    if (mesh->id.index > 0x00ffffffu) {
        ++_submission.outOfRange;
        return std::nullopt;
    }
    if (skinned) {
        ++_submission.skinned;
    }
    InstanceMaterial material;
    material.selfIllumColor = glm::vec4(mesh->material.selfIllumColor, 0.0f);
    material.diffuseColor = glm::vec4(mesh->material.diffuseColor, 1.0f);
    material.uv0 = mesh->material.uv[0];
    material.uv1 = mesh->material.uv[1];
    material.uv2 = mesh->material.uv[2];
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(mesh->material));
    // Bits 27-30 carry the object category - a scene::ModelUsage value,
    // 8 for meshes without a model root - so per-category material
    // overrides resolve at hit time without touching the layout. Must
    // match kTraceCategoryShift/Mask in slang/rayquery.slang.
    uint32_t categoryIndex = mesh->cullRoot
                                 ? static_cast<uint32_t>(mesh->cullRoot->usage())
                                 : 8u;
    material.featureMask |= (categoryIndex & 0xFu) << 27;
    // The one sky room, detected by the scene-overlap pre-pass above.
    // Its meshes render their texture as prelit radiance, terminate
    // paths, take the sky dial, and stay transparent to shadow rays in
    // the candidate shader: the environment never occludes the sun.
    // Everything else - lit panels, backdrop strips, vista rooms - is
    // plain geometry with real occlusion. The bit is tracing-local,
    // deliberately above the shared UniformsFeatureFlags range; must
    // match kTraceSky in slang/rayquery.slang.
    if (skyRoom && mesh->cullRoot == skyRoom) {
        material.featureMask |= 1u << 24;
        ++_submission.sky;
    }
    // The curated per-name record - the manual level pass. Prelit is
    // Odyssey's actual selfIllum semantics: fullbright authored
    // texture, occluding, casting nothing. None strips a wrong
    // selfIllum outright. Material operations ride the same record.
    const auto *curated = _gpuScene.traceMaterials().curatedByIndex(mesh->material.curatedIndex);
    // Dangly selfIllum is Odyssey's fullbright trick for foliage, not
    // emission - danm14ab carries 653 dangly canopies that were glowing
    // and casting. Stripped by default; the curated emissive class
    // restores it for any plant that genuinely glows.
    if (dangly && (!curated || curated->klass != TraceClass::Emissive)) {
        material.selfIllumColor = glm::vec4(0.0f);
    }
    if (curated) {
        if (curated->klass == TraceClass::None) {
            material.selfIllumColor = glm::vec4(0.0f);
        }
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
    // The surface model, assigned here - the shader never classifies.
    // Additive-blended diffuse is Odyssey's other authored glow: no
    // selfIllum controller, the texture is the light, and the ray passes
    // through it (saber blades, glow decals). The sky room and curated
    // prelit imagery are unlit emissive: radiance as authored, path
    // ends. Everything else is PBR; transparent non-additive meshes -
    // alpha-blended leaves above all - carry their coverage in diffuse
    // alpha and resolve stochastically in the surface model.
    if (const auto *diffuse = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        if (diffuse->features().blending == Texture::Blending::Additive) {
            material.surfaceType = 1;
            ++_submission.additive;
        } else if (diffuse->features().blending == Texture::Blending::PunchThrough ||
                   mesh->material.type == MaterialType::TransparentModel) {
            material.featureMask |= 1u << 26;
        }
    }
    if ((material.featureMask & (1u << 24)) != 0 ||
        (curated && curated->klass == TraceClass::Prelit)) {
        material.surfaceType = 2;
    }
    // The per-category calibration override, baked per instance so the
    // dials stay live through the per-frame admission - no GPU table.
    {
        const auto &src = _options.ptCategoryOverrides[std::min<uint32_t>(categoryIndex, 8u)];
        material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                           std::clamp(src.colorWeight, 0.0f, 1.0f));
        material.overrideParams = glm::vec4(src.roughness,
                                            std::max(0.0f, src.emissionScale),
                                            std::max(0.0f, src.envScale),
                                            std::max(0.0f, src.metallicScale));
        material.roughnessScale = std::max(0.0f, src.roughnessScale);
        // Baked into the emission values here rather than left for the
        // shader to multiply. Emission reaches the surface models by two
        // routes - the self-illum controller and the curated override -
        // and only one of them passed through a scale, so the dial moved
        // some emitters and not others.
        // Additive surfaces are the exception: their emission is
        // max(selfIllum, 1) * albedo, so a scale folded into selfIllum
        // vanishes below 1 and would double up above it. That model keeps
        // reading overrideParams.y directly.
        const float emissionScale = std::max(0.0f, src.emissionScale);
        if (material.surfaceType != 1) {
            material.selfIllumColor *= emissionScale;
        }
        if (curated && curated->emissionMode != 0) {
            material.curatedEmission = glm::vec4(glm::vec3(material.curatedEmission) * emissionScale,
                                                 material.curatedEmission.w);
        }
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::NormalMap)]) {
        material.normalMap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)]) {
        material.lightmap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::BumpMapArray)]) {
        material.bumpMapArray = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        material.bumpMapFrame = mesh->material.bumpMapFrame;
        material.bumpMapScale = texture->features().bumpMapScaling;
    }
    const bool nonOpaque = material.surfaceType == 1 ||
                           (material.featureMask & ((1u << 24) | (1u << 26))) != 0;
    _submission.dynamicTriangles += (skinned || dangly || saber) ? static_cast<uint32_t>(mesh->mesh.get().faces().size()) : 0;
    if (!dangly && (!curated || curated->klass == TraceClass::Default) &&
        glm::any(glm::greaterThan(mesh->material.selfIllumColor, glm::vec3(0.0f))))
        ++_submission.emissive;
    // Material::staticObject is an authored room hint, not the admission
    // proof required to retain geometry. Until a stronger classifier exists,
    // publish this consumer's objects as dynamic.
    return {{material, nonOpaque ? GpuScene::PrimitiveClass::NonOpaque : GpuScene::PrimitiveClass::Opaque,
             GpuScene::ResidencyClass::Dynamic, skinned}};
}

std::optional<GpuScene::Classification> RayQueryPipeline::classifyGrass(const RegisteredGrass &grass) {
    // Clusters, not entries: one RegisteredGrass carries a whole hillside, so
    // counting entries reported 1 where 1482 quads were admitted. The point of
    // this counter is to answer "is it actually there", which a count of
    // registrations cannot.
    _submission.grass += static_cast<uint32_t>(grass.instances.size());
    InstanceMaterial material;
    material.diffuseColor = glm::vec4(grass.material.diffuseColor, 1.0f);
    material.uv0 = grass.material.uv[0];
    material.uv1 = grass.material.uv[1];
    material.uv2 = grass.material.uv[2];
    // Raster grass always uses hashed coverage, irrespective of the texture's
    // blending metadata. Preserve that same candidate semantics for the BLAS.
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(grass.material)) |
                           UniformsFeatureFlags::hashedalphatest | (1u << 26) | (8u << 27);
    if (const auto *texture = grass.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = grass.material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)]) {
        material.lightmap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    const auto &src = _options.ptCategoryOverrides[8];
    material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                       std::clamp(src.colorWeight, 0.0f, 1.0f));
    material.overrideParams = glm::vec4(src.roughness,
                                        std::max(0.0f, src.emissionScale),
                                        std::max(0.0f, src.envScale),
                                        std::max(0.0f, src.metallicScale));
    material.roughnessScale = std::max(0.0f, src.roughnessScale);
    return {{material, GpuScene::PrimitiveClass::NonOpaque,
             GpuScene::ResidencyClass::Dynamic, nullptr}};
}

std::optional<GpuScene::Classification> RayQueryPipeline::classifyParticles(const RegisteredParticles &particles) {
    _submission.particles += static_cast<uint32_t>(particles.instances.size());
    InstanceMaterial material;
    material.diffuseColor = glm::vec4(particles.material.diffuseColor, 1.0f);
    material.uv0 = particles.material.uv[0];
    material.uv1 = particles.material.uv[1];
    material.uv2 = particles.material.uv[2];
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(particles.material));
    if (const auto *texture = particles.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    // Particle OIT is a coverage layer. The tracer has no OIT compositing
    // path, so ordinary particles use its non-opaque alpha candidate path;
    // additive emitters retain their existing pass-through emission semantics.
    if (particles.material.blending == BlendMode::Lighten) {
        material.surfaceType = 1;
    } else {
        // Particle OIT represents continuous coverage. It is a real PBR
        // surface in the tracer, with the unoccluded portion transmitted.
        // Without this bit ptTraceNearest commits the quad unconditionally
        // and never reads its alpha at all, which renders smoke as solid
        // black blocks.
        material.featureMask |= 1u << 25;
    }
    const auto &src = _options.ptCategoryOverrides[8];
    material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                       std::clamp(src.colorWeight, 0.0f, 1.0f));
    material.overrideParams = glm::vec4(src.roughness,
                                        std::max(0.0f, src.emissionScale),
                                        std::max(0.0f, src.envScale),
                                        std::max(0.0f, src.metallicScale));
    material.roughnessScale = std::max(0.0f, src.roughnessScale);
    return {{material, GpuScene::PrimitiveClass::NonOpaque,
             GpuScene::ResidencyClass::Dynamic, nullptr}};
}

std::optional<GpuScene::Classification> RayQueryPipeline::classifyBillboard(const RegisteredBillboard &billboard) {
    ++_submission.billboards;
    InstanceMaterial material;
    material.diffuseColor = billboard.color;
    material.mainTex = _renderer.resources().textureId(billboard.texture.get()).value_or(UINT32_MAX);
    // Registered billboards are lens flares: raster draws them additively with
    // no depth test. They are nevertheless represented in the BLAS so rays
    // can see their emitted contribution, while remaining non-occluding.
    material.surfaceType = 1;
    return {{material, GpuScene::PrimitiveClass::NonOpaque,
             GpuScene::ResidencyClass::Dynamic, nullptr}};
}

GpuSceneUpload RayQueryPipeline::prepare(const glm::mat4 &view,
                                         const ModelSceneNode *skyRoom,
                                         bool skyBaked) {
    return _gpuScene.prepare(
        [this, skyRoom, skyBaked](const RegisteredMesh &mesh) {
            return classifyMesh(mesh, skyRoom, skyBaked);
        },
        [this](const RegisteredGrass &grass) { return classifyGrass(grass); },
        [this](const RegisteredParticles &particles) { return classifyParticles(particles); },
        [this](const RegisteredBillboard &billboard) { return classifyBillboard(billboard); },
        view);
}

GpuSceneUpload RayQueryPipeline::prepareRaster(const glm::mat4 &view) {
    _submission = {};
    // Raster keeps the background room as ordinary opaque geometry. Only the
    // traced path selects and bakes a sky room before entering this shared
    // classifier/texture-registration implementation.
    auto upload = prepare(view, nullptr, false);
    // GpuScene reserves source strides 0 and 1 for expanded procedural quads;
    // ordinary meshes carry their byte stride. Annotate only this raster copy
    // of the material table so the shared traced upload remains byte-identical.
    for (const auto &object : upload.objects) {
        if (object.data.srcVertexStride <= 1) {
            upload.materials[object.data.materialIndex].featureMask |=
                kGpuSceneFeatureProcedural;
        }
    }
    return upload;
}

void RayQueryPipeline::render(const VulkanPrimaryRayContext &context) {
    _submission = {};

    // There is exactly one sky: the background-scenery room whose union
    // bounds overlap the scene on all axes. This remains scene policy; the
    // native half only knows how to bake the selected meshes.
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

    const ModelSceneNode *skyRoom = nullptr;
    glm::vec3 skyOrigin(0.0f);
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
                skyRoom = room;
                skyOrigin = 0.5f * (candidate.boundsMin + candidate.boundsMax);
            }
        }
        if (skyRoom && _frameNumber == 0) {
            info("Vulkan: sky room is '" + skyRoom->model().name() + "'", LogChannel::Graphics);
        }
    }

    _gpuScene.setSkyRoom(skyRoom);
    bool skyBaked = false;
    if (skyRoom) {
        RayQuerySkyRoom bake;
        bake.identity = reinterpret_cast<uint64_t>(skyRoom);
        bake.name = skyRoom->model().name();
        bake.origin = skyOrigin;
        bool valid = true;
        for (const auto &object : _gpuScene.objects()) {
            const auto *mesh = std::get_if<RegisteredMesh>(&object);
            if (!mesh || mesh->cullRoot != skyRoom || !_gpuScene.isObjectEnabled(mesh->id.index))
                continue;
            if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                     renderCategory(RenderCategory::Transparent))) == 0)
                continue;
            if (!std::holds_alternative<std::monostate>(mesh->deformation)) {
                valid = false;
                break;
            }
            const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)];
            if (!texture || !_native->supportsSkyTexture(*texture)) {
                valid = false;
                break;
            }
            bake.meshes.push_back({&mesh->mesh.get(), texture, mesh->transform,
                                   mesh->transformInv, mesh->prevTransform,
                                   mesh->material.uv});
        }
        if (!valid)
            bake.meshes.clear();
        try {
            skyBaked = _native->bakeSkyRoom(context.commandBuffer, bake);
        } catch (const std::exception &e) {
            warn("Vulkan: sky bake failed for '" + skyRoom->model().name() + "': " + e.what() +
                     "; keeping sky geometry",
                 LogChannel::Graphics);
        }
    } else {
        _native->clearSkyRoom();
    }

    _submission.upload = prepare(context.view, skyRoom, skyBaked);
    _native->render(context.commandBuffer, context.globalsOffset, *context.output,
                    context.view, context.projection, context.jitter,
                    std::move(_submission), _deviceGpuScene, skyBaked);
    ++_frameNumber;
}

} // namespace reone::scene
