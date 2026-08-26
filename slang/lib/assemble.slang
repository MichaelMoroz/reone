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
module assemble;

/*
 * The assembly contract: how the channels every renderer writes become the
 * picture. One implementation, three callers - the trace kernel's own
 * assembly when no denoiser runs, the composite pass, and the raster
 * provider path - so the rules that turn light into an image cannot drift
 * between modes. Nothing here reads a binding or a push constant.
 *
 *   final = noiseFree + (diffuse + directDiffuse) * diffFactor + specular * specFactor
 *
 * noiseFree carries what bypasses any denoiser: emission at the primary, the
 * sky on a miss, the radiance of a sky-class surface. The three radiance
 * channels are demodulated; the two factors are the surface model
 * (lib/surface_model.slang) and put the material back.
 */

/** Environment / miss distance: beyond any scene, inside NRD's defaults. */
public static const float kAssembleInfDistance = 1.0e6;
/** Denoised channels are undefined beyond the denoiser's range. */
public static const float kAssembleDenoisedRange = 1.0e5;

/**
 * Sky, and any path-terminating surface, carries viewZ at the sentinel and
 * has all of its radiance in noiseFree already; nothing is assembled onto it.
 */
public bool isTerminatingSurface(float viewZ) {
    return abs(viewZ) >= kAssembleDenoisedRange;
}

/** The contract line. Callers clamp their channels first if they must. */
public float3 assembleRadiance(float3 noiseFree, float3 diffuse, float3 directDiffuse,
                               float3 specular, float3 diffFactor, float3 specFactor) {
    return noiseFree + (diffuse + directDiffuse) * diffFactor + specular * specFactor;
}

/**
 * A sky-class surface - the sky shell, or a painted backdrop standing in for
 * a distance never modelled - is radiance, not geometry. Its texture is the
 * light, on its own dial: the sky dial for the shell, the backdrop dial for
 * the painting. Neither is graded by the emissive dial, which is for lamps
 * and screens - objects in the scene that also light it.
 */
public float3 assembleSkyClass(bool skyShell, float3 radianceTexel,
                               float skyIntensity, float backdropIntensity) {
    return radianceTexel * (skyShell ? skyIntensity : backdropIntensity);
}

/**
 * Emission at a surface: the self-illumination mask over the texture read as
 * radiance, graded by the curated record (w > 1.5 replaces, w > 0.5 scales)
 * and the emissive dial. Radiance into noiseFree - not a coefficient on the
 * lighting, and not multiplied by the albedo.
 */
public float3 assembleEmission(float3 selfIllumLinear, float3 radianceTexel,
                               float4 curatedEmission, float emissiveIntensity) {
    float3 emission = selfIllumLinear;
    if (curatedEmission.w > 1.5) {
        emission = curatedEmission.rgb;
    } else if (curatedEmission.w > 0.5) {
        emission *= curatedEmission.rgb;
    }
    return emission * radianceTexel * emissiveIntensity;
}

/** Linear fog fraction over the authored near/far range. */
/**
 * Fog from the depth channel, which is what the resolve has and what every
 * pixel has - including the sky and any path-terminating surface.
 *
 * Computed here rather than per surface at shading time. A per-surface amount
 * is gated by that material's fog feature bit, so whether a pixel fogged
 * depended on what KIND of object it was: the sky never fogged, terminators
 * never fogged, and a fogged wall met an unfogged backdrop at a hard seam.
 * Distance is the only thing fog depends on, and the resolve knows it for
 * everything it draws.
 */
public float fogFromViewDepth(float viewDepth, float fogNear, float fogFar) {
    float f = (fogFar - abs(viewDepth)) / max(1e-4, fogFar - fogNear);
    return 1.0 - saturate(f);
}

public float fogAmount(float3 worldPos, float3 cameraPos, float fogNear, float fogFar) {
    float distance = length(worldPos - cameraPos);
    float f = (fogFar - distance) / (fogFar - fogNear);
    return 1.0 - saturate(f);
}

/** Fog mixes energy: a linear-space lerp toward the linearised swatch. */
public float3 applyFog(float3 color, float3 fogColorLinear, float fog) {
    return lerp(color, fogColorLinear, fog);
}
