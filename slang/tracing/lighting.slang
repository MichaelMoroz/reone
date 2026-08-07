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

// The scale that keeps a physical sphere delivering the same total light
// inside a light's bounding radius as Odyssey's falloff did:
//
//   integral of x^2/(1+x)^2 dx over [0,1]  =  3/2 - 2*ln(2)
//
// It is a pure number rather than a graded dial because the whole problem is
// scale-free. Writing x = distance/radius, Odyssey's radius^2/(radius+d)^2
// collapses to 1/(1+x)^2 and the sphere's subtended solid angle collapses to
// a function of x and the size ratio alone, so one constant serves every
// light in the game at every authored radius. It is exact as the emitter
// shrinks to a point and drifts about 1% at the default ratio.
static const float kPtLightBudget = 0.1137056389;

// A point light is a sphere, not a point and not a fixed cone. Its emitter
// radius is a fraction of the authored influence radius - KotOR authored no
// emitter size, and the influence radius is a range, not a fixture size, so
// the fraction is a dial rather than data.
struct SphereLight {
    /** Solid angle the emitter subtends, normalised to a brightness. */
    float attenuation;
    /** Half-angle of the cone the emitter fills, for shadow sampling. */
    float angularRadius;
    /** Distance to the near surface of the emitter, not to its centre. */
    float shadowDistance;
};

// Attenuation alone, kept separate from the rest of SphereLight because the
// selection table evaluates it for every light at every path vertex and has
// no use for the angle or the shadow distance.
float ptSphereAttenuation(GlobalUniformsLight light, float lightDistance) {
    float ratio = max(pushConstants.emitterRadiusRatio, 1e-3);
    float sinTheta = saturate(light.radius * ratio / max(lightDistance, 1e-4));
    float cosTheta = sqrt(max(0.0, 1.0 - sinTheta * sinTheta));
    // Dividing by the emitter's own projected solid angle turns the authored
    // colour into an intensity rather than a radiance, which is what makes
    // the size dial brightness-neutral: it moves the penumbra and the
    // near-field saturation, while the far field stays at budget*radius^2/d^2
    // however large the emitter is. Saturating rather than diverging at the
    // surface is the sphere's doing - the solid angle caps at 2*pi - so there
    // is no 1/d^2 singularity to clamp away.
    return kPtLightBudget * kPtTwoPi * (1.0 - cosTheta) / (kPtPi * ratio * ratio);
}

SphereLight ptSphereLight(GlobalUniformsLight light, float lightDistance) {
    SphereLight result;
    float emitterRadius = light.radius * max(pushConstants.emitterRadiusRatio, 1e-3);
    result.attenuation = ptSphereAttenuation(light, lightDistance);
    result.angularRadius = asin(saturate(emitterRadius / max(lightDistance, 1e-4)));
    result.shadowDistance = max(lightDistance - emitterRadius, 1e-3);
    return result;
}

// The direct-light selection weight: receiver-dependent importance, with
// attenuation pinned to 1 for promoted directionals so distance never erases
// the sun. Ambient-only point lights are inert here - ambient has no place in
// path tracing - while directionals are promoted to sampled suns even when
// authored ambient-only: raster has no direct sun at all, so there is no
// parity target and the Sun dial grades it.
float directLightWeight(GlobalUniformsLight light, float3 position, float3 shadingNormal,
                        bool thin) {
    bool directional = light.position.w == 0.0;
    float3 fragToLight = light.position.xyz - position;
    float lightDistance = length(fragToLight);
    if (!directional && light.ambientOnly != 0) return 0.0;
    // No cutoff. A light does not stop existing at a boundary; inverse-square
    // makes a distant one improbable in this table, which is the same saving
    // without the terminator the hard cutoff drew across large surfaces.
    float attenuation = directional ? 1.0 : ptSphereAttenuation(light, lightDistance);
    float cosine = dot(shadingNormal, fragToLight / max(lightDistance, 1e-4));
    // A thin surface is lit from behind too, so a light on the far side has to
    // be selectable. Clamping the cosine at zero here excluded it from the
    // table outright, and no amount of flipping the normal downstream can
    // recover a light that was never drawn - the back-lit branch simply never
    // ran.
    //
    // Weighted by the transmission it will actually receive, not by the front
    // face's cosine: this table is an importance distribution, and the
    // estimator divides by the weight it picked with, so a weight that
    // overstates the contribution just moves the noise rather than the mean.
    float NdotL = thin && cosine < 0.0 ? -cosine * pushConstants.thinTransmission
                                       : max(0.0, cosine);
    float luma = dot(ptGammaToLinear(light.color.rgb), float3(0.2126, 0.7152, 0.0722));
    return max(0.0, luma * light.multiplier * attenuation * NdotL);
}

public struct DirectLight {
    /** Diffuse irradiance transport before the surface response. */
    public float3 diffuse;
    public float3 specular;
    /**
     * Half-width, in world units, of the penumbra this sample expects, or 0
     * where nothing blocked the light.
     *
     * Geometry, not a guess: a source subtending half-angle t, seen from the
     * shaded point, projects an edge blurred over roughly d*tan(t) at a blocker
     * distance d. It is the quantity a filter downstream needs in order to blur
     * a soft shadow by exactly as much as it is soft and leave a contact edge
     * alone.
     */
    public float penumbra;
};

/**
 * Direct light at a path vertex: weighted single draw over the analytic
 * lights, BRDF-shaded, cone-jittered shadow ray from the terminator-shifted
 * origin. Geometry decides visibility - a light below the actual triangle
 * contributes nothing however curved the smooth normal pretends the surface
 * is - and the smooth normal only shades.
 *
 * The point on the light comes from the caller rather than from rng, because
 * the two call sites want different sequences. The primary vertex feeds a
 * dithered, stratified pair (ptShadowSample) since its result is the shadow the
 * viewer looks straight at and is resolved temporally rather than denoised;
 * bounce vertices pass white noise, which is all their contribution needs
 * before NRD sees it.
 */
public DirectLight ptDirectLight(HitGeometry hit, SurfaceShading shading, float3 view,
                                 float2 lightXi, inout uint rng) {
    DirectLight result;
    result.diffuse = float3(0.0);
    result.specular = float3(0.0);
    result.penumbra = 0.0;
    float totalWeight = 0.0;
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        totalWeight += directLightWeight(globalUniforms.lights[i], hit.position, shading.normal,
                                         shading.thin);
    }
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
    bool directional = light.position.w == 0.0;
    // Directionals are placed as distant points; the position difference is
    // the light direction for both kinds, exactly as the raster path has it.
    float3 toLight = light.position.xyz - hit.position;
    float lightDistance = length(toLight);
    float3 L = toLight / max(lightDistance, 1e-4);
    // Deliberately NOT culled against the geometric normal. That test is
    // binary and the geometric normal is constant across a triangle, so on a
    // smooth-shaded mesh whole facets fail it while their neighbours pass and
    // direct light acquires visible triangle edges. What the surface receives
    // is a shading question - NdotL below answers it continuously - and what
    // the surface occludes is the shadow ray's answer, traced from an origin
    // the terminator shift and the offset already lift clear of the facet.
    //
    // A thin surface has no far side to be in shadow: it is lit from whichever
    // side the light is on, and pays a transmission factor for the privilege.
    bool litFromBehind = dot(L, shading.normal) <= 0.0;
    if (litFromBehind && !shading.thin) return result;
    float3 shadeNormal = shading.thin && litFromBehind ? -shading.normal : shading.normal;
    float transmission = litFromBehind ? pushConstants.thinTransmission : 1.0;
    SphereLight sphere = ptSphereLight(light, lightDistance);
    float3 radiance = ptGammaToLinear(light.color.rgb) * light.multiplier;
    radiance *= directional ? pushConstants.sunIntensity
                            : sphere.attenuation * pushConstants.directIntensity;
    // One angle drives both the falloff and the penumbra, because they are
    // the same quantity: how much of the sky this emitter covers from here.
    // The sun keeps its own dial - it is not placed at a physical distance.
    float angular = directional ? pushConstants.sunAngularRadius : sphere.angularRadius;
    float3 shadowDirection = ptConeSample(L, angular, lightXi);
    // Stop at the emitter's near surface. Running to the centre lets the
    // fixture the lamp is mounted in occlude its own light.
    // Leaving from the side the light is on. The offset exists to lift the ray
    // clear of the surface it started on; pointed the wrong way on a back-lit
    // thin surface it buries the origin inside that surface instead, and every
    // sample comes back occluded by the very triangle being shaded.
    float3 shadowNormal = shading.thin && dot(hit.geometricNormal, L) < 0.0
                              ? -hit.geometricNormal
                              : hit.geometricNormal;
    ShadowRay shadow = ptShadowRay(hit.shadowOrigin, shadowNormal, shadowDirection,
                                   directional ? 10000.0 : sphere.shadowDistance);
    radiance *= shadow.transmittance * (totalWeight / max(selectedWeight, 1e-6));
    // The small-angle form for both kinds. Exact for a directional source; for
    // a local one it reads the angle from the receiver and so understates the
    // penumbra as the blocker approaches the light, which is the regime where
    // the blocker is nearly the light's own fixture and the shadow is already
    // wide enough that a filter is not what decides the picture.
    result.penumbra = shadow.occluderDistance < kPtNoOccluder
                          ? shadow.occluderDistance * tan(angular)
                          : 0.0;
    float NdotL = max(0.0, dot(shadeNormal, L));
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
    // The BSDF is evaluated for a light direction, not sampled, so its NdotL
    // cancels against the cosine factor analytically. Forming it numerically -
    // dividing by 4*NdotL*NdotV and multiplying NdotL back in - is what
    // produced high-intensity speckle with a single bounce and only the sun:
    // where the sun grazes, NdotL*NdotV falls under the 1e-4 floor and the
    // floor stops being a guard and becomes a ten-thousandfold gain on
    // whichever pixels the cone sample happened to unshadow.
    float3 spec = (D * G * F) / max(1e-4, 4.0 * NdotV);
    // The primary diffuse channel has no material response; its one shared
    // NRD factor restores the whole lobe after denoising. Raster's dynamic-
    // light specular is meaningful only on the same env-mapped surfaces the
    // tracer gives a specular lobe.
    result.diffuse = radiance * NdotL * transmission / kPtPi;
    // Specular keeps the true normal and the true side: a leaf transmits light,
    // not a highlight.
    if (shading.envmapped && !litFromBehind) {
        result.specular = spec * radiance;
    }
    return result;
}
