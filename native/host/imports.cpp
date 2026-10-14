// What the page's JavaScript and Emscripten's SDL do for the browser build,
// done natively: the window and its scaling, input events, the clock, frame
// pacing, sound and the few page hooks (src/port/*.c's EM_JS functions).

#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

#include "host.h"

HostOptions g_opts;
bool g_exiting;
int g_exit_code;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static int g_w = 800, g_h = 600;
static uint32_t g_frame_ptr, g_frame_pitch;   // the last frame, for test screenshots

void guest_exit(int code) {
  g_exiting = true;
  g_exit_code = code;
  wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
}

std::string mem_str(uint32_t p) {
  std::string s;
  while (p && mem_ok(p, 1) && *mem(p)) s += (char)*mem(p++);
  return s;
}

// ------------------------------------------------------------------ options

static std::string options_path() { return g_paths.data + "settings.ini"; }

void load_options() {
  std::ifstream in(options_path());
  std::string line;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (k == "screen_width") g_opts.screen_w = atoi(v.c_str());
    else if (k == "edge_scroll") g_opts.edge_scroll = v != "0";
    else if (k == "smooth_scaling") g_opts.linear = v != "0";
    else if (k == "fullscreen") g_opts.fullscreen = v == "1";
  }
  if (g_opts.screen_w < 800 || g_opts.screen_w > 1280) g_opts.screen_w = 800;
  g_opts.screen_w &= ~1;
}

void save_options() {
  std::ofstream out(options_path());
  out << "# SimGolf settings. screen_width: 800 for 4:3, up to 1280 for widescreen (1066 = 16:9).\n"
      << "screen_width=" << g_opts.screen_w << "\n"
      << "edge_scroll=" << (g_opts.edge_scroll ? 1 : 0) << "\n"
      << "smooth_scaling=" << (g_opts.linear ? 1 : 0) << "\n"
      << "fullscreen=" << (g_opts.fullscreen ? 1 : 0) << "\n";
}

// ------------------------------------------------------------------ window

static void apply_fullscreen() {
  SDL_SetWindowFullscreen(g_window, g_opts.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void apply_filter() {
  if (!g_renderer) return;
  if (g_texture) SDL_DestroyTexture(g_texture);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_opts.linear ? "1" : "0");
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, g_w, g_h);
}

// The largest whole multiple of the game's size that fits the screen.
static int initial_scale(int w, int h) {
  SDL_Rect r;
  if (SDL_GetDisplayUsableBounds(0, &r) != 0) return 1;
  int s = 1;
  while (w * (s + 1) <= r.w * 9 / 10 && h * (s + 1) <= r.h * 9 / 10) s++;
  return s;
}

extern "C" u32 w2c_env_SDL_Init(struct w2c_env *, u32) { return 0; }
extern "C" void w2c_env_SDL_StartTextInput(struct w2c_env *) { SDL_StartTextInput(); }
extern "C" void w2c_env_SDL_StopTextInput(struct w2c_env *) { SDL_StopTextInput(); }
extern "C" u32 w2c_env_SDL_ShowCursor(struct w2c_env *, u32 toggle) { return (u32)SDL_ShowCursor((int)toggle); }

extern "C" u32 w2c_env_SDL_CreateWindow(struct w2c_env *, u32 title, u32 x, u32 y, u32 w, u32 h, u32 flags) {
  (void)x; (void)y; (void)flags; (void)title;
  g_w = (int)w;
  g_h = (int)h;
  int s = initial_scale(g_w, g_h);
  g_window = SDL_CreateWindow("SimGolf", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_w * s, g_h * s,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!g_window) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    guest_exit(1);
  }
  SDL_SetWindowMinimumSize(g_window, g_w, g_h);
  apply_fullscreen();
  return 1;
}

extern "C" u32 w2c_env_SDL_CreateRenderer(struct w2c_env *, u32 window, u32 index, u32 flags) {
  (void)window; (void)index; (void)flags;
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, 0);
  SDL_RenderSetLogicalSize(g_renderer, g_w, g_h);
  return 1;
}

extern "C" u32 w2c_env_SDL_CreateTexture(struct w2c_env *, u32 r, u32 format, u32 access, u32 w, u32 h) {
  (void)r; (void)format; (void)access; (void)w; (void)h;
  apply_filter();
  return 1;
}

extern "C" u32 w2c_env_SDL_UpdateTexture(struct w2c_env *, u32 tex, u32 rect, u32 pixels, u32 pitch) {
  (void)tex; (void)rect;
  if (!mem_ok(pixels, (uint64_t)pitch * g_h)) return (u32)-1;
  g_frame_ptr = pixels;
  g_frame_pitch = pitch;
  return (u32)SDL_UpdateTexture(g_texture, nullptr, mem(pixels), (int)pitch);
}

extern "C" u32 w2c_env_SDL_RenderClear(struct w2c_env *, u32) {
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  return (u32)SDL_RenderClear(g_renderer);
}

extern "C" u32 w2c_env_SDL_RenderCopy(struct w2c_env *, u32, u32, u32, u32) {
  return (u32)SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
}

extern "C" void w2c_env_SDL_RenderPresent(struct w2c_env *, u32) { SDL_RenderPresent(g_renderer); }

// ------------------------------------------------------------------ test script
// SIMGOLF_SCRIPT=<file> drives the game for smoke tests, one command per line
// at a time in ms since start: "<ms> click X Y", "<ms> rclick X Y",
// "<ms> move X Y", "<ms> key NAME" (an SDL key name), "<ms> text CHARS",
// "<ms> shot FILE" (the frame as a .bmp) and "<ms> quit". X and Y are game
// coordinates.

struct ScriptLine { double t; std::string cmd, a; int x = 0, y = 0; };
static std::vector<ScriptLine> g_script;
static size_t g_script_pos;
static std::vector<SDL_Event> g_injected;

static void save_frame(const std::string &file) {
  if (!g_frame_ptr || !mem_ok(g_frame_ptr, (uint64_t)g_frame_pitch * g_h)) return;
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(mem(g_frame_ptr), g_w, g_h, 32, (int)g_frame_pitch,
                                                      SDL_PIXELFORMAT_ABGR8888);
  if (!s) return;
  SDL_Surface *rgb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_BGR24, 0);
  if (rgb) SDL_SaveBMP(rgb, file.c_str());
  SDL_FreeSurface(rgb);
  SDL_FreeSurface(s);
}

void load_script() {
  const char *path = getenv("SIMGOLF_SCRIPT");
  if (!path) return;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    ScriptLine l;
    char cmd[32] = {0}, rest[512] = {0};
    if (sscanf(line.c_str(), "%lf %31s %511[^\n]", &l.t, cmd, rest) < 2) continue;
    l.cmd = cmd;
    l.a = rest;
    sscanf(rest, "%d %d", &l.x, &l.y);
    if (l.cmd == "click" || l.cmd == "rclick") {
      // like tools/port/run.mjs: approach, settle, then press for 80 ms
      bool right = l.cmd == "rclick";
      ScriptLine m = l;
      m.cmd = "move", m.x = l.x - 3, m.y = l.y - 3;
      g_script.push_back(m);
      m.t += 100, m.x = l.x, m.y = l.y;
      g_script.push_back(m);
      m.t += 150, m.cmd = right ? "rdown" : "down";
      g_script.push_back(m);
      m.t += 80, m.cmd = right ? "rup" : "up";
      g_script.push_back(m);
      continue;
    }
    g_script.push_back(l);
  }
}

static double now_ms();

static void run_script() {
  while (g_script_pos < g_script.size() && g_script[g_script_pos].t <= now_ms()) {
    const ScriptLine &l = g_script[g_script_pos++];
    SDL_Event e{};
    fprintf(stderr, "script: %s %s\n", l.cmd.c_str(), l.a.c_str());
    if (l.cmd == "move") {
      e.type = SDL_MOUSEMOTION;
      e.motion.x = l.x;
      e.motion.y = l.y;
      g_injected.push_back(e);
    } else if (l.cmd == "down" || l.cmd == "up" || l.cmd == "rdown" || l.cmd == "rup") {
      e.type = l.cmd == "down" || l.cmd == "rdown" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
      e.button.button = l.cmd[0] == 'r' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
      e.button.clicks = 1;
      e.button.x = l.x;
      e.button.y = l.y;
      g_injected.push_back(e);
    } else if (l.cmd == "key") {
      e.type = SDL_KEYDOWN;
      e.key.keysym.sym = SDL_GetKeyFromName(l.a.c_str());
      e.key.keysym.scancode = SDL_GetScancodeFromKey(e.key.keysym.sym);
      g_injected.push_back(e);
      e.type = SDL_KEYUP;
      g_injected.push_back(e);
    } else if (l.cmd == "text") {
      e.type = SDL_TEXTINPUT;
      snprintf(e.text.text, sizeof e.text.text, "%s", l.a.c_str());
      g_injected.push_back(e);
    } else if (l.cmd == "shot") {
      save_frame(l.a);
    } else if (l.cmd == "quit") {
      guest_exit(0);
    }
  }
}

// ------------------------------------------------------------------ input

static bool host_shortcut(const SDL_KeyboardEvent &k) {
  const SDL_Keycode sym = k.keysym.sym;
  const Uint16 mod = k.keysym.mod;
  bool fs = (sym == SDLK_RETURN && (mod & KMOD_ALT)) || sym == SDLK_F11 ||
            (sym == SDLK_f && (mod & KMOD_GUI) && (mod & KMOD_CTRL));
  if (fs) {
    if (k.type == SDL_KEYDOWN && !k.repeat) {
      g_opts.fullscreen = !g_opts.fullscreen;
      apply_fullscreen();
      save_options();
    }
    return true;
  }
  // Ctrl+F10: smooth or sharp scaling (the browser build's 2D filter key)
  if (sym == SDLK_F10 && (mod & KMOD_CTRL)) {
    if (k.type == SDL_KEYDOWN && !k.repeat) {
      g_opts.linear = !g_opts.linear;
      apply_filter();
      save_options();
    }
    return true;
  }
  // Cmd+Q on macOS
  if (sym == SDLK_q && (mod & KMOD_GUI)) {
    if (k.type == SDL_KEYDOWN) guest_exit(0);
    return true;
  }
  return false;
}

extern "C" u32 w2c_env_SDL_PollEvent(struct w2c_env *, u32 out) {
  SDL_Event e;
  if (g_script_pos < g_script.size()) run_script();
  if (!g_injected.empty()) {
    e = g_injected.front();
    g_injected.erase(g_injected.begin());
    memcpy(mem(out), &e, sizeof e);
    return 1;
  }
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        guest_exit(0);
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        if (host_shortcut(e.key)) continue;
        break;
      case SDL_MOUSEMOTION:
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
      case SDL_MOUSEWHEEL:
      case SDL_TEXTINPUT:
      case SDL_WINDOWEVENT:
        break;
      default:
        continue;
    }
    // These event types hold no pointers, so the wasm32 and native layouts
    // match (SDL_Event is 56 bytes on both).
    static_assert(sizeof(SDL_Event) == 56, "SDL_Event layout");
    if (!mem_ok(out, sizeof e)) return 0;
    memcpy(mem(out), &e, sizeof e);
    return 1;
  }
  return 0;
}

// See js_edge_state in src/port/win32.c: 0x100 setting off, 0x200 pointer
// outside the window, else the x edge (bits 0-1) and y edge (bits 2-3), each
// 0 = low, 1 = none, 2 = high. Within 2 points of the game's edge, or out in
// the bars beside it, counts as on that edge.
extern "C" u32 w2c_host_js_edge_state(struct w2c_host *) {
  if (!g_opts.edge_scroll) return 0x100;
  if (!g_window || SDL_GetMouseFocus() != g_window || !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS))
    return 0x200;
  int mx, my, ww, wh;
  SDL_GetMouseState(&mx, &my);
  SDL_GetWindowSize(g_window, &ww, &wh);
  float s = std::min((float)ww / g_w, (float)wh / g_h);
  float vw = g_w * s, vh = g_h * s, vx = (ww - vw) / 2, vy = (wh - vh) / 2;
  const float band = 2;
  int ex = mx <= vx + band ? -1 : mx >= vx + vw - 1 - band ? 1 : 0;
  int ey = my <= vy + band ? -1 : my >= vy + vh - 1 - band ? 1 : 0;
  return (u32)((ex + 1) | ((ey + 1) << 2));
}

// ------------------------------------------------------------------ time

static const auto g_t0 = std::chrono::steady_clock::now();

static double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t0).count();
}

extern "C" f64 w2c_host_emscripten_get_now(struct w2c_host *) { return now_ms(); }

extern "C" void w2c_host_js_wait_frame(struct w2c_host *, u32 ms) {
  if ((int)ms > 0) SDL_Delay(ms);
  else std::this_thread::yield();
}

// ------------------------------------------------------------------ page hooks

extern "C" u32 w2c_host_js_screen_w(struct w2c_host *) { return (u32)g_opts.screen_w; }
extern "C" u32 w2c_host_js_api_trace_enabled(struct w2c_host *) { return getenv("SIMGOLF_API_TRACE") ? 1 : 0; }

// Game files are on disk already; there's nothing to fetch or mirror.
extern "C" u32 w2c_host_js_fetch_game_file(struct w2c_host *, u32, u32) { return 0; }
extern "C" u32 w2c_host_js_persist_load(struct w2c_host *) { return 0; }
extern "C" void w2c_host_js_persist_file(struct w2c_host *, u32, u32) {}

// The intro and closing videos need a Bink decoder; skipped for now.
extern "C" u32 w2c_host_port_no_videos(struct w2c_host *) { return 1; }
extern "C" void w2c_host_port_play_video(struct w2c_host *, u32, u32) {}

extern "C" void w2c_host_game_exited(struct w2c_host *) { guest_exit(0); }

// The upscaled presentation (hd.c / hd_web.js) is browser-only for now:
// reporting it inactive makes gdi.c present through SDL above.
extern "C" u32 w2c_env_hd_js_active(struct w2c_env *) { return 0; }
extern "C" u32 w2c_env_hd_js_terrain_on(struct w2c_env *) { return 0; }
extern "C" u32 w2c_env_hd_js_text_on(struct w2c_env *) { return 0; }
extern "C" u32 w2c_env_hd_js_font(struct w2c_env *, u32, u32, u32) { return 0; }
extern "C" void w2c_env_hd_js_text(struct w2c_env *, u32, u32) {}
extern "C" void w2c_env_hd_js_texture(struct w2c_env *, u32, u32, u32, u32) {}
extern "C" void w2c_env_hd_js_terrain(struct w2c_env *, u32, u32, u32, u32, u32, u32) {}
extern "C" void w2c_env_hd_js_frame(struct w2c_env *, u32, u32, u32, u32) {}

// ------------------------------------------------------------------ sound

extern "C" void w2c_env_snd_js_init(struct w2c_env *) { audio_init(); }
extern "C" void w2c_env_snd_js_shutdown(struct w2c_env *) { audio_shutdown(); }

// The guest names files "assets/<path in the game folder>" (src/platform/sound.cpp).
extern "C" void w2c_env_snd_js_register(struct w2c_env *, u32 index, u32 url, u32 single) {
  std::string u = mem_str(url);
  if (u.compare(0, 7, "assets/") == 0) u = g_paths.game + "/" + u.substr(7);
  else u.clear();
  audio_register((int)index, u, single != 0);
}

extern "C" void w2c_env_snd_js_play(struct w2c_env *, u32 index, u32 volume, u32 pan, u32 cents, u32 delay) {
  audio_play((int)index, (int)volume, (int)pan, (int)cents, (int)delay);
}

extern "C" void w2c_env_snd_js_fade_out(struct w2c_env *, u32 index, u32 ms) { audio_fade_out((int)index, (int)ms); }
