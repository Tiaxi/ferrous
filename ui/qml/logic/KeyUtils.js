.pragma library

// Whether `event` has no modifier but the keypad's. A focused control
// takes only such keys from the shortcuts, which keep the keys with
// modifiers, such as Ctrl+Shift+Space for Stop.
function isUnmodified(event) {
    return (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
}

// Whether `event` is Space alone, which presses a focused button instead
// of playing or pausing.
function isPlainSpace(event) {
    return event.key === Qt.Key_Space && isUnmodified(event)
}
