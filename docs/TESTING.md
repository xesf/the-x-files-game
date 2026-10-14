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
```

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

- **JPEG:** `xftool sample` saves the raw frame, and `sips` (macOS)
  decodes it as a reference.

Current results:

| Decoder | Reference | Result |
|---|---|---|
| Cinepak | ffmpeg | Bit-exact on the frames tested |
| JPEG | macOS ImageIO | Mean difference 0.13 to 0.31 per channel; ImageIO interpolates chroma, we use nearest neighbour |
