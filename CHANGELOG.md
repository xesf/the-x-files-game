# Changelog

All notable changes to this project.

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
