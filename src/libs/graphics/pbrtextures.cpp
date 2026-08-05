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

#include "reone/graphics/pbrtextures.h"

#include "reone/graphics/textureutil.h"

#include "reone/graphics/texture.h"
#include "reone/graphics/types.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/debugscope.h"
#include "reone/graphics/vulkan/commandbuffer.h"
#include "reone/graphics/vulkan/renderpass.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/renderer.h"
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

void PBRTextures::init() {
    if (_inited) {
        return;
    }
    _brdf = makeImage(_device);
    _brdf->initColorAttachment({kBRDFSize, kBRDFSize}, Format::R16G16Sfloat);

    _irradiance = makeImage(_device);
    _irradiance->initCubeArrayAttachment({kIrradianceSize, kIrradianceSize},
                                         Format::R8G8B8A8Unorm,
                                         kMaxDerivedLayers, 1);

    _prefiltered = makeImage(_device);
    _prefiltered->initCubeArrayAttachment({kPrefilteredSize, kPrefilteredSize},
                                          Format::R8G8B8A8Unorm,
                                          kMaxDerivedLayers, kNumPrefilteredMips);

    _device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                          reinterpret_cast<uint64_t>(toVulkanImage(*_brdf).handle()), "BRDF LUT");
    _device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                          reinterpret_cast<uint64_t>(toVulkanImage(*_irradiance).handle()), "Irradiance maps");
    _device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                          reinterpret_cast<uint64_t>(toVulkanImage(*_prefiltered).handle()),
                          "Prefiltered environment maps");

    // Everything starts sampleable, because the resolve reads all three whether
    // or not anything has been generated into them yet.
    _renderer.immediateSubmit([this](ICommandBuffer &commandBuffer) {
        commandBuffer.transitionImage(*_brdf, ImageLayout::ShaderRead);
        commandBuffer.transitionImage(*_irradiance, ImageLayout::ShaderRead);
        commandBuffer.transitionImage(*_prefiltered, ImageLayout::ShaderRead);
    });

    // The BRDF table is indexed by NdotV and roughness, both of which reach the
    // edge of their domain - a grazing angle sits at NdotV zero and roughness
    // clamps to one. Sampled with the global repeating sampler those wrap round
    // to the opposite edge, which is exactly where a metallic surface reads.
    auto &samplers = _resources.samplers();
    auto clamped = getTextureProperties(TextureUsage::ColorBuffer);
    _brdf->setSampler(toSampler(samplers.get(clamped)));
    _irradiance->setSampler(toSampler(samplers.get(clamped)));

    // The prefiltered map is the exception: roughness selects a mip, so it
    // needs the chain that ColorBuffer's non-mipmapped filter would clamp away.
    auto prefilteredProps = clamped;
    prefilteredProps.minFilter = Texture::Filtering::LinearMipmapLinear;
    _prefiltered->setSampler(toSampler(samplers.get(prefilteredProps)));


    _inited = true;
}

void PBRTextures::deinit() {
    if (_brdf) {
        _brdf->deinit();
    }
    if (_irradiance) {
        _irradiance->deinit();
    }
    if (_prefiltered) {
        _prefiltered->deinit();
    }
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

Texture &PBRTextures::brdf() {
    throw std::logic_error("Vulkan PBR textures are not exposed as a Texture");
}

void PBRTextures::refresh() {
    _requests.clear();
    _envMapToLayer.clear();
    _envMapSources.clear();
    _nextLayer = 0;
}

int PBRTextures::requestEnvMapDerivedLayer(Texture &envMap) {
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

void PBRTextures::process(ICommandBuffer &commandBuffer, uint32_t globalsOffset) {
    if (!_inited) {
        return;
    }
    if (!_brdfGenerated) {
        VulkanDebugScope scope(_device, static_cast<VulkanCommandBuffer &>(commandBuffer).handle(),
                               "IBL: BRDF integration", {0.5f, 0.3f, 0.6f});
        commandBuffer.transitionImage(*_brdf, ImageLayout::ColorAttachment);
        generateBRDF(commandBuffer, globalsOffset);
        commandBuffer.transitionImage(*_brdf, ImageLayout::ShaderRead);
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

    VulkanDebugScope scope(_device, static_cast<VulkanCommandBuffer &>(commandBuffer).handle(),
                           "IBL: derive environment map", {0.5f, 0.3f, 0.6f});
    commandBuffer.transitionImage(*_irradiance, ImageLayout::ColorAttachment);
    commandBuffer.transitionImage(*_prefiltered, ImageLayout::ColorAttachment);
    generateDerived(commandBuffer, globalsOffset, envMap, layer);
    commandBuffer.transitionImage(*_irradiance, ImageLayout::ShaderRead);
    commandBuffer.transitionImage(*_prefiltered, ImageLayout::ShaderRead);

    _envMapSources[layer] = &envMap;
    debug("Vulkan: derived environment map " + envMap.name() + " into layer " +
              std::to_string(layer),
          LogChannel::Graphics);
}

void PBRTextures::generateBRDF(ICommandBuffer &commandBuffer, uint32_t globalsOffset) {
    PipelineKey key;
    key.module = kModule;
    key.vertexEntry = "iblVertex";
    key.fragmentEntry = "brdfFragment";
    key.colorFormats = {Format::R16G16Sfloat};
    auto pipeline = _pipelines.get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    RenderPassScope rendering(
        static_cast<VulkanCommandBuffer &>(commandBuffer).handle(), {kBRDFSize, kBRDFSize},
        {{toVulkanImageView(_brdf->sampleView()),
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_ATTACHMENT_LOAD_OP_CLEAR,
          VK_ATTACHMENT_STORE_OP_STORE}});
    commandBuffer.bindPipeline(pipeline.pipeline);
    auto uniformSet = _descriptors.uniformDescriptorSet(_ring.frame());
    commandBuffer.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                    offsets.data(), static_cast<uint32_t>(offsets.size()));
    auto textureSet = _descriptors.acquireTextureDescriptorSet(_ring.frame(), nullptr);
    commandBuffer.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, textureSet, nullptr, 0);
    commandBuffer.draw(3, 1);
}

void PBRTextures::generateDerived(ICommandBuffer &commandBuffer, uint32_t globalsOffset,
                                        Texture &envMap, int layer) {
    renderCubeFaces(commandBuffer, globalsOffset, *_irradiance, layer, 0,
                    {kIrradianceSize, kIrradianceSize},
                    "irradianceFragment", &envMap, 0.0f);

    for (int mip = 0; mip < kNumPrefilteredMips; ++mip) {
        int size = static_cast<int>(kPrefilteredSize * std::pow(0.5, mip));
        float roughness = mip / static_cast<float>(kNumPrefilteredMips - 1);
        renderCubeFaces(commandBuffer, globalsOffset, *_prefiltered, layer, mip,
                        {size, size}, "prefilterFragment", &envMap, roughness);
    }
}

void PBRTextures::renderCubeFaces(ICommandBuffer &commandBuffer,
                                        uint32_t globalsOffset,
                                        IImage &target,
                                        int cube,
                                        int mip,
                                        glm::ivec2 extent,
                                        const char *fragmentEntry,
                                        const Texture *envMap,
                                        float roughness) {
    // Six views, one per face. The vertex stage reads SV_ViewID and the fragment
    // stage turns it into a direction, so one draw fills the whole cube.
    PipelineKey key;
    key.module = kModule;
    key.vertexEntry = "iblVertex";
    key.fragmentEntry = fragmentEntry;
    key.colorFormats = {target.pixelFormat()};
    key.viewMask = kCubeViewMask;
    auto pipeline = _pipelines.get(key);

    LocalUniforms locals;
    locals.reset();
    locals.iblRoughness = roughness;
    if (envMap && envMap->isCubeMap()) {
        locals.featureMask |= UniformsFeatureFlags::envmapcube;
    }

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);

    std::vector<std::pair<int, const IImage *>> textures;
    if (envMap) {
        auto unit = envMap->isCubeMap() ? TextureUnits::envMapCube : TextureUnits::envMap;
        textures.push_back({unit, &_resources.get(*envMap)});
    }

    RenderPassScope rendering(
        static_cast<VulkanCommandBuffer &>(commandBuffer).handle(), extent,
        {{toVulkanImageView(target.attachmentView(cube, mip)),
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_ATTACHMENT_LOAD_OP_CLEAR,
          VK_ATTACHMENT_STORE_OP_STORE}},
        std::nullopt, kCubeViewMask);
    commandBuffer.bindPipeline(pipeline.pipeline);
    auto uniformSet = _descriptors.uniformDescriptorSet(_ring.frame());
    commandBuffer.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                    offsets.data(), static_cast<uint32_t>(offsets.size()));
    auto textureSet = _descriptors.acquireTextureDescriptorSet(_ring.frame(), textures);
    commandBuffer.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, textureSet, nullptr, 0);
    commandBuffer.draw(3, 1);
}

} // namespace graphics

} // namespace reone
