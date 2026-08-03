/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The NRD-facing output split, set 2, gathered behind one struct. The trace
// pass separates what is noise from what is signal so a denoiser can tell
// them apart. The composite contract:
//
//   final = noiseFree + diffuse.rgb * diffFactor + specular.rgb * specFactor
//
// The radiance channels are demodulated by the primary material factors so the
// denoiser sees transport rather than material variation, and each carries its REBLUR-
// normalized average first-bounce hit distance in alpha, converted to YCoCg
// at write. Normal/roughness match NRD's RGBA16_SNORM-with-FP encoding.
// Motion is world-space, previous minus current.

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

/** Environment / miss distance: beyond any scene, inside NRD's defaults. */
public static const float kPtInfDistance = 1.0e6;

public struct TraceOutputs {
    public float3 diffuse;      // demodulated diffuse radiance
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

    /** The assembled image the kernel writes when the denoiser is off. */
    public float3 assemble() {
        return noiseFree + diffuse * transportDiffFactor +
               specular * transportSpecFactor;
    }

    public void write(uint2 pixel) {
        outDiffuse[pixel] = float4(ptLinearToYCoCg(diffuse), diffuseHitDist);
        outSpecular[pixel] = float4(ptLinearToYCoCg(specular), specularHitDist);
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
    }
};

/**
 * REBLUR's hit-distance normalization at its default parameters
 * (3.0, 0.1, 20.0, -25.0), roughness-dependent for the specular channel and
 * evaluated at roughness 1 for diffuse.
 */
public float ptNormalizeHitDist(float hitDist, float viewZ, float roughness) {
    float f = (3.0 + abs(viewZ) * 0.1) *
              lerp(1.0, 20.0, saturate(exp2(-25.0 * roughness * roughness)));
    return saturate(hitDist / f);
}
