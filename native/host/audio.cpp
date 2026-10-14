// The sound back end: what src/platform/sound_web.js does with Web Audio,
// done with a small SDL audio mixer. Same rules (see sound.h): gain
// (v+1)/128, sound.dll's linear pan law, pitch in cents, a start delay,
// fades, `smpl` loop points, at most 16 voices per index and one for
// streamed (>128 KB) or dedicated sounds. Files load on their first play.

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <deque>
#include <map>
#include <vector>

#include "host.h"

namespace {

constexpr int kRate = 44100;            // sound.dll mixed at 44.1 kHz
constexpr int kMaxVoices = 16;
constexpr uint32_t kStreamBytes = 128 * 1024;

struct Sound {
  std::string path;
  bool single = false;
  enum { IDLE, READY, FAILED } state = IDLE;
  std::vector<float> pcm;   // interleaved
  int channels = 1, rate = kRate;
  size_t frames = 0;
  bool loop = false;
  double loop_start = 0, loop_end = 0;   // frames
  int voices = 0;
};

struct Voice {
  int index;
  Sound *s;
  double pos = 0, step = 1;
  float gl = 1, gr = 1;
  long delay = 0;          // output frames before it starts
  float fade = 1, fade_step = 0;
  bool done = false;
};

std::map<int, Sound> g_sounds;

// The video's soundtrack: stereo at its own rate, played as it's pushed.
struct Stream {
  std::deque<float> pcm;   // interleaved stereo
  int rate = kRate;
  double pos = 0;          // fractional frame within pcm
} g_stream;
std::vector<Voice> g_voices;
SDL_AudioDeviceID g_dev;
float g_master = 1;

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool decode(Sound &s) {
  std::ifstream in(s.path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) return false;
  int tag = 0, bits = 0;
  size_t data_off = 0, data_len = 0;
  bool have_fmt = false, have_data = false, have_loop = false;
  uint32_t lstart = 0, lend = 0;
  for (size_t p = 12; p + 8 <= b.size();) {
    uint32_t size = rd32(&b[p + 4]);
    size_t body = p + 8;
    if (!memcmp(&b[p], "fmt ", 4) && body + 16 <= b.size()) {
      tag = rd16(&b[body]);
      s.channels = rd16(&b[body + 2]);
      s.rate = (int)rd32(&b[body + 4]);
      bits = rd16(&b[body + 14]);
      have_fmt = true;
    } else if (!memcmp(&b[p], "data", 4)) {
      data_off = body;
      data_len = std::min<size_t>(size, b.size() - body);
      have_data = true;
    } else if (!memcmp(&b[p], "smpl", 4) && size >= 60 && body + 60 <= b.size() && rd32(&b[body + 28]) > 0) {
      lstart = rd32(&b[body + 44]);
      lend = rd32(&b[body + 48]) + 1;
      have_loop = true;
    }
    p = body + size + (size & 1);
  }
  if (!have_fmt || !have_data || tag != 1 || (bits != 8 && bits != 16) || s.channels < 1 || s.channels > 2) return false;
  int bytes = bits / 8;
  s.frames = data_len / (bytes * s.channels);
  s.pcm.resize(s.frames * s.channels);
  const uint8_t *d = &b[data_off];
  for (size_t i = 0; i < s.pcm.size(); i++)
    s.pcm[i] = bytes == 2 ? (int16_t)rd16(d + i * 2) / 32768.0f : (d[i] - 128) / 128.0f;
  s.single = s.single || data_len > kStreamBytes;
  if (have_loop && lend > lstart) {
    s.loop = true;
    s.loop_start = lstart;
    s.loop_end = std::min<double>(lend, (double)s.frames);
  }
  return true;
}

void mix(void *, Uint8 *stream, int len) {
  float *out = (float *)stream;
  int n = len / (int)(2 * sizeof(float));
  memset(out, 0, len);
  for (Voice &v : g_voices) {
    Sound &s = *v.s;
    for (int i = 0; i < n && !v.done; i++) {
      if (v.delay > 0) { v.delay--; continue; }
      if (s.loop && v.pos >= s.loop_end) v.pos = s.loop_start + std::fmod(v.pos - s.loop_start, s.loop_end - s.loop_start);
      if (v.pos >= (double)s.frames) { v.done = true; break; }
      size_t i0 = (size_t)v.pos;
      size_t i1 = i0 + 1 < s.frames ? i0 + 1 : (s.loop ? (size_t)s.loop_start : i0);
      float t = (float)(v.pos - i0);
      float l, r;
      if (s.channels == 1) {
        l = r = s.pcm[i0] + (s.pcm[i1] - s.pcm[i0]) * t;
      } else {
        l = s.pcm[i0 * 2] + (s.pcm[i1 * 2] - s.pcm[i0 * 2]) * t;
        r = s.pcm[i0 * 2 + 1] + (s.pcm[i1 * 2 + 1] - s.pcm[i0 * 2 + 1]) * t;
      }
      out[i * 2] += l * v.gl * v.fade;
      out[i * 2 + 1] += r * v.gr * v.fade;
      v.pos += v.step;
      if (v.fade_step) {
        v.fade -= v.fade_step;
        if (v.fade <= 0) v.done = true;
      }
    }
  }
  if (!g_stream.pcm.empty()) {
    double step = (double)g_stream.rate / kRate;
    for (int i = 0; i < n; i++) {
      size_t i0 = (size_t)g_stream.pos;
      if ((i0 + 1) * 2 + 1 >= g_stream.pcm.size()) break;
      float t = (float)(g_stream.pos - i0);
      out[i * 2] += g_stream.pcm[i0 * 2] + (g_stream.pcm[i0 * 2 + 2] - g_stream.pcm[i0 * 2]) * t;
      out[i * 2 + 1] += g_stream.pcm[i0 * 2 + 1] + (g_stream.pcm[i0 * 2 + 3] - g_stream.pcm[i0 * 2 + 1]) * t;
      g_stream.pos += step;
    }
    size_t used = (size_t)g_stream.pos;
    g_stream.pcm.erase(g_stream.pcm.begin(), g_stream.pcm.begin() + (long)std::min(used * 2, g_stream.pcm.size()));
    g_stream.pos -= (double)used;
  }
  for (int i = 0; i < n * 2; i++) out[i] = std::max(-1.0f, std::min(1.0f, out[i] * g_master));
  for (size_t k = 0; k < g_voices.size();) {
    if (g_voices[k].done) {
      g_voices[k].s->voices--;
      g_voices.erase(g_voices.begin() + (long)k);
    } else {
      k++;
    }
  }
}

struct Lock {
  Lock() { if (g_dev) SDL_LockAudioDevice(g_dev); }
  ~Lock() { if (g_dev) SDL_UnlockAudioDevice(g_dev); }
};

}  // namespace

void audio_init() {
  if (g_dev) return;
  SDL_AudioSpec want{}, have{};
  want.freq = kRate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = mix;
  g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!g_dev) {
    fprintf(stderr, "sound: %s\n", SDL_GetError());
    return;
  }
  SDL_PauseAudioDevice(g_dev, 0);
}

void audio_register(int index, const std::string &path, bool single) {
  Lock lock;
  Sound s;
  s.path = path;
  s.single = single;
  if (path.empty()) s.state = Sound::FAILED;
  g_sounds[index] = std::move(s);
}

void audio_play(int index, int volume, int pan, int cents, int delay_ms) {
  if (!g_dev) return;
  auto it = g_sounds.find(index);
  if (it == g_sounds.end()) return;
  Sound &s = it->second;
  if (s.state == Sound::IDLE) {
    Sound loaded = s;   // decode outside the lock
    if (decode(loaded)) loaded.state = Sound::READY;
    else {
      fprintf(stderr, "sound: can't read %s\n", s.path.c_str());
      loaded.state = Sound::FAILED;
    }
    Lock lock;
    loaded.voices = s.voices;
    s = std::move(loaded);
  }
  if (s.state != Sound::READY) return;
  Lock lock;
  if (s.voices >= (s.single ? 1 : kMaxVoices)) return;
  Voice v;
  v.index = index;
  v.s = &s;
  v.step = s.rate * std::pow(2.0, cents / 1200.0) / kRate;
  float gain = (std::max(0, std::min(127, volume)) + 1) / 128.0f;
  pan = std::max(-64, std::min(63, pan));
  v.gl = gain * (pan > 0 ? std::max(0.0f, 1 - pan / 63.0f) : 1.0f);
  v.gr = gain * (pan < 0 ? std::max(0.0f, 1 + pan / 63.0f) : 1.0f);
  v.delay = std::max(0, delay_ms) * (long)kRate / 1000;
  g_voices.push_back(v);
  s.voices++;
}

void audio_fade_out(int index, int ms) {
  Lock lock;
  float step = 1.0f / (std::max(ms, 1) * (kRate / 1000.0f));
  for (Voice &v : g_voices)
    if (v.index == index && !v.fade_step) v.fade_step = step * v.fade;
}

void audio_stop(int index) {
  Lock lock;
  for (Voice &v : g_voices)
    if (v.index == index) v.done = true;
}

void audio_set_master(float gain) {
  Lock lock;
  g_master = gain;
}

void audio_shutdown() {
  if (!g_dev) return;
  SDL_CloseAudioDevice(g_dev);
  g_dev = 0;
  g_voices.clear();
  g_sounds.clear();
}

void audio_stream_push(const float *samples, int frames, int channels, int rate) {
  if (!g_dev) audio_init();
  Lock lock;
  g_stream.rate = rate;
  for (int i = 0; i < frames; i++) {
    float l = samples[i * channels], r = channels > 1 ? samples[i * channels + 1] : l;
    g_stream.pcm.push_back(l);
    g_stream.pcm.push_back(r);
  }
}

void audio_stream_end() {
  Lock lock;
  g_stream.pcm.clear();
  g_stream.pos = 0;
}
