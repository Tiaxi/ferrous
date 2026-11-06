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

// Whether `popup` still takes its keys: shown and not closing. A dialog
// closes from the start of its exit transition.
function popupTakesKeys(popup) {
    return !!popup && popup.visible && !(popup.exit && popup.exit.running)
}
