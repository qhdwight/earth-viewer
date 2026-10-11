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

// Laser beams: a cyan core in a faint cyan glow, dimmer than the coastlines
// so the dense mesh around stations does not drown them.
const vec3 kLinkColor = vec3(0.3, 0.85, 1.0);
const float kLinkIntensity = 0.6;
const vec3 kLinkGlowColor = vec3(0.01, 0.1, 0.16);
const float kLinkGlowPixels = 4.0; // UI pixels; matches kLineGlowPixels in main.cc
// Points on the surface sit exactly at t = 1 below; the margin keeps a
// station's end of the link from flickering in and out.
const float kOcclusionMargin = 0.001;

void main() {
    // Round caps and a one-pixel anti-aliased edge, as in earth.frag.
    // Derivatives come before any non-uniform discard.
    const float beyond = max(max(ribbon.x, ribbon.y), 0.0);
    const float distance = length(vec2(beyond, ribbon.z));
    const float aa = max(0.5 * fwidth(distance), 0.0001);
    const float alpha = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, distance);
    const float glowPixels = max(distance - 1.0, 0.0) * 0.5 / aa;
    const float glowFade = max(1.0 - glowPixels / (kLinkGlowPixels * pc.line.w), 0.0);
    const float glow = glowFade * glowFade;

    // Hidden where the line of sight from the camera meets the globe first:
    // solve |o + t d| = 1 for t in (0, 1).
    const vec3 o = pc.cameraPos.xyz;
    const vec3 d = worldPos - o;
    const float a = dot(d, d);
    const float b = dot(o, d);
    const float discriminant = b * b - a * (dot(o, o) - 1.0);
    const float t = (-b - sqrt(max(discriminant, 0.0))) / a;
    const bool occluded = discriminant > 0.0 && t > 0.0 && t < 1.0 - kOcclusionMargin;
    if (alpha + glow <= 0.0 || occluded) {
        discard;
    }
    // Premultiplied and blended source-over, unlike the MAX-blended scene, so
    // links cover the border lines they cross. Full coverage is opaque. The
    // glow carries no alpha, which makes it add light instead of covering.
    outColor = vec4(kLinkColor * kLinkIntensity * alpha + kLinkGlowColor * glow, alpha);
}
