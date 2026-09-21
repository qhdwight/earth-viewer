#version 460
#extension GL_EXT_buffer_reference : require

layout(location = 0) out vec3 worldPos;

// Each vertex packs a longitude (low 16 bits) and a latitude (high 16 bits)
// as two's complement integers in units of 1/180 degree. Matches
// PackBorderPoint in main.cc.
layout(buffer_reference, std430) readonly buffer VertexBuffer {
    uint points[];
};

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
    VertexBuffer vertexBuffer;
} pc;

const float kUnitsPerDegree = 180.0;

void main() {
    const int packed = int(pc.vertexBuffer.points[gl_VertexIndex]);
    const float lon = radians(float((packed << 16) >> 16) / kUnitsPerDegree);
    const float lat = radians(float(packed >> 16) / kUnitsPerDegree);
    // Parametric unit sphere: +Y through the north pole, +Z through the
    // prime meridian, +X through 90 degrees east.
    const vec3 p = vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
    worldPos = p;
    gl_Position = pc.viewProj * vec4(p, 1.0);
}
