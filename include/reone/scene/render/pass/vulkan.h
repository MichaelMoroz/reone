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

#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/pipelinecache.h"

#include "../pass.h"
#include "../registry.h"

namespace reone {

namespace graphics {

class VulkanDescriptors;
class VulkanDevice;
class IMeshRegistry;
class VulkanPBRTextures;
class VulkanResources;
class VulkanUniformRing;

struct LocalUniforms;

} // namespace graphics

namespace scene {

/**
 * Records geometry into the Vulkan G-buffer.
 *
 * The counterpart of PBRRenderPass. Where that one changes context state and
 * draws, this one picks a pipeline out of the cache, writes a slice of the
 * frame's uniform arena and records a draw. The uniform contents are the same,
 * because both feed the same shaders.
 *
 * Only the opaque geometry pass is implemented. The other entry points on
 * IRenderPass are stubs that warn once - a scene containing particles should
 * render its geometry and say what it dropped, not abort the frame.
 */
class VulkanRenderPass : public IRenderPass, boost::noncopyable {
public:
    /**
     * Which of the pipeline's passes is recording.
     *
     * It decides three things at once - which fragment stage a draw picks,
     * whether it writes depth, and how it blends - and those three always move
     * together, so they are one choice rather than three flags.
     */
    enum class Kind {
        /** Deferred opaque geometry into the G-buffer. Writes depth. */
        Geometry,
        /**
         * Weighted-blended transparency into the pair of OIT targets. Depth
         * tests against the geometry pass but does not write.
         */
        OIT,
        /**
         * Straight onto an already-resolved image, in draw order. Read-only
         * depth as well; what lens flares and the like use.
         */
        Forward
    };

    VulkanRenderPass(graphics::GraphicsOptions &options,
                     graphics::VulkanDevice &device,
                     graphics::VulkanPipelineCache &pipelines,
                     graphics::VulkanUniformRing &ring,
                     graphics::VulkanDescriptors &descriptors,
                     graphics::VulkanResources &resources,
                     graphics::IUniforms &uniforms,
                     graphics::VulkanPBRTextures &pbrTextures,
                     graphics::IMeshRegistry &meshRegistry,
                     RenderRegistry &registry,
                     VkCommandBuffer cmd,
                     std::vector<VkFormat> colorFormats,
                     VkFormat depthFormat,
                     Kind kind = Kind::Geometry) :
        _options(options),
        _device(device),
        _pipelines(pipelines),
        _ring(ring),
        _descriptors(descriptors),
        _resources(resources),
        _uniforms(uniforms),
        _pbrTextures(pbrTextures),
        _meshRegistry(meshRegistry),
        _registry(registry),
        _cmd(cmd),
        _colorFormats(std::move(colorFormats)),
        _depthFormat(depthFormat),
        _kind(kind) {
    }

    void draw(graphics::Mesh &mesh,
              graphics::Material &material,
              const glm::mat4 &transform,
              const glm::mat4 &transformInv,
              const glm::mat4 &prevTransform) override;

    void drawSkinned(graphics::Mesh &mesh,
                     graphics::Material &material,
                     const glm::mat4 &transform,
                     const glm::mat4 &transformInv,
                     const glm::mat4 &prevTransform,
                     const std::vector<glm::mat4> &bones,
                     const std::vector<glm::mat4> &prevBones) override;

    void drawDangly(graphics::Mesh &mesh,
                    graphics::Material &material,
                    const glm::mat4 &transform,
                    const glm::mat4 &transformInv,
                    const glm::mat4 &prevTransform,
                    const std::vector<glm::vec4> &positions) override;

    void drawSaber(graphics::Mesh &mesh,
                   graphics::Material &material,
                   const glm::mat4 &transform,
                   const glm::mat4 &transformInv,
                   const glm::mat4 &prevTransform,
                   const glm::vec4 &displacement) override;

    void drawBillboard(graphics::Texture &texture,
                       const glm::vec4 &color,
                       const glm::mat4 &transform,
                       const glm::mat4 &transformInv,
                       std::optional<float> size) override;

    void drawParticles(graphics::Material &material,
                       const glm::ivec2 &gridSize,
                       const std::vector<ParticleInstance> &particles) override;

    void drawGrass(float radius,
                   float quadSize,
                   graphics::Material &material,
                   const std::vector<GrassInstance> &instances) override;

    void drawAABB(const std::vector<glm::vec4> &corners) override;

    /**
     * Offset of this frame's GlobalUniforms slice. The pipeline pushes it once
     * and every draw binds the same one.
     */
    void setGlobalsOffset(uint32_t offset) { _globalsOffset = offset; }

    /**
     * Which views a shadow draw broadcasts to. Zero outside a shadow pass; the
     * pipeline it builds has to declare the same mask the pass does.
     */
    void setShadowViewMask(uint32_t mask) { _shadowViewMask = mask; }

    int drawCount() const { return _drawCount; }

private:
    graphics::GraphicsOptions &_options;
    graphics::VulkanDevice &_device;
    graphics::VulkanPipelineCache &_pipelines;
    graphics::VulkanUniformRing &_ring;
    graphics::VulkanDescriptors &_descriptors;
    graphics::VulkanResources &_resources;
    graphics::IUniforms &_uniforms;
    graphics::VulkanPBRTextures &_pbrTextures;
    graphics::IMeshRegistry &_meshRegistry;
    RenderRegistry &_registry;

    VkCommandBuffer _cmd;
    std::vector<VkFormat> _colorFormats;
    VkFormat _depthFormat;
    Kind _kind;
    uint32_t _shadowViewMask {0};
    uint32_t _globalsOffset {0};
    int _drawCount {0};

    /** Set once per unimplemented entry point, so a frame warns rather than spams. */
    std::set<std::string> _warned;

    void warnOnce(const std::string &what);

    /**
     * Offset of the walkmesh block in this frame's arena, pushed on first use.
     *
     * Cached because the block is set once per frame by the scene graph but
     * read by every walkmesh draw, and because it cannot be pushed when the
     * pass is built: the scene graph fills it from inside the pass callback,
     * after that point.
     */
    uint32_t walkmeshOffset();

    void drawShadow(graphics::Mesh &mesh,
                    graphics::Material &material,
                    const glm::mat4 &transform);

    std::optional<uint32_t> _walkmeshOffset;

    /** Bind pipeline, both descriptor sets and draw. Every path ends here. */
    void bindAndDraw(const graphics::VulkanPipeline &pipeline,
                     const std::array<uint32_t, graphics::VulkanDescriptors::kNumUniformBlocks> &offsets,
                     const std::vector<std::pair<int, const graphics::VulkanImage *>> &textures,
                     const graphics::VulkanMesh &mesh,
                     int instances);

    /**
     * The common path: pick the pipeline for @p vertexEntry, fill locals from
     * the material, bind the material's textures and draw.
     *
     * @param extraOffsets per-block dynamic offsets the caller has already
     *                     pushed - bones for skinned, dangly positions, and so
     *                     on. Everything else defaults to the frame's globals.
     */
    void drawGeometry(graphics::Mesh &mesh,
                      graphics::Material &material,
                      const char *vertexEntry,
                      const glm::mat4 &transform,
                      const glm::mat4 &transformInv,
                      const glm::mat4 &prevTransform,
                      int extraFeatureBits,
                      const std::vector<std::pair<int, uint32_t>> &extraOffsets,
                      std::optional<glm::vec4> saberDisplacement = std::nullopt);

    void fillLocals(graphics::LocalUniforms &locals,
                    const graphics::Material &material,
                    const glm::mat4 &transform,
                    const glm::mat4 &transformInv,
                    const glm::mat4 &prevTransform,
                    int extraFeatureBits,
                    std::optional<glm::vec4> saberDisplacement) const;

};

} // namespace scene

} // namespace reone
