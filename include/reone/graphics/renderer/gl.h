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

#include "../renderer.h"

namespace reone {

namespace graphics {

class IContext;
class IMeshRegistry;
class IShaderRegistry;
class IStatistic;
class IUniforms;
class Window;

/**
 * The frame as OpenGL sees it: the default framebuffer, composited by drawing a
 * fullscreen quad, presented by swapping the window buffers.
 */
class GLRenderer : public IRenderer, boost::noncopyable {
public:
    /**
     * @param window presents finished frames, or null when the target belongs
     *               to someone else and this renderer must not swap - the
     *               toolkit draws into a canvas wxWidgets presents itself.
     */
    GLRenderer(
        IContext &context,
        IMeshRegistry &meshRegistry,
        IShaderRegistry &shaderRegistry,
        IStatistic &statistic,
        IUniforms &uniforms,
        Window *window = nullptr) :
        _context(context),
        _meshRegistry(meshRegistry),
        _shaderRegistry(shaderRegistry),
        _statistic(statistic),
        _uniforms(uniforms),
        _window(window) {
    }

    ~GLRenderer() { deinit(); }

    void init() override;
    void deinit() override;

    void beginFrame(glm::ivec2 extent) override;
    void drawSceneOutput(Texture &output) override;
    std::shared_ptr<Texture> captureFrame() override;
    void endFrame() override;

private:
    IContext &_context;
    IMeshRegistry &_meshRegistry;
    IShaderRegistry &_shaderRegistry;
    IStatistic &_statistic;
    IUniforms &_uniforms;
    Window *_window;

    bool _inited {false};
    bool _inFrame {false};
    glm::ivec2 _extent {0};
};

} // namespace graphics

} // namespace reone
