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
#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <unordered_map>

#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/renderer.h"

namespace reone::graphics {

class TracingPipeline : boost::noncopyable {
public:
    TracingPipeline(IRenderer &renderer, glm::ivec2 extent,
                    GraphicsOptions &options);
    ~TracingPipeline();

    void init();
    void loadBlueNoise();
    void deinit();
    std::unique_ptr<ITracingStructure> makeTracingStructure();
    TracingStats render(const TracingPipelineInput &input);

    std::vector<TracingChannel> channels() const;
    void restartTemporalHistory();

private:
    struct Frame {
        std::unique_ptr<IBuffer> traceStats;
    };

    IRenderer &_renderer;
    GraphicsOptions &_options;
    glm::ivec2 _extent;
    std::unique_ptr<ITracingPipeline> _pipeline;
    std::array<Frame, 2> _frames;
    uint32_t _bindlessTextureCapacity {0};
    uint32_t _lastBindlessTextureCount {0};

    struct TracePushConstants {
        uint32_t frameIndex;
        uint32_t samplesPerPixel;
        float skyIntensity;
        float emissiveIntensity;
        float lightmapIntensity;
        float directIntensity;
        float rayOriginOffset;
        float sunIntensity;
        uint32_t traceFlags;
        uint32_t bounceCount;
        float emitterRadiusRatio;
        float sunAngularRadius;
        float bounceRoughness;
        float roughnessFloor;
        float indirectClamp;
        float thinTransmission;
        float exposure;
        uint32_t geometryBase0;
        uint32_t geometryBase1;
        uint32_t skyAvailable;
        float albedoGamma;
    };

    bool _restartHistoryRequested {false};
    bool _inited {false};
#ifdef R_ENABLE_NRD
    /**
     * Fixed when the instance is built, and read by the trace kernel as well:
     * the two denoisers want their radiance packed differently, so the writer
     * and the reader have to agree on one answer for the whole frame rather
     * than each consulting the live option.
     */
    /**
     * The blue-noise atlas, loaded once. Held as a Texture rather than an
     * IImage because the resource cache owns the upload and hands back the
     * image; this keeps the CPU-side pixels alive for as long as it is bound.
     */
    static constexpr const char *kBlueNoiseFile = "bluenoise_rgba_64x64x64.tga";
    /** Mirrored by kPtBlueNoiseSize / kPtBlueNoiseTiles in slang/tracing/rng.slang. */
    static constexpr uint32_t kBlueNoiseTileSize = 64;
    static constexpr uint32_t kBlueNoiseGrid = 8;
    std::shared_ptr<Texture> _blueNoise;
    /**
     * The shadow filter's target. Not an aux image: those are bound into the
     * trace kernel's own set by name, and the kernel neither writes nor reads
     * this one - it is produced by a later pass and consumed by a later one
     * still. Double-buffered like the rest, so two frames in flight cannot be
     * writing and reading the same texels.
     */
    std::array<std::unique_ptr<IImage>, 2> _shadowFiltered;
    std::unique_ptr<IComputePipeline> _shadowFilterPipeline;
    std::vector<ComputeResourceSlot> _shadowFilterBindings;
    TracingDenoiserKind _denoiserKind {TracingDenoiserKind::Relax};
    std::unique_ptr<ITracingDenoiser> _nrdDenoiser;
    std::unique_ptr<IComputePipeline> _compositePipeline;
    std::vector<ComputeResourceSlot> _compositeBindings;
#endif

    static constexpr int kNumAuxImages = 15;
    std::array<std::array<std::unique_ptr<IImage>, kNumAuxImages>, 2> _auxImages;
    int _lastAuxFrame {-1};

    void clearFrame(Frame &frame);
};

} // namespace reone::graphics
