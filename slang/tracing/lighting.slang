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

// Next-event estimation: analytic lights drawn per path vertex from a
// receiver-dependent importance table, shaded through the shared BRDF terms,
// shadowed by a cone-jittered ray. The same estimator serves every vertex.

import uniforms;
import lib.direct_light;
import lib.scene_schema;
import tracing.rng;
import lib.brdf;
import lib.math;
import tracing.resources;
import tracing.material;
import tracing.trace;

/**
 * A point light is a sphere whose emitter radius is a dialled fraction of the
 * authored influence radius - KotOR authored no emitter size.
 */
struct SphereLight {
    /** Half-angle of the cone the emitter fills, for shadow sampling. */
    float angularRadius;
    /** Distance to the near surface of the emitter, not to its centre. */
    float shadowDistance;
};

SphereLight ptSphereLight(GlobalUniformsLight light, float lightDistance) {
    SphereLight result;
    float emitterRadius = light.radius * max(pushConstants.emitterRadiusRatio, 1e-3);
    result.angularRadius = asin(saturate(emitterRadius / max(lightDistance, 1e-4)));
    result.shadowDistance = max(lightDistance - emitterRadius, 1e-3);
    return result;
}

/**
 * Selection weight: receiver-dependent importance. Attenuation is pinned to 1
 * for directionals so distance never erases the sun; ambient-only point lights
 * are inert. A thin surface weights a back-side light by the transmission it
 * will receive, so the back-lit branch can be drawn at all.
 */
float directLightWeight(GlobalUniformsLight light, float3 position, float3 shadingNormal,
                        bool thin) {
    bool directional = light.position.w == 0.0;
    if (!directional && light.ambientOnly != 0) return 0.0;
    float3 fragToLight = directional ? light.position.xyz : light.position.xyz - position;
    float lightDistance = directional ? 1.0 : length(fragToLight);
    float attenuation = directional
        ? 1.0
        : sphereLightAttenuation(light.radius, lightDistance, pushConstants.emitterRadiusRatio);
    float cosine = dot(shadingNormal, fragToLight / max(lightDistance, 1e-4));
    float NdotL = thin && cosine < 0.0 ? -cosine * pushConstants.thinTransmission : max(0.0, cosine);
    float luma = luminance(directLightColor(light));
    return max(0.0, luma * attenuation * NdotL);
}

float totalDirectLightWeight(float3 position, float3 shadingNormal, bool thin) {
    float total = 0.0;
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        total += directLightWeight(globalUniforms.lights[i], position, shadingNormal, thin);
    }
    return total;
}

public struct DirectLight {
    /** Diffuse irradiance transport before the surface response. */
    public float3 diffuse;
    public float3 specular;
    /** Expected penumbra half-width in world units, d*tan(t) at blocker distance d; 0 unblocked. */
    public float penumbra;
};

/**
 * Grass occlusion already traced at this vertex, per light. A draw that picks
 * a cached light traces geometry only and multiplies the factor back in.
 * Two entries, unrolled: draws overwhelmingly re-pick the dominant light.
 */
static const uint kGrassCacheSize = 2u;
struct GrassShadowCache {
    int light[kGrassCacheSize];
    float2 grass[kGrassCacheSize];
    uint count;
};

/**
 * One next-event draw: pick a light from the importance table, trace one
 * shadow ray at it, evaluate the BSDF for that direction. Every draw is
 * distributed identically - the cache changes what is traced, not what is
 * estimated.
 */
DirectLight ptDirectLightDraw(HitGeometry hit, SurfaceShading shading, float3 view,
                              float2 lightXi, inout uint rng, float totalWeight,
                              inout GrassShadowCache cache) {
    DirectLight result = { float3(0.0), float3(0.0), 0.0 };
    if (totalWeight <= 0.0) return result;
    float pick = ptRandom01(rng) * totalWeight;
    int selectedIndex = -1;
    float selectedWeight = 0.0;
    float accum = 0.0;
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        float w = directLightWeight(globalUniforms.lights[i], hit.position, shading.normal,
                                    shading.thin);
        if (w <= 0.0) continue;
        accum += w;
        selectedIndex = i;
        selectedWeight = w;
        if (pick <= accum) break;
    }
    if (selectedIndex < 0) return result;
    GlobalUniformsLight light = globalUniforms.lights[selectedIndex];
    // The shared module owns what the light is; this file owns the shadow ray,
    // the estimator weight and the lobe. A directional's w == 0 IS the
    // direction toward it, the same vector for every receiver.
    DirectLightSample sampled = sampleDirectLight(light, hit.position,
                                                  pushConstants.emitterRadiusRatio,
                                                  pushConstants.directIntensity,
                                                  pushConstants.sunIntensity);
    bool directional = sampled.directional;
    float3 L = sampled.direction;
    // Not culled against the geometric normal: that test is binary over a
    // constant-per-triangle normal and draws facet edges on smooth meshes. A
    // thin surface has no far side to be in shadow.
    bool litFromBehind = dot(L, shading.normal) <= 0.0;
    if (litFromBehind && !shading.thin) return result;
    SphereLight sphere = ptSphereLight(light, sampled.distance);
    // The sun is not placed at a physical distance; its angle is a dial.
    float angular = directional ? pushConstants.sunAngularRadius : sphere.angularRadius;
    float3 shadowDirection = ptConeSample(L, angular, lightXi);
    // Leave from the side the light is on, or a back-lit thin surface buries
    // the origin inside itself.
    float3 shadowNormal = shading.thin && dot(hit.geometricNormal, L) < 0.0
                              ? -hit.geometricNormal
                              : hit.geometricNormal;
    int cached = -1;
    [ForceUnroll]
    for (uint c = 0; c < kGrassCacheSize; ++c) {
        if (c < cache.count && cache.light[c] == selectedIndex) cached = int(c);
    }
    bool traceGrass = ptFlag(kPtFlagGrassShadows) && cached < 0;
    uint inclusion = kPtInstanceGeometry | (traceGrass ? kPtInstanceGrass : 0u);
    ShadowRay shadow = ptShadowRay(hit.shadowOrigin, shadowNormal, shadowDirection,
                                   directional ? 10000.0 : sphere.shadowDistance, inclusion);
    float2 grass = float2(shadow.grassTransmittance, shadow.grassDistance);
    [ForceUnroll]
    for (uint c = 0; c < kGrassCacheSize; ++c) {
        if (cached == int(c)) grass = cache.grass[c];
    }
    if (traceGrass && shadow.grassKnown && cache.count < kGrassCacheSize) {
        [ForceUnroll]
        for (uint c = 0; c < kGrassCacheSize; ++c) {
            if (cache.count == c) {
                cache.light[c] = selectedIndex;
                cache.grass[c] = grass;
            }
        }
        ++cache.count;
    }
    float transmittance = shadow.transmittance * grass.x;
    float occluderDistance = min(shadow.occluderDistance, grass.x < 1.0 ? grass.y : kPtNoOccluder);
    // Small-angle form for both kinds; understates the penumbra only where the
    // blocker is nearly the light's own fixture.
    result.penumbra = transmittance < 1.0 && occluderDistance < kPtNoOccluder
                          ? occluderDistance * tan(angular)
                          : 0.0;
    if (transmittance <= 0.0) return result;
    float3 radiance = sampled.radiance * transmittance * (totalWeight / max(selectedWeight, 1e-6));
    // The primary diffuse channel has no material response; its NRD factor
    // restores the lobe after denoising.
    result.diffuse = directLightDiffuse(radiance, shading.normal, L,
                                        shading.thin, pushConstants.thinTransmission);
    // Specular keeps the true normal and the true side: a leaf transmits light,
    // not a highlight. Only env-mapped surfaces carry a specular lobe.
    if (shading.envmapped && !litFromBehind) {
        float3 F0 = materialRf0(shading.albedo, shading.metallic, shading.envStrength);
        result.specular = specularGGX(shading.normal, view, L, shading.roughness, F0) * radiance;
    }
    return result;
}

/**
 * Next-event estimation averaged over neeSamples draws. Each draw re-picks its
 * light, so the mean is the single-draw estimate and only the variance falls.
 * Grass is traced once per distinct light and shared through the cache, which
 * keeps the draws identically distributed: averaging draws that see grass with
 * draws that do not would dilute every grass shadow by 1/N. The penumbra
 * averages over the occluded draws only - an unoccluded draw has no blocker.
 */
public DirectLight ptDirectLight(HitGeometry hit, SurfaceShading shading, float3 view,
                                 float2 lightXi, inout uint rng) {
    uint draws = max(pushConstants.neeSamples, 1u);
    float totalWeight = totalDirectLightWeight(hit.position, shading.normal, shading.thin);
    GrassShadowCache cache;
    cache.count = 0;
    DirectLight total = ptDirectLightDraw(hit, shading, view, lightXi, rng, totalWeight, cache);
    if (draws == 1u) return total;
    float penumbraSum = total.penumbra;
    float penumbraCount = total.penumbra > 0.0 ? 1.0 : 0.0;
    for (uint i = 1u; i < draws; ++i) {
        // A fresh pair per draw: lightXi may be the primary vertex's stratified
        // choice, and reusing it would trace the same cone direction N times.
        float2 xi = float2(ptRandom01(rng), ptRandom01(rng));
        DirectLight next = ptDirectLightDraw(hit, shading, view, xi, rng, totalWeight, cache);
        total.diffuse += next.diffuse;
        total.specular += next.specular;
        penumbraSum += next.penumbra;
        penumbraCount += next.penumbra > 0.0 ? 1.0 : 0.0;
    }
    total.diffuse /= float(draws);
    total.specular /= float(draws);
    total.penumbra = penumbraCount > 0.0 ? penumbraSum / penumbraCount : 0.0;
    return total;
}
