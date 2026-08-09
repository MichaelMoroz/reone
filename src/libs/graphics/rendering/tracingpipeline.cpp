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

    // The NRD output split: seven storage images in their own set, because
    // the main set's bindless arrays hold the variable-descriptor-count slot
    // and the API allows nothing above it. Plain pool, static writes - the
    // images never change identity within a pipeline lifetime.
    {
        // Formats mirror the shader's declarations; normal/roughness rides
        // RGBA16F, the FP form NRD's RGBA16_SNORM encoding accepts.
        static constexpr Format kAuxFormats[kNumAuxImages] {
            Format::R16G16B16A16Sfloat, // diffuse radiance + hit dist
            Format::R16G16B16A16Sfloat, // specular radiance + hit dist
            Format::R16G16B16A16Sfloat, // normal + roughness
            Format::R32Sfloat,          // viewZ
            Format::R16G16B16A16Sfloat, // motion
            Format::R16G16B16A16Sfloat, // noise-free
            Format::R16G16B16A16Sfloat, // diffuse material factor
            // Diagnostics only since the upscaler moved to the common tail and
            // took its guides from the G-buffer instead. Kept because they are
            // the traced counterparts the G-buffer pair is compared against.
            Format::R32Sfloat,          // device depth
            Format::R16G16B16A16Sfloat, // screen-space motion
            Format::R16G16B16A16Sfloat, // specular material factor
            Format::R8G8B8A8Unorm,      // canonical raster/tracer diffuse
            Format::R8G8B8A8Unorm,      // canonical packed eye normal
            Format::R32Sfloat,          // canonical positive linear view depth
            Format::R16G16Sfloat,       // canonical current-minus-previous UV motion
            // Direct diffuse at the primary vertex, straight to the resolve.
            Format::R16G16B16A16Sfloat, // direct diffuse + expected penumbra
        };
        static constexpr const char *kAuxBindingNames[kNumAuxImages] {
            "outDiffuse", "outSpecular", "outNormalRoughness", "outViewZ", "outMotion",
            "outNoiseFree", "outDiffFactor", "outDeviceDepth", "outScreenMotion", "outSpecFactor",
            "outGBufferDiffuse", "outGBufferEyeNormal", "outGBufferDepth", "outGBufferMotion",
            "outDirectDiffuse"};
        for (int frame = 0; frame < 2; ++frame) {
            for (int i = 0; i < kNumAuxImages; ++i) {
                auto image = _renderer.resources().makeImage();
                image->initColorAttachment(_extent, kAuxFormats[i]);
                const TracingBinding binding {kAuxBindingNames[i], *image};
                _pipeline->updateBindings(2, frame, {&binding, 1});
                _auxImages[frame][i] = std::move(image);
            }
            auto filtered = _renderer.resources().makeImage();
            filtered->initColorAttachment(_extent, Format::R16G16B16A16Sfloat);
            _shadowFiltered[frame] = std::move(filtered);
        }
    }

    loadBlueNoise();

#ifdef R_ENABLE_NRD
    {
        _nrdDenoiser = _renderer.makeTracingDenoiser(_extent);
        if (_nrdDenoiser) {

            _shadowFilterPipeline = _renderer.makeComputePipeline({"shadow_filter", "main", 2});
            _shadowFilterBindings = _shadowFilterPipeline->resolveBindings(
                {"outFiltered", "inDirectDiffuse", "inViewZ", "inNormalRoughness"});
            _compositePipeline = _renderer.makeComputePipeline({"nrd_resolve", "main", 2});
            _compositeBindings = _compositePipeline->resolveBindings(
                {"outputImage", "inNoiseFree", "inDiffFactor", "inSpecFactor",
                 "inDenoisedDiffuse", "inDenoisedSpecular", "inViewZ", "inRawDiffuse",
                 "inRawSpecular", "inDirectDiffuse"});
        }
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
    _compositePipeline.reset();
    _compositeBindings.clear();
    _nrdDenoiser.reset();
#endif
    _pipeline.reset();
    for (auto &frame : _auxImages) {
        for (auto &image : frame)
            image.reset();
    }
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
    // Order and names follow the aux bindings in tracing/outputs.slang.
    // Unsized on purpose, with the count asserted below. Written as
    // kNames[kNumAuxImages] these tables accept too few initialisers without a
    // word of complaint - C++ value-initialises the rest to nullptr - and the
    // missing entry only shows up as a null dereference when someone opens the
    // viewer. Adding an aux image is now a build error until it is named.
    static constexpr const char *kNames[] {
        "Traced diffuse radiance", "Traced specular radiance", "Traced normal/roughness",
        "Traced viewZ", "Traced NRD motion", "Traced noise-free", "Traced diffuse factor",
        "Traced device depth", "Traced screen motion", "Traced specular factor",
        "Traced diffuse", "Traced eye normal", "Traced depth", "Traced motion",
        "Traced direct diffuse"};
    static constexpr const char *kDumpNames[] {
        "traced_radiance_diffuse", "traced_radiance_specular", "traced_normal_roughness",
        "traced_view_z", "traced_nrd_motion", "traced_noise_free", "traced_diff_factor",
        "traced_device_depth", "traced_screen_motion", "traced_spec_factor",
        "traced_diffuse", "traced_eye_normal", "traced_depth", "traced_motion",
        "traced_direct_diffuse"};
    static_assert(std::size(kNames) == kNumAuxImages);
    static_assert(std::size(kDumpNames) == kNumAuxImages);
    const auto &aux = _auxImages[_lastAuxFrame];
    std::vector<TracingChannel> result;
    for (int i = 0; i < kNumAuxImages; ++i) {
        if (aux[i]) {
            result.push_back({kNames[i], kDumpNames[i], aux[i].get()});
        }
    }
#ifdef R_ENABLE_NRD
    // What NRD made of the two radiance channels. Comparing these against the
    // raw pair above is the difference between "the tracer is noisy" and "the
    // denoiser is not removing it".
    if (_nrdDenoiser) {
        result.push_back({"Denoised diffuse", "denoised_diffuse", &_nrdDenoiser->denoisedDiffuse()});
        result.push_back({"Denoised specular", "denoised_specular", &_nrdDenoiser->denoisedSpecular()});
    }
    if (auto &filtered = _shadowFiltered[_lastAuxFrame]) {
        result.push_back({"Shadow filtered direct", "shadow_filtered", filtered.get()});
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

    std::array<TracingBinding, 9> frameBindings {{
        {"outputImage", output},
        {"sceneTLAS", input.structure},
        {"instanceMaterials", {scene.materials.buffer, 0, scene.materials.size}},
        {"traceStats", {frame.traceStats.get(), 0, frame.traceStats->size()}},
        {"mergedVertices", scene.vertices},
        {"mergedIndices", scene.indices},
        {"mergedMaterialIds", scene.materialIds},
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
        // Every aux image lives in GENERAL. The tracked transition is a no-op
        // after the first traced frame while retaining the one dependency.
        for (int frameIndex = 0; frameIndex < 2; ++frameIndex) {
            for (int i = 0; i < kNumAuxImages; ++i) {
                commandBuffer.transitionImage(*_auxImages[frameIndex][i], ImageLayout::General);
            }
        }
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
                                  std::max(0.0f, _options.skyIntensity),
                                  std::max(0.0f, _options.ptEmissiveIntensity),
                                  std::max(0.0f, _options.ptLightmapIntensity),
                                  std::max(0.0f, _options.ptDirectIntensity),
                                  std::max(0.0001f, _options.ptRayOffset),
                                  std::max(0.0f, _options.ptSunIntensity),
                                  (_options.ptTraceStats ? 1u : 0u) |
                                      (_options.ptDirectChannel ? 4u : 0u) |
                                      (static_cast<uint32_t>(std::clamp(_options.debugView, 0, kMaxDebugView)) << 4) |
                                      (static_cast<uint32_t>(std::clamp(_options.tonemap, 0, 1)) << 10),
                                  static_cast<uint32_t>(std::clamp(_options.ptBounces, 1, 8)),
                                  std::clamp(_options.ptPointEmitterRatio, 0.01f, 0.5f),
                                  glm::radians(std::clamp(_options.ptSunAngularSize, 0.05f, 10.0f)),
                                  std::clamp(_options.ptBounceRoughness, 0.0f, 1.0f),
                                  std::clamp(_options.ptRoughnessFloor, 0.0f, 1.0f),
                                  std::max(0.0f, _options.ptIndirectClamp),
                                  std::clamp(_options.thinTransmission, 0.0f, 1.0f),
                                  std::max(0.01f, _options.exposure),
                                  0,
                                  scene.opaqueTriangleCount,
                                  sky.baked ? 1u : 0u,
                                  std::clamp(_options.albedoGamma, 0.1f, 4.0f),
                                  std::max(0.0f, _options.ptBackdropIntensity),
                                  std::clamp(_options.emissiveGamma, 0.1f, 4.0f)};
    commandBuffer.pushRayTracingConstants(_pipeline->pipelineLayout(), &constants, sizeof(constants));
    {
        R_PROFILE_ZONE("RayQuery::dispatch record");
        commandBuffer.traceRays(
            _pipeline->pipeline(), input.structure,
            {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y)});
    }
#ifdef R_ENABLE_NRD
    if (_nrdDenoiser) {
        // The trace pass's storage writes feed NRD's sampled reads.
        const auto &aux = _auxImages[_renderer.frameIndex()];
        std::array<IImage *, kNumAuxImages + 1> traceOutputs {};
        traceOutputs[0] = &output;
        for (int i = 0; i < kNumAuxImages; ++i)
            traceOutputs[i + 1] = aux[i].get();
        for (auto *image : traceOutputs) {
            commandBuffer.imageBarrier(*image, ImageUse::RayTracingStore, ImageUse::ComputeRead);
        }
        // Which image the composite reads for direct light, given what settled
        // it. Declared here so the binding list below stays one expression.
        const auto directChannelView = [&](IImage *raw) {
            switch (_options.ptShadowFilter) {
            case graphics::ShadowFilter::Penumbra:
                return _shadowFiltered[_renderer.frameIndex()]->sampleView();
            case graphics::ShadowFilter::Denoiser:
                if (auto *denoised = _nrdDenoiser->denoisedDirect())
                    return denoised->sampleView();
                return raw->sampleView();
            default:
                return raw->sampleView();
            }
        };
        TracingDenoiserInputs inputs;
        inputs.diffRadianceHitDist = aux[0].get();
        inputs.specRadianceHitDist = aux[1].get();
        // Only when the direct channel exists as its own signal. With the
        // channel off the tracer sums direct light into the diffuse one, so
        // there is nothing separate to denoise and the second denoiser's
        // batch is not recorded at all.
        if (_options.ptDirectChannel &&
            _options.ptShadowFilter == graphics::ShadowFilter::Denoiser) {
            inputs.directRadianceHitDist = aux[14].get();
        }
        inputs.normalRoughness = aux[2].get();
        inputs.viewZ = aux[3].get();
        inputs.motion = aux[4].get();
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
        // The composite normally stands aside for a debug view, because the
        // kernel has already written the channel it was asked for. Three of
        // them are this pass's own output and cannot exist before it runs, so
        // for those it goes ahead and writes them itself.
        if (_options.ptDenoise &&
            (_options.debugView == 0 || isResolveDebugView(_options.debugView))) {
            // The assembly from denoised channels, overwriting the trace
            // kernel's own write. Debug views keep the kernel's output.
            //
            // It writes the scene output directly and stops at linear HDR. The
            // temporal resolve and the display transform both live in the
            // common tail now, so this pass no longer has to know whether
            // either of them is running.
            //
            // Motion is not among the bindings: it existed only for the removed
            // TAA's reprojection. NRD still consumes it directly.
            constexpr uint32_t kCompositeBindings = 10;
            const std::array<ComputeBinding, kCompositeBindings> compositeBindings {{
                {_compositeBindings[0], output.sampleView()},
                {_compositeBindings[1], aux[5]->sampleView()},
                {_compositeBindings[2], aux[6]->sampleView()},
                {_compositeBindings[3], aux[9]->sampleView()},
                {_compositeBindings[4], _nrdDenoiser->denoisedDiffuse().sampleView()},
                {_compositeBindings[5], _nrdDenoiser->denoisedSpecular().sampleView()},
                {_compositeBindings[6], aux[3]->sampleView()},
                {_compositeBindings[7], aux[0]->sampleView()},
                {_compositeBindings[8], aux[1]->sampleView()},
                // Whichever of the three settled this channel. The denoiser's
                // output only exists when its batch actually ran, so this falls
                // back to the raw channel rather than binding an image the
                // denoiser never wrote.
                {_compositeBindings[9],
                 directChannelView(aux[14].get())},
            }};
            if (_options.ptShadowFilter == graphics::ShadowFilter::Penumbra) {
                // Mirrors ShadowFilterPushConstants in slang/shadow_filter.slang.
                struct ShadowFilterPushConstants {
                    float pixelWorldPerDepth;
                    float maxRadius;
                    float depthTolerance;
                    float normalTolerance;
                    float radiusScale;
                    float minRadius;
                } filterConstants {
                    // 2*tan(fovY/2)/height, read back off the projection rather
                    // than from a field: projection[1][1] is 1/tan(fovY/2), and
                    // taking it from here cannot disagree with the matrix the
                    // frame was actually rendered with.
                    2.0f / (projection[1][1] * static_cast<float>(_extent.y)),
                    std::max(1.0f, _options.ptShadowFilterMaxRadius),
                    std::max(1e-4f, _options.ptShadowFilterDepthTolerance),
                    _options.ptShadowFilterNormalTolerance,
                    std::max(0.0f, _options.ptShadowFilterRadiusScale),
                    std::max(0.0f, _options.ptShadowFilterMinRadius)};
                auto &filtered = *_shadowFiltered[_renderer.frameIndex()];
                const std::array<ComputeBinding, 4> filterBindings {{
                    {_shadowFilterBindings[0], filtered.sampleView()},
                    {_shadowFilterBindings[1], aux[14]->sampleView()},
                    {_shadowFilterBindings[2], aux[3]->sampleView()},
                    {_shadowFilterBindings[3], aux[2]->sampleView()},
                }};
                commandBuffer.dispatch(*_shadowFilterPipeline,
                                       {static_cast<uint32_t>((_extent.x + 7) / 8),
                                        static_cast<uint32_t>((_extent.y + 7) / 8), 1},
                                       {filterBindings.data(),
                                        static_cast<uint32_t>(filterBindings.size())},
                                       nullptr, &filterConstants, sizeof(filterConstants));
                commandBuffer.imageBarrier(filtered, ImageUse::ComputeStore, ImageUse::ComputeRead);
            }
            // Mirrors NrdResolvePushConstants in slang/nrd_resolve.slang.
            struct NrdResolvePushConstants {
                uint32_t debugView;
            } resolveConstants {isResolveDebugView(_options.debugView)
                                    ? static_cast<uint32_t>(_options.debugView)
                                    : 0u};
            commandBuffer.dispatch(*_compositePipeline,
                                   {static_cast<uint32_t>((_extent.x + 7) / 8),
                                    static_cast<uint32_t>((_extent.y + 7) / 8), 1},
                                   {compositeBindings.data(),
                                    static_cast<uint32_t>(compositeBindings.size())},
                                   nullptr, &resolveConstants, sizeof(resolveConstants));
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
