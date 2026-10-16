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
about a minute. It reports only the two `XG` files with no movie header.

The `QDMC` sound decoder is checked against ffmpeg: `xftool audio` and
`ffmpeg -i <file> -f wav -acodec pcm_s16le` give the same samples within
one step (float rounding) for all 15 `QDMC` files of the CDs.

## Ground truth from the original game

The original game, with the X-Files PC enhancement patch, runs under
Wine:

```sh
cd "$HOME/.wine/drive_c/Games/The X-Files" && wine XFilesPlay.exe
```

- Window captures (`screencapture -l <window id>`) give screen layouts.
  Matching decoded movie frames against a capture gives exact positions
  (this is how the main menu layout was measured).
- `logs/quicktime.log` lists every QuickTime call the game makes, with
  the calling address in the CD build.
- The patch's developer tools (Ctrl+F11) show game variables and their
  history, and browse the database.

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

## Story scripts

`xftool play` runs the game logic from a script, without a window or
media: every clip ends as soon as it starts, so its start and end
triggers still run in order. The scripts in `tests/play` follow the
story disc by disc, from the first scene to the credits (`disc7.txt`
ends with `expect over 1` after the closing clips). `make playtest` runs
all of them, with `XF_SAVE_DIR` set to `build/saves`.

The scripts name the rectangle in `shoot <id>`, which works without the
HOT file. The window needs the file to hit-test a click. The DVD
install folder has the HOT files of the warehouse, Rauch and woods
fights. It lacks those of eight secret-base clips (21792, 23428, 49015,
49017, 49024, 66733, 70515 and 90178), which play from the disc, and
the 360 clips whose rectangles 500 and 600 let the player shoot the
people Willmore talks to. To play those with DVD data, copy the CD's
`XV/*.hot` files into the DVD's `xv` folder.

```sh
build/xftool play ../Installations/x-files-en-dvd tests/play/disc1.txt
XF_TRACE=1 build/xftool play <game> <script>   # with every action
```

A script has one command per line; `#` starts a comment.

| Command | What it does |
|---|---|
| `new` | Starts a new game, as the New item of the main menu |
| `view <id>` | Jumps to a view |
| `walk <view>` | Walks to a view through the moves that are enabled now |
| `click <id>` | Clicks a hotspot or interface item by object ID; for a move, the target view also works |
| `at <x> <y>` | Clicks a screen point |
| `back` | Goes back to the previous view |
| `wait <ms>` | Advances the clock, firing timers |
| `include <script>` | Plays another script first, without printing its steps |
| `ask <conversation>` | Asks one of the questions on offer |
| `askall <character>` | Talks to a character and asks every new question, until none is left |
| `hold <item>` / `hold none` | Takes an inventory item in hand by name (`CaseFiles`), or empties the hand |
| `inv <id>` | Clicks a `VCInventory` item in the inventory bar |
| `evidence <id>` | Hands a piece of evidence over from the lab's evidence bar |
| `icon <id>` | Clicks an emotion or action icon |
| `dial <number>` / `nodial <number>` | Types a number on the cell phone and sends it; `nodial` expects no answer |
| `rollover <item>` / `rollover none` | Moves the pointer onto an inventory item, or off it (the evidence kit's popup) |
| `open <layout>` | Opens an interface layout, as the menu's Options and Help do |
| `shoot [id]` | Shoots at a HOT rectangle of the view's clip (by default its first); most shots need the gun in hand |
| `until-clip <ms>` | Lets time pass until a clip is queued, and leaves it unplayed so the next step can shoot into it |
| `type <text>` | Types the text, key by key, into the interface item with focus |
| `ing <P\|L\|N> <database> <text>` / `noing ...` | Runs an ING search at the office computer and expects a record, or none |
| `set <var> <value>` | Sets a GAM variable |
| `save <file> [name]` / `load <file>` | Writes the game to a save file, or loads one; a `.x` file is in the original's format |
| `need <file>` | Stops the script, as passed, when the file is missing: for files that are not part of the game |
| `reload` | Saves to memory and loads again at once |
| `restore` | Goes back to the last `SaveGameState` checkpoint |
| `slots save` / `slots load` | Opens the Save or Load screen, as the menu does (needs `XF_SAVE_DIR`) |
| `answer yes\|no\|ok` | Answers the message box |
| `previous` | Loads the slot saved or loaded last |
| `route <view>` | Prints the shortest way to a view over every move, and the enable state that closes each closed move |
| `gate <id>` / `gate enable <id>` | Prints the GAM enable state of an object, or the object an enable state controls |
| `var <id>` | Prints a GAM variable with its name |
| `text <id>` | Prints the text of a media reference |
| `assets <item>` | Prints a layout item's media, with their text and enable state |
| `state` | Prints the view, its hotspots and the interface layouts that are up |
| `solve <depth> var <id> <value>` / `solve <depth> view <id>` | Searches for moves (clicks with or without an item, questions, icons, shots, waits) that reach the goal, and prints the first sequence found |
| `reach` | Lists the views reachable through the moves enabled now, with their objects and characters |
| `expect view <id>` | Fails unless the current view is `<id>` |
| `expect window menu\|paused\|playing\|credit 0\|1` | Window scripts: the main menu is up, the game's media hold, a clip plays, an opening credit card shows |
| `expect window sound <asset>` / `clip <asset>` | Window scripts: the sound playing beside the clips, or the clip playing (0 for none); a failed check prints the value |
| `expect disc <n>` | Fails unless the game is on disc `n` |
| `expect var <id> <value>` | Fails unless the GAM variable has the value |
| `expect enabled <id> 0\|1` | Checks the enabled state of an HDB object |
| `expect spot <id>` / `nospot <id>` | Checks that a hotspot is or is not in the view |
| `expect iface <id>` / `noiface <id>` | Checks that a layout is or is not up |
| `expect over 0\|1` | Checks whether the game has ended (`EndGame`: death, or the credits) |
| `expect played <id>` | Checks that a clip was among the last 32 started |
| `expect checkpoint 0\|1` | Checks whether `SaveGameState` has taken a checkpoint |
| `expect message <string>` | Checks that the message box shows this engine string |
| `expect mail <id>` | Checks that this mail is in the mailbox |
| `expect choice <id>` / `nochoice <id>` | Checks that a question is or is not on offer |
| `expect photo <id>` | Checks that the camera holds a photo of this media |

The window can run the same scripts and take screenshots, with or
without showing the window:

```sh
XF_HEADLESS=1 build/xfiles --explore <game> 0 1 --media <cd> --script shots.txt
```

| Command | What it does in the window |
|---|---|
| `clips on` / `clips off` | Lets clips play across steps instead of ending with each step |
| `key escape\|return\|tab\|space\|left\|right\|up\|down\|<char>` | Presses a key, as the platform would send it; without a window, it goes to the layout field with focus |
| `press <x> <y>` | Clicks a 640x480 screen point through the window's input, in any layout |
| `play <ms>` | Lets time pass with the clips playing |
| `skip` | Ends every clip at once, running its triggers |
| `shot <file.bmp>` | Saves the screen |

For example, `new`, `clips on`, `play 2500`, `shot card.bmp` captures the
location card of a new game. With `XF_SCREEN=wide` and
`XF_OUTPUT=<w>x<h>`, the shots show the wide layout at that size (see
[engine/screen.md](engine/screen.md)).

The run stops at the first failing step and prints the state there. At
the end it lists the engine functions the story called that are not
implemented yet.

Besides the disc scripts, scripts follow the strategy guide's side
routes and warnings: filing the APB yourself (`apb.txt`), the night
stakeout (`stakeout-death.txt`), the Media Index and forwarding an
article (`media.txt`, `forward.txt`), the bomb's wire cutters and Try
again (`bomb-death.txt`), the kiss (`kiss.txt`), lying to Scully
(`scully-lie.txt`), the NSA car and the porch gunfight (`nsa-car.txt`,
`rauch-gunfight.txt`), a death in the woods (`woods-death.txt`), Cook in
the lab (`lab-cook-death.txt`, `lab-shoot-cook.txt`), the suicide note
(`suicide.txt`), the phone (`phone.txt`), the logins' other answers and Scully's
PowerBook (`passwords.txt`), the Options screen
(`options.txt`) and saving (`save.txt`).

Scripts in `tests/window` need the window: `menu.txt` opens the main
menu with Escape and with the X icon during a clip, and returns;
`keyboard.txt` moves and clicks with the keys; `credits.txt` checks the
opening credits and their music.

```sh
XF_HEADLESS=1 build/xfiles --explore <game> 0 1 --media <cd> --script tests/window/menu.txt
```

`xsave.txt` writes and loads a `.x` save, then loads saves written by
the original engine. These files are not part of the game, so the
script looks for them in `../Others/x-files-the-game` and stops early,
as passed, when they are missing. `XF_PLAY_RELOAD=x` reloads through
`.x` saves instead (see [formats/save.md](formats/save.md)).

With `XF_PLAY_RELOAD=1`, a script saves and loads the game after every
step that ends at rest: no layout open and no conversation running. All
seven disc scripts reach the credits this way, which shows that a save
keeps everything the story needs (see [formats/save.md](formats/save.md)).

To find the IDs for a walkthrough step, `tools/re/vc_map.py` prints the
views around a view, their moves and objects, and what clicking each
object does:

```sh
python3 -I tools/re/vc_map.py XFILES.HDB 691 40
```

`tools/re/vc_who.py` finds what sets a variable or enable state, what
calls a function, and what plays a media list (`media <list id>`).

`tools/re/vc_layout.py` prints an interface layout: its items, their
regions and pictures, and what each trigger does:

```sh
python3 -I tools/re/vc_layout.py XFILES.HDB 15108
```
