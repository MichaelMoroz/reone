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
        float skyGamma;
        float lightmapIntensity;
        float directIntensity;
        float rayOriginOffset;
        float sunIntensity;
        uint32_t traceFlags;
        uint32_t bounceCount;
        float emitterRadiusRatio;
        float sunAngularRadius;
        float bounceRoughness;
        float indirectClamp;
        float thinTransmission;
        float refraction;
        float exposure;
        uint32_t geometryBase0;
        uint32_t geometryBase1;
        uint32_t skyAvailable;
        float albedoGamma;
        /** Light samples per shading vertex; mirrors PtPushConstants.neeSamples. */
        uint32_t neeSamples;
        float lightDistanceClamp;
    };

    bool _restartHistoryRequested {false};
    bool _inited {false};
    /**
     * The blue-noise atlas, loaded once. Held as a Texture rather than an
     * IImage because the resource cache owns the upload and hands back the
     * image; this keeps the CPU-side pixels alive for as long as it is bound.
     *
     * Denoiser-independent: the trace kernel binds this whether or not a
     * denoiser is linked, so it lives outside the NRD guard.
     */
    static constexpr const char *kBlueNoiseFile = "bluenoise_rgba_64x64x64.tga";
    /** Mirrored by kPtBlueNoiseSize / kPtBlueNoiseTiles in slang/tracing/rng.slang. */
    static constexpr uint32_t kBlueNoiseTileSize = 64;
    static constexpr uint32_t kBlueNoiseGrid = 8;
    std::shared_ptr<Texture> _blueNoise;
#ifdef R_ENABLE_NRD
    std::unique_ptr<ITracingDenoiser> _nrdDenoiser;
#endif

    /**
     * The frame slot the last trace wrote, for indexing the shadow-filter
     * target below. The channel images themselves are ScenePipeline's now; this
     * only tracks which of the two shadow-filter buffers is current.
     */
    int _lastAuxFrame {-1};

    void clearFrame(Frame &frame);
};

} // namespace reone::graphics
