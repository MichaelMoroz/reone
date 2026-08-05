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
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/renderer.h"
#include "reone/graphics/renderer2d.h"
#include "reone/graphics/statistic.h"
#include "reone/graphics/textureregistry.h"
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
    MOCK_METHOD(void, immediateSubmit, (const std::function<void(ICommandBuffer &)> &), (override));
};

class Mock2DRenderer : public I2DRenderer, boost::noncopyable {
public:
    MOCK_METHOD(void, init, (), (override));
    MOCK_METHOD(void, deinit, (), (override));
    MOCK_METHOD(void, drawImage, (Texture &, const glm::vec2 &, const glm::vec2 &, const glm::vec4 &, const glm::mat3x4 &), (override));
    MOCK_METHOD(void, drawImage, (Texture &, const glm::mat4 &, const glm::vec4 &, const glm::mat3x4 &), (override));
    MOCK_METHOD(void, drawRect, (const glm::vec2 &, const glm::vec2 &, const glm::vec4 &), (override));
    MOCK_METHOD(void, drawFullTargetImage, (Texture &, const glm::mat3x4 &), (override));
    MOCK_METHOD(void, drawText, (Font &, std::string_view, const glm::vec3 &, const glm::vec3 &, TextGravity), (override));
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
