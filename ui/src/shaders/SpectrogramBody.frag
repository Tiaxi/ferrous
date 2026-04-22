#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D sourceTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
};

void main() {
    float value = texture(sourceTex, vTexCoord).r;
    float premultiplied = value * qt_Opacity;
    fragColor = vec4(premultiplied, premultiplied, premultiplied, qt_Opacity);
}
