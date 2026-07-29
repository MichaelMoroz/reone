// The path-tracing kernel. Every hit reads its material and vertex
// attributes from GPU-resident data; no raster draw state is used.
//
// The structure is exactly: visibility ray -> SPP loop -> bounce loop.
// There is no separate transparency handling - transparent materials are
// surfaces like any other, committed nearest and resolved by their surface
// model: additive light passes the ray through, partial PBR coverage is a
// stochastic substance roll, sky and prelit imagery terminate. Modules:
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

/** Pass-through steps (additive layers, coverage misses) per path. */
static const uint kMaxPassThroughSteps = 8;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
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
    // Unlit-transparent surfaces occupy no volume: the visibility phase pays
    // their radiance deterministically - a saber blade's plane stack
    // composites identically every frame - and settles on the first
    // substantial surface, which owns the pixel's guides and anchors every
    // sample path. Without this, blade pixels carried environment guides and
    // the denoiser dropped everything behind them.
    // Coverage commits are stochastic per frame; TAA jitter and the denoiser
    // own the reconstruction, exactly as they do for the lighting.
    uint visibilityRng = ptHash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu ^
                                pushConstants.frameIndex ^ 0x9d2c5681u);
    TraceHit primary;
    float visibilityDistance = 0.0;
    for (uint layer = 0; ; ++layer) {
        primary = ptTraceNearest(ray, visibilityRng);
        if (!primary.hit) break;
        visibilityDistance += primary.hitT;
        if (primary.material.surfaceType != kPtSurfaceUnlitTransparent || layer >= 8) break;
        HitGeometry layerHit = ptMakeHitGeometry(primary, ray, spreadAngle * visibilityDistance);
        outputs.noiseFree += ptShade(layerHit).emission;
        ray.Origin = layerHit.position + ray.Direction * 1e-3;
    }
    if (!primary.hit) {
        outputs.write(pixel);
        outputImage[pixel] = float4(ptDisplayTransform(outputs.assemble(), pushConstants.exposure,
                                                       ptTonemapOperator()), 1.0);
        return;
    }
    HitGeometry primaryHit = ptMakeHitGeometry(primary, ray, spreadAngle * visibilityDistance);
    SurfaceShading primaryShading = ptShade(primaryHit);

    if (debug.wantsSurfaceView()) {
        outputImage[pixel] = float4(debug.surfaceColor(primaryHit, primaryShading, ray.Direction), 1.0);
        return;
    }

    outputs.normal = primaryShading.normal;
    outputs.roughness = primaryShading.roughness;
    outputs.viewZ = primaryShading.terminatesPath
                        ? kPtInfDistance
                        : mul(globalUniforms.view, float4(primaryHit.position, 1.0)).z;
    outputs.motion = primaryShading.terminatesPath ? float3(0.0) : ptMotionVector(primaryHit);
    outputs.albedo = primaryShading.terminatesPath ? float3(1.0) : primaryShading.albedo;
    outputs.noiseFree += primaryShading.emission;

    if (primaryShading.terminatesPath) {
        outputs.write(pixel);
        outputImage[pixel] = float4(ptDisplayTransform(outputs.assemble(), pushConstants.exposure,
                                                       ptTonemapOperator()), 1.0);
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
        RayDesc path = ray;
        float3 pathWeight = float3(1.0);
        float3 pathRadiance = float3(0.0);
        float pathDistance = visibilityDistance;
        // Channel routing: decided by the first scattering lobe. The primary
        // vertex's diffuse weight is demodulated (albedo lives in the guide);
        // deeper vertices carry full weights.
        bool pathDecided = false;
        bool specularPath = false;
        bool primaryScatter = true;
        float firstScatterT = -1.0;
        uint scatters = 0;
        uint passThroughs = 0;

        // ---- The bounce loop ----
        while (true) {
            float3 view = -path.Direction;
            PathSample sample = ptSampleRay(hit, shading, view, rng);
            if (sample.terminate) break;
            if (!sample.passThrough) {
                DirectLight nee = ptDirectLight(hit, shading, view, rng);
                if (primaryScatter) {
                    diffuseChannel += nee.diffuse;
                    specularChannel += nee.specular;
                    if (!pathDecided) {
                        pathDecided = true;
                        specularPath = sample.specularLobe;
                    }
                    // Demodulate the primary diffuse lobe: the albedo guide
                    // multiplies back in at assembly and at the composite.
                    pathWeight *= sample.specularLobe
                                      ? sample.weight
                                      : sample.weight / max(shading.albedo, float3(1e-3));
                    primaryScatter = false;
                } else {
                    pathRadiance += pathWeight * (nee.diffuse * shading.albedo + nee.specular);
                    pathWeight *= sample.weight;
                }
                if (++scatters > scatterBudget) break;
                path.Origin = hit.position + hit.geometricNormal * pushConstants.rayOriginOffset;
            } else {
                if (++passThroughs > kMaxPassThroughSteps) break;
                pathWeight *= sample.weight;
                // Continue through the surface, not off it.
                path.Origin = hit.position + path.Direction * 1e-3;
            }
            if (max(pathWeight.x, max(pathWeight.y, pathWeight.z)) < 1e-3) break;
            path.Direction = sample.direction;
            path.TMin = 0.001;
            path.TMax = 100000.0;

            TraceHit bounce = ptTraceNearest(path, rng);
            if (statsOn) ++secondaryRays;
            if (!bounce.hit) {
                if (statsOn) ++secondaryMisses;
                if (firstScatterT < 0.0 && !primaryScatter) firstScatterT = kPtInfDistance;
                break;
            }
            if (firstScatterT < 0.0 && !primaryScatter) firstScatterT = bounce.hitT;
            pathDistance += bounce.hitT;
            hit = ptMakeHitGeometry(bounce, path, spreadAngle * pathDistance);
            shading = ptShade(hit);
            // Every vertex past the first pays its emission - additive
            // glows, emissive panels, the sky - plus the lightmap radiance
            // cache on PBR surfaces (a bounded double-count preferred to
            // unlit interiors; the dial grades it).
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
    outputs.diffuseHitDist = diffHitCount > 0.0
                                 ? ptNormalizeHitDist(diffHitSum / diffHitCount, outputs.viewZ, 1.0)
                                 : 0.0;
    outputs.specularHitDist = specHitCount > 0.0
                                  ? ptNormalizeHitDist(specHitSum / specHitCount, outputs.viewZ,
                                                       primaryShading.roughness)
                                  : 0.0;
    outputs.write(pixel);

    float3 color;
    if (debug.wantsChannelView() && debug.channelColor(outputs, color)) {
        if (!debug.rawChannelView()) {
            color = ptDisplayTransform(color, pushConstants.exposure, ptTonemapOperator());
        }
        outputImage[pixel] = float4(color, 1.0);
        return;
    }
    outputImage[pixel] = float4(ptDisplayTransform(outputs.assemble(), pushConstants.exposure,
                                                   ptTonemapOperator()), 1.0);
}
