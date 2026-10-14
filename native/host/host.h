// Native desktop host for the game (native/README.md): runs golf-host.wasm,
// translated to C by wasm2c, with SDL2 for the window, input and sound, and a
// small WASI layer over the app's data folder for files.

#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "golf.h"
}

// wasm2c passes these to imports; the host keeps its state in globals.
struct w2c_env { int unused; };
struct w2c_host { int unused; };
struct w2c_wasi__snapshot__preview1 { int unused; };

// UTF-8 strings <-> paths (std::filesystem converts to UTF-16 on Windows).
inline std::filesystem::path upath(const std::string &s) {
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
  return std::filesystem::u8path(s);
#endif
}
inline std::string path_str(const std::filesystem::path &p) {
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// ------------------------------------------------------------------ guest memory

extern wasm_rt_memory_t *g_mem;

inline bool mem_ok(uint32_t p, uint64_t n) { return (uint64_t)p + n <= g_mem->size; }
inline uint8_t *mem(uint32_t p) { return g_mem->data + p; }
inline uint32_t mem_u32(uint32_t p) { uint32_t v; memcpy(&v, mem(p), 4); return v; }
inline void mem_put_u32(uint32_t p, uint32_t v) { memcpy(mem(p), &v, 4); }
inline void mem_put_u64(uint32_t p, uint64_t v) { memcpy(mem(p), &v, 8); }
std::string mem_str(uint32_t p);

// ------------------------------------------------------------------ folders

struct Paths {
  std::string data;    // per-user app data (SDL_GetPrefPath), ends with '/'
  std::string game;    // data/game: the installed game, saves included
  std::string image;   // data/image: golf.bin, jgld.bin
  std::string cache;   // data/cache: files the host writes for the guest
  std::string fonts;   // bundled fonts (app resources)
};
extern Paths g_paths;

// Maps a guest path ("/game/...", "assets/...") to a native one; "" if none.
std::string guest_to_native(const std::string &path);

// WASI: preopens /game, /image, /fonts and /assets (= cache).
void wasi_init();

// Ends the guest (WinMain returned, exit(), or the window was closed): traps
// out of the wasm call with g_exiting set, so main can tell it from a crash.
[[noreturn]] void guest_exit(int code);
extern bool g_exiting;
extern int g_exit_code;

// ------------------------------------------------------------------ video and input (imports.cpp)

struct HostOptions {
  int screen_w = 800;        // 800, or wider for the opt-in widescreen mode
  bool edge_scroll = true;
  bool linear = true;        // scale filter
  bool fullscreen = false;
};
extern HostOptions g_opts;
void load_options();
void load_script();   // SIMGOLF_SCRIPT, for smoke tests
void save_options();

// ------------------------------------------------------------------ sound (audio.cpp)

void audio_init();
void audio_register(int index, const std::string &native_path, bool single);
void audio_play(int index, int volume, int pan, int cents, int delay_ms);
void audio_fade_out(int index, int ms);
void audio_stop(int index);
void audio_shutdown();
void audio_set_master(float gain);

// ------------------------------------------------------------------ first run (setup.cpp, importer.cpp)

// Writes data/game, data/image and data/installed.txt from the player's
// files. progress(fraction, text) is called as it goes; returns "" on
// success or an error message. missing is set when one of the two inputs is
// still needed.
std::string import_game(const std::vector<std::string> &files,
                        void (*progress)(void *ctx, float, const std::string &), void *ctx,
                        bool *missing);
bool game_installed();
// Shows the setup window until the game is installed. Returns false if the
// player closed it.
bool run_setup(const std::string &resources);
