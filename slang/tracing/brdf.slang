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

// The path tracer's display transform: colour maths taking its dials as
// arguments rather than reading push constants.

import lib.math;

// Scene-referred in, display-referred out. The calibration rule survives the
// change of curve: direct light alone should land around 0.3-0.5 of the final
// value in lit areas, leaving headroom that bounce light fills.
/**
 * The Gran Turismo tone curve: a toe, a linear midsection and a shoulder that
 * approaches the peak asymptotically, so highlights compress instead of
 * clipping and midtones keep the slope the artist authored. Replaces the ACES
 * fit that stood here, which crushed the shadows and shifted hue on the way.
 */
float GTTonemap(float x)
{
    float P = 1.0f;
    float a = 1.0f;
    float m = 0.22f;
    float l = 0.4f;
    float c = 1.33f;
    float b = 0.0f;
    float l0 = (P - m) * l / a;
    float Lx = m + a * (x - m);
    float Tx = m * pow(x / m, c) + b;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = a * P / (P - S1);
    float Sx = P - (P - S1) * exp(-C2 * (x - S0) / P);
    float w0 = 1.0f - smoothstep(0, m, x);
    float w2 = step(m + l0, x);
    float w1 = 1.0f - w0 - w2;
    return Tx * w0 + Lx * w1 + Sx * w2;
}

float3 GTTonemap(float3 c)
{
    // Another option is to tonemap max from rgb components and scale others accordingly
    return float3(GTTonemap(c.r), GTTonemap(c.g), GTTonemap(c.b));
}

/** tonemap: 0 pass-through, 1 the Gran Turismo curve; the Tonemap dial. */
public float3 ptDisplayTransform(float3 linearColor, float exposure, uint tonemap) {
    float3 mapped = linearColor * exposure;
    if (tonemap == 1) mapped = GTTonemap(mapped);
    return linearToGamma(mapped);
}

