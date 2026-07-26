module geometry;

import uniforms;

/**
 * Object-space vertex transforms.
 *
 * These exist as functions rather than branches inside one entry point because
 * each one becomes a separate pipeline, and because everything here has to be
 * resolved into real geometry before a ray tracer can see it (section 5.1). A
 * compute pass will call the same functions to write deformed vertices into a
 * buffer, so they take their inputs explicitly rather than reading varyings.
 */

public struct VertexSkin {
    public float4 indices;
    public float4 weights;
};

/** Rigid geometry - the object-space position passes through unchanged. */
public float4 transformStatic(float3 position) {
    return float4(position, 1.0);
}

public float4 skinPosition(float4 position, VertexSkin skin, bool previousFrame) {
    int i1 = max(0, int(skin.indices[0]));
    int i2 = max(0, int(skin.indices[1]));
    int i3 = max(0, int(skin.indices[2]));
    int i4 = max(0, int(skin.indices[3]));

    if (previousFrame) {
        return mul(boneUniforms.prevBones[i1], position) * skin.weights[0] +
               mul(boneUniforms.prevBones[i2], position) * skin.weights[1] +
               mul(boneUniforms.prevBones[i3], position) * skin.weights[2] +
               mul(boneUniforms.prevBones[i4], position) * skin.weights[3];
    }
    return mul(boneUniforms.bones[i1], position) * skin.weights[0] +
           mul(boneUniforms.bones[i2], position) * skin.weights[1] +
           mul(boneUniforms.bones[i3], position) * skin.weights[2] +
           mul(boneUniforms.bones[i4], position) * skin.weights[3];
}

/** Dangly vertices are simulated on the CPU and uploaded per frame. */
public float4 danglyPosition(uint vertexId) {
    return danglyUniforms.positions[vertexId];
}

/**
 * Lightsaber blades are widened in the vertex stage from the vertex index. The
 * blade has 88 vertices per side, four per segment, 22 segments.
 */
public float4 saberPosition(float3 position, uint vertexId) {
    float signum = 2.0 * (vertexId / 88) - 1.0;
    float hdist = ((vertexId % 88) / 4) / 21.0;
    float vdist = (vertexId % 4) / 3.0;
    return float4(position + 0.5 * signum * hdist * vdist * localUniforms.saberDisplacement.xyz, 1.0);
}

/**
 * Transforms a direction by the inverse-transpose of the model matrix. The
 * original spells this transpose(mat3(uModelInv)) * v; mul(v, M) is the
 * row-vector product, which is the same thing without building the transpose.
 */
public float3 transformNormal(float3 n) {
    return mul(n, (float3x3)localUniforms.modelInv);
}

/** Tangent frame, carried as three vectors so no matrix convention is involved. */
public struct TangentFrame {
    public float3 tangent;
    public float3 bitangent;
    public float3 normal;
};

public float3 tangentToWorld(TangentFrame frame, float3 n) {
    return frame.tangent * n.x + frame.bitangent * n.y + frame.normal * n.z;
}
