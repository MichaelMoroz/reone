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
#include "../renderer.h"
#include "../renderer2d.h"
#include "../shaderregistry.h"
#include "../statistic.h"
#include "../textureregistry.h"
#include "../uniforms.h"

#include "services.h"

namespace reone {

namespace graphics {

class GraphicsModule : boost::noncopyable {
public:
    GraphicsModule(GraphicsOptions &options) :
        _options(options) {
    }

    /** Supply the Vulkan renderers. Must be called before init(). */
    void setRenderers(IRenderer &renderer, I2DRenderer &renderer2d) {
        _externalRenderer = &renderer;
        _externalRenderer2d = &renderer2d;
    }

    ~GraphicsModule() { deinit(); }

    void init();
    void deinit();

    Context &context() { return *_context; }
    MeshRegistry &meshRegistry() { return *_meshRegistry; }
    PBRTextures &pbrTextures() { return *_pbrTextures; }
    IRenderer &renderer() { return *_externalRenderer; }
    I2DRenderer &renderer2d() { return *_externalRenderer2d; }
    ShaderRegistry &shaderRegistry() { return *_shaderRegistry; }
    Statistic &statistic() { return *_statistic; }
    TextureRegistry &textureRegistry() { return *_textureRegistry; }
    Uniforms &uniforms() { return *_uniforms; }

    GraphicsServices &services() { return *_services; }

private:
    GraphicsOptions &_options;

    std::unique_ptr<Context> _context;
    std::unique_ptr<MeshRegistry> _meshRegistry;
    std::unique_ptr<PBRTextures> _pbrTextures;
    IRenderer *_externalRenderer {nullptr};
    I2DRenderer *_externalRenderer2d {nullptr};
    std::unique_ptr<ShaderRegistry> _shaderRegistry;
    std::unique_ptr<Statistic> _statistic;
    std::unique_ptr<TextureRegistry> _textureRegistry;
    std::unique_ptr<Uniforms> _uniforms;

    std::unique_ptr<GraphicsServices> _services;
};

} // namespace graphics

} // namespace reone
