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

#include "reone/graphics/shader.h"

#include "SDL3/SDL.h"

#include "reone/system/threadutil.h"

namespace reone {

namespace graphics {

static GLenum getShaderTypeGL(ShaderType type) {
    switch (type) {
    case ShaderType::Vertex:
        return GL_VERTEX_SHADER;
    case ShaderType::Geometry:
        return GL_GEOMETRY_SHADER;
    case ShaderType::Fragment:
        return GL_FRAGMENT_SHADER;
    default:
        throw std::invalid_argument("Invalid shader type: " + std::to_string(static_cast<int>(type)));
    }
}

void Shader::init() {
    if (_inited) {
        return;
    }
    checkMainThread();

    if (!_spirv.empty()) {
        // The bundled glad loader is generated for OpenGL 4.0, so it declares
        // neither of these - glShaderBinary is 4.1 and glSpecializeShader is 4.6.
        // Resolved here for the spike; regenerating glad is the real fix.
        static constexpr GLenum kShaderBinaryFormatSpirV = 0x9551;
        using PfnShaderBinary = void(GLAD_API_PTR *)(GLsizei, const GLuint *, GLenum, const void *, GLsizei);
        using PfnSpecializeShader = void(GLAD_API_PTR *)(GLuint, const GLchar *, GLuint, const GLuint *, const GLuint *);
        static auto shaderBinary = reinterpret_cast<PfnShaderBinary>(SDL_GL_GetProcAddress("glShaderBinary"));
        static auto specializeShader = reinterpret_cast<PfnSpecializeShader>(SDL_GL_GetProcAddress("glSpecializeShader"));
        if (!shaderBinary || !specializeShader) {
            throw std::runtime_error("SPIR-V shaders require OpenGL 4.6; entry points not available");
        }

        _nameGL = glCreateShader(getShaderTypeGL(_type));
        shaderBinary(1, &_nameGL, kShaderBinaryFormatSpirV,
                     _spirv.data(), static_cast<GLsizei>(_spirv.size()));
        specializeShader(_nameGL, _entryPoint.c_str(), 0, nullptr, nullptr);
        GLint specialized;
        glGetShaderiv(_nameGL, GL_COMPILE_STATUS, &specialized);
        if (!specialized) {
            char log[4096];
            GLsizei logSize;
            glGetShaderInfoLog(_nameGL, sizeof(log), &logSize, log);
            throw std::runtime_error(str(boost::format("Failed specializing SPIR-V shader, entry point '%s': %s") %
                                         _entryPoint % std::string(log, logSize)));
        }
        _inited = true;
        return;
    }

    std::vector<const char *> sourcePtrs;
    for (auto &src : _sources) {
        sourcePtrs.push_back(src.c_str());
    }
    _nameGL = glCreateShader(getShaderTypeGL(_type));
    glShaderSource(_nameGL, static_cast<GLsizei>(sourcePtrs.size()), &sourcePtrs[0], nullptr);
    glCompileShader(_nameGL);

    GLint success;
    glGetShaderiv(_nameGL, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[4096];
        GLsizei logSize;
        glGetShaderInfoLog(_nameGL, sizeof(log), &logSize, log);
        throw std::runtime_error(str(boost::format("Failed compiling shader %d: %s") % static_cast<int>(_nameGL) % std::string(log, logSize)));
    }

    _inited = true;
}

void Shader::deinit() {
    if (!_inited) {
        return;
    }
    checkMainThread();
    glDeleteShader(_nameGL);
    _inited = false;
}

} // namespace graphics

} // namespace reone
