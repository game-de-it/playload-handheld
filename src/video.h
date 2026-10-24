#pragma once
#include <stdint.h>

// Opens a fullscreen OpenGL ES 2.0 window (SDL must be initialized).
int video_open(const uint32_t *palette, int fit);
int video_has_crt(void);
int video_ready(void);
// Builds the CRT shader from a Playload crt.js; keeps the current one on failure.
int video_load_crt(const char *crt_js);
// Shows a 160x144 frame of palette indices, flat or through the CRT shader.
void video_present(const uint8_t *pixels, int crt);
// Last frame's GPU time when PLAYLOAD_GPU_TIMING is set (diagnostics).
extern double video_gpu_ms;
