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
#include "reone/graphics/rendering/skystage.h"

#include <glm/gtc/matrix_transform.hpp>

#include "reone/graphics/texture.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/rhi/commandbuffer.h"
#include "reone/graphics/rhi/descriptors.h"
#include "reone/graphics/rhi/image.h"
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/graphics/rhi/uniformring.h"

#include "reone/system/logutil.h"

namespace reone::graphics {
namespace {

// Sky cubemap face resolution. Measured on danm14ab against the geometry sky
// it replaces, as a ratio of surviving horizontal detail: 512 keeps 0.59,
// 1024 keeps 0.73, 2048 keeps 0.77 for four times the memory. The curve is
// already flattening at 1024, so the rest of the gap is resampling and
// filtering rather than resolution, and paying 192 MB for it buys little.
// Frame time is flat across all three.
static constexpr uint32_t kSkyCubeSize = 1024;

} // namespace

SkyStage::SkyStage(IRenderer &renderer) :
    _renderer(renderer) {}

SkyStage::~SkyStage() {
    deinit();
}

void SkyStage::init() {
    if (_inited)
        return;

    // A descriptor is required even when no room qualifies. Sampling is gated
    // in the shader, but this black cube keeps the descriptor type valid.
    const float black[4] {0.0f, 0.0f, 0.0f, 1.0f};
    _skyFallbackCube = _renderer.resources().makeImage("Path-traced sky fallback cube");
    _skyFallbackCube->initSampledLayered({1, 1}, Format::R16G16B16A16Sfloat,
                                         kNumCubeFaces, true, black);
    _skyFallbackCube->setSampler(
        _renderer.resources().sampler(getTextureProperties(TextureUsage::ColorBuffer)));
    _inited = true;
}

void SkyStage::deinit() {
    _skyCube.reset();
    for (auto &depth : _skyDepth)
        depth.reset();
    _skyFallbackCube.reset();
    _skyCubeRoom = 0;
    _skyCubeReady = false;
    _inited = false;
}

bool SkyStage::bakeSkyRoom(ICommandBuffer &commandBuffer,
                           const RayQuerySkyRoom &room) {
    // A failed bake is deliberately sticky for this detected room: the fallback
    // cube is stable, and retrying a known-invalid asset every frame would turn
    // that path into a standing cost. Admission suppresses the shell either way.
    if (_skyCubeRoom == room.identity) {
        return _skyCubeReady;
    }
    _skyCubeRoom = room.identity;
    _skyCubeReady = false;
    if (room.meshes.empty())
        return false;

    auto &resources = _renderer.resources();
    auto &ring = _renderer.uniformRing();
    auto &descriptors = _renderer.descriptors();
    if (!_skyCube) {
        _skyCube = resources.makeImage("Path-traced sky cube");
        _skyCube->initCubeArrayAttachment({kSkyCubeSize, kSkyCubeSize}, Format::R16G16B16A16Sfloat, 1, 1);
        _skyCube->setSampler(
            resources.sampler(getTextureProperties(TextureUsage::ColorBuffer)));
    }
    bool createDepth = !_skyDepth[0];
    if (createDepth) {
        for (auto &depth : _skyDepth) {
            depth = resources.makeImage();
            depth->initDepthAttachment({kSkyCubeSize, kSkyCubeSize}, Format::D32Sfloat);
        }
    }

    commandBuffer.transitionImage(*_skyCube, ImageLayout::ColorAttachment);
    if (createDepth) {
        for (int face = 0; face < kNumCubeFaces; ++face) {
            commandBuffer.transitionImage(*_skyDepth[face], ImageLayout::DepthAttachment);
        }
    }

    static const glm::vec3 kDirections[kNumCubeFaces] {
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
    };
    static const glm::vec3 kUps[kNumCubeFaces] {
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
    };
    const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10000.0f);
    const auto uniformSet = descriptors.uniformDescriptorSet(_renderer.frameIndex());

    for (int face = 0; face < kNumCubeFaces; ++face) {
        GlobalUniforms globals;
        globals.reset();
        globals.projection = projection;
        globals.projectionInv = glm::inverse(projection);
        globals.view = glm::lookAt(room.origin, room.origin + kDirections[face], kUps[face]);
        globals.viewInv = glm::inverse(globals.view);
        globals.viewProjection = globals.projection * globals.view;
        globals.prevViewProjection = globals.viewProjection;
        globals.cameraPosition = glm::vec4(room.origin, 1.0f);
        std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = ring.push(globals);
        ring.setGlobalsOffset(offsets[UniformBlockBindingPoints::globals]);

        ClearValue colorClear;
        colorClear.color = {0.0f, 0.0f, 0.0f, 1.0f};
        const RenderAttachment color {_skyCube->faceAttachmentView(0, face),
                                      ImageLayout::ColorAttachment,
                                      AttachmentLoad::Clear,
                                      AttachmentStore::Store,
                                      colorClear};
        ClearValue depthClear;
        depthClear.depth = 1.0f;
        depthClear.depthOnly = true;
        const RenderAttachment depth {_skyDepth[face]->sampleView(),
                                      ImageLayout::DepthAttachment,
                                      AttachmentLoad::Clear,
                                      AttachmentStore::DontCare,
                                      depthClear};
        commandBuffer.beginRendering({kSkyCubeSize, kSkyCubeSize}, {color}, &depth, 0, false);
        for (const auto &mesh : room.meshes) {
            PipelineKey key;
            key.module = "sky";
            key.vertexEntry = "skyVertex";
            key.fragmentEntry = "skyFragment";
            key.colorFormats = {_skyCube->pixelFormat()};
            key.depthFormat = Format::D32Sfloat;
            key.depthTest = true;
            key.depthWrite = true;
            key.cull = FaceCullMode::None;
            key.vertexLayout = mesh.mesh->vertexLayout();
            const auto pipeline = _renderer.pipelines().get(key);

            LocalUniforms locals;
            locals.reset();
            locals.model = mesh.transform;
            locals.modelInv = mesh.transformInv;
            locals.prevModel = mesh.prevTransform;
            locals.uv = mesh.uv;
            offsets[UniformBlockBindingPoints::locals] = ring.push(locals);
            commandBuffer.bindPipeline(pipeline.pipeline);
            commandBuffer.bindDescriptorSet(pipeline.layout,
                                            IDescriptors::kUniformSet,
                                            uniformSet,
                                            offsets.data(), static_cast<uint32_t>(offsets.size()));
            auto textureSet = descriptors.acquireTextureDescriptorSet(
                _renderer.frameIndex(), {{TextureUnits::mainTex, &resources.get(*mesh.texture)}});
            commandBuffer.bindDescriptorSet(pipeline.layout,
                                            IDescriptors::kTextureSet,
                                            textureSet, nullptr, 0);
            resources.drawMesh(commandBuffer, *mesh.mesh);
        }
        commandBuffer.endRendering();
    }

    commandBuffer.transitionImage(*_skyCube, ImageLayout::ShaderRead);
    _skyCubeReady = true;
    info("Tracing: baked sky room '" + room.name + "' into a " + std::to_string(kSkyCubeSize) + "px cubemap",
         LogChannel::Graphics);
    return true;
}

void SkyStage::clearSkyRoom() {
    _skyCubeRoom = 0;
    _skyCubeReady = false;
}

bool SkyStage::supportsSkyTexture(const Texture &texture) const {
    return _renderer.resources().supports(texture.pixelFormat());
}

SkyBinding SkyStage::binding(bool useBaked) const {
    if (useBaked) {
        return {_skyCube.get(), _skyCube->cubeSampleView(0), true};
    }
    return {_skyFallbackCube.get(), _skyFallbackCube->sampleView(), false};
}

} // namespace reone::graphics
