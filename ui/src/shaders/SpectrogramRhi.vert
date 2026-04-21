#version 440

layout(location = 0) in vec2 vertexPosition;
layout(location = 1) in vec2 vertexUv;

layout(location = 0) out vec2 fragmentUv;

layout(std140, binding = 0) uniform SpectrogramUniforms {
    mat4 clipMatrix;
} uniforms;

void main() {
    fragmentUv = vertexUv;
    gl_Position = uniforms.clipMatrix * vec4(vertexPosition, 0.0, 1.0);
}
