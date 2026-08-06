// The path-tracing kernel. Every hit reads its material and vertex
// attributes from GPU-resident data; no raster draw state is used.
//
// The structure is exactly: visibility ray -> SPP loop -> bounce loop.
// Transparent materials are surfaces like any other, committed nearest and
// resolved by their surface model: additive light passes the ray through,
// sky and prelit imagery terminate. The one exception is the visibility ray,
// which walks past additive layers rather than letting them own the pixel -
// see the walk below for why the guides cannot come from a glow plane.
// Modules:
//
//   tracing.resources - bindings, the instance material record, fetches
//   tracing.rng       - hashing and directional sampling
//   tracing.brdf      - microfacet terms and color transforms
//   tracing.material  - the three surface models behind ISurfaceModel
//   tracing.trace     - the two ray kinds: nearest and shadow
//   tracing.lighting  - per-vertex next-event estimation
//   tracing.outputs   - the NRD-facing output split
//   tracing.debug     - the debug views
//
// Material assignment and calibration overrides are CPU registry work; the
// shader only evaluates what the admission baked into the material records.

import uniforms;
import tracing.rng;
import tracing.brdf;
import tracing.resources;
import tracing.material;
import tracing.trace;
import tracing.lighting;
import tracing.outputs;
import tracing.debug;

/** Additive layers (sabers, glow planes) per path. */
static const uint kMaxPassThroughSteps = 16;
/**
 * Real transmitting hits (smoke) per path. This is independent of ptBounces:
 * a short stack of puffs must not consume the two scattering vertices, but a
 * malformed overlapping emitter stack must never spin a GPU ray forever.
 */
static const uint kMaxTransmissionSteps = 12;

/**
 * The one exit: write the split, then the image - a channel debug view when
 * one is active, the assembled color otherwise. Every path out of the kernel
 * ends here, so the debug views see sky and miss pixels too.
 *
 * The assembled image leaves here linear. Every traced frame ends in the
 * common post-process pass, which owns the display transform, and a second
 * one applied here would be applied twice. A debug view is the exception in
 * both directions: it is a final diagnostic image the common tail is skipped
 * for, so it still finishes itself.
 */
void finishPixel(uint2 pixel, TraceOutputs outputs, PtDebug debug) {
    outputs.write(pixel);
    float3 color;
    if (debug.wantsChannelView() && debug.channelColor(outputs, color)) {
        if (!debug.rawChannelView()) {
            color = ptDisplayTransform(color, pushConstants.exposure, ptTonemapOperator());
        }
        outputImage[pixel] = float4(color, 1.0);
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

    // Unproject both depth-range ends rather than assuming a perspective
    // frustum: GUI scenes render through orthographic cameras, whose rays
    // are parallel with per-pixel origins. The projection carries the TAA
    // jitter, so the rays are jittered with it.
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

    // ---- The visibility ray ----
    // Coverage is a plain fixed-threshold alpha test: no RNG, no dither, so
    // the surface never flickers and the TAA jitter is the frame's only
    // stochastic input.
    //
    // Additive layers own nothing but their own light. A saber blade or a
    // glow plane in front of a wall contributes emission and nothing else -
    // it must not publish depth, motion, normal, roughness or albedo, because
    // those guides belong to the wall behind it. Letting the blade write them
    // reprojects the pixel along the blade's motion and drags the background
    // through TAA and the denoiser as a smear. So the ray walks the additive
    // layers, paying each one's emission into the noise-free channel, until a
    // surface with substance commits. That surface is the pixel.
    float3 layerEmission = float3(0.0);
    RayDesc probe = ray;
    TraceHit primary;
    HitGeometry primaryHit;
    SurfaceShading primaryShading;
    float primaryDistance = 0.0;
    uint layers = 0;
    while (true) {
        primary = ptTraceNearest(probe);
        if (!primary.hit) break;
        primaryDistance += primary.hitT;
        primaryHit = ptMakeHitGeometry(primary, probe, spreadAngle * primaryDistance);
        primaryShading = ptShade(primaryHit);
        if (instanceMaterials[primaryHit.materialIndex].surfaceType != kPtSurfaceUnlitTransparent) break;
        layerEmission += primaryShading.emission;
        if (++layers > kMaxPassThroughSteps) {
            primary.hit = false;
            break;
        }
        // Continue through the layer, not off it.
        probe.Origin = primaryHit.position + probe.Direction * 1e-3;
        probe.TMin = 0.001;
    }
    outputs.noiseFree = layerEmission;
    if (!primary.hit) {
        if (pushConstants.skyAvailable != 0) {
            outputs.noiseFree += ptSkyRadiance(ray.Direction);
            if (debug.wantsSurfaceView()) {
                outputImage[pixel] = float4(debug.skyMissColor(), 1.0);
                return;
            }
        }
        finishPixel(pixel, outputs, debug);
        return;
    }

    if (debug.wantsSurfaceView()) {
        outputImage[pixel] = float4(debug.surfaceColor(primaryHit, primaryShading, ray.Direction), 1.0);
        return;
    }

    // Transport remains tied to the shaded primary. In particular, its
    // demodulation factors cannot follow a different guide surface without
    // changing both the dumped radiance channels and the undenoised image.
    bool pbrPrimary = instanceMaterials[primaryHit.materialIndex].surfaceType == kPtSurfacePbr;
    bool primaryBlendedCoverage =
        (instanceMaterials[primaryHit.materialIndex].featureMask & kPtMaskBlendedCoverage) != 0;
    float transportViewZ = primaryShading.terminatesPath
                               ? kPtInfDistance
                               : mul(globalUniforms.view,
                                     float4(primaryHit.position, 1.0)).z;
    if (pbrPrimary && !primaryBlendedCoverage) {
        float3 Rf0 = lerp(lerp(float3(0.04), primaryShading.albedo, primaryShading.metallic),
                          float3(1.0), primaryShading.envStrength);
        ptNrdMaterialFactors(primaryShading.normal, -probe.Direction, primaryShading.albedo,
                             primaryShading.metallic, Rf0, primaryShading.roughness,
                             outputs.transportDiffFactor, outputs.transportSpecFactor);
    }

    // The guide surface is usually the transport primary, at no additional
    // traversal cost. A blended primary is the exceptional path: continue
    // just beyond it with a query that ignores every blended/additive layer.
    bool guideNeedsTrace = primaryBlendedCoverage ||
                           instanceMaterials[primaryHit.materialIndex].surfaceType ==
                               kPtSurfaceUnlitTransparent;
    TraceHit guide = primary;
    HitGeometry guideHit = primaryHit;
    SurfaceShading guideShading = primaryShading;
    bool guideHitFound = true;
    if (guideNeedsTrace) {
        RayDesc guideRay = probe;
        guideRay.Origin = primaryHit.position + guideRay.Direction *
                          max(pushConstants.rayOriginOffset, 1e-3);
        guideRay.TMin = 0.001;
        guide = ptTraceGuideNearest(guideRay);
        guideHitFound = guide.hit;
        if (guideHitFound) {
            guideHit = ptMakeHitGeometry(guide, guideRay,
                                         spreadAngle * (primaryDistance + guide.hitT));
            guideShading = ptShade(guideHit);
        }
    }

    if (guideHitFound) {
        // The sky sits at environment depth for the denoiser; every real
        // guide surface reports its own depth and motion. An unlit guide keeps
        // the default white factors because its radiance is emission.
        outputs.normal = guideShading.normal;
        outputs.roughness = guideShading.roughness;
        outputs.viewZ = guideShading.terminatesPath
                            ? kPtInfDistance
                            : mul(globalUniforms.view,
                                  float4(guideHit.position, 1.0)).z;
        outputs.motion = guideShading.terminatesPath
                             ? float3(0.0)
                             : ptMotionVector(guideHit);
        if (instanceMaterials[guideHit.materialIndex].surfaceType == kPtSurfacePbr) {
            if (!guideNeedsTrace) {
                outputs.diffFactor = outputs.transportDiffFactor;
                outputs.specFactor = outputs.transportSpecFactor;
            } else {
                float3 Rf0 = lerp(lerp(float3(0.04), guideShading.albedo,
                                      guideShading.metallic),
                                  float3(1.0), guideShading.envStrength);
                ptNrdMaterialFactors(guideShading.normal, -ray.Direction,
                                     guideShading.albedo, guideShading.metallic, Rf0,
                                     guideShading.roughness, outputs.diffFactor,
                                     outputs.specFactor);
            }
        }
        // The comparison G-buffer follows raster exactly: diffuse is
        // pre-lighting albedo, eye normal is signed view-space normal packed
        // into [0,1], depth is positive linear view distance, and motion is
        // current-minus-previous UV displacement from unjittered matrices.
        // VulkanGBuffer stores the authored gamma-encoded texture colour (the
        // deferred resolve converts it later), whereas tracing shades linear.
        outputs.gBufferDiffuse = float4(ptLinearToGamma(guideShading.albedo),
                                        guideShading.alpha);
        outputs.gBufferEyeNormal = 0.5 * mul(guideShading.normal,
                                             (float3x3)globalUniforms.viewInv) + 0.5;
        float guideViewZ = mul(globalUniforms.view, float4(guideHit.position, 1.0)).z;
        outputs.gBufferDepth = max(0.0, -guideViewZ);
        if (!guideShading.terminatesPath) {
            float4 currentClip = mul(globalUniforms.viewProjection,
                                     float4(guideHit.position, 1.0));
            float4 previousClip = mul(globalUniforms.prevViewProjection,
                                      float4(guideHit.position + outputs.motion, 1.0));
            if (currentClip.w > 1e-6 && previousClip.w > 1e-6) {
                outputs.gBufferMotion = 0.5 * (currentClip.xy / currentClip.w -
                                               previousClip.xy / previousClip.w);
            }
        }
        // The sky keeps the far-plane defaults: it has no position to
        // reproject and an upscaler must not try to chase one.
        if (!guideShading.terminatesPath) {
            outputs.setUpscalerGuides(pixel, uint2(width, height), guideHit.position,
                                      outputs.motion);
        }
    }
    // A transmitting primary carries radiance from surfaces behind it. That
    // mixed signal cannot be demodulated by the particle's material factor:
    // multiplying it back at composite would tint and darken the transmitted
    // background. Keep the channel in radiance space and apply the smoke's
    // response only to its own local contribution below.
    float primaryOpacity = primaryBlendedCoverage ? primaryShading.alpha : 1.0;
    outputs.noiseFree = layerEmission + primaryShading.emission * primaryOpacity;

    if (primaryShading.terminatesPath) {
        finishPixel(pixel, outputs, debug);
        return;
    }

    // ---- The SPP loop ----
    bool statsOn = ptStatsEnabled();
    uint sampleCount = max(pushConstants.samplesPerPixel, 1u);
    uint scatterBudget = max(pushConstants.bounceCount, 1u);
    float3 diffuseChannel = float3(0.0);
    float3 specularChannel = float3(0.0);
    float diffHitSum = 0.0, diffHitCount = 0.0;
    float specHitSum = 0.0, specHitCount = 0.0;
    uint secondaryRays = 0, secondaryMisses = 0;

    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        uint rng = ptHash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu ^
                          sampleIndex * 0xc2b2ae35u ^ pushConstants.frameIndex);
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
        uint transmissions = 0;

        // ---- The bounce loop ----
        while (true) {
            float3 view = -path.Direction;
            PathSample sample = ptSampleRay(hit, shading, view, rng);
            if (sample.terminate) break;
            bool blendedCoverage =
                (instanceMaterials[hit.materialIndex].featureMask & kPtMaskBlendedCoverage) != 0;
            if (!sample.passThrough) {
                DirectLight nee = ptDirectLight(hit, shading, view, rng);
                if (!pathDecided) {
                    pathDecided = true;
                    specularPath = sample.specularLobe;
                }
                if (primaryScatter) {
                    // Invariant: channel * diffFactor equals the primary
                    // diffuse radiance, so direct irradiance stays unscaled.
                    diffuseChannel += nee.diffuse;
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
                    pathRadiance += pathWeight *
                                    (nee.diffuse * diffuseMaterialResponse(shading, view) +
                                     nee.specular);
                    pathWeight *= sample.weight;
                }
                primaryScatter = false;
                if (++scatters > scatterBudget) break;
                path.Origin = hit.position + hit.geometricNormal * pushConstants.rayOriginOffset;
            } else if (!blendedCoverage) {
                // Additive layers pass through unattenuated. The visibility
                // walk already stepped past primary layers, so this is always
                // a deeper vertex.
                primaryScatter = false;
                if (++passThroughs > kMaxPassThroughSteps) break;
                pathWeight *= sample.weight;
                // Continue through the surface, not off it.
                path.Origin = hit.position + path.Direction * 1e-3;
            } else {
                // Blended coverage is a real, lit surface plus straight
                // transmission. Its scatter contribution is alpha-weighted;
                // only the transmitted remainder continues, and this does
                // not spend a scattering bounce.
                DirectLight nee = ptDirectLight(hit, shading, view, rng);
                float opacity = shading.alpha;
                if (!pathDecided) {
                    pathDecided = true;
                    specularPath = false;
                }
                if (primaryScatter) {
                    // Transmitted background and the particle's local light
                    // share one radiance-space channel. Demodulating both by
                    // the particle response makes a 2%-opaque puff multiply
                    // the entire background by its dark texture.
                    diffuseChannel += opacity *
                                      (nee.diffuse * diffuseMaterialResponse(shading, view) +
                                       nee.specular);
                } else {
                    pathRadiance += pathWeight * opacity *
                                    (nee.diffuse * diffuseMaterialResponse(shading, view) +
                                     nee.specular);
                }
                primaryScatter = false;
                // This is a hard termination condition. Retain every local
                // contribution accumulated above, but never trace a 13th
                // transmitting hit along this path.
                if (++transmissions > kMaxTransmissionSteps) break;
                pathWeight *= sample.weight;
                // Offset along the outgoing ray: a transmissive surface must
                // begin beyond the triangle it just hit, otherwise it can
                // repeatedly re-hit itself and hang the device.
                path.Origin = hit.position + path.Direction *
                              max(pushConstants.rayOriginOffset, 1e-3);
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
            // denoiser, and TAA alone was left to average it. Only the
            // visibility ray's deterministic layer walk earns that channel.
            float hitOpacity =
                (instanceMaterials[hit.materialIndex].featureMask & kPtMaskBlendedCoverage) != 0
                    ? shading.alpha
                    : 1.0;
            pathRadiance += pathWeight * shading.emission * hitOpacity;
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
    outputs.diffuseHitDist = diffHitCount > 0.0
                                 ? ptNormalizeHitDist(diffHitSum / diffHitCount, transportViewZ, 1.0)
                                 : 0.0;
    outputs.specularHitDist = specHitCount > 0.0
                                  ? ptNormalizeHitDist(specHitSum / specHitCount, transportViewZ,
                                                       primaryShading.roughness)
                                  : 0.0;
    finishPixel(pixel, outputs, debug);
}
