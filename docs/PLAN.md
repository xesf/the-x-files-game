# Reverse-engineering and reimplementation plan

Goal: a clean C99 reimplementation of the VirtualCinema engine as used by
*The X-Files Game* (HyperBole Studios / Fox Interactive, 1998). It runs on the
original game data and has no third-party dependencies. The first target is
macOS. Next come Steam Deck (Linux) and PS Vita, then the other platforms.

## 1. What we have

### Binaries

| Binary | Use |
|---|---|
| `xfiles.exe`, DVD build (in `ida/`) | **Primary RE target.** It handles both DVD `.vob` and CD data. |
| `xfiles.exe`, EN CD, localized CD and JP builds | Diff against the DVD build for language and disc differences. |
| Mac PowerPC build | Cross-check of the original Mac code path: endianness, QuickTime calls, a non-DirectX renderer. |
| PSX `XFILES.EXE` | Low-memory reference that shows how the engine was cut down. It helps with PSP and Vita budgets. |
| Older Hex-Rays dumps and IDBs | Earlier annotation work. Import names and types where they still apply. |

### Private reference material

The team gave us non-public engine builds, tools and HyperBole's original
design documents. They are **reference only**:

- Use them to understand and name things: class names, function purposes,
  field meanings, enums and intended behavior.
- Never commit them or anything taken from them verbatim (strings, paths,
  file names, build names, document text).
- Never cite them in code, comments, commit messages or public docs. Our
  names and wording are our own, and we verify every fact against the
  shipped game binary.
- Keep local notes from them in a git-ignored folder (`notes/private/`).
- We write our own engine documentation in `docs/engine/` (subsystems,
  game state, save format, timers, hotspots, conversations, navigation).
  It is written from scratch in our words and verified against the shipped
  binary and data. It never copies or paraphrases the original documents
  section by section.

### Data knowledge

- **ScummVM wiki:** container and codec list. `.xmv`, `.nmv`, `.amv`,
  `.dmv` and `.mus` are QuickTime MOV files. Video codecs: `cvid`, `jpeg`,
  `rpza`. Audio codecs: `ima4`, `qdmc` (QDesign Music 2). Subtitles use a
  `text` track. `.pff` is a JPEG bundle, `.hot` stores hotspot rectangles,
  `.xtx` is text and `.ttr` is TrueType.
- **mthcore/x-files-game (MIT):** byte-exact NeoPersist 3.0 decoding of
  `XFILES.HDB`: 51 VC classes, triggers, hotspots, conversations, the
  decision tree, emails, the GAM variable table and the PFF format.
- **Zeffuro/x-files-pc-enhancement-patch (MIT):** a DirectDraw/QuickTime
  replacement that runs the original under Wine or Windows. We use it to
  capture ground truth.
- `../extract/` (our own scripts): HDB/GAM to JSON. `../Installations/data`
  holds an already-extracted asset tree for tests.
- Save files `../Others/x-files-the-game/start*.x` load straight into a
  specific disc, which makes them good test fixtures.

## 2. License

**GPL-3.0-or-later.** ScummVM is GPL-3.0-or-later, and the MIT sources can be
included in a GPL-3 project. So GPL-3 lets us learn from, and where useful
port, ScummVM's QuickTime and codec code, and still credit both MIT
projects. The code carries no references to these projects. All credits and
MIT notices go in `README.md`. Do not copy the vendored NeoPersist headers
from mthcore: they are proprietary, and NOTICE limits them to
interoperability use. Write our own definitions from the format notes. Never
commit game data, binaries or IDA databases built from them (see §8).

## 3. Engine architecture (target)

```
src/
  core/      memory, file I/O, endian, log, assert, fixed-size containers
  neo/       NeoPersist object-store reader (HDB, GAM, save files)
  vc/        VirtualCinema model, one module per engine class
             (scene, node, link, hotspot, trigger, action, variable,
             uberstate, conversation, inventory, PDA, timer)
  script/    trigger/condition/action interpreter
  media/     mov (atom parser + sample tables), jpeg, cinepak, rpza,
             ima4, qdm2, text track, pff, hot, xtx, ttf rasterizer
  gfx/       640x480 framebuffer, blits, blend/colormask FX, cursor, fonts
  audio/     mixer (music, ambient, dialogue, sfx, 3D panning)
  game/      app loop, navigation, UI screens, save/load, prefs
  platform/  one thin backend per OS (see §6)
tools/       xftool CLI: list/extract/convert/verify all formats
tests/       format round-trips and decoder golden frames
```

- C99. The original is C++ with a single-root class hierarchy and a
  persistable base class for HDB objects. We model its classes as C structs
  with explicit vtable structs, and name each one after its role in the
  game. IDA addresses map to our code through the address comments (§4). Use C++ only if
  a platform SDK requires it (none of the first three do).
- The engine is driven by data from the HDB. Most game logic lives in the
  data, so the engine code is mostly a VM plus a media player. This keeps
  the RE scope manageable.
- Render internally at 640×480 in 32-bit color (the original ran at 8 bpp
  with JPEG dithering), then scale in the backend.

## 4. Reverse-engineering workflow

1. **IDA setup.** Start IDA with the MCP plugin server (port 13337) before
   each RE session. If the session started without it, reconnect with
   `/mcp`.
2. **Repair the initial auto-analysis.** Done when
   `tools/ida/fix_noreturn.py` has run and its "noreturn" list is reviewed.
   - FLIRT matched the allocator wrapper at `0x521d20` as `_exit`. It is
     `operator new`: size 0 becomes 1, it calls the heap allocator and
     retries through the new-handler. IDA flagged it as no-return, so every
     function that allocates was cut short at that call.
   - The MCP server cannot clear function flags or reanalyze, so the script
     must run inside IDA (File > Script file…).
   - Check every other FLIRT no-return match the same way. The allocator is
     a third-party heap (Compuware copyright string), so library matches
     near `0x521000` need checking too.
3. **Naming pass on the DVD build.** Names in our IDB and code are our own
   descriptive names.
   - **Class backbone (public anchor).** The exe holds a table of 52
     VirtualCinema class-name strings at `0x6b8b5c`. These include VCNode,
     VCHotSpot, VCTrigger, VCAction, VCConversation, VCGameState, VCVariable,
     VCInventory and VCEmail. Only code at `0x4980a4` (inside `0x498060`)
     uses the table. Follow it to the class registration, then to each
     class's factory, constructor and vtable, then to its HDB load method.
     These names also appear in the game data, so the code may use them.
   - Use the private reference builds to identify the remaining classes and
     functions. Match by unique string references, then by call-graph
     neighbors. Script this through the MCP.
   - The older `.idb` and `.c` dumps carry about six names and add almost
     nothing, so we skip them as a naming source.
   - Recover vtables (the release build has no RTTI) and rebuild class
     layouts from constructors and load methods.
   - Name the platform layer. The import table has DirectDraw
     (`DirectDrawCreate`), GDI, WinMM (`mmio*`, `waveOut*`, MCI for DVD)
     and the registry, but no QuickTime. QuickTime is loaded at run time
     (`LoadLibraryA`/`GetProcAddress`, "QuickTime.qts"). Find that loader
     and name each resolved entry point, because every QuickTime call site
     marks a media feature we must reimplement.
4. **Per-subsystem decompilation** (use the `decomp` skill). Start from the
   entry points that the data drives:
   NeoPersist loading → VC class load methods → trigger evaluation → action
   dispatch → scene and node transitions → media playback calls.
   Each ported function records its original address in a one-line comment
   (`/* 0x4xxxxx */`) so we can audit it.
5. **Ground truth.** The original game, with the enhancement patch, is
   installed under Wine at
   `~/.wine/drive_c/Games/The X-Files/XFilesPlay.exe` (Wine 9.3 at
   `/usr/local/bin/wine`).
   - Its `XFiles.exe` is the **EN CD build** (MD5 `e9f3…75`), not the DVD
     build in our IDB. Addresses differ between the two, so match functions
     by content, not by address. The patch adds DVD movies through its own
     MPEG DLL.
   - Its developer tools (Ctrl+F11) give state ground truth with no extra
     hooks:
     - **Game state:** named variables and inventory, a "changed since
       baseline" filter, and a per-session history with old and new values
       and the ID of the action that wrote them.
     - **Database & assets:** HDB records, their fields and links, and the
       raw bytes.
     - **Movies:** live clips, subtitles and asset labels.
   - `logs/game-context.log` dumps the registered variables (for example
     `RegNode*Vars`, `RegLink*Vars`, `RegCurrInvVar`) with their IDs.
     `logs/quicktime.log` shows which media calls the game makes.
   - Testing method: play the same inputs in the original and in our engine,
     then diff the variable history and the scene/node/asset sequence.
     Compare screenshots with the `pixel-parity` skill.
   - Copy logs from there into `notes/private/` if needed. Never commit them.

## 5. Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | Repo skeleton, LICENSE, CMake + Makefile, macOS backend shows a window and plays a WAV tone | Builds with `clang` only, no external dependencies |
| M1 | `xftool` + format readers: HDB/GAM (NeoPersist), PFF, HOT, XTX, MOV atoms | Byte-exact round-trip on HDB/GAM. All assets in `../Installations/data` listed and mapped |
| M2 | Codecs: JPEG baseline, Cinepak, RPZA, IMA4, text track, then QDM2 | Every `.xmv`/`.nmv`/`.amv` decodes. Frame hashes match FFmpeg reference dumps (dev-only) |
| M3 | Media player: A/V sync, looping, NMV dual-stream nav, subtitles, TTF text | One FMV with subtitles plays correctly in the macOS app |
| M4 | VC model + script VM from the HDB; navigation, hotspots, cursor | Walk the CD1 opening (Mulder's apartment / Seattle) with the mouse |
| M5 | Inventory, PDA, phone, computer, conversations, UberStates, timers, save/load (`.x`) | Original `start*.x` saves load and continue. State traces match the original |
| M6 | Full game on all 7 CDs + DVD `.vob` (MPEG-2 decoder; may be optional), all languages | Complete playthrough on every major branch, verified by state traces |
| M7 | Ports: Steam Deck → Vita → Windows/Linux → iOS/Android → Web → PSP | See §6 |

M1 and M2 do not need IDA. They can start right away, in parallel with the
naming pass for M4.

## 6. Platforms

There are no third-party libraries. Each backend uses only the OS or SDK
APIs, behind one small interface: `plat_init`, `plat_present(fb)`,
`plat_poll(events)`, `plat_audio_submit`, `plat_fs_*`, `plat_time`.

| Platform | Backend | Notes |
|---|---|---|
| macOS | Cocoa (a small Objective-C shim) + Metal or CoreGraphics + CoreAudio | First target |
| Steam Deck / Steam Machine / Linux | X11 (XWayland under gamescope) + ALSA, evdev or the Steam Input gamepad | Map gamepad controls to the pointer and hotspot cycling, as the original keyboard navigation does |
| PS Vita | VitaSDK (GXM, audio out, ctrl, touch) | 512 MB RAM and a 444 MHz ARM. Use NEON paths for the IDCT and Cinepak. Stream video from storage |
| Windows | Win32 + GDI/D3D11 + WASAPI | |
| iOS / Android | UIKit/Metal; NativeActivity + AAudio | Touch makes a good point-and-click interface |
| Web | Emscripten + a small JS glue file for canvas and WebAudio | Data must be provided by the user |
| PSP | PSPSDK | 32 MB RAM and a 333 MHz CPU. Might need lower-resolution decode or pre-transcoding. Stretch goal |
| Analogue Pocket | openFPGA has no general CPU core that fits this workload | **Not feasible** unless someone builds a soft-core platform. Leave it out of scope for now |

Keep the core free of platform code, use fixed-point audio, and make
allocation explicit (arena per scene). This lets the slow platforms work
without a redesign.

## 7. Risks

- **QDM2 audio** is the hardest codec. Port the decoder logic from ScummVM's
  `qdm2` (GPL-compatible) and check it against reference output.
- **MPEG-2 for DVD `.vob`:** this is a large job. Ship CD assets first and
  treat DVD video as optional.
- **HDB semantics:** the bytes are decoded, but what the fields mean comes
  from the engine. Each VC class's load method in IDA is the authority.
- **The reference builds are not the shipped version:** class layouts may
  differ from 1998. Use them for understanding only, and verify layouts in
  the DVD binary.

## 8. Repo hygiene

- `.gitignore` the IDA databases, every game file and `notes/private/`. `ida/xfiles.exe.i64`
  is currently staged and contains the original code. Keep it out of any
  public remote.
- The game data path is set at runtime. The engine never ships assets.
