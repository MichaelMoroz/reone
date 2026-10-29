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

// Ray traversal. ptTraceNearest commits the nearest visible surface;
// ptShadowRay reports visibility to a light and the blocking distance.
//
// Two independent axes classify a non-opaque candidate. Coverage decides how
// much light it removes: CUTOUT (kMaterialPunchThrough) is a binary alpha test,
// PURELY EMISSIVE (kPtSurfaceUnlitTransparent) commits for its radiance and
// never occludes, TRANSMISSIVE (kMaterialTransmissive) tints and continues.
// Instance class - merged scene or grass card - decides which occlusion
// accumulator it lands in, and the inclusion mask decides whether it is
// reported at all. Procedural sprites (smoke, plumes) are volume, not surface:
// normal rays skip them; shadow rays still take their sheet coverage.

import lib.math;
import lib.grasscard;
import lib.scene_schema;
import lib.material_ops;
import uniforms;
import tracing.resources;
import tracing.rng;

public struct TraceHit {
    public bool hit;
    public uint materialIndex;
    public uint3 indices;
    public uint primitiveIndex;
    /** kPtNoCard means the merged BLAS; every card has its TLAS custom index. */
    public uint cardId;
    public float3 weights;
    public float hitT;
};

public static const uint kPtNoCard = 0xffffffffu;

public void ptCardTriangleVertices(uint cardId, uint primitive,
                                   out GrassCardVertex v0, out GrassCardVertex v1,
                                   out GrassCardVertex v2) {
    GrassCardInstance card = grassCardInstances[cardId];
    uint indexCount, indexStride;
    grassCardIndices.GetDimensions(indexCount, indexStride);
    uint cardTris = indexCount / 12;
    uint vertexCount, vertexStride;
    grassCardVertices.GetDimensions(vertexCount, vertexStride);
    uint cardVerts = vertexCount / 4;
    uint variant = uint(card.lightmapVariantCulled.z);
    uint first = variant * cardTris * 3 + primitive * 3;
    float3 camera = globalUniforms.cameraPosition.xyz;
    v0 = resolveGrassCardVertex(card, grassCardVertices[variant * cardVerts + grassCardIndices[first]], camera);
    v1 = resolveGrassCardVertex(card, grassCardVertices[variant * cardVerts + grassCardIndices[first + 1]], camera);
    v2 = resolveGrassCardVertex(card, grassCardVertices[variant * cardVerts + grassCardIndices[first + 2]], camera);
}

/** What a candidate is, read once per Proceed(). Instance 0 is the merged scene. */
struct Candidate {
    uint geometryIndex;
    uint primitive;
    uint instanceIndex;
    uint materialIndex;
    uint features;
    bool grass;
};

Candidate readCandidate(inout RayQuery<RAY_FLAG_NONE> query) {
    Candidate c;
    c.geometryIndex = query.CandidateGeometryIndex();
    c.primitive = query.CandidatePrimitiveIndex();
    c.instanceIndex = query.CandidateInstanceID();
    c.grass = c.instanceIndex != 0;
    c.materialIndex = c.grass ? grassCardInstances[c.instanceIndex - 1].materialIndex.x
                              : materialIndexFor(c.geometryIndex, c.primitive);
    c.features = instanceMaterials[c.materialIndex].featureMask;
    return c;
}

float3 barycentricWeights(float2 b) {
    return float3(1.0 - b.x - b.y, b.x, b.y);
}

/** Coverage of the candidate texel at the base level, unfiltered: a yes/no on the authored texel. */
float candidateAlpha(inout RayQuery<RAY_FLAG_NONE> query, Candidate c) {
    InstanceMaterial material = instanceMaterials[c.materialIndex];
    bool blended = (material.featureMask & kMaterialBlendedCoverage) != 0;
    bool envmapped = (material.featureMask & kMaterialEnvMap) != 0;
    bool textured = material.mainTex != kNoTexture;
    if (!blended && !textured) return 1.0;
    bool sample = textured && !(blended && envmapped);
    float3 w = barycentricWeights(query.CandidateTriangleBarycentrics());
    float2 uv = float2(0.0);
    float vertexAlpha = 1.0;
    if (c.grass) {
        if (sample) {
            GrassCardVertex v0, v1, v2;
            ptCardTriangleVertices(c.instanceIndex - 1, c.primitive, v0, v1, v2);
            uv = v0.uv * w.x + v1.uv * w.y + v2.uv * w.z;
        }
    } else {
        uint3 i = ptTriangleIndices(c.geometryIndex, c.primitive);
        uv = mergedVertices[i.x].uv1 * w.x + mergedVertices[i.y].uv1 * w.y + mergedVertices[i.z].uv1 * w.z;
        vertexAlpha = mergedVertices[i.x].color.a * w.x + mergedVertices[i.y].color.a * w.y +
                      mergedVertices[i.z].color.a * w.z;
    }
    float alpha = 1.0;
    if (sample) {
        float4 texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)]
                           .SampleLevel(ptTransformedUV(material, uv), 0.0);
        alpha = texelCoverage(material, texel);
    }
    if (blended) alpha *= material.diffuseColor.a;
    return alpha * vertexAlpha;
}

TraceHit committedHit(inout RayQuery<RAY_FLAG_NONE> query) {
    TraceHit result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    if (result.hit) {
        uint geometryIndex = query.CommittedGeometryIndex();
        uint instanceIndex = query.CommittedInstanceID();
        result.primitiveIndex = query.CommittedPrimitiveIndex();
        result.cardId = instanceIndex == 0 ? kPtNoCard : instanceIndex - 1;
        result.materialIndex = instanceIndex == 0
            ? materialIndexFor(geometryIndex, result.primitiveIndex)
            : grassCardInstances[instanceIndex - 1].materialIndex.x;
        result.indices = instanceIndex == 0 ? ptTriangleIndices(geometryIndex, result.primitiveIndex)
                                            : uint3(0);
        result.weights = barycentricWeights(query.CommittedTriangleBarycentrics());
        result.hitT = query.CommittedRayT();
    }
    return result;
}

bool isCutout(uint features) { return (features & kMaterialPunchThrough) != 0; }
/** Non-emissive translucency; skipped by normal rays unless traced transmissive. */
bool isBlendedCoverage(uint features) { return (features & kMaterialBlendedCoverage) != 0; }
/** Blended coverage that is scene geometry: committed, tinted through. */
bool isTracedTransmissive(uint features) { return (features & kMaterialTransmissive) != 0; }
/**
 * A particle plume or lens-flare billboard: a camera-facing picture of a
 * volume, wrong from anywhere but the camera, so no ray sees one. Grass is
 * procedural too but punch-through, and keeps its cutout traversal.
 */
bool isProceduralSprite(uint features) {
    return (features & kMaterialProcedural) != 0 && (features & kMaterialPunchThrough) == 0;
}

/**
 * The nearest ray. Opaque geometry commits in hardware; of the non-opaque
 * candidates, purely emissive surfaces always commit, cutouts commit at their
 * threshold, transmissive geometry commits above the epsilon, and other
 * translucency is skipped.
 */
public TraceHit ptTraceNearest(RayDesc ray) {
    // Grass is dropped by the instance mask, not in the loop: a candidate skip
    // has already paid the descent and the intersection.
    uint inclusion = kPtInstanceGeometry | (ptFlag(kPtFlagGrassScatter) ? kPtInstanceGrass : 0u);
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, inclusion, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        Candidate c = readCandidate(query);
        // Before the emissive commit: an additive plume is emissive too.
        if (isProceduralSprite(c.features)) continue;
        if (instanceMaterials[c.materialIndex].surfaceType == kPtSurfaceUnlitTransparent) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        if (isTracedTransmissive(c.features)) {
            // Tested against the epsilon, not the threshold: a hole in a leaf
            // card is not a surface; everything above it commits and the bounce
            // loop decides how much light survives.
            if (candidateAlpha(query, c) >= kPtAlphaTestEpsilon) query.CommitNonOpaqueTriangleHit();
            continue;
        }
        if (isBlendedCoverage(c.features)) continue;
        if (!isCutout(c.features)) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        float alpha = candidateAlpha(query, c);
        if (alpha >= alphaTestThreshold(instanceMaterials[c.materialIndex]) && alpha >= kPtAlphaTestEpsilon) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return committedHit(query);
}

/**
 * The shadow ray's answer. Purely emissive surfaces do not occlude; cutouts
 * block at their threshold; lit blends multiply the surviving fraction and the
 * walk continues, so a stack of panes accumulates. Grass keeps its own share so
 * a later draw toward the same light can reuse it without re-traversing the
 * cards; grassKnown is false when a non-grass blocker ended traversal with
 * grass beyond it unseen.
 */
public struct ShadowRay {
    /** Fraction of the light reaching the surface along this ray. */
    public float transmittance;
    /** Distance to the nearest blocker, or kPtNoOccluder; the penumbra estimate reads it. */
    public float occluderDistance;
    public float grassTransmittance;
    public float grassDistance;
    public bool grassKnown;
};

public static const float kPtNoOccluder = 1.0e6;

public ShadowRay ptShadowRay(float3 origin, float3 geometricNormal, float3 direction,
                             float lightDistance, uint inclusion) {
    ShadowRay result = { 1.0, kPtNoOccluder, 1.0, kPtNoOccluder, true };
    static const float kShadowEpsilon = 1e-3;
    if (lightDistance <= kShadowEpsilon) return result;
    RayDesc ray;
    ray.Origin = origin + geometricNormal * pushConstants.rayOriginOffset;
    ray.Direction = direction;
    ray.TMin = kShadowEpsilon;
    ray.TMax = max(0.0, lightDistance - kShadowEpsilon);
    // A generic query, not ACCEPT_FIRST_HIT: punch-through candidates must
    // stay visible to the loop; opaque geometry still commits in hardware.
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, inclusion, ray);
    // Index 0 is the merged scene, 1 the grass cards.
    float passed[2] = { 1.0, 1.0 };
    float nearest[2] = { kPtNoOccluder, kPtNoOccluder };
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        Candidate c = readCandidate(query);
        // Baked sky triangles occlude only when the fallback keeps them admitted.
        if (pushConstants.skyAvailable == 0 && (c.features & kMaterialSky) != 0) continue;
        // Sky-room scenery casts nothing. Reaches non-opaque candidates only -
        // cutout foliage is what this catches; a solid backdrop prop still
        // commits in hardware, the merged scene being one TLAS instance.
        if ((c.features & kMaterialBackdropNoShadow) != 0) continue;
        if (isProceduralSprite(c.features)) continue;
        if (instanceMaterials[c.materialIndex].surfaceType == kPtSurfaceUnlitTransparent) continue;
        bool attenuates = isTracedTransmissive(c.features) || isBlendedCoverage(c.features);
        if (!attenuates && !isCutout(c.features)) continue;
        float alpha = candidateAlpha(query, c);
        if (alpha < kPtAlphaTestEpsilon) continue;
        uint k = c.grass ? 1u : 0u;
        if (attenuates) {
            // An opaque texel still blocks completely: the alpha decides.
            passed[k] *= 1.0 - saturate(alpha);
            nearest[k] = min(nearest[k], query.CandidateTriangleRayT());
            if (passed[0] * passed[1] >= kPtAlphaTestEpsilon) continue;
        } else if (alpha < alphaTestThreshold(instanceMaterials[c.materialIndex])) {
            continue;
        } else {
            nearest[k] = min(nearest[k], query.CandidateTriangleRayT());
        }
        passed[k] = 0.0;
        break;
    }
    // Hardware commits opaque hits; the loop resolves cutouts; either may be nearer.
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        passed[0] = 0.0;
        nearest[0] = min(nearest[0], query.CommittedRayT());
    }
    result.transmittance = passed[0] * passed[1];
    result.occluderDistance = result.transmittance < 1.0 ? min(nearest[0], nearest[1]) : kPtNoOccluder;
    result.grassTransmittance = passed[1];
    result.grassDistance = nearest[1];
    result.grassKnown = passed[0] > 0.0 || passed[1] <= 0.0;
    return result;
}
