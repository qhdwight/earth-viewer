#version 460
#extension GL_GOOGLE_include_directive : require

#include "pga.glsl"

// One triangle covering the whole screen; the fragment shader ray-casts the
// unit sphere so its silhouette is exact at any zoom. The ray direction is
// affine in clip space, so interpolating it across the triangle is exact.
layout(location = 0) out vec3 rayDir;

layout(push_constant) uniform PushConstants {
    Motor view; // world to camera space
    vec4 projection; // Perspective in math.h
} pc;

const int kVertexCount = 3;
const vec2 kCorners[kVertexCount] = vec2[kVertexCount](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

void main() {
    const vec2 ndc = kCorners[gl_VertexIndex];
    // The ray through this corner in camera space, which looks down -Z, turned
    // into world space by the inverse of the view motor. Only the rotation
    // acts on a direction, so no camera position is subtracted.
    const vec3 cameraRay = vec3(ndc.x / pc.projection.x, ndc.y / pc.projection.y, -1.0);
    rayDir = TransformDirection(Antireverse(pc.view), cameraRay);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
