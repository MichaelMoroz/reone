module scene_material;

// Shader mirror of GpuSceneMaterial. Keep field order and storage-buffer
// stride synchronized with include/reone/graphics/gpuscene.h.
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
