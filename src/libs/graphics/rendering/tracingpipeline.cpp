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

#include "reone/system/logutil.h"

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

void TracingPipeline::init() {
    if (_inited)
        return;
    _pipeline = _renderer.makeTracingPipeline(
        {"rayquery", _renderer.reflection("rayquery"), sizeof(TracePushConstants),
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
            Format::R32Sfloat,          // device depth, for the upscaler
            Format::R16G16B16A16Sfloat, // screen-space motion, for the upscaler
            Format::R16G16B16A16Sfloat, // specular material factor
            Format::R8G8B8A8Unorm,      // canonical raster/tracer diffuse
            Format::R8G8B8A8Unorm,      // canonical packed eye normal
            Format::R32Sfloat,          // canonical positive linear view depth
            Format::R16G16Sfloat,       // canonical current-minus-previous UV motion
        };
        static constexpr const char *kAuxBindingNames[kNumAuxImages] {
            "outDiffuse", "outSpecular", "outNormalRoughness", "outViewZ", "outMotion",
            "outNoiseFree", "outDiffFactor", "outDeviceDepth", "outScreenMotion", "outSpecFactor",
            "outGBufferDiffuse", "outGBufferEyeNormal", "outGBufferDepth", "outGBufferMotion"};
        for (int frame = 0; frame < 2; ++frame) {
            for (int i = 0; i < kNumAuxImages; ++i) {
                auto image = _renderer.resources().makeImage();
                image->initColorAttachment(_extent, kAuxFormats[i]);
                const TracingBinding binding {kAuxBindingNames[i], *image};
                _pipeline->updateBindings(2, frame, {&binding, 1});
                _auxImages[frame][i] = std::move(image);
            }
        }
    }

#ifdef R_ENABLE_NRD
    {
        _nrdDenoiser = _renderer.makeTracingDenoiser(_extent);
        if (_nrdDenoiser) {

            _compositePipeline = _renderer.makeComputePipeline({"nrd_composite", "main", 2});
            _compositeBindings = _compositePipeline->resolveBindings(
                {"outputImage", "inNoiseFree", "inDiffFactor", "inSpecFactor",
                 "inDenoisedDiffuse", "inDenoisedSpecular", "inViewZ", "inRawDiffuse",
                 "inRawSpecular"});
        }
    }
#endif
#ifdef R_ENABLE_FSR
    if (_options.ptFsr) {
        // Both at render resolution: NativeAA does not change the size, and the
        // composite/tonemap pair either side of FSR work on the same grid.
        _fsrColor = _renderer.resources().makeImage();
        _fsrColor->initColorAttachment(_extent, Format::R16G16B16A16Sfloat);
        _fsrOutput = _renderer.resources().makeImage();
        _fsrOutput->initColorAttachment(_extent, Format::R16G16B16A16Sfloat);

        _tonemapPipeline = _renderer.makeComputePipeline({"pt_tonemap", "main", 2});
        _tonemapBindings = _tonemapPipeline->resolveBindings({"outputImage", "inColor"});

        try {
            _fsr = _renderer.makeTracingUpscaler(_extent);
        } catch (const std::exception &e) {
            // Losing the upscaler must not lose the frame; it costs the
            // anti-aliasing, since FSR is the only temporal resolve left.
            warn(std::string("FSR unavailable, rendering without anti-aliasing: ") + e.what());
            _fsr.reset();
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
    _temporalHistoryValid = false;
    _nrdDenoiser.reset();
#endif
    _pipeline.reset();
    for (auto &frame : _auxImages) {
        for (auto &image : frame)
            image.reset();
    }
#ifdef R_ENABLE_FSR
    // Before the device goes: the upscaler owns native objects of its own, and
    // the two images own VMA allocations that must not outlive the allocator.
    _fsr.reset();
    _fsrColor.reset();
    _fsrOutput.reset();
    _tonemapPipeline.reset();
    _tonemapBindings.clear();
#endif
    _bindlessTextureCapacity = 0;
    _lastBindlessTextureCount = 0;
    _lastAuxFrame = -1;
    _inited = false;
}

void TracingPipeline::restartTemporalHistory() {
    _restartHistoryRequested = true;
#ifdef R_ENABLE_NRD
    _temporalHistoryValid = false;
#endif
}

std::vector<TracingChannel> TracingPipeline::channels() const {
    if (!_inited || _lastAuxFrame < 0) {
        return {};
    }
    // Order and names follow the aux bindings in tracing/outputs.slang.
    static constexpr const char *kNames[kNumAuxImages] {
        "Traced diffuse radiance", "Traced specular radiance", "Traced normal/roughness",
        "Traced viewZ", "Traced NRD motion", "Traced noise-free", "Traced diffuse factor",
        "Traced device depth", "Traced screen motion", "Traced specular factor",
        "Traced diffuse", "Traced eye normal", "Traced depth", "Traced motion"};
    static constexpr const char *kDumpNames[kNumAuxImages] {
        "traced_radiance_diffuse", "traced_radiance_specular", "traced_normal_roughness",
        "traced_view_z", "traced_nrd_motion", "traced_noise_free", "traced_diff_factor",
        "traced_device_depth", "traced_screen_motion", "traced_spec_factor",
        "traced_diffuse", "traced_eye_normal", "traced_depth", "traced_motion"};
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

    std::array<TracingBinding, 8> frameBindings {{
        {"outputImage", output},
        {"sceneTLAS", input.structure},
        {"instanceMaterials", {scene.materials.buffer, 0, scene.materials.size}},
        {"traceStats", {frame.traceStats.get(), 0, frame.traceStats->size()}},
        {"mergedVertices", scene.vertices},
        {"mergedIndices", scene.indices},
        {"mergedMaterialIds", scene.materialIds},
        {"skyCube", *sky.cube},
    }};
    frameBindings.back().imageView = sky.view;
    frameBindings.back().hasImageView = true;
    _pipeline->updateBindings(1, _renderer.frameIndex(),
                              {frameBindings.data(), static_cast<uint32_t>(frameBindings.size())});
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
                                  std::max(0.0f, _options.ptSkyIntensity),
                                  std::max(0.0f, _options.ptEmissiveIntensity),
                                  std::max(0.0f, _options.ptLightmapIntensity),
                                  std::max(0.0f, _options.ptDirectIntensity),
                                  std::max(0.0001f, _options.ptRayOffset),
                                  std::max(0.0f, _options.ptSunIntensity),
                                  (_options.ptTraceStats ? 1u : 0u) |
                                      (static_cast<uint32_t>(std::clamp(_options.ptDebugView, 0, 12)) << 4) |
                                      (static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)) << 8),
                                  static_cast<uint32_t>(std::clamp(_options.ptBounces, 1, 8)),
                                  std::clamp(_options.ptPointEmitterRatio, 0.01f, 0.5f),
                                  glm::radians(std::clamp(_options.ptSunAngularSize, 0.05f, 10.0f)),
                                  std::max(0.01f, _options.ptExposure),
                                  0,
                                  scene.opaqueTriangleCount,
                                  sky.baked ? 1u : 0u};
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
        TracingDenoiserInputs inputs;
        inputs.diffRadianceHitDist = aux[0].get();
        inputs.specRadianceHitDist = aux[1].get();
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
        tuning.maxAccumulatedFrames = _options.ptNrdMaxAccumulatedFrames;
        tuning.maxFastAccumulatedFrames = _options.ptNrdMaxFastAccumulatedFrames;
        tuning.maxStabilizedFrames = _options.ptNrdMaxStabilizedFrames;
        tuning.historyFixFrames = _options.ptNrdHistoryFixFrames;
        tuning.diffusePrepassBlurRadius = _options.ptNrdDiffusePrepassBlurRadius;
        tuning.specularPrepassBlurRadius = _options.ptNrdSpecularPrepassBlurRadius;
        tuning.minBlurRadius = _options.ptNrdMinBlurRadius;
        tuning.maxBlurRadius = _options.ptNrdMaxBlurRadius;
        tuning.lobeAngleFraction = _options.ptNrdLobeAngleFraction;
        tuning.roughnessFraction = _options.ptNrdRoughnessFraction;
        tuning.planeDistanceSensitivity = _options.ptNrdPlaneDistanceSensitivity;
        tuning.disocclusionThreshold = _options.ptNrdDisocclusionThreshold;
        tuning.antiFirefly = _options.ptNrdAntiFirefly;
        const bool restartHistory = frameNumber == 0 || _restartHistoryRequested;
        _restartHistoryRequested = false;
        _nrdDenoiser->denoise(commandBuffer, _renderer.frameIndex(), inputs, tuning, view, unjitteredProjection,
                               jitterPixels, frameNumber, restartHistory);
        if (_options.ptDenoise && _options.ptDebugView == 0) {
            // The noise-free history resets whenever NRD's would: first
            // frame, or a teleport-sized camera jump.
            const auto cameraPosition = glm::vec3(glm::inverse(view)[3]);
            if (frameNumber == 0 ||
                glm::distance(cameraPosition, _prevCameraPosition) > 20.0f) {
                _temporalHistoryValid = false;
            }
            _prevCameraPosition = cameraPosition;
            const bool temporalReset = !_temporalHistoryValid;

            // The assembly from denoised channels, overwriting the trace
            // kernel's own write. Debug views keep the kernel's output.
            // History ping-pong: the frame at index i reads what the previous
            // frame (index 1-i) wrote into slot i, and writes slot 1-i.
            // With FSR the composite hands off linear HDR to the upscaler
            // instead of writing the finished frame; the display transform
            // happens after, in pt_tonemap.
            bool fsrActive = false;
#ifdef R_ENABLE_FSR
            fsrActive = static_cast<bool>(_fsr);
#endif
            ImageView compositeTarget = output.sampleView();
#ifdef R_ENABLE_FSR
            if (fsrActive) {
                commandBuffer.transitionImage(*_fsrColor, ImageLayout::General);
                commandBuffer.transitionImage(*_fsrOutput, ImageLayout::General);
                compositeTarget = _fsrColor->sampleView();
            }
#endif
            // Motion is not among them: it existed only for the removed TAA's
            // reprojection. NRD still consumes it directly.
            constexpr uint32_t kCompositeBindings = 9;
            const std::array<ComputeBinding, kCompositeBindings> compositeBindings {{
                {_compositeBindings[0], compositeTarget},
                {_compositeBindings[1], aux[5]->sampleView()},
                {_compositeBindings[2], aux[6]->sampleView()},
                {_compositeBindings[3], aux[9]->sampleView()},
                {_compositeBindings[4], _nrdDenoiser->denoisedDiffuse().sampleView()},
                {_compositeBindings[5], _nrdDenoiser->denoisedSpecular().sampleView()},
                {_compositeBindings[6], aux[3]->sampleView()},
                {_compositeBindings[7], aux[0]->sampleView()},
                {_compositeBindings[8], aux[1]->sampleView()},
            }};
            struct CompositePush {
                uint32_t tonemap;
                float exposure;
                uint32_t linearOutput;
            } compositePush {static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)),
                             std::max(0.01f, _options.ptExposure),
                             fsrActive ? 1u : 0u};
            commandBuffer.dispatch(*_compositePipeline,
                                   {static_cast<uint32_t>((_extent.x + 7) / 8),
                                    static_cast<uint32_t>((_extent.y + 7) / 8), 1},
                                   {compositeBindings.data(),
                                    static_cast<uint32_t>(compositeBindings.size())},
                                   nullptr,
                                   &compositePush, sizeof(compositePush));
            _temporalHistoryValid = true;
#ifdef R_ENABLE_FSR
            if (fsrActive) {
                // Composite writes, FSR reads. FSR's backend barriers its own
                // internal resources but not ours, so the handoff is ours.
                commandBuffer.imageBarrier(*_fsrColor, ImageUse::ComputeStore,
                                           ImageUse::ComputeRead);

                TracingUpscalerInputs fsrInputs;
                fsrInputs.color = _fsrColor.get();
                fsrInputs.depth = aux[7].get();
                fsrInputs.motion = aux[8].get();
                fsrInputs.output = _fsrOutput.get();
                // The same sub-pixel offset NRD is given: FSR's jitter
                // convention and ours already agree, both being a pixel-space
                // Halton(2,3) with y negated for the UV-down axis.
                const float verticalFov =
                    2.0f * std::atan(1.0f / std::max(1e-4f, projection[1][1]));
                // Read the planes back out of the native projection
                // rather than plumbing them down: unprojecting both ends of
                // the depth range avoids depending on its coefficient layout.
                const glm::mat4 projectionInv = glm::inverse(projection);
                const glm::vec4 nearH = projectionInv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                const glm::vec4 farH = projectionInv * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
                const float cameraNear = std::abs(nearH.z / nearH.w);
                const float cameraFar = std::abs(farH.z / farH.w);
                _fsr->dispatch(commandBuffer, fsrInputs, jitterPixels, 1.0f / 60.0f,
                               cameraNear, cameraFar, verticalFov, _options.ptFsrSharpness,
                                frameNumber == 0 || temporalReset);

                // The backend deliberately leaves its inputs ready for sampled
                // reads. The trace and composite passes write these images as
                // storage images again on the next frame, so restore GENERAL.
                commandBuffer.imageBarrier(*_fsrColor, ImageUse::ComputeSample,
                                           ImageUse::ComputeStore);
                commandBuffer.imageBarrier(*aux[7], ImageUse::ComputeSample,
                                           ImageUse::ComputeStore);
                commandBuffer.imageBarrier(*aux[8], ImageUse::ComputeSample,
                                           ImageUse::ComputeStore);
                commandBuffer.imageBarrier(*_fsrOutput, ImageUse::ComputeStore,
                                           ImageUse::ComputeStorageRead);

                const std::array<ComputeBinding, 2> tonemapBindings {{
                    {_tonemapBindings[0], output.sampleView()},
                    {_tonemapBindings[1], _fsrOutput->sampleView()},
                }};
                struct TonemapPush {
                    uint32_t tonemap;
                    float exposure;
                } tonemapPush {static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)),
                               std::max(0.01f, _options.ptExposure)};
                commandBuffer.dispatch(*_tonemapPipeline,
                                       {static_cast<uint32_t>((_extent.x + 7) / 8),
                                        static_cast<uint32_t>((_extent.y + 7) / 8), 1},
                                       {tonemapBindings.data(),
                                        static_cast<uint32_t>(tonemapBindings.size())},
                                       nullptr,
                                       &tonemapPush, sizeof(tonemapPush));
            }
#endif
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
