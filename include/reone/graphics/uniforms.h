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

#include "types.h"

namespace reone {

namespace graphics {

struct UniformBlockBindingPoints {
    static constexpr int globals = 0;
    static constexpr int locals = 1;
    static constexpr int bones = 2;
    static constexpr int dangly = 3;
    static constexpr int aabb = 4;
    static constexpr int particles = 5;
    static constexpr int grass = 6;
    static constexpr int walkmesh = 7;
    static constexpr int text = 8;
    static constexpr int screenEffect = 9;
};

struct UniformsFeatureFlags {
    static constexpr int lightmap = 1 << 0;
    static constexpr int envmap = 1 << 1;
    static constexpr int normalmap = 1 << 2;
    static constexpr int bumpmap = 1 << 3;
    static constexpr int skin = 1 << 4;
    static constexpr int dangly = 1 << 5;
    static constexpr int saber = 1 << 6;
    static constexpr int shadows = 1 << 7;
    static constexpr int water = 1 << 8;
    static constexpr int fog = 1 << 9;
    static constexpr int fixedsize = 1 << 10;
    static constexpr int hashedalphatest = 1 << 11;
    static constexpr int premulalpha = 1 << 12;
    static constexpr int envmapcube = 1 << 13;
    static constexpr int staticobj = 1 << 14;
    /**
     * A surface with no meaningful thickness: leaves, cloth, grass - whatever
     * an alpha cutout was standing in for.
     *
     * It is lit from both sides and it lets light through. Treating one as a
     * solid means the half of it facing away from the sun goes black, which is
     * wrong for a leaf and very visible on a curved blade of grass, where the
     * normal sweeps through the light and back out again.
     */
    static constexpr int thin = 1 << 15;
    /** Procedural quad/strand geometry rather than a model mesh. */
    static constexpr int procedural = 1 << 16;
};

struct alignas(16) GlobalUniformsLight {
    glm::vec4 position {0.0f}; /**< W = 0 if light is directional */
    glm::vec4 color {1.0f};
    float multiplier {1.0f};
    float radius {1.0f};
    int ambientOnly {0};
    int dynamicType {0};
    /**
     * Which shadow slot this light casts from, or -1 if it casts nothing.
     *
     * An index rather than a flag because a frame now holds several casters
     * and a receiver has to know WHICH map to sample, not merely that one
     * exists. A shadow is only visible on the light it attenuates, so the
     * corrected model has to let a caster reach static geometry even where the
     * original's dynamic-type rule would drop it - which is why this is tested
     * for "is a caster" as well as used as an index.
     */
    int shadowSlot {-1};
};

/**
 * One shadow-casting light, as the receiver needs it.
 *
 * Point lights carry no matrices here: a cube face's view is derivable from
 * the light position and a fixed 90-degree projection, so the shadow pass
 * builds its own and the receiver only ever samples by direction. Only the
 * cascaded directional maps need their transforms transported, and those live
 * in one flat array indexed by mapIndex.
 */
struct GlobalUniformsShadowLight {
    /** Direction when W is 0, world position when W is 1 - as lights are. */
    glm::vec4 positionOrDirection {0.0f};
    /** Fade, already multiplied by the area's authored ShadowOpacity. */
    float strength {0.0f};
    float radius {0.0f};
    /**
     * First cascade for a directional caster, cube slice for a point one.
     * The two index different images, so the kind in positionOrDirection.w is
     * what says which.
     */
    int mapIndex {0};
    int padding {0};
};

struct GlobalUniforms {
    glm::mat4 projection {1.0f};
    glm::mat4 projectionInv {1.0f};
    glm::mat4 view {1.0f};
    glm::mat4 viewInv {1.0f};
    glm::vec4 cameraPosition {0.0f};
    glm::vec4 worldAmbientColor {1.0f};
    GlobalUniformsLight lights[kMaxLights];
    GlobalUniformsShadowLight shadowLights[kMaxShadowLights];
    /** Shared by every directional caster - they split one camera frustum. */
    glm::vec4 shadowCascadeFarPlanes {0.0f};
    glm::mat4 shadowCascadeSpace[kMaxShadowCascadeMatrices] {glm::mat4(1.0f)};
    /** Six consecutive faces per point caster, indexed by mapIndex * 6. */
    glm::mat4 shadowPointSpace[kMaxShadowPointMatrices] {glm::mat4(1.0f)};
    /**
     * Unjittered view-projection of this frame and the previous one. Kept
     * separate from projection/view, which carry TAA jitter when it is enabled,
     * so that motion vectors never encode the jitter offset.
     */
    glm::mat4 viewProjection {1.0f};
    glm::mat4 prevViewProjection {1.0f};
    glm::vec4 fogColor {0.0f};
    glm::vec4 jitter {0.0f}; /**< XY = this frame's NDC jitter, ZW = previous frame's */
    float clipNear {kDefaultClipPlaneNear};
    float clipFar {kDefaultClipPlaneFar};
    int numLights {0};
    int numShadowLights {0};
    float fogNear {0.0f};
    float fogFar {0.0f};
    /**
     * Seconds since startup, this frame and the previous one.
     *
     * Two of them rather than one because anything that moves with time has to
     * be evaluable at both: a vertex animated to time() and reported as having
     * been in the same place last frame hands the temporal resolve a motion
     * vector of zero and it smears. Whatever reads time to place a vertex reads
     * prevTime to place where that vertex was.
     */
    float time {0.0f};
    float prevTime {0.0f};

    void reset() {
        projection = glm::mat4(1.0f);
        projectionInv = glm::mat4(1.0f);
        view = glm::mat4(1.0f);
        viewInv = glm::mat4(1.0f);
        viewProjection = glm::mat4(1.0f);
        prevViewProjection = glm::mat4(1.0f);
        cameraPosition = glm::vec4(0.0f);
        worldAmbientColor = glm::vec4(1.0f);
        shadowCascadeFarPlanes = glm::vec4(0.0f);
        fogColor = glm::vec4(0.0f);
        jitter = glm::vec4(0.0f);
        clipNear = kDefaultClipPlaneNear;
        clipFar = kDefaultClipPlaneFar;
        numLights = 0;
        numShadowLights = 0;
        fogNear = 0.0f;
        fogFar = 0.0f;
    }
};

/**
 * alignas(16) so that sizeof matches the std140 block size. glm's types are not
 * 16-byte aligned by default, and without this the struct ends at 324 bytes
 * against a 336-byte block - leaving the uniform buffer smaller than the block
 * the shader declares. The other blocks get the same alignment implicitly, from
 * an alignas(16) member.
 */
struct alignas(16) LocalUniforms {
    glm::mat4 model;
    glm::mat4 modelInv;
    glm::mat4 prevModel; /**< model transform as of the previous rendered frame */
    glm::mat3x4 uv;
    glm::vec4 color;
    glm::vec4 ambientColor;
    glm::vec4 diffuseColor;
    glm::vec4 selfIllumColor;
    /**
     * Formerly loose uniforms set by name per draw. Vulkan has no equivalent, so
     * they live in the block; see doc/tasks/CONVENTIONS.md.
     */
    glm::vec4 saberDisplacement;
    int featureMask;
    int bumpMapFrame;
    float bumpMapScale;
    float waterAlpha;
    float billboardSize;
    int envMapDerivedLayer;
    /** Roughness the prefiltered environment mip being generated stands for. */
    float iblRoughness;

    LocalUniforms() {
        reset();
    }

    void reset() {
        model = glm::mat4(1.0f);
        modelInv = glm::mat4(1.0f);
        prevModel = glm::mat4(1.0f);
        uv = glm::mat3x4(1.0f);
        color = glm::vec4(1.0f);
        ambientColor = glm::vec4(1.0f);
        diffuseColor = glm::vec4(1.0f);
        selfIllumColor = glm::vec4(0.0f);
        saberDisplacement = glm::vec4(0.0f);
        featureMask = 0;
        bumpMapFrame = 0;
        bumpMapScale = 1.0f;
        waterAlpha = 0.0f;
        billboardSize = 1.0f;
        envMapDerivedLayer = 0;
        iblRoughness = 0.0f;
    }
};

struct BoneUniforms {
    glm::mat4 bones[kMaxBones] {glm::mat4(1.0f)};
    glm::mat4 prevBones[kMaxBones] {glm::mat4(1.0f)}; /**< bones as of the previous rendered frame */
};

struct DanglyUniforms {
    glm::vec4 positions[kMaxDanglyVertices] {glm::vec4(0.0f)};
};

struct alignas(16) ParticleUniformsParticle {
    glm::vec4 positionFrame {0.0f};
    glm::vec4 right {1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 up {0.0f, 0.0f, 1.0f, 0.0f};
    glm::vec4 color {1.0f};
    glm::vec2 size {1.0f};
};

struct ParticleUniforms {
    glm::ivec2 gridSize {0};
    ParticleUniformsParticle particles[kMaxParticles];
};

struct alignas(16) GrassUniformsCluster {
    glm::vec4 positionVariant {0.0f}; /**< fourth component is a variant (0-3) */
    glm::vec2 lightmapUV {0.0f};
    float yaw {0.0f};
};

struct GrassUniforms {
    glm::vec2 quadSize {0.0f};
    float radius {0.0f};
    GrassUniformsCluster clusters[kMaxGrassClusters];
};

struct alignas(16) TextUniformsCharacter {
    glm::vec4 posScale {0.0f};
    glm::vec4 uv {0.0f};
};

struct TextUniforms {
    TextUniformsCharacter chars[kMaxTextChars];
};

struct AABBUniforms {
    glm::vec4 corners[8] {glm::vec4(0.0f)};
};

struct WalkmeshUniforms {
    glm::vec4 materials[kMaxWalkmeshMaterials] {glm::vec4(1.0f)};
};

/** alignas(16) for the same reason as LocalUniforms. */
struct alignas(16) ScreenEffectUniforms {
    glm::mat4 projection {1.0f};
    glm::mat4 projectionInv {1.0f};
    glm::mat4 screenProjection {1.0f};
    glm::vec4 ssaoSamples[kNumSSAOSamples] {glm::vec4(0.0f)};
    glm::vec2 screenResolution {0.0f};
    glm::vec2 screenResolutionRcp {0.0f};
    glm::vec2 blurDirection {0.0f};
    float clipNear {kDefaultClipPlaneNear};
    float clipFar {kDefaultClipPlaneFar};
    float ssaoSampleRadius {0.5f};
    float ssaoBias {0.1f};
    float ssrBias {0.5f};
    float ssrPixelStride {4.0f};
    float ssrMaxSteps {32.0f};
    float sharpenAmount {0.25f};
};

class Uniforms {
public:
    void setGlobals(const std::function<void(GlobalUniforms &)> &block);
    void setScreenEffect(const std::function<void(ScreenEffectUniforms &)> &block);

    const GlobalUniforms &globals() const { return _globals; }

private:
    GlobalUniforms _globals;
    ScreenEffectUniforms _screenEffect;
};

} // namespace graphics

} // namespace reone
