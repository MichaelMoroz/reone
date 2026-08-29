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
module gbuf;

/**
 * View-space position of a G-buffer texel. Device depth stays in [0,1]; NDC y
 * is negated because the geometry pass rasterises through a negative-height
 * viewport. Every consumer of the G-buffer places a pixel through this one
 * function, so the modes cannot disagree about where a pixel is.
 */
public float3 gbufViewPosition(float2 uv, float depth, float4x4 projectionInv) {
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}

// Geometry feature bits packed into the alpha of the lightmap target.
// Ported from glsl/i_gbuf.glsl.

static const int kGeometryFeatureEnvMap = 1 << 0;
static const int kGeometryFeatureShadows = 1 << 1;
static const int kGeometryFeatureFog = 1 << 2;
static const int kGeometryFeatureLightmap = 1 << 3;
static const int kGeometryFeatureStatic = 1 << 4;
static const int kGeometryFeatureThin = 1 << 5;

public float packGeometryFeatures(bool envmap, bool lightmap, bool staticObject,
                                  bool shadows, bool fog, bool thin) {
    int mask = 0;
    mask |= int(envmap) * kGeometryFeatureEnvMap;
    mask |= int(shadows) * kGeometryFeatureShadows;
    mask |= int(fog) * kGeometryFeatureFog;
    mask |= int(lightmap) * kGeometryFeatureLightmap;
    mask |= int(staticObject) * kGeometryFeatureStatic;
    mask |= int(thin) * kGeometryFeatureThin;
    return mask / 255.0;
}

public void unpackGeometryFeatures(float packed, out bool envmap,
                                   out bool lightmap, out bool staticObject,
                                   out bool shadows, out bool fog, out bool thin) {
    int mask = int(round(packed * 255.0));
    envmap = (mask & kGeometryFeatureEnvMap) != 0;
    lightmap = (mask & kGeometryFeatureLightmap) != 0;
    staticObject = (mask & kGeometryFeatureStatic) != 0;
    shadows = (mask & kGeometryFeatureShadows) != 0;
    fog = (mask & kGeometryFeatureFog) != 0;
    thin = (mask & kGeometryFeatureThin) != 0;
}

