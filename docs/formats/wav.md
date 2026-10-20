# WAVE sounds (earlier games)

Quantum Gate (450 files) and Vortex (21) keep sound effects as RIFF
WAVE files. All but one are PCM: 8-bit unsigned (11025 or 22050 Hz,
mono, three stereo) or, once, 16-bit signed. Quantum Gate's
`SAUNDERS/2C_SND/ASK16X.WAV` says format 3 at 4 bits, which no player
reads; the reader leaves it out (470 of 471 decode).

A WAVE file is a `RIFF` chunk of type `WAVE` holding chunks of a
four-letter name and a little-endian size, padded to an even size. The
`fmt ` chunk gives the format (1 for PCM), the channels, the rate and
the bits per sample; the `data` chunk holds the samples, interleaved.
An 8-bit sample is unsigned, 128 being silence.

- Reader: `src/media/wav.c`.
- `xftool wav <files>` decodes and reports failures.
