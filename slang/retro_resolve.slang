// Deferred version of the retained retro material in pbr_model.slang.
// Geometry has already published the surface into the G-buffer; this pass
// reconstructs the fragment position and applies the original light/fog model.

import uniforms;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;

static const int kGeometryFeatureFog = 1 << 2;
static const int kLightDynamicTypeAll = 1;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings retroResolveVertex(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

// This is the same Vulkan reconstruction as pbr_resolve.slang: device depth
// remains in [0,1], and y undoes the negative-height geometry viewport.
float3 reconstructViewPos(float2 uv) {
    float depth = sGBufDepth.Sample(uv).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}

float lightAttenuationQuadratic(GlobalUniformsLight light, float distance) {
    float d = light.radius + distance;
    return (light.radius * light.radius) / (d * d);
}

[shader("fragment")]
float4 retroResolveFragment(Varyings input) : SV_Target0 {
    float4 diffuseSample = sGBufDiffuse.Sample(input.uv);
    float4 lightmapSample = sGBufLightmap.Sample(input.uv);

    // The attachment clear and an uncovered alpha-tested pixel both carry this
    // sentinel. Sky will eventually composite over these black pixels.
    if (diffuseSample.a == 0.0 && lightmapSample.a == 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 eyePos = reconstructViewPos(input.uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.Sample(input.uv).xyz - 1.0);
    float3 worldPos = mul(globalUniforms.viewInv, float4(eyePos, 1.0)).xyz;
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);

    // No-lightmap surfaces publish the old contract's neutral white value.
    // The contract has no explicit presence bit, so an exactly-white lightmap
    // texel is indistinguishable and takes this neutral-material fallback.
    bool hasLightmap = any(lightmapSample.rgb < float3(254.5 / 255.0));

    float3 ambient = float3(0.0);
    float3 lit = sGBufSelfIllum.Sample(input.uv).rgb;
    if (hasLightmap) {
        lit += lightmapSample.rgb;
    } else {
        // ambientColor is per-object and absent from the G-buffer. Its authored
        // default is white, which is the least destructive deferred substitute.
        ambient += globalUniforms.worldAmbientColor.rgb;
    }

    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];

        // Static identity is not deferred either. Lightmapped geometry is the
        // closest available proxy for the old static-object gate.
        if (hasLightmap && light.dynamicType != kLightDynamicTypeAll) continue;

        float3 lightVector = light.position.xyz - worldPos;
        float distance = length(lightVector);
        if (distance > light.radius * light.radius) continue;

        float attenuation = lightAttenuationQuadratic(light, distance);
        if (light.ambientOnly != 0) {
            // ambientColor's neutral substitute is white.
            ambient += light.multiplier * attenuation * light.color.rgb;
        } else {
            // diffuseColor's neutral substitute is white.
            lit += light.multiplier *
                   max(0.0, dot(normal, lightVector / max(0.0001, distance))) *
                   attenuation * light.color.rgb;
        }
    }

    // G7 restores shadows. All remaining terms preserve retroOpaqueFragment's
    // ordering, saturation, and gamma-space arithmetic.
    float3 color = min(float3(1.0), ambient + max(float3(0.0), lit)) *
                   diffuseSample.rgb;

    int featureMask = int(round(lightmapSample.a * 255.0));
    if ((featureMask & kGeometryFeatureFog) != 0) {
        float distance = length(worldPos - globalUniforms.cameraPosition.xyz);
        float fog = 1.0 - saturate((globalUniforms.fogFar - distance) /
                                   max(0.0001, globalUniforms.fogFar - globalUniforms.fogNear));
        color = lerp(color, globalUniforms.fogColor.rgb, fog);
    }
    return float4(color, 1.0);
}
