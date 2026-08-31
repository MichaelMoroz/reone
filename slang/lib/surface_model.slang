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
module surface_model;

import lib.brdf;

/*
 * The surface model: how a shaded surface responds to the light that reaches
 * it. ONE implementation, consumed by every renderer through the channel
 * contract in lib/assemble.slang - the demodulation factors written at the
 * primary are the surface response, and the composite multiplies them back.
 *
 * Nothing here reads a binding or a push constant; every dial is an argument.
 */

/**
 * Port of _NRD_EnvironmentTerm_Rtg from NRD v4.17.3 Shaders/NRD.hlsli. Keep
 * this fit and materialFactors aligned with the NRD front end: the factors
 * are stored at the primary and used unchanged after denoising.
 */
public float3 nrdEnvironmentTermRtg(float3 Rf0, float NoV, float roughness) {
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

public static const float kDemodulationFloor = 0.02;
public static const float kNrdRoughnessFactorMinScale = 0.1;

/** The specular layer's hemispherical response at one view angle. */
public struct SpecularEnergy {
    /** Directional albedo, with the multiply-scattered energy put back. */
    public float3 albedo;
    /** Turquin's scale on the single-scatter lobe: 1 + F0 (1 - Ess) / Ess. */
    public float3 compensation;
};

/**
 * The split-sum fit above is linear in F0, so its F0 = 1 value is the
 * single-scatter directional albedo Ess that the compensation needs.
 */
public SpecularEnergy specularEnergy(SpecularLayer layer, float NoV, float roughness) {
    float Ess = nrdEnvironmentTermRtg(float3(1.0), NoV, roughness).x;
    SpecularEnergy energy;
    energy.compensation = 1.0 + layer.weight * layer.f0 * (1.0 - Ess) / max(Ess, 1e-3);
    energy.albedo = layer.weight * nrdEnvironmentTermRtg(layer.f0, NoV, roughness) * energy.compensation;
    return energy;
}

/** The diffuse layer gets the energy the specular layer did not take. */
public float3 diffuseResponse(float3 albedo, float metallic, SpecularEnergy energy) {
    return (1.0 - energy.albedo) * (1.0 - metallic) * albedo;
}

/**
 * The demodulation factors of one surface: what a unit of diffuse and of
 * specular transport becomes once the material is put back.
 */
public void materialFactors(float3 N, float3 V, float3 albedo, float metallic,
                            float specular, float f0, float roughness,
                            out float3 diffFactor, out float3 specFactor) {
    SpecularLayer layer = materialSpecular(albedo, metallic, specular, f0);
    SpecularEnergy energy = specularEnergy(layer, abs(dot(N, V)), roughness);
    // Unfloored, unlike NRD's own helper. NRD raises this off zero because it
    // DIVIDES the signal by it; the diffuse channel holds transport and is
    // never divided, so the floor stops being protection and becomes bias -
    // the composite would multiply back 0.02 + 0.98x where the correct
    // multiplier is x, brightening dark materials by construction.
    diffFactor = diffuseResponse(albedo, metallic, energy);
    specFactor = energy.albedo * lerp(kNrdRoughnessFactorMinScale, 1.0, roughness);
    specFactor = lerp(float3(kDemodulationFloor), float3(1.0), specFactor);
}
