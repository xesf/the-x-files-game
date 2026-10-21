# Saved games

## The original `.x` files

The original engine keeps the running state in `Temp.gam`, a copy of
`XFiles.gam` made at start-up. A save is that NeoAccess database written
out with the extension `.x` (`0x4D14B0` writes it). It has the same
header as the GAM and the same object IDs: the variables (class `0x53`),
enable states (`0x46`) and strings (`0x50`) hold their values at the
time of the save. Seven classes come on top:

| Class | Name | Data |
|---|---|---|
| `0x56` | `VCConversationHistory` | A blob of conversation IDs, one for each time a question was asked |
| `0x57` | `VCGameState` | The header (below) |
| `0x58` | `VCPDANotes` | A blob of the PDA notes' media IDs |
| `0x59` | `VCPhoto` | One object for each photo in the camera (below) |
| `0x5A` | `VCEmail` | A blob of the mail received (text media IDs) |
| `0x5B` | `VCEmailRead` | A blob of the mail read |
| `0x5C` | `VCEmailPending` | A blob of the replies on the way |

An object starts with the 6-byte record header (flags, version). A blob
is a file offset and a length. The numbers inside these blobs are
little-endian: the engine copies them from memory. All other numbers are
big-endian.

### The header object (class `0x57`)

`0x5EF6D0` reads it:

| Field | Size | Data |
|---|---|---|
| Place | 4 × 4 | Node, location, viewpoint, view |
| Objects | 5 × 4 | IDs of the `0x56`, `0x58`, `0x5A`, `0x5B` and `0x5C` objects |
| Disc | 4 | The disc in the drive |
| Mode | 4 | How the engine found the place: 0 from the screen, 1 from a queued move, 2 from the GAM |
| AI list | 4 | The Artificial Intuition action list (`SetAIList`), or 0 |
| AI trigger | 4 | Its trigger, or 0 |
| | 4 | 0 in every save seen |
| | 4 | −1 in every save seen |
| Sounds | blob | The sounds playing: asset reference, time, a flag (1 for a loop) |
| Inventory | blob | The `VCInventory` items held |
| Evidence | blob | Evidence icons on show: icon, flag |
| Kit | blob | The evidence kit's icons (`EnableKitIcon`) |
| Timers | blob | The running timers (`0x5EF3D0`): timer, time, a flag (1 for a periodic timer). The time is what is left of a single timer, or the period |
| Flags | 7 × 1 | Engine settings |
| View media | 4 + 1 + 4 | The view's clip, a flag and its time in ms |
| | 4 | A character view, or 0 |
| | 4 | 0, or one of five layout items |
| | 2 | 1 |

The records in the Sounds, Evidence and Timers blobs are packed in the
shipped engine: 9, 5 and 9 bytes. Saves from the 1998 beta builds pad
them to 12 and 8 bytes, and put the loop flag before the time.

### Photos (class `0x59`)

| Field | Size | Data |
|---|---|---|
| Picture | blob | An 80 × 60 PICT of the photo (the original's hold a JPEG) |
| Media | 4 | The media photographed (`TakePhoto`) |
| Time | 4 | The time in the media, in ms |
| Rectangle | 4 × 2 | The part of the frame photographed: left, top, right, bottom |
| Disc | 1 | The disc the photo was taken on |
| Downloaded | 1 | 1 once the computer has downloaded it |

### Loading a `.x` save

`src/games/xfiles/xsave.c` reads `.x` files, and `game_load_file` and the
`load` script command accept them. The loader:

1. starts a fresh game and runs the title's start-up triggers, which
   make the engine registrations (the original runs them once at
   start-up);
2. applies every variable, enable state and string by GAM ID;
3. restores the lists: questions asked, PDA notes, photos, mail, the
   evidence kit, the AI hint and the running timers;
4. goes to the saved view through its node, location and viewpoint,
   running their entering triggers, as `0x486140` does after a load.

The variable IDs of the 1998 beta saves match the shipped GAM; variables
added later keep their starting value. A file copied as text gains a CR
before every LF; the loader undoes that when every LF has one.

`0x486140` queues the same actions as a move (`act_set_view`), so the
original also loses what the entering triggers reset. View 26186 clears `bAskedCookToCover`: a save made there
after "Cover me" loads without it.

### Writing a `.x` save

`game_save_file` writes a `.x` save for a path ending in `.x`
(`game_save_x_mem`). The engine starts from the shipped GAM, as the
original starts from `Temp.gam`:

1. It writes each variable's value, enable state and string into its
   record. A string that changed gets new data at the end of the file.
2. It appends the header object, the five list objects and one object
   for each photo, from the GAM's next free ID (header field 16).
   The photo pictures are 80 × 60 stills of the media, unpacked
   (`DirectBitsRect`, 32 bits). A photo whose media is not in the game
   folder is grey.
3. It appends an ID index (one leaf node) for each new class, a new
   subclass list for class 1, and a new class index. Header field 8
   points at the new class index; field 12 holds the 11 classes.

Fields the engine does not track are written as in the original saves:
mode 2, the flags `0 0 1 0 0 0 0`, the photo time 0 and the rectangle
(0, 0, 300, 240).

`XF_PLAY_RELOAD=x` makes the play scripts save and load a `.x` file
after every step. Discs 1 to 4 pass this way; disc 5 stops at the
"Cover me" case above.

The menu's Save and Load screens are HDB layouts: 82537 is Save, where
the player types a slot name, and 82634 is Load. Each has three slots.
Their items call these functions:

| Function | What it does |
|---|---|
| `SaveLoadInit` | Reads the slot names |
| `SaveSlotSelect(n)` | Selects slot `n` |
| `SaveKeyPress(n, key)` | Types into the name of slot `n` |
| `SaveLoadComit(0)` | Cancel |
| `SaveLoadComit(1)` | Saves or loads the selected slot |

The Save screen's text cursor (item 82561) follows the typed name, which
holds up to 21 characters. Slot `n` is drawn at y = 166 + 58·n.

Before saving, the engine checks the name. It shows engine string 32
when there is no name, 31 when the slot holds a game, and 38 when
another slot has the same name.

`SaveGameState` takes a checkpoint. The story calls it before dangerous
scenes. After a death, `EndGame` asks "Try again?" (engine string 39).
On yes, the game goes back to the checkpoint; on no, it returns to the
main menu.

## Our save files

`src/games/xfiles/save.c` writes its own format. All numbers are 32-bit
little-endian, so a save works on every platform build. The file starts
with `XFSV` and the version, 1. Chunks follow; each one has four letters,
then the size of its data.

| Chunk | Data |
|---|---|
| `NAME` | The slot name, ending with a zero byte |
| `VIEW` | View, disc |
| `HIST` | Count, then the views behind the Back button |
| `VARS` | Count, then (GAM id, value) for each variable |
| `ENAB` | Count, then (GAM id, 0 or 1) for each enable state |
| `STRS` | Count, then (GAM id, 64 bytes of text) for each string variable |
| `TLOC` | Count, then (trigger, 24 values) for each trigger's parameters |
| `TIME` | Count, then (timer, time left in ms, period) for each running timer |
| `ASKD` | Count, then (conversation, times asked) for each conversation |
| `MAIL` | Count, then (text media, read) for each mail |
| `REGS` | Count, then (16-byte name, count, variables) for each engine registration |
| `PHOT`, `NOTE`, `KITI`, `EVID` | Count, then IDs: photos, PDA notes, evidence kit icons, the lab's evidence bar |
| `PEND` | Count, then the replies on the way (`EmailFlushMess` delivers them) |
| `MISC` | Count, then other engine state: the random seed, the Artificial Intuition hint, the shootout counters, the flashlight, the ambient sound, the PDA notes page, the photos downloaded, the opening credits (time since they started, and the first card) |

Variables and enable states are stored by GAM id. A save therefore loads
into a GAM that has gained or lost entries.

Loading starts from a fresh game and applies each chunk. Then it puts
the player back in the saved view without running entry triggers: the
save already holds what those triggers did. A layout that was open, such
as a close-up, is not saved.

The slots are `slot1.xfs` to `slot3.xfs` in the save folder, and the
file `previous` holds the number of the slot saved or loaded last.
