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
    /** The first of two stable scene-colour allocations. */
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

    /** The second of two stable scene-colour allocations. */
    std::unique_ptr<graphics::VulkanImage> _ping;
    /**
     * The image containing the latest complete scene colour, and the image
     * available for the next full-screen pass.
     *
     * Keeping this state separate from ownership makes every pass publish its
     * result directly. Presentation therefore does not have to reconstruct the
     * answer from how many passes happened to run.
     */
    graphics::VulkanImage *_frameImage {nullptr};
    graphics::VulkanImage *_spareImage {nullptr};

    /**
     * Weighted-blended transparency, as the OpenGL pipeline accumulates it:
     * rgb of the first is the weighted colour sum and its alpha is revealage,
     * and the second holds the sum of the weights. oitBlendPass divides one by
     * the other and composites the result onto the opaque image.
     */
    std::unique_ptr<graphics::VulkanImage> _oitAccum;
    std::unique_ptr<graphics::VulkanImage> _oitRevealage;

    VkDescriptorSet _resolveSet {VK_NULL_HANDLE};
    /** Unit 0 pointed at the two stable scene-colour allocations. */
    VkDescriptorSet _outputAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _pingAsSourceSet {VK_NULL_HANDLE};
    /** The OIT targets plus each possible scene-colour source. */
    VkDescriptorSet _oitBlendOutputSet {VK_NULL_HANDLE};
    VkDescriptorSet _oitBlendPingSet {VK_NULL_HANDLE};

    void geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void shadowPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void transparencyPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    /** Resolve the OIT targets onto the opaque image. Follows transparencyPass. */
    void oitBlendPass(VkCommandBuffer cmd);
    void postProcessingPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void filterChainPass(VkCommandBuffer cmd);
    /** Apply one full-screen filter and publish its destination as the frame. */
    void filterPass(VkCommandBuffer cmd,
                    uint32_t screenEffectOffset,
                    const char *fragmentEntry,
                    const char *label);
    void drawOntoOutput(VkCommandBuffer cmd,
                        uint32_t globalsOffset,
                        const std::function<void(IRenderPass &)> &callback,
                        const char *label);
    void resolvePass(VkCommandBuffer cmd, uint32_t globalsOffset);
};

} // namespace scene

} // namespace reone
