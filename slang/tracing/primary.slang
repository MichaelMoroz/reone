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

// The primary surface, read out of the rasterized G-buffer instead of traced.
//
// Every render mode rasterizes primary visibility before it shades anything,
// this one included, so a camera ray would be finding a second time the exact
// surface the geometry pass already resolved. It no longer does: the kernel
// starts at the surface this module reconstructs and traces only outwards.
//
// What that means for correctness is one invariant: the two representations
// must describe the same surfaces. They do by construction - the G-buffer's
// cutout gate in scene_draw.slang is the same test candidateAlpha /
// kAlphaTestThreshold apply in tracing/trace.slang, over the same merged
// geometry and the same material records - so alpha-tested coverage needs no
// special handling here at all. A cutout pixel simply carries whichever surface
// the raster pass let through.
//
// The G-buffer names the TRIANGLE each pixel came from, so the geometry is not
// approximated here - it is recomputed. The stored id indexes the same merged
// arrays traversal walks, and from it this module fetches the three world-space
// vertices and takes the geometric normal as their cross product, exactly as
// ptMakeHitGeometry does at a traced hit. The material comes from
// mergedMaterialIds at the same index, which is why there is no material-id
// target any more.
//
// The two normals therefore have the jobs they have everywhere else in the
// tracer: the GEOMETRIC one owns every geometry decision - the ray-origin lift,
// the next-event hemisphere test, the sampled lobe's below-surface clamp - and
// the stored, interpolated, normal-mapped one is the shading normal and nothing
// else. Reading rays off the shading normal was visible as artefacts across
// large smooth-shaded geometry, because an interpolated normal is not the
// surface the ray leaves.
//
// What the G-buffer still cannot supply, and what is done about it:
//
//   PREVIOUS WORLD POSITION. The raster pass writes a SCREEN displacement, and
//   one 2D vector does not determine a 3D one. NRD is owed world-space motion,

import lib.assemble;

import uniforms;
import lib.gbuf;
import lib.grasscard;
import lib.math;
import lib.scene_schema;
import lib.material_ops;
import tracing.brdf;
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

void ptPrimaryCardTriangleVertices(uint triangleId, out uint materialIndex, out float3 p0,
                            out float3 p1, out float3 p2, out float3 prev0,
                            out float3 prev1, out float3 prev2) {
    uint indexCount, indexStride;
    grassCardIndices.GetDimensions(indexCount, indexStride);
    uint cardTris = indexCount / 12;
    uint encoded = triangleId & 0x7fffffffu;
    uint cardId = encoded / cardTris;
    uint primitive = encoded % cardTris;
    GrassCardInstance card = grassCardInstances[cardId];
    GrassCardVertex a, b, c;
    ptCardTriangleVertices(cardId, primitive, a, b, c);
    p0 = a.position;
    p1 = b.position;
    p2 = c.position;
    prev0 = a.prevPosition;
    prev1 = b.prevPosition;
    prev2 = c.prevPosition;
    materialIndex = card.materialIndex.x;
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
    if (triangleId == kPtNoTriangle) {
        // Nothing was drawn and nothing wrote depth - the same pixels both
        // raster resolves hand to the sky. See the caller.
        return primary;
    }
    uint materialIndex;
    float3 p0, p1, p2, prev0, prev1, prev2;
    bool cardTriangle = ptPrimaryCardTriangle(triangleId);
    if (cardTriangle) {
        ptPrimaryCardTriangleVertices(triangleId, materialIndex, p0, p1, p2, prev0, prev1, prev2);
    } else {
        materialIndex = mergedMaterialIds[triangleId];
        uint3 mergedIndices = ptTriangleIndicesById(triangleId);
        p0 = mergedVertices[mergedIndices.x].position;
        p1 = mergedVertices[mergedIndices.y].position;
        p2 = mergedVertices[mergedIndices.z].position;
        prev0 = mergedVertices[mergedIndices.x].prevPosition;
        prev1 = mergedVertices[mergedIndices.y].prevPosition;
        prev2 = mergedVertices[mergedIndices.z].prevPosition;
    }

    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    // Depth is already the 0..1 clip space uses here, and NDC y is negated
    // because the geometry pass rasterises through a negative-height viewport.
    // Identical to reconstructViewPos in pbr_channels.slang, deliberately: the
    // two modes must place the same pixel at the same world point.
    float depth = gbufDepth.SampleLevel(uv, 0.0).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewHomogeneous = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    float3 viewPosition = viewHomogeneous.xyz / viewHomogeneous.w;
    float3 world = mul(globalUniforms.viewInv, float4(viewPosition, 1.0)).xyz;

    // The triangle, expanded from the stored id: the same three world-space
    // vertices traversal would have found, so the cross product below is the
    // GEOMETRIC normal itself and not a reconstruction of one. Nothing is
    // derived from depth taps or neighbouring pixels.
    uint3 indices = cardTriangle ? uint3(0) : ptTriangleIndicesById(triangleId);
    float3 geometricNormal = normalize(cross(p1 - p0, p2 - p0));
    // Face-forward to the camera ray, exactly as ptMakeHitGeometry does: the
    // merged geometry is drawn with culling off, so either winding can face us.
    if (dot(geometricNormal, ray.Direction) > 0.0) geometricNormal = -geometricNormal;

    float3 eyeNormal = normalize(2.0 * gbufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);
    // Flip into the geometric hemisphere rather than merely toward the viewer -
    // PbrSurface::materialNormal's rule. The geometric normal is already
    // face-forward, so agreeing with it implies facing the ray, and the BRDF and
    // the hemisphere tests can never contradict each other at a silhouette.
    if (dot(normal, geometricNormal) < 0.0) normal = -normal;

    // Barycentrics of the reconstructed point within its own triangle, from the
    // areas of the three sub-triangles about it. Exact for a point on the plane,
    // and the depth round trip puts it there up to its own precision. Nothing on
    // the primary path reads them - the G-buffer already carries the shaded
    // texel - but they belong to the indices beside them, so a later consumer
    // that does read them finds the two agreeing.
    float3 weights = float3(1.0, 0.0, 0.0);
    {
        float3 faceCross = cross(p1 - p0, p2 - p0);
        float faceAreaSquared = dot(faceCross, faceCross);
        if (faceAreaSquared > 1e-20) {
            float inverseArea = 1.0 / faceAreaSquared;
            weights.y = dot(cross(world - p0, p2 - p0), faceCross) * inverseArea;
            weights.z = dot(cross(p1 - p0, world - p0), faceCross) * inverseArea;
            weights.x = 1.0 - weights.y - weights.z;
        }
    }

    InstanceMaterial material = instanceMaterials[materialIndex];
    float4 diffuseSample = gbufDiffuse.SampleLevel(uv, 0.0);
    float4 lightmapSample = gbufLightmap.SampleLevel(uv, 0.0);
    float3 selfIllumSample = gbufSelfIllum.SampleLevel(uv, 0.0).rgb;

    SurfaceShading shading;
    shading.terminatesPath = false;
    // VulkanGBuffer stores the authored gamma-encoded texel with the vertex
    // colour already multiplied in; tracing shades linear.
    // The material chain is lib/material_ops.slang's, the same one the raster
    // resolve runs, over a G-buffer texel instead of a texture fetch. Alpha is
    // the reflectance stand-in Odyssey has no channel for; the closure
    // saturates rather than re-clamping to the floor, so an authored roughness
    // scale is not dead at both ends - see the note there.
    MaterialSurface surface = resolveMaterial(material, diffuseSample.rgb, diffuseSample.a,
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
    shading.envStrength = (envmapped ? (1.0 - shading.alpha) : 0.0) *
                          material.overrideParams.z;
    shading.hasLightmap = hasLightmap;
    // White where there is no bake - see the note in tracing/material.slang.
    shading.lightmap = hasLightmap ? ptGammaToLinear(lightmapSample.rgb) : float3(1.0);

    // The same texel decoded as radiance rather than as reflectance. Emission
    // is the texture's colour being emitted, not reflected, so it does not
    // follow the albedo dial - that one is a look control for how Odyssey art
    // sits under traced light, and it has no business changing how bright a
    // lamp or a skyline is.
    const float3 radianceTexel = ptDecodeEmission(diffuseSample.rgb);

    shading.emission = assembleEmission(ptGammaToLinear(selfIllumSample), radianceTexel,
                                        material.curatedEmission,
                                        pushConstants.emissiveIntensity);

    // The two non-PBR surface models, when one of them reaches the G-buffer.
    // Both are radiance rather than reflectance, and both end the path here:
    // whatever stood behind them was never rasterized, so there is nothing for
    // a continuation to find.
    if (material.surfaceType == kPtSurfaceUnlitEmissive) {
        // Backdrop imagery and, if a bake ever fails, the sky shell. Each on
        // its own dial - see tracing/material.slang, which shades the same two
        // kinds when a ray finds them instead of the camera.
        shading.emission = assembleSkyClass((material.featureMask & kPtMaskSky) != 0,
                                            radianceTexel, pushConstants.skyIntensity,
                                            pushConstants.backdropIntensity);
        shading.terminatesPath = true;
    } else if (material.surfaceType == kPtSurfaceUnlitTransparent) {
        // PURELY EMISSIVE, and in the G-buffer only when it is also cutout.
        shading.emission = max(material.selfIllumColor.rgb, float3(1.0)) * radianceTexel *
                           pushConstants.emissiveIntensity * material.overrideParams.y;
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
    // The reconstructed point itself. Nothing is nudged toward the camera: the
    // ray-origin lift is ptRayOffset along the geometric normal, which is now a
    // real one and therefore always points out of the surface.
    hit.position = world;
    hit.p0 = p0;
    hit.p1 = p1;
    hit.p2 = p2;
    hit.geometricNormal = geometricNormal;
    // The Hanika terminator shift stays unapplied. It is now recoverable - the
    // triangle and its vertex normals are both here - but it moves shadow-ray
    // origins, which is a separate change from getting the normal right.
    // The same shift every traced hit gets, convex clamp included - see
    // ptTerminatorShift. The G-buffer primary lost it when it lost the
    // triangle; the triangle is back, so this is back.
    hit.shadowOrigin = cardTriangle ? hit.position
                                    : ptTerminatorShift(hit.position, p0, p1, p2, indices, weights);
    // Recoverable from indices and weights; unread, because the G-buffer
    // already carries the texel these would have fetched.
    hit.uv = float2(0.0);
    hit.uv2 = float2(0.0);

    // Camera-distance fog for this vertex, gated by the surface's own fog
    // feature and the global switch, and never on a path-terminating surface -
    // the sky and the backdrops sit at infinity and carry their radiance whole.
    // Stored as the blend amount; the composite (or the denoiser-off assembly)
    // applies the colour. See lib.assemble.
    shading.fog = (fogEnabled && ptFogEnabled() && !shading.terminatesPath)
                      ? fogAmount(world, globalUniforms.cameraPosition.xyz,
                                  globalUniforms.fogNear, globalUniforms.fogFar)
                      : 0.0;

    primary.covered = true;
    primary.hit = hit;
    primary.shading = shading;
    primary.worldPosition = world;
    primary.screenMotion = gbufMotion.SampleLevel(uv, 0.0).xy;
    // The previous world position, interpolated from the triangle's own
    // vertices exactly as a traced hit does it - not inferred. NRD is
    // configured for world-space motion, and the inference this replaces could
    // not represent movement along the surface normal at all: it solved two
    // screen equations plus a tangent-plane constraint, so anything that left
    // that plane read as zero and the denoiser rejected history it should have
    // kept. Measured: per-frame instability over a traced sequence nearly
    // doubled when the primary started inferring this instead of reading it.
    float3 prevWorld = prev0 * weights.x + prev1 * weights.y + prev2 * weights.z;
    primary.motion = shading.terminatesPath ? float3(0.0) : prevWorld - world;
    primary.distance = distance;
    return primary;
}
