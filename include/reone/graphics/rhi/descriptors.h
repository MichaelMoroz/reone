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
};

/** Descriptor operations used by the 2D and image-based-lighting clients. */
class IDescriptors {
public:
    virtual ~IDescriptors() = default;

    static constexpr int kNumUniformBlocks = 10;
    static constexpr int kUniformSet = 0;
    static constexpr int kTextureSet = 1;

    virtual DescriptorSet uniformDescriptorSet(int frame) const = 0;
    virtual DescriptorSet updateMegaDrawSet(int frame, const GpuScene::View &scene,
                                            const IResources &resources) = 0;
    virtual DescriptorSet acquireTextureDescriptorSet(int frame, const IImage *mainTex) = 0;
    virtual DescriptorSet acquireTextureDescriptorSet(
        int frame, const std::vector<TextureBinding> &bindings) = 0;
    /** A texture table that remains fixed for the lifetime of a scene target. */
    virtual DescriptorSet createPersistentTextureSet(
        const std::vector<std::pair<int, const IImage *>> &bindings) = 0;
};

} // namespace graphics

} // namespace reone
