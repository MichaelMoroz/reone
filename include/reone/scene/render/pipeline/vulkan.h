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

#pragma once

#include <volk.h>

#include "reone/graphics/texture.h"
#include "reone/graphics/vulkan/gbuffer.h"

#include "../pipeline.h"

namespace reone {

namespace graphics {

class IMeshRegistry;
class IUniforms;
class VulkanRenderer;

} // namespace graphics

namespace scene {

/**
 * The Vulkan scene pipeline.
 *
 * At present it runs one pass - opaque geometry into the G-buffer - and
 * resolves it. Shadows, transparency, SSAO, SSR and post-processing are
 * registered by the scene graph and dropped here; each will become a pass as
 * section 3.2 of the plan works through them.
 *
 * It records into the frame's command buffer rather than owning one, so the
 * scene passes and the GUI end up in the same submission. That means render()
 * must be called outside any active rendering scope: a render pass cannot be
 * begun inside another.
 */
class VulkanRenderPipeline : public IRenderPipeline, boost::noncopyable {
public:
    VulkanRenderPipeline(glm::ivec2 targetSize,
                         graphics::GraphicsOptions &options,
                         graphics::VulkanRenderer &renderer,
                         graphics::IUniforms &uniforms,
                         graphics::IMeshRegistry &meshRegistry) :
        _targetSize(std::move(targetSize)),
        _options(options),
        _renderer(renderer),
        _uniforms(uniforms),
        _meshRegistry(meshRegistry) {
    }

    ~VulkanRenderPipeline() { deinit(); }

    void init() override;
    void deinit();

    void reset() override {
        _passCallbacks.clear();
    }

    void inRenderPass(RenderPassName name, std::function<void(IRenderPass &)> callback) override {
        _passCallbacks[name] = std::move(callback);
    }

    graphics::Texture &render() override;

    std::vector<RenderTargetInfo> targets() const override;
    void dumpTargets(const std::filesystem::path &dir) override;

private:
    glm::ivec2 _targetSize;
    graphics::GraphicsOptions &_options;
    graphics::VulkanRenderer &_renderer;
    graphics::IUniforms &_uniforms;
    graphics::IMeshRegistry &_meshRegistry;

    bool _inited {false};
    std::unordered_map<RenderPassName, std::function<void(IRenderPass &)>> _passCallbacks;

    std::unique_ptr<graphics::VulkanGBuffer> _gbuffer;
    /** What the resolve writes, and what the frame composites. */
    std::unique_ptr<graphics::VulkanImage> _output;
    /** Four directional cascades, as a 2D array. */
    std::unique_ptr<graphics::VulkanImage> _dirShadows;
    /** Six faces of one point light, as a cube. */
    std::unique_ptr<graphics::VulkanImage> _pointShadows;
    VkImageLayout _dirShadowLayout {VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout _pointShadowLayout {VK_IMAGE_LAYOUT_UNDEFINED};
    /**
     * A Texture with no pixels, existing only so the output can cross the
     * `IRenderer::drawSceneOutput(Texture &)` seam. VulkanResources maps it back
     * to the image.
     */
    std::shared_ptr<graphics::Texture> _outputHandle;

    VkDescriptorSet _resolveSet {VK_NULL_HANDLE};

    void geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void shadowPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void transparencyPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void postProcessingPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void drawOntoOutput(VkCommandBuffer cmd,
                        uint32_t globalsOffset,
                        const std::function<void(IRenderPass &)> &callback);
    void resolvePass(VkCommandBuffer cmd, uint32_t globalsOffset);
};

} // namespace scene

} // namespace reone
