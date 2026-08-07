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

// The microfacet BRDF terms and color transforms shared by the path tracer's
// modules. Pure functions; the display transform lives here too because it is
// color math, taking its dials as arguments rather than reading push
// constants.

import tracing.rng;

public static const float kPtGamma = 2.2;
public static const float kDemodulationFloor = 0.02;
public static const float kNrdRoughnessFactorMinScale = 0.1;

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

/**
 * Port of _NRD_EnvironmentTerm_Rtg from NRD v4.17.3 Shaders/NRD.hlsli.
 * Keep this fit and ptNrdMaterialFactors aligned with the NRD front end:
 * the factors are stored at the primary hit and used unchanged after NRD.
 */
float3 ptNrdEnvironmentTermRtg(float3 Rf0, float NoV, float roughness) {
    float m = saturate(roughness * roughness);
    float4 X = float4(1.0, NoV, NoV * NoV, NoV * NoV * NoV);
    float4 Y = float4(1.0, m, m * m, m * m * m);
    const float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    const float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592,
                                 121.563, 626.13, 316.627);
    const float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    const float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949,
                                 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(M1, X.xy), Y.xy) /
                 max(dot(mul(M2, X.xyw), Y.xyw), 1e-6);
    float scale = dot(mul(M3, X.xy), Y.xy) /
                  max(dot(mul(M4, X.xzw), Y.xyw), 1e-6);
    return saturate(Rf0 * scale + bias);
}

public void ptNrdMaterialFactors(float3 N, float3 V, float3 albedo, float metallic, float3 Rf0,
                                  float roughness, out float3 diffFactor,
                                  out float3 specFactor) {
    float3 Fenv = ptNrdEnvironmentTermRtg(Rf0, abs(dot(N, V)), roughness);
    // Unfloored, unlike NRD's own helper. NRD raises this off zero because it
    // DIVIDES the signal by it; our diffuse channel holds transport and is
    // never divided, so the floor stops being protection and becomes bias -
    // the composite would multiply back 0.02 + 0.98x where the correct
    // multiplier is x, brightening dark materials by construction.
    diffFactor = (1.0 - Fenv) * (1.0 - metallic) * albedo;
    specFactor = Fenv * lerp(kNrdRoughnessFactorMinScale, 1.0, roughness);
    specFactor = lerp(float3(kDemodulationFloor), float3(1.0), specFactor);
}

// Scene-referred in, display-referred out. The calibration rule survives the
// change of curve: direct light alone should land around 0.3-0.5 of the final
// value in lit areas, leaving headroom that bounce light fills.
/**
 * The Gran Turismo tone curve: a toe, a linear midsection and a shoulder that
 * approaches the peak asymptotically, so highlights compress instead of
 * clipping and midtones keep the slope the artist authored. Replaces the ACES
 * fit that stood here, which crushed the shadows and shifted hue on the way.
 */
float GTTonemap(float x)
{
    float P = 1.0f;
    float a = 1.0f;
    float m = 0.22f;
    float l = 0.4f;
    float c = 1.33f;
    float b = 0.0f;
    float l0 = (P - m) * l / a;
    float Lx = m + a * (x - m);
    float Tx = m * pow(x / m, c) + b;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = a * P / (P - S1);
    float Sx = P - (P - S1) * exp(-C2 * (x - S0) / P);
    float w0 = 1.0f - smoothstep(0, m, x);
    float w2 = step(m + l0, x);
    float w1 = 1.0f - w0 - w2;
    return Tx * w0 + Lx * w1 + Sx * w2;
}

float3 GTTonemap(float3 c)
{
    // Another option is to tonemap max from rgb components and scale others accordingly
    return float3(GTTonemap(c.r), GTTonemap(c.g), GTTonemap(c.b));
}

/** tonemap: 0 pass-through, 1 the Gran Turismo curve; the Tonemap dial. */
public float3 ptDisplayTransform(float3 linearColor, float exposure, uint tonemap) {
    float3 mapped = linearColor * exposure;
    if (tonemap == 1) mapped = GTTonemap(mapped);
    return ptLinearToGamma(mapped);
}

/**
 * REBLUR filters radiance in YCoCg; the front end owes it the conversion on
 * the noisy channels and the composite undoes it on the denoised result.
 * RELAX takes linear radiance instead and neither of these runs for it.
 *
 * These are _NRD_LinearToYCoCg / _NRD_YCoCgToLinear from NRD.hlsli, transcribed
 * rather than reimplemented. What stood here before was the reversible lifting
 * transform, which agrees on luma but carries twice the chroma: the pair
 * round-tripped so colour came out right, and every chroma-sensitive decision
 * inside REBLUR - the fast-history colour box, the firefly clamp - silently saw
 * a box twice as wide as the one it was tuned for.
 */
public float3 ptLinearToYCoCg(float3 color) {
    float Y = dot(color, float3(0.25, 0.5, 0.25));
    float Co = dot(color, float3(0.5, 0.0, -0.5));
    float Cg = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

public float3 ptYCoCgToLinear(float3 color) {
    float t = color.x - color.z;
    float3 r;
    r.y = color.x + color.z;
    r.x = t + color.y;
    r.z = t - color.y;
    return max(r, 0.0);
}
