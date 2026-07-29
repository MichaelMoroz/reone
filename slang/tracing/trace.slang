/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Ray traversal. Exactly two kinds of ray exist:
//
//   ptTraceNearest - the normal ray: commits the nearest surface whose
//                    coverage passes the alpha test, transparent or not.
//                    Compositing is the path's job, not the traversal's.
//   ptShadowTransmittance - the shadow ray: accumulates transmittance to a
//                    light, attenuated by punch-through coverage; additive
//                    surfaces cast nothing and the sky never occludes.

import tracing.resources;
import tracing.rng;

public struct TraceHit {
    public bool hit;
    public uint instanceIndex;
    public InstanceMaterial material;
    public uint3 indices;
    public float3 weights;
    public float hitT;
    public float3x4 objectToWorld;
    public float3x4 worldToObject;
};

float candidateAlpha(InstanceMaterial material, uint3 indices, float3 weights) {
    if (material.mainTex == kPtNoTexture) return 1.0;
    float2 uv = float2(0.0);
    if (material.offUV1 != -1) {
        uv = ptTransformedUV(material, ptInterpolateFloat2(material, indices, material.offUV1, weights));
    }
    // Base level: coverage is a yes/no on the authored texel, and the alpha
    // test is sharper unfiltered.
    return bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(uv, 0.0).a;
}

/**
 * The normal ray. Opaque geometry commits in hardware. Non-opaque PBR
 * candidates alpha-test STOCHASTICALLY: a texel commits with probability
 * equal to its coverage, which resolves partial transparency inside the one
 * traversal - unbiased, and never a re-trace. The committed surface is
 * substance; whatever passes was never there. Additive surfaces always
 * commit (nearest wins) and the path passes through them explicitly.
 */
public TraceHit ptTraceNearest(RayDesc ray, inout uint rng) {
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        InstanceMaterial material = instanceMaterials[query.CandidateInstanceIndex()];
        if (material.surfaceType == kPtSurfaceUnlitTransparent) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        uint3 indices = ptTriangleIndices(material, query.CandidatePrimitiveIndex());
        float2 barycentrics = query.CandidateTriangleBarycentrics();
        float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        if (ptRandom01(rng) < candidateAlpha(material, indices, weights)) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    TraceHit result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    if (result.hit) {
        result.instanceIndex = query.CommittedInstanceIndex();
        result.material = instanceMaterials[result.instanceIndex];
        result.indices = ptTriangleIndices(result.material, query.CommittedPrimitiveIndex());
        float2 barycentrics = query.CommittedTriangleBarycentrics();
        result.weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        result.hitT = query.CommittedRayT();
        result.objectToWorld = query.CommittedObjectToWorld3x4();
        result.worldToObject = query.CommittedWorldToObject3x4();
    }
    return result;
}

/**
 * The shadow ray. Instance mask 0x1: the sky rides mask 0x2, visible to
 * normal rays that shade it as the environment but never an occluder - the
 * promoted sun sits beyond the dome, and with the full mask every sun ray
 * committed on it and the sun lit nothing at all. Additive surfaces are
 * emitters, not occluders; punch-through attenuates by coverage.
 */
public float ptShadowTransmittance(float3 origin, float3 geometricNormal, float3 direction,
                                   float lightDistance) {
    static const float kShadowEpsilon = 1e-3;
    if (lightDistance <= kShadowEpsilon) return 1.0;
    RayDesc ray;
    ray.Origin = origin + geometricNormal * pushConstants.rayOriginOffset;
    ray.Direction = direction;
    ray.TMin = kShadowEpsilon;
    ray.TMax = max(0.0, lightDistance - kShadowEpsilon);
    // A generic query, not ACCEPT_FIRST_HIT: punch-through candidates must
    // stay visible to the loop; opaque geometry still commits in hardware.
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0x1, ray);
    float transmittance = 1.0;
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        InstanceMaterial material = instanceMaterials[query.CandidateInstanceIndex()];
        if (material.surfaceType == kPtSurfaceUnlitTransparent) continue;
        if ((material.featureMask & kPtMaskPunchThrough) != 0) {
            uint3 indices = ptTriangleIndices(material, query.CandidatePrimitiveIndex());
            float2 barycentrics = query.CandidateTriangleBarycentrics();
            float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
            transmittance = clamp(transmittance * (1.0 - candidateAlpha(material, indices, weights)), 0.0, 1.0);
            if (transmittance < kShadowEpsilon) break;
        }
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
}
