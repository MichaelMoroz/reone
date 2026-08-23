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
#include <limits>

#include "reone/graphics/options.h"
#include "reone/graphics/rhi/computepipeline.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/system/logutil.h"

namespace reone::graphics {
namespace {

uint32_t cardRegionCapacity(const GraphicsOptions &options, const GpuScene::View &scene) {
    if (scene.grassCardCount == 0)
        return 0;
    uint64_t cardBudget = scene.grassCardCount;
    if (options.grassTriangleBudget > 0 && scene.grassCardTris != 0) {
        cardBudget = std::max(
            cardBudget,
            static_cast<uint64_t>(options.grassTriangleBudget) / scene.grassCardTris);
    }
    const uint64_t capacity = (cardBudget + kGrassCardVariants - 1) / kGrassCardVariants;
    if (capacity > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Grass card region capacity exceeds uint32 range");
    return static_cast<uint32_t>(capacity);
}

void releaseBuffer(std::unique_ptr<IBuffer> &buffer) {
    if (buffer)
        buffer->deinit();
    buffer.reset();
}

} // namespace

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
    _instancePipeline = _renderer.makeComputePipeline({"tracing_instances", "main", 2});
    _instanceBindings = _instancePipeline->resolveBindings(
        {"grassCardInstances", "tracingInstances", "variantCounts"});
    _inited = true;
}

void RayQuery::clearFrame(Frame &frame) {
    if (frame.tracingStructure) {
        frame.tracingStructure->deinit();
        frame.tracingStructure.reset();
    }
    releaseBuffer(frame.instances);
    releaseBuffer(frame.variantCounts);
    frame.cardRegionCapacity = 0;
    frame.variantCountsValid = false;
}

void RayQuery::deinit() {
    for (auto &frame : _frames)
        clearFrame(frame);
    if (_pipeline)
        _pipeline->deinit();
    _pipeline.reset();
    _instanceBindings.clear();
    _instancePipeline.reset();
    _lastVariantOverflows = {};
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
                       const SkyBinding &sky, const GBufferBinding &gbuffer) {
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
    _lastInstances = scene.vertices.buffer ? 1 + scene.grassCardCount : 0;
    if (!frame.tracingStructure)
        frame.tracingStructure = _pipeline->makeTracingStructure();

    const auto begin = std::chrono::steady_clock::now();
    if (scene.vertices.buffer) {
        R_PROFILE_ZONE("RayQuery::BLAS/TLAS build record");
        if (frame.variantCountsValid) {
            frame.variantCounts->invalidateMapped();
            const auto *counts = static_cast<const uint32_t *>(frame.variantCounts->mapped());
            std::array<uint32_t, kGrassCardVariants> overflows {};
            uint64_t dropped = 0;
            for (uint32_t variant = 0; variant < kGrassCardVariants; ++variant) {
                overflows[variant] = counts[variant] > frame.cardRegionCapacity
                    ? counts[variant] - frame.cardRegionCapacity
                    : 0;
                dropped += overflows[variant];
            }
            if (overflows != _lastVariantOverflows) {
                _lastVariantOverflows = overflows;
                if (dropped != 0) {
                    warn("Grass card TLAS regions overflowed: dropped " +
                             std::to_string(dropped) + " cards; per-variant overflow " +
                             std::to_string(overflows[0]) + "/" +
                             std::to_string(overflows[1]) + "/" +
                             std::to_string(overflows[2]) + "/" +
                             std::to_string(overflows[3]) + ", capacity " +
                             std::to_string(frame.cardRegionCapacity) + " each",
                         LogChannel::Graphics);
                }
            }
        }

        const uint32_t regionCapacity = cardRegionCapacity(_options, scene);
        const uint64_t instanceCount64 =
            1 + static_cast<uint64_t>(kGrassCardVariants) * regionCapacity;
        if (instanceCount64 > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Tracing instance count exceeds uint32 range");
        const uint32_t instanceCount = static_cast<uint32_t>(instanceCount64);
        if (!frame.instances || regionCapacity != frame.cardRegionCapacity) {
            releaseBuffer(frame.instances);
            frame.instances = _renderer.makeBuffer();
            frame.instances->initDeviceLocalStorage(
                static_cast<uint64_t>(instanceCount) * 64);
            frame.cardRegionCapacity = regionCapacity;
            ++frame.instanceGeneration;
        }
        if (!frame.variantCounts) {
            frame.variantCounts = _renderer.makeBuffer();
            frame.variantCounts->initHostVisibleReadback(
                kGrassCardVariants * sizeof(uint32_t));
        }
        _lastInstances = instanceCount;
        SceneTracingGeometry geometry {scene.vertices, scene.indices, scene.vertexCount,
                                       scene.opaqueTriangleCount, scene.triangleCount,
                                       scene.spriteTriangleCount,
                                       scene.grassCardVertices, scene.grassCardIndices,
                                       scene.grassCardInstances, scene.grassCardCount,
                                       scene.grassCardVerts, scene.grassCardTris,
                                       scene.grassCardGeneration, frame.instances.get(),
                                       instanceCount, regionCapacity,
                                       frame.instanceGeneration};
        commandBuffer.prepareSceneTracingStructure(*frame.tracingStructure, geometry);
        struct InstancePushConstants {
            uint32_t cardCount;
            uint32_t cardRegionCapacity;
            uint32_t instanceCount;
            uint32_t clearRecords;
        } constants {scene.grassCardCount, regionCapacity, instanceCount, 1};
        static_assert(sizeof(InstancePushConstants) == 16);
        const BufferView instanceView {frame.instances.get(), 0, frame.instances->size()};
        const BufferView countView {
            frame.variantCounts.get(), 0, kGrassCardVariants * sizeof(uint32_t)};
        const std::array<ComputeBinding, 3> bindings {{
            {_instanceBindings[0], scene.grassCardInstances},
            {_instanceBindings[1], instanceView},
            {_instanceBindings[2], countView},
        }};
        commandBuffer.dispatch(*_instancePipeline,
                               {(std::max(instanceCount, kGrassCardVariants) + 63) / 64, 1, 1},
                               {bindings.data(), static_cast<uint32_t>(bindings.size())}, nullptr,
                               &constants, sizeof(constants));
        if (scene.grassCardCount != 0) {
            commandBuffer.bufferBarrier(*frame.instances, BufferUse::ComputeWrite,
                                        BufferUse::ComputeWrite);
            commandBuffer.bufferBarrier(*frame.variantCounts, BufferUse::ComputeWrite,
                                        BufferUse::ComputeReadWrite);
            constants.clearRecords = 0;
            commandBuffer.dispatch(*_instancePipeline,
                                   {(scene.grassCardCount + 63) / 64, 1, 1},
                                   {bindings.data(), static_cast<uint32_t>(bindings.size())},
                                   nullptr, &constants, sizeof(constants));
        }
        commandBuffer.bufferBarrier(*frame.instances, BufferUse::ComputeWrite,
                                    BufferUse::AccelerationStructureBuildRead);
        commandBuffer.bufferBarrier(*frame.variantCounts, BufferUse::ComputeWrite,
                                    BufferUse::HostRead);
        commandBuffer.buildSceneTracingStructure(*frame.tracingStructure, geometry);
        frame.variantCountsValid = true;
    } else {
        commandBuffer.clearColor(output, {0.02f, 0.03f, 0.06f, 1.0f});
    }
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count();
    const auto stats = _pipeline->render({commandBuffer, globalsOffset, output,
                                          view, projection, jitter, scene,
                                          *frame.tracingStructure, frameIndex,
                                          _frameNumber, sky, gbuffer});
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
