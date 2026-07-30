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

#include "attachment.h"
#include "types.h"

namespace reone {

namespace graphics {

enum class TextureType {
    TwoDim,
    TwoDimArray,
    CubeMap,
    CubeMapArray
};

class Texture : public IAttachment, boost::noncopyable {
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
    };

    struct Features {
        Blending blending {Blending::None};
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

    ~Texture() { deinit(); }

    void init();
    void deinit();

    void bind();
    void unbind();

    void flushGPUToCPU();

    bool is2D() const { return _type == TextureType::TwoDim; }
    bool is2DArray() const { return _type == TextureType::TwoDimArray; }
    bool isCubeMap() const { return _type == TextureType::CubeMap; }
    bool isCubeMapArray() const { return _type == TextureType::CubeMapArray; };

    bool isGrayscale() const { return _pixelFormat == PixelFormat::R8; }

    bool isTexture() const override { return true; }
    bool isRenderbuffer() const override { return false; }

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

    void clear(int w, int h, PixelFormat format, int numLayers = 1, bool refresh = false);

    void setPixels(int w, int h, PixelFormat format, Layer layer, bool refresh = false);
    void setPixels(int w, int h, PixelFormat format, std::vector<Layer> layers, bool refresh = false);

    // END Pixels

    // OpenGL

    uint32_t nameGL() const { return _nameGL; }

    // END OpenGL

private:
    std::string _name;
    TextureType _type;
    Properties _properties;

    bool _inited {false};

    int _width {0};
    int _height {0};
    PixelFormat _pixelFormat {PixelFormat::BGR8};
    std::vector<Layer> _layers; /**< either one for 2D textures, or six for cube maps */
    Features _features;

    // OpenGL

    uint32_t _nameGL {0};

    // END OpenGL

    void configure();
    void refresh();
    void initGL();

    void configure2D();
    void configureCubeMap();

    void refresh2D();
    /** One mip level of a 2D texture, compressed or not. */
    void uploadLevel2D(int level, int width, int height,
                       const void *pixelsData, size_t pixelsSize);
    void refresh2DArray();
    void refreshCubeMap();
    /** One mip level of one cube face. */
    void uploadLevelCubeFace(int face, int level, int width, int height,
                             const void *pixelsData, size_t pixelsSize);
    void refreshCubeMapArray();

    uint32_t getTargetGL() const;
};

} // namespace graphics

} // namespace reone
