// Canonical merged-scene rasterization. The index buffer supplies the original
// vertex id, while all attributes and material state come from set 2.

import uniforms;
import lib.gbuf;
import lib.math;
import lib.megadraw_geometry;
import lib.motion;
import lib.scene_material;

static const uint kMegaFeatureLightmap = 1u << 0;
static const uint kMegaFeatureEnvMap = 1u << 1;
static const uint kMegaFeatureNormalMap = 1u << 2;
static const uint kMegaFeatureBumpMap = 1u << 3;
static const uint kMegaFeatureShadows = 1u << 7;
static const uint kMegaFeatureFog = 1u << 9;
static const uint kMegaFeatureWater = 1u << 8;
static const uint kMegaFeatureStatic = 1u << 14;
static const uint kMegaFeatureBlendedCoverage = 1u << 25;
static const uint kMegaSurfaceUnlitTransparent = 1;
static const float kMegaAlphaTestEpsilon = 0.01;

struct Varyings {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float3 tangent : TEXCOORD1;
    float3 bitangent : TEXCOORD2;
    float3 tanSpaceNormal : TEXCOORD3;
    float2 uv1 : TEXCOORD4;
    float2 uv2 : TEXCOORD5;
    float4 color : TEXCOORD6;
    float4 curClipPos : TEXCOORD7;
    float4 prevClipPos : TEXCOORD8;
};

[shader("vertex")]
Varyings megadrawVertex(uint vertexId : SV_VertexID) {
    MergedVertex vertex = mergedVertices[vertexId];
    Varyings output;
    output.curClipPos = mul(globalUniforms.viewProjection, float4(vertex.position, 1.0));
    output.prevClipPos = mul(globalUniforms.prevViewProjection,
                             float4(vertex.prevPosition, 1.0));
    // Rasterise on the jittered grid. globalUniforms.projection carries the
    // sub-pixel offset and the deferred resolves invert it to reconstruct
    // position from depth, so depth must be written through the same matrix -
    // writing it unjittered and inverting it jittered wobbles every
    // reconstructed world position by the Halton offset, which the shadow
    // lookup turns into a per-frame binary flip along every shadow edge.
    // curClipPos/prevClipPos stay unjittered, per uniforms.slang, so motion
    // vectors never encode the offset.
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, float4(vertex.position, 1.0)));
    output.normal = vertex.normal;
    output.tangent = vertex.tangent;
    output.bitangent = vertex.bitangent;
    output.tanSpaceNormal = vertex.tanSpaceNormal;
    output.uv1 = vertex.uv1;
    output.uv2 = vertex.uv2;
    output.color = vertex.color;
    return output;
}

struct GBufferOutput {
    float4 diffuseColor : SV_Target0;
    float4 eyeNormal : SV_Target1;
    float4 lightmapColor : SV_Target2;
    float4 selfIllumColor : SV_Target3;
    float2 motion : SV_Target4;
    uint materialId : SV_Target5;
};

float3 safeNormal(float3 value, float3 fallback) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-10 ? value * rsqrt(lengthSquared) : fallback;
}

[shader("fragment")]
GBufferOutput megadrawFragment(Varyings input, uint primitiveId : SV_PrimitiveID,
                               bool isFrontFace : SV_IsFrontFace) {
    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];

    float2 uv = transformedUV(material, input.uv1);
    if (!commitsTracedCoverage(material, uv, input.color.a)) discard;

    float4 diffuse = float4(1.0);
    if (material.mainTex != kNoTexture) {
        diffuse = bindlessTextures[NonUniformResourceIndex(material.mainTex)].Sample(uv);
    }
    diffuse *= input.color;
    if ((material.featureMask & kMegaFeatureWater) != 0) {
        diffuse *= material.waterAlpha;
    }

    bool envmapped = (material.featureMask & kMegaFeatureEnvMap) != 0;

    float3 normal = safeNormal(input.normal, float3(0.0, 0.0, 1.0));
    float3 tangent = safeNormal(input.tangent, float3(1.0, 0.0, 0.0));
    float3 bitangent = safeNormal(input.bitangent, float3(0.0, 1.0, 0.0));
    float3 frameNormal = safeNormal(input.tanSpaceNormal, normal);
    if ((material.featureMask & kMegaFeatureNormalMap) != 0 &&
        material.normalMap != kNoTexture) {
        float3 n = bindlessTextures[NonUniformResourceIndex(material.normalMap)].Sample(uv).rgb * 2.0 - 1.0;
        normal = safeNormal(tangent * n.x + bitangent * n.y + frameNormal * n.z, normal);
    } else if ((material.featureMask & kMegaFeatureBumpMap) != 0 &&
               material.bumpMapArray != kNoTexture) {
        float layer = float(material.bumpMapFrame);
        float height = bindlessTextureArrays[NonUniformResourceIndex(material.bumpMapArray)].Sample(
            float3(uv, layer)).r;
        float dX = material.bumpMapScale * ddx(height);
        float dY = material.bumpMapScale * ddy(height);
        float3 n = safeNormal(float3(-dX, -dY, 1.0), float3(0.0, 0.0, 1.0));
        normal = safeNormal(tangent * n.x + bitangent * n.y + frameNormal * n.z, normal);
    }
    // The traced G-buffer face-forwards its material normal against the
    // committed geometric side. With culling disabled, raster must do the same
    // for back-facing fragments or the two encodings differ by a sign.
    if (!isFrontFace) normal = -normal;

    bool hasLightmap = (material.featureMask & kMegaFeatureLightmap) != 0 &&
                       material.lightmap != kNoTexture;
    float3 lightmap = float3(1.0);
    if (hasLightmap) {
        lightmap = bindlessTextures[NonUniformResourceIndex(material.lightmap)].Sample(input.uv2).rgb;
    }
    float features = packGeometryFeatures(
        envmapped,
        hasLightmap,
        (material.featureMask & kMegaFeatureStatic) != 0,
        (material.featureMask & kMegaFeatureShadows) != 0,
        (material.featureMask & kMegaFeatureFog) != 0);
    float3 eyeNormal = 0.5 * mul(normal, (float3x3)globalUniforms.viewInv) + 0.5;

    GBufferOutput output;
    output.diffuseColor = diffuse;
    output.eyeNormal = float4(eyeNormal, 0.0);
    output.lightmapColor = float4(lightmap, features);
    output.selfIllumColor = float4(material.selfIllumColor.rgb,
                                   material.envMapDerivedLayer / 255.0);
    output.motion = computeMotion(input.curClipPos, input.prevClipPos);
    output.materialId = materialIndex;
    return output;
}

// G8's forward blended pass, over the same merged geometry and the same
// non-opaque range the G-buffer already draws for cutout coverage.
//
// The two transparent kinds are one draw, not two. Output is premultiplied and
// the blend state is fixed at ONE, ONE_MINUS_SRC_ALPHA, so an additive surface
// is simply alpha zero and a lit-blended one carries alpha equal to coverage.
// The material decides which by the alpha it writes, so there is no second
// pipeline and no branch in the frame graph.
//
// Order is submission order. kvp-main's replay of the retail draw stream
// reorders only true-opaque depth-writing batches and never touches blended
// ones, so a distance sort would be a deviation before it was an improvement.
[shader("fragment")]
float4 megadrawBlendedFragment(Varyings input, uint primitiveId : SV_PrimitiveID) {
    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];

    // Cutouts share this range but are opaque and already in the G-buffer.
    if ((material.featureMask & kMegaFeaturePunchThrough) != 0) discard;

    float2 uv = transformedUV(material, input.uv1);
    float4 texel = float4(1.0);
    if (material.mainTex != kNoTexture) {
        texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)].Sample(uv);
    }

    // A premultiplied texture carries no usable alpha channel; luma recovers
    // the coverage, exactly as particles.slang does.
    float coverage = (material.featureMask & kMegaFeaturePremulAlpha) != 0
                         ? rgbToLuma(texel.rgb)
                         : texel.a;
    float3 color = texel.rgb * material.diffuseColor.rgb * input.color.rgb;
    color += material.selfIllumColor.rgb;
    float alpha = coverage * input.color.a * material.diffuseColor.a;
    if ((material.featureMask & kMegaFeatureWater) != 0) {
        alpha *= material.waterAlpha;
    }
    if (alpha <= 0.0) discard;

    // Blended surfaces occlude in proportion to coverage; additive ones add
    // and never occlude, which premultiplied output expresses as alpha zero.
    bool blended = (material.featureMask & kMegaFeatureBlendedCoverage) != 0;
    return blended ? float4(color * alpha, alpha) : float4(color * alpha, 0.0);
}
