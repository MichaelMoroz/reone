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

// Ray traversal. Exactly two kinds of ray exist:
//
//   ptTraceNearest - the normal ray: commits the nearest surface a ray can
//                    see at all.
//   ptShadowRay - the shadow ray: visibility to a light and the distance at
//     which it was blocked, blocked by
//                    the same surfaces; the sky never occludes.
//
// Only two coverage kinds survive traversal, and they are named at every site:
//
//   CUTOUT coverage (kPtMaskPunchThrough) - a binary in-or-out alpha test on
//   the authored texel. Decided here, once, with no RNG; a committed texel is
//   solid substance from then on.
//
//   PURELY EMISSIVE surfaces (kPtSurfaceUnlitTransparent) - saber blades and
//   glow planes. They occupy no volume: the ray commits them so the path can
//   collect their radiance, then continues through unattenuated.
//
// Everything else non-opaque - a translucent surface that tints or attenuates
// what is behind it - is NOT TRACED. Traversal skips those candidates outright,
// so no ray, shadow ray included, can see one. That path used to shade the
// surface and carry the transmitted remainder onward; it cost more than it
// bought and a ray marcher is to replace it. The marcher's seam is exactly the
// two `isBlendedCoverage` skips below and the blended branches that used to sit
// beside the emissive ones in path_trace.slang.

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

/**
 * The threshold this material's texture authored, or the shared constant.
 *
 * The raster gate in scene_draw.slang resolves it the same way, and it has to:
 * the traced primary is read from the G-buffer raster produced, so a cutout
 * admitted by one and rejected by the other would put a hole in the frame that
 * neither pass drew.
 */
float ptAlphaTestThreshold(uint materialIndex) {
    float authored = instanceMaterials[materialIndex].alphaTest;
    return authored >= 0.0 ? authored : kAlphaTestThreshold;
}

/** Non-emissive translucency. Not traced: rays skip these candidates. */
bool isBlendedCoverage(uint materialIndex) {
    return (instanceMaterials[materialIndex].featureMask & kPtMaskBlendedCoverage) != 0;
}

/**
 * The nearest ray. Opaque geometry commits in hardware. Of the non-opaque
 * candidates, PURELY EMISSIVE surfaces always commit (the path collects their
 * radiance and passes through), CUTOUT coverage commits at its binary
 * threshold, and non-emissive translucency is skipped - it is not traced.
 */
public TraceHit ptTraceNearest(RayDesc ray) {
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint geometryIndex = query.CandidateGeometryIndex();
        uint materialIndex = materialIndexFor(geometryIndex, query.CandidatePrimitiveIndex());
        // PURELY EMISSIVE.
        if (instanceMaterials[materialIndex].surfaceType == kPtSurfaceUnlitTransparent) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        // The marcher's seam: what this skip drops is every translucent
        // surface that tints or attenuates what is behind it.
        if (isBlendedCoverage(materialIndex)) continue;
        if (!isCutout(materialIndex)) {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }
        // CUTOUT.
        uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
        float2 barycentrics = query.CandidateTriangleBarycentrics();
        float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        float alpha = candidateAlpha(materialIndex, indices, weights);
        if (alpha >= ptAlphaTestThreshold(materialIndex) && alpha >= kPtAlphaTestEpsilon) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return committedHit(query);
}

/**
 * The shadow ray. Instance mask 0x1: the sky rides mask 0x2, visible to
 * normal rays that shade it as the environment but never an occluder - the
 * promoted sun sits beyond the dome, and with the full mask every sun ray
 * committed on it and the sun lit nothing at all. PURELY EMISSIVE surfaces are
 * emitters, not occluders. CUTOUT coverage above its threshold fully blocks and
 * clear texels do not. Non-emissive translucency is skipped along with the
 * partial transmittance it used to accumulate, so visibility is now binary and
 * the walk stops at the first blocker.
 */
public struct ShadowRay {
    /** Fraction of the light that reaches the surface along this ray. */
    public float transmittance;
    /**
     * Distance from the shaded point to whatever blocked it, or kPtNoOccluder
     * when nothing did. The penumbra estimate is built from this: a blocker
     * close to the receiver casts a sharp edge however large the source, and a
     * distant one casts a soft edge however small.
     */
    public float occluderDistance;
};

public static const float kPtNoOccluder = 1.0e6;

public ShadowRay ptShadowRay(float3 origin, float3 geometricNormal, float3 direction,
                             float lightDistance) {
    ShadowRay result;
    result.transmittance = 1.0;
    result.occluderDistance = kPtNoOccluder;
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
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    float transmittance = 1.0;
    float cutoutDistance = kPtNoOccluder;
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint geometryIndex = query.CandidateGeometryIndex();
        uint materialIndex = materialIndexFor(geometryIndex, query.CandidatePrimitiveIndex());
        // Baked sky triangles are absent. This preserves the old sky-room
        // shadow behavior only when the safety fallback keeps them in the TLAS.
        if (pushConstants.skyAvailable == 0 &&
            (instanceMaterials[materialIndex].featureMask & kPtMaskSky) != 0) continue;
        // PURELY EMISSIVE surfaces cast nothing.
        if (instanceMaterials[materialIndex].surfaceType == kPtSurfaceUnlitTransparent) continue;
        // The marcher's seam: translucent occlusion is no longer traced.
        if (isBlendedCoverage(materialIndex)) continue;
        if (isCutout(materialIndex)) {
            uint3 indices = ptTriangleIndices(geometryIndex, query.CandidatePrimitiveIndex());
            float2 barycentrics = query.CandidateTriangleBarycentrics();
            float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
            float alpha = candidateAlpha(materialIndex, indices, weights);
            if (alpha < kPtAlphaTestEpsilon) continue;
            if (alpha >= ptAlphaTestThreshold(materialIndex)) {
                transmittance = 0.0;
                cutoutDistance = min(cutoutDistance, query.CandidateTriangleRayT());
                break;
            }
        }
    }
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        result.transmittance = 0.0;
        // Whichever blocked first: hardware commits opaque hits, the loop above
        // resolves cutouts, and either may be the nearer.
        result.occluderDistance = min(query.CommittedRayT(), cutoutDistance);
    } else {
        result.transmittance = transmittance;
        result.occluderDistance = transmittance < 1.0 ? cutoutDistance : kPtNoOccluder;
    }
    return result;
}
