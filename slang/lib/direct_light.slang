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
module direct_light;

import lib.math;

import uniforms;

/*
 * What a light IS, shared by every renderer.
 *
 * The raster modes and the path tracer are meant to differ in how light
 * reaches a surface - shadow maps against shadow rays, SSR against traced
 * specular, lightmaps against traced diffuse - and never in what the light
 * is. This module holds the "what": the source radiance, the falloff, the
 * direction, and the Lambert term. It was proved identical between the two
 * implementations first (unoccluded, light size at minimum, ratio 1.00x,
 * correlation +0.98..+1.00 on seven modules) and then merged, so a future
 * edit cannot make one renderer's sun disagree with the other's again -
 * there is one sun to edit.
 *
 * What deliberately stays with each renderer: the occlusion answer (map or
 * ray), the tracer's selection weight and penumbra, and each side's specular
 * assembly.
 */

static const float kDirectLightPi = 3.14159265;

/** Odyssey's own falloff, radius^2 / (radius + d)^2; retro's fidelity target and what the sphere below is calibrated against. */
public float odysseyLightAttenuation(float radius, float distance) {
    float d = radius + distance;
    return (radius * radius) / (d * d);
}

/**
 * The solid-angle falloff of a spherical emitter, normalised so the emitter
 * ratio dial is brightness-neutral in the far field: far away it decays as
 * budget * radius^2 / d^2, and at the surface it saturates instead of
 * diverging, because a subtended solid angle caps at a hemisphere.
 */
public static const float kDirectLightBudget = 0.1137056389;

public float sphereLightAttenuation(float radius, float distance, float emitterRadiusRatio) {
    float ratio = max(emitterRadiusRatio, 1e-3);
    float sinTheta = saturate(radius * ratio / max(distance, 1e-4));
    float cosTheta = sqrt(max(0.0, 1.0 - sinTheta * sinTheta));
    return kDirectLightBudget * (2.0 * kDirectLightPi) * (1.0 - cosTheta) /
           (kDirectLightPi * ratio * ratio);
}

/** Authored light colour is gamma-encoded; radiance arithmetic is linear. */
public float3 directLightColor(GlobalUniformsLight light) {
    return gammaToLinear(light.color.rgb) * light.multiplier;
}

/** One light's geometry from one receiver, before any visibility answer. */
public struct DirectLightSample {
    public bool directional;
    /** Unit direction from the receiver toward the light. */
    public float3 direction;
    /** Receiver-to-light distance; 1 for a directional, which has none. */
    public float distance;
    /** Source radiance after falloff and the intensity dials. Visibility is
        each renderer's own question and is NOT included. */
    public float3 radiance;
};

/**
 * The shared evaluation. W = 0 in the light's position marks a directional
 * and carries a unit direction TOWARD the light: a light standing in for one
 * infinitely far away has no position to subtract and no distance to fall
 * off over, so its magnitude is the sun dial alone - the calibrated,
 * distance-free intensity - and its direction is the same everywhere.
 */
public DirectLightSample sampleDirectLight(GlobalUniformsLight light, float3 worldPos,
                                           float emitterRadiusRatio, float distanceClamp,
                                           float directIntensity, float sunIntensity) {
    DirectLightSample result;
    result.directional = light.position.w == 0.0;
    float3 toLight = result.directional ? light.position.xyz
                                        : light.position.xyz - worldPos;
    result.distance = result.directional ? 1.0 : length(toLight);
    result.direction = result.directional ? normalize(toLight)
                                          : toLight / max(result.distance, 1e-4);
    // Falloff is evaluated no closer than the clamp's share of the authored
    // radius, so a receiver at the fixture is not driven toward infinity.
    float attenuation = result.directional
                            ? 1.0
                            : sphereLightAttenuation(light.radius,
                                                     max(result.distance, distanceClamp * light.radius),
                                                     emitterRadiusRatio);
    float intensity = result.directional ? sunIntensity : directIntensity;
    result.radiance = directLightColor(light) * attenuation * intensity;
    return result;
}

/**
 * The Lambert diffuse for one light, demodulated (no albedo) and after this
 * renderer's visibility. A thin surface is lit from whichever side the light
 * is on and pays the transmission dial for the far side.
 */
public float3 directLightDiffuse(float3 radiance, float3 normal, float3 direction,
                                 bool thinSurface, float thinTransmission) {
    float cosine = dot(normal, direction);
    float3 shadeNormal = thinSurface && cosine < 0.0 ? -normal : normal;
    float NdotL = max(0.0, dot(shadeNormal, direction));
    float transmission = thinSurface && cosine < 0.0 ? thinTransmission : 1.0;
    return radiance * NdotL * transmission / kDirectLightPi;
}

/**
 * The parity image: what BOTH renderers must produce when everything but
 * unoccluded direct diffuse is stripped away.
 *
 *   albedo * sum over every light of  radiance * NdotL / pi
 *
 * A dumb sum, not a weighted draw: the tracer's NEE selection is an estimator
 * OF this sum, and the point of the parity mode is to compare the sum itself.
 * No visibility, no specular, no bounces, no lightmap, no environment, no
 * emission - each renderer's switch strips those on its own side; what
 * remains is this one function, so the two cannot differ except by their
 * inputs (the shared G-buffer).
 */
public float3 parityDirectColor(float3 albedo, float3 worldPos, float3 normal,
                                bool thinSurface, float thinTransmission,
                                float emitterRadiusRatio, float distanceClamp,
                                float directIntensity, float sunIntensity) {
    float3 total = float3(0.0);
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];
        // Non-directional ambient-only records are fill, not direct - the
        // same rule both shaded paths already share.
        if (light.position.w != 0.0 && light.ambientOnly != 0) {
            continue;
        }
        DirectLightSample sample = sampleDirectLight(light, worldPos, emitterRadiusRatio,
                                                     distanceClamp, directIntensity, sunIntensity);
        total += directLightDiffuse(sample.radiance, normal, sample.direction,
                                    thinSurface, thinTransmission);
    }
    return albedo * total;
}
