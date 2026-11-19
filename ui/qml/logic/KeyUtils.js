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

// Whether `event` types a character other than Space, such as a letter
// or a digit, with Shift or without: a list's type-ahead takes it before
// the shortcuts, such as F for View › Follow Playhead. Space stays Play or
// Pause, and keys with Ctrl, Alt or Meta stay the shortcuts'.
function isTypedCharacter(event) {
    const text = event.text || ""
    if (text.length !== 1 || text.trim().length !== 1) {
        return false
    }
    const code = text.charCodeAt(0)
    return code >= 0x20 && code !== 0x7f
        && (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) === 0
}

// Whether `event` is Return or Enter alone, which presses a dialog's
// default button.
function isEnter(event) {
    return (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && isUnmodified(event)
}

// Whether `item` is a text field or area the user types in, such as a spin
// box's: it takes Enter itself, to accept what was typed.
function editsText(item) {
    return !!item && ("cursorPosition" in item) && ("readOnly" in item) && !item.readOnly
}
