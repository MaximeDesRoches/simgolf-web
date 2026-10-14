# Native desktop builds

The desktop app runs the same game code as the browser build. The port keeps
each game module at its original 32-bit address (docs/porting.md), so it
can't simply be recompiled as a 64-bit program; instead the game is built as
a plain wasm32 module and translated back to native C with
[wasm2c](https://github.com/WebAssembly/wabt/tree/main/wasm2c), which keeps
that memory layout inside a sandboxed linear memory. A small SDL2 host does
what the page's JavaScript does in the browser.

```
native/guest   the game as golf-host.wasm (wasi-sdk). Same sources as the
               browser's golf target; include/emscripten.h turns every
               EM_JS function into an import from the "host" module, and SDL
               calls become imports too.
native/host    the app: wasm2c output of golf-host.wasm + the host.
  main.cpp       data folder, first run, runs the module
  wasi.cpp       the WASI calls wasi-libc makes (files under /game, /image,
                 /fonts, /assets), stdout/stderr, clock, exit
  imports.cpp    window and scaling, input, timing, page hooks, test script
  audio.cpp      sound_web.js's Web Audio rules on an SDL audio mixer
  importer.cpp   first-run install from the player's disc image + no-CD exe
                 (web/pages/install-worker.js natively: ISO 9660, zip, 7z
                 via third_party/lzma-sdk, unshield)
  setup.cpp      the first-run window
```

The player's files go to the per-user app data folder
(`SDL_GetPrefPath`: `~/Library/Application Support/SimGolf/`,
`%APPDATA%\SimGolf\`): `game/` (the installed game; saves are written
straight into `game/Saved Games`), `image/` (golf.bin, jgld.bin),
`cache/sound-files.txt`, `settings.ini`.

## Building

The game module needs the translated game in `build/gen`
(tools/port/rebuild.sh), so it is built on a machine that has it; the host
only needs the resulting `golf-host.wasm`.

```sh
# the game module (wasi-sdk 34)
cmake -G Ninja -B build/guest -S native/guest \
  -DCMAKE_TOOLCHAIN_FILE=$WASI_SDK/share/cmake/wasi-sdk-p1.cmake -DWASI_SDK_PREFIX=$WASI_SDK
cmake --build build/guest

# the app (SDL2, zlib, wasm2c from WABT 1.0.42)
cmake -G Ninja -B build/native -S native/host -DSIMGOLF_WASM=build/guest/golf-host.wasm
cmake --build build/native
build/native/SimGolf.app/Contents/MacOS/SimGolf --data /tmp/sgdata   # or ./SimGolf
```

Release builds: `tools/native/build-mac.sh` (universal .app with SDL2 linked
in, ad-hoc signed, .dmg) and `.github/workflows/desktop.yml` (macOS and
Windows, from a published `golf-host.wasm`).

Command line: `--data DIR` uses another data folder, `--import FILE...`
installs without the setup window, `--reinstall` shows it again.

## Testing

`SIMGOLF_SCRIPT=file` drives the game: lines of `<ms> click X Y`,
`rclick`, `move`, `key NAME` (SDL key names), `text CHARS`, `shot FILE.bmp`
and `quit`, in game coordinates and ms since start. For example:

```
5000 click 140 445     # Sandbox Mode
8000 click 270 35      # Monterey
30000 shot /tmp/course.bmp
31000 quit
```

## Not done yet

- Intro and closing videos (Bink; the browser build converts them with
  FFmpeg's decoder at install time).
- The HD presentation (hd.c + hd_web.js: native-resolution terrain, xBR,
  sharp text). Its imports are stubbed to "inactive", so frames go through
  SDL's scaler. hd_web.js is WebGL2, so an OpenGL 3.3 port is direct.
- The browser build's save history panel and cross-browser save sync.
