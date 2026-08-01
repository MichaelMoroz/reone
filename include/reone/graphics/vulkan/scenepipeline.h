/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <volk.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/gbuffer.h"

namespace reone::graphics {

class IMeshRegistry;
class IUniforms;
class VulkanImage;
class VulkanRenderer;
class VulkanRenderPass;
class TextureRegistry;
struct GraphicsOptions;

enum class VulkanSceneDraw {
    DirectionalShadow,
    PointShadow,
    Opaque,
    RetroOpaque,
    PostProcessing,
    LensFlare,
    Transparent,
};

enum class VulkanSceneStep {
    ProcessPBRTextures,
    Shadow,
    Geometry,
    RetroGeometry,
    ScreenSpaceEffects,
    Resolve,
    Transparency,
    OITBlend,
    PostProcessing,
    FilterChain,
};

enum class VulkanSceneShadow { None,
                               Directional,
                               Point };

struct VulkanSceneFramePlan {
    VulkanSceneShadow shadow {VulkanSceneShadow::None};
    std::vector<VulkanSceneStep> steps;
};

struct VulkanPrimaryRayContext {
    VkCommandBuffer commandBuffer {VK_NULL_HANDLE};
    uint32_t globalsOffset {0};
    VulkanImage *output {nullptr};
    glm::mat4 view {1.0f};
    glm::mat4 projection {1.0f};
    glm::vec4 jitter {0.0f};
};

struct VulkanExternalTarget {
    const char *name {nullptr};
    const char *dumpName {nullptr};
    VulkanImage *image {nullptr};
};

class IVulkanSceneCallbacks {
public:
    virtual ~IVulkanSceneCallbacks() = default;
    virtual void draw(VulkanSceneDraw draw, VulkanRenderPass &pass) = 0;
    virtual void renderPrimary(const VulkanPrimaryRayContext &context) = 0;
    virtual std::vector<VulkanExternalTarget> primaryTargets() const = 0;
};

enum class VulkanTargetKind { Color,
                              Depth,
                              EyeNormal,
                              Motion };
struct VulkanTargetInfo {
    std::string name;
    VulkanTargetKind kind {VulkanTargetKind::Color};
};

/** Owns native render targets, descriptors, barriers, pipelines and commands. */
class VulkanScenePipeline : boost::noncopyable {
public:
    VulkanScenePipeline(glm::ivec2 targetSize,
                        GraphicsOptions &options,
                        VulkanRenderer &renderer,
                        IUniforms &uniforms,
                        IMeshRegistry &meshRegistry,
                        TextureRegistry &textureRegistry,
                        bool primaryRayMode);
    ~VulkanScenePipeline();

    void init();
    void deinit();
    Texture &render(const VulkanSceneFramePlan &plan, IVulkanSceneCallbacks &callbacks);
    std::vector<VulkanTargetInfo> targets(const IVulkanSceneCallbacks &callbacks) const;
    void *renderTargetPreview(const std::string &name, int mode, float scale,
                              const IVulkanSceneCallbacks &callbacks);
    void dumpTargets(const std::filesystem::path &dir,
                     const IVulkanSceneCallbacks &callbacks);

private:
    glm::ivec2 _targetSize;
    GraphicsOptions &_options;
    VulkanRenderer &_renderer;
    IUniforms &_uniforms;
    IMeshRegistry &_meshRegistry;
    TextureRegistry &_textureRegistry;
    bool _inited {false};
    bool _primaryRayMode {false};
    VulkanSceneShadow _shadow {VulkanSceneShadow::None};

    std::unique_ptr<VulkanGBuffer> _gbuffer;
    std::unique_ptr<VulkanImage> _output;
    std::unique_ptr<VulkanImage> _dirShadows;
    std::unique_ptr<VulkanImage> _pointShadows;
    VkImageLayout _dirShadowLayout {VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout _pointShadowLayout {VK_IMAGE_LAYOUT_UNDEFINED};
    std::shared_ptr<Texture> _outputHandle;
    std::unique_ptr<VulkanImage> _ping;
    std::unique_ptr<VulkanImage> _hilights;
    std::unique_ptr<VulkanImage> _ssao;
    std::unique_ptr<VulkanImage> _ssr;
    std::unique_ptr<VulkanImage> _ssaoPing;
    std::unique_ptr<VulkanImage> _halfPing;
    std::array<glm::vec4, kNumSSAOSamples> _ssaoSamples;
    VulkanImage *_frameImage {nullptr};
    VulkanImage *_spareImage {nullptr};
    std::unique_ptr<VulkanImage> _oitAccum;
    std::unique_ptr<VulkanImage> _oitRevealage;

    VkDescriptorSet _resolveSet {VK_NULL_HANDLE};
    VkDescriptorSet _outputAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _pingAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _hilightsAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssaoSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssrSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssaoAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssrAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _ssaoPingAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _halfPingAsSourceSet {VK_NULL_HANDLE};
    VkDescriptorSet _oitBlendOutputSet {VK_NULL_HANDLE};
    VkDescriptorSet _oitBlendPingSet {VK_NULL_HANDLE};

    struct Preview {
        std::unique_ptr<VulkanImage> image;
        void *imguiTexture {nullptr};
        std::string target;
        int mode {0};
        float scale {1.0f};
    };
    std::unique_ptr<Preview> _preview;

    struct Target {
        const char *name;
        const char *dumpName;
        VulkanTargetKind kind;
        const VulkanImage *image;
        VkImageLayout layout;
        bool depth;
    };

    void shadowPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                    IVulkanSceneCallbacks &callbacks);
    void geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                      IVulkanSceneCallbacks &callbacks);
    void retroGeometryPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                           IVulkanSceneCallbacks &callbacks);
    void hilightsBlurPass(VkCommandBuffer cmd);
    void screenSpaceEffectsPass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void transparencyPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                          IVulkanSceneCallbacks &callbacks);
    void oitBlendPass(VkCommandBuffer cmd);
    void postProcessingPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                            IVulkanSceneCallbacks &callbacks);
    void filterChainPass(VkCommandBuffer cmd);
    void filterPass(VkCommandBuffer cmd, uint32_t screenEffectOffset,
                    const char *fragmentEntry, const char *label);
    void drawOntoOutput(VkCommandBuffer cmd, uint32_t globalsOffset,
                        VulkanSceneDraw draw, const char *label,
                        IVulkanSceneCallbacks &callbacks);
    void resolvePass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void previewPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                     const IVulkanSceneCallbacks &callbacks);
    std::vector<Target> targetEntries(const IVulkanSceneCallbacks &callbacks) const;
};

} // namespace reone::graphics
