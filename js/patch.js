// Loaded as a global script before any engine module. The engine modules are
// Playload's own files (built in, or downloaded by the in-app updater); this
// adapts them to the native player without editing them by hand.

// QuickJS has no Intl. Graphemes are approximated as a base code point plus
// combining marks (which include variation selectors), skin tones and ZWJ joins.
// Games draw the same strings every tick, so results are cached: engine.js
// segments and normalizes each non-ASCII character it is given.
if (typeof Intl === 'undefined' || !Intl.Segmenter) {
  const mark = /^\p{M}$/u;
  const extend = (c) => (c >= 0x1f3fb && c <= 0x1f3ff) || mark.test(String.fromCodePoint(c));
  const segments = new Map();
  globalThis.Intl = {
    Segmenter: class {
      segment(str) {
        let segs = segments.get(str);
        if (segs) return segs;
        const cps = Array.from(str);
        segs = [];
        for (let i = 0; i < cps.length; i++) {
          let s = cps[i];
          while (i + 1 < cps.length) {
            const c = cps[i + 1].codePointAt(0);
            if (extend(c)) s += cps[++i];
            else if (c === 0x200d && i + 2 < cps.length) s += cps[++i] + cps[++i];
            else break;
          }
          segs.push(Object.freeze({ segment: s }));
        }
        Object.freeze(segs);
        if (segments.size >= 4096) segments.clear();
        segments.set(str, segs);
        return segs;
      }
    },
  };
}
{
  const normalize = String.prototype.normalize,
    normalized = new Map();
  Object.defineProperty(String.prototype, 'normalize', {
    configurable: true,
    writable: true,
    value: function (form) {
      const str = String(this);
      if (form !== undefined && form !== 'NFC') return normalize.call(str, form);
      let result = normalized.get(str);
      if (result === undefined) {
        result = normalize.call(str, 'NFC');
        if (normalized.size >= 4096) normalized.clear();
        normalized.set(str, result);
      }
      return result;
    },
  });
}

// Browser games may log; QuickJS has no console. Logs go to the port's log.txt.
if (typeof console === 'undefined') {
  const log = (...args) => print(args.map((a) => (typeof a === 'string' ? a : JSON.stringify(a))).join(' '));
  globalThis.console = { log, info: log, warn: log, error: log, debug: log };
}

// Returns the body range of the function whose header starts at `start`.
function functionRange(source, start) {
  const open = source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '{') depth++;
    else if (source[i] === '}' && --depth === 0) return [start, i + 1];
  }
  return null;
}

// engine.js rasterizes every pixel in JS, which is too slow for QuickJS on a
// handheld. Its render() is replaced by one that hands the screen cells and
// sprites to the native rasterizer (globalThis.__nativeRender, see host.js).
globalThis.__patchModule = (name, source) => {
  if (name !== 'engine.js') return source;
  // Optional speed-up: remember characterKey() results. Games pass the same
  // emoji every tick, and each call normalizes, matches and segments it.
  // Inputs that throw are not cached, so errors stay the same. If a newer
  // engine.js no longer has this function, it simply runs unchanged.
  const key = 'const characterKey = (chr) => {';
  if (source.includes(key))
    source = source.replace(
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
  const header = source.indexOf('function render() {');
  const range = header >= 0 && functionRange(source, header);
  if (!range) throw new Error('engine.js: render() not found');
  return (
    source.slice(0, range[0]) +
    'function render() {\n    return globalThis.__nativeRender(scr, sprites, lookup, pixels);\n  }' +
    source.slice(range[1])
  );
};
