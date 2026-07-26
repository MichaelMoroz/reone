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
        _device(device) {
    }

    void deinit();

    /** Upload @p texture if it has not been seen, and return the image. */
    const VulkanImage &get(const Texture &texture);

    /** Upload @p mesh if it has not been seen, and return it. */
    const VulkanMesh &get(const Mesh &mesh);

    size_t textureCount() const { return _textures.size(); }
    size_t meshCount() const { return _meshes.size(); }

    /**
     * Whether a pixel format can be uploaded. The compressed formats cannot
     * yet, and the caller has to know rather than get a silently wrong image.
     */
    static bool supported(PixelFormat format);

private:
    VulkanDevice &_device;

    std::unordered_map<const Texture *, std::unique_ptr<VulkanImage>> _textures;
    std::unordered_map<const Mesh *, std::unique_ptr<VulkanMesh>> _meshes;
};

} // namespace graphics

} // namespace reone
