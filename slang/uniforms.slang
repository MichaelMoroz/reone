/*
 * Copyright (c) 2026 The reone project contributors
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
// Uniform block layouts, mirroring the structs in
// include/reone/graphics/uniforms.h.
//
// Slang reflection checks this std140 layout against the C++ mirrors at Vulkan
// renderer startup. See reone/graphics/vulkan/shadercompiler.h.
//
// Struct and member names deliberately match the C++ ones rather than the
// u-prefixed GLSL ones, so the generated assertions need no name mapping. The
// Vulkan shaders written in phase 4 will use these blocks directly.
//
/** Mirrors kMaxLights in include/reone/graphics/types.h - it sizes the block. */
static const int kMaxLights = 64;
static const int kNumShadowLightSpace = 6;
static const int kNumShadowCascades = 4;
/** Mirrors the ceilings in include/reone/graphics/types.h - they size the block. */
static const int kMaxDirectionalShadows = 4;
static const int kMaxPointShadows = 48; // mirrors graphics/types.h; the schema check enforces it
static const int kMaxShadowLights = kMaxDirectionalShadows + kMaxPointShadows;
static const int kMaxShadowCascadeMatrices = kMaxDirectionalShadows * kNumShadowCascades;
static const int kMaxBones = 24;
static const int kMaxDanglyVertices = 768;
static const int kMaxParticles = 64;
static const int kMaxGrassClusters = 256;
static const int kMaxTextChars = 128;
static const int kMaxWalkmeshMaterials = 32;
static const int kNumSSAOSamples = 64;

struct GlobalUniformsLight {
    float4 position; // W = 0 if light is directional
    float4 color;
    float multiplier;
    float radius;
    int ambientOnly;
    int dynamicType;
    /** Which shadow slot this light casts from, or -1 if it casts nothing. */
    int shadowSlot;
};

/**
 * One shadow-casting light, as the receiver needs it.
 *
 * A point caster carries no matrices: its cube faces are derived from the
 * position and a fixed 90-degree projection where they are rendered, and the
 * receiver samples the cube by direction. Only cascaded directional casters
 * need their transforms here, in shadowCascadeSpace indexed by mapIndex.
 */
struct GlobalUniformsShadowLight {
    /** Direction when w is 0, world position when w is 1 - as lights are. */
    float4 positionOrDirection;
    /** Fade, already multiplied by the area's authored ShadowOpacity. */
    float strength;
    float radius;
    /** First cascade for a directional caster, cube slice for a point one. */
    int mapIndex;
    /** Face resolution of this caster's map; carried, not queried. */
    int mapResolution;
};

struct GlobalUniforms {
    float4x4 projection;
    float4x4 projectionInv;
    float4x4 view;
    float4x4 viewInv;
    float4 cameraPosition;
    float4 worldAmbientColor;
    GlobalUniformsLight lights[kMaxLights];
    GlobalUniformsShadowLight shadowLights[kMaxShadowLights];
    // Shared by every directional caster - they split one camera frustum.
    float4 shadowCascadeFarPlanes;
    float4x4 shadowCascadeSpace[kMaxShadowCascadeMatrices];
    // Six consecutive faces per point caster, indexed by mapIndex * 6.
    // Unjittered, so that motion vectors never encode the jitter offset.
    float4x4 viewProjection;
    float4x4 prevViewProjection;
    float4 fogColor;
    float4 jitter; // XY = this frame's NDC jitter, ZW = previous frame's
    float clipNear;
    float clipFar;
    int numLights;
    int numShadowLights;
    float fogNear;
    float fogFar;
    /** Seconds since startup, this frame and the previous - see uniforms.h. */
    float time;
    float prevTime;
};

struct LocalUniforms {
    float4x4 model;
    float4x4 modelInv;
    float4x4 prevModel;
    // glm::mat3x4 - three columns of four floats. Declared as an array so that
    // the layout is unambiguous under Slang's row-major convention.
    float4 uv[3];
    float4 color;
    float4 ambientColor;
    float4 diffuseColor;
    float4 selfIllumColor;
    // Former loose uniforms. Vulkan has no equivalent, so they move into the
    // block; see doc/tasks/CONVENTIONS.md.
    float4 saberDisplacement;
    int featureMask;
    int bumpMapFrame;
    float bumpMapScale;
    float waterAlpha;
    float billboardSize;
    int envMapDerivedLayer;
    float iblRoughness;
};

struct BoneUniforms {
    float4x4 bones[kMaxBones];
    float4x4 prevBones[kMaxBones];
};

struct DanglyUniforms {
    float4 positions[kMaxDanglyVertices];
};

struct ParticleUniformsParticle {
    float4 positionFrame;
    float4 right;
    float4 up;
    float4 color;
    float2 size;
};

struct ParticleUniforms {
    int2 gridSize;
    ParticleUniformsParticle particles[kMaxParticles];
};

struct GrassUniformsCluster {
    float4 positionVariant; // fourth component is a variant (0-3)
    float2 lightmapUV;
    float yaw;
};

struct GrassUniforms {
    float2 quadSize;
    float radius;
    GrassUniformsCluster clusters[kMaxGrassClusters];
};

struct TextUniformsCharacter {
    float4 posScale;
    float4 uv;
};

struct TextUniforms {
    TextUniformsCharacter chars[kMaxTextChars];
};

struct AABBUniforms {
    float4 corners[8];
};

struct WalkmeshUniforms {
    float4 materials[kMaxWalkmeshMaterials];
};

struct ScreenEffectUniforms {
    float4x4 projection;
    float4x4 projectionInv;
    float4x4 screenProjection;
    float4 ssaoSamples[kNumSSAOSamples];
    float2 screenResolution;
    float2 screenResolutionRcp;
    float2 blurDirection;
    float clipNear;
    float clipFar;
    float ssaoSampleRadius;
    float ssaoBias;
    float ssrBias;
    float ssrPixelStride;
    float ssrMaxSteps;
    float sharpenAmount;
};

// Binding indices match UniformBlockBindingPoints in reone/graphics/types.h. In
// GLSL these become uniform block binding points; the host no longer has to look
// blocks up by name.
//
// No descriptor set index is given: Slang emits `set = N` into GLSL, which is not
// valid GLSL. That means block and texture bindings share one number space when
// targeting SPIR-V, so the two will need distinct ranges once Vulkan lands.
[[vk::binding(0)]] ConstantBuffer<GlobalUniforms> globalUniforms;
[[vk::binding(1)]] ConstantBuffer<LocalUniforms> localUniforms;
[[vk::binding(2)]] ConstantBuffer<BoneUniforms> boneUniforms;
[[vk::binding(3)]] ConstantBuffer<DanglyUniforms> danglyUniforms;
[[vk::binding(4)]] ConstantBuffer<AABBUniforms> aabbUniforms;
[[vk::binding(5)]] ConstantBuffer<ParticleUniforms> particleUniforms;
[[vk::binding(6)]] ConstantBuffer<GrassUniforms> grassUniforms;
[[vk::binding(7)]] ConstantBuffer<WalkmeshUniforms> walkmeshUniforms;
[[vk::binding(8)]] ConstantBuffer<TextUniforms> textUniforms;
[[vk::binding(9)]] ConstantBuffer<ScreenEffectUniforms> screenEffectUniforms;

// UniformsFeatureFlags in reone/graphics/uniforms.h
static const int kFeatureLightmap = 1 << 0;
static const int kFeatureEnvMap = 1 << 1;
static const int kFeatureNormalMap = 1 << 2;
static const int kFeatureBumpMap = 1 << 3;
static const int kFeatureSkin = 1 << 4;
static const int kFeatureDangly = 1 << 5;
static const int kFeatureSaber = 1 << 6;
static const int kFeatureShadows = 1 << 7;
static const int kFeatureWater = 1 << 8;
static const int kFeatureFog = 1 << 9;
static const int kFeatureFixedSize = 1 << 10;
static const int kFeatureHashedAlphaTest = 1 << 11;
static const int kFeaturePremulAlpha = 1 << 12;
static const int kFeatureEnvMapCube = 1 << 13;
static const int kFeatureStatic = 1 << 14;

bool isFeatureEnabled(int flag) {
    return (localUniforms.featureMask & flag) != 0;
}
