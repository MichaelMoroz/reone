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

#include "reone/graphics/rendering/scenepipeline.h"

#include "reone/system/profiler.h"

#include "reone/graphics/dxtutil.h"
#include "reone/graphics/npyutil.h"
#include "reone/graphics/options.h"
#include "reone/graphics/textureregistry.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/rendering/pbrtextures.h"
#include "reone/graphics/rhi/descriptors.h"
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/graphics/rhi/uniformring.h"
#include "reone/system/logutil.h"

#include <algorithm>
#include <cmath>
#include <string_view>

using namespace reone::graphics;

namespace reone {

namespace graphics {

static constexpr char kPostProcessModule[] = "postprocess";

struct MegaDrawPushConstants {
    uint32_t triangleBase;
    uint32_t materialGated;
};

/** The shadow pass's third word: which categories may write the map. */
struct ShadowPushConstants {
    uint32_t triangleBase;
    uint32_t materialGated;
    uint32_t casterCategories;
};

/** Mirrors PostProcessPushConstants in postprocess.slang. */
struct PostProcessPushConstants {
    uint32_t transform;
    float exposure;
    uint32_t tonemap;
};

ScenePipeline::ScenePipeline(glm::ivec2 targetSize,
                                         GraphicsOptions &options,
                                         IRenderer &renderer,
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

ScenePipeline::~ScenePipeline() {
    deinit();
}

static void transitionGBuffer(ICommandBuffer &cmd, GBuffer &gbuffer,
                              ImageLayout layout) {
    cmd.transitionImages(gbuffer.colorImages(), layout);
}

void ScenePipeline::init() {
    if (_inited) {
        return;
    }
    _gbuffer = std::make_unique<GBuffer>(_renderer);
    _gbuffer->init(_targetSize);

    // The raster modes resolve straight into display-space bytes, so their
    // output keeps the swapchain's format and the tail is byte-exact over it.
    // The traced mode hands linear HDR to the display transform, which an
    // 8-bit unorm would clamp before the tonemap ever sees it.
    const Format outputFormat = _primaryRayMode ? Format::R16G16B16A16Sfloat
                                                : _renderer.sceneOutputFormat();
    _output = _renderer.resources().makeImage();
    _output->initColorAttachment(_targetSize, outputFormat);
    _tailColor = _renderer.resources().makeImage();
    _tailColor->initColorAttachment(_targetSize, outputFormat);

    auto colorSampler = _renderer.resources().sampler(
        getTextureProperties(TextureUsage::ColorBuffer));
    auto depthSampler = _renderer.resources().sampler(
        getTextureProperties(TextureUsage::DepthBuffer));
    auto materialIdProperties = getTextureProperties(TextureUsage::ColorBuffer);
    materialIdProperties.minFilter = Texture::Filtering::Nearest;
    materialIdProperties.magFilter = Texture::Filtering::Nearest;
    auto materialIdSampler = _renderer.resources().sampler(materialIdProperties);
    _output->setSampler(colorSampler);
    _tailColor->setSampler(colorSampler);
    _gbuffer->setSamplers(colorSampler, depthSampler, materialIdSampler);

    if (!_primaryRayMode) {
        glm::ivec2 shadowSize {_options.shadowResolution, _options.shadowResolution};
        _dirShadows = _renderer.resources().makeImage();
        _dirShadows->initLayeredDepthAttachment(shadowSize, Format::D32Sfloat,
                                                kNumShadowCascades, false);
        _pointShadows = _renderer.resources().makeImage();
        _pointShadows->initLayeredDepthAttachment(shadowSize, Format::D32Sfloat,
                                                  kNumCubeFaces, true);
        _dirShadows->setSampler(depthSampler);
        _pointShadows->setSampler(depthSampler);

        // Both resolve sets always bind both sampler shapes. Clear each target to
        // the far plane once so the inactive light kind is a valid no-shadow map.
        _renderer.immediateSubmit([this, shadowSize](ICommandBuffer &cmd) {
            auto clear = [&](IImage &image, int layers, bool cube) {
                cmd.transitionImage(image, ImageLayout::DepthAttachment);
                RenderAttachment depth {image.attachmentView(0, 0), ImageLayout::DepthAttachment,
                                        AttachmentLoad::Clear, AttachmentStore::Store};
                depth.clear.depthOnly = true;
                cmd.beginRendering(shadowSize, {}, &depth, cube ? (1u << layers) - 1u : 0, false);
                cmd.endRendering();
                cmd.transitionImage(image, ImageLayout::DepthRead);
            };
            clear(*_dirShadows, kNumShadowCascades, false);
            clear(*_pointShadows, kNumCubeFaces, true);
        });

        IDescriptors &descriptors = _renderer.descriptors();
        _retroResolveSet = descriptors.createPersistentTextureSet(
            {{1, &_gbuffer->color(GBufferAttachment::Diffuse)},
             {2, &_gbuffer->color(GBufferAttachment::EyeNormal)},
             {3, &_gbuffer->color(GBufferAttachment::Lightmap)},
             {4, &_gbuffer->color(GBufferAttachment::SelfIllum)},
             {5, &_gbuffer->depth()},
             {15, _dirShadows.get()},
             {17, &_renderer.pbrTextures().prefilteredArray()},
             {19, _pointShadows.get()},
             {21, &_gbuffer->color(GBufferAttachment::MaterialId)}});
        _pbrResolveSet = descriptors.createPersistentTextureSet(
            {{1, &_gbuffer->color(GBufferAttachment::Diffuse)},
             {2, &_gbuffer->color(GBufferAttachment::EyeNormal)},
             {3, &_gbuffer->color(GBufferAttachment::Lightmap)},
             {4, &_gbuffer->color(GBufferAttachment::SelfIllum)},
             {5, &_gbuffer->depth()},
             {13, &_renderer.pbrTextures().brdfImage()},
             {15, _dirShadows.get()},
             {16, &_renderer.pbrTextures().irradianceArray()},
             {17, &_renderer.pbrTextures().prefilteredArray()},
             {19, _pointShadows.get()},
             {21, &_gbuffer->color(GBufferAttachment::MaterialId)}});
    }

    _outputHandle = std::make_shared<Texture>(
        _primaryRayMode ? "vk_primary_ray_output" : "vk_scene_output",
        TextureType::TwoDim, Texture::Properties());
    _renderer.resources().registerExternal(*_outputHandle, *_output);

    _renderer.immediateSubmit([this](ICommandBuffer &cmd) {
        cmd.transitionImage(*_output, ImageLayout::ShaderRead);
        cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    });

    if (_options.antialiasing == AntiAliasing::Fsr) {
        try {
            // The colour it will resolve is linear only in the traced mode;
            // every raster resolve hands it display-referred bytes.
            _upscaler = _renderer.makeUpscaler(_targetSize, _primaryRayMode);
        } catch (const std::exception &e) {
            warn(std::string("Upscaler unavailable, the anti-aliasing slot is empty: ") + e.what(),
                 LogChannel::Graphics);
            _upscaler.reset();
        }
        if (!_upscaler) {
            // A build without the upscaler compiled in. Loud rather than
            // silent, because the frame that comes out has no anti-aliasing at
            // all and nothing else in the image says so.
            warn("Anti-aliasing is set to FSR, which this build does not carry; "
                 "the slot is empty",
                 LogChannel::Graphics);
        }
    }
    // No history on the first frame either; init leaves the flag clear and the
    // first dispatch is therefore a reset.
    _temporalHistoryValid = false;
    _prevCameraPosition = glm::vec3(0.0f);

    _inited = true;
}
void ScenePipeline::deinit() {
    if (!_inited) {
        return;
    }
    // The descriptor owns a reference to the preview view, so release it
    // before that view. Engine teardown keeps ImGui alive until its pipelines
    // have done the same.
    if (_preview && _preview->imguiTexture) {
        _renderer.removePreviewTexture(_preview->imguiTexture);
    }
    _preview.reset();
    // Deregistered before the image goes: the registry holds a raw pointer to
    // it, keyed on the Texture, and would outlive both.
    if (_outputHandle) {
        _renderer.resources().unregisterExternal(*_outputHandle);
    }
    // Before the images it reprojects: the backend holds device objects of its
    // own and must not outlive the allocator either.
    _upscaler.reset();
    _output.reset();
    _tailColor.reset();
    _dirShadows.reset();
    _pointShadows.reset();
    _gbuffer.reset();
    _outputHandle.reset();
    // Persistent sets are not recycled by any per-frame pool reset, so a
    // pipeline that is thrown away on a graphics rebuild has to hand its own
    // back. Left leaked, a dozen rebuilds exhaust the pool and the next one
    // fails to allocate. The caller has waited the device idle before getting
    // here, so the GPU is done with them.
    if (_retroResolveSet)
        _renderer.descriptors().freePersistentTextureSet(_retroResolveSet);
    if (_pbrResolveSet)
        _renderer.descriptors().freePersistentTextureSet(_pbrResolveSet);
    _retroResolveSet = {};
    _pbrResolveSet = {};
    _resolveMaterialSet = {};
    _mergedScene = {};
    _mergedScenePrepared = false;
    _inited = false;
}

const GpuScene::View &ScenePipeline::prepareMergedScene(
    ICommandBuffer &cmd, ISceneCallbacks &callbacks) {
    if (_mergedScenePrepared) {
        return _mergedScene;
    }
    // Upload and compute-merge are recorded once before the first consumer.
    // GpuScene publishes the compute-to-vertex/index barrier; the same
    // buffers and descriptor set then feed shadows and the G-buffer.
    _mergedScene = callbacks.mergeGeometry(cmd);
    _mergedScenePrepared = true;
    _resolveMaterialSet = {};
    if (!_mergedScene.vertices.buffer || _mergedScene.triangleCount == 0) {
        return _mergedScene;
    }
    const auto materialCount =
        _mergedScene.materials.size / sizeof(InstanceMaterial);
    if (materialCount > GBuffer::kNoMaterial) {
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

void ScenePipeline::shadowPass(ICommandBuffer &cmd,
                                     uint32_t globalsOffset,
                                     ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::shadowPass record");
    if (_shadow == SceneShadow::None) {
        return;
    }
    const bool directional = _shadow == SceneShadow::Directional;
    auto &image = directional ? *_dirShadows : *_pointShadows;
    const int layers = directional ? kNumShadowCascades : kNumCubeFaces;
    const uint32_t viewMask = (1u << layers) - 1u;
    const auto &scene = prepareMergedScene(cmd, callbacks);

    cmd.transitionImage(image, ImageLayout::DepthAttachment);

    const auto extent = image.extent();
    {
        RenderAttachment depth {image.attachmentView(0, 0), ImageLayout::DepthAttachment,
                                AttachmentLoad::Clear, AttachmentStore::Store};
        depth.clear.depthOnly = true;
        cmd.beginRendering(extent, {}, &depth, viewMask, false);
        if (scene.vertices.buffer && scene.triangleCount != 0) {
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
        std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        cmd.bindIndexBuffer(*scene.indices.buffer, scene.indices.offset);

        auto drawRange = [&](uint32_t triangleBase, uint32_t triangleCount,
                             bool gated) {
            if (triangleCount == 0) {
                return;
            }
            PipelineKey key;
            key.module = "shadow_megadraw";
            key.vertexEntry = directional ? "directionalShadowMegadrawVertex"
                                          : "pointShadowMegadrawVertex";
            key.fragmentEntry = directional
                                    ? "directionalShadowMegadrawFragment"
                                    : "pointShadowMegadrawFragment";
            key.depthFormat = Format::D32Sfloat;
            key.viewMask = viewMask;
            key.depthTest = true;
            key.depthWrite = true;
            key.depthBias = true;
            // D32_SFLOAT constant bias is expressed in representable depth
            // increments; the slope term supplies the useful offset on curved
            // surfaces that approach parallel to the light.
            //
            // Both terms carry more than they used to because nothing is
            // culled any more. Rendering back faces alone put the stored depth
            // a wall's thickness behind the lit surface, which is a free bias
            // - but only for geometry that HAS a back face. Odyssey's exterior
            // shells are single-sided, so from the sun's side they wrote
            // nothing at all and light poured into the rooms behind them. With
            // both faces written, a lit surface now finds its own depth in the
            // map and needs a real offset instead. The receiver-side normal
            // offset in lib/shadow.slang remains the primary defence; these
            // are the dials to turn if acne or peter-panning shows up.
            key.depthBiasConstantFactor = 2.0f;
            key.depthBiasSlopeFactor = 2.0f;
            // Never cull. A single-sided wall has to occlude from whichever
            // side the light is on, and a cutout fence or leaf card likewise.
            key.cull = FaceCullMode::None;
            PipelineBinding pipeline = _renderer.pipelines().get(key);
            cmd.bindPipeline(pipeline.pipeline);
            cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                  offsets.data(), static_cast<uint32_t>(offsets.size()));
            cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
            // Three words here, two everywhere else: only this pass reads the
            // caster filter. See PushConstants in lib/megadraw_geometry.slang.
            const ShadowPushConstants push {triangleBase, gated ? 1u : 0u,
                                            _shadowCasterCategories};
            cmd.pushFragmentConstants(pipeline.layout, &push, sizeof(push));
            cmd.drawIndexed(triangleCount * 3, triangleBase * 3);
        };

        drawRange(0, scene.opaqueTriangleCount, false);
        const uint32_t gatedTriangles =
            scene.triangleCount - scene.opaqueTriangleCount;
        drawRange(scene.opaqueTriangleCount, gatedTriangles, true);
        }
        cmd.endRendering();
    }
    cmd.transitionImage(image, ImageLayout::DepthRead);
}

void ScenePipeline::geometryPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                                       ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::geometryPass record");
    const auto &scene = prepareMergedScene(cmd, callbacks);

    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ColorAttachment);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthAttachment);
    std::vector<RenderAttachment> colors;
    colors.reserve(kGBufferAttachments.size());
    for (auto gbufferAttachment : kGBufferAttachments) {
        RenderAttachment attachment {
            _gbuffer->color(gbufferAttachment).sampleView(),
            ImageLayout::ColorAttachment, AttachmentLoad::Clear, AttachmentStore::Store};
        if (gbufferAttachment == GBufferAttachment::MaterialId) {
            attachment.clear.integer = true;
            attachment.clear.uintValue = GBuffer::kNoMaterial;
        }
        colors.push_back(attachment);
    }
    RenderAttachment depth {_gbuffer->depth().sampleView(), ImageLayout::DepthAttachment,
                            AttachmentLoad::Clear, AttachmentStore::Store};
    depth.clear.depthOnly = true;
    cmd.beginRendering(_targetSize, colors, &depth, 0, true);

    if (scene.vertices.buffer && scene.triangleCount != 0) {
        PipelineKey key;
        key.module = "megadraw";
        key.vertexEntry = "megadrawVertex";
        key.fragmentEntry = "megadrawFragment";
        key.colorFormats = _gbuffer->colorFormats();
        key.depthFormat = _gbuffer->depthFormat();
        key.depthTest = true;
        key.depthWrite = true;
        key.cull = FaceCullMode::None;
        PipelineBinding pipeline = _renderer.pipelines().get(key);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
        std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        cmd.bindPipeline(pipeline.pipeline);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
        cmd.bindIndexBuffer(*scene.indices.buffer, scene.indices.offset);

        if (scene.opaqueTriangleCount != 0) {
            const MegaDrawPushConstants push {0, 0};
            cmd.pushFragmentConstants(pipeline.layout, &push, sizeof(push));
            cmd.drawIndexed(scene.opaqueTriangleCount * 3, 0);
        }
        const uint32_t nonOpaqueTriangles =
            scene.triangleCount - scene.opaqueTriangleCount;
        if (nonOpaqueTriangles != 0) {
            const MegaDrawPushConstants push {scene.opaqueTriangleCount, 1};
            cmd.pushFragmentConstants(pipeline.layout, &push, sizeof(push));
            cmd.drawIndexed(nonOpaqueTriangles * 3, scene.opaqueTriangleCount * 3);
        }
    }
    cmd.endRendering();
}

void ScenePipeline::blendedPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                                      ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::blendedPass record");
    const auto &scene = prepareMergedScene(cmd, callbacks);
    const uint32_t nonOpaqueTriangles =
        scene.triangleCount > scene.opaqueTriangleCount
            ? scene.triangleCount - scene.opaqueTriangleCount
            : 0;
    if (!scene.vertices.buffer || nonOpaqueTriangles == 0) {
        return;
    }
    // Depth-test against the opaque G-buffer but never write: blended
    // fragments must not reject each other, or the result depends on which
    // one happened to be drawn first rather than on coverage.
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    cmd.transitionImage(*_output, ImageLayout::ColorAttachment);

    // The resolve already wrote this image; loading preserves it.
    PipelineKey key;
    key.module = "megadraw";
    key.vertexEntry = "megadrawVertex";
    key.fragmentEntry = "megadrawBlendedFragment";
    key.colorFormats = {_output->pixelFormat()};
    key.depthFormat = _gbuffer->depthFormat();
    key.depthTest = true;
    key.depthWrite = false;
    key.blend = BlendMode::Premultiplied;
    key.cull = FaceCullMode::None;
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    {
        RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::Load, AttachmentStore::Store};
        RenderAttachment depth {_gbuffer->depth().sampleView(), ImageLayout::DepthRead,
                                AttachmentLoad::Load, AttachmentStore::DontCare};
        cmd.beginRendering(_targetSize, {color}, &depth, 0, true);
        cmd.bindPipeline(pipeline.pipeline);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
        cmd.bindIndexBuffer(*scene.indices.buffer, scene.indices.offset);
        // Submission order, deliberately. See megadraw.slang.
        const MegaDrawPushConstants push {scene.opaqueTriangleCount, 2};
        cmd.pushFragmentConstants(pipeline.layout, &push, sizeof(push));
        cmd.drawIndexed(nonOpaqueTriangles * 3, scene.opaqueTriangleCount * 3);
        cmd.endRendering();
    }
    // Publish the composited image in the layout expected by the preview and
    // post-process descriptors, just as both resolve passes do after writing.
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
}

void ScenePipeline::retroResolvePass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::retroResolvePass record");
    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    cmd.transitionImage(*_output, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = "retro_resolve";
    key.vertexEntry = "retroResolveVertex";
    key.fragmentEntry = "retroResolveFragment";
    key.colorFormats = {_output->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    {
        RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::Clear, AttachmentStore::Store};
        color.clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd.beginRendering(_targetSize, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, _retroResolveSet, nullptr, 0);
        if (_resolveMaterialSet) {
            cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
            cmd.draw(3, 1);
        }
        cmd.endRendering();
    }
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
}

void ScenePipeline::skyCompositePass(ICommandBuffer &cmd, uint32_t globalsOffset,
                                     ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::skyCompositePass record");
    // Outside any pass: a first bake of a room records six cube-face passes of
    // its own here.
    const auto sky = callbacks.prepareSky(cmd);
    if (!sky.cube) {
        return;
    }
    // The bake pushes its own per-face globals through the ring and leaves the
    // last face's offset latched. Nothing downstream of here reads the latch
    // today, but restoring it keeps this frame's globals the frame's globals.
    _renderer.uniformRing().setGlobalsOffset(globalsOffset);
    if (!sky.baked) {
        // The fallback cube is black by construction, so compositing it would
        // write the black the resolve already left. Skipping keeps a module
        // without a sky at exactly the pixels it had before.
        return;
    }
    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    cmd.transitionImage(*_output, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = "sky";
    key.vertexEntry = "skyCompositeVertex";
    key.fragmentEntry = "skyCompositeFragment";
    key.colorFormats = {_output->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    // The cube is bound through the view the sky hands over rather than the
    // image's own: the baked cube is one cube inside a cube-array image, and
    // the shader declares a plain SamplerCube.
    auto textureSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.frameIndex(),
        {{TextureUnits::gBufDepth, &_gbuffer->depth()},
         {TextureUnits::envMapCube, sky.cube, sky.view}});

    {
        // Loading preserves the resolved image; the shader discards every
        // pixel the geometry pass wrote depth into.
        RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::Load, AttachmentStore::Store};
        cmd.beginRendering(_targetSize, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, textureSet, nullptr, 0);
        cmd.draw(3, 1);
        cmd.endRendering();
    }
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
}

void ScenePipeline::pbrResolvePass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::pbrResolvePass record");
    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    cmd.transitionImage(*_output, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = "pbr_resolve";
    key.vertexEntry = "resolveVertex";
    key.fragmentEntry = "resolveFragment";
    key.colorFormats = {_output->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    {
        RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::Clear, AttachmentStore::Store};
        color.clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd.beginRendering(_targetSize, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, _pbrResolveSet, nullptr, 0);
        if (_resolveMaterialSet) {
            cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
            cmd.draw(3, 1);
        }
        cmd.endRendering();
    }
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
}

void ScenePipeline::tailPass(ICommandBuffer &cmd, const char *fragmentEntry,
                             uint32_t globalsOffset, uint32_t screenEffectOffset,
                             const void *pushConstants, uint32_t pushConstantSize) {
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    cmd.transitionImage(*_tailColor, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = fragmentEntry;
    key.colorFormats = {_tailColor->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(), _output.get());

    {
        // Every pixel is written, so the previous contents of the target are
        // never read back.
        RenderAttachment color {_tailColor->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::DontCare, AttachmentStore::Store};
        cmd.beginRendering(_targetSize, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        if (pushConstants && pushConstantSize != 0) {
            cmd.pushFragmentConstants(pipeline.layout, pushConstants, pushConstantSize);
        }
        cmd.draw(3, 1);
        cmd.endRendering();
    }
    cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    // The pair are interchangeable, so the result becomes the output by
    // exchanging the handles rather than by copying pixels back.
    std::swap(_output, _tailColor);
}

void ScenePipeline::restartTemporalHistory() {
    _temporalHistoryValid = false;
}

void ScenePipeline::upscalePass(ICommandBuffer &cmd) {
    R_PROFILE_ZONE("ScenePipeline::upscalePass record");
    if (!_upscaler) {
        return;
    }
    const auto &globals = _uniforms.globals();
    auto &motion = _gbuffer->color(GBufferAttachment::Motion);
    auto &depth = _gbuffer->depth();

    // Primary visibility is rasterized in every mode, traced included, so
    // these two attachments describe the same surfaces the colour was shaded
    // for whichever pass shaded it. That invariant is what lets one temporal
    // resolve serve every mode.
    //
    // The backend transitions each image from the state it is told it is in,
    // so all three inputs are published as plain sampled images first: the
    // depth attachment otherwise sits in a depth-read layout that no upscaler
    // state maps to. They are left sampled afterwards and the depth is put
    // back before anything reads it as a depth attachment again.
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    cmd.transitionImage(motion, ImageLayout::ShaderRead);
    cmd.transitionImage(depth, ImageLayout::ShaderRead);
    cmd.transitionImage(*_tailColor, ImageLayout::General);

    UpscalerInputs inputs;
    inputs.color = _output.get();
    inputs.depth = &depth;
    inputs.motion = &motion;
    inputs.output = _tailColor.get();
    // The G-buffer stores current minus previous, as half a clip-space delta
    // with y up. FSR wants previous minus current, in pixels, with y down.
    // Both corrections are a sign per axis, so they ride in the scale rather
    // than in a rewrite of an attachment several passes and both dump paths
    // already read.
    inputs.motionScale = {-static_cast<float>(_targetSize.x),
                          static_cast<float>(_targetSize.y)};

    // The sub-pixel offset this frame's projection was built with, in pixels
    // with y down. Raster only jitters when the taajitter option is on; with
    // it off this is zero, and the resolve still reprojects and still cleans
    // edges - it simply has no sub-pixel information to accumulate, so it
    // anti-aliases less.
    const glm::vec2 jitterPixels {globals.jitter.x * 0.5f * static_cast<float>(_targetSize.x),
                                  -globals.jitter.y * 0.5f * static_cast<float>(_targetSize.y)};
    const float verticalFov =
        2.0f * std::atan(1.0f / std::max(1e-4f, globals.projection[1][1]));

    // History is worthless across a cut, and a teleport-sized step is a cut
    // whether or not anything announced one.
    const auto cameraPosition = glm::vec3(globals.cameraPosition);
    if (glm::distance(cameraPosition, _prevCameraPosition) > 20.0f) {
        _temporalHistoryValid = false;
    }
    _prevCameraPosition = cameraPosition;
    const bool reset = !_temporalHistoryValid;
    _temporalHistoryValid = true;

    _upscaler->dispatch(cmd, inputs, jitterPixels, 1.0f / 60.0f,
                        globals.clipNear, globals.clipFar, verticalFov,
                        std::clamp(_options.fsrSharpness, 0.0f, 1.0f), reset);

    cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    cmd.transitionImage(depth, ImageLayout::DepthRead);
    std::swap(_output, _tailColor);
}

void ScenePipeline::antiAliasingPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::antiAliasingPass record");
    // A new resolve in this slot is a new case here; the ping-pong and the
    // ordering are already common. Nothing in this slot applies a display
    // transform, in any mode - that belongs to the post-process pass alone,
    // and a resolve that tonemapped to find edges would be applying it twice.
    const char *fragmentEntry = nullptr;
    switch (_options.antialiasing) {
    case AntiAliasing::None:
        return;
    case AntiAliasing::Fsr:
        upscalePass(cmd);
        return;
    case AntiAliasing::Fxaa:
        fragmentEntry = "fxaaFragment";
        break;
    }
    if (!fragmentEntry) {
        return;
    }
    // FXAA works off neighbouring texel offsets, which nothing else in this
    // pipeline publishes; the block is otherwise irrelevant to the pass.
    ScreenEffectUniforms screenEffect;
    screenEffect.screenResolution = glm::vec2(_targetSize);
    screenEffect.screenResolutionRcp = 1.0f / glm::vec2(_targetSize);
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);
    tailPass(cmd, fragmentEntry, globalsOffset, screenEffectOffset, nullptr, 0);
}

void ScenePipeline::postProcessPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::postProcessPass record");
    // The raster resolves already write display-space colour, so the transform
    // is an identity over them and their bytes are preserved exactly. Only the
    // traced chain arrives linear, carrying the dials its calibration is
    // defined in.
    PostProcessPushConstants push {_primaryRayMode ? 1u : 0u,
                                   std::max(0.01f, _options.exposure),
                                   static_cast<uint32_t>(std::clamp(_options.tonemap, 0, 1))};
    tailPass(cmd, "postProcessFragment", globalsOffset, 0, &push, sizeof(push));
}

Texture &ScenePipeline::render(const SceneFramePlan &plan,
                                     ISceneCallbacks &callbacks) {
    auto &cmd = _renderer.recordingCommandBuffer();
    _shadow = plan.shadow;
    _shadowCasterCategories = plan.shadowCasterCategories;
    _mergedScene = {};
    _mergedScenePrepared = false;
    // The scene graph stores the frame's Vulkan-native uniform values here;
    // copy them into this frame's arena. Clip-space y is still handled by the
    // flipped viewport so triangle winding remains unchanged.
    auto globals = _uniforms.globals();
    auto globalsOffset = _renderer.uniformRing().push(globals);
    _renderer.uniformRing().setGlobalsOffset(globalsOffset);

    if (_primaryRayMode) {
        // V1b deliberately records primary visibility before tracing. The
        // tracer still owns the rendered image in this step; the G-buffer is
        // a validation target only and its read layout is published below.
        for (const auto step : plan.steps) {
            if (step == SceneStep::Geometry) {
                geometryPass(cmd, globalsOffset, callbacks);
            }
        }
        cmd.transitionImage(*_output, ImageLayout::General);
        callbacks.renderPrimary(
            {&cmd, globalsOffset, _output.get(), _mergedScene,
             globals.view, globals.projection, globals.jitter});
        cmd.transitionImage(*_output, ImageLayout::ShaderRead);
        // Keep the raster result available to target previews and dumps, but
        // never bind it into the trace path. This explicit handoff makes the
        // independent validation channels insensitive to command ordering.
        transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
        cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
        // The common tail runs over the traced image exactly as it does over a
        // resolved one; the tracer stops at linear and the display transform
        // is the pass below, not the kernel.
        for (const auto step : plan.steps) {
            if (step == SceneStep::AntiAliasing) {
                antiAliasingPass(cmd, globalsOffset);
            } else if (step == SceneStep::PostProcess) {
                postProcessPass(cmd, globalsOffset);
            }
        }
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
        case SceneStep::ProcessPBRTextures:
            _renderer.pbrTextures().process(_renderer.recordingCommandBuffer(), globalsOffset);
            break;
        case SceneStep::Shadow:
            shadowPass(cmd, globalsOffset, callbacks);
            break;
        case SceneStep::Geometry:
            geometryPass(cmd, globalsOffset, callbacks);
            break;
        case SceneStep::PBRResolve:
            pbrResolvePass(cmd, globalsOffset);
            outputResolved = true;
            break;
        case SceneStep::Blended:
            blendedPass(cmd, globalsOffset, callbacks);
            break;
        case SceneStep::RetroResolve:
            retroResolvePass(cmd, globalsOffset);
            outputResolved = true;
            break;
        case SceneStep::SkyComposite:
            skyCompositePass(cmd, globalsOffset, callbacks);
            break;
        case SceneStep::AntiAliasing:
            antiAliasingPass(cmd, globalsOffset);
            break;
        case SceneStep::PostProcess:
            postProcessPass(cmd, globalsOffset);
            break;
        }
    }

    // An empty raster plan is a supported degenerate frame. Keep the output
    // alive as a regular attachment, clear it to black, then publish it in the
    // layout the 2D compositor samples.
    if (!outputResolved) {
        cmd.transitionImage(*_output, ImageLayout::ColorAttachment);
        {
            RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                    AttachmentLoad::Clear, AttachmentStore::Store};
            color.clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
            cmd.beginRendering(_targetSize, {color}, nullptr, 0, false);
            cmd.endRendering();
        }
        cmd.transitionImage(*_output, ImageLayout::ShaderRead);
        transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
        cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
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

static std::optional<DumpFormat> dumpFormatFor(Format format) {
    switch (format) {
    case Format::R8G8B8A8Unorm:
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb:
        return DumpFormat {4, NpyType::UInt8, false};
    case Format::R8Unorm:
        return DumpFormat {1, NpyType::UInt8, false};
    case Format::R16Uint:
        return DumpFormat {1, NpyType::UInt16, false};
    case Format::R16Sfloat:
        return DumpFormat {1, NpyType::Float32, true};
    case Format::R16G16Sfloat:
        return DumpFormat {2, NpyType::Float32, true};
    case Format::R16G16B16A16Sfloat:
        return DumpFormat {4, NpyType::Float32, true};
    case Format::R32Sfloat:
    case Format::D32Sfloat:
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
static bool isBGRA(Format format) {
    switch (format) {
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb:
        return true;
    default:
        return false;
    }
}

std::vector<ScenePipeline::Target> ScenePipeline::targetEntries(
    const ISceneCallbacks &callbacks) const {
    if (!_inited) {
        return {};
    }
    std::vector<Target> entries;
    if (_primaryRayMode) {
        entries.push_back({"Traced output", "traced_output", TargetKind::Color,
                           _output.get(), ImageLayout::ShaderRead, false});
        // The split behind that image. Without these a traced frame can only
        // be judged as a whole, which cannot separate a noisy channel from a
        // denoiser that is not clearing it. They live in GENERAL: the trace
        // and composite passes read and write them as storage images and
        // nothing transitions them afterwards.
        for (const auto &channel : callbacks.primaryTargets()) {
            entries.push_back({channel.name, channel.dumpName, TargetKind::Color,
                               channel.image, ImageLayout::General, false});
        }
    }
    static const char *kDisplayNames[kGBufferAttachments.size()] = {
        "G-buffer diffuse", "G-buffer eye normal", "G-buffer lightmap",
        "G-buffer self-illum", "G-buffer motion", "G-buffer material ID"};
    static const char *kDumpNames[kGBufferAttachments.size()] = {
        "g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_lightmap",
        "g_buffer_self_illum", "g_buffer_motion", "g_buffer_material_id"};
    for (size_t i = 0; i < kGBufferAttachments.size(); ++i) {
        auto attachment = kGBufferAttachments[i];
        auto kind = attachment == GBufferAttachment::EyeNormal ? TargetKind::EyeNormal : attachment == GBufferAttachment::Motion ? TargetKind::Motion
                                                                                                                        : TargetKind::Color;
        entries.push_back({kDisplayNames[i], kDumpNames[i], kind, &_gbuffer->color(attachment),
                           ImageLayout::ShaderRead, false});
    }
    entries.push_back({"G-buffer depth", "g_buffer_depth", TargetKind::Depth,
                       &_gbuffer->depth(), ImageLayout::DepthRead, true});
    if (!_primaryRayMode) {
        entries.push_back({"Output", "output", TargetKind::Color,
                           _output.get(), ImageLayout::ShaderRead, false});
    }
    return entries;
}

void *ScenePipeline::renderTargetPreview(const std::string &name, int mode, float scale,
                                               const ISceneCallbacks &callbacks) {
    auto entries = targetEntries(callbacks);
    if (std::none_of(entries.begin(), entries.end(), [&name](const auto &entry) {
            return entry.name == name;
        })) {
        return nullptr;
    }
    if (!_preview) {
        _preview = std::make_unique<Preview>();
        _preview->image = _renderer.resources().makeImage();
        _preview->image->initColorAttachment({480, 360}, Format::R8G8B8A8Unorm);
        _preview->image->setSampler(_renderer.resources().sampler(
            getTextureProperties(TextureUsage::ColorBuffer)));
        _renderer.immediateSubmit([this](ICommandBuffer &cmd) {
            cmd.transitionImage(*_preview->image, ImageLayout::ShaderRead);
        });
        _preview->imguiTexture = _renderer.addPreviewTexture(*_preview->image);
    }
    _preview->target = name;
    _preview->mode = mode;
    _preview->scale = scale;
    return _preview->imguiTexture;
}

void ScenePipeline::previewPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                                      const ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::previewPass record");
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

    cmd.transitionImage(*_preview->image, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = "debugTextureFragment";
    key.colorFormats = {_preview->image->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    ScreenEffectUniforms screenEffect;
    screenEffect.clipNear = _uniforms.globals().clipNear;
    screenEffect.clipFar = _uniforms.globals().clipFar;
    // These fields are otherwise irrelevant to this pass and avoid another
    // uniform block solely for the two viewer controls.
    screenEffect.ssaoSampleRadius = static_cast<float>(_preview->mode);
    screenEffect.ssrBias = _preview->scale;
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);
    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(), selected->image);
    {
        RenderAttachment color {_preview->image->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::DontCare, AttachmentStore::Store};
        cmd.beginRendering({480, 360}, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        cmd.draw(3, 1);
        cmd.endRendering();
    }
    cmd.transitionImage(*_preview->image, ImageLayout::ShaderRead);
}

void ScenePipeline::dumpTargets(const std::filesystem::path &dir,
                                      const ISceneCallbacks &callbacks) {
    if (!_inited) {
        return;
    }
    std::filesystem::create_directories(dir);

    auto entries = targetEntries(callbacks);

    for (const auto &entry : entries) {
        auto format = dumpFormatFor(entry.image->pixelFormat());
        if (!format) {
            warn("Cannot dump target '" + std::string(entry.name) + "': unsupported format",
                 LogChannel::Graphics);
            continue;
        }
        auto raw = entry.image->readBack(entry.depth);
        auto extent = entry.image->extent();
        // The output image carries the swapchain's format, which is BGRA here
        // while every G-buffer target is RGBA. A dump exists to be compared
        // against the OpenGL backend, so it is written in one channel order
        // rather than leaving whoever reads it to know which target is which -
        // getting that wrong once already turned an 0.9 difference into an
        // apparent 11.7 and invented a colour cast that was not there.
        if (isBGRA(entry.image->pixelFormat())) {
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
        const bool yCoCgRadiance = dumpName == "traced_radiance_diffuse" ||
                                   dumpName == "traced_radiance_specular" ||
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
        auto dumpCubeArray = [&dir](const char *name, const IImage &image, int mip,
                                     uint32_t layers) {
            constexpr uint32_t kFaces = 6;
            auto format = dumpFormatFor(image.pixelFormat());
            if (!format) {
                warn("Cannot dump cube array '" + std::string(name) + "': unsupported format",
                     LogChannel::Graphics);
                return;
            }
            auto raw = image.readBack(mip, layers);
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
        auto &irradiance = pbr.irradianceArray();
        auto &prefiltered = pbr.prefilteredArray();
        dumpCubeArray("irradiance_map_array", irradiance, 0, 16 * 6);
        for (int mip = 0; mip < prefiltered.mipLevels(); ++mip) {
            dumpCubeArray(("prefiltered_env_map_array_mip" + std::to_string(mip)).c_str(),
                          prefiltered, mip, 16 * 6);
        }

        auto dumpSourceEnvMap = [&dir, this](int layer, const Texture &texture) {
            if (!texture.is2D() && !texture.isCubeMap()) {
                warn("Cannot dump environment source '" + texture.name() +
                         "': unsupported texture shape",
                     LogChannel::Graphics);
                return;
            }
            const auto &image = _renderer.resources().get(texture);
            auto format = dumpFormatFor(image.pixelFormat());
            bool compressed = image.pixelFormat() == Format::BC1RGBAUnormBlock ||
                              image.pixelFormat() == Format::BC3UnormBlock;
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
                auto raw = image.readBack(mip, layers);
                auto extent = glm::max(glm::ivec2(1), image.extent() >> mip);
                if (compressed) {
                    size_t blockBytes = image.pixelFormat() == Format::BC1RGBAUnormBlock ? 8 : 16;
                    size_t faceBytes = static_cast<size_t>((extent.x + 3) / 4) *
                                       ((extent.y + 3) / 4) * blockBytes;
                    std::vector<uint8_t> decoded(static_cast<size_t>(extent.x) * extent.y * layers * 4);
                    std::vector<uint32_t> pixels(static_cast<size_t>(extent.x) * extent.y);
                    for (uint32_t face = 0; face < layers; ++face) {
                        if (image.pixelFormat() == Format::BC1RGBAUnormBlock) {
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
                            dst[3] = image.pixelFormat() == Format::BC1RGBAUnormBlock ? 0xff : pixel & 0xff;
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

std::vector<TargetInfo> ScenePipeline::targets(
    const ISceneCallbacks &callbacks) const {
    std::vector<TargetInfo> result;
    for (const auto &entry : targetEntries(callbacks)) {
        result.push_back({entry.name, entry.kind});
    }
    return result;
}

} // namespace graphics

} // namespace reone
