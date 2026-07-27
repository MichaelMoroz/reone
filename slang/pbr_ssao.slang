import uniforms;

[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(6, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(12, 1)]] Sampler2D sNoise;

static const float kNoiseScale = 1.0 / 4.0;
static const float kMaxDistance = 500.0;

struct Varyings { float4 position : SV_Position; noperspective float2 uv : TEXCOORD0; };

[shader("vertex")]
Varyings ssaoVertex(uint vertexId: SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

// The G-buffer's viewport is y-flipped and depth is already 0..1.
float3 reconstructViewPos(float2 uv) {
    float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}

[shader("fragment")]
float4 ssaoFragment(Varyings input) : SV_Target {
    float2 uv = input.uv;
    float3 pos = reconstructViewPos(uv);
    float3 normal = normalize(2.0 * sGBufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 random = float3(sNoise.SampleLevel(uv * screenEffectUniforms.screenResolution * kNoiseScale, 0.0).rg, 0.0);
    float3 tangent = normalize(random - normal * dot(random, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    float occlusion = 0.0;
    for (int i = 0; i < kNumSSAOSamples; ++i) {
        // GLSL mat3 takes these basis vectors as columns. Slang lowers this
        // constructor as the same SPIR-V columns, so multiply the sample from
        // the left to obtain tangent*x + bitangent*y + normal*z.
        float3 samplePos = pos + screenEffectUniforms.ssaoSampleRadius * mul(screenEffectUniforms.ssaoSamples[i].xyz, tbn);
        float4 sampleClip = mul(globalUniforms.projection, float4(samplePos, 1.0));
        float2 sampleUV = sampleClip.xy / sampleClip.w;
        sampleUV = float2(sampleUV.x * 0.5 + 0.5, 0.5 - sampleUV.y * 0.5);
        float sceneZ = reconstructViewPos(sampleUV).z;
        float rangeCheck = smoothstep(0.0, 1.0, screenEffectUniforms.ssaoSampleRadius / abs(pos.z - sceneZ));
        occlusion += (sceneZ >= samplePos.z + screenEffectUniforms.ssaoBias ? 1.0 : 0.0) * rangeCheck;
    }
    occlusion = 1.0 - (occlusion / float(kNumSSAOSamples)) * (1.0 - smoothstep(0.0, kMaxDistance, abs(pos.z)));
    return float4(occlusion.xxx, 1.0);
}
