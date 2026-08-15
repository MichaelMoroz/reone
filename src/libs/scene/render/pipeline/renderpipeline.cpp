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
        _owner._rayQuery->render(context, std::move(_owner._admissionResult), context.sky);
    }

    graphics::SkyBinding prepareSky(graphics::ICommandBuffer &commandBuffer) override {
        return _owner.skyBinding(commandBuffer);
    }

    graphics::GpuScene::View mergeGeometry(graphics::ICommandBuffer &commandBuffer) override {
        auto view = _owner._deviceGpuScene->update(commandBuffer,
                                                   _owner._admissionResult.submission.upload);
        // The one moment both halves exist: the merge has just filled the
        // triangle bases and handed back the primitive-id ranges, and the
        // tracer has not yet taken ownership of the upload.
        _owner.serveRecordDump(view);
        return view;
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
        // The tracer runs at render resolution, the same extent the G-buffer
        // it reads its primary from was rastered at.
        _rayQuery = std::make_unique<RayQueryPipeline>(
            _renderer, graphics::renderExtentFor(_options, _targetSize), _options);
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
                                                RenderShadowKind shadow,
                                                SceneOutputAlpha alpha) {
    graphics::SceneFramePlan plan;
    plan.transparentOutput = alpha == SceneOutputAlpha::Coverage;
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
    // The common tail. Every mode reaches it with linear scene-referred colour,
    // and it ends with the one display transform that turns that into a
    // picture. A debug view is excluded from all of it because it is not a
    // picture - what is written is a diagnostic, already display-referred, and
    // filtering or transforming it would change the values it exists to show.
    //
    // In every mode now, for the same reason. The channels are G-buffer
    // quantities and every mode draws that G-buffer, so the selection is
    // answerable anywhere; it would be strange for the same view to be a raw
    // diagnostic in one mode and a tonemapped, sharpened one in the next.
    const bool diagnosticImage = _options.debugView != 0;
    const bool antialiased =
        _options.antialiasing != graphics::AntiAliasing::None && !diagnosticImage;
    // Which side of the display transform the anti-aliasing slot falls on is
    // decided by its occupant, because the two kinds want opposite inputs.
    //
    // A temporal resolve accumulates and reprojects radiance across frames, so
    // it belongs on linear pre-tonemap colour - where FSR already sat, and
    // where it stays. A spatial filter is a judgement about the finished
    // picture: FXAA thresholds luma differences against constants tuned for
    // display-referred colour, and on linear input those thresholds are far too
    // coarse in the highlights, so the brightest edges - the ones aliasing is
    // most visible on - fall below its early-out and are never touched. It
    // therefore runs after the transform.
    const bool temporalResolve =
        antialiased && _options.antialiasing == graphics::AntiAliasing::Fsr;
    if (!diagnosticImage) {
        // Before the resolve, reversing the order this pass used to hold. The
        // forward pass shares the G-buffer depth attachment, so it has to run
        // while the colour chain is still at the render extent: once FSR has
        // handed the display-sized image to the tail, pairing it with that
        // render-sized depth opens a mismatched dynamic-rendering area and
        // leaves the rest of the display target untouched. That is what showed
        // as a menu's embedded scene filling only part of its panel.
        //
        // The order it replaces existed for a reason that has not gone away: a
        // temporal resolve cannot reproject a billboard, transparency writes
        // no motion and no depth, and drawn ahead of one it smears behind the
        // camera - a lightsaber once came out as a hilt with no blade. FSR's
        // shading-change detection is the only thing standing in for that
        // until a reactive mask exists, so this trades a smear that heuristic
        // can partly absorb for a mismatch it cannot.
        // Before transparency, after the resolve: the reference adds its
        // blurred hilights when the opaque image is complete and nothing
        // blended has touched it yet.
        if (_options.bloom)
            plan.steps.push_back(graphics::SceneStep::Bloom);
        plan.steps.push_back(graphics::SceneStep::Blended);
    }
    if (temporalResolve)
        plan.steps.push_back(graphics::SceneStep::AntiAliasing);
    // Unconditional. This pass is the encode, not an effect: without it a
    // linear image would be presented as though it were already display
    // colour. What used to switch it off is now GraphicsOptions::grade, which
    // the pass reads itself - it gates the exposure and the tone curve, and an
    // ungraded frame is still a correctly encoded one.
    if (!diagnosticImage)
        plan.steps.push_back(graphics::SceneStep::PostProcess);
    if (antialiased && !temporalResolve)
        plan.steps.push_back(graphics::SceneStep::AntiAliasing);
    // Last, after both. The mask judges the displayed picture, so it wants the
    // colour a viewer sees rather than scene radiance.
    if (_options.sharpen && !diagnosticImage)
        plan.steps.push_back(graphics::SceneStep::Sharpen);
    // The debug view, over whatever the mode shaded. Skipped in exactly one
    // case: the traced mode showing one of the tracer's own channels, which the
    // kernel has already written into the output itself. Everywhere else the
    // shared pass runs - to answer a G-buffer channel, or to say plainly that
    // this mode cannot answer a traced one.
    if (diagnosticImage &&
        !(_primaryRayMode && graphics::isTracedOnlyDebugView(_options.debugView))) {
        plan.steps.push_back(graphics::SceneStep::DebugView);
    }
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

void RenderPipeline::dumpSceneRecords(const std::filesystem::path &dir) {
    // Served at the top of the next render, where the upload is still owned by
    // this pipeline - see the note there.
    _pendingRecordDump = dir;
}

void RenderPipeline::serveRecordDump(const graphics::GpuScene::View &view) {
    if (_pendingRecordDump.empty()) {
        return;
    }
    const auto dir = _pendingRecordDump;
    _pendingRecordDump.clear();
    const auto &upload = _admissionResult.submission.upload;
    {
        // The table that turns a pixel into a name. The triangle range comes
        // from the merge's own primitive-id ranges rather than from the object
        // record: dstTriangleBase is only filled during that merge, and the
        // ranges already carry the opaque/non-opaque offset applied, so a
        // triangle id sampled out of the G-buffer can be compared directly.
        // object_id is the same number objects.tsv prints as id.
        std::ofstream out(dir / "records.tsv");
        out << "record\tobject_id\tfirst_triangle\ttriangle_count\tmaterial\tvertex_count"
               "\tgeometry\n";
        for (uint32_t i = 0; i < view.primitiveIds.rangeCount; ++i) {
            const auto &range = view.primitiveIds.ranges[i];
            const auto &data = upload.objects[i].data;
            out << i << "\t" << range.first.objectIndex << "\t" << range.firstTriangle << "\t"
                << range.triangleCount << "\t" << data.materialIndex << "\t" << data.vertexCount
                << "\t" << data.geometryIndex << "\n";
        }
    }
    {
        std::ofstream out(dir / "materials.tsv");
        out << "material\tsurface_type\tfeature_mask\tcategory\tmain_tex\tlightmap\tenvmap"
               "\tself_illum_r\tself_illum_g\tself_illum_b\n";
        for (size_t i = 0; i < upload.materials.size(); ++i) {
            const auto &m = upload.materials[i];
            out << i << "\t" << m.surfaceType << "\t" << m.featureMask << "\t"
                << ((m.featureMask >> 27) & 0xFu) << "\t" << m.mainTex << "\t" << m.lightmap
                << "\t" << m.envMap << "\t" << m.selfIllumColor.r << "\t" << m.selfIllumColor.g
                << "\t" << m.selfIllumColor.b << "\n";
        }
    }
    info("Wrote " + std::to_string(upload.objects.size()) + " object records and " +
             std::to_string(upload.materials.size()) + " materials to " + dir.string(),
         LogChannel::Graphics);
}

void RenderPipeline::restartTemporalHistory() {
    debug("Temporal history restarted", LogChannel::Graphics);
    if (_rayQuery)
        _rayQuery->restartTemporalHistory();
    // The common tail carries a temporal resolve of its own in every mode, so
    // a cut has to reach it whether or not this frame is traced.
    if (_executor)
        _executor->restartTemporalHistory();
}

} // namespace reone::scene
