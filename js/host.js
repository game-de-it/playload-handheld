// Glue between the native player and Playload's engine (engine.js, sound.js,
// glyphs.js, loaded as Playload ships them; see patch.js). The C side calls
// __start(source, inject) once and __frame(mask) at 60 Hz.
import { createEngine } from './engine.js';
import { compileScore } from './sound.js';
import { glyphs } from './glyphs.js';

const native = globalThis.__native;
let engine, tick;

// Screen cells: 8 pattern rows + ink + paper per cell. Sprites: x, y, col and
// 8 pattern rows. The native rasterizer draws from these each frame.
const cells = new Uint8Array(20 * 18 * 10);
let spritePos = new Int32Array(3 * 1024),
  spritePat = new Uint8Array(8 * 1024);

// patterns: engine.js's element -> private pattern bytes (0.9.3+), or null.
// glitches: overflow noise to XOR over the frame (0.9.4+), or null.
globalThis.__nativeRender = (scr, sprites, lookup, pixels, patterns, glitches) => {
  const patOf = patterns ? (element) => patterns.get(element) : (element) => element.pat;
  let i = 0;
  for (const row of scr)
    for (const element of row) {
      const character = lookup(element.chr),
        own = element.col,
        standard = character.col;
      cells.set(patOf(character), i);
      cells[i + 8] = own[0] ?? standard[0];
      cells[i + 9] = own[1] ?? standard[1];
      i += 10;
    }
  if (sprites.length > spritePos.length / 3) {
    spritePos = new Int32Array(3 * sprites.length * 2);
    spritePat = new Uint8Array(8 * sprites.length * 2);
  }
  let n = 0;
  for (const sprite of sprites) {
    spritePos[n * 3] = sprite.x;
    spritePos[n * 3 + 1] = sprite.y;
    spritePos[n * 3 + 2] = sprite.col;
    spritePat.set(patOf(lookup(sprite.chr)), n * 8);
    n++;
  }
  native.raster(cells, spritePos, spritePat, n, glitches);
  return pixels;
};

// Compiled scores are flattened for the native AY: [op, a, b] per event.
// Unknown ops (from a newer sound.js) become 0, which the native side skips.
const ops = { note: 1, repeat: 2, T: 3, M: 4, V: 5, W: 6, X: 7, U: 8, H: 9 };
const flattened = new WeakMap();
function flatten(score) {
  let flat = flattened.get(score);
  if (!flat) {
    flat = new Float64Array(score.code.length * 3);
    score.code.forEach((event, i) => {
      flat[i * 3] = ops[event.op] ?? 0;
      if (event.op === 'note') {
        flat[i * 3 + 1] = event.frequency;
        flat[i * 3 + 2] = event.beats;
      } else if (event.op === 'repeat') {
        flat[i * 3 + 1] = event.start;
        flat[i * 3 + 2] = event.count;
      } else flat[i * 3 + 1] = event.value ?? 0;
    });
    flattened.set(score, flat);
  }
  return flat;
}

function emitSounds() {
  for (const { ch, score } of engine.sounds())
    native.sound(ch, flatten(score), score.fixedVolume ? 1 : 0);
}

globalThis.__start = (source, inject = '') => {
  engine = createEngine(glyphs, compileScore);
  const api = engine.api;
  tick = new Function(
    ...Object.keys(api),
    source + '\n;' + inject + '\n;return typeof tick === "function" ? tick : null;',
  )(...Object.values(api));
  if (!tick) throw new Error('Define function tick()');
  // As in the browser, the first tick runs right after the top-level code.
  const delay = engine.step(tick);
  emitSounds();
  engine.pixels;
  return delay | 0;
};

// Returns the frames to wait after this tick (0.9.4+: 1 per exceeded limit).
globalThis.__frame = (mask) => {
  engine.input(mask);
  const delay = engine.step(tick);
  emitSounds();
  engine.pixels;
  return delay | 0;
};

// Shows an error on a fresh engine, so it works even if the game's state is broken.
globalThis.__error = (message) => {
  const view = createEngine(glyphs, compileScore);
  const { cls, put } = view.api;
  cls(0);
  put(1, 1, 'ERROR', 3);
  const words = String(message).replace(/[^\x20-\x7e]/g, '?').split(' ');
  let line = '',
    y = 3;
  for (const word of words) {
    if ((line + ' ' + word).trim().length > 18 && line) {
      put(1, y++, line.trim(), 7);
      line = '';
    }
    line += ' ' + word;
    if (y > 14) break;
  }
  if (line.trim() && y <= 14) put(1, y, line.trim(), 7);
  put(1, 16, 'SELECT+START: QUIT', 2);
  view.pixels;
};
