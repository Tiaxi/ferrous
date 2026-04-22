#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D sourceTex;
layout(binding = 1) uniform sampler2D dbRemapTex;
layout(binding = 2) uniform sampler2D paletteTex;

void main() {
    float value = texture(sourceTex, vTexCoord).x;
    float mapped = texture(dbRemapTex, vec2(value, 0.5)).x;
    vec4 paletteColor = texture(paletteTex, vec2(1.0 - mapped, 0.5));
    fragColor = paletteColor;
}
