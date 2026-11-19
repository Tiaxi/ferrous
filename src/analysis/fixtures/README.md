# Test fixtures

`raw-tone.ac3` and `raw-tone.dts` are raw AC-3 and DTS streams of a 440 Hz mono
tone at 48 kHz: 32 AC-3 frames (1.024 s) and 47 DTS frames (about 0.5 s). Tests
add leading partial frames and `APEv2` tags to copies of them. They were made
with FFmpeg 8.1, whose `-bitexact` output is reproducible:

```bash
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 -ac 1 \
  -c:a ac3 -b:a 32k -f ac3 -bitexact raw-tone.ac3
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=48000:duration=0.5 -ac 1 \
  -c:a dca -strict -2 -b:a 192k -f dts -bitexact raw-tone.dts
```

`raw-stereo-coupled.ac3`, `raw-2.1.ac3`, `raw-3.1-coupled.ac3`, `raw-5.0.ac3`,
`raw-5.1-coupled.ac3`, and `raw-5.1-gated.ac3` are short AC-3 streams in other
channel layouts, some with coupling, for the parser that finds the dynamic range
words of each block (`src/raw_audio/ac3_dynamic_range.rs`). They are 6 frames of
tones with a little noise and a click at 0.1 s, but for `raw-5.0.ac3`, whose
tones are high over quieter noise, and `raw-5.1-gated.ac3`, frames 7 to 9 of
gated noise and tones, whose bands reach far into the PSD integration's
log-addition table. The FFmpeg encoder writes no dynamic range words; tests
add them. Made with FFmpeg 8.1:

```bash
tone() { echo "0.25*sin(2*PI*$1*t)+0.03*(random($2)-0.5)+0.5*exp(-2000*abs(t-0.1))*sin(2*PI*3000*t)"; }
high() { echo "0.002*(random($1)-0.5)+0.3*sin(2*PI*$2*t)+0.3*sin(2*PI*$3*t)+0.2*sin(2*PI*15000*t)+0.5*exp(-2000*abs(t-0.1))*sin(2*PI*3000*t)"; }
ac3() { # name, layout, bitrate, coupling, seconds, then one expression a channel
  local name=$1 layout=$2 rate=$3 coupling=$4 seconds=$5; shift 5; local IFS='|'
  ffmpeg -f lavfi -i "aevalsrc=$*:s=48000:d=$seconds:c=$layout" -c:a ac3 -b:a "$rate" \
    -channel_coupling "$coupling" -f ac3 -bitexact "$name.ac3"
}
ac3 raw-stereo-coupled stereo 96k 1 0.192 "$(tone 440 1)" "$(tone 554 2)"
ac3 raw-2.1 2.1 96k 0 0.192 "$(tone 440 1)" "$(tone 554 2)" "0.3*sin(2*PI*60*t)"
ac3 raw-3.1-coupled 4.0 128k 1 0.192 "$(tone 440 1)" "$(tone 554 2)" "$(tone 660 3)" "$(tone 880 5)"
ac3 raw-5.0 5.0 160k 0 0.192 "$(high 1 5100 9300)" "$(high 2 6100 11300)" \
  "$(high 3 7100 12700)" "$(high 5 8100 13900)" "$(high 6 9900 16100)"
ac3 raw-5.1-coupled 5.1 192k 1 0.192 "$(tone 440 1)" "$(tone 554 2)" "$(tone 660 3)" \
  "0.3*sin(2*PI*60*t)" "$(tone 880 5)" "$(tone 990 6)"
ac3 gated 5.1 448k -1 0.384 "(random(1)-0.5)*gt(sin(2*PI*23*t)\,0)" \
  "(random(2)-0.5)*gt(sin(2*PI*29*t)\,0)" "sin(2*PI*12000*t)*gt(sin(2*PI*37*t)\,0.5)" \
  "0.5*sin(2*PI*50*t)" "(random(3)-0.5)*0.01+sin(2*PI*16000*t)*gt(sin(2*PI*13*t)\,0)" \
  "(random(4)-0.5)*lt(mod(t\,0.05)\,0.002)"
dd if=gated.ac3 of=raw-5.1-gated.ac3 bs=1792 skip=7 count=3
```

`raw-5.1-side.ac3` is 8 frames (0.256 s) of tones in a 5.1(side) layout at
48 kHz and 448 kb/s, as surround discs carry it. A Qt test repeats its frames,
each of which decodes on its own, into a stream of a few seconds that the
GStreamer backend plays. Made with FFmpeg 8.1:

```bash
ffmpeg -f lavfi -i "aevalsrc=0.25*sin(2*PI*440*t)|0.25*sin(2*PI*554*t)|0.25*sin(2*PI*660*t)|\
0.3*sin(2*PI*60*t)|0.25*sin(2*PI*880*t)|0.25*sin(2*PI*990*t):s=48000:d=0.256:c=5.1(side)" \
  -c:a ac3 -b:a 448k -f ac3 -bitexact raw-5.1-side.ac3
```

`raw-tone-44k.dts` is the same tone at 44.1 kHz in 44 DTS frames of 224 bytes
(about 0.51 s). That bitrate makes each frame fill whole 14-bit words, as the
frames of DTS-CDs do; GStreamer's parser loses most frames of a stream whose
frames end in a padded word. Tests pack the frames into the low 14 bits of 16-bit words and put
them in a WAV file, as DTS-CD rips hold them:

```bash
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=44100:duration=0.5 -ac 1 \
  -c:a dca -strict -2 -b:a 154350 -f dts -bitexact raw-tone-44k.dts
```

`tag-tone.m4a`, `tag-tone.ogg`, `tag-tone.opus`, and `tag-tone.wv` are 0.1 s
mono 440 Hz tones in ALAC (MP4), Ogg Vorbis, Opus, and WavPack. Tag writing tests replace the tags
of copies of them. They were made with FFmpeg 8.1, whose `-bitexact`
output is reproducible:

```bash
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=8000:duration=0.1 -ac 1 \
  -c:a alac -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
  -f ipod tag-tone.m4a
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=48000:duration=0.1 -ac 1 \
  -c:a libopus -b:a 16k -map_metadata -1 -fflags +bitexact \
  -flags:a +bitexact tag-tone.opus
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=8000:duration=0.1 -ac 1 \
  -c:a libvorbis -b:a 32k -map_metadata -1 -fflags +bitexact \
  -flags:a +bitexact tag-tone.ogg
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=8000:duration=0.1 -ac 1 \
  -c:a wavpack -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
  tag-tone.wv
```

`gapless-click.mp3` is 0.4 s of 44.1 kHz mono silence with a single click at
0.2 s (frame 8,820), in 64 kbps MP3 with a LAME tag that gives the encoder
delay and padding. Playback (GStreamer) and analysis cut both, so the click
plays and shows at frame 8,820; uncut, it is 1,105 frames later. The frame of
the tag must be large enough to hold it, hence 64 kbps. It was made with
FFmpeg 8.1 and LAME 3.100:

```bash
ffmpeg -f lavfi -i "aevalsrc='if(eq(n\,8820)\,0.9\,0)':s=44100:d=0.4" \
  -c:a pcm_s16le click.wav
lame -b 64 --resample 44.1 -m m click.wav gapless-click.mp3
```

`channel-change.mp3` is 0.25 s of a stereo 440 Hz tone followed by 0.25 s of
a mono 880 Hz tone, as 22.05 kHz MP3 frames without tags, so a decoder's
channel count changes mid-stream:

```bash
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=22050:duration=0.25 -ac 2 \
  -c:a pcm_s16le stereo.wav
ffmpeg -f lavfi -i sine=frequency=880:sample_rate=22050:duration=0.25 -ac 1 \
  -c:a pcm_s16le mono.wav
lame -t -b 64 --resample 22.05 -m j stereo.wav stereo.mp3
lame -t -b 32 --resample 22.05 -m m mono.wav mono.mp3
cat stereo.mp3 mono.mp3 > channel-change.mp3
```

`centered-view.txt` lists where a centered spectrogram view starts for a
playhead position, a window and a track duration. The decoder's model of the
view (`src/analysis/centered_view.rs`) and the Qt view
(`SpectrogramItem::centeredDisplayRange`) are both tested against it, so the
two agree.
