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

#include "../context.h"
#include "../meshregistry.h"
#include "../pbrtextures.h"
#include "../renderer/gl.h"
#include "../renderer/gl2d.h"
#include "../shaderregistry.h"
#include "../statistic.h"
#include "../textureregistry.h"
#include "../uniforms.h"

#include "services.h"

namespace reone {

namespace graphics {

class GraphicsModule : boost::noncopyable {
public:
    /**
     * @param window presents finished frames. Null in hosts that own
     *               presentation themselves, such as the wxWidgets toolkit.
     */
    GraphicsModule(GraphicsOptions &options, Window *window = nullptr) :
        _options(options),
        _window(window) {
    }

    ~GraphicsModule() { deinit(); }

    void init();
    void deinit();

    Context &context() { return *_context; }
    MeshRegistry &meshRegistry() { return *_meshRegistry; }
    PBRTextures &pbrTextures() { return *_pbrTextures; }
    IRenderer &renderer() { return *_renderer; }
    I2DRenderer &renderer2d() { return *_renderer2d; }
    ShaderRegistry &shaderRegistry() { return *_shaderRegistry; }
    Statistic &statistic() { return *_statistic; }
    TextureRegistry &textureRegistry() { return *_textureRegistry; }
    Uniforms &uniforms() { return *_uniforms; }

    GraphicsServices &services() { return *_services; }

private:
    GraphicsOptions &_options;
    Window *_window;

    std::unique_ptr<Context> _context;
    std::unique_ptr<MeshRegistry> _meshRegistry;
    std::unique_ptr<PBRTextures> _pbrTextures;
    std::unique_ptr<GLRenderer> _renderer;
    std::unique_ptr<GL2DRenderer> _renderer2d;
    std::unique_ptr<ShaderRegistry> _shaderRegistry;
    std::unique_ptr<Statistic> _statistic;
    std::unique_ptr<TextureRegistry> _textureRegistry;
    std::unique_ptr<Uniforms> _uniforms;

    std::unique_ptr<GraphicsServices> _services;
};

} // namespace graphics

} // namespace reone
