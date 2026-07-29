/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The microfacet BRDF terms and color transforms shared by the path tracer's
// modules. Pure functions; the display transform lives here too because it is
// color math, taking its dials as arguments rather than reading push
// constants.

import tracing.rng;

public static const float kPtGamma = 2.2;

public float3 ptGammaToLinear(float3 rgb) {
    return pow(rgb, float3(kPtGamma));
}

public float3 ptLinearToGamma(float3 rgb) {
    return pow(max(rgb, 0.0), 1.0 / kPtGamma);
}

public float3 ptFresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

public float ptDistributionGGX(float NdotH2, float a2) {
    float d = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (kPtPi * d * d);
}

public float ptGeometrySchlick(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

public float ptGeometrySmith(float NdotL, float NdotV, float k) {
    return ptGeometrySchlick(NdotL, k) * ptGeometrySchlick(NdotV, k);
}

// Narkowicz's ACES fit. Scene-referred in, display-referred out; the
// calibration rule is that direct light alone lands around 0.3-0.5 of the
// final value in lit areas, leaving headroom that bounce light fills.
public float3 ptAcesTonemap(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (x * 2.43 + 0.59) + 0.14));
}

/** tonemap: 0 pass-through, 1 ACES; matches the Tonemap panel dial. */
public float3 ptDisplayTransform(float3 linearColor, float exposure, uint tonemap) {
    float3 mapped = linearColor * exposure;
    if (tonemap == 1) mapped = ptAcesTonemap(mapped);
    return ptLinearToGamma(mapped);
}

/**
 * REBLUR filters radiance in YCoCg; the front end owes it the conversion on
 * the noisy channels and the composite undoes it on the denoised result.
 * Mirrors _NRD_LinearToYCoCg / _NRD_YCoCgToLinear in NRD.hlsli.
 */
public float3 ptLinearToYCoCg(float3 color) {
    float Co = color.x - color.z;
    float t = color.z + Co * 0.5;
    float Cg = color.y - t;
    float Y = t + Cg * 0.5;
    return float3(Y, Co, Cg);
}

public float3 ptYCoCgToLinear(float3 color) {
    float t = color.x - color.z * 0.5;
    float g = color.z + t;
    float b = t - color.y * 0.5;
    return max(float3(b + color.y, g, b), 0.0);
}
