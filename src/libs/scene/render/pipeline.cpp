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

#include "reone/scene/render/pipeline.h"

#include "reone/graphics/backend.h"
#include "reone/graphics/context.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/npyutil.h"
#include "reone/graphics/pbrtextures.h"
#include "reone/graphics/shaderregistry.h"
#include "reone/graphics/statistic.h"
#include "reone/graphics/textureregistry.h"
#include "reone/graphics/uniforms.h"
#include "reone/scene/render/pipeline/pbr.h"
#ifdef R_ENABLE_VULKAN
#include "reone/scene/render/pipeline/vulkan.h"
#endif
#include "reone/scene/render/pipeline/retro.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

/**
 * What a PixelFormat looks like once it is off the GPU: how many channels, what
 * the elements are, and what to ask OpenGL for.
 *
 * Read back at the stored width rather than a convenient one - a depth target
 * is 32-bit float and a motion target is signed half float, and rounding either
 * into bytes would throw away exactly the differences this is meant to find.
 */
struct ReadbackFormat {
    int channels;
    NpyType type;
    uint32_t glFormat;
    uint32_t glType;
};

static std::optional<ReadbackFormat> readbackFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::R8:
        return ReadbackFormat {1, NpyType::UInt8, GL_RED, GL_UNSIGNED_BYTE};
    case PixelFormat::RG8:
        return ReadbackFormat {2, NpyType::UInt8, GL_RG, GL_UNSIGNED_BYTE};
    case PixelFormat::RGB8:
        return ReadbackFormat {3, NpyType::UInt8, GL_RGB, GL_UNSIGNED_BYTE};
    case PixelFormat::RGBA8:
        return ReadbackFormat {4, NpyType::UInt8, GL_RGBA, GL_UNSIGNED_BYTE};
    case PixelFormat::BGR8:
        return ReadbackFormat {3, NpyType::UInt8, GL_BGR, GL_UNSIGNED_BYTE};
    case PixelFormat::BGRA8:
        return ReadbackFormat {4, NpyType::UInt8, GL_BGRA, GL_UNSIGNED_BYTE};
    case PixelFormat::R16F:
        return ReadbackFormat {1, NpyType::Float32, GL_RED, GL_FLOAT};
    case PixelFormat::RG16F:
        return ReadbackFormat {2, NpyType::Float32, GL_RG, GL_FLOAT};
    case PixelFormat::RGB16F:
        return ReadbackFormat {3, NpyType::Float32, GL_RGB, GL_FLOAT};
    case PixelFormat::RGBA16F:
        return ReadbackFormat {4, NpyType::Float32, GL_RGBA, GL_FLOAT};
    case PixelFormat::Depth24:
    case PixelFormat::Depth32F:
        return ReadbackFormat {1, NpyType::Float32, GL_DEPTH_COMPONENT, GL_FLOAT};
    default:
        // Compressed and stencil formats have no useful flat readback.
        return std::nullopt;
    }
}

/** Strip characters a filename cannot carry, so target names can be free text. */
static std::string toFileName(const std::string &name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        result.push_back(std::isalnum(static_cast<unsigned char>(c)) ? std::tolower(c) : '_');
    }
    return result;
}

void RenderPipelineBase::dumpTargets(const std::filesystem::path &dir) {
    std::filesystem::create_directories(dir);
    info("OpenGL scene traversals: " + std::to_string(_registry->traversalCount()),
         LogChannel::Graphics);
    info("OpenGL registry registered: " + formatRegistryCounts(_registry->registeredCounts()),
         LogChannel::Graphics);
    for (const auto &[pass, drawn] : _registry->drawnCountsByPass()) {
        info("OpenGL registry drawn " + renderPassName(pass) +
                 ": " + formatRegistryCounts(drawn),
             LogChannel::Graphics);
    }
    for (const auto &target : targets()) {
        if (!target.texture) {
            continue;
        }
        auto format = readbackFormat(target.texture->pixelFormat());
        if (!format) {
            warn("Cannot dump target '" + target.name + "': unsupported pixel format",
                 LogChannel::Graphics);
            continue;
        }
        auto width = target.texture->width();
        auto height = target.texture->height();
        size_t elementSize = format->type == NpyType::UInt8 ? 1 : 4;
        std::vector<uint8_t> pixels(elementSize * format->channels * width * height);

        // Straight from the texture rather than through a framebuffer: a depth
        // attachment cannot be a colour read source, and glGetTexImage does not
        // care which framebuffer happens to be bound.
        glBindTexture(GL_TEXTURE_2D, target.texture->nameGL());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, format->glFormat, format->glType, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        // OpenGL hands back the bottom row first. Flipped here so both backends
        // write top-down and the arrays line up index for index.
        size_t stride = elementSize * format->channels * width;
        std::vector<uint8_t> flipped(pixels.size());
        for (int y = 0; y < height; ++y) {
            std::memcpy(flipped.data() + y * stride,
                        pixels.data() + (height - 1 - y) * stride,
                        stride);
        }

        writeNpy(dir / (toFileName(target.name) + ".npy"), flipped.data(),
                 width, height, format->channels, format->type);
    }
    info("Dumped " + std::to_string(targets().size()) + " render targets to " + dir.string(),
         LogChannel::Graphics);
}

void RenderPipelineBase::applyBoxBlur(Texture &srcTexture, Framebuffer &dst, const glm::ivec2 &size) {
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::postBoxBlur4));
    _context.bindDrawFramebuffer(dst, {0});
    _context.bindTexture(srcTexture);
    _context.withViewport(glm::ivec4(0, 0, size), [this]() {
        _context.clearColorDepth();
        _meshRegistry.get(MeshName::quadNDC).draw(_statistic);
    });
}

void RenderPipelineBase::applyGaussianBlur(Texture &tex,
                                           Framebuffer &dst,
                                           const glm::ivec2 &size,
                                           const GaussianBlurParams &params) {
    _uniforms.setScreenEffect([&size, &params](auto &se) {
        se.screenResolution = glm::vec2(size);
        se.screenResolutionRcp = 1.0f / se.screenResolution;
        se.blurDirection = params.vertical ? glm::vec2(0.0f, 1.0f) : glm::vec2(1.0f, 0.0f);
    });
    _context.useProgram(_shaderRegistry.get(params.strong
                                                ? ShaderProgramId::postGaussianBlur13
                                                : ShaderProgramId::postGaussianBlur9));
    _context.bindDrawFramebuffer(dst, {0});
    _context.bindTexture(tex);
    _context.withViewport(glm::ivec4(0, 0, size), [this]() {
        _context.clearColorDepth();
        _meshRegistry.get(MeshName::quadNDC).draw(_statistic);
    });
}

void RenderPipelineBase::applyMedianFilter(Texture &tex,
                                           Framebuffer &dst,
                                           const glm::ivec2 &size,
                                           bool strong) {
    _context.useProgram(_shaderRegistry.get(strong
                                                ? ShaderProgramId::postMedianFilter5
                                                : ShaderProgramId::postMedianFilter3));
    _context.bindDrawFramebuffer(dst, {0});
    _context.bindTexture(tex);
    _context.withViewport(glm::ivec4(0, 0, size), [this]() {
        _context.clearColorDepth();
        _meshRegistry.get(MeshName::quadNDC).draw(_statistic);
    });
}

void RenderPipelineBase::applyFXAA(Texture &tex, Framebuffer &dst, const glm::ivec2 &size) {
    _uniforms.setScreenEffect([&size](auto &se) {
        se.screenResolution = glm::vec2(size);
        se.screenResolutionRcp = 1.0f / se.screenResolution;
    });
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::postFXAA));
    _context.bindDrawFramebuffer(dst, {0});
    _context.bindTexture(tex);
    _context.withViewport(glm::ivec4(0, 0, size), [this]() {
        _context.clearColorDepth();
        _meshRegistry.get(MeshName::quadNDC).draw(_statistic);
    });
}

void RenderPipelineBase::applySharpen(Texture &tex,
                                      Framebuffer &dst,
                                      const glm::ivec2 &size,
                                      float amount) {
    _uniforms.setScreenEffect([&size, &amount](auto &se) {
        se.screenResolution = glm::vec2(size);
        se.screenResolutionRcp = 1.0f / se.screenResolution;
        se.sharpenAmount = amount;
    });
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::postSharpen));
    _context.bindDrawFramebuffer(dst, {0});
    _context.bindTexture(tex);
    _context.withViewport(glm::ivec4(0, 0, size), [this]() {
        _context.clearColorDepth();
        _meshRegistry.get(MeshName::quadNDC).draw(_statistic);
    });
}

std::unique_ptr<IRenderPipeline> RenderPipelineFactory::create(RenderMode mode, glm::ivec2 targetSize) {
    // Backend and mode are separate axes and only some pairs exist. Where one
    // does not, say so: Vulkan quietly running PBR while the caller asked for
    // retro is how an entire session of backend comparisons ended up measuring
    // two different renderers against each other.
#ifdef R_ENABLE_VULKAN
    if (graphics::isVulkanBackend()) {
        if (!_vulkanRenderer) {
            throw std::logic_error("Vulkan renderer was not supplied to the pipeline factory");
        }
        if (mode == RenderMode::Retro) {
            warn("No retro pipeline on Vulkan; rendering PBR instead. Pass --pbr 1 to "
                 "silence this, and do not compare this frame against an OpenGL retro one.",
                 LogChannel::Graphics);
        }
        return std::make_unique<VulkanRenderPipeline>(
            std::move(targetSize), _options, *_vulkanRenderer, _uniforms, _meshRegistry, _textureRegistry);
    }
#endif
    switch (mode) {
    case RenderMode::Retro:
        return std::make_unique<RetroRenderPipeline>(
            std::move(targetSize),
            _options,
            _context,
            _meshRegistry,
            _shaderRegistry,
            _statistic,
            _textureRegistry,
            _uniforms);
    case RenderMode::PBR:
        return std::make_unique<PBRRenderPipeline>(
            std::move(targetSize),
            _options,
            _context,
            _meshRegistry,
            _pbrTextures,
            _shaderRegistry,
            _statistic,
            _textureRegistry,
            _uniforms);
    default:
        throw std::invalid_argument("Unsupported render mode: " + std::to_string(static_cast<int>(mode)));
    }
}

} // namespace scene

} // namespace reone
