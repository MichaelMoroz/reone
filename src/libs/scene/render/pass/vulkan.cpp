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
#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/uniformring.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

static constexpr char kModelModule[] = "pbr_model";
static constexpr char kOpaqueFragment[] = "opaqueFragment";

void VulkanRenderPass::warnOnce(const std::string &what) {
    if (_warned.count(what) > 0) {
        return;
    }
    _warned.insert(what);
    warn("Vulkan scene pass: " + what + " is not implemented; skipping",
         LogChannel::Graphics);
}

int VulkanRenderPass::materialFeatureMask(const Material &material) const {
    // Mirrors PBRRenderPass::materialFeatureMask. The two must agree, because
    // they feed the same fragment shader.
    int mask = 0;
    const auto &textures = material.textures;
    if (textures.count(TextureUnits::mainTex) > 0) {
        const auto &mainTex = textures.at(TextureUnits::mainTex).get();
        switch (mainTex.features().blending) {
        case Texture::Blending::PunchThrough:
            mask |= UniformsFeatureFlags::hashedalphatest;
            break;
        case Texture::Blending::Additive:
            if (textures.count(TextureUnits::envMap) == 0 &&
                textures.count(TextureUnits::envMapCube) == 0) {
                mask |= UniformsFeatureFlags::premulalpha;
            }
            break;
        default:
            break;
        }
        if (mainTex.features().waterAlpha != -1.0f) {
            mask |= UniformsFeatureFlags::water;
        }
    }
    if (textures.count(TextureUnits::lightmap) > 0) {
        mask |= UniformsFeatureFlags::lightmap;
    }
    if (textures.count(TextureUnits::envMap) > 0) {
        mask |= UniformsFeatureFlags::envmap;
    }
    if (textures.count(TextureUnits::envMapCube) > 0) {
        mask |= UniformsFeatureFlags::envmap | UniformsFeatureFlags::envmapcube;
    }
    if (textures.count(TextureUnits::normalMap) > 0) {
        mask |= UniformsFeatureFlags::normalmap;
    }
    if (textures.count(TextureUnits::bumpMapArray) > 0) {
        mask |= UniformsFeatureFlags::bumpmap;
    }
    if (material.affectedByShadows) {
        mask |= UniformsFeatureFlags::shadows;
    }
    if (material.affectedByFog) {
        mask |= UniformsFeatureFlags::fog;
    }
    return mask;
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
    locals.featureMask |= materialFeatureMask(material) | extraFeatureBits;
    locals.uv = material.uv;
    locals.color = material.color;
    locals.ambientColor = glm::vec4 {material.ambientColor, 0.0f};
    locals.diffuseColor = glm::vec4 {material.diffuseColor, 0.0f};
    locals.selfIllumColor = glm::vec4 {material.selfIllumColor, 1.0f};

    if (material.textures.count(TextureUnits::mainTex) > 0) {
        const auto &mainTex = material.textures.at(TextureUnits::mainTex).get();
        if (mainTex.features().waterAlpha != -1.0f) {
            locals.waterAlpha = mainTex.features().waterAlpha;
        }
    }
    if (material.textures.count(TextureUnits::bumpMapArray) > 0) {
        const auto &bumpmap = material.textures.at(TextureUnits::bumpMapArray).get();
        locals.bumpMapFrame = material.bumpMapFrame;
        locals.bumpMapScale = bumpmap.features().bumpMapScaling;
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

    VulkanPipelineCache::Key key;
    key.module = kModelModule;
    key.vertexEntry = vertexEntry;
    key.fragmentEntry = kOpaqueFragment;
    key.colorFormats = _colorFormats;
    key.depthFormat = _depthFormat;
    key.depthTest = true;
    key.depthWrite = true;
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
    for (const auto &extra : extraOffsets) {
        offsets[extra.first] = extra.second;
    }

    // The material's texture units, uploaded on first use.
    std::vector<std::pair<int, const VulkanImage *>> bindings;
    bindings.reserve(material.textures.size());
    for (const auto &entry : material.textures) {
        bindings.push_back({entry.first, &_resources.get(entry.second.get())});
    }

    vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    auto uniformSet = _descriptors.uniformSet(_ring.frame());
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());

    auto textureSet = _descriptors.acquireTextureSet(_ring.frame(), bindings);
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &textureSet, 0, nullptr);

    vkMesh.draw(_cmd, _resources.zeroBuffer());
    ++_drawCount;
}

void VulkanRenderPass::draw(Mesh &mesh,
                            Material &material,
                            const glm::mat4 &transform,
                            const glm::mat4 &transformInv,
                            const glm::mat4 &prevTransform) {
    drawGeometry(mesh, material, "staticVertex",
                 transform, transformInv, prevTransform, 0, {});
}

void VulkanRenderPass::drawSkinned(Mesh &mesh,
                                   Material &material,
                                   const glm::mat4 &transform,
                                   const glm::mat4 &transformInv,
                                   const glm::mat4 &prevTransform,
                                   const std::vector<glm::mat4> &bones,
                                   const std::vector<glm::mat4> &prevBones) {
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

void VulkanRenderPass::drawDangly(Mesh &mesh,
                                  Material &material,
                                  const glm::mat4 &transform,
                                  const glm::mat4 &transformInv,
                                  const glm::mat4 &prevTransform,
                                  const std::vector<glm::vec4> &positions) {
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

void VulkanRenderPass::drawSaber(Mesh &mesh,
                                 Material &material,
                                 const glm::mat4 &transform,
                                 const glm::mat4 &transformInv,
                                 const glm::mat4 &prevTransform,
                                 const glm::vec4 &displacement) {
    // The displacement rides in LocalUniforms rather than a block of its own.
    drawGeometry(mesh, material, "saberVertex",
                 transform, transformInv, prevTransform,
                 UniformsFeatureFlags::saber, {}, displacement);
}

void VulkanRenderPass::drawBillboard(Texture &texture,
                                     const glm::vec4 &color,
                                     const glm::mat4 &transform,
                                     const glm::mat4 &transformInv,
                                     std::optional<float> size) {
    warnOnce("billboards");
}

void VulkanRenderPass::drawParticles(Texture &texture,
                                     FaceCullMode faceCulling,
                                     bool premultipliedAlpha,
                                     const glm::ivec2 &gridSize,
                                     const std::vector<ParticleInstance> &particles) {
    warnOnce("particles");
}

void VulkanRenderPass::drawGrass(float radius,
                                 float quadSize,
                                 Texture &texture,
                                 std::optional<std::reference_wrapper<Texture>> &lightmap,
                                 const std::vector<GrassInstance> &instances) {
    warnOnce("grass");
}

void VulkanRenderPass::drawAABB(const std::vector<glm::vec4> &corners) {
    warnOnce("AABBs");
}

} // namespace scene

} // namespace reone
