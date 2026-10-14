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
               calls become imports too. video.c replaces src/port/video.c:
               it decodes the disc's .bik with FFmpeg's Bink decoder
               (tools/native/build-ffmpeg-wasi.sh) and passes frames and
               audio to the host.
native/host    the app: wasm2c output of golf-host.wasm + the host.
  main.cpp       data folder, first run, runs the module
  wasi.cpp       the WASI calls wasi-libc makes (files under /game, /image,
                 /fonts, /assets), stdout/stderr, clock, exit
  imports.cpp    window, input, timing, page hooks, video frames, test script
  present_gl.cpp hd_web.js in OpenGL 3.3: the frame through xBR/linear/
                 nearest, terrain_hd.cpp's triangles at full resolution,
                 TextOut text redrawn with stb_truetype; SDL_Renderer when
                 there's no GL
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
`cache/sound-files.txt`, `settings.ini`, and `mods/` (one folder per mod,
mapped to `/mods` in the game; docs/modding.md).

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
(the game's frame), `hdshot FILE.bmp` (the window's output) and `quit`, in
game coordinates and ms since start. `SIMGOLF_WINDOW=WxH` sets the window
size, `SIMGOLF_NO_VIDEOS=1` skips the videos, `SIMGOLF_NO_GL=1` forces
SDL_Renderer. For example:

```
5000 click 140 445     # Sandbox Mode
8000 click 270 35      # Monterey
30000 shot /tmp/course.bmp
31000 quit
```

## Not done yet

- The Windows build compiles (CI and `tools/native/build-windows.sh`) but
  has never been run on Windows.
- The intro plays before the game loads; the browser plays it over the
  loading.
- The browser build's save history panel and cross-browser save sync.
