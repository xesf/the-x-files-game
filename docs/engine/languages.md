# Languages

The game shipped in English, French, German, Italian, Spanish and
Japanese. Our engine can hold several of these releases in one game
folder, and it can add translations of the text. The player picks the
voice language and the text language separately.

## What each release translates

All releases share one database: the `XFILES.HDB` of the CD releases
are byte for byte the same, and the DVD's differs in 3 bytes. Only the
media and the text files change.

| Kind | Files | Translated by |
|---|---|---|
| Voices | `XV/*.xmv` (clips), `XS/*` (voice sounds), `vob/*.vob` | Every release dubs its clips |
| Interface text | `XFILEST.DLL`, `XFILESE.DLL`, `XFILESC.DLL` | Every release |
| Documents | `XT/*.xtx` (mails, notes, articles) | Every release, about 385 of 467 |
| Pictures with text | `X.PFF`, `X7.PFF`, `XG/*` | Every release |
| Fonts | `DLG`, `HCD`, `JRN`, `PHN` (`.TTR`, or `.TTF` in the English CD install) | None: the same files everywhere |
| Captions | Text tracks of the English clips | English only |

Only the English clips have captions: the dubbed clips keep 37 English
captions of sound effects and no dialogue. German voices with English
captions therefore take the captions from the English copy of each
clip.

### Japanese text

The Japanese release writes its documents in Shift-JIS (Windows code page
932) and its string tables in Unicode. Its four fonts are the Western
ones, with no Japanese characters: the original drew Japanese with a
Windows font. Our engine:

- reads documents as UTF-8, converting the Japanese folder's from
  Shift-JIS (`xf_sjis_to_utf8`, with a table from
  `tools/text/gen_sjis.py`) and the others from Windows-1252;
- draws every character a game font lacks with a Japanese font: the
  language's own (`font=` in `lang.txt`, or `font.ttf` or `font.ttc` in
  its folder), else the system's (Arial Unicode on macOS, MS Gothic,
  Meiryo or Yu Gothic on Windows, IPA, Takao or Droid fonts on Linux).
  The font must have TrueType outlines (`glyf`); fonts with CFF
  outlines, such as Hiragino and Noto CJK, do not load;
- breaks lines between Japanese characters, never before closing
  punctuation, and reads the full-width colon of mail headers.

The Vita and DOS have no such system font: put a TrueType font with
Japanese characters in the `jp` folder as `font.ttf`, for example
IPAexGothic (IPA Font License, free to redistribute).

## The folder layout

Put each release in a folder named by its language code, inside the
game folder:

```
game/
  en/    the English release (XFILES.HDB, XFILEST.DLL, XV/, XT/, ...)
  de/    the German release
  fr/    ...
  pt/    a translation: text only
```

The game folder then holds no `XFILES.HDB` of its own. Language folders
are named by a code of 2 or 3 letters, maybe with a region (`pt-BR`);
other folders, such as `media` and `saves`, stay what they are. A folder counts
as a language when it holds a release (`XFILES.HDB`), clips (`XV`), or
text (`XFILEST.DLL`, `xfilest.txt`, `XT` or `lang.txt`). The game's
database comes from the English release, else the first one.

Each file comes from the chosen language for its kind: voices for
`XV/`, `XS/` and `vob/`, text for everything else. When that language
lacks the file, it comes from the language's fallback, then English.

## Choosing

When the game folder holds several languages, the Options screen has a
fourth tab, **Language**, under Game Play. Its panel, drawn like the
Audio panel, has two rows: **Voices** and **Text**; the arrows at the
ends of a row choose the previous or next language that has them. The choice is saved with the preferences. The
environment variables `XF_VOICES` and `XF_TEXT` (a folder name such as
`de`) override it.

## Adding a translation

A translation is a folder of text files. Anything it leaves out comes
from its fallback language (English unless `lang.txt` says otherwise).

| File | Content |
|---|---|
| `lang.txt` | `name=Português` (shown on the Options screen), `fallback=en`, `encoding=shift-jis` for Shift-JIS documents, `font=` a font file for characters the game fonts lack, and the words drawn over pictures the translation has no copy of: `talk=FALAR`, `history=HISTÓRICO` (the dialogue folder's tabs) |
| `xfilest.txt`, `xfilese.txt`, `xfilesc.txt` | One string a line: `id<TAB>text`, UTF-8, `\n` for a line break |
| `xt/<id>.xtx` | Documents, UTF-8 |
| `xv/<id>.srt` | Captions of the clip `XV/<id>.xmv`, SRT, UTF-8 |

Translations may use the characters of Windows-1252, which covers the
Western European languages.

To start a translation:

1. Export the English text:

   ```sh
   xftool lang-export game/en game/pt
   ```

   This writes the three string tables, the 467 documents and the
   captions of 1,153 clips (about 53,000 words).
2. Write `game/pt/lang.txt`.
3. Translate the files in place. Keep the ids, the caption numbers and
   their times.
4. Check the translation against the release:

   ```sh
   xftool lang-check game/en game/pt
   ```

   It reports strings whose `%` placeholders changed, documents whose
   header lines changed in number, and captions whose number of lines or
   times changed.

Text the game builds from strings follows the text language too:

- The location card takes its place name from a string, and writes the
  date and time the way the language does ("Terça-feira, 2 de abril de
  1996", 24-hour time). The original took them from the Windows locale.
- The names shown when the pointer rolls over an emotion or action icon,
  an inventory item or a piece of evidence are strings (the object's
  fifth field refers to them), not the writers' object names.

Some strings are words the player searches for in the ING and the media
search. A search matches the translated word and the release's own
word, so either works.

Mails start with three header lines in the order To, From, Subject. A
translation can name them in its language (`Para:`, `De:`,
`Assunto:`): the mail list reads them by position.
