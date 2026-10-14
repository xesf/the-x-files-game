# VirtualCinema classes

The engine keeps its game model in persistent objects of 52 classes. The
class names are stored in the game executable and match the names used in
`XFILES.HDB`. At startup the engine registers 51 of them with the database
layer, each with a one-byte class ID and a factory function that creates an
empty object. `VCDbIdList` is named but registered separately.

The class ID is what the database stores to tell objects apart. The
descriptions below are short working notes from the names and will be
expanded as each class is reverse-engineered.

| ID | Class | Working description |
|---|---|---|
| `0x27` | `VCTitle` | Title screen |
| `0x28` | `VCNode` | A node: one place the player can stand |
| `0x29` | `VCLocaton` | A location (the misspelling is in the game data) |
| `0x2A` | `VCViewPoint` | A viewpoint inside a node |
| `0x2B` | `VCView` | A view (camera direction) |
| `0x2C` | `VCCharacter` | A character |
| `0x2D` | `VCCharView` | A view of a character |
| `0x2E` | `VCCharViewList` | List of character views |
| `0x2F` | `VCName` | A name string |
| `0x31` | `VCConversation` | A conversation with a character |
| `0x32` | `VCConversationList` | List of conversations |
| `0x33` | `VCNav` | A navigation move between viewpoints |
| `0x34` | `VCNav_List` | List of navigation moves |
| `0x35` | `VCAssetRef` | Reference to a media file |
| `0x36` | `VCAssetRefList` | List of media references |
| `0x37` | `VCExplorable` | Something the player can examine |
| `0x38` | `VCExplorableList` | List of explorables |
| `0x39` | `VCHotSpot` | A clickable area |
| `0x3A` | `VCHotSpotList` | List of hotspots |
| `0x3B` | `VCStdHotSpotList` | Standard hotspot list |
| `0x3C` | `VCAssetCategory` | Media category |
| `0x3D` | `VCCursor` | A mouse cursor |
| `0x3E` | `VCDefaultCursors` | Default cursor set |
| `0x40` | `VCDefaultHotSpots` | Default hotspot set |
| `0x41` | `VCAction` | An action run by a hotspot or trigger |
| `0x42` | `VCActionList` | List of actions |
| `0x43` | `VCBlob` | Raw data block |
| `0x44` | `VCPlayer` | The player |
| `0x46` | `VCEnabled` | Enabled state |
| `0x47` | `VCActionIcon` | Action icon |
| `0x48` | `VCEmotionIcon` | Emotion icon in conversations |
| `0x49` | `VCEvidenceIcon` | Evidence icon |
| `0x4A` | `VCIdeaIcon` | Idea icon |
| `0x4B` | `VCInventory` | Inventory |
| `0x4C` | `VCIdeaMap` | Idea map |
| `0x4D` | `VCIdeaMapList` | List of idea maps |
| `0x4E` | `VCIFaceLayout` | Interface layout |
| `0x4F` | `VCInterfaceItem` | Interface element |
| `0x50` | `VCString` | A text string |
| `0x51` | `VCTrigger` | A trigger: a condition that runs actions |
| `0x52` | `VCTriggerList` | List of triggers |
| `0x53` | `VCVariable` | A game variable |
| `0x54` | `VCStdAction` | Standard action |
| `0x55` | `VCStdActionList` | List of standard actions |
| `0x56` | `VCConversationHistory` | What has been said in conversations |
| `0x57` | `VCGameState` | Saved game state |
| `0x58` | `VCPDANotes` | PDA notes |
| `0x59` | `VCPhoto` | A photo |
| `0x5A` | `VCEmail` | An e-mail |
| `0x5B` | `VCEmailRead` | A read e-mail |
| `0x5C` | `VCEmailPending` | An e-mail not yet delivered |

## How objects are read

Each class reads itself from a database stream in a fixed field order.
The stream writes no field tags, so the order of the reads is the file
format. Every object starts with a common header:

| Size | Content |
|---|---|
| 2 | Object flags |
| 4 | Object version, present only when the database format stores versions |

The class-specific fields follow. For example, `VCTrigger` reads one 4-byte
value and then one byte. Numbers are big-endian, because the database
library came from the Macintosh.

Lists inside an object (a trigger's actions, for example) are not stored
in the object itself. They are linked through the database indexes.

## Tools

```sh
python3 -I tools/re/vc_classes.py path/to/xfiles.exe   # class table from the executable
```
