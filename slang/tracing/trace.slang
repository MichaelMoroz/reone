/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Ray traversal. Exactly two kinds of ray exist:
//
//   ptTraceNearest - the normal ray: commits the nearest surface whose
//                    coverage passes the alpha test, transparent or not.
//                    Compositing is the path's job, not the traversal's.
//                    Two coverage modes: the visibility ray alpha-tests
//                    against a fixed threshold, fully deterministic; bounce
//                    rays roll coverage stochastically.
//   ptShadowTransmittance - the shadow ray: accumulates transmittance to a
//                    light, attenuated by punch-through coverage; additive
//                    surfaces cast nothing and the sky never occludes.

import tracing.resources;
import tracing.rng;

public struct TraceHit {
    public bool hit;
    public uint materialIndex;
    public uint3 indices;
    public float3 weights;
    public float hitT;
};

float candidateAlpha(uint materialIndex, uint3 indices, float3 weights) {
    if (instanceMaterials[materialIndex].mainTex == kPtNoTexture) return 1.0;
    float2 uv = ptTransformedUV(materialIndex, mergedVertices[indices.x].uv1 * weights.x + mergedVertices[indices.y].uv1 * weights.y + mergedVertices[indices.z].uv1 * weights.z);
    // Base level: coverage is a yes/no on the authored texel, and the alpha
    // test is sharper unfiltered.
    return bindlessTextures[NonUniformResourceIndex(instanceMaterials[materialIndex].mainTex)].SampleLevel(uv, 0.0).a;
}

TraceHit committedHit(inout RayQuery<RAY_FLAG_NONE> query) {
    TraceHit result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    if (result.hit) {
        uint geometryIndex = query.CommittedGeometryIndex();
        result.materialIndex = materialIndexFor(geometryIndex, query.CommittedPrimitiveIndex());
        result.indices = ptTriangleIndices(geometryIndex, query.CommittedPrimitiveIndex());
        float2 barycentrics = query.CommittedTriangleBarycentrics();
        result.weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        result.hitT = query.CommittedRayT();
    }
    return result;
}

/** The visibility ray commits at half coverage: in or out, nothing between. */
static const float kAlphaTestThreshold = 0.5;

/**
 * The visibility ray. Opaque geometry commits in hardware. Non-opaque PBR
 * candidates alpha-test against a fixed threshold: fully deterministic, no
 * dither of any kind - TAA jitter is the frame's only stochastic input, and
 * the hard coverage edge is exactly what it antialiases. Additive surfaces
 * always commit (nearest wins); the path passes through them explicitly.
 */
public TraceHit ptTraceNearest(RayDesc ray) {
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint geometryIndex = query.CandidateGeometryIndex();
        uint materialIndex = materialIndexFor(geometryIndex, query.CandidatePrimitiveIndex());
        if (instanceMaterials[materialIndex].surfaceType == kPtSurfaceUnlitTransparent) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
        float2 barycentrics = query.CandidateTriangleBarycentrics();
        float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        if (candidateAlpha(materialIndex, indices, weights) >= kAlphaTestThreshold) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return committedHit(query);
}

/**
 * The bounce ray. Same traversal, but coverage rolls STOCHASTICALLY: a texel
 * commits with probability equal to its coverage, which resolves partial
 * transparency inside the one traversal - unbiased, never a re-trace, and
 * the denoiser integrates the noise with the rest of the lighting. The
 * committed surface is substance; whatever passes was never there.
 */
public TraceHit ptTraceNearest(RayDesc ray, inout uint rng) {
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint geometryIndex = query.CandidateGeometryIndex();
        uint materialIndex = materialIndexFor(geometryIndex, query.CandidatePrimitiveIndex());
        if (instanceMaterials[materialIndex].surfaceType == kPtSurfaceUnlitTransparent) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
        float2 barycentrics = query.CandidateTriangleBarycentrics();
        float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        if (ptRandom01(rng) < candidateAlpha(materialIndex, indices, weights)) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return committedHit(query);
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
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    float transmittance = 1.0;
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint geometryIndex = query.CandidateGeometryIndex();
        uint materialIndex = materialIndexFor(geometryIndex, query.CandidatePrimitiveIndex());
        if (instanceMaterials[materialIndex].surfaceType == kPtSurfaceUnlitTransparent) continue;
        if ((instanceMaterials[materialIndex].featureMask & kPtMaskSky) != 0) continue;
        if ((instanceMaterials[materialIndex].featureMask & kPtMaskPunchThrough) != 0) {
            uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
            float2 barycentrics = query.CandidateTriangleBarycentrics();
            float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
            transmittance = clamp(transmittance * (1.0 - candidateAlpha(materialIndex, indices, weights)), 0.0, 1.0);
            if (transmittance < kShadowEpsilon) break;
        }
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
}
