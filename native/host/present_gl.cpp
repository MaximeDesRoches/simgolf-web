// OpenGL presentation: src/platform/hd_web.js done natively. The game keeps
// drawing its 800x600 (or wider) RGB555 frame; this shows it at the window's
// pixel resolution with three layers (see src/port/hd.c):
//  - the frame, scaled by xBR, bilinear or nearest in a shader,
//  - the course terrain re-rendered at full resolution from terrain_hd.cpp's
//    triangles, replacing the pixels where the frame shows bare terrain,
//  - the game's TextOut text, redrawn in its own fonts (stb_truetype) at
//    full resolution.
// With HD off (Ctrl+F11) the plain frame is scaled the same way. The shaders
// are hd_web.js's, in GLSL 3.30.

#include <SDL.h>
#include <SDL_opengl.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

#include "stb_truetype.h"

#include "host.h"
#include "present.h"

namespace {

// ---------------------------------------------------------------- GL entry points beyond 1.1

#define GL_FUNCS(X)                                                                     \
  X(PFNGLCREATESHADERPROC, CreateShader) X(PFNGLSHADERSOURCEPROC, ShaderSource)         \
  X(PFNGLCOMPILESHADERPROC, CompileShader) X(PFNGLGETSHADERIVPROC, GetShaderiv)         \
  X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
  X(PFNGLATTACHSHADERPROC, AttachShader) X(PFNGLLINKPROGRAMPROC, LinkProgram)           \
  X(PFNGLGETPROGRAMIVPROC, GetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
  X(PFNGLDELETESHADERPROC, DeleteShader) X(PFNGLUSEPROGRAMPROC, UseProgram)             \
  X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) X(PFNGLUNIFORM1IPROC, Uniform1i)   \
  X(PFNGLUNIFORM1FPROC, Uniform1f) X(PFNGLUNIFORM2FPROC, Uniform2f)                     \
  X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray) \
  X(PFNGLGENBUFFERSPROC, GenBuffers) X(PFNGLBINDBUFFERPROC, BindBuffer)                 \
  X(PFNGLBUFFERDATAPROC, BufferData) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray) \
  X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer)                                  \
  X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
  X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                                \
  X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap) X(PFNGLACTIVETEXTUREPROC, ActiveTexture)   \
  X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate)

struct Gl {
#define DECL(type, name) type name;
  GL_FUNCS(DECL)
#undef DECL
} gl;

bool load_gl() {
#define LOAD(type, name)                                          \
  gl.name = (type)SDL_GL_GetProcAddress("gl" #name);              \
  if (!gl.name) {                                                 \
    fprintf(stderr, "hd: no gl" #name "\n");                      \
    return false;                                                 \
  }
  GL_FUNCS(LOAD)
#undef LOAD
  return true;
}

// ---------------------------------------------------------------- shaders (hd_web.js)

const char *kTerrainVs = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in vec2 aUV;
uniform vec2 uTarget;
out vec4 vCol;
out vec2 vUV;
void main() {
  vCol = aCol;
  vUV = aUV;
  gl_Position = vec4(aPos / uTarget * 2.0 - 1.0, 0.0, 1.0);
})";

const char *kTerrainFs = R"(#version 330 core
in vec4 vCol;
in vec2 vUV;
uniform sampler2D uTex;
uniform int uFlags;  // 1 blend, 2 textured, 4 texture has alpha
out vec4 o;
void main() {
  vec4 c = vCol;
  if ((uFlags & 2) != 0) {
    vec4 t = texture(uTex, vUV);
    c.rgb *= t.rgb;
    if ((uFlags & 4) != 0) c.a *= t.a;
  }
  if ((uFlags & 1) == 0) c.a = 1.0;
  o = c;
})";

const char *kQuadVs = R"(#version 330 core
out vec2 vUV;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  vUV = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// The 2D layer. xBR is Hyllian's xBR level 2 (single pass), working on the
// luma of the 5x5 neighbourhood minus its corners.
const char *kFrameFs = R"(#version 330 core
uniform usampler2D uScreen;
uniform sampler2D uMask;
uniform sampler2D uTerrain;
uniform vec2 uSrc;
uniform vec2 uTerrainScale;
uniform float uScale;
uniform int uFilter;     // 0 nearest, 1 linear, 2 xBR
uniform int uUseTerrain;
uniform int uDebug;
in vec2 vUV;
out vec4 o;

vec3 px(ivec2 p) {
  p = clamp(p, ivec2(0), ivec2(uSrc) - 1);
  uint c = texelFetch(uScreen, p, 0).r;
  return vec3(float((c >> 10) & 31u), float((c >> 5) & 31u), float(c & 31u)) / 31.0;
}

const vec3 kY = vec3(0.2126, 0.7152, 0.0722) * 48.0;
vec4 luma(vec3 a, vec3 b, vec3 c, vec3 d) { return vec4(dot(a, kY), dot(b, kY), dot(c, kY), dot(d, kY)); }
vec4 df(vec4 a, vec4 b) { return abs(a - b); }
vec4 eq(vec4 a, vec4 b) { return vec4(lessThan(df(a, b), vec4(15.0))); }
vec4 neq(vec4 a, vec4 b) { return vec4(notEqual(a, b)); }
vec4 wd(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {
  return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);
}
float cdf(vec3 a, vec3 b) { vec3 d = abs(a - b); return d.r + d.g + d.b; }

vec3 xbr(vec2 src) {
  ivec2 q = ivec2(floor(src));
  vec2 fp = fract(src);
  vec3 A1 = px(q + ivec2(-1, -2)), B1 = px(q + ivec2(0, -2)), C1 = px(q + ivec2(1, -2));
  vec3 A0 = px(q + ivec2(-2, -1)), A = px(q + ivec2(-1, -1)), B = px(q + ivec2(0, -1)), C = px(q + ivec2(1, -1)), C4 = px(q + ivec2(2, -1));
  vec3 D0 = px(q + ivec2(-2, 0)), D = px(q + ivec2(-1, 0)), E = px(q), F = px(q + ivec2(1, 0)), F4 = px(q + ivec2(2, 0));
  vec3 G0 = px(q + ivec2(-2, 1)), G = px(q + ivec2(-1, 1)), H = px(q + ivec2(0, 1)), I = px(q + ivec2(1, 1)), I4 = px(q + ivec2(2, 1));
  vec3 G5 = px(q + ivec2(-1, 2)), H5 = px(q + ivec2(0, 2)), I5 = px(q + ivec2(1, 2));

  vec4 b = luma(B, D, H, F), c = luma(C, A, G, I);
  vec4 e = luma(E, E, E, E);
  vec4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
  vec4 i4 = luma(I4, C1, A0, G5), i5 = luma(I5, C4, A1, G0), h5 = luma(H5, F4, B1, D0);
  vec4 f4 = h5.yzwx;

  const vec4 Ao = vec4(1.0, -1.0, -1.0, 1.0), Bo = vec4(1.0, 1.0, -1.0, -1.0), Co = vec4(1.5, 0.5, -0.5, 0.5);
  const vec4 Bx = vec4(0.5, 2.0, -0.5, -2.0), Cx = vec4(1.0, 1.0, -0.5, 0.0);
  const vec4 By = vec4(2.0, 0.5, -2.0, -0.5), Cy = vec4(2.0, 0.0, -1.0, 0.5);
  vec4 fx = Ao * fp.y + Bo * fp.x;
  vec4 fxLeft = Ao * fp.y + Bx * fp.x;
  vec4 fxUp = Ao * fp.y + By * fp.x;
  vec4 delta = vec4(1.0 / uScale);
  vec4 deltaL = vec4(0.5, 1.0, 0.5, 1.0) / uScale;
  vec4 deltaU = deltaL.yxwz;

  vec4 lv1 = neq(e, f) * neq(e, h) * clamp((1.0 - eq(f, b)) * (1.0 - eq(h, d)) +
             eq(e, i) * (1.0 - eq(f, i4)) * (1.0 - eq(h, i5)) + eq(e, g) + eq(e, c), 0.0, 1.0);
  vec4 lv2Left = neq(e, g) * neq(d, g);
  vec4 lv2Up = neq(e, c) * neq(b, c);
  vec4 edr = vec4(lessThan(wd(e, c, g, i, h5, f4, h, f), wd(h, d, i5, f, i4, b, e, i))) * lv1;
  vec4 edrLeft = vec4(lessThanEqual(2.0 * df(f, g), df(h, c))) * lv2Left;
  vec4 edrUp = vec4(greaterThanEqual(df(f, g), 2.0 * df(h, c))) * lv2Up;

  vec4 fx45 = edr * smoothstep(Co - delta, Co + delta, fx);
  vec4 fx30 = edr * edrLeft * smoothstep(Cx - deltaL, Cx + deltaL, fxLeft);
  vec4 fx60 = edr * edrUp * smoothstep(Cy - deltaU, Cy + deltaU, fxUp);
  vec4 m = max(max(fx30, fx60), fx45);
  vec4 pf = vec4(lessThanEqual(df(e, f), df(e, h)));

  vec3 r1 = mix(E, mix(H, F, pf.x), m.x);
  r1 = mix(r1, mix(B, D, pf.z), m.z);
  vec3 r2 = mix(E, mix(F, B, pf.y), m.y);
  r2 = mix(r2, mix(D, H, pf.w), m.w);
  return mix(r1, r2, step(cdf(E, r1), cdf(E, r2)));
}

vec3 bilinear(vec2 src) {
  vec2 s = src - 0.5;
  ivec2 q = ivec2(floor(s));
  vec2 f = fract(s);
  return mix(mix(px(q), px(q + ivec2(1, 0)), f.x), mix(px(q + ivec2(0, 1)), px(q + ivec2(1, 1)), f.x), f.y);
}

void main() {
  vec2 src = vUV * uSrc;
  vec3 c = uFilter == 2 ? xbr(src) : uFilter == 1 ? bilinear(src) : px(ivec2(floor(src)));
  if (uUseTerrain != 0) {
    vec2 t = vUV * uTerrainScale;
    vec4 terrain = texture(uTerrain, vec2(t.x, 1.0 - t.y));
    vec2 m = texture(uMask, vUV).rg;
    float w = smoothstep(0.35, 0.65, m.r) * terrain.a;
    c = mix(c, terrain.rgb * (m.g / max(m.r, 1e-3)), w);
    if (uDebug != 0) c = mix(c, vec3(1.0, 0.0, 1.0), 0.4 * w);
  }
  o = vec4(c, 1.0);
})";

const char *kTextFs = R"(#version 330 core
uniform sampler2D uText;
in vec2 vUV;
out vec4 o;
void main() { o = texture(uText, vUV); })";

struct Prog {
  GLuint p = 0;
  std::map<std::string, GLint> u;
  GLint operator[](const char *n) {
    auto it = u.find(n);
    if (it != u.end()) return it->second;
    return u[n] = gl.GetUniformLocation(p, n);
  }
};

bool compile(Prog &out, const char *vs, const char *fs) {
  GLuint p = gl.CreateProgram();
  for (auto [type, src] : {std::pair<GLenum, const char *>{GL_VERTEX_SHADER, vs}, {GL_FRAGMENT_SHADER, fs}}) {
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048];
      gl.GetShaderInfoLog(s, sizeof log, nullptr, log);
      fprintf(stderr, "hd: shader: %s\n", log);
      return false;
    }
    gl.AttachShader(p, s);
    gl.DeleteShader(s);
  }
  gl.LinkProgram(p);
  GLint ok = 0;
  gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    gl.GetProgramInfoLog(p, sizeof log, nullptr, log);
    fprintf(stderr, "hd: link: %s\n", log);
    return false;
  }
  out.p = p;
  return true;
}

// ---------------------------------------------------------------- state

SDL_Window *g_win;
SDL_GLContext g_ctx;
int g_fw = 800, g_fh = 600;          // the game's frame
int g_vx, g_vy, g_vw, g_vh;          // where it goes in the drawable (pixels)
int g_last_vw, g_last_vh;
Prog g_terrain, g_frame, g_text;
GLuint g_vbo, g_vao, g_empty_vao;
GLuint g_screen_tex, g_mask_tex, g_terrain_tex, g_text_tex, g_fbo;
int g_fbo_w, g_fbo_h, g_src_w, g_src_h, g_target_w, g_target_h;
bool g_have_terrain_target;
std::vector<uint16_t> g_rgb555;      // the plain path's frame, converted back
unsigned g_builds;

struct TerrainTex { GLuint tex; unsigned used; };
std::unordered_map<int32_t, TerrainTex> g_textures;

// Text
struct Font { std::vector<uint8_t> ttf; stbtt_fontinfo info; bool ok = false; };
std::map<int, Font> g_fonts;
std::vector<float> g_runs;
std::vector<float> g_drawn_runs;
int g_text_w, g_text_h;
std::vector<uint8_t> g_text_px;      // premultiplied RGBA, row 0 at the top
bool g_text_any;
struct Glyph { std::vector<uint8_t> a; int w, h, x0, y0; };
std::unordered_map<uint64_t, Glyph> g_glyphs;

GLuint make_tex(GLint filter) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

// The frame's place in the drawable: as large as fits, centred.
bool layout() {
  int dw, dh;
  SDL_GL_GetDrawableSize(g_win, &dw, &dh);
  double s = std::min((double)dw / g_fw, (double)dh / g_fh);
  g_vw = std::max(1, (int)std::lround(g_fw * s));
  g_vh = std::max(1, (int)std::lround(g_fh * s));
  g_vx = (dw - g_vw) / 2;
  g_vy = (dh - g_vh) / 2;
  bool changed = g_vw != g_last_vw || g_vh != g_last_vh;
  g_last_vw = g_vw;
  g_last_vh = g_vh;
  return changed;
}

void begin_output() {
  int dw, dh;
  SDL_GL_GetDrawableSize(g_win, &dw, &dh);
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, dw, dh);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(g_vx, g_vy, g_vw, g_vh);
}

void upload_screen(const uint16_t *px, int w, int h) {
  gl.ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_screen_tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (g_src_w != w || g_src_h != h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, px);
    glBindTexture(GL_TEXTURE_2D, g_mask_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    g_src_w = w;
    g_src_h = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_SHORT, px);
  }
}

void draw_frame(int w, int h, const uint8_t *mask) {
  bool use_terrain = mask && g_fbo_w > 0;
  if (use_terrain) {
    gl.ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_mask_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, mask);
    gl.ActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_terrain_tex);
  }
  gl.UseProgram(g_frame.p);
  gl.Uniform1i(g_frame["uScreen"], 0);
  gl.Uniform1i(g_frame["uMask"], 1);
  gl.Uniform1i(g_frame["uTerrain"], 2);
  gl.Uniform2f(g_frame["uSrc"], (float)w, (float)h);
  float tw = g_have_terrain_target ? (float)g_target_w : (float)w, th = g_have_terrain_target ? (float)g_target_h : (float)h;
  gl.Uniform2f(g_frame["uTerrainScale"], w / tw, h / th);
  gl.Uniform1f(g_frame["uScale"], (float)g_vw / w);
  gl.Uniform1i(g_frame["uFilter"], g_opts.filter);
  gl.Uniform1i(g_frame["uUseTerrain"], use_terrain ? 1 : 0);
  gl.Uniform1i(g_frame["uDebug"], getenv("SIMGOLF_HD_DEBUG") ? 1 : 0);
  gl.BindVertexArray(g_empty_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  gl.BindVertexArray(0);
  gl.ActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------- text (hd_web.js drawText)

const Glyph &glyph(int file, Font &f, float scale, int cp, int sub) {
  uint64_t key = (uint64_t)file << 56 ^ (uint64_t)(uint32_t)std::lround(scale * 65536.0f) << 20 ^ (uint64_t)cp << 2 ^ (uint64_t)sub;
  auto it = g_glyphs.find(key);
  if (it != g_glyphs.end()) return it->second;
  Glyph g{};
  float shift = sub / 4.0f;
  int x1, y1;
  stbtt_GetCodepointBitmapBoxSubpixel(&f.info, cp, scale, scale, shift, 0, &g.x0, &g.y0, &x1, &y1);
  g.w = std::max(0, x1 - g.x0);
  g.h = std::max(0, y1 - g.y0);
  g.a.resize((size_t)g.w * g.h);
  if (g.w && g.h) stbtt_MakeCodepointBitmapSubpixel(&f.info, g.a.data(), g.w, g.h, g.w, scale, scale, shift, 0, cp);
  if (g_glyphs.size() > 20000) g_glyphs.clear();
  return g_glyphs.emplace(key, std::move(g)).first->second;
}

void render_text() {
  const int W = g_vw, H = g_vh;
  if (g_text_w != W || g_text_h != H) {
    g_text_w = W;
    g_text_h = H;
    g_text_px.assign((size_t)W * H * 4, 0);
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_text_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_text_px.data());
  } else {
    std::fill(g_text_px.begin(), g_text_px.end(), 0);
  }
  const float sx = (float)W / g_fw, sy = (float)H / g_fh;
  const std::vector<float> &r = g_runs;
  std::vector<size_t> starts;
  for (size_t i = 0; i + 13 <= r.size(); i += 13 + (size_t)r[i + 12] * 3) starts.push_back(i);
  g_text_any = !starts.empty();
  // Runs come newest first; paint oldest first.
  for (size_t j = starts.size(); j-- > 0;) {
    size_t i = starts[j];
    auto fit = g_fonts.find((int)r[i]);
    if (fit == g_fonts.end() || !fit->second.ok) continue;
    Font &f = fit->second;
    uint32_t rgb = (uint32_t)r[i + 2];
    int cr = rgb & 255, cg = (rgb >> 8) & 255, cb = (rgb >> 16) & 255;
    int n = (int)r[i + 12];
    int clx0 = std::max(0, (int)std::floor(r[i + 5] * sx)), cly0 = std::max(0, (int)std::floor(r[i + 6] * sy));
    int clx1 = std::min(W, (int)std::ceil(r[i + 7] * sx)), cly1 = std::min(H, (int)std::ceil(r[i + 8] * sy));
    if (clx0 >= clx1 || cly0 >= cly1) continue;
    float scale = stbtt_ScaleForMappingEmToPixels(&f.info, r[i + 1] * sy);
    auto blend = [&](int x, int y, int a) {
      if (x < clx0 || x >= clx1 || y < cly0 || y >= cly1 || a <= 0) return;
      uint8_t *p = &g_text_px[((size_t)y * W + x) * 4];
      int inv = 255 - a;
      p[0] = (uint8_t)((cr * a + p[0] * inv) / 255);
      p[1] = (uint8_t)((cg * a + p[1] * inv) / 255);
      p[2] = (uint8_t)((cb * a + p[2] * inv) / 255);
      p[3] = (uint8_t)(a + p[3] * inv / 255);
    };
    // The game's bold is the glyph drawn again one pixel to the right.
    int steps = r[i + 3] != 0 ? std::max(1, (int)std::ceil(sx)) : 0;
    for (int g = 0; g < n; g++) {
      int cp = (int)r[i + 13 + g * 3];
      float gx = r[i + 14 + g * 3], gy = r[i + 15 + g * 3];
      for (int t = 0; t <= steps; t++) {
        float ox = (gx + (steps ? (float)t / steps : 0)) * sx;
        int ix = (int)std::floor(ox);
        int sub = (int)((ox - ix) * 4) & 3;
        const Glyph &gl_ = glyph((int)r[i], f, scale, cp, sub);
        int by = (int)std::lround(gy * sy);
        for (int y = 0; y < gl_.h; y++)
          for (int x = 0; x < gl_.w; x++) blend(ix + gl_.x0 + x, by + gl_.y0 + y, gl_.a[(size_t)y * gl_.w + x]);
      }
    }
    if (r[i + 4] != 0) {
      int x0 = (int)std::floor(r[i + 9] * sx), x1 = (int)std::ceil(r[i + 10] * sx);
      int y0 = (int)std::floor(r[i + 11] * sy), y1 = (int)std::ceil((r[i + 11] + 1) * sy);
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) blend(x, y, 255);
    }
  }
  gl.ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_text_tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, g_text_px.data());
}

void draw_text() {
  bool size_changed = g_text_w != g_vw || g_text_h != g_vh;
  if (size_changed || g_runs != g_drawn_runs) {
    render_text();
    g_drawn_runs = g_runs;
  }
  if (!g_text_any) return;
  gl.ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_text_tex);
  gl.UseProgram(g_text.p);
  gl.Uniform1i(g_text["uText"], 0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  gl.BindVertexArray(g_empty_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  gl.BindVertexArray(0);
  glDisable(GL_BLEND);
}

std::string g_shot;   // a test screenshot to take of the next frame

// Saves the output (before the swap, while the back buffer is defined).
void finish_frame() {
  if (!g_shot.empty()) {
    std::vector<uint8_t> px((size_t)g_vw * g_vh * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(g_vx, g_vy, g_vw, g_vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flipped(px.size());
    for (int y = 0; y < g_vh; y++)
      memcpy(&flipped[(size_t)y * g_vw * 3], &px[(size_t)(g_vh - 1 - y) * g_vw * 3], (size_t)g_vw * 3);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), g_vw, g_vh, 24, g_vw * 3, SDL_PIXELFORMAT_RGB24);
    if (s) {
      SDL_Surface *bgr = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_BGR24, 0);
      if (bgr) SDL_SaveBMP(bgr, g_shot.c_str());
      SDL_FreeSurface(bgr);
      SDL_FreeSurface(s);
    }
    g_shot.clear();
  }
  SDL_GL_SwapWindow(g_win);
}

}  // namespace

bool gl_present_active() { return g_ctx != nullptr; }
void gl_request_shot(const std::string &file) { g_shot = file; }

// ---------------------------------------------------------------- setup and the plain path

bool gl_present_init(SDL_Window *win, int w, int h) {
  g_win = win;
  g_fw = w;
  g_fh = h;
  g_ctx = SDL_GL_CreateContext(win);
  if (!g_ctx) {
    fprintf(stderr, "hd: no OpenGL 3.3 context: %s\n", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(win, g_ctx);
  SDL_GL_SetSwapInterval(1);
  if (!load_gl() || !compile(g_terrain, kTerrainVs, kTerrainFs) || !compile(g_frame, kQuadVs, kFrameFs) ||
      !compile(g_text, kQuadVs, kTextFs)) {
    SDL_GL_DeleteContext(g_ctx);
    g_ctx = nullptr;
    return false;
  }
  gl.GenBuffers(1, &g_vbo);
  gl.GenVertexArrays(1, &g_vao);
  gl.BindVertexArray(g_vao);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.EnableVertexAttribArray(0);
  gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
  gl.EnableVertexAttribArray(1);
  gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (void *)8);
  gl.EnableVertexAttribArray(2);
  gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
  gl.BindVertexArray(0);
  gl.GenVertexArrays(1, &g_empty_vao);
  g_screen_tex = make_tex(GL_NEAREST);
  g_mask_tex = make_tex(GL_LINEAR);
  g_terrain_tex = make_tex(GL_LINEAR);
  g_text_tex = make_tex(GL_NEAREST);
  gl.GenFramebuffers(1, &g_fbo);
  layout();
  fprintf(stderr, "hd: OpenGL %s\n", (const char *)glGetString(GL_VERSION));
  return true;
}

// HD off: the game's RGBA frame (gdi.c present_sdl), scaled by the same shader.
void gl_present_rgba(const uint8_t *rgba, int pitch) {
  g_rgb555.resize((size_t)g_fw * g_fh);
  for (int y = 0; y < g_fh; y++) {
    const uint8_t *s = rgba + (size_t)y * pitch;
    for (int x = 0; x < g_fw; x++, s += 4)
      g_rgb555[(size_t)y * g_fw + x] = (uint16_t)((s[0] >> 3) << 10 | (s[1] >> 3) << 5 | (s[2] >> 3));
  }
  layout();
  upload_screen(g_rgb555.data(), g_fw, g_fh);
  begin_output();
  draw_frame(g_fw, g_fh, nullptr);
  finish_frame();
}

void gl_present_video(const uint8_t *rgba, int w, int h) {
  static GLuint tex;
  static int tw, th;
  if (!tex) tex = make_tex(GL_LINEAR);
  gl.ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (tw != w || th != h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    tw = w;
    th = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  }
  layout();
  int dw, dh;
  SDL_GL_GetDrawableSize(g_win, &dw, &dh);
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, dw, dh);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  double s = std::min((double)dw / w, (double)dh / h);
  int vw = (int)std::lround(w * s), vh = (int)std::lround(h * s);
  glViewport((dw - vw) / 2, (dh - vh) / 2, vw, vh);
  gl.UseProgram(g_text.p);
  gl.Uniform1i(g_text["uText"], 0);
  gl.BindVertexArray(g_empty_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  gl.BindVertexArray(0);
  finish_frame();
}

void gl_viewport_points(float *x, float *y, float *w, float *h) {
  int ww, wh, dw, dh;
  SDL_GetWindowSize(g_win, &ww, &wh);
  SDL_GL_GetDrawableSize(g_win, &dw, &dh);
  float k = dw ? (float)ww / dw : 1;
  *x = g_vx * k;
  *y = g_vy * k;
  *w = g_vw * k;
  *h = g_vh * k;
}

// ---------------------------------------------------------------- hd.c's imports (hd_web.js)

extern "C" u32 w2c_env_hd_js_active(struct w2c_env *) {
  if (!g_ctx || !g_opts.hd) return 0;
  return layout() ? 2 : 1;
}

extern "C" u32 w2c_env_hd_js_terrain_on(struct w2c_env *) { return g_opts.hd_terrain ? 1 : 0; }
extern "C" u32 w2c_env_hd_js_text_on(struct w2c_env *) { return g_opts.hd_text ? 1 : 0; }

extern "C" u32 w2c_env_hd_js_font(struct w2c_env *, u32 file, u32 data, u32 size) {
  auto it = g_fonts.find((int)file);
  if (it == g_fonts.end()) {
    Font &f = g_fonts[(int)file];
    if (mem_ok(data, size) && size) {
      f.ttf.assign(mem(data), mem(data) + size);
      f.ok = stbtt_InitFont(&f.info, f.ttf.data(), stbtt_GetFontOffsetForIndex(f.ttf.data(), 0)) != 0;
    }
    if (!f.ok) fprintf(stderr, "hd: font %u unreadable; its text stays 1x\n", file);
    return f.ok;
  }
  return it->second.ok;
}

extern "C" void w2c_env_hd_js_text(struct w2c_env *, u32 runs, u32 n) {
  if (!n || !mem_ok(runs, (uint64_t)n * 4)) {
    g_runs.clear();
    return;
  }
  g_runs.resize(n);
  memcpy(g_runs.data(), mem(runs), (size_t)n * 4);
}

extern "C" void w2c_env_hd_js_texture(struct w2c_env *, u32 serial, u32 w, u32 h, u32 rgba) {
  auto it = g_textures.find((int32_t)serial);
  if (it != g_textures.end()) {
    it->second.used = g_builds;
    return;
  }
  if (!mem_ok(rgba, (uint64_t)w * h * 4)) return;
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, mem(rgba));
  gl.GenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  g_textures[(int32_t)serial] = {t, g_builds};
}

extern "C" void w2c_env_hd_js_terrain(struct w2c_env *, u32 verts, u32 nverts, u32 batches, u32 nbatches,
                                      u32 tw, u32 th) {
  if (!mem_ok(verts, (uint64_t)nverts * 32) || !mem_ok(batches, (uint64_t)nbatches * 20)) return;
  g_builds++;
  // The framebuffer matches the output's pixel density over the 1:1 target.
  int fw = std::max(1, (int)std::lround((double)g_vw * tw / g_fw));
  int fh = std::max(1, (int)std::lround((double)g_vh * th / g_fh));
  if (g_fbo_w != fw || g_fbo_h != fh) {
    glBindTexture(GL_TEXTURE_2D, g_terrain_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_terrain_tex, 0);
    g_fbo_w = fw;
    g_fbo_h = fh;
  }
  g_target_w = (int)tw;
  g_target_h = (int)th;
  g_have_terrain_target = true;
  gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
  glViewport(0, 0, fw, fh);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  gl.UseProgram(g_terrain.p);
  gl.Uniform2f(g_terrain["uTarget"], (float)tw, (float)th);
  gl.Uniform1i(g_terrain["uTex"], 0);
  gl.BindVertexArray(g_vao);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * 32, mem(verts), GL_STREAM_DRAW);
  gl.ActiveTexture(GL_TEXTURE0);
  // Blending keeps the destination alpha, so uncovered pixels stay at 0.
  gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  const int32_t *b = (const int32_t *)mem(batches);
  int flags = -1;
  for (u32 k = 0; k < nbatches; k++) {
    int32_t first = b[k * 5], count = b[k * 5 + 1], serial = b[k * 5 + 3], f = b[k * 5 + 4];
    if (f & 2) {
      auto it = g_textures.find(serial);
      if (it == g_textures.end()) continue;
      glBindTexture(GL_TEXTURE_2D, it->second.tex);
    }
    if (f != flags) {
      gl.Uniform1i(g_terrain["uFlags"], f);
      if (f & 1) glEnable(GL_BLEND);
      else glDisable(GL_BLEND);
      flags = f;
    }
    glDrawArrays(GL_TRIANGLES, first, count);
  }
  glDisable(GL_BLEND);
  gl.BindVertexArray(0);
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  // Textures of a previous course type go once they've been unused a while.
  if (g_builds % 64 == 0)
    for (auto it = g_textures.begin(); it != g_textures.end();) {
      if (g_builds - it->second.used > 64) {
        glDeleteTextures(1, &it->second.tex);
        it = g_textures.erase(it);
      } else {
        ++it;
      }
    }
}

extern "C" void w2c_env_hd_js_frame(struct w2c_env *, u32 screen, u32 w, u32 h, u32 mask) {
  if (!mem_ok(screen, (uint64_t)w * h * 2) || (mask && !mem_ok(mask, (uint64_t)w * h * 2))) return;
  upload_screen((const uint16_t *)mem(screen), (int)w, (int)h);
  begin_output();
  draw_frame((int)w, (int)h, mask ? mem(mask) : nullptr);
  if (!g_runs.empty() || g_text_any) draw_text();
  finish_frame();
}
