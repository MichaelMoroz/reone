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

#include "reone/graphics/types.h"

#include "image.h"
#include "samplers.h"
#include "mesh.h"

namespace reone {

namespace graphics {

class Mesh;
class Texture;
class VulkanDevice;

/**
 * The device-side copy of an engine resource, made on first use and kept.
 *
 * The engine's Texture and Mesh are asset containers: they hold decoded pixels
 * and interleaved vertices and know nothing about a backend. Uploading them is
 * a separate concern, and one that has to happen exactly once per resource no
 * matter how many draws reference it.
 *
 * Keyed by address. Both are owned elsewhere by shared_ptr and outlive the
 * frame, and neither has a stable identifier that survives a reload - name
 * collisions across resource types are real in this engine.
 */
class VulkanResources : boost::noncopyable {
public:
    VulkanResources(VulkanDevice &device) :
        _device(device),
        _samplers(device) {
    }

    void deinit();

    /** The sampler cache, for images this class did not upload. */
    VulkanSamplers &samplers() { return _samplers; }

    /**
     * Drop uploaded Textures and Meshes, keeping externally registered images.
     *
     * Those are render targets owned by a pipeline that outlives the module,
     * and dropping them would dangle the scene output handle.
     */
    void clearUploaded();

    /**
     * Forget one uploaded texture after waiting for draws that may sample it.
     *
     * The replacement is created by get() in the current frame. Waiting here
     * is intentionally conservative: the old image can be sampled by either
     * of the two frames in flight, and destroying it earlier is a use-after-
     * free on the GPU rather than a normal cache eviction.
     */
    void invalidate(const Texture &texture);

    /** Upload @p texture if it has not been seen, and return the image. */
    const VulkanImage &get(const Texture &texture);

    /**
     * Dense bindless descriptor index assigned when a material texture is
     * uploaded. Each descriptor view shape publishes the same id in its own
     * table. A texture the upload path cannot represent has no id, so a shader
     * can deliberately fall back rather than sampling an unrelated descriptor.
     */
    std::optional<uint32_t> textureId(const Texture &texture);

    /** Uploaded 2D textures and their stable bindless indices. */
    std::vector<std::pair<uint32_t, const VulkanImage *>> uploadedTextures() const;

    /** Uploaded 2D-array textures and their stable bindless indices. */
    std::vector<std::pair<uint32_t, const VulkanImage *>> uploadedTextureArrays() const;

    /** Uploaded cube textures and their stable bindless indices. */
    std::vector<std::pair<uint32_t, const VulkanImage *>> uploadedTextureCubes() const;

    /**
     * Associate @p texture with an image this cache does not own.
     *
     * Render targets cross the backend seam as a `Texture &` - that is what
     * `IRenderer::drawSceneOutput` takes - but they have no pixels to upload.
     * Registering the pair lets the identity survive the seam without changing
     * the interface or giving Texture a backend-specific field.
     */
    void registerExternal(const Texture &texture, const VulkanImage &image);

    /**
     * Drop a registration, before the image behind it is destroyed. Without
     * this the map keeps pointing at freed memory.
     */
    void unregisterExternal(const Texture &texture);

    /**
     * Whether this Texture is a render target this backend produced, rather
     * than pixels uploaded from an asset.
     *
     * The distinction matters to anything that samples it: uploaded rows are in
     * OpenGL's bottom-up order, a render target's are not.
     */
    bool isExternal(const Texture &texture) const {
        return _external.find(&texture) != _external.end();
    }

    /**
     * A small zero-filled buffer, bound at VulkanMesh::kZeroBinding so that
     * attributes a mesh does not provide read zeros. Shared by every draw.
     */
    VkBuffer zeroBuffer();

    /** Upload @p mesh if it has not been seen, and return it. */
    const VulkanMesh &get(const Mesh &mesh);

    /**
     * Monotonically changes whenever module-owned uploads are discarded.
     * Consumers with pointer-keyed side caches must discard their entries at
     * this boundary: the next module may reuse an old asset address.
     */
    uint64_t generation() const { return _generation; }

    size_t textureCount() const { return _textures.size(); }
    size_t meshCount() const { return _meshes.size(); }

    /**
     * Whether a pixel format can be uploaded. The compressed formats cannot
     * yet, and the caller has to know rather than get a silently wrong image.
     */
    static bool supported(PixelFormat format);

private:
    VulkanDevice &_device;
    /**
     * Owned here because a sampler is chosen per texture at upload, which is
     * the only point where the Texture and its properties are both in hand.
     */
    VulkanSamplers _samplers;

    struct UploadedTexture {
        std::unique_ptr<VulkanImage> image;
        uint32_t id {UINT32_MAX};
    };
    std::unordered_map<const Texture *, UploadedTexture> _textures;
    uint32_t _nextTextureId {0};
    std::unordered_map<const Texture *, const VulkanImage *> _external;

    /**
     * Stand-ins for textures this cache cannot upload yet - cube maps, arrays,
     * and anything with no pixel data. One per view shape, because a descriptor
     * must match how the shader declares the sampler.
     */
    std::unique_ptr<VulkanBuffer> _zeroBuffer;
    std::unique_ptr<VulkanImage> _fallback2D;
    std::unique_ptr<VulkanImage> _fallbackArray;
    std::unique_ptr<VulkanImage> _fallbackCube;
    std::set<std::string> _warned;

    const VulkanImage &fallbackFor(const Texture &texture, const std::string &why);
    std::unordered_map<const Mesh *, std::unique_ptr<VulkanMesh>> _meshes;
    uint64_t _generation {0};
};

} // namespace graphics

} // namespace reone
