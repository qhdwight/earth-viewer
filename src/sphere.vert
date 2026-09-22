#version 460

// One triangle covering the whole screen; the fragment shader ray-casts the
// unit sphere so its silhouette is exact at any zoom. The ray direction is
// affine in clip space, so interpolating it across the triangle is exact.
layout(location = 0) out vec3 rayDir;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
} pc;

const int kVertexCount = 3;
const vec2 kCorners[kVertexCount] = vec2[kVertexCount](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

void main() {
    const vec2 ndc = kCorners[gl_VertexIndex];
    const vec4 nearPoint = inverse(pc.viewProj) * vec4(ndc, 0.0, 1.0);
    rayDir = nearPoint.xyz / nearPoint.w - pc.cameraPos.xyz;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
