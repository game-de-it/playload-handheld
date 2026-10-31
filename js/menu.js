// @title Playload Handheld menu
// The menu is itself a Playload game, run in QuickJS by the player; it talks
// to the player through __native. It opens on the game played last.
//
// Main view: up/down pick a game or an item (held: repeat); on the game list
// left/right turn a page. A plays a game or changes a setting; left/right also
// change settings. ENGINE: A checks for a new Playload release, left/right
// switch versions. ARCADE: A opens Playload Arcade, the curated gallery.
//
// Arcade list: A shows a game, B goes back. Arcade game: a live preview; A
// saves it to games/ (or plays it once saved), left/right show the previous or
// next game, B goes back to the list.
const R = 6,
  K = 5,
  AR = 10;
let T, N, sel, top;
let msg = '',
  busy = null,
  wait = 0,
  held = 0;
let view = 'main',
  games = [],
  saved = [],
  asel = 0,
  atop = 0;

const cut = (s, n) => Array.from(String(s)).slice(0, n).join('');
// Titles and names come from the internet: show what the font can draw.
const say = (x, y, s, c) => {
  try {
    put(x, y, s, c);
  } catch {
    put(x, y, String(s).replace(/[^\x20-\x7e]/g, '?'), c);
  }
};
// The font has kana but few kanji; a character without one draws as blank.
const known = new Map();
const drawable = (s) =>
  Array.from(String(s)).every((c) => {
    if (c === ' ') return true;
    if (!known.has(c)) {
      let ok = false;
      try {
        ok = Array.from(pcg[c].pat).some(Boolean);
      } catch {}
      known.set(c, ok);
    }
    return known.get(c);
  });
const full = (url) => (url.startsWith('/') ? 'https://kitao.dev' + url : url);
const payload = (url) => (/#v1\.([A-Za-z0-9_-]+)/.exec(url || '') || [])[1];
const savedIndex = (g) => {
  const own = [payload(g.url), payload(g.originalUrl)];
  return saved.findIndex((p) => p && own.includes(p));
};
const slug = (g, i) => (/([a-z0-9-]+)\.png$/.exec(g.image || '') || [])[1] || 'arcade-' + i;
// Names the font cannot draw fall back to the screenshot's name / the author's account.
const titleOf = (g, i) => (drawable(g.title) ? g.title : slug(g, i).replace(/-/g, ' ').toUpperCase());
const authorOf = (g) => {
  const name = g.author || '';
  if (drawable(name)) return name;
  const account = /x\.com\/([A-Za-z0-9_]+)/.exec(g.authorUrl || '');
  return account ? '@' + account[1] : '';
};

function loadTitles(at) {
  T = globalThis.__titles;
  N = T.length;
  sel = Math.min(at, Math.max(N - 1, 0));
  top = Math.max(0, Math.min(sel - (R >> 1), N - R));
}

const settings = () => [
  'ARCADE',
  'BUTTONS:' + ['NINTENDO', 'XBOX'][__native.layout()],
  'CRT:' + (__native.crt() ? 'ON' : 'OFF'),
  'ENGINE:' + cut(__native.engine().current, 10),
  'RUNTIME:' + (__native.runtime() ? 'NODE.JS' : 'QUICKJS'),
];

function header(name, at, count) {
  cls(0);
  put(1, 1, name, 6);
  if (count) {
    const pos = `${Math.min(at, count - 1) + 1}/${count}`;
    put(19 - pos.length, 1, pos, at < count ? 7 : 2);
  }
  put(1, 2, '-'.repeat(18), 1);
}

function drawMain() {
  header('PLAYLOAD', sel, N);
  if (!N) put(1, 4, 'NO GAMES IN games/', 3);
  for (let i = 0; i < R && top + i < N; i++) {
    const k = top + i;
    const name = drawable(T[k]) ? T[k] : globalThis.__files[k].replace(/-/g, ' ').toUpperCase();
    say(1, 4 + i, (k === sel ? '>' : ' ') + cut(name, 17), k === sel ? 7 : 2);
  }
  if (top > 0) put(19, 4, '↑', 2);
  if (top + R < N) put(19, 3 + R, '↓', 2);
  settings().forEach((s, i) =>
    put(1, 11 + i, (sel === N + i ? '>' : ' ') + s, sel === N + i ? 7 : i ? 5 : 6),
  );
  say(1, 16, cut(msg, 18), 6);
  put(1, 17, 'A OK  SEL+ST QUIT', 2);
}

function drawList() {
  header('ARCADE', asel, games.length);
  for (let i = 0; i < AR && atop + i < games.length; i++) {
    const k = atop + i,
      on = k === asel;
    put(1, 4 + i, on ? '>' : ' ', 7);
    if (savedIndex(games[k]) >= 0) put(2, 4 + i, '*', 5);
    say(3, 4 + i, cut(titleOf(games[k], k), 16), on ? 7 : 2);
  }
  if (atop > 0) put(19, 4, '↑', 2);
  if (atop + AR < games.length) put(19, 3 + AR, '↓', 2);
  say(1, 15, cut(msg, 18), 6);
  put(1, 17, 'A VIEW  B BACK', 2);
}

// The preview itself (80x72 at 40,24) is drawn by the player.
function drawGame() {
  const g = games[asel],
    have = savedIndex(g) >= 0;
  header('ARCADE', asel, games.length);
  say(1, 13, cut(titleOf(g, asel), 18), 7);
  say(1, 14, cut(authorOf(g), 18), 2);
  say(1, 16, cut(msg || (have ? 'SAVED IN GAMES' : ''), 18), have && !msg ? 5 : 6);
  put(1, 17, (have ? 'A PLAY' : 'A SAVE') + '  B BACK  ←→', 2);
}

const draw = () => (view === 'main' ? drawMain() : view === 'list' ? drawList() : drawGame());

// Shows text first, then runs a slow step (network) on a later tick.
function later(text, step) {
  msg = text;
  busy = step;
  wait = 2;
  draw();
}

function openArcade(cachedOnly) {
  const { text, online } = __native.arcade(cachedOnly ? 1 : 0);
  if (!text) {
    msg = 'NO CONNECTION';
    return false;
  }
  try {
    games = JSON.parse(text).filter((g) => g && typeof g.title === 'string' && payload(g.url));
  } catch {
    msg = 'BAD ARCADE DATA';
    return false;
  }
  saved = __native.payloads();
  msg = online || cachedOnly ? '' : 'OFFLINE: SAVED LIST';
  view = 'list';
  asel = Math.min(asel, Math.max(games.length - 1, 0));
  return true;
}

function showGame(i) {
  asel = (i + games.length) % games.length;
  atop = Math.max(0, Math.min(atop, asel), asel - AR + 1);
  view = 'game';
  msg = '';
  __native.preview(full(games[asel].url));
}

// Moves with up/down, repeating while held.
function stepped() {
  const v = btn(1) - btn(0);
  held = v ? held + 1 : 0;
  return v && (held === 1 || (held > 18 && held % 4 === 0)) ? v : 0;
}

function tickMain() {
  const v = stepped();
  if (v) {
    sel = (sel + v + N + K) % (N + K);
    if (sel < N && sel < top) top = sel;
    if (sel < N && sel >= top + R) top = sel - R + 1;
    msg = '';
    draw();
    play(1, 'V8O6N1c');
  }
  // On the game list, left/right turn a page.
  const h = btnp(3) - btnp(2);
  if (h && sel < N) {
    sel = Math.max(0, Math.min(N - 1, sel + h * R));
    if (sel < top) top = sel;
    if (sel >= top + R) top = sel - R + 1;
    draw();
    play(1, 'V8O6N1c');
  }
  const change = btnp(4) || (sel > N && h !== 0);
  if (sel === N && btnp(4))
    return later('LOADING ARCADE...', () => {
      if (openArcade(false)) atop = Math.max(0, Math.min(atop, asel));
      play(1, 'V8O5N1cg');
    });
  if (sel === N + 1 && change) __native.layout(1 - __native.layout());
  if (sel === N + 2 && change) __native.crt(1 - __native.crt());
  if (sel === N + 4 && change) __native.runtime(1 - __native.runtime());
  if (sel === N + 3 && btnp(4))
    return later('UPDATING...', () => {
      msg = __native.update();
      play(1, 'V8O5N1cgC');
    });
  if (sel === N + 3 && h) {
    const { current, list } = __native.engine();
    const i = (list.indexOf(current) + (h > 0 ? 1 : list.length - 1)) % list.length;
    msg = 'USING ' + __native.engine(list[i]);
  }
  if (sel > N && change) {
    draw();
    play(1, 'V8O5N1cg');
  }
  if (sel < N && btnp(4)) __native.launch(sel);
}

function tickList() {
  const v = stepped(),
    h = btnp(3) - btnp(2),
    n = games.length;
  if ((v || h) && n) {
    asel = v ? (asel + v + n) % n : Math.max(0, Math.min(n - 1, asel + h * AR));
    if (asel < atop) atop = asel;
    if (asel >= atop + AR) atop = asel - AR + 1;
    msg = '';
    draw();
    play(1, 'V8O6N1c');
  }
  if (btnp(4) && n) {
    showGame(asel);
    draw();
    play(1, 'V8O5N1cg');
  } else if (btnp(5)) {
    view = 'main';
    msg = '';
    loadTitles(sel);
    draw();
    play(1, 'V8O5N1c');
  }
}

function tickGame() {
  const h = btnp(3) - btnp(2) || stepped();
  if (h) {
    showGame(asel + h);
    draw();
    play(1, 'V8O6N1c');
  }
  const g = games[asel];
  if (btnp(4)) {
    const have = savedIndex(g);
    if (have >= 0) {
      // Come back to this game after playing it.
      __native.state('arcade ' + asel);
      return __native.launch(have);
    }
    msg = __native.save(slug(g, asel), full(g.url)) >= 0 ? 'SAVED IN GAMES' : 'SAVE FAILED';
    saved = __native.payloads();
    draw();
    play(1, 'V8O5N1cegC');
  } else if (btnp(5)) {
    __native.preview('');
    view = 'list';
    msg = '';
    draw();
    play(1, 'V8O5N1c');
  }
}

function tick() {
  if (busy) {
    if (--wait === 0) {
      const step = busy;
      busy = null;
      step();
      draw();
    }
    return;
  }
  if (view === 'main') tickMain();
  else if (view === 'list') tickList();
  else tickGame();
}

loadTitles(globalThis.__menuSel || 0);
// Back from a game started in Arcade: reopen it there, from the saved list.
const back = /^arcade (\d+)$/.exec(__native.state() || '');
__native.state('');
if (back && openArcade(true)) {
  showGame(+back[1]);
  loadTitles(sel);
}
draw();
