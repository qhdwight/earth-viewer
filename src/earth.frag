#version 460

layout(location = 0) in vec3 worldPos;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec4 cameraPos;
} pc;

void main() {
    // The globe is opaque: a point on the unit sphere is visible only when
    // its normal (which is the point itself) faces the camera.
    if (dot(worldPos, pc.cameraPos.xyz - worldPos) < 0.0) {
        discard;
    }
    outColor = vec4(1.0);
}
