// SimGolf desktop app: finds (or sets up) the player's game files, then runs
// the game module. Usage: SimGolf [--data DIR] [--import FILE...] [--reinstall]

#include <SDL.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "host.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern "C" int simgolf_run(w2c_golf *inst);   // run.c

namespace fs = std::filesystem;

Paths g_paths;
wasm_rt_memory_t *g_mem;
extern std::string g_resources;

static std::string with_slash(std::string s) {
  if (!s.empty() && s.back() != '/' && s.back() != '\\') s += '/';
  return s;
}

// sound.cpp matches the game's sound paths against this list of WAVs.
static void write_sound_list() {
  std::ofstream out(upath(g_paths.cache + "/sound-files.txt"));
  std::error_code ec;
  fs::path root = upath(g_paths.game);
  for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    auto ext = it->path().extension().u8string();
    std::string e(ext.begin(), ext.end());
    for (char &c : e) c = (char)tolower((unsigned char)c);
    if (e != ".wav") continue;
    auto rel = fs::relative(it->path(), root, ec).generic_u8string();
    out << std::string(rel.begin(), rel.end()) << "\n";
  }
}

static void progress_to_stderr(void *, float f, const std::string &text) {
  fprintf(stderr, "[%3d%%] %s\n", (int)(f * 100), text.c_str());
}

static int fail(const std::string &msg) {
  fprintf(stderr, "%s\n", msg.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SimGolf", msg.c_str(), nullptr);
  return 1;
}

int main(int argc, char **argv) {
  std::string data_dir;
  std::vector<std::string> import_files;
  bool reinstall = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--data" && i + 1 < argc) data_dir = argv[++i];
    else if (a == "--reinstall") reinstall = true;
    else if (a == "--import") {
      while (i + 1 < argc && argv[i + 1][0] != '-') import_files.push_back(argv[++i]);
    } else if (a.rfind("-psn_", 0) == 0) {
      // macOS Finder launch argument
    } else {
      fprintf(stderr, "usage: %s [--data DIR] [--import FILE...] [--reinstall]\n", argv[0]);
      return 2;
    }
  }

  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) return fail(SDL_GetError());

  if (data_dir.empty()) {
    char *pref = SDL_GetPrefPath(nullptr, "SimGolf");
    if (!pref) return fail(std::string("No app data folder: ") + SDL_GetError());
    data_dir = pref;
    SDL_free(pref);
  }
  g_paths.data = with_slash(data_dir);
  g_paths.game = g_paths.data + "game";
  g_paths.image = g_paths.data + "image/";
  g_paths.cache = g_paths.data + "cache/";
  if (char *base = SDL_GetBasePath()) {
    g_resources = with_slash(base);
    SDL_free(base);
  }
  // A development build runs from build/native: its resources sit next to it.
  if (!fs::exists(upath(g_resources + "fonts"))) g_resources = with_slash(g_resources + "resources");
  g_paths.fonts = g_resources + "fonts";
  std::error_code ec;
  fs::create_directories(upath(g_paths.data), ec);
  // Started from Finder or Explorer: keep the log in the data folder.
#ifdef _WIN32
  if (!_isatty(2)) {
    _wfreopen(upath(g_paths.data + "simgolf.log").c_str(), L"w", stderr);
    _wfreopen(upath(g_paths.data + "simgolf.log").c_str(), L"a", stdout);
  }
#else
  if (!isatty(2)) {
    freopen((g_paths.data + "simgolf.log").c_str(), "w", stderr);
    freopen((g_paths.data + "simgolf.log").c_str(), "a", stdout);
  }
#endif
  setvbuf(stderr, nullptr, _IONBF, 0);
  load_options();

  if (!import_files.empty()) {
    bool missing;
    std::string err = import_game(import_files, progress_to_stderr, nullptr, &missing);
    if (!err.empty()) return fail(err);
  } else if (reinstall || !game_installed()) {
    if (!run_setup(g_resources)) return 0;
  }
  // The game prints its paths with a trailing slash in places; keep /game bare.
  g_paths.image = g_paths.data + "image";
  g_paths.cache = g_paths.data + "cache";
  fs::create_directories(upath(g_paths.cache), ec);
  write_sound_list();
  load_script();
  wasi_init();

  wasm_rt_init();
  static w2c_golf inst;
  static w2c_env env;
  static w2c_host host;
  static w2c_wasi__snapshot__preview1 wasi;
  wasm2c_golf_instantiate(&inst, &env, &host, &wasi);
  g_mem = w2c_golf_memory(&inst);

  int code = 0;
  wasm_rt_trap_t trap = (wasm_rt_trap_t)simgolf_run(&inst);
  if (trap == WASM_RT_TRAP_NONE) {
  } else if (g_exiting) {
    code = g_exit_code;
  } else {
    std::string msg = std::string("SimGolf stopped unexpectedly (") + wasm_rt_strerror(trap) +
                      "). Your last autosave is in the Load Game menu.";
    fail(msg);
    code = 1;
  }
  audio_shutdown();
  wasm2c_golf_free(&inst);
  wasm_rt_free();
  SDL_Quit();
  return code;
}
