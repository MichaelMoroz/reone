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
import lib.grasscard;
import lib.scene_schema;
import lib.shadow;
import lib.assemble;

// --- Merged-geometry addressing, shared by both variants ---
// Shared declarations for raster passes over the canonical merged geometry.
// Both the G-buffer and shadow mega-draws use the same material gate so a
// punch-through surface cannot disagree about coverage between the two.

static const uint kNoTexture = 0xffffffffu;
/** Authored colour is display-referred; the blended pass converts on the way
    out, because the scene target it draws into is linear. Must match the
    exponent the post-process encode inverts. */
static const float kGamma = 2.2;
static const uint kMegaFeatureLightmap = 1u << 0;
static const uint kMegaFeatureEnvMap = 1u << 1;
static const uint kMegaFeatureNormalMap = 1u << 2;
static const uint kMegaFeatureBumpMap = 1u << 3;
static const uint kMegaFeatureShadows = 1u << 7;
static const uint kMegaFeatureWater = 1u << 8;
static const uint kMegaFeatureFog = 1u << 9;
static const uint kMegaFeaturePremulAlpha = 1u << 12;
static const uint kMegaFeatureEnvMapCube = 1u << 13;
static const uint kMegaFeatureStatic = 1u << 14;
static const uint kMegaFeatureThin = 1u << 15;
static const uint kMegaFeatureProcedural = 1u << 16;
static const uint kMegaFeatureBlendedCoverage = 1u << 25;
static const uint kMegaFeaturePunchThrough = 1u << 26;
static const float kMegaAlphaTestThreshold = 0.5;
// Must match the RGBA8 poison created by VulkanDescriptors. Requiring the
// private alpha byte as well as magenta makes an authored false positive both
// vanishingly narrow and self-locating.
static const float4 kBindlessPoisonSample = float4(1.0, 0.0, 1.0, 254.0 / 255.0);

[[vk::binding(0, 2)]] StructuredBuffer<MergedVertex> mergedVertices;
[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(4, 2)]] Sampler2DArray bindlessTextureArrays[];
[[vk::binding(5, 2)]] SamplerCube bindlessTextureCubes[];
[[vk::binding(6, 2)]] StructuredBuffer<float4> grassCardVertices;
[[vk::binding(7, 2)]] StructuredBuffer<uint> grassCardIndices;
[[vk::binding(8, 2)]] StructuredBuffer<GrassCardInstance> grassCardInstances;

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
/** kResolveFlagDisplayReferred, as the blended pass receives it. */
static const uint kMegaFlagDisplayReferred = 8u;
/** kResolveFlagFog: the area authored fog and the player's switch is on. */
static const uint kMegaFlagFog = 32u;
static const int kMegaLightDynamicTypeAll = 1;

struct PushConstants {
    uint triangleBase;
    uint materialGated;
    /**
     * The third word, whose meaning is the pass's own.
     *
     * The shadow pass reads it as a bit per object category allowed to write
     * the map, from kMegaCategoryShift of the material's feature mask. The
     * blended pass reads it as the kResolveFlag* set, for which it needs
     * exactly one bit. The G-buffer pass pushes two words and reads neither
     * meaning, which is why the two passes share this one rather than each
     * taking their own.
     */
    uint casterCategories;
    /**
     * Which caster the shadow pass is rendering, as an index into
     * globalUniforms.shadowLights. Read by the VERTEX stage, which is why the
     * push range covers both graphics stages - see VulkanCommandBuffer.
     *
     * SV_ViewID alone cannot carry this. It gives the layer within the pass,
     * so it selects a cascade or a cube face but not which caster's set of
     * them, and a mask wide enough to make it absolute would need more views
     * than multiview allows once point casters are cubes.
     */
    uint shadowSlot;
    /**
     * Height fog, for the blended pass alone. A blended surface writes no
     * depth, so the fog tail pass - which reads the depth buffer - cannot
     * reach it: fogged there, a near tree took the horizon's fog through the
     * depth BEHIND it. It fogs itself instead, at its own position, with the
     * same function. Density at the fog plane, its falloff with height, and
     * the plane's world Z - the walkmesh.
     */
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
 * Cube face `view` of the point caster being rendered, COMPUTED rather than
 * looked up.
 *
 * Every term is fixed by the format - a 90-degree square frustum, near 0.25,
 * far 2500, and six axis-aligned views - so the light's own position is the
 * only variable, and it is already in the uniform block. Storing six matrices
 * per caster instead spent 6 KB of the block's 16 KB ceiling, and that array
 * alone was what pinned the cube budget at sixteen while a module like
 * tar_m09aa carries 46 lamps. Nothing SAMPLES these matrices - the cube is
 * sampled by direction and radial distance - so they existed only to transform
 * vertices in this pass.
 *
 * Verified against the CPU's own matrices, all six faces, to five digits, and
 * the rendered result is bit-identical to the stored-array path.
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
    // The texture's own threshold when it authored one. kMegaAlphaTestThreshold
    // is the fallback for textures that do not - a TGA, or a TPC whose header
    // says 1.0 - not a value the data ever asked for. Must stay in lockstep
    // with ptAlphaTestThreshold in tracing/trace.slang.
    float threshold = material.alphaTest >= 0.0 ? material.alphaTest
                                                : kMegaAlphaTestThreshold;
    return alpha >= threshold;
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
 * Opaque casters pass the mode's category filter. Non-opaque casters instead
 * pass their authored coverage gate: cutouts are binary, while lit blends
 * write a stable fraction of shadow texels for the PCF lookup to integrate.
 *
 * The opaque caster set is a category filter rather than a separate submission
 * because the merge produces one buffer per frame ordered by coverage, not by
 * object kind; splitting it by category would rebuild that ordering on the CPU
 * every frame to save a fragment-shader test that costs nothing when nothing
 * is filtered. Whole-category filtering also means the range stays contiguous,
 * so the draw count does not depend on how many creatures are in the room.
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
    bool envmapped = (material.featureMask & kMegaFeatureEnvMap) != 0;
    if (!envmapped && material.mainTex != kNoTexture) {
        float4 texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)]
                           .SampleLevel(uv, 0.0);
        coverage = (material.featureMask & kMegaFeaturePremulAlpha) != 0
                       ? rgbToLuma(texel.rgb)
                       : texel.a;
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

    bool cutout = (material.featureMask & kMegaFeaturePunchThrough) != 0;
    bool blended = (material.featureMask & kMegaFeatureBlendedCoverage) != 0;
    // PROCEDURAL SPRITES cast nothing, exactly as the tracer's shadow ray
    // skips them (trace.slang, isProceduralSprite): a smoke plume is volume
    // the marcher owns, not a surface, and letting its lit-blend branch fall
    // through to the coverage dither below stamped the plume into the map as
    // per-texel noise - grainy dark garbage in the shadows wherever particles
    // drifted through a caster's frustum. Grass keeps its punch-through bit
    // and keeps casting, which also matches the tracer.
    if ((material.featureMask & kMegaFeatureProcedural) != 0 &&
        (material.featureMask & kMegaFeaturePunchThrough) == 0) {
        discard;
    }
    uint category = (material.featureMask >> kMegaCategoryShift) & kMegaCategoryMask;
    // The category filter applies to cutouts and blended surfaces too.
    //
    // It used to exempt them, which defeated the filter for exactly the
    // geometry most likely to be scenery: foliage, fences, grilles and grass
    // cards are cutouts, so in retro - whose caster set is creatures and
    // equipment and nothing else - they cast shadows the original never drew.
    // Measured on danm14ab: 42% of retro's shadowed pixels came from cutout
    // geometry, 13.3% of the frame.
    //
    // This does not stop a creature casting. The test is on the object's
    // category, not on whether the surface has holes, so a cutout that belongs
    // to a creature - hair, a cape, a robe hem - still passes the mask. What
    // it stops is a cutout that belongs to a room or a placeable.
    //
    // PBR and the tracer are unaffected: their mask is every category, so the
    // test can never reject.
    if ((pushConstants.casterCategories & (1u << category)) == 0) discard;

    if (pushConstants.materialGated == kMegaCoverageUngated) return;
    float2 uv = transformedUV(material, rawUv);
    if (cutout) {
        if (!commitsTracedCoverage(material, uv, vertexAlpha)) discard;
        return;
    }
    // Additive-emissive sheets carry radiance, not coverage, and therefore do
    // not remove light. A lit blend is scalar here because a depth map has no
    // colour channel; stable texel coverage preserves density without turning
    // glass into a wall or making its shadow crawl between frames.
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

float3 sampleMegaEnvMap(InstanceMaterial material, float3 worldReflection) {
    if ((material.featureMask & kMegaFeatureEnvMapCube) != 0 &&
        material.envMapCube != kNoTexture) {
        return bindlessTextureCubes[NonUniformResourceIndex(material.envMapCube)]
            .Sample(worldReflection).rgb;
    }
    if (material.envMap == kNoTexture) return float3(0.0);
    return bindlessTextures[NonUniformResourceIndex(material.envMap)]
        .Sample(authoredMirrorUV(worldReflection)).rgb;
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
        (material.featureMask & kMegaFeatureFog) != 0,
        (material.featureMask & kMegaFeatureThin) != 0);
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
        (material.featureMask & kMegaFeaturePunchThrough) != 0;
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

        bool shadowed = (material.featureMask & kMegaFeatureShadows) != 0;
        float3 materialNormal = input.normal;
        if (envmapped || shadowed) {
            materialNormal = megaMaterialNormal(material, input, uv, false,
                                                isFrontFace);
        }
        if (shadowed) {
            float3 viewPos = mul(globalUniforms.view,
                                 float4(input.worldPos, 1.0)).xyz;
            // Forward transparency has no separable direct-light lobe. Apply
            // visibility to its authored diffuse contribution, while leaving
            // the captured environment reflection below untouched.
            //
            // The FIRST caster's, not every caster's, and that is a choice
            // rather than a derivation: with no per-light lobe there is
            // nothing to attenuate per light, and combining several casters
            // into one factor would darken a blended surface once per light
            // that happens to cast. Slot zero is the defensible single answer
            // because selection orders casters directional-first then by
            // distance, so it holds the light most likely to dominate.
            color *= getShadow(
                viewPos, input.worldPos, materialNormal,
                (material.featureMask & kMegaFeatureThin) != 0);
        }
        if (envmapped) {
            float3 worldReflection = authoredMirrorReflection(
                input.worldPos, globalUniforms.cameraPosition.xyz, materialNormal);
            color += sampleMegaEnvMap(material, worldReflection) * (1.0 - coverage);
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
    //
    // None of which applies when the target is already display-referred, which
    // is retro's chain: there the composed colour goes out as it was composed,
    // and the blend the hardware then performs is the one the original
    // performed, in the space the original performed it in.
    BlendedOutput output;
    if ((pushConstants.casterCategories & kMegaFlagDisplayReferred) != 0) {
        output.color = blended ? float4(color * objectAlpha, objectAlpha)
                               : float4(color * objectAlpha, 0.0);
    } else if (blended) {
        output.color = float4(pow(max(color, 0.0), float3(kGamma)) * objectAlpha,
                              objectAlpha);
    } else {
        output.color = float4(pow(max(color * objectAlpha, 0.0), float3(kGamma)),
                              0.0);
    }

    // Fog, here rather than in the tail pass, because this surface has no
    // depth for that pass to read. Applied to the OUTPUT, which is what the
    // blend state sees: a coverage blend lerps toward the fog colour scaled by
    // its own coverage, so the covered fraction of the pixel fogs and the rest
    // is left to what lies behind; an additive surface is light and carries no
    // coverage, so fog can only attenuate it - lerping it toward the fog colour
    // would ADD fog-coloured energy at every transparent texel of a saber.
    //
    // Retro composes display-referred and preserves the original's model, so
    // its transparents take the same linear ramp its resolve applies, with the
    // fog colour as authored; the corrected modes take the height fog in
    // linear space, matching the tail pass over the opaque image.
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
            fogColor = pow(max(globalUniforms.fogColor.rgb, 0.0), float3(kGamma));
        }
        if (blended) {
            output.color.rgb = lerp(output.color.rgb, fogColor * objectAlpha, fog);
        } else {
            output.color.rgb *= 1.0 - fog;
        }
    }

    // The motion target uses the same premultiplied blend as colour. A
    // partially covered surface therefore contributes in the same proportion
    // as the visible layer, while the opaque motion already in the target
    // remains the remainder. Additive colour still has alpha zero, but its
    // separate motion output carries the texel intensity as coverage so a
    // moving glow does not masquerade as stationary background.
    float motionCoverage = saturate(objectAlpha);
    output.motion = float4(
        computeMotion(input.curClipPos, input.prevClipPos) * motionCoverage,
        0.0, motionCoverage);
    return output;
}
