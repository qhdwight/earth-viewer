#version 460
#extension GL_EXT_buffer_reference : require

struct TextVertex {
    vec2 position;
    vec2 uv;
    vec4 color;
};
layout(buffer_reference, std430) readonly buffer TextVertices {
    TextVertex vertices[];
};
layout(push_constant) uniform TextPush {
    vec2 screenScale;
    TextVertices vertexBuffer;
} pc;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;

void main() {
    const TextVertex vertex = pc.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = vec4(vertex.position * pc.screenScale - 1.0, 0.0, 1.0);
    uv = vertex.uv;
    color = vertex.color;
}
