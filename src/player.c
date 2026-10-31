// Playload Handheld: runs Playload game URLs on Linux handhelds (plumOS /
// PortMaster). Games run in Node.js or QuickJS; rasterizing, the AY sound
// chip and presentation (SDL2 + OpenGL ES) are native.
//
//   playload [options] <game.url | game.js | games-directory>
//     --headless          no window or audio; run --ticks ticks and print timings
//     --ticks N           ticks to run in headless mode (default 600)
//     --inject FILE       append FILE's JS to the game (bench bots)
//     --seed N            replace Math.random with a seeded generator
//     --dump-at T --dump FILE   write the frame after tick T as a PPM image
//     --audio             headless: also synthesize the game's sound and time it
//     --dump-audio FILE   headless: write that sound as raw float32 mono 48 kHz
//     --engine NAME       use engine/NAME (or "builtin") instead of the saved choice
//     --runtime node      headless: run the game in Node.js (PLAYLOAD_NODE_ROOT, default node/)
//     --update            check GitHub for a newer Playload release, test and install it
//     --selftest DIR      run the updater's self-test against the engine files in DIR
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "ay.h"
#include "inflate.h"
#include "js_bundle.h"
#include "noderun.h"
#include "quickjs.h"

#ifdef HAVE_SDL
#include <SDL.h>

#include "video.h"
#endif

#define PLAYLOAD_HANDHELD_VERSION "1.1.0"
#define W 160
#define H 144

static const uint32_t palette[8] = {0x000000, 0x55415f, 0x646964, 0xd77355,
                                    0x508cd7, 0x64b964, 0xe6c86e, 0xdcf5ff};

// ---------------------------------------------------------------------------
// Native rasterizer, fed by host.js's __nativeRender with the screen cells
// (8 pattern rows, ink, paper) and sprites (x, y, col + 8 pattern rows).

static uint8_t framebuffer[W * H];
// Per thread (the Arcade preview runs a game on its own thread): where the
// rasterizer draws, and how many frames it drew (the updater's self-test checks it).
static __thread uint8_t *raster_target = framebuffer;
static __thread long raster_calls;

// glitch: Playload 0.9.4's overflow noise, XORed over the frame (NULL: none).
static void raster(const uint8_t *cells, const int32_t *pos, const uint8_t *pat, int sprites,
                   const uint8_t *glitch) {
  raster_calls++;
  for (int cy = 0; cy < 18; cy++)
    for (int cx = 0; cx < 20; cx++) {
      const uint8_t *cell = cells + (cy * 20 + cx) * 10;
      const uint8_t c1 = cell[8] & 7, c2 = cell[9] & 7;
      for (int row = 0; row < 8; row++) {
        uint8_t *out = raster_target + (cy * 8 + row) * W + cx * 8;
        const uint8_t bits = cell[row];
        for (int bit = 0; bit < 8; bit++) out[bit] = bits & (128 >> bit) ? c1 : c2;
      }
    }
  for (int s = 0; s < sprites; s++) {
    const int x = pos[s * 3], y = pos[s * 3 + 1];
    const uint8_t col = (uint8_t)(pos[s * 3 + 2] & 7);
    if (x <= -8 || x >= W || y <= -8 || y >= H) continue;
    const uint8_t *pattern = pat + s * 8;
    for (int row = 0; row < 8; row++) {
      const int py = y + row;
      if (py < 0 || py >= H || !pattern[row]) continue;
      for (int bit = 0; bit < 8; bit++) {
        const int px = x + bit;
        if (px >= 0 && px < W && (pattern[row] & (128 >> bit))) raster_target[py * W + px] = col;
      }
    }
  }
  if (glitch)
    for (int i = 0; i < W * H; i++) raster_target[i] ^= glitch[i] & 7;
}

// ---------------------------------------------------------------------------
// Sound commands: the game thread queues programs, the audio thread plays them.

#define QUEUE 512
static struct {
  int ch[QUEUE];
  AYProgram *program[QUEUE];
  int head, tail;
} Q;
static AY ay;
static int audio_open = 0;
#ifdef HAVE_SDL
static SDL_AudioDeviceID audio_device;
#endif

static void queue_sound(int ch, AYProgram *program) {
#ifdef HAVE_SDL
  if (audio_open) SDL_LockAudioDevice(audio_device);
#endif
  const int next = (Q.tail + 1) % QUEUE;
  if (next == Q.head) {
    ay_program_free(program);  // a full queue drops the newest sound
  } else {
    Q.ch[Q.tail] = ch;
    Q.program[Q.tail] = program;
    Q.tail = next;
  }
#ifdef HAVE_SDL
  if (audio_open) SDL_UnlockAudioDevice(audio_device);
#endif
}

static void drain_sounds(void) {
  while (Q.head != Q.tail) {
    ay_play(&ay, Q.ch[Q.head], Q.program[Q.head]);
    Q.head = (Q.head + 1) % QUEUE;
  }
}

// ---------------------------------------------------------------------------
// Native functions exposed to JS as __native.bind/render/sound

static uint8_t *typed_pointer(JSContext *ctx, JSValueConst v, size_t *bytes) {
  size_t offset, length, per;
  JSValue buffer = JS_GetTypedArrayBuffer(ctx, v, &offset, &length, &per);
  if (JS_IsException(buffer)) return NULL;
  size_t size;
  uint8_t *base = JS_GetArrayBuffer(ctx, &size, buffer);
  JS_FreeValue(ctx, buffer);
  if (!base) return NULL;
  if (bytes) *bytes = length;
  return base + offset;
}

static JSValue js_raster(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  size_t cell_bytes, pos_bytes, pat_bytes;
  const uint8_t *cells = typed_pointer(ctx, argv[0], &cell_bytes);
  const int32_t *pos = (const int32_t *)typed_pointer(ctx, argv[1], &pos_bytes);
  const uint8_t *pat = typed_pointer(ctx, argv[2], &pat_bytes);
  int32_t n = 0;
  JS_ToInt32(ctx, &n, argv[3]);
  if (!cells || !pos || !pat) return JS_EXCEPTION;
  if (cell_bytes < 3600 || n < 0 || (size_t)n * 12 > pos_bytes || (size_t)n * 8 > pat_bytes)
    return JS_ThrowRangeError(ctx, "bad raster input");
  size_t glitch_bytes = 0;
  const uint8_t *glitch = argc > 4 && JS_IsObject(argv[4]) ? typed_pointer(ctx, argv[4], &glitch_bytes) : NULL;
  if (glitch && glitch_bytes < W * H) return JS_ThrowRangeError(ctx, "bad raster input");
  raster(cells, pos, pat, n, glitch);
  return JS_UNDEFINED;
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *t = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
  if (t) {
    printf("%s\n", t);
    JS_FreeCString(ctx, t);
  }
  return JS_UNDEFINED;
}

static __thread int mute_sounds;  // the updater's self-test and the Arcade preview run games silently

// Rasterizes a screen sent by node-host.mjs (see NodeRasterFn).
static void node_raster(uint8_t *pixels, const uint8_t *cells, const uint8_t *patterns,
                        int pattern_count, const uint8_t *sprites, int sprite_count) {
  static int32_t *pos;
  static uint8_t *pat;
  static int capacity;
  if (sprite_count > capacity) {
    capacity = sprite_count * 2;
    pos = realloc(pos, sizeof(int32_t) * 3 * (size_t)capacity);
    pat = realloc(pat, 8 * (size_t)capacity);
  }
  // Column layout: x i16[], y i16[], pattern u16[], col u8[] (little-endian).
  const uint8_t *xs = sprites, *ys = xs + sprite_count * 2, *ids = ys + sprite_count * 2,
                *cols = ids + sprite_count * 2;
  int n = 0;
  for (int i = 0; i < sprite_count; i++) {
    const int id = ids[i * 2] | ids[i * 2 + 1] << 8;
    if (id >= pattern_count) continue;
    pos[n * 3] = (int16_t)(xs[i * 2] | xs[i * 2 + 1] << 8);
    pos[n * 3 + 1] = (int16_t)(ys[i * 2] | ys[i * 2 + 1] << 8);
    pos[n * 3 + 2] = cols[i];
    memcpy(pat + n * 8, patterns + id * 8, 8);
    n++;
  }
  raster(cells, pos, pat, n, NULL);  // noise comes separately, see node_read_frame
  if (pixels != framebuffer) memcpy(pixels, framebuffer, sizeof framebuffer);
}

static void queue_sound(int ch, AYProgram *program);
static void node_sound(void *user, int ch, const double *events, int count, int fixed) {
  if (!audio_open || mute_sounds || ch < 0 || ch > 2) return;
  AYProgram *program = ay_program_new(events, count, fixed);
  if (program) queue_sound(ch, program);
}

static JSValue js_sound(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t ch = 0, fixed = 0;
  JS_ToInt32(ctx, &ch, argv[0]);
  JS_ToInt32(ctx, &fixed, argv[2]);
  size_t bytes;
  const double *events = (const double *)typed_pointer(ctx, argv[1], &bytes);
  if (!events) return JS_EXCEPTION;
  if (audio_open && !mute_sounds) {
    AYProgram *program = ay_program_new(events, (int)(bytes / (sizeof(double) * 3)), fixed);
    if (program) queue_sound(ch, program);
  }
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Modules. engine.js, sound.js and glyphs.js are Playload's own files: built in,
// or downloaded by the updater into engine/<version>/. patch.js adapts them.

static const char *module_dir;  // NULL: built in

static const char *bundle_source(const char *name, size_t *len) {
  for (int i = 0; i < js_bundle_count; i++)
    if (!strcmp(js_bundle[i].name, name)) {
      *len = js_bundle[i].length;
      return js_bundle[i].source;
    }
  return NULL;
}

static char *read_file(const char *path, size_t *len);

static JSModuleDef *load_module(JSContext *ctx, const char *name, void *opaque) {
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  const int updatable =
      !strcmp(base, "engine.js") || !strcmp(base, "sound.js") || !strcmp(base, "glyphs.js");
  char *owned = NULL;
  size_t len = 0;
  const char *source = NULL;
  if (updatable && module_dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", module_dir, base);
    source = owned = read_file(path, &len);
    if (!source) {
      JS_ThrowReferenceError(ctx, "missing %s", path);
      return NULL;
    }
  } else {
    source = bundle_source(base, &len);
  }
  if (!source) {
    JS_ThrowReferenceError(ctx, "module not found: %s", name);
    return NULL;
  }
  JSValue text = JS_NewStringLen(ctx, source, len);
  free(owned);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue patch = JS_GetPropertyStr(ctx, global, "__patchModule");
  JSValue args[2] = {JS_NewString(ctx, base), text};
  JSValue patched = JS_Call(ctx, patch, JS_UNDEFINED, 2, args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, patch);
  JS_FreeValue(ctx, global);
  if (JS_IsException(patched)) return NULL;
  size_t plen;
  const char *ptext = JS_ToCStringLen(ctx, &plen, patched);
  JS_FreeValue(ctx, patched);
  if (!ptext) return NULL;
  JSValue fn = JS_Eval(ctx, ptext, plen, base, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  JS_FreeCString(ctx, ptext);
  if (JS_IsException(fn)) return NULL;
  JSModuleDef *m = JS_VALUE_GET_PTR(fn);
  JS_FreeValue(ctx, fn);
  return m;
}

static __thread char error_text[1024];

static const char *take_error(JSContext *ctx) {
  JSValue ex = JS_GetException(ctx);
  const char *msg = JS_ToCString(ctx, ex);
  snprintf(error_text, sizeof error_text, "%s", msg ? msg : "unknown error");
  if (msg) JS_FreeCString(ctx, msg);
  JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
  if (!JS_IsUndefined(stack)) {
    const char *s = JS_ToCString(ctx, stack);
    if (s) {
      fprintf(stderr, "%s\n%s\n", error_text, s);
      JS_FreeCString(ctx, s);
    }
  } else {
    fprintf(stderr, "%s\n", error_text);
  }
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, ex);
  return error_text;
}

static int run_jobs(JSContext *ctx) {
  JSContext *job_ctx;
  int r;
  while ((r = JS_ExecutePendingJob(JS_GetRuntime(ctx), &job_ctx)) > 0) {
  }
  return r;
}

static __thread JSValue start_fn, frame_fn, error_fn;  // of the thread's current game
static JSValue js_launch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_layout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_crt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_runtime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_engine(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_arcade(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_payloads(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_preview(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static int setup_js(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue native = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, native, "raster", JS_NewCFunction(ctx, js_raster, "raster", 4));
  JS_SetPropertyStr(ctx, native, "sound", JS_NewCFunction(ctx, js_sound, "sound", 3));
  JS_SetPropertyStr(ctx, native, "engine", JS_NewCFunction(ctx, js_engine, "engine", 1));
  JS_SetPropertyStr(ctx, native, "update", JS_NewCFunction(ctx, js_update, "update", 0));
  JS_SetPropertyStr(ctx, native, "launch", JS_NewCFunction(ctx, js_launch, "launch", 1));
  JS_SetPropertyStr(ctx, native, "layout", JS_NewCFunction(ctx, js_layout, "layout", 1));
  JS_SetPropertyStr(ctx, native, "crt", JS_NewCFunction(ctx, js_crt, "crt", 1));
  JS_SetPropertyStr(ctx, native, "runtime", JS_NewCFunction(ctx, js_runtime, "runtime", 1));
  JS_SetPropertyStr(ctx, native, "arcade", JS_NewCFunction(ctx, js_arcade, "arcade", 1));
  JS_SetPropertyStr(ctx, native, "payloads", JS_NewCFunction(ctx, js_payloads, "payloads", 0));
  JS_SetPropertyStr(ctx, native, "save", JS_NewCFunction(ctx, js_save, "save", 2));
  JS_SetPropertyStr(ctx, native, "preview", JS_NewCFunction(ctx, js_preview, "preview", 1));
  JS_SetPropertyStr(ctx, native, "state", JS_NewCFunction(ctx, js_state, "state", 1));
  JS_SetPropertyStr(ctx, global, "__native", native);
  JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));  // console.*
  size_t patch_len;
  const char *patch_js = bundle_source("patch.js", &patch_len);
  JSValue pr = JS_Eval(ctx, patch_js, patch_len, "patch.js", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(pr)) {
    take_error(ctx);
    return -1;
  }
  JS_FreeValue(ctx, pr);
  JS_SetModuleLoaderFunc(JS_GetRuntime(ctx), NULL, load_module, NULL);
  const char *boot = "import './host.js';";
  JSValue result = JS_Eval(ctx, boot, strlen(boot), "boot.js", JS_EVAL_TYPE_MODULE);
  if (JS_IsException(result)) {
    take_error(ctx);
    return -1;
  }
  run_jobs(ctx);
  if (JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
    JSValue reason = JS_PromiseResult(ctx, result);
    JS_Throw(ctx, reason);
    take_error(ctx);
    JS_FreeValue(ctx, result);
    return -1;
  }
  JS_FreeValue(ctx, result);
  start_fn = JS_GetPropertyStr(ctx, global, "__start");
  frame_fn = JS_GetPropertyStr(ctx, global, "__frame");
  error_fn = JS_GetPropertyStr(ctx, global, "__error");
  JS_FreeValue(ctx, global);
  return 0;
}

// Frames the last tick asked to wait (Playload 0.9.4+: one per exceeded limit).
static __thread int tick_delay;

static void take_delay(JSContext *ctx, JSValue r) {
  int32_t d = 0;
  if (JS_IsNumber(r)) JS_ToInt32(ctx, &d, r);
  tick_delay = d > 0 && d < 10 ? d : 0;
}

static int call_start(JSContext *ctx, const char *source, const char *inject) {
  JSValue args[2] = {JS_NewString(ctx, source), JS_NewString(ctx, inject ? inject : "")};
  JSValue r = JS_Call(ctx, start_fn, JS_UNDEFINED, 2, args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  if (JS_IsException(r)) return -1;
  take_delay(ctx, r);
  JS_FreeValue(ctx, r);
  run_jobs(ctx);
  return 0;
}

static int call_frame(JSContext *ctx, int mask) {
  JSValue arg = JS_NewInt32(ctx, mask);
  JSValue r = JS_Call(ctx, frame_fn, JS_UNDEFINED, 1, &arg);
  if (JS_IsException(r)) return -1;
  take_delay(ctx, r);
  JS_FreeValue(ctx, r);
  run_jobs(ctx);
  return 0;
}

static void show_error(JSContext *ctx, const char *message) {
  JSValue arg = JS_NewString(ctx, message);
  JSValue r = JS_Call(ctx, error_fn, JS_UNDEFINED, 1, &arg);
  JS_FreeValue(ctx, arg);
  if (JS_IsException(r)) take_error(ctx);
  JS_FreeValue(ctx, r);
}

static int eval_global(JSContext *ctx, const char *code) {
  JSValue r = JS_Eval(ctx, code, strlen(code), "<setup>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) return -1;
  JS_FreeValue(ctx, r);
  return 0;
}

// ---------------------------------------------------------------------------
// Game files

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0 || n > (1 << 24)) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)n + 1);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  if (buf) {
    buf[n] = 0;
    if (len) *len = (size_t)n;
  }
  return buf;
}

static int b64(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

static char *decode_game(char *text, char *err, size_t errlen);

// Accepts a file containing a Playload URL (…#v1.<payload>) or plain JS source.
static char *load_game(const char *path, char *err, size_t errlen) {
  size_t len;
  char *text = read_file(path, &len);
  if (!text) {
    snprintf(err, errlen, "cannot read %s", path);
    return NULL;
  }
  return decode_game(text, err, errlen);
}

// Takes text holding a Playload URL or plain JS source (and frees it); returns the source.
static char *decode_game(char *text, char *err, size_t errlen) {
  char *hash = strstr(text, "#v1.");
  int skip = 4;
  if (!hash && (hash = strstr(text, "#1."))) skip = 3;
  if (!hash) return text;  // plain source
  const char *p = hash + skip;
  size_t n = 0;
  while (b64((unsigned char)p[n]) >= 0) n++;
  if (n % 4 == 1) {
    snprintf(err, errlen, "invalid payload length");
    free(text);
    return NULL;
  }
  uint8_t *bytes = malloc(n * 3 / 4 + 3);
  size_t out = 0;
  uint32_t acc = 0;
  int bitsn = 0;
  for (size_t i = 0; i < n; i++) {
    acc = (acc << 6) | (uint32_t)b64((unsigned char)p[i]);
    bitsn += 6;
    if (bitsn >= 8) {
      bitsn -= 8;
      bytes[out++] = (uint8_t)(acc >> bitsn);
    }
  }
  free(text);
  uint8_t *source;
  size_t source_len;
  if (raw_inflate(bytes, out, &source, &source_len, 1 << 20)) {
    snprintf(err, errlen, "invalid game URL");
    free(bytes);
    return NULL;
  }
  free(bytes);
  char *s = realloc(source, source_len + 1);
  if (!s) {
    free(source);
    return NULL;
  }
  s[source_len] = 0;
  return s;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void write_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) {
    const uint32_t c = palette[framebuffer[i] & 7];
    const uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static void report(const char *label, double *t, int n) {
  if (n <= 0) return;
  double sum = 0;
  int over = 0;
  for (int i = 0; i < n; i++) {
    sum += t[i];
    over += t[i] > 16.7;
  }
  qsort(t, (size_t)n, sizeof *t, cmp_double);
  printf("[%s] avg %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  over16.7 %d/%d\n", label,
         sum / n, t[n / 2], t[n * 95 / 100], t[n * 99 / 100], t[n - 1], over, n);
}

// ---------------------------------------------------------------------------

typedef struct {
  const char *game, *inject_path, *dump_path, *audio_path;
  int headless, ticks, dump_at, seed, has_seed, audio, update, version;
  const char *engine, *selftest_dir, *menu_dump, *runtime;
} Options;

// ---------------------------------------------------------------------------
// Game list and menu

#define MAX_GAMES 256
static char *game_paths[MAX_GAMES];
static char *game_titles[MAX_GAMES];
static int game_count;

static int has_suffix(const char *name, const char *suffix) {
  const size_t n = strlen(name), m = strlen(suffix);
  return n > m && !strcasecmp(name + n - m, suffix);
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// The title is the game's "// @title" line, or else its file name.
static char *title_of(const char *path) {
  char err[64];
  char *src = load_game(path, err, sizeof err);
  const char *t = src ? strstr(src, "@title") : NULL;
  char buf[64];
  if (t) {
    t += 6;
    while (*t == ' ' || *t == '\t') t++;
    size_t n = 0;
    while (t[n] && t[n] != '\n' && t[n] != '\r' && n < sizeof buf - 1) n++;
    memcpy(buf, t, n);
    buf[n] = 0;
  } else {
    const char *base = strrchr(path, '/');
    snprintf(buf, sizeof buf, "%s", base ? base + 1 : path);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = 0;
  }
  free(src);
  return strdup(buf);
}

static char games_dir[512];  // the directory given on the command line, if any

static void find_games(const char *path) {
  DIR *d = opendir(path);
  if (d) snprintf(games_dir, sizeof games_dir, "%s", path);
  if (!d) {
    game_paths[game_count] = strdup(path);
    game_titles[game_count++] = title_of(path);
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) && game_count < MAX_GAMES) {
    if (e->d_name[0] == '.' || !(has_suffix(e->d_name, ".url") || has_suffix(e->d_name, ".js")))
      continue;
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", path, e->d_name);
    game_paths[game_count++] = strdup(full);
  }
  closedir(d);
  qsort(game_paths, (size_t)game_count, sizeof *game_paths, cmp_str);
  for (int i = 0; i < game_count; i++) game_titles[i] = title_of(game_paths[i]);
}

// The menu is js/menu.js, a Playload game embedded like the engine files.
static const char *menu_source(void) {
  size_t len;
  return bundle_source("menu.js", &len);
}

static int pending_launch = -1;

// Button layout. "nintendo" maps the controller's A/B to Playload's A/B as SDL reports
// them; "xbox" swaps them, for devices whose labels follow the other convention.
// Saved as "buttons=nintendo|xbox" in playload.cfg in the working directory.
static int swap_ab = 0;
static int crt_on = 0;  // CRT view; saved as "crt=on|off"
static int fps_on = 0;  // performance overlay; saved as "fps=on|off"
// Where games run: Node.js (V8) when it is installed, else QuickJS. Saved as
// "runtime=node|quickjs". The menu, updater and error screens always use QuickJS.
static int use_node = -1;
static const char *config_path = "playload.cfg";
// Engine version: "builtin", or a directory name under engine/ made by the updater.
static char engine_name[64] = "builtin";
static void select_engine(const char *name);
// The menu starts on the game played last; saved as "last=<file name>".
static int last_game = 0;

static void load_config(void) {
  char *text = read_file(config_path, NULL);
  const char *env = getenv("PLAYLOAD_BUTTONS");
  if (text && strstr(text, "buttons=xbox")) swap_ab = 1;
  if (text && strstr(text, "crt=on")) crt_on = 1;
  if (text && strstr(text, "fps=on")) fps_on = 1;
  if (text && strstr(text, "runtime=quickjs")) use_node = 0;
  if (text && strstr(text, "runtime=node")) use_node = 1;
  if (getenv("PLAYLOAD_RUNTIME")) use_node = !strcmp(getenv("PLAYLOAD_RUNTIME"), "node");
  if (use_node < 0 || !node_available()) use_node = node_available();
  const char *l = text ? strstr(text, "last=") : NULL;
  if (l) {
    char name[256] = "";
    sscanf(l + 5, "%255[^\r\n]", name);
    for (int i = 0; i < game_count; i++) {
      const char *base = strrchr(game_paths[i], '/');
      if (!strcmp(base ? base + 1 : game_paths[i], name)) last_game = i;
    }
  }
  const char *e = text ? strstr(text, "engine=") : NULL;
  if (e) {
    char name[64] = "";
    sscanf(e + 7, "%63[A-Za-z0-9._-]", name);
    select_engine(name);
  }
  if (env) swap_ab = !strcmp(env, "xbox");
  if (getenv("PLAYLOAD_CRT")) crt_on = !strcmp(getenv("PLAYLOAD_CRT"), "on");
  if (getenv("PLAYLOAD_FPS")) fps_on = !strcmp(getenv("PLAYLOAD_FPS"), "on");
  free(text);
}

static void save_config(void) {
  FILE *f = fopen(config_path, "w");
  if (!f) return;
  const char *last = game_count ? strrchr(game_paths[last_game], '/') : NULL;
  last = last ? last + 1 : game_count ? game_paths[last_game] : "";
  fprintf(f, "buttons=%s\ncrt=%s\nfps=%s\nruntime=%s\nengine=%s\nlast=%s\n",
          swap_ab ? "xbox" : "nintendo", crt_on ? "on" : "off", fps_on ? "on" : "off",
          use_node ? "node" : "quickjs", engine_name, last);
  fclose(f);
}

static JSValue js_layout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if (argc > 0 && !JS_IsUndefined(argv[0])) {
    int32_t v = 0;
    JS_ToInt32(ctx, &v, argv[0]);
    swap_ab = v != 0;
    save_config();
  }
  return JS_NewInt32(ctx, swap_ab);
}

static JSValue js_runtime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if (argc > 0 && !JS_IsUndefined(argv[0])) {
    int32_t v = 0;
    JS_ToInt32(ctx, &v, argv[0]);
    use_node = v != 0 && node_available();
    save_config();
  }
  return JS_NewInt32(ctx, use_node);
}

static JSValue js_crt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if (argc > 0 && !JS_IsUndefined(argv[0])) {
    int32_t v = 0;
    JS_ToInt32(ctx, &v, argv[0]);
    crt_on = v != 0;
    save_config();
  }
  return JS_NewInt32(ctx, crt_on);
}

static JSValue js_launch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t i = -1;
  JS_ToInt32(ctx, &i, argv[0]);
  if (i >= 0 && i < game_count) pending_launch = i;
  return JS_UNDEFINED;
}

static void set_titles(JSContext *ctx) {
  JSValue list = JS_NewArray(ctx);
  for (int i = 0; i < game_count; i++)
    JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_NewString(ctx, game_titles[i]));
  // File names without extension: the menu shows them for titles the font cannot draw.
  JSValue files = JS_NewArray(ctx);
  for (int i = 0; i < game_count; i++) {
    const char *base = strrchr(game_paths[i], '/');
    base = base ? base + 1 : game_paths[i];
    const char *dot = strrchr(base, '.');
    JS_SetPropertyUint32(ctx, files, (uint32_t)i,
                         JS_NewStringLen(ctx, base, dot ? (size_t)(dot - base) : strlen(base)));
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "__titles", list);
  JS_SetPropertyStr(ctx, global, "__files", files);
  JS_SetPropertyStr(ctx, global, "__menuSel", JS_NewInt32(ctx, last_game));
  JS_FreeValue(ctx, global);
}

// Where the menu reopens after a game (see js/menu.js), kept across sessions.
static char menu_state[64];

// ---------------------------------------------------------------------------
// One QuickJS runtime per game, so nothing leaks from one game into the next.

static JSContext *vm_new(Options *o) {
  JSRuntime *rt = JS_NewRuntime();
  JS_SetMaxStackSize(rt, 4 << 20);
  JSContext *ctx = JS_NewContext(rt);
  const double t0 = now_ms();
  if (setup_js(ctx)) return NULL;
  printf("engine ready in %.0f ms\n", now_ms() - t0);
  if (o->has_seed) {
    char code[512];
    snprintf(code, sizeof code,
             "{let seed=%d;Math.random=()=>{seed=(seed+0x6d2b79f5)|0;"
             "let t=Math.imul(seed^(seed>>>15),1|seed);t=(t+Math.imul(t^(t>>>7),61|t))^t;"
             "return((t^(t>>>14))>>>0)/4294967296;};}",
             o->seed);
    eval_global(ctx, code);
  }
  return ctx;
}

static void vm_free(JSContext *ctx) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  JS_FreeValue(ctx, start_fn);
  JS_FreeValue(ctx, frame_fn);
  JS_FreeValue(ctx, error_fn);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

// ---------------------------------------------------------------------------
// Engine versions and the in-app updater.
//
// The updater takes the newest tagged Playload release from GitHub, downloads
// engine.js, sound.js, glyphs.js and crt.js at that commit into engine/.new,
// and runs an API smoke test and up to six games from games/ silently for a
// second or two each (see selftest()). Only if all of them run without errors, and draw through the native
// rasterizer, does it become engine/<tag> and the selected version. Versions
// stay installed, so the menu can switch back to an older one or to "builtin".

#define ENGINE_ROOT "engine"
static const char *engine_files[4] = {"engine.js", "sound.js", "glyphs.js", "crt.js"};

static int engine_complete(const char *dir) {
  char path[512];
  for (int i = 0; i < 4; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, engine_files[i]);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
  }
  return 1;
}

static int valid_name(const char *s, int max) {
  int n = 0;
  for (; s[n]; n++)
    if (!(isalnum((unsigned char)s[n]) || s[n] == '.' || s[n] == '_' || s[n] == '-')) return 0;
  return n > 0 && n <= max && s[0] != '.';
}

static char engine_dir_buf[256];

static void select_engine(const char *requested) {
  char name[64], dir[256];
  // Copy first: callers may pass engine_name itself, which is overwritten below.
  snprintf(name, sizeof name, "%s", requested);
  snprintf(dir, sizeof dir, ENGINE_ROOT "/%s", name);
  if (strcmp(name, "builtin") && valid_name(name, 63) && engine_complete(dir)) {
    snprintf(engine_name, sizeof engine_name, "%s", name);
    snprintf(engine_dir_buf, sizeof engine_dir_buf, "%s", dir);
    module_dir = engine_dir_buf;
  } else {
    snprintf(engine_name, sizeof engine_name, "builtin");
    module_dir = NULL;
  }
#ifdef HAVE_SDL
  // The CRT shader comes from the same version's crt.js.
  if (video_ready()) {
    size_t len;
    char *own = NULL;
    const char *crt = NULL;
    if (module_dir) {
      char path[512];
      snprintf(path, sizeof path, "%s/crt.js", module_dir);
      crt = own = read_file(path, &len);
    } else {
      crt = bundle_source("crt.js", &len);
    }
    if (crt && video_load_crt(crt)) printf("crt.js of %s not usable; keeping shader\n", engine_name);
    free(own);
  }
#endif
}

static int list_engines(char names[][64], int max) {
  int n = 0;
  snprintf(names[n++], 64, "builtin");
  DIR *d = opendir(ENGINE_ROOT);
  if (!d) return n;
  struct dirent *e;
  while ((e = readdir(d)) && n < max) {
    char dir[512];
    snprintf(dir, sizeof dir, ENGINE_ROOT "/%s", e->d_name);
    if (valid_name(e->d_name, 63) && engine_complete(dir)) snprintf(names[n++], 64, "%s", e->d_name);
  }
  closedir(d);
  qsort(names + 1, (size_t)(n - 1), 64, (int (*)(const void *, const void *))strcmp);
  return n;
}

static JSValue js_engine(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if (argc > 0 && JS_IsString(argv[0])) {
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) {
      select_engine(name);
      save_config();
      JS_FreeCString(ctx, name);
    }
    return JS_NewString(ctx, engine_name);
  }
  char names[32][64];
  const int n = list_engines(names, 32);
  JSValue result = JS_NewObject(ctx), list = JS_NewArray(ctx);
  for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_NewString(ctx, names[i]));
  JS_SetPropertyStr(ctx, result, "current", JS_NewString(ctx, engine_name));
  JS_SetPropertyStr(ctx, result, "list", list);
  return result;
}

static const char *curl_command(void) {
  // plumOS ships curl as a wrapper that sets up its CA bundle. Ports run with
  // the GPU driver in LD_PRELOAD, which curl's own loader cannot satisfy, so
  // curl gets a clean environment.
  FILE *f = fopen("/storage/plumos/bin/curl", "rb");
  if (f) {
    fclose(f);
    return "LD_PRELOAD= LD_LIBRARY_PATH= sh /storage/plumos/bin/curl";
  }
  return "LD_PRELOAD= curl";
}

// Runs a command and captures up to outlen-1 bytes of its output. Returns the exit status.
static int run_capture(const char *cmd, char *out, size_t outlen) {
  FILE *p = popen(cmd, "r");
  if (!p) return -1;
  size_t n = 0;
  int c;
  while ((c = fgetc(p)) != EOF)
    if (out && n + 1 < outlen) out[n++] = (char)c;
  if (out) out[n] = 0;
  return pclose(p);
}

// Finds `"key": "value"` after `from`; copies the value. Returns a pointer after it or NULL.
static const char *json_string(const char *from, const char *key, char *value, size_t len) {
  char pattern[64];
  snprintf(pattern, sizeof pattern, "\"%s\"", key);
  const char *k = strstr(from, pattern);
  if (!k) return NULL;
  const char *q = strchr(k + strlen(pattern), '"');
  if (!q) return NULL;
  q++;
  size_t n = 0;
  while (q[n] && q[n] != '"' && n + 1 < len) {
    value[n] = q[n];
    n++;
  }
  value[n] = 0;
  return q + n;
}

static Options *current_options;

// API smoke test for the self-test: every Playload function and variable.
static const char *smoke_game =
    "pcg['\xe2\x91\xa0'].pat = '0103071f3f7f7f6f';\n"
    "pcg['A'].col = [6, 0];\n"
    "cls(1);\n"
    "put(1, 1, 'SELF TEST \xf0\x9f\x92\x8e', 7, 0);\n"
    "scr[2][3].chr = '\xc2\xb7';\n"
    "scr[2][3].col = [5, null];\n"
    "play(0, 'T150M7V8O4N4cegC');\n"
    "let n = 0;\n"
    "function tick() {\n"
    "  n++;\n"
    "  spr(n % 160, 60, '\xf0\x9f\x91\xbe', 4);\n"
    "  spr(10, 80, 'AB\\nCD', 3);\n"
    "  if (btnp(4) || btn(0)) play(1, 'V5O6N1c');\n"
    "}\n";

static uint8_t update_screen[W * H];
#ifdef HAVE_SDL
static void show_progress(const char *text);
#endif
static void update_progress(const char *text) {
  printf("update: %s\n", text);
  fflush(stdout);
#ifdef HAVE_SDL
  show_progress(text);
#endif
}

// The self-test runs the API smoke test and at most SELFTEST_GAMES games
// (the bundled ones first), each until SELFTEST_MS have passed after its
// first SELFTEST_MIN_FRAMES frames, or for 300 frames. Running every game in
// full took minutes with heavy games in QuickJS (rubble. alone: 43 s on RK3566).
#define SELFTEST_GAMES 6
#define SELFTEST_MS 1500
#define SELFTEST_MIN_FRAMES 30
static const char *bundled_games[] = {"brr3kb.url", "shooter.url", "glyph-quest.url",
                                      "jump.url",   "snake.url",   "chiptune.url"};

// Runs one game silently for up to `frames` frames, or until `max_ms` have
// passed after SELFTEST_MIN_FRAMES. Returns NULL or an error message.
static const char *selftest_game(const char *label, const char *source, int frames, double max_ms,
                                 char *err, size_t errlen) {
  JSContext *ctx = vm_new(current_options);
  if (!ctx) {
    snprintf(err, errlen, "%s: engine failed to load", label);
    return err;
  }
  const long before = raster_calls;
  const double t0 = now_ms();
  int failed = call_start(ctx, source, NULL), ran = 0;
  for (; !failed && ran < frames; ran++) {
    if (ran >= SELFTEST_MIN_FRAMES && now_ms() - t0 > max_ms) break;
    failed = call_frame(ctx, ((ran / 15) % 2 ? 16 : 0) | (1 << ((ran / 40) % 4)));
  }
  if (failed) snprintf(err, errlen, "%s: %s", label, take_error(ctx));
  else if (raster_calls - before != ran + 1) {
    snprintf(err, errlen, "%s: render not native", label);
    failed = 1;
  }
  vm_free(ctx);
  return failed ? err : NULL;
}

static const char *selftest(const char *dir, char *err, size_t errlen) {
  // The menu's runtime keeps running: stash its entry points.
  const JSValue keep[3] = {start_fn, frame_fn, error_fn};
  const char *saved_dir = module_dir;
  module_dir = dir;
  mute_sounds = 1;
  // Bundled games first, then the rest in menu order.
  int order[MAX_GAMES], tests = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < game_count && tests < SELFTEST_GAMES; i++) {
      const char *base = strrchr(game_paths[i], '/');
      base = base ? base + 1 : game_paths[i];
      int bundled = 0;
      for (size_t b = 0; b < sizeof bundled_games / sizeof *bundled_games; b++)
        bundled |= !strcmp(base, bundled_games[b]);
      if (bundled == !pass) order[tests++] = i;
    }
  char step[24];
  snprintf(step, sizeof step, "TEST 1/%d", tests + 1);
  update_progress(step);
  const char *result = selftest_game("SMOKE", smoke_game, 120, 1e9, err, errlen);
  if (!result) {
    // "SELF TEST" is drawn in ink 7 on text row 1; glyphs must actually show up.
    int ink = 0;
    for (int y = 8; y < 16; y++)
      for (int x = 8; x < 88; x++) ink += framebuffer[y * W + x] == 7;
    if (ink < 30) {
      snprintf(err, errlen, "SMOKE: text not drawn");
      result = err;
    }
  }
  for (int t = 0; !result && t < tests; t++) {
    const int i = order[t];
    snprintf(step, sizeof step, "TEST %d/%d", t + 2, tests + 1);
    update_progress(step);
    char load_err[128];
    char *source = load_game(game_paths[i], load_err, sizeof load_err);
    if (!source) continue;  // a broken URL file is not the engine's fault
    const char *base = strrchr(game_paths[i], '/');
    result = selftest_game(base ? base + 1 : game_paths[i], source, 300, SELFTEST_MS, err, errlen);
    free(source);
  }
  mute_sounds = 0;
  module_dir = saved_dir;
  start_fn = keep[0];
  frame_fn = keep[1];
  error_fn = keep[2];
  return result;
}

// Returns a short status line for the menu (at most 18 characters).
static const char *update_engine(char *msg, size_t len) {
  char json[16384], tag[64] = "", sha[64] = "", cmd[2048];
  memcpy(update_screen, framebuffer, sizeof update_screen);
  const char *curl = curl_command();
  snprintf(cmd, sizeof cmd, "%s -fsSL -m 20 https://api.github.com/repos/kitao/playload/tags",
           curl);
  if (run_capture(cmd, json, sizeof json) != 0) return snprintf(msg, len, "NO CONNECTION"), msg;
  const char *after = json_string(json, "name", tag, sizeof tag);
  if (!after || !json_string(after, "sha", sha, sizeof sha) || !valid_name(tag, 40) ||
      strlen(sha) != 40)
    return snprintf(msg, len, "NO RELEASE FOUND"), msg;
  printf("latest release %s (%s)\n", tag, sha);
  char dir[256];
  snprintf(dir, sizeof dir, ENGINE_ROOT "/%s", tag);
  if (engine_complete(dir)) {
    const int same = !strcmp(engine_name, tag);
    select_engine(tag);
    save_config();
    return snprintf(msg, len, "%s %s", same ? "LATEST" : "USING", tag), msg;
  }
  static const char *sources[4] = {"src/engine/engine.js", "src/engine/sound.js",
                                   "src/engine/glyphs.js", "src/app/crt.js"};
  run_capture("rm -rf " ENGINE_ROOT "/.new && mkdir -p " ENGINE_ROOT "/.new", NULL, 0);
  // One curl for all four files, so they share one connection.
  update_progress("DOWNLOAD");
  int n = snprintf(cmd, sizeof cmd, "%s -fsSL -m 60", curl);
  for (int i = 0; i < 4; i++)
    n += snprintf(cmd + n, sizeof cmd - (size_t)n,
                  " -o " ENGINE_ROOT "/.new/%s https://raw.githubusercontent.com/kitao/playload/%s/%s",
                  engine_files[i], sha, sources[i]);
  if (n >= (int)sizeof cmd || run_capture(cmd, NULL, 0) != 0)
    return snprintf(msg, len, "DOWNLOAD FAILED"), msg;
  FILE *v = fopen(ENGINE_ROOT "/.new/VERSION", "w");
  if (v) {
    fprintf(v, "%s %s\n", tag, sha);
    fclose(v);
  }
  char err[512];
  if (selftest(ENGINE_ROOT "/.new", err, sizeof err)) {
    printf("self-test failed for %s: %s\n", tag, err);
    return snprintf(msg, len, "%s TEST FAILED", tag), msg;
  }
  snprintf(cmd, sizeof cmd, "rm -rf %s && mv " ENGINE_ROOT "/.new %s", dir, dir);
  if (run_capture(cmd, NULL, 0) != 0) return snprintf(msg, len, "INSTALL FAILED"), msg;
  select_engine(tag);
  save_config();
  printf("installed %s\n", tag);
  return snprintf(msg, len, "UPDATED %s", tag), msg;
}

static JSValue js_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  char msg[64];
  return JS_NewString(ctx, update_engine(msg, sizeof msg));
}

// ---------------------------------------------------------------------------
// Playload Arcade: the curated gallery's list (data/arcade.json in Playload's
// repository), saving its games into games/, and a live preview of a game.

#define ARCADE_DIR "arcade"
#define ARCADE_LIST ARCADE_DIR "/arcade.json"
#define ARCADE_URL "https://raw.githubusercontent.com/kitao/playload/main/data/arcade.json"

// __native.arcade(cachedOnly) -> {text, online}: downloads the list (kept for
// offline use) unless cachedOnly; text is null when there is no list at all.
static JSValue js_arcade(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t cached_only = 0;
  if (argc > 0) JS_ToInt32(ctx, &cached_only, argv[0]);
  int online = 0;
  if (!cached_only) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "mkdir -p " ARCADE_DIR " && %s -fsSL -m 20 -o " ARCADE_LIST ".new " ARCADE_URL
             " && mv " ARCADE_LIST ".new " ARCADE_LIST,
             curl_command());
    online = run_capture(cmd, NULL, 0) == 0;
    printf("arcade list %s\n", online ? "downloaded" : "not downloaded");
  }
  char *text = read_file(ARCADE_LIST, NULL);
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "text", text ? JS_NewString(ctx, text) : JS_NULL);
  JS_SetPropertyStr(ctx, result, "online", JS_NewBool(ctx, online));
  free(text);
  return result;
}

// The #v1. payload of each game, by menu index ("" for plain JS files).
static JSValue js_payloads(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue list = JS_NewArray(ctx);
  for (int i = 0; i < game_count; i++) {
    char *text = read_file(game_paths[i], NULL);
    const char *p = text ? strstr(text, "#v1.") : NULL;
    size_t n = 0;
    if (p)
      while (b64((unsigned char)p[4 + n]) >= 0) n++;
    JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_NewStringLen(ctx, p ? p + 4 : "", n));
    free(text);
  }
  return list;
}

static void sort_games(void) {
  for (int i = 0; i < game_count; i++) free(game_titles[i]);
  qsort(game_paths, (size_t)game_count, sizeof *game_paths, cmp_str);
  for (int i = 0; i < game_count; i++) game_titles[i] = title_of(game_paths[i]);
}

// __native.save(name, url) -> menu index of the new game in games/, or -1.
// Refreshes __titles. An existing file of that name is kept (name-2.url, ...).
static JSValue js_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *name = JS_ToCString(ctx, argv[0]), *url = JS_ToCString(ctx, argv[1]);
  int index = -1;
  char path[1024] = "";
  if (name && url && games_dir[0] && valid_name(name, 60) && strstr(url, "#v1.") &&
      game_count < MAX_GAMES) {
    for (int k = 1; k < 100; k++) {
      if (k == 1) snprintf(path, sizeof path, "%s/%s.url", games_dir, name);
      else snprintf(path, sizeof path, "%s/%s-%d.url", games_dir, name, k);
      if (access(path, F_OK) != 0) break;
    }
    FILE *f = fopen(path, "wbx");
    if (f) {
      const int ok = fprintf(f, "%s\n", url) > 0;
      if (fclose(f) == 0 && ok) {
        game_paths[game_count++] = strdup(path);
        game_titles[game_count - 1] = NULL;
        sort_games();
        for (int i = 0; i < game_count; i++)
          if (!strcmp(game_paths[i], path)) index = i;
        set_titles(ctx);
        printf("saved %s\n", path);
      }
    }
  }
  JS_FreeCString(ctx, name);
  JS_FreeCString(ctx, url);
  return JS_NewInt32(ctx, index);
}

static JSValue js_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if (argc > 0 && JS_IsString(argv[0])) {
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) snprintf(menu_state, sizeof menu_state, "%s", s);
    JS_FreeCString(ctx, s);
  }
  return JS_NewString(ctx, menu_state);
}

// The preview runs the game silently in its own QuickJS runtime on its own
// thread, at up to 60 ticks per second with no input, so a heavy game only
// runs slower in the preview and never holds up the menu.
#define PREVIEW_X 40
#define PREVIEW_Y 24
static pthread_t preview_thread;
static int preview_running;
static volatile int preview_quit;
static pthread_mutex_t preview_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t preview_pixels[W * H];  // the latest frame, under preview_lock
static int preview_ready;

static int preview_interrupt(JSRuntime *rt, void *opaque) { return preview_quit; }

static void *preview_main(void *arg) {
  char *source = arg;
  uint8_t *own = calloc(W * H, 1);
  raster_target = own;
  mute_sounds = 1;
  JSContext *ctx = own ? vm_new(current_options) : NULL;
  if (ctx) {
    // Stop a long tick (or an endless loop) as soon as the preview closes.
    JS_SetInterruptHandler(JS_GetRuntime(ctx), preview_interrupt, NULL);
    int failed = call_start(ctx, source, NULL);
    double next = now_ms();
    while (!failed && !preview_quit) {
      pthread_mutex_lock(&preview_lock);
      memcpy(preview_pixels, own, sizeof preview_pixels);
      preview_ready = 1;
      pthread_mutex_unlock(&preview_lock);
      next += 1000.0 / 60;
      const double now = now_ms();
      if (next > now) usleep((useconds_t)((next - now) * 1000));
      else next = now;  // behind: a heavy game just runs slower
      failed = call_frame(ctx, 0);
    }
    if (failed && !preview_quit) printf("preview: %s\n", take_error(ctx));
    vm_free(ctx);
  }
  free(own);
  free(source);
  return NULL;
}

static void preview_stop(void) {
  if (!preview_running) return;
  preview_quit = 1;
  pthread_join(preview_thread, NULL);
  preview_running = 0;
  preview_ready = 0;
}

// __native.preview(url): shows that game; "" stops the preview.
static JSValue js_preview(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  preview_stop();
  const char *url = JS_ToCString(ctx, argv[0]);
  char err[128];
  char *source = url && url[0] ? decode_game(strdup(url), err, sizeof err) : NULL;
  JS_FreeCString(ctx, url);
  if (source) {
    preview_quit = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20);  // QuickJS is allowed 4 MB of stack
    preview_running = pthread_create(&preview_thread, &attr, preview_main, source) == 0;
    pthread_attr_destroy(&attr);
    if (!preview_running) free(source);
  }
  return JS_UNDEFINED;
}

// Called after each menu tick: draws the preview's latest frame over the menu,
// halved (of each 2x2 block, the first non-black pixel, so thin lines survive).
static void preview_frame(void) {
  if (!preview_running) return;
  pthread_mutex_lock(&preview_lock);
  const int ready = preview_ready;
  for (int y = 0; ready && y < H / 2; y++)
    for (int x = 0; x < W / 2; x++) {
      const uint8_t *p = preview_pixels + y * 2 * W + x * 2;
      const uint8_t c = p[0] ? p[0] : p[1] ? p[1] : p[W] ? p[W] : p[W + 1];
      framebuffer[(PREVIEW_Y + y) * W + PREVIEW_X + x] = c;
    }
  pthread_mutex_unlock(&preview_lock);
  // A frame in palette color 1 sets the picture off from the menu.
  for (int x = PREVIEW_X - 1; x <= PREVIEW_X + W / 2; x++)
    framebuffer[(PREVIEW_Y - 1) * W + x] = framebuffer[(PREVIEW_Y + H / 2) * W + x] = 1;
  for (int y = PREVIEW_Y - 1; y <= PREVIEW_Y + H / 2; y++)
    framebuffer[y * W + PREVIEW_X - 1] = framebuffer[y * W + PREVIEW_X + W / 2] = 1;
}


static int run_headless_node(Options *o, const char *game) {
  NodeGame node;
  const double t0 = now_ms();
  if (node_start(&node, game, module_dir) || node_read_frame(&node, framebuffer, NULL, NULL, 15000)) {
    printf("error: %s\n", node.error);
    node_stop(&node);
    return 1;
  }
  printf("node started in %.0f ms\n", now_ms() - t0);
  double *t = malloc(sizeof(double) * (size_t)o->ticks);
  for (int i = 0; i < o->ticks; i++) {
    // Steady right/left sweeps with occasional A and B presses.
    // PLAYLOAD_NO_INPUT: no buttons at all (demos that pause on any button).
    const int mask = getenv("PLAYLOAD_NO_INPUT")
                         ? 0
                         : ((i / 40) % 2 ? 4 : 8) | (i % 7 == 0 ? 16 : 0) | (i % 90 == 0 ? 32 : 0);
    const double a = now_ms();
    if (node_frame(&node, mask, framebuffer, NULL, NULL, 5000)) {
      printf("error at tick %d: %s\n", i, node.error);
      node_stop(&node);
      return 1;
    }
    t[i] = now_ms() - a;
    if (o->dump_path && i == o->dump_at) write_ppm(o->dump_path);
  }
  node_stop(&node);
  report("node", t, o->ticks);
  free(t);
  return 0;
}

static int run_headless(JSContext *ctx, Options *o, const char *source, const char *inject) {
  if (o->audio) {
    ay_init(&ay, 48000);
    audio_open = 1;
  }
  if (call_start(ctx, source, inject)) {
    take_error(ctx);
    return 1;
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue bot = JS_GetPropertyStr(ctx, global, "__bot");
  JS_FreeValue(ctx, global);
  double *t = malloc(sizeof(double) * (size_t)o->ticks);
  double *ta = o->audio ? malloc(sizeof(double) * (size_t)o->ticks) : NULL;
  volatile float sink = 0;
  FILE *audio_file = o->audio_path ? fopen(o->audio_path, "wb") : NULL;
  const double begin = now_ms();
  const clock_t cpu0 = clock();
  for (int i = 0; i < o->ticks; i++) {
    int mask = 0;
    if (JS_IsFunction(ctx, bot)) {
      JSValue m = JS_Call(ctx, bot, JS_UNDEFINED, 0, NULL);
      if (JS_IsException(m)) {
        take_error(ctx);
        return 1;
      }
      JS_ToInt32(ctx, &mask, m);
      JS_FreeValue(ctx, m);
    }
    const double a = now_ms();
    if (call_frame(ctx, mask)) {
      printf("error at tick %d: %s\n", i, take_error(ctx));
      return 1;
    }
    t[i] = now_ms() - a;
    if (ta) {
      // One tick's worth of the game's own sound, as the audio thread would make it.
      const double b = now_ms();
      drain_sounds();
      for (int k = 0; k < 800; k++) {
        const float v = ay_sample(&ay);
        sink += v;
        if (audio_file) fwrite(&v, sizeof v, 1, audio_file);
      }
      ta[i] = now_ms() - b;
    }
    if (o->dump_path && i == o->dump_at) write_ppm(o->dump_path);
  }
  const double wall = (now_ms() - begin) / 1000, cpu = (double)(clock() - cpu0) / CLOCKS_PER_SEC;
  JS_FreeValue(ctx, bot);
  const int phases[4] = {0, 3600, 7200, o->ticks};
  const char *names[3] = {"0-60s", "60-120s", "120s-"};
  for (int p = 0; p < 3; p++) {
    const int a = phases[p], b = phases[p + 1] < o->ticks ? phases[p + 1] : o->ticks;
    if (b > a) report(names[p], t + a, b - a);
  }
  if (ta) report("audio48k", ta, o->ticks);
  if (audio_file) fclose(audio_file);
  printf("ticks %d  wall %.1fs  cpu %.1fs\n", o->ticks, wall, cpu);
  free(t);
  free(ta);
  return 0;
}

#ifdef HAVE_SDL
static void audio_callback(void *userdata, Uint8 *stream, int len) {
  drain_sounds();
  int16_t *out = (int16_t *)stream;
  const int frames = len / 4;
  for (int i = 0; i < frames; i++) {
    float v = ay_sample(&ay) * 1.4f;
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    const int16_t s = (int16_t)(v * 32767);
    out[i * 2] = out[i * 2 + 1] = s;
  }
}

// A session runs one game (or the menu) until it quits, goes back, or launches.
enum { ACT_QUIT, ACT_BACK, ACT_LAUNCH };

static int combo_held = 1;  // Select+Start must be released before it acts again

static int read_input(int *running) {
  int mask = 0, back = 0, startb = 0, shoulder = 0, left_shoulder = 0;
  static int crt_held = 0, fps_held = 0;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) *running = ACT_QUIT;
    if (e.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(e.cdevice.which);
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) *running = ACT_BACK;
  }
  const Uint8 *k = SDL_GetKeyboardState(NULL);
  if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) mask |= 1;
  if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) mask |= 2;
  if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) mask |= 4;
  if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) mask |= 8;
  if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J]) mask |= 16;
  if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_K]) mask |= 32;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    SDL_GameController *c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
    if (!c) continue;
    const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX),
              ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -16000) mask |= 1;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 16000) mask |= 2;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -16000) mask |= 4;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 16000) mask |= 8;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) mask |= swap_ab ? 32 : 16;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) mask |= swap_ab ? 16 : 32;
    back |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK);
    startb |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START);
    shoulder |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    left_shoulder |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  }
  // SELECT+L1 toggles the performance overlay.
  const int fps_combo = back && left_shoulder;
  if (fps_combo && !fps_held) {
    fps_on = !fps_on;
    save_config();
  }
  fps_held = fps_combo;
  // SELECT+R1 toggles the CRT view at any time.
  const int crt_combo = back && shoulder;
  if (crt_combo && !crt_held) {
    crt_on = !crt_on;
    save_config();
  }
  crt_held = crt_combo;
  const int combo = back && startb;
  if (combo && !combo_held) *running = ACT_BACK;
  combo_held = combo;
  return mask;
}

// Performance overlay: a 3x5 pixel font drawn over a copy of the frame, so the
// game's own picture is never touched.
static const char *osd_chars = "0123456789.FPSTMAX DEGILNOW/";
static const uint16_t osd_font[] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,  // 0-9
    0x0002, 0x79E4, 0x7BE4, 0x79CF, 0x7492, 0x5F6D, 0x2BED, 0x5AAD, 0x0000,          // . F P S T M A X space
    0x6B6E, 0x79E7, 0x796F, 0x7497, 0x4927, 0x6B6D, 0x7B6F, 0x5BFD, 0x12A4,          // D E G I L N O W /
};
static char osd_text[2][24];

static void osd_print(uint8_t *pix, int x0, int y0, const char *text) {
  const int len = (int)strlen(text);
  for (int y = y0 - 1; y < y0 + 6; y++)
    for (int x = x0 - 1; x < x0 + len * 4; x++) pix[y * W + x] = 0;
  for (int i = 0; i < len; i++) {
    const char *at = strchr(osd_chars, text[i]);
    const uint16_t bits = at ? osd_font[at - osd_chars] : 0;
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 3; c++)
        if (bits & (1 << (14 - r * 3 - c))) pix[(y0 + r) * W + x0 + i * 4 + c] = 7;
  }
}

static void osd_draw(uint8_t *pix, int line, const char *text) { osd_print(pix, 1, 1 + line * 7, text); }

static void present(void) {
  if (!fps_on) {
    video_present(framebuffer, crt_on);
    return;
  }
  static uint8_t shown[W * H];
  memcpy(shown, framebuffer, sizeof shown);
  osd_draw(shown, 0, osd_text[0]);
  osd_draw(shown, 1, osd_text[1]);
  video_present(shown, crt_on);
}

// The updater's progress, in the bottom right corner of the menu as it was
// when the update started (the self-test renders games into framebuffer).
static void show_progress(const char *text) {
  if (!video_ready()) return;
  static uint8_t shown[W * H];
  memcpy(shown, update_screen, sizeof shown);
  osd_print(shown, W - 1 - (int)strlen(text) * 4, 130, text);
  video_present(shown, crt_on);
  SDL_PumpEvents();
}

static void reset_audio(void) {
  if (!audio_open) return;
  SDL_LockAudioDevice(audio_device);
  while (Q.head != Q.tail) {
    ay_program_free(Q.program[Q.head]);
    Q.head = (Q.head + 1) % QUEUE;
  }
  ay_reset(&ay);
  SDL_UnlockAudioDevice(audio_device);
}

// node_game: run this game file in Node.js instead of in ctx (QuickJS).
static int run_session(JSContext *ctx, const char *source, const char *load_error,
                       const char *inject, const char *node_game) {
  int failed = 0;
  NodeGame node = {.pid = -1};
  reset_audio();
  if (!source) {
    call_start(ctx, "function tick(){}", NULL);
    show_error(ctx, load_error);
    failed = 1;
  } else if (node_game) {
    // Startup includes Node.js itself, so it gets more time than a frame.
    // Then one tick is always requested ahead: Node.js computes it while the
    // player presents and waits for vsync (one frame of input latency).
    if (node_start(&node, node_game, module_dir) ||
        node_read_frame(&node, framebuffer, node_sound, NULL, 15000) ||
        node_request(&node, 0)) {
      fprintf(stderr, "%s\n", node.error);
      // A game error is shown; Node.js itself failing (e.g. a missing library
      // on an untested device) falls back to QuickJS for this game.
      const int game_error = strstr(node.error, "exited unexpectedly") == NULL &&
                             strstr(node.error, "stopped responding") == NULL &&
                             strncmp(node.error, "pipe", 4) && strncmp(node.error, "fork", 4);
      node_stop(&node);
      if (game_error) {
        show_error(ctx, node.error);
        failed = 1;
      } else {
        printf("Node.js unavailable, running this game in QuickJS\n");
        node_game = NULL;
        if (call_start(ctx, source, inject)) {
          show_error(ctx, take_error(ctx));
          failed = 1;
        }
      }
    }
  } else if (call_start(ctx, source, inject)) {
    show_error(ctx, take_error(ctx));
    failed = 1;
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue bot = JS_GetPropertyStr(ctx, global, "__bot");
  JS_FreeValue(ctx, global);
  const int stats = getenv("PLAYLOAD_STATS") != NULL;
  const int autoplay = node_game && getenv("PLAYLOAD_AUTOPLAY") != NULL;
  int autoplay_frame = 0;
  double stat_sum = 0, stat_max = 0;
  int stat_n = 0;
  const Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();
  double acc = 0, present_last = now_ms(), present_max = 0, gpu_sum = 0;
  int action = -1, presents = 0, missed = 0;
  // Overlay counters, refreshed once per second.
  double osd_start = now_ms(), osd_sum = 0, osd_max = 0;
  int osd_ticks = 0, osd_presents = 0;
  snprintf(osd_text[0], sizeof osd_text[0], "--FPS --TPS");
  snprintf(osd_text[1], sizeof osd_text[1], "-.-MS MAX -.-");
  while (action < 0) {
    int mask = read_input(&action);
    const Uint64 nowc = SDL_GetPerformanceCounter();
    acc += (double)(nowc - last) / freq;
    last = nowc;
    // Keep tick() at 60 Hz; after a long stall, drop time instead of catching up.
    if (acc > 5.0 / 60) acc = 1.0 / 60;
    while (acc >= 1.0 / 60 - 0.002 && action < 0) {
      acc -= 1.0 / 60;
      if (failed) continue;
      // PLAYLOAD_AUTOPLAY (measurements): A held 10 frames of every 60, and the
      // d-pad cycling up/right/down/left every 45 frames.
      if (autoplay) {
        static const int pad[4] = {1, 8, 2, 4};
        mask |= (autoplay_frame % 60 < 10 ? 16 : 0) | pad[(autoplay_frame / 45) % 4];
        autoplay_frame++;
      }
      const double a = now_ms();
      if (JS_IsFunction(ctx, bot)) {
        JSValue m = JS_Call(ctx, bot, JS_UNDEFINED, 0, NULL);
        int32_t b = 0;
        if (!JS_IsException(m)) JS_ToInt32(ctx, &b, m);
        JS_FreeValue(ctx, m);
        mask |= b;
      }
      if (node_game) {
        if (node_read_frame(&node, framebuffer, node_sound, NULL, 3000) ||
            node_request(&node, mask)) {
          fprintf(stderr, "%s\n", node.error);
          show_error(ctx, node.error);
          failed = 1;
        }
      } else if (call_frame(ctx, mask)) {
        show_error(ctx, take_error(ctx));
        failed = 1;
      } else {
        preview_frame();  // the menu's Arcade preview, if any
      }
      const double ms = now_ms() - a;
      // Playload 0.9.4+ waits a frame per exceeded sprite/PCG limit, as its
      // browser player does; sound keeps playing, and the 60 Hz schedule
      // catches up afterwards.
      const int wait = failed ? 0 : node_game ? node.delay : tick_delay;
      if (wait) SDL_Delay((Uint32)(wait * 1000 / 60));
      osd_ticks++;
      osd_sum += ms;
      if (ms > osd_max) osd_max = ms;
      stat_sum += ms;
      if (ms > stat_max) stat_max = ms;
      if (++stat_n == 300) {
        if (stats)
          printf("frame avg %.2f max %.2f ms | present %d, over 20ms %d, max %.1f ms | gpu %.2f ms | crt %s\n",
                 stat_sum / stat_n, stat_max, presents, missed, present_max,
                 presents ? gpu_sum / presents : 0, crt_on ? "on" : "off");
        gpu_sum = 0;
        presents = missed = 0;
        present_max = 0;
        fflush(stdout);
        stat_sum = stat_max = 0;
        stat_n = 0;
      }
      if (pending_launch >= 0) action = ACT_LAUNCH;
    }
    present();
    osd_presents++;
    if (now_ms() - osd_start >= 1000) {
      const double secs = (now_ms() - osd_start) / 1000;
      snprintf(osd_text[0], sizeof osd_text[0], "%dFPS %dTPS", (int)(osd_presents / secs + 0.5),
               (int)(osd_ticks / secs + 0.5));
      snprintf(osd_text[1], sizeof osd_text[1], "%.1fMS MAX %.1f", osd_ticks ? osd_sum / osd_ticks : 0,
               osd_max);
      osd_start = now_ms();
      osd_sum = osd_max = 0;
      osd_ticks = osd_presents = 0;
    }
    gpu_sum += video_gpu_ms;
    const double t = now_ms(), gap = t - present_last;
    present_last = t;
    presents++;
    if (gap > 20) missed++;
    if (gap > present_max) present_max = gap;
  }
  JS_FreeValue(ctx, bot);
  node_stop(&node);
  return action;
}

static int sdl_open(void) {
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return -1;
  }
  const char *fit = getenv("PLAYLOAD_SCALE");
  if (video_open(palette, fit && !strcmp(fit, "fit"))) return -1;
  select_engine(engine_name);  // loads that version's CRT shader

  SDL_AudioSpec want = {0}, have;
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  want.callback = audio_callback;
  audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (audio_device) {
    ay_init(&ay, have.freq);
    audio_open = 1;
    SDL_PauseAudioDevice(audio_device, 0);
    printf("audio %d Hz, %d samples\n", have.freq, have.samples);
  } else {
    fprintf(stderr, "no audio: %s\n", SDL_GetError());
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  return 0;
}

static int run_sdl(Options *o) {
  if (sdl_open()) return 1;
  const int has_menu = game_count != 1;
  int current = has_menu ? -1 : 0;
  for (;;) {
    JSContext *ctx = vm_new(o);
    if (!ctx) return 1;
    char err[256] = "";
    char *source;
    if (current < 0) {
      set_titles(ctx);
      source = strdup(menu_source());
    } else {
      source = load_game(game_paths[current], err, sizeof err);
      printf("playing %s\n", game_paths[current]);
      fflush(stdout);
    }
    pending_launch = -1;
    char *inject = current >= 0 && o->inject_path ? read_file(o->inject_path, NULL) : NULL;
    const int on_node = use_node && current >= 0 && source;
    if (current >= 0) printf("runtime %s\n", on_node ? "node" : "quickjs");
    const int action =
        run_session(ctx, source, err, inject, on_node ? game_paths[current] : NULL);
    free(inject);
    free(source);
    preview_stop();
    vm_free(ctx);
    if (action == ACT_LAUNCH) {
      current = last_game = pending_launch;
      save_config();
      continue;
    }
    if (action == ACT_BACK && current >= 0 && has_menu) {
      current = -1;
      continue;
    }
    break;
  }
  if (audio_open) SDL_CloseAudioDevice(audio_device);
  SDL_Quit();
  return 0;
}
#endif

int main(int argc, char **argv) {
  Options o = {.ticks = 600, .dump_at = -1};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--headless")) o.headless = 1;
    else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) o.ticks = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--inject") && i + 1 < argc) o.inject_path = argv[++i];
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc) o.dump_path = argv[++i];
    else if (!strcmp(argv[i], "--dump-at") && i + 1 < argc) o.dump_at = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      o.seed = atoi(argv[++i]);
      o.has_seed = 1;
    } else if (!strcmp(argv[i], "--audio")) o.audio = 1;
    else if (!strcmp(argv[i], "--engine") && i + 1 < argc) o.engine = argv[++i];
    else if (!strcmp(argv[i], "--update")) o.update = 1;
    else if (!strcmp(argv[i], "--version")) o.version = 1;
    else if (!strcmp(argv[i], "--runtime") && i + 1 < argc) o.runtime = argv[++i];
    else if (!strcmp(argv[i], "--menu-dump") && i + 1 < argc) o.menu_dump = argv[++i];
    else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) o.selftest_dir = argv[++i];
    else if (!strcmp(argv[i], "--dump-audio") && i + 1 < argc) {
      o.audio = 1;
      o.audio_path = argv[++i];
    } else o.game = argv[i];
  }
  if (o.version) {
    printf("Playload Handheld %s (QuickJS-ng %s)\n", PLAYLOAD_HANDHELD_VERSION, JS_GetVersion());
    return 0;
  }
  if (!o.game) {
    fprintf(stderr, "usage: %s [--headless] <game.url|game.js|directory>\n", argv[0]);
    return 2;
  }
  printf("Playload Handheld %s\n", PLAYLOAD_HANDHELD_VERSION);
  find_games(o.game);
  current_options = &o;
  node_set_raster(node_raster);
  load_config();
  if (o.engine) select_engine(o.engine);
  if (o.menu_dump) {
    // Development aid: draw the menu headlessly into a PPM image.
    JSContext *ctx = vm_new(&o);
    set_titles(ctx);
    char *inject = o.inject_path ? read_file(o.inject_path, NULL) : NULL;
    if (call_start(ctx, menu_source(), inject)) printf("%s\n", take_error(ctx));
    free(inject);
    // With --inject, __bot() supplies the buttons for --ticks frames.
    JSValue global = JS_GetGlobalObject(ctx), bot = JS_GetPropertyStr(ctx, global, "__bot");
    JS_FreeValue(ctx, global);
    const int frames = JS_IsFunction(ctx, bot) ? o.ticks : 3;
    for (int i = 0; i < frames; i++) {
      int32_t mask = 0;
      if (JS_IsFunction(ctx, bot)) {
        JSValue m = JS_Call(ctx, bot, JS_UNDEFINED, 0, NULL);
        JS_ToInt32(ctx, &mask, m);
        JS_FreeValue(ctx, m);
      }
      if (call_frame(ctx, mask)) printf("%s\n", take_error(ctx));
      else preview_frame();
      if (preview_running) usleep(16667);  // the preview runs in real time
    }
    JS_FreeValue(ctx, bot);
    write_ppm(o.menu_dump);
    preview_stop();
    vm_free(ctx);
    return 0;
  }
  if (o.selftest_dir) {
    char err[512];
    const char *failure = selftest(o.selftest_dir, err, sizeof err);
    printf("self-test %s: %s\n", o.selftest_dir, failure ? failure : "passed");
    return failure ? 1 : 0;
  }
  if (o.update) {
    char msg[64];
    printf("%s\n", update_engine(msg, sizeof msg));
    return 0;
  }
  int rc;
  char *source = NULL, *inject = NULL;
  if (o.headless) {
    if (game_count < 1) {
      fprintf(stderr, "no game found in %s\n", o.game);
      return 1;
    }
    char err[256] = "";
    source = load_game(game_paths[0], err, sizeof err);
    inject = o.inject_path ? read_file(o.inject_path, NULL) : NULL;
    if (!source) {
      fprintf(stderr, "%s\n", err);
      return 1;
    }
    if (o.runtime && !strcmp(o.runtime, "node")) return run_headless_node(&o, game_paths[0]);
    JSContext *ctx = vm_new(&o);
    if (!ctx) return 1;
    rc = run_headless(ctx, &o, source, inject);
  } else {
#ifdef HAVE_SDL
    rc = run_sdl(&o);
#else
    fprintf(stderr, "built without SDL; use --headless\n");
    rc = 2;
#endif
  }
  free(source);
  free(inject);
  return rc;
}
