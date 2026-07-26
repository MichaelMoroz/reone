// A hash for hashed alpha testing, over integer cell coordinates.
//
// This was fract(1e4 * sin(...) * ...). Algebraically fine, but not
// reproducible between backends: sin() is not bit-exact across the GLSL and
// SPIR-V compilation paths, and multiplying by ten thousand before taking the
// fraction turns a difference of one ulp into an unrelated number. The alpha
// test then kept different fragments under OpenGL and Vulkan, which showed up
// as speckle over every piece of foliage - depth and the sampled alpha agreed,
// only the threshold did not.
//
// Integer arithmetic is exact everywhere, so this gives both backends the same
// pattern. The caller has already floored its input to a cell.

uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash(vec3 p) {
    ivec3 c = ivec3(p);
    uint h = hashUint(uint(c.x) ^ hashUint(uint(c.y) ^ hashUint(uint(c.z))));
    return float(h) * (1.0 / 4294967296.0);
}
