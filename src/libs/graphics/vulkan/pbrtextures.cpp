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

#include "reone/graphics/vulkan/pbrtextures.h"

#include "reone/graphics/textureutil.h"

#include "reone/graphics/texture.h"
#include "reone/graphics/types.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/debugscope.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/renderpass.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/pipeline.h"
#include "reone/graphics/vulkan/pipelinecache.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/uniformring.h"
#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

// Matching the OpenGL implementation exactly, so the two produce comparable
// results rather than merely similar ones.
static constexpr int kBRDFSize = 512;
static constexpr int kIrradianceSize = 32;
static constexpr int kPrefilteredSize = 128;
static constexpr int kNumPrefilteredMips = 5;
static constexpr int kMaxDerivedLayers = 16;

static constexpr char kModule[] = "pbr_ibl";
/** All six faces at once; see the view mask in renderCubeFaces. */
static constexpr uint32_t kCubeViewMask = (1u << kNumCubeFaces) - 1u;

void VulkanPBRTextures::init() {
    if (_inited) {
        return;
    }
    _brdf = std::make_unique<VulkanImage>(_device);
    _brdf->initColorAttachment({kBRDFSize, kBRDFSize}, VK_FORMAT_R16G16_SFLOAT);

    _irradiance = std::make_unique<VulkanImage>(_device);
    _irradiance->initCubeArrayAttachment({kIrradianceSize, kIrradianceSize},
                                         VK_FORMAT_R8G8B8A8_UNORM,
                                         kMaxDerivedLayers, 1);

    _prefiltered = std::make_unique<VulkanImage>(_device);
    _prefiltered->initCubeArrayAttachment({kPrefilteredSize, kPrefilteredSize},
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          kMaxDerivedLayers, kNumPrefilteredMips);

    _device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                          reinterpret_cast<uint64_t>(_brdf->handle()), "BRDF LUT");
    _device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                          reinterpret_cast<uint64_t>(_irradiance->handle()), "Irradiance maps");
    _device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                          reinterpret_cast<uint64_t>(_prefiltered->handle()),
                          "Prefiltered environment maps");

    // Everything starts sampleable, because the resolve reads all three whether
    // or not anything has been generated into them yet.
    _device.immediateSubmit([this](VkCommandBuffer cmd) {
        auto toShaderRead = [&](VulkanImage &image) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.image = image.handle();
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        toShaderRead(*_brdf);
        toShaderRead(*_irradiance);
        toShaderRead(*_prefiltered);
    });

    // The BRDF table is indexed by NdotV and roughness, both of which reach the
    // edge of their domain - a grazing angle sits at NdotV zero and roughness
    // clamps to one. Sampled with the global repeating sampler those wrap round
    // to the opposite edge, which is exactly where a metallic surface reads.
    auto &samplers = _resources.samplers();
    auto clamped = getTextureProperties(TextureUsage::ColorBuffer);
    _brdf->setSampler(samplers.get(clamped));
    _irradiance->setSampler(samplers.get(clamped));

    // The prefiltered map is the exception: roughness selects a mip, so it
    // needs the chain that ColorBuffer's non-mipmapped filter would clamp away.
    auto prefilteredProps = clamped;
    prefilteredProps.minFilter = Texture::Filtering::LinearMipmapLinear;
    _prefiltered->setSampler(samplers.get(prefilteredProps));


    _inited = true;
}

void VulkanPBRTextures::deinit() {
    _brdf.reset();
    _irradiance.reset();
    _prefiltered.reset();
    _requests.clear();
    _envMapToLayer.clear();
    _envMapSources.clear();
    // Reset the generation state too, so a re-init starts from nothing rather
    // than believing a BRDF table it no longer owns is still there.
    _brdfGenerated = false;
    _nextLayer = 0;
    _inited = false;
}

Texture &VulkanPBRTextures::brdf() {
    throw std::logic_error("Vulkan PBR textures are not exposed as a Texture");
}

void VulkanPBRTextures::refresh() {
    _requests.clear();
    _envMapToLayer.clear();
    _envMapSources.clear();
    _nextLayer = 0;
}

int VulkanPBRTextures::requestEnvMapDerivedLayer(Texture &envMap) {
    if (auto existing = findEnvMapDerivedLayer(envMap.name())) {
        return *existing;
    }
    if (_nextLayer >= kMaxDerivedLayers) {
        warn("Vulkan: more than " + std::to_string(kMaxDerivedLayers) +
                 " environment maps requested; using derived layer 0 for " +
                 envMap.name(),
             LogChannel::Graphics);
        return 0;
    }

    const int layer = _nextLayer++;
    _envMapToLayer.emplace(envMap.name(), layer);
    _requests.insert({envMap});
    return layer;
}

/**
 * Move a whole image between being rendered into and being sampled.
 *
 * Conservative masks on both sides: this runs at most once per frame, and the
 * cost of being exact here is not worth the risk of being subtly wrong.
 */
static void transition(VkCommandBuffer cmd, VulkanImage &image,
                       VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image.handle();
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void VulkanPBRTextures::process(VkCommandBuffer cmd, uint32_t globalsOffset) {
    if (!_inited) {
        return;
    }
    if (!_brdfGenerated) {
        VulkanDebugScope scope(_device, cmd, "IBL: BRDF integration", {0.5f, 0.3f, 0.6f});
        transition(cmd, *_brdf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        generateBRDF(cmd, globalsOffset);
        transition(cmd, *_brdf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        _brdfGenerated = true;
        return;
    }
    if (_requests.empty()) {
        return;
    }
    // One per frame. Each is a thousand-sample convolution over six faces and
    // five mips; doing the lot in one frame would stall visibly.
    auto request = *_requests.begin();
    _requests.erase(_requests.begin());

    auto &envMap = request.texture;
    const int layer = _envMapToLayer.at(envMap.name());

    VulkanDebugScope scope(_device, cmd, "IBL: derive environment map", {0.5f, 0.3f, 0.6f});
    transition(cmd, *_irradiance, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, *_prefiltered, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    generateDerived(cmd, globalsOffset, envMap, layer);
    transition(cmd, *_irradiance, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, *_prefiltered, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    _envMapSources[layer] = &envMap;
    debug("Vulkan: derived environment map " + envMap.name() + " into layer " +
              std::to_string(layer),
          LogChannel::Graphics);
}

void VulkanPBRTextures::generateBRDF(VkCommandBuffer cmd, uint32_t globalsOffset) {
    VulkanPipelineCache::Key key;
    key.module = kModule;
    key.vertexEntry = "iblVertex";
    key.fragmentEntry = "brdfFragment";
    key.colorFormats = {VK_FORMAT_R16G16_SFLOAT};
    auto &pipeline = _pipelines.get(key);

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    RenderPassScope rendering(
        cmd, {kBRDFSize, kBRDFSize},
        {{_brdf->view(),
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_ATTACHMENT_LOAD_OP_CLEAR,
          VK_ATTACHMENT_STORE_OP_STORE}});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    auto uniformSet = _descriptors.uniformSet(_ring.frame());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    auto textureSet = _descriptors.acquireTextureSet(_ring.frame(), nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &textureSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VulkanPBRTextures::generateDerived(VkCommandBuffer cmd, uint32_t globalsOffset,
                                        Texture &envMap, int layer) {
    renderCubeFaces(cmd, globalsOffset, *_irradiance, layer, 0,
                    {kIrradianceSize, kIrradianceSize},
                    "irradianceFragment", &envMap, 0.0f);

    for (int mip = 0; mip < kNumPrefilteredMips; ++mip) {
        int size = static_cast<int>(kPrefilteredSize * std::pow(0.5, mip));
        float roughness = mip / static_cast<float>(kNumPrefilteredMips - 1);
        renderCubeFaces(cmd, globalsOffset, *_prefiltered, layer, mip,
                        {size, size}, "prefilterFragment", &envMap, roughness);
    }
}

void VulkanPBRTextures::renderCubeFaces(VkCommandBuffer cmd,
                                        uint32_t globalsOffset,
                                        VulkanImage &target,
                                        int cube,
                                        int mip,
                                        glm::ivec2 extent,
                                        const char *fragmentEntry,
                                        const Texture *envMap,
                                        float roughness) {
    // Six views, one per face. The vertex stage reads SV_ViewID and the fragment
    // stage turns it into a direction, so one draw fills the whole cube.
    VulkanPipelineCache::Key key;
    key.module = kModule;
    key.vertexEntry = "iblVertex";
    key.fragmentEntry = fragmentEntry;
    key.colorFormats = {target.format()};
    key.viewMask = kCubeViewMask;
    auto &pipeline = _pipelines.get(key);

    LocalUniforms locals;
    locals.reset();
    locals.iblRoughness = roughness;
    if (envMap && envMap->isCubeMap()) {
        locals.featureMask |= UniformsFeatureFlags::envmapcube;
    }

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);

    std::vector<std::pair<int, const VulkanImage *>> textures;
    if (envMap) {
        auto unit = envMap->isCubeMap() ? TextureUnits::envMapCube : TextureUnits::envMap;
        textures.push_back({unit, &_resources.get(*envMap)});
    }

    RenderPassScope rendering(
        cmd, extent,
        {{target.renderView(cube, mip),
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_ATTACHMENT_LOAD_OP_CLEAR,
          VK_ATTACHMENT_STORE_OP_STORE}},
        std::nullopt, kCubeViewMask);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    auto uniformSet = _descriptors.uniformSet(_ring.frame());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    auto textureSet = _descriptors.acquireTextureSet(_ring.frame(), textures);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &textureSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace graphics

} // namespace reone
