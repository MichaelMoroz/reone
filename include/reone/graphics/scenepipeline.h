/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/commandbuffer.h"
#include "reone/graphics/vulkan/gbuffer.h"
#include "reone/graphics/gpuscene.h"

namespace reone::graphics {

class IMeshRegistry;
class Uniforms;
class VulkanImage;
class VulkanRenderer;
class TextureRegistry;
struct GraphicsOptions;

enum class VulkanSceneStep {
    ProcessPBRTextures,
    Shadow,
    Geometry,
    PBRResolve,
    RetroResolve,
    Blended,
};

enum class VulkanSceneShadow {
    None,
    Directional,
    Point,
};

struct VulkanSceneFramePlan {
    VulkanSceneShadow shadow {VulkanSceneShadow::None};
    std::vector<VulkanSceneStep> steps;
};

struct VulkanPrimaryRayContext {
    ICommandBuffer *commandBuffer {nullptr};
    uint32_t globalsOffset {0};
    IImage *output {nullptr};
    glm::mat4 view {1.0f};
    glm::mat4 projection {1.0f};
    glm::vec4 jitter {0.0f};
};

struct VulkanExternalTarget {
    const char *name {nullptr};
    const char *dumpName {nullptr};
    IImage *image {nullptr};
};

class IVulkanSceneCallbacks {
public:
    virtual ~IVulkanSceneCallbacks() = default;
    virtual void renderPrimary(const VulkanPrimaryRayContext &context) = 0;
    virtual GpuScene::View mergeGeometry(ICommandBuffer &commandBuffer) = 0;
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
class ScenePipeline : boost::noncopyable {
public:
    ScenePipeline(glm::ivec2 targetSize,
                        GraphicsOptions &options,
                        VulkanRenderer &renderer,
                        Uniforms &uniforms,
                        IMeshRegistry &meshRegistry,
                        TextureRegistry &textureRegistry,
                        bool primaryRayMode);
    ~ScenePipeline();

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
    VulkanSceneShadow _shadow {VulkanSceneShadow::None};

    std::unique_ptr<VulkanGBuffer> _gbuffer;
    std::unique_ptr<VulkanImage> _output;
    std::unique_ptr<VulkanImage> _dirShadows;
    std::unique_ptr<VulkanImage> _pointShadows;
    std::shared_ptr<Texture> _outputHandle;
    DescriptorSet _retroResolveSet;
    DescriptorSet _pbrResolveSet;
    DescriptorSet _resolveMaterialSet;
    GpuScene::View _mergedScene;
    bool _mergedScenePrepared {false};

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
        const IImage *image;
        ImageLayout layout;
        bool depth;
    };

    const GpuScene::View &prepareMergedScene(
        ICommandBuffer &cmd, IVulkanSceneCallbacks &callbacks);
    void shadowPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                    IVulkanSceneCallbacks &callbacks);
    void previewPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                     const IVulkanSceneCallbacks &callbacks);
    void geometryPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                      IVulkanSceneCallbacks &callbacks);
    void retroResolvePass(ICommandBuffer &cmd, uint32_t globalsOffset);
    /** G8: the transparent surfaces the G-buffer deliberately leaves out,
        drawn forward onto the resolved image in submission order. */
    void blendedPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                     IVulkanSceneCallbacks &callbacks);
    void pbrResolvePass(ICommandBuffer &cmd, uint32_t globalsOffset);
    std::vector<Target> targetEntries(const IVulkanSceneCallbacks &callbacks) const;
};

} // namespace reone::graphics
