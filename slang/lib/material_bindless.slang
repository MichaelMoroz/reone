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

// The material records and bindless texture tables of the raster passes (set
// 2), and the one authored-mirror read every raster consumer shares. The
// tracer binds the same tables in its own set and has its own reader.

module material_bindless;

import lib.scene_schema;
import lib.envmap;

[[vk::binding(2, 2)]] public StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] public Sampler2D bindlessTextures[];
[[vk::binding(5, 2)]] public SamplerCube bindlessTextureCubes[];

bool hasMirrorCube(InstanceMaterial material) {
    return (material.featureMask & kMaterialEnvMapCube) != 0 && material.envMapCube != kNoTexture;
}

/**
 * The authored mirror: the original's sharp world-space reflection, a cube
 * where one was authored, the equirectangular map otherwise. Fragment stages:
 * the 2D map takes the hardware's mip, the cube its base level - a reflection
 * vector's derivatives, off a quantized or interpolated normal, pick no stable
 * cube LOD.
 */
public float3 sampleAuthoredMirror(InstanceMaterial material, float3 worldReflection) {
    if (hasMirrorCube(material)) {
        return bindlessTextureCubes[NonUniformResourceIndex(material.envMapCube)]
            .SampleLevel(worldReflection, 0.0).rgb;
    }
    if (material.envMap == kNoTexture) return float3(0.0);
    return bindlessTextures[NonUniformResourceIndex(material.envMap)]
        .Sample(authoredMirrorUV(worldReflection)).rgb;
}

/** The same read at the base level, for compute stages and quantized normals. */
public float3 sampleAuthoredMirrorLevel0(InstanceMaterial material, float3 worldReflection) {
    if (hasMirrorCube(material)) {
        return bindlessTextureCubes[NonUniformResourceIndex(material.envMapCube)]
            .SampleLevel(worldReflection, 0.0).rgb;
    }
    if (material.envMap == kNoTexture) return float3(0.0);
    return bindlessTextures[NonUniformResourceIndex(material.envMap)]
        .SampleLevel(authoredMirrorUV(worldReflection), 0.0).rgb;
}
