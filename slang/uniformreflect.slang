// Entry point that exists only so that slangc emits reflection data for every
// uniform block. Reflection reports the parameters an entry point actually
// references, so this touches all of them. It is never executed, and is not
// packed into the shader pack.
//
// See src/apps/uniformgen and doc/vulkan-rt-backend.md section 4.2.

import uniforms;

RWStructuredBuffer<float> sink;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    sink[0] =
        globalUniforms.projection[0][0] +
        localUniforms.model[0][0] +
        boneUniforms.bones[0][0][0] +
        danglyUniforms.positions[0].x +
        particleUniforms.particles[0].size.x +
        grassUniforms.clusters[0].lightmapUV.x +
        textUniforms.chars[0].uv.x +
        walkmeshUniforms.materials[0].x +
        screenEffectUniforms.projection[0][0];
}
