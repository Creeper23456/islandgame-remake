#version 450
layout(location = 0) in vec3 in_attr;
layout(location = 0) out vec4 out_colour;
layout(set = 0, binding = 0) uniform usampler2D material;  // R16G16_UINT: RGB565, weight [0, 256]
layout(set = 1, binding = 0) uniform sampler2D previous;   // independent RGBA8 copy of the target
layout(push_constant) uniform Push {
    vec2 view;
    ivec2 tex;
    int kind;
    float shift;
    float alpha;
    layout(offset = 28) int projected;
    layout(offset = 32) ivec2 center;
} push;
void main() {
    vec3 attr = in_attr - push.shift * dFdx(in_attr);
    // As in mode 4, the guest floors after fixed-point stepping between divided
    // 16-pixel endpoints (63867–63911); here the division is per fragment instead.
    ivec2 t = ivec2(floor(attr.xy / max(attr.z, 64.0 / 268435456.0)));
    int w = push.tex.x, h = push.tex.y;
    int stride = w <= 256 ? w : 64;
    int a = (t.x & (w - 1)) + (t.y & (h - 1)) * stride;
    uvec2 texel = texelFetch(material, ivec2(a % w, a / w), 0).rg;
    uvec3 src = (uvec3(texel.r) >> uvec3(11, 5, 0)) & uvec3(31, 63, 31);
    vec4 dst = texelFetch(previous, ivec2(gl_FragCoord.xy), 0);
    // Recover the RGBA8 bytes, then truncate to their RGB565 high bits.
    uvec3 dst565 = uvec3(dst.rgb * 255.0 + 0.5) >> uvec3(3, 2, 3);
    uint weight = texel.g;
    uvec3 rgb = (weight * src + (256u - weight) * dst565) >> 8u;
    out_colour = vec4(vec3(rgb) / vec3(31, 63, 31), max(dst.a, float(weight) / 256.0));
}
