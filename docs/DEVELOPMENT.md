# Playload Handheld — developer guide

[日本語](DEVELOPMENT.ja.md)

This document describes how Playload Handheld is built, how it behaves, and the
performance work behind it. User documentation is in the [README](../README.md);
measurements are in [BENCHMARKS.md](BENCHMARKS.md).

## Goals

1. Run any Playload game URL on a 64-bit ARM handheld, offline, at 60 fps where the hardware allows.
2. Behave exactly like the browser: same pixels, same sound.
3. Follow Playload's development without forking its engine: Playload's own `engine.js`, `sound.js`, `glyphs.js` and `crt.js` are used as published, and can be updated from inside the app.

## Architecture

```
playload.aarch64 (C, one process)                          node (child process, optional)
┌──────────────────────────────────────────────────────┐   ┌──────────────────────────────┐
│ SDL2: window, controller, audio device                │   │ node-host.mjs                │
│ video.c: OpenGL ES presenter (flat / CRT shader)      │   │  └─ Playload engine.js,      │
│ ay.c: AY-3-8910 synth on the audio thread             │   │     sound.js, glyphs.js      │
│ raster: 160×144 rasterizer                            │◀─▶│     (render() patched)       │
│ inflate.c: Playload URL decoding                      │   │  game: tick() at 60 Hz       │
│ QuickJS-ng: menu, error screen, updater self-test,    │   └──────────────────────────────┘
│             and games when RUNTIME = QUICKJS          │      pipes: requests on fd 3,
│   └─ host.js + patch.js + the same Playload files     │             replies on fd 4
└──────────────────────────────────────────────────────┘
```

The design rule that came out of the measurements: **game logic runs in JavaScript (Playload's code), everything per-pixel or per-sample runs natively.** On these SoCs JavaScript is fast enough for game logic but not for rasterizing 23,040 pixels or synthesizing 48,000 oversampled audio samples per second.

### One frame

- **QuickJS runtime**: input → `engine.step(tick)` → `engine.pixels` (patched `render()` hands cells and sprites to the native rasterizer) → sounds queued for the audio thread → GPU present. All sequential in one thread.
- **Node.js runtime**: the player always keeps one tick requested ahead. While it presents frame *n* and waits for vsync, Node.js computes tick *n+1*. The player then reads the reply (screen description + sounds), rasterizes it and requests the next tick with the current buttons. This overlaps JavaScript with the GPU and vsync at the cost of one frame of input latency.

Ticks run on a fixed 60 Hz accumulator; after a stall of more than five ticks the player drops time instead of catching up, like Playload's browser runtime.

## Source layout

| Path | Contents |
|---|---|
| `src/player.c` | Main program: options, game list, config, QuickJS host, sessions, input, headless mode, engine updater, Arcade (list, saving, preview) |
| `src/noderun.c` | Node.js child process: start, request/reply protocol, timeouts |
| `src/video.c` | OpenGL ES presenter: flat view, CRT shader (built from Playload's `crt.js`), diagnostics |
| `src/ay.c` | AY-3-8910 model ported from Playload's `sound.js` |
| `src/inflate.c` | Raw DEFLATE decoder (puff-style) for Playload URLs |
| `js/playload/` | Playload's `engine.js`, `sound.js`, `glyphs.js`, `crt.js` — unmodified; `VERSION` records the upstream commit |
| `js/patch.js` | QuickJS: load-time patches, `Intl.Segmenter` replacement, caches, `console` |
| `js/host.js` | QuickJS: glue between the player and Playload's engine |
| `js/menu.js` | The menu, itself a Playload game (main view, Arcade list and game views) |
| `js/node-host.mjs` | Node.js: game host, load-time patches, screen/sound serialization |
| `port/` | `Playload.sh` (PortMaster launch script) and bundled games |
| `vendor/` | QuickJS-ng amalgamation, SDL 2 headers and a link-time copy of `libSDL2` |
| `tools/` | `embed.py` (embeds JS into the binary), `package.sh` (release archive), `dev/` (bench bot, reference renderer) |

## Specifications

### Game files

- `games/*.url`: any text containing a Playload URL. The player takes the part after `#v1.` (or the legacy `#1.`), decodes base64url, and inflates raw DEFLATE (up to 1 MiB of source). Files without `#` are treated as JavaScript source (`.js`).
- With one game the player starts it directly; with several it shows the menu. Titles come from the game's `// @title` line, else the file name.

### Display

- Playload's 160×144 picture with its 8-color palette, scaled by the largest integer that fits (3× on 640×480, centered). `PLAYLOAD_SCALE=fit` fills the screen instead.
- Presentation uses OpenGL ES directly (not the SDL renderer), so the same code draws the flat view and the CRT view.
- **CRT view**: the fragment and vertex shaders are taken from Playload's `crt.js` at run time (template literals `vertexSource` / `fragmentSource`), so they follow engine updates. If the GL context is ES 3, the shader's `fetch()` is replaced so it reads an sRGB8 texture of palette colors; otherwise the original palette-index path is used. A `crt.js` that cannot be parsed or compiled keeps the previous shader.

### Input

Playload's six buttons map to a mask (up 1, down 2, left 4, right 8, A 16, B 32). Controller d-pad and left stick (threshold 16000), A/B (swapped when `BUTTONS = XBOX`), and the keyboard (arrows/WASD, Z/J, X/K) all feed it. SELECT+START goes back to the menu (or quits), SELECT+R1 toggles CRT, SELECT+L1 the overlay; each acts once per press.

### Sound

`play()` scores are compiled by Playload's `compileScore` (in JavaScript), flattened to `[op, a, b]` triplets and queued to the audio thread, where `ay.c` plays them: three square-wave channels, shared 17-bit noise and envelope, 8× oversampling and a 257-tap windowed-sinc filter, exactly as `sound.js`. Output is 48 kHz (or what the device grants), duplicated to stereo. Unknown ops from a newer `sound.js` are skipped.

### Settings (`playload.cfg`)

| Key | Values | Default |
|---|---|---|
| `buttons` | `nintendo`, `xbox` | `nintendo` |
| `crt` | `on`, `off` | `off` |
| `fps` | `on`, `off` | `off` |
| `runtime` | `node`, `quickjs` | `node` if `node/node` exists |
| `engine` | `builtin` or a directory name in `engine/` | `builtin` |
| `last` | file name of the last game played | first game |

Environment overrides: `PLAYLOAD_BUTTONS`, `PLAYLOAD_CRT`, `PLAYLOAD_FPS`, `PLAYLOAD_RUNTIME`, `PLAYLOAD_SCALE`.

### The menu

The menu is itself a Playload game (`js/menu.js`, embedded like the engine files) run by QuickJS. It talks to the player through `__native`: `launch(i)`, `layout(v)`, `crt(v)`, `runtime(v)`, `engine(name)`, `update()`, and for Arcade `arcade(cachedOnly)`, `payloads()`, `save(name, url)`, `preview(url)`, `state(s)`. Each game then runs in a fresh JavaScript runtime, so nothing leaks between games.

**Arcade.** The list is Playload's `data/arcade.json` (title, author, author URL, game URL, screenshot path), downloaded from `raw.githubusercontent.com/kitao/playload/main/` with the updater's `curl` and kept in `arcade/arcade.json` for offline use; no HTML is scraped. Games are saved as `games/<screenshot name>.url` holding the full URL (relative URLs get `https://kitao.dev`); an existing file is never overwritten. A game counts as saved when any file in `games/` holds the same `#v1.` payload as its `url` or `originalUrl`. The preview runs the game in a second, silent QuickJS runtime on its own thread (up to 60 ticks per second, no input; heavy games just run slower), and the menu draws its latest frame halved (first non-black pixel of each 2×2 block) at (40, 24). The rasterizer target, the JS entry points and the mute flag are thread-local for this; an interrupt handler stops a long tick when the preview closes. (A first version ran it on the menu's thread and froze games whose tick took over 40 ms, which left games that start dark, such as shatter., black on RK3566.) The font has kana but few kanji, so titles and author names with characters whose glyph is empty fall back to the screenshot name or X account (`drawable()` checks `pcg[c].pat`). Playing a saved game from Arcade stores `arcade <index>` with `state()`, and the menu reopens that game afterwards from the cached list.

### Runtimes

- **Node.js** (default when installed): `node/node` with `node/lib` first on `LD_LIBRARY_PATH`, then the port environment's own paths; `LD_PRELOAD` is cleared (plumOS preloads GPU and helper libraries that Node does not need).
- **QuickJS-ng** (built in): used for the menu, error screens and the updater self-test, for games when selected, and automatically for a game when Node.js fails to start (missing library, crash on start).

### Node.js protocol

Requests on the child's fd 3: `'F' <mask>` (run one tick) and `'Q'` (quit). Replies on fd 4: a type byte, a little-endian u32 length and a body.

| Reply | Body |
|---|---|
| `R` | screen description: 360 cells × (8 pattern rows, ink, paper); u16 pattern count + 8 rows each; u32 sprite count; then columns `x i16[]`, `y i16[]`, `pattern u16[]`, `col u8[]` (on-screen sprites only); then sounds; then the trailer |
| `F` | 23,040 palette indices (Playload's own renderer, used if the render patch cannot be applied); then sounds; then the trailer |
| `E` | UTF-8 error message (the player shows it on the error screen) |

Sounds: u16 count, then per sound u8 channel, u8 fixed-volume flag, u32 event count and f64 `[op, a, b]` triplets. Trailer: u8 frames to wait after this tick and u8 noise flag, followed for `R` replies with the flag set by 23,040 bytes of overflow noise to XOR over the frame (Playload 0.9.4+, see below). The first reply (after the game's top-level code and first tick) is sent unrequested. A reply has 15 s at start and 3 s per tick before the game counts as not responding. stdout and stderr go to `log.txt`.

### Engine versions and the in-app updater

- `builtin` is embedded in the binary (and copied to `node/builtin/` for Node.js). Downloaded versions live in `engine/<tag>/`.
- **ENGINE → A**: fetch the newest tag from `api.github.com/repos/kitao/playload/tags` (plumOS's `curl`, with a clean environment), download the four files at that commit into `engine/.new` (one `curl` call, one connection), run the self-test, then rename to `engine/<tag>` and select it. An existing complete version is reused.
- **Self-test**: an API smoke game (every Playload function and variable; checks that text is actually drawn) plus up to six games from `games/` (bundled games first), run silently with scripted input in QuickJS: 300 ticks each, or 1.5 s after the first 30 ticks, whichever comes first. A version passes only if nothing throws and every frame went through the native rasterizer. The menu shows `DOWNLOAD` and `TEST n/m` in the bottom right corner meanwhile.
  Running every game in full took up to minutes (13 games on RK3566: 89 s, of which rubble. alone 43 s); the bounded test takes 7 s, and the whole update about 9 s.
- Only the JavaScript side can be updated this way. Changes to Playload's sound synthesis (`ay.c`) or URL format (`inflate.c`) need a new player build.

## Load-time patches

Playload's files are never edited on disk; they are adapted when loaded. Optional patches are skipped if their anchor text is missing, so a changed upstream file runs unpatched (slower, still correct).

| Patch | Runtime | Required | Why |
|---|---|---|---|
| `render()` → native rasterizer (screen cells + sprites) | both | QuickJS: yes; Node.js: no (falls back to Playload's renderer) | Playload rasterizes every pixel in JS: 22–24 ms per frame in QuickJS, 23 ms for 10,000 sprites in Node.js on RK3566. Since 0.9.3, `pcg[chr].pat` is a Proxy that counts edits; the native renderer reads the engine's private `patterns` WeakMap instead (reading the Proxy made BRR 3KB about 3× slower in QuickJS) |
| `render()` also passes the overflow noise (`glitches`) on frames over the sprite or PCG limit; the native side XORs it over the frame | both | no (absent before 0.9.4) | Playload 0.9.4 shows overflow as noise. `step()`'s return value (frames to wait, one per exceeded limit) is honored in sessions with a sleep, as the browser player waits; sound keeps playing and the 60 Hz schedule catches up |
| `characterKey()` memoized (throws are not cached) | both | no | Every `spr()`/`put()` normalizes, matches and segments its string |
| `Intl.Segmenter` results cached per string | both | — | QuickJS has no `Intl` (replaced by an approximation); in Node.js, ICU segmentation dominated `put()`-heavy games |
| `String.prototype.normalize` cached | QuickJS | — | Same strings every tick |
| `colorPair` exposes its values array | Node.js | no | Reading colors through Playload's `Proxy` cost ~8 ms per frame for 360 cells |
| Character elements cached by string | Node.js | — | See "V8 deoptimization" below |
| A `-0` sprite created before the first tick | Node.js | — | See "V8 deoptimization" below |
| `console` → log | QuickJS | — | Browser games may call `console.log` |

## Performance work

All numbers are from real devices; see [BENCHMARKS.md](BENCHMARKS.md).

1. **Choosing the JavaScript engine.** Running Playload unmodified in QuickJS took 26–32 ms per frame on RK3566 (renderer 22–24 ms) and its AY synth needed 885 % of a core. Node.js (V8) was fast but stalled for 70–360 ms at times. The first player therefore moved rendering and sound to C and kept QuickJS (1.2–3.4 ms per frame).
2. **Using Playload's files unmodified.** To follow upstream updates, the hand-adapted engine was replaced by Playload's own files plus load-time patches (frame time rose to 3.2–4.9 ms after caching `characterKey`, segmentation and normalization).
3. **CRT on Mali.** Playload's shader looks up a palette color with a chain of comparisons for each of its 31 taps per pixel: 21.9 ms of GPU time on Mali-G52. Feeding an sRGB8 texture (decoded to linear by the GPU, the same formula `crt.js` uses) made it 7.0 ms, pixels within 2/255.
4. **Node.js runtime for heavy games.** A game drawing ~10,000 sprites per frame (AFTER LOAD HD) ran at 6–10 fps in QuickJS. In Node.js game logic took 2.6 ms, but the frame rate decayed from fast to ~10 fps after a few seconds. V8 traces showed why:
   - **V8 deoptimization by shape changes.** Playload creates character elements lazily; whenever a new character first appeared, its character table changed shape and V8 discarded optimized code that touched it, eventually giving up. The renderer now caches elements by string and never touches that table in its loops.
   - **`-0` coordinates.** The first sprite at `x = -0` (from `Math.trunc(-0.3)`) changed the sprite objects' field representation mid-game and aborted TurboFan. Creating one such sprite before the first tick fixes the shape up front.
   - Result: the render bridge went from 36 ms to 2.6 ms; AFTER LOAD HD from 52 ms (QuickJS) to 7.1 ms per frame.
5. **ICU in `put()`-heavy games.** 10SEC RACE calls `put()` ~250 times per frame; Intl.Segmenter (native ICU, no JIT) dominated. Caching segments and `characterKey` took it from 19.6 ms to 7.7 ms.
6. **Pipelining.** rubble. needs ~12 ms of JavaScript per frame. Done serially with GPU work and vsync, a frame occasionally missed vsync, then catch-up ticks made the next frames later still: ~20 fps with 60 ticks per second. Requesting the next tick before presenting overlaps the two: 60 fps.
7. **Measure first.** Most of these problems did not show on the Mac at all (V8 deoptimization, ICU cost, GPU cost). The tools below exist because of that.

## Devices

| plumOS variant | SoC / GPU | Ports folder | plumOS root | Notes |
|---|---|---|---|---|
| Bubble | RK3566 / Mali-G52 (libmali, GLES 3.2) | `/storage/user/Roms/PORTS` | `/storage/plumos` | `plumos-cpu-control` for the governor |
| Pixel2 | RK3326 / Mali-G31 (Mesa Panfrost, GLES 3.1) | `/roms/PORTS` | `/storage` | Portrait 480×640 panel: the launcher preloads SDL/GL rotation libraries (270°); the player draws 640×480 as usual |
| V90S | A133P / PowerVR GE8300 (GLES 3.2) | `/mnt/plumos/roms/ports` | `/mnt/plumos` | SDL video driver `mali`; Debian 12 rootfs; SSH logout kills background jobs and `/tmp` is per session |

`Playload.sh` switches the CPU governor to `performance` for the session, through `plumos-cpu-control` where present and through sysfs otherwise, and restores it on exit. The port launcher stops and restores the frontend.

## Building

Requirements: macOS or Linux with [zig](https://ziglang.org/) 0.15.x (cross compiler) and Python 3.

```bash
./build.sh            # build/playload.aarch64 (device) and build/playload-mac (headless, for development)
./build.sh aarch64    # device binary only
tools/package.sh      # dist/playload-handheld-<version>.zip
```

The device binary targets `aarch64-linux-gnu.2.36`, links SDL 2 dynamically (provided by the device) and needs only `libc`, `libm` and `libSDL2-2.0.so.0`. `tools/package.sh` downloads Node.js and the Debian libraries with pinned SHA-256 checksums.

## Testing and diagnostics

| Command / variable | Purpose |
|---|---|
| `--headless --ticks N <game>` | Run without a window and print per-tick timing (QuickJS) |
| `--headless --runtime node` | Same with Node.js (`PLAYLOAD_NODE_ROOT` points to a node root; `PLAYLOAD_NO_INPUT=1` for no buttons) |
| `--seed N --inject tools/dev/bot-brr.js` | Deterministic runs with a bot (Bit-Rate Rush 3KB) |
| `--dump-at T --dump f.ppm`, `--dump-audio f.f32` | Frame and audio dumps for comparison |
| `tools/dev/refdump.mjs` | Renders the same run with Playload's original engine in QuickJS; frames must match the player byte for byte |
| `--selftest DIR`, `--update`, `--menu-dump f.ppm` | Updater self-test, updater, menu image |
| `--version` | Version (needs the SDL library on the path) |
| `PLAYLOAD_STATS=1` | Every 300 ticks: wait time, presents, frames over 20 ms, longest frame |
| `PLAYLOAD_AUTOPLAY=1` | Scripted buttons in real sessions (A held 10 of every 60 frames, d-pad cycling) |
| `PLAYLOAD_PROFILE=1` | Node.js: tick / render / send time split |
| `PLAYLOAD_NODE_FLAGS="--trace-deopt …"`, `NODE_OPTIONS=--cpu-prof` | V8 traces and CPU profiles |
| `PLAYLOAD_GPU_TIMING=1`, `PLAYLOAD_SHOT=f.ppm PLAYLOAD_SHOT_AT=N` | GPU time per frame (diagnostic only), screen capture |

Verification used for releases: frames identical to Playload's engine at several ticks of a seeded bot run (QuickJS and Node.js), audio identical to `sound.js`'s AY sample for sample, all bundled games run, and the updater self-test passes on the newest Playload tag.

## Updating the built-in Playload

1. Copy `src/engine/engine.js`, `sound.js`, `glyphs.js` and `src/app/crt.js` of the new tag into `js/playload/` and update `js/playload/VERSION`.
2. Check that the patch anchors still exist (`function render() {` with the `patterns`, `glitches`, `spriteOverflow` and `pcgOverflow` names it passes, `const characterKey = (chr) => {`, `const pair = new Proxy(values, {`, the shader template literals and `vec3 fetch(vec2 pixel) {`).
3. Compare the AY code in `sound.js` and the shader in `crt.js` with the previous version; port any change to `src/ay.c` / `src/video.c`.
4. Run the verification above, then `tools/package.sh`.

## Known limitations

- Node.js mode adds one frame of input latency (pipelining) and shows a short hitch in the first seconds of a game while V8 compiles it.
- QuickJS approximates grapheme segmentation (base code point + combining marks, skin tones and ZWJ sequences).
- The CRT view is too heavy for Mali-G31 (RK3326) and PowerVR GE8300 (A133P) at 60 fps.
- Only plumOS has been tested.
