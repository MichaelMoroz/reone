/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <volk.h>

#include "reone/graphics/rhi.h"

namespace reone {

namespace graphics {

namespace detail {

class HandleAccess {
public:
    template <typename Tag>
    static Handle<Tag> make(uintptr_t value) {
        return Handle<Tag>(value);
    }

    template <typename Tag>
    static uintptr_t value(Handle<Tag> handle) {
        return handle._value;
    }
};

} // namespace detail

inline VkFormat toVulkanFormat(Format format) {
    switch (format) {
    case Format::R16G16Sfloat:
        return VK_FORMAT_R16G16_SFLOAT;
    case Format::R8G8B8A8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::B8G8R8A8Unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
    throw std::invalid_argument("Unknown RHI format");
}

inline Format fromVulkanFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16_SFLOAT:
        return Format::R16G16Sfloat;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return Format::R8G8B8A8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return Format::B8G8R8A8Unorm;
    default:
        throw std::invalid_argument("Vulkan format is outside the RHI seed");
    }
}

inline ImageView toImageView(VkImageView view) {
    return detail::HandleAccess::make<ImageViewTag>(reinterpret_cast<uintptr_t>(view));
}

inline Sampler toSampler(VkSampler sampler) {
    return detail::HandleAccess::make<SamplerTag>(reinterpret_cast<uintptr_t>(sampler));
}

inline DescriptorSet toDescriptorSet(VkDescriptorSet set) {
    return detail::HandleAccess::make<DescriptorSetTag>(reinterpret_cast<uintptr_t>(set));
}

inline Pipeline toPipeline(VkPipeline pipeline) {
    return detail::HandleAccess::make<PipelineTag>(reinterpret_cast<uintptr_t>(pipeline));
}

inline PipelineLayout toPipelineLayout(VkPipelineLayout layout) {
    return detail::HandleAccess::make<PipelineLayoutTag>(reinterpret_cast<uintptr_t>(layout));
}

inline VkImageView toVulkanImageView(ImageView view) {
    return reinterpret_cast<VkImageView>(detail::HandleAccess::value(view));
}

inline VkSampler toVulkanSampler(Sampler sampler) {
    return reinterpret_cast<VkSampler>(detail::HandleAccess::value(sampler));
}

inline VkDescriptorSet toVulkanDescriptorSet(DescriptorSet set) {
    return reinterpret_cast<VkDescriptorSet>(detail::HandleAccess::value(set));
}

inline VkPipeline toVulkanPipeline(Pipeline pipeline) {
    return reinterpret_cast<VkPipeline>(detail::HandleAccess::value(pipeline));
}

inline VkPipelineLayout toVulkanPipelineLayout(PipelineLayout layout) {
    return reinterpret_cast<VkPipelineLayout>(detail::HandleAccess::value(layout));
}

} // namespace graphics

} // namespace reone
