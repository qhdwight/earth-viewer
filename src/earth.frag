#version 460

layout(location = 0) in vec3 worldPos;
// x: distance past the segment start, y: distance past the segment end,
// z: distance from the centre line. All in half widths.
layout(location = 1) in vec3 ribbon;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec4 cameraPos;
} pc;

const vec3 kLineColor = vec3(1.0);

void main() {
    // The globe is opaque: a point on the unit sphere is visible only when
    // its normal (which is the point itself) faces the camera.
    if (dot(worldPos, pc.cameraPos.xyz - worldPos) < 0.0) {
        discard;
    }

    // Round caps: inside the segment the distance is just the offset from the
    // centre line; past either end it is the distance to that end point.
    const float beyond = max(max(ribbon.x, ribbon.y), 0.0);
    const float distance = length(vec2(beyond, ribbon.z));
    // Fade the last pixel of the edge for anti-aliasing.
    const float alpha = 1.0 - smoothstep(1.0 - fwidth(distance), 1.0, distance);
    if (alpha <= 0.0) {
        discard;
    }
    outColor = vec4(kLineColor, alpha);
}
