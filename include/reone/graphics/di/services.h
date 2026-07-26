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

namespace reone {

namespace graphics {

class IContext;
class IMeshRegistry;
class IPBRTextures;
class IRenderer;
class I2DRenderer;
class IShaderRegistry;
class IStatistic;
class ITextureRegistry;
class IUniforms;

struct GraphicsServices {
    IContext &context;
    IMeshRegistry &meshRegistry;
    IPBRTextures &pbrTextures;
    IRenderer &renderer;
    I2DRenderer &renderer2d;
    IShaderRegistry &shaderRegistry;
    IStatistic &statistic;
    ITextureRegistry &textureRegistry;
    IUniforms &uniforms;

    GraphicsServices(
        IContext &context,
        IMeshRegistry &meshRegistry,
        IPBRTextures &pbrTextures,
        IRenderer &renderer,
        I2DRenderer &renderer2d,
        IShaderRegistry &shaderRegistry,
        IStatistic &statistic,
        ITextureRegistry &textureRegistry,
        IUniforms &uniforms) :
        context(context),
        meshRegistry(meshRegistry),
        pbrTextures(pbrTextures),
        renderer(renderer),
        renderer2d(renderer2d),
        shaderRegistry(shaderRegistry),
        statistic(statistic),
        textureRegistry(textureRegistry),
        uniforms(uniforms) {
    }
};

} // namespace graphics

} // namespace reone
