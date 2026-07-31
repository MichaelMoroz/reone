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

#include "reone/scene/render/pass/vulkan.h"

#include "reone/graphics/material.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/pbrtextures.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/uniformring.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

static constexpr char kModelModule[] = "pbr_model";
static constexpr char kOpaqueFragment[] = "opaqueFragment";
static constexpr char kRetroOpaqueFragment[] = "retroOpaqueFragment";
static constexpr char kTransparentFragment[] = "transparentFragment";
static constexpr char kOITModelFragment[] = "oitModelFragment";
static constexpr char kGrassModule[] = "grass";
static constexpr char kParticleModule[] = "particles";
static constexpr char kCommonModule[] = "common";
static constexpr char kShadowModule[] = "shadow";
static constexpr char kWalkmeshModule[] = "walkmesh";
/** Both grass and walkmesh name their G-buffer fragment stage this. */
static constexpr char kPBRFragment[] = "pbrFragment";

void VulkanRenderPass::warnOnce(const std::string &what) {
    if (_warned.count(what) > 0) {
        return;
    }
    _warned.insert(what);
    warn("Vulkan scene pass: " + what + " is not implemented; skipping",
         LogChannel::Graphics);
}

bool VulkanRenderPass::isShadowPass() const {
    return _pass == RenderPassName::DirLightShadowsPass ||
           _pass == RenderPassName::PointLightShadows;
}

void VulkanRenderPass::fillLocals(LocalUniforms &locals,
                                  const Material &material,
                                  const glm::mat4 &transform,
                                  const glm::mat4 &transformInv,
                                  const glm::mat4 &prevTransform,
                                  int extraFeatureBits,
                                  std::optional<glm::vec4> saberDisplacement) const {
    locals.reset();
    locals.model = transform;
    locals.modelInv = transformInv;
    locals.prevModel = prevTransform;
    locals.featureMask |= graphics::materialFeatureMask(material) | extraFeatureBits;
    auto *envMap = material.textures[static_cast<size_t>(MaterialTextureSlot::EnvMapCube)];
    if (!envMap) {
        envMap = material.textures[static_cast<size_t>(MaterialTextureSlot::EnvMap)];
    }
    if (envMap && _options.pbr) {
        // The resolve samples a convolved copy, not the source environment map, so the first
        // sighting only asks for one and settles for layer zero until it exists.
        auto layer = _pbrTextures.findEnvMapDerivedLayer(envMap->name());
        if (layer) {
            locals.envMapDerivedLayer = *layer;
        } else {
            locals.envMapDerivedLayer = 0;
            _pbrTextures.requestEnvMapDerived({*envMap});
        }
    }
    locals.uv = material.uv;
    locals.color = material.color;
    locals.ambientColor = glm::vec4 {material.ambientColor, 0.0f};
    locals.diffuseColor = glm::vec4 {material.diffuseColor, 0.0f};
    locals.selfIllumColor = glm::vec4 {material.selfIllumColor, 1.0f};

    if (const auto *mainTex = material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        if (mainTex->features().waterAlpha != -1.0f) {
            locals.waterAlpha = mainTex->features().waterAlpha;
        }
    }
    if (const auto *bumpmap = material.textures[static_cast<size_t>(MaterialTextureSlot::BumpMapArray)]) {
        locals.bumpMapFrame = material.bumpMapFrame;
        locals.bumpMapScale = bumpmap->features().bumpMapScaling;
    }
    if (saberDisplacement) {
        locals.saberDisplacement = *saberDisplacement;
    }
}

void VulkanRenderPass::drawGeometry(Mesh &mesh,
                                    Material &material,
                                    const char *vertexEntry,
                                    const glm::mat4 &transform,
                                    const glm::mat4 &transformInv,
                                    const glm::mat4 &prevTransform,
                                    int extraFeatureBits,
                                    const std::vector<std::pair<int, uint32_t>> &extraOffsets,
                                    std::optional<glm::vec4> saberDisplacement) {
    const auto &vkMesh = _resources.get(mesh);

    // A shadow draw writes depth from one shared vertex stage and nothing else,
    // so it ignores the material entirely.
    if (isShadowPass()) {
        drawShadow(mesh, material, transform);
        return;
    }

    // Walkmeshes are debug geometry with their own tiny shader; everything else
    // in this pass is a model.
    bool walkmesh = material.type == MaterialType::Walkmesh;

    const char *modelFragment = _kind == Kind::Retro ? kRetroOpaqueFragment : kOpaqueFragment;
    switch (_kind) {
    case Kind::OIT:
        modelFragment = kOITModelFragment;
        break;
    case Kind::Forward:
        modelFragment = kTransparentFragment;
        break;
    default:
        break;
    }

    VulkanPipelineCache::Key key;
    key.module = walkmesh ? kWalkmeshModule : kModelModule;
    key.vertexEntry = walkmesh ? "walkmeshVertex" : vertexEntry;
    key.fragmentEntry = walkmesh ? (_kind == Kind::Retro ? "retroFragment" : kPBRFragment) : modelFragment;
    key.colorFormats = _colorFormats;
    key.depthFormat = _depthFormat;
    key.depthTest = true;
    // Transparent surfaces are shaded forward, and must not write depth: one
    // would otherwise hide the surface behind it instead of showing through
    // to it.
    key.depthWrite = _kind == Kind::Geometry || _kind == Kind::Retro;
    if (_kind == Kind::OIT) {
        // One blend state for the whole pass, as the OpenGL pipeline has -
        // beginTransparentGeometryPass pushes it over whatever the material
        // asked for.
        key.blend = BlendMode::OIT_Transparent;
    } else if (_kind == Kind::Forward) {
        key.blend = material.blending.value_or(BlendMode::Normal);
    }
    key.cull = material.faceCulling.value_or(FaceCullMode::Back);
    key.vertexBindings = VulkanMesh::bindingDescriptions(mesh.vertexLayout());
    key.vertexAttributes = VulkanMesh::attributeDescriptions(mesh.vertexLayout());
    auto &pipeline = _pipelines.get(key);

    LocalUniforms locals;
    fillLocals(locals, material, transform, transformInv, prevTransform, extraFeatureBits,
               saberDisplacement);

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = _globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);
    if (walkmesh) {
        // Without this the offset stays zero and the shader indexes whatever
        // happens to sit at the start of the arena, which is the globals block.
        // The surfaces still draw, in arbitrary colours.
        offsets[UniformBlockBindingPoints::walkmesh] = walkmeshOffset();
    }
    for (const auto &extra : extraOffsets) {
        offsets[extra.first] = extra.second;
    }

    // The material's texture units, uploaded on first use.
    std::vector<std::pair<int, const VulkanImage *>> bindings;
    bindings.reserve(material.textures.size());
    for (size_t i = 0; i < material.textures.size(); ++i) {
        if (auto *texture = material.textures[i]) {
            bindings.push_back(
                {materialTextureUnit(static_cast<MaterialTextureSlot>(i)), &_resources.get(*texture)});
        }
    }

    bindAndDraw(pipeline, offsets, bindings, vkMesh, 1);
}

uint32_t VulkanRenderPass::walkmeshOffset() {
    if (!_walkmeshOffset) {
        _walkmeshOffset = _ring.push(_uniforms.walkmesh());
    }
    return *_walkmeshOffset;
}

void VulkanRenderPass::drawShadow(Mesh &mesh, Material &material, const glm::mat4 &transform) {
    const auto &vkMesh = _resources.get(mesh);

    VulkanPipelineCache::Key key;
    key.module = kShadowModule;
    key.vertexEntry = "shadowVertex";
    // A directional cascade needs no fragment stage; a point light writes radial
    // distance instead of projected depth.
    key.fragmentEntry = _pass == RenderPassName::DirLightShadowsPass
                            ? "nullFragment"
                            : "pointFragment";
    key.depthFormat = _depthFormat;
    key.viewMask = _shadowViewMask;
    key.depthTest = true;
    key.depthWrite = true;
    // Front faces, not back: shadow acne comes from the surface shadowing
    // itself, and casting from the far side of the geometry moves the error
    // behind whatever it is that receives the shadow.
    key.cull = FaceCullMode::Front;
    key.vertexBindings = VulkanMesh::bindingDescriptions(mesh.vertexLayout());
    key.vertexAttributes = VulkanMesh::attributeDescriptions(mesh.vertexLayout());
    auto &pipeline = _pipelines.get(key);

    LocalUniforms locals;
    locals.reset();
    locals.model = transform;
    locals.color = material.color;

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = _globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);

    bindAndDraw(pipeline, offsets, {}, vkMesh, 1);
}

void VulkanRenderPass::bindAndDraw(
    const VulkanPipeline &pipeline,
    const std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> &offsets,
    const std::vector<std::pair<int, const VulkanImage *>> &textures,
    const VulkanMesh &mesh,
    int instances) {
    vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    auto uniformSet = _descriptors.uniformSet(_ring.frame());
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());

    auto textureSet = _descriptors.acquireTextureSet(_ring.frame(), textures);
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &textureSet, 0, nullptr);

    mesh.draw(_cmd, _resources.zeroBuffer(), instances);
    ++_drawCount;
}

void VulkanRenderPass::executeDraw(Mesh &mesh,
                            Material &material,
                            const glm::mat4 &transform,
                            const glm::mat4 &transformInv,
                            const glm::mat4 &prevTransform) {
    drawGeometry(mesh, material, "staticVertex",
                 transform, transformInv, prevTransform, 0, {});
}

void VulkanRenderPass::executeDrawSkinned(Mesh &mesh,
                                   Material &material,
                                   const glm::mat4 &transform,
                                   const glm::mat4 &transformInv,
                                   const glm::mat4 &prevTransform,
                                   const std::vector<glm::mat4> &bones,
                                   const std::vector<glm::mat4> &prevBones) {
    if (isShadowPass()) {
        executeDraw(mesh, material, transform, transformInv, prevTransform);
        return;
    }
    BoneUniforms uniforms;
    for (size_t i = 0; i < bones.size() && i < kMaxBones; ++i) {
        uniforms.bones[i] = bones[i];
    }
    for (size_t i = 0; i < prevBones.size() && i < kMaxBones; ++i) {
        uniforms.prevBones[i] = prevBones[i];
    }
    auto offset = _ring.push(uniforms);
    drawGeometry(mesh, material, "skinnedVertex",
                 transform, transformInv, prevTransform,
                 UniformsFeatureFlags::skin,
                 {{UniformBlockBindingPoints::bones, offset}});
}

void VulkanRenderPass::executeDrawDangly(Mesh &mesh,
                                  Material &material,
                                  const glm::mat4 &transform,
                                  const glm::mat4 &transformInv,
                                  const glm::mat4 &prevTransform,
                                  const std::vector<glm::vec4> &positions) {
    if (isShadowPass()) {
        executeDraw(mesh, material, transform, transformInv, prevTransform);
        return;
    }
    DanglyUniforms uniforms;
    for (size_t i = 0; i < positions.size() && i < kMaxDanglyVertices; ++i) {
        uniforms.positions[i] = positions[i];
    }
    auto offset = _ring.push(uniforms);
    drawGeometry(mesh, material, "danglyVertex",
                 transform, transformInv, prevTransform,
                 UniformsFeatureFlags::dangly,
                 {{UniformBlockBindingPoints::dangly, offset}});
}

void VulkanRenderPass::executeDrawSaber(Mesh &mesh,
                                 Material &material,
                                 const glm::mat4 &transform,
                                 const glm::mat4 &transformInv,
                                 const glm::mat4 &prevTransform,
                                 const glm::vec4 &displacement) {
    if (isShadowPass()) {
        executeDraw(mesh, material, transform, transformInv, prevTransform);
        return;
    }
    // The displacement rides in LocalUniforms rather than a block of its own.
    drawGeometry(mesh, material, "saberVertex",
                 transform, transformInv, prevTransform,
                 UniformsFeatureFlags::saber, {}, displacement);
}

void VulkanRenderPass::executeDrawBillboard(Texture &texture,
                                     const glm::vec4 &color,
                                     const glm::mat4 &transform,
                                     const glm::mat4 &transformInv,
                                     std::optional<float> size) {
    const auto &quad = _resources.get(_meshRegistry.get(MeshName::billboard));

    VulkanPipelineCache::Key key;
    key.module = kCommonModule;
    key.vertexEntry = "billboardVertex";
    key.fragmentEntry = "textureFragment";
    key.colorFormats = _colorFormats;
    key.depthFormat = _depthFormat;
    // Lens flares are meant to be seen through whatever is in front of them -
    // the caller has already decided the light is visible by tracing to it.
    key.depthTest = false;
    key.depthWrite = false;
    key.blend = BlendMode::Additive;
    key.cull = FaceCullMode::None;
    key.vertexBindings = VulkanMesh::bindingDescriptions(
        _meshRegistry.get(MeshName::billboard).vertexLayout());
    key.vertexAttributes = VulkanMesh::attributeDescriptions(
        _meshRegistry.get(MeshName::billboard).vertexLayout());
    auto &pipeline = _pipelines.get(key);

    LocalUniforms locals;
    locals.reset();
    locals.model = transform;
    locals.modelInv = transformInv;
    locals.color = color;
    if (size) {
        locals.featureMask |= UniformsFeatureFlags::fixedsize;
        locals.billboardSize = *size;
    }

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = _globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);

    bindAndDraw(pipeline, offsets,
                {{TextureUnits::mainTex, &_resources.get(texture)}},
                quad, 1);
}

void VulkanRenderPass::executeDrawParticles(Material &material,
                                     const glm::ivec2 &gridSize,
                                     const std::vector<ParticleInstance> &particles) {
    if (particles.empty()) {
        return;
    }
    auto &texture = *material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)];
    auto faceCulling = material.faceCulling.value_or(FaceCullMode::Back);
    bool premultipliedAlpha = material.blending == BlendMode::Lighten;
    // One instanced billboard per particle, oriented in the vertex shader from
    // the axes the emitter computed. This runs in the transparency pass, so it
    // accumulates into the OIT targets rather than writing the G-buffer.
    const auto &billboard = _resources.get(_meshRegistry.get(MeshName::billboard));

    bool oit = _kind == Kind::OIT;

    VulkanPipelineCache::Key key;
    key.module = kParticleModule;
    key.vertexEntry = "particleVertex";
    key.fragmentEntry = oit ? "oitParticleFragment" : "particleFragment";
    key.colorFormats = _colorFormats;
    key.depthFormat = _depthFormat;
    key.depthTest = true;
    // No depth write: particles are translucent, and one occluding the next
    // would punch a hole in the puff behind it.
    key.depthWrite = false;
    key.blend = oit ? BlendMode::OIT_Transparent : BlendMode::Normal;
    key.cull = faceCulling;
    key.vertexBindings = VulkanMesh::bindingDescriptions(
        _meshRegistry.get(MeshName::billboard).vertexLayout());
    key.vertexAttributes = VulkanMesh::attributeDescriptions(
        _meshRegistry.get(MeshName::billboard).vertexLayout());
    auto &pipeline = _pipelines.get(key);

    LocalUniforms locals;
    locals.reset();
    if (premultipliedAlpha) {
        locals.featureMask |= UniformsFeatureFlags::premulalpha;
    }

    ParticleUniforms particleUniforms;
    // A zero grid would divide by zero in the shader's frame lookup. Emitters
    // without an animation grid legitimately report one.
    particleUniforms.gridSize = glm::max(gridSize, glm::ivec2(1));
    auto count = std::min(particles.size(), static_cast<size_t>(kMaxParticles));
    for (size_t i = 0; i < count; ++i) {
        const auto &particle = particles[i];
        auto &dst = particleUniforms.particles[i];
        dst.positionFrame = glm::vec4(particle.position, static_cast<float>(particle.frame));
        dst.right = glm::vec4(particle.right, 0.0f);
        dst.up = glm::vec4(particle.up, 0.0f);
        dst.color = particle.color;
        dst.size = particle.size;
    }

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = _globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);
    offsets[UniformBlockBindingPoints::particles] = _ring.push(particleUniforms);

    bindAndDraw(pipeline, offsets,
                {{TextureUnits::mainTex, &_resources.get(texture)}},
                billboard, static_cast<int>(count));
}

void VulkanRenderPass::executeDrawGrass(float radius,
                                 float quadSize,
                                 Material &material,
                                 const std::vector<GrassInstance> &instances) {
    if (instances.empty()) {
        return;
    }
    auto &texture = *material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)];
    auto *lightmap = material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)];
    bool hasLightmap = lightmap;
    // One instanced, world-upright quad per cluster. The uniform carries the
    // deterministic per-cluster yaw shared with the traced merged quad.
    const auto &quad = _resources.get(_meshRegistry.get(MeshName::grass));

    VulkanPipelineCache::Key key;
    key.module = kGrassModule;
    key.vertexEntry = "grassVertex";
    key.fragmentEntry = _kind == Kind::Retro ? "retroFragment" : kPBRFragment;
    key.colorFormats = _colorFormats;
    key.depthFormat = _depthFormat;
    key.depthTest = true;
    key.depthWrite = true;
    key.cull = FaceCullMode::None;
    key.vertexBindings = VulkanMesh::bindingDescriptions(
        _meshRegistry.get(MeshName::grass).vertexLayout());
    key.vertexAttributes = VulkanMesh::attributeDescriptions(
        _meshRegistry.get(MeshName::grass).vertexLayout());
    auto &pipeline = _pipelines.get(key);

    LocalUniforms locals;
    locals.reset();
    locals.featureMask |= UniformsFeatureFlags::hashedalphatest;
    if (hasLightmap) {
        locals.featureMask |= UniformsFeatureFlags::lightmap;
    }

    GrassUniforms grass;
    grass.radius = radius;
    grass.quadSize = glm::vec2(quadSize);
    auto count = std::min(instances.size(), static_cast<size_t>(kMaxGrassClusters));
    for (size_t i = 0; i < count; ++i) {
        grass.clusters[i].positionVariant =
            glm::vec4(instances[i].position, static_cast<float>(instances[i].variant));
        grass.clusters[i].lightmapUV = instances[i].lightmapUV;
        grass.clusters[i].yaw = instances[i].yaw;
    }

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = _globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);
    offsets[UniformBlockBindingPoints::grass] = _ring.push(grass);

    std::vector<std::pair<int, const VulkanImage *>> bindings {
        {TextureUnits::mainTex, &_resources.get(texture)}};
    if (hasLightmap) {
        bindings.push_back(
            {TextureUnits::lightmap, &_resources.get(*lightmap)});
    }

    bindAndDraw(pipeline, offsets, bindings, quad, static_cast<int>(count));
}

void VulkanRenderPass::executeDrawAABB(const std::vector<glm::vec4> &corners) {
    warnOnce("AABBs");
}

void VulkanRenderPass::executeDrawDebug(const std::function<void()> &execute) {
    warnOnce("draw debug");
}

} // namespace scene

} // namespace reone
