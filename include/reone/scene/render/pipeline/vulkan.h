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

#include "rayquery.h"
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
 * resolves it. Shadows, transparency, screen-space effects and post-processing
 * are recorded into the same command buffer as the scene graph.
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
                         graphics::IMeshRegistry &meshRegistry,
                         graphics::TextureRegistry &textureRegistry,
                         GpuScene &gpuScene,
                         bool primaryRayMode = false) :
        _targetSize(std::move(targetSize)),
        _options(options),
        _renderer(renderer),
        _uniforms(uniforms),
        _meshRegistry(meshRegistry),
        _textureRegistry(textureRegistry),
        _gpuScene(gpuScene),
        _primaryRayMode(primaryRayMode) {
    }

    ~VulkanRenderPipeline() { deinit(); }

    void init() override;
    void deinit();

    graphics::Texture &render(const CameraSceneNode *camera,
                              RenderPassName activeShadowPass,
                              const graphics::Frustum *shadowFrusta,
                              size_t numShadowFrusta) override;

    std::vector<RenderTargetInfo> targets() const override;
    void *renderTargetPreview(const std::string &name, int mode, float scale) override;
    void dumpTargets(const std::filesystem::path &dir) override;
    void restartTemporalHistory() override;

private:
    glm::ivec2 _targetSize;
    graphics::GraphicsOptions &_options;
    graphics::VulkanRenderer &_renderer;
    graphics::IUniforms &_uniforms;
    graphics::IMeshRegistry &_meshRegistry;
    graphics::TextureRegistry &_textureRegistry;
    GpuScene &_gpuScene;
    const CameraSceneNode *_cullCamera {nullptr};
    RenderPassName _shadowPass {RenderPassName::None};

    bool _inited {false};
    bool _primaryRayMode {false};

    std::unique_ptr<graphics::VulkanGBuffer> _gbuffer;
    std::unique_ptr<RayQueryPipeline> _rayQuery;
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
    /** Opaque shading writes the original renderer's highlight buffer here. */
    std::unique_ptr<graphics::VulkanImage> _hilights;
    std::unique_ptr<graphics::VulkanImage> _ssao;
    std::unique_ptr<graphics::VulkanImage> _ssr;
    std::unique_ptr<graphics::VulkanImage> _ssaoPing;
    std::unique_ptr<graphics::VulkanImage> _halfPing;
    std::array<glm::vec4, graphics::kNumSSAOSamples> _ssaoSamples;
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
    /** Unit 0 pointed at the highlight buffer, for blurring it in place. */
    VkDescriptorSet _hilightsAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssaoSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssrSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssaoAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssrAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssaoPingAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _halfPingAsSourceSet {VK_NULL_HANDLE};
    /** The OIT targets plus each possible scene-colour source. */
    VkDescriptorSet _oitBlendOutputSet {VK_NULL_HANDLE};
    VkDescriptorSet _oitBlendPingSet {VK_NULL_HANDLE};

    struct Preview {
        std::unique_ptr<graphics::VulkanImage> image;
        void *imguiTexture {nullptr};
        std::string target;
        int mode {0};
        float scale {1.0f};
    };
    std::unique_ptr<Preview> _preview;

    struct Target {
        const char *name;
        const char *dumpName;
        RenderTargetKind kind;
        const graphics::VulkanImage *image;
        VkImageLayout layout;
        bool depth;
    };

    void geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void retroGeometryPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    /** Separable blur of the retro highlight buffer, in place. Follows retroGeometryPass. */
    void hilightsBlurPass(VkCommandBuffer cmd);
    void screenSpaceEffectsPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void shadowPass(VkCommandBuffer cmd, uint32_t globalsOffset, VisibilityPolicy visibility);
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
                        RenderPassName passName,
                        const char *label);
    void resolvePass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void previewPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    std::vector<Target> targetEntries() const;
};

} // namespace scene

} // namespace reone
