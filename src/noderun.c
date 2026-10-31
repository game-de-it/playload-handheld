// Runs a game in a Node.js child process (js/node-host.mjs) and exchanges one
// frame per request over two pipes: requests on the child's fd 3, replies on fd 4.
#include "noderun.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Files under the Node root: node (binary), lib/ (its shared libraries),
// host.mjs and builtin/ (the engine files of the built-in version).
const char *node_root(void) {
  const char *root = getenv("PLAYLOAD_NODE_ROOT");
  return root ? root : "node";
}

int node_available(void) {
  char path[512];
  snprintf(path, sizeof path, "%s/node", node_root());
  return access(path, X_OK) == 0;
}

static NodeRasterFn raster_fn;
void node_set_raster(NodeRasterFn fn) { raster_fn = fn; }

static int read_full(NodeGame *g, void *buf, size_t len, int timeout_ms) {
  uint8_t *p = buf;
  while (len) {
    struct pollfd pfd = {.fd = g->from, .events = POLLIN};
    const int r = poll(&pfd, 1, timeout_ms);
    if (r == 0) return -2;  // timed out
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    const ssize_t n = read(g->from, p, len);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

int node_start(NodeGame *g, const char *game_path, const char *engine_dir) {
  memset(g, 0, sizeof *g);
  g->pid = -1;
  int req[2], rep[2];
  if (pipe(req) || pipe(rep)) {
    snprintf(g->error, sizeof g->error, "pipe: %s", strerror(errno));
    return -1;
  }
  signal(SIGPIPE, SIG_IGN);
  char root[512], node[640], host[640], libs[640], game[1024], engine[1024];
  if (!realpath(node_root(), root)) snprintf(root, sizeof root, "%s", node_root());
  snprintf(node, sizeof node, "%s/node", root);
  snprintf(host, sizeof host, "%s/host.mjs", root);
  snprintf(libs, sizeof libs, "%s/lib", root);
  if (!realpath(game_path, game)) snprintf(game, sizeof game, "%s", game_path);
  if (engine_dir) {
    if (!realpath(engine_dir, engine)) snprintf(engine, sizeof engine, "%s", engine_dir);
  } else {
    snprintf(engine, sizeof engine, "%s/builtin", root);
  }
  const pid_t pid = fork();
  if (pid < 0) {
    snprintf(g->error, sizeof g->error, "fork: %s", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    // The port runs with the GPU driver preloaded; Node needs none of it.
    unsetenv("LD_PRELOAD");
    // The bundled libraries first; then what the port environment provides.
    char path[2048];
    const char *inherited = getenv("LD_LIBRARY_PATH");
    snprintf(path, sizeof path, "%s%s%s", libs, inherited && *inherited ? ":" : "",
             inherited ? inherited : "");
    setenv("LD_LIBRARY_PATH", path, 1);
    // The pipe ends may themselves be fds 3 or 4: move them out of the way first.
    const int in = fcntl(req[0], F_DUPFD, 10), out = fcntl(rep[1], F_DUPFD, 10);
    if (in < 0 || out < 0 || dup2(in, 3) < 0 || dup2(out, 4) < 0) _exit(127);
    for (int fd = 5; fd < 256; fd++) close(fd);
    // PLAYLOAD_NODE_FLAGS: extra space-separated Node.js options (diagnostics).
    char *argv[24], flags[1024] = "";
    int argc = 0;
    argv[argc++] = node;
    if (getenv("PLAYLOAD_NODE_FLAGS"))
      snprintf(flags, sizeof flags, "%s", getenv("PLAYLOAD_NODE_FLAGS"));
    for (char *tok = strtok(flags, " "); tok && argc < 20; tok = strtok(NULL, " ")) argv[argc++] = tok;
    argv[argc++] = host;
    argv[argc++] = game;
    argv[argc++] = engine;
    argv[argc] = NULL;
    execv(node, argv);
    fprintf(stderr, "cannot run %s: %s\n", node, strerror(errno));
    _exit(127);
  }
  close(req[0]);
  close(rep[1]);
  g->pid = pid;
  g->to = req[1];
  g->from = rep[0];
  fcntl(g->to, F_SETFD, FD_CLOEXEC);
  fcntl(g->from, F_SETFD, FD_CLOEXEC);
  return 0;
}

// Reads one reply. Returns 0 for a frame, -1 with g->error set otherwise.
int node_read_frame(NodeGame *g, uint8_t *pixels, NodeSoundFn on_sound, void *user,
                    int timeout_ms) {
  uint8_t header[5];
  int r = read_full(g, header, 5, timeout_ms);
  if (r) {
    snprintf(g->error, sizeof g->error,
             r == -2 ? "The game stopped responding" : "Node.js exited unexpectedly");
    return -1;
  }
  const uint32_t len = header[1] | header[2] << 8 | header[3] << 16 | (uint32_t)header[4] << 24;
  if (len > (16u << 20)) {
    snprintf(g->error, sizeof g->error, "bad reply from Node.js");
    return -1;
  }
  uint8_t *body = malloc(len + 1);
  if (!body || read_full(g, body, len, timeout_ms)) {
    free(body);
    snprintf(g->error, sizeof g->error, "Node.js exited unexpectedly");
    return -1;
  }
  if (header[0] == 'E') {
    body[len] = 0;
    snprintf(g->error, sizeof g->error, "%s", (char *)body);
    free(body);
    return -1;
  }
  size_t at;
  if (header[0] == 'F' && len >= 160 * 144 + 2) {
    memcpy(pixels, body, 160 * 144);
    at = 160 * 144;
  } else if (header[0] == 'R' && raster_fn && len >= 3600 + 2) {
    const uint8_t *cells = body;
    const int patterns = body[3600] | body[3601] << 8;
    at = 3602 + (size_t)patterns * 8;
    if (at + 4 > len) goto bad;
    const uint32_t sprites = body[at] | body[at + 1] << 8 | body[at + 2] << 16 | (uint32_t)body[at + 3] << 24;
    const uint8_t *list = body + at + 4;  // x i16[], y i16[], pattern u16[], col u8[]
    at += 4 + (size_t)sprites * 7;
    if (at + 2 > len) goto bad;
    raster_fn(pixels, cells, body + 3602, patterns, list, (int)sprites);
  } else {
  bad:
    free(body);
    snprintf(g->error, sizeof g->error, "bad reply from Node.js");
    return -1;
  }
  const int count = body[at] | body[at + 1] << 8;
  at += 2;
  for (int i = 0; i < count && at + 6 <= len; i++) {
    const int ch = body[at], fixed = body[at + 1];
    const uint32_t events = body[at + 2] | body[at + 3] << 8 | body[at + 4] << 16 |
                            (uint32_t)body[at + 5] << 24;
    at += 6;
    if (at + (size_t)events * 24 > len) break;
    double *data = malloc(events * 24 + 1);
    memcpy(data, body + at, events * 24);  // f64 values, little-endian on both sides
    at += events * 24;
    if (on_sound) on_sound(user, ch, data, (int)events, fixed);
    free(data);
  }
  // Trailer: <u8 frames to wait> <u8 noise> [160x144 noise to XOR, 'R' replies only].
  g->delay = 0;
  if (at + 2 <= len) {
    g->delay = body[at] < 10 ? body[at] : 0;
    if (body[at + 1] && header[0] == 'R' && at + 2 + 160 * 144 <= len)
      for (int i = 0; i < 160 * 144; i++) pixels[i] ^= body[at + 2 + i] & 7;
  }
  free(body);
  return 0;
}

int node_request(NodeGame *g, int mask) {
  const uint8_t request[2] = {'F', (uint8_t)mask};
  if (write(g->to, request, 2) != 2) {
    snprintf(g->error, sizeof g->error, "Node.js exited unexpectedly");
    return -1;
  }
  return 0;
}

int node_frame(NodeGame *g, int mask, uint8_t *pixels, NodeSoundFn on_sound, void *user,
               int timeout_ms) {
  const uint8_t request[2] = {'F', (uint8_t)mask};
  if (write(g->to, request, 2) != 2) {
    snprintf(g->error, sizeof g->error, "Node.js exited unexpectedly");
    return -1;
  }
  return node_read_frame(g, pixels, on_sound, user, timeout_ms);
}

void node_stop(NodeGame *g) {
  if (g->pid <= 0) return;
  const uint8_t quit = 'Q';
  if (write(g->to, &quit, 1)) {
  }
  close(g->to);
  close(g->from);
  // Give it up to two seconds to exit (profiles are written on exit), then make sure.
  for (int i = 0; i < 200; i++) {
    if (waitpid(g->pid, NULL, WNOHANG) == g->pid) {
      g->pid = -1;
      return;
    }
    usleep(10000);
  }
  kill(g->pid, SIGKILL);
  waitpid(g->pid, NULL, 0);
  g->pid = -1;
}
