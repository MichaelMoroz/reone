/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "texture.h"
#include "reone/graphics/rhi/commandbuffer.h"
#include "reone/graphics/rhi/descriptors.h"
#include "reone/graphics/rhi/image.h"
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/graphics/rhi/uniformring.h"

namespace reone {

namespace graphics {

struct EnvMapDerivedRequest {
    Texture &texture;

    EnvMapDerivedRequest(Texture &texture) :
        texture(texture) {
    }
};

} // namespace graphics

} // namespace reone

template <>
struct std::less<reone::graphics::EnvMapDerivedRequest> {
    bool operator()(const reone::graphics::EnvMapDerivedRequest &lhs,
                    const reone::graphics::EnvMapDerivedRequest &rhs) const {
        return lhs.texture.name() < rhs.texture.name();
    }
};

namespace reone {

namespace graphics {

class IPBRTextures {
public:
    virtual ~IPBRTextures() = default;

    virtual void refresh() = 0;
    virtual void requestEnvMapDerived(EnvMapDerivedRequest request) = 0;
    /** Reserve a stable derived-map layer for an admitted environment map. */
    virtual int requestEnvMapDerivedLayer(Texture &envMap) = 0;
    virtual std::optional<int> findEnvMapDerivedLayer(const std::string &name) = 0;
    virtual void process(ICommandBuffer &commandBuffer, uint32_t globalsOffset) = 0;
    virtual const IImage &brdfImage() const = 0;
    virtual const IImage &irradianceArray() const = 0;
    virtual const IImage &prefilteredArray() const = 0;
    virtual const std::map<int, Texture *> &sourceEnvMaps() const = 0;
};

class PBRTextures : public IPBRTextures, boost::noncopyable {
public:
    PBRTextures(IRenderer &renderer,
                      IPipelineCache &pipelines,
                      IUniformRing &ring,
                      IDescriptors &descriptors,
                      IResources &resources) :
        _renderer(renderer),
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
    void process(ICommandBuffer &commandBuffer, uint32_t globalsOffset) override;

    /** Forget module-owned source reservations; process performs generation. */
    void refresh() override;

    void requestEnvMapDerived(EnvMapDerivedRequest request) override {
        requestEnvMapDerivedLayer(request.texture);
    }

    /** Reserve the layer immediately so cached material records stay stable. */
    int requestEnvMapDerivedLayer(Texture &envMap) override;

    std::optional<int> findEnvMapDerivedLayer(const std::string &name) override {
        auto it = _envMapToLayer.find(name);
        if (it == _envMapToLayer.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    const IImage &brdfImage() const override { return *_brdf; }
    const IImage &irradianceArray() const override { return *_irradiance; }
    const IImage &prefilteredArray() const override { return *_prefiltered; }
    /** Source textures currently occupying the derived-map ring, for diagnostics. */
    const std::map<int, Texture *> &sourceEnvMaps() const override { return _envMapSources; }

private:
    IRenderer &_renderer;
    IPipelineCache &_pipelines;
    IUniformRing &_ring;
    IDescriptors &_descriptors;
    IResources &_resources;

    bool _inited {false};
    bool _brdfGenerated {false};

    std::unique_ptr<IImage> _brdf;
    std::unique_ptr<IImage> _irradiance;
    std::unique_ptr<IImage> _prefiltered;

    std::set<EnvMapDerivedRequest> _requests;
    std::unordered_map<std::string, int> _envMapToLayer;
    std::map<int, Texture *> _envMapSources;
    int _nextLayer {0};

    void generateBRDF(ICommandBuffer &commandBuffer, uint32_t globalsOffset);
    void generateDerived(ICommandBuffer &commandBuffer, uint32_t globalsOffset,
                         Texture &envMap, int layer);

    /** One six-view pass over a cube's faces at one mip. */
    void renderCubeFaces(ICommandBuffer &commandBuffer,
                         uint32_t globalsOffset,
                         IImage &target,
                         int cube,
                         int mip,
                         glm::ivec2 extent,
                         const char *fragmentEntry,
                         const Texture *envMap,
                         float roughness);
};

} // namespace graphics

} // namespace reone
