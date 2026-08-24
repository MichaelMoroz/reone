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

// Comparison samplers. The filtering unit performs the depth test itself and
// returns the filtered fraction that passed, so each tap below is already a
// 2x2 percentage-closer filter rather than a single binary answer. Both must
// be declared this way together: a comparison sampler cannot be read with a
// plain sample, nor a plain sampler with SampleCmp.
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

/**
 * Point kernel and bias, both in TEXELS of the cube face rather than in world
 * units, for the same reason the directional offset is: one face spans ninety
 * degrees, so a texel covers 2*distance/resolution of world at whatever range
 * the receiver happens to be. A fixed world offset is enormous next to the
 * light and sub-texel far from it, and neither end filters or biases.
 */
static const float kPCFSampleRadiusTexels = 1.5;
static const float kPointNormalOffsetTexels = 2.5;
/**
 * Depth bias, in texels of world distance at the receiver.
 *
 * This is the whole of the point path's bias, and it has to be: the shadow
 * fragment writes SV_Depth to store radial distance, and a shader-replaced
 * depth does not receive the pipeline's depth bias - so depthBiasConstantFactor
 * and depthBiasSlopeFactor, which the directional pass relies on, reach this
 * one not at all. Without it every lit surface self-shadows on roughly half
 * its taps, which reads as a uniform half-darkening rather than as acne and
 * was measured at 48% of a frame sitting near 0.5 visibility.
 */
static const float kPointDepthBiasTexels = 3.0;
static const float kPointShadowFar = 2500.0;
// Receiver normal offset handles grazing surfaces without moving every caster
// away from its contact point as a large constant depth bias does.
//
// In texels, not world units. A cascade's texel covers a world area that grows
// with its far plane - the outermost spans some fifty times what the innermost
// does - so one world-space offset cannot serve all four: sized for the near
// cascade it leaves distant receivers inside their own texel and they stipple
// themselves, and sized for the far one it detaches every near contact shadow.
// Scaling by the cascade's own texel diagonal makes the offset mean the same
// thing everywhere.
static const float kDirectionalNormalOffsetTexels = 2.5;

// One visibility value, and it means one thing: how much of the DIRECT light
// from the shadow-casting light reaches this surface. It is never applied to
// lightmap irradiance, to flat world ambient, or to an environment reflection.
//
// A shadow map answers "is this point occluded from that one light". Baked
// irradiance already contains its own occlusion, and flat ambient is a constant
// standing in for everything the renderer does not simulate; attenuating either
// by a depth test against a single light darkens surfaces for a reason that has
// nothing to do with them. That deviation is what made lightmapped geometry go
// dark under the shadow term, and it is why the second, NdotL-free factor this
// struct used to carry no longer exists - every consumer of it was a term that
// should not have been shadowed at all.

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
float shadowInCascade(int mapIndex, int cascade, float3 worldPos, float3 normal, float3 lightVector) {
    float NdotL = dot(normal, lightVector);
    float sinAngle = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    // The cascade's world units per texel, read off its own matrix: an
    // orthographic light-space transform scales the world extent it covers
    // into [-1,1], so the extent is 2 / the row's length, and a texel is that
    // over the map's resolution.
    uint width;
    uint height;
    uint layers;
    sShadowMap.GetDimensions(width, height, layers);
    float2 texelSize = 1.0 / float2(width, height);
    float4x4 cascadeSpace = globalUniforms.shadowCascadeSpace[mapIndex + cascade];
    float3 lightSpaceX = float3(cascadeSpace[0][0], cascadeSpace[1][0], cascadeSpace[2][0]);
    float texelWorldSize = 2.0 / (max(1e-6, length(lightSpaceX)) * float(width));
    float3 offsetWorldPos =
        worldPos + normal * sinAngle * kDirectionalNormalOffsetTexels * texelWorldSize;
    float4 lightSpacePos = mul(cascadeSpace, float4(offsetWorldPos, 1.0));
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords.xy = 0.5 * projCoords.xy + 0.5;

    float currentDepth = projCoords.z;
    if (currentDepth > 1.0) {
        return 0.0;
    }

    // Hold the penumbra to a fixed width in WORLD units rather than in texels.
    // The cascade divisors step 0.005 / 0.015 / 0.045, so each cascade covers
    // three times the ground of the one before and its texels are three times
    // wider. A radius fixed in texels therefore triples in world width at every
    // split, and that discontinuity is what makes the transition obvious.
    // Probing the matrix - rather than reconstructing the extent from the
    // divisors - keeps this correct whatever the split scheme becomes.
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

    // A rotated Poisson disk instead of a 3x3 box. The box could only ever
    // produce ten discrete penumbra levels one texel wide, which reads as a
    // stair-stepped edge; the disk spreads the same idea over a wider radius
    // and the per-receiver rotation turns the remaining banding into noise.
    //
    // The rotation is keyed on world position, not screen position. A
    // screen-space key re-rolls every pixel as the camera moves, which is
    // exactly the per-frame edge crawl the jitter fix removed - a stable
    // world-space key gives each surface point the same kernel every frame.
    float angle = frac(sin(dot(floor(worldPos * 32.0).xy, float2(12.9898, 78.233))) *
                       43758.5453) * 6.2831853;
    float2 rot = float2(cos(angle), sin(angle));

    float shadow = 0.0;
    for (int i = 0; i < kNumDirectionalPCFSamples; ++i) {
        float2 disk = kPoissonDisk[i];
        float2 rotated = float2(disk.x * rot.x - disk.y * rot.y,
                                disk.x * rot.y + disk.y * rot.x);
        float2 offset = rotated * texelSize * radiusTexels;
        // The comparison is the sampler's, and it returns the fraction of the
        // 2x2 neighbourhood the receiver is behind rather than a binary answer.
        // Each disk tap therefore carries four texels of evidence, which is why
        // the same sample count reads smoother than the hand-rolled test it
        // replaces. Level zero explicitly: the kernel diverges across a quad,
        // and a depth target has no mip chain to select anyway.
        shadow += 1.0 - sShadowMap.SampleCmpLevelZero(
                            float3(projCoords.xy + offset, float(mapIndex + cascade)), currentDepth);
    }
    return shadow / float(kNumDirectionalPCFSamples);
}

float getDirectionalLightShadow(GlobalUniformsShadowLight slot, float3 viewPos,
                                float3 worldPos, float3 normal) {
    float3 lightVector = -normalize(slot.positionOrDirection.xyz);
    int cascade = cascadeFor(viewPos);
    float shadow = shadowInCascade(slot.mapIndex, cascade, worldPos, normal, lightVector);

    // Cross-fade into the next cascade over the tail of this one. Even with the
    // penumbra held constant the two disagree at the split - they carry
    // different depth precision, and their texel grids are snapped
    // independently - so a hard switch draws a line across the ground at a
    // fixed distance from the camera. The band makes that line a gradient.
    if (cascade + 1 < kNumShadowCascades) {
        float depth = abs(viewPos.z);
        float far = globalUniforms.shadowCascadeFarPlanes[cascade];
        float near = cascade > 0 ? globalUniforms.shadowCascadeFarPlanes[cascade - 1] : 0.0;
        float t = saturate((depth - near) / max(far - near, 1e-4));
        float blend = smoothstep(1.0 - kCascadeBlendBand, 1.0, t);
        if (blend > 0.0) {
            float next = shadowInCascade(slot.mapIndex, cascade + 1, worldPos, normal, lightVector);
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

    // World size of one cube texel at this receiver's range. A face is a
    // 90-degree frustum, so its half-extent equals the distance and one texel
    // spans 2*distance/resolution.
    //
    // The resolution comes from the uniform, not from GetDimensions: that query
    // does not report the face width for this sampler, and the size it did
    // report made the bias clamp the depth reference to zero and the kernel
    // sample at random - which looks like a working shadow right up until you
    // change the bias and nothing moves.
    float texelWorld = 2.0 * rawDistance / max(1.0, float(slot.mapResolution));

    float3 offsetWorldPos =
        worldPos + normal * sinAngle * kPointNormalOffsetTexels * texelWorld;
    float3 fragToLight = offsetWorldPos - slot.positionOrDirection.xyz;
    float currentDepth = length(fragToLight);

    // Explicit level, like every other tap here. The PBR resolve is a compute
    // dispatch and compute has no implicit derivatives, so an implicit-LOD
    // sample makes Slang emit SPV_KHR_compute_shader_derivatives - a device
    // feature this engine does not request, which the validation layers
    // report and which is undefined behaviour where it is not supported.
    // The map has no mip chain anyway, so level zero is what the implicit
    // form resolved to.
    // The cube map holds distance normalised by the far plane, so the reference
    // is divided rather than the stored value multiplied - the comparison
    // happens inside the sampler and cannot scale what it reads.
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
    // No fade with range. There used to be one, smoothstep(radius, 2*radius),
    // and it did not match the reach of the light it belongs to: both resolves
    // admit a point light out to radius SQUARED - the Odyssey cull, 400 units
    // for a lamp of radius 20 - while the shadow was gone by 40. Between those
    // the light arrived with no occlusion at all and shone through walls,
    // which is what made point shadows look absent in a module while a fixture
    // whose receiver sits well inside the radius looked correct.
    //
    // Nothing is needed in its place. Where the light is too far to matter its
    // own attenuation is already ~0, so occluding it changes nothing, and the
    // cube's far plane covers far more than the cull does.
    return shadow;
}

/**
 * Display-space visibility of the shadow-casting light, at the authored
 * ShadowOpacity strength.
 *
 * Back-facing and grazing surfaces fade to one: their own NdotL already
 * rejects that light, so shadowing them again would darken a surface twice
 * for the same geometric fact.
 */
/**
 * The side of a thin surface that faces the light.
 *
 * A leaf lit from behind is still lit; treating it as a solid sends the whole
 * back face black. Flipping the normal is the two-sided half of that, and the
 * caller pays for it with a transmission factor rather than getting the front
 * face's brightness for free.
 */
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
    // A thin surface is sampled from the side the light is on, which is the
    // whole of "the shadow passes through to the other side": the receiver
    // offset then pushes toward the light instead of away from it, and the
    // fade below stops treating a back-facing leaf as something that cannot be
    // lit. Without this a blade lit from behind is in its own shadow.
    float3 facing = thin ? thinFacingNormal(normal, lightVector) : normal;
    float shadow = slot.positionOrDirection.w == 0.0
                       ? getDirectionalLightShadow(slot, viewPos, worldPos, facing)
                       : getPointLightShadow(slot, worldPos, facing);
    float visibility = 1.0 - shadow * slot.strength;
    float normalFade = saturate(dot(facing, lightVector) * 10.0);
    return lerp(1.0, visibility, normalFade);
}

/**
 * The frame's first caster, which is the only one a single-shadow consumer can
 * mean. Kept while the resolves still apply one visibility over the whole lit
 * product; a consumer that shadows per light calls getShadowFrom with that
 * light's own slot instead.
 */
public float getShadow(float3 viewPos, float3 worldPos, float3 normal, bool thin = false) {
    return getShadowFrom(0, viewPos, worldPos, normal, thin);
}
