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
#include "reone/graphics/vulkan/descriptors.h"
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

void VulkanRenderPipeline::init() {
    if (_inited) {
        return;
    }
    auto &device = _renderer.device();

    _gbuffer = std::make_unique<VulkanGBuffer>(device);
    _gbuffer->init(_targetSize);

    _output = std::make_unique<VulkanImage>(device);
    _output->initColorAttachment(_targetSize, _renderer.swapchain().imageFormat());

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
    _resolveSet = _renderer.descriptors().createPersistentTextureSet(resolveTextures);

    // The output is written as an attachment and then sampled by the 2D
    // compositor, so it starts in the layout the first pass expects.
    device.immediateSubmit([this](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = _output->handle();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    });

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
    _gbuffer.reset();
    _outputHandle.reset();
    _inited = false;
}

void VulkanRenderPipeline::geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    auto callback = _passCallbacks.find(RenderPassName::OpaqueGeometry);
    if (callback == _passCallbacks.end()) {
        return;
    }

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
                          _meshRegistry,
                          cmd,
                          VulkanGBuffer::colorFormats(),
                          VulkanGBuffer::depthFormat());
    pass.setGlobalsOffset(globalsOffset);
    callback->second(pass);

    vkCmdEndRendering(cmd);
}

void VulkanRenderPipeline::resolvePass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    // Attachments become textures.
    _gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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

void VulkanRenderPipeline::transparencyPass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    auto callback = _passCallbacks.find(RenderPassName::TransparentGeometry);
    if (callback == _passCallbacks.end()) {
        return;
    }

    // Forward, not deferred. Transparent surfaces have no single depth to
    // resolve lighting at, so they blend straight onto the resolved image,
    // depth-tested against the opaque geometry but writing no depth of their
    // own - which is also what leaves the emitter's own back-to-front ordering
    // in charge of how overlapping particles stack.
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
                          _meshRegistry,
                          cmd,
                          {_renderer.swapchain().imageFormat()},
                          VulkanGBuffer::depthFormat(),
                          true);
    pass.setGlobalsOffset(globalsOffset);
    callback->second(pass);

    vkCmdEndRendering(cmd);
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
    auto globalsOffset = _renderer.uniformRing().push(globals);

    geometryPass(cmd, globalsOffset);
    resolvePass(cmd, globalsOffset);
    transparencyPass(cmd, globalsOffset);

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
