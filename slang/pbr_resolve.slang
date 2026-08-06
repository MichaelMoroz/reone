// The deferred resolve: read the G-buffer, light once per pixel, write the
// scene output. The Vulkan counterpart of glsl/f_pbr_combine.glsl.
//
// Ported faithfully for everything the Vulkan pipeline can currently supply:
// the scene light list, the GGX/Smith/Fresnel BRDF, world ambient, gamma
// handling, the geometry feature bits packed into the lightmap alpha, and fog.
//
// The important part is the shape rather than the lighting. SurfaceParams is
// filled here from a G-buffer, and a path tracer will fill the same struct from
// a hit record; everything downstream of it is shared. Keep that boundary.

import uniforms;
import lib.math;
import lib.gbuf;
import lib.shadow;
import lib.scene_schema;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(13, 1)]] Sampler2D sBRDFLUT;
[[vk::binding(16, 1)]] SamplerCubeArray sIrradianceMapArray;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;
[[vk::binding(21, 1), vk::image_format("r16ui")]] Sampler2D<uint> sGBufMaterialId;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(5, 2)]] SamplerCube bindlessTextureCubes[];

static const uint kNoMaterial = 0xffffu;
static const uint kNoTexture = 0xffffffffu;

/** Roughness spans this many prefiltered mips; see VulkanPBRTextures. */
static const float kMaxReflectionLOD = 4.0;

static const float kGamma = 2.2;

/**
 * A surface's shading parameters at a point, independent of how the point was
 * reached. The raster resolve reaches it through a G-buffer, a path tracer will
 * reach it through a hit record; neither the producer nor the BSDF needs to know
 * which.
 *
 * Odyssey assets do not carry these directly - they carry diffuse, lightmap,
 * envmap and bump slots plus ambient/diffuse/self-illum colours - so something
 * has to map between the two. That mapping wants to move into the material
 * buffer of section 5.6, computed once per material rather than per fragment.
 * Keeping it behind this struct means that move changes where SurfaceParams
 * comes from and nothing that consumes it.
 */
struct SurfaceParams {
    float3 albedo;
    float alpha;
    float3 normal;    // world space
    float3 emissive;
    float roughness;
    float metallic;
    /** Baked irradiance, or 1 where a surface carries no lightmap. */
    float3 lightmap;
    bool hasLightmap;
};

SurfaceParams defaultSurface() {
    SurfaceParams s;
    s.albedo = float3(1.0);
    s.alpha = 1.0;
    s.normal = float3(0.0, 0.0, 1.0);
    s.emissive = float3(0.0);
    // Odyssey materials carry no roughness or metallic channel. These are the
    // stand-ins the PBR pipeline already assumes; deriving something better from
    // the asset is a separate question - see section 5.4.
    s.roughness = 1.0;
    s.metallic = 0.0;
    s.lightmap = float3(1.0);
    s.hasLightmap = false;
    return s;
}



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

float lightAttenuationQuadratic(GlobalUniformsLight light, float distance) {
    float d = light.radius + distance;
    return (light.radius * light.radius) / (d * d);
}

float3 sampleAuthoredMirror(InstanceMaterial material, float3 eyeReflection) {
    if (material.envMapCube != kNoTexture) {
        // This fullscreen pass cannot derive a stable cube LOD from its
        // quantized normal. Minified-surface sharpness is a separate policy;
        // the authored mirror deliberately uses its base level here.
        return bindlessTextureCubes[NonUniformResourceIndex(material.envMapCube)]
            .SampleLevel(eyeReflection, 0.0).rgb;
    }
    float3 r = eyeReflection;
    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y +
                         (r.z + 1.0) * (r.z + 1.0));
    float2 uv = float2(r.x / m + 0.5, r.y / m + 0.5);
    return bindlessTextures[NonUniformResourceIndex(material.envMap)]
        .Sample(uv).rgb;
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
float4 resolveFragment(Varyings input) : SV_Target0 {
    float2 uv = input.uv;

    float4 mainTexSample = sGBufDiffuse.Sample(uv);
    float4 lightmapSample = sGBufLightmap.Sample(uv);
    float4 selfIllumSample = sGBufSelfIllum.Sample(uv);
    uint materialId = sGBufMaterialId.Load(int3(int2(input.position.xy), 0)).r;

    // Nothing was drawn here: leave the clear colour alone rather than lighting
    // it, which would tint the whole background.
    if (materialId == kNoMaterial) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    InstanceMaterial material = instanceMaterials[materialId];

    float3 eyePos = reconstructViewPos(uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.Sample(uv).xyz - 1.0);
    float3 worldPos = mul(globalUniforms.viewInv, float4(eyePos, 1.0)).xyz;
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);

    float3 V = normalize(globalUniforms.cameraPosition.xyz - worldPos);

    float3 albedo = gammaToLinear(mainTexSample.rgb);
    float3 emission = gammaToLinear(selfIllumSample.rgb);
    float3 materialAmbient = gammaToLinear(material.ambientColor.rgb);
    float3 materialDiffuse = gammaToLinear(material.diffuseColor.rgb);
    // Odyssey assets carry neither metalness nor roughness. Diffuse alpha is
    // the authored mirror strength, handled separately below exactly as the
    // original renderer did; it is not a second F0 for the IBL lobe.
    float metallic = 0.0;
    // Odyssey assets carry no roughness channel; the diffuse alpha stands in,
    // floored so that nothing turns into a perfect mirror.
    float roughness = clamp(mainTexSample.a, 0.2, 1.0);
    // SSAO is not part of the shared executor yet; its neutral term is one.
    float ao = 1.0;

    bool envmapped;
    bool hasLightmap;
    bool staticObject;
    bool shadowsEnabled;
    bool fogEnabled;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled);

    float shadowFactor = 1.0;
    if (shadowsEnabled) {
        shadowFactor = getShadow(eyePos, worldPos, normal);
    }
    // ShadowOpacity was authored for the original gamma-space renderer. PBR
    // applies lighting in linear space, so convert the display visibility to
    // an energy multiplier before using it on the direct BRDF terms.
    //
    // Direct terms only. It used to scale the ambient/IBL lobes as well, and
    // since irradiance is the LIGHTMAP on lightmapped geometry and the flat
    // materialAmbient * worldAmbient product everywhere else, that made a
    // depth test against one light attenuate baked and ambient light. That is
    // what darkened lightmapped rooms in this mode while retro - whose
    // lightmapped branch never took the shadow term - kept the authored tone.
    float directVisibility = pow(shadowFactor, kGamma);
    float fog = fogEnabled ? getFog(worldPos) : 0.0;

    float NdotV = max(0.0, dot(normal, V));
    float3 F0 = lerp(float3(0.04), albedo, metallic);

    // Odyssey generated eye-space reflection coordinates per vertex from the
    // geometric normal. A deferred resolve necessarily does this per pixel
    // from the normal-mapped, quantized G-buffer normal instead.
    float3 R = reflect(-V, normal);
    float3 eyeR = reflect(normalize(eyePos), eyeNormal);
    int envLayer = int(round(selfIllumSample.a * 255.0));

    float3 environment = float3(0.0);
    // The path tracer deliberately does not add this flat term: outdoors its
    // sky hemisphere integral already is ambient, so adding both doubles it.
    // Interiors consequently lose this small raster-only baseline; direct
    // scene lights are the intentionally shared illumination there.
    // Static geometry with no lightmap authored reads as a white bake, not
    // as an absence of light: whole exteriors ship without lightmaps (the
    // Tatooine dune sea has none at all), and calling that unlit either
    // blackens them or exempts them from shadows. Dynamic objects keep flat
    // world ambient - they were never lightmapped and the light loop shades
    // them. Mirrors the same choice in retro_resolve.slang.
    float3 irradiance = hasLightmap ? gammaToLinear(lightmapSample.rgb)
                        : staticObject
                            ? float3(1.0)
                            : materialAmbient *
                                  gammaToLinear(globalUniforms.worldAmbientColor.rgb);
    if (envmapped) {
        irradiance += gammaToLinear(
            sIrradianceMapArray.Sample(float4(eyeR, float(envLayer))).rgb);
        environment = gammaToLinear(
            sPrefilteredEnvMapArray.SampleLevel(float4(eyeR, float(envLayer)),
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
        // The original excluded all but dynamicType 1 from static geometry,
        // because a lightmap already held that light and the hardware could
        // not have afforded it twice. Retro keeps that rule; the corrected
        // model keeps it only for lights that cast nothing, for one concrete
        // reason: a shadow is visible only on the light it attenuates, and
        // the shadow-casting light is exactly what this test dropped. Static
        // geometry therefore took no direct light at all, and a shadow on it
        // had nothing to darken. Letting that one light through double-counts
        // the bake's own share of it, which is bounded by the lightmap's
        // weight and is the honest price of shadows that are not painted onto
        // baked light.
        if (staticObject && light.dynamicType != 1 && light.shadowCaster == 0) {
            continue;
        }

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
            irradiance += materialAmbient * radiance;
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
        directD += materialDiffuse * kD / kPi * radiance * NdotL;
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
        // Diffuse alpha partitions the environment response: (1 - alpha)
        // belongs to the authored mirror below, while IBL fills the remaining
        // alpha share with roughness-varying specular. The two lobes therefore
        // complement rather than double-light one another.
        ambientS = mainTexSample.a * environment * (F * brdf.x + brdf.y);
    }

    float3 selfIllumStep = step(float3(1e-4), selfIllumSample.rgb);
    float selfIllumed = max(selfIllumStep.x, max(selfIllumStep.y, selfIllumStep.z));

    // Self-illumination is a coefficient on the light reaching the surface, not
    // a colour added on top of it. Adding it instead saturates anything fully
    // self-illuminated - a sky - to white.
    // Ambient occlusion still weights the ambient lobes; the shadow term does
    // not. AO is a property of the surface's own neighbourhood and applies to
    // light arriving from everywhere, which is exactly what these lobes are.
    // Received exactly as retro receives it: one visibility over the whole lit
    // product, not a term applied to direct light alone. On content whose
    // light budget is mostly a baked lightmap, a shadow that may only touch
    // the dynamic share is a shadow nobody can see - measured at 14/255
    // against 89 for the same scene in retro. What differs between the modes
    // is the caster set, not this expression: retro's map holds characters,
    // this one holds every opaque surface, so terrain shadowing terrain is
    // present here and is geometry, not error. Self-shadowing acne is the
    // failure to watch for, and it belongs to the depth bias and the receiver
    // normal offset in lib/shadow.slang - not to this line.
    float3 color = min(float3(1.0),
                       directVisibility * (ao * ambientD +
                           max(float3(0.0), directD)) + emission) *
                   albedo;
    color += ao * ambientS;
    // A self-illuminated surface does not take a specular highlight.
    color += (1.0 - selfIllumed) * directVisibility * max(float3(0.0), directS);

    color = linearToGamma(color);
    // Authored mirror and roughness-varying IBL are distinct. The IBL above
    // keeps the ordinary dielectric response; this gamma-space add reserves
    // (1 - diffuse alpha) for the original game's sharp mirror term, so that
    // authored strength is not counted in both lobes.
    //
    // Unshadowed, like the other environment lobe. The cube is a static
    // capture of the whole surround, not of the shadow-casting light, so a
    // depth test against that one light says nothing about how much of the
    // cube this surface can see.
    if (envmapped) {
        color += sampleAuthoredMirror(material, eyeR) * (1.0 - mainTexSample.a);
    }
    color = lerp(color, globalUniforms.fogColor.rgb, fog);

    return float4(color, 1.0);
}
