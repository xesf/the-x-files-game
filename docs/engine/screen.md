# Screen layouts

The original draws a 640x480 screen. Our engine draws the same screen,
then shows it in one of two layouts:

- **Original**: the 640x480 screen, scaled to fit the window with bars at
  the sides. This is the default, and the only layout of the DOS and
  Windows 9x builds.
- **Wide**: the scene fills a wide screen, and the interface goes around
  it. This is the default on the Vita.

Ctrl+Shift+W switches between the two. The setting is kept in the `prefs`
file with the Options screen's settings. `XF_SCREEN=wide` or
`XF_SCREEN=original` overrides it for one run.

## Why the scene can fill the screen

All 741 video tracks on the CDs are listed below by frame size. The
navigation stills are 600x240 too.

| Frame | Clips | Content |
|---|---|---|
| 600x240 (2.5:1) | 726 | Every scene clip |
| 600x400, 560x400 | 13 | The logos and other full-screen clips |
| Other | 2 | Small inserts |

In the original screen this 600x240 strip sits at (20, 90): 90 rows of
black above and 150 below. The interface lies over the strip or in the
rows under it. A wider output therefore gives the scene more room
without cutting anything.

| Output | Scene in the original layout | Scene in the wide layout |
|---|---|---|
| 1920x1080 (16:9) | 1350x540 | 1920x768 |
| 1280x800 (Steam Deck) | 1000x400 | 1280x512 |
| 960x544 (Vita) | 680x272 | 960x384 |
| 3440x1440 (21:9) | 1800x720 | 3440x1376 |
| 5120x1440 (32:9) | 1800x720 | 3600x1440, bars at the sides |

## The wide layout

`src/gfx/screen.c` places everything; `src/game/explore.c` draws in two
layers:

1. The **scene layer**, 640x480 as before: the clip or view picture, the
   flashlight's darkness, fades, the location card, the hotspot outlines
   and a `PlayAssetAt` clip that lies on the strip. The strip of this
   layer is scaled to fill the output width, or its height on outputs
   wider than 2.5:1. The space left is shared as in the original: 90
   parts above the strip and 150 below.
2. The **interface layer**, 640x480, transparent where nothing is drawn.
   Each element is cut out of it as a piece with an anchor:

| Piece | Anchor |
|---|---|
| Dialogue box | Under the strip, its centre under the character's centre on the strip |
| Emotion and action icons, captions | Under the strip, centred |
| Inventory | The bottom-left corner of the output |
| Message box | The centre of the output |
| Debug label | The top-left corner of the output |

Pieces scale with the interface scale: the output height over 480 (the
width over 640 on outputs narrower than 4:3). Pieces under the strip
stay above the inventory's band at the bottom, so that the inventory can
slide in without covering the dialogue box. On short outputs these
pieces lie over the strip, as the dialogue box already does in the
original. The cursor is drawn last, at the pointer's position on the
output.

Screens drawn for 640x480 as a whole use the original layout: interface
layouts (the computer, the PDA, the phone, close-ups), the main menu, the
Save and Load screens, and clips that are not 600x240, such as the logos.

The pointer maps back through the same placement (`scr_to_screen`):
on a piece it maps into the piece, on the strip into the strip, and
below the strip it is measured from the bottom of the output, where the
inventory's band starts. The game itself sees 640x480 positions only.

## Platforms

`plat_output_size` gives the size of a frame that fills the window or
screen one to one: the window on macOS, Linux and Windows, 960x544 on
the Vita, and 640x480 under DOS. A frame of another size keeps its
proportions in the window, and mouse events arrive in its pixels.

The compositor works in software. It limits frames to 1920x1200; a larger
window gets a smaller frame that the platform scales up. Scaling is
bilinear. A 1920x1080 frame takes about 6 ms on the development Mac; a
Vita frame is under a third of that work.

## Upscaled media

The scene layer is separate from the interface so that its pictures can
come from elsewhere. Clips and stills upscaled ahead of time, or a
filter run on each frame, would replace the strip before the scaling
step. The interface and the pointer mapping stay the same.

## Testing

`XF_OUTPUT=<w>x<h>` sets the output size of a headless run, and `shot`
saves the frame as shown:

```sh
XF_HEADLESS=1 XF_SCREEN=wide XF_OUTPUT=1920x1080 \
  build/xfiles --explore <game> 0 1 --media <cd> --script shots.txt
```

The unit tests check the placement for the outputs in the table above.
