// Deferred version of the retained retro material in pbr_model.slang.
// Geometry has already published the surface into the G-buffer; this pass
// reconstructs the fragment position and applies the original light/fog model.

import uniforms;
import lib.gbuf;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;
[[vk::binding(21, 1)]] Sampler2D sGBufMaterialAmbient;
[[vk::binding(22, 1)]] Sampler2D sGBufMaterialDiffuse;

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
    float4 selfIllumSample = sGBufSelfIllum.Sample(input.uv);

    // The attachment clear and an uncovered alpha-tested pixel both carry this
    // sentinel. Sky will eventually composite over these black pixels.
    if (diffuseSample.a == 0.0 && lightmapSample.a == 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 eyePos = reconstructViewPos(input.uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.Sample(input.uv).xyz - 1.0);
    float3 worldPos = mul(globalUniforms.viewInv, float4(eyePos, 1.0)).xyz;
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);

    bool envmapped;
    bool hasLightmap;
    bool staticObject;
    bool shadowsEnabled;
    bool fogEnabled;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled);
    float3 materialAmbient = sGBufMaterialAmbient.Sample(input.uv).rgb;
    float3 materialDiffuse = sGBufMaterialDiffuse.Sample(input.uv).rgb;

    float3 ambient = float3(0.0);
    float3 lit = selfIllumSample.rgb;
    if (hasLightmap) {
        lit += lightmapSample.rgb;
    } else if (!staticObject) {
        ambient += materialAmbient * globalUniforms.worldAmbientColor.rgb;
    }

    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];

        if (staticObject && light.dynamicType != kLightDynamicTypeAll) continue;

        float3 lightVector = light.position.xyz - worldPos;
        float distance = length(lightVector);
        if (distance > light.radius * light.radius) continue;

        float attenuation = lightAttenuationQuadratic(light, distance);
        if (light.ambientOnly != 0) {
            ambient += light.multiplier * attenuation * materialAmbient * light.color.rgb;
        } else {
            lit += light.multiplier *
                   max(0.0, dot(normal, lightVector / max(0.0001, distance))) *
                   attenuation * materialDiffuse * light.color.rgb;
        }
    }

    // G7 restores shadows. All remaining terms preserve retroOpaqueFragment's
    // ordering, saturation, and gamma-space arithmetic.
    float3 color = min(float3(1.0), ambient + max(float3(0.0), lit)) *
                   diffuseSample.rgb;
    if (envmapped) {
        float3 viewDir = normalize(globalUniforms.cameraPosition.xyz - worldPos);
        float3 reflection = reflect(-viewDir, normal);
        int envLayer = int(round(selfIllumSample.a * 255.0));
        color += sPrefilteredEnvMapArray.SampleLevel(
                     float4(reflection, float(envLayer)), 0.0).rgb *
                 (1.0 - diffuseSample.a);
    }

    if (fogEnabled) {
        float distance = length(worldPos - globalUniforms.cameraPosition.xyz);
        float fog = 1.0 - saturate((globalUniforms.fogFar - distance) /
                                   max(0.0001, globalUniforms.fogFar - globalUniforms.fogNear));
        color = lerp(color, globalUniforms.fogColor.rgb, fog);
    }
    return float4(color, 1.0);
}
