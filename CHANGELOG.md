# Changelog

All notable changes to this project.

## Unreleased

- Wide screens: the scene sits higher (smaller on very wide screens) so the emotion icons never cover it.
- Every game ending the data can reach has a play script: 26 of 28 (one is orphaned data, one needs a missing clip).
- View clips that play once have their sound and captions, and run their end triggers when they end.
- Random chances differ from run to run and across Try again, as in the original.
- Smolnikoff's barricade: a goon who misses no longer kills Willmore.
- Play scripts for eight more game endings; `xftool hdb-up` shows what leads to any object.
- Scully's PowerBook login works, and a refused computer login shows its message, including the four hidden ones.
- Layout text uses the original pixel sizes: messages and mails fit their boxes.
- The DVD edition's `.vob` clips play: an MPEG-2 video decoder, the DVD's LPCM sound, and its opening titles.
- Keyboard and d-pad navigation: arrows, Tab, Enter and I; on the Vita, the d-pad, R, L, Square and Triangle.
- A `media` folder in the game folder for the CD's clips, for installs that only have the DVD's `.vob` files.
- The computer's login connects with Enter, and Tab moves to the password.
- The phone stops ringing once a call is answered.
- The opening credits and their music start together, when Cook leaves after the answer clip.
- Captions show under the emotion icons, not behind them.
- Close-up pictures are no longer cut at the bottom.

## 0.1.0 - 2026-10-16

The first release: the whole game plays from the original game files.

### Engine and data

- NeoAccess reader for `XFILES.HDB` and `XFILES.GAM`, with the field layout of every game class.
- Action interpreter: trigger kinds, expressions, parameters kept per trigger, and the engine functions the game data calls.
- Scene graph: nodes, locations, viewpoints and views, with their entering and leaving triggers.
- Game text from the `xfilest.dll`, `xfilese.dll` and `xfilesc.dll` string tables.

### Media

- QuickTime movies: Cinepak, Photo JPEG, Motion JPEG A and Apple Video (rpza) pictures.
- Sound: IMA ADPCM, PCM and QDesign Music 1 (QDMC), with a software mixer.
- Subtitles from text tracks, PICT pictures and PFF bundles.

### Gameplay

- The whole story plays, from the intro to the end credits, on all seven discs.
- Conversations, emotion and action icons, inventory, evidence and the lab's evidence bar.
- Travel, location cards, timers, the flashlight, night vision, binoculars and fades.
- Gunfights, the bomb, the isolation chamber, deaths and Try again from a checkpoint.
- The cell phone, the ING database, mail (reading and writing), photos, the Media Index, the PDA notes and address book, the journal and the Bible.
- Artificial Intuition hints, with the spinning icon in the top-right corner.
- Opening credits: the cards from `xfilesc.dll`, with the credits music.

### Interface

- Main menu with its animations: the background, the centre loop, the item rollovers and the title's rollover.
- The menu during a game: Escape or the X icon, the game holds, Return resumes.
- Options screen: game play, graphics and audio settings, saved between runs; Closed Captioning can be turned on.
- The game's own cursors, with the white cross as the default pointer.
- Wide layout: the scene fills 16:9, 16:10 and ultrawide screens, the interface goes around it.

### Saved games

- Save and Load screens with three slots, Previous, and our own save format.
- Load and write the original engine's `.x` saves, including the 1998 beta saves.

### Platforms

- macOS, Linux (Steam Deck), Windows, Windows 9x, DOS and PlayStation Vita builds.

### Tools and tests

- `xftool`: inspect HDB objects, movies, pictures and sounds, and play the story from scripts.
- Play scripts for every disc and for the strategy guide's side routes, run headless.
- Window scripts for the menu and the opening credits.
