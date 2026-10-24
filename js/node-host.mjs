// Runs one Playload game in Node.js for the native player.
//   node node-host.mjs <game.url|game.js> <engine directory>
// The engine directory holds Playload's own engine.js, sound.js and glyphs.js,
// used unchanged: V8 is fast enough to run Playload's renderer as is.
//
// Protocol with the player (binary, over fd 3 = requests and fd 4 = replies):
//   request  'F' <mask byte>     run one tick with the buttons in mask
//            'Q'                 quit
//   reply    'R' <u32 length> <screen> <sounds>   the player rasterizes (normal case)
//            'F' <u32 length> <23040 pixel indices> <sounds>   Playload's own renderer
//                 screen: 360 cells of <8 pattern rows> <ink> <paper>, <u16 patterns>,
//                         <8 rows per pattern>, <u32 sprites>, then for the sprites:
//                         <i16 x[]> <i16 y[]> <u16 pattern[]> <u8 col[]>
//                 sounds: <u16 count>, per sound <u8 ch> <u8 fixed volume> <u32 events> <f64 [op, a, b] * events>
//            'E' <u32 length> <utf-8 error message>
// The first reply (after the game's top-level code and first tick) comes unrequested.
// stdout and stderr (console.log, crashes) go to the port's log.txt.
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import zlib from 'node:zlib';

const [gamePath, engineDir] = process.argv.slice(2);
const IN = 3,
  OUT = 4;

function writeAll(buffer) {
  for (let offset = 0; offset < buffer.length; )
    offset += fs.writeSync(OUT, buffer, offset, buffer.length - offset);
}

function send(type, payload) {
  const header = Buffer.alloc(5);
  header[0] = type.charCodeAt(0);
  header.writeUInt32LE(payload.length, 1);
  writeAll(Buffer.concat([header, payload]));
}

function fail(error) {
  send('E', Buffer.from(String(error?.stack ?? error), 'utf8'));
}

// A Playload URL (…#v1.<base64url deflate-raw>) or plain JavaScript source.
function loadSource(file) {
  const text = fs.readFileSync(file, 'utf8');
  const match = /#(?:v1|1)\.([A-Za-z0-9_-]+)/.exec(text);
  if (!match) return text;
  const bytes = Buffer.from(match[1], 'base64url');
  return zlib.inflateRawSync(bytes, { maxOutputLength: 1 << 20 }).toString('utf8');
}

// Playload's renderer fills every sprite pixel in JS (23 ms per frame for 10,000
// sprites on a handheld). Its render() is replaced by one that hands the screen
// cells and sprites to the player, which rasterizes them natively. If a newer
// engine.js has no render() of this shape, the original renderer is used.
function functionRange(source, start) {
  const open = source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '{') depth++;
    else if (source[i] === '}' && --depth === 0) return [start, i + 1];
  }
  return null;
}

// put() and characterKey() segment every string into graphemes, and ICU's
// segmenter is native code V8 cannot speed up: a game that redraws its screen
// with hundreds of put() calls a frame spends most of its time there. Results
// are cached per string; engine.js only iterates them.
{
  const segment = Intl.Segmenter.prototype.segment,
    graphemes = new WeakMap();
  Intl.Segmenter.prototype.segment = function (input) {
    let isGrapheme = graphemes.get(this);
    if (isGrapheme === undefined) {
      isGrapheme = this.resolvedOptions().granularity === 'grapheme';
      graphemes.set(this, isGrapheme);
    }
    if (!isGrapheme) return segment.call(this, input);
    const text = String(input);
    let cache = this.__cache;
    if (!cache) Object.defineProperty(this, '__cache', { value: (cache = new Map()) });
    let parts = cache.get(text);
    if (!parts) {
      parts = Object.freeze(Array.from(segment.call(this, text), ({ segment }) => Object.freeze({ segment })));
      if (cache.size >= 4096) cache.clear();
      cache.set(text, parts);
    }
    return parts;
  };
}

// Same optional speed-up as the QuickJS player (js/patch.js): remember
// characterKey() results; inputs that throw are not cached.
function memoizeCharacterKey(source) {
  const key = 'const characterKey = (chr) => {';
  if (!source.includes(key)) return source;
  return source.replace(
    key,
    'const characterKeyCache = new Map();\n' +
      '  const characterKey = (chr) => {\n' +
      '    const known = typeof chr === "string" && characterKeyCache.get(chr);\n' +
      '    if (known) return known;\n' +
      '    const result = characterKeyUncached(chr);\n' +
      '    if (characterKeyCache.size >= 4096) characterKeyCache.clear();\n' +
      '    characterKeyCache.set(chr, result);\n' +
      '    return result;\n' +
      '  };\n' +
      '  const characterKeyUncached = (chr) => {',
  );
}

async function importEngine() {
  const file = path.resolve(engineDir, 'engine.js');
  const source = memoizeCharacterKey(fs.readFileSync(file, 'utf8'));
  const header = source.indexOf('function render() {');
  const range = header >= 0 && functionRange(source, header);
  if (range) {
    let patched =
      source.slice(0, range[0]) +
      'function render() {\n    return globalThis.__nativeRender(scr, sprites, lookup, pixels);\n  }' +
      source.slice(range[1]);
    // Reading colors through colorPair's Proxy costs ~8 ms per frame for the 360
    // cells. Keep each pair's plain values array so the renderer can read it
    // directly; skipped (Proxy reads remain) if colorPair has changed shape.
    const pair = '    const pair = new Proxy(values, {';
    const back = patched.indexOf('    return {', patched.indexOf(pair));
    if (patched.includes(pair) && back > 0)
      patched =
        patched.slice(0, back) +
        '    globalThis.__pairValues.set(pair, values);\n' +
        patched.slice(back);
    try {
      const module = await import('data:text/javascript;base64,' + Buffer.from(patched).toString('base64'));
      return { module, native: true };
    } catch (error) {
      console.error('render patch not usable, using the JS renderer:', error.message);
    }
  }
  return { module: await import(pathToFileURL(file).href), native: false };
}

const cells = Buffer.alloc(360 * 10);
// Proxy -> plain [col1, col2] array, filled by the colorPair patch.
globalThis.__pairValues = new WeakMap();
const pairValues = globalThis.__pairValues;
const colorsOf = (pair) => pairValues.get(pair) ?? pair;
let cellColors = null;
// Character -> its pcg element. engine.js creates elements lazily, so its
// character table changes shape whenever a new character first appears; V8
// then throws away optimized code that touched it, and after a few times stays
// slow. Elements keep their identity (and their live pat/col arrays), so the
// renderer remembers them and never touches that table in its loops.
const elements = new Map();
let lookupElement = null;
const elementOf = (chr) => {
  let element = elements.get(chr);
  if (element === undefined) {
    element = lookupElement(chr);
    elements.set(chr, element);
  }
  return element;
};
const patternIds = new Map(),
  patternList = [];
let screen = null,
  screenBuffer = null,
  spriteCapacity = 1024,
  spriteX = new Int16Array(spriteCapacity),
  spriteY = new Int16Array(spriteCapacity),
  spriteId = new Uint16Array(spriteCapacity),
  spriteCol = new Uint8Array(spriteCapacity);
globalThis.__nativeRender = (scr, sprites, lookup, pixels) => {
  // Screen cells and their color pairs never change identity: resolve them once.
  if (!cellColors) cellColors = scr.flatMap((row) => row.map((element) => colorsOf(element.col)));
  lookupElement = lookup;
  let i = 0,
    k = 0;
  for (const row of scr)
    for (const element of row) {
      const character = elementOf(element.chr),
        own = cellColors[k++],
        standard = colorsOf(character.col);
      cells.set(character.pat, i);
      cells[i + 8] = own[0] ?? standard[0];
      cells[i + 9] = own[1] ?? standard[1];
      i += 10;
    }
  // Only sprites that touch the screen; patterns are sent once per frame.
  if (sprites.length > spriteCapacity) {
    spriteCapacity = sprites.length * 2;
    spriteX = new Int16Array(spriteCapacity);
    spriteY = new Int16Array(spriteCapacity);
    spriteId = new Uint16Array(spriteCapacity);
    spriteCol = new Uint8Array(spriteCapacity);
  }
  patternIds.clear();
  patternList.length = 0;
  let n = 0;
  for (const sprite of sprites) {
    const x = sprite.x,
      y = sprite.y;
    if (x <= -8 || x >= 160 || y <= -8 || y >= 144) continue;
    // Patterns are numbered per character string (fast to hash), looked up once a frame.
    const chr = sprite.chr;
    let id = patternIds.get(chr);
    if (id === undefined) {
      id = patternList.length;
      patternIds.set(chr, id);
      patternList.push(elementOf(chr).pat);
    }
    spriteX[n] = x;
    spriteY[n] = y;
    spriteId[n] = id;
    spriteCol[n] = sprite.col;
    n++;
  }
  // cells | u16 patterns | patterns | u32 sprites | x i16[] | y i16[] | pattern u16[] | col u8[]
  const size = 3600 + 2 + patternList.length * 8 + 4 + n * 7;
  if (!screenBuffer || screenBuffer.length < size) screenBuffer = Buffer.alloc(size * 2);
  const out = screenBuffer;
  out.set(cells, 0);
  let at = 3600;
  out.writeUInt16LE(patternList.length, at);
  at += 2;
  for (const pat of patternList) {
    out.set(pat, at);
    at += 8;
  }
  out.writeUInt32LE(n, at);
  at += 4;
  for (const array of [spriteX, spriteY, spriteId]) {
    const bytes = new Uint8Array(array.buffer, 0, n * 2);
    out.set(bytes, at);
    at += n * 2;
  }
  out.set(spriteCol.subarray(0, n), at);
  at += n;
  screen = out.subarray(0, at);
  return pixels;
};

// Same op codes as the native AY (src/ay.c); unknown ops become 0 and are skipped.
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

let engine, tick;

// PLAYLOAD_PROFILE=1 prints where each frame's time goes (to the log).
const profile = process.env.PLAYLOAD_PROFILE ? { step: 0, render: 0, send: 0, n: 0 } : null;
let stepStart = 0,
  profileFrame = 0;

function reply() {
  const renderStart = performance.now();
  screen = null;
  const pixels = engine.pixels;
  const frame = screen ?? pixels;
  const renderEnd = performance.now();
  const sounds = engine.sounds();
  let size = frame.length + 2;
  for (const { score } of sounds) size += 6 + score.code.length * 24;
  const out = Buffer.alloc(size);
  out.set(frame, 0);
  let at = frame.length;
  out.writeUInt16LE(sounds.length, at);
  at += 2;
  for (const { ch, score } of sounds) {
    const flat = flatten(score);
    out[at++] = ch;
    out[at++] = score.fixedVolume ? 1 : 0;
    out.writeUInt32LE(flat.length / 3, at);
    at += 4;
    Buffer.from(flat.buffer, flat.byteOffset, flat.byteLength).copy(out, at);
    at += flat.byteLength;
  }
  send(screen ? 'R' : 'F', out);
  if (profile) {
    profile.render += renderEnd - renderStart;
    profile.send += performance.now() - renderEnd;
    profile.step += renderStart - stepStart;
    profileFrame++;
    if (++profile.n === 240) {
      const f = (v) => (v / 240).toFixed(2);
      console.error(`profile[${profileFrame}]: tick ${f(profile.step)} ms, render ${f(profile.render)} ms, send ${f(profile.send)} ms`);
      profile.step = profile.render = profile.send = profile.n = 0;
    }
  }
}

const request = Buffer.alloc(2);
function readRequest() {
  let got = 0;
  while (got < 1) {
    const n = fs.readSync(IN, request, got, 1, null);
    if (n === 0) process.exit(0);
    got += n;
  }
  if (request[0] !== 'F'.charCodeAt(0)) process.exit(0);
  while (got < 2) {
    const n = fs.readSync(IN, request, got, 1, null);
    if (n === 0) process.exit(0);
    got += n;
  }
  return request[1];
}

try {
  const load = (name) => import(pathToFileURL(path.resolve(engineDir, name)).href);
  const { createEngine } = (await importEngine()).module;
  const { compileScore } = await load('sound.js');
  const { glyphs } = await load('glyphs.js');
  engine = createEngine(glyphs, compileScore);
  const api = engine.api;
  // Coordinates are truncated, so a game drawing at x = -0.3 makes a sprite with
  // x = -0, which V8 stores differently from small integers. When that first
  // happens mid-game, V8 changes the sprite object shape and abandons optimized
  // code. Make that shape from the start; step() clears these before any frame.
  api.spr(-0.5, -0.5, ' ', 0);
  api.spr(0, 0, ' ', 0);
  tick = new Function(
    ...Object.keys(api),
    loadSource(gamePath) + '\n;return typeof tick === "function" ? tick : null;',
  )(...Object.values(api));
  if (!tick) throw new Error('Define function tick()');
  // As in the browser, the first tick runs right after the top-level code.
  engine.step(tick);
  reply();
} catch (error) {
  fail(error);
  // The player shows the error; wait until it closes the pipe.
  for (;;) readRequest();
}

for (;;) {
  const mask = readRequest();
  try {
    engine.input(mask);
    stepStart = performance.now();
    engine.step(tick);
    reply();
  } catch (error) {
    fail(error);
    for (;;) readRequest();
  }
}
