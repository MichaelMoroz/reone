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

#include "reone/graphics/gbuffer.h"
#include "image.h"

namespace reone {

namespace graphics {

class VulkanDevice;

std::unique_ptr<IGBuffer> makeGBuffer(VulkanDevice &device);

/**
 * The opaque geometry pass targets: what a fragment knows about the surface it
 * covered, kept so lighting can be resolved once per pixel afterwards rather
 * than once per fragment.
 *
 * The first five attachments match the retained geometry contract. The final
 * attachment identifies the material record used by deferred lighting.
 */
class VulkanGBuffer : public IGBuffer, boost::noncopyable {
public:
    /** Attachment order, matching fbOpaqueGeometry in the GL pipeline. */
    enum Attachment {
        Diffuse = 0,
        EyeNormal,
        Lightmap,
        SelfIllum,
        Motion,
        /**
         * Extension point for deferred material data. A resolve that needs a
         * new field adds it to InstanceMaterial, not to the G-buffer.
         */
        MaterialId,
        Count
    };

    /** R16_UINT clear value; valid material indices stop at 0xfffe. */
    static constexpr uint32_t kNoMaterial = IGBuffer::kNoMaterial;

    VulkanGBuffer(VulkanDevice &device) :
        _device(device) {
    }

    void init(glm::ivec2 extent) override;
    void deinit() override;

    /** Formats in attachment order, for building a pipeline against this set. */
    static std::vector<VkFormat> nativeColorFormats();
    static VkFormat nativeDepthFormat() { return VK_FORMAT_D32_SFLOAT; }
    std::vector<Format> colorFormats() const override;
    Format depthFormat() const override { return Format::D32Sfloat; }

    glm::ivec2 extent() const override { return _extent; }
    /**
     * Assign the samplers these targets are read through.
     *
     * Filtering is per-texture in OpenGL and per-sampler in Vulkan, so images
     * the renderer creates itself have to be told, or they fall back to the
     * global sampler - which repeats, and filters depth.
     */
    void setSamplers(VkSampler color, VkSampler depth, VkSampler materialId) {
        for (auto &image : _color) {
            image->setSampler(color);
        }
        _color[MaterialId]->setSampler(materialId);
        _depth->setSampler(depth);
    }
    void setSamplers(Sampler color, Sampler depth, Sampler materialId) override {
        setSamplers(toVulkanSampler(color), toVulkanSampler(depth), toVulkanSampler(materialId));
    }

    VulkanImage &color(int attachment) { return *_color[attachment]; }
    VulkanImage &color(GBufferAttachment attachment) override {
        return color(static_cast<int>(attachment));
    }
    VulkanImage &depth() override { return *_depth; }

private:
    VulkanDevice &_device;

    glm::ivec2 _extent {0};
    std::array<std::unique_ptr<VulkanImage>, Count> _color;
    std::unique_ptr<VulkanImage> _depth;
};

} // namespace graphics

} // namespace reone
