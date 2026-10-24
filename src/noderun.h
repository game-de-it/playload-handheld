#pragma once
#include <stdint.h>
#include <sys/types.h>

typedef struct {
  pid_t pid;
  int to, from;
  char error[2048];
} NodeGame;

typedef void (*NodeSoundFn)(void *user, int ch, const double *events, int count, int fixed);
// Rasterizes a screen description: 360 cells (8 rows, ink, paper), patterns of 8 rows,
// and sprites (x, y, col, pattern index), into 160x144 palette indices.
typedef void (*NodeRasterFn)(uint8_t *pixels, const uint8_t *cells, const uint8_t *patterns,
                             int pattern_count, const uint8_t *sprites, int sprite_count);
void node_set_raster(NodeRasterFn fn);

const char *node_root(void);
int node_available(void);
// Starts node-host.mjs for a game; engine_dir NULL means the built-in engine files.
int node_start(NodeGame *g, const char *game_path, const char *engine_dir);
// Reads the reply that follows start (top-level code and first tick).
int node_read_frame(NodeGame *g, uint8_t *pixels, NodeSoundFn on_sound, void *user,
                    int timeout_ms);
// Asks for one tick with the given buttons without waiting for its reply, so
// Node.js computes it while the player presents the previous frame.
int node_request(NodeGame *g, int mask);
// Runs one tick with the given buttons. Returns -1 with g->error on failure.
int node_frame(NodeGame *g, int mask, uint8_t *pixels, NodeSoundFn on_sound, void *user,
               int timeout_ms);
void node_stop(NodeGame *g);
