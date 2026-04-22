#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D sourceTex;

void main() {
    float value = texture(sourceTex, vTexCoord).x;
    fragColor = vec4(value, value, value, 1.0);
}
