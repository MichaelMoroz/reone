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
// Canonical merged-scene rasterization. The index buffer supplies the original
// vertex id, while all attributes and material state come from set 2.

import uniforms;
import lib.gbuf;
import lib.math;
import lib.scene_schema;

// --- Merged-geometry addressing, shared by both variants ---
// Shared declarations for raster passes over the canonical merged geometry.
// Both the G-buffer and shadow mega-draws use the same material gate so a
// punch-through surface cannot disagree about coverage between the two.


static const uint kNoTexture = 0xffffffffu;
/** Authored colour is display-referred; the blended pass converts on the way
    out, because the scene target it draws into is linear. Must match the
    exponent the post-process encode inverts. */
static const float kGamma = 2.2;
static const uint kMegaFeaturePremulAlpha = 1u << 12;
static const uint kMegaFeaturePunchThrough = 1u << 26;
static const float kMegaAlphaTestThreshold = 0.5;

[[vk::binding(0, 2)]] StructuredBuffer<MergedVertex> mergedVertices;
[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(4, 2)]] Sampler2DArray bindlessTextureArrays[];
[[vk::binding(5, 2)]] SamplerCube bindlessTextureCubes[];

// materialGated selects which coverage a pass publishes over the same range.
// Ungated is the opaque range. Cutout is the punch-through gate the G-buffer
// and both shadow passes share. Blended is G8's forward pass, which draws the
// surfaces the G-buffer deliberately leaves out.
static const uint kMegaCoverageUngated = 0u;
static const uint kMegaCoverageCutout = 1u;
static const uint kMegaCoverageBlended = 2u;

// The object category the admission path packs into every material record;
// mirrors scene::ModelUsage and the shift in scene/render/admission.cpp.
static const uint kMegaCategoryShift = 27;
static const uint kMegaCategoryMask = 0xFu;
/** Every category casts. The shadow pass compares against this to skip the
    per-fragment material fetch entirely when nothing is being filtered. */
static const uint kMegaCastersAll = 0xFFFFFFFFu;

struct PushConstants {
    uint triangleBase;
    uint materialGated;
    /**
     * Bit per object category allowed to write the shadow map, from
     * kMegaCategoryShift of the material's feature mask.
     *
     * The shadow pass is the only stage that pushes this third word; the
     * G-buffer and blended passes push two and never read it. Adding a fourth
     * would need the pipeline layout's fragment range widened past the three
     * words every graphics pipeline here declares.
     */
    uint casterCategories;
};
[[vk::push_constant]] PushConstants pushConstants;

float2 transformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}

// Keep the cutout thresholds in lockstep with candidateAlpha and
// kAlphaTestThreshold in tracing/trace.slang. That agreement is now load
// bearing rather than a nicety: the tracer reads its primary surface out of
// this G-buffer, so if the two disagreed about which texels are there, the
// traced mode would shade a surface no ray could have found.
//
// The gated draw publishes CUTOUT COVERAGE ONLY: blended and additive surfaces
// contribute nothing until G8.
bool commitsTracedCoverage(InstanceMaterial material, float2 uv, float vertexAlpha) {
    if (pushConstants.materialGated == kMegaCoverageUngated) return true;

    if ((material.featureMask & kMegaFeaturePunchThrough) == 0) return false;

    float alpha = 1.0;
    if (material.mainTex != kNoTexture) {
        float4 texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)]
                           .SampleLevel(uv, 0.0);
        alpha = (material.featureMask & kMegaFeaturePremulAlpha) != 0
                    ? rgbToLuma(texel.rgb)
                    : texel.a;
        alpha *= vertexAlpha;
    }
    return alpha >= kMegaAlphaTestThreshold;
}

// --- The depth-only variant: the same merged geometry, written to a
// shadow map instead of a G-buffer ---
// Shadow map rendering from canonical merged geometry. Unlike shadow.slang,
// this stage has no per-mesh vertex input or LocalUniforms model matrix: the
// merge has already produced world-space positions and one indexed buffer.


static const float kPointShadowFar = 2500.0;
// Point shadows replace raster depth with radial distance. Pipeline depth bias
// is therefore replaced along with it, so reproduce the same constant+slope
// model in radial-depth units. Receiver normal offset now handles most grazing
// cases, leaving only a small write-side slope term here.
static const float kPointShadowConstantBias = 0.0;
static const float kPointShadowSlopeBias = 1.0;

struct DirectionalShadowVaryings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float vertexAlpha : TEXCOORD1;
};

[shader("vertex")]
DirectionalShadowVaryings directionalShadowVertex(
    uint vertexId : SV_VertexID, uint viewId : SV_ViewID) {
    MergedVertex vertex = mergedVertices[vertexId];
    DirectionalShadowVaryings output;
    output.position = mul(globalUniforms.shadowLightSpace[viewId],
                          float4(vertex.position, 1.0));
    output.uv = vertex.uv1;
    output.vertexAlpha = vertex.color.a;
    return output;
}

/**
 * The two gates a caster has to pass: which objects this mode lets cast at
 * all, and - on the gated range - whether the texel is there.
 *
 * The caster set is a category filter rather than a separate submission
 * because the merge produces one buffer per frame ordered by coverage, not by
 * object kind; splitting it by category would rebuild that ordering on the CPU
 * every frame to save a fragment-shader test that costs nothing when nothing
 * is filtered. Whole-category filtering also means the range stays contiguous,
 * so the draw count does not depend on how many creatures are in the room.
 */
void applyShadowGates(float2 rawUv, float vertexAlpha, uint primitiveId) {
    // Nothing filtered and no coverage gate: the common case reads no memory.
    if (pushConstants.casterCategories == kMegaCastersAll &&
        pushConstants.materialGated == kMegaCoverageUngated) {
        return;
    }
    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];

    uint category = (material.featureMask >> kMegaCategoryShift) & kMegaCategoryMask;
    if ((pushConstants.casterCategories & (1u << category)) == 0) discard;

    if (pushConstants.materialGated == kMegaCoverageUngated) return;
    float2 uv = transformedUV(material, rawUv);
    if (!commitsTracedCoverage(material, uv, vertexAlpha)) discard;
}

[shader("fragment")]
void directionalShadowFragment(DirectionalShadowVaryings input,
                                       uint primitiveId : SV_PrimitiveID) {
    applyShadowGates(input.uv, input.vertexAlpha, primitiveId);
}

struct PointShadowVaryings {
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float vertexAlpha : TEXCOORD2;
};

[shader("vertex")]
PointShadowVaryings pointShadowVertex(
    uint vertexId : SV_VertexID, uint viewId : SV_ViewID) {
    MergedVertex vertex = mergedVertices[vertexId];
    PointShadowVaryings output;
    output.worldPos = vertex.position;
    output.position = mul(globalUniforms.shadowLightSpace[viewId],
                          float4(vertex.position, 1.0));
    output.uv = vertex.uv1;
    output.vertexAlpha = vertex.color.a;
    return output;
}

[shader("fragment")]
float pointShadowFragment(PointShadowVaryings input,
                                  uint primitiveId : SV_PrimitiveID) : SV_Depth {
    applyShadowGates(input.uv, input.vertexAlpha, primitiveId);
    float lightDistance = length(input.worldPos -
                                 globalUniforms.shadowLightPosition.xyz);
    float radialDepth = lightDistance / kPointShadowFar;
    float radialSlope = max(abs(ddx(radialDepth)), abs(ddy(radialDepth)));
    return min(1.0, radialDepth + kPointShadowConstantBias +
                        kPointShadowSlopeBias * radialSlope);
}

static const uint kMegaFeatureLightmap = 1u << 0;
static const uint kMegaFeatureEnvMap = 1u << 1;
static const uint kMegaFeatureNormalMap = 1u << 2;
static const uint kMegaFeatureBumpMap = 1u << 3;
static const uint kMegaFeatureShadows = 1u << 7;
static const uint kMegaFeatureFog = 1u << 9;
static const uint kMegaFeatureWater = 1u << 8;
static const uint kMegaFeatureEnvMapCube = 1u << 13;
static const uint kMegaFeatureStatic = 1u << 14;
static const uint kMegaFeatureBlendedCoverage = 1u << 25;
static const uint kMegaSurfaceUnlitTransparent = 1;
static const float kMegaAlphaTestEpsilon = 0.01;

struct Varyings {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float3 tangent : TEXCOORD1;
    float3 bitangent : TEXCOORD2;
    float3 tanSpaceNormal : TEXCOORD3;
    float2 uv1 : TEXCOORD4;
    float2 uv2 : TEXCOORD5;
    float4 color : TEXCOORD6;
    float4 curClipPos : TEXCOORD7;
    float4 prevClipPos : TEXCOORD8;
    float3 worldPos : TEXCOORD9;
};

[shader("vertex")]
Varyings sceneDrawVertex(uint vertexId : SV_VertexID) {
    MergedVertex vertex = mergedVertices[vertexId];
    Varyings output;
    output.curClipPos = mul(globalUniforms.viewProjection, float4(vertex.position, 1.0));
    output.prevClipPos = mul(globalUniforms.prevViewProjection,
                             float4(vertex.prevPosition, 1.0));
    // Rasterise on the jittered grid. globalUniforms.projection carries the
    // sub-pixel offset and the deferred resolves invert it to reconstruct
    // position from depth, so depth must be written through the same matrix -
    // writing it unjittered and inverting it jittered wobbles every
    // reconstructed world position by the Halton offset, which the shadow
    // lookup turns into a per-frame binary flip along every shadow edge.
    // curClipPos/prevClipPos stay unjittered, per uniforms.slang, so motion
    // vectors never encode the offset.
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, float4(vertex.position, 1.0)));
    output.normal = vertex.normal;
    output.tangent = vertex.tangent;
    output.bitangent = vertex.bitangent;
    output.tanSpaceNormal = vertex.tanSpaceNormal;
    output.uv1 = vertex.uv1;
    output.uv2 = vertex.uv2;
    output.color = vertex.color;
    output.worldPos = vertex.position;
    return output;
}

struct GBufferOutput {
    float4 diffuseColor : SV_Target0;
    float4 eyeNormal : SV_Target1;
    float4 lightmapColor : SV_Target2;
    float4 selfIllumColor : SV_Target3;
    float2 motion : SV_Target4;
    /**
     * The merged-geometry triangle, R32Uint, cleared to 0xffffffff.
     *
     * This is the whole surface record rather than a decoded field of it: a
     * consumer expands it back into the three vertices and so recovers the
     * GEOMETRIC normal - which no interpolated, normal-mapped stored normal can
     * stand in for - along with the material, through mergedMaterialIds. It
     * replaced a separate material-id target for exactly that reason.
     */
    uint triangleId : SV_Target5;
};

float3 safeNormal(float3 value, float3 fallback) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-10 ? value * rsqrt(lengthSquared) : fallback;
}

float3 megaMaterialNormal(InstanceMaterial material, Varyings input, float2 uv,
                          bool faceForward, bool isFrontFace) {
    float3 normal = safeNormal(input.normal, float3(0.0, 0.0, 1.0));
    float3 tangent = safeNormal(input.tangent, float3(1.0, 0.0, 0.0));
    float3 bitangent = safeNormal(input.bitangent, float3(0.0, 1.0, 0.0));
    float3 frameNormal = safeNormal(input.tanSpaceNormal, normal);
    if ((material.featureMask & kMegaFeatureNormalMap) != 0 &&
        material.normalMap != kNoTexture) {
        float3 n = bindlessTextures[NonUniformResourceIndex(material.normalMap)]
                       .Sample(uv).rgb * 2.0 - 1.0;
        normal = safeNormal(tangent * n.x + bitangent * n.y + frameNormal * n.z,
                            normal);
    } else if ((material.featureMask & kMegaFeatureBumpMap) != 0 &&
               material.bumpMapArray != kNoTexture) {
        float layer = float(material.bumpMapFrame);
        float height = bindlessTextureArrays[
                           NonUniformResourceIndex(material.bumpMapArray)]
                           .Sample(float3(uv, layer)).r;
        float dX = material.bumpMapScale * ddx(height);
        float dY = material.bumpMapScale * ddy(height);
        float3 n = safeNormal(float3(-dX, -dY, 1.0),
                              float3(0.0, 0.0, 1.0));
        normal = safeNormal(tangent * n.x + bitangent * n.y + frameNormal * n.z,
                            normal);
    }
    if (faceForward && !isFrontFace) normal = -normal;
    return normal;
}

float3 sampleMegaEnvMap(InstanceMaterial material, float3 eyeReflection) {
    if ((material.featureMask & kMegaFeatureEnvMapCube) != 0 &&
        material.envMapCube != kNoTexture) {
        return bindlessTextureCubes[NonUniformResourceIndex(material.envMapCube)]
            .Sample(eyeReflection).rgb;
    }
    if (material.envMap == kNoTexture) return float3(0.0);
    float m = 2.0 * sqrt(eyeReflection.x * eyeReflection.x +
                         eyeReflection.y * eyeReflection.y +
                         (eyeReflection.z + 1.0) * (eyeReflection.z + 1.0));
    float2 uv = float2(eyeReflection.x / m + 0.5,
                       eyeReflection.y / m + 0.5);
    return bindlessTextures[NonUniformResourceIndex(material.envMap)].Sample(uv).rgb;
}

[shader("fragment")]
GBufferOutput sceneDrawFragment(Varyings input, uint primitiveId : SV_PrimitiveID,
                               bool isFrontFace : SV_IsFrontFace) {
    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];

    float2 uv = transformedUV(material, input.uv1);
    if (!commitsTracedCoverage(material, uv, input.color.a)) discard;

    float4 diffuse = float4(1.0);
    if (material.mainTex != kNoTexture) {
        diffuse = bindlessTextures[NonUniformResourceIndex(material.mainTex)].Sample(uv);
    }
    diffuse *= input.color;
    if ((material.featureMask & kMegaFeatureWater) != 0) {
        diffuse *= material.waterAlpha;
    }

    bool envmapped = (material.featureMask & kMegaFeatureEnvMap) != 0;

    // Match the traced G-buffer's committed-side convention on two-sided
    // geometry. Transparent forward shading below deliberately retains the
    // authored normal, as pbr_model.slang does.
    float3 normal = megaMaterialNormal(material, input, uv, true, isFrontFace);

    bool hasLightmap = (material.featureMask & kMegaFeatureLightmap) != 0 &&
                       material.lightmap != kNoTexture;
    float3 lightmap = float3(1.0);
    if (hasLightmap) {
        lightmap = bindlessTextures[NonUniformResourceIndex(material.lightmap)].Sample(input.uv2).rgb;
    }
    float features = packGeometryFeatures(
        envmapped,
        hasLightmap,
        (material.featureMask & kMegaFeatureStatic) != 0,
        (material.featureMask & kMegaFeatureShadows) != 0,
        (material.featureMask & kMegaFeatureFog) != 0);
    float3 eyeNormal = 0.5 * mul(normal, (float3x3)globalUniforms.viewInv) + 0.5;

    GBufferOutput output;
    output.diffuseColor = diffuse;
    output.eyeNormal = float4(eyeNormal, 0.0);
    output.lightmapColor = float4(lightmap, features);
    output.selfIllumColor = float4(material.selfIllumColor.rgb,
                                   material.envMapDerivedLayer / 255.0);
    output.motion = computeMotion(input.curClipPos, input.prevClipPos);
    output.triangleId = triangleId;
    return output;
}

// G8's forward blended pass, over the same merged geometry and the same
// non-opaque range the G-buffer already draws for cutout coverage.
//
// The two transparent kinds are one draw, not two. Output is premultiplied and
// the blend state is fixed at ONE, ONE_MINUS_SRC_ALPHA, so an additive surface
// is simply alpha zero and a lit-blended one carries alpha equal to coverage.
// The material decides which by the alpha it writes, so there is no second
// pipeline and no branch in the frame graph.
//
// Order is submission order. kvp-main's replay of the retail draw stream
// reorders only true-opaque depth-writing batches and never touches blended
// ones, so a distance sort would be a deviation before it was an improvement.
[shader("fragment")]
float4 sceneDrawBlendedFragment(Varyings input, uint primitiveId : SV_PrimitiveID,
                               bool isFrontFace : SV_IsFrontFace) {
    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];

    // Cutouts share this range but are opaque and already in the G-buffer.
    if ((material.featureMask & kMegaFeaturePunchThrough) != 0) discard;

    float2 uv = transformedUV(material, input.uv1);
    float4 texel = float4(1.0);
    if (material.mainTex != kNoTexture) {
        texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)].Sample(uv);
    }

    bool blended = (material.featureMask & kMegaFeatureBlendedCoverage) != 0;
    float coverage = texel.a;
    float3 texelColor = texel.rgb;
    if ((material.featureMask & kMegaFeaturePremulAlpha) != 0) {
        // Match particles.slang and pbr_model.slang completely: recover the
        // missing coverage and undo it before applying the blend once below.
        coverage = rgbToLuma(texel.rgb);
        texelColor *= 1.0 / max(0.0001, coverage);
    }

    float objectAlpha = input.color.a * material.diffuseColor.a;
    float3 color = texelColor * input.color.rgb;
    if (blended) {
        bool envmapped = (material.featureMask & kMegaFeatureEnvMap) != 0;
        if (!envmapped) objectAlpha *= coverage;

        float3 lighting = float3(1.0);
        if ((material.featureMask & kMegaFeatureLightmap) != 0 &&
            material.lightmap != kNoTexture) {
            lighting = bindlessTextures[NonUniformResourceIndex(material.lightmap)]
                           .Sample(input.uv2).rgb;
            if ((material.featureMask & kMegaFeatureWater) != 0) {
                lighting = lerp(float3(1.0), lighting, 0.2);
            }
        }
        color *= lighting;

        if (envmapped) {
            float3 normal = megaMaterialNormal(material, input, uv, false,
                                               isFrontFace);
            float3 incident = normalize(input.worldPos -
                                        globalUniforms.cameraPosition.xyz);
            float3 worldReflection = reflect(incident, normal);
            float3 eyeReflection = normalize(
                mul(globalUniforms.view, float4(worldReflection, 0.0)).xyz);
            color += sampleMegaEnvMap(material, eyeReflection) * (1.0 - coverage);
        }
        if ((material.featureMask & kMegaFeatureWater) != 0) {
            color *= material.waterAlpha;
        }
    } else {
        objectAlpha *= coverage;
    }
    if (objectAlpha <= 0.0) discard;

    // Everything above works on authored, display-referred texels - the main
    // texture, the vertex colour, the lightmap, the environment cube - which is
    // how the original composed these surfaces and how the material fields are
    // written. The target is linear now, so the composed colour is converted
    // once here, at the point it leaves the shader.
    //
    // Only the colour. The coverage is a fraction of the pixel, not a light
    // value, so it is not a quantity gamma applies to: alpha stays exactly what
    // it was and the fixed ONE / ONE_MINUS_SRC_ALPHA state still means what it
    // meant. What genuinely changes is that the hardware now interpolates
    // energy instead of encoded values, which is the physically correct blend
    // and is visibly different on a bright surface over a dark one.
    // Blended surfaces occlude in proportion to coverage; additive ones add and
    // never occlude, which premultiplied output expresses as alpha zero.
    //
    // Where the gamma goes differs between them, and it is not a detail. For a
    // COVERAGE blend the alpha is a fraction of the pixel the surface fills, so
    // it scales energy and belongs outside: linear(colour) * coverage. For an
    // ADDITIVE surface the alpha is not coverage at all - it is the
    // premultiplication this shader just undid, recovered as luma - so
    // colour * alpha is the authored display texel, and the gamma applies to
    // that product. Applying it to the colour alone overstates every partially
    // covered texel by alpha^(1-gamma): sevenfold at alpha 0.2, which is a
    // saber blade's soft edge, while leaving its saturated core untouched. That
    // is a glowing outline drawn around every additive sprite.
    if (blended) {
        return float4(pow(max(color, 0.0), float3(kGamma)) * objectAlpha, objectAlpha);
    }
    return float4(pow(max(color * objectAlpha, 0.0), float3(kGamma)), 0.0);
}
