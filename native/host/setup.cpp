// The first-run window: asks for the player's own copy of the game (dropped
// on the window or picked with the system file dialog) and runs the importer
// on a worker thread while showing its progress. Text is drawn with
// stb_truetype in the bundled Liberation Sans.

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "host.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

extern std::string g_resources;

namespace {

const int W = 720, H = 440;

struct Font {
  std::vector<uint8_t> ttf;
  stbtt_fontinfo info;
  bool ok = false;
};

struct State {
  std::mutex m;
  std::vector<std::string> files;
  std::string status, error;
  float progress = -1;
  bool busy = false, done = false, missing = false;
} g;

void on_progress(void *, float f, const std::string &text) {
  std::lock_guard<std::mutex> l(g.m);
  g.progress = f;
  g.status = text;
}

void start_import() {
  std::vector<std::string> files;
  {
    std::lock_guard<std::mutex> l(g.m);
    if (g.busy || g.files.empty()) return;
    g.busy = true;
    g.error.clear();
    files = g.files;
  }
  std::thread([files] {
    bool missing = false;
    std::string err = import_game(files, on_progress, nullptr, &missing);
    std::lock_guard<std::mutex> l(g.m);
    g.busy = false;
    g.progress = -1;
    g.missing = missing;
    if (err.empty()) g.done = true;
    else {
      g.error = err;
      g.status.clear();
      // keep what was chosen when only one half is missing
      if (!missing) g.files.clear();
    }
  }).detach();
}

std::vector<std::string> choose_files() {
  std::vector<std::string> out;
#if defined(__APPLE__)
  const char *cmd =
      "osascript -e 'set fs to choose file with prompt \"Choose the SimGolf disc image and the no-CD golf.exe "
      "(or the zip they came in)\" with multiple selections allowed' -e 'set out to \"\"' "
      "-e 'repeat with f in fs' -e 'set out to out & POSIX path of f & linefeed' -e 'end repeat' "
      "-e 'return out' 2>/dev/null";
  if (FILE *p = popen(cmd, "r")) {
    char line[4096];
    while (fgets(line, sizeof line, p)) {
      std::string s(line);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      if (!s.empty()) out.push_back(s);
    }
    pclose(p);
  }
#elif defined(_WIN32)
  std::vector<wchar_t> buf(65536);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.lpstrFilter = L"SimGolf files (*.iso;*.bin;*.img;*.mdf;*.exe;*.zip;*.7z)\0*.iso;*.bin;*.img;*.mdf;*.exe;*.zip;*.7z\0All files\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = (DWORD)buf.size();
  ofn.lpstrTitle = L"Choose the SimGolf disc image and the no-CD golf.exe (or the zip they came in)";
  ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
  if (GetOpenFileNameW(&ofn)) {
    auto utf8 = [](const std::wstring &w) {
      int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
      std::string s(n > 0 ? n - 1 : 0, '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
      return s;
    };
    std::wstring first(buf.data());
    const wchar_t *p = buf.data() + first.size() + 1;
    if (!*p) out.push_back(utf8(first));
    for (; *p; p += wcslen(p) + 1) out.push_back(utf8(first + L"\\" + p));
  }
#else
  if (FILE *p = popen("zenity --file-selection --multiple --separator='\n' 2>/dev/null", "r")) {
    char line[4096];
    while (fgets(line, sizeof line, p)) {
      std::string s(line);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      if (!s.empty()) out.push_back(s);
    }
    pclose(p);
  }
#endif
  return out;
}

// ---------------------------------------------------------------- drawing

struct Canvas {
  std::vector<uint32_t> px = std::vector<uint32_t>(W * H);
  void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = std::max(0, y); j < std::min(H, y + h); j++)
      for (int i = std::max(0, x); i < std::min(W, x + w); i++) px[j * W + i] = c;
  }
};

uint32_t blend(uint32_t dst, uint32_t c, int a) {
  auto ch = [&](int s) { return (((dst >> s) & 255) * (255 - a) + ((c >> s) & 255) * a) / 255; };
  return 0xff000000u | ch(16) << 16 | ch(8) << 8 | ch(0);
}

// Draws text wrapped to width w; returns the height used.
int text(Canvas &cv, Font &f, const std::string &s, int x, int y, int w, float size, uint32_t color, bool draw = true) {
  if (!f.ok) return 0;
  float scale = stbtt_ScaleForPixelHeight(&f.info, size);
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&f.info, &asc, &desc, &gap);
  int line_h = (int)((asc - desc + gap) * scale * 1.15f);
  auto width_of = [&](const std::string &t) {
    float wsum = 0;
    for (unsigned char c : t) {
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&f.info, c, &adv, &lsb);
      wsum += adv * scale;
    }
    return wsum;
  };
  std::vector<std::string> lines;
  std::string cur;
  size_t i = 0;
  while (i <= s.size()) {
    size_t sp = s.find_first_of(" \n", i);
    if (sp == std::string::npos) sp = s.size();
    std::string word = s.substr(i, sp - i);
    std::string trial = cur.empty() ? word : cur + " " + word;
    if (!cur.empty() && width_of(trial) > w) {
      lines.push_back(cur);
      cur = word;
    } else {
      cur = trial;
    }
    if (sp < s.size() && s[sp] == '\n') {
      lines.push_back(cur);
      cur.clear();
    }
    i = sp + 1;
  }
  if (!cur.empty()) lines.push_back(cur);
  if (draw) {
    int base = y + (int)(asc * scale);
    for (const std::string &ln : lines) {
      float xpos = (float)x;
      for (unsigned char c : ln) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&f.info, c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&f.info, c, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
          std::vector<uint8_t> bm((size_t)gw * gh);
          stbtt_MakeCodepointBitmap(&f.info, bm.data(), gw, gh, gw, scale, scale, c);
          for (int j = 0; j < gh; j++)
            for (int k = 0; k < gw; k++) {
              int px = (int)xpos + x0 + k, py = base + y0 + j;
              if (px >= 0 && px < W && py >= 0 && py < H && bm[j * gw + k])
                cv.px[py * W + px] = blend(cv.px[py * W + px], color, bm[j * gw + k]);
            }
        }
        xpos += adv * scale;
      }
      base += line_h;
    }
  }
  return (int)lines.size() * line_h;
}

bool load_font(Font &f, const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  f.ttf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  f.ok = !f.ttf.empty() && stbtt_InitFont(&f.info, f.ttf.data(), stbtt_GetFontOffsetForIndex(f.ttf.data(), 0));
  return f.ok;
}

}  // namespace

bool run_setup(const std::string &resources) {
  Font regular, bold;
  load_font(regular, resources + "fonts/LiberationSans-Regular.ttf");
  load_font(bold, resources + "fonts/LiberationSans-Bold.ttf");
  if (!bold.ok) bold = regular;

  SDL_Window *win = SDL_CreateWindow("SimGolf setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                     SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  SDL_RenderSetLogicalSize(ren, W, H);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
  SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

  const SDL_Rect button = {40, 300, 200, 44};
  bool hover = false, quit = false;
  Canvas cv;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) quit = true;
      else if (e.type == SDL_DROPFILE) {
        {
          std::lock_guard<std::mutex> l(g.m);
          if (!g.busy) g.files.push_back(e.drop.file);
        }
        SDL_free(e.drop.file);
      } else if (e.type == SDL_DROPCOMPLETE) {
        start_import();
      } else if (e.type == SDL_MOUSEMOTION) {
        SDL_Point p = {e.motion.x, e.motion.y};
        hover = SDL_PointInRect(&p, &button);
      } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = {e.button.x, e.button.y};
        bool busy;
        {
          std::lock_guard<std::mutex> l(g.m);
          busy = g.busy;
        }
        if (SDL_PointInRect(&p, &button) && !busy) {
          auto chosen = choose_files();
          SDL_RaiseWindow(win);
          if (!chosen.empty()) {
            {
              std::lock_guard<std::mutex> l(g.m);
              g.files.insert(g.files.end(), chosen.begin(), chosen.end());
            }
            start_import();
          }
        }
      }
    }

    std::string status, error;
    float progress;
    bool busy, done;
    {
      std::lock_guard<std::mutex> l(g.m);
      status = g.status;
      error = g.error;
      progress = g.progress;
      busy = g.busy;
      done = g.done;
    }
    if (done) break;

    const uint32_t bg = 0xff16301e, fg = 0xffeef2ea, dim = 0xffa9bba6, accent = 0xff4f9a4a;
    cv.fill(0, 0, W, H, bg);
    int y = 36;
    y += text(cv, bold, "SimGolf", 40, y, W - 80, 34, fg) + 10;
    y += text(cv, regular,
              "This app plays your own copy of Sid Meier's SimGolf. It needs two things: the SimGolf CD image "
              "(.iso, .bin or .img) and the v1.02 no-CD golf.exe, or the zip or .7z they came in.",
              40, y, W - 80, 17, fg) + 12;
    y += text(cv, regular,
              "Drop them on this window, or choose them below. They're unpacked into this computer's "
              "app data folder and never leave it.",
              40, y, W - 80, 17, dim) + 12;
    if (busy) {
      cv.fill(40, button.y + 6, W - 80, 10, 0xff2a4a31);
      if (progress > 0) cv.fill(40, button.y + 6, (int)((W - 80) * std::min(1.0f, progress)), 10, accent);
      text(cv, regular, status, 40, button.y + 26, W - 80, 15, dim);
    } else {
      cv.fill(button.x, button.y, button.w, button.h, hover ? 0xff5fae58 : accent);
      text(cv, bold, "Choose files…", button.x + 42, button.y + 12, button.w, 18, 0xffffffff);
      if (!error.empty()) text(cv, regular, error, 40, button.y + button.h + 16, W - 80, 15, 0xffffb4a0);
    }
    SDL_UpdateTexture(tex, nullptr, cv.px.data(), W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
    SDL_Delay(16);
  }
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  return !quit;
}
