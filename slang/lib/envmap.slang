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

// Where an authored environment map is read from, for every consumer.
//
// One implementation because three passes need it - the retro resolve, the PBR
// resolve and the blended forward draw - and a reflection that disagrees
// between them is a difference nobody would think to look for.

module envmap;

import lib.math;

/**
 * The texel an authored 2D environment map lends to a reflection.
 *
 * EQUIRECTANGULAR, on the negated direction, which is what the original
 * sampled: `0.5 + atan(d.x, d.z) / 2pi` across and `0.5 - asin(d.y) / pi` down,
 * for `d = normalize(-R)`.
 *
 * This tree used the OpenGL fixed-function SPHERE_MAP projection instead -
 * `r.xy / (2 * sqrt(r.x^2 + r.y^2 + (r.z+1)^2)) + 0.5` - which is a different
 * texel for the same reflection everywhere except dead centre, and folds the
 * whole rear hemisphere into a ring at the edge of the image.
 *
 * The polar axis is Y even though the world is Z-up. That is the authored
 * convention, not an oversight to correct: these maps were painted against it,
 * and turning them upright would rotate every reflection in the game.
 */
public float2 authoredMirrorUV(float3 reflection) {
    float3 d = normalize(-reflection);
    return float2(0.5 + atan2(d.x, d.z) / (2.0 * kPi),
                  0.5 - asin(clamp(d.y, -1.0, 1.0)) / kPi);
}

/**
 * The reflection an authored environment map is sampled with.
 *
 * WORLD space. The cube is a static capture of the surround and the 2D map is a
 * painting of it, so both are authored in world orientation; sampling either
 * with an eye-space vector rotates the reflection with the camera, which reads
 * as the surround sliding around a window as you walk past it. All three passes
 * used to do exactly that.
 */
public float3 authoredMirrorReflection(float3 worldPos, float3 cameraPos, float3 worldNormal) {
    return reflect(normalize(worldPos - cameraPos), worldNormal);
}
