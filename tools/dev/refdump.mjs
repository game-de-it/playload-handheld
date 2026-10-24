// Reference renderer: runs the ORIGINAL Playload engine (engine.js from kitao/playload)
// under QuickJS with the same seed and bot, and dumps frames as PPM for comparison.
// Usage (from a dir holding the original engine.js, sound.js, glyphs.js):
//   qjs refdump.mjs game.js bot.js out-prefix tick1,tick2,...
import * as std from 'qjs:std';
const [gamePath, botPath, prefix, ticksArg] = scriptArgs.slice(1);
const mark = /^\p{M}$/u;
const extend = (c) => (c >= 0x1f3fb && c <= 0x1f3ff) || mark.test(String.fromCodePoint(c));
globalThis.Intl = { Segmenter: class { segment(str) { const cps = Array.from(str), segs = [];
  for (let i = 0; i < cps.length; i++) { let s = cps[i];
    while (i + 1 < cps.length) { const c = cps[i + 1].codePointAt(0);
      if (extend(c)) s += cps[++i]; else if (c === 0x200d && i + 2 < cps.length) s += cps[++i] + cps[++i]; else break; }
    segs.push({ segment: s }); } return segs; } } };
{ let seed = 12345; Math.random = () => { seed = (seed + 0x6d2b79f5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
const { createEngine, palette, rgb } = await import('./engine.js');
const { compileScore } = await import('./sound.js');
const { glyphs } = await import('./glyphs.js');
const engine = createEngine(glyphs, compileScore);
const api = engine.api;
const tick = new Function(...Object.keys(api), std.loadFile(gamePath) + '\n;' + std.loadFile(botPath) + '\n;return tick;')(...Object.values(api));
engine.step(tick);
engine.sounds();
const wanted = new Set(ticksArg.split(',').map(Number)), last = Math.max(...wanted);
const pal = palette.map(rgb);
for (let i = 0; i <= last; i++) {
  engine.input(globalThis.__bot());
  engine.step(tick);
  engine.sounds();
  if (wanted.has(i)) {
    const px = engine.pixels, f = std.open(`${prefix}${i}.ppm`, 'wb');
    f.puts(`P6\n160 144\n255\n`);
    const buf = new Uint8Array(160 * 144 * 3);
    for (let k = 0; k < px.length; k++) buf.set(pal[px[k]], k * 3);
    f.write(buf.buffer, 0, buf.length);
    f.close();
  }
}
