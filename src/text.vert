#version 460
#extension GL_EXT_buffer_reference : require

// Text vertex shader: a GLSL port of SlugVertexShader.hlsl from the Slug
// reference code, https://github.com/EricLengyel/Slug.
// Slug shader code Copyright 2017 by Eric Lengyel, MIT License; see LICENSE-SLUG.
//
// Changes from the reference: vertices are pulled from a buffer instead of
// vertex attributes, the transform is the fixed pixel-to-clip mapping of the
// overlay, the glyph's band location is a linear buffer index, and solid
// quads for UI panels skip dilation.

// Matches TextVertex in text.h, which mirrors the reference's five attributes.
struct TextVertex {
    vec4 pos;    // xy: position in pixels, zw: dilation normal
    vec2 em;     // em-space sample coordinates (reference tex.xy)
    uvec2 glyph; // x: band location, y: band max x | band max y << 16 | flags (reference tex.zw)
    vec4 jac;    // inverse Jacobian entries (00, 01, 10, 11), em per pixel
    vec4 bnd;    // band scale x, y and band offset x, y
    vec4 col;    // colour, straight alpha
};

layout(buffer_reference, std430) readonly buffer TextVertices {
    TextVertex vertices[];
};

layout(push_constant) uniform TextPush {
    vec2 viewport; // pixels
    TextVertices vertexBuffer;
} pc;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 texcoord;
layout(location = 2) flat out vec4 banding;
layout(location = 3) flat out ivec4 glyph;

// A bit the reference leaves zero, marking quads drawn as solid fill.
const uint kSolidFlag = 0x8000u;

// Moves the vertex outward along its normal so the quad covers every pixel
// whose center lies within half a pixel of the glyph's bounds, and returns
// the matching em-space coordinates. Unchanged from the reference.
vec2 SlugDilate(vec4 pos, vec2 tex, vec4 jac, vec4 m0, vec4 m1, vec4 m3, vec2 dim, out vec2 vpos) {
    vec2 n = normalize(pos.zw);
    float s = dot(m3.xy, pos.xy) + m3.w;
    float t = dot(m3.xy, n);

    float u = (s * dot(m0.xy, n) - t * (dot(m0.xy, pos.xy) + m0.w)) * dim.x;
    float v = (s * dot(m1.xy, n) - t * (dot(m1.xy, pos.xy) + m1.w)) * dim.y;

    float s2 = s * s;
    float st = s * t;
    float uv = u * u + v * v;
    vec2 d = pos.zw * (s2 * (st + sqrt(uv)) / (uv - st * st));

    vpos = pos.xy + d;
    return vec2(tex.x + dot(d, jac.xy), tex.y + dot(d, jac.zw));
}

void main() {
    const TextVertex vertex = pc.vertexBuffer.vertices[gl_VertexIndex];

    // Rows of the matrix taking pixels to clip space; Vulkan's y points down
    // like the pixel coordinates.
    const vec4 m0 = vec4(2.0 / pc.viewport.x, 0.0, 0.0, -1.0);
    const vec4 m1 = vec4(0.0, 2.0 / pc.viewport.y, 0.0, -1.0);
    const vec4 m2 = vec4(0.0, 0.0, 0.0, 0.0);
    const vec4 m3 = vec4(0.0, 0.0, 0.0, 1.0);

    vec2 p = vertex.pos.xy;
    texcoord = vertex.em;
    if ((vertex.glyph.y & kSolidFlag) == 0u) {
        texcoord = SlugDilate(vertex.pos, vertex.em, vertex.jac, m0, m1, m3, pc.viewport, p);
    }

    gl_Position.x = p.x * m0.x + p.y * m0.y + m0.w;
    gl_Position.y = p.x * m1.x + p.y * m1.y + m1.w;
    gl_Position.z = p.x * m2.x + p.y * m2.y + m2.w;
    gl_Position.w = p.x * m3.x + p.y * m3.y + m3.w;

    // SlugUnpack, with the band location already linear.
    banding = vertex.bnd;
    glyph = ivec4(int(vertex.glyph.x), 0, int(vertex.glyph.y & 0xFFFFu), int(vertex.glyph.y >> 16u));
    color = vertex.col;
}
