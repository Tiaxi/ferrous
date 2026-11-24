#!/usr/bin/env python3
"""The showcase library: the screenshots for the README and AppStream (the `showcase` sessions in states.py). The
fixture library's tones look bare in the spectrogram, so this adds one album of synthesized music with rich
spectral content: chords of band-limited saw waves under a slow filter sweep, a bass line, a plucked arpeggio,
kick, snare and hi-hats made of noise, and a noise riser with a sine sweep before each phrase's end. Artist, album
and titles are invented; nothing is downloaded; the cover is make_cover_art.py's landscape.

`<fixtures>/showcase-library/` holds hard links to the fixture library's artists and the new album, so a session
scans one library root (the tree then has no root rows) and the other sessions' library stays as it is. The
iTunes stand-in covers go to `<fixtures>/showcase-itunes/`. A finished run leaves `showcase-library/.done`.
Usage: make_showcase_fixtures.py <fixtures>"""
import os
import shutil
import subprocess
import sys

import numpy as np

from make_cover_art import landscape

SR = 44100
ARTIST, ALBUM, YEAR = "Aurelia Fold", "Night Signals", "2024"
GENRE = "Electronic"
# (title, seconds, root as MIDI note, tempo)
TRACKS = [("Undertow", 214, 45, 118), ("Glasshouse", 241, 50, 122), ("Northern Wires", 228, 43, 120),
          ("Paper Satellites", 256, 47, 116), ("Slow Comet", 199, 52, 112), ("Low Orbit", 233, 45, 124),
          ("Weatherglass", 247, 48, 118), ("Tidal Lock", 221, 41, 120), ("Long Exposure", 268, 46, 114),
          ("Afterglow Engine", 236, 49, 122)]
ALBUM_DIR = f"{YEAR} - {ALBUM}"
# the iTunes stand-ins (states.m_itunes showcase): one the album's own cover, four others
ITUNES = [ALBUM_DIR, "Night Signals (Deluxe Edition)", "Night Signals – Live", "Night Signals (Remastered)",
          "The Night Signals Sessions"]
# the minor-key progression i, VI, III, VII: chord tones in semitones from the root
CHORDS = [(0, 3, 7), (-4, 0, 3), (3, 7, 10), (-2, 2, 5)]
ARP = [0, 2, 1, 2, 3, 2, 1, 2]  # index into the chord's tones an octave up (3 = the root two octaves up)
TABLE = 4096


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


def saw_table(top_harmonic, odd_only=False, slope=1.6):
    """One cycle of a band-limited wave: harmonics 1..top_harmonic at 1/k^slope (odd ones only: square-like); a
    slope over 1 rolls the top off as instruments do."""
    ph = np.arange(TABLE) / TABLE
    w = np.zeros(TABLE)
    for k in range(1, max(1, int(top_harmonic)) + 1):
        if odd_only and k % 2 == 0:
            continue
        w += np.sin(2 * np.pi * k * ph) / k ** slope
    return w / np.max(np.abs(w))


class Osc:
    """A wavetable oscillator that keeps its phase from one call to the next."""

    def __init__(self):
        self.phase = 0.0

    def run(self, table, freq, n):
        ph = (self.phase + freq * np.arange(n) / SR) % 1.0
        self.phase = (self.phase + freq * n / SR) % 1.0
        idx = ph * TABLE
        i0 = idx.astype(np.int64)
        fr = idx - i0
        return table[i0] * (1 - fr) + table[(i0 + 1) % TABLE] * fr


def brightness_tables(f0):
    """Saw tables for a note: dull to bright (harmonics up to 1.5, 3, 6 and 10 kHz)."""
    return [saw_table(min(60, top / f0)) for top in (1500, 3000, 6000, 10000)]


def bright_mix(tables, osc, freq, n, level):
    """A note through the filter sweep: `level` 0..1 crossfades the tables, dull to bright."""
    pos = level * (len(tables) - 1)
    i = min(int(pos), len(tables) - 2)
    f = pos - i
    phase = osc.phase
    a = osc.run(tables[i], freq, n)
    osc.phase = phase
    b = osc.run(tables[i + 1], freq, n)
    return a * (1 - f) + b * f


def env_ar(n, attack, release):
    e = np.ones(n)
    a, r = int(attack * SR), int(release * SR)
    e[:a] = np.linspace(0, 1, a)
    e[n - r:] = np.linspace(1, 0, r)
    return e


def tilt(x, times):
    """Brighter noise: first differences, each +6 dB per octave."""
    for _ in range(times):
        x = np.diff(x, prepend=0.0)
    return x / (np.max(np.abs(x)) + 1e-9)


def reverb(x, rng, seconds=1.6, decay=0.4, wet=0.3):
    """A room: x convolved (overlap-add, by FFT) with decaying noise whose top is rolled off above 9 kHz; each
    channel its own noise, so the two differ a little."""
    m = int(seconds * SR)
    spec = np.fft.rfft(rng.standard_normal(m) * np.exp(-np.arange(m) / SR / decay))
    ir = np.fft.irfft(spec / (1 + (np.fft.rfftfreq(m, 1 / SR) / 9000) ** 2), m)
    ir /= np.sqrt(np.sum(ir ** 2))
    block = 1 << 18
    size = 1 << int(np.ceil(np.log2(block + m)))
    h = np.fft.rfft(ir, size)
    y = np.zeros(len(x) + m)
    for i in range(0, len(x), block):
        seg = np.fft.irfft(np.fft.rfft(x[i:i + block], size) * h, size)[:block + m]
        y[i:i + len(seg)] += seg[:len(y) - i]
    return x + wet * y[:len(x)]


def track(seconds, root, bpm, seed):
    rng = np.random.default_rng(seed)
    beat = 60.0 / bpm
    bar_n = int(round(4 * beat * SR))
    n_bars = int(seconds * SR // bar_n)
    total = n_bars * bar_n
    out = np.zeros((2, total), dtype=np.float32)
    pad_osc = [[Osc(), Osc()] for _ in range(3)]
    bass_osc, arp_osc = Osc(), Osc()
    sweep_phase = 0.0
    pad_tabs, bass_tabs, arp_tabs = {}, {}, {}
    t_bar = np.arange(bar_n) / SR
    sixteenth = bar_n // 16
    t16 = np.arange(sixteenth) / SR
    eighth = bar_n // 8
    t8 = np.arange(eighth) / SR
    kick = np.sin(2 * np.pi * np.cumsum(45 + 85 * np.exp(-t_bar[:int(0.4 * SR)] / 0.04)) / SR) \
        * np.exp(-t_bar[:int(0.4 * SR)] / 0.22)
    for bar in range(n_bars):
        s0 = bar * bar_n
        phrase, pos = divmod(bar, 8)
        intro = phrase == 0
        outro = bar >= n_bars - 4
        brk = pos >= 6 and not intro  # the last two bars of a phrase: a break with a riser
        chord = CHORDS[bar % 4]
        sweep = 0.15 + 0.85 * (0.5 - 0.5 * np.cos(np.pi * ((bar % 16) + t_bar / (bar_n / SR)) / 8))
        if outro:
            sweep = sweep * (n_bars - bar) / 5
        # the pad: three saw voices, each a little detuned between the channels
        pad = np.zeros((2, bar_n))
        e = env_ar(bar_n, 0.06, 0.12)
        for v, semis in enumerate(chord):
            f = hz(root + 12 + semis)
            if f not in pad_tabs:
                pad_tabs[f] = brightness_tables(f)
            lvl = float(np.mean(sweep))
            for c, det in enumerate((0.998, 1.002)):
                pad[c] += bright_mix(pad_tabs[f], pad_osc[v][c], f * det, bar_n, lvl) * e * 0.16
        # the bass: the chord's root, two octaves down, on eighths
        fb = hz(root + chord[0] - 12 if chord[0] >= 0 else root + chord[0])
        if fb not in bass_tabs:
            bass_tabs[fb] = saw_table(min(30, 2500 / fb))
        bass = bass_osc.run(bass_tabs[fb], fb, bar_n)
        benv = np.concatenate([np.exp(-t8 / 0.18)] * 8)[:bar_n]
        bass = bass * np.pad(benv, (0, bar_n - len(benv))) * (0.0 if intro and pos < 4 else 0.30)
        # the arpeggio: chord tones an octave up, plucked sixteenths, panned left and right in turn
        arp = np.zeros((2, bar_n))
        tones = [root + 24 + s for s in chord] + [root + 36 + chord[0]]
        for k in range(16):
            f = hz(tones[ARP[k % 8]])
            if f not in arp_tabs:
                arp_tabs[f] = saw_table(min(40, 8000 / f), odd_only=True, slope=1.4)
            note = arp_osc.run(arp_tabs[f], f, sixteenth) * np.exp(-t16 / 0.07) * 0.13
            pan = 0.75 if k % 2 == 0 else 0.25
            arp[0, k * sixteenth:(k + 1) * sixteenth] += note * pan
            arp[1, k * sixteenth:(k + 1) * sixteenth] += note * (1 - pan)
        if outro:
            arp *= 0.5
        mix = pad + arp + bass
        drums = np.zeros((2, bar_n))
        if not intro and not outro:
            if not brk:
                for b in range(4):  # kick on every beat, snare on 2 and 4
                    i = int(b * beat * SR)
                    k_ = kick[:bar_n - i]
                    drums[:, i:i + len(k_)] += k_ * 0.55
                    if b % 2 == 1:
                        m = int(0.25 * SR)
                        sn = tilt(rng.standard_normal(m), 1) * np.exp(-np.arange(m) / SR / 0.11) * 0.16 \
                            + np.sin(2 * np.pi * 190 * np.arange(m) / SR) * np.exp(-np.arange(m) / SR / 0.06) * 0.2
                        drums[:, i:i + m] += sn[:bar_n - i]
            for h in range(8):  # hi-hats on eighths, the off-beat open before a bar's end
                i = h * eighth
                op = h == 7
                m = int((0.35 if op else 0.06) * SR)
                hat = tilt(rng.standard_normal(m), 2) * np.exp(-np.arange(m) / SR / (0.12 if op else 0.04))
                hat = hat[:bar_n - i] * (0.03 if h % 2 else 0.018)
                drums[0, i:i + len(hat)] += hat * 0.8
                drums[1, i:i + len(hat)] += hat
        if brk:  # a noise riser and a sine sweep going up an octave per half bar
            frac = (pos - 6 + t_bar / (bar_n / SR)) / 2
            noise = rng.standard_normal(bar_n)
            riser = (tilt(noise, 1) * frac + noise / 4 * (1 - frac)) * frac ** 2 * 0.08
            f_sw = 220 * 2 ** (frac * 4.5)
            ph = sweep_phase + np.cumsum(f_sw) / SR
            sweep_phase = ph[-1] % 1.0
            sw = np.sin(2 * np.pi * ph) * 0.09 + np.sin(4 * np.pi * ph) * 0.04
            drums[0] += riser * 0.9 + sw
            drums[1] += riser + sw
        if pos == 0 and not intro and not outro:  # a crash at each phrase's start
            m = int(1.6 * SR)
            cr = tilt(rng.standard_normal(m), 2) * np.exp(-np.arange(m) / SR / 0.6) * 0.035
            drums[:, :m] += cr[:bar_n]
        out[:, s0:s0 + bar_n] = mix + drums
    out = np.stack([reverb(out[c], rng) for c in range(2)]).astype(np.float32)
    out[:, :int(0.02 * SR)] *= np.linspace(0, 1, int(0.02 * SR))
    fade = int(3 * SR)
    out[:, -fade:] *= np.linspace(1, 0, fade)
    return out * (0.89 / np.max(np.abs(out)))


def encode(samples, path, meta):
    cmd = ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "f32le", "-ar", str(SR), "-ac", "2", "-i", "-",
           "-c:a", "flac", "-sample_fmt", "s16"]
    for k, v in meta.items():
        cmd += ["-metadata", f"{k}={v}"]
    subprocess.run(cmd + [path], input=np.ascontiguousarray(samples.T).astype("<f4").tobytes(), check=True)


def main(fixtures):
    lib = os.path.join(fixtures, "library")
    out = os.path.join(fixtures, "showcase-library")
    if os.path.exists(os.path.join(out, ".done")):
        return
    if not os.path.isdir(lib):
        sys.exit(f"no fixture library at {lib}: run make_fixtures.py first")
    shutil.rmtree(out, ignore_errors=True)
    for artist in sorted(os.listdir(lib)):  # hard links: no copy of the audio
        shutil.copytree(os.path.join(lib, artist), os.path.join(out, artist), copy_function=os.link)
    d = os.path.join(out, ARTIST, ALBUM_DIR)
    os.makedirs(d)
    landscape(ALBUM_DIR).save(os.path.join(d, "cover.jpg"), quality=90)
    for i, (title, secs, root, bpm) in enumerate(TRACKS, 1):
        encode(track(secs, root, bpm, seed=4100 + i), os.path.join(d, f"{i:02d} - {title}.flac"),
               dict(title=title, artist=ARTIST, album_artist=ARTIST, album=ALBUM, date=YEAR, genre=GENRE,
                    track=f"{i}/{len(TRACKS)}"))
        print("showcase:", title, flush=True)
    it = os.path.join(fixtures, "showcase-itunes")
    os.makedirs(it, exist_ok=True)
    for k, name in enumerate(ITUNES):
        landscape(name).save(os.path.join(it, f"{k}.jpg"), quality=90)
    open(os.path.join(out, ".done"), "w").close()


if __name__ == "__main__":
    main(os.path.abspath(sys.argv[1]))
