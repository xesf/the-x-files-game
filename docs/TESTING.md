# Testing

## Unit tests

`make test` runs checks that need no game data.

## Checks against game data

`xftool` decodes every format and reports failures. Point it at your own
copy of the game:

```sh
G=/path/to/game
xftool pff-image $G/X.PFF all            # every image in a bundle
xftool mov $G/XV/*.xmv                   # every movie header
xftool frame $G/XV/19672.xmv 1 150 a.bmp # one decoded video frame
xftool check $G/XV/*.xmv $G/XS/*.mus     # decode every frame and sound chunk
```

On the English CD release, `xftool check` over all 1,198 movie files takes
about a minute. Today it reports only the 15 files with `QDMC` sound
(decoder not written yet) and the two `XG` files with no movie header.

## Comparing decoders with a reference

During development we compare our decoders with an independent one. These
tools are not needed to build or run the engine.

- **Video:** `ffmpeg` decodes the same frame, then
  `tools/re/bmpdiff.py` compares the two BMP files:

  ```sh
  ffmpeg -i $G/XV/19672.xmv -map 0:v:0 -vf "select=eq(n\,150)" -vsync 0 \
         -frames:v 1 -pix_fmt bgr24 ref.bmp
  xftool frame $G/XV/19672.xmv 1 150 our.bmp
  python3 -I tools/re/bmpdiff.py ref.bmp our.bmp
  ```

- **Audio:** `xftool audio` writes a WAV file, ffmpeg writes raw
  samples, and `tools/re/pcmdiff.py` compares them:

  ```sh
  xftool audio $G/XS/30401.mus our.wav
  ffmpeg -i $G/XS/30401.mus -map 0:a:0 -f s16le ref.raw
  python3 -I tools/re/pcmdiff.py our.wav ref.raw
  ```

- **JPEG:** `xftool sample` saves the raw frame, and `sips` (macOS)
  decodes it as a reference.

Current results:

| Decoder | Reference | Result |
|---|---|---|
| Cinepak | ffmpeg | Bit-exact on the frames tested |
| RPZA | ffmpeg | Bit-exact, first and last frames of three clips |
| IMA ADPCM (`ima4`) | ffmpeg | Bit-exact on music, ambient sound and scene audio |
| PCM (`twos`) | ffmpeg | Bit-exact |
| Motion JPEG A | ffmpeg | Maximum difference 4, mean 0.71 |
| JPEG | macOS ImageIO | Mean difference 0.13 to 0.31 per channel; ImageIO interpolates chroma, we use nearest neighbour |
