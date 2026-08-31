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
import uniforms;

// Comparison samplers: each tap is already a 2x2 percentage-closer filter.
[[vk::binding(15, 1)]] Sampler2DArrayShadow sShadowMap;
[[vk::binding(19, 1)]] SamplerCubeArrayShadow sShadowMapCube;

// kNumShadowCascades comes from uniforms, which sizes the cascade table with it.
static const int kNumPCFSamples = 20;
static const int kNumDirectionalPCFSamples = 16;
/** Upper bound on the kernel, in texels of whichever cascade is sampled. */
static const float kDirectionalPCFRadiusTexels = 2.5;
/** Penumbra half-width in world units, held equal across all cascades.
    Matches what 2.5 texels of the nearest cascade already produced. */
static const float kDirectionalPenumbraWorld = 0.01;
/** Fraction of a cascade's depth range spent fading into the next one. */
static const float kCascadeBlendBand = 0.2;

static const float2 kPoissonDisk[kNumDirectionalPCFSamples] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)};

/** Point kernel and bias in texels of the cube face: a texel covers 2*distance/resolution of world. */
static const float kPCFSampleRadiusTexels = 1.5;
static const float kPointNormalOffsetTexels = 2.5;
/** The whole of the point path's bias: a shader-written radial depth receives no pipeline bias. */
static const float kPointDepthBiasTexels = 3.0;
static const float kPointShadowFar = 2500.0;
// Receiver normal offset for grazing surfaces, in texels: a cascade's texel
// grows with its far plane, so one world-space offset cannot serve all four.
static const float kDirectionalNormalOffsetTexels = 2.5;

// One visibility value: how much of the casting light's direct light reaches
// the surface. Never applied to baked irradiance, flat ambient or a reflection
// - those carry their own occlusion or none.

/** Cascade whose far plane first contains this depth; the last one otherwise. */
int cascadeFor(float3 viewPos) {
    for (int i = 0; i < kNumShadowCascades; ++i) {
        if (abs(viewPos.z) < globalUniforms.shadowCascadeFarPlanes[i]) {
            return i;
        }
    }
    return kNumShadowCascades - 1;
}

/** Shadow from one cascade, with a penumbra of constant width in world units. */
float shadowInCascade(int mapIndex, int mapResolution, int cascade, float3 worldPos,
                      float3 normal, float3 lightVector) {
    float NdotL = dot(normal, lightVector);
    float sinAngle = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    // World units per texel off the cascade's own orthographic matrix: extent
    // is 2 / the row's length. Resolution from the uniform, not GetDimensions.
    const float width = float(max(1, mapResolution));
    float2 texelSize = 1.0 / float2(width, width);
    float4x4 cascadeSpace = globalUniforms.shadowCascadeSpace[mapIndex + cascade];
    float3 lightSpaceX = float3(cascadeSpace[0][0], cascadeSpace[1][0], cascadeSpace[2][0]);
    float texelWorldSize = 2.0 / (max(1e-6, length(lightSpaceX)) * width);
    float3 offsetWorldPos =
        worldPos + normal * sinAngle * kDirectionalNormalOffsetTexels * texelWorldSize;
    float4 lightSpacePos = mul(cascadeSpace, float4(offsetWorldPos, 1.0));
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords.xy = 0.5 * projCoords.xy + 0.5;

    float currentDepth = projCoords.z;
    if (currentDepth > 1.0) {
        return 0.0;
    }

    // Penumbra fixed in world units, not texels: a texel radius would triple
    // at every cascade split and draw the transition.
    float3 probeAxis = abs(lightVector.z) < 0.99 ? float3(0.0, 0.0, 1.0)
                                                 : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(lightVector, probeAxis));
    float4 probe = mul(cascadeSpace, float4(offsetWorldPos + tangent, 1.0));
    float2 probeUv = 0.5 * (probe.xy / probe.w) + 0.5;
    float uvPerWorld = max(length(probeUv - projCoords.xy), 1e-7);
    // Below one texel the kernel degenerates to a point sample, so the coarse
    // far cascades stay at that floor instead of aliasing.
    float radiusTexels = clamp(kDirectionalPenumbraWorld * uvPerWorld / texelSize.x,
                               1.0, kDirectionalPCFRadiusTexels);

    // A Poisson disk rotated per receiver, keyed on world position so the
    // kernel is stable frame to frame; a box gave ten stair-stepped levels.
    float angle = frac(sin(dot(floor(worldPos * 32.0).xy, float2(12.9898, 78.233))) *
                       43758.5453) * 6.2831853;
    float2 rot = float2(cos(angle), sin(angle));

    float shadow = 0.0;
    for (int i = 0; i < kNumDirectionalPCFSamples; ++i) {
        float2 disk = kPoissonDisk[i];
        float2 rotated = float2(disk.x * rot.x - disk.y * rot.y,
                                disk.x * rot.y + disk.y * rot.x);
        float2 offset = rotated * texelSize * radiusTexels;
        // Level zero explicitly: the kernel diverges across a quad and a depth
        // target has no mips.
        shadow += 1.0 - sShadowMap.SampleCmpLevelZero(
                            float3(projCoords.xy + offset, float(mapIndex + cascade)), currentDepth);
    }
    return shadow / float(kNumDirectionalPCFSamples);
}

float getDirectionalLightShadow(GlobalUniformsShadowLight slot, float3 viewPos,
                                float3 worldPos, float3 normal) {
    float3 lightVector = -normalize(slot.positionOrDirection.xyz);
    int cascade = cascadeFor(viewPos);
    float shadow = shadowInCascade(slot.mapIndex, slot.mapResolution, cascade, worldPos,
                                   normal, lightVector);

    // Cross-fade into the next cascade: the two disagree at the split, and a
    // hard switch draws a line across the ground.
    if (cascade + 1 < kNumShadowCascades) {
        float depth = abs(viewPos.z);
        float far = globalUniforms.shadowCascadeFarPlanes[cascade];
        float near = cascade > 0 ? globalUniforms.shadowCascadeFarPlanes[cascade - 1] : 0.0;
        float t = saturate((depth - near) / max(far - near, 1e-4));
        float blend = smoothstep(1.0 - kCascadeBlendBand, 1.0, t);
        if (blend > 0.0) {
            float next = shadowInCascade(slot.mapIndex, slot.mapResolution, cascade + 1,
                                         worldPos, normal, lightVector);
            shadow = lerp(shadow, next, blend);
        }
    }
    return shadow;
}

static const float3 kPCFOffsets[kNumPCFSamples] = {
    float3(1, 1, 1), float3(1, -1, 1), float3(-1, -1, 1), float3(-1, 1, 1),
    float3(1, 1, -1), float3(1, -1, -1), float3(-1, -1, -1), float3(-1, 1, -1),
    float3(1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0), float3(-1, 1, 0),
    float3(1, 0, 1), float3(-1, 0, 1), float3(1, 0, -1), float3(-1, 0, -1),
    float3(0, 1, 1), float3(0, -1, 1), float3(0, -1, -1), float3(0, 1, -1)};

float getPointLightShadow(GlobalUniformsShadowLight slot, float3 worldPos, float3 normal) {
    float3 toLight = slot.positionOrDirection.xyz - worldPos;
    float rawDistance = max(1e-4, length(toLight));
    float3 lightVector = toLight / rawDistance;
    float NdotL = dot(normal, lightVector);
    float sinAngle = sqrt(max(0.0, 1.0 - NdotL * NdotL));

    // One cube texel at this range spans 2*distance/resolution; the resolution
    // comes from the uniform, since GetDimensions misreports this sampler.
    float texelWorld = 2.0 * rawDistance / max(1.0, float(slot.mapResolution));

    float3 offsetWorldPos =
        worldPos + normal * sinAngle * kPointNormalOffsetTexels * texelWorld;
    float3 fragToLight = offsetWorldPos - slot.positionOrDirection.xyz;
    float currentDepth = length(fragToLight);

    // Explicit level: compute has no implicit derivatives. The cube holds
    // distance over the far plane, so the reference is divided, not the store.
    float referenceDepth =
        max(0.0, currentDepth - kPointDepthBiasTexels * texelWorld) / kPointShadowFar;
    float shadow = 0.0;
    for (int i = 0; i < kNumPCFSamples; ++i) {
        shadow += 1.0 - sShadowMapCube.SampleCmpLevelZero(
                            float4(fragToLight +
                                       kPCFSampleRadiusTexels * texelWorld * kPCFOffsets[i],
                                   float(slot.mapIndex)),
                            referenceDepth);
    }
    shadow /= float(kNumPCFSamples);
    // No fade with range: the light reaches radius squared, and where it is
    // too far to matter its own attenuation is already ~0.
    return shadow;
}

/** The side of a thin surface that faces the light: a leaf lit from behind is still lit. */
public float3 thinFacingNormal(float3 normal, float3 toLight) {
    return dot(normal, toLight) < 0.0 ? -normal : normal;
}

// Public because thinFacingNormal above is: marking one declaration in a Slang
// module switches the rest from implicit visibility to internal, so the first
// export turns every other entry point into a compile error at its caller.
public float getShadowFrom(int slotIndex, float3 viewPos, float3 worldPos, float3 normal,
                           bool thin = false) {
    if (slotIndex < 0 || slotIndex >= globalUniforms.numShadowLights) {
        return 1.0;
    }
    GlobalUniformsShadowLight slot = globalUniforms.shadowLights[slotIndex];
    float3 lightVector = slot.positionOrDirection.w == 0.0
                             ? -normalize(slot.positionOrDirection.xyz)
                             : normalize(slot.positionOrDirection.xyz - worldPos);
    // Sampled from the side the light is on, or a blade lit from behind is in
    // its own shadow.
    float3 facing = thin ? thinFacingNormal(normal, lightVector) : normal;
    // Ahead of the lookup: a surface facing away fades to lit, so its taps
    // would only be multiplied out again.
    float normalFade = saturate(dot(facing, lightVector) * 10.0);
    if (normalFade <= 0.0) {
        return 1.0;
    }
    float shadow = slot.positionOrDirection.w == 0.0
                       ? getDirectionalLightShadow(slot, viewPos, worldPos, facing)
                       : getPointLightShadow(slot, worldPos, facing);
    float visibility = 1.0 - shadow * slot.strength;
    return lerp(1.0, visibility, normalFade);
}

/** The frame's first caster, for a consumer with one visibility over the whole lit product. */
public float getShadow(float3 viewPos, float3 worldPos, float3 normal, bool thin = false) {
    return getShadowFrom(0, viewPos, worldPos, normal, thin);
}
