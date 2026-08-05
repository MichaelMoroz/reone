/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>

namespace reone {

namespace graphics {

/** Pixel formats named by the 2D and image-based-lighting clients. */
enum class Format {
    D32Sfloat,
    R16G16Sfloat,
    R16Uint,
    R8G8B8A8Unorm,
    B8G8R8A8Unorm,
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

using ImageView = Handle<ImageViewTag>;
using Sampler = Handle<SamplerTag>;
using DescriptorSet = Handle<DescriptorSetTag>;
using Pipeline = Handle<PipelineTag>;
using PipelineLayout = Handle<PipelineLayoutTag>;

} // namespace graphics

} // namespace reone
