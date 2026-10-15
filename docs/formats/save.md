# Saved games

## The original `.x` files

The original engine keeps the running state in `Temp.gam`, a copy of
`XFiles.gam` made at start-up. A save is that NeoAccess database written
out with the extension `.x`, plus extra objects:

- class 0x57, the header: node, location, viewpoint and view, with
  references to the other save objects;
- classes 0x56 and 0x58 to 0x5C: state that the engine holds outside the
  GAM.

`0x4D14B0` writes a save. Our engine cannot read `.x` files yet.

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

Before saving, the engine checks the name with engine strings 31, 32 and
38: no name, a slot that is not empty, and a name that another slot
already has.

`SaveGameState` takes a checkpoint. The story calls it before dangerous
scenes. After a death, `EndGame` asks "Try again?" (engine string 39).
On yes, the game goes back to the checkpoint; on no, it returns to the
main menu.

## Our save files

`src/game/save.c` writes its own format. All numbers are 32-bit
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
| `MISC` | Count, then other engine state: the random seed, the Artificial Intuition hint, the shootout counters, the flashlight, the ambient sound |

Variables and enable states are stored by GAM id. A save therefore loads
into a GAM that has gained or lost entries.

Loading starts from a fresh game and applies each chunk. Then it puts
the player back in the saved view without running entry triggers: the
save already holds what those triggers did. A layout that was open, such
as a close-up, is not saved.

The slots are `slot1.xfs` to `slot3.xfs` in the save folder, and the
file `previous` holds the number of the slot saved or loaded last.
