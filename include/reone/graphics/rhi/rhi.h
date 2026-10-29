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

#include <cstdint>

namespace reone {

namespace graphics {

/** Pixel formats named by the 2D and image-based-lighting clients. */
enum class Format {
    D32Sfloat,
    R8Unorm,
    R16Sfloat,
    R16G16Sfloat,
    R32Uint,
    R16G16B16A16Sfloat,
    R32Sfloat,
    R8G8B8A8Unorm,
    B8G8R8A8Unorm,
    B8G8R8A8Srgb,
    BC1RGBAUnormBlock,
    BC3UnormBlock,
};

namespace detail {

class HandleAccess;

} // namespace detail

template <typename Tag>
class Handle {
public:
    constexpr Handle() = default;

    constexpr explicit operator bool() const { return _value != 0; }

private:
    explicit constexpr Handle(uintptr_t value) :
        _value(value) {
    }

    uintptr_t _value {0};

    friend class detail::HandleAccess;
};

struct ImageViewTag;
struct SamplerTag;
struct DescriptorSetTag;
struct PipelineTag;
struct PipelineLayoutTag;
struct TracingStructureTag;
struct BufferTag;

using ImageView = Handle<ImageViewTag>;
using Sampler = Handle<SamplerTag>;
using DescriptorSet = Handle<DescriptorSetTag>;
using Pipeline = Handle<PipelineTag>;
using PipelineLayout = Handle<PipelineLayoutTag>;
using TracingStructure = Handle<TracingStructureTag>;
using Buffer = Handle<BufferTag>;

} // namespace graphics

} // namespace reone
