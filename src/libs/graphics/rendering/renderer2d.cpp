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

#include "reone/graphics/rendering/renderer2d.h"

#include "reone/graphics/font.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

static constexpr char kModule[] = "vk2d";
/** Two triangles, synthesised in the shader. */
static constexpr int kQuadVertices = 6;

void Renderer2D::init() {
}

void Renderer2D::deinit() {
}

void Renderer2D::begin(ICommandBuffer &commandBuffer, glm::ivec2 extent,
                             glm::ivec2 physicalExtent, Format colorFormat) {
    _commandBuffer = &commandBuffer;
    _extent = extent;
    _physicalExtent = physicalExtent;
    _colorFormat = colorFormat;
    _blend = BlendMode::Normal;
    _drawCount = 0;

    // Pixels, y down from the top left, matching the GL path's ortho and every
    // caller's assumption about where things go.
    GlobalUniforms globals;
    globals.reset();
    // Two departures from the legacy form, both because this backend's clip space
    // differs. Depth is 0..1, so orthoRH_ZO rather than ortho - the GL form puts
    // every quad at z_ndc -1, which the depth range clips, and nothing draws at all.
    // And clip-space y points down, so bottom and top are *not* swapped here:
    // passing (h, 0) as the GL path does would put screen y=0 at the bottom and
    // turn the whole frame upside down.
    globals.projection = glm::orthoRH_ZO(0.0f, static_cast<float>(extent.x),
                                         0.0f, static_cast<float>(extent.y),
                                         0.0f, 100.0f);
    globals.projectionInv = glm::inverse(globals.projection);
    _globalsOffset = _ring.push(globals);
}

void Renderer2D::end() {
    _commandBuffer = nullptr;
}

void Renderer2D::drawQuads(const char *vertexEntry,
                                 const char *fragmentEntry,
                                 const LocalUniforms &locals,
                                 uint32_t textOffset,
                                 int instances,
                                 const Texture *texture) {
    if (!_commandBuffer) {
        throw std::logic_error("2D: no frame begun");
    }

    PipelineKey key;
    key.module = kModule;
    key.vertexEntry = vertexEntry;
    key.fragmentEntry = fragmentEntry;
    key.colorFormats = {_colorFormat};
    key.blend = _blend;
    // 2D is painter's-algorithm ordered by the caller; depth would only get in
    // the way, and there is no depth attachment bound during the 2D pass.
    key.depthTest = false;
    key.depthWrite = false;
    auto pipeline = _pipelines.get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = _globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);
    offsets[UniformBlockBindingPoints::text] = textOffset;
    const IImage *mainTex = texture ? &_resources.get(*texture) : nullptr;

    _commandBuffer->bindPipeline(pipeline.pipeline);

    auto uniformSet = _descriptors.uniformDescriptorSet(_ring.frame());
    _commandBuffer->bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                      offsets.data(), static_cast<uint32_t>(offsets.size()));

    auto textureSet = _descriptors.acquireTextureDescriptorSet(_ring.frame(), mainTex);
    _commandBuffer->bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, textureSet,
                                      nullptr, 0);
    _commandBuffer->draw(kQuadVertices, static_cast<uint32_t>(instances));
    ++_drawCount;
}

void Renderer2D::drawImage(Texture &texture,
                                 const glm::vec2 &position,
                                 const glm::vec2 &size,
                                 const glm::vec4 &color,
                                 const glm::mat3x4 &uv) {
    auto transform = glm::translate(glm::vec3(position, 0.0f));
    transform *= glm::scale(glm::vec3(size, 1.0f));
    drawImage(texture, transform, color, uv);
}

/**
 * Undo the v flip the 2D vertex shader applies unconditionally.
 *
 * quadUV in vk2d.slang hands the fragment stage (u, 1 - v), because an uploaded
 * texture's rows are in OpenGL's bottom-up order. An image this backend
 * rendered is already the right way up, so that flip has to be cancelled - and
 * cancelled *before* the caller's own transform, not after, or a caller
 * selecting a sub-rect gets the mirrored part of the texture instead of the
 * rect they asked for.
 *
 * The shader reads the mat3x4 as three affine columns and multiplies on the
 * right, so composing F (its own inverse) ahead of `uv` is this.
 */
static glm::mat3x4 cancelVFlip(const glm::mat3x4 &uv) {
    glm::mat3x4 result = uv;
    result[1] = -uv[1];
    result[2] = uv[1] + uv[2];
    return result;
}

void Renderer2D::drawImage(Texture &texture,
                                 const glm::mat4 &transform,
                                 const glm::vec4 &color,
                                 const glm::mat3x4 &uv) {
    LocalUniforms locals;
    locals.reset();
    locals.model = transform;
    locals.color = color;
    locals.uv = _resources.isExternal(texture) ? cancelVFlip(uv) : uv;
    drawQuads("quadVertex", "imageFragment", locals, 0, 1, &texture);
}

void Renderer2D::drawRect(const glm::vec2 &position,
                                const glm::vec2 &size,
                                const glm::vec4 &color) {
    LocalUniforms locals;
    locals.reset();
    locals.model = glm::translate(glm::vec3(position, 0.0f));
    locals.model *= glm::scale(glm::vec3(size, 1.0f));
    locals.color = color;
    drawQuads("quadVertex", "colorFragment", locals, 0, 1, nullptr);
}

void Renderer2D::drawFullTargetImage(Texture &texture, const glm::mat3x4 &uv) {
    LocalUniforms locals;
    locals.reset();
    locals.uv = _resources.isExternal(texture) ? cancelVFlip(uv) : uv;
    drawQuads("fullTargetVertex", "imageFragment", locals, 0, 1, &texture);
}

void Renderer2D::drawText(Font &font,
                                std::string_view text,
                                const glm::vec3 &position,
                                const glm::vec3 &color,
                                TextGravity gravity) {
    if (text.empty()) {
        return;
    }
    LocalUniforms locals;
    locals.reset();
    locals.color = glm::vec4(color, 1.0f);

    const auto &glyphs = font.glyphs();
    glm::vec2 offset = font.textOffset(text, gravity);

    // The text block holds a fixed number of glyphs, so a long run becomes
    // several instanced draws, as in the GL path.
    int numBlocks = static_cast<int>(text.size()) / kMaxTextChars;
    if (text.size() % kMaxTextChars > 0) {
        ++numBlocks;
    }
    for (int block = 0; block < numBlocks; ++block) {
        int numChars = glm::min(kMaxTextChars,
                                static_cast<int>(text.size()) - block * kMaxTextChars);
        auto line = text.substr(block * kMaxTextChars, numChars);

        TextUniforms chars;
        for (int i = 0; i < numChars; ++i) {
            const auto &glyph = glyphs[static_cast<unsigned char>(line[i])];
            chars.chars[i].posScale = glm::vec4(position.x + offset.x,
                                                position.y + offset.y,
                                                glyph.size.x,
                                                glyph.size.y);
            chars.chars[i].uv = glm::vec4(glyph.ul.x,
                                          glyph.lr.y,
                                          glyph.lr.x - glyph.ul.x,
                                          glyph.ul.y - glyph.lr.y);
            offset.x += glyph.size.x;
        }
        auto textOffset = _ring.push(chars);
        drawQuads("textVertex", "textFragment", locals, textOffset, numChars, &font.texture());
    }
}

void Renderer2D::withBlendMode(BlendMode mode, const std::function<void()> &block) {
    // Not a state change: it selects which pipeline the draws inside will use.
    auto previous = _blend;
    _blend = mode;
    block();
    _blend = previous;
}

void Renderer2D::withScissor(const glm::ivec4 &bounds, const std::function<void()> &block) {
    if (!_commandBuffer) {
        throw std::logic_error("2D: no frame begun");
    }
    // Caller bounds are logical coordinates; the scissor is physical pixels.
    // The two differ when the OS clamps the window below the configured
    // resolution and the viewport scales to fit.
    float scaleX = _extent.x > 0 ? static_cast<float>(_physicalExtent.x) / _extent.x : 1.0f;
    float scaleY = _extent.y > 0 ? static_cast<float>(_physicalExtent.y) / _extent.y : 1.0f;
    _commandBuffer->setScissor(
        {static_cast<int32_t>(bounds[0] * scaleX), static_cast<int32_t>(bounds[1] * scaleY)},
        {static_cast<uint32_t>(bounds[2] * scaleX + 0.5f),
         static_cast<uint32_t>(bounds[3] * scaleY + 0.5f)});

    block();

    _commandBuffer->setScissor(
        {0, 0}, {static_cast<uint32_t>(_physicalExtent.x), static_cast<uint32_t>(_physicalExtent.y)});
}

} // namespace graphics

} // namespace reone
