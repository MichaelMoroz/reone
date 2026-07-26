module surface;

import uniforms;

/**
 * A surface's shading parameters at a point, independent of how the point was
 * reached. The raster resolve reaches it through a G-buffer, a path tracer will
 * reach it through a hit record; neither the producer nor the BSDF needs to know
 * which.
 *
 * Odyssey assets do not carry these directly - they carry diffuse, lightmap,
 * envmap and bump slots plus ambient/diffuse/self-illum colours - so something
 * has to map between the two. That mapping wants to move into the material
 * buffer of section 5.6, computed once per material rather than per fragment.
 * Keeping it behind this struct means that move changes where SurfaceParams
 * comes from and nothing that consumes it.
 */
public struct SurfaceParams {
    public float3 albedo;
    public float alpha;
    public float3 normal;    // world space
    public float3 emissive;
    public float roughness;
    public float metallic;
    /** Baked irradiance, or 1 where a surface carries no lightmap. */
    public float3 lightmap;
    public bool hasLightmap;
};

public SurfaceParams defaultSurface() {
    SurfaceParams s;
    s.albedo = float3(1.0);
    s.alpha = 1.0;
    s.normal = float3(0.0, 0.0, 1.0);
    s.emissive = float3(0.0);
    // Odyssey materials carry no roughness or metallic channel. These are the
    // stand-ins the PBR pipeline already assumes; deriving something better from
    // the asset is a separate question - see section 5.4.
    s.roughness = 1.0;
    s.metallic = 0.0;
    s.lightmap = float3(1.0);
    s.hasLightmap = false;
    return s;
}
