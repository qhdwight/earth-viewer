#version 460
#extension GL_EXT_buffer_reference : require

layout(location = 0) out vec3 fragColor;

struct Vertex {
    vec4 position;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout(push_constant) uniform PushConstants {
    VertexBuffer vertexBuffer;
} pc;

void main() {
    Vertex v = pc.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = v.position;
    fragColor = v.color.rgb;
}
