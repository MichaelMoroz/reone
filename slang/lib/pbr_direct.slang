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
module pbr_direct;

import uniforms;
import lib.direct_light;
import lib.math;
import lib.shadow;

/** Everything downstream needs from one PBR direct-light evaluation. */
public struct PbrDirectLight {
    public bool direct;
    public float3 direction;
    /** Linear source radiance after intensity and this mode's visibility. */
    public float3 radiance;
    /** Demodulated Lambertian diffuse, including 1/pi and thin transmission. */
    public float3 diffuse;
    /** Cosine against the unflipped normal, for the specular lobe. */
    public float specularNdotL;
    /** The legacy ambient-only fill, before material ambient is applied. */
    public float3 ambientRadiance;
};


/**
 * One implementation of the PBR resolve's per-light direct contribution.
 *
 * Visibility is deliberately evaluated here rather than by either consumer.
 * Otherwise the debug channel can drift from the resolve while still calling
 * the same radiance helper, which is the failure this boundary exists to stop.
 */
public PbrDirectLight pbrDirectLight(GlobalUniformsLight light,
                                     float3 viewPos, float3 worldPos,
                                     float3 normal, bool thinSurface,
                                     bool shadowsEnabled, float thinTransmission,
                                     float emitterRadiusRatio,
                                     float directIntensity, float sunIntensity) {
    PbrDirectLight result;
    result.direct = false;
    result.direction = float3(0.0, 0.0, 1.0);
    result.radiance = float3(0.0);
    result.diffuse = float3(0.0);
    result.specularNdotL = 0.0;
    result.ambientRadiance = float3(0.0);

    // What the light IS comes from the shared module, so this renderer and
    // the tracer cannot disagree on it; only the visibility below is PBR's.
    DirectLightSample sample = sampleDirectLight(light, worldPos, emitterRadiusRatio,
                                                 directIntensity, sunIntensity);
    result.direction = sample.direction;
    // Non-directional only: the tracer deliberately promotes a DIRECTIONAL
    // ambient-only record to a sampled sun (lighting.slang keeps only the
    // local ones out of its table), so routing it to the ambient path here
    // made the same light a fill in one renderer and a sun in the other.
    if (!sample.directional && light.ambientOnly != 0) {
        result.ambientRadiance = directLightColor(light) *
                                 sphereLightAttenuation(light.radius, sample.distance,
                                                        emitterRadiusRatio);
        return result;
    }
    // A directional past the caster budget is applied UNSHADOWED, not
    // deleted. Deleting it changed what light exists, and the two renderers
    // may differ only in how occlusion is answered - the tracer samples every
    // light and shadow-rays it; a map-less directional here is the same light
    // with the weakest possible occlusion answer. The cost is real (an
    // unmapped sun reaches through walls) and is the caster budget's problem,
    // not admission's.
    result.direct = true;

    float visibility = 1.0;
    if (shadowsEnabled && light.shadowSlot >= 0) {
        // The map returns a LINEAR occlusion fraction - the mean of comparison
        // taps - exactly as the tracer's shadow ray returns a linear
        // transmittance. It was raised to 2.2 here as if it were an encoded
        // colour, which darkened every penumbra (0.5 became 0.22) with no
        // counterpart in the reference; the unoccluded parity proof holds only
        // because this branch was not running.
        visibility = getShadowFrom(light.shadowSlot, viewPos, worldPos,
                                   normal, thinSurface);
    }
    result.radiance = sample.radiance * visibility;
    result.diffuse = directLightDiffuse(result.radiance, normal, result.direction,
                                        thinSurface, thinTransmission);
    result.specularNdotL = max(0.0, dot(normal, result.direction));
    return result;
}
