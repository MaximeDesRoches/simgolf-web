// The intro and closing videos for the desktop build, in place of
// src/port/video.c (a WebM in a <video> element). Decodes the disc's .bik
// directly with FFmpeg's Bink decoder (tools/native/build-ffmpeg-wasi.sh)
// and hands each frame and the audio to the host. A key press or click skips
// it. Unlike the browser, the intro plays before the game loads rather than
// over it.

#include <emscripten.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/fs.h"

HOST_IMPORT(host_video_frame) void host_video_frame(const uint8_t *rgba, int w, int h);
HOST_IMPORT(host_video_audio) void host_video_audio(const float *samples, int frames, int channels, int rate);
HOST_IMPORT(host_video_end) void host_video_end(void);
HOST_IMPORT(js_wait_frame) void js_wait_frame(int ms);
HOST_IMPORT(port_no_videos) int port_no_videos(void);

static AVCodecContext *open_dec(AVFormatContext *fmt, int idx) {
  if (idx < 0) return NULL;
  AVStream *st = fmt->streams[idx];
  const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
  if (!c) return NULL;
  AVCodecContext *ctx = avcodec_alloc_context3(c);
  avcodec_parameters_to_context(ctx, st->codecpar);
  ctx->pkt_timebase = st->time_base;
  if (avcodec_open2(ctx, c, NULL) < 0) {
    avcodec_free_context(&ctx);
    return NULL;
  }
  return ctx;
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// I420 (BT.601, limited range) to RGBA
static void to_rgba(const AVFrame *f, uint8_t *out) {
  for (int y = 0; y < f->height; y++) {
    const uint8_t *py = f->data[0] + y * f->linesize[0];
    const uint8_t *pu = f->data[1] + (y >> 1) * f->linesize[1];
    const uint8_t *pv = f->data[2] + (y >> 1) * f->linesize[2];
    uint8_t *o = out + (size_t)y * f->width * 4;
    for (int x = 0; x < f->width; x++) {
      int c = (py[x] - 16) * 298, d = pu[x >> 1] - 128, e = pv[x >> 1] - 128;
      o[0] = (uint8_t)clamp255((c + 409 * e + 128) >> 8);
      o[1] = (uint8_t)clamp255((c - 100 * d - 208 * e + 128) >> 8);
      o[2] = (uint8_t)clamp255((c + 516 * d + 128) >> 8);
      o[3] = 255;
      o += 4;
    }
  }
}

// A key press or a click skips the video.
static int skipped(void) {
  SDL_Event e;
  int skip = 0;
  while (SDL_PollEvent(&e))
    if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) skip = 1;
  return skip;
}

static void play(const char *path) {
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
    fprintf(stderr, "video: can't open %s\n", path);
    if (fmt) avformat_close_input(&fmt);
    return;
  }
  int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  AVCodecContext *vdec = open_dec(fmt, vidx), *adec = open_dec(fmt, aidx);
  if (!vdec) {
    fprintf(stderr, "video: no Bink video in %s\n", path);
    avformat_close_input(&fmt);
    return;
  }
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  uint8_t *rgba = NULL;
  float *pcm = NULL;
  size_t pcm_cap = 0;
  double t0 = emscripten_get_now();
  double tb = av_q2d(fmt->streams[vidx]->time_base);
  int done = 0;
  while (!done) {
    int r = av_read_frame(fmt, pkt);
    if (r < 0) break;
    AVCodecContext *dec = pkt->stream_index == vidx ? vdec : pkt->stream_index == aidx ? adec : NULL;
    if (dec && avcodec_send_packet(dec, pkt) >= 0) {
      while (!done && avcodec_receive_frame(dec, frame) == 0) {
        if (dec == vdec) {
          if (!rgba) rgba = malloc((size_t)frame->width * frame->height * 4);
          to_rgba(frame, rgba);
          double at = frame->best_effort_timestamp == AV_NOPTS_VALUE ? 0 : frame->best_effort_timestamp * tb * 1000.0;
          for (;;) {
            if (skipped()) { done = 1; break; }
            double wait = at - (emscripten_get_now() - t0);
            if (wait <= 0) break;
            js_wait_frame(wait > 10 ? 10 : (int)wait);
          }
          if (!done) host_video_frame(rgba, frame->width, frame->height);
        } else {
          int ch = adec->ch_layout.nb_channels, n = frame->nb_samples;
          if (ch < 1 || ch > 2) continue;
          if (pcm_cap < (size_t)n * ch) {
            pcm_cap = (size_t)n * ch;
            pcm = realloc(pcm, pcm_cap * sizeof *pcm);
          }
          for (int i = 0; i < n; i++)
            for (int c = 0; c < ch; c++)
              pcm[i * ch + c] = adec->sample_fmt == AV_SAMPLE_FMT_FLTP ? ((const float *)frame->extended_data[c])[i]
                                : adec->sample_fmt == AV_SAMPLE_FMT_FLT ? ((const float *)frame->data[0])[i * ch + c]
                                : 0.0f;
          host_video_audio(pcm, n, ch, adec->sample_rate);
        }
      }
    }
    av_packet_unref(pkt);
  }
  host_video_end();
  free(rgba);
  free(pcm);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&vdec);
  avcodec_free_context(&adec);
  avformat_close_input(&fmt);
}

// Plays the .bik at a game path. background (the intro, which the browser
// plays while the game loads) is played first here.
void port_play_game_video(const char *dospath, int background) {
  (void)background;
  if (port_no_videos()) return;
  char real[512];
  if (!fs_resolve(dospath, real, sizeof real, 0)) {
    fprintf(stderr, "video: %s not found\n", dospath);
    return;
  }
  play(real);
}
