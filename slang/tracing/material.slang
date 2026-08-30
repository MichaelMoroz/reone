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

// The surface model. Three material kinds exist, chosen CPU-side at TLAS
// admission and read from InstanceMaterial::surfaceType:
//
//   PbrSurface             - parametrized PBR with optional emission and
//                            lightmap; blended alpha transmits continuously
//   UnlitTransparentSurface - additive radiance (saber blades, glow decals);
//                            the ray passes through unattenuated
//   UnlitEmissiveSurface   - sky and prelit imagery; radiance, path ends
//
// Each conforms to ISurfaceModel: one shading function gathering parameters,
// one ray sampling function producing the continuation. ptShade/ptSampleRay
// dispatch on the type; everything downstream is type-blind.

import lib.assemble;

import lib.math;
import lib.grasscard;
import lib.scene_schema;
import lib.material_ops;
import uniforms;
import tracing.rng;
import lib.brdf;
import lib.surface_model;
import tracing.resources;
import tracing.trace;

/** Everything about the hit point the surface model needs. */
public struct HitGeometry {
    public uint materialIndex;
    public uint3 indices;
    public float3 weights;
    public float3 rayDirection;
    public float hitT;
    public float coneWidth;
    public float3 position;        // world
    public float3 p0, p1, p2;      // world triangle corners
    public float3 geometricNormal; // world, face-forward to the ray
    public float3 shadowOrigin;    // Hanika terminator shift
    public float2 uv, uv2;
    public bool card;
    public float2 uv0, uv1, uv2Vertex;
};

/** The gathered surface parameters, uniform across surface types. */
public struct SurfaceShading {
    public float3 albedo;
    public float alpha;
    public float3 normal;    // material (shading) normal - BRDF only
    public float roughness;
    public float metallic;
    public float3 emission;  // radiance added at this vertex, dial-scaled
    public float3 lightmap;
    public bool hasLightmap;
    /** The category's specular weight on the dielectric Fresnel curve; 0 is no lobe. */
    public float specular;
    /** The category's dielectric reflectance at normal incidence. */
    public float f0;
    public bool terminatesPath;
    /**
     * A surface with no thickness - a leaf, cloth, a blade of grass.
     *
     * It is lit from whichever side the light is on, and the shadow ray leaves
     * from that side rather than from the side that was hit. Shading one as a
     * solid puts the whole far half in its own shadow, which is what an alpha
     * cutout has always looked like and what it never should have.
     */
    public bool thin;
};

float4 vertexColor(HitGeometry hit) {
    if (hit.card) return float4(1.0);
    return mergedVertices[hit.indices.x].color * hit.weights.x +
           mergedVertices[hit.indices.y].color * hit.weights.y +
           mergedVertices[hit.indices.z].color * hit.weights.z;
}

/** A sampled continuation ray. */
public struct PathSample {
    public float3 direction;
    public float3 weight;      // throughput multiplier for this lobe choice
    /**
     * weight with the surface's own material response divided out, in closed
     * form. The primary vertex needs exactly this, and computing it as a
     * ratio of two nearly-equal small numbers produces fireflies wherever the
     * response approaches zero - a black texel divides a tiny numerator by a
     * tiny guard. The lobe knows the answer analytically, so it says so.
     */
    public float3 demodulatedWeight;
    public bool specularLobe;
    public bool passThrough;   // continuation, not a scattering event
    /**
     * A continuation that still costs a bounce.
     *
     * Additive light occupies no volume, so passing through it is free and
     * bounded only by a step cap. Passing through glass is not free: the ray
     * has interacted with a surface and the path is one event shorter for it.
     */
    public bool costsScatter;
    public bool terminate;
};

public interface ISurfaceModel {
    public SurfaceShading shade(HitGeometry hit);
    public PathSample sampleRay(HitGeometry hit, SurfaceShading shading, float3 view, inout uint rng);
};

/**
 * The terminator shift (Hanika, RT Gems II): a ray origin lifted from the flat
 * triangle onto the smooth surface the vertex normals describe. Low-poly
 * geometry with aggressively smoothed normals - all of KotOR - otherwise
 * shadows itself along the terminator in faceted bands, and the alternative, a
 * larger global ray bias, detaches every contact shadow to fix one artefact.
 *
 * The min(0, .) is the CONVEX CLAMP and is not optional: each vertex term
 * lifts the point only when it lies below that vertex's tangent plane. Without
 * it a concave patch - normals converging rather than splaying - displaces the
 * origin INTO the mesh, which is the artefact this exists to prevent, worse.
 *
 * It moves an origin and nothing else. The silhouette stays polygonal and the
 * shading normal still claims a curvature the mesh does not have; this fixes
 * the self-intersection that claim causes, not the claim.
 */
public float3 ptTerminatorShift(float3 position, float3 p0, float3 p1, float3 p2,
                                uint3 indices, float3 weights) {
    // A degenerate vertex normal means the mesh carries none worth trusting.
    if (dot(mergedVertices[indices.x].normal, mergedVertices[indices.x].normal) <= 1e-10) {
        return position;
    }
    float3 n0 = normalize(mergedVertices[indices.x].normal);
    float3 n1 = normalize(mergedVertices[indices.y].normal);
    float3 n2 = normalize(mergedVertices[indices.z].normal);
    float3 u0 = position - p0;
    float3 u1 = position - p1;
    float3 u2 = position - p2;
    u0 -= min(0.0, dot(u0, n0)) * n0;
    u1 -= min(0.0, dot(u1, n1)) * n1;
    u2 -= min(0.0, dot(u2, n2)) * n2;
    return weights.x * (p0 + u0) + weights.y * (p1 + u1) + weights.z * (p2 + u2);
}

/**
 * The merged-triangle half of a HitGeometry, given the header (material,
 * indices, weights, ray direction, position): corners, the geometric normal
 * face-forwarded to the ray, the UVs and the terminator-shifted shadow origin.
 * The traced hit and the G-buffer primary both build their surface from this.
 */
public void ptFillMergedHitGeometry(inout HitGeometry hit) {
    uint3 indices = hit.indices;
    float3 weights = hit.weights;
    hit.card = false;
    hit.p0 = mergedVertices[indices.x].position;
    hit.p1 = mergedVertices[indices.y].position;
    hit.p2 = mergedVertices[indices.z].position;
    hit.geometricNormal = normalize(cross(hit.p1 - hit.p0, hit.p2 - hit.p0));
    if (dot(hit.geometricNormal, hit.rayDirection) > 0.0) hit.geometricNormal = -hit.geometricNormal;
    hit.uv0 = mergedVertices[indices.x].uv1;
    hit.uv1 = mergedVertices[indices.y].uv1;
    hit.uv2Vertex = mergedVertices[indices.z].uv1;
    hit.uv = ptTransformedUV(hit.materialIndex, hit.uv0 * weights.x + hit.uv1 * weights.y + hit.uv2Vertex * weights.z);
    hit.uv2 = mergedVertices[indices.x].uv2 * weights.x + mergedVertices[indices.y].uv2 * weights.y + mergedVertices[indices.z].uv2 * weights.z;
    hit.shadowOrigin = ptTerminatorShift(hit.position, hit.p0, hit.p1, hit.p2, indices, weights);
}

/** Builds the HitGeometry for a committed hit. */
public HitGeometry ptMakeHitGeometry(TraceHit trace, RayDesc ray, float coneWidth) {
    uint3 indices = trace.indices;
    float3 weights = trace.weights;
    float3 rayDirection = ray.Direction;
    HitGeometry hit;
    hit.materialIndex = trace.materialIndex;
    hit.indices = indices;
    hit.weights = weights;
    hit.rayDirection = rayDirection;
    hit.hitT = trace.hitT;
    hit.coneWidth = coneWidth;
    hit.position = ray.Origin + rayDirection * trace.hitT;
    hit.card = trace.cardId != kPtNoCard;
    if (hit.card) {
        GrassCardVertex v0, v1, v2;
        ptCardTriangleVertices(trace.cardId, trace.primitiveIndex, v0, v1, v2);
        hit.p0 = v0.position;
        hit.p1 = v1.position;
        hit.p2 = v2.position;
        hit.geometricNormal = normalize(v0.normal * weights.x + v1.normal * weights.y + v2.normal * weights.z);
        if (dot(hit.geometricNormal, rayDirection) > 0.0) hit.geometricNormal = -hit.geometricNormal;
        hit.uv0 = v0.uv;
        hit.uv1 = v1.uv;
        hit.uv2Vertex = v2.uv;
        hit.uv = ptTransformedUV(trace.materialIndex,
                                 v0.uv * weights.x + v1.uv * weights.y + v2.uv * weights.z);
        hit.uv2 = v0.lightmapUV * weights.x + v1.lightmapUV * weights.y + v2.lightmapUV * weights.z;
        hit.shadowOrigin = hit.position;
        return hit;
    }
    ptFillMergedHitGeometry(hit);
    return hit;
}

/**
 * True anisotropic footprint: the circular ray cone projected onto the
 * triangle plane along the ray is an ellipse, its axes taken through the
 * triangle's affine world-to-UV map and handed to the hardware as SampleGrad
 * gradients. Any basis perpendicular to the ray serves - a circular cone has
 * no preferred orientation - so the same math holds at bounce hits.
 */
struct UvGradients {
    float2 x;
    float2 y;
};

UvGradients anisotropicGradients(HitGeometry hit) {
    UvGradients gradients;
    gradients.x = float2(0.0);
    gradients.y = float2(0.0);
    if (hit.coneWidth <= 0.0) return gradients;
    float3 basis1 = normalize(cross(hit.rayDirection,
                                    abs(hit.rayDirection.z) < 0.99 ? float3(0.0, 0.0, 1.0)
                                                                   : float3(0.0, 1.0, 0.0)));
    float3 basis2 = cross(hit.rayDirection, basis1);
    float3 e1 = hit.p1 - hit.p0;
    float3 e2 = hit.p2 - hit.p0;
    float3 planeNormal = cross(e1, e2);
    float planeNormal2 = dot(planeNormal, planeNormal);
    float ndotd = dot(planeNormal, hit.rayDirection);
    if (planeNormal2 < 1e-18 || abs(ndotd) < 1e-6) return gradients;
    float3 axisX = (basis1 - hit.rayDirection * (dot(planeNormal, basis1) / ndotd)) * hit.coneWidth;
    float3 axisY = (basis2 - hit.rayDirection * (dot(planeNormal, basis2) / ndotd)) * hit.coneWidth;
    float2 uvA = hit.uv0;
    float2 duv1 = hit.uv1 - uvA;
    float2 duv2 = hit.uv2Vertex - uvA;
    float invPlane = 1.0 / planeNormal2;
    gradients.x = (dot(cross(axisX, e2), planeNormal) * invPlane) * duv1 +
                  (dot(cross(e1, axisX), planeNormal) * invPlane) * duv2;
    gradients.y = (dot(cross(axisY, e2), planeNormal) * invPlane) * duv1 +
                  (dot(cross(e1, axisY), planeNormal) * invPlane) * duv2;
    return gradients;
}

/**
 * Isotropic ray-cone LOD (RT Gems ch. 20, simplified) for call sites that
 * cannot afford gradients: transparency candidates during traversal.
 */
public float ptRayConeLod(InstanceMaterial material, HitGeometry hit) {
    if (material.mainTex == kNoTexture) return 0.0;
    float3 q0 = hit.p0;
    float3 q1 = hit.p1;
    float3 q2 = hit.p2;
    float2 uv0 = hit.uv0;
    float2 uv1 = hit.uv1;
    float2 uv2 = hit.uv2Vertex;
    float ta = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));
    float pa = length(cross(q1 - q0, q2 - q0));
    uint texWidth, texHeight;
    bindlessTextures[NonUniformResourceIndex(material.mainTex)].GetDimensions(texWidth, texHeight);
    float density = ta * float(texWidth) * float(texHeight) / max(pa, 1e-9);
    return max(0.0, 0.5 * log2(max(density, 1e-12)) + log2(max(hit.coneWidth, 1e-9)));
}

/** Additive radiance of a purely emissive surface: authored as light even without a self-illum controller. */
public float3 ptUnlitTransparentEmission(InstanceMaterial material, float3 radianceTexel) {
    return max(material.selfIllumColor.rgb, float3(1.0)) * radianceTexel * material.emission.x;
}

public struct PbrSurface : ISurfaceModel {
    public SurfaceShading shade(HitGeometry hit) {
        InstanceMaterial material = instanceMaterials[hit.materialIndex];
        SurfaceShading shading;
        shading.terminatesPath = false;
        float4 texel = float4(1.0);
        if (material.mainTex != kNoTexture) {
            UvGradients gradients = anisotropicGradients(hit);
            texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleGrad(hit.uv, gradients.x, gradients.y);
        }
        // The same texel plays two roles - reflectance for the light it bounces,
        // radiance for the light it emits - and only the first follows the albedo
        // dial. Premultiplied colour has its coverage divided back out of both.
        float coverage = texelCoverage(material, texel);
        float unpremultiply = (material.featureMask & kMaterialPremulAlpha) != 0
                                  ? 1.0 / max(1e-4, coverage) : 1.0;
        MaterialSurface surface = resolveMaterial(material, texel.rgb, coverage, unpremultiply,
                                                  pushConstants.albedoGamma);
        float3 radianceTexel = ptDecodeEmission(texel.rgb, material) * unpremultiply;
        float4 color = vertexColor(hit);
        shading.albedo = surface.albedo * color.rgb;
        shading.alpha = coverage * color.a;
        shading.roughness = surface.roughness;
        shading.metallic = surface.metallic;

        shading.normal = materialNormal(hit, hit.materialIndex);
        shading.specular = surface.specular;
        shading.f0 = surface.f0;
        shading.hasLightmap = (material.featureMask & kMaterialLightmap) != 0;
        // White where a surface declares a bake it has no texture for: zero is
        // a lightmap that absorbs everything, and the lightmaps toggle clears
        // the texture while leaving the bit set.
        shading.lightmap = float3(1.0);
        if (shading.hasLightmap && material.lightmap != kNoTexture) {
            shading.lightmap = gammaToLinear(
                bindlessTextures[NonUniformResourceIndex(material.lightmap)].SampleLevel(hit.uv2, 0.0).rgb);
        }
        shading.emission = assembleEmission(gammaToLinear(material.selfIllumColor.rgb), radianceTexel,
                                            material.curatedEmission, material.emission.x);
        return shading;
    }

    public PathSample sampleRay(HitGeometry hit, SurfaceShading shading, float3 view, inout uint rng) {
        PathSample sample;
        // Non-emissive translucency is not traced. A blended surface used to
        // shade here and carry the remainder of the ray onward, accumulating
        // transmission through every puff and pane it crossed; that path cost
        // more than it bought and a ray marcher is to replace it. Rays no
        // longer see such a surface at all - traversal skips it - so the only
        // coverage this model still resolves is CUTOUT, which traversal has
        // already decided as a binary in-or-out and which therefore scatters
        // exactly like opaque PBR from here on.
        sample.terminate = false;
        sample.passThrough = false;
        sample.costsScatter = false;
        sample.demodulatedWeight = sample.weight;
        sample.specularLobe = false;
        // Cutout coverage was resolved deterministically in traversal: a
        // committed texel is solid substance. No re-roll, no re-trace.
        // Lobe choice follows the energy each layer actually carries at this
        // view angle, so a weightless lobe is never traced.
        SpecularLayer layer = materialSpecular(shading.albedo, shading.metallic, shading.specular, shading.f0);
        float NdotV = max(0.0, dot(shading.normal, view));
        SpecularEnergy energy = specularEnergy(layer, NdotV, shading.roughness);
        float specularProbability = clamp(luminance(energy.albedo), 1e-3, 1.0 - 1e-3);
        if (ptRandom01(rng) < specularProbability) {
            float3 half = ptGgxVisibleHalfVector(shading.normal, view, shading.roughness,
                                                 float2(ptRandom01(rng), ptRandom01(rng)));
            sample.direction = normalize(reflect(-view, half));
            // Visible-normal sampling leaves F * G2/G1, and with the separable
            // Smith form this codebase uses that ratio is G1 of the outgoing
            // direction alone. Bounded by F, unlike the NDF-sampled weight this
            // replaces, which owed a factor that diverges at grazing angles.
            float NdotL = max(0.0, dot(shading.normal, sample.direction));
            float VdotH = max(0.0, dot(view, half));
            sample.weight = specularFresnel(layer, VdotH) * energy.compensation *
                            geometrySchlick(NdotL, punctualSmithK(shading.roughness)) /
                            specularProbability;
            sample.demodulatedWeight = sample.weight;
            sample.specularLobe = true;
        } else {
            sample.direction = ptCosineHemisphere(shading.normal,
                                                   float2(ptRandom01(rng), ptRandom01(rng)));
            // The response is the primary's diffFactor exactly; only the
            // lobe-selection probability survives demodulation.
            sample.weight = diffuseResponse(shading.albedo, shading.metallic, energy) /
                            (1.0 - specularProbability);
            sample.demodulatedWeight = float3(1.0 / (1.0 - specularProbability));
        }
        // A smooth normal can sample below the geometric surface at a
        // silhouette. Reflect that lobe into the geometric hemisphere rather
        // than immediately self-intersecting the same triangle.
        if (dot(sample.direction, hit.geometricNormal) < 0.0) {
            sample.direction = normalize(sample.direction -
                2.0 * dot(sample.direction, hit.geometricNormal) * hit.geometricNormal);
        }
        return sample;
    }

    float3 materialNormal(HitGeometry hit, uint materialIndex) {
        if (hit.card) return hit.geometricNormal;
        // The two normals have different jobs: the geometric normal owns
        // every geometry decision, the interpolated material normal exists
        // only inside the BRDF. Authored normals are object-space and take
        // the inverse-transpose; the geometric fallback is already world.
        bool hasAuthoredNormals = dot(mergedVertices[hit.indices.x].normal, mergedVertices[hit.indices.x].normal) > 1e-10;
        float3 shadingNormal = hit.geometricNormal;
        if (hasAuthoredNormals) {
            shadingNormal = normalize(mergedVertices[hit.indices.x].normal * hit.weights.x + mergedVertices[hit.indices.y].normal * hit.weights.y + mergedVertices[hit.indices.z].normal * hit.weights.z);
        }
        if (dot(mergedVertices[hit.indices.x].tanSpaceNormal, mergedVertices[hit.indices.x].tanSpaceNormal) > 1e-10) {
            // offTanSpace is bitangent, tangent, tangent-space normal in that
            // order. The third vector also guards corrupt tangent frames.
            float3 interpolatedBitangent = mergedVertices[hit.indices.x].bitangent * hit.weights.x + mergedVertices[hit.indices.y].bitangent * hit.weights.y + mergedVertices[hit.indices.z].bitangent * hit.weights.z;
            float3 tangent = mergedVertices[hit.indices.x].tangent * hit.weights.x + mergedVertices[hit.indices.y].tangent * hit.weights.y + mergedVertices[hit.indices.z].tangent * hit.weights.z;
            float3 tangentSpaceNormal = mergedVertices[hit.indices.x].tanSpaceNormal * hit.weights.x + mergedVertices[hit.indices.y].tanSpaceNormal * hit.weights.y + mergedVertices[hit.indices.z].tanSpaceNormal * hit.weights.z;
            tangent = normalize(tangent - shadingNormal * dot(tangent, shadingNormal));
            float handedness = dot(cross(shadingNormal, tangent), interpolatedBitangent) < 0.0 ? -1.0 : 1.0;
            float3 bitangent = cross(shadingNormal, tangent) * handedness;
            if (dot(tangentSpaceNormal, tangentSpaceNormal) >= 1e-10 &&
                dot(tangent, tangent) >= 1e-10 && dot(bitangent, bitangent) >= 1e-10) {
                UvGradients gradients = anisotropicGradients(hit);
                if ((instanceMaterials[materialIndex].featureMask & uint(kFeatureNormalMap)) != 0 && instanceMaterials[materialIndex].normalMap != kNoTexture) {
                    float3 n = bindlessTextures[NonUniformResourceIndex(instanceMaterials[materialIndex].normalMap)].SampleGrad(hit.uv, gradients.x, gradients.y).rgb * 2.0 - 1.0;
                    shadingNormal = normalize(tangent * n.x + bitangent * n.y + shadingNormal * n.z);
                } else if ((instanceMaterials[materialIndex].featureMask & uint(kFeatureBumpMap)) != 0 && instanceMaterials[materialIndex].bumpMapArray != kNoTexture) {
                    uint bumpWidth, bumpHeight, bumpLayers;
                    bindlessTextureArrays[NonUniformResourceIndex(instanceMaterials[materialIndex].bumpMapArray)].GetDimensions(bumpWidth, bumpHeight, bumpLayers);
                    float2 texel = 1.0 / float2(max(bumpWidth, 1u), max(bumpHeight, 1u));
                    float layer = float(instanceMaterials[materialIndex].bumpMapFrame);
                    float height = bindlessTextureArrays[NonUniformResourceIndex(instanceMaterials[materialIndex].bumpMapArray)].SampleLevel(float3(hit.uv, layer), 0.0).r;
                    // Compute dispatches have no screen-space ddx/ddy; these
                    // explicit texel differences approximate the raster path.
                    float dX = instanceMaterials[materialIndex].bumpMapScale * (bindlessTextureArrays[NonUniformResourceIndex(instanceMaterials[materialIndex].bumpMapArray)].SampleLevel(float3(hit.uv + float2(texel.x, 0.0), layer), 0.0).r - height);
                    float dY = instanceMaterials[materialIndex].bumpMapScale * (bindlessTextureArrays[NonUniformResourceIndex(instanceMaterials[materialIndex].bumpMapArray)].SampleLevel(float3(hit.uv + float2(0.0, texel.y), layer), 0.0).r - height);
                    float3 n = normalize(float3(-dX, -dY, 1.0));
                    shadingNormal = normalize(tangent * n.x + bitangent * n.y + shadingNormal * n.z);
                }
            }
        }
        float3 worldNormal = shadingNormal;
        // Flip into the geometric hemisphere, not merely toward the viewer:
        // the geometric normal is already face-forward, so agreement with it
        // implies facing the ray, and the BRDF and the hemisphere tests can
        // never contradict each other at a silhouette.
        if (dot(worldNormal, hit.geometricNormal) < 0.0) worldNormal = -worldNormal;
        return worldNormal;
    }
};

public struct UnlitTransparentSurface : ISurfaceModel {
    public SurfaceShading shade(HitGeometry hit) {
        InstanceMaterial material = instanceMaterials[hit.materialIndex];
        SurfaceShading shading;
        shading.terminatesPath = false;
        // Radiance, not reflectance: this model is additive light, so the texel
        // takes the emission decode rather than the albedo one.
        shading.albedo = ptDecodeEmission(material.diffuseColor.rgb, material);
        shading.alpha = 1.0;
        if (material.mainTex != kNoTexture) {
            float lod = ptRayConeLod(material, hit);
            float4 sample = bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(hit.uv, lod);
            shading.albedo = ptDecodeEmission(sample.rgb, material);
            shading.alpha = sample.a;
        }
        float4 color = vertexColor(hit);
        shading.albedo *= color.rgb;
        shading.alpha *= color.a;
        shading.normal = hit.geometricNormal;
        shading.roughness = 1.0;
        shading.metallic = 0.0;
        shading.hasLightmap = false;
        shading.lightmap = float3(1.0);
        shading.specular = 0.0;
        shading.f0 = 0.0;
        shading.emission = ptUnlitTransparentEmission(material, shading.albedo);
        return shading;
    }

    public PathSample sampleRay(HitGeometry hit, SurfaceShading shading, float3 view, inout uint rng) {
        // Additive light occupies no volume: the ray continues as if the
        // surface were not there. Its radiance was already paid in shade().
        PathSample sample;
        sample.direction = hit.rayDirection;
        sample.weight = float3(1.0);
        sample.demodulatedWeight = sample.weight;
        sample.specularLobe = false;
        sample.passThrough = true;
        sample.costsScatter = false;
        sample.terminate = false;
        return sample;
    }
};

public struct UnlitEmissiveSurface : ISurfaceModel {
    public SurfaceShading shade(HitGeometry hit) {
        InstanceMaterial material = instanceMaterials[hit.materialIndex];
        SurfaceShading shading;
        shading.albedo = float3(1.0);
        shading.alpha = 1.0;
        if (material.mainTex != kNoTexture) {
            float lod = ptRayConeLod(material, hit);
            // The emission decode: sky and backdrop imagery are radiance, and
            // the albedo dial has no say over how bright a sky is.
            shading.albedo = ptDecodeEmission(
                bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(hit.uv, lod).rgb, material);
        }
        shading.normal = hit.geometricNormal;
        shading.roughness = 1.0;
        shading.metallic = 0.0;
        shading.hasLightmap = false;
        shading.lightmap = float3(1.0);
        shading.specular = 0.0;
        shading.f0 = 0.0;
        // The texture is the radiance, and each kind takes its own dial: the
        // sky shell, and the painted backdrop standing in for a distance that
        // was never modelled. Nothing exists behind either, which is why
        // neither is graded by the emissive dial - that one is for lamps and
        // screens, which are objects in the scene that also light it.
        shading.emission = assembleSkyClass(shading.albedo, material.emission.x);
        shading.terminatesPath = true;
        return shading;
    }

    public PathSample sampleRay(HitGeometry hit, SurfaceShading shading, float3 view, inout uint rng) {
        PathSample sample;
        sample.direction = hit.rayDirection;
        sample.weight = float3(0.0);
        sample.demodulatedWeight = sample.weight;
        sample.specularLobe = false;
        sample.passThrough = false;
        sample.costsScatter = false;
        sample.terminate = true;
        return sample;
    }
};

/**
 * World-space motion, previous minus current. Rigid meshes: the hit point
 * back to object space through the instance's previous transform. Skinned
 * meshes: the previous-pose positions the skin compute wrote with last
 * frame's bone palette, interpolated at the same barycentrics - full
 * per-bone motion.
 */
public float3 ptMotionVector(HitGeometry hit) {
    float3 prevWorldPosition = mergedVertices[hit.indices.x].prevPosition * hit.weights.x + mergedVertices[hit.indices.y].prevPosition * hit.weights.y + mergedVertices[hit.indices.z].prevPosition * hit.weights.z;
    return prevWorldPosition - hit.position;
}

/** Type dispatch: the one switch in the system. */
public SurfaceShading ptShade(HitGeometry hit) {
    switch (instanceMaterials[hit.materialIndex].surfaceType) {
    case kPtSurfaceUnlitTransparent: {
        UnlitTransparentSurface surface;
        return surface.shade(hit);
    }
    case kPtSurfaceUnlitEmissive: {
        UnlitEmissiveSurface surface;
        return surface.shade(hit);
    }
    default: {
        PbrSurface surface;
        return surface.shade(hit);
    }
    }
}

/**
 * A transparent surface's continuation: tinted passthrough, costing a bounce.
 *
 * Odyssey transparency is a flat card with no thickness, no authored index and
 * no interior, so there is no medium to model - what a pane does to a ray is
 * take a fraction of it and colour the rest. Coverage decides the fraction and
 * the texel decides the colour, which is the same composite the raster blended
 * pass computes, expressed as throughput instead of as a blend.
 *
 * A fully covered texel terminates the path rather than continuing at zero:
 * traversal already committed the hit, and carrying a black throughput onward
 * would spend the rest of the path's bounces learning nothing.
 *
 * The bend is invention and is dialled from zero. It is skipped on thin
 * surfaces - a double-sided card is one interface, not two, and refracting at
 * both would bend a single pane twice.
 */
PathSample transmissiveSample(HitGeometry hit, SurfaceShading shading) {
    PathSample sample;
    float coverage = saturate(shading.alpha);
    sample.direction = hit.rayDirection;
    if (pushConstants.refraction > 0.0 && !shading.thin) {
        // The geometric normal is already face-forward, so it opposes the
        // incident ray and refract's convention is satisfied. A grazing ray can
        // total-internally-reflect, which returns zero; keep the straight path
        // there rather than reflecting a surface the original never reflected.
        static const float kTransmissiveIor = 1.1;
        float3 refracted = refract(hit.rayDirection, hit.geometricNormal, 1.0 / kTransmissiveIor);
        if (dot(refracted, refracted) > 1e-6) {
            sample.direction = normalize(lerp(hit.rayDirection, refracted,
                                              saturate(pushConstants.refraction)));
        }
    }
    sample.weight = (1.0 - coverage) * shading.albedo;
    sample.demodulatedWeight = sample.weight;
    sample.specularLobe = false;
    sample.passThrough = true;
    sample.costsScatter = true;
    sample.terminate = coverage >= 1.0;
    return sample;
}

public PathSample ptSampleRay(HitGeometry hit, SurfaceShading shading, float3 view, inout uint rng) {
    // Before the type switch, because this is not a fourth surface type: a
    // window is an ordinary lit surface that also lets light through, and it
    // must keep shading as one. Only the continuation differs.
    if ((instanceMaterials[hit.materialIndex].featureMask & kMaterialTransmissive) != 0) {
        return transmissiveSample(hit, shading);
    }
    switch (instanceMaterials[hit.materialIndex].surfaceType) {
    case kPtSurfaceUnlitTransparent: {
        UnlitTransparentSurface surface;
        return surface.sampleRay(hit, shading, view, rng);
    }
    case kPtSurfaceUnlitEmissive: {
        UnlitEmissiveSurface surface;
        return surface.sampleRay(hit, shading, view, rng);
    }
    default: {
        PbrSurface surface;
        return surface.sampleRay(hit, shading, view, rng);
    }
    }
}
