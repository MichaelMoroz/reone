// Deferred version of the retained retro material in pbr_model.slang.
// Geometry has already published the surface into the G-buffer; this pass
// reconstructs the fragment position and applies the original light/fog model.

import uniforms;
import lib.gbuf;
import lib.shadow;
import lib.scene_schema;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;
[[vk::binding(21, 1), vk::image_format("r16ui")]] Sampler2D<uint> sGBufMaterialId;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(5, 2)]] SamplerCube bindlessTextureCubes[];

static const uint kNoMaterial = 0xffffu;
static const uint kNoTexture = 0xffffffffu;

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

[shader("fragment")]
float4 retroResolveFragment(Varyings input) : SV_Target0 {
    float4 diffuseSample = sGBufDiffuse.Sample(input.uv);
    float4 lightmapSample = sGBufLightmap.Sample(input.uv);
    float4 selfIllumSample = sGBufSelfIllum.Sample(input.uv);
    uint materialId = sGBufMaterialId.Load(int3(int2(input.position.xy), 0)).r;

    // The material-id clear and an uncovered alpha-tested pixel both carry this
    // sentinel. sky_composite.slang paints over these black pixels afterwards,
    // testing far depth rather than the sentinel - the two sets coincide,
    // since the material id and the depth are written by one fragment.
    if (materialId == kNoMaterial) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    InstanceMaterial material = instanceMaterials[materialId];

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
    float3 materialAmbient = material.ambientColor.rgb;
    float3 materialDiffuse = material.diffuseColor.rgb;

    float3 ambient = float3(0.0);
    float3 direct = float3(0.0);
    if (!hasLightmap && !staticObject) {
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
            direct += light.multiplier *
                      max(0.0, dot(normal, lightVector / max(0.0001, distance))) *
                      attenuation * materialDiffuse * light.color.rgb;
        }
    }

    float shadowFactor = 1.0;
    if (shadowsEnabled) {
        shadowFactor = getShadow(eyePos, worldPos, normal);
    }
    float3 color;
    if (hasLightmap) {
        // Every reference engine - xoreos, KotOR.js, and the original's own
        // captured GL state - multiplies the lightmap against the diffuse
        // texel at full strength, never clamped against the dynamic sum, with
        // lighting replaced by the bake rather than added to it. Dynamic
        // (flicker) lights join after the product, and self-illum is an
        // additive term the albedo never modulates - the original drew it as a
        // separate additive blend pass.
        //
        // The shadow term multiplies that whole product, which is a deliberate
        // departure from "never shadow the lightmap" and only defensible
        // because of what casts in this mode. Retro's shadow map holds
        // characters and nothing else, so a factor below one here means a
        // character is standing between this texel and the light - never that
        // the terrain occluded itself, which is the case the rule was written
        // against. The original drew exactly this as a stencil pass that
        // darkened the ground beneath a character, at the module's authored
        // ShadowOpacity; getShadow already folds that strength in. Attenuating
        // only the dynamic term instead would put no shadow at all under a
        // character on baked ground, which is where the original always had
        // one.
        color = shadowFactor *
                    (lightmapSample.rgb + ambient + max(float3(0.0), direct)) *
                    diffuseSample.rgb +
                selfIllumSample.rgb;
    } else if (staticObject) {
        // Static geometry without a lightmap is the unlit backdrop (far
        // hills, sky shells). xoreos renders it full-bright - the generated
        // shader for a non-lightmapped surface is the diffuse texel with no
        // lighting terms at all - and the original's GL path is the same
        // draw with lighting disabled.
        color = diffuseSample.rgb + selfIllumSample.rgb;
    } else {
        // Dynamic geometry: the shadow term takes the direct sum and leaves
        // the flat ambient alone. Ambient here is materialAmbient against the
        // world ambient colour plus any ambient-only lights - a constant
        // standing in for unsimulated light, not something one light's depth
        // test has any claim on.
        color = min(float3(1.0),
                    ambient + shadowFactor * max(float3(0.0), direct) +
                        selfIllumSample.rgb) *
                diffuseSample.rgb;
    }
    if (envmapped) {
        float3 eyeReflection = reflect(normalize(eyePos), eyeNormal);
        // Odyssey generated eye-space reflection coordinates per vertex from
        // the geometric normal. A deferred resolve necessarily does this per
        // pixel from the normal-mapped, quantized G-buffer normal instead.
        //
        // Unshadowed: the authored mirror is a static capture of the surround,
        // and the original applied no shadow to it either.
        color += sampleAuthoredMirror(material, eyeReflection) *
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
