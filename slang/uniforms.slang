// Uniform block layouts, mirroring the structs in
// include/reone/graphics/uniforms.h.
//
// This file exists so that Slang's reflection can compute the std140 offsets and
// a generator can assert them against the C++ structs, replacing a contract that
// was previously maintained by hand and checked by nothing. See
// doc/vulkan-rt-backend.md section 4.2.
//
// Struct and member names deliberately match the C++ ones rather than the
// u-prefixed GLSL ones, so the generated assertions need no name mapping. The
// Vulkan shaders written in phase 4 will use these blocks directly.
//
// Regenerate the assertions with:
//     cmake --build build --target uniformlayout

static const int kMaxLights = 32;
static const int kNumShadowLightSpace = 6;
static const int kMaxBones = 24;
static const int kMaxDanglyVertices = 768;
static const int kMaxParticles = 64;
static const int kMaxGrassClusters = 256;
static const int kMaxTextChars = 128;
static const int kMaxWalkmeshMaterials = 32;
static const int kNumSSAOSamples = 64;

struct GlobalUniformsLight {
    float4 position; // W = 0 if light is directional
    float4 color;
    float multiplier;
    float radius;
    int ambientOnly;
    int dynamicType;
};

struct GlobalUniforms {
    float4x4 projection;
    float4x4 projectionInv;
    float4x4 view;
    float4x4 viewInv;
    float4 cameraPosition;
    float4 worldAmbientColor;
    GlobalUniformsLight lights[kMaxLights];
    float4 shadowLightPosition; // W = 0 if light is directional
    float4 shadowCascadeFarPlanes;
    float4x4 shadowLightSpace[kNumShadowLightSpace];
    // Unjittered, so that motion vectors never encode the jitter offset.
    float4x4 viewProjection;
    float4x4 prevViewProjection;
    float4 fogColor;
    float4 jitter; // XY = this frame's NDC jitter, ZW = previous frame's
    float clipNear;
    float clipFar;
    int numLights;
    float shadowStrength;
    float shadowRadius;
    float fogNear;
    float fogFar;
};

struct LocalUniforms {
    float4x4 model;
    float4x4 modelInv;
    float4x4 prevModel;
    // glm::mat3x4 - three columns of four floats. Declared as an array so that
    // the layout is unambiguous under Slang's row-major convention.
    float4 uv[3];
    float4 color;
    float4 ambientColor;
    float4 diffuseColor;
    float4 selfIllumColor;
    int featureMask;
    int bumpMapFrame;
    float bumpMapScale;
    float waterAlpha;
    float billboardSize;
};

struct BoneUniforms {
    float4x4 bones[kMaxBones];
    float4x4 prevBones[kMaxBones];
};

struct DanglyUniforms {
    float4 positions[kMaxDanglyVertices];
};

struct ParticleUniformsParticle {
    float4 positionFrame;
    float4 right;
    float4 up;
    float4 color;
    float2 size;
};

struct ParticleUniforms {
    int2 gridSize;
    ParticleUniformsParticle particles[kMaxParticles];
};

struct GrassUniformsCluster {
    float4 positionVariant; // fourth component is a variant (0-3)
    float2 lightmapUV;
};

struct GrassUniforms {
    float2 quadSize;
    float radius;
    GrassUniformsCluster clusters[kMaxGrassClusters];
};

struct TextUniformsCharacter {
    float4 posScale;
    float4 uv;
};

struct TextUniforms {
    TextUniformsCharacter chars[kMaxTextChars];
};

struct WalkmeshUniforms {
    float4 materials[kMaxWalkmeshMaterials];
};

struct ScreenEffectUniforms {
    float4x4 projection;
    float4x4 projectionInv;
    float4x4 screenProjection;
    float4 ssaoSamples[kNumSSAOSamples];
    float2 screenResolution;
    float2 screenResolutionRcp;
    float2 blurDirection;
    float clipNear;
    float clipFar;
    float ssaoSampleRadius;
    float ssaoBias;
    float ssrBias;
    float ssrPixelStride;
    float ssrMaxSteps;
    float sharpenAmount;
};

ConstantBuffer<GlobalUniforms> globalUniforms;
ConstantBuffer<LocalUniforms> localUniforms;
ConstantBuffer<BoneUniforms> boneUniforms;
ConstantBuffer<DanglyUniforms> danglyUniforms;
ConstantBuffer<ParticleUniforms> particleUniforms;
ConstantBuffer<GrassUniforms> grassUniforms;
ConstantBuffer<TextUniforms> textUniforms;
ConstantBuffer<WalkmeshUniforms> walkmeshUniforms;
ConstantBuffer<ScreenEffectUniforms> screenEffectUniforms;

RWStructuredBuffer<float> sink;

// Reflection only reports parameters an entry point actually uses, so this
// touches every block. It is never executed.
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
