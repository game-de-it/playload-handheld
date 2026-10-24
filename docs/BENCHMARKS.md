# Playload Handheld benchmarks

[日本語](BENCHMARKS.ja.md)

Measured 2026-10-22 to 2026-10-24 with Playload Handheld (this repository), Playload engine v0.9.0 (built in) / v0.9.1, Node.js v24.22.1 (official linux-arm64 build) and QuickJS-ng 0.17.0.

## Devices

| Device | SoC | CPU | GPU / driver | Screen | plumOS |
|---|---|---|---|---|---|
| plumOS-Bubble | Rockchip RK3566 | 4× Cortex-A55, up to 1.99 GHz | Mali-G52, ARM libmali, OpenGL ES 3.2 | 640×480 | Bubble |
| GameKiddy GKD Pixel2 | Rockchip RK3326 | 4× Cortex-A35, up to 1.51 GHz | Mali-G31, Mesa Panfrost 22.3.6, OpenGL ES 3.1 | 480×640 (portrait panel rotated 270° and used as 640×480) | Pixel2 0.1.4 |
| V90S | Allwinner A133P | 4× Cortex-A53, up to 1.8 GHz | PowerVR Rogue GE8300, OpenGL ES 3.2 | 640×480 (~59 Hz) | V90S 1.0.2 (Debian 12 based) |

All have about 1 GB of memory and glibc 2.36.

## Method

- **CPU governor**: performance during measurements (Playload.sh switches it for the session and restores it afterwards), unless noted.
- **Real sessions**: started through plumOS's port launcher (`plumos-portmaster-port-launch`) with display and sound. `PLAYLOAD_STATS=1` prints these every 300 ticks (5 seconds):
  - **frame avg / max**: time the player waited for one game tick (close to 0 when Node.js pipelining works)
  - **present**: screen updates in 5 seconds (300 = 60 fps)
  - **over 20ms**: screen updates more than 20 ms apart (visible hitches)
  - **max (present)**: longest time between screen updates
- **Autoplay** (`PLAYLOAD_AUTOPLAY=1`): A held for 10 of every 60 frames, d-pad cycling up → right → down → left every 45 frames. It gets past title screens and keeps firing. rubble. pauses on any button, so it was measured without input.
- **Headless** (`--headless`): time per tick (game + rendering) running back to back, with a fixed input pattern (left/right alternating, A every 7 frames, B every 90 frames).
- Runs without input or with fixed patterns can be lighter than real play (for example, firing continuously).

## 1. RK3566 (Bubble)

### 1.1 Choosing the JavaScript engine (BRR 3KB, headless, 9000 ticks, ondemand governor pinned at the top clock)

Playload's `engine.js` run entirely in JavaScript:

| Setup | Game logic | Rendering (JS) | Total, average | Notes |
|---|---|---|---|---|
| QuickJS | 3.7–7.7 ms | 22–24 ms | 26–32 ms | Cannot reach 60 fps. 17 MB of memory |
| Node.js (V8) | 2–4 ms | 2.5 ms | median 4.5–6.6 ms | p99 70–130 ms, stalls up to 360 ms. 150 MB of memory |
| Node.js `--jitless` | — | — | about the same as QuickJS | V8's speed comes from its JIT |

Sound (AY-3-8910, one second at 48 kHz): 885 % of a core in QuickJS, 31 % in Node.js → **ported to C** (about 2.1 ms per tick of 800 samples).

Node.js at a real-time 60 Hz: average 9.99 ms and 77 % CPU with ondemand; 6.99 ms, p99 8.77 ms and 61 % CPU with performance.

### 1.2 QuickJS with native rendering (BRR 3KB, headless, 9000 ticks, performance)

| Setup | 0–60 s | 60–120 s | Boss fight (90 enemies) | Max |
|---|---|---|---|---|
| Hand-adapted engine (first version) | 1.17 ms | 1.59 ms | 2.45 ms | 3.39 ms |
| Playload's `engine.js` + `render()` patch (before tuning) | 5.69 ms | 7.13 ms | 9.50 ms | 11.73 ms |
| Same + `characterKey` and segmentation caches | 3.19 ms | 3.72 ms | 4.89 ms | 6.08 ms |

Real session (display, sound, bot input): about 40 % of one core for the whole process (including audio and display threads), 65 MB RSS.

### 1.3 Display (GPU time, drawing 480×432)

| View | GPU time per frame | Result |
|---|---|---|
| Flat | 2.0 ms | 60 fps |
| CRT, Playload's shader as is (palette search for each tap) | 21.9 ms | ~42 fps |
| CRT, colors passed as an sRGB texture (used) | 7.0 ms | 60 fps (pixels within 2/255 of the original method) |

### 1.4 Tuning the Node.js runtime (AFTER LOAD HD, headless)

| Stage | Game logic | Screen data | Total, average |
|---|---|---|---|
| QuickJS (reference) | ~45 ms | — | ~52 ms |
| Node.js, Playload's JS renderer | 2.6 ms | 23 ms | 26 ms |
| Node.js, native renderer (before tuning) | 2.6 ms | 36–43 ms | 40 ms |
| + character cache and a pre-created `-0` sprite (V8 deoptimization fixed) | 2.6 ms | **2.6 ms** | **7.1 ms** |

10SEC RACE (~250 `put()` calls per frame): caching segmentation (ICU) and `characterKey` took it from 19.6 ms to 7.7 ms.

### 1.5 Node.js, all games (headless, 900 ticks, performance, after tuning)

| Game | Average | p95 | Max |
|---|---|---|---|
| 10SEC RACE | 7.70 ms | 8.68 ms | 25.65 ms |
| AFTER LOAD HD | 7.53 ms | 10.68 ms | 48.21 ms |
| Block crash | 1.42 ms | 2.10 ms | 16.49 ms |
| Dungeon Escape | 3.31 ms | 4.29 ms | 57.67 ms |
| Puchi Jump | 4.96 ms | 5.85 ms | 21.43 ms |
| BRR 3KB | 1.36 ms | 2.00 ms | 16.69 ms |
| chiptune | 1.31 ms | 2.56 ms | 16.13 ms |
| glyph-quest | 1.17 ms | 1.81 ms | 17.11 ms |
| jump | 1.11 ms | 1.68 ms | 21.53 ms |
| rubble. | 12.54 ms | 14.29 ms | 34.66 ms |
| shooter | 1.26 ms | 1.88 ms | 16.44 ms |
| snake | 1.49 ms | 4.00 ms | 15.74 ms |

### 1.6 Pipelining (rubble., real session, CRT on, no input)

| | present (per 5 s) | over 20ms | frame avg |
|---|---|---|---|
| Without pipelining | 93–191 (~19–38 fps) | 93–136 | 10.7–13.0 ms |
| With pipelining (used) | 300 (60 fps) | 0–2 | 0.3–0.6 ms (after the first 5 seconds) |

Observed on the device before the change: FPS 19 / TPS 60 / MS 11.8 / MAX 15.7. AFTER LOAD HD ran at about 60 fps.

### 1.7 All games, real sessions (20 s, CRT on, Node.js)

**After the first 5 seconds**: every game had 300 presents, no update over 20 ms, and a longest interval of 16.8–17.4 ms. Exceptions: one 20.0 ms interval each for Dungeon Escape and chiptune with autoplay.

**First 5 seconds** (when V8 compiles most of the game):

| Game | No input: over 20ms / longest | Autoplay: over 20ms / longest | Autoplay: frame avg afterwards |
|---|---|---|---|
| 10SEC RACE | 7 / 55.0 ms | 6 / 75.1 ms | 0.09 ms |
| AFTER LOAD HD | 7 / 84.8 ms | 5 / 70.4 ms | 0.98 ms |
| Block crash | 7 / 57.2 ms | 6 / 61.1 ms | 0.10 ms |
| Dungeon Escape | 7 / 60.8 ms | 5 / 90.1 ms | 0.08 ms |
| Puchi Jump | 6 / 57.7 ms | 6 / 59.1 ms | 0.08 ms |
| BRR 3KB | 12 / 54.7 ms | 7 / 61.5 ms | 0.11 ms |
| chiptune | 6 / 67.1 ms | 7 / 55.9 ms | 0.09 ms |
| glyph-quest | 24 / 64.4 ms | 8 / 56.7 ms | 0.08 ms |
| jump | 7 / 55.2 ms | 6 / 53.7 ms | 0.08 ms |
| rubble. | 21 / 85.3 ms | (measured without input) | — |
| shooter | 7 / 59.1 ms | 6 / 63.9 ms | 0.08 ms |
| snake | 10 / 55.7 ms | 6 / 65.4 ms | 0.08 ms |

## 2. RK3326 (GKD Pixel2)

Real sessions, Node.js, CRT off, 20 s (autoplay except rubble.).

| Game | present (each 5 s) | over 20ms | longest | frame avg |
|---|---|---|---|---|
| AFTER LOAD HD | 226 / 241 / 269 (~45–54 fps) | 66 / 43 / 28 | 83.6 / 78.1 / 71.3 ms | 6.9–8.7 ms |
| rubble. | 153 / 149 (~30 fps) | 153 / 149 | 160.9 / 131.4 ms | 23.0–24.4 ms |
| 10SEC RACE | 257 / 300 / 301 | 41 / 0 / 0 | 97.1 / 19.8 / 18.5 ms | 1.0–7.4 ms |
| Puchi Jump | 296 / 301 / 300 | 7 / 0 / 0 | 38.1 / 17.2 / 16.8 ms | 0.2–0.6 ms |
| BRR 3KB | 300 / 300 / 301 | 3 / 0 / 0 | 32.2 / 16.9 / 17.0 ms | 0.2–0.4 ms |

- CRT on: every game dropped below 60 fps when checked on the device (not recorded numerically). → **CRT off recommended.**
- The two heaviest games need more than 16.7 ms of JavaScript per frame here, which no display-side change can hide.

## 3. A133P (V90S)

Real sessions, Node.js, 20 s (autoplay except rubble.). This screen runs at about 59 Hz, so about 295 presents is the maximum.

| Game | CRT off: present (each 5 s) | CRT off: over 20ms / longest | CRT on: present | CRT on: frame avg |
|---|---|---|---|---|
| AFTER LOAD HD | 293 / 295 / 295 | 5·0·0 / 38.2·17.2·18.2 ms | 147–148 (30 fps) | 5.3–5.9 ms |
| rubble. | 243 / 295 / 295 | 37·2·0 / 82.5·22.4·18.3 ms | 142–148 (30 fps) | 8.1–12.3 ms |
| 10SEC RACE | 296 / 295 / 295 | 1·0·0 / 25.6·17.1·17.2 ms | 147–149 (30 fps) | 4.4–4.8 ms |
| Puchi Jump | 296 / 295 / 295 | 0·0·0 / 19.1·17.3·17.2 ms | 147–149 (30 fps) | 2.7–2.9 ms |
| BRR 3KB | 296 / 295 / 295 | 0·0·0 / 17.1·17.1·17.1 ms | 148–149 (30 fps) | 1.1–1.3 ms |

- CRT off: every game, including the two heaviest, at about 59 fps (rubble. at 243 only in its first 5 seconds).
- CRT on: the CRT shader does not fit in 16.7 ms on the PowerVR GE8300, so every game runs at 30 fps regardless of its own load. → **CRT off recommended.**

## Summary

| SoC | Recommended | Ordinary games | Heaviest (AFTER LOAD HD / rubble., CRT off) |
|---|---|---|---|
| RK3566 | CRT on or off | 60 fps | 60 fps / 60 fps |
| RK3326 | CRT off | 60 fps | ~45–54 fps / ~30 fps |
| A133P | CRT off | ~59 fps (screen limit) | ~59 fps / ~59 fps |

On every device, the Node.js runtime shows short hitches (up to 50–160 ms) **only in the first seconds** of a game, while V8 compiles it.

## Reproducing

- Headless: `./playload.aarch64 --headless --runtime node --ticks 900 games/xxx.url` (`PLAYLOAD_NO_INPUT=1` for no input, `PLAYLOAD_PROFILE=1` for the Node.js time split)
- Real session: `PLAYLOAD_STATS=1 PLAYLOAD_AUTOPLAY=1 PLAYLOAD_RUNTIME=node sh <plumOS>/bin/plumos-portmaster-port-launch <ports>/Playload.sh games/xxx.url`, then read the `frame avg` lines in `playload/log.txt`
- GPU time: `PLAYLOAD_GPU_TIMING=1` (waits for the GPU with `glFinish` after each frame; diagnostics only)
- On V90S plumOS, logging out of SSH stops background jobs and `/tmp` differs per session: measure in a held session and keep results under `/mnt/plumos/state/`.
