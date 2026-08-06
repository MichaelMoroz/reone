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
module skycube;

// The one place any mode reads the baked sky.
//
// The bake stores LINEAR radiance - pow(texel, 2.2) of the authored skybox
// faces - so radiance is the seam between the modes rather than pixels. The
// tracer already treated the cube that way; the raster resolves undo the bake
// to recover the authored texel, which is what keeps their frame the frame the
// game shipped. Both now go through one body, so a correction to the sky
// reaches every mode at once.
//
// The cube arrives as a parameter rather than as a binding here: the tracer
// binds it in its own resource set and the raster resolves bind it in theirs,
// and importing a module must not drag one consumer's descriptor layout - or
// its push constants - into another's.

/**
 * Linear radiance leaving the sky along @p direction.
 *
 * @p intensity scales the source, not the image. The tracer grades the sky
 * against its other light sources with it; the raster resolves pass one,
 * because they composite the cube at texel identity exactly as the sky
 * composite pass they replace did.
 */
public float3 skyRadiance(SamplerCube cube, float3 direction, float intensity) {
    return cube.SampleLevel(normalize(direction), 0.0).rgb * intensity;
}

/**
 * The world-space ray through a pixel, for a resolve that has no ray of its own.
 *
 * Taken at the far plane, so the reconstructed point lies on the ray rather
 * than on a surface and the direction is exact without a separate camera basis.
 * NDC y undoes the geometry pass's negative-height viewport, the same
 * correction both resolves apply when they reconstruct a view position.
 */
public float3 skyDirection(float2 uv, float4x4 projectionInv, float4x4 viewInv,
                           float3 cameraPosition) {
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, 1.0);
    float4 viewPos = mul(projectionInv, float4(ndc, 1.0));
    float3 worldPos = mul(viewInv, float4(viewPos.xyz / viewPos.w, 1.0)).xyz;
    return normalize(worldPos - cameraPosition);
}

// skyToDisplay is gone. It existed because the raster resolves encoded their
// own output, so the one caller that had linear radiance in hand had to push it
// into display space itself. Every mode now writes linear and the shared
// post-process pass performs the single encode, which reproduces the authored
// skybox texel exactly where this function used to - so there is nothing left
// for a second gamma helper to do.
