module math;

public static const float kPi = 3.14159265358979323846;

/**
 * Perceived brightness, ported from glsl/i_luma.glsl.
 *
 * Used to recover coverage from a premultiplied-alpha texture, which carries
 * colour already multiplied by its own alpha and no usable alpha channel.
 */
public float rgbToLuma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}
