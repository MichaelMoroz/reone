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

#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include "reone/graphics/rhi/image.h"

#include "rhi.h"

namespace reone {

namespace graphics {

class VulkanDevice;
class VulkanImage;

std::unique_ptr<IImage> makeImage(VulkanDevice &device);
VulkanImage &toVulkanImage(IImage &image);
const VulkanImage &toVulkanImage(const IImage &image);

/**
 * A device-local image, its allocation and its view, freed together.
 *
 * Unlike a GL texture, an image has a layout: the same memory is arranged
 * differently for being written by a transfer, sampled by a shader, or used as
 * an attachment, and moving between those is an explicit barrier. Upload is
 * therefore three steps rather than one - transition to a transfer target, copy
 * from a staging buffer, transition to something a shader can read.
 */
class VulkanImage : public IImage, boost::noncopyable {
public:
    VulkanImage(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanImage() override { deinit(); }

    /**
     * Create a sampled 2D image and fill it from @p data, which must be
     * @p extent.x * @p extent.y texels in @p format. Leaves the image in
     * SHADER_READ_ONLY_OPTIMAL.
     */
    void initSampled2D(glm::ivec2 extent, VkFormat format, const void *data);

    /**
     * As initSampled2D, but with the byte count given rather than derived.
     *
     * Block-compressed formats have no per-texel size - BC1 is eight bytes per
     * four-by-four block, BC3 sixteen - so the caller supplies the length. The
     * data is uploaded as it stands; the GPU samples it compressed, which is
     * the point of shipping it that way.
     */
    void initSampled2DSized(glm::ivec2 extent,
                            VkFormat format,
                            const void *data,
                            VkDeviceSize size);

    /** One (layer, mip) of an uploaded texture, with its bytes. */
    struct Subresource {
        const void *data {nullptr};
        VkDeviceSize size {0};
        uint32_t layer {0};
        uint32_t mip {0};
    };

    /**
     * Create a sampled image with a full mip chain and fill every level.
     *
     * The general form of initSampled2DSized and initSampledLayers, and the
     * one the asset path uses. Mip levels cannot be generated here the way
     * OpenGL generates them with glGenerateMipmap: vkCmdBlitImage does not
     * accept block-compressed formats, and most of the game's textures are
     * BC1 or BC3. So the levels arrive already built, read from the TPC.
     *
     * Level i is max(1, extent >> i); sizes come from the caller because a
     * compressed level's length is a function of its block count, not its
     * texel count. Leaves the image in SHADER_READ_ONLY_OPTIMAL.
     *
     * When @p generateMips is true, @p subresources contains only level zero
     * and the remaining levels are filtered from it on the GPU.  Callers use
     * this only for formats Vulkan can blit.
     */
    void initSampledChain(glm::ivec2 extent,
                          VkFormat format,
                          bool cube,
                          uint32_t layerCount,
                          uint32_t mipCount,
                          const std::vector<Subresource> &subresources,
                          bool generateMips = false);

    /**
     * A depth attachment. Left in UNDEFINED: dynamic rendering transitions it
     * on first use, and its contents never need to survive a frame.
     */
    void initDepth(glm::ivec2 extent, VkFormat format);
    void initDepthAttachment(glm::ivec2 extent, Format format) override {
        initDepth(extent, toVulkanFormat(format));
    }

    /**
     * A layered depth image that is also sampled: a shadow map.
     *
     * @param cube view it as a cube map rather than a 2D array. Point lights
     *             are sampled by direction, directional cascades by index.
     */
    void initDepthLayered(glm::ivec2 extent, VkFormat format, int layers, bool cube);
    void initLayeredDepthAttachment(glm::ivec2 extent, Format format, int layers,
                                   bool cube) override {
        initDepthLayered(extent, toVulkanFormat(format), layers, cube);
    }

    /**
     * A colour attachment that is also sampled afterwards, which is what every
     * G-buffer target is.
     */
    void initColorAttachment(glm::ivec2 extent, VkFormat format);
    void initColorAttachment(glm::ivec2 extent, Format format) override {
        initColorAttachment(extent, toVulkanFormat(format));
    }

    /**
     * A cube map array that is rendered into and then sampled: the derived
     * environment maps.
     *
     * @param cubes number of cube maps; the image holds six layers per cube.
     * @param mips  roughness levels, for a prefiltered map. One otherwise.
     *
     * The sampling view covers the whole thing as a cube array. Rendering needs
     * a different view per cube and per mip, which renderView supplies.
     */
    void initCubeArrayAttachment(glm::ivec2 faceExtent, VkFormat format, int cubes, int mips);
    void initCubeArrayAttachment(glm::ivec2 faceExtent, Format format, int cubes,
                                 int mips) override {
        initCubeArrayAttachment(faceExtent, toVulkanFormat(format), cubes, mips);
    }

    /**
     * A sampled-only cube array, every face filled from @p data.
     *
     * For the stand-in a descriptor needs when nothing real is bound yet: the
     * view type has to match how the shader declares the sampler, so a 2D array
     * will not do in a cube array's place.
     */
    void initSampledCubeArray(glm::ivec2 faceExtent, VkFormat format, int cubes,
                              const void *data);

    /**
     * A view of six consecutive layers at one mip, as a 2D array.
     *
     * This is what a cube's faces are rendered through - a six-view mask writes
     * one face per view. Works for colour and depth cube images. Created on
     * demand and owned by the image, because the number wanted is small and
     * fixed and they outlive any one frame.
     */
    VkImageView renderView(int cube, int mip);
    ImageView attachmentView(int cube, int mip) override {
        return toImageView(renderView(cube, mip));
    }

    /** A cube view of one cube in a cube-compatible image, for SamplerCube. */
    VkImageView cubeView(int cube);

    /** One face of a cube, suitable for a per-face dynamic-rendering pass. */
    VkImageView faceRenderView(int cube, int face, int mip = 0);
    ImageView faceAttachmentView(int cube, int face, int mip = 0) override {
        return toImageView(faceRenderView(cube, face, mip));
    }

    int mipLevels() const override { return _mipLevels; }

    /**
     * A sampled array or cube image, filled with @p data repeated per layer.
     *
     * A descriptor's view type has to match how the shader declares the
     * sampler: binding a 2D view where the shader says Sampler2DArray is a
     * validation error, not a coercion. The texture set therefore needs a
     * default of each shape, not one default.
     */
    void initSampledLayered(glm::ivec2 extent,
                            VkFormat format,
                            int layers,
                            bool cube,
                            const void *data);
    void initSampledLayered(glm::ivec2 extent,
                            Format format,
                            int layers,
                            bool cube,
                            const void *data) {
        initSampledLayered(extent, toVulkanFormat(format), layers, cube, data);
    }

    /**
     * A sampled array or cube image, each layer filled from its own pixels.
     *
     * The layer count comes from the data rather than being asked for
     * separately: a cube map is six faces in the order Vulkan and OpenGL agree
     * on, +X -X +Y -Y +Z -Z, and an array is however many frames it has. Sizes
     * are per layer because block-compressed data has no per-texel size.
     */
    void initSampledLayers(glm::ivec2 extent,
                           VkFormat format,
                           bool cube,
                           const std::vector<std::pair<const void *, VkDeviceSize>> &layers);

    /**
     * Move this image to @p layout.
     *
     * The image, rather than its caller, owns the current layout: only the
     * image can know which pass or frame last used it. A transition to the
     * current layout records nothing.
     */
    void transitionTo(VkCommandBuffer cmd, VkImageLayout layout);

    /** Move several images to one layout in a single dependency. */
    static void transitionTo(VkCommandBuffer cmd,
                             const std::vector<VulkanImage *> &images,
                             VkImageLayout layout);

    /**
     * Copy the image back to host memory, exactly as stored.
     *
     * @param depth  true to copy the depth aspect rather than colour.
     */
    std::vector<uint8_t> readBack(bool depth = false) const override;

    /**
     * Copy one mip's consecutive colour layers back to host memory. This is
     * used for cube-array diagnostics, where each cube face becomes one
     * vertically unrolled image in the dump.
     */
    std::vector<uint8_t> readBack(uint32_t mip, uint32_t layers) const override;

    void deinit() override;

    VkImage handle() const { return _image; }
    VkImageView view() const { return _view; }
    ImageView sampleView() const override { return toImageView(_view); }

    /**
     * The sampler this image should be read through, or null for the default.
     *
     * Filtering belongs to the texture in OpenGL and to the sampler in Vulkan,
     * so the sampler is chosen where the Texture is still in hand - at upload -
     * and carried here. Not owned; VulkanSamplers caches and destroys them.
     */
    VkSampler sampler() const { return _sampler; }
    Sampler sampleSampler() const override { return toSampler(_sampler); }
    void setSampler(VkSampler sampler) { _sampler = sampler; }
    void setSampler(Sampler sampler) override { _sampler = toVulkanSampler(sampler); }
    glm::ivec2 extent() const { return _extent; }
    Format pixelFormat() const override { return fromVulkanFormat(_format); }
    VkFormat format() const { return _format; }

private:
    VulkanDevice &_device;

    VkImage _image {VK_NULL_HANDLE};
    VkImageView _view {VK_NULL_HANDLE};
    VkSampler _sampler {VK_NULL_HANDLE};
    int _mipLevels {1};
    /** Keyed on cube * mipLevels + mip; see renderView. */
    std::unordered_map<int, VkImageView> _renderViews;
    std::unordered_map<int, VkImageView> _cubeViews;
    std::unordered_map<int, VkImageView> _faceRenderViews;
    VmaAllocation _allocation {VK_NULL_HANDLE};
    glm::ivec2 _extent {0};
    VkFormat _format {VK_FORMAT_UNDEFINED};
    uint32_t _layers {1};
    VkImageLayout _layout {VK_IMAGE_LAYOUT_UNDEFINED};
};

} // namespace graphics

} // namespace reone
