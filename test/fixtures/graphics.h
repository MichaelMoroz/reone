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

#include <gmock/gmock.h>

#include "reone/graphics/di/services.h"
#include "reone/graphics/rhi/descriptors.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rendering/renderer2d.h"
#include "reone/graphics/rendering/pbrtextures.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/graphics/statistic.h"
#include "reone/graphics/textureregistry.h"
#include "reone/graphics/rhi/uniformring.h"
#include "reone/graphics/uniforms.h"
#include "reone/system/exception/notimplemented.h"

namespace reone {

namespace graphics {

class MockMeshRegistry : public IMeshRegistry, boost::noncopyable {
public:
    MOCK_METHOD(Mesh &, get, (const std::string &), (override));
};

class MockRenderer : public IRenderer, boost::noncopyable {
public:
    MOCK_METHOD(std::unique_ptr<IBuffer>, makeBuffer, (), (override));
    MOCK_METHOD(std::unique_ptr<IComputePipeline>, makeComputePipeline,
                (const ComputePipelineDesc &), (override));
    MOCK_METHOD(ShaderReflection, reflection, (const std::string &), (const override));
    MOCK_METHOD(std::unique_ptr<ITracingPipeline>, makeTracingPipeline,
                (const TracingPipelineDesc &), (override));
    MOCK_METHOD(std::unique_ptr<ITracingDenoiser>, makeTracingDenoiser,
                (glm::ivec2), (override));
    MOCK_METHOD(std::unique_ptr<IUpscaler>, makeUpscaler, (glm::ivec2, glm::ivec2, bool), (override));
    MOCK_METHOD(std::unique_ptr<ITracingStructure>, makeTracingStructure, (), (override));
    MOCK_METHOD(void, prepareMesh, (const Mesh &), (override));
    MOCK_METHOD(uint64_t, resourceGeneration, (), (const override));
    MOCK_METHOD(void, init, (), (override));
    MOCK_METHOD(void, deinit, (), (override));
    MOCK_METHOD(void, initImGui, (), (override));
    MOCK_METHOD(void, beginImGuiFrame, (), (override));
    MOCK_METHOD(void, renderImGui, (ImDrawData &), (override));
    MOCK_METHOD(void, deinitImGui, (), (override));
    MOCK_METHOD(void, beginFrame, (glm::ivec2), (override));
    MOCK_METHOD(void, drawSceneOutput, (Texture &), (override));
    MOCK_METHOD(std::shared_ptr<Texture>, captureFrame, (), (override));
    MOCK_METHOD(void, endFrame, (), (override));
    MOCK_METHOD(IResources &, resources, (), (override));
    MOCK_METHOD(IDescriptors &, descriptors, (), (override));
    MOCK_METHOD(IUniformRing &, uniformRing, (), (override));
    MOCK_METHOD(IPipelineCache &, pipelines, (), (override));
    MOCK_METHOD(IPBRTextures &, pbrTextures, (), (override));
    MOCK_METHOD(I2DRenderer &, renderer2d, (), (override));
    MOCK_METHOD(int, frameIndex, (), (const override));
    MOCK_METHOD(ICommandBuffer &, recordingCommandBuffer, (), (override));
    MOCK_METHOD(Format, sceneOutputFormat, (), (const override));
    MOCK_METHOD(bool, recompileShaders, (), (override));
    MOCK_METHOD(void, flushFrame, (), (override));
    MOCK_METHOD(void, setVsync, (bool), (override));
    MOCK_METHOD(void, with2DRendering, (glm::ivec2, const std::function<void()> &), (override));
    MOCK_METHOD(void, waitIdle, (), (override));
    MOCK_METHOD(bool, rayQueryAvailable, (), (const override));
    MOCK_METHOD(void, immediateSubmit, (const std::function<void(ICommandBuffer &)> &), (override));
    MOCK_METHOD(void *, addPreviewTexture, (const IImage &), (override));
    MOCK_METHOD(void, removePreviewTexture, (void *), (override));
};

class Mock2DRenderer : public I2DRenderer, boost::noncopyable {
public:
    MOCK_METHOD(void, init, (), (override));
    MOCK_METHOD(void, deinit, (), (override));
    MOCK_METHOD(void, drawImage, (Texture &, const glm::vec2 &, const glm::vec2 &, const glm::vec4 &, const glm::mat3x4 &), (override));
    MOCK_METHOD(void, drawImage, (Texture &, const glm::mat4 &, const glm::vec4 &, const glm::mat3x4 &), (override));
    MOCK_METHOD(void, drawRect, (const glm::vec2 &, const glm::vec2 &, const glm::vec4 &), (override));
    MOCK_METHOD(void, drawFullTargetImage, (Texture &, const glm::mat3x4 &), (override));
    MOCK_METHOD(void, drawText, (Font &, std::string_view, const glm::vec3 &, const glm::vec4 &, TextGravity), (override));
    MOCK_METHOD(void, withBlendMode, (BlendMode, const std::function<void()> &), (override));
    MOCK_METHOD(void, withScissor, (const glm::ivec4 &, const std::function<void()> &), (override));
};

class MockStatistic : public IStatistic, boost::noncopyable {
public:
    MOCK_METHOD(void, resetDrawCalls, (), (override));
    MOCK_METHOD(void, incrementDrawCalls, (), (override));
    MOCK_METHOD(int, numDrawCalls, (), (const override));
};

class MockTextureRegistry : public ITextureRegistry,
                            boost::noncopyable {
public:
    MOCK_METHOD(Texture &, get, (const std::string &), (override));
};

class TestGraphicsModule : boost::noncopyable {
public:
    void init() {
        _meshRegistry = std::make_unique<MockMeshRegistry>();
        _renderer = std::make_unique<MockRenderer>();
        _renderer2d = std::make_unique<Mock2DRenderer>();
        _statistic = std::make_unique<MockStatistic>();
        _textureRegistry = std::make_unique<MockTextureRegistry>();
        _uniforms = std::make_unique<Uniforms>();

        _services = std::make_unique<GraphicsServices>(
            *_meshRegistry,
            *_renderer,
            *_renderer2d,
            *_statistic,
            *_textureRegistry,
            *_uniforms);
    }

    GraphicsServices &services() {
        return *_services;
    }

private:
    std::unique_ptr<MockMeshRegistry> _meshRegistry;
    std::unique_ptr<MockRenderer> _renderer;
    std::unique_ptr<Mock2DRenderer> _renderer2d;
    std::unique_ptr<MockStatistic> _statistic;
    std::unique_ptr<MockTextureRegistry> _textureRegistry;
    std::unique_ptr<Uniforms> _uniforms;

    std::unique_ptr<GraphicsServices> _services;
};

} // namespace graphics

} // namespace reone
