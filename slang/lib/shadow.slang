import uniforms;

[[vk::binding(15, 1)]] Sampler2DArray sShadowMap;
[[vk::binding(19, 1)]] SamplerCube sShadowMapCube;

static const int kNumShadowCascades = 4;
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

static const float kPCFSampleRadius = 0.1;
static const float kPointShadowFar = 2500.0;
// Receiver normal offset handles grazing surfaces without moving every caster
// away from its contact point as a large constant depth bias does.
static const float kDirectionalNormalOffset = 0.0115;
static const float kPointNormalOffset = 0.005;

struct ShadowResult {
    // Display-space visibility for the directly lit term. Back-facing and
    // grazing surfaces fade to one because their BRDF already rejects the sun.
    float factor;
    // Depth-tested visibility without the NdotL cutoff, for ambient/IBL.
    float iblFactor;
};

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
float shadowInCascade(int cascade, float3 worldPos, float3 normal, float3 lightVector) {
    float NdotL = dot(normal, lightVector);
    float sinAngle = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    float3 offsetWorldPos = worldPos + normal * sinAngle * kDirectionalNormalOffset;
    float4 lightSpacePos = mul(globalUniforms.shadowLightSpace[cascade], float4(offsetWorldPos, 1.0));
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords.xy = 0.5 * projCoords.xy + 0.5;

    float currentDepth = projCoords.z;
    if (currentDepth > 1.0) {
        return 0.0;
    }

    uint width;
    uint height;
    uint layers;
    sShadowMap.GetDimensions(width, height, layers);
    float2 texelSize = 1.0 / float2(width, height);

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
    float4 probe = mul(globalUniforms.shadowLightSpace[cascade],
                       float4(offsetWorldPos + tangent, 1.0));
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
        // Explicit LOD: the kernel diverges across a quad, and there are no
        // mips on a depth target to select anyway.
        float pcfDepth = sShadowMap.SampleLevel(
            float3(projCoords.xy + offset, float(cascade)), 0.0).r;
        shadow += currentDepth > pcfDepth ? 1.0 : 0.0;
    }
    return shadow / float(kNumDirectionalPCFSamples);
}

float getDirectionalLightShadow(float3 viewPos, float3 worldPos, float3 normal) {
    float3 lightVector = -normalize(globalUniforms.shadowLightPosition.xyz);
    int cascade = cascadeFor(viewPos);
    float shadow = shadowInCascade(cascade, worldPos, normal, lightVector);

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
            float next = shadowInCascade(cascade + 1, worldPos, normal, lightVector);
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

float getPointLightShadow(float3 worldPos, float3 normal) {
    float3 lightVector = normalize(globalUniforms.shadowLightPosition.xyz - worldPos);
    float NdotL = dot(normal, lightVector);
    float sinAngle = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    float3 offsetWorldPos = worldPos + normal * sinAngle * kPointNormalOffset;
    float3 fragToLight = offsetWorldPos - globalUniforms.shadowLightPosition.xyz;
    float currentDepth = length(fragToLight);

    float shadow = 0.0;
    for (int i = 0; i < kNumPCFSamples; ++i) {
        float closestDepth = kPointShadowFar *
                             sShadowMapCube.Sample(
                                 fragToLight + kPCFSampleRadius * kPCFOffsets[i]).r;
        shadow += currentDepth > closestDepth ? 1.0 : 0.0;
    }
    shadow /= float(kNumPCFSamples);
    shadow *= 1.0 - smoothstep(globalUniforms.shadowRadius,
                               2.0 * globalUniforms.shadowRadius, currentDepth);
    return shadow;
}

ShadowResult getShadow(float3 viewPos, float3 worldPos, float3 normal) {
    float shadow = globalUniforms.shadowLightPosition.w == 0.0
                       ? getDirectionalLightShadow(viewPos, worldPos, normal)
                       : getPointLightShadow(worldPos, normal);
    float visibility = 1.0 - shadow * globalUniforms.shadowStrength;
    float3 lightVector = globalUniforms.shadowLightPosition.w == 0.0
                             ? -normalize(globalUniforms.shadowLightPosition.xyz)
                             : normalize(globalUniforms.shadowLightPosition.xyz - worldPos);
    float normalFade = saturate(dot(normal, lightVector) * 10.0);

    ShadowResult result;
    result.factor = lerp(1.0, visibility, normalFade);
    result.iblFactor = visibility;
    return result;
}
