/*
 * GENERATED FILE - do not edit.
 *
 * Regenerate with: cmake --build build --target uniformlayout
 * Source of truth: slang/uniforms.slang
 *
 * Asserts that the std140 layout Slang computes for the shader uniform
 * blocks matches the C++ structs the engine uploads. A failure here means
 * the two have drifted, which at runtime would silently render garbage.
 */

#pragma once

#include <cstddef>

#include "reone/graphics/uniforms.h"

namespace reone {

namespace graphics {

// GlobalUniforms
static_assert(offsetof(GlobalUniforms, projection) == 0, "GlobalUniforms::projection moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, projectionInv) == 64, "GlobalUniforms::projectionInv moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, view) == 128, "GlobalUniforms::view moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, viewInv) == 192, "GlobalUniforms::viewInv moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, cameraPosition) == 256, "GlobalUniforms::cameraPosition moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, worldAmbientColor) == 272, "GlobalUniforms::worldAmbientColor moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, lights) == 288, "GlobalUniforms::lights moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, shadowLightPosition) == 1824, "GlobalUniforms::shadowLightPosition moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, shadowCascadeFarPlanes) == 1840, "GlobalUniforms::shadowCascadeFarPlanes moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, shadowLightSpace) == 1856, "GlobalUniforms::shadowLightSpace moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, viewProjection) == 2240, "GlobalUniforms::viewProjection moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, prevViewProjection) == 2304, "GlobalUniforms::prevViewProjection moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, fogColor) == 2368, "GlobalUniforms::fogColor moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, jitter) == 2384, "GlobalUniforms::jitter moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, clipNear) == 2400, "GlobalUniforms::clipNear moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, clipFar) == 2404, "GlobalUniforms::clipFar moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, numLights) == 2408, "GlobalUniforms::numLights moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, shadowStrength) == 2412, "GlobalUniforms::shadowStrength moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, shadowRadius) == 2416, "GlobalUniforms::shadowRadius moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, fogNear) == 2420, "GlobalUniforms::fogNear moved; shader layout disagrees");
static_assert(offsetof(GlobalUniforms, fogFar) == 2424, "GlobalUniforms::fogFar moved; shader layout disagrees");
static_assert(sizeof(GlobalUniforms) == 2432, "GlobalUniforms is not the size std140 expects");

// LocalUniforms
static_assert(offsetof(LocalUniforms, model) == 0, "LocalUniforms::model moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, modelInv) == 64, "LocalUniforms::modelInv moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, prevModel) == 128, "LocalUniforms::prevModel moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, uv) == 192, "LocalUniforms::uv moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, color) == 240, "LocalUniforms::color moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, ambientColor) == 256, "LocalUniforms::ambientColor moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, diffuseColor) == 272, "LocalUniforms::diffuseColor moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, selfIllumColor) == 288, "LocalUniforms::selfIllumColor moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, featureMask) == 304, "LocalUniforms::featureMask moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, bumpMapFrame) == 308, "LocalUniforms::bumpMapFrame moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, bumpMapScale) == 312, "LocalUniforms::bumpMapScale moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, waterAlpha) == 316, "LocalUniforms::waterAlpha moved; shader layout disagrees");
static_assert(offsetof(LocalUniforms, billboardSize) == 320, "LocalUniforms::billboardSize moved; shader layout disagrees");
static_assert(sizeof(LocalUniforms) == 336, "LocalUniforms is not the size std140 expects");

// BoneUniforms
static_assert(offsetof(BoneUniforms, bones) == 0, "BoneUniforms::bones moved; shader layout disagrees");
static_assert(offsetof(BoneUniforms, prevBones) == 1536, "BoneUniforms::prevBones moved; shader layout disagrees");
static_assert(sizeof(BoneUniforms) == 3072, "BoneUniforms is not the size std140 expects");

// DanglyUniforms
static_assert(offsetof(DanglyUniforms, positions) == 0, "DanglyUniforms::positions moved; shader layout disagrees");
static_assert(sizeof(DanglyUniforms) == 12288, "DanglyUniforms is not the size std140 expects");

// ParticleUniforms
static_assert(offsetof(ParticleUniforms, gridSize) == 0, "ParticleUniforms::gridSize moved; shader layout disagrees");
static_assert(offsetof(ParticleUniforms, particles) == 16, "ParticleUniforms::particles moved; shader layout disagrees");
static_assert(sizeof(ParticleUniforms) == 5136, "ParticleUniforms is not the size std140 expects");

// GrassUniforms
static_assert(offsetof(GrassUniforms, quadSize) == 0, "GrassUniforms::quadSize moved; shader layout disagrees");
static_assert(offsetof(GrassUniforms, radius) == 8, "GrassUniforms::radius moved; shader layout disagrees");
static_assert(offsetof(GrassUniforms, clusters) == 16, "GrassUniforms::clusters moved; shader layout disagrees");
static_assert(sizeof(GrassUniforms) == 8208, "GrassUniforms is not the size std140 expects");

// TextUniforms
static_assert(offsetof(TextUniforms, chars) == 0, "TextUniforms::chars moved; shader layout disagrees");
static_assert(sizeof(TextUniforms) == 4096, "TextUniforms is not the size std140 expects");

// WalkmeshUniforms
static_assert(offsetof(WalkmeshUniforms, materials) == 0, "WalkmeshUniforms::materials moved; shader layout disagrees");
static_assert(sizeof(WalkmeshUniforms) == 512, "WalkmeshUniforms is not the size std140 expects");

// ScreenEffectUniforms
static_assert(offsetof(ScreenEffectUniforms, projection) == 0, "ScreenEffectUniforms::projection moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, projectionInv) == 64, "ScreenEffectUniforms::projectionInv moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, screenProjection) == 128, "ScreenEffectUniforms::screenProjection moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, ssaoSamples) == 192, "ScreenEffectUniforms::ssaoSamples moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, screenResolution) == 1216, "ScreenEffectUniforms::screenResolution moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, screenResolutionRcp) == 1224, "ScreenEffectUniforms::screenResolutionRcp moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, blurDirection) == 1232, "ScreenEffectUniforms::blurDirection moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, clipNear) == 1240, "ScreenEffectUniforms::clipNear moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, clipFar) == 1244, "ScreenEffectUniforms::clipFar moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, ssaoSampleRadius) == 1248, "ScreenEffectUniforms::ssaoSampleRadius moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, ssaoBias) == 1252, "ScreenEffectUniforms::ssaoBias moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, ssrBias) == 1256, "ScreenEffectUniforms::ssrBias moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, ssrPixelStride) == 1260, "ScreenEffectUniforms::ssrPixelStride moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, ssrMaxSteps) == 1264, "ScreenEffectUniforms::ssrMaxSteps moved; shader layout disagrees");
static_assert(offsetof(ScreenEffectUniforms, sharpenAmount) == 1268, "ScreenEffectUniforms::sharpenAmount moved; shader layout disagrees");
static_assert(sizeof(ScreenEffectUniforms) == 1280, "ScreenEffectUniforms is not the size std140 expects");

// GlobalUniformsLight, an array element - its size is the array stride
static_assert(offsetof(GlobalUniformsLight, position) == 0, "GlobalUniformsLight::position moved; shader layout disagrees");
static_assert(offsetof(GlobalUniformsLight, color) == 16, "GlobalUniformsLight::color moved; shader layout disagrees");
static_assert(offsetof(GlobalUniformsLight, multiplier) == 32, "GlobalUniformsLight::multiplier moved; shader layout disagrees");
static_assert(offsetof(GlobalUniformsLight, radius) == 36, "GlobalUniformsLight::radius moved; shader layout disagrees");
static_assert(offsetof(GlobalUniformsLight, ambientOnly) == 40, "GlobalUniformsLight::ambientOnly moved; shader layout disagrees");
static_assert(offsetof(GlobalUniformsLight, dynamicType) == 44, "GlobalUniformsLight::dynamicType moved; shader layout disagrees");
static_assert(sizeof(GlobalUniformsLight) == 48, "GlobalUniformsLight does not match the std140 array stride");

// ParticleUniformsParticle, an array element - its size is the array stride
static_assert(offsetof(ParticleUniformsParticle, positionFrame) == 0, "ParticleUniformsParticle::positionFrame moved; shader layout disagrees");
static_assert(offsetof(ParticleUniformsParticle, right) == 16, "ParticleUniformsParticle::right moved; shader layout disagrees");
static_assert(offsetof(ParticleUniformsParticle, up) == 32, "ParticleUniformsParticle::up moved; shader layout disagrees");
static_assert(offsetof(ParticleUniformsParticle, color) == 48, "ParticleUniformsParticle::color moved; shader layout disagrees");
static_assert(offsetof(ParticleUniformsParticle, size) == 64, "ParticleUniformsParticle::size moved; shader layout disagrees");
static_assert(sizeof(ParticleUniformsParticle) == 80, "ParticleUniformsParticle does not match the std140 array stride");

// GrassUniformsCluster, an array element - its size is the array stride
static_assert(offsetof(GrassUniformsCluster, positionVariant) == 0, "GrassUniformsCluster::positionVariant moved; shader layout disagrees");
static_assert(offsetof(GrassUniformsCluster, lightmapUV) == 16, "GrassUniformsCluster::lightmapUV moved; shader layout disagrees");
static_assert(sizeof(GrassUniformsCluster) == 32, "GrassUniformsCluster does not match the std140 array stride");

// TextUniformsCharacter, an array element - its size is the array stride
static_assert(offsetof(TextUniformsCharacter, posScale) == 0, "TextUniformsCharacter::posScale moved; shader layout disagrees");
static_assert(offsetof(TextUniformsCharacter, uv) == 16, "TextUniformsCharacter::uv moved; shader layout disagrees");
static_assert(sizeof(TextUniformsCharacter) == 32, "TextUniformsCharacter does not match the std140 array stride");

} // namespace graphics

} // namespace reone
