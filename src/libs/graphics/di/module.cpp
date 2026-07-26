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

    if (!isVulkanBackend()) {
        // All four are OpenGL objects that make GL calls on init. Under Vulkan
        // they stay constructed but uninitialised: the services struct still
        // has to hand out references, and nothing on the Vulkan path calls
        // them. Anything that does will fault loudly rather than silently
        // drawing nothing, which is the behaviour we want while the backend is
        // incomplete.
        _context->init();
        _meshRegistry->init();
        _textureRegistry->init();
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
