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

#include "../gpuscene.h"
#include "rhi.h"

namespace reone {

namespace graphics {

class IImage;
class IResources;

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
        int frame, const std::vector<std::pair<int, const IImage *>> &bindings) = 0;
    /** A texture table that remains fixed for the lifetime of a scene target. */
    virtual DescriptorSet createPersistentTextureSet(
        const std::vector<std::pair<int, const IImage *>> &bindings) = 0;
};

} // namespace graphics

} // namespace reone
