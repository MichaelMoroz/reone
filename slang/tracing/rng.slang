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

/**
 * Sample the GGX distribution of VISIBLE normals (Heitz, JCGT 2018), given the
 * view direction. Takes perceptual roughness; alpha is squared internally, as
 * in the raster resolve.
 *
 * Not the plain NDF, which this replaces, and the difference is not a
 * refinement. Pairing an NDF sample with a weight of F is only correct for a
 * visible-normal pdf; with the NDF's own pdf the weight owes a further
 * G * VdotH / (NdotV * NdotH), which is about one head-on and diverges at
 * grazing incidence - so a glossy floor seen at a shallow angle scattered
 * occasional enormous samples, which is what the speckle was. Sampling the
 * visible normals instead makes the weight F * G2/G1, bounded by F, and it is
 * the lower-variance sampler besides: it never proposes a microfacet the
 * viewer cannot see.
 */
public float3 ptGgxVisibleHalfVector(float3 normal, float3 view, float roughness, float2 xi) {
    float a = roughness * roughness;
    // Into the tangent frame the sampler is defined in, then into the
    // hemisphere configuration where the ellipsoid is a sphere.
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(reference, normal));
    float3 bitangent = cross(normal, tangent);
    float3 viewLocal = float3(dot(view, tangent), dot(view, bitangent), dot(view, normal));
    float3 viewHemi = normalize(float3(a * viewLocal.x, a * viewLocal.y, max(0.0, viewLocal.z)));
    // An orthonormal basis around it, degenerate-safe at normal incidence.
    float lensq = viewHemi.x * viewHemi.x + viewHemi.y * viewHemi.y;
    float3 t1 = lensq > 0.0 ? float3(-viewHemi.y, viewHemi.x, 0.0) * rsqrt(lensq)
                            : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(viewHemi, t1);
    // A disk sample, squashed to the projected area the viewer actually sees.
    float r = sqrt(xi.x);
    float phi = kPtTwoPi * xi.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + viewHemi.z);
    p2 = lerp(sqrt(max(0.0, 1.0 - p1 * p1)), p2, s);
    float3 halfHemi = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * viewHemi;
    float3 halfLocal = normalize(float3(a * halfHemi.x, a * halfHemi.y, max(0.0, halfHemi.z)));
    return tangent * halfLocal.x + bitangent * halfLocal.y + normal * halfLocal.z;
}

// Uniform direction inside a cone: light sources are never points, so every
// shadow ray samples the source's angular extent and shadows get penumbra.
public float3 ptConeSample(float3 direction, float angularRadius, float2 xi) {
    float cosTheta = lerp(1.0, cos(angularRadius), xi.x);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = kPtTwoPi * xi.y;
    return ptTangentToWorld(direction, float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}
