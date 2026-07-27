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

#include "reone/scene/render/pipeline/vulkan.h"

#include "reone/graphics/npyutil.h"
#include "reone/graphics/options.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/textureregistry.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/debugscope.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/pbrtextures.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/uniformring.h"
#include "reone/scene/render/pass/vulkan.h"
#include "reone/system/logutil.h"
#include "reone/system/randomutil.h"

#include "imgui_impl_vulkan.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

static constexpr char kResolveModule[] = "pbr_resolve";
static constexpr char kPostProcessModule[] = "postprocess";
static constexpr char kSSAOModule[] = "pbr_ssao";
static constexpr char kSSRModule[] = "pbr_ssr";

/** What the OpenGL pipeline gives cbTransparentGeometry1 and 2. */
static constexpr VkFormat kOITAccumFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
static constexpr VkFormat kOITRevealageFormat = VK_FORMAT_R16_SFLOAT;

/** Matches the OpenGL pipeline's constant of the same name. */
static constexpr float kSharpenAmount = 0.25f;
static constexpr float kSSAOSampleRadius = 0.5f;
static constexpr float kSSAOBias = 0.1f;
static constexpr float kSSRBias = 0.25f;
static constexpr float kSSRPixelStride = 4.0f;
static constexpr float kSSRMaxSteps = 64.0f;

/**
 * Map an OpenGL clip volume onto Vulkan's.
 *
 * The scene graph builds its matrices for OpenGL, where clip z runs -1..1.
 * Vulkan clips at 0..1, so half the depth range would be discarded. This
 * rescales z without touching x or y - the y difference is handled by the
 * viewport instead, which leaves triangle winding alone.
 */
static glm::mat4 glToVulkanClip(const glm::mat4 &m) {
    glm::mat4 correction {1.0f};
    correction[2][2] = 0.5f;
    correction[3][2] = 0.5f;
    return correction * m;
}

/**
 * Move a shadow map between being written and being sampled.
 *
 * Local rather than on VulkanImage because the layout is tracked here: these
 * two images are the pipeline's own, and nothing else touches them.
 */
static void transitionShadowMap(VkCommandBuffer cmd,
                                const VulkanImage &image,
                                VkImageLayout &from,
                                VkImageLayout to) {
    if (from == to) {
        return;
    }
    VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.dstStageMask = b.srcStageMask;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image.handle();
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    from = to;
}

/**
 * Move a colour image between being sampled and being drawn into.
 *
 * The filter chain flips both of its images between the two roles on every
 * pass, and the OIT targets do the same once per frame, so this is called far
 * more often than the one-way transitions elsewhere in this file.
 */
static void transitionColorImage(VkCommandBuffer cmd,
                                 const VulkanImage &image,
                                 VkImageLayout from,
                                 VkImageLayout to) {
    // A transfer layout has to name the transfer stage: the copy at the end of
    // an odd-length chain is neither an attachment write nor a shader read, and
    // describing it as one is a layout-transition error even though the copy
    // itself would appear to work.
    auto stageFor = [](VkImageLayout layout) -> VkPipelineStageFlags2 {
        switch (layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        default:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        }
    };
    auto accessFor = [](VkImageLayout layout) -> VkAccessFlags2 {
        switch (layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        default:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
    };

    VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = stageFor(from);
    b.srcAccessMask = accessFor(from);
    b.dstStageMask = stageFor(to);
    b.dstAccessMask = accessFor(to);
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image.handle();
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void VulkanRenderPipeline::init() {
    if (_inited) {
        return;
    }
    auto &device = _renderer.device();

    _gbuffer = std::make_unique<VulkanGBuffer>(device);
    _gbuffer->init(_targetSize);

    _output = std::make_unique<VulkanImage>(device);
    _output->initColorAttachment(_targetSize, _renderer.swapchain().imageFormat());

    _ping = std::make_unique<VulkanImage>(device);
    _ping->initColorAttachment(_targetSize, _renderer.swapchain().imageFormat());

    _hilights = std::make_unique<VulkanImage>(device);
    _hilights->initColorAttachment(_targetSize, _renderer.swapchain().imageFormat());
    auto halfSize = _targetSize / 2;
    _ssao = std::make_unique<VulkanImage>(device);
    _ssao->initColorAttachment(halfSize, VK_FORMAT_R8_UNORM);
    _ssr = std::make_unique<VulkanImage>(device);
    _ssr->initColorAttachment(halfSize, VK_FORMAT_R8G8B8A8_UNORM);
    _ssaoPing = std::make_unique<VulkanImage>(device);
    _ssaoPing->initColorAttachment(halfSize, VK_FORMAT_R8_UNORM);
    _halfPing = std::make_unique<VulkanImage>(device);
    _halfPing->initColorAttachment(halfSize, VK_FORMAT_R8G8B8A8_UNORM);

    _oitAccum = std::make_unique<VulkanImage>(device);
    _oitAccum->initColorAttachment(_targetSize, kOITAccumFormat);
    _oitRevealage = std::make_unique<VulkanImage>(device);
    _oitRevealage->initColorAttachment(_targetSize, kOITRevealageFormat);

    // The filters sample these, and OpenGL's colour buffers clamp to edge with
    // no mip filtering. Left on the default sampler they would repeat, so a tap
    // just past one screen edge would read the opposite edge - which FXAA does
    // at every border pixel, and sharpen and the blurs do too.
    //
    // The OIT targets are sampled one texel at a time and never step outside
    // the image, but they take the same sampler because it is what OpenGL gives
    // a colour buffer, and because an image created here has no sampler at all
    // until one is set - see commit e2cd6629.
    auto &samplers = _renderer.resources().samplers();
    auto filterSampler = samplers.get(getTextureProperties(TextureUsage::ColorBuffer));
    _output->setSampler(filterSampler);
    _ping->setSampler(filterSampler);
    _hilights->setSampler(filterSampler);
    _ssao->setSampler(filterSampler);
    _ssr->setSampler(filterSampler);
    _ssaoPing->setSampler(filterSampler);
    _halfPing->setSampler(filterSampler);

    // Disabled effects still occupy the resolve's fixed descriptor set. Clear
    // them to their neutral terms once so sampling them leaves today's result.
    device.immediateSubmit([this, halfSize](VkCommandBuffer cmd) {
        auto clear = [&](VulkanImage &image, VkClearColorValue value) {
            transitionColorImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            attachment.imageView = image.view();
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.clearValue.color = value;
            VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = {static_cast<uint32_t>(halfSize.x), static_cast<uint32_t>(halfSize.y)};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &attachment;
            vkCmdBeginRendering(cmd, &rendering);
            vkCmdEndRendering(cmd);
            transitionColorImage(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        };
        // Ambient occlusion of one, and a reflection the resolve weights by its
        // own alpha, so a disabled pass contributes nothing.
        clear(*_ssao, {{1.0f, 1.0f, 1.0f, 1.0f}});
        clear(*_ssr, {{0.0f, 0.0f, 0.0f, 0.0f}});
        clear(*_ssaoPing, {{0.0f, 0.0f, 0.0f, 0.0f}});
        clear(*_halfPing, {{0.0f, 0.0f, 0.0f, 0.0f}});
    });

    if (_options.ssao) {
        for (int i = 0; i < kNumSSAOSamples; ++i) {
            float scale = i / static_cast<float>(kNumSSAOSamples);
            scale = glm::mix(0.1f, 1.0f, scale * scale);
            auto sample = glm::vec3(renderRandomFloat(-1.0f, 1.0f), renderRandomFloat(-1.0f, 1.0f),
                                    renderRandomFloat(0.0f, 1.0f));
            _ssaoSamples[i] = glm::vec4(glm::normalize(sample) * scale, 0.0f);
        }
    }
    _oitAccum->setSampler(filterSampler);
    _oitRevealage->setSampler(filterSampler);

    glm::ivec2 shadowSize {_options.shadowResolution, _options.shadowResolution};
    _dirShadows = std::make_unique<VulkanImage>(device);
    _dirShadows->initDepthLayered(shadowSize, VulkanGBuffer::depthFormat(),
                                  kNumShadowCascades, false);
    _pointShadows = std::make_unique<VulkanImage>(device);
    _pointShadows->initDepthLayered(shadowSize, VulkanGBuffer::depthFormat(),
                                    kNumCubeFaces, true);

    // The resolve samples both maps whichever kind of light is casting, so the
    // one that is not rendered this frame still has to hold something valid.
    // Cleared to the far plane once here, which reads as nothing occluding, and
    // moved into the layout the descriptor was written against.
    device.immediateSubmit([this, shadowSize](VkCommandBuffer cmd) {
        auto clear = [&](VulkanImage &image, int layers) {
            VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = image.view();
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil.depth = 1.0f;

            VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = {static_cast<uint32_t>(shadowSize.x),
                                           static_cast<uint32_t>(shadowSize.y)};
            rendering.layerCount = 1;
            rendering.viewMask = (1u << layers) - 1u;
            rendering.pDepthAttachment = &depth;

            VkImageLayout from = VK_IMAGE_LAYOUT_UNDEFINED;
            transitionShadowMap(cmd, image, from, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
            vkCmdBeginRendering(cmd, &rendering);
            vkCmdEndRendering(cmd);
        };
        clear(*_dirShadows, kNumShadowCascades);
        clear(*_pointShadows, kNumCubeFaces);

        _dirShadowLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        _pointShadowLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        transitionShadowMap(cmd, *_dirShadows, _dirShadowLayout,
                            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        transitionShadowMap(cmd, *_pointShadows, _pointShadowLayout,
                            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    });

    // Everything the resolve samples needs the filtering its OpenGL counterpart
    // has, or it silently gets the global sampler - linear and repeating. For a
    // shadow map that means depth is interpolated before being compared, and a
    // lookup just outside a cascade reads the opposite edge instead of the
    // white border that means "not occluded".
    auto depthSampler = samplers.get(getTextureProperties(TextureUsage::DepthBuffer));
    _gbuffer->setSamplers(filterSampler, depthSampler);
    _dirShadows->setSampler(depthSampler);
    _pointShadows->setSampler(depthSampler);
    // Forward retro draws acquire material sets, not the resolve's fixed set,
    // so their shadow lookups must use these stable bindings as well.
    _renderer.descriptors().setTexture(TextureUnits::shadowMapArray, *_dirShadows);
    _renderer.descriptors().setTexture(TextureUnits::shadowMapCube, *_pointShadows);

    // The output crosses the seam as a Texture. It has no pixels and is never
    // uploaded; the resource cache maps it straight back to the image.
    _outputHandle = std::make_shared<Texture>(
        "vk_scene_output", TextureType::TwoDim, Texture::Properties());
    _renderer.resources().registerExternal(*_outputHandle, *_output);

    // The resolve samples the G-buffer at fixed units, so its set is written
    // once rather than acquired per frame - see VulkanDescriptors.
    std::vector<std::pair<int, const VulkanImage *>> resolveTextures;
    for (int i = 0; i < VulkanGBuffer::Count - 1; ++i) {
        resolveTextures.push_back({i + 1, &_gbuffer->color(i)});
    }
    // The resolve reconstructs world position from depth, so it samples the
    // same image the geometry pass wrote.
    resolveTextures.push_back({TextureUnits::gBufDepth, &_gbuffer->depth()});
    resolveTextures.push_back({TextureUnits::ssao, _ssao.get()});
    resolveTextures.push_back({TextureUnits::ssr, _ssr.get()});
    resolveTextures.push_back({TextureUnits::shadowMapArray, _dirShadows.get()});
    resolveTextures.push_back({TextureUnits::shadowMapCube, _pointShadows.get()});
    auto &pbr = _renderer.pbrTextures();
    resolveTextures.push_back({TextureUnits::brdfLUT, &pbr.brdfImage()});
    resolveTextures.push_back({TextureUnits::irradianceMapArray, &pbr.irradianceArray()});
    resolveTextures.push_back({TextureUnits::prefilteredEnvMapArray, &pbr.prefilteredArray()});
    _resolveSet = _renderer.descriptors().createPersistentTextureSet(resolveTextures);

    // Full-screen passes alternate between these allocations, so each needs a
    // standing set with itself at unit 0.
    _outputAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _output.get()}});
    _pingAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _ping.get()}});
    _hilightsAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _hilights.get()}});
    _ssaoSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::gBufDepth, &_gbuffer->depth()},
         {TextureUnits::gBufEyeNormal, &_gbuffer->color(VulkanGBuffer::EyeNormal)},
         {TextureUnits::noise, &_renderer.resources().get(_textureRegistry.get(TextureName::noiseRg))}});
    _ssrSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, &_gbuffer->color(VulkanGBuffer::Diffuse)},
         {TextureUnits::lightmap, &_gbuffer->color(VulkanGBuffer::Lightmap)},
         {TextureUnits::gBufDepth, &_gbuffer->depth()},
         {TextureUnits::gBufEyeNormal, &_gbuffer->color(VulkanGBuffer::EyeNormal)}});
    _ssaoAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _ssao.get()}});
    _ssrAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _ssr.get()}});
    _ssaoPingAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _ssaoPing.get()}});
    _halfPingAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _halfPing.get()}});

    _oitBlendOutputSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _output.get()},
         {TextureUnits::hilights, _hilights.get()},
         {TextureUnits::oitAccum, _oitAccum.get()},
         {TextureUnits::oitRevealage, _oitRevealage.get()}});
    _oitBlendPingSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _ping.get()},
         {TextureUnits::hilights, _hilights.get()},
         {TextureUnits::oitAccum, _oitAccum.get()},
         {TextureUnits::oitRevealage, _oitRevealage.get()}});

    _frameImage = _output.get();
    _spareImage = _ping.get();

    // The output is written as an attachment and then sampled by the 2D
    // compositor, so it starts in the layout the first pass expects.
    device.immediateSubmit([this](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        // The filter chain expects to find the ping image sampleable, since
        // that is the state every pass leaves it in. So do the OIT targets,
        // which the transparency pass moves back to being attachments.
        std::array<VkImageMemoryBarrier2, 5> barriers {barrier, barrier, barrier, barrier, barrier};
        barriers[0].image = _output->handle();
        barriers[1].image = _ping->handle();
        barriers[2].image = _oitAccum->handle();
        barriers[3].image = _oitRevealage->handle();
        barriers[4].image = _hilights->handle();

        VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo hilights {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        hilights.imageView = _hilights->view();
        hilights.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hilights.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        hilights.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        hilights.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x), static_cast<uint32_t>(_targetSize.y)};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &hilights;
        transitionColorImage(cmd, *_hilights, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdEndRendering(cmd);
        transitionColorImage(cmd, *_hilights, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    auto name = [&device](const VulkanImage &image, const std::string &label) {
        device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                             reinterpret_cast<uint64_t>(image.handle()), label);
        device.setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW,
                             reinterpret_cast<uint64_t>(image.view()), label + " view");
    };
    static const char *kColorNames[VulkanGBuffer::Count] = {
        "G-buffer diffuse", "G-buffer eye normal", "G-buffer lightmap",
        "G-buffer self-illum", "G-buffer motion"};
    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
        name(_gbuffer->color(i), kColorNames[i]);
    }
    name(_gbuffer->depth(), "G-buffer depth");
    name(*_dirShadows, "Shadow map (cascades)");
    name(*_pointShadows, "Shadow map (cube)");
    // Numbered rather than named for their role: the two exchange being the
    // output on every OIT resolve, so "Scene output" would name whichever one
    // happened to hold it when init ran.
    name(*_output, "Scene colour 0");
    name(*_ping, "Scene colour 1");
    name(*_hilights, "Retro highlights");
    name(*_ssao, "SSAO");
    name(*_ssr, "SSR");
    name(*_ssaoPing, "SSAO filter ping");
    name(*_halfPing, "Half-resolution filter ping");
    name(*_oitAccum, "OIT accum");
    name(*_oitRevealage, "OIT revealage");

    _inited = true;
}

void VulkanRenderPipeline::deinit() {
    if (!_inited) {
        return;
    }
    // The descriptor owns a reference to the preview view, so release it
    // before that view. Engine teardown keeps ImGui alive until its pipelines
    // have done the same.
    if (_preview && _preview->imguiTexture) {
        ImGui_ImplVulkan_RemoveTexture(
            static_cast<VkDescriptorSet>(_preview->imguiTexture));
    }
    _preview.reset();
    // Deregistered before the image goes: the registry holds a raw pointer to
    // it, keyed on the Texture, and would outlive both.
    if (_outputHandle) {
        _renderer.resources().unregisterExternal(*_outputHandle);
    }
    _frameImage = nullptr;
    _spareImage = nullptr;
    _output.reset();
    _ping.reset();
    _hilights.reset();
    _ssao.reset();
    _ssr.reset();
    _ssaoPing.reset();
    _halfPing.reset();
    _oitAccum.reset();
    _oitRevealage.reset();
    _dirShadows.reset();
    _pointShadows.reset();
    _gbuffer.reset();
    _outputHandle.reset();
    _inited = false;
}

/**
 * Render the shadow map for whichever light is casting this frame.
 *
 * One pass covers every cascade, or every cube face, through multiview: the
 * view mask says how many, and the vertex stage picks its matrix by view index.
 * Only one of the two runs; the graph supplies the kind of light it chose.
 */
void VulkanRenderPipeline::shadowPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    if (_shadowPass == RenderPassName::None) {
        return;
    }
    bool isDirectional = _shadowPass == RenderPassName::DirLightShadowsPass;

    auto &image = isDirectional ? *_dirShadows : *_pointShadows;
    auto &layout = isDirectional ? _dirShadowLayout : _pointShadowLayout;
    int layers = isDirectional ? kNumShadowCascades : kNumCubeFaces;
    uint32_t viewMask = (1u << layers) - 1u;

    VulkanDebugScope scope(_renderer.device(), cmd,
                           isDirectional ? "Shadows (directional cascades)"
                                         : "Shadows (point light cube)",
                           {0.2f, 0.2f, 0.5f});

    transitionShadowMap(cmd, image, layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = image.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.0f;

    auto extent = image.extent();
    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(extent.x),
                                   static_cast<uint32_t>(extent.y)};
    rendering.layerCount = 1;
    rendering.viewMask = viewMask;
    rendering.pDepthAttachment = &depth;

    // No flipped viewport here. A shadow map is only ever compared against
    // itself, so the one requirement is that rendering and lookup agree, and
    // both use Vulkan's own convention.
    VkViewport viewport {0.0f, 0.0f, static_cast<float>(extent.x),
                         static_cast<float>(extent.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(extent.x),
                               static_cast<uint32_t>(extent.y)}};

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VulkanRenderPass pass(_options,
                          _renderer.device(),
                          _renderer.pipelines(),
                          _renderer.uniformRing(),
                          _renderer.descriptors(),
                          _renderer.resources(),
                          _uniforms,
                          _renderer.pbrTextures(),
                          _meshRegistry,
                          cmd,
                          {},
                          VulkanGBuffer::depthFormat());
    pass.setGlobalsOffset(globalsOffset);
    pass.setShadowViewMask(viewMask);
    _registry->drawScene(
        pass,
        {_shadowPass, RenderCategory::ShadowCaster},
        _cullCamera);

    vkCmdEndRendering(cmd);

    transitionShadowMap(cmd, image, layout, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
}

void VulkanRenderPipeline::geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    if (!_options.pbr) {
        retroGeometryPass(cmd, globalsOffset);
        return;
    }
    VulkanDebugScope scope(_renderer.device(), cmd, "Opaque geometry (G-buffer)",
                           {0.3f, 0.6f, 0.3f});

    std::array<VkRenderingAttachmentInfo, VulkanGBuffer::Count> attachments {};
    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
        auto &a = attachments[i];
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageView = _gbuffer->color(i).view();
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = _gbuffer->depth().view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // Kept, not discarded: the transparency pass depth-tests against it so
    // particles behind a wall stay behind it.
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.0f;

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = VulkanGBuffer::Count;
    rendering.pColorAttachments = attachments.data();
    rendering.pDepthAttachment = &depth;

    // Negative height flips clip y, which is what makes an OpenGL projection
    // rasterise the same way here. Doing it in the viewport rather than in the
    // matrix leaves triangle winding untouched, so front faces stay front -
    // getting this wrong inverts face culling while still looking upright,
    // because the composite used to flip it back.
    VkViewport viewport {0.0f, static_cast<float>(_targetSize.y),
                         static_cast<float>(_targetSize.x),
                         -static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x),
                               static_cast<uint32_t>(_targetSize.y)}};

    _gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VulkanRenderPass pass(_options,
                          _renderer.device(),
                          _renderer.pipelines(),
                          _renderer.uniformRing(),
                          _renderer.descriptors(),
                          _renderer.resources(),
                          _uniforms,
                          _renderer.pbrTextures(),
                          _meshRegistry,
                          cmd,
                          VulkanGBuffer::colorFormats(),
                          VulkanGBuffer::depthFormat());
    pass.setGlobalsOffset(globalsOffset);
    _registry->drawScene(
        pass,
        {RenderPassName::OpaqueGeometry, RenderCategory::Opaque},
        _cullCamera);

    vkCmdEndRendering(cmd);
}

void VulkanRenderPipeline::retroGeometryPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    VulkanDebugScope scope(_renderer.device(), cmd, "Opaque geometry (retro forward)",
                           {0.3f, 0.6f, 0.3f});
    transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transitionColorImage(cmd, *_hilights, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    std::array<VkRenderingAttachmentInfo, 2> attachments {};
    for (auto &attachment : attachments) {
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    attachments[0].imageView = _output->view();
    attachments[1].imageView = _hilights->view();
    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = _gbuffer->depth().view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x), static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<uint32_t>(attachments.size());
    rendering.pColorAttachments = attachments.data();
    rendering.pDepthAttachment = &depth;
    VkViewport viewport {0.0f, static_cast<float>(_targetSize.y), static_cast<float>(_targetSize.x),
                         -static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x), static_cast<uint32_t>(_targetSize.y)}};
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    VulkanRenderPass pass(_options, _renderer.device(), _renderer.pipelines(), _renderer.uniformRing(),
                          _renderer.descriptors(), _renderer.resources(), _uniforms, _renderer.pbrTextures(),
                          _meshRegistry, cmd, {_renderer.swapchain().imageFormat(), _renderer.swapchain().imageFormat()},
                          VulkanGBuffer::depthFormat(), VulkanRenderPass::Kind::Retro);
    pass.setGlobalsOffset(globalsOffset);
    _registry->drawScene(pass, {RenderPassName::OpaqueGeometry, RenderCategory::Opaque}, _cullCamera);
    vkCmdEndRendering(cmd);

    hilightsBlurPass(cmd);
}

/**
 * Blur the highlight buffer, as the OpenGL retro pipeline does before blending.
 *
 * Two thirteen-tap passes, one per axis, through the ping allocation and back.
 * The buffer holds only the self-illuminated pixels that came out near white, so
 * leaving it sharp does not merely soften the bloom - the blend adds it to the
 * opaque image, and an unspread highlight adds all of its energy to the few
 * pixels that produced it.
 *
 * Both images are left in the layout they arrived in, so nothing downstream has
 * to know this ran.
 */
void VulkanRenderPipeline::hilightsBlurPass(VkCommandBuffer cmd) {
    VulkanDebugScope scope(_renderer.device(), cmd, "Highlight blur", {0.8f, 0.8f, 0.4f});

    ScreenEffectUniforms screenEffect;
    screenEffect.screenResolution = glm::vec2(_targetSize);
    screenEffect.screenResolutionRcp = 1.0f / screenEffect.screenResolution;

    VulkanPipelineCache::Key key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = "gausBlur13Fragment";
    key.colorFormats = {_renderer.swapchain().imageFormat()};
    auto &pipeline = _renderer.pipelines().get(key);

    // Not flipped, for the reason given in filterPass.
    VkViewport viewport {0.0f, 0.0f, static_cast<float>(_targetSize.x),
                         static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x),
                               static_cast<uint32_t>(_targetSize.y)}};

    auto axis = [&](const VulkanImage &source, VkDescriptorSet sourceSet,
                    const VulkanImage &destination, glm::vec2 direction) {
        transitionColorImage(cmd, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transitionColorImage(cmd, destination, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        screenEffect.blurDirection = direction;
        auto offset = _renderer.uniformRing().push(screenEffect);

        VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = destination.view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // Every texel is written.
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                       static_cast<uint32_t>(_targetSize.y)};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;

        std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::screenEffect] = offset;

        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        auto uniformSet = _renderer.descriptors().uniformSet(_renderer.uniformRing().frame());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kUniformSet, 1, &uniformSet,
                                static_cast<uint32_t>(offsets.size()), offsets.data());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kTextureSet, 1, &sourceSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    };

    // The second axis reads the ping allocation back, which leaves it
    // sampleable - the state every pass leaves it in, and the one the passes
    // after this expect to find.
    axis(*_hilights, _hilightsAsSourceSet, *_ping, {1.0f, 0.0f});
    axis(*_ping, _pingAsSourceSet, *_hilights, {0.0f, 1.0f});
}

void VulkanRenderPipeline::screenSpaceEffectsPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    if (!_options.ssao && !_options.ssr) {
        return;
    }
    VulkanDebugScope scope(_renderer.device(), cmd, "Screen-space effects", {0.4f, 0.7f, 0.6f});
    auto halfSize = _targetSize / 2;
    _gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    VkViewport viewport {0.0f, 0.0f, static_cast<float>(halfSize.x), static_cast<float>(halfSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(halfSize.x), static_cast<uint32_t>(halfSize.y)}};
    auto draw = [&](const char *module, const char *vertex, const char *fragment, VkFormat format,
                    VulkanImage &destination, VkDescriptorSet textures, const ScreenEffectUniforms &screenEffect) {
        transitionColorImage(cmd, destination, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VulkanPipelineCache::Key key;
        key.module = module;
        key.vertexEntry = vertex;
        key.fragmentEntry = fragment;
        key.colorFormats = {format};
        auto &pipeline = _renderer.pipelines().get(key);
        auto offset = _renderer.uniformRing().push(screenEffect);
        VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = destination.view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {static_cast<uint32_t>(halfSize.x), static_cast<uint32_t>(halfSize.y)};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        offsets[UniformBlockBindingPoints::screenEffect] = offset;
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        auto uniformSet = _renderer.descriptors().uniformSet(_renderer.uniformRing().frame());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kUniformSet, 1, &uniformSet,
                                static_cast<uint32_t>(offsets.size()), offsets.data());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kTextureSet, 1, &textures, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    };
    auto filter = [&](VulkanImage &source, VkDescriptorSet sourceSet, VulkanImage &destination,
                      VkFormat format, const char *fragment, glm::vec2 direction) {
        transitionColorImage(cmd, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ScreenEffectUniforms screenEffect;
        screenEffect.screenResolution = glm::vec2(halfSize);
        screenEffect.screenResolutionRcp = 1.0f / screenEffect.screenResolution;
        screenEffect.blurDirection = direction;
        draw(kPostProcessModule, "postVertex", fragment, format, destination, sourceSet, screenEffect);
    };

    if (_options.ssao) {
        ScreenEffectUniforms screenEffect;
        screenEffect.screenResolution = glm::vec2(halfSize);
        screenEffect.screenResolutionRcp = 1.0f / screenEffect.screenResolution;
        screenEffect.ssaoSampleRadius = kSSAOSampleRadius;
        screenEffect.ssaoBias = kSSAOBias;
        std::copy(_ssaoSamples.begin(), _ssaoSamples.end(), std::begin(screenEffect.ssaoSamples));
        draw(kSSAOModule, "ssaoVertex", "ssaoFragment", VK_FORMAT_R8_UNORM, *_ssao, _ssaoSet, screenEffect);
        filter(*_ssao, _ssaoAsSourceSet, *_ssaoPing, VK_FORMAT_R8_UNORM, "boxBlur4Fragment", {0.0f, 0.0f});
        transitionColorImage(cmd, *_ssaoPing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionColorImage(cmd, *_ssao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[1] = {halfSize.x, halfSize.y, 1};
        blit.dstOffsets[1] = {halfSize.x, halfSize.y, 1};
        vkCmdBlitImage(cmd, _ssaoPing->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       _ssao->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        transitionColorImage(cmd, *_ssaoPing, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transitionColorImage(cmd, *_ssao, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (_options.ssr) {
        ScreenEffectUniforms screenEffect;
        screenEffect.screenResolution = glm::vec2(halfSize);
        screenEffect.screenResolutionRcp = 1.0f / screenEffect.screenResolution;
        screenEffect.clipNear = _uniforms.globals().clipNear;
        screenEffect.ssrBias = kSSRBias;
        screenEffect.ssrPixelStride = kSSRPixelStride;
        screenEffect.ssrMaxSteps = kSSRMaxSteps;
        draw(kSSRModule, "ssrVertex", "ssrFragment", VK_FORMAT_R8G8B8A8_UNORM, *_ssr, _ssrSet, screenEffect);
        filter(*_ssr, _ssrAsSourceSet, *_halfPing, VK_FORMAT_R8G8B8A8_UNORM, "gausBlur13Fragment", {1.0f, 0.0f});
        filter(*_halfPing, _halfPingAsSourceSet, *_ssr, VK_FORMAT_R8G8B8A8_UNORM, "gausBlur13Fragment", {0.0f, 1.0f});
        transitionColorImage(cmd, *_ssr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void VulkanRenderPipeline::resolvePass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    VulkanDebugScope scope(_renderer.device(), cmd, "Deferred resolve",
                           {0.9f, 0.7f, 0.3f});

    // Attachments become textures. Depth moves to the read-only layout it is
    // sampled from, which is also what the transparency pass afterwards needs,
    // so it stays there for the rest of the frame.
    _gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    VkImageMemoryBarrier2 toAttachment {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toAttachment.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.image = _frameImage->handle();
    toAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toAttachment.subresourceRange.levelCount = 1;
    toAttachment.subresourceRange.layerCount = 1;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toAttachment;
    vkCmdPipelineBarrier2(cmd, &dep);
    transitionColorImage(cmd, *_hilights, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    std::array<VkRenderingAttachmentInfo, 2> attachments {};
    for (auto &attachment : attachments) {
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    attachments[0].imageView = _frameImage->view();
    attachments[1].imageView = _hilights->view();

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<uint32_t>(attachments.size());
    rendering.pColorAttachments = attachments.data();

    VulkanPipelineCache::Key key;
    key.module = kResolveModule;
    key.vertexEntry = "resolveVertex";
    key.fragmentEntry = "resolveFragment";
    key.colorFormats = {_renderer.swapchain().imageFormat(), _renderer.swapchain().imageFormat()};
    auto &pipeline = _renderer.pipelines().get(key);

    VkViewport viewport {0.0f, 0.0f, static_cast<float>(_targetSize.x),
                         static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x),
                               static_cast<uint32_t>(_targetSize.y)}};

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    auto uniformSet = _renderer.descriptors().uniformSet(_renderer.uniformRing().frame());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &_resolveSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    hilightsBlurPass(cmd);
    // Deliberately left as a colour attachment: transparency blends onto it
    // next, and render() moves it to a sampleable layout once that is done.
}

/**
 * Draw onto the resolved image, depth-testing against the opaque geometry but
 * writing no depth.
 *
 * What post-processing draws through. Forward, not deferred: these surfaces
 * have no single depth at which to resolve lighting, so they shade in place and
 * blend onto what is already there, in draw order. Transparent geometry proper
 * does not come through here - it goes through the OIT targets instead.
 */
void VulkanRenderPipeline::drawOntoOutput(VkCommandBuffer cmd,
                                          uint32_t globalsOffset,
                                          RenderPassName passName,
                                          const char *label) {
    VulkanDebugScope scope(_renderer.device(), cmd, label, {0.7f, 0.4f, 0.7f});

    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _frameImage->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = _gbuffer->depth().view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    rendering.pDepthAttachment = &depth;

    // The same flipped viewport the geometry pass uses. Without it transparent
    // geometry lands mirrored relative to the opaque geometry it sits among.
    VkViewport viewport {0.0f, static_cast<float>(_targetSize.y),
                         static_cast<float>(_targetSize.x),
                         -static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x),
                               static_cast<uint32_t>(_targetSize.y)}};

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VulkanRenderPass pass(_options,
                          _renderer.device(),
                          _renderer.pipelines(),
                          _renderer.uniformRing(),
                          _renderer.descriptors(),
                          _renderer.resources(),
                          _uniforms,
                          _renderer.pbrTextures(),
                          _meshRegistry,
                          cmd,
                          {_renderer.swapchain().imageFormat()},
                          VulkanGBuffer::depthFormat(),
                          VulkanRenderPass::Kind::Forward);
    pass.setGlobalsOffset(globalsOffset);
    auto category = passName == RenderPassName::PostProcessing
                        ? RenderCategory::LensFlare
                        : RenderCategory::Debug;
    _registry->drawScene(pass, {passName, category}, _cullCamera);

    vkCmdEndRendering(cmd);
}

/**
 * Transparent geometry, accumulated into the two OIT targets.
 *
 * Not onto the output: each surface contributes a weighted colour and a weight
 * without regard to draw order, and oitBlendPass divides one by the other
 * afterwards. The counterpart of beginTransparentGeometryPass in the OpenGL
 * pipeline, down to the clear values - accum's alpha starts at one and the
 * blend multiplies it down, which is what makes it revealage.
 *
 * Runs even with nothing to draw, as OpenGL's does, because the resolve reads
 * these targets either way and a cleared pair composites to the opaque image
 * unchanged.
 */
void VulkanRenderPipeline::transparencyPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    VulkanDebugScope scope(_renderer.device(), cmd, "Transparent geometry (OIT)",
                           {0.7f, 0.4f, 0.7f});

    transitionColorImage(cmd, *_oitAccum, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transitionColorImage(cmd, *_oitRevealage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    std::array<VkRenderingAttachmentInfo, 2> attachments {};
    for (auto &a : attachments) {
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    attachments[0].imageView = _oitAccum->view();
    attachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    attachments[1].imageView = _oitRevealage->view();
    attachments[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    // Depth tests against the opaque geometry so that a transparent surface
    // behind a wall stays behind it, but writes nothing: one transparent
    // surface must not occlude the next.
    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = _gbuffer->depth().view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<uint32_t>(attachments.size());
    rendering.pColorAttachments = attachments.data();
    rendering.pDepthAttachment = &depth;

    // The same flipped viewport the geometry pass uses, so that transparent
    // geometry lands where the opaque geometry it sits among did.
    VkViewport viewport {0.0f, static_cast<float>(_targetSize.y),
                         static_cast<float>(_targetSize.x),
                         -static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x),
                               static_cast<uint32_t>(_targetSize.y)}};

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VulkanRenderPass pass(_options,
                              _renderer.device(),
                              _renderer.pipelines(),
                              _renderer.uniformRing(),
                              _renderer.descriptors(),
                              _renderer.resources(),
                              _uniforms,
                              _renderer.pbrTextures(),
                              _meshRegistry,
                              cmd,
                              {kOITAccumFormat, kOITRevealageFormat},
                              VulkanGBuffer::depthFormat(),
                              VulkanRenderPass::Kind::OIT);
    pass.setGlobalsOffset(globalsOffset);
    _registry->drawScene(
        pass,
        {RenderPassName::TransparentGeometry, RenderCategory::Transparent},
        _cullCamera);

    vkCmdEndRendering(cmd);
}

/**
 * Composite the accumulated transparency onto the opaque image.
 *
 * A full-screen pass cannot read and write one image, so this publishes its
 * destination as the new frame image for every later pass.
 */
void VulkanRenderPipeline::oitBlendPass(VkCommandBuffer cmd) {
    VulkanDebugScope scope(_renderer.device(), cmd, "OIT resolve", {0.8f, 0.5f, 0.5f});

    transitionColorImage(cmd, *_oitAccum, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionColorImage(cmd, *_oitRevealage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionColorImage(cmd, *_frameImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Both the retro geometry pass and the deferred resolve blur highlights
    // before reaching this point. The blur restores its destination's incoming
    // layout, so this is a colour attachment on either path.
    transitionColorImage(cmd, *_hilights, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionColorImage(cmd, *_spareImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _spareImage->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // Every texel is written, so there is nothing to preserve.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    VulkanPipelineCache::Key key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = "oitBlendFragment";
    key.colorFormats = {_renderer.swapchain().imageFormat()};
    auto &pipeline = _renderer.pipelines().get(key);

    // Not flipped: this samples by UV and writes the same UV, and all three of
    // the images it reads were written the same way up.
    VkViewport viewport {0.0f, 0.0f, static_cast<float>(_targetSize.x),
                         static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x),
                               static_cast<uint32_t>(_targetSize.y)}};

    // The shader reads no uniform block, but the layout still declares them all,
    // so the set is bound with every offset at zero.
    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    auto uniformSet = _renderer.descriptors().uniformSet(_renderer.uniformRing().frame());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    auto oitBlendSet = _frameImage == _output.get()
                           ? _oitBlendOutputSet
                           : _oitBlendPingSet;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &oitBlendSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    std::swap(_frameImage, _spareImage);
}

/**
 * Whatever the scene draws over the finished image.
 *
 * Only lens flares at present - billboards blended additively with no depth
 * test. The filter chain that runs after this is filterChainPass.
 */
void VulkanRenderPipeline::postProcessingPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    drawOntoOutput(
        cmd, globalsOffset, RenderPassName::PostProcessing, "Post-processing");
}

void VulkanRenderPipeline::filterPass(VkCommandBuffer cmd,
                                      uint32_t screenEffectOffset,
                                      const char *fragmentEntry,
                                      const char *label) {
    VulkanDebugScope scope(_renderer.device(), cmd, label, {0.4f, 0.6f, 0.9f});

    transitionColorImage(cmd, *_frameImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionColorImage(cmd, *_spareImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _spareImage->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // Every texel is written, so there is nothing to preserve.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    VulkanPipelineCache::Key key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = fragmentEntry;
    key.colorFormats = {_renderer.swapchain().imageFormat()};
    auto &pipeline = _renderer.pipelines().get(key);

    // Not flipped: this samples by UV and writes the same UV, so flipping the
    // viewport here would turn the image over on every pass.
    VkViewport viewport {0.0f, 0.0f, static_cast<float>(_targetSize.x),
                         static_cast<float>(_targetSize.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(_targetSize.x),
                               static_cast<uint32_t>(_targetSize.y)}};

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    auto uniformSet = _renderer.descriptors().uniformSet(_renderer.uniformRing().frame());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    auto sourceSet = _frameImage == _output.get()
                         ? _outputAsSourceSet
                         : _pingAsSourceSet;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &sourceSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    std::swap(_frameImage, _spareImage);
}

/**
 * Antialias and sharpen the finished image, as the OpenGL pipeline does.
 *
 * Each pass publishes the image it wrote, so the consumer does not depend on
 * whether the enabled chain has odd or even length.
 */
void VulkanRenderPipeline::filterChainPass(VkCommandBuffer cmd) {
    if (!_options.fxaa && !_options.sharpen) {
        return;
    }
    VulkanDebugScope scope(_renderer.device(), cmd, "Filter chain", {0.3f, 0.5f, 0.8f});

    // Filled here rather than read back from IUniforms: the scene graph never
    // sets this block, because under OpenGL each filter sets it itself as it
    // runs.
    ScreenEffectUniforms screenEffect;
    screenEffect.screenResolution = glm::vec2(_targetSize);
    screenEffect.screenResolutionRcp = 1.0f / screenEffect.screenResolution;
    screenEffect.sharpenAmount = kSharpenAmount;
    auto offset = _renderer.uniformRing().push(screenEffect);

    if (_options.fxaa && _options.sharpen) {
        filterPass(cmd, offset, "fxaaFragment", "FXAA");
        filterPass(cmd, offset, "sharpenFragment", "Sharpen");
        return;
    }
    if (_options.fxaa) {
        filterPass(cmd, offset, "fxaaFragment", "FXAA");
    } else {
        filterPass(cmd, offset, "sharpenFragment", "Sharpen");
    }
}

Texture &VulkanRenderPipeline::render(RenderRegistry &registry,
                                      const CameraSceneNode *camera,
                                      RenderPassName activeShadowPass) {
    auto cmd = _renderer.commandBuffer();
    _registry = &registry;
    _cullCamera = camera;
    _shadowPass = activeShadowPass;

    // The scene graph filled GlobalUniforms through the GL Uniforms object,
    // which is inert under Vulkan, so the values are read back from its CPU
    // mirror and pushed into this frame's arena instead.
    // The matrices arrive in OpenGL convention; only the depth range needs
    // rewriting here. See glToVulkanClip and the flipped viewport below.
    auto globals = _uniforms.globals();
    globals.projection = glToVulkanClip(globals.projection);
    globals.projectionInv = glm::inverse(globals.projection);
    globals.viewProjection = glToVulkanClip(globals.viewProjection);
    globals.prevViewProjection = glToVulkanClip(globals.prevViewProjection);
    // The shadow matrices are built for OpenGL too, and are used twice: once to
    // render the map and once to look into it. Correcting them here keeps the
    // two agreeing, and puts shadow depth in 0..1 like everything else.
    for (int i = 0; i < kNumShadowLightSpace; ++i) {
        globals.shadowLightSpace[i] = glToVulkanClip(globals.shadowLightSpace[i]);
    }
    auto globalsOffset = _renderer.uniformRing().push(globals);

    // Before anything else this frame: a newly seen environment map has to be
    // convolved before the resolve can sample it, and this begins its own
    // render passes, so it cannot sit inside one.
    if (_options.pbr) {
        _renderer.pbrTextures().process(cmd, globalsOffset);
    }

    shadowPass(cmd, globalsOffset);
    geometryPass(cmd, globalsOffset);
    // Both allocations are sampleable at the end of every frame. The deferred
    // resolve starts a new frame image in the first one; every later full-screen
    // pass publishes whichever allocation it writes.
    _frameImage = _output.get();
    _spareImage = _ping.get();
    if (_options.pbr) {
        screenSpaceEffectsPass(cmd, globalsOffset);
        resolvePass(cmd, globalsOffset);
    }
    transparencyPass(cmd, globalsOffset);
    oitBlendPass(cmd);
    postProcessingPass(cmd, globalsOffset);
    filterChainPass(cmd);

    // Everything that draws into the output has now run, so hand it to the 2D
    // compositor in a layout it can sample.
    VkImageMemoryBarrier2 toRead {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = _frameImage->handle();
    toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRead.subresourceRange.levelCount = 1;
    toRead.subresourceRange.layerCount = 1;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toRead;
    vkCmdPipelineBarrier2(cmd, &dep);

    previewPass(cmd, globalsOffset);

    _renderer.resources().registerExternal(*_outputHandle, *_frameImage);
    return *_outputHandle;
}

/**
 * How a target's format comes back on the CPU: channel count and element type.
 *
 * Read back as stored. Half-float targets are widened to float on the way out
 * rather than written as halves, so a dump from either backend has the same
 * dtype and the two can be subtracted without a cast - OpenGL's readback widens
 * them in the driver, and half to float is exact either way.
 */
struct DumpFormat {
    int channels;
    NpyType type;
    bool halfToFloat;
};

static std::optional<DumpFormat> dumpFormatFor(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DumpFormat {4, NpyType::UInt8, false};
    case VK_FORMAT_R8_UNORM:
        return DumpFormat {1, NpyType::UInt8, false};
    case VK_FORMAT_R16_SFLOAT:
        return DumpFormat {1, NpyType::Float32, true};
    case VK_FORMAT_R16G16_SFLOAT:
        return DumpFormat {2, NpyType::Float32, true};
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return DumpFormat {4, NpyType::Float32, true};
    case VK_FORMAT_D32_SFLOAT:
        return DumpFormat {1, NpyType::Float32, false};
    default:
        return std::nullopt;
    }
}

/** IEEE half to float. Exact - every half has an exact float representation. */
static float halfToFloat(uint16_t half) {
    uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1f;
    uint32_t mantissa = half & 0x3ff;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: renormalise into float's wider exponent range.
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ff;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1f) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

/** Whether a format stores blue first, and so needs swizzling on the way out. */
static bool isBGRA(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return true;
    default:
        return false;
    }
}

std::vector<VulkanRenderPipeline::Target> VulkanRenderPipeline::targetEntries() const {
    if (!_inited) {
        return {};
    }
    std::vector<Target> entries;
    if (!_options.pbr) {
        entries.push_back({"Output", "output", RenderTargetKind::Color,
                           _frameImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
        return entries;
    }
    static const char *kDisplayNames[VulkanGBuffer::Count] = {
        "G-buffer diffuse", "G-buffer eye normal", "G-buffer lightmap",
        "G-buffer self-illum", "G-buffer motion"};
    static const char *kDumpNames[VulkanGBuffer::Count] = {
        "g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_lightmap",
        "g_buffer_self_illum", "g_buffer_motion"};
    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
        auto kind = i == VulkanGBuffer::EyeNormal ? RenderTargetKind::EyeNormal :
                    i == VulkanGBuffer::Motion ? RenderTargetKind::Motion :
                                                  RenderTargetKind::Color;
        entries.push_back({kDisplayNames[i], kDumpNames[i], kind, &_gbuffer->color(i),
                           _gbuffer->colorLayout(), false});
    }
    entries.push_back({"G-buffer depth", "g_buffer_depth", RenderTargetKind::Depth,
                       &_gbuffer->depth(), _gbuffer->depthLayout(), true});
    entries.push_back({"SSAO", "ssao", RenderTargetKind::Color,
                       _ssao.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
    entries.push_back({"SSR", "ssr", RenderTargetKind::Color,
                       _ssr.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
    entries.push_back({"Deferred opaque 2", "deferred_opaque_2", RenderTargetKind::Color,
                       _hilights.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
    entries.push_back({"OIT accum", "oit_accum", RenderTargetKind::Color,
                       _oitAccum.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
    entries.push_back({"OIT revealage", "oit_revealage", RenderTargetKind::Color,
                       _oitRevealage.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
    entries.push_back({"Output", "output", RenderTargetKind::Color,
                       _frameImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
    return entries;
}

void *VulkanRenderPipeline::renderTargetPreview(const std::string &name, int mode, float scale) {
    auto entries = targetEntries();
    if (std::none_of(entries.begin(), entries.end(), [&name](const auto &entry) {
            return entry.name == name;
        })) {
        return nullptr;
    }
    if (!_preview) {
        _preview = std::make_unique<Preview>();
        _preview->image = std::make_unique<VulkanImage>(_renderer.device());
        _preview->image->initColorAttachment({480, 360}, VK_FORMAT_R8G8B8A8_UNORM);
        _preview->image->setSampler(
            _renderer.resources().samplers().get(getTextureProperties(TextureUsage::ColorBuffer)));
        _renderer.device().immediateSubmit([this](VkCommandBuffer cmd) {
            transitionColorImage(cmd, *_preview->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        _preview->imguiTexture = ImGui_ImplVulkan_AddTexture(
            _preview->image->sampler(), _preview->image->view(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    _preview->target = name;
    _preview->mode = mode;
    _preview->scale = scale;
    return _preview->imguiTexture;
}

void VulkanRenderPipeline::previewPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    if (!_preview) {
        return;
    }
    auto entries = targetEntries();
    auto selected = std::find_if(entries.begin(), entries.end(), [this](const auto &entry) {
        return entry.name == _preview->target;
    });
    if (selected == entries.end()) {
        return;
    }

    VulkanDebugScope scope(_renderer.device(), cmd, "Render target preview", {0.5f, 0.7f, 0.9f});
    transitionColorImage(cmd, *_preview->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _preview->image->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {480, 360};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    VulkanPipelineCache::Key key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = "debugTextureFragment";
    key.colorFormats = {_preview->image->format()};
    auto &pipeline = _renderer.pipelines().get(key);

    ScreenEffectUniforms screenEffect;
    screenEffect.clipNear = _uniforms.globals().clipNear;
    screenEffect.clipFar = _uniforms.globals().clipFar;
    // These fields are otherwise irrelevant to this pass and avoid another
    // uniform block solely for the two viewer controls.
    screenEffect.ssaoSampleRadius = static_cast<float>(_preview->mode);
    screenEffect.ssrBias = _preview->scale;
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);
    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;
    auto sourceSet = _renderer.descriptors().acquireTextureSet(
        _renderer.uniformRing().frame(), selected->image);
    VkViewport viewport {0.0f, 0.0f, 480.0f, 360.0f, 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {480, 360}};

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    auto uniformSet = _renderer.descriptors().uniformSet(_renderer.uniformRing().frame());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &sourceSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    transitionColorImage(cmd, *_preview->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanRenderPipeline::dumpTargets(const std::filesystem::path &dir) {
    if (!_inited) {
        return;
    }
    std::filesystem::create_directories(dir);

    info("Vulkan scene traversals: " + std::to_string(_registry->traversalCount()),
         LogChannel::Graphics);
    info("Vulkan registry registered: " + formatRegistryCounts(_registry->registeredCounts()),
         LogChannel::Graphics);
    for (const auto &[pass, drawn] : _registry->drawnCountsByPass()) {
        info("Vulkan registry drawn " + renderPassName(pass) +
                 ": " + formatRegistryCounts(drawn),
             LogChannel::Graphics);
    }

    auto entries = targetEntries();

    for (const auto &entry : entries) {
        auto format = dumpFormatFor(entry.image->format());
        if (!format) {
            warn("Cannot dump target '" + std::string(entry.name) + "': unsupported format",
                 LogChannel::Graphics);
            continue;
        }
        auto raw = entry.image->readBack(entry.layout, entry.depth);
        auto extent = entry.image->extent();
        // The output image carries the swapchain's format, which is BGRA here
        // while every G-buffer target is RGBA. A dump exists to be compared
        // against the OpenGL backend, so it is written in one channel order
        // rather than leaving whoever reads it to know which target is which -
        // getting that wrong once already turned an 0.9 difference into an
        // apparent 11.7 and invented a colour cast that was not there.
        if (isBGRA(entry.image->format())) {
            for (size_t i = 0; i + 3 < raw.size(); i += 4) {
                std::swap(raw[i], raw[i + 2]);
            }
        }
        auto path = dir / (std::string(entry.dumpName) + ".npy");
        if (format->halfToFloat) {
            size_t count = raw.size() / sizeof(uint16_t);
            std::vector<float> widened(count);
            for (size_t i = 0; i < count; ++i) {
                uint16_t half;
                std::memcpy(&half, raw.data() + i * sizeof(uint16_t), sizeof(half));
                widened[i] = halfToFloat(half);
            }
            writeNpy(path, widened.data(), extent.x, extent.y, format->channels, format->type);
        } else {
            writeNpy(path, raw.data(), extent.x, extent.y, format->channels, format->type);
        }
    }
    if (_options.pbr) {
        // Cube arrays are unrolled face-after-face: layer 0 +X..-Z, then
        // layer 1 +X..-Z, and so on. Keeping every layer makes the dump useful
        // even when a scene derives more than one environment map.
        auto dumpCubeArray = [&dir](const char *name, const VulkanImage &image, int mip,
                                    uint32_t layers) {
            constexpr uint32_t kFaces = 6;
            auto format = dumpFormatFor(image.format());
            if (!format) {
                warn("Cannot dump cube array '" + std::string(name) + "': unsupported format",
                     LogChannel::Graphics);
                return;
            }
            auto raw = image.readBack(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mip, layers);
            auto extent = glm::max(glm::ivec2(1), image.extent() >> mip);
            // Vulkan's image-copy rows are upside down relative to the GL
            // cube-array readback. Normalize the diagnostic layout here; this
            // does not affect the texture's sampling convention.
            std::vector<uint8_t> flipped(raw.size());
            size_t rowBytes = raw.size() / (static_cast<size_t>(layers) * extent.y);
            size_t faceBytes = rowBytes * extent.y;
            for (uint32_t face = 0; face < layers; ++face) {
                for (int y = 0; y < extent.y; ++y) {
                    std::memcpy(flipped.data() + face * faceBytes + y * rowBytes,
                                raw.data() + face * faceBytes + (extent.y - 1 - y) * rowBytes,
                                rowBytes);
                }
            }
            raw = std::move(flipped);
            auto path = dir / (std::string(name) + ".npy");
            if (format->halfToFloat) {
                size_t count = raw.size() / sizeof(uint16_t);
                std::vector<float> widened(count);
                for (size_t i = 0; i < count; ++i) {
                    uint16_t half;
                    std::memcpy(&half, raw.data() + i * sizeof(uint16_t), sizeof(half));
                    widened[i] = halfToFloat(half);
                }
                writeNpy(path, widened.data(), extent.x, extent.y * static_cast<int>(layers),
                         format->channels, format->type);
            } else {
                writeNpy(path, raw.data(), extent.x, extent.y * static_cast<int>(layers),
                         format->channels, format->type);
            }
        };
        auto &pbr = _renderer.pbrTextures();
        dumpCubeArray("irradiance_map_array", pbr.irradianceArray(), 0, 16 * 6);
        for (int mip = 0; mip < pbr.prefilteredArray().mipLevels(); ++mip) {
            dumpCubeArray(("prefiltered_env_map_array_mip" + std::to_string(mip)).c_str(),
                          pbr.prefilteredArray(), mip, 16 * 6);
        }
    }
    info("Dumped " + std::to_string(entries.size()) + " render targets to " + dir.string(),
         LogChannel::Graphics);
}

std::vector<RenderTargetInfo> VulkanRenderPipeline::targets() const {
    std::vector<RenderTargetInfo> result;
    for (const auto &entry : targetEntries()) {
        result.push_back({entry.name, entry.kind, nullptr});
    }
    return result;
}

} // namespace scene

} // namespace reone
