.pragma library
.import FerrousUi 1.0 as FerrousUi

// The visualization's modes, by index, for its mode switches: the plot's
// plate and the fullscreen viewer's title strip. The spectrogram's glyph is
// the app icon's glide and harmonic, which says frequency over time
// (Breeze's bar chart said statistics); the waveform's is Breeze's.
// `assetsUrl` is the app's assets folder, as the caller resolves it.
function modes(assetsUrl) {
    return [
        { name: FerrousUi.I18n.i18nc("@option:radio visualization mode", "Spectrogram"),
          iconName: "", iconSource: assetsUrl + "spectrogram-symbolic.svg" },
        { name: FerrousUi.I18n.i18nc("@option:radio visualization mode", "Waveform"),
          iconName: "waveform", iconSource: "" }
    ]
}
