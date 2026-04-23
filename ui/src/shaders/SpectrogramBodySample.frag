#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D sourceTex;
layout(binding = 1) uniform sampler2D dbRemapTex;
layout(binding = 2) uniform sampler2D paletteTex;
layout(binding = 3) uniform sampler2D rowLutTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 sourceTextureMetrics;
    vec4 sourceRectMetrics;
};

float sampleSourceValue(float sourceColumn, float sourceBin) {
    vec2 sourceSize = max(sourceTextureMetrics.xy, vec2(1.0, 1.0));
    vec2 sampleUv = vec2(
        (clamp(sourceColumn, 0.0, sourceSize.x - 1.0) + 0.5) / sourceSize.x,
        (clamp(sourceBin, 0.0, sourceSize.y - 1.0) + 0.5) / sourceSize.y);
    return texture(sourceTex, sampleUv).x;
}

void main() {
    vec2 sourceSize = max(sourceTextureMetrics.xy, vec2(1.0, 1.0));
    float effectiveZoom = max(sourceTextureMetrics.z, 0.001);
    float sourceColumnOffset = sourceTextureMetrics.w;
    float sourceColumn = vTexCoord.x * sourceSize.x - 0.5;
    float localColumn = sourceColumn - sourceColumnOffset;
    float columnsPerPixel = 1.0 / effectiveZoom;
    float rangeStart = localColumn + 0.5 - 0.5 * columnsPerPixel;
    float interpolationT = clamp(fract(rangeStart), 0.0, 1.0);
    float sourceRectWidth = max(sourceRectMetrics.x, 1.0);

    vec2 rowSample = texture(rowLutTex, vec2(0.5, vTexCoord.y)).xy;
    float sourceBin = rowSample.x * 255.0 + rowSample.y * 65280.0;

    float value = 0.0;
    if (effectiveZoom > 1.001 && interpolationT > 0.001) {
        float leftColumn = sourceColumnOffset + floor(rangeStart);
        float rightColumn = min(leftColumn + 1.0, sourceColumnOffset + sourceRectWidth - 1.0);
        float leftValue = sampleSourceValue(leftColumn, sourceBin);
        float rightValue = sampleSourceValue(rightColumn, sourceBin);
        value = mix(leftValue, rightValue, interpolationT);
    } else {
        float firstColumn = sourceColumnOffset + floor(rangeStart);
        float lastColumn = sourceColumnOffset + ceil(rangeStart + columnsPerPixel - 1.0);
        firstColumn = clamp(firstColumn, sourceColumnOffset, sourceColumnOffset + sourceRectWidth - 1.0);
        lastColumn = clamp(lastColumn, firstColumn, sourceColumnOffset + sourceRectWidth - 1.0);
        for (float column = firstColumn; column <= lastColumn; column += 1.0) {
            value = max(value, sampleSourceValue(column, sourceBin));
        }
    }

    float mapped = texture(dbRemapTex, vec2(value, 0.5)).x;
    vec4 paletteColor = texture(paletteTex, vec2(1.0 - mapped, 0.5));
    fragColor = paletteColor;
}
