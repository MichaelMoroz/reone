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

// The primary surface, reconstructed from the rasterized G-buffer rather
// than traced: primary visibility is already resolved before shading, so
// the kernel starts here and traces only outwards.
//
// The G-buffer stores the triangle id, so geometry is recomputed rather
// than approximated. Geometric normal owns geometry decisions (ray lift,
// hemisphere test, lobe clamp); the stored normal is shading only.

import lib.assemble;

import uniforms;
import lib.gbuf;
import lib.grasscard;
import lib.math;
import lib.scene_schema;
import lib.material_ops;
import tracing.resources;
import tracing.material;
import tracing.trace;

/**
 * A reconstructed primary, in the shape the rest of the kernel already speaks:
 * the hit record and the shading it would have gathered.
 */
public struct PrimarySurface {
    /** False at the sky: nothing was drawn, so there is no surface. */
    public bool covered;
    public HitGeometry hit;
    public SurfaceShading shading;
    /** World position of the surface; the guides and viewZ use this one. */
    public float3 worldPosition;
    /** World-space motion, previous minus current, for NRD. */
    public float3 motion;
    /** The raster pass's own current-minus-previous UV displacement. */
    public float2 screenMotion;
    /** Distance from the camera plane's ray origin to the surface. */
    public float distance;
};

bool ptPrimaryCardTriangle(uint triangleId) { return (triangleId & 0x80000000u) != 0; }

/** A card triangle id encodes cardId * cardTris + primitive under the tag bit. */
void ptDecodeCardTriangle(uint triangleId, out uint cardId, out uint primitive) {
    uint indexCount, indexStride;
    grassCardIndices.GetDimensions(indexCount, indexStride);
    uint cardTris = indexCount / 12;
    uint encoded = triangleId & 0x7fffffffu;
    cardId = encoded / cardTris;
    primitive = encoded % cardTris;
}

/**
 * Reconstructs the primary surface at one pixel.
 *
 * @p ray is the camera ray the pixel would have traced: its direction is still
 * needed - to face-forward the normal, to orient the BRDF and to size the ray
 * cone - but it is never traversed.
 */
public PrimarySurface ptReadPrimary(uint2 pixel, uint2 extent, RayDesc ray,
                                    float spreadAngle) {
    PrimarySurface primary;
    primary.covered = false;
    primary.worldPosition = ray.Origin;
    primary.motion = float3(0.0);
    primary.screenMotion = float2(0.0);
    primary.distance = 0.0;

    uint triangleId = gbufTriangleId.Load(int3(int2(pixel), 0)).r;
    if (triangleId == kNoTriangle) {
        // Nothing was drawn and nothing wrote depth - the same pixels both
        // raster resolves hand to the sky. See the caller.
        return primary;
    }
    uint materialIndex;
    float3 p0, p1, p2, prev0, prev1, prev2;
    bool cardTriangle = ptPrimaryCardTriangle(triangleId);
    uint3 indices = cardTriangle ? uint3(0) : ptTriangleIndicesById(triangleId);
    if (cardTriangle) {
        uint cardId, primitive;
        ptDecodeCardTriangle(triangleId, cardId, primitive);
        GrassCardVertex a, b, c;
        ptCardTriangleVertices(cardId, primitive, a, b, c);
        materialIndex = grassCardInstances[cardId].materialIndex.x;
        p0 = a.position; p1 = b.position; p2 = c.position;
        prev0 = a.prevPosition; prev1 = b.prevPosition; prev2 = c.prevPosition;
    } else {
        materialIndex = mergedMaterialIds[triangleId];
        p0 = mergedVertices[indices.x].position;
        p1 = mergedVertices[indices.y].position;
        p2 = mergedVertices[indices.z].position;
        prev0 = mergedVertices[indices.x].prevPosition;
        prev1 = mergedVertices[indices.y].prevPosition;
        prev2 = mergedVertices[indices.z].prevPosition;
    }

    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float3 viewPosition = gbufViewPosition(uv, gbufDepth.SampleLevel(uv, 0.0).r,
                                           globalUniforms.projectionInv);
    float3 world = mul(globalUniforms.viewInv, float4(viewPosition, 1.0)).xyz;

    // The triangle's own geometric normal, face-forwarded to the camera ray:
    // geometry is drawn with culling off. ptFillMergedHitGeometry repeats this
    // for a merged triangle; a card has no vertex normals or UVs to fill.
    float3 geometricNormal = normalize(cross(p1 - p0, p2 - p0));
    if (dot(geometricNormal, ray.Direction) > 0.0) geometricNormal = -geometricNormal;

    float3 eyeNormal = gbufDecodeEyeNormal(gbufEyeNormal.SampleLevel(uv, 0.0).xyz);
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);
    // Into the geometric hemisphere, PbrSurface::materialNormal's rule, so the
    // BRDF and the hemisphere tests never contradict at a silhouette.
    if (dot(normal, geometricNormal) < 0.0) normal = -normal;

    // Barycentrics within the triangle, exact on the plane; unread on the
    // primary path but kept consistent with `indices`.
    float3 weights = float3(1.0, 0.0, 0.0);
    float3 faceCross = cross(p1 - p0, p2 - p0);
    float faceAreaSquared = dot(faceCross, faceCross);
    if (faceAreaSquared > 1e-20) {
        float inverseArea = 1.0 / faceAreaSquared;
        weights.y = dot(cross(world - p0, p2 - p0), faceCross) * inverseArea;
        weights.z = dot(cross(p1 - p0, world - p0), faceCross) * inverseArea;
        weights.x = 1.0 - weights.y - weights.z;
    }

    InstanceMaterial material = instanceMaterials[materialIndex];
    float4 diffuseSample = gbufDiffuse.SampleLevel(uv, 0.0);
    float4 lightmapSample = gbufLightmap.SampleLevel(uv, 0.0);
    float3 selfIllumSample = gbufSelfIllum.SampleLevel(uv, 0.0).rgb;

    SurfaceShading shading;
    shading.terminatesPath = false;
    // lib/material_ops.slang's chain, the raster resolve's, over a G-buffer
    // texel; alpha stands in for the reflectance Odyssey has no channel for.
    MaterialSurface surface = resolveMaterial(material, diffuseSample.rgb, diffuseSample.a, 1.0,
                                              pushConstants.albedoGamma,
                                              pushConstants.roughnessFloor);
    shading.albedo = surface.albedo;
    shading.alpha = diffuseSample.a;
    shading.normal = normal;
    shading.metallic = surface.metallic;
    shading.roughness = surface.roughness;

    bool envmapped, hasLightmap, staticObject, shadowsEnabled, fogEnabled, thinSurface;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled, thinSurface);
    shading.thin = thinSurface;
    shading.envmapped = envmapped;
    shading.envStrength = ptEnvStrength(envmapped, shading.alpha);
    shading.f0 = material.overrideParams.z;
    shading.hasLightmap = hasLightmap;
    // White where there is no bake - see the note in tracing/material.slang.
    shading.lightmap = hasLightmap ? gammaToLinear(lightmapSample.rgb) : float3(1.0);

    // The same texel decoded as radiance: emission does not follow the albedo dial.
    const float3 radianceTexel = ptDecodeEmission(diffuseSample.rgb, material);

    shading.emission = assembleEmission(gammaToLinear(selfIllumSample), radianceTexel,
                                        material.curatedEmission, material.emission.x);

    // The two non-PBR surface models are radiance and end the path: nothing
    // behind them was rasterized. Same shading as tracing/material.slang.
    if (material.surfaceType == kPtSurfaceUnlitEmissive) {
        shading.emission = assembleSkyClass(radianceTexel, material.emission.x);
        shading.terminatesPath = true;
    } else if (material.surfaceType == kPtSurfaceUnlitTransparent) {
        // PURELY EMISSIVE, and in the G-buffer only when it is also cutout.
        shading.emission = ptUnlitTransparentEmission(material, radianceTexel);
        shading.terminatesPath = true;
    }

    float distance = length(world - ray.Origin);
    HitGeometry hit;
    hit.materialIndex = materialIndex;
    hit.indices = indices;
    hit.weights = weights;
    hit.rayDirection = ray.Direction;
    hit.hitT = distance;
    hit.coneWidth = spreadAngle * distance;
    hit.position = world;
    if (cardTriangle) {
        hit.card = true;
        hit.p0 = p0;
        hit.p1 = p1;
        hit.p2 = p2;
        hit.geometricNormal = geometricNormal;
        hit.shadowOrigin = hit.position;
        hit.uv = float2(0.0);
        hit.uv2 = float2(0.0);
        hit.uv0 = float2(0.0);
        hit.uv1 = float2(0.0);
        hit.uv2Vertex = float2(0.0);
    } else {
        ptFillMergedHitGeometry(hit);
    }

    primary.covered = true;
    primary.hit = hit;
    primary.shading = shading;
    primary.worldPosition = world;
    primary.screenMotion = gbufMotion.SampleLevel(uv, 0.0).xy;
    // Interpolated from the triangle's own previous vertices, as a traced hit
    // does: NRD wants world-space motion, and the raster's screen displacement
    // cannot represent movement along the normal.
    float3 prevWorld = prev0 * weights.x + prev1 * weights.y + prev2 * weights.z;
    primary.motion = shading.terminatesPath ? float3(0.0) : prevWorld - world;
    primary.distance = distance;
    return primary;
}
