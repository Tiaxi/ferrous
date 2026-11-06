.pragma library

function clamp01(value) {
    return Math.max(0, Math.min(1, value))
}

function mixColor(colorA, colorB, amount) {
    const t = clamp01(amount)
    return Qt.rgba(
        (colorA.r * (1 - t)) + (colorB.r * t),
        (colorA.g * (1 - t)) + (colorB.g * t),
        (colorA.b * (1 - t)) + (colorB.b * t),
        (colorA.a * (1 - t)) + (colorB.a * t))
}

// `colorValue` with its alpha replaced by `alpha`.
function withAlpha(colorValue, alpha) {
    return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, clamp01(alpha))
}

function colorLuma(colorValue) {
    return (0.2126 * colorValue.r) + (0.7152 * colorValue.g) + (0.0722 * colorValue.b)
}

// WCAG's relative luminance of an opaque colour.
function relativeLuminance(colorValue) {
    const linear = function(channel) {
        return channel <= 0.04045 ? channel / 12.92 : Math.pow((channel + 0.055) / 1.055, 2.4)
    }
    return 0.2126 * linear(colorValue.r) + 0.7152 * linear(colorValue.g) + 0.0722 * linear(colorValue.b)
}

// WCAG's contrast ratio of two opaque colours.
function contrastRatio(colorA, colorB) {
    const a = relativeLuminance(colorA)
    const b = relativeLuminance(colorB)
    return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05)
}

// The opacity of `foreground` over the opaque `background`, at least
// `opacity`, that reaches `ratio` against the background; 1 when none does.
function opacityForContrast(foreground, background, opacity, ratio) {
    for (let alpha = clamp01(opacity); alpha < 1; alpha += 0.01) {
        if (contrastRatio(mixColor(background, foreground, alpha), background) >= ratio) {
            return alpha
        }
    }
    return 1
}
