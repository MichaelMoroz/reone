/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <string>
#include <memory>
#include <vector>

#include "rhi.h"
#include "types.h"
#include "gpuscene.h"

namespace reone {

namespace graphics {

class ICommandBuffer;
class IImage;
class ITracingStructure;
class Texture;
struct GraphicsOptions;
struct RayQuerySkyRoom;

/** Results read from the previous frame's trace counters. */
struct TracingStats {
    uint32_t secondaryRays {0};
    uint32_t secondaryMisses {0};
    uint32_t survivingLights {0};
    uint32_t primaryHits {0};
    uint32_t shadowRays {0};
    uint32_t bindlessTextures {0};
};

/** Everything the tracing implementation needs to turn one merged scene into pixels. */
struct TracingPipelineInput {
    ICommandBuffer &commandBuffer;
    uint32_t globalsOffset {0};
    IImage &output;
    const glm::mat4 &view;
    const glm::mat4 &projection;
    const glm::vec4 &jitter;
    const GpuScene::View &scene;
    ITracingStructure &structure;
    int frameIndex {0};
    uint32_t frameNumber {0};
    bool skyBaked {false};
};

struct TracingChannel {
    const char *name {nullptr};
    const char *dumpName {nullptr};
    IImage *image {nullptr};
};

/**
 * Traces an admitted merged scene, including its native descriptor, denoising
 * and upscaling work. The caller supplies scene meaning; the backend owns how
 * that work is expressed to its API.
 */
class ITracingPipeline {
public:
    virtual ~ITracingPipeline() = default;

    virtual void init() = 0;
    virtual void deinit() = 0;
    virtual std::unique_ptr<ITracingStructure> makeTracingStructure() = 0;
    virtual bool bakeSkyRoom(ICommandBuffer &commandBuffer,
                             const RayQuerySkyRoom &room) = 0;
    virtual void clearSkyRoom() = 0;
    virtual bool supportsSkyTexture(const Texture &texture) const = 0;
    virtual TracingStats render(const TracingPipelineInput &input) = 0;
    virtual void restartTemporalHistory() = 0;
    virtual std::vector<TracingChannel> channels() const = 0;
};

/** Pipeline state selected by the 2D and image-based-lighting clients. */
struct PipelineKey {
    std::string module;
    std::string vertexEntry;
    std::string fragmentEntry;
    std::vector<Format> colorFormats;
    Format depthFormat {Format::D32Sfloat};
    uint32_t viewMask {0};
    BlendMode blend {BlendMode::None};
    bool depthTest {false};
    bool depthWrite {false};
    bool depthBias {false};
    float depthBiasConstantFactor {0.0f};
    float depthBiasSlopeFactor {0.0f};
    FaceCullMode cull {FaceCullMode::None};
};

struct PipelineBinding {
    Pipeline pipeline;
    PipelineLayout layout;
};

/** Pipeline-cache operation used by the 2D and image-based-lighting clients. */
class IPipelineCache {
public:
    virtual ~IPipelineCache() = default;

    virtual PipelineBinding get(const PipelineKey &key) = 0;

    /** Build the native pipeline which traces an admitted scene into its output. */
    virtual std::unique_ptr<ITracingPipeline> makeTracingPipeline(
        glm::ivec2 extent, GraphicsOptions &options) = 0;
};

} // namespace graphics

} // namespace reone
