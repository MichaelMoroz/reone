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

#include "reone/graphics/di/module.h"

#include "reone/graphics/backend.h"

namespace reone {

namespace graphics {

void GraphicsModule::init() {
    _context = std::make_unique<Context>(_options);
    _statistic = std::make_unique<Statistic>();
    _meshRegistry = std::make_unique<MeshRegistry>(*_statistic);
    _shaderRegistry = std::make_unique<ShaderRegistry>();
    _textureRegistry = std::make_unique<TextureRegistry>();
    _uniforms = std::make_unique<Uniforms>(*_context);
    _pbrTextures = std::make_unique<PBRTextures>(
        *_context,
        *_meshRegistry,
        *_shaderRegistry,
        *_statistic,
        *_uniforms);
    if (!_externalRenderer) {
        _renderer = std::make_unique<GLRenderer>(
            *_context,
            *_meshRegistry,
            *_shaderRegistry,
            *_statistic,
            *_uniforms,
            _window);
    }
    if (!_externalRenderer2d) {
        _renderer2d = std::make_unique<GL2DRenderer>(
            *_context,
            *_meshRegistry,
            *_shaderRegistry,
            *_statistic,
            *_uniforms);
    }

    _services = std::make_unique<GraphicsServices>(
        *_context,
        *_meshRegistry,
        *_pbrTextures,
        renderer(),
        renderer2d(),
        *_shaderRegistry,
        *_statistic,
        *_textureRegistry,
        *_uniforms);

    // Context::init loads the GL entry points, so nothing that touches GL may
    // run before it. TextureRegistry does not itself - it builds default
    // Textures, whose init() is backend-aware - but those Textures do, so the
    // original ordering is preserved rather than hoisting it.
    if (!isVulkanBackend()) {
        _context->init();
    }
    // Both of these only build Mesh and Texture objects, whose init() is
    // backend-aware, and both are needed on either path: the resource layer
    // looks the default textures up by name while loading a module, and the
    // grass and billboard quads come from the mesh registry. They stay after
    // Context::init because that is what loads the GL entry points.
    _meshRegistry->init();
    _textureRegistry->init();
    if (!isVulkanBackend()) {
        // OpenGL uniform buffers. Under Vulkan they stay constructed but
        // uninitialised, as do Context and MeshRegistry above: the services
        // struct still hands out references, but nothing on the Vulkan path may
        // call them, and anything that does faults loudly rather than silently
        // drawing nothing.
        _uniforms->init();
    }
    renderer().init();
    renderer2d().init();
}

void GraphicsModule::deinit() {
    _services.reset();

    _renderer2d.reset();
    _renderer.reset();
    _externalRenderer = nullptr;
    _externalRenderer2d = nullptr;
    _pbrTextures.reset();
    _uniforms.reset();
    _meshRegistry.reset();
    _textureRegistry.reset();
    _statistic.reset();
    _context.reset();
}

} // namespace graphics

} // namespace reone
