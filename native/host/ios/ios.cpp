// iPad build: the game files ship inside the app (tools/ios/make-gamedata.mjs),
// so the first-run window becomes a copy into the app's data folder. The
// presenter is SDL_Renderer (Metal) until present_gl.cpp speaks OpenGL ES.
#include <SDL.h>

#include <filesystem>

#include "../host.h"
#include "../present.h"

namespace fs = std::filesystem;

// The game writes saves into game/Saved Games, so the bundle (read-only)
// can't be used in place.
bool run_setup(const std::string &resources) {
  std::error_code ec;
  fs::path src = fs::path(resources) / "gamedata";
  fs::path dst = fs::path(g_paths.data);
  fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
  if (ec) {
    std::string msg = "Couldn't copy the game files: " + ec.message();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SimGolf", msg.c_str(), nullptr);
    return false;
  }
  return true;
}

bool gl_present_init(SDL_Window *, int, int) { return false; }
bool gl_present_active() { return false; }
void gl_present_rgba(const uint8_t *, int) {}
void gl_viewport_points(float *x, float *y, float *w, float *h) { *x = *y = *w = *h = 0; }
void gl_request_shot(const std::string &) {}
void gl_present_video(const uint8_t *, int, int) {}

// hd.c's imports: HD off, so the game draws its own 800x600 frame
extern "C" u32 w2c_env_hd_js_active(struct w2c_env *) { return 0; }
extern "C" u32 w2c_env_hd_js_terrain_on(struct w2c_env *) { return 0; }
extern "C" u32 w2c_env_hd_js_text_on(struct w2c_env *) { return 0; }
extern "C" u32 w2c_env_hd_js_font(struct w2c_env *, u32, u32, u32) { return 0; }
extern "C" void w2c_env_hd_js_text(struct w2c_env *, u32, u32) {}
extern "C" void w2c_env_hd_js_texture(struct w2c_env *, u32, u32, u32, u32) {}
extern "C" void w2c_env_hd_js_terrain(struct w2c_env *, u32, u32, u32, u32, u32, u32) {}
extern "C" void w2c_env_hd_js_frame(struct w2c_env *, u32, u32, u32, u32) {}
