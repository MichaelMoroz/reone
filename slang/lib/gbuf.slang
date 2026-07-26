module gbuf;

// Geometry feature bits packed into the alpha of the lightmap target.
// Ported from glsl/i_gbuf.glsl.

static const int kGeometryFeatureEnvMap = 1 << 0;
static const int kGeometryFeatureShadows = 1 << 1;
static const int kGeometryFeatureFog = 1 << 2;

public float packGeometryFeatures(bool envmap, bool shadows, bool fog) {
    int mask = 0;
    mask |= int(envmap) * kGeometryFeatureEnvMap;
    mask |= int(shadows) * kGeometryFeatureShadows;
    mask |= int(fog) * kGeometryFeatureFog;
    return mask / 255.0;
}
