#version 450
layout(location = 0) in vec3 in_attr;
layout(location = 0) out vec4 out_colour;
layout(set = 0, binding = 0) uniform usampler2D indices;  // R8_UINT texels, top-down, stride W
layout(set = 0, binding = 1) uniform sampler2D palette;   // 256x1 RGBA8; alpha 0 = colour key 0xF81F
layout(push_constant) uniform Push {
    vec2 view;
    ivec2 tex;
    int kind;
    float shift;  // half a guest pixel, in render-target pixels
    float alpha;  // output alpha, independent of the palette colour key
    layout(offset = 28) int projected;
    layout(offset = 32) ivec2 center;
} push;
void main() {
    int w = push.tex.x, h = push.tex.y;
    // The guest takes attributes at the integer pixel position, Vulkan at the pixel centre: step back half a
    // guest pixel along x (y is already aligned by the vertex stage) so texel choice follows the guest.
    vec3 attr = in_attr - push.shift * dFdx(in_attr);
    ivec2 c;
    if (push.kind == 4) {
        // Guest mode 4 (62804–62851) integer-divides at 16-pixel span boundaries, with
        // a clamped Q28 depth shifted right by 6, then steps fixed-point UV by the
        // endpoint delta >> 4. Its final >> 22 floors UV (no half-texel offset).
        // Here UV is divided in floating point at each fragment instead: neither
        // the guest's endpoint quantization nor its linear steps between divisions match exactly.
        ivec2 t = ivec2(floor(attr.xy / max(attr.z, 64.0 / 268435456.0)));
        int stride = w <= 256 ? w : 64;
        int a = (t.x & (w - 1)) + (t.y & (h - 1)) * stride;
        c = ivec2(a % w, a / w);
    } else {
        bool affine = push.kind == 1;
        vec2 uv = affine ? attr.xy : attr.xy / attr.z;
        ivec2 t = ivec2(floor(uv + 0.5));  // nearest texel in normal modes
        if (affine) {
            int a = (t.x + t.y * w) & (w * h - 1);  // one mask after the linear sum; U carries into V
            c = ivec2(a % w, a / w);
        } else {
            c = ivec2(t.x & (w - 1), t.y & (h - 1));
        }
    }
    vec4 colour = texelFetch(palette, ivec2(int(texelFetch(indices, c, 0).r), 0), 0);
    if (colour.a == 0.0) discard;  // keyed texels write neither colour nor depth
    out_colour = vec4(colour.rgb, push.alpha);
}
