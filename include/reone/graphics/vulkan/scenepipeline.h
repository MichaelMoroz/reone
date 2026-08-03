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
#include "reone/graphics/vulkan/gpuscene.h"

namespace reone::graphics {

class IMeshRegistry;
class Uniforms;
class VulkanImage;
class VulkanRenderer;
class TextureRegistry;
struct GraphicsOptions;

enum class VulkanSceneStep {
    ProcessPBRTextures,
    Geometry,
    PBRResolve,
    RetroResolve,
};

struct VulkanSceneFramePlan {
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
    virtual void renderPrimary(const VulkanPrimaryRayContext &context) = 0;
    virtual VulkanGpuScene::View mergeGeometry(VkCommandBuffer commandBuffer) = 0;
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
                        Uniforms &uniforms,
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
    Uniforms &_uniforms;
    IMeshRegistry &_meshRegistry;
    TextureRegistry &_textureRegistry;
    bool _inited {false};
    bool _primaryRayMode {false};

    std::unique_ptr<VulkanGBuffer> _gbuffer;
    std::unique_ptr<VulkanImage> _output;
    std::shared_ptr<Texture> _outputHandle;
    VkDescriptorSet _retroResolveSet {VK_NULL_HANDLE};
    VkDescriptorSet _pbrResolveSet {VK_NULL_HANDLE};

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

    void previewPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                     const IVulkanSceneCallbacks &callbacks);
    void geometryPass(VkCommandBuffer cmd, uint32_t globalsOffset,
                      IVulkanSceneCallbacks &callbacks);
    void retroResolvePass(VkCommandBuffer cmd, uint32_t globalsOffset);
    void pbrResolvePass(VkCommandBuffer cmd, uint32_t globalsOffset);
    std::vector<Target> targetEntries(const IVulkanSceneCallbacks &callbacks) const;
};

} // namespace reone::graphics
