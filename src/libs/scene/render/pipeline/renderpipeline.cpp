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
#include "reone/scene/render/pipeline/renderpipeline.h"

#include <iomanip>
#include <sstream>

#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rendering/rayquery.h"
#include "reone/graphics/rendering/gpuscene.h"
#include "reone/graphics/rendering/scenepipeline.h"
#include "reone/graphics/rendering/sky.h"
#include "reone/scene/node/model.h"
#include "reone/scene/render/pipeline/rayquery.h"
#include "reone/system/logutil.h"

namespace reone::scene {

class RenderPipeline::Callbacks : public graphics::ISceneCallbacks {
public:
    explicit Callbacks(RenderPipeline &owner) : _owner(owner) {}

    void renderPrimary(const graphics::PrimaryRayContext &context) override {
        if (!_owner._rayQuery)
            return;
        // Ordered: the bake records before the trace that samples the cube,
        // and the admission result is still intact when the gather reads it.
        const auto sky = _owner.skyBinding(*context.commandBuffer);
        _owner._rayQuery->render(context, std::move(_owner._admissionResult), sky);
    }

    graphics::SkyBinding prepareSky(graphics::ICommandBuffer &commandBuffer) override {
        return _owner.skyBinding(commandBuffer);
    }

    graphics::GpuScene::View mergeGeometry(graphics::ICommandBuffer &commandBuffer) override {
        return _owner._deviceGpuScene->update(commandBuffer,
                                              _owner._admissionResult.submission.upload);
    }

    std::vector<graphics::ExternalTarget> primaryTargets() const override {
        std::vector<graphics::ExternalTarget> result;
        if (!_owner._rayQuery)
            return result;
        for (const auto &channel : _owner._rayQuery->native().channels())
            result.push_back({channel.name, channel.dumpName, channel.image});
        return result;
    }

private:
    RenderPipeline &_owner;
};

RenderPipeline::RenderPipeline(glm::ivec2 targetSize,
                                           graphics::GraphicsOptions &options,
                                           graphics::IRenderer &renderer,
                                           graphics::Uniforms &uniforms,
                                           graphics::IMeshRegistry &meshRegistry,
                                           graphics::TextureRegistry &textureRegistry,
                                           GpuScene &gpuScene,
                                           bool primaryRayMode) :
    _targetSize(std::move(targetSize)),
    _options(options),
    _renderer(renderer),
    _uniforms(uniforms),
    _meshRegistry(meshRegistry),
    _textureRegistry(textureRegistry),
    _gpuScene(gpuScene),
    _primaryRayMode(primaryRayMode) {
}

RenderPipeline::~RenderPipeline() {
    deinit();
}

void RenderPipeline::init() {
    if (_inited)
        return;
    _executor = std::make_unique<graphics::ScenePipeline>(
        _targetSize, _options, _renderer, _uniforms, _meshRegistry,
        _textureRegistry, _primaryRayMode);
    _executor->init();
    _deviceGpuScene = std::make_unique<graphics::GpuScene>();
    _deviceGpuScene->init(_renderer);
    _admission = std::make_unique<GpuSceneAdmission>(_renderer, _options, _gpuScene);
    // Every mode: the tracer samples the cube as an environment light and the
    // raster modes composite it as the picture, from one bake.
    _sky = std::make_unique<graphics::Sky>(_renderer);
    _sky->init();
    if (_primaryRayMode) {
        _rayQuery = std::make_unique<RayQueryPipeline>(
            _renderer, _targetSize, _options);
        _rayQuery->init();
    }
    _callbacks = std::make_unique<Callbacks>(*this);
    _inited = true;
}

void RenderPipeline::deinit() {
    if (!_inited)
        return;
    _callbacks.reset();
    if (_rayQuery)
        _rayQuery->deinit();
    _rayQuery.reset();
    // The cube and its six depth targets are VMA allocations, released here at
    // the point the tracer used to release them - after its consumer is gone
    // and well before the renderer takes the allocator down.
    if (_sky)
        _sky->deinit();
    _sky.reset();
    _admission.reset();
    if (_deviceGpuScene)
        _deviceGpuScene->deinit();
    _deviceGpuScene.reset();
    _executor.reset();
    _inited = false;
}

uint32_t RenderPipeline::shadowCasterCategories() const {
    if (_options.mode != graphics::RenderMode::Retro) {
        // The corrected renderer shadows the scene it actually lights, so
        // everything opaque casts.
        return graphics::kAllShadowCasters;
    }
    // Retro casts characters and what they carry, and nothing else, because
    // that is all the original ever drew: a stencil volume per creature, no
    // shadow from architecture or terrain at all. It is also what removes the
    // mottling on distant hills at the root - that was terrain shadowing
    // itself across cascade texels, and a terrain that never casts cannot.
    // Equipment is in because a weapon or mask is part of the silhouette the
    // creature it hangs on projects, not a separate object in the world.
    const auto category = [](ModelUsage usage) {
        return 1u << static_cast<uint32_t>(usage);
    };
    return category(ModelUsage::Creature) | category(ModelUsage::Equipment);
}

graphics::SkyBinding RenderPipeline::skyBinding(graphics::ICommandBuffer &commandBuffer) {
    if (!_sky)
        return {};
    bool skyBaked = false;
    if (_admissionResult.skyRoom) {
        graphics::RayQuerySkyRoom bake;
        bake.identity = reinterpret_cast<uint64_t>(_admissionResult.skyRoom);
        bake.name = _admissionResult.skyRoom->model().name();
        bake.origin = _admissionResult.skyOrigin;
        bool valid = true;
        for (const auto &object : _gpuScene.objects()) {
            const auto *mesh = std::get_if<RegisteredMesh>(&object);
            if (!mesh || mesh->cullRoot != _admissionResult.skyRoom ||
                !_gpuScene.isObjectEnabled(mesh->id.index)) {
                continue;
            }
            if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                     renderCategory(RenderCategory::Transparent))) == 0) {
                continue;
            }
            if (!std::holds_alternative<std::monostate>(mesh->deformation)) {
                valid = false;
                break;
            }
            const auto *texture = mesh->material.textures[static_cast<size_t>(graphics::MaterialTextureSlot::MainTex)];
            if (!texture || !_sky->supportsSkyTexture(*texture)) {
                valid = false;
                break;
            }
            bake.meshes.push_back({&mesh->mesh.get(), texture, mesh->transform,
                                   mesh->transformInv, mesh->prevTransform,
                                   mesh->material.uv});
        }
        if (!valid)
            bake.meshes.clear();
        // A valid room with an empty gather is not a failed room - it is a room
        // whose meshes have not activated yet. Loading from a save staggers room
        // visibility, so the first frames here can see zero enabled shell
        // meshes; attempting the bake then would latch bakeSkyRoom's sticky
        // per-room failure and leave the fallback cube - no sky, no sun - for
        // the whole session. Skip the attempt and retry next frame; only a
        // genuinely invalid room (unsupported texture, deforming shell) is
        // handed over empty so the stickiness still applies to it.
        if (valid && bake.meshes.empty()) {
            skyBaked = false;
        } else {
            try {
                skyBaked = _sky->bakeSkyRoom(commandBuffer, bake);
            } catch (const std::exception &e) {
                warn("Sky bake failed for '" + _admissionResult.skyRoom->model().name() +
                         "': " + e.what() + "; using fallback cube",
                     LogChannel::Graphics);
            }
        }
    } else {
        _sky->clearSkyRoom();
    }
    return _sky->binding(skyBaked);
}

graphics::Texture &RenderPipeline::render(const CameraSceneNode *camera,
                                                RenderShadowKind shadow) {
    graphics::SceneFramePlan plan;
    plan.shadowCasterCategories = shadowCasterCategories();
    switch (shadow) {
    case RenderShadowKind::Directional:
        plan.shadow = graphics::SceneShadow::Directional;
        break;
    case RenderShadowKind::Point:
        plan.shadow = graphics::SceneShadow::Point;
        break;
    default:
        plan.shadow = graphics::SceneShadow::None;
        break;
    }
    auto uploadArena = std::move(_admissionResult.submission.upload);
    _admissionResult = _admission->prepare(
        _uniforms.globals().view, std::move(uploadArena));
    _lastUploadHash = _admissionResult.uploadHash;
    _lastMaterialReferences =
        _admissionResult.submission.upload.materialReferenceCount;
    _lastMaterialCount =
        static_cast<uint32_t>(_admissionResult.submission.upload.materials.size());
    if (_primaryRayMode) {
        plan.steps.push_back(graphics::SceneStep::Geometry);
    } else {
        plan.steps.push_back(graphics::SceneStep::ProcessPBRTextures);
        plan.steps.push_back(graphics::SceneStep::Shadow);
        plan.steps.push_back(graphics::SceneStep::Geometry);
        // Anything that is not retro resolves with PBR. That covers the traced
        // mode reaching this branch, which it does on a device that cannot
        // trace: the factory falls back to a raster pipeline, and falling back
        // to the ORIGINAL's lighting model would be a second, unasked-for
        // change of renderer.
        const bool pbr = _options.mode != graphics::RenderMode::Retro;
        if (pbr)
            plan.steps.push_back(graphics::SceneStep::PBRResolve);
        else
            plan.steps.push_back(graphics::SceneStep::RetroResolve);
        // The sky is no longer a step: both resolves shade it themselves at the
        // pixels nothing covered, from one shared function. The bake that fills
        // its cube still runs, outside any pass, at the top of the frame.
        //
        // Screen-space reflections are PBR's alone. Retro is the original's
        // model and the original had none; giving it reflections it never had
        // would be an improvement in the one mode that exists not to improve.
        // Off, the step is not appended at all, so the frame is untouched
        // rather than passed through a kernel that decides to change nothing.
        if (pbr && _options.ssr)
            plan.steps.push_back(graphics::SceneStep::ScreenSpaceReflections);
    }
    // The common tail. Anti-aliasing resolves the opaque image, transparency
    // is drawn over the result, and the single display transform closes the
    // frame. A traced debug view is excluded from all of it because it is not
    // a picture - the kernel writes a diagnostic that is already
    // display-referred, and filtering or transforming it would change the
    // values it exists to show.
    const bool diagnosticImage = _primaryRayMode && _options.ptDebugView != 0;
    if (_options.antialiasing != graphics::AntiAliasing::None && !diagnosticImage)
        plan.steps.push_back(graphics::SceneStep::AntiAliasing);
    if (!_primaryRayMode) {
        // After the resolve, not before it. A temporal resolve cannot
        // reproject a billboard - transparency writes no motion and no depth,
        // so drawn ahead of one it smears behind the camera. It also keeps the
        // depth the resolve reads free of transparency by construction.
        // Raster only: in the traced mode additive sprites belong to the march
        // and drawing them here would double them.
        plan.steps.push_back(graphics::SceneStep::Blended);
    }
    if (_options.post && !diagnosticImage)
        plan.steps.push_back(graphics::SceneStep::PostProcess);
    return _executor->render(plan, *_callbacks);
}

std::vector<RenderTargetInfo> RenderPipeline::targets() const {
    std::vector<RenderTargetInfo> result;
    for (const auto &target : _executor->targets(*_callbacks)) {
        RenderTargetKind kind = RenderTargetKind::Color;
        switch (target.kind) {
        case graphics::TargetKind::Depth: kind = RenderTargetKind::Depth; break;
        case graphics::TargetKind::EyeNormal: kind = RenderTargetKind::EyeNormal; break;
        case graphics::TargetKind::Motion: kind = RenderTargetKind::Motion; break;
        default: break;
        }
        result.push_back({target.name, kind, nullptr});
    }
    return result;
}

void *RenderPipeline::renderTargetPreview(const std::string &name,
                                                int mode,
                                                float scale) {
    return _executor->renderTargetPreview(name, mode, scale, *_callbacks);
}

void RenderPipeline::dumpTargets(const std::filesystem::path &dir) {
    info("Scene contents: " + formatSceneCounts(_gpuScene.counts()),
         LogChannel::Graphics);
    std::ostringstream hash;
    hash << std::hex << std::setw(16) << std::setfill('0') << _lastUploadHash;
    info("GpuScene upload hash=" + hash.str() +
             ", materials=" + std::to_string(_lastMaterialReferences) + "->" +
             std::to_string(_lastMaterialCount),
         LogChannel::Graphics);
    uint64_t grassClusters = 0;
    for (const auto &range : _admissionResult.submission.upload.grassRanges)
        grassClusters += range.clusterCount;
    info("GpuScene grass faces=" +
             std::to_string(_admissionResult.submission.upload.grassFaces.size()) +
             ", in_band_faces=" +
             std::to_string(_admissionResult.submission.upload.grassRanges.size()) +
             ", clusters=" + std::to_string(grassClusters),
         LogChannel::Graphics);
    _executor->dumpTargets(dir, *_callbacks);
}

void RenderPipeline::restartTemporalHistory() {
    if (_rayQuery)
        _rayQuery->restartTemporalHistory();
    // The common tail carries a temporal resolve of its own in every mode, so
    // a cut has to reach it whether or not this frame is traced.
    if (_executor)
        _executor->restartTemporalHistory();
}

} // namespace reone::scene
