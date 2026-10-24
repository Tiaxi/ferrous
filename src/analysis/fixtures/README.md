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

`tag-tone.m4a`, `tag-tone.opus`, and `tag-tone.wv` are 0.1 s mono 440 Hz
tones in ALAC (MP4), Opus, and WavPack. Tag writing tests replace the tags
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
  -c:a wavpack -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
  tag-tone.wv
```
