#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D sourceTex;
layout(binding = 2) uniform sampler2D dbRemapTex;
layout(binding = 3) uniform sampler2D paletteTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
};

void main() {
    float value = texture(sourceTex, vTexCoord).r;
    float mapped = texture(dbRemapTex, vec2(value, 0.5)).r;
    vec4 paletteColor = texture(paletteTex, vec2(mapped, 0.5));
    fragColor = vec4(paletteColor.rgb * qt_Opacity, paletteColor.a * qt_Opacity);
}
