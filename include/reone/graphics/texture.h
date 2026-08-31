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

#include "reone/system/types.h"

#include "types.h"

namespace reone {

namespace graphics {

enum class TextureType {
    TwoDim,
    TwoDimArray,
    CubeMap,
    CubeMapArray
};

class Texture : boost::noncopyable {
public:
    enum class Filtering {
        Nearest,
        Linear,
        NearestMipmapNearest,
        LinearMipmapNearest,
        NearestMipmapLinear,
        LinearMipmapLinear
    };

    enum class Wrapping {
        Repeat,
        ClampToEdge,
        ClampToBorder
    };

    enum class Blending {
        None,
        Default,
        Additive,
        PunchThrough
    };

    enum class ProcedureType {
        Invalid,
        Cycle,
        Arturo,
        Water
    };

    struct Properties {
        Filtering minFilter {Filtering::LinearMipmapLinear};
        Filtering magFilter {Filtering::Linear};
        Wrapping wrap {Wrapping::Repeat};
        glm::vec4 borderColor {0.0f};
        float anisotropy {1.0f};
        /**
         * A depth-comparison sampler: the filtering unit compares each texel
         * against a reference the shader supplies and returns the filtered
         * fraction that passed, rather than the depth itself. One tap becomes a
         * 2x2 percentage-closer filter for free.
         *
         * Not a mode a texture can be sampled either way through. A comparison
         * sampler is only usable with SampleCmp, and a plain one is only usable
         * without it, so this is decided where the image is bound.
         */
        bool compare {false};
    };

    struct Features {
        Blending blending {Blending::None};
        /**
         * The cutout threshold this texture was authored with, or -1 where the
         * format carries none.
         *
         * A float in the TPC header, not a TXI key, and the only per-texture
         * alpha threshold the data supplies. It was skipped by this reader
         * until now, so every cutout used one constant instead - which is what
         * the actively-maintained reimplementation of this engine reads, and
         * what it feeds its alpha test.
         */
        float alphaTest {-1.0f};
        float waterAlpha {-1.0f};
        bool cube {false};
        bool decal {false};

        // Companion textures

        std::string envmapTexture;
        std::string bumpyShinyTexture;
        std::string bumpmapTexture;

        float bumpMapScaling {1.0f};

        // END Companion textures

        // Font

        int numChars {0};
        float fontHeight {0.0f};
        float spacingR {0.0f};
        std::vector<glm::vec3> upperLeftCoords;
        std::vector<glm::vec3> lowerRightCoords;

        // END Font

        // Animation

        ProcedureType procedureType {ProcedureType::Invalid};
        int numX {1};
        int numY {1};
        int fps {0};

        // END Animation
    };

    struct Layer {
        std::shared_ptr<ByteBuffer> pixels;

        /**
         * Levels 1 and beyond, when the file shipped them; empty otherwise.
         *
         * Kept beside the base level rather than folded into a single list of
         * levels, because almost everything that touches a Layer wants level
         * zero and nothing else - a screenshot, a movie frame, a decompressed
         * grid. Those keep working untouched, and a backend that can use the
         * chain asks for it.
         *
         * No dimensions are stored: level i is max(1, width >> i) by
         * max(1, height >> i), which both backends can work out.
         */
        std::vector<std::shared_ptr<ByteBuffer>> mips;
    };

    Texture(std::string name,
            TextureType type,
            Properties properties) :
        _name(std::move(name)),
        _type(type),
        _properties(std::move(properties)) {
    }

    void init();

    bool is2D() const { return _type == TextureType::TwoDim; }
    bool is2DArray() const { return _type == TextureType::TwoDimArray; }
    bool isCubeMap() const { return _type == TextureType::CubeMap; }
    bool isCubeMapArray() const { return _type == TextureType::CubeMapArray; };

    bool isGrayscale() const { return _pixelFormat == PixelFormat::R8; }

    const std::string &name() const { return _name; }
    TextureType type() const { return _type; }
    int width() const { return _width; }
    int height() const { return _height; }
    std::vector<Layer> &layers() { return _layers; }
    const std::vector<Layer> &layers() const { return _layers; }
    const Features &features() const { return _features; }
    /**
     * Filtering and wrapping, chosen from the texture's usage.
     *
     * OpenGL applies these to the texture object itself. Vulkan keeps them in
     * a sampler, so its backend has to read them here to build a matching one.
     */
    const Properties &properties() const { return _properties; }
    PixelFormat pixelFormat() const { return _pixelFormat; }

    void setType(TextureType type) { _type = type; }
    void setFeatures(Features features) { _features = std::move(features); }
    void setPixelFormat(PixelFormat format) { _pixelFormat = format; }
    void setAnisotropy(float anisotropy) { _properties.anisotropy = anisotropy; }

    // Pixels

    void clear(int w, int h, PixelFormat format, int numLayers = 1);

    void setPixels(int w, int h, PixelFormat format, Layer layer);
    void setPixels(int w, int h, PixelFormat format, std::vector<Layer> layers);

    // END Pixels

private:
    std::string _name;
    TextureType _type;
    Properties _properties;

    int _width {0};
    int _height {0};
    PixelFormat _pixelFormat {PixelFormat::BGR8};
    std::vector<Layer> _layers; /**< either one for 2D textures, or six for cube maps */
    Features _features;

};

} // namespace graphics

} // namespace reone
