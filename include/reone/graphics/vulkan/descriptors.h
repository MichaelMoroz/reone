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

#include "image.h"
#include "gpuscene.h"

namespace reone {

namespace graphics {

class VulkanDevice;
class VulkanUniformRing;
class VulkanResources;

/**
 * The uniform descriptor set: one dynamic uniform buffer per block, at the
 * bindings `slang/uniforms.slang` pins and `UniformBlockBindingPoints` names.
 *
 * Dynamic, so a draw selects its slice of the frame's arena with an offset
 * given at bind time rather than by owning a descriptor. Without that, every
 * draw would need its own descriptor set and the set count would track the
 * draw count.
 *
 * One set per frame in flight is enough because the descriptors themselves
 * never change - they always point at that frame's whole arena, and only the
 * offsets move.
 */
class VulkanDescriptors : boost::noncopyable {
public:
    /** Must match the number of blocks in uniforms.h and uniforms.slang. */
    static constexpr int kNumUniformBlocks = 10;

    /** Must cover every unit in TextureUnits. */
    static constexpr int kNumTextures = 23;

    /**
     * Uniform blocks and textures both start numbering at zero, so they cannot
     * share a set. Uniforms are set 0 and textures set 1, which the shaders
     * spell as [[vk::binding(n, 1)]].
     */
    static constexpr int kUniformSet = 0;
    static constexpr int kTextureSet = 1;
    static constexpr int kMegaDrawSet = 2;

    /** Distinct textures one frame may draw with before the pool is exhausted. */
    static constexpr uint32_t kMaxTextureSetsPerFrame = 1024;

    /** Passes with fixed textures: the resolve, and later the post chain. */
    static constexpr uint32_t kMaxPersistentTextureSets = 32;

    VulkanDescriptors(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanDescriptors();

    void init(int framesInFlight, VulkanUniformRing &ring);
    void deinit();

    VkDescriptorSetLayout uniformLayout() const { return _uniformLayout; }
    VkDescriptorSet uniformSet(int frame) const { return _uniformSets[frame]; }

    VkDescriptorSetLayout textureLayout() const { return _textureLayout; }
    VkDescriptorSetLayout megaDrawLayout() const { return _megaDrawLayout; }

    /** Publish one frame's merged geometry/material buffers and bindless
        texture tables to graphics set 2. */
    VkDescriptorSet updateMegaDrawSet(
        int frame, const VulkanGpuScene::View &scene,
        const VulkanResources &resources);

    /**
     * Point a texture unit at @p image for every set acquired from now on.
     *
     * For bindings that change rarely - G-buffer attachments, the BRDF table -
     * rather than per draw. Takes effect on the next acquireTextureSet, so it
     * is safe to call between frames but not mid-recording.
     */
    void setTexture(int unit, const VulkanImage &image);

    /** Release @p frame's texture sets for reuse. Call once per frame. */
    void beginFrame(int frame);

    /**
     * A texture set with @p mainTex at unit 0 and the standing bindings
     * elsewhere, valid for the rest of this frame.
     *
     * A set that is already bound to a recording command buffer may not be
     * written, so a draw that needs different textures needs a different set.
     * Sets are therefore handed out per distinct texture and recycled per
     * frame. Repeated draws with the same texture share one.
     *
     * This is the placeholder for descriptor indexing. §5.6 wants one bindless
     * array indexed per draw, which removes the whole problem, but that changes
     * how every shader declares its textures and is not worth doing before the
     * path tracer needs it.
     */
    VkDescriptorSet acquireTextureSet(int frame, const VulkanImage *mainTex);

    /**
     * A texture set with @p bindings applied over the standing ones, valid for
     * the rest of this frame.
     *
     * A material binds several units at once, so keying on one image is not
     * enough. Sets are cached per distinct combination within a frame, which
     * means a scene of N materials costs N sets rather than one per draw.
     */
    VkDescriptorSet acquireTextureSet(
        int frame,
        const std::vector<std::pair<int, const VulkanImage *>> &bindings);

    /**
     * A texture set written once and never recycled, for passes whose textures
     * do not change - the deferred resolve reading the G-buffer, say.
     *
     * Kept apart from the per-frame sets deliberately. Putting the G-buffer
     * into the standing bindings would mean the geometry pass also binds a
     * descriptor pointing at images that are colour attachments at that moment,
     * which is a layout mismatch the validation layers reject.
     */
    VkDescriptorSet createPersistentTextureSet(
        const std::vector<std::pair<int, const VulkanImage *>> &bindings);

private:
    VulkanDevice &_device;

    VkDescriptorPool _pool {VK_NULL_HANDLE};
    VkDescriptorSetLayout _uniformLayout {VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> _uniformSets;

    VkDescriptorSetLayout _textureLayout {VK_NULL_HANDLE};
    VkDescriptorSetLayout _megaDrawLayout {VK_NULL_HANDLE};
    VkDescriptorPool _megaDrawPool {VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> _megaDrawSets;
    uint32_t _bindlessTextureCapacity {0};
    VkSampler _sampler {VK_NULL_HANDLE};
    /**
     * One default per view shape. A unit declared Sampler2DArray in the shader
     * must be bound with an array view even when nothing has filled it in.
     */
    std::unique_ptr<VulkanImage> _default2D;
    std::unique_ptr<VulkanImage> _defaultArray;
    std::unique_ptr<VulkanImage> _defaultCube;
    /**
     * The irradiance and prefiltered environment maps are declared as cube
     * arrays. A 2D-array stand-in there is not merely wrong-looking; the view
     * type has to match how the shader declares the sampler or the descriptor
     * is invalid.
     */
    std::unique_ptr<VulkanImage> _defaultCubeArray;

    static const VulkanImage *defaultFor(int unit,
                                         const VulkanImage *twoD,
                                         const VulkanImage *array,
                                         const VulkanImage *cube,
                                         const VulkanImage *cubeArray);

    /** What every acquired set gets, before the per-draw main texture. */
    std::array<const VulkanImage *, kNumTextures> _standing {};

    struct TextureFrame {
        VkDescriptorPool pool {VK_NULL_HANDLE};
        std::unordered_map<const VulkanImage *, VkDescriptorSet> byTexture;
        /** Keyed by a hash of the whole binding list, for material sets. */
        std::unordered_map<size_t, VkDescriptorSet> byBindings;
    };
    std::vector<TextureFrame> _textureFrames;
    VkDescriptorPool _persistentPool {VK_NULL_HANDLE};

    void writeTextureSet(VkDescriptorSet set, const VulkanImage *mainTex);
    void writeTextureSet(VkDescriptorSet set,
                         const std::vector<std::pair<int, const VulkanImage *>> &bindings);
};

} // namespace graphics

} // namespace reone
