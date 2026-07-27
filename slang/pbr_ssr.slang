import uniforms;

[[vk::binding(0, 1)]] Sampler2D sMainTex;
[[vk::binding(1, 1)]] Sampler2D sLightmap;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(6, 1)]] Sampler2D sGBufEyeNormal;

static const float kMaxDistance = 100.0;
static const float kEdgeFadeStart = 0.8;
struct Varyings { float4 position : SV_Position; noperspective float2 uv : TEXCOORD0; };

[shader("vertex")]
Varyings ssrVertex(uint vertexId: SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float3 reconstructViewPos(float2 uv) {
    float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}
float distanceSquared(float2 a, float2 b) { float2 d = a - b; return dot(d, d); }
void swapIfGreater(inout float a, inout float b) { if (a > b) { float t = a; a = b; b = t; } }

bool traceScreenSpaceRay(float3 rayOrigin, float3 rayDirection, float jitter, out float2 hitUV, out float3 hitPoint, out float numSteps) {
    hitUV = float2(0.0); hitPoint = float3(0.0); numSteps = 0.0;
    float rayLength = rayOrigin.z + rayDirection.z * kMaxDistance > -screenEffectUniforms.clipNear ? (-screenEffectUniforms.clipNear - rayOrigin.z) / rayDirection.z : kMaxDistance;
    float3 rayEnd = rayOrigin + rayDirection * rayLength;
    float4 h0 = mul(globalUniforms.projection, float4(rayOrigin, 1.0));
    float k0 = 1.0 / h0.w; float3 q0 = rayOrigin * k0;
    float2 p0 = float2(h0.x * k0 * 0.5 + 0.5, 0.5 - h0.y * k0 * 0.5) * screenEffectUniforms.screenResolution;
    float4 h1 = mul(globalUniforms.projection, float4(rayEnd, 1.0));
    float k1 = 1.0 / h1.w; float3 q1 = rayEnd * k1;
    float2 p1 = float2(h1.x * k1 * 0.5 + 0.5, 0.5 - h1.y * k1 * 0.5) * screenEffectUniforms.screenResolution;
    p1 += distanceSquared(p0, p1) < 0.0001 ? float2(0.01) : float2(0.0);
    float2 delta = p1 - p0; bool permute = abs(delta.x) < abs(delta.y);
    if (permute) { delta = delta.yx; p0 = p0.yx; p1 = p1.yx; }
    float stepDir = sign(delta.x); float invdx = stepDir / delta.x;
    float3 dQ = (q1 - q0) * invdx; float dk = (k1 - k0) * invdx; float2 dP = float2(stepDir, delta.y * invdx);
    float stride = screenEffectUniforms.ssrPixelStride;
    dP *= stride; dQ *= stride; dk *= stride;
    p0 += (1.0 + jitter) * dP; q0 += (1.0 + jitter) * dQ; k0 += (1.0 + jitter) * dk;
    float2 p = p0; float3 q = q0; float k = k0; float end = p1.x * stepDir; float previousZMax = rayOrigin.z;
    for (float i = 0.0; i < screenEffectUniforms.ssrMaxSteps && p.x * stepDir <= end; i += 1.0) {
        float2 sceneUV = (permute ? p.yx : p) * screenEffectUniforms.screenResolutionRcp;
        float rayZMin = previousZMax; float rayZMax = (dQ.z * 0.5 + q.z) / (dk * 0.5 + k); previousZMax = rayZMax;
        swapIfGreater(rayZMin, rayZMax);
        float sceneZ = reconstructViewPos(sceneUV).z;
        if (rayZMin <= sceneZ && rayZMax >= sceneZ - screenEffectUniforms.ssrBias) { hitUV = sceneUV; hitPoint = q / k; numSteps = i; return true; }
        p += dP; q += dQ; k += dk;
    }
    return false;
}

[shader("fragment")]
float4 ssrFragment(Varyings input) : SV_Target {
    float4 mainTex = sMainTex.SampleLevel(input.uv, 0.0);
    if (mainTex.a == 1.0) return float4(0.0);
    float3 position = reconstructViewPos(input.uv);
    float3 reflection = reflect(normalize(position), normalize(2.0 * sGBufEyeNormal.SampleLevel(input.uv, 0.0).xyz - 1.0));
    uint2 pixel = uint2(input.position.xy); float jitter = float((pixel.x + pixel.y) & 1) * 0.5;
    float2 hitUV; float3 hitPoint; float numSteps; float3 reflectionColor = float3(0.0); float reflectionStrength = 0.0;
    if (traceScreenSpaceRay(position, reflection, jitter, hitUV, hitPoint, numSteps)) {
        float2 hitNDC = hitUV * 2.0 - 1.0; float maxDim = min(1.0, max(abs(hitNDC.x), abs(hitNDC.y)));
        float4 hitMain = sMainTex.SampleLevel(hitUV, 0.0); float4 hitLightmap = sLightmap.SampleLevel(hitUV, 0.0);
        reflectionColor = hitMain.rgb * hitMain.a * hitLightmap.rgb;
        reflectionStrength = 1.0 - clamp(reflection.z, 0.0, 1.0);
        reflectionStrength *= 1.0 - numSteps / screenEffectUniforms.ssrMaxSteps;
        reflectionStrength *= 1.0 - max(0.0, (maxDim - kEdgeFadeStart) / (1.0 - kEdgeFadeStart));
    }
    return float4(reflectionColor, reflectionStrength);
}
