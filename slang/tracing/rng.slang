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

// Random number generation and low-level directional sampling for the path
// tracer. Pure functions only: no resources, no push constants.

public static const float kPtPi = 3.14159265359;
public static const float kPtTwoPi = 6.28318530718;

public uint ptHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

public float ptRandom01(inout uint state) {
    state = ptHash(state);
    return float(state & 0x00ffffffu) / 16777216.0;
}

public float3 ptTangentToWorld(float3 normal, float3 local) {
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(reference, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * local.x + bitangent * local.y + normal * local.z;
}

public float3 ptCosineHemisphere(float3 normal, float2 xi) {
    float radius = sqrt(xi.x);
    float phi = kPtTwoPi * xi.y;
    float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(1.0 - xi.x));
    return ptTangentToWorld(normal, local);
}

// Importance sample the GGX normal-distribution function. Takes perceptual
// roughness; alpha is squared internally, as in the raster resolve.
public float3 ptGgxHalfVector(float3 normal, float roughness, float2 xi) {
    float a = roughness * roughness;
    float a2 = a * a;
    float phi = kPtTwoPi * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / max(1e-6, 1.0 + (a2 - 1.0) * xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return ptTangentToWorld(normal, float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}

// Uniform direction inside a cone: light sources are never points, so every
// shadow ray samples the source's angular extent and shadows get penumbra.
public float3 ptConeSample(float3 direction, float angularRadius, float2 xi) {
    float cosTheta = lerp(1.0, cos(angularRadius), xi.x);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = kPtTwoPi * xi.y;
    return ptTangentToWorld(direction, float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}
