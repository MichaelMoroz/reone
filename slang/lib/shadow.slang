import uniforms;

[[vk::binding(15, 1)]] Sampler2DArray sShadowMap;
[[vk::binding(19, 1)]] SamplerCube sShadowMapCube;

static const int kNumShadowCascades = 4;
static const int kNumPCFSamples = 20;
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

float getDirectionalLightShadow(float3 viewPos, float3 worldPos, float3 normal) {
    int cascade = cascadeFor(viewPos);
    float3 lightVector = -normalize(globalUniforms.shadowLightPosition.xyz);
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

    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float2 offset = float2(x, y) * texelSize;
            float pcfDepth = sShadowMap.Sample(
                float3(projCoords.xy + offset, float(cascade))).r;
            shadow += currentDepth > pcfDepth ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
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
