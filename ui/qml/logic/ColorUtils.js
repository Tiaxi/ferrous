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

function colorLuma(colorValue) {
    return (0.2126 * colorValue.r) + (0.7152 * colorValue.g) + (0.0722 * colorValue.b)
}
