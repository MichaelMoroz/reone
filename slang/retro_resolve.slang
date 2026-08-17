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
// Deferred version of the retained retro material in pbr_model.slang.
// Geometry has already published the surface into the G-buffer; this pass
// reconstructs the fragment position and applies the original light/fog model.

import uniforms;
import lib.gbuf;
import lib.shadow;
import lib.scene_schema;
import lib.skycube;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;
[[vk::binding(21, 1), vk::image_format("r32ui")]] Sampler2D<uint> sGBufTriangleId;
// The merged triangle-to-material table. The G-buffer stores the triangle
// because the tracer needs the geometry behind it; the material follows from
// the triangle, so there is no material-id target for this pass to read.
[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(5, 2)]] SamplerCube bindlessTextureCubes[];
// Set 3 is the resolve set: this frame's storage output, which only the compute
// PBR resolve writes, and the baked sky cube, which both resolves read. Retro
// stays a fragment pass and therefore leaves binding 0 alone.
[[vk::binding(1, 3)]] SamplerCube sSkyCube;

/** Mirrors ResolvePushConstants in scenepipeline.cpp. */
struct ResolvePushConstants {
    /** Kept in step with pbr_resolve.slang; retro reads only the flags. */
    /** Bit 0: sky. Bit 1 is PBR's SSAO. Bit 2 preserves uncovered GUI alpha. */
    uint flags;
    float thinTransmission;
};
[[vk::push_constant]] ResolvePushConstants pushConstants;

static const uint kResolveFlagSky = 1u;
static const uint kResolveFlagTransparentOutput = 4u;
static const uint kResolveFlagDisplayReferred = 8u;

static const uint kNoTriangle = 0xffffffffu;
static const uint kNoTexture = 0xffffffffu;

/**
 * The exponent this pass converts its finished colour back through.
 *
 * Held locally, as pbr_resolve.slang holds its own, because it must be the same
 * number the post-process encode inverts - kPtGamma in tracing/brdf.slang - and
 * a mismatch between the two would show up as a global contrast shift rather
 * than as anything that names itself.
 */
static const float kGamma = 2.2;

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
    uint triangleId = sGBufTriangleId.Load(int3(int2(input.position.xy), 0)).r;

    // The triangle-id clear and an uncovered alpha-tested pixel both carry this
    // sentinel, and nothing wrote depth there either - the triangle id and the
    // depth come from one fragment - so this is exactly the set the sky
    // composite pass used to paint over. It is now the sky's own branch.
    // Without a bake the cube is black by construction, so skipping it writes
    // the same black rather than a different one.
    if (triangleId == kNoTriangle) {
        if ((pushConstants.flags & kResolveFlagSky) == 0) {
            float alpha = (pushConstants.flags & kResolveFlagTransparentOutput) != 0 ? 0.0 : 1.0;
            return float4(0.0, 0.0, 0.0, alpha);
        }
        float3 direction = skyDirection(input.uv, globalUniforms.projectionInv,
                                        globalUniforms.viewInv,
                                        globalUniforms.cameraPosition.xyz);
        // The bake stores LINEAR radiance, so what this pass owes the target
        // depends on which space the target is in. Linear: hand it over at
        // identity and let the post-process encode reproduce the authored
        // skybox texel. Display-referred, which is retro's chain: encode it
        // here, because nothing downstream will. Getting this wrong is not
        // subtle - it presents the sky two stops dark and takes every exterior
        // with it, which is how it was found.
        float3 sky = skyRadiance(sSkyCube, direction, 1.0);
        if ((pushConstants.flags & kResolveFlagDisplayReferred) != 0) {
            sky = pow(max(sky, 0.0), float3(1.0 / kGamma));
        }
        return float4(sky, 1.0);
    }
    InstanceMaterial material = instanceMaterials[mergedMaterialIds[triangleId]];

    float3 eyePos = reconstructViewPos(input.uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.Sample(input.uv).xyz - 1.0);
    float3 worldPos = mul(globalUniforms.viewInv, float4(eyePos, 1.0)).xyz;
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);

    bool envmapped;
    bool hasLightmap;
    bool staticObject;
    bool shadowsEnabled;
    bool fogEnabled, thinSurface;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled, thinSurface);
    // Retro takes the shadow-through, so a cutout is not lit as though it were
    // solid, but not the two-sided diffuse: its lighting model is the
    // reference, and brightening the far side of every leaf would be an
    // improvement to it rather than a correction of it.
    float3 materialAmbient = material.ambientColor.rgb;
    float3 materialDiffuse = material.diffuseColor.rgb;

    // Two accumulators, exactly as the reference keeps them: `ambient` is
    // never shadowed, `diffuse` is. Self-illum seeds the shadowed one, so the
    // albedo multiplies it and the clamp bounds it - it is not a separate
    // additive term. The lightmap adds into the same accumulator, and world
    // ambient is the alternative to a lightmap rather than an addition to it.
    float3 ambient = float3(0.0);
    float3 diffuse = selfIllumSample.rgb;
    if (hasLightmap) {
        diffuse += lightmapSample.rgb;
    } else if (staticObject) {
        // A white bake, which is the one place this expression departs from the
        // reference's, and it departs deliberately.
        //
        // Static geometry is excluded from every dynamic light below because a
        // lightmap is presumed to hold that light already. Where no lightmap
        // was authored the presumption is simply false, and the reference's
        // arithmetic then resolves to black: no lightmap, no ambient, no light
        // that passes the static gate. Dantooine's far hills are exactly that
        // case - `hillitop` carries no lightmap and no self-illumination - and
        // they render at 4% of their albedo here against 90% in the reference
        // build, which reaches that number through geometry this branch does
        // not draw.
        //
        // Reading the absence as a white bake keeps the authored texel and
        // still takes the shadow term and the clamp, which is what an
        // unlightmapped surface looked like in the original. Restored from
        // 92ababccf, which had it before c91c916c7 rewrote this expression to
        // match the reference term for term and dropped it with the rest.
        diffuse += float3(1.0);
    } else {
        ambient += materialAmbient * globalUniforms.worldAmbientColor.rgb;
    }

    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];

        // The dynamic-type rule keeps a light out of static geometry
        // because the lightmap already holds it. With no lightmap there is
        // nothing to double-count, so the rule has no subject and the
        // surface takes the light - which is also what lets a shadow land on
        // unlightmapped ground.
        if (staticObject && light.dynamicType != kLightDynamicTypeAll) continue;

        float3 lightVector = light.position.xyz - worldPos;
        float distance = length(lightVector);
        if (distance > light.radius * light.radius) continue;

        float attenuation = lightAttenuationQuadratic(light, distance);
        if (light.ambientOnly != 0) {
            ambient += light.multiplier * attenuation * materialAmbient * light.color.rgb;
        } else {
            diffuse += light.multiplier *
                       max(0.0, dot(normal, lightVector / max(0.0001, distance))) *
                       attenuation * materialDiffuse * light.color.rgb;
        }
    }

    float shadowFactor = 1.0;
    if (shadowsEnabled) {
        shadowFactor = getShadow(eyePos, worldPos, normal, thinSurface);
    }
    // The reference's expression, term for term:
    //
    //   min(vec3(1.0), (ambient + (1.0 - shadow) * max(vec3(0.0), diffuse)))
    //       * mainTexSample.rgb
    //
    // Three things this branch previously did differently, all of them
    // visible: the sum had no upper bound, the shadow term multiplied the
    // ambient share as well as the diffuse one, and self-illum was added
    // after the albedo multiply instead of before it. `shadowFactor` here is
    // already the light-through fraction, so it stands where (1.0 - shadow)
    // does there. The clamp precedes the envmap and fog terms in the
    // reference too, so what leaves this function is still unbounded.
    float3 color = min(float3(1.0),
                       ambient + shadowFactor * max(float3(0.0), diffuse)) *
                   diffuseSample.rgb;

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
    // Everything above is deliberately the original's gamma-space arithmetic,
    // down to the fog lerp and the radius-squared cull, and none of it is
    // linearised: that model IS the fidelity target of this mode.
    //
    // So the result is handed over as it was computed, and the post-process
    // encode is a pass-through for this mode. It used to be converted to linear
    // here and converted back there, on the reasoning that the pair cancel and
    // the pipeline in between could then be uniformly linear. They do cancel -
    // for the two ENDPOINTS. Every stage that reads or writes the target
    // between them was quietly given a different image than the original had:
    // the alpha blend, the bloom threshold and the bloom blur all produce
    // different numbers in the two spaces, and the original did all three in
    // display space on an 8-bit framebuffer. PBR and the tracer keep the linear
    // chain, where the round trip does not cancel and linear is the right space
    // to work in.
    if ((pushConstants.flags & kResolveFlagDisplayReferred) != 0) {
        return float4(max(color, 0.0), 1.0);
    }
    return float4(pow(max(color, 0.0), float3(kGamma)), 1.0);
}
