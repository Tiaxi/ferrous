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
