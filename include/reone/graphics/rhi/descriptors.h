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

#include <utility>
#include <vector>

#include "../rendering/gpuscene.h"
#include "rhi.h"

namespace reone {

namespace graphics {

class IImage;
class IResources;

/**
 * One entry of a texture set: the unit, the image, and optionally the view to
 * read it through.
 *
 * The view is only needed when the image's own view has the wrong shape for
 * the sampler the shader declares - a cube array holding a single cube is the
 * case that forced it, since the baked sky cube and the fallback cube are
 * different image shapes behind one binding. Leaving it null binds the image's
 * own view, which is what every other caller wants.
 */
struct TextureBinding {
    int unit {0};
    const IImage *image {nullptr};
    ImageView view;
    /**
     * Overrides the image's own sampler for this binding alone. The one
     * client is the raw view of the point-shadow cube: the same image binds
     * with its comparison sampler at shadowMapCube and with a plain sampler
     * at pointShadowRaw, and a sampler that lives on the image cannot be two
     * things at once.
     */
    Sampler sampler;
};

/** Descriptor operations used by the 2D and image-based-lighting clients. */
class IDescriptors {
public:
    virtual ~IDescriptors() = default;

    static constexpr int kNumUniformBlocks = 10;
    static constexpr int kUniformSet = 0;
    static constexpr int kTextureSet = 1;
    static constexpr int kMegaDrawSet = 2;
    /** The resolve set; see acquireResolveDescriptorSet. */
    static constexpr int kResolveSet = 3;

    virtual DescriptorSet uniformDescriptorSet(int frame) const = 0;
    virtual DescriptorSet updateMegaDrawSet(int frame, const GpuScene::View &scene,
                                            const IResources &resources) = 0;
    virtual DescriptorSet acquireTextureDescriptorSet(int frame, const IImage *mainTex) = 0;
    virtual DescriptorSet acquireTextureDescriptorSet(
        int frame, const std::vector<TextureBinding> &bindings) = 0;
    /**
     * The two bindings a resolve needs that its persistent texture table cannot
     * hold: the image it writes, and this frame's sky cube.
     *
     * Both change from frame to frame for reasons the persistent set cannot
     * express. The scene output and the tail target exchange identities every
     * time a tail pass runs, so "the image the resolve writes" is not one image;
     * and the sky cube is a view into whichever room was last baked, which a set
     * written once at pipeline construction could never have named.
     *
     * @p output may be null - the retro resolve is a fragment pass and writes an
     * attachment - in which case that binding is left unwritten, which the
     * layout permits because nothing that omits it declares it.
     */
    virtual DescriptorSet acquireResolveDescriptorSet(int frame, const IImage *output,
                                                      const IImage *skyCube,
                                                      ImageView skyView,
                                                      const IImage *const *channels = nullptr,
                                                      uint32_t channelCount = 0) = 0;
    /** A texture table that remains fixed for the lifetime of a scene target. */
    virtual DescriptorSet createPersistentTextureSet(
        const std::vector<TextureBinding> &bindings) = 0;
    /**
     * Give a persistent set back.
     *
     * Persistent sets outlive a frame by definition, so nothing recycles them
     * per frame the way the transient pools are recycled. Without this, every
     * graphics rebuild - a resolution change, an anti-aliasing change, a render
     * mode switch - leaks the sets its old scene pipeline allocated, and the
     * pool runs out after a handful of them. The caller owns the wait: the GPU
     * must have finished with the set before it is freed.
     */
    virtual void freePersistentTextureSet(DescriptorSet set) = 0;
};

} // namespace graphics

} // namespace reone
