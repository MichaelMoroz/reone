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

#include "reone/graphics/vulkan/renderer2d.h"

#include "reone/graphics/font.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/uniformring.h"

namespace reone {

namespace graphics {

static constexpr char kModule[] = "vk2d";
/** Two triangles, synthesised in the shader. */
static constexpr int kQuadVertices = 6;

void Vulkan2DRenderer::init() {
}

void Vulkan2DRenderer::deinit() {
}

void Vulkan2DRenderer::begin(VkCommandBuffer cmd, glm::ivec2 extent, VkFormat colorFormat) {
    _cmd = cmd;
    _extent = extent;
    _colorFormat = colorFormat;
    _blend = BlendMode::Normal;
    _drawCount = 0;

    // Pixels, y down from the top left, matching the GL path's ortho and every
    // caller's assumption about where things go.
    GlobalUniforms globals;
    globals.reset();
    // orthoRH_ZO, not ortho: GLM defaults to OpenGL's -1..1 depth range unless
    // the whole project defines GLM_FORCE_DEPTH_ZERO_TO_ONE, which would change
    // the GL backend too. At z=0 the GL form puts every quad at z_ndc -1, which
    // Vulkan clips, and nothing is drawn at all.
    globals.projection = glm::orthoRH_ZO(0.0f, static_cast<float>(extent.x),
                                         static_cast<float>(extent.y), 0.0f,
                                         0.0f, 100.0f);
    globals.projectionInv = glm::inverse(globals.projection);
    _globalsOffset = _ring.push(globals);
}

void Vulkan2DRenderer::end() {
    _cmd = VK_NULL_HANDLE;
}

void Vulkan2DRenderer::drawQuads(const char *vertexEntry,
                                 const char *fragmentEntry,
                                 const LocalUniforms &locals,
                                 uint32_t textOffset,
                                 int instances,
                                 const Texture *texture) {
    if (_cmd == VK_NULL_HANDLE) {
        throw std::logic_error("Vulkan 2D: no frame begun");
    }

    VulkanPipelineCache::Key key;
    key.module = kModule;
    key.vertexEntry = vertexEntry;
    key.fragmentEntry = fragmentEntry;
    key.colorFormats = {_colorFormat};
    key.blend = _blend;
    // 2D is painter's-algorithm ordered by the caller; depth would only get in
    // the way, and there is no depth attachment bound during the 2D pass.
    key.depthTest = false;
    key.depthWrite = false;
    auto &pipeline = _pipelines.get(key);

    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = _globalsOffset;
    offsets[UniformBlockBindingPoints::locals] = _ring.push(locals);
    offsets[UniformBlockBindingPoints::text] = textOffset;

    const VulkanImage *mainTex = texture ? &_resources.get(*texture) : nullptr;

    vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    auto uniformSet = _descriptors.uniformSet(_ring.frame());
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());

    auto textureSet = _descriptors.acquireTextureSet(_ring.frame(), mainTex);
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                            VulkanDescriptors::kTextureSet, 1, &textureSet, 0, nullptr);

    vkCmdDraw(_cmd, kQuadVertices, static_cast<uint32_t>(instances), 0, 0);
    ++_drawCount;
}

void Vulkan2DRenderer::drawImage(Texture &texture,
                                 const glm::vec2 &position,
                                 const glm::vec2 &size,
                                 const glm::vec4 &color,
                                 const glm::mat3x4 &uv) {
    auto transform = glm::translate(glm::vec3(position, 0.0f));
    transform *= glm::scale(glm::vec3(size, 1.0f));
    drawImage(texture, transform, color, uv);
}

void Vulkan2DRenderer::drawImage(Texture &texture,
                                 const glm::mat4 &transform,
                                 const glm::vec4 &color,
                                 const glm::mat3x4 &uv) {
    LocalUniforms locals;
    locals.reset();
    locals.model = transform;
    locals.color = color;
    locals.uv = uv;
    drawQuads("quadVertex", "imageFragment", locals, 0, 1, &texture);
}

void Vulkan2DRenderer::drawRect(const glm::vec2 &position,
                                const glm::vec2 &size,
                                const glm::vec4 &color) {
    LocalUniforms locals;
    locals.reset();
    locals.model = glm::translate(glm::vec3(position, 0.0f));
    locals.model *= glm::scale(glm::vec3(size, 1.0f));
    locals.color = color;
    drawQuads("quadVertex", "colorFragment", locals, 0, 1, nullptr);
}

void Vulkan2DRenderer::drawFullTargetImage(Texture &texture, const glm::mat3x4 &uv) {
    LocalUniforms locals;
    locals.reset();
    locals.uv = uv;
    drawQuads("fullTargetVertex", "imageFragment", locals, 0, 1, &texture);
}

void Vulkan2DRenderer::drawText(Font &font,
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

void Vulkan2DRenderer::withBlendMode(BlendMode mode, const std::function<void()> &block) {
    // Not a state change: it selects which pipeline the draws inside will use.
    auto previous = _blend;
    _blend = mode;
    block();
    _blend = previous;
}

void Vulkan2DRenderer::withScissor(const glm::ivec4 &bounds, const std::function<void()> &block) {
    if (_cmd == VK_NULL_HANDLE) {
        throw std::logic_error("Vulkan 2D: no frame begun");
    }
    VkRect2D scissor {};
    scissor.offset = {bounds[0], bounds[1]};
    scissor.extent = {static_cast<uint32_t>(bounds[2]), static_cast<uint32_t>(bounds[3])};
    vkCmdSetScissor(_cmd, 0, 1, &scissor);

    block();

    VkRect2D full {{0, 0}, {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y)}};
    vkCmdSetScissor(_cmd, 0, 1, &full);
}

} // namespace graphics

} // namespace reone
