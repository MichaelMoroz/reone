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
import lib.envmap;
import lib.material_bindless;
import lib.grasscard;
import lib.scene_schema;
import lib.shadow;
import lib.assemble;
import lib.material_ops;

// Raster passes over the canonical merged geometry. The G-buffer and shadow
// draws share one material gate, so coverage cannot disagree between them.

/** Authored colour is display-referred; the blended pass converts on the way
    out, because the scene target it draws into is linear. Must match the
    exponent the post-process encode inverts. */
// Must match the RGBA8 poison created by VulkanDescriptors. Requiring the
// private alpha byte as well as magenta makes an authored false positive both
// vanishingly narrow and self-locating.
static const float4 kBindlessPoisonSample = float4(1.0, 0.0, 1.0, 254.0 / 255.0);

[[vk::binding(0, 2)]] StructuredBuffer<MergedVertex> mergedVertices;
[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(4, 2)]] Sampler2DArray bindlessTextureArrays[];
[[vk::binding(6, 2)]] StructuredBuffer<float4> grassCardVertices;
[[vk::binding(7, 2)]] StructuredBuffer<uint> grassCardIndices;
[[vk::binding(8, 2)]] StructuredBuffer<GrassCardInstance> grassCardInstances;

// Which coverage a pass publishes over the same range: the opaque range, the
// shared punch-through gate, or the forward blended surfaces.
static const uint kMegaCoverageUngated = 0u;
static const uint kMegaCoverageCutout = 1u;
static const uint kMegaCoverageBlended = 2u;

/** Every category casts. The shadow pass compares against this to skip the
    per-fragment material fetch entirely when nothing is being filtered. */
static const uint kMegaCastersAll = 0xFFFFFFFFu;
/** kResolveFlagDisplayReferred, as the blended pass receives it. */
static const uint kMegaFlagDisplayReferred = 8u;
/** kResolveFlagFog: the area authored fog and the player's switch is on. */
static const uint kMegaFlagFog = 32u;
static const int kMegaLightDynamicTypeAll = 1;

struct PushConstants {
    uint triangleBase;
    uint materialGated;
    /** Per pass: the shadow pass's caster-category bits, the blended pass's kResolveFlag* set. */
    uint casterCategories;
    /** The caster the shadow pass renders, into globalUniforms.shadowLights; SV_ViewID
        only selects the layer within it. Read by the vertex stage. */
    uint shadowSlot;
    /** Height fog for the blended pass, which writes no depth for the tail pass to read:
        density at the plane, falloff with height, and the plane's world Z. */
    float fogDensity;
    float fogFalloff;
    float fogPlaneZ;
};
[[vk::push_constant]] PushConstants pushConstants;

/** The caster this shadow pass is rendering. */
GlobalUniformsShadowLight shadowSlotLight() {
    return globalUniforms.shadowLights[pushConstants.shadowSlot];
}

/** Cascade `view` of the directional caster being rendered. */
float4x4 directionalCascadeSpace(uint view) {
    return globalUniforms.shadowCascadeSpace[shadowSlotLight().mapIndex + view];
}

/**
 * Cube face `view` of the point caster, computed: every term but the light's
 * position is fixed by the format, and six stored matrices per caster would
 * spend the uniform block's ceiling.
 */
float4x4 pointFaceSpace(uint view) {
    const float3 lightPos = shadowSlotLight().positionOrDirection.xyz;
    // The six (forward, up) pairs of getPointLightView, in face order.
    float3 fwd, up;
    switch (view) {
    case 0:  fwd = float3(1.0, 0.0, 0.0);  up = float3(0.0, -1.0, 0.0); break;
    case 1:  fwd = float3(-1.0, 0.0, 0.0); up = float3(0.0, -1.0, 0.0); break;
    case 2:  fwd = float3(0.0, 1.0, 0.0);  up = float3(0.0, 0.0, 1.0);  break;
    case 3:  fwd = float3(0.0, -1.0, 0.0); up = float3(0.0, 0.0, -1.0); break;
    case 4:  fwd = float3(0.0, 0.0, 1.0);  up = float3(0.0, -1.0, 0.0); break;
    default: fwd = float3(0.0, 0.0, -1.0); up = float3(0.0, -1.0, 0.0); break;
    }
    // glm::lookAt, right-handed.
    float3 f = normalize(fwd);
    float3 s = normalize(cross(f, up));
    float3 u = cross(s, f);
    float4x4 lightView = float4x4(
        float4(s.x, s.y, s.z, -dot(s, lightPos)),
        float4(u.x, u.y, u.z, -dot(u, lightPos)),
        float4(-f.x, -f.y, -f.z, dot(f, lightPos)),
        float4(0.0, 0.0, 0.0, 1.0));
    // glm::perspectiveRH_ZO(90 degrees, aspect 1, near, far): tan(45) = 1.
    const float kNear = 0.25;
    const float kFar = 2500.0;
    const float a = kFar / (kNear - kFar);
    float4x4 lightProjection = float4x4(
        float4(1.0, 0.0, 0.0, 0.0),
        float4(0.0, 1.0, 0.0, 0.0),
        float4(0.0, 0.0, a, a * kNear),
        float4(0.0, 0.0, -1.0, 0.0));
    return mul(lightProjection, lightView);
}

float2 transformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}

// In lockstep with candidateAlpha and kAlphaTestThreshold in tracing/trace.slang:
// the tracer reads its primary out of this G-buffer. Cutout coverage only;
// blended and additive surfaces wait for the forward pass.
bool commitsTracedCoverage(InstanceMaterial material, float2 uv, float vertexAlpha) {
    if (pushConstants.materialGated == kMegaCoverageUngated) return true;

    if ((material.featureMask & kMaterialPunchThrough) == 0) return false;

    float alpha = 1.0;
    if (material.mainTex != kNoTexture) {
        float4 texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)]
                           .SampleLevel(uv, 0.0);
        alpha = texelCoverage(material, texel) * vertexAlpha;
    }
    // The authored threshold or the fallback, as ptAlphaTestThreshold resolves it.
    float threshold = alphaTestThreshold(material);
    return alpha >= threshold;
}

// The depth-only variant: the same merged geometry, already in world space,
// written to a shadow map.

static const float kPointShadowFar = 2500.0;
// Point shadows store radial distance, so pipeline depth bias is reproduced in
// radial-depth units; the receiver normal offset handles most grazing cases.
static const float kPointShadowConstantBias = 0.0;
static const float kPointShadowSlopeBias = 1.0;

struct GrassCardDrawData {
    GrassCardVertex vertex;
    uint materialIndex;
    uint triangleBase;
};

GrassCardDrawData grassCardDrawData(uint vertexId, uint instanceId) {
    GrassCardInstance card = grassCardInstances[instanceId];
    uint indexCount;
    uint indexStride;
    grassCardIndices.GetDimensions(indexCount, indexStride);
    uint cardTris = indexCount / 12;
    uint variant = uint(card.lightmapVariantCulled.z);
    uint templateIndex = grassCardIndices[variant * cardTris * 3 + vertexId];
    uint vertexCount;
    uint vertexStride;
    grassCardVertices.GetDimensions(vertexCount, vertexStride);
    uint cardVerts = vertexCount / 4;
    GrassCardDrawData result;
    result.vertex = resolveGrassCardVertex(
        card, grassCardVertices[variant * cardVerts + templateIndex],
        globalUniforms.cameraPosition.xyz);
    result.materialIndex = card.materialIndex.x;
    result.triangleBase = 0x80000000u | (instanceId * cardTris);
    return result;
}

struct DirectionalShadowVaryings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float vertexAlpha : TEXCOORD1;
    nointerpolation uint materialIndex : TEXCOORD2;
    nointerpolation bool culled : TEXCOORD3;
};

[shader("vertex")]
DirectionalShadowVaryings directionalShadowVertex(
    uint vertexId : SV_VertexID, uint viewId : SV_ViewID) {
    MergedVertex vertex = mergedVertices[vertexId];
    DirectionalShadowVaryings output;
    output.position = mul(directionalCascadeSpace(viewId),
                          float4(vertex.position, 1.0));
    output.uv = vertex.uv1;
    output.vertexAlpha = vertex.color.a;
    output.materialIndex = kNoTexture;
    output.culled = false;
    return output;
}

[shader("vertex")]
DirectionalShadowVaryings grassCardDirectionalShadowVertex(
    uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID,
    uint viewId : SV_ViewID) {
    GrassCardDrawData card = grassCardDrawData(vertexId, instanceId);
    DirectionalShadowVaryings output;
    output.position = mul(directionalCascadeSpace(viewId),
                          float4(card.vertex.position, 1.0));
    output.uv = card.vertex.uv;
    output.vertexAlpha = 1.0;
    output.materialIndex = card.materialIndex;
    output.culled = card.vertex.culled;
    return output;
}

/**
 * Casters pass the mode's category filter and their authored coverage gate:
 * cutouts are binary, lit blends write a stable fraction of texels for the
 * PCF to integrate. A filter rather than a split submission, so the merged
 * range stays contiguous.
 */
float shadowNoise(uint2 texel, uint triangleId, uint materialIndex) {
    uint hash = texel.x * 0x8da6b343u ^ texel.y * 0xd8163841u ^
                triangleId * 0xcb1ab31fu ^ materialIndex * 0x165667b1u;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    return float(hash & 0x00ffffffu) / 16777216.0;
}

float blendedShadowCoverage(InstanceMaterial material, float2 uv,
                            float vertexAlpha) {
    float coverage = 1.0;
    bool envmapped = (material.featureMask & kMaterialEnvMap) != 0;
    if (!envmapped && material.mainTex != kNoTexture) {
        float4 texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)]
                           .SampleLevel(uv, 0.0);
        coverage = texelCoverage(material, texel);
    }
    return saturate(coverage * vertexAlpha * material.diffuseColor.a);
}

void applyShadowGates(float2 rawUv, float vertexAlpha, float2 fragCoord,
                      uint primitiveId, uint directMaterialIndex, bool culled) {
    if (culled) discard;
    // Nothing filtered and no coverage gate: the common case reads no memory.
    if (pushConstants.casterCategories == kMegaCastersAll &&
        pushConstants.materialGated == kMegaCoverageUngated) {
        return;
    }
    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = directMaterialIndex == kNoTexture
        ? mergedMaterialIds[triangleId] : directMaterialIndex;
    InstanceMaterial material = instanceMaterials[materialIndex];

    bool cutout = (material.featureMask & kMaterialPunchThrough) != 0;
    bool blended = (material.featureMask & kMaterialBlendedCoverage) != 0;
    // Procedural sprites cast nothing, as the tracer's shadow ray agrees: a
    // plume is volume, and dithering it into the map is per-texel noise.
    if ((material.featureMask & kMaterialProcedural) != 0 &&
        (material.featureMask & kMaterialPunchThrough) == 0) {
        discard;
    }
    uint category = (material.featureMask >> kMaterialCategoryShift) & kMaterialCategoryMask;
    // The category filter applies to cutouts and blends too: in retro, whose
    // casters are creatures only, scenery foliage must not cast. The test is
    // on the object's category, so a creature's cutout hair still passes.
    if ((pushConstants.casterCategories & (1u << category)) == 0) discard;

    if (pushConstants.materialGated == kMegaCoverageUngated) return;
    float2 uv = transformedUV(material, rawUv);
    if (cutout) {
        if (!commitsTracedCoverage(material, uv, vertexAlpha)) discard;
        return;
    }
    // Additive sheets are radiance and remove no light; a lit blend writes
    // stable scalar coverage, a depth map having no colour channel.
    if (!blended) discard;
    float coverage = blendedShadowCoverage(material, uv, vertexAlpha);
    if (coverage <= shadowNoise(uint2(fragCoord), triangleId, materialIndex)) discard;
}

[shader("fragment")]
void directionalShadowFragment(DirectionalShadowVaryings input,
                                       uint primitiveId : SV_PrimitiveID) {
    applyShadowGates(input.uv, input.vertexAlpha, input.position.xy, primitiveId,
                     input.materialIndex, input.culled);
}

struct PointShadowVaryings {
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float vertexAlpha : TEXCOORD2;
    nointerpolation uint materialIndex : TEXCOORD3;
    nointerpolation bool culled : TEXCOORD4;
};

[shader("vertex")]
PointShadowVaryings pointShadowVertex(
    uint vertexId : SV_VertexID, uint viewId : SV_ViewID) {
    MergedVertex vertex = mergedVertices[vertexId];
    PointShadowVaryings output;
    output.worldPos = vertex.position;
    output.position = mul(pointFaceSpace(viewId), float4(vertex.position, 1.0));
    output.uv = vertex.uv1;
    output.vertexAlpha = vertex.color.a;
    output.materialIndex = kNoTexture;
    output.culled = false;
    return output;
}

[shader("vertex")]
PointShadowVaryings grassCardPointShadowVertex(
    uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID,
    uint viewId : SV_ViewID) {
    GrassCardDrawData card = grassCardDrawData(vertexId, instanceId);
    PointShadowVaryings output;
    output.worldPos = card.vertex.position;
    output.position = mul(pointFaceSpace(viewId), float4(card.vertex.position, 1.0));
    output.uv = card.vertex.uv;
    output.vertexAlpha = 1.0;
    output.materialIndex = card.materialIndex;
    output.culled = card.vertex.culled;
    return output;
}

[shader("fragment")]
float pointShadowFragment(PointShadowVaryings input,
                                  uint primitiveId : SV_PrimitiveID) : SV_Depth {
    applyShadowGates(input.uv, input.vertexAlpha, input.position.xy, primitiveId,
                     input.materialIndex, input.culled);
    float lightDistance = length(input.worldPos - shadowSlotLight().positionOrDirection.xyz);
    float radialDepth = lightDistance / kPointShadowFar;
    float radialSlope = max(abs(ddx(radialDepth)), abs(ddy(radialDepth)));
    return min(1.0, radialDepth + kPointShadowConstantBias +
                        kPointShadowSlopeBias * radialSlope);
}

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
    nointerpolation uint materialIndex : TEXCOORD10;
    nointerpolation uint triangleId : TEXCOORD11;
    nointerpolation bool culled : TEXCOORD12;
};

[shader("vertex")]
Varyings sceneDrawVertex(uint vertexId : SV_VertexID) {
    MergedVertex vertex = mergedVertices[vertexId];
    Varyings output;
    output.curClipPos = mul(globalUniforms.viewProjection, float4(vertex.position, 1.0));
    output.prevClipPos = mul(globalUniforms.prevViewProjection,
                             float4(vertex.prevPosition, 1.0));
    // Rasterise on the jittered grid: the resolves invert this same matrix to
    // reconstruct position from depth. Motion stays unjittered, per uniforms.slang.
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
    output.materialIndex = kNoTexture;
    output.triangleId = 0;
    output.culled = false;
    return output;
}

[shader("vertex")]
Varyings grassCardDrawVertex(uint vertexId : SV_VertexID,
                             uint instanceId : SV_InstanceID) {
    GrassCardDrawData card = grassCardDrawData(vertexId, instanceId);
    GrassCardVertex vertex = card.vertex;
    Varyings output;
    output.curClipPos = mul(globalUniforms.viewProjection, float4(vertex.position, 1.0));
    output.prevClipPos = mul(globalUniforms.prevViewProjection,
                             float4(vertex.prevPosition, 1.0));
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, float4(vertex.position, 1.0)));
    output.normal = vertex.normal;
    output.tangent = float3(0.0);
    output.bitangent = float3(0.0);
    output.tanSpaceNormal = float3(0.0);
    output.uv1 = vertex.uv;
    output.uv2 = vertex.lightmapUV;
    output.color = float4(1.0);
    output.worldPos = vertex.position;
    output.materialIndex = card.materialIndex;
    output.triangleId = card.triangleBase;
    output.culled = vertex.culled;
    return output;
}

struct GBufferOutput {
    float4 diffuseColor : SV_Target0;
    float4 eyeNormal : SV_Target1;
    float4 lightmapColor : SV_Target2;
    float4 selfIllumColor : SV_Target3;
    float2 motion : SV_Target4;
    /** The merged-geometry triangle, R32Uint, cleared to 0xffffffff: a consumer
        expands it to the three vertices, the geometric normal and the material. */
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
    if ((material.featureMask & kMaterialNormalMap) != 0 &&
        material.normalMap != kNoTexture) {
        float3 n = bindlessTextures[NonUniformResourceIndex(material.normalMap)]
                       .Sample(uv).rgb * 2.0 - 1.0;
        normal = safeNormal(tangent * n.x + bitangent * n.y + frameNormal * n.z,
                            normal);
    } else if ((material.featureMask & kMaterialBumpMap) != 0 &&
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

[shader("fragment")]
GBufferOutput sceneDrawFragment(Varyings input, uint primitiveId : SV_PrimitiveID,
                               bool isFrontFace : SV_IsFrontFace) {
    if (input.culled) discard;
    uint triangleId = input.materialIndex == kNoTexture
        ? pushConstants.triangleBase + primitiveId
        : input.triangleId + primitiveId;
    uint materialIndex = input.materialIndex == kNoTexture
        ? mergedMaterialIds[triangleId]
        : input.materialIndex;
    InstanceMaterial material = instanceMaterials[materialIndex];

    float2 uv = transformedUV(material, input.uv1);
    if (!commitsTracedCoverage(material, uv, input.color.a)) discard;

    float4 diffuse = float4(1.0);
    if (material.mainTex != kNoTexture) {
        diffuse = bindlessTextures[NonUniformResourceIndex(material.mainTex)].Sample(uv);
    }
    diffuse *= input.color;
    if ((material.featureMask & kMaterialWater) != 0) {
        diffuse *= material.waterAlpha;
    }

    bool envmapped = (material.featureMask & kMaterialEnvMap) != 0;

    // Match the traced G-buffer's committed-side convention on two-sided
    // geometry. Transparent forward shading below deliberately retains the
    // authored normal, as pbr_model.slang does.
    float3 normal = megaMaterialNormal(material, input, uv, true, isFrontFace);

    bool hasLightmap = (material.featureMask & kMaterialLightmap) != 0 &&
                       material.lightmap != kNoTexture;
    float3 lightmap = float3(1.0);
    if (hasLightmap) {
        lightmap = bindlessTextures[NonUniformResourceIndex(material.lightmap)].Sample(input.uv2).rgb;
    }
    float features = packGeometryFeatures(
        envmapped,
        hasLightmap,
        (material.featureMask & kMaterialStatic) != 0,
        (material.featureMask & kMaterialShadows) != 0,
        (material.featureMask & kMaterialFog) != 0,
        (material.featureMask & kMaterialThin) != 0);
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

// The forward blended pass over the merged non-opaque range. One draw for both
// transparent kinds: output is premultiplied under ONE, ONE_MINUS_SRC_ALPHA, so
// additive is alpha zero and lit-blended carries coverage. Submission order,
// as the original: it never sorted blended batches.
struct BlendedOutput {
    float4 color : SV_Target0;
    float4 motion : SV_Target1;
};

[shader("fragment")]
BlendedOutput sceneDrawBlendedFragment(Varyings input,
                                       uint primitiveId : SV_PrimitiveID,
                                       bool isFrontFace : SV_IsFrontFace) {
    if (input.culled) discard;
    uint triangleId = input.materialIndex == kNoTexture
        ? pushConstants.triangleBase + primitiveId
        : input.triangleId + primitiveId;
    uint materialIndex = input.materialIndex == kNoTexture
        ? mergedMaterialIds[triangleId]
        : input.materialIndex;
    InstanceMaterial material = instanceMaterials[materialIndex];

    bool punchThrough =
        (material.featureMask & kMaterialPunchThrough) != 0;
    // Cutouts share this range but are opaque and already in the G-buffer.
    if (punchThrough) discard;

    float2 uv = transformedUV(material, input.uv1);
    float4 texel = float4(1.0);
    if (material.mainTex != kNoTexture) {
        texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)].Sample(uv);
        if (all(texel == kBindlessPoisonSample)) {
            // The stale surface is transparent and bypasses the G-buffer. Make
            // the diagnostic opaque and full-bright before authored lighting
            // can turn it into a plausible dark purple.
            BlendedOutput poison;
            poison.color = float4(1.0, 0.0, 1.0, 1.0);
            poison.motion = float4(computeMotion(input.curClipPos, input.prevClipPos),
                                   0.0, 1.0);
            return poison;
        }
    }

    bool blended = (material.featureMask & kMaterialBlendedCoverage) != 0;
    float coverage = texelCoverage(material, texel);
    float3 texelColor = texel.rgb;
    // Premultiplied colour has its coverage divided back out before the blend applies it once.
    if ((material.featureMask & kMaterialPremulAlpha) != 0) texelColor *= 1.0 / max(0.0001, coverage);

    float objectAlpha = input.color.a * material.diffuseColor.a;
    float3 color = texelColor * input.color.rgb;
    if (blended) {
        bool envmapped = (material.featureMask & kMaterialEnvMap) != 0;
        if (!envmapped) objectAlpha *= coverage;

        float3 lighting = float3(1.0);
        if ((material.featureMask & kMaterialLightmap) != 0 &&
            material.lightmap != kNoTexture) {
            lighting = bindlessTextures[NonUniformResourceIndex(material.lightmap)]
                           .Sample(input.uv2).rgb;
            if ((material.featureMask & kMaterialWater) != 0) {
                lighting = lerp(float3(1.0), lighting, 0.2);
            }
        }
        color *= lighting;

        bool shadowed = (material.featureMask & kMaterialShadows) != 0;
        float3 materialNormal = input.normal;
        if (envmapped || shadowed) {
            materialNormal = megaMaterialNormal(material, input, uv, false,
                                                isFrontFace);
        }
        if (shadowed) {
            float3 viewPos = mul(globalUniforms.view,
                                 float4(input.worldPos, 1.0)).xyz;
            // Slot zero only: with no per-light lobe there is nothing to
            // attenuate per light, and selection orders casters
            // directional-first, so slot zero holds the dominant one.
            color *= getShadow(
                viewPos, input.worldPos, materialNormal,
                (material.featureMask & kMaterialThin) != 0);
        }
        if (envmapped) {
            float3 worldReflection = authoredMirrorReflection(
                input.worldPos, globalUniforms.cameraPosition.xyz, materialNormal);
            color += sampleAuthoredMirror(material, worldReflection) * (1.0 - coverage);
        }
        if ((material.featureMask & kMaterialWater) != 0) {
            color *= material.waterAlpha;
        }
    } else {
        objectAlpha *= coverage;
    }
    if (objectAlpha <= 0.0) discard;

    // The composed colour is display-referred; convert once on the way out,
    // and only the colour - alpha is coverage. Coverage scales energy, so its
    // gamma applies outside: linear(colour) * coverage. Additive alpha is
    // recovered premultiplication, so the gamma applies to colour * alpha, or
    // partial coverage is overstated by alpha^(1-gamma). Retro's target is
    // already display-referred.
    bool displayReferred = (pushConstants.casterCategories & kMegaFlagDisplayReferred) != 0;
    float3 rgb = displayReferred ? color * objectAlpha
               : blended        ? gammaToLinear(color) * objectAlpha
                                : gammaToLinear(color * objectAlpha);
    BlendedOutput output;
    output.color = float4(rgb, blended ? objectAlpha : 0.0);

    // Fog here, this surface having no depth for the tail pass. Applied to the
    // output, which is what the blend state sees: a coverage blend lerps
    // toward fog scaled by coverage; an additive surface is light, so fog can
    // only attenuate it. Retro takes its authored ramp, the corrected modes
    // the linear height fog.
    if ((pushConstants.casterCategories & kMegaFlagFog) != 0) {
        const float3 cameraPos = globalUniforms.cameraPosition.xyz;
        const bool displayReferred =
            (pushConstants.casterCategories & kMegaFlagDisplayReferred) != 0;
        float fog;
        float3 fogColor;
        if (displayReferred) {
            fog = fogAmount(input.worldPos, cameraPos, globalUniforms.fogNear,
                            globalUniforms.fogFar);
            fogColor = globalUniforms.fogColor.rgb;
        } else {
            const float h0 = max(0.0, cameraPos.z - pushConstants.fogPlaneZ);
            const float h1 = max(0.0, input.worldPos.z - pushConstants.fogPlaneZ);
            fog = heightFogAmount(h0, h1, distance(input.worldPos, cameraPos),
                                  pushConstants.fogDensity, pushConstants.fogFalloff);
            fogColor = gammaToLinear(globalUniforms.fogColor.rgb);
        }
        if (blended) {
            output.color.rgb = lerp(output.color.rgb, fogColor * objectAlpha, fog);
        } else {
            output.color.rgb *= 1.0 - fog;
        }
    }

    // Motion blends premultiplied like colour; additive carries texel intensity
    // as its coverage so a moving glow does not read as stationary background.
    float motionCoverage = saturate(objectAlpha);
    output.motion = float4(
        computeMotion(input.curClipPos, input.prevClipPos) * motionCoverage,
        0.0, motionCoverage);
    return output;
}
