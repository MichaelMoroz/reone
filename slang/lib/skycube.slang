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

import lib.math;

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

/**
 * The screen displacement of a sky direction, in the G-buffer's convention.
 *
 * The sky is infinitely far, so a camera TRANSLATION moves nothing in it and a
 * camera ROTATION moves all of it. Projecting the direction as a point at
 * infinity - w zero, so the matrices' translation columns drop out - is the
 * whole of that distinction, and it is why this cannot be the surface path with
 * a large depth substituted.
 *
 * Both ends go through the UNJITTERED view-projections, exactly as
 * scene_draw.slang's surface motion does: the resolve that consumes this is
 * handed the jitter separately and would otherwise resolve it twice. @p
 * direction still comes from the jittered projection, because it names the
 * texel the resolve actually sampled - which is the same choice the surface
 * path makes when it reports the point a jittered ray landed on.
 */
public float2 skyMotion(float3 direction, float4x4 viewProjection,
                        float4x4 prevViewProjection) {
    float4 current = mul(viewProjection, float4(direction, 0.0));
    float4 previous = mul(prevViewProjection, float4(direction, 0.0));
    if (previous.w <= 0.0) {
        // This direction was not in front of the previous camera at all, so it
        // has no previous screen position to report. computeMotion answers zero
        // there, and zero asserts that the sky stood still - reinstating the
        // smear on the one band of pixels that is guaranteed to have no
        // history. Measured at 27.9% of the sky, a solid block down the leading
        // edge, on an 86-degree single-frame turn.
        //
        // A full screen away from the side it came from instead, which
        // reprojects off the image so the resolve takes the pixel fresh. The
        // side is sign(previous.xy) whichever sign w has: a direction sweeping
        // past ninety degrees flips the projected position but not the half of
        // the world it swept in from, and it is the latter this is naming.
        float2 cameFrom = sign(previous.xy);
        return all(cameFrom == float2(0.0)) ? float2(1.0, 0.0) : -cameFrom;
    }
    // A full screen of displacement, either way, and no more.
    //
    // The sky covers every direction, so a fast turn brings in pixels whose
    // direction was grazing the previous camera plane - w a hair above zero -
    // and the perspective divide takes those to tens of thousands. Measured at
    // 65504 on a 16-degree single-frame snap, which is not a large motion
    // vector but the half-float ceiling. A surface never does this because a
    // surface at the plane is not in the frustum to begin with.
    //
    // Clamping rather than zeroing keeps the meaning: one screen of motion
    // already reprojects off the history, which is exactly the "there is no
    // previous sample here" the resolve needs. Reporting no motion would
    // instead assert the sky stood still, which is the smear this pass exists
    // to remove.
    return clamp(computeMotion(current, previous), float2(-1.0), float2(1.0));
}

// skyToDisplay is gone. It existed because the raster resolves encoded their
// own output, so the one caller that had linear radiance in hand had to push it
// into display space itself. Every mode now writes linear and the shared
// post-process pass performs the single encode, which reproduces the authored
// skybox texel exactly where this function used to - so there is nothing left
// for a second gamma helper to do.
