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

// The G-buffer as the deferred resolves see it: the persistent resolve table's
// units (set 1) and one read that decodes a pixel into the surface every
// consumer starts from. The PBR provider, the retro resolve and the debug view
// all begin here, so they cannot disagree about what a pixel holds.

module gbuf_resolve;

import uniforms;
import lib.gbuf;
import lib.scene_schema;

[[vk::binding(1, 1)]] public Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] public Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] public Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] public Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] public Sampler2D sGBufDepth;
[[vk::binding(21, 1), vk::image_format("r32ui")]] public Sampler2D<uint> sGBufTriangleId;

/** The triangle a pixel came from; kNoTriangle where nothing was drawn. */
public uint gbufTriangleId(int2 pixel) {
    return sGBufTriangleId.Load(int3(pixel, 0)).r;
}

public struct GBufferSurface {
    /** The authored texel with the vertex colour multiplied in; alpha is the roughness stand-in. */
    public float4 diffuse;
    /** Baked light in rgb, packed geometry features in alpha. */
    public float4 lightmap;
    /** Self-illumination in rgb, the environment layer in alpha. */
    public float4 selfIllum;
    public float3 eyeNormal;
    public float3 eyePosition;
    public float3 worldPosition;
    /** World-space shading normal. */
    public float3 normal;
    public bool envmapped;
    public bool hasLightmap;
    public bool staticObject;
    public bool shadowsEnabled;
    public bool fogEnabled;
    public bool thin;
};

/** Everything a drawn pixel holds, decoded once. Base level: the G-buffer is the resolve's own size. */
public GBufferSurface readGBufferSurface(float2 uv) {
    GBufferSurface g;
    g.diffuse = sGBufDiffuse.SampleLevel(uv, 0.0);
    g.lightmap = sGBufLightmap.SampleLevel(uv, 0.0);
    g.selfIllum = sGBufSelfIllum.SampleLevel(uv, 0.0);
    g.eyeNormal = gbufDecodeEyeNormal(sGBufEyeNormal.SampleLevel(uv, 0.0).xyz);
    g.eyePosition = gbufViewPosition(uv, sGBufDepth.SampleLevel(uv, 0.0).r, globalUniforms.projectionInv);
    g.worldPosition = mul(globalUniforms.viewInv, float4(g.eyePosition, 1.0)).xyz;
    g.normal = normalize(mul(globalUniforms.viewInv, float4(g.eyeNormal, 0.0)).xyz);
    unpackGeometryFeatures(g.lightmap.a, g.envmapped, g.hasLightmap, g.staticObject,
                           g.shadowsEnabled, g.fogEnabled, g.thin);
    return g;
}
