.pragma library

// One wheel notch, in the units of a wheel event's angleDelta.
const angleUnitsPerNotch = 120

// How far a wheel event scrolls vertically, in notches, which are
// fractional for touchpads and high-resolution wheels. Positive is away
// from the user. Events with no vertical delta, such as horizontal
// scrolling, give 0. Pixel-only events count pixelsPerNotch per notch.
function verticalNotches(wheel, pixelsPerNotch) {
    if (!wheel) {
        return 0
    }
    const angleY = wheel.angleDelta ? (wheel.angleDelta.y || 0) : 0
    if (angleY !== 0) {
        return angleY / angleUnitsPerNotch
    }
    const pixelY = wheel.pixelDelta ? (wheel.pixelDelta.y || 0) : 0
    if (pixelY !== 0) {
        return pixelY / Math.max(1, pixelsPerNotch || angleUnitsPerNotch)
    }
    return 0
}

// How far a wheel event scrolls vertically, in pixels. Positive is away
// from the user. Touchpads report the pixels to scroll; a wheel notch
// scrolls notchPixels.
function verticalScrollPixels(wheel, notchPixels) {
    if (!wheel) {
        return 0
    }
    const pixelY = wheel.pixelDelta ? (wheel.pixelDelta.y || 0) : 0
    if (pixelY !== 0) {
        return pixelY
    }
    const angleY = wheel.angleDelta ? (wheel.angleDelta.y || 0) : 0
    return angleY / angleUnitsPerNotch * notchPixels
}
