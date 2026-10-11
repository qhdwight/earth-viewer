#version 460

layout(location = 0) in vec3 worldPos;
// x: distance past the segment start, y: distance past the segment end,
// z: distance from the centre line. All in half widths.
layout(location = 1) in vec3 ribbon;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec4 cameraPos;
    vec4 line; // w: UI scale, physical pixels per UI pixel
} pc;

// Neon green coastlines with a soft glow around them. The ribbon is padded
// wide enough for the glow; MAX blending keeps overlapping glows from
// stacking into blobs where many segments meet.
const vec3 kLineColor = vec3(0.35, 1.0, 0.3);
const vec3 kLineGlowColor = vec3(0.02, 0.45, 0.06);
const float kLineGlowPixels = 4.0; // UI pixels; matches kLineGlowPixels in main.cc

void main() {
    // Round caps: inside the segment the distance is just the offset from the
    // centre line; past either end it is the distance to that end point.
    const float beyond = max(max(ribbon.x, ribbon.y), 0.0);
    const float distance = length(vec2(beyond, ribbon.z));
    // Centre the one-pixel fade on the true edge. The padded ribbon includes
    // the outer half of the fade, preserving thin lines at distant zooms.
    // Evaluate derivatives before any non-uniform discard.
    const float aa = max(0.5 * fwidth(distance), 0.0001);
    const float alpha = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, distance);
    // aa is half a pixel in half widths, which turns distances into pixels.
    const float glowPixels = max(distance - 1.0, 0.0) * 0.5 / aa;
    const float glowFade = max(1.0 - glowPixels / (kLineGlowPixels * pc.line.w), 0.0);
    const float glow = glowFade * glowFade;
    // The globe is opaque: only the camera-facing side is visible.
    if (alpha + glow <= 0.0 || dot(worldPos, pc.cameraPos.xyz - worldPos) < 0.0) {
        discard;
    }
    // MAX blending uses RGB directly, so coverage must be premultiplied.
    outColor = vec4(kLineColor * alpha + kLineGlowColor * glow, alpha);
}
