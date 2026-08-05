/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/gpuscene.h"
#include "reone/graphics/pipelinecache.h"
#include "reone/graphics/tracingstructure.h"

namespace reone::graphics {

class IRenderer;
class Mesh;
class Texture;
struct GraphicsOptions;

struct RayQuerySkyMesh {
    const Mesh *mesh {nullptr};
    const Texture *texture {nullptr};
    glm::mat4 transform {1.0f};
    glm::mat4 transformInv {1.0f};
    glm::mat4 prevTransform {1.0f};
    glm::mat3x4 uv {1.0f};
};

/** Backend-free description of the room selected for the fixed sky bake. */
struct RayQuerySkyRoom {
    uint64_t identity {0};
    std::string name;
    glm::vec3 origin {0.0f};
    std::vector<RayQuerySkyMesh> meshes;
};

/** What the scene side hands the tracer for this frame: the upload and its counts. */
struct RayQuerySubmission {
    GpuSceneUpload upload;
    uint32_t dynamicTriangles {0};
    uint32_t skinned {0};
    uint32_t deforming {0};
    uint32_t outOfRange {0};
    uint32_t emissive {0};
    uint32_t additive {0};
    uint32_t sabers {0};
    uint32_t sky {0};
    uint32_t dangly {0};
    uint32_t grass {0};
    uint32_t particles {0};
    uint32_t billboards {0};
};

class RayQuery : boost::noncopyable {
public:
    RayQuery(IRenderer &renderer, glm::ivec2 extent, GraphicsOptions &options);
    ~RayQuery();

    void init();
    void deinit();
    bool bakeSkyRoom(ICommandBuffer &commandBuffer, const RayQuerySkyRoom &room);
    void clearSkyRoom();
    void render(ICommandBuffer &commandBuffer, uint32_t globalsOffset,
                IImage &output, const glm::mat4 &view,
                const glm::mat4 &projection, const glm::vec4 &jitter,
                RayQuerySubmission submission, GpuScene &deviceGpuScene,
                bool skyBaked);

    using Channel = TracingChannel;
    std::vector<Channel> channels() const;
    void restartTemporalHistory();
    std::optional<uint32_t> textureId(const Texture &texture) const;
    bool supportsSkyTexture(const Texture &texture) const;

private:
    struct Frame {
        std::unique_ptr<ITracingStructure> tracingStructure;
    };

    IRenderer &_renderer;
    GraphicsOptions &_options;
    glm::ivec2 _extent;
    std::unique_ptr<ITracingPipeline> _pipeline;
    std::array<Frame, 2> _frames;
    uint32_t _lastInstances {0};
    uint32_t _lastTriangles {0};
    uint32_t _lastDynamicTriangles {0};
    uint32_t _lastSkinned {0};
    uint32_t _lastDeforming {0};
    uint32_t _lastOutOfRange {0};
    uint32_t _lastEmissive {0};
    uint32_t _lastAdditive {0};
    uint32_t _lastSabers {0};
    uint32_t _lastSky {0};
    uint32_t _lastDangly {0};
    uint32_t _lastGrass {0};
    uint32_t _lastParticles {0};
    uint32_t _lastBillboards {0};
    uint32_t _lastSecondaryRays {0};
    uint32_t _lastSecondaryMisses {0};
    uint32_t _lastSurvivingLights {0};
    uint32_t _lastPrimaryHits {0};
    uint32_t _lastShadowRays {0};
    uint32_t _lastBindlessTextureCount {0};
    uint32_t _frameNumber {0};
    bool _inited {false};

    void clearFrame(Frame &frame);
};

} // namespace reone::graphics
