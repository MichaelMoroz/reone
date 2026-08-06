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
//   so ptPrimaryWorldMotion below adds the one constraint the surface itself
//   supplies - see there.
//
//   VERTEX COLOUR AND UVs. Folded into the stored texel and gone - recoverable
//   from the triangle and the barycentrics below, but not needed, because the
//   texel they produced is what the G-buffer already carries. Only one consumer
//   notices: the flat-paint category override is a lerp toward an authored
//   colour, and applying it after the vertex colour has already been multiplied
//   in is not the same expression as before it.
//
//   ANY SURFACE THE RASTER PASS SKIPPED. The G-buffer holds opaque and cutout
//   coverage alone. Additive layers - saber blades, glow planes - are drawn by
//   the blended pass, which the traced mode does not run, so they are no longer
//   the primary of any pixel. Rays still collect them at every deeper vertex.

import uniforms;
import lib.gbuf;
import lib.math;
import lib.scene_schema;
import tracing.brdf;
import tracing.resources;
import tracing.material;

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

/**
 * World-space motion for a surface the G-buffer describes.
 *
 * The stored vector is a screen displacement, and NRD wants a world one. Two
 * equations, three unknowns - so the surface supplies the third: the previous
 * position is taken to lie in the tangent plane through the current one. That
 * makes the answer EXACT for static geometry under any camera motion (the
 * solution is the point itself) and for any motion within the surface plane,
 * and it makes motion ALONG the normal unrepresentable, where it reads as zero.
 *
 * Solved as a 3x3 rather than by inverting the previous view-projection: two
 * rows are the perspective divide written out, the third is the plane.
 */
float3 ptPrimaryWorldMotion(float3 position, float3 normal, float2 screenMotion) {
    float4 current = mul(globalUniforms.viewProjection, float4(position, 1.0));
    if (current.w <= 1e-6) return float3(0.0);
    // computeMotion stores 0.5 * (current - previous) in NDC.
    float2 previousNdc = current.xy / current.w - 2.0 * screenMotion;
    float4x4 previous = globalUniforms.prevViewProjection;
    float4 rowX = previous[0] - previousNdc.x * previous[3];
    float4 rowY = previous[1] - previousNdc.y * previous[3];
    float3 a0 = rowX.xyz;
    float3 a1 = rowY.xyz;
    float3 a2 = normal;
    float3 b = float3(-rowX.w, -rowY.w, dot(normal, position));
    float3 c0 = cross(a1, a2);
    float3 c1 = cross(a2, a0);
    float3 c2 = cross(a0, a1);
    float determinant = dot(a0, c0);
    // Degenerate exactly where the tangent plane is edge-on to the previous
    // camera ray, which is where the reconstruction would be meaningless.
    if (abs(determinant) < 1e-12) return float3(0.0);
    float3 previousPosition = float3(dot(b, c0), dot(b, c1), dot(b, c2)) / determinant;
    return previousPosition - position;
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
    uint materialIndex = mergedMaterialIds[triangleId];

    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    // Depth is already the 0..1 clip space uses here, and NDC y is negated
    // because the geometry pass rasterises through a negative-height viewport.
    // Identical to reconstructViewPos in pbr_resolve.slang, deliberately: the
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
    uint3 indices = ptTriangleIndicesById(triangleId);
    float3 p0 = mergedVertices[indices.x].position;
    float3 p1 = mergedVertices[indices.y].position;
    float3 p2 = mergedVertices[indices.z].position;
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
    shading.albedo = ptGammaToLinear(diffuseSample.rgb);
    shading.alpha = diffuseSample.a;
    shading.normal = normal;
    // Exactly PbrSurface::shade's stand-ins and curation, over a G-buffer texel
    // instead of a texture fetch. Alpha is only a reflectance control on
    // env-mapped materials, never metalness; roughness uses alpha everywhere.
    shading.metallic = 0.0;
    shading.roughness = clamp(shading.alpha, 0.2, 1.0);
    shading.albedo *= material.curatedAlbedoMul.rgb;
    shading.roughness = curatedChannel(material.curatedRoughA, material.curatedRoughB,
                                       shading.albedo, shading.roughness);
    shading.metallic = curatedChannel(material.curatedMetalA, material.curatedMetalB,
                                      shading.albedo, shading.metallic);
    shading.metallic = saturate(shading.metallic * material.overrideParams.w);
    // The one ordering the G-buffer changes: the flat paint lands on an albedo
    // the vertex colour has already scaled, where the hit record applied it
    // first. Identical wherever overrideColor.w is zero, which is every
    // material no category override has been pointed at.
    shading.albedo = lerp(shading.albedo, ptGammaToLinear(material.overrideColor.rgb),
                          material.overrideColor.w);
    if (material.overrideParams.x >= 0.0) shading.roughness = material.overrideParams.x;
    shading.roughness = clamp(shading.roughness * material.roughnessScale, 0.2, 1.0);

    bool envmapped, hasLightmap, staticObject, shadowsEnabled, fogEnabled;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled);
    shading.envmapped = envmapped;
    shading.envStrength = (envmapped ? (1.0 - shading.alpha) : 0.0) *
                          material.overrideParams.z;
    shading.hasLightmap = hasLightmap;
    shading.lightmap = hasLightmap ? ptGammaToLinear(lightmapSample.rgb) : float3(0.0);

    float3 emission = ptGammaToLinear(selfIllumSample);
    if (material.curatedEmission.w > 1.5) {
        emission = material.curatedEmission.rgb;
    } else if (material.curatedEmission.w > 0.5) {
        emission *= material.curatedEmission.rgb;
    }
    shading.emission = emission * shading.albedo * pushConstants.emissiveIntensity;

    // The two non-PBR surface models, when one of them reaches the G-buffer.
    // Both are radiance rather than reflectance, and both end the path here:
    // whatever stood behind them was never rasterized, so there is nothing for
    // a continuation to find.
    if (material.surfaceType == kPtSurfaceUnlitEmissive) {
        // Prelit imagery and, if a bake ever fails, the sky shell.
        bool sky = (material.featureMask & kPtMaskSky) != 0;
        shading.emission = shading.albedo * (sky ? pushConstants.skyIntensity : 1.0);
        shading.terminatesPath = true;
    } else if (material.surfaceType == kPtSurfaceUnlitTransparent) {
        // PURELY EMISSIVE, and in the G-buffer only when it is also cutout.
        shading.emission = max(material.selfIllumColor.rgb, float3(1.0)) * shading.albedo *
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
    hit.shadowOrigin = hit.position;
    // Recoverable from indices and weights; unread, because the G-buffer
    // already carries the texel these would have fetched.
    hit.uv = float2(0.0);
    hit.uv2 = float2(0.0);

    primary.covered = true;
    primary.hit = hit;
    primary.shading = shading;
    primary.worldPosition = world;
    primary.screenMotion = gbufMotion.SampleLevel(uv, 0.0).xy;
    // The tangent plane the solve closes on is the triangle's own plane, so the
    // constraint is now exact rather than nearly so; a normal-mapped normal
    // tilted the plane away from the surface the point actually lies in.
    primary.motion = shading.terminatesPath
                         ? float3(0.0)
                         : ptPrimaryWorldMotion(world, geometricNormal,
                                                primary.screenMotion);
    primary.distance = distance;
    return primary;
}
