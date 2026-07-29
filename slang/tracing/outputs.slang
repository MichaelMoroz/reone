/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The NRD-facing output split, set 2, gathered behind one struct. The trace
// pass separates what is noise from what is signal so a denoiser can tell
// them apart. The composite contract:
//
//   final = noiseFree + diffuse.rgb * albedo + specular.rgb
//
// Diffuse is demodulated - primary albedo divided out so the denoiser sees
// transport, not texture - and each radiance channel carries its REBLUR-
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
[[vk::binding(6, 2)]] RWTexture2D<float4> outAlbedo;

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
    public float3 albedo;       // demodulation guide

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
        outputs.albedo = float3(1.0);
        return outputs;
    }

    /** The assembled image the kernel writes when the denoiser is off. */
    public float3 assemble() {
        return noiseFree + diffuse * albedo + specular;
    }

    public void write(uint2 pixel) {
        outDiffuse[pixel] = float4(ptLinearToYCoCg(diffuse), diffuseHitDist);
        outSpecular[pixel] = float4(ptLinearToYCoCg(specular), specularHitDist);
        outNormalRoughness[pixel] = float4(normal, roughness);
        outViewZ[pixel] = viewZ;
        outMotion[pixel] = float4(motion, 0.0);
        outNoiseFree[pixel] = float4(noiseFree, 1.0);
        outAlbedo[pixel] = float4(albedo, 1.0);
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
