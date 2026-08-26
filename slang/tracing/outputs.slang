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

// The NRD-facing output split, set 2, gathered behind one struct. The trace
// pass separates what is noise from what is signal so a denoiser can tell
// them apart. The composite contract:
//
//   final = noiseFree + diffuse.rgb * diffFactor + specular.rgb * specFactor
//
// The radiance channels are demodulated by the primary material factors so the
// denoiser sees transport rather than material variation, and each carries its
// average first-bounce hit distance in alpha, in world units, which is what
// RELAX reads. Normal/roughness match NRD's RGBA16_SNORM-with-FP encoding.
// Motion is world-space, previous minus current.

import lib.assemble;
import uniforms;
import tracing.brdf;
import tracing.resources;

[[vk::binding(0, 2)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(1, 2)]] RWTexture2D<float4> outSpecular;
[[vk::binding(2, 2)]] RWTexture2D<float4> outNormalRoughness;
[[vk::binding(3, 2)]] RWTexture2D<float> outViewZ;
[[vk::binding(4, 2)]] RWTexture2D<float4> outMotion;
[[vk::binding(5, 2)]] RWTexture2D<float4> outNoiseFree;
[[vk::binding(6, 2)]] RWTexture2D<float4> outDiffFactor;
// The upscaler's two guides. NRD and FSR want different things and both are
// right: NRD takes world-space motion and a linear viewZ, FSR takes a
// post-projection device depth and a UV-space screen motion. Deriving one from
// the other downstream would mean unprojecting what the tracer already has in
// hand, so both are written here at the point of the hit.
[[vk::binding(7, 2)]] RWTexture2D<float> outDeviceDepth;
[[vk::binding(8, 2)]] RWTexture2D<float4> outScreenMotion;
[[vk::binding(9, 2)]] RWTexture2D<float4> outSpecFactor;
// Canonical primary-hit G-buffer for direct raster/tracer comparison. These
// formats and encodings deliberately match VulkanGBuffer, not NRD's guides.
[[vk::binding(10, 2)]] RWTexture2D<float4> outGBufferDiffuse;
[[vk::binding(11, 2)]] RWTexture2D<float4> outGBufferEyeNormal;
[[vk::binding(12, 2)]] RWTexture2D<float> outGBufferDepth;
[[vk::binding(13, 2)]] RWTexture2D<float2> outGBufferMotion;
// Direct diffuse light at the primary vertex. Written linear and unpacked: no
// YCoCg. Diffuse only - the specular half stays in the denoised channel, where
// its hit distance and roughness drive a kernel built for it.
//
// This channel does reach a denoiser, under ShadowFilter::Denoiser, as its own
// NRD instance rather than a summand of the diffuse one - see
// TracingDenoiserInputs::directRadianceHitDist for why the two signals cannot
// share a kernel.
//
// NRD documents that slot as IN_DIFF_RADIANCE_HITDIST, so alpha ought to be a
// hit distance rather than the penumbra written below. It is not, and nothing
// reads it: RELAX_PrePass is the only pass that takes the incoming diffuse
// alpha as a distance, and the direct denoiser sets diffusePrepassBlurRadius to
// 0 with hit-distance reconstruction off, so that pass contributes nothing.
// Everything downstream rewrites alpha with its own variance, moments and
// history length. That is what makes the penumbra free here. Measured
// 2026-08-09: correcting it moved the frame by 0.30 mean against a 0.23
// nondeterminism floor. TRC-048, closed.
[[vk::binding(14, 2)]] RWTexture2D<float4> outDirectDiffuse;

/** Environment / miss distance; the assembly contract owns the value. */
public static const float kPtInfDistance = kAssembleInfDistance;

public struct TraceOutputs {
    public float3 diffuse;      // demodulated diffuse radiance
    /** Primary-vertex direct diffuse, demodulated the same way, denoiser-free. */
    public float3 directDiffuse;
    /** Expected penumbra half-width in world units; 0 where nothing blocked. */
    public float penumbra;
    public float diffuseHitDist;
    public float3 specular;
    public float specularHitDist;
    public float3 normal;
    public float roughness;
    public float viewZ;
    public float3 motion;
    public float3 noiseFree;    // layers, emission, sky - bypasses the denoiser
    public float3 diffFactor;
    public float3 specFactor;
    // The output factor textures describe the guide surface. These internal
    // assembly factors retain the transport primary's material response so a
    // guide change cannot alter the undenoised radiance.
    public float3 transportDiffFactor;
    public float3 transportSpecFactor;
    /**
     * Post-projection depth in [0,1], jittered exactly like the colour, and
     * UV-space screen motion as previous minus current with y down - the two
     * conventions FSR requires. The screen motion is deliberately built from
     * the UNJITTERED matrices at both ends: FSR wants motion with no jitter in
     * it, and taking the current position through the jittered projection
     * would fold this frame's sub-pixel offset into every vector.
     */
    public float deviceDepth;
    public float2 screenMotion;
    public float4 gBufferDiffuse;
    public float3 gBufferEyeNormal;
    public float gBufferDepth;
    public float2 gBufferMotion;

    /** Miss / environment defaults; the shaded path overwrites. */
    public static TraceOutputs makeDefault() {
        TraceOutputs outputs;
        outputs.diffuse = float3(0.0);
        outputs.directDiffuse = float3(0.0);
        outputs.penumbra = 0.0;
        outputs.diffuseHitDist = 0.0;
        outputs.specular = float3(0.0);
        outputs.specularHitDist = 0.0;
        outputs.normal = float3(0.0);
        outputs.roughness = 1.0;
        outputs.viewZ = kPtInfDistance;
        outputs.motion = float3(0.0);
        outputs.noiseFree = float3(0.0);
        outputs.diffFactor = float3(1.0);
        outputs.specFactor = float3(1.0);
        outputs.transportDiffFactor = float3(1.0);
        outputs.transportSpecFactor = float3(1.0);
        // A miss is the far plane, standing still.
        outputs.deviceDepth = 1.0;
        outputs.screenMotion = float2(0.0);
        outputs.gBufferDiffuse = float4(0.0);
        outputs.gBufferEyeNormal = float3(0.5);
        outputs.gBufferDepth = kPtInfDistance;
        outputs.gBufferMotion = float2(0.0);
        return outputs;
    }

    /** Fill the upscaler guides for a surface at this world position. */
    [mutating]
    public void setUpscalerGuides(uint2 pixel, uint2 extent, float3 position, float3 worldMotion) {
        // Jittered, because the colour FSR pairs this with is jittered.
        float4 clip = mul(globalUniforms.projection,
                          mul(globalUniforms.view, float4(position, 1.0)));
        deviceDepth = clip.w > 1e-6 ? saturate(clip.z / clip.w) : 1.0;

        // Unjittered at both ends, so no sub-pixel offset survives into the
        // vector. viewProjection is this frame's without the jitter translate;
        // prevViewProjection is last frame's, likewise.
        float4 currentClip = mul(globalUniforms.viewProjection, float4(position, 1.0));
        float4 previousClip = mul(globalUniforms.prevViewProjection,
                                  float4(position + worldMotion, 1.0));
        if (currentClip.w > 1e-6 && previousClip.w > 1e-6) {
            float2 currentUV = float2(currentClip.x, -currentClip.y) / currentClip.w * 0.5 + 0.5;
            float2 previousUV = float2(previousClip.x, -previousClip.y) / previousClip.w * 0.5 + 0.5;
            screenMotion = previousUV - currentUV;
        } else {
            screenMotion = float2(0.0);
        }
    }

    /** The assembled image the kernel writes when the denoiser is off. Fog is
        applied here too, so the diagnostic denoiser-off image and the composited
        one agree; the blend amount is 0 on any unfogged surface, so this is
        identity there. */
    public float3 assemble() {
        float3 radiance = assembleRadiance(noiseFree, diffuse, directDiffuse, specular,
                                           transportDiffFactor, transportSpecFactor);
        // From this pixel's depth, exactly as the composite does it - one
        // rule, so the denoiser-off image and the composited one agree.
        if (!ptFogEnabled()) {
            return radiance;
        }
        const float fog = fogFromViewDepth(viewZ, globalUniforms.fogNear,
                                           globalUniforms.fogFar);
        return applyFog(radiance, ptGammaToLinear(globalUniforms.fogColor.rgb), fog);
    }

    public void write(uint2 pixel) {
        // RELAX reads linear radiance and a raw world-space hit distance, which
        // is what these are. This was a branch while REBLUR was selectable:
        // that one wants YCoCg and a hit distance normalized against its own
        // curve, and feeding either denoiser the other's packing is silent,
        // because both are four floats in range. One denoiser, one encoding,
        // and no way left to get it wrong.
        outDiffuse[pixel] = float4(diffuse, diffuseHitDist);
        outSpecular[pixel] = float4(specular, specularHitDist);
        outNormalRoughness[pixel] = float4(normal, roughness);
        outViewZ[pixel] = viewZ;
        outMotion[pixel] = float4(motion, 0.0);
        outNoiseFree[pixel] = float4(noiseFree, 1.0);
        outDiffFactor[pixel] = float4(diffFactor, 1.0);
        outDeviceDepth[pixel] = deviceDepth;
        outScreenMotion[pixel] = float4(screenMotion, 0.0, 0.0);
        outSpecFactor[pixel] = float4(specFactor, 1.0);
        outGBufferDiffuse[pixel] = gBufferDiffuse;
        outGBufferEyeNormal[pixel] = float4(gBufferEyeNormal, 0.0);
        // Raster clears depth to its far plane. Make misses comparable rather
        // than using NRD's large environment sentinel.
        outGBufferDepth[pixel] = min(gBufferDepth, globalUniforms.clipFar);
        outGBufferMotion[pixel] = gBufferMotion;
        // Deliberately outside the branch above: alpha is the penumbra for the
        // spatial shadow filter, and RELAX never reads it - see the binding
        // comment for which pass would have, and why it is switched off. Do not
        // "fix" this to a hit distance without first checking that something
        // consumes one; that was tried, measured at the noise floor, reverted.
        outDirectDiffuse[pixel] = float4(directDiffuse, penumbra);
    }
};

