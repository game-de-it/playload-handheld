// OpenGL ES 2.0 presentation: a flat palette view and the CRT view.
// The CRT fragment shader is Playload's adaptation (src/app/crt.js, MIT) of
// Timothy Lottes' public-domain CRT shader, used unchanged.
#ifdef HAVE_SDL
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#define GL_GLES_PROTOTYPES 0
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#include <SDL_opengles2.h>

#define GL_FUNCS(X)                                                                       \
  X(PFNGLCREATESHADERPROC, glCreateShader)                                                \
  X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                \
  X(PFNGLCOMPILESHADERPROC, glCompileShader)                                              \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                  \
  X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                        \
  X(PFNGLDELETESHADERPROC, glDeleteShader)                                                \
  X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                              \
  X(PFNGLATTACHSHADERPROC, glAttachShader)                                                \
  X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)                                    \
  X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                  \
  X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                \
  X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                    \
  X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                    \
  X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                    \
  X(PFNGLBUFFERDATAPROC, glBufferData)                                                    \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                          \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                  \
  X(PFNGLGENTEXTURESPROC, glGenTextures)                                                  \
  X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                  \
  X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                              \
  X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                              \
  X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                  \
  X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                                    \
  X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)                                              \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                    \
  X(PFNGLUNIFORM1IPROC, glUniform1i)                                                      \
  X(PFNGLUNIFORM3FVPROC, glUniform3fv)                                                    \
  X(PFNGLVIEWPORTPROC, glViewport)                                                        \
  X(PFNGLCLEARCOLORPROC, glClearColor)                                                    \
  X(PFNGLCLEARPROC, glClear)                                                              \
  X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                    \
  X(PFNGLGETSTRINGPROC, glGetString)                                                      \
  X(PFNGLFINISHPROC, glFinish)                                                            \
  X(PFNGLREADPIXELSPROC, glReadPixels)

#define DECLARE(type, name) static type p_##name;
GL_FUNCS(DECLARE)
#undef DECLARE

static const char *vertex_source =
    "attribute vec2 position;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(position, 0.0, 1.0);\n"
    "  uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);\n"
    "}\n";

// Both views read the same 166x148 index texture with a black guard band.
#define FETCH_SOURCE                                                                           \
  "vec3 fetch(vec2 pixel) {\n"                                                                 \
  "  float index = texture2D(image, (pixel + vec2(3.5, 2.5)) / vec2(166.0, 148.0)).r * 255.0;\n" \
  "  if (index < 3.5) {\n"                                                                     \
  "    if (index < 1.5) return index < 0.5 ? colors[0] : colors[1];\n"                         \
  "    return index < 2.5 ? colors[2] : colors[3];\n"                                          \
  "  }\n"                                                                                      \
  "  if (index < 5.5) return index < 4.5 ? colors[4] : colors[5];\n"                           \
  "  return index < 6.5 ? colors[6] : index < 7.5 ? colors[7] : vec3(0.0);\n"                  \
  "}\n"

static const char *flat_source =
    "precision mediump float;\n"
    "uniform sampler2D image;\n"
    "uniform vec3 colors[8];\n"
    "varying vec2 uv;\n" FETCH_SOURCE
    "void main() {\n"
    "  gl_FragColor = vec4(fetch(floor(uv * vec2(160.0, 144.0))), 1.0);\n"
    "}\n";

// With the sRGB8 texture each of the 31 taps is a plain read of a linear color.
#define FETCH_LINEAR_SOURCE                                                         \
  "vec3 fetch(vec2 pixel) {\n"                                                      \
  "  return texture2D(image, (pixel + vec2(3.5, 2.5)) / vec2(166.0, 148.0)).rgb;\n" \
  "}\n"

#define CRT_HEADER                                                                                    \
  "precision highp float;\n"                                                                          \
  "uniform sampler2D image;\n"                                                                        \
  "uniform vec3 background;\n"                                                                        \
  "uniform vec3 colors[8];\n"                                                                         \
  "varying vec2 uv;\n"                                                                                \
  "const vec2 sourceSize = vec2(160.0, 144.0);\n"                                                     \
  "float toSrgb(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }\n"

static const char *crt_body =
    "float gauss(float x, float hardness) { return exp2(hardness * x * x); }\n"
    "vec3 reconstruct(vec2 p) {\n"
    "  vec2 pixel = floor(p * sourceSize);\n"
    "  vec2 d = 0.5 - fract(p * sourceSize);\n"
    "  float sharp[5];\n"
    "  float wide[7];\n"
    "  float stepX = exp2(-3.0 * d.x);\n"
    "  float inverseX = 1.0 / stepX;\n"
    "  wide[0] = 0.00008631674575031098 * inverseX * inverseX * inverseX;\n"
    "  wide[1] = 0.015625 * inverseX * inverseX;\n"
    "  wide[2] = 0.3535533905932738 * inverseX;\n"
    "  wide[3] = 1.0;\n"
    "  wide[4] = 0.3535533905932738 * stepX;\n"
    "  wide[5] = 0.015625 * stepX * stepX;\n"
    "  wide[6] = 0.00008631674575031098 * stepX * stepX * stepX;\n"
    "  float sharpTotal = 0.0;\n"
    "  float wideTotal = 0.0;\n"
    "  for (int x = 0; x < 5; x++) {\n"
    "    float weight = wide[x + 1];\n"
    "    sharp[x] = weight * weight * weight;\n"
    "    sharpTotal += sharp[x];\n"
    "  }\n"
    "  for (int x = 0; x < 7; x++) wideTotal += wide[x];\n"
    "  float narrowTotal = sharp[1] + sharp[2] + sharp[3];\n"
    "  float narrow[3];\n"
    "  for (int x = 0; x < 3; x++) narrow[x] = sharp[x + 1] / narrowTotal;\n"
    "  for (int x = 0; x < 5; x++) sharp[x] /= sharpTotal;\n"
    "  for (int x = 0; x < 7; x++) wide[x] /= wideTotal;\n"
    "  float vertical[5];\n"
    "  float center = gauss(d.y, -2.0);\n"
    "  float stepY = exp2(-4.0 * d.y);\n"
    "  float inverseY = 1.0 / stepY;\n"
    "  vertical[0] = center * 0.00390625 * inverseY * inverseY;\n"
    "  vertical[1] = center * 0.25 * inverseY;\n"
    "  vertical[2] = center;\n"
    "  vertical[3] = center * 0.25 * stepY;\n"
    "  vertical[4] = center * 0.00390625 * stepY * stepY;\n"
    "  vec3 color = vec3(0.0);\n"
    "  for (int row = -2; row <= 2; row++) {\n"
    "    float weightY = vertical[row + 2];\n"
    "    float glow = 0.13 * weightY;\n"
    "    float scan = weightY * weightY * weightY;\n"
    "    if (row == -2 || row == 2) {\n"
    "      for (int x = 0; x < 5; x++)\n"
    "        color += fetch(pixel + vec2(float(x - 2), float(row))) * sharp[x] * glow;\n"
    "    } else {\n"
    "      for (int x = 0; x < 7; x++) {\n"
    "        float weight = wide[x] * glow;\n"
    "        if (row == 0) {\n"
    "          if (x >= 1 && x <= 5) weight += sharp[x - 1] * scan;\n"
    "        } else {\n"
    "          if (x >= 2 && x <= 4) weight += narrow[x - 2] * scan;\n"
    "        }\n"
    "        color += fetch(pixel + vec2(float(x - 3), float(row))) * weight;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  return color;\n"
    "}\n"
    "vec3 mask(vec2 p) {\n"
    "  float stripe = mod(floor(p.x), 3.0);\n"
    "  if (stripe < 1.0) return vec3(1.16, 0.92, 0.92);\n"
    "  if (stripe < 2.0) return vec3(0.92, 1.16, 0.92);\n"
    "  return vec3(0.92, 0.92, 1.16);\n"
    "}\n"
    "void main() {\n"
    "  vec2 position = uv * 2.0 - 1.0;\n"
    "  vec2 warped = position * vec2(1.0 + 0.031 * position.y * position.y, 1.0 + 0.041 * position.x * position.x);\n"
    "  vec2 p = warped * 0.5 + 0.5;\n"
    "  vec2 corner = max(abs(warped * 0.98) - vec2(0.97), 0.0);\n"
    "  float glass = 1.0 - smoothstep(0.028, 0.032, length(corner));\n"
    "  float vignette = 1.0 - 0.13 * dot(position, position) * 0.5;\n"
    "  vec3 color = reconstruct(p) * mask(gl_FragCoord.xy) * vignette;\n"
    "  vec3 srgb = vec3(toSrgb(color.r), toSrgb(color.g), toSrgb(color.b));\n"
    "  vec2 edge = (abs(warped) - 1.0) * sourceSize * 0.5;\n"
    "  vec3 surround = background * clamp(max(edge.x, edge.y) + 0.5, 0.0, 1.0);\n"
    "  gl_FragColor = vec4(mix(background, srgb + surround, glass), 1.0);\n"
    "}\n";

static char crt_source[16384];
double video_gpu_ms;

static SDL_Window *window;
static SDL_GLContext context;
static GLuint programs[2];
static GLuint texture, linear_texture;
static uint8_t data[166 * 148];
// For the CRT on OpenGL ES 3: palette colors in an sRGB8 texture, which the GPU
// decodes to linear light on each read (the same conversion crt.js does on the
// CPU), so the 31 taps per pixel need no palette search.
static uint8_t rgb_data[166 * 148 * 3];
static uint8_t rgb_palette[8][3];
static int use_linear;
static int crt_linear;  // whether programs[1] reads the sRGB texture
#define GL_SRGB8_ES3 0x8C41
static int screen_w, screen_h, view_x, view_y, view_w, view_h;

static GLuint compile(GLenum type, const char *source) {
  GLuint shader = p_glCreateShader(type);
  p_glShaderSource(shader, 1, &source, NULL);
  p_glCompileShader(shader);
  GLint ok = 0;
  p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    p_glGetShaderInfoLog(shader, sizeof log, NULL, log);
    fprintf(stderr, "shader compile failed: %s\n", log);
    p_glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static const uint32_t *crt_palette;

static GLuint link_program_sources(const char *vertex, const char *fragment,
                                   const uint32_t *palette, int linear) {
  GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
  if (!vs || !fs) return 0;
  GLuint program = p_glCreateProgram();
  p_glAttachShader(program, vs);
  p_glAttachShader(program, fs);
  p_glBindAttribLocation(program, 0, "position");
  p_glLinkProgram(program);
  p_glDeleteShader(vs);
  p_glDeleteShader(fs);
  GLint ok = 0;
  p_glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    fprintf(stderr, "shader link failed\n");
    return 0;
  }
  p_glUseProgram(program);
  p_glUniform1i(p_glGetUniformLocation(program, "image"), 0);
  float colors[24];
  for (int i = 0; i < 8; i++)
    for (int k = 0; k < 3; k++) {
      const float c = ((palette[i] >> (16 - 8 * k)) & 0xff) / 255.0f;
      // The CRT works in linear light, like crt.js; the flat view shows sRGB as is.
      colors[i * 3 + k] =
          linear ? (c <= 0.04045f ? c / 12.92f : SDL_powf((c + 0.055f) / 1.055f, 2.4f)) : c;
    }
  p_glUniform3fv(p_glGetUniformLocation(program, "colors[0]"), 8, colors);
  const GLint background = p_glGetUniformLocation(program, "background");
  if (background >= 0) {
    const float black[3] = {0, 0, 0};
    p_glUniform3fv(background, 1, black);
  }
  return program;
}

static GLuint link_program(const char *fragment, const uint32_t *palette, int linear) {
  return link_program_sources(vertex_source, fragment, palette, linear);
}

int video_open(const uint32_t *palette, int fit) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_DisplayMode mode = {0};
  int sw = 640, sh = 480;
  if (!SDL_GetCurrentDisplayMode(0, &mode) && mode.w > 0) {
    sw = mode.w;
    sh = mode.h;
  }
  window = SDL_CreateWindow("Playload", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, sw, sh,
                            SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return -1;
  }
  context = SDL_GL_CreateContext(window);
  if (!context) {
    fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
    return -1;
  }
  SDL_GL_SetSwapInterval(1);
#define LOAD(type, name)                                     \
  if (!(p_##name = (type)SDL_GL_GetProcAddress(#name))) {    \
    fprintf(stderr, "missing GL function %s\n", #name);      \
    return -1;                                               \
  }
  GL_FUNCS(LOAD)
#undef LOAD
  SDL_GL_GetDrawableSize(window, &screen_w, &screen_h);
  // Integer scaling keeps every pixel the same size; fit fills the screen.
  if (fit) {
    const double k = SDL_min((double)screen_w / 160, (double)screen_h / 144);
    view_w = (int)(160 * k);
    view_h = (int)(144 * k);
  } else {
    int k = SDL_min(screen_w / 160, screen_h / 144);
    if (k < 1) k = 1;
    view_w = 160 * k;
    view_h = 144 * k;
  }
  view_x = (screen_w - view_w) / 2;
  view_y = (screen_h - view_h) / 2;
  printf("display %dx%d (%s), picture %dx%d at %d,%d\n", screen_w, screen_h,
         (const char *)p_glGetString(GL_RENDERER), view_w, view_h, view_x, view_y);

  const char *version = (const char *)p_glGetString(GL_VERSION);
  use_linear = version && strstr(version, "OpenGL ES 3") && !getenv("PLAYLOAD_CRT_PALETTE");
  const char *variant = getenv("PLAYLOAD_CRT_FETCH");
  const int plain = variant && !strcmp(variant, "plain");
  snprintf(crt_source, sizeof crt_source, "%s%s%s", CRT_HEADER,
           use_linear || plain ? FETCH_LINEAR_SOURCE : FETCH_SOURCE, crt_body);
  printf("GL %s\n", version ? version : "?");
  printf("crt input: %s\n", use_linear ? "sRGB8 texture" : "palette indices");
  crt_palette = palette;
  programs[0] = link_program(flat_source, palette, 0);
  programs[1] = link_program(crt_source, palette, 1);
  crt_linear = use_linear;
  if (!programs[0]) return -1;
  if (!programs[1]) fprintf(stderr, "CRT shader unavailable; flat view only\n");
  GLuint buffer;
  p_glGenBuffers(1, &buffer);
  p_glBindBuffer(GL_ARRAY_BUFFER, buffer);
  static const float quad[12] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
  p_glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  p_glEnableVertexAttribArray(0);
  p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  p_glGenTextures(1, &texture);
  p_glActiveTexture(GL_TEXTURE0);
  p_glBindTexture(GL_TEXTURE_2D, texture);
  p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // Index 255 is black: the guard band around the picture stays black.
  memset(data, 255, sizeof data);
  p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  p_glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 166, 148, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
  if (use_linear) {
    for (int i = 0; i < 8; i++)
      for (int k = 0; k < 3; k++) rgb_palette[i][k] = (uint8_t)(palette[i] >> (16 - 8 * k));
    memset(rgb_data, 0, sizeof rgb_data);
    p_glGenTextures(1, &linear_texture);
    p_glBindTexture(GL_TEXTURE_2D, linear_texture);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ES3, 166, 148, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
    p_glBindTexture(GL_TEXTURE_2D, texture);
  }
  return 0;
}

int video_has_crt(void) { return programs[1] != 0; }

int video_ready(void) { return programs[0] != 0; }


// Copies the JS template literal `const <name> = `...`;` out of crt.js.
static char *template_literal(const char *js, const char *name) {
  char key[64];
  snprintf(key, sizeof key, "const %s = `", name);
  const char *a = strstr(js, key);
  if (!a) return NULL;
  a += strlen(key);
  const char *b = strchr(a, '`');
  if (!b) return NULL;
  const size_t n = (size_t)(b - a);
  char *out = malloc(n + 1);
  memcpy(out, a, n);
  out[n] = 0;
  if (strstr(out, "${")) {  // interpolated shaders are not plain GLSL
    free(out);
    return NULL;
  }
  return out;
}

// Replaces crt.js's palette-search fetch() with a plain read of the sRGB texture.
static char *linear_fetch(const char *fragment) {
  const char *head = "vec3 fetch(vec2 pixel) {";
  const char *a = strstr(fragment, head);
  if (!a) return NULL;
  int depth = 0;
  const char *b = strchr(a, '{');
  for (; *b; b++) {
    if (*b == '{') depth++;
    else if (*b == '}' && --depth == 0) break;
  }
  if (!*b) return NULL;
  const char *body = FETCH_LINEAR_SOURCE;
  const size_t n = (size_t)(a - fragment), m = strlen(body), rest = strlen(b + 1);
  char *out = malloc(n + m + rest + 1);
  memcpy(out, fragment, n);
  memcpy(out + n, body, m);
  memcpy(out + n + m, b + 1, rest + 1);
  return out;
}

int video_load_crt(const char *crt_js) {
  char *vertex = template_literal(crt_js, "vertexSource");
  char *fragment = template_literal(crt_js, "fragmentSource");
  int rc = -1;
  if (vertex && fragment) {
    char *fast = use_linear ? linear_fetch(fragment) : NULL;
    GLuint program = link_program_sources(vertex, fast ? fast : fragment, crt_palette, 1);
    if (program) {
      programs[1] = program;
      crt_linear = fast != NULL;
      rc = 0;
    }
    free(fast);
  }
  free(vertex);
  free(fragment);
  return rc;
}

void video_present(const uint8_t *pixels, int crt) {
  crt = crt && programs[1];
  if (crt && crt_linear) {
    for (int y = 0; y < 144; y++) {
      uint8_t *row = rgb_data + ((y + 2) * 166 + 3) * 3;
      for (int x = 0; x < 160; x++) memcpy(row + x * 3, rgb_palette[pixels[y * 160 + x] & 7], 3);
    }
    p_glBindTexture(GL_TEXTURE_2D, linear_texture);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 166, 148, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
  } else {
    for (int y = 0; y < 144; y++) memcpy(data + (y + 2) * 166 + 3, pixels + y * 160, 160);
    p_glBindTexture(GL_TEXTURE_2D, texture);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 166, 148, GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
  }
  p_glViewport(0, 0, screen_w, screen_h);
  p_glClearColor(0, 0, 0, 1);
  p_glClear(GL_COLOR_BUFFER_BIT);
  p_glViewport(view_x, view_y, view_w, view_h);
  p_glUseProgram(programs[crt ? 1 : 0]);
  p_glDrawArrays(GL_TRIANGLES, 0, 6);
  if (getenv("PLAYLOAD_GPU_TIMING")) {
    // Diagnostics only: waiting for the GPU serializes the pipeline.
    const Uint64 a = SDL_GetPerformanceCounter();
    p_glFinish();
    video_gpu_ms = (double)(SDL_GetPerformanceCounter() - a) * 1000 / SDL_GetPerformanceFrequency();
  }
  // Diagnostics: PLAYLOAD_SHOT=file.ppm saves the screen at frame PLAYLOAD_SHOT_AT.
  static int frame = 0;
  const char *shot = getenv("PLAYLOAD_SHOT");
  if (shot && frame++ == (getenv("PLAYLOAD_SHOT_AT") ? atoi(getenv("PLAYLOAD_SHOT_AT")) : 120)) {
    static uint8_t rgba[1920 * 1080 * 4];
    if (screen_w * screen_h * 4 <= (int)sizeof rgba) {
      p_glReadPixels(0, 0, screen_w, screen_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      FILE *f = fopen(shot, "wb");
      if (f) {
        fprintf(f, "P6\n%d %d\n255\n", screen_w, screen_h);
        for (int y = screen_h - 1; y >= 0; y--)
          for (int x = 0; x < screen_w; x++) fwrite(rgba + (y * screen_w + x) * 4, 1, 3, f);
        fclose(f);
      }
    }
  }
  SDL_GL_SwapWindow(window);
}
#endif
