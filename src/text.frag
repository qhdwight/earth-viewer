#version 460

layout(set = 0, binding = 0) uniform sampler2D fontAtlas;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 outColor;

void main() {
    const float alpha = color.a * texture(fontAtlas, uv).r;
    outColor = vec4(color.rgb * alpha, alpha);
}
