// The deferred resolve: read the G-buffer, light once per pixel, write the
// scene output. The Vulkan counterpart of glsl/f_pbr_combine.glsl.
//
// Ported faithfully for everything the Vulkan pipeline can currently supply:
// the scene light list, the GGX/Smith/Fresnel BRDF, world ambient, gamma
// handling, the geometry feature bits packed into the lightmap alpha, and fog.
//
// Stubbed, because the pipeline has no counterpart yet - each written so that
// the term drops out rather than being approximated:
//   - SSAO and SSR. Ambient occlusion is one.
//
// The important part is the shape rather than the lighting. SurfaceParams is
// filled here from a G-buffer, and a path tracer will fill the same struct from
// a hit record; everything downstream of it is shared. Keep that boundary.

import uniforms;
import lib.math;
import lib.shadow;
import lib.surface;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(13, 1)]] Sampler2D sBRDFLUT;
[[vk::binding(16, 1)]] SamplerCubeArray sIrradianceMapArray;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;

/** Roughness spans this many prefiltered mips; see VulkanPBRTextures. */
static const float kMaxReflectionLOD = 4.0;

static const float kGamma = 2.2;


// Packed into the lightmap alpha by the geometry pass; see lib/gbuf.slang.
static const int kGeometryFeatureEnvMap = 1 << 0;
static const int kGeometryFeatureShadows = 1 << 1;
static const int kGeometryFeatureFog = 1 << 2;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings resolveVertex(uint vertexId: SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

float3 gammaToLinear(float3 rgb) {
    return pow(rgb, float3(kGamma));
}

float3 linearToGamma(float3 rgb) {
    return pow(rgb, float3(1.0 / kGamma));
}

/**
 * View-space position of the pixel, from the depth the geometry pass left.
 *
 * Two Vulkan-specific corrections, both silent if wrong - the picture stays
 * plausible and only the lighting is misplaced:
 *
 * Depth is already the 0..1 that clip space uses here, so it goes into NDC
 * unchanged. OpenGL's -1..1 conversion would halve the reconstructed range.
 *
 * NDC y is negated. The geometry pass rasterises through a negative-height
 * viewport, which is what lets an OpenGL projection produce an upright image;
 * that flip has to be undone to recover the clip-space y a fragment really had.
 */
float3 reconstructViewPos(float2 uv) {
    float depth = sGBufDepth.Sample(uv).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}

void unpackGeometryFeatures(float packed, out bool envmapped,
                            out bool shadowsEnabled, out bool fogEnabled) {
    int mask = int(round(packed * 255.0));
    envmapped = (mask & kGeometryFeatureEnvMap) != 0;
    shadowsEnabled = (mask & kGeometryFeatureShadows) != 0;
    fogEnabled = (mask & kGeometryFeatureFog) != 0;
}

float lightAttenuationQuadratic(GlobalUniformsLight light, float distance) {
    float d = light.radius + distance;
    return (light.radius * light.radius) / (d * d);
}

float distributionGGX(float NdotH2, float a2) {
    float d = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float geometrySchlick(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotL, float NdotV, float k) {
    return geometrySchlick(NdotL, k) * geometrySchlick(NdotV, k);
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    float3 ceiling = max(F0, float3(1.0 - roughness));
    return F0 + (ceiling - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float getFog(float3 worldPos) {
    float distance = length(worldPos - globalUniforms.cameraPosition.xyz);
    float f = (globalUniforms.fogFar - distance) /
              (globalUniforms.fogFar - globalUniforms.fogNear);
    return 1.0 - saturate(f);
}

[shader("fragment")]
float4 resolveFragment(Varyings input) : SV_Target {
    float2 uv = input.uv;

    float4 mainTexSample = sGBufDiffuse.Sample(uv);
    float4 lightmapSample = sGBufLightmap.Sample(uv);
    float4 selfIllumSample = sGBufSelfIllum.Sample(uv);

    // Nothing was drawn here: leave the clear colour alone rather than lighting
    // it, which would tint the whole background.
    if (mainTexSample.a == 0.0 && lightmapSample.a == 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 eyePos = reconstructViewPos(uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.Sample(uv).xyz - 1.0);
    float3 worldPos = mul(globalUniforms.viewInv, float4(eyePos, 1.0)).xyz;
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);

    float3 V = normalize(globalUniforms.cameraPosition.xyz - worldPos);

    float3 albedo = gammaToLinear(mainTexSample.rgb);
    float3 emission = gammaToLinear(selfIllumSample.rgb);
    // Odyssey assets carry neither metalness nor roughness. What an
    // environment-mapped surface does carry is diffuse alpha, which the retro
    // renderer uses as a reflection strength: it adds the environment at
    // (1 - alpha), see glsl/f_rtr_opaqmodel.glsl. Deferred PBR read that alpha
    // as roughness and dropped the strength entirely, so every reflection came
    // back weighted by a dielectric's 4% - a droid whose alpha averages 0.7
    // reflected about a fifth of what the original renderer gave it.
    //
    // Feeding the strength into F0 restores the magnitude without a second
    // environment term: alpha 0 reflects like a mirror, alpha 1 like any other
    // dielectric. Reflectance rather than metalness, so the reflection stays
    // untinted by albedo and the diffuse lobe is not switched off, which is
    // what the retro path does and what metalness would not.
    float metallic = 0.0;
    // Odyssey assets carry no roughness channel; the diffuse alpha stands in,
    // floored so that nothing turns into a perfect mirror.
    float roughness = clamp(mainTexSample.a, 0.2, 1.0);
    float ao = 1.0;

    bool envmapped;
    bool shadowsEnabled;
    bool fogEnabled;
    unpackGeometryFeatures(lightmapSample.a, envmapped, shadowsEnabled, fogEnabled);

    // A surface with no lightmap carries (1,1,1), which reads as no shadow.
    float shadow = 0.0;
    if (shadowsEnabled) {
        shadow = getShadow(eyePos, worldPos);
        shadow = max(shadow, 1.0 - rgbToLuma(lightmapSample.rgb));
    }
    float fog = fogEnabled ? getFog(worldPos) : 0.0;

    float NdotV = max(0.0, dot(normal, V));
    // Gated on envmapped: on everything else the diffuse alpha is an opacity
    // or an alpha-test threshold and means nothing about reflectance.
    float envMapStrength = envmapped ? (1.0 - mainTexSample.a) : 0.0;
    float3 F0 = lerp(lerp(float3(0.04), albedo, metallic), float3(1.0), envMapStrength);

    // The reflection direction is what both derived maps are indexed by.
    float3 R = reflect(-V, normal);
    int envLayer = int(round(selfIllumSample.a * 255.0));

    float3 environment = float3(0.0);
    float3 irradiance = gammaToLinear(globalUniforms.worldAmbientColor.rgb);
    if (envmapped) {
        irradiance += gammaToLinear(sIrradianceMapArray.Sample(float4(R, float(envLayer))).rgb);
        environment = gammaToLinear(
            sPrefilteredEnvMapArray.SampleLevel(float4(R, float(envLayer)),
                                                roughness * kMaxReflectionLOD).rgb);
    }
    float3 directD = float3(0.0);
    float3 directS = float3(0.0);

    float a = roughness * roughness;
    float a2 = a * a;
    float k = roughness + 1.0;
    k = k * k * (1.0 / 8.0);

    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];

        float3 fragToLight = light.position.xyz - worldPos;
        float lightDistance = length(fragToLight);
        // Kept exactly as the OpenGL shader has it, radius-squared cutoff and
        // all, so that any difference between the two is not this.
        if (lightDistance > light.radius * light.radius) {
            continue;
        }

        float attenuation = lightAttenuationQuadratic(light, lightDistance);
        float3 radiance = light.multiplier * attenuation * gammaToLinear(light.color.rgb);
        if (light.ambientOnly != 0) {
            irradiance += radiance;
            continue;
        }

        float3 L = normalize(fragToLight);
        float3 H = normalize(V + L);
        float NdotL = max(0.0, dot(normal, L));
        float NdotH = max(0.0, dot(normal, H));
        float VdotH = max(0.0, dot(V, H));

        float D = distributionGGX(NdotH * NdotH, a2);
        float G = geometrySmith(NdotL, NdotV, k);
        float3 F = fresnelSchlick(VdotH, F0);
        float3 spec = (D * G * F) / max(1e-4, 4.0 * NdotL * NdotV);

        float3 kD = (1.0 - F) * (1.0 - metallic);
        directD += kD / kPi * radiance * NdotL;
        directS += spec * radiance * NdotL;
    }

    float3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 ambientD = kD * irradiance;

    // Ambient specular: the environment blurred to this surface's roughness,
    // weighted by the split-sum approximation's scale and bias.
    float3 ambientS = float3(0.0);
    if (envmapped) {
        float2 brdf = sBRDFLUT.Sample(float2(NdotV, roughness)).xy;
        ambientS = environment * (F * brdf.x + brdf.y);
    }

    float3 selfIllumStep = step(float3(1e-4), selfIllumSample.rgb);
    float selfIllumed = max(selfIllumStep.x, max(selfIllumStep.y, selfIllumStep.z));

    // Self-illumination is a coefficient on the light reaching the surface, not
    // a colour added on top of it. Adding it instead saturates anything fully
    // self-illuminated - a sky - to white.
    float3 color = min(float3(1.0),
                       ao * ambientD +
                           (1.0 - shadow) * (max(float3(0.0), directD) + emission)) *
                   albedo;
    color += ao * ambientS;
    // A self-illuminated surface does not take a specular highlight.
    color += (1.0 - selfIllumed) * (1.0 - shadow) * max(float3(0.0), directS);

    color = linearToGamma(color);
    color = lerp(color, globalUniforms.fogColor.rgb, fog);
    return float4(color, 1.0);
}
