/*
 * Copyright (c) 2026 The reone project contributors
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
#include "reone/graphics/rendering/rayquery.h"

#include "reone/graphics/rendering/tracingpipeline.h"

#include "reone/system/profiler.h"

#include <algorithm>
#include <chrono>

#include "reone/graphics/options.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/system/logutil.h"

namespace reone::graphics {

RayQuery::RayQuery(IRenderer &renderer, glm::ivec2 extent,
                   GraphicsOptions &options) :
    _renderer(renderer), _options(options), _extent(extent) {}

RayQuery::~RayQuery() {
    deinit();
}

void RayQuery::init() {
    if (_inited)
        return;
    _pipeline = std::make_unique<TracingPipeline>(_renderer, _extent, _options);
    _pipeline->init();
    _inited = true;
}

void RayQuery::clearFrame(Frame &frame) {
    if (frame.tracingStructure) {
        frame.tracingStructure->deinit();
        frame.tracingStructure.reset();
    }
}

void RayQuery::deinit() {
    for (auto &frame : _frames)
        clearFrame(frame);
    if (_pipeline)
        _pipeline->deinit();
    _pipeline.reset();
    _inited = false;
}

std::optional<uint32_t> RayQuery::textureId(const Texture &texture) const {
    return _renderer.resources().textureId(texture);
}

void RayQuery::restartTemporalHistory() {
    _pipeline->restartTemporalHistory();
}

std::vector<RayQuery::Channel> RayQuery::channels() const {
    return _pipeline ? _pipeline->channels() : std::vector<Channel> {};
}

void RayQuery::render(ICommandBuffer &commandBuffer, uint32_t globalsOffset,
                       IImage &output, const glm::mat4 &view,
                       const glm::mat4 &projection, const glm::vec4 &jitter,
                       RayQuerySubmission submission, const GpuScene::View &scene,
                       const SkyBinding &sky) {
    R_PROFILE_ZONE("RayQuery::render");
    const int frameIndex = _renderer.frameIndex();
    auto &frame = _frames[frameIndex];
    _lastDynamicTriangles = submission.dynamicTriangles;
    _lastSkinned = submission.skinned;
    _lastDeforming = submission.deforming;
    _lastOutOfRange = submission.outOfRange;
    _lastEmissive = submission.emissive;
    _lastAdditive = submission.additive;
    _lastSabers = submission.sabers;
    _lastSky = submission.sky;
    _lastDangly = submission.dangly;
    _lastGrass = submission.grass;
    _lastParticles = submission.particles;
    _lastBillboards = submission.billboards;
    _lastTriangles = scene.triangleCount;
    _lastInstances = scene.vertices.buffer ? 1 : 0;
    if (!frame.tracingStructure)
        frame.tracingStructure = _pipeline->makeTracingStructure();

    const auto begin = std::chrono::steady_clock::now();
    if (scene.vertices.buffer) {
        R_PROFILE_ZONE("RayQuery::BLAS/TLAS build record");
        commandBuffer.buildSceneTracingStructure(
            *frame.tracingStructure,
            {scene.vertices, scene.indices, scene.vertexCount,
             scene.opaqueTriangleCount, scene.triangleCount});
    } else {
        commandBuffer.clearColor(output, {0.02f, 0.03f, 0.06f, 1.0f});
    }
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count();
    const auto stats = _pipeline->render({commandBuffer, globalsOffset, output,
                                          view, projection, jitter, scene,
                                          *frame.tracingStructure, frameIndex,
                                          _frameNumber, sky});
    _lastSecondaryRays = stats.secondaryRays;
    _lastSecondaryMisses = stats.secondaryMisses;
    _lastSurvivingLights = stats.survivingLights;
    _lastPrimaryHits = stats.primaryHits;
    _lastShadowRays = stats.shadowRays;
    _lastBindlessTextureCount = stats.bindlessTextures;
    ++_frameNumber;

    const std::string statsPart = _options.ptTraceStats
        ? "previous frame secondary misses " + std::to_string(_lastSecondaryMisses) +
              "/" + std::to_string(_lastSecondaryRays) + "; "
        : "trace stats off; ";
    info("TLAS " + std::to_string(_lastInstances) + " instances, " +
             std::to_string(_lastTriangles / 1000) + "k triangles (" +
             std::to_string(_lastDynamicTriangles / 1000) + "k dynamic), " +
             std::to_string(_lastSkinned) + " skinned, skipped " +
             std::to_string(_lastDeforming) + " deforming and " +
             std::to_string(_lastOutOfRange) + " out-of-range meshes, build recorded in " +
             std::to_string(microseconds) + " us; " + std::to_string(_lastEmissive) +
             " emissive, " + std::to_string(_lastAdditive) + " additive, " +
             std::to_string(_lastSabers) + " saber, " + std::to_string(_lastDangly) + " dangly, " +
             std::to_string(_lastGrass) + " grass clusters, " + std::to_string(_lastParticles) + " particles, " +
             std::to_string(_lastBillboards) + " billboards, " + std::to_string(_lastSky) + " sky; " +
             statsPart + std::to_string(_lastBindlessTextureCount) +
             " bindless 2D textures; " +
             std::to_string(std::max(1, _options.pathTracingSamples)) + " spp",
         LogChannel::Graphics);
}

} // namespace reone::graphics
