#version 440

layout(location = 0) in vec2 fragmentUv;
layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D sourceTexture;
layout(binding = 2) uniform sampler2D dbRemapTexture;
layout(binding = 3) uniform sampler2D paletteTexture;

void main() {
    float intensity = texture(sourceTexture, fragmentUv).r;
    float remapped = texture(dbRemapTexture, vec2(intensity, 0.5)).r;
    vec3 paletteColor = texture(paletteTexture, vec2(remapped, 0.5)).rgb;
    outColor = vec4(paletteColor, 1.0);
}
