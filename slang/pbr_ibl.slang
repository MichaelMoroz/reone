// Precomputation for image-based lighting, ported from glsl/f_pbr_brdf,
// f_pbr_irradiance and f_pbr_prefilter.
//
// These run once per environment map rather than per frame, turning a cube map
// into the two things the resolve actually samples: an irradiance map for
// ambient diffuse, and a roughness-mipped prefiltered map for ambient specular.
// The BRDF integration LUT is generated once for everything.
//
// The OpenGL versions draw a cube six times, once per face, with a projection
// and view matrix per face. Here a fullscreen triangle is drawn once with a
// six-view mask and the direction is derived from the view index and the screen
// position, which needs neither a mesh nor those matrices.

import uniforms;
import lib.math;

// Sample counts, gathered here because they are the expensive part and are
// worth being able to turn down in one place. These match the OpenGL shaders
// they were ported from, which is what keeps the two backends comparable.
//
// The cost is large but bounded: one environment map is convolved per frame, so
// this is a spike on the frames where a new one comes into view rather than a
// standing per-frame cost.
static const uint kBrdfSamples = 1024u;
static const uint kPrefilterSamples = 1024u;
/** Radians between irradiance samples; the cost is quadratic in 1/this. */
static const float kIrradianceDelta = 0.025;

[[vk::binding(18, 1)]] SamplerCube sEnvMapCube;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation int face : TEXCOORD1;
};

/**
 * Fullscreen triangle, one per cube face.
 *
 * With no view mask this still works and renders face 0, which is what the
 * BRDF pass wants.
 */
[shader("vertex")]
Varyings iblVertex(uint vertexId: SV_VertexID, uint viewId: SV_ViewID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = uv;
    output.face = int(viewId);
    return output;
}

/**
 * The world direction a cube face texel looks along.
 *
 * Face order is Vulkan's and OpenGL's shared one, +X -X +Y -Y +Z -Z. The signs
 * follow the cube map convention, which is left-handed and flips v - getting
 * one of them wrong mirrors a face rather than failing.
 */
float3 directionFor(int face, float2 uv) {
    float2 st = 2.0 * uv - 1.0;
    st.y = -st.y;
    switch (face) {
    case 0:
        return normalize(float3(1.0, st.y, -st.x));
    case 1:
        return normalize(float3(-1.0, st.y, st.x));
    case 2:
        return normalize(float3(st.x, 1.0, -st.y));
    case 3:
        return normalize(float3(st.x, -1.0, st.y));
    case 4:
        return normalize(float3(st.x, st.y, 1.0));
    default:
        return normalize(float3(-st.x, st.y, -1.0));
    }
}

/** Van der Corput radical inverse, the second dimension of a Hammersley set. */
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 hammersley(uint i, uint n) {
    return float2(float(i) / float(n), radicalInverseVdC(i));
}

/** A halfway vector drawn towards where the GGX lobe actually has weight. */
float3 importanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;

    float phi = 2.0 * kPi * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

/** Note the k here differs from the direct-lighting one; this is the IBL form. */
float geometrySchlickGGX(float NdotV, float roughness) {
    float k = (roughness * roughness) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

/**
 * The split-sum BRDF integration: scale and bias for the Fresnel term, indexed
 * by view angle and roughness. Independent of any environment, so this is
 * generated once and never again.
 */
[shader("fragment")]
float2 brdfFragment(Varyings input) : SV_Target {
    float NdotV = input.uv.x;
    float roughness = input.uv.y;

    float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    float3 N = float3(0.0, 0.0, 1.0);

    const uint kSampleCount = kBrdfSamples;
    float A = 0.0;
    float B = 0.0;
    for (uint i = 0u; i < kSampleCount; ++i) {
        float2 Xi = hammersley(i, kSampleCount);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);
        if (NdotL > 0.0) {
            float G = geometrySmith(N, V, L, roughness);
            float GVis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * GVis;
            B += Fc * GVis;
        }
    }
    return float2(A, B) / float(kSampleCount);
}

/** Cosine-weighted convolution of the environment: ambient diffuse, per direction. */
[shader("fragment")]
float4 irradianceFragment(Varyings input) : SV_Target {
    float3 N = directionFor(input.face, input.uv);

    float3 up = float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    float3 irradiance = float3(0.0);
    float sampleDelta = kIrradianceDelta;
    float samples = 0.0;
    for (float phi = 0.0; phi < 2.0 * kPi; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * kPi; theta += sampleDelta) {
            float3 tangentSample = float3(sin(theta) * cos(phi),
                                          sin(theta) * sin(phi),
                                          cos(theta));
            float3 sampleVec = tangentSample.x * right +
                               tangentSample.y * up +
                               tangentSample.z * N;
            irradiance += sEnvMapCube.Sample(sampleVec).rgb * cos(theta) * sin(theta);
            samples += 1.0;
        }
    }
    return float4(kPi * irradiance / samples, 1.0);
}

float distributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

/**
 * The environment blurred to one roughness, written to one mip.
 *
 * Roughness arrives in the local block rather than as a loose uniform, which is
 * what that block exists for.
 */
[shader("fragment")]
float4 prefilterFragment(Varyings input) : SV_Target {
    float roughness = localUniforms.iblRoughness;
    float3 N = directionFor(input.face, input.uv);
    // The usual simplification: assume the view direction is the normal, which
    // costs the stretched reflections at grazing angles and saves a dimension.
    float3 R = N;
    float3 V = R;

    const uint kSampleCount = kPrefilterSamples;
    float3 prefiltered = float3(0.0);
    float totalWeight = 0.0;

    for (uint i = 0u; i < kSampleCount; ++i) {
        float2 Xi = hammersley(i, kSampleCount);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) {
            continue;
        }
        // Sample from a mip chosen by how much of the environment this sample
        // stands for, which is what keeps a rough surface from sparkling.
        float D = distributionGGX(N, H, roughness);
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);
        float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;

        float resolution = 512.0;
        float saTexel = 4.0 * kPi / (6.0 * resolution * resolution);
        float saSample = 1.0 / (float(kSampleCount) * pdf + 0.0001);
        float mipLevel = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

        prefiltered += sEnvMapCube.SampleLevel(L, mipLevel).rgb * NdotL;
        totalWeight += NdotL;
    }
    return float4(prefiltered / totalWeight, 1.0);
}
