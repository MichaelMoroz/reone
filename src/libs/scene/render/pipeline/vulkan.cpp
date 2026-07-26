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

using namespace reone::graphics;

namespace reone {

namespace scene {

static constexpr char kResolveModule[] = "pbr_resolve";
static constexpr char kPostProcessModule[] = "postprocess";

/** Matches the OpenGL pipeline's constant of the same name. */
static constexpr float kSharpenAmount = 0.25f;

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
    resolveTextures.push_back({TextureUnits::shadowMapArray, _dirShadows.get()});
    resolveTextures.push_back({TextureUnits::shadowMapCube, _pointShadows.get()});
    auto &pbr = _renderer.pbrTextures();
    resolveTextures.push_back({TextureUnits::brdfLUT, &pbr.brdfImage()});
    resolveTextures.push_back({TextureUnits::irradianceMapArray, &pbr.irradianceArray()});
    resolveTextures.push_back({TextureUnits::prefilteredEnvMapArray, &pbr.prefilteredArray()});
    _resolveSet = _renderer.descriptors().createPersistentTextureSet(resolveTextures);

    // The filter chain alternates between these two, so each needs a standing
    // set with itself at unit 0.
    _outputAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _output.get()}});
    _pingAsSourceSet = _renderer.descriptors().createPersistentTextureSet(
        {{TextureUnits::mainTex, _ping.get()}});

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
        // that is the state every pass leaves it in.
        std::array<VkImageMemoryBarrier2, 2> barriers {barrier, barrier};
        barriers[0].image = _output->handle();
        barriers[1].image = _ping->handle();

        VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
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
    name(*_output, "Scene output");

    _inited = true;
}

void VulkanRenderPipeline::deinit() {
    if (!_inited) {
        return;
    }
    // Deregistered before the image goes: the registry holds a raw pointer to
    // it, keyed on the Texture, and would outlive both.
    if (_outputHandle) {
        _renderer.resources().unregisterExternal(*_outputHandle);
    }
    _output.reset();
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
 * Only one of the two runs - the scene graph registers the callback for the
 * kind of light it chose.
 */
void VulkanRenderPipeline::shadowPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    auto directional = _passCallbacks.find(RenderPassName::DirLightShadowsPass);
    auto point = _passCallbacks.find(RenderPassName::PointLightShadows);
    bool isDirectional = directional != _passCallbacks.end();
    auto callback = isDirectional ? directional : point;
    if (callback == _passCallbacks.end()) {
        return;
    }

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
    callback->second(pass);

    vkCmdEndRendering(cmd);

    transitionShadowMap(cmd, image, layout, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
}

void VulkanRenderPipeline::geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    auto callback = _passCallbacks.find(RenderPassName::OpaqueGeometry);
    if (callback == _passCallbacks.end()) {
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
    callback->second(pass);

    vkCmdEndRendering(cmd);
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
    toAttachment.image = _output->handle();
    toAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toAttachment.subresourceRange.levelCount = 1;
    toAttachment.subresourceRange.layerCount = 1;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toAttachment;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _output->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    VulkanPipelineCache::Key key;
    key.module = kResolveModule;
    key.vertexEntry = "resolveVertex";
    key.fragmentEntry = "resolveFragment";
    key.colorFormats = {_renderer.swapchain().imageFormat()};
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
    // Deliberately left as a colour attachment: transparency blends onto it
    // next, and render() moves it to a sampleable layout once that is done.
}

/**
 * Draw onto the resolved image, depth-testing against the opaque geometry but
 * writing no depth.
 *
 * Shared by everything that runs after the resolve. Forward, not deferred:
 * these surfaces have no single depth at which to resolve lighting, so they
 * shade in place and blend onto what is already there.
 */
void VulkanRenderPipeline::drawOntoOutput(VkCommandBuffer cmd,
                                          uint32_t globalsOffset,
                                          const std::function<void(IRenderPass &)> &callback,
                                          const char *label) {
    VulkanDebugScope scope(_renderer.device(), cmd, label, {0.7f, 0.4f, 0.7f});

    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _output->view();
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
                          true);
    pass.setGlobalsOffset(globalsOffset);
    callback(pass);

    vkCmdEndRendering(cmd);
}

void VulkanRenderPipeline::transparencyPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    auto callback = _passCallbacks.find(RenderPassName::TransparentGeometry);
    if (callback == _passCallbacks.end()) {
        return;
    }
    drawOntoOutput(cmd, globalsOffset, callback->second, "Transparent geometry");
}

/**
 * Whatever the scene draws over the finished image.
 *
 * Only lens flares at present - billboards blended additively with no depth
 * test. The filter chain that runs after this is filterChainPass.
 */
void VulkanRenderPipeline::postProcessingPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    auto callback = _passCallbacks.find(RenderPassName::PostProcessing);
    if (callback == _passCallbacks.end()) {
        return;
    }
    drawOntoOutput(cmd, globalsOffset, callback->second, "Post-processing");
}

/**
 * Move an image between being sampled and being drawn into.
 *
 * The filter chain flips both of its images between the two roles on every
 * pass, so this is called far more often than the one-way transitions
 * elsewhere in this file.
 */
static void transitionFilterImage(VkCommandBuffer cmd,
                                  const VulkanImage &image,
                                  VkImageLayout from,
                                  VkImageLayout to) {
    VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.dstStageMask = b.srcStageMask;
    b.dstAccessMask = b.srcAccessMask;
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

void VulkanRenderPipeline::filterPass(VkCommandBuffer cmd,
                                      uint32_t screenEffectOffset,
                                      VulkanImage &src,
                                      VulkanImage &dst,
                                      VkDescriptorSet srcSet,
                                      const char *fragmentEntry,
                                      const char *label) {
    VulkanDebugScope scope(_renderer.device(), cmd, label, {0.4f, 0.6f, 0.9f});

    transitionFilterImage(cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionFilterImage(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = dst.view();
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &srcSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

/**
 * Antialias and sharpen the finished image, as the OpenGL pipeline does.
 *
 * The result has to end up back in _output either way, because that is what
 * the frame composites, so a chain with an odd number of filters copies the
 * ping image back. That mirrors the blit OpenGL does for the same reason.
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
        filterPass(cmd, offset, *_output, *_ping, _outputAsSourceSet,
                   "fxaaFragment", "FXAA");
        filterPass(cmd, offset, *_ping, *_output, _pingAsSourceSet,
                   "sharpenFragment", "Sharpen");
        return;
    }
    if (_options.fxaa) {
        filterPass(cmd, offset, *_output, *_ping, _outputAsSourceSet,
                   "fxaaFragment", "FXAA");
    } else {
        filterPass(cmd, offset, *_output, *_ping, _outputAsSourceSet,
                   "sharpenFragment", "Sharpen");
    }

    // Odd chain: the result is in ping, and the frame expects it in output.
    transitionFilterImage(cmd, *_ping, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionFilterImage(cmd, *_output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource = region.srcSubresource;
    region.extent = {static_cast<uint32_t>(_targetSize.x),
                     static_cast<uint32_t>(_targetSize.y), 1};
    vkCmdCopyImage(cmd,
                   _ping->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   _output->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    transitionFilterImage(cmd, *_output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transitionFilterImage(cmd, *_ping, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

Texture &VulkanRenderPipeline::render() {
    auto cmd = _renderer.commandBuffer();

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
    _renderer.pbrTextures().process(cmd, globalsOffset);

    shadowPass(cmd, globalsOffset);
    geometryPass(cmd, globalsOffset);
    resolvePass(cmd, globalsOffset);
    transparencyPass(cmd, globalsOffset);
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
    toRead.image = _output->handle();
    toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRead.subresourceRange.levelCount = 1;
    toRead.subresourceRange.layerCount = 1;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toRead;
    vkCmdPipelineBarrier2(cmd, &dep);

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

void VulkanRenderPipeline::dumpTargets(const std::filesystem::path &dir) {
    if (!_inited) {
        return;
    }
    std::filesystem::create_directories(dir);

    struct Entry {
        const char *name;
        const VulkanImage *image;
        VkImageLayout layout;
        bool depth;
    };
    std::vector<Entry> entries;
    static const char *kColorNames[VulkanGBuffer::Count] = {
        "g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_lightmap",
        "g_buffer_self_illum", "g_buffer_motion"};
    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
        entries.push_back({kColorNames[i], &_gbuffer->color(i), _gbuffer->colorLayout(), false});
    }
    entries.push_back({"g_buffer_depth", &_gbuffer->depth(), _gbuffer->depthLayout(), true});
    // The output has been handed to the compositor by the time a dump runs.
    entries.push_back({"output", _output.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});

    for (const auto &entry : entries) {
        auto format = dumpFormatFor(entry.image->format());
        if (!format) {
            warn("Cannot dump target '" + std::string(entry.name) + "': unsupported format",
                 LogChannel::Graphics);
            continue;
        }
        auto raw = entry.image->readBack(entry.layout, entry.depth);
        auto extent = entry.image->extent();
        auto path = dir / (std::string(entry.name) + ".npy");
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
    info("Dumped " + std::to_string(entries.size()) + " render targets to " + dir.string(),
         LogChannel::Graphics);
}

std::vector<RenderTargetInfo> VulkanRenderPipeline::targets() const {
    // Nothing is exposed for inspection yet: the render target viewer is part of
    // the ImGui editor, which does not run on Vulkan.
    return {};
}

} // namespace scene

} // namespace reone
