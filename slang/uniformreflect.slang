// Reflection-only entry point for the C++ uniform ABI guard. It touches every
// block because reflection reports only parameters an entry point references.
// Do not add it to a render pipeline or shader pack.

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
        aabbUniforms.corners[0].x +
        screenEffectUniforms.projection[0][0];
}
