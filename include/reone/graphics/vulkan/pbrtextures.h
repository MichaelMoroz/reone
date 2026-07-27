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

#include "reone/graphics/pbrtextures.h"

#include "image.h"

namespace reone {

namespace graphics {

class VulkanDescriptors;
class VulkanDevice;
class VulkanPipelineCache;
class VulkanResources;
class VulkanUniformRing;

/**
 * The environment-derived textures image-based lighting needs, generated on the
 * GPU rather than loaded.
 *
 * An Odyssey environment map is a plain cube map. What the resolve samples is
 * two things computed from it - a cosine-convolved irradiance map for ambient
 * diffuse, and a roughness-mipped prefiltered map for ambient specular - plus a
 * BRDF integration lookup that depends on nothing at all. This is the Vulkan
 * counterpart of PBRTextures, which does the same with the OpenGL pipeline.
 *
 * Cube maps are derived at most one per frame, into a fixed ring of layer
 * slots, so a module with more environment maps than slots recycles the oldest
 * rather than growing without bound. That matches the OpenGL behaviour,
 * including its consequences.
 */
class VulkanPBRTextures : public IPBRTextures, boost::noncopyable {
public:
    VulkanPBRTextures(VulkanDevice &device,
                      VulkanPipelineCache &pipelines,
                      VulkanUniformRing &ring,
                      VulkanDescriptors &descriptors,
                      VulkanResources &resources) :
        _device(device),
        _pipelines(pipelines),
        _ring(ring),
        _descriptors(descriptors),
        _resources(resources) {
    }

    void init();
    void deinit();

    /**
     * Generate whatever is outstanding, recording into @p cmd.
     *
     * Called from inside the frame rather than from a standalone submit, so it
     * can use the frame's command buffer, uniform arena and descriptor pools
     * instead of needing its own of each. Must not be called inside a render
     * pass; it begins its own.
     */
    void process(VkCommandBuffer cmd, uint32_t globalsOffset);

    /** No-op here: generation needs a command buffer, so process does the work. */
    void refresh() override {}

    void requestEnvMapDerived(EnvMapDerivedRequest request) override {
        _requests.insert(std::move(request));
    }

    std::optional<int> findEnvMapDerivedLayer(const std::string &name) override {
        auto it = _envMapToLayer.find(name);
        if (it == _envMapToLayer.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /** Unused under Vulkan - the resolve binds the image, not the Texture. */
    Texture &brdf() override;

    const VulkanImage &brdfImage() const { return *_brdf; }
    const VulkanImage &irradianceArray() const { return *_irradiance; }
    const VulkanImage &prefilteredArray() const { return *_prefiltered; }
    /** Source textures currently occupying the derived-map ring, for diagnostics. */
    const std::map<int, Texture *> &sourceEnvMaps() const { return _envMapSources; }

private:
    VulkanDevice &_device;
    VulkanPipelineCache &_pipelines;
    VulkanUniformRing &_ring;
    VulkanDescriptors &_descriptors;
    VulkanResources &_resources;

    bool _inited {false};
    bool _brdfGenerated {false};

    std::unique_ptr<VulkanImage> _brdf;
    std::unique_ptr<VulkanImage> _irradiance;
    std::unique_ptr<VulkanImage> _prefiltered;

    std::set<EnvMapDerivedRequest> _requests;
    std::unordered_map<std::string, int> _envMapToLayer;
    std::map<int, Texture *> _envMapSources;
    int _nextLayer {0};

    void generateBRDF(VkCommandBuffer cmd, uint32_t globalsOffset);
    void generateDerived(VkCommandBuffer cmd, uint32_t globalsOffset,
                         Texture &envMap, int layer);

    /** One six-view pass over a cube's faces at one mip. */
    void renderCubeFaces(VkCommandBuffer cmd,
                         uint32_t globalsOffset,
                         VulkanImage &target,
                         int cube,
                         int mip,
                         glm::ivec2 extent,
                         const char *fragmentEntry,
                         const Texture *envMap,
                         float roughness);
};

} // namespace graphics

} // namespace reone
