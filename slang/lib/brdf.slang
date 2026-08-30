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

// The microfacet BRDF terms. One implementation for the path tracer and the
// PBR provider, which are required to agree: pure functions, no bindings.

module brdf;

import lib.math;

public float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

/** The ambient lobe's Fresnel, with a roughness ceiling the punctual form does not take. */
public float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    float3 ceiling = max(F0, float3(1.0 - roughness));
    return F0 + (ceiling - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

public float distributionGGX(float NdotH2, float a2) {
    float d = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

public float geometrySchlick(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

public float geometrySmith(float NdotL, float NdotV, float k) {
    return geometrySchlick(NdotL, k) * geometrySchlick(NdotV, k);
}

/** Smith k for image-based lighting, alpha / 2. */
public float iblSmithK(float roughness) {
    return (roughness * roughness) * 0.5;
}

/** Smith k for punctual lights, (roughness + 1)^2 / 8. */
public float punctualSmithK(float roughness) {
    return (roughness + 1.0) * (roughness + 1.0) * 0.125;
}

/**
 * Reflectance at normal incidence: the dielectric base f0 tinted toward
 * albedo by metalness, lifted toward a mirror by the authored env-map share.
 */
public float3 materialRf0(float3 albedo, float metallic, float envStrength, float f0) {
    return lerp(lerp(float3(f0), albedo, metallic), float3(1.0), envStrength);
}

/**
 * The GGX lobe for a known light direction, with the cosine factor cancelled
 * analytically: D * G * F / (4 * NdotV). Forming NdotL numerically and
 * dividing it back out turns the guard into a large gain at grazing incidence.
 * Multiply by the light's radiance.
 */
public float3 specularGGX(float3 N, float3 V, float3 L, float roughness, float3 F0) {
    float NdotL = max(0.0, dot(N, L));
    float NdotV = max(0.0, dot(N, V));
    float3 H = normalize(V + L);
    float NdotH = max(0.0, dot(N, H));
    float VdotH = max(0.0, dot(V, H));
    float alpha = roughness * roughness;
    float D = distributionGGX(NdotH * NdotH, alpha * alpha);
    float G = geometrySmith(NdotL, NdotV, punctualSmithK(roughness));
    float3 F = fresnelSchlick(VdotH, F0);
    return (D * G * F) / max(1e-4, 4.0 * NdotV);
}
