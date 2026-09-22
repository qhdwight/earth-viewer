#version 460

layout(location = 0) in vec3 rayDir;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec4 cameraPos;
} pc;

const vec3 kSphereColor = vec3(0.01); // linear; the swapchain is sRGB

void main() {
    // Nearest intersection of the ray o + t d with the unit sphere:
    // t^2 + 2 t (o.d) + (o.o - 1) = 0.
    const vec3 o = pc.cameraPos.xyz;
    const vec3 d = normalize(rayDir);
    const float b = dot(o, d);
    const float discriminant = b * b - (dot(o, o) - 1.0);
    // Fade the limb over about one pixel instead of cutting it hard.
    const float alpha = smoothstep(0.0, fwidth(discriminant), discriminant);
    if (alpha <= 0.0 || -b - sqrt(max(discriminant, 0.0)) < 0.0) {
        discard;
    }
    outColor = vec4(kSphereColor, alpha);
}
