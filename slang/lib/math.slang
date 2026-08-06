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
module math;

public static const float kPi = 3.14159265358979323846;

/**
 * Perceived brightness, ported from glsl/i_luma.glsl.
 *
 * Used to recover coverage from a premultiplied-alpha texture, which carries
 * colour already multiplied by its own alpha and no usable alpha channel.
 */
public float rgbToLuma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

/**
 * Screen-space motion vectors. Ported from glsl/i_motion.glsl.
 *
 * Clip positions must come from the unjittered view-projection matrices so that
 * the sub-pixel jitter does not leak into the result. The vector points from
 * where the surface was to where it is now, as a UV-space delta.
 */
public float2 computeMotion(float4 curClipPos, float4 prevClipPos) {
    if (curClipPos.w <= 0.0 || prevClipPos.w <= 0.0) {
        return float2(0.0);
    }
    float2 cur = curClipPos.xy / curClipPos.w;
    float2 prev = prevClipPos.xy / prevClipPos.w;
    return 0.5 * (cur - prev);
}
