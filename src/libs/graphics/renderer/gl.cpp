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

#include "reone/graphics/renderer/gl.h"

#include "reone/graphics/context.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/shaderregistry.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/window.h"

namespace reone {

namespace graphics {

void GLRenderer::init() {
    _inited = true;
}

void GLRenderer::deinit() {
    _inited = false;
}

void GLRenderer::beginFrame(glm::ivec2 extent) {
    if (_inFrame) {
        throw std::logic_error("Renderer: frame already begun");
    }
    _extent = extent;
    _inFrame = true;
    // Pushed rather than assumed: Dear ImGui sets the viewport directly, behind
    // the context's back, so the frame cannot rely on whatever was left over.
    _context.pushViewport({0, 0, extent.x, extent.y});
    _context.clearColorDepth();
}

void GLRenderer::drawSceneOutput(Texture &output) {
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    _uniforms.setLocals(std::bind(&LocalUniforms::reset, std::placeholders::_1));
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::ndcTexture));
    _context.bindTexture(output);
    _meshRegistry.get(MeshName::quadNDC).draw(_statistic);
}

std::shared_ptr<Texture> GLRenderer::captureFrame() {
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    return _context.captureScreen(_extent.x, _extent.y);
}

void GLRenderer::endFrame() {
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    _context.popViewport();
    _inFrame = false;
    if (_window) {
        _window->swap();
    }
}

} // namespace graphics

} // namespace reone
