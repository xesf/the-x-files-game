# Conversations and atoms

How a character answers: the questions of the dialogue box, the replies
to questions asked again, and the replies to items in Willmore's hand.
The addresses are in the English DVD `xfiles.exe`; the IDs are from the
English HDB.

## Conversations

A `VCConversation` (class `0x31`, read by `0x5F6720`) has these fields:

| Field | Class | Content |
|---|---|---|
| name | `VCName` | The question as text in the dialogue box |
| icons | `VCAssetRefList` | Its icon in the dialogue box |
| answer | `VCAssetRefList` | The character's answer |
| triggers | `VCTriggerList` | Kind 0 on asking, 8 when it starts, 9 when the answer ends |
| question | `VCAssetRefList` | Willmore asking |
| atoms | `VCAssetRefList` | Stock replies, see below |
| flag | byte | |

From a media list, the first enabled entry plays.

The first time, a question plays Willmore asking, then the answer.
`0x4ED930` counts each question in the conversation history, which the
History tab lists and saves keep (`VCConversationHistory`).

## Atoms

Atoms are a character's short stock replies, filmed once per character:
"I just told you", "Don't do that" and so on. The engine strings them
around the answers so that a question asked again, or an item pointed at
someone, gets a fitting reply without a clip for every case.

A character view that has atoms lists a placeholder conversation first in
its conversation list. The placeholder is disabled, so it is never on
offer, and its atom list holds seven slots in a fixed order:

| Slot | Reply |
|---|---|
| 0 | "I just told you" |
| 1 | "As I just said" |
| 2 | "What's the matter with you" |
| 3 | "What are you doing" |
| 4 | "I don't want that" |
| 5 | "Don't do that" |
| 6 | "I don't know anything about that" |

A slot the character has no clip for holds a text asset instead (67221,
"Dummy Text for Atoms"), which counts as empty.

`0x43CFEB` reads the slots into the character view (`+0x13C` to
`+0x154`). It adds two end-game clips as slots 7 and 8: 19656, Willmore
in prison, and 19654, Willmore out of the FBI. It also sets `+0x160`, the
flag that `0x4907F0` reads to tell whether a character view has atoms.

Cook at his desk (character views 21993 and 85696) has placeholder
conversation 67223 with atom list 67224: all slots but slot 1.

### Items in hand

An item has a value, its `Inventory_i` constant: badge 1, binoculars 2,
gun 9, handcuffs 10, flashlight 8, night vision goggles 13, and so on.
With an item in hand, the character's own triggers come first. When a
trigger of the event's kind tests for the item in hand (`iCurrInventory
== x`, with `x` that item), the triggers answer it and no atom plays.
Authors use such a test to keep a character from answering an item.

**Pointing** (`0x442B70`, the table at `0x4EDF80`): the pointer onto the
character runs its kind 4 triggers. The gun, the handcuffs (10) and the
stiletto (23) get slot 5, "Don't do that".

**Clicking** (`0x435C00`, the table at `0x490520`, indexed by item − 1
from `0x4906C0`):

| Items | Reply |
|---|---|
| 1, 4, 6, 7, 11, 12, 21, 22, 25, 26, 30, 31 | Slot 4, "I don't want that" |
| 9 (gun), 23 (stiletto) | Slot 7: prison, the game ends |
| 10 (handcuffs) | Slot 8: out of the FBI, the game ends |
| 16 to 20 (the photos) | Slot 6, "I don't know anything about that" |
| 5 (cattle prod) | Slot 3 or slot 5, at random when the character has both |
| The others | None |

The end-game clips end the game through their own triggers.

**Taking an item in hand** (`0x518C80` calls `0x5192B0`): the binoculars
(2) and the flashlight (8) get slot 2, "What's the matter with you", and
the night vision goggles (13) get slot 3, "What are you doing". The first
enabled character in the view with that atom says it.

### Questions asked again

A question asked again plays Willmore asking, then a reply that
`0x4D9890` and `0x4DA090` vary with rolls of 1 to 10. A conversation's
own atom list holds at most one atom: its "Yes" or "No".

1. From the fourth time on, the character may not answer again. The
   chance is 5 in 10 the fourth time, then 6, 7 and 8 in 10. A question
   gets "I just told you", followed 2 times in 10 by "What's the matter
   with you". An idea gets "What's the matter with you".
2. Otherwise, when the conversation has a yes or no atom, 8 times in 10
   that atom plays, followed 3 times in 10 by the answer.
3. Otherwise, 6 times in 10: "I just told you" or "As I just said" (half
   each for a question, always the second for an idea), 2 times in 10
   "What's the matter with you", then the answer (not for an idea).
4. Otherwise the answer.

An idea is a conversation that a `VCIdeaMap` of the character view
leads to. A character without atoms plays the conversation's first atom,
then the answer.

The engine does all of this in `src/games/xfiles/game.c`, in the section
"Atoms".
