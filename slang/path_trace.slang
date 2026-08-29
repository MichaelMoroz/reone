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
// The path-tracing kernel: read the primary out of the G-buffer (no camera
// ray is traced), publish its guides, then the SPP loop of bounce paths.
// Cutout coverage is resolved in traversal, purely emissive surfaces let the
// ray through, and non-emissive translucency is not traced (tracing/trace).
//
//   tracing.resources - bindings, the instance material record, fetches
//   tracing.rng       - hashing and directional sampling
//   tracing.brdf      - the display transform
//   tracing.material  - the three surface models behind ISurfaceModel
//   tracing.primary   - the G-buffer primary
//   tracing.trace     - the two ray kinds: nearest and shadow
//   tracing.lighting  - per-vertex next-event estimation
//   tracing.outputs   - the NRD-facing output split

import uniforms;
import lib.direct_light;
import lib.surface_model;
import tracing.rng;
import lib.brdf;
import lib.math;
import tracing.brdf;
import tracing.resources;
import tracing.material;
import tracing.primary;
import tracing.trace;
import tracing.lighting;
import tracing.outputs;

// The debug channels only the tracer can show; the shared debug pass owns the
// rest, and the numbering is the common one.

static const uint kDebugOff = 0;
static const uint kDebugDiffuseChannel = 8;
static const uint kDebugSpecularChannel = 9;
static const uint kDebugDirectShadow = 15;
static const uint kDebugPenumbra = 16;
static const uint kDebugNoiseFree = 11;

struct PtDebug {
    uint view;

    static PtDebug make() {
        PtDebug debug;
        debug.view = ptDebugView();
        return debug;
    }

    /** True only for the channels no other mode can produce. */
    bool wantsChannelView() {
        return view == kDebugDiffuseChannel || view == kDebugSpecularChannel ||
               view == kDebugNoiseFree || view == kDebugDirectShadow ||
               view == kDebugPenumbra;
    }

    /** Returns true and sets color when a channel view replaces the image. */
    bool channelColor(TraceOutputs outputs, out float3 color) {
        switch (view) {
        case kDebugDiffuseChannel:
            color = outputs.diffuse;
            return true;
        case kDebugSpecularChannel:
            color = outputs.specular;
            return true;
        case kDebugNoiseFree:
            color = outputs.noiseFree;
            return true;
        case kDebugDirectShadow:
            // Demodulated - the shadow signal itself - and exposed with
            // 1 - exp(-x): raw, an unbounded radiance clips in a capture and
            // shows no shadows. The raster channel takes the same curve.
            color = float3(1.0) - exp(-outputs.directDiffuse);
            return true;
        case kDebugPenumbra:
            // The expected penumbra half-width, in world units, scaled so a
            // tenth of a unit reads as full white. Black is a contact edge or
            // an unshadowed pixel; both ask the filter for no radius at all.
            color = float3(saturate(outputs.penumbra * 10.0));
            return true;
        default:
            color = float3(0.0);
            return false;
        }
    }
};

/** Additive layers (sabers, glow planes) per path. */
static const uint kMaxPassThroughSteps = 16;

/**
 * The one exit: write the split, then the image - linear, since the common
 * post-process owns the display transform; a channel debug view is the
 * diagnostic exception and finishes itself.
 */
void finishPixel(uint2 pixel, TraceOutputs outputs, PtDebug debug) {
    outputs.write(pixel);
    float3 color;
    if (debug.wantsChannelView() && debug.channelColor(outputs, color)) {
        outputImage[pixel] = float4(
            ptDisplayTransform(color, pushConstants.exposure, ptTonemapOperator()), 1.0);
        return;
    }
    outputImage[pixel] = float4(outputs.assemble(), 1.0);
}

/**
 * Deeper vertices are part of the transport seen through the primary lobe,
 * where an environment integral is neither available nor appropriate. This
 * view-direction Schlick split matches the continuation sampler.
 */
float3 diffuseMaterialResponse(SurfaceShading shading, float3 view) {
    float3 F0 = materialRf0(shading.albedo, shading.metallic, shading.envStrength);
    float3 response = shading.albedo * (1.0 - shading.metallic);
    if (shading.envmapped) {
        response *= 1.0 - fresnelSchlick(max(0.0, dot(shading.normal, view)), F0);
    }
    return response;
}

/**
 * The pixel's own camera ray: not traced, but the BRDF needs a view vector,
 * the normal is faced against it and the ray cone is sized from it. Both depth
 * ends are unprojected so orthographic GUI cameras work; the projection
 * carries the jitter the geometry pass rasterised with.
 */
RayDesc cameraRay(uint2 pixel, uint2 extent) {
    float2 ndc = ((float2(pixel) + 0.5) / float2(extent)) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 nearH = mul(globalUniforms.projectionInv, float4(ndc, 0.0, 1.0));
    float4 farH = mul(globalUniforms.projectionInv, float4(ndc, 1.0, 1.0));
    float3 nearPoint = nearH.xyz / nearH.w;
    float3 farPoint = farH.xyz / farH.w;
    RayDesc ray;
    ray.Origin = mul(globalUniforms.viewInv, float4(nearPoint, 1.0)).xyz;
    ray.Direction = normalize(mul(globalUniforms.viewInv, float4(farPoint - nearPoint, 0.0)).xyz);
    ray.TMin = 0.01;
    ray.TMax = 100000.0;
    return ray;
}

/** The angle one pixel subtends; zero for orthographic GUI scenes, whose content is near-field. */
float pixelSpreadAngle(uint height) {
    return globalUniforms.projection[3][3] == 0.0
               ? 2.0 / (float(height) * globalUniforms.projection[1][1])
               : 0.0;
}

/**
 * The primary's guides and factors into the output split. The guide surface
 * and the transport primary are the same surface by construction: the
 * G-buffer holds opaque and cutout coverage only. The sky sits at environment
 * depth; an unlit surface keeps the default white factors.
 */
void publishPrimary(inout TraceOutputs outputs, PrimarySurface primary, RayDesc ray,
                    uint2 pixel, uint2 extent) {
    bool pbrPrimary = instanceMaterials[primary.hit.materialIndex].surfaceType == kPtSurfacePbr;
    float surfaceViewZ = mul(globalUniforms.view, float4(primary.worldPosition, 1.0)).z;
    if (pbrPrimary) {
        materialFactors(primary.shading.normal, -ray.Direction, primary.shading.albedo,
                        primary.shading.metallic, primary.shading.envStrength,
                        primary.shading.roughness,
                        outputs.transportDiffFactor, outputs.transportSpecFactor);
    }
    outputs.normal = primary.shading.normal;
    outputs.roughness = primary.shading.roughness;
    outputs.viewZ = primary.shading.terminatesPath ? kPtInfDistance : surfaceViewZ;
    outputs.motion = primary.motion;
    outputs.diffFactor = outputs.transportDiffFactor;
    outputs.specFactor = outputs.transportSpecFactor;
    // The traced-side dump targets --dumptargets names; tautologically raster's.
    outputs.gBufferDiffuse = float4(linearToGamma(primary.shading.albedo), primary.shading.alpha);
    outputs.gBufferEyeNormal = 0.5 * mul(primary.shading.normal, (float3x3)globalUniforms.viewInv) + 0.5;
    outputs.gBufferDepth = max(0.0, -surfaceViewZ);
    outputs.gBufferMotion = primary.screenMotion;
    // The sky keeps the far-plane defaults: nothing for an upscaler to chase.
    if (!primary.shading.terminatesPath) {
        outputs.setUpscalerGuides(pixel, extent, primary.worldPosition, outputs.motion);
    }
    outputs.noiseFree = primary.shading.emission;
}

/** What one path contributes to the pixel's channels. */
struct PathResult {
    float3 diffuse;
    float3 specular;
    float3 directDiffuse;
    /** Expected penumbra at the primary, 0 when unblocked; counted only when blocked. */
    float penumbra;
    /** Distance to the first scatter, kPtInfDistance for a miss, negative when there was none. */
    float firstScatterT;
    bool specularPath;
    uint secondaryRays;
    uint secondaryMisses;
};

/**
 * One path from the primary: next-event estimation at every vertex, a
 * sampled continuation, sky on a miss. Channel routing is decided by the
 * first scattering lobe; the primary vertex's material factors are
 * demodulated out, deeper vertices carry full weights.
 */
PathResult tracePath(HitGeometry primaryHit, SurfaceShading primaryShading, RayDesc probe,
                     float primaryDistance, float spreadAngle, float2 primaryLightXi,
                     TraceOutputs outputs, uint scatterBudget, bool statsOn, inout uint rng) {
    PathResult result = { float3(0.0), float3(0.0), float3(0.0), 0.0, -1.0, false, 0, 0 };
    HitGeometry hit = primaryHit;
    SurfaceShading shading = primaryShading;
    RayDesc path = probe;
    float3 pathWeight = float3(1.0);
    float3 pathRadiance = float3(0.0);
    float pathDistance = primaryDistance;
    bool pathDecided = false;
    bool primaryScatter = true;
    uint scatters = 0;
    uint passThroughs = 0;

    while (true) {
        float3 view = -path.Direction;
        // Path regularisation: after the first scatter the lobe is widened to a
        // floor. A deliberate bias that keeps the energy of the primary
        // highlight and spends the bias where the variance is - a glossy
        // bounce sampling the sun is a caustic no frame budget converges.
        if (scatters > 0u) {
            shading.roughness = max(shading.roughness, pushConstants.bounceRoughness);
        }
        PathSample sample = ptSampleRay(hit, shading, view, rng);
        if (sample.terminate) break;
        if (!sample.passThrough) {
            float2 lightXi = primaryScatter && ptFlag(kPtFlagDirectChannel)
                                 ? primaryLightXi
                                 : float2(ptRandom01(rng), ptRandom01(rng));
            // NEE off is a diagnostic: the only light left is what a BSDF ray hits.
            DirectLight nee = { float3(0.0), float3(0.0), 0.0 };
            if (ptFlag(kPtFlagNee)) nee = ptDirectLight(hit, shading, view, lightXi, rng);
            if (!pathDecided) {
                pathDecided = true;
                result.specularPath = sample.specularLobe;
            }
            if (primaryScatter) {
                // Invariant: channel * diffFactor is the primary diffuse
                // radiance. Only the diffuse half may leave the denoiser;
                // direct specular stays in NRD's specular channel, which is
                // built for a tight lobe against a small light.
                if (ptFlag(kPtFlagDirectChannel)) {
                    result.directDiffuse += nee.diffuse;
                } else {
                    result.diffuse += nee.diffuse;
                }
                result.penumbra = nee.penumbra;
                result.specular += nee.specular / outputs.transportSpecFactor;
                // The lobe hands back the demodulated ratio in closed form:
                // dividing weight by the response fireflies on a black texel.
                pathWeight *= sample.specularLobe ? sample.weight / outputs.transportSpecFactor
                                                  : sample.demodulatedWeight;
            } else {
                float3 indirect = pathWeight *
                                  (nee.diffuse * diffuseMaterialResponse(shading, view) +
                                   nee.specular);
                // The indirect ceiling, off by default: it truncates energy
                // rather than widening a lobe.
                if (pushConstants.indirectClamp > 0.0) {
                    float peak = max(indirect.r, max(indirect.g, indirect.b));
                    if (peak > pushConstants.indirectClamp) {
                        indirect *= pushConstants.indirectClamp / peak;
                    }
                }
                pathRadiance += indirect;
                pathWeight *= sample.weight;
            }
            primaryScatter = false;
            if (++scatters > scatterBudget) break;
            path.Origin = hit.position + hit.geometricNormal * pushConstants.rayOriginOffset;
        } else {
            // A continuation: purely emissive light occupies no volume and the
            // step is free, bounded by the pass-through cap; a transmissive
            // surface spends a bounce like any other interaction.
            primaryScatter = false;
            if (sample.costsScatter) {
                if (++scatters > scatterBudget) break;
            } else if (++passThroughs > kMaxPassThroughSteps) {
                break;
            }
            pathWeight *= sample.weight;
            path.Origin = hit.position + path.Direction * 1e-3;
        }
        if (max(pathWeight.x, max(pathWeight.y, pathWeight.z)) < 1e-3) break;
        path.Direction = sample.direction;
        path.TMin = 0.001;
        path.TMax = 100000.0;

        TraceHit bounce = ptTraceNearest(path);
        if (statsOn) ++result.secondaryRays;
        if (!bounce.hit) {
            if (statsOn) ++result.secondaryMisses;
            if (result.firstScatterT < 0.0 && scatters > 0) result.firstScatterT = kPtInfDistance;
            if (pushConstants.skyAvailable != 0) {
                pathRadiance += pathWeight * ptSkyRadiance(path.Direction);
            }
            break;
        }
        if (result.firstScatterT < 0.0 && scatters > 0) result.firstScatterT = bounce.hitT;
        pathDistance += bounce.hitT;
        hit = ptMakeHitGeometry(bounce, path, spreadAngle * pathDistance);
        shading = ptShade(hit);
        // Every vertex past the first pays its emission, additive included -
        // which blade plane a scattered ray finds is a sampling outcome, so it
        // belongs in the denoised channel - plus the lightmap radiance cache on
        // PBR surfaces, a bounded double-count the dial grades.
        pathRadiance += pathWeight * shading.emission;
        if (shading.terminatesPath) break;
        if (shading.hasLightmap) {
            pathRadiance += pathWeight * shading.albedo * shading.lightmap *
                            pushConstants.lightmapIntensity;
        }
    }
    if (result.specularPath) result.specular += pathRadiance;
    else result.diffuse += pathRadiance;
    return result;
}

[shader("raygeneration")]
void main() {
    uint3 dispatchId = DispatchRaysIndex();
    uint2 extent = DispatchRaysDimensions().xy;
    uint2 pixel = dispatchId.xy;
    RayDesc ray = cameraRay(pixel, extent);
    float spreadAngle = pixelSpreadAngle(extent.y);

    TraceOutputs outputs = TraceOutputs.makeDefault();
    PtDebug debug = PtDebug.make();

    PrimarySurface primary = ptReadPrimary(pixel, extent, ray, spreadAngle);
    if (!primary.covered) {
        // Nothing drawn: the same sky both raster resolves composite. Parity
        // excludes environment, so black on both sides.
        if (pushConstants.skyAvailable != 0 && !ptFlag(kPtFlagParityDirect)) {
            outputs.noiseFree += ptSkyRadiance(ray.Direction);
        }
        finishPixel(pixel, outputs, debug);
        return;
    }
    publishPrimary(outputs, primary, ray, pixel, extent);

    if (primary.shading.terminatesPath) {
        // A terminating surface is emission, which parity excludes.
        if (ptFlag(kPtFlagParityDirect)) outputs.noiseFree = float3(0.0);
        finishPixel(pixel, outputs, debug);
        return;
    }
    // Parity: the shared unoccluded direct sum and nothing else, one
    // deterministic evaluation with white factors. See lib/direct_light.slang.
    if (ptFlag(kPtFlagParityDirect)) {
        outputs.noiseFree = parityDirectColor(
            primary.shading.albedo, primary.hit.position, primary.shading.normal,
            primary.shading.thin, pushConstants.thinTransmission,
            pushConstants.emitterRadiusRatio, pushConstants.directIntensity,
            pushConstants.sunIntensity);
        outputs.diffuse = float3(0.0);
        outputs.specular = float3(0.0);
        outputs.directDiffuse = float3(0.0);
        outputs.diffFactor = float3(1.0);
        outputs.specFactor = float3(1.0);
        outputs.transportDiffFactor = float3(1.0);
        outputs.transportSpecFactor = float3(1.0);
        finishPixel(pixel, outputs, debug);
        return;
    }

    // The continuation ray starts at the primary, pointing the way the camera looked.
    RayDesc probe = ray;
    probe.Origin = primary.hit.position;
    bool statsOn = ptFlag(kPtFlagStats);
    uint sampleCount = max(pushConstants.samplesPerPixel, 1u);
    // Zero bounces is legal: the primary still draws next-event estimation.
    uint scatterBudget = pushConstants.bounceCount;
    float3 diffuseChannel = float3(0.0);
    float3 specularChannel = float3(0.0);
    // Direct light at the primary, kept apart: a filter wide enough to settle
    // bounce noise takes a contact edge with it, so this channel is stratified
    // at the source and resolved temporally.
    float3 directDiffuseChannel = float3(0.0);
    // Penumbra and hit distances average over the samples that were blocked
    // or that scattered; the others say nothing about them.
    float penumbraSum = 0.0, penumbraCount = 0.0;
    float diffHitSum = 0.0, diffHitCount = 0.0;
    float specHitSum = 0.0, specHitCount = 0.0;
    uint secondaryRays = 0, secondaryMisses = 0;

    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        // One counter across frames and samples, so consecutive draws never
        // alias the way a frame-xor-sample seed did.
        uint sequence = pushConstants.frameIndex * sampleCount + sampleIndex;
        uint rng = ptHash(ptHash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu) +
                          sequence * 0x9e3779b9u);
        // Stratified and screen-dithered, for the primary vertex only.
        float2 primaryLightXi = ptShadowSample(pixel, sequence);
        PathResult path = tracePath(primary.hit, primary.shading, probe, primary.distance,
                                    spreadAngle, primaryLightXi, outputs, scatterBudget,
                                    statsOn, rng);
        diffuseChannel += path.diffuse;
        specularChannel += path.specular;
        directDiffuseChannel += path.directDiffuse;
        if (path.penumbra > 0.0) {
            penumbraSum += path.penumbra;
            penumbraCount += 1.0;
        }
        if (path.firstScatterT >= 0.0) {
            if (path.specularPath) {
                specHitSum += path.firstScatterT;
                specHitCount += 1.0;
            } else {
                diffHitSum += path.firstScatterT;
                diffHitCount += 1.0;
            }
        }
        secondaryRays += path.secondaryRays;
        secondaryMisses += path.secondaryMisses;
    }

    if (statsOn) {
        InterlockedAdd(traceStats[0], secondaryRays);
        InterlockedAdd(traceStats[1], secondaryMisses);
    }
    outputs.diffuse = diffuseChannel / float(sampleCount);
    outputs.specular = specularChannel / float(sampleCount);
    outputs.directDiffuse = directDiffuseChannel / float(sampleCount);
    outputs.penumbra = penumbraCount > 0.0 ? penumbraSum / penumbraCount : 0.0;
    // World-space, unnormalized; zero means no distance to report, which NRD
    // reconstructs from the neighbourhood rather than reading as contact-close.
    outputs.diffuseHitDist = diffHitCount > 0.0 ? diffHitSum / diffHitCount : 0.0;
    outputs.specularHitDist = specHitCount > 0.0 ? specHitSum / specHitCount : 0.0;
    finishPixel(pixel, outputs, debug);
}
