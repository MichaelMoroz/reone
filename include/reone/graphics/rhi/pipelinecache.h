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

#include <string>
#include <memory>
#include <optional>
#include <vector>

#include "rhi.h"
#include "../mesh.h"
#include "../types.h"
#include "../rendering/gpuscene.h"
#include "../rendering/sky.h"

namespace reone {

namespace graphics {

/**
 * Shared push-constant range exposed by every cached graphics/compute layout.
 * ResolvePushConstants is the largest caller at five 32-bit words; smaller
 * passes may update only the prefix they use.
 */
constexpr uint32_t kCachedPipelinePushConstantSize = 5 * sizeof(uint32_t);

class ICommandBuffer;
class IImage;
class ITracingStructure;
class Texture;
struct GraphicsOptions;

/** Results read from the previous frame's trace counters. */
struct TracingStats {
    uint32_t secondaryRays {0};
    uint32_t secondaryMisses {0};
    uint32_t survivingLights {0};
    uint32_t primaryHits {0};
    uint32_t shadowRays {0};
    uint32_t bindlessTextures {0};
};

/**
 * The rasterized primary, handed to the tracer instead of a camera ray.
 *
 * Every mode runs the geometry pass before it shades anything, so by the time
 * the tracer starts these attachments already say which surface each pixel
 * shows. The kernel reads its primary out of them and traces only outwards; the
 * images must therefore be published as sampled before the trace is recorded,
 * not after it as they were when they were validation targets only.
 */
struct GBufferBinding {
    IImage *diffuse {nullptr};
    IImage *eyeNormal {nullptr};
    IImage *lightmap {nullptr};
    IImage *selfIllum {nullptr};
    IImage *motion {nullptr};
    IImage *depth {nullptr};
    IImage *triangleId {nullptr};
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
    SkyBinding sky;
    GBufferBinding gbuffer;
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
/** Pipeline state selected by the 2D and image-based-lighting clients. */
struct PipelineKey {
    std::string module;
    std::string vertexEntry;
    std::string fragmentEntry;
    /**
     * Set instead of the two above to build a compute pipeline over the same
     * descriptor set layouts every graphics pipeline here uses.
     *
     * The cache exists so that a pass can name a kernel and get one back with
     * the frame uniforms, the texture table, the merged material set and the
     * resolve set already in its layout. A dispatch whose resources are its own
     * wants IRenderer::makeComputePipeline instead, which reflects them.
     */
    std::string computeEntry;
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
    /** Empty when the vertex shader synthesises geometry from SV_VertexID. */
    std::optional<Mesh::VertexLayout> vertexLayout;
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

};

} // namespace graphics

} // namespace reone
