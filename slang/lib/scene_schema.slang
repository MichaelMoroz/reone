// Canonical GPU-scene storage schema.  These declarations deliberately have
// no bindings: each pass owns its descriptor set, while every pass shares the
// byte layout.  C++ mirrors live in graphics/gpuscene.h and are checked from
// Slang reflection when the Vulkan renderer starts.
module scene_schema;

public struct InstanceMaterial {
    public float4 selfIllumColor;
    public float4 diffuseColor;
    public float4 uv0;
    public float4 uv1;
    public float4 uv2;
    public uint mainTex;
    public uint normalMap;
    public uint lightmap;
    public uint bumpMapArray;
    public uint featureMask;
    public int bumpMapFrame;
    public float bumpMapScale;
    public uint envMap;
    public float4 curatedAlbedoMul;
    public float4 curatedRoughA;
    public float4 curatedRoughB;
    public float4 curatedMetalA;
    public float4 curatedMetalB;
    public float4 curatedEmission;
    public uint surfaceType;
    public float roughnessScale;
    public uint envMapCube;
    public float waterAlpha;
    public float4 overrideColor;
    public float4 overrideParams;
    public float4 ambientColor;
    public int envMapDerivedLayer;
};

public struct Matrix3x4 {
    public float4 row0;
    public float4 row1;
    public float4 row2;
};

public struct MergedVertex {
    public float3 position;
    public float3 normal;
    public float2 uv1;
    public float2 uv2;
    public float3 tangent;
    public float3 bitangent;
    public float3 tanSpaceNormal;
    public float3 prevPosition;
    public float2 pad;
    public float4 color;
};

public struct SceneObject {
    public float4x4 transform;
    public float4x4 prevTransform;
    public float4x4 transformInv;
    public uint srcVertexOffset;
    public uint srcIndexOffset;
    public uint srcVertexStride;
    public int offPosition;
    public int offNormals;
    public int offUV1;
    public int offUV2;
    public int offTanSpace;
    public int offBoneIndices;
    public int offBoneWeights;
    public uint vertexCount;
    public uint triangleCount;
    public uint dstVertexBase;
    public uint dstTriangleBase;
    public uint geometryIndex;
    public uint boneBase;
    public uint boneCount;
    public uint materialIndex;
    public uint danglyBase;
    public uint danglyCount;
    public float4 saberDisplacement;
};

public struct ProceduralQuad {
    public float4 positionVariant;
    public float4 right;
    public float4 up;
    public float4 uvOffsetScale;
    public float2 lightmapUV;
    public float2 pad;
    public float4 color;
};

public struct GrassFace {
    public float4 vertex0Uv0x;
    public float4 vertex1Uv0y;
    public float4 vertex2Uv1x;
    public float4 uv1yUv2QuadSize;
    public float4 probabilities;
    public float4 boundsMin;
    public float4 boundsMax;
    public uint4 faceBudgetMaterialVariants;
};

public struct GrassRange {
    public uint faceIndex;
    public uint clusterOffset;
    public uint clusterCount;
    public uint pad;
};
