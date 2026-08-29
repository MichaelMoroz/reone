/*
 * Copyright (c) 2020-2023 The reone project contributors
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
#include "reone/graphics/rendering/tracingpipeline.h"

#include "reone/system/profiler.h"

#include <algorithm>

#include "reone/graphics/options.h"
#include "reone/graphics/rhi/commandbuffer.h"
#include "reone/graphics/rhi/descriptors.h"
#include "reone/graphics/rhi/image.h"
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/resources.h"

#include "reone/graphics/format/tgareader.h"
#include "reone/system/logutil.h"
#include "reone/system/stream/fileinput.h"

#include <SDL3/SDL_filesystem.h>

#include <filesystem>

#include <chrono>
#include <cstddef>

#include <glm/gtc/matrix_transform.hpp>

using namespace reone::graphics;

namespace reone::graphics {
namespace {

struct TraceStats {
    uint32_t secondaryRays {0};
    uint32_t secondaryMisses {0};
    uint32_t survivingLights {0};
    uint32_t primaryHits {0};
    uint32_t shadowRays {0};
};

// The trace kernel's set-2 storage-output slots, in the channel index order.
// The images themselves are ScenePipeline's; the kernel binds them into these
// slots by name every frame, the same way it binds the G-buffer block, because
// a resize hands over new images and binding once at init would keep the old
// ones. Count is asserted against kNumTracingChannels below.
constexpr const char *kChannelBindingNames[kNumTracingChannels] {
    "outDiffuse", "outSpecular", "outNormalRoughness", "outViewZ", "outMotion",
    "outNoiseFree", "outDiffFactor", "outDeviceDepth", "outScreenMotion", "outSpecFactor",
    "outGBufferDiffuse", "outGBufferEyeNormal", "outGBufferDepth", "outGBufferMotion",
    "outDirectDiffuse"};

} // namespace

TracingPipeline::TracingPipeline(IRenderer &renderer,
                                 glm::ivec2 extent,
                                 GraphicsOptions &options) :
    _renderer(renderer), _options(options), _extent(extent) {}

TracingPipeline::~TracingPipeline() {
    deinit();
}

void TracingPipeline::loadBlueNoise() {
    if (_blueNoise) {
        return;
    }
    // Resolved exactly as the shader tree is: a deployed copy beside the
    // executable wins, and the source tree is the fallback a developer build
    // lands on. Same rule, so there is one answer to "which copy is live".
    std::filesystem::path path;
    // SDL3 owns this string; SDL2 did not. Freeing it corrupts the CRT heap,
    // which surfaces as a heap-corruption abort somewhere later and entirely
    // unrelated - see SDL_GetBasePath against SDL_GetPrefPath, where only the
    // latter is documented as "should be freed with SDL_free()".
    if (const auto *base = SDL_GetBasePath()) {
        auto deployed = std::filesystem::path(base) / "assets" / kBlueNoiseFile;
        if (std::filesystem::is_regular_file(deployed)) {
            path = std::move(deployed);
        }
    }
#ifdef REONE_ASSET_SOURCE_DIR
    if (path.empty()) {
        auto source = std::filesystem::path(REONE_ASSET_SOURCE_DIR) / kBlueNoiseFile;
        if (std::filesystem::is_regular_file(source)) {
            path = std::move(source);
        }
    }
#endif
    if (path.empty()) {
        throw std::runtime_error("Blue noise atlas not found: expected " +
                                 std::string(kBlueNoiseFile) + " beside the executable or in the "
                                 "asset source tree");
    }
    FileInputStream stream {path};
    TgaReader reader {stream, "bluenoise", TextureUsage::Noise};
    reader.load();
    _blueNoise = reader.texture();
    if (!_blueNoise) {
        throw std::runtime_error("Blue noise atlas failed to decode: " + path.string());
    }
    // The tracer indexes tiles by arithmetic on these, so a differently sized
    // atlas would silently read the wrong field rather than fail.
    const int expected = static_cast<int>(kBlueNoiseTileSize * kBlueNoiseGrid);
    if (_blueNoise->width() != expected || _blueNoise->height() != expected) {
        throw std::runtime_error("Blue noise atlas is " +
                                 std::to_string(_blueNoise->width()) + "x" +
                                 std::to_string(_blueNoise->height()) + "; the tracer indexes it as " +
                                 std::to_string(expected) + " square");
    }
    _blueNoise->init();
    info("Blue noise atlas: " + path.string(), LogChannel::Graphics);
}

void TracingPipeline::init() {
    if (_inited)
        return;
    _pipeline = _renderer.makeTracingPipeline(
        {"path_trace", _renderer.reflection("path_trace"), sizeof(TracePushConstants),
         "rayquery:primaryRay"});
    _bindlessTextureCapacity = _pipeline->bindlessTextureCapacity();

    // The shadow filter's own double-buffered target. The channel images the
    // kernel writes are ScenePipeline's now and arrive bound each frame; this
    // one stays here because no other stage produces or consumes it.
    static_assert(std::size(kChannelBindingNames) == kNumTracingChannels);

    loadBlueNoise();

#ifdef R_ENABLE_NRD
    {
        _nrdDenoiser = _renderer.makeTracingDenoiser(_extent);
    }
#endif
    _inited = true;
}

void TracingPipeline::clearFrame(Frame &frame) {
    // The merge and acceleration-structure buffers are capacity-managed. The
    // renderer has waited this in-flight frame's fence before reuse, so a full
    // BLAS/TLAS rebuild may overwrite them, but their allocations survive it.
    frame.traceStats.reset();
}

void TracingPipeline::deinit() {
    for (auto &frame : _frames) {
        clearFrame(frame);
    }
#ifdef R_ENABLE_NRD
    _nrdDenoiser.reset();
#endif
    _pipeline.reset();
    _bindlessTextureCapacity = 0;
    _lastBindlessTextureCount = 0;
    _lastAuxFrame = -1;
    _inited = false;
}

void TracingPipeline::restartTemporalHistory() {
    _restartHistoryRequested = true;
}

std::vector<TracingChannel> TracingPipeline::channels() const {
    if (!_inited || _lastAuxFrame < 0) {
        return {};
    }
    // Only the images this pipeline still owns. The fifteen channel images are
    // ScenePipeline's now and it enumerates them itself; what is left here is
    // the denoiser's own output - the denoised counterparts of the raw radiance
    // pair, whose whole diagnostic value is comparing the two - and the shadow
    // filter's target, a later pass's product the kernel never touches.
    std::vector<TracingChannel> result;
#ifdef R_ENABLE_NRD
    if (_nrdDenoiser) {
        result.push_back({"Denoised diffuse", "denoised_diffuse", &_nrdDenoiser->denoisedDiffuse()});
        result.push_back({"Denoised specular", "denoised_specular", &_nrdDenoiser->denoisedSpecular()});
    }
#endif
    return result;
}

TracingStats TracingPipeline::render(const TracingPipelineInput &input) {
    auto &commandBuffer = input.commandBuffer;
    auto &output = input.output;
    const auto &view = input.view;
    const auto &projection = input.projection;
    const auto &jitter = input.jitter;
    const auto &scene = input.scene;
    const auto globalsOffset = input.globalsOffset;
    const auto frameNumber = input.frameNumber;
    const auto &sky = input.sky;
    R_PROFILE_ZONE("TracingPipeline::render");
    const int frameIndex = input.frameIndex;
    // A valid traced frame can contain no merged geometry. That path clears
    // the output and returns below, but its auxiliary images are still useful
    // diagnostics (and must not disappear from --dumptargets just because the
    // scene is empty). Record the selected double-buffer slot before that
    // early return, rather than only after the trace dispatch.
    _lastAuxFrame = frameIndex;
    auto &frame = _frames[frameIndex];
    TracingStats previousStats;
    // The renderer waited this in-flight frame's fence before calling us, so
    // its previous GPU-written counters are now safe to inspect.
    if (frame.traceStats) {
        frame.traceStats->invalidateMapped();
        const auto *stats = static_cast<const TraceStats *>(frame.traceStats->mapped());
        previousStats = {stats->secondaryRays, stats->secondaryMisses,
                         stats->survivingLights, stats->primaryHits,
                         stats->shadowRays};
    }
    clearFrame(frame);
    if (!scene.vertices.buffer) {
        previousStats.bindlessTextures = _lastBindlessTextureCount;
        return previousStats;
    }
    frame.traceStats = _renderer.makeBuffer();
    frame.traceStats->initHostVisibleReadback(sizeof(TraceStats));
    std::memset(frame.traceStats->mapped(), 0, sizeof(TraceStats));

    std::array<TracingBinding, 12> frameBindings {{
        {"outputImage", output},
        {"sceneTLAS", input.structure},
        {"instanceMaterials", {scene.materials.buffer, 0, scene.materials.size}},
        {"traceStats", {frame.traceStats.get(), 0, frame.traceStats->size()}},
        {"mergedVertices", scene.vertices},
        {"mergedIndices", scene.indices},
        {"mergedMaterialIds", scene.materialIds},
        {"grassCardVertices", scene.grassCardVertices},
        {"grassCardIndices", scene.grassCardIndices},
        {"grassCardInstances", scene.grassCardInstances},
        {"skyCube", *sky.cube},
        {"blueNoise", _renderer.resources().get(*_blueNoise)},
    }};
    // The sky's cube view is the one that needs naming explicitly; it is no
    // longer the last entry, so say which.
    frameBindings[7].imageView = sky.view;
    frameBindings[7].hasImageView = true;
    _pipeline->updateBindings(1, _renderer.frameIndex(),
                              {frameBindings.data(), static_cast<uint32_t>(frameBindings.size())});
    // The rasterized primary. The kernel no longer traces a camera ray: the
    // geometry pass has already decided which surface each pixel shows, and
    // these are the attachments it decided it in. Written every frame with the
    // rest of set 1 rather than once, because a pipeline rebuild or a resize
    // hands over new images and nothing else would notice.
    {
        const std::array<TracingBinding, 7> gbufferBindings {{
            {"gbufDiffuse", *input.gbuffer.diffuse},
            {"gbufEyeNormal", *input.gbuffer.eyeNormal},
            {"gbufLightmap", *input.gbuffer.lightmap},
            {"gbufSelfIllum", *input.gbuffer.selfIllum},
            {"gbufMotion", *input.gbuffer.motion},
            {"gbufDepth", *input.gbuffer.depth},
            {"gbufTriangleId", *input.gbuffer.triangleId},
        }};
        _pipeline->updateBindings(
            1, _renderer.frameIndex(),
            {gbufferBindings.data(), static_cast<uint32_t>(gbufferBindings.size())});
    }
    // ScenePipeline's channel images, bound into set 2 by name every frame.
    // They used to be bound once at init, when the tracer owned them and their
    // identity was fixed for its lifetime; ScenePipeline owns them now, a resize
    // replaces them, and the binding has to follow - exactly like the G-buffer
    // block above, and for the same reason.
    {
        std::vector<TracingBinding> channelBindings;
        channelBindings.reserve(kNumTracingChannels);
        for (int i = 0; i < kNumTracingChannels; ++i) {
            channelBindings.emplace_back(kChannelBindingNames[i], *input.channels.images[i]);
        }
        _pipeline->updateBindings(2, _renderer.frameIndex(),
                                  {channelBindings.data(),
                                   static_cast<uint32_t>(channelBindings.size())});
    }
    // Texture ids are assigned by the resource cache at upload time. The set is
    // update-after-bind and partially-bound so new assets can take a slot
    // without rebuilding it or populating unrelated descriptors.
    const auto uploadedTextures = _renderer.resources().uploadedTextures();
    for (const auto &[id, texture] : uploadedTextures) {
        if (id >= _bindlessTextureCapacity) {
            throw std::runtime_error("Ray-query bindless texture array exhausted");
        }
        TracingBinding textureBinding {"bindlessTextures", *texture};
        textureBinding.arrayIndex = id;
        textureBinding.arrayElement = true;
        _pipeline->updateBindings(1, _renderer.frameIndex(), {&textureBinding, 1});
    }
    const auto uploadedTextureArrays = _renderer.resources().uploadedTextureArrays();
    for (const auto &[id, texture] : uploadedTextureArrays) {
        if (id >= _bindlessTextureCapacity) {
            throw std::runtime_error("Ray-query bindless texture array exhausted");
        }
        TracingBinding textureArrayBinding {"bindlessTextureArrays", *texture};
        textureArrayBinding.arrayIndex = id;
        textureArrayBinding.arrayElement = true;
        _pipeline->updateBindings(1, _renderer.frameIndex(), {&textureArrayBinding, 1});
    }
    _lastBindlessTextureCount = static_cast<uint32_t>(uploadedTextures.size());
    {
        // The channel images must be GENERAL before the kernel writes them as
        // storage. Only this frame's set is transitioned: ScenePipeline owns
        // both and hands over the one the kernel is about to write, and the
        // tracked transition is a no-op after the slot's first traced frame.
        commandBuffer.transitionImages(
            {input.channels.images.begin(), input.channels.images.end()},
            ImageLayout::General);
    }
    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[0] = globalsOffset;
    auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
    commandBuffer.bindRayTracingPipeline(_pipeline->pipeline());
    commandBuffer.bindRayTracingDescriptorSet(_pipeline->pipelineLayout(), 0,
                                              uniformSet, offsets.data(),
                                              static_cast<uint32_t>(offsets.size()));
    commandBuffer.bindRayTracingDescriptorSet(_pipeline->pipelineLayout(), 1,
                                              _pipeline->descriptorSet(1, _renderer.frameIndex()), nullptr, 0);
    const auto auxSet = _pipeline->descriptorSet(2, _renderer.frameIndex());
    commandBuffer.bindRayTracingDescriptorSet(_pipeline->pipelineLayout(), 2, auxSet, nullptr, 0);
    // Clamped rather than trusted: the option is user-editable in reone.cfg
    // and a zero would divide the accumulated radiance by zero.
    TracePushConstants constants {frameNumber,
                                  static_cast<uint32_t>(std::max(1, _options.pathTracingSamples)),
                                  std::max(0.0f, _options.ptSkyIntensity),
                                  std::max(0.0f, _options.ptEmissiveIntensity),
                                  std::max(0.0f, _options.ptLightmapIntensity),
                                  std::max(0.0f, _options.ptDirectIntensity),
                                  std::max(0.0001f, _options.ptRayOffset),
                                  std::max(0.0f, _options.ptSunIntensity),
                                  (_options.ptTraceStats ? 1u : 0u) |
                                      (_options.ptDirectChannel ? 4u : 0u) |
                                      (_options.parityDirect ? (1u << 12) : 0u) |
                                      (_options.fog ? (1u << 13) : 0u) |
                                      (_options.ptNee ? (1u << 14) : 0u) |
                                      (static_cast<uint32_t>(std::clamp(_options.debugView, 0, kMaxDebugView)) << 4) |
                                      (static_cast<uint32_t>(std::clamp(_options.tonemap, 0, 1)) << 10),
                                  static_cast<uint32_t>(
                                      std::clamp(_options.ptBounces, kMinPtBounces, kMaxPtBounces)),
                                  std::clamp(_options.ptPointEmitterRatio, 0.01f, 0.5f),
                                  glm::radians(std::clamp(_options.ptSunAngularSize, 0.05f, 10.0f)),
                                  std::clamp(_options.ptBounceRoughness, 0.0f, 1.0f),
                                  std::clamp(_options.ptRoughnessFloor, 0.0f, 1.0f),
                                  std::max(0.0f, _options.ptIndirectClamp),
                                  std::clamp(_options.thinTransmission, 0.0f, 1.0f),
                                  std::clamp(_options.ptRefraction, 0.0f, 1.0f),
                                  std::max(0.01f, _options.exposure),
                                  0,
                                  scene.opaqueTriangleCount,
                                  sky.baked ? 1u : 0u,
                                  std::clamp(_options.albedoGamma, 0.1f, 4.0f),
                                  std::max(0.0f, _options.ptBackdropIntensity),
                                  std::clamp(_options.emissiveGamma, 0.1f, 4.0f),
                                  static_cast<uint32_t>(std::clamp(_options.ptNeeSamples,
                                                                   kMinPtNeeSamples,
                                                                   kMaxPtNeeSamples))};
    commandBuffer.pushRayTracingConstants(_pipeline->pipelineLayout(), &constants, sizeof(constants));
    {
        R_PROFILE_ZONE("RayQuery::dispatch record");
        CommandBufferDebugScope traceScope(commandBuffer, "trace rays");
        commandBuffer.traceRays(
            _pipeline->pipeline(), input.structure,
            {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y)});
    }
#ifdef R_ENABLE_NRD
    if (_nrdDenoiser) {
        CommandBufferDebugScope denoiseScope(commandBuffer, "NRD denoise");
        // The trace pass's storage writes feed NRD's sampled reads. The channel
        // images are ScenePipeline's, handed in for this frame.
        const auto &channels = input.channels;
        std::array<IImage *, kNumTracingChannels + 1> traceOutputs {};
        traceOutputs[0] = &output;
        for (int i = 0; i < kNumTracingChannels; ++i)
            traceOutputs[i + 1] = channels.images[i];
        for (auto *image : traceOutputs) {
            commandBuffer.imageBarrier(*image, ImageUse::RayTracingStore, ImageUse::ComputeRead);
        }
        // Which image the composite reads for direct light, given what settled
        // it. Declared here so the binding list below stays one expression.
        const auto directChannelView = [&](IImage *raw) {
            switch (_options.ptShadowFilter) {
            case graphics::ShadowFilter::Denoiser:
                if (auto *denoised = _nrdDenoiser->denoisedDirect())
                    return denoised->sampleView();
                return raw->sampleView();
            default:
                return raw->sampleView();
            }
        };
        TracingDenoiserInputs inputs;
        inputs.diffRadianceHitDist = channels[ChannelSlot::Diffuse];
        inputs.specRadianceHitDist = channels[ChannelSlot::Specular];
        // Only when the direct channel exists as its own signal. With the
        // channel off the tracer sums direct light into the diffuse one, so
        // there is nothing separate to denoise and the second denoiser's
        // batch is not recorded at all.
        if (_options.ptDirectChannel &&
            _options.ptShadowFilter == graphics::ShadowFilter::Denoiser) {
            inputs.directRadianceHitDist = channels[ChannelSlot::DirectDiffuse];
        }
        inputs.normalRoughness = channels[ChannelSlot::NormalRoughness];
        inputs.viewZ = channels[ChannelSlot::ViewZ];
        inputs.motion = channels[ChannelSlot::NrdMotion];
        // The projection arrives carrying the TAA jitter (applied as a clip
        // translate); NRD is owed the unjittered matrix and the sub-pixel
        // offset separately, the latter in pixels with UV-down y.
        glm::mat4 unjitteredProjection =
            glm::translate(glm::vec3(-jitter.x, -jitter.y, 0.0f)) * projection;
        glm::vec2 jitterPixels {jitter.x * 0.5f * static_cast<float>(_extent.x),
                                -jitter.y * 0.5f * static_cast<float>(_extent.y)};
        TracingDenoiserTuning tuning;
        tuning.accumulationTime = _options.ptNrdAccumulationTime;
        tuning.fastAccumulationTime = _options.ptNrdFastAccumulationTime;
        tuning.historyFixFrames = _options.ptNrdHistoryFixFrames;
        tuning.directAccumulationTime = _options.ptNrdDirectAccumulationTime;
        tuning.directAtrousIterations = _options.ptNrdDirectAtrousIterations;
        tuning.directPhiLuminance = _options.ptNrdDirectPhiLuminance;
        tuning.diffusePrepassBlurRadius = _options.ptNrdDiffusePrepassBlurRadius;
        tuning.specularPrepassBlurRadius = _options.ptNrdSpecularPrepassBlurRadius;
        tuning.lobeAngleFraction = _options.ptNrdLobeAngleFraction;
        tuning.roughnessFraction = _options.ptNrdRoughnessFraction;
        tuning.disocclusionThreshold = _options.ptNrdDisocclusionThreshold;
        tuning.antiFirefly = _options.ptNrdAntiFirefly;
        tuning.atrousIterations = _options.ptNrdAtrousIterations;
        tuning.diffusePhiLuminance = _options.ptNrdDiffusePhiLuminance;
        tuning.specularPhiLuminance = _options.ptNrdSpecularPhiLuminance;
        tuning.depthThreshold = _options.ptNrdDepthThreshold;
        tuning.specularLobeAngleSlack = _options.ptNrdSpecularLobeAngleSlack;
        const bool restartHistory = frameNumber == 0 || _restartHistoryRequested;
        _restartHistoryRequested = false;
        _nrdDenoiser->denoise(commandBuffer, _renderer.frameIndex(), inputs, tuning, view, unjitteredProjection,
                               jitterPixels, frameNumber, restartHistory);
        // The composite is ScenePipeline's pass now. The tracer prepares only
        // what only it can produce: the denoiser's outputs, the shadow filter's
        // result, and the two push values the composite pushes. When there is
        // nothing to denoise, or the debug view asked for is one of the
        // kernel's own, the trace kernel has already written the final image
        // itself - runComposite stays false and the composite stands aside.
        if (_options.ptDenoise &&
            (_options.debugView == 0 || isResolveDebugView(_options.debugView))) {
            // Hand ScenePipeline the pieces its composite cannot get from the
            // channel images. The jitter sign is the negation of the offset
            // handed to NRD: the denoised channels settle on the pixel centre,
            // so they are read from where the jittered projection put that
            // content one offset earlier. Checked by measurement - the wrong
            // sign doubles the mismatch instead of cancelling it.
            if (input.composite) {
                input.composite->runComposite = true;
                input.composite->denoisedDiffuse = _nrdDenoiser->denoisedDiffuse().sampleView();
                input.composite->denoisedSpecular = _nrdDenoiser->denoisedSpecular().sampleView();
                input.composite->directDiffuse =
                    directChannelView(channels[ChannelSlot::DirectDiffuse]);
                input.composite->denoisedJitter[0] = -jitterPixels.x;
                input.composite->denoisedJitter[1] = -jitterPixels.y;
                input.composite->directDenoised =
                    _options.ptShadowFilter == graphics::ShadowFilter::Denoiser ? 1u : 0u;
                input.composite->debugView =
                    isResolveDebugView(_options.debugView)
                        ? static_cast<uint32_t>(_options.debugView)
                        : 0u;
            }
        }
    }
#endif
    previousStats.bindlessTextures = _lastBindlessTextureCount;
    return previousStats;
}

std::unique_ptr<ITracingStructure> TracingPipeline::makeTracingStructure() {
    return _renderer.makeTracingStructure();
}
} // namespace reone::graphics
