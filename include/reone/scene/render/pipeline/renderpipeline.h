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

#include "reone/graphics/rendering/sky.h"

#include "../admission.h"
#include "../pipeline.h"

namespace reone::graphics {
class ICommandBuffer;
class IMeshRegistry;
class Uniforms;
class IRenderer;
class ScenePipeline;
class GpuScene;
} // namespace reone::graphics

namespace reone::scene {

class RayQueryPipeline;

/** Scene-side frame ordering and draw selection for the scene executor. */
class RenderPipeline : public IRenderPipeline, boost::noncopyable {
public:
    RenderPipeline(glm::ivec2 targetSize,
                         graphics::GraphicsOptions &options,
                         graphics::IRenderer &renderer,
                         graphics::Uniforms &uniforms,
                         graphics::IMeshRegistry &meshRegistry,
                         graphics::TextureRegistry &textureRegistry,
                         GpuScene &gpuScene,
                         bool primaryRayMode = false);
    ~RenderPipeline();

    void init() override;
    void deinit();
    graphics::Texture &render(const CameraSceneNode *camera,
                              const std::vector<RenderShadowCaster> &shadowCasters,
                              SceneOutputAlpha alpha) override;
    std::vector<RenderTargetInfo> targets() const override;
    void *renderTargetPreview(const std::string &name, int mode, float scale) override;
    void dumpTargets(const std::filesystem::path &dir) override;
    void dumpSceneRecords(const std::filesystem::path &dir) override;
    /** Served from the merge, where the triangle ranges have just been filled. */
    void serveRecordDump(const graphics::GpuScene::View &view);
    /** Where the next render should write its record tables, or empty. */
    std::filesystem::path _pendingRecordDump;
    void restartTemporalHistory() override;
    void setDebugOverlayShapes(std::vector<graphics::DebugOverlayShape> shapes) override {
        _overlayShapes = std::move(shapes);
    }

private:
    class Callbacks;

    /**
     * This frame's sky cube, baking the admitted sky room when it changes.
     *
     * Both middles reach it: the tracer binds it as an environment light and
     * the raster sky composite reads it as the picture. One instance, one
     * bake, one set of latches - the gather and its guards are here rather
     * than in either consumer so neither can drift from the other.
     */
    graphics::SkyBinding skyBinding(graphics::ICommandBuffer &commandBuffer);

    /**
     * Which object categories this mode lets into the shadow map.
     *
     * The decision lives here because only the scene layer knows what a
     * category is; the pass downstream applies the mask without interpreting
     * it.
     */
    uint32_t shadowCasterCategories() const;

    glm::ivec2 _targetSize;
    graphics::GraphicsOptions &_options;
    graphics::IRenderer &_renderer;
    graphics::Uniforms &_uniforms;
    graphics::IMeshRegistry &_meshRegistry;
    graphics::TextureRegistry &_textureRegistry;
    GpuScene &_gpuScene;
    bool _primaryRayMode {false};
    bool _inited {false};
    /** Debug-overlay boxes handed in by the scene graph for the next render. */
    std::vector<graphics::DebugOverlayShape> _overlayShapes;
    std::unique_ptr<graphics::ScenePipeline> _executor;
    std::unique_ptr<graphics::GpuScene> _deviceGpuScene;
    std::unique_ptr<GpuSceneAdmission> _admission;
    GpuSceneAdmissionResult _admissionResult;
    /** Frame-owned rather than tracer-owned: raster's sky composite is the next
        consumer of the same cube, and both middles run under this pipeline. */
    std::unique_ptr<graphics::Sky> _sky;
    std::unique_ptr<RayQueryPipeline> _rayQuery;
    uint64_t _lastUploadHash {0};
    uint32_t _lastMaterialReferences {0};
    uint32_t _lastMaterialCount {0};
    std::unique_ptr<Callbacks> _callbacks;
};

} // namespace reone::scene
