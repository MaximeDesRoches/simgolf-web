// OpenGL presentation (present_gl.cpp): the window's output when OpenGL 3.3
// is available, with hd.c's high-resolution layers; SDL_Renderer otherwise.
#pragma once

#include <SDL.h>
#include <string>

// Creates the context on win (made with SDL_WINDOW_OPENGL) for a w x h frame.
bool gl_present_init(SDL_Window *win, int w, int h);
bool gl_present_active();
// The plain path (HD off): gdi.c's RGBA frame.
void gl_present_rgba(const uint8_t *rgba, int pitch);
// Where the frame sits in the window, in window points (for mouse mapping).
void gl_viewport_points(float *x, float *y, float *w, float *h);
// Saves the next presented frame, as shown, to a .bmp (SIMGOLF_SCRIPT "shot").
void gl_request_shot(const std::string &file);
// One video frame, scaled to fit the window.
void gl_present_video(const uint8_t *rgba, int w, int h);
