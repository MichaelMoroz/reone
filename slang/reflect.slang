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
// Reflection-only entry points for the two C++ ABI guards: the scene-table
// layout and the uniform blocks. Never added to a render pipeline - they
// exist because reflection reports only what an entry point references, so
// each guard needs a function that touches everything it checks.

import lib.scene_schema;

[[vk::binding(0, 0)]] StructuredBuffer<InstanceMaterial> materials;
[[vk::binding(1, 0)]] StructuredBuffer<Matrix3x4> matrices;
[[vk::binding(2, 0)]] StructuredBuffer<MergedVertex> vertices;
[[vk::binding(3, 0)]] StructuredBuffer<SceneObject> objects;
[[vk::binding(4, 0)]] StructuredBuffer<ProceduralQuad> quads;
[[vk::binding(5, 0)]] StructuredBuffer<GrassFace> faces;
[[vk::binding(6, 0)]] StructuredBuffer<GrassRange> ranges;

[shader("compute")]
[numthreads(1, 1, 1)]
void sceneSchemaGuard() {}

import uniforms;

RWStructuredBuffer<float> sink;

[shader("compute")]
[numthreads(1, 1, 1)]
void uniformGuard() {
    sink[0] =
        globalUniforms.projection[0][0] +
        localUniforms.model[0][0] +
        boneUniforms.bones[0][0][0] +
        danglyUniforms.positions[0].x +
        particleUniforms.particles[0].size.x +
        grassUniforms.clusters[0].lightmapUV.x +
        textUniforms.chars[0].uv.x +
        walkmeshUniforms.materials[0].x +
        aabbUniforms.corners[0].x +
        screenEffectUniforms.projection[0][0];
}
