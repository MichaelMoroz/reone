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
// The path-tracing kernel. Every hit past the primary reads its material and
// vertex attributes from GPU-resident data; no raster draw state is used.
//
// The structure is: read the primary -> SPP loop -> bounce loop.
//
// There is no camera ray. Primary visibility is rasterized in this mode as in
// every other, so the geometry pass has already resolved which surface each
// pixel shows; tracing a ray to find it a second time was work spent proving
// the two agree. tracing/primary.slang reconstructs that surface from the
// G-buffer and lists, in one place, everything the G-buffer cannot answer that
// a hit record could.
//
// Two coverage kinds are traced and no others. CUTOUT coverage is a binary
// visibility test and is resolved in traversal. PURELY EMISSIVE surfaces
// contribute their own radiance and let the ray through. Non-emissive
// translucency - a surface that tints or attenuates what is behind it - is not
// traced at all; a ray marcher is to take that over, and the seams it attaches
// to are the two skips in tracing/trace.slang.
//
// Modules:
//
//   tracing.resources - bindings, the instance material record, fetches
//   tracing.rng       - hashing and directional sampling
//   tracing.brdf      - microfacet terms and color transforms
//   tracing.material  - the three surface models behind ISurfaceModel
//   tracing.primary   - the G-buffer primary
//   tracing.trace     - the two ray kinds: nearest and shadow
//   tracing.lighting  - per-vertex next-event estimation
//   tracing.outputs   - the NRD-facing output split
//
// Material assignment and calibration overrides are CPU registry work; the
// shader only evaluates what the admission baked into the material records.

import uniforms;
import lib.direct_light;
import lib.surface_model;
import tracing.rng;
import tracing.brdf;
import tracing.resources;
import tracing.material;
import tracing.primary;
import tracing.trace;
import tracing.lighting;
import tracing.outputs;

// --- Debug channel views ---
//
// The channel selector is common to every render mode now, and most of what it
// selects are G-buffer quantities every mode has. Those are rendered by the
// shared debug pass (slang/debug_view.slang), which is also what paints the
// "not available in this mode" card for the three below when the mode is not
// this one. What is left here is exactly the set that exists only inside the
// tracer, so only the tracer can show it. Numbering is the common one; the
// gaps are the channels the shared pass owns.

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
            // The channel as the resolve receives it: demodulated, so this is
            // the shadow signal itself rather than the lit picture.
            //
            // Exposed with 1 - exp(-x) rather than written raw. This is an
            // unbounded radiance read out of an 8-bit capture; raw, it clips
            // over half the frame in a bright scene, and a clipped frame shows
            // no shadows at all because a shadowed pixel is still above one.
            // The raster direct-light channel takes the same curve so the two
            // can be compared.
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
 * The one exit: write the split, then the image - a channel debug view when one
 * is active, the assembled color otherwise. Every path out of the kernel ends
 * here, so the debug views see sky and miss pixels too.
 *
 * The assembled image leaves here linear. Every traced frame ends in the common
 * post-process pass, which owns the display transform, and a second one applied
 * here would be applied twice. A debug view is the exception in both directions:
 * it is a final diagnostic image the common tail is skipped for, so it still
 * finishes itself.
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
    float3 F0 = lerp(lerp(float3(0.04), shading.albedo, shading.metallic), float3(1.0),
                     shading.envStrength);
    float3 response = shading.albedo * (1.0 - shading.metallic);
    if (shading.envmapped) {
        response *= 1.0 - ptFresnelSchlick(max(0.0, dot(shading.normal, view)), F0);
    }
    return response;
}

[shader("raygeneration")]
void main() {
    uint3 dispatchId = DispatchRaysIndex();
    uint3 dispatchDimensions = DispatchRaysDimensions();
    uint width = dispatchDimensions.x;
    uint height = dispatchDimensions.y;
    uint2 pixel = dispatchId.xy;

    // The camera ray is no longer traced, but it is still the pixel's own
    // direction: the BRDF needs a view vector, the normal is faced forward
    // against it, and the ray cone is sized from it. Both depth-range ends are
    // unprojected rather than assuming a perspective frustum, because GUI
    // scenes render through orthographic cameras whose rays are parallel with
    // per-pixel origins. The projection carries the TAA jitter, exactly as the
    // geometry pass rasterised with it, so the two agree at sub-pixel scale.
    float2 ndc = ((float2(pixel) + 0.5) / float2(width, height)) * 2.0 - 1.0;
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

    // Ray-cone spread: the angle one pixel subtends. Zero for orthographic
    // GUI scenes, where projection[1][1] is a world scale, not a tangent -
    // their content is authored near-field and mip 0 is right anyway.
    float spreadAngle = globalUniforms.projection[3][3] == 0.0
                            ? 2.0 / (float(height) * globalUniforms.projection[1][1])
                            : 0.0;

    TraceOutputs outputs = TraceOutputs.makeDefault();
    PtDebug debug = PtDebug.make();

    // ---- The primary surface ----
    PrimarySurface primary = ptReadPrimary(pixel, uint2(width, height), ray, spreadAngle);
    if (!primary.covered) {
        // The sentinel material id: nothing was drawn here, so this is the sky.
        // The same cube, through the same shared read, that both raster
        // resolves composite - all three modes therefore keep one sky.
        //
        // Except in parity, where the sky is environment, and environment is
        // excluded by definition: black on both sides, so the comparison is
        // over lit geometry only.
        if (pushConstants.skyAvailable != 0 && !ptParityDirectEnabled()) {
            outputs.noiseFree += ptSkyRadiance(ray.Direction);
        }
        finishPixel(pixel, outputs, debug);
        return;
    }

    HitGeometry primaryHit = primary.hit;
    SurfaceShading primaryShading = primary.shading;
    float primaryDistance = primary.distance;
    // The continuation ray starts at the primary, pointing the way the camera
    // looked. Nothing traverses it before the first scatter.
    RayDesc probe = ray;
    probe.Origin = primaryHit.position;

    // Transport remains tied to the shaded primary. In particular, its
    // demodulation factors cannot follow a different guide surface without
    // changing both the dumped radiance channels and the undenoised image.
    bool pbrPrimary = instanceMaterials[primaryHit.materialIndex].surfaceType == kPtSurfacePbr;
    float surfaceViewZ = mul(globalUniforms.view, float4(primary.worldPosition, 1.0)).z;
    // The denoiser's environment sentinel, not the surface's own depth: a
    // terminating surface is radiance with nothing behind it.
    float transportViewZ = primaryShading.terminatesPath ? kPtInfDistance : surfaceViewZ;
    if (pbrPrimary) {
        materialFactors(primaryShading.normal, -ray.Direction, primaryShading.albedo,
                        primaryShading.metallic, primaryShading.envStrength,
                        primaryShading.roughness,
                        outputs.transportDiffFactor, outputs.transportSpecFactor);
    }

    // The guide surface and the transport primary are now the same surface by
    // construction: the G-buffer holds opaque and cutout coverage only, so
    // there is no blended layer in front of the pixel for the guides to have to
    // see past, and the second traversal that used to resolve one is gone.
    //
    // The sky sits at environment depth for the denoiser; every real surface
    // reports its own depth and motion. An unlit surface keeps the default
    // white factors because its radiance is emission.
    outputs.normal = primaryShading.normal;
    outputs.roughness = primaryShading.roughness;
    outputs.viewZ = transportViewZ;
    outputs.motion = primary.motion;
    outputs.diffFactor = outputs.transportDiffFactor;
    outputs.specFactor = outputs.transportSpecFactor;
    // The comparison G-buffer follows raster exactly - and, now that the raster
    // G-buffer is where these values came from, tautologically so. They are
    // retained as the traced-side dump targets --dumptargets already names.
    outputs.gBufferDiffuse = float4(ptLinearToGamma(primaryShading.albedo),
                                    primaryShading.alpha);
    outputs.gBufferEyeNormal = 0.5 * mul(primaryShading.normal,
                                         (float3x3)globalUniforms.viewInv) + 0.5;
    outputs.gBufferDepth = max(0.0, -surfaceViewZ);
    outputs.gBufferMotion = primary.screenMotion;
    // The sky keeps the far-plane defaults: it has no position to reproject and
    // an upscaler must not try to chase one.
    if (!primaryShading.terminatesPath) {
        outputs.setUpscalerGuides(pixel, uint2(width, height), primary.worldPosition,
                                  outputs.motion);
    }

    outputs.noiseFree = primaryShading.emission;

    if (primaryShading.terminatesPath) {
        // Parity: a terminating surface - sky shell, prelit backdrop - is
        // emission, which the parity image excludes by definition. Black on
        // both sides, so the comparison is over lit geometry only.
        if (ptParityDirectEnabled()) {
            outputs.noiseFree = float3(0.0);
        }
        finishPixel(pixel, outputs, debug);
        return;
    }

    // Parity mode: the final image is the shared unoccluded direct sum and
    // nothing else - no bounces, no NEE draw, no specular, no emission, no
    // sky contribution on covered pixels. One deterministic evaluation; the
    // spp loop below never runs. See lib/direct_light.slang.
    if (ptParityDirectEnabled()) {
        outputs.noiseFree = parityDirectColor(
            primaryShading.albedo, primaryHit.position, primaryShading.normal,
            primaryShading.thin, pushConstants.thinTransmission,
            pushConstants.emitterRadiusRatio, pushConstants.directIntensity,
            pushConstants.sunIntensity);
        outputs.diffuse = float3(0.0);
        outputs.specular = float3(0.0);
        outputs.directDiffuse = float3(0.0);
        // White factors, so the composite's demodulation does not reshape the
        // sum on its way to the image.
        outputs.diffFactor = float3(1.0);
        outputs.specFactor = float3(1.0);
        outputs.transportDiffFactor = float3(1.0);
        outputs.transportSpecFactor = float3(1.0);
        finishPixel(pixel, outputs, debug);
        return;
    }

    // ---- The SPP loop ----
    bool statsOn = ptStatsEnabled();
    uint sampleCount = max(pushConstants.samplesPerPixel, 1u);
    uint scatterBudget = max(pushConstants.bounceCount, 1u);
    float3 diffuseChannel = float3(0.0);
    float3 specularChannel = float3(0.0);
    // Direct light at the pixel the viewer is looking at, kept apart from the
    // two channels above because it does not want what happens to them. A
    // denoiser earns its keep on indirect light, which is smooth and slow; a
    // shadow edge is neither, and every spatial filter wide enough to settle
    // the bounce noise is wide enough to take the contact edge with it. This
    // channel is stratified at the source instead and resolved temporally.
    float3 directDiffuseChannel = float3(0.0);
    // Averaged over the samples that were actually blocked, which is the
    // classic average-blocker-depth of percentage-closer soft shadows: a pixel
    // straddling a shadow edge has some samples occluded and some not, and only
    // the occluded ones say anything about how far away the blocker is.
    float penumbraSum = 0.0;
    float penumbraCount = 0.0;
    float diffHitSum = 0.0, diffHitCount = 0.0;
    float specHitSum = 0.0, specHitCount = 0.0;
    uint secondaryRays = 0, secondaryMisses = 0;

    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        // One counter across frames and samples both, so sample 3 of frame 10
        // and sample 0 of frame 11 are consecutive draws rather than two
        // independent seeds that may coincide. The previous form xored the
        // frame index and the sample index into the same word, which lets
        // (frame, sample) pairs alias: with 2 spp the pair that collides is
        // every other frame, and a shadow edge that redraws the same point on
        // the light stalls and then lurches when the collision breaks.
        uint sequence = pushConstants.frameIndex * sampleCount + sampleIndex;
        uint rng = ptHash(ptHash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu) +
                          sequence * 0x9e3779b9u);
        // Stratified and screen-dithered, for the primary vertex only.
        float2 primaryLightXi = ptShadowSample(dispatchId.xy, sequence);
        HitGeometry hit = primaryHit;
        SurfaceShading shading = primaryShading;
        RayDesc path = probe;
        float3 pathWeight = float3(1.0);
        float3 pathRadiance = float3(0.0);
        float pathDistance = primaryDistance;
        // Channel routing: decided by the first scattering lobe. The primary
        // vertex's material factors are demodulated; deeper vertices carry
        // full weights.
        bool pathDecided = false;
        bool specularPath = false;
        bool primaryScatter = true;
        float firstScatterT = -1.0;
        uint scatters = 0;
        uint passThroughs = 0;

        // ---- The bounce loop ----
        while (true) {
            float3 view = -path.Direction;
            // Path regularisation. After the first scatter the lobe is widened
            // to a floor, which is a deliberate bias and the standard one: a
            // roughness of 0.2 puts the GGX peak near 200, so a path that
            // bounces off one glossy surface onto another and then samples the
            // sun returns a spike two orders of magnitude above its
            // neighbours. That is a caustic, and an unbiased estimator
            // converges on it only with sample counts a frame does not have.
            //
            // Measured on a sun-only, single-bounce, undenoised frame:
            // suppressing bounce-vertex specular entirely removed 46 of the 47
            // spikes above 150 while costing 0.5 of 19.2 luma - the energy is
            // in the primary highlight, the variance is in the bounces. This
            // keeps the energy and spends the bias where the variance is.
            if (scatters > 0u) {
                shading.roughness = max(shading.roughness, pushConstants.bounceRoughness);
            }
            PathSample sample = ptSampleRay(hit, shading, view, rng);
            if (sample.terminate) break;
            if (!sample.passThrough) {
                float2 lightXi = primaryScatter && ptDirectChannelEnabled()
                                     ? primaryLightXi
                                     : float2(ptRandom01(rng), ptRandom01(rng));
                DirectLight nee = ptDirectLight(hit, shading, view, lightXi, rng);
                if (!pathDecided) {
                    pathDecided = true;
                    specularPath = sample.specularLobe;
                }
                if (primaryScatter) {
                    // Invariant: channel * diffFactor equals the primary
                    // diffuse radiance, so direct irradiance stays unscaled.
                    // The same invariant holds for the direct channel, which
                    // carries the same quantity to the same factors - it simply
                    // reaches the resolve without passing through NRD.
                    // Only the diffuse half leaves the denoiser. Direct
                    // specular stays in NRD's specular channel, which is built
                    // for it - it arrives with a hit distance and a roughness,
                    // and the kernel narrows as the surface gets glossier.
                    //
                    // Sending it out with the diffuse made a glossy floor
                    // glitter. Not a routing mistake so much as a variance one
                    // it stopped hiding: a tight lobe against a small light is
                    // sampled here by drawing a point on the light and
                    // evaluating the BSDF there, and that estimator has no
                    // second strategy to fall back on when the drawn point
                    // lands near the mirror direction and D spikes. Multiple
                    // importance sampling is the fix, and until there is one
                    // the denoiser is where this belongs.
                    if (ptDirectChannelEnabled()) {
                        directDiffuseChannel += nee.diffuse;
                    } else {
                        diffuseChannel += nee.diffuse;
                    }
                    if (nee.penumbra > 0.0) {
                        penumbraSum += nee.penumbra;
                        penumbraCount += 1.0;
                    }
                    specularChannel += nee.specular / outputs.transportSpecFactor;
                    if (sample.specularLobe) {
                        pathWeight *= sample.weight / outputs.transportSpecFactor;
                    } else {
                        // Sampling remains importance-weighted by the lobe,
                        // while the path channel must retain only transport
                        // after the primary factor is stripped. The lobe hands
                        // back that ratio in closed form rather than us
                        // dividing weight by the response: both go to zero on a
                        // black texel, and the quotient of two vanishing
                        // numbers is where the fireflies came from.
                        pathWeight *= sample.demodulatedWeight;
                    }
                } else {
                    // The indirect ceiling, when one is set. Blunter than the
                    // regularisation above - it truncates energy rather than
                    // widening a lobe - so it is off by default and exists for
                    // the cases regularisation alone does not settle.
                    float3 indirect = pathWeight *
                                      (nee.diffuse * diffuseMaterialResponse(shading, view) +
                                       nee.specular);
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
                // A continuation rather than a scatter, of which there are two
                // kinds. PURELY EMISSIVE light occupies no volume, so the ray
                // passes unattenuated and the step is free - bounded only by
                // the cap that stops a stack of glow planes looping. A
                // TRANSMISSIVE surface is not free: the ray met a surface and
                // took its tint, so it spends a bounce like any other
                // interaction, which is also what stops a corridor of panes
                // from tracing forever.
                primaryScatter = false;
                if (sample.costsScatter) {
                    if (++scatters > scatterBudget) break;
                } else if (++passThroughs > kMaxPassThroughSteps) {
                    break;
                }
                pathWeight *= sample.weight;
                // Continue through the surface, not off it.
                path.Origin = hit.position + path.Direction * 1e-3;
            }
            if (max(pathWeight.x, max(pathWeight.y, pathWeight.z)) < 1e-3) break;
            path.Direction = sample.direction;
            path.TMin = 0.001;
            path.TMax = 100000.0;

            TraceHit bounce = ptTraceNearest(path);
            if (statsOn) ++secondaryRays;
            if (!bounce.hit) {
                if (statsOn) ++secondaryMisses;
                if (firstScatterT < 0.0 && scatters > 0) firstScatterT = kPtInfDistance;
                if (pushConstants.skyAvailable != 0) {
                    pathRadiance += pathWeight * ptSkyRadiance(path.Direction);
                }
                break;
            }
            if (firstScatterT < 0.0 && scatters > 0) firstScatterT = bounce.hitT;
            pathDistance += bounce.hitT;
            hit = ptMakeHitGeometry(bounce, path, spreadAngle * pathDistance);
            shading = ptShade(hit);
            // Every vertex past the first pays its emission - plus the
            // lightmap radiance cache on PBR surfaces (a bounded
            // double-count preferred to unlit interiors; the dial grades
            // it). Additive emission included, with no special case: this
            // is a scattered ray, so which blade plane it happens to find
            // is a sampling outcome and varies per sample and per frame.
            // Routing that to the noise-free target - as this did - put raw
            // path-tracer noise in the one channel that bypasses the
            // denoiser, and TAA alone was left to average it.
            pathRadiance += pathWeight * shading.emission;
            if (shading.terminatesPath) break;
            if (shading.hasLightmap) {
                pathRadiance += pathWeight * shading.albedo * shading.lightmap *
                                pushConstants.lightmapIntensity;
            }
        }

        if (specularPath) {
            specularChannel += pathRadiance;
        } else {
            diffuseChannel += pathRadiance;
        }
        if (firstScatterT >= 0.0) {
            if (specularPath) {
                specHitSum += firstScatterT;
                specHitCount += 1.0;
            } else {
                diffHitSum += firstScatterT;
                diffHitCount += 1.0;
            }
        }
    }

    if (statsOn) {
        InterlockedAdd(traceStats[0], secondaryRays);
        InterlockedAdd(traceStats[1], secondaryMisses);
    }

    outputs.diffuse = diffuseChannel / float(sampleCount);
    outputs.specular = specularChannel / float(sampleCount);
    outputs.directDiffuse = directDiffuseChannel / float(sampleCount);
    outputs.penumbra = penumbraCount > 0.0 ? penumbraSum / penumbraCount : 0.0;
    // World-space, unnormalized. Which of the two denoiser conventions this
    // ends up in is decided where it is written, not here.
    //
    // Zero still means "this pixel has no distance to report", which happens
    // whenever the lobe the path took was the other one. NRD is told to
    // reconstruct those from the neighbourhood rather than read them as
    // contact-close, which is what a zero would otherwise mean.
    outputs.diffuseHitDist = diffHitCount > 0.0 ? diffHitSum / diffHitCount : 0.0;
    outputs.specularHitDist = specHitCount > 0.0 ? specHitSum / specHitCount : 0.0;
    finishPixel(pixel, outputs, debug);
}
