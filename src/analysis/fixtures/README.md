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
