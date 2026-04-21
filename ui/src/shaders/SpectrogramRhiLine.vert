#version 440

layout(location = 0) in vec2 vertexPosition;
layout(location = 1) in vec4 vertexColor;

layout(location = 0) out vec4 fragmentColor;

void main() {
    fragmentColor = vertexColor;
    gl_Position = vec4(vertexPosition, 0.0, 1.0);
}
