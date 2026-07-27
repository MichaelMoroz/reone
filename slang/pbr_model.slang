// Opaque model, PBR pipeline - rewritten against the architecture in
// doc/vulkan-rt-backend.md section 9, replacing the transliteration in
// pbr_opaque_model.slang.
//
// The geometry paths are separate entry points over shared transform functions
// rather than branches on a feature flag, because each becomes its own pipeline
// and because a ray tracer needs them resolved into real geometry before tracing.
// Material features remain runtime branches - they are uniform across a draw.

import uniforms;
import lib.geometry;
import lib.math;
import lib.gbuf;
import lib.hashedalpha;
import lib.motion;
import lib.oit;
import lib.shadow;
import lib.surface;

[[vk::binding(0, 1)]] Sampler2D sMainTex;
[[vk::binding(1, 1)]] Sampler2D sLightmap;
[[vk::binding(2, 1)]] Sampler2D sEnvMap;
[[vk::binding(3, 1)]] Sampler2D sNormalMap;
[[vk::binding(18, 1)]] SamplerCube sEnvMapCube;
[[vk::binding(14, 1)]] Sampler2DArray sBumpMapArray;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv1 : TEXCOORD0;
    [[vk::location(3)]] float2 uv2 : TEXCOORD1;
    [[vk::location(4)]] float3 bitangent : BINORMAL;
    [[vk::location(5)]] float3 tangent : TANGENT;
    [[vk::location(6)]] float3 tanSpaceNormal : TEXCOORD2;
    [[vk::location(7)]] float4 boneIndices : BLENDINDICES;
    [[vk::location(8)]] float4 boneWeights : BLENDWEIGHT;
    uint vertexId : SV_VertexID;
};

/** Owned by this program alone - nothing else is paired with these stages. */
struct Varyings {
    float4 position : SV_Position;
    float4 objectPos : TEXCOORD0;
    float4 worldPos : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float2 uv1 : TEXCOORD3;
    float2 uv2 : TEXCOORD4;
    float3 tangent : TEXCOORD5;
    float3 bitangent : TEXCOORD6;
    float3 tanSpaceNormal : TEXCOORD7;
    float4 curClipPos : TEXCOORD8;
    float4 prevClipPos : TEXCOORD9;
};

/** Everything after the object-space position is settled is common. */
Varyings finishVertex(VertexInput input, float4 objectPos, float4 prevObjectPos, float3 objectNormal) {
    Varyings output;
    output.objectPos = objectPos;
    output.worldPos = mul(localUniforms.model, objectPos);
    output.worldNormal = normalize(transformNormal(objectNormal));
    output.uv1 = input.uv1;
    output.uv2 = input.uv2;

    if (isFeatureEnabled(kFeatureNormalMap) || isFeatureEnabled(kFeatureBumpMap)) {
        output.tangent = normalize(transformNormal(input.tangent));
        output.bitangent = normalize(transformNormal(input.bitangent));
        output.tanSpaceNormal = normalize(transformNormal(input.tanSpaceNormal));
    } else {
        output.tangent = float3(0.0);
        output.bitangent = float3(0.0);
        output.tanSpaceNormal = float3(0.0);
    }

    output.curClipPos = mul(globalUniforms.viewProjection, output.worldPos);
    output.prevClipPos = mul(globalUniforms.prevViewProjection,
                             mul(localUniforms.prevModel, prevObjectPos));
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, output.worldPos));
    return output;
}

[shader("vertex")]
Varyings staticVertex(VertexInput input) {
    float4 P = transformStatic(input.position);
    return finishVertex(input, P, P, input.normal);
}

[shader("vertex")]
Varyings skinnedVertex(VertexInput input) {
    VertexSkin skin;
    skin.indices = input.boneIndices;
    skin.weights = input.boneWeights;

    float4 P = skinPosition(float4(input.position, 1.0), skin, false);
    float4 prevP = skinPosition(float4(input.position, 1.0), skin, true);
    float3 N = skinPosition(float4(input.normal, 0.0), skin, false).xyz;
    return finishVertex(input, P, prevP, N);
}

[shader("vertex")]
Varyings danglyVertex(VertexInput input) {
    // Deformation is simulated on the CPU, so the previous position is not
    // tracked - motion vectors here carry the rigid movement only.
    float4 P = danglyPosition(input.vertexId);
    return finishVertex(input, P, P, input.normal);
}

[shader("vertex")]
Varyings saberVertex(VertexInput input) {
    float4 P = saberPosition(input.position, input.vertexId);
    return finishVertex(input, P, P, input.normal);
}

struct GBufferOutput {
    float4 diffuseColor : SV_Target0;
    float4 eyeNormal : SV_Target1;
    float4 lightmapColor : SV_Target2;
    float4 selfIllumColor : SV_Target3;
    float2 motion : SV_Target4;
};

float2 transformedUV(float2 uv1) {
    float3 h = float3(uv1, 1.0);
    return float2(
        localUniforms.uv[0].x * h.x + localUniforms.uv[1].x * h.y + localUniforms.uv[2].x * h.z,
        localUniforms.uv[0].y * h.x + localUniforms.uv[1].y * h.y + localUniforms.uv[2].y * h.z);
}

/**
 * Builds the surface description from the Odyssey texture slots. This is the
 * function that moves into the material buffer of section 5.6 - a path tracer
 * will fill SurfaceParams from a hit record instead, and everything downstream
 * is unchanged.
 */
SurfaceParams sampleSurface(Varyings input, float2 uv) {
    SurfaceParams surface = defaultSurface();

    float4 mainTexSample = sMainTex.Sample(uv);
    surface.albedo = mainTexSample.rgb;
    surface.alpha = mainTexSample.a;

    if (isFeatureEnabled(kFeatureNormalMap)) {
        TangentFrame frame = {input.tangent, input.bitangent, input.tanSpaceNormal};
        float3 n = sNormalMap.Sample(uv).rgb * 2.0 - 1.0;
        surface.normal = tangentToWorld(frame, normalize(n));
    } else if (isFeatureEnabled(kFeatureBumpMap)) {
        TangentFrame frame = {input.tangent, input.bitangent, input.tanSpaceNormal};
        float height = sBumpMapArray.Sample(float3(uv, localUniforms.bumpMapFrame)).r;
        float dX = localUniforms.bumpMapScale * ddx(height);
        float dY = localUniforms.bumpMapScale * ddy(height);
        surface.normal = tangentToWorld(frame, normalize(float3(-dX, -dY, 1.0)));
    } else {
        surface.normal = normalize(input.worldNormal);
    }

    if (isFeatureEnabled(kFeatureLightmap)) {
        surface.lightmap = sLightmap.Sample(input.uv2).rgb;
        surface.hasLightmap = true;
    }

    surface.emissive = localUniforms.selfIllumColor.rgb;
    return surface;
}

/**
 * Reflection from either shape of environment map, ported from i_envmap.glsl.
 *
 * A cube map is sampled by direction. A sphere map is the same reflection
 * flattened to equirectangular uv - note the negated direction, which is what
 * makes the two agree.
 */
float4 sampleEnvMap(float3 R) {
    if (isFeatureEnabled(kFeatureEnvMapCube)) {
        return sEnvMapCube.Sample(R);
    }
    float3 d = normalize(-R);
    float2 uv = float2(0.5 + atan2(d.x, d.z) / (2.0 * kPi),
                       0.5 - asin(d.y) / kPi);
    return sEnvMap.Sample(uv);
}

/**
 * Transparent models, ported from glsl/f_oit_model.glsl.
 *
 * Forward rather than deferred: a transparent surface has no single depth at
 * which to resolve lighting, so it is shaded here rather than through the
 * G-buffer. There is no PBR lighting in either backend - a transparent surface
 * takes its light from the lightmap alone, or none at all.
 *
 * Returns colour with the coverage the surface ends up with, which the two
 * entry points below either write straight out or split across the OIT pair.
 */
float4 shadeTransparent(Varyings input) {
    float2 uv = transformedUV(input.uv1);
    SurfaceParams surface = sampleSurface(input, uv);

    float3 diffuseColor = surface.albedo;
    float diffuseAlpha = surface.alpha;
    if (isFeatureEnabled(kFeaturePremulAlpha)) {
        // Colour already multiplied by coverage, with no usable alpha channel.
        diffuseAlpha = rgbToLuma(diffuseColor);
        diffuseColor *= 1.0 / max(0.0001, diffuseAlpha);
    }

    // An environment-mapped surface takes its opacity from the material alone:
    // the texture's alpha is what lets the reflection through, so folding it in
    // here as well would make the surface vanish where it should be shiniest.
    float objectAlpha = localUniforms.color.a;
    if (!isFeatureEnabled(kFeatureEnvMap)) {
        objectAlpha *= diffuseAlpha;
    }
    if (objectAlpha == 0.0) {
        discard;
    }

    float3 lighting = float3(1.0);
    if (isFeatureEnabled(kFeatureLightmap)) {
        lighting = surface.lightmap;
        if (isFeatureEnabled(kFeatureWater)) {
            // Water is barely lightmapped; mostly it carries its own colour.
            lighting = lerp(float3(1.0), lighting, 0.2);
        }
    }

    float3 color = lighting * localUniforms.color.rgb * diffuseColor;
    if (isFeatureEnabled(kFeatureEnvMap)) {
        float3 I = normalize(input.worldPos.xyz - globalUniforms.cameraPosition.xyz);
        float3 R = reflect(I, surface.normal);
        color += sampleEnvMap(R).rgb * (1.0 - diffuseAlpha);
    }
    if (isFeatureEnabled(kFeatureWater)) {
        color *= localUniforms.waterAlpha;
    }
    return float4(color, objectAlpha);
}

/**
 * Weighted-blended order-independent transparency, the pipeline's usual path.
 *
 * The counterpart of glsl/f_oit_model.glsl: the same shading, accumulated into
 * the two OIT targets instead of being blended onto the image in draw order.
 */
[shader("fragment")]
OITOutput oitModelFragment(Varyings input) {
    float4 shaded = shadeTransparent(input);
    return packOIT(shaded.rgb, shaded.a, input.position.z);
}

/**
 * The same surface blended straight onto the resolved image.
 *
 * What a pass with one colour attachment and no OIT targets uses. Order
 * dependent, so it is only right where nothing overlaps.
 */
[shader("fragment")]
float4 transparentFragment(Varyings input) {
    return shadeTransparent(input);
}

[shader("fragment")]
GBufferOutput opaqueFragment(Varyings input) {
    float2 uv = transformedUV(input.uv1);
    SurfaceParams surface = sampleSurface(input, uv);

    if (!isFeatureEnabled(kFeatureEnvMap)) {
        if (isFeatureEnabled(kFeatureHashedAlphaTest)) {
            if (hashedAlphaTestFails(surface.alpha, input.objectPos.xyz)) {
                discard;
            }
        } else if (surface.alpha == 0.0) {
            discard;
        }
    }

    float3 albedo = surface.albedo;
    float alpha = surface.alpha;
    if (isFeatureEnabled(kFeatureWater)) {
        albedo *= localUniforms.waterAlpha;
        alpha *= localUniforms.waterAlpha;
    }

    float features = packGeometryFeatures(
        isFeatureEnabled(kFeatureEnvMap),
        isFeatureEnabled(kFeatureShadows),
        isFeatureEnabled(kFeatureFog));

    // Row-vector product stands in for transpose(mat3(uViewInv)).
    float3 eyeNormal = mul(surface.normal, (float3x3)globalUniforms.viewInv);
    eyeNormal = 0.5 * eyeNormal + 0.5;

    GBufferOutput output;
    output.diffuseColor = float4(albedo, alpha);
    output.lightmapColor = float4(surface.lightmap, features);
    output.selfIllumColor = float4(surface.emissive, localUniforms.envMapDerivedLayer / 255.0);
    output.eyeNormal = float4(eyeNormal, 0.0);
    output.motion = computeMotion(input.curClipPos, input.prevClipPos);
    return output;
}

struct RetroOutput {
    float4 color : SV_Target0;
    float4 hilights : SV_Target1;
};

float lightAttenuationQuadratic(GlobalUniformsLight light, float distance) {
    float d = light.radius + distance;
    return (light.radius * light.radius) / (d * d);
}

/** Forward lighting used by the original renderer, retained separately from PBR resolve. */
[shader("fragment")]
RetroOutput retroOpaqueFragment(Varyings input) {
    float2 uv = transformedUV(input.uv1);
    SurfaceParams surface = sampleSurface(input, uv);
    if (!isFeatureEnabled(kFeatureEnvMap)) {
        if (isFeatureEnabled(kFeatureHashedAlphaTest)) {
            if (hashedAlphaTestFails(surface.alpha, input.objectPos.xyz)) discard;
        } else if (surface.alpha == 0.0) discard;
    }

    float3 ambient = float3(0.0);
    float3 diffuse = localUniforms.selfIllumColor.rgb;
    if (isFeatureEnabled(kFeatureLightmap)) {
        diffuse += surface.lightmap;
    } else if (!isFeatureEnabled(kFeatureStatic)) {
        ambient += localUniforms.ambientColor.rgb * globalUniforms.worldAmbientColor.rgb;
    }
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];
        if (isFeatureEnabled(kFeatureStatic) && light.dynamicType != 2) continue;
        float3 lightVector = light.position.xyz - input.worldPos.xyz;
        float distance = length(lightVector);
        if (distance > light.radius * light.radius) continue;
        float attenuation = lightAttenuationQuadratic(light, distance);
        if (light.ambientOnly != 0) {
            ambient += light.multiplier * attenuation * localUniforms.ambientColor.rgb * light.color.rgb;
        } else {
            diffuse += light.multiplier * max(0.0, dot(surface.normal, lightVector / max(0.0001, distance))) * attenuation * localUniforms.diffuseColor.rgb * light.color.rgb;
        }
    }
    float shadow = isFeatureEnabled(kFeatureShadows)
                       ? getShadow(mul(globalUniforms.view, input.worldPos).xyz,
                                   input.worldPos.xyz)
                       : 0.0;
    float3 color = min(float3(1.0),
                       ambient + (1.0 - shadow) * max(float3(0.0), diffuse)) *
                   surface.albedo;
    if (isFeatureEnabled(kFeatureEnvMap)) {
        float3 viewDir = normalize(globalUniforms.cameraPosition.xyz - input.worldPos.xyz);
        color += sampleEnvMap(reflect(-viewDir, surface.normal)).rgb * (1.0 - surface.alpha);
    }
    if (isFeatureEnabled(kFeatureFog)) {
        float d = length(input.worldPos.xyz - globalUniforms.cameraPosition.xyz);
        float fog = 1.0 - saturate((globalUniforms.fogFar - d) /
                                   max(0.0001, globalUniforms.fogFar - globalUniforms.fogNear));
        color = lerp(color, globalUniforms.fogColor.rgb, fog);
    }
    RetroOutput output;
    output.color = float4(color, 1.0);
    output.hilights = float4(step(float3(0.0001), localUniforms.selfIllumColor.rgb) * step(float3(0.95), color) * color, 1.0);
    return output;
}
