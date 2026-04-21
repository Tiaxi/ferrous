#version 440

layout(location = 0) in vec2 fragmentUv;
layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D sourceTexture;

void main() {
    float intensity = texture(sourceTexture, fragmentUv).r;
    outColor = vec4(intensity, intensity, intensity, 1.0);
}
