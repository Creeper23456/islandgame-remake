#version 450
// Guest pixels in fallback mode; otherwise raw camera x, y and the guest's inverse scale.
layout(location = 0) in ivec3 in_pos;
layout(location = 1) in vec3 in_attr; // perspective: (u*z, v*z, z); affine: (u, v, z)
layout(location = 0) out vec3 out_attr;
layout(push_constant) uniform Push {
    vec2 view;    // guest scene size in pixels (the 3D target extent at scale 1)
    ivec2 tex;    // texture width, height
    int kind;     // guest raster mode
    float shift;  // half a guest pixel in render-target pixels (fragment stage)
    float alpha;  // output alpha (fragment stage)
    layout(offset = 28) int projected; // nonzero: project raw camera x, y
    layout(offset = 32) ivec2 center;  // guest projection center in pixels
} push;
void main() {
    ivec2 xy = in_pos.xy;
    if (push.projected != 0) {
        // Wrap the multiply before interpreting it as signed for the guest's ASR.
        ivec2 delta = ivec2(uvec2(xy) * uint(in_pos.z)) >> 16;
        xy = ivec2(uvec2(push.center) + uvec2(delta));
    }
    // The guest evaluates a scanline's edges at the integer row y, not the row centre, so shift y by half
    // a pixel: sampling pixel centres then reproduces the same span ends.
    vec2 p = (vec2(xy) + vec2(0.0, 0.5)) / push.view * 2.0 - 1.0;
    const float q28 = 1.0 / 268435456.0;
    float z = in_attr.z;
    if (push.kind == 4 || push.kind == 5) {
        float bias = push.kind == 4 ? 150000.0 : 100000.0;
        z = max(z, 64.0 * q28) + bias * q28;
    }
    gl_Position = vec4(p, z * 0.5, 1.0);
    out_attr = in_attr;
}
