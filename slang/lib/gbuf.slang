module gbuf;

// Geometry feature bits packed into the alpha of the lightmap target.
// Ported from glsl/i_gbuf.glsl.

static const int kGeometryFeatureEnvMap = 1 << 0;
static const int kGeometryFeatureShadows = 1 << 1;
static const int kGeometryFeatureFog = 1 << 2;
static const int kGeometryFeatureLightmap = 1 << 3;
static const int kGeometryFeatureStatic = 1 << 4;

public float packGeometryFeatures(bool envmap, bool lightmap, bool staticObject,
                                  bool shadows, bool fog) {
    int mask = 0;
    mask |= int(envmap) * kGeometryFeatureEnvMap;
    mask |= int(shadows) * kGeometryFeatureShadows;
    mask |= int(fog) * kGeometryFeatureFog;
    mask |= int(lightmap) * kGeometryFeatureLightmap;
    mask |= int(staticObject) * kGeometryFeatureStatic;
    return mask / 255.0;
}

public void unpackGeometryFeatures(float packed, out bool envmap,
                                   out bool lightmap, out bool staticObject,
                                   out bool shadows, out bool fog) {
    int mask = int(round(packed * 255.0));
    envmap = (mask & kGeometryFeatureEnvMap) != 0;
    lightmap = (mask & kGeometryFeatureLightmap) != 0;
    staticObject = (mask & kGeometryFeatureStatic) != 0;
    shadows = (mask & kGeometryFeatureShadows) != 0;
    fog = (mask & kGeometryFeatureFog) != 0;
}
