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

#include "reone/graphics/vulkan/scenepipeline.h"

#include "reone/system/profiler.h"

#include "reone/graphics/dxtutil.h"
#include "reone/graphics/npyutil.h"
#include "reone/graphics/options.h"
#include "reone/graphics/textureregistry.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/debugscope.h"
#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/pbrtextures.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/pipeline.h"
#include "reone/graphics/vulkan/uniformring.h"
#include "reone/system/logutil.h"

#include "imgui_impl_vulkan.h"

#include <string_view>

using namespace reone::graphics;

namespace reone {

namespace graphics {

static constexpr char kPostProcessModule[] = "postprocess";

struct MegaDrawPushConstants {
    uint32_t triangleBase;
    uint32_t materialGated;
};

VulkanScenePipeline::VulkanScenePipeline(glm::ivec2 targetSize,
                                         GraphicsOptions &options,
                                         VulkanRenderer &renderer,
                                         Uniforms &uniforms,
                                         IMeshRegistry &meshRegistry,
                                         TextureRegistry &textureRegistry,
                                         bool primaryRayMode) :
    _targetSize(std::move(targetSize)),
    _options(options),
    _renderer(renderer),
    _uniforms(uniforms),
    _meshRegistry(meshRegistry),
    _textureRegistry(textureRegistry),
    _primaryRayMode(primaryRayMode) {
}

VulkanScenePipeline::~VulkanScenePipeline() {
    deinit();
}

/**
 * Move a colour image between its rendering and sampling roles.
 */
static void transitionColorImage(VkCommandBuffer cmd,
                                 const VulkanImage &image,
                                 VkImageLayout from,
                                 VkImageLayout to) {
    auto stageFor = [](VkImageLayout layout) -> VkPipelineStageFlags2 {
        switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        default:
            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    };
    auto accessFor = [](VkImageLayout layout) -> VkAccessFlags2 {
        switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return 0;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        default:
            return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
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

/** Move a layered shadow map between attachment and sampled layouts. */
static void transitionShadowMap(VkCommandBuffer cmd,
                                const VulkanImage &image,
                                VkImageLayout &from,
                                VkImageLayout to) {
    if (from == to) {
        return;
    }
    VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = from == VK_IMAGE_LAYOUT_UNDEFINED
                               ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0
                                : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image.handle();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    from = to;
}

void VulkanScenePipeline::init() {
    if (_inited) {
        return;
    }
    auto &device = _renderer.device();
    if (_primaryRayMode) {
        _output = std::make_unique<VulkanImage>(device);
        _output->initColorAttachment(_targetSize, _renderer.swapchain().imageFormat());
        _outputHandle = std::make_shared<Texture>(
            "vk_primary_ray_output", TextureType::TwoDim, Texture::Properties());
        _renderer.resources().registerExternal(*_outputHandle, *_output);
        _inited = true;
        return;
    }

    _gbuffer = std::make_unique<VulkanGBuffer>(device);
    _gbuffer->init(_targetSize);

    _output = std::make_unique<VulkanImage>(device);
    _output->initColorAttachment(_targetSize, _renderer.swapchain().imageFormat());

    glm::ivec2 shadowSize {_options.shadowResolution, _options.shadowResolution};
    _dirShadows = std::make_unique<VulkanImage>(device);
    _dirShadows->initDepthLayered(shadowSize, VulkanGBuffer::depthFormat(),
                                  kNumShadowCascades, false);
    _pointShadows = std::make_unique<VulkanImage>(device);
    _pointShadows->initDepthLayered(shadowSize, VulkanGBuffer::depthFormat(),
                                    kNumCubeFaces, true);

    auto &samplers = _renderer.resources().samplers();
    auto colorSampler = samplers.get(getTextureProperties(TextureUsage::ColorBuffer));
    auto depthSampler = samplers.get(getTextureProperties(TextureUsage::DepthBuffer));
    auto materialIdProperties = getTextureProperties(TextureUsage::ColorBuffer);
    materialIdProperties.minFilter = Texture::Filtering::Nearest;
    materialIdProperties.magFilter = Texture::Filtering::Nearest;
    auto materialIdSampler = samplers.get(materialIdProperties);
    _output->setSampler(colorSampler);
    _gbuffer->setSamplers(colorSampler, depthSampler, materialIdSampler);
    _dirShadows->setSampler(depthSampler);
    _pointShadows->setSampler(depthSampler);

    // Both resolve sets always bind both sampler shapes. Clear each target to
    // the far plane once so the inactive light kind is a valid no-shadow map.
    device.immediateSubmit([this, shadowSize](VkCommandBuffer cmd) {
        auto clear = [&](VulkanImage &image, VkImageLayout &layout,
                         int layers, bool cube) {
            transitionShadowMap(cmd, image, layout,
                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo depth {
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = cube ? image.renderView(0, 0) : image.view();
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = {
                static_cast<uint32_t>(shadowSize.x),
                static_cast<uint32_t>(shadowSize.y)};
            rendering.layerCount = 1;
            rendering.viewMask = (1u << layers) - 1u;
            rendering.pDepthAttachment = &depth;
            vkCmdBeginRendering(cmd, &rendering);
            vkCmdEndRendering(cmd);
            transitionShadowMap(cmd, image, layout,
                                VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        };
        clear(*_dirShadows, _dirShadowLayout, kNumShadowCascades, false);
        clear(*_pointShadows, _pointShadowLayout, kNumCubeFaces, true);
    });

    _retroResolveSet = _renderer.descriptors().createPersistentTextureSet(
        {{1, &_gbuffer->color(VulkanGBuffer::Diffuse)},
         {2, &_gbuffer->color(VulkanGBuffer::EyeNormal)},
         {3, &_gbuffer->color(VulkanGBuffer::Lightmap)},
         {4, &_gbuffer->color(VulkanGBuffer::SelfIllum)},
         {5, &_gbuffer->depth()},
         {15, _dirShadows.get()},
         {17, &_renderer.pbrTextures().prefilteredArray()},
         {19, _pointShadows.get()},
         {21, &_gbuffer->color(VulkanGBuffer::MaterialId)}});
    _pbrResolveSet = _renderer.descriptors().createPersistentTextureSet(
        {{1, &_gbuffer->color(VulkanGBuffer::Diffuse)},
         {2, &_gbuffer->color(VulkanGBuffer::EyeNormal)},
         {3, &_gbuffer->color(VulkanGBuffer::Lightmap)},
         {4, &_gbuffer->color(VulkanGBuffer::SelfIllum)},
         {5, &_gbuffer->depth()},
         {13, &_renderer.pbrTextures().brdfImage()},
         {15, _dirShadows.get()},
         {16, &_renderer.pbrTextures().irradianceArray()},
         {17, &_renderer.pbrTextures().prefilteredArray()},
         {19, _pointShadows.get()},
         {21, &_gbuffer->color(VulkanGBuffer::MaterialId)}});

    _outputHandle = std::make_shared<Texture>(
        "vk_scene_output", TextureType::TwoDim, Texture::Properties());
    _renderer.resources().registerExternal(*_outputHandle, *_output);

    device.immediateSubmit([this](VkCommandBuffer cmd) {
        transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_UNDEFINED,
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
        "G-buffer self-illum", "G-buffer motion", "G-buffer material ID"};
    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
        name(_gbuffer->color(i), kColorNames[i]);
    }
    name(_gbuffer->depth(), "G-buffer depth");
    name(*_dirShadows, "Shadow map (directional cascades)");
    name(*_pointShadows, "Shadow map (point cube)");
    name(*_output, "Scene output");

    _inited = true;
}
void VulkanScenePipeline::deinit() {
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
    _output.reset();
    _dirShadows.reset();
    _pointShadows.reset();
    _dirShadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    _pointShadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    _gbuffer.reset();
    _outputHandle.reset();
    _retroResolveSet = VK_NULL_HANDLE;
    _pbrResolveSet = VK_NULL_HANDLE;
    _resolveMaterialSet = VK_NULL_HANDLE;
    _mergedScene = {};
    _mergedScenePrepared = false;
    _inited = false;
}

const VulkanGpuScene::View &VulkanScenePipeline::prepareMergedScene(
    VkCommandBuffer cmd, IVulkanSceneCallbacks &callbacks) {
    if (_mergedScenePrepared) {
        return _mergedScene;
    }
    // Upload and compute-merge are recorded once before the first consumer.
    // VulkanGpuScene publishes the compute-to-vertex/index barrier; the same
    // buffers and descriptor set then feed shadows and the G-buffer.
    _mergedScene = callbacks.mergeGeometry(cmd);
    _mergedScenePrepared = true;
    _resolveMaterialSet = VK_NULL_HANDLE;
    if (!_mergedScene.vertices.buffer || _mergedScene.triangleCount == 0) {
        return _mergedScene;
    }
    const auto materialCount =
        _mergedScene.materials.size / sizeof(GpuSceneMaterial);
    if (materialCount > VulkanGBuffer::kNoMaterial) {
        warn("Vulkan: G-buffer R16_UINT material ID exhausted by " +
                 std::to_string(materialCount) +
                 " material records; refusing to wrap into the 0xffff sentinel",
             LogChannel::Graphics);
        throw std::runtime_error(
            "Vulkan: too many materials for the G-buffer material ID");
    }
    _resolveMaterialSet = _renderer.descriptors().updateMegaDrawSet(
        _renderer.frameIndex(), _mergedScene, _renderer.resources());
    return _mergedScene;
}

void VulkanScenePipeline::shadowPass(VkCommandBuffer cmd,
                                     uint32_t globalsOffset,
                                     IVulkanSceneCallbacks &callbacks) {
    R_PROFILE_ZONE("VulkanScenePipeline::shadowPass record");
    if (_shadow == VulkanSceneShadow::None) {
        return;
    }
    const bool directional = _shadow == VulkanSceneShadow::Directional;
    auto &image = directional ? *_dirShadows : *_pointShadows;
    auto &layout = directional ? _dirShadowLayout : _pointShadowLayout;
    const int layers = directional ? kNumShadowCascades : kNumCubeFaces;
    const uint32_t viewMask = (1u << layers) - 1u;
    const auto &scene = prepareMergedScene(cmd, callbacks);

    VulkanDebugScope scope(
        _renderer.device(), cmd,
        directional ? "Shadows (merged directional cascades)"
                    : "Shadows (merged point cube)",
        {0.2f, 0.2f, 0.5f});
    transitionShadowMap(cmd, image, layout,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo depth {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = directional ? image.view() : image.renderView(0, 0);
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    const auto extent = image.extent();
    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(extent.x),
                                   static_cast<uint32_t>(extent.y)};
    rendering.layerCount = 1;
    rendering.viewMask = viewMask;
    rendering.pDepthAttachment = &depth;
    VkViewport viewport {0.0f, 0.0f, static_cast<float>(extent.x),
                         static_cast<float>(extent.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(extent.x),
                               static_cast<uint32_t>(extent.y)}};

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (scene.vertices.buffer && scene.triangleCount != 0) {
        auto uniformSet = _renderer.uniformSet();
        std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        vkCmdBindIndexBuffer(cmd, scene.indices.buffer->handle(),
                             scene.indices.offset, VK_INDEX_TYPE_UINT32);

        auto drawRange = [&](uint32_t triangleBase, uint32_t triangleCount,
                             bool gated, FaceCullMode cull) {
            if (triangleCount == 0) {
                return;
            }
            VulkanPipelineCache::Key key;
            key.module = "shadow_megadraw";
            key.vertexEntry = directional ? "directionalShadowMegadrawVertex"
                                          : "pointShadowMegadrawVertex";
            key.fragmentEntry = directional
                                    ? "directionalShadowMegadrawFragment"
                                    : "pointShadowMegadrawFragment";
            key.depthFormat = VulkanGBuffer::depthFormat();
            key.viewMask = viewMask;
            key.depthTest = true;
            key.depthWrite = true;
            key.depthBias = true;
            // D32_SFLOAT constant bias is expressed in representable depth
            // increments; the slope term supplies the useful offset on curved
            // surfaces that approach parallel to the light.
            key.depthBiasConstantFactor = 0.0f;
            key.depthBiasSlopeFactor = 1.0f;
            key.cull = cull;
            auto &pipeline = _renderer.pipelines().get(key);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline.handle());
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                VulkanDescriptors::kUniformSet, 1, &uniformSet,
                static_cast<uint32_t>(offsets.size()), offsets.data());
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                VulkanDescriptors::kMegaDrawSet, 1, &_resolveMaterialSet,
                0, nullptr);
            const MegaDrawPushConstants push {triangleBase, gated ? 1u : 0u};
            vkCmdPushConstants(cmd, pipeline.layout(),
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdDrawIndexed(cmd, triangleCount * 3, 1,
                             triangleBase * 3, 0, 0);
        };

        // Back faces put the stored depth behind an opaque receiver and avoid
        // self-shadowing without changing the retained sampling code. Cutout
        // cards remain two-sided: a one-sided fence or leaf must cast from
        // either light direction, with its alpha gate still applied.
        drawRange(0, scene.opaqueTriangleCount, false, FaceCullMode::Front);
        const uint32_t gatedTriangles =
            scene.triangleCount - scene.opaqueTriangleCount;
        drawRange(scene.opaqueTriangleCount, gatedTriangles, true,
                  FaceCullMode::None);
    }
    vkCmdEndRendering(cmd);
    transitionShadowMap(cmd, image, layout,
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
}

void VulkanScenePipeline::geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                                       IVulkanSceneCallbacks &callbacks) {
    R_PROFILE_ZONE("VulkanScenePipeline::geometryPass record");
    VulkanDebugScope scope(_renderer.device(), cmd, "Merged geometry (G-buffer)",
                           {0.3f, 0.6f, 0.3f});

    const auto &scene = prepareMergedScene(cmd, callbacks);

    std::array<VkRenderingAttachmentInfo, VulkanGBuffer::Count> attachments {};
    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
        auto &attachment = attachments[i];
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = _gbuffer->color(i).view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    attachments[VulkanGBuffer::MaterialId].clearValue.color.uint32[0] =
        VulkanGBuffer::kNoMaterial;
    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = _gbuffer->depth().view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = VulkanGBuffer::Count;
    rendering.pColorAttachments = attachments.data();
    rendering.pDepthAttachment = &depth;
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

    if (scene.vertices.buffer && scene.triangleCount != 0) {
        VulkanPipelineCache::Key key;
        key.module = "megadraw";
        key.vertexEntry = "megadrawVertex";
        key.fragmentEntry = "megadrawFragment";
        key.colorFormats = VulkanGBuffer::colorFormats();
        key.depthFormat = VulkanGBuffer::depthFormat();
        key.depthTest = true;
        key.depthWrite = true;
        key.cull = FaceCullMode::None;
        auto &pipeline = _renderer.pipelines().get(key);
        auto uniformSet = _renderer.uniformSet();
        std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kUniformSet, 1, &uniformSet,
                                static_cast<uint32_t>(offsets.size()), offsets.data());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kMegaDrawSet, 1, &_resolveMaterialSet, 0,
                                nullptr);
        vkCmdBindIndexBuffer(cmd, scene.indices.buffer->handle(), scene.indices.offset,
                             VK_INDEX_TYPE_UINT32);

        if (scene.opaqueTriangleCount != 0) {
            const MegaDrawPushConstants push {0, 0};
            vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, scene.opaqueTriangleCount * 3, 1, 0, 0, 0);
        }
        const uint32_t nonOpaqueTriangles =
            scene.triangleCount - scene.opaqueTriangleCount;
        if (nonOpaqueTriangles != 0) {
            const MegaDrawPushConstants push {scene.opaqueTriangleCount, 1};
            vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, nonOpaqueTriangles * 3, 1,
                             scene.opaqueTriangleCount * 3, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
}

void VulkanScenePipeline::retroResolvePass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("VulkanScenePipeline::retroResolvePass record");
    VulkanDebugScope scope(_renderer.device(), cmd, "Retro deferred resolve",
                           {0.9f, 0.7f, 0.3f});

    _gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _output->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    VulkanPipelineCache::Key key;
    key.module = "retro_resolve";
    key.vertexEntry = "retroResolveVertex";
    key.fragmentEntry = "retroResolveFragment";
    key.colorFormats = {_output->format()};
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
                            VulkanDescriptors::kTextureSet, 1, &_retroResolveSet, 0, nullptr);
    if (_resolveMaterialSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kMegaDrawSet, 1, &_resolveMaterialSet, 0,
                                nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);

    transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanScenePipeline::pbrResolvePass(VkCommandBuffer cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("VulkanScenePipeline::pbrResolvePass record");
    VulkanDebugScope scope(_renderer.device(), cmd, "PBR deferred resolve",
                           {0.9f, 0.7f, 0.3f});

    _gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _output->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                   static_cast<uint32_t>(_targetSize.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    VulkanPipelineCache::Key key;
    key.module = "pbr_resolve";
    key.vertexEntry = "resolveVertex";
    key.fragmentEntry = "resolveFragment";
    key.colorFormats = {_output->format()};
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
                            VulkanDescriptors::kTextureSet, 1, &_pbrResolveSet, 0, nullptr);
    if (_resolveMaterialSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                VulkanDescriptors::kMegaDrawSet, 1, &_resolveMaterialSet, 0,
                                nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);

    transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

Texture &VulkanScenePipeline::render(const VulkanSceneFramePlan &plan,
                                     IVulkanSceneCallbacks &callbacks) {
    auto cmd = _renderer.commandBuffer();
    _shadow = plan.shadow;
    _mergedScene = {};
    _mergedScenePrepared = false;
    // The scene graph stores the frame's Vulkan-native uniform values here;
    // copy them into this frame's arena. Clip-space y is still handled by the
    // flipped viewport so triangle winding remains unchanged.
    auto globals = _uniforms.globals();
    auto globalsOffset = _renderer.uniformRing().push(globals);

    if (_primaryRayMode) {
        VkImageMemoryBarrier2 toGeneral {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.image = _output->handle();
        toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGeneral.subresourceRange.levelCount = 1;
        toGeneral.subresourceRange.layerCount = 1;
        VkDependencyInfo beginDep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        beginDep.imageMemoryBarrierCount = 1;
        beginDep.pImageMemoryBarriers = &toGeneral;
        vkCmdPipelineBarrier2(cmd, &beginDep);
        callbacks.renderPrimary(
            {cmd, globalsOffset, _output.get(), globals.view, globals.projection, globals.jitter});
        VkImageMemoryBarrier2 toRead {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.image = _output->handle();
        toRead.subresourceRange = toGeneral.subresourceRange;
        VkDependencyInfo endDep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        endDep.imageMemoryBarrierCount = 1;
        endDep.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &endDep);
        // The traced image is sampleable by now, so the preview can read it
        // like any other target. Without this the window would offer a target
        // it never draws, which is only marginally better than crashing.
        previewPass(cmd, globalsOffset, callbacks);
        _renderer.resources().registerExternal(*_outputHandle, *_output);
        return *_outputHandle;
    }

    bool outputResolved = false;
    for (const auto step : plan.steps) {
        switch (step) {
        case VulkanSceneStep::ProcessPBRTextures:
            _renderer.pbrTextures().process(cmd, globalsOffset);
            break;
        case VulkanSceneStep::Shadow:
            shadowPass(cmd, globalsOffset, callbacks);
            break;
        case VulkanSceneStep::Geometry:
            geometryPass(cmd, globalsOffset, callbacks);
            break;
        case VulkanSceneStep::PBRResolve:
            pbrResolvePass(cmd, globalsOffset);
            outputResolved = true;
            break;
        case VulkanSceneStep::RetroResolve:
            retroResolvePass(cmd, globalsOffset);
            outputResolved = true;
            break;
        }
    }

    // An empty raster plan is a supported degenerate frame. Keep the output
    // alive as a regular attachment, clear it to black, then publish it in the
    // layout the 2D compositor samples.
    if (!outputResolved) {
        transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = _output->view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {static_cast<uint32_t>(_targetSize.x),
                                       static_cast<uint32_t>(_targetSize.y)};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdEndRendering(cmd);
        transitionColorImage(cmd, *_output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        _gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        _gbuffer->transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    }

    previewPass(cmd, globalsOffset, callbacks);

    _renderer.resources().registerExternal(*_outputHandle, *_output);
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
    case VK_FORMAT_R16_UINT:
        return DumpFormat {1, NpyType::UInt16, false};
    case VK_FORMAT_R16_SFLOAT:
        return DumpFormat {1, NpyType::Float32, true};
    case VK_FORMAT_R16G16_SFLOAT:
        return DumpFormat {2, NpyType::Float32, true};
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return DumpFormat {4, NpyType::Float32, true};
    case VK_FORMAT_R32_SFLOAT:
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

std::vector<VulkanScenePipeline::Target> VulkanScenePipeline::targetEntries(
    const IVulkanSceneCallbacks &callbacks) const {
    if (!_inited) {
        return {};
    }
    std::vector<Target> entries;
    // Report what this mode actually produced, not what the pipeline can
    // produce in general. Tracing returns from render() before any raster pass
    // and init() returns before the G-buffer is even allocated, so describing
    // the G-buffer here dereferenced a null _gbuffer the moment the render
    // target window was opened.
    if (_primaryRayMode) {
        entries.push_back({"Traced output", "traced_output", VulkanTargetKind::Color,
                           _output.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
        // The split behind that image. Without these a traced frame can only
        // be judged as a whole, which cannot separate a noisy channel from a
        // denoiser that is not clearing it. They live in GENERAL: the trace
        // and composite passes read and write them as storage images and
        // nothing transitions them afterwards.
        for (const auto &channel : callbacks.primaryTargets()) {
                entries.push_back({channel.name, channel.dumpName, VulkanTargetKind::Color,
                                   channel.image, VK_IMAGE_LAYOUT_GENERAL, false});
        }
        return entries;
    }
    static const char *kDisplayNames[VulkanGBuffer::Count] = {
        "G-buffer diffuse", "G-buffer eye normal", "G-buffer lightmap",
        "G-buffer self-illum", "G-buffer motion", "G-buffer material ID"};
    static const char *kDumpNames[VulkanGBuffer::Count] = {
        "g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_lightmap",
        "g_buffer_self_illum", "g_buffer_motion", "g_buffer_material_id"};
    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
        auto kind = i == VulkanGBuffer::EyeNormal ? VulkanTargetKind::EyeNormal : i == VulkanGBuffer::Motion ? VulkanTargetKind::Motion
                                                                                                             : VulkanTargetKind::Color;
        entries.push_back({kDisplayNames[i], kDumpNames[i], kind, &_gbuffer->color(i),
                           _gbuffer->colorLayout(), false});
    }
    entries.push_back({"G-buffer depth", "g_buffer_depth", VulkanTargetKind::Depth,
                       &_gbuffer->depth(), _gbuffer->depthLayout(), true});
    entries.push_back({"Output", "output", VulkanTargetKind::Color,
                       _output.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false});
    return entries;
}

void *VulkanScenePipeline::renderTargetPreview(const std::string &name, int mode, float scale,
                                               const IVulkanSceneCallbacks &callbacks) {
    auto entries = targetEntries(callbacks);
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

void VulkanScenePipeline::previewPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                                      const IVulkanSceneCallbacks &callbacks) {
    R_PROFILE_ZONE("VulkanScenePipeline::previewPass record");
    if (!_preview) {
        return;
    }
    auto entries = targetEntries(callbacks);
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

void VulkanScenePipeline::dumpTargets(const std::filesystem::path &dir,
                                      const IVulkanSceneCallbacks &callbacks) {
    if (!_inited) {
        return;
    }
    std::filesystem::create_directories(dir);

    auto entries = targetEntries(callbacks);

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
        const std::string_view dumpName(entry.dumpName);
        if (dumpName == "g_buffer_depth" && entry.depth) {
            // A depth attachment is a projective device-depth value. Dumps
            // compare scene representations, so publish positive linear
            // view-space distance in world units, matching the traced target.
            const float near = _uniforms.globals().clipNear;
            const float far = _uniforms.globals().clipFar;
            const size_t count = raw.size() / sizeof(float);
            std::vector<float> linearDepth(count);
            for (size_t i = 0; i < count; ++i) {
                float deviceDepth;
                std::memcpy(&deviceDepth, raw.data() + i * sizeof(float), sizeof(deviceDepth));
                // Scene projections are built in Vulkan's [0,1] depth range,
                // so this is the Vulkan form, not OpenGL's 2*n*f denominator.
                linearDepth[i] = near * far /
                                 std::max(far - deviceDepth * (far - near), 1e-6f);
            }
            writeNpy(dir / "g_buffer_depth.npy", linearDepth.data(), extent.x, extent.y, 1,
                     NpyType::Float32);
            continue;
        }
        const bool yCoCgRadiance = dumpName == "traced_diffuse" ||
                                   dumpName == "traced_specular" ||
                                   dumpName == "denoised_diffuse" ||
                                   dumpName == "denoised_specular";
        auto path = dir / (std::string(entry.dumpName) + ".npy");
        if (format->halfToFloat) {
            size_t count = raw.size() / sizeof(uint16_t);
            std::vector<float> widened(count);
            for (size_t i = 0; i < count; ++i) {
                uint16_t half;
                std::memcpy(&half, raw.data() + i * sizeof(uint16_t), sizeof(half));
                widened[i] = halfToFloat(half);
            }
            // NRD consumes its radiance targets in YCoCg, but diagnostics use
            // one colour space across every dumped target.
            if (yCoCgRadiance) {
                for (size_t i = 0; i + 3 < widened.size(); i += 4) {
                    const float y = widened[i];
                    const float co = widened[i + 1];
                    const float cg = widened[i + 2];
                    const float t = y - cg * 0.5f;
                    widened[i] = std::max(t - co * 0.5f + co, 0.0f);
                    widened[i + 1] = std::max(cg + t, 0.0f);
                    widened[i + 2] = std::max(t - co * 0.5f, 0.0f);
                }
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

        auto dumpSourceEnvMap = [&dir, this](int layer, const Texture &texture) {
            if (!texture.is2D() && !texture.isCubeMap()) {
                warn("Cannot dump environment source '" + texture.name() +
                         "': unsupported texture shape",
                     LogChannel::Graphics);
                return;
            }
            const auto &image = _renderer.resources().get(texture);
            auto format = dumpFormatFor(image.format());
            bool compressed = image.format() == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
                              image.format() == VK_FORMAT_BC3_UNORM_BLOCK;
            // The OpenGL counterpart explicitly widens every source to RGBA8.
            // Decode BC sources to that same layout before writing the dump.
            if ((!format || format->channels != 4 || format->type != NpyType::UInt8) && !compressed) {
                warn("Cannot dump environment source '" + texture.name() +
                         "': unsupported Vulkan format",
                     LogChannel::Graphics);
                return;
            }
            uint32_t layers = texture.isCubeMap() ? kNumCubeFaces : 1;
            for (int mip = 0; mip < image.mipLevels(); ++mip) {
                auto raw = image.readBack(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mip, layers);
                auto extent = glm::max(glm::ivec2(1), image.extent() >> mip);
                if (compressed) {
                    size_t blockBytes = image.format() == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16;
                    size_t faceBytes = static_cast<size_t>((extent.x + 3) / 4) *
                                       ((extent.y + 3) / 4) * blockBytes;
                    std::vector<uint8_t> decoded(static_cast<size_t>(extent.x) * extent.y * layers * 4);
                    std::vector<uint32_t> pixels(static_cast<size_t>(extent.x) * extent.y);
                    for (uint32_t face = 0; face < layers; ++face) {
                        if (image.format() == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) {
                            decompressDXT1(extent.x, extent.y, raw.data() + face * faceBytes,
                                           pixels.data());
                        } else {
                            decompressDXT5(extent.x, extent.y, raw.data() + face * faceBytes,
                                           pixels.data());
                        }
                        for (size_t i = 0; i < pixels.size(); ++i) {
                            auto pixel = pixels[i];
                            auto *dst = decoded.data() + (static_cast<size_t>(face) * pixels.size() + i) * 4;
                            dst[0] = (pixel >> 24) & 0xff;
                            dst[1] = (pixel >> 16) & 0xff;
                            dst[2] = (pixel >> 8) & 0xff;
                            dst[3] = image.format() == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 0xff : pixel & 0xff;
                        }
                    }
                    raw = std::move(decoded);
                }
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
                auto name = "environment_map_layer" + std::to_string(layer) +
                            "_mip" + std::to_string(mip);
                writeNpy(dir / (name + ".npy"), flipped.data(), extent.x,
                         extent.y * static_cast<int>(layers), 4, NpyType::UInt8);
            }
        };
        for (const auto &[layer, texture] : pbr.sourceEnvMaps()) {
            dumpSourceEnvMap(layer, *texture);
        }
    }
    info("Dumped " + std::to_string(entries.size()) + " render targets to " + dir.string(),
         LogChannel::Graphics);
}

std::vector<VulkanTargetInfo> VulkanScenePipeline::targets(
    const IVulkanSceneCallbacks &callbacks) const {
    std::vector<VulkanTargetInfo> result;
    for (const auto &entry : targetEntries(callbacks)) {
        result.push_back({entry.name, entry.kind});
    }
    return result;
}

} // namespace graphics

} // namespace reone
