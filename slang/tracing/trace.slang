/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Ray traversal. Exactly two kinds of ray exist:
//
//   ptTraceNearest - the normal ray: commits the nearest surface whose
//                    has nonzero coverage. Compositing is the path's job,
//                    not traversal's: punch-through coverage is binary,
//                    blended coverage is a transmitting surface.
//   ptTraceGuideNearest - the primary guide ray: commits only opaque or
//                    binary cutout coverage; blended and additive surfaces
//                    are invisible to it.
//   ptShadowTransmittance - the shadow ray: accumulates transmittance to a
//                    light, attenuated by punch-through coverage; additive
//                    surfaces cast nothing and the sky never occludes.

import lib.math;
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
    float4 texel = bindlessTextures[NonUniformResourceIndex(instanceMaterials[materialIndex].mainTex)].SampleLevel(uv, 0.0);
    // Premultiplied textures have no usable alpha channel; coverage lives in
    // the luma, exactly as particles.slang recovers it.
    float alpha = (instanceMaterials[materialIndex].featureMask & kPtMaskPremulAlpha) != 0
                      ? rgbToLuma(texel.rgb)
                      : texel.a;
    float vertexAlpha = mergedVertices[indices.x].color.a * weights.x +
                        mergedVertices[indices.y].color.a * weights.y +
                        mergedVertices[indices.z].color.a * weights.z;
    return alpha * vertexAlpha;
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

/** Cutout coverage is binary: in or out, nothing between. */
static const float kAlphaTestThreshold = 0.5;

bool isCutout(uint materialIndex) {
    return (instanceMaterials[materialIndex].featureMask & kPtMaskPunchThrough) != 0;
}

bool isBlendedCoverage(uint materialIndex) {
    return (instanceMaterials[materialIndex].featureMask & kPtMaskBlendedCoverage) != 0;
}

bool commitsCoverage(uint materialIndex, float alpha) {
    if (alpha < kPtAlphaTestEpsilon) return false;
    return !isCutout(materialIndex) || alpha >= kAlphaTestThreshold;
}

/**
 * The nearest ray. Opaque geometry commits in hardware. For non-opaque
 * coverage candidates, clear texels are skipped outright; cutouts commit at
 * their binary threshold and blended surfaces commit at every remaining
 * coverage value. Additive surfaces always commit and pass through later.
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
        if (!isCutout(materialIndex) && !isBlendedCoverage(materialIndex)) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
        float2 barycentrics = query.CandidateTriangleBarycentrics();
        float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        if (commitsCoverage(materialIndex, candidateAlpha(materialIndex, indices, weights))) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return committedHit(query);
}

/**
 * The primary guide ray. Hardware-opaque geometry still commits directly.
 * Of the non-opaque candidates, only binary cutout coverage is a guide
 * surface; blended coverage and additive emission are transparent. Other
 * terminating candidates retain their existing surface semantics.
 */
public TraceHit ptTraceGuideNearest(RayDesc ray) {
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint geometryIndex = query.CandidateGeometryIndex();
        uint materialIndex = materialIndexFor(geometryIndex, query.CandidatePrimitiveIndex());
        if (instanceMaterials[materialIndex].surfaceType == kPtSurfaceUnlitTransparent ||
            isBlendedCoverage(materialIndex)) continue;
        if (!isCutout(materialIndex)) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
        float2 barycentrics = query.CandidateTriangleBarycentrics();
        float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                                barycentrics.x, barycentrics.y);
        if (candidateAlpha(materialIndex, indices, weights) >= kAlphaTestThreshold) {
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
 * emitters, not occluders. Cutouts above their threshold fully block; blended
 * coverage attenuates continuously. Clear texels do neither.
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
        // Baked sky triangles are absent. This preserves the old sky-room
        // shadow behavior only when the safety fallback keeps them in the TLAS.
        if (pushConstants.skyAvailable == 0 &&
            (instanceMaterials[materialIndex].featureMask & kPtMaskSky) != 0) continue;
        if (instanceMaterials[materialIndex].surfaceType == kPtSurfaceUnlitTransparent) continue;
        if (isCutout(materialIndex) || isBlendedCoverage(materialIndex)) {
            uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
            float2 barycentrics = query.CandidateTriangleBarycentrics();
            float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
            float alpha = candidateAlpha(materialIndex, indices, weights);
            if (alpha < kPtAlphaTestEpsilon) continue;
            if (isCutout(materialIndex)) {
                if (alpha >= kAlphaTestThreshold) {
                    transmittance = 0.0;
                    break;
                }
            } else {
                transmittance = clamp(transmittance * (1.0 - alpha), 0.0, 1.0);
            }
            if (transmittance < kShadowEpsilon) break;
        }
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
}
