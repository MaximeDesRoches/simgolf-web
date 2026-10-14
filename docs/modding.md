# Mods

A mod is a set of files laid out like the game folder. The game reads a mod's
file in place of its own when both exist, and sees the mod's extra files in
folder listings. Mods never contain the game's own data; players install the
game from their own copy as usual.

What works today with no engine changes, because the original game discovers
these by listing folders:

| Content | Where it goes | Format |
|---|---|---|
| Theme packs (the main menu's **Select A Theme**) | `Themes/<Theme name>/` | |
| Club regulars | `Themes/<theme>/*.chr` | `formats/golfers.md` |
| Extra touring pros | `Themes/<theme>/*.pro` | `formats/golfers.md` |
| The theme's club pro | `Themes/<theme>/Joe Pro.glf` | `formats/golfers.md` |
| Pro golfer and celebrity lists | `Themes/<theme>/progolfers.dta`, `celebrities.dta` (the game falls back to Standard's) | `formats/golfers.md` §4–5 |
| Stories | `Themes/<theme>/<8-letter filter><Title>.txt` | `formats/text.md` §2 |
| Course lighting | `<Course>Lighting.txt` | `formats/text.md` §1 |
| Terrain help text | `Interface/<course>.txt` | `formats/text.md` §3 |
| Saved courses | `Saved Games/*.sve` | `formats/sve.md` |

Any other file the game opens (interface `.pcx`, terrain textures, `.flc`
sprites) is replaced the same way, but there are no tools yet to make those.
Sounds aren't covered: the browser build fetches them in JavaScript
(`sound_web.js`), outside the game's file layer.

## Package

```
seaside-club.zip
  mod.json          {"id", "name", "version", "author", "description", "game": "simgolf-1.02"}
  files/            laid out like the game folder
    Themes/Seaside Club/Captain Reed.chr
    ...
```

`id` names the mod's folder (a mod with the same id replaces it). A zip with
no `mod.json` is taken as a bare game-folder tree (`Themes/...`), the way the
original theme packs were distributed.

`tools/mods/pack.py <source> <zip>` builds the zip from a source folder like
`mods/seaside-club`. It turns `<name>.chr.json` (and `.pro.json`, `.glf.json`)
into golfer files with `tools/mods/chr.py`, and writes `.txt`/`.dta` as the
game reads them (Windows-1252, CRLF). `tools/mods/chr.py decode <file>` turns
an existing golfer file into that JSON, which is the easiest way to start a
character.

```sh
python3 tools/mods/pack.py mods/seaside-club build/mods/seaside-club.zip
```

## Installing

- **Browser:** Saves panel (Alt+S / Option+S, or the Saves button) → Mods → Add mod
  (.zip). Mods can be switched off, reordered (later ones win when two change
  the same file) or removed there; changes apply after a reload.
  `web/mods.js` keeps them in the origin private file system under
  `simgolf-mods/`, apart from the installed game, and copies the enabled ones
  into the game's file system before it starts.
- **Desktop app:** unzip into the `mods` folder of the app data folder
  (`~/Library/Application Support/SimGolf/mods/`, `%APPDATA%\SimGolf\mods\`),
  one folder per mod. Every folder is loaded, in name order, unless
  `mods/order.txt` lists the ones to load (one per line, later ones winning).

## How it works

`src/port/fs.c`, shared by both builds. At start `mods_init` reads
`/mods/order.txt` (or every folder in `/mods`) and indexes each mod's
`files/` folder (or the mod folder itself when it has no `files/`) by
lowercased game path, later mods replacing earlier ones' entries.
`fs_resolve` returns the mod's copy for reads; writes always go to the game
folder. `fs_list` adds the mods' files and folders to listings, after the
game's own.

`fs_list` also reports `.` and `..` first in a subfolder, as Windows does: the
theme screen (`FUN_004725b0`) skips the first entry of `Themes\*.*` on that
assumption, so without them the first real folder in the listing was
skipped (that's how a theme pack from a mod first went missing).

## Not done

- Tools for art: PNG to PCX (with `_A` masks) and to Firaxis FLC sprite sheets.
- Sound replacement (needs `sound_web.js` and the native audio to look in mods).
- Tuning game data (prices, economy) and scripting: see the tiers in the
  project's modding write-up.
- Saves don't record which mods were on.
