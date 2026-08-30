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
module material_ops;

import lib.scene_schema;
import lib.math;

// What an authored Odyssey texel becomes before any light touches it: the
// decode, the curated per-object operations and the per-category calibration
// override.
//
// One implementation, because the path tracer is the reference PBR is measured
// against and two copies of this drift silently - the difference shows up as
// "the same object is a different brightness in the other mode", which is
// exactly how it was found. The tracer's hit path, the tracer's G-buffer path,
// the raster PBR resolve and the debug views all evaluate this.

/**
 * Curated channel op: mode 0 passes the derived value through, mode 1
 * overrides with a constant, mode 2 evaluates
 * lerp(base, smoothstep(a, b, dot(albedo, weights)), t).
 */
public float curatedChannel(float4 A, float4 B, float3 albedo, float current) {
    if (A.x < 0.5) return current;
    if (A.x < 1.5) return A.y;
    return lerp(A.y, smoothstep(A.z, A.w, dot(albedo, B.yzw)), B.x);
}

/**
 * Authored surface colour to linear.
 *
 * The exponent is a dial rather than 2.2 because Odyssey content was not
 * authored to be decoded at all: xoreos, KotOR.js and kvp all multiply the
 * texel by light as-is, and so did the original. Decoding at 2.2 is correct
 * for sRGB-authored art and squares an albedo the artist picked directly -
 * 0.5 becomes 0.22 - which a path tracer then compounds per bounce. 1.0
 * reproduces the reference engines' convention exactly.
 *
 * Reflectance only. The output encode stays a fixed 2.2 because post-process
 * inverts that exact number, and light colour and emission are not
 * reflectance.
 */
public float3 decodeAlbedo(float3 rgb, float gamma) {
    return pow(max(rgb, 0.0), float3(gamma));
}

/** Coverage of an authored texel: its alpha, or its luma where the colour is premultiplied. */
public float texelCoverage(InstanceMaterial material, float4 texel) {
    return (material.featureMask & kMaterialPremulAlpha) != 0 ? rgbToLuma(texel.rgb) : texel.a;
}

/** The shading parameters an authored texel resolves to. */
public struct MaterialSurface {
    public float3 albedo;
    public float roughness;
    public float metallic;
    public float specular;
    public float f0;
};

/**
 * The authored texel and its material record, resolved to shading parameters.
 *
 * texelAlpha is the diffuse alpha, Odyssey's env-map mask: the category says
 * what roughness, specular weight and f0 it lerps between. albedoScale is
 * applied after the decode and before curation - a premultiplied texel divides
 * its coverage back out there. Curation runs against the albedo it derived,
 * then the category override replaces that albedo.
 */
public MaterialSurface resolveMaterial(InstanceMaterial material, float3 texelRgb,
                                       float texelAlpha, float albedoScale,
                                       float albedoGamma) {
    MaterialSurface s;
    s.albedo = decodeAlbedo(texelRgb, albedoGamma) * albedoScale;
    s.roughness = lerp(material.overrideParams.x, material.overrideParams.y, texelAlpha);
    s.specular = lerp(material.overrideParams.z, material.overrideParams.w, texelAlpha);
    s.f0 = lerp(material.overrideF0.x, material.overrideF0.y, texelAlpha);
    s.metallic = 0.0;

    s.albedo *= material.curatedAlbedoMul.rgb;
    s.roughness = curatedChannel(material.curatedRoughA, material.curatedRoughB,
                                 s.albedo, s.roughness);
    s.metallic = curatedChannel(material.curatedMetalA, material.curatedMetalB,
                                s.albedo, s.metallic);
    if (material.overrideMetallic >= 0.0) s.metallic = material.overrideMetallic;
    s.albedo = lerp(s.albedo, decodeAlbedo(material.overrideColor.rgb, albedoGamma),
                    material.overrideColor.w);
    return s;
}
