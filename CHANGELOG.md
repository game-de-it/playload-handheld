# Changelog

## 1.1.0

- Playload Arcade in the menu: browse the curated gallery (Wi-Fi; the last list is kept for offline use), with a live preview of each game, and save games to `games/` or play saved ones.
- Built-in Playload engine updated to v0.9.4, with the control symbols (⬆ ⬇ ⬅ ➡ Ⓐ Ⓑ) added in 0.9.3.
- Playload 0.9.4's overflow feedback: noise over frames that exceed the sprite or PCG limits, and its one-frame waits, in both QuickJS and Node.js (Node.js output verified pixel-identical with Playload, noise included).
- Titles the font cannot draw (most kanji) are shown by their file name in the menu.
- The menu moved to `js/menu.js`.

## 1.0.2

- Faster in-app engine update: the self-test now runs the API smoke test and up to six games (bundled games first) for at most about 1.5 seconds each, instead of every game for 300 ticks. On RK3566 with 13 games the update went from about 2 minutes to about 9 seconds.
- The update shows its progress (`DOWNLOAD`, `TEST n/m`) in the bottom right corner of the menu.
- The four engine files are downloaded with one `curl` call.

## 1.0.1

- Read pattern bytes directly from Playload 0.9.3's engine, whose `pcg[chr].pat` is now an edit-counting Proxy (about 3× faster rendering in QuickJS than reading through it). Rendering stays pixel-identical with Playload 0.9.2 and 0.9.3.
- Bit-Rate Rush 3KB: draw text with `put()` and keep sprites within 128 per frame, so the level-up screen shows on Playload 0.9.3 and later.

## 1.0.0

First public release.

- Runs Playload game URLs (`.url` files) offline on plumOS handhelds as a PortMaster-style port.
- Two JavaScript runtimes: Node.js (V8, default) and QuickJS-ng (built in, used as a fallback).
- Native rasterizer and native AY-3-8910 sound chip; Playload's own `engine.js`, `sound.js` and `glyphs.js` run unmodified apart from load-time patches.
- OpenGL ES presentation with Playload's CRT shader (optional).
- Game menu that remembers the last game; settings for button layout (Nintendo / Xbox), CRT, engine version and runtime.
- In-app update of the Playload engine from tagged GitHub releases, with a self-test before switching.
- Performance overlay (SELECT+L1).
- Built-in Playload engine: v0.9.2.
- Tested on RK3566 (plumOS-Bubble), RK3326 (GKD Pixel2) and A133P (V90S).
