#version 460

layout(location = 0) in vec2 local; // position in the sprite, in dot radii
layout(location = 1) flat in vec3 color;
layout(location = 2) flat in float pixelsPerRadius;
layout(location = 0) out vec4 outColor;

const float kHaloRadii = 3.0; // must match satellite.vert
const float kHaloStrength = 0.35;

void main() {
    const float d = length(local);
    // Anti-aliased core: fade across one pixel centred on the dot's edge.
    const float edge = 0.5 / pixelsPerRadius;
    const float core = 1.0 - smoothstep(1.0 - edge, 1.0 + edge, d);
    // Soft halo out to the sprite's edge.
    const float fade = max(1.0 - (d - 1.0) / (kHaloRadii - 1.0), 0.0);
    const float intensity = max(core, kHaloStrength * fade * fade);
    if (intensity <= 0.0) {
        discard;
    }
    outColor = vec4(color * intensity, intensity); // premultiplied, blended with MAX
}
