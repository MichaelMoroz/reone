/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Next-event estimation: one analytic light drawn per path vertex from a
// receiver-dependent importance table, shaded through the shared BRDF terms,
// shadowed by a cone-jittered ray. The same estimator serves every vertex of
// every path - the primary hit is not special.

import uniforms;
import tracing.rng;
import tracing.brdf;
import tracing.resources;
import tracing.material;
import tracing.trace;

// The direct-light selection weight: receiver-dependent importance, with
// attenuation pinned to 1 for promoted directionals so distance never erases
// the sun. Ambient-only point lights are inert here - ambient has no place in
// path tracing - while directionals are promoted to sampled suns even when
// authored ambient-only: raster has no direct sun at all, so there is no
// parity target and the Sun dial grades it.
float directLightWeight(GlobalUniformsLight light, float3 position, float3 shadingNormal) {
    bool directional = light.position.w == 0.0;
    float3 fragToLight = light.position.xyz - position;
    float lightDistance = length(fragToLight);
    // Raster's radius-squared cutoff quirk, kept for parity; ambient-only
    // points are inert in a path tracer.
    if (!directional && lightDistance > light.radius * light.radius) return 0.0;
    if (!directional && light.ambientOnly != 0) return 0.0;
    float attenuation = 1.0;
    if (!directional) {
        float d = light.radius + lightDistance;
        attenuation = (light.radius * light.radius) / (d * d);
    }
    float NdotL = max(0.0, dot(shadingNormal, fragToLight / max(lightDistance, 1e-4)));
    float luma = dot(ptGammaToLinear(light.color.rgb), float3(0.2126, 0.7152, 0.0722));
    return max(0.0, luma * light.multiplier * attenuation * NdotL);
}

public struct DirectLight {
    /** Diffuse radiance WITHOUT the albedo factor - demodulated. */
    public float3 diffuse;
    public float3 specular;
};

/**
 * Direct light at a path vertex: weighted single draw over the analytic
 * lights, BRDF-shaded, cone-jittered shadow ray from the terminator-shifted
 * origin. Geometry decides visibility - a light below the actual triangle
 * contributes nothing however curved the smooth normal pretends the surface
 * is - and the smooth normal only shades.
 */
public DirectLight ptDirectLight(HitGeometry hit, SurfaceShading shading, float3 view,
                                 inout uint rng) {
    DirectLight result;
    result.diffuse = float3(0.0);
    result.specular = float3(0.0);
    float totalWeight = 0.0;
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        totalWeight += directLightWeight(globalUniforms.lights[i], hit.position, shading.normal);
    }
    if (totalWeight <= 0.0) return result;
    float pick = ptRandom01(rng) * totalWeight;
    int selectedIndex = -1;
    float selectedWeight = 0.0;
    float accum = 0.0;
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        float w = directLightWeight(globalUniforms.lights[i], hit.position, shading.normal);
        if (w <= 0.0) continue;
        accum += w;
        selectedIndex = i;
        selectedWeight = w;
        if (pick <= accum) break;
    }
    if (selectedIndex < 0) return result;
    GlobalUniformsLight light = globalUniforms.lights[selectedIndex];
    bool directional = light.position.w == 0.0;
    // Directionals are placed as distant points; the position difference is
    // the light direction for both kinds, exactly as the raster path has it.
    float3 toLight = light.position.xyz - hit.position;
    float lightDistance = length(toLight);
    float3 L = toLight / max(lightDistance, 1e-4);
    // The smooth normal may claim this surface sees the light while the
    // actual triangle faces away. Trust the geometry.
    if (dot(L, hit.geometricNormal) <= 0.0) return result;
    float attenuation = 1.0;
    if (!directional) {
        float d = light.radius + lightDistance;
        attenuation = (light.radius * light.radius) / (d * d);
    }
    float3 radiance = ptGammaToLinear(light.color.rgb) * light.multiplier;
    radiance *= directional ? pushConstants.sunIntensity
                            : attenuation * pushConstants.directIntensity;
    float angular = directional ? pushConstants.sunAngularRadius
                                : pushConstants.pointAngularRadius;
    float3 shadowDirection = ptConeSample(L, angular, float2(ptRandom01(rng), ptRandom01(rng)));
    radiance *= ptShadowTransmittance(hit.shadowOrigin, hit.geometricNormal, shadowDirection,
                                      directional ? 10000.0 : lightDistance) *
                (totalWeight / max(selectedWeight, 1e-6));
    float NdotL = max(0.0, dot(shading.normal, L));
    float NdotV = max(0.0, dot(shading.normal, view));
    float3 F0 = lerp(lerp(float3(0.04), shading.albedo, shading.metallic), float3(1.0),
                     shading.envStrength);
    float3 H = normalize(view + L);
    float NdotH = max(0.0, dot(shading.normal, H));
    float VdotH = max(0.0, dot(view, H));
    float ggxAlpha = shading.roughness * shading.roughness;
    float geomK = shading.roughness + 1.0;
    geomK = geomK * geomK * (1.0 / 8.0);
    float D = ptDistributionGGX(NdotH * NdotH, ggxAlpha * ggxAlpha);
    float G = ptGeometrySmith(NdotL, NdotV, geomK);
    float3 F = ptFresnelSchlick(VdotH, F0);
    float3 spec = (D * G * F) / max(1e-4, 4.0 * NdotL * NdotV);
    float3 kD = (1.0 - F) * (1.0 - shading.metallic);
    // Demodulated: the caller multiplies albedo back in at assembly, so the
    // diffuse channel carries transport rather than texture. Raster's
    // dynamic-light specular is meaningful only on the same env-mapped
    // surfaces the tracer gives a specular lobe.
    result.diffuse = kD / kPtPi * radiance * NdotL;
    if (shading.envmapped) {
        result.specular = spec * radiance * NdotL;
    }
    return result;
}
