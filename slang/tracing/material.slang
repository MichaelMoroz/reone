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

import lib.math;
import lib.scene_schema;
import uniforms;
import tracing.rng;
import tracing.brdf;
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
    public bool envmapped;
    public float envStrength;
    public bool terminatesPath;
};

float4 vertexColor(HitGeometry hit) {
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
    public bool terminate;
};

public interface ISurfaceModel {
    public SurfaceShading shade(HitGeometry hit);
    public PathSample sampleRay(HitGeometry hit, SurfaceShading shading, float3 view, inout uint rng);
};

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
    hit.p0 = mergedVertices[indices.x].position;
    hit.p1 = mergedVertices[indices.y].position;
    hit.p2 = mergedVertices[indices.z].position;
    hit.geometricNormal = normalize(cross(hit.p1 - hit.p0, hit.p2 - hit.p0));
    if (dot(hit.geometricNormal, rayDirection) > 0.0) hit.geometricNormal = -hit.geometricNormal;
    hit.uv = ptTransformedUV(trace.materialIndex, mergedVertices[indices.x].uv1 * weights.x + mergedVertices[indices.y].uv1 * weights.y + mergedVertices[indices.z].uv1 * weights.z);
    hit.uv2 = mergedVertices[indices.x].uv2 * weights.x + mergedVertices[indices.y].uv2 * weights.y + mergedVertices[indices.z].uv2 * weights.z;
    // The terminator shift (Hanika, RT Gems II): the shadow-ray origin lifted
    // onto the smooth surface the vertex normals describe. Low-poly geometry
    // with aggressively smoothed normals - all of KotOR - otherwise shows the
    // shadow terminator as faceted bands.
    hit.shadowOrigin = hit.position;
    if (dot(mergedVertices[indices.x].normal, mergedVertices[indices.x].normal) > 1e-10) {
        float3 n0 = normalize(mergedVertices[indices.x].normal);
        float3 n1 = normalize(mergedVertices[indices.y].normal);
        float3 n2 = normalize(mergedVertices[indices.z].normal);
        float3 u0 = hit.position - hit.p0;
        float3 u1 = hit.position - hit.p1;
        float3 u2 = hit.position - hit.p2;
        u0 -= min(0.0, dot(u0, n0)) * n0;
        u1 -= min(0.0, dot(u1, n1)) * n1;
        u2 -= min(0.0, dot(u2, n2)) * n2;
        hit.shadowOrigin = weights.x * (hit.p0 + u0) + weights.y * (hit.p1 + u1) + weights.z * (hit.p2 + u2);
    }
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
    float2 uvA = mergedVertices[hit.indices.x].uv1;
    float2 duv1 = mergedVertices[hit.indices.y].uv1 - uvA;
    float2 duv2 = mergedVertices[hit.indices.z].uv1 - uvA;
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
public float ptRayConeLod(InstanceMaterial material, uint3 indices, float coneWidth) {
    if (material.mainTex == kPtNoTexture) return 0.0;
    float3 q0 = mergedVertices[indices.x].position;
    float3 q1 = mergedVertices[indices.y].position;
    float3 q2 = mergedVertices[indices.z].position;
    float2 uv0 = mergedVertices[indices.x].uv1;
    float2 uv1 = mergedVertices[indices.y].uv1;
    float2 uv2 = mergedVertices[indices.z].uv1;
    float ta = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));
    float pa = length(cross(q1 - q0, q2 - q0));
    uint texWidth, texHeight;
    bindlessTextures[NonUniformResourceIndex(material.mainTex)].GetDimensions(texWidth, texHeight);
    float density = ta * float(texWidth) * float(texHeight) / max(pa, 1e-9);
    return max(0.0, 0.5 * log2(max(density, 1e-12)) + log2(max(coneWidth, 1e-9)));
}

// Curated channel op: mode 0 passes the derived value through, mode 1
// overrides with a constant, mode 2 evaluates
// lerp(base, smoothstep(a, b, dot(albedo, weights)), t).
//
// Public because tracing/primary.slang evaluates the same curation over a
// G-buffer texel instead of over a hit record, and the two must not drift.
public float curatedChannel(float4 A, float4 B, float3 albedo, float current) {
    if (A.x < 0.5) return current;
    if (A.x < 1.5) return A.y;
    return lerp(A.y, smoothstep(A.z, A.w, dot(albedo, B.yzw)), B.x);
}

public struct PbrSurface : ISurfaceModel {
    public SurfaceShading shade(HitGeometry hit) {
        InstanceMaterial material = instanceMaterials[hit.materialIndex];
        SurfaceShading shading;
        shading.terminatesPath = false;
        shading.albedo = float3(1.0);
        shading.alpha = 1.0;
        if (material.mainTex != kPtNoTexture) {
            UvGradients gradients = anisotropicGradients(hit);
            // Textures are stored gamma-encoded; every term this shader
            // computes is linear, exactly as the raster resolve does it.
            float4 sample = bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleGrad(hit.uv, gradients.x, gradients.y);
            shading.albedo = ptGammaToLinear(sample.rgb);
            shading.alpha = sample.a;
            if ((material.featureMask & kPtMaskPremulAlpha) != 0) {
                // Colour is already multiplied by coverage and the alpha
                // channel is unusable, so luma recovers the coverage and
                // dividing it back out gives the unpremultiplied colour.
                // Reading .a here instead renders KotOR's smoke as its own
                // negative: opaque wherever the plume is thin. Same recovery
                // as particles.slang:64-71.
                shading.alpha = rgbToLuma(sample.rgb);
                shading.albedo *= 1.0 / max(1e-4, shading.alpha);
            }
        }
        // Keep the tracer's stand-ins exactly aligned with pbr_resolve.slang:
        // alpha is only a reflectance control on env-mapped materials, never
        // metalness, and roughness still uses alpha for every material.
        shading.metallic = 0.0;
        shading.roughness = clamp(shading.alpha, 0.2, 1.0);
        // Curated per-object operations, then the per-category calibration
        // override - both baked into the material record CPU-side; the
        // shader only evaluates.
        shading.albedo *= material.curatedAlbedoMul.rgb;
        shading.roughness = curatedChannel(material.curatedRoughA, material.curatedRoughB,
                                           shading.albedo, shading.roughness);
        shading.metallic = curatedChannel(material.curatedMetalA, material.curatedMetalB,
                                          shading.albedo, shading.metallic);
        // A scale rather than an override, so curation stays in charge of which
        // surfaces are metal and this only says how far. Above 0 it tints Rf0
        // toward albedo, which is the only thing that makes the specular
        // demodulation factor chromatic - KotOR's own materials are all
        // dielectric, so without this dial that path cannot be exercised.
        shading.metallic = saturate(shading.metallic * material.overrideParams.w);
        shading.albedo = lerp(shading.albedo, ptGammaToLinear(material.overrideColor.rgb),
                              material.overrideColor.w);
        float4 color = vertexColor(hit);
        shading.albedo *= color.rgb;
        shading.alpha *= color.a;
        if (material.overrideParams.x >= 0.0) shading.roughness = material.overrideParams.x;
        shading.roughness = clamp(shading.roughness * material.roughnessScale, 0.2, 1.0);

        shading.normal = materialNormal(hit, hit.materialIndex);
        shading.envmapped = (material.featureMask & uint(kFeatureEnvMap)) != 0;
        shading.envStrength = (shading.envmapped ? (1.0 - shading.alpha) : 0.0) *
                              material.overrideParams.z;
        shading.hasLightmap = (material.featureMask & uint(kFeatureLightmap)) != 0;
        shading.lightmap = float3(0.0);
        if (shading.hasLightmap && material.lightmap != kPtNoTexture) {
            shading.lightmap = ptGammaToLinear(
                bindlessTextures[NonUniformResourceIndex(material.lightmap)].SampleLevel(hit.uv2, 0.0).rgb);
        }
        float3 emission = ptGammaToLinear(material.selfIllumColor.rgb);
        if (material.curatedEmission.w > 1.5) {
            emission = material.curatedEmission.rgb;
        } else if (material.curatedEmission.w > 0.5) {
            emission *= material.curatedEmission.rgb;
        }
        // Emission is additive over shading, like raster self-illumination;
        // the dial and the category emission scale ride along here.
        shading.emission = emission * shading.albedo * pushConstants.emissiveIntensity;
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
        sample.demodulatedWeight = sample.weight;
        sample.specularLobe = false;
        // Cutout coverage was resolved deterministically in traversal: a
        // committed texel is solid substance. No re-roll, no re-trace.
        // The GGX lobe exists only where raster shows this heuristic a
        // specular response: environment-mapped surfaces. Everywhere else the
        // diffuse alpha is coverage or opacity, so the derived roughness is
        // meaningless. Non-envmapped surfaces are pure diffuse.
        float3 F0 = lerp(lerp(float3(0.04), shading.albedo, shading.metallic), float3(1.0),
                         shading.envStrength);
        float NdotV = max(0.0, dot(shading.normal, view));
        float3 F = ptFresnelSchlick(NdotV, F0);
        float specularProbability = shading.envmapped
                                        ? clamp(luminanceOf(F), 1e-3, 1.0 - 1e-3)
                                        : 0.0;
        if (shading.envmapped && ptRandom01(rng) < specularProbability) {
            float3 half = ptGgxHalfVector(shading.normal, shading.roughness,
                                          float2(ptRandom01(rng), ptRandom01(rng)));
            sample.direction = normalize(reflect(-view, half));
            sample.weight = F / specularProbability;
            sample.demodulatedWeight = sample.weight;
            sample.specularLobe = true;
        } else {
            sample.direction = ptCosineHemisphere(shading.normal,
                                                   float2(ptRandom01(rng), ptRandom01(rng)));
            sample.weight = shading.albedo;
            // Keep the diffuse lobe sampleable for blended metals while using
            // the same metallic energy split as next-event estimation.
            sample.weight *= 1.0 - shading.metallic;
            // The albedo and metallic split cancel against the material
            // response exactly; only the lobe-selection probability survives.
            sample.demodulatedWeight = float3(1.0);
            if (shading.envmapped) {
                sample.weight *= (1.0 - F) / (1.0 - specularProbability);
                sample.demodulatedWeight = float3(1.0 / (1.0 - specularProbability));
            }
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
                if ((instanceMaterials[materialIndex].featureMask & uint(kFeatureNormalMap)) != 0 && instanceMaterials[materialIndex].normalMap != kPtNoTexture) {
                    float3 n = bindlessTextures[NonUniformResourceIndex(instanceMaterials[materialIndex].normalMap)].SampleGrad(hit.uv, gradients.x, gradients.y).rgb * 2.0 - 1.0;
                    shadingNormal = normalize(tangent * n.x + bitangent * n.y + shadingNormal * n.z);
                } else if ((instanceMaterials[materialIndex].featureMask & uint(kFeatureBumpMap)) != 0 && instanceMaterials[materialIndex].bumpMapArray != kPtNoTexture) {
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
        shading.albedo = ptGammaToLinear(material.diffuseColor.rgb);
        shading.alpha = 1.0;
        if (material.mainTex != kPtNoTexture) {
            float lod = ptRayConeLod(material, hit.indices, hit.coneWidth);
            float4 sample = bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(hit.uv, lod);
            shading.albedo = ptGammaToLinear(sample.rgb);
            shading.alpha = sample.a;
        }
        float4 color = vertexColor(hit);
        shading.albedo *= color.rgb;
        shading.alpha *= color.a;
        shading.normal = hit.geometricNormal;
        shading.roughness = 1.0;
        shading.metallic = 0.0;
        shading.hasLightmap = false;
        shading.lightmap = float3(0.0);
        shading.envmapped = false;
        shading.envStrength = 0.0;
        // Additive diffuse textures are authored as light even when no
        // self-illum controller is present.
        shading.emission = max(material.selfIllumColor.rgb, float3(1.0)) * shading.albedo *
                           pushConstants.emissiveIntensity * material.overrideParams.y;
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
        if (material.mainTex != kPtNoTexture) {
            float lod = ptRayConeLod(material, hit.indices, hit.coneWidth);
            shading.albedo = ptGammaToLinear(
                bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(hit.uv, lod).rgb);
        }
        shading.normal = hit.geometricNormal;
        shading.roughness = 1.0;
        shading.metallic = 0.0;
        shading.hasLightmap = false;
        shading.lightmap = float3(0.0);
        shading.envmapped = false;
        shading.envStrength = 0.0;
        // The texture is the radiance: the sky takes its own dial, prelit
        // imagery renders as authored. Nothing exists behind either.
        bool sky = (material.featureMask & kPtMaskSky) != 0;
        shading.emission = shading.albedo * (sky ? pushConstants.skyIntensity : 1.0);
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
        sample.terminate = true;
        return sample;
    }
};

float luminanceOf(float3 rgb) {
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

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

public PathSample ptSampleRay(HitGeometry hit, SurfaceShading shading, float3 view, inout uint rng) {
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
