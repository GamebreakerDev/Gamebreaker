/* Gamebreaker launcher: shared behaviour. Motion comes from window.MOTION.
   Two modes. Without window.__gbLive this is the design mockup: fake data,
   nothing read, downloaded or built. With it (the launcher window started
   with -Live) the setup steps, Play and Settings go to the host, which runs
   the real engine (setup\setup.ps1) and the game; see the "live" section. */
(() => {
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
const root = document.documentElement;
const app = $('.app');
const M = window.MOTION;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const LIVE = (window.__gbLive && typeof window.__gbLive === 'object') ? window.__gbLive : null;

/* ------------------------------------------------------------------ state */
const SCEN = ['ok', 'sac', 'game', 'release', 'modified', 'compressed', 'space', 'antivirus', 'buildfail'];
const S = {
  mode: 'setup', page: 'home', tab: 'display', step: 0, input: 'keyboard',
  busy: false, updated: false, sheetOpen: false, launchOpen: false, mockOpen: false, built: false,
  set: {
    theme: M.defaultTheme || 'system', motion: 'full', size: 'auto',
    windowMode: 'window', monitor: 0, res: 'fit', aspect: '16:9', aspect2d: 'stretch', fps: 0, gameHz: '60', vsync: true,
    aniso: '16', msaa: '4', gpuPref: 'default', gpuVertex: false,
    skipIntro: true, pauseBg: true, songRotate: true, demoDelay: '90', rumbleOn: true, mouseMenus: false,
    smoothAudio: true, bgAudio: true, volume: 100,
    volMusic: 100, volSfx: 100, volCrowd: 100, volPlayers: 100, volCommentary: 100, commentary: true, mono: false, showFps: false, texDump: false,
    cheats: false, cheat_street_kids: false, cheat_no_counters: false, cheat_gb_trails: false, cheat_all_quicks: false, cheat_unlimited_turbo: false, cheat_easy_2_pointers: false, cheat_hard_2_pointers: false, cheat_big_heads: false, cheat_wnba_ball: false, cheat_alternate_ball: false, cheat_no_hud: false, cheat_explosive_rims: false,
    pads: false, preset: 'original', trigger: 12, deadzone: 0, rumble1: 100, rumble2: 100, rumble3: 100, rumble4: 100, padOrder: 'auto', kbPlayer: 1, padBg: false,
    renderer: 'd3d11', updates: true, sound: true, mods: true,
    scenario: 0, speed: 'fast', padSim: false, mockMods: 'sample', mockZip: 0
  }
};
const scenario = () => SCEN[S.set.scenario] || 'ok';

/* ------------------------------------------------------------- remembering */
/* Settings and "setup is finished" are kept between runs. Inside the
   launcher window the host owns the file and hands the saved values in as
   window.__gbSaved; in a plain browser they go to localStorage.

   Only what the player chose sticks. A setting the player never changed
   follows the current default, so a new release's defaults (or a different
   display) reach players who never touched it, and never override one who
   did. The save lists the chosen keys (format 2). A save from before
   2026-11-16 (format 1) has no such list: there, a value that differs from
   that time's default counts as chosen, and one equal to it as not chosen. */
const PERSIST = ['theme', 'size', 'windowMode', 'monitor', 'res', 'aspect', 'aspect2d', 'gameHz', 'vsync', 'aniso', 'msaa', 'skipIntro', 'smoothAudio', 'bgAudio', 'volume',
  'pads', 'preset', 'trigger', 'deadzone', 'rumble1', 'rumble2', 'rumble3', 'rumble4', 'padOrder', 'kbPlayer', 'padBg', 'gpuPref', 'updates', 'sound', 'mods',
  'pauseBg', 'songRotate', 'demoDelay', 'rumbleOn', 'mouseMenus', 'gpuVertex',
  'volMusic', 'volSfx', 'volCrowd', 'volPlayers', 'volCommentary', 'commentary', 'mono', 'showFps', 'texDump',
  'cheats', 'cheat_street_kids', 'cheat_no_counters', 'cheat_gb_trails', 'cheat_all_quicks', 'cheat_unlimited_turbo', 'cheat_easy_2_pointers', 'cheat_hard_2_pointers', 'cheat_big_heads', 'cheat_wnba_ball', 'cheat_alternate_ball', 'cheat_no_hud', 'cheat_explosive_rims'];
/* The renderer is always Direct3D 11 (the slow reference renderer was removed
   from the launcher, 2026-11-16); a remembered "cpu" is no longer read. */
const SAVE_FORMAT = 2;
const BASE = Object.assign({}, S.set);
/* What a format 1 save was compared with: that time's defaults, written out
   wherever today's differ, so changing a default never makes an old value look chosen. */
const V1_DEFAULTS = Object.assign({}, BASE, { windowMode: 'window', res: '2', aspect: '4:3', aspect2d: 'auto', vsync: false, smoothAudio: false });
/* Defaults: a window (the owner's choice for 0.1.2; borderless fullscreen
   before), vertical sync, the intro film skipped, and the shape and internal
   resolution of the display the game opens on. */
const SHAPES = [['4:3', 4 / 3], ['16:10', 16 / 10], ['16:9', 16 / 9], ['21:9', 21 / 9], ['32:9', 32 / 9]];
function targetDisplay() {
  const d = (LIVE && LIVE.displays) || [];
  return (S.set.monitor > 0 && d[S.set.monitor - 1]) || d.find(x => x.primary) || d[0] || null;
}
function nearestShape(w, h) {
  const r = w / h;
  let best = SHAPES[0];
  for (const s of SHAPES) if (Math.abs(Math.log(r / s[1])) < Math.abs(Math.log(r / best[1]))) best = s;
  return best[0];
}
/* Resolution: Fit (the display's own) on PCs with a graphics card; players can
   turn it down. A PC whose only graphics are built into the processor starts
   lighter: 1x, 2x anti-aliasing, 4x texture filtering. Fit with 4x
   anti-aliasing at 1080p is about 4 million pixels a frame with four samples
   each; on a Radeon 780M-class chip that alone is more than the 16.7 ms of a
   frame, and the chip shares the memory the CPU's graphics thread works in.
   Only defaults change: a value the player chose is kept. */
const lowGpu = () => !!(LIVE && LIVE.gpu && LIVE.gpu.integratedOnly);
const DEFAULTS = {
  aspect: () => { const t = targetDisplay(); return t ? nearestShape(t.w, t.h) : BASE.aspect; },
  res: () => (lowGpu() ? '1' : BASE.res),
  msaa: () => (lowGpu() ? '2' : BASE.msaa),
  aniso: () => (lowGpu() ? '4' : BASE.aniso)
};
S.chosen = new Set();
const defaultOf = k => (DEFAULTS[k] ? DEFAULTS[k]() : BASE[k]);
function applyDefaults() { for (const k of PERSIST) if (!S.chosen.has(k)) S.set[k] = defaultOf(k); }
function chose(k) { if (PERSIST.includes(k)) S.chosen.add(k); }
(() => {
  let saved = null;
  try { saved = window.__gbSaved || JSON.parse(localStorage.getItem('gb-launcher') || 'null'); } catch (e) { saved = null; }
  if (saved && typeof saved === 'object') {
    const st = saved.settings || {};
    const listed = saved.v >= 2 ? new Set(Array.isArray(saved.chosen) ? saved.chosen : []) : null;
    for (const k of PERSIST) {
      if (!(k in st) || typeof st[k] !== typeof BASE[k]) continue;
      if (listed ? listed.has(k) : st[k] !== V1_DEFAULTS[k]) { S.set[k] = st[k]; S.chosen.add(k); }
    }
    // One rumble strength for all players (before 2026-11-17) becomes each player's.
    if (typeof st.rumble === 'number' && (listed ? listed.has('rumble') : st.rumble !== 100)) {
      for (const k of ['rumble1', 'rumble2', 'rumble3', 'rumble4']) if (!S.chosen.has(k)) { S.set[k] = st.rumble; S.chosen.add(k); }
    }
    S.built = saved.built === true;
  }
  applyDefaults();
})();
let saveTimer = 0;
function save() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    const data = JSON.stringify({ v: SAVE_FORMAT, settings: Object.fromEntries(PERSIST.map(k => [k, S.set[k]])), chosen: PERSIST.filter(k => S.chosen.has(k)), built: S.built });
    try { if (hostBridge) hostBridge.postMessage('save:' + data); else localStorage.setItem('gb-launcher', data); } catch (e) { /* not fatal */ }
  }, 250);
}

/* ------------------------------------------------------------------ sound */
/* Slight tactile sounds, made in code. Quiet by design, and switched off
   with the Sound setting. */
let actx = null, noiseBuf = null, lastTick = 0;
function audio() {
  if (!S.set.sound) return null;
  try {
    if (!actx) { actx = new (window.AudioContext || window.webkitAudioContext)(); setTimeout(() => { if (hostBridge) hostBridge.postMessage('audio ' + actx.state); }, 400); }
    if (actx.state === 'suspended') actx.resume();
  } catch (e) { return null; }
  if (!noiseBuf) {
    const n = Math.floor(actx.sampleRate * 0.03);
    noiseBuf = actx.createBuffer(1, n, actx.sampleRate);
    const d = noiseBuf.getChannelData(0);
    for (let i = 0; i < n; i++) d[i] = (Math.random() * 2 - 1) * Math.pow(1 - i / n, 4);
  }
  return actx;
}
/* A knock, not a hiss: a sine that starts hard and drops in pitch within a
   few milliseconds (what makes a "tock"), plus a 2 ms click at the front.
   move = light tick (18 ms), select = deeper clock (34 ms), back = lower (30 ms). */
function tick(kind) {
  const c = audio();
  if (!c) return;
  const now = performance.now();
  if (kind === 'move' && now - lastTick < 45) return;
  lastTick = now;
  //            start Hz, end Hz, length s, level, click level
  const v = { move: [1500, 1050, .018, .081, .04], select: [820, 520, .034, .117, .056], back: [560, 380, .030, .099, .04] }[kind] || [1500, 1050, .018, .081, .04];
  const t = c.currentTime;
  const o = c.createOscillator(), g = c.createGain();
  o.type = 'sine';
  o.frequency.setValueAtTime(v[0], t);
  o.frequency.exponentialRampToValueAtTime(v[1], t + v[2] * .6);
  g.gain.setValueAtTime(.0001, t);
  g.gain.exponentialRampToValueAtTime(v[3], t + .0017);
  g.gain.exponentialRampToValueAtTime(.0001, t + v[2]);
  o.connect(g); g.connect(c.destination); o.start(t); o.stop(t + v[2] + .005);
  const src = c.createBufferSource(); src.buffer = noiseBuf;          // the click at the front
  const hp = c.createBiquadFilter(); hp.type = 'highpass'; hp.frequency.value = 2500;
  const ng = c.createGain();
  ng.gain.setValueAtTime(v[4], t); ng.gain.exponentialRampToValueAtTime(.0001, t + .003);
  src.connect(hp); hp.connect(ng); ng.connect(c.destination); src.start(t); src.stop(t + .004);
}
function playTone() {   // Play: a soft low fifth that swells and fades with the rings
  const c = audio();
  if (!c) return;
  const t = c.currentTime;
  for (const [f, level] of [[147, .05], [220, .03]]) {
    const o = c.createOscillator(), g = c.createGain();
    o.type = 'sine'; o.frequency.value = f;
    g.gain.setValueAtTime(.0001, t); g.gain.exponentialRampToValueAtTime(level, t + .12); g.gain.exponentialRampToValueAtTime(.0001, t + 1.6);
    o.connect(g); g.connect(c.destination); o.start(t); o.stop(t + 1.7);
  }
}
function consume(name) {
  if (scenario() !== name) return false;
  S.set.scenario = 0; renderCycler($('.cycler[data-setting="scenario"]'));
  return true;
}

/* The halo ring is the launcher's status light: idle, work (fills to p%),
   spin (busy, no known length), error, ok. */
/* When a job ends, the ring runs back to empty and its colour eases back to
   the resting gradient ("settle") instead of switching at once. */
const HALO_SETTLE_MS = 1000;
let haloT = 0;
function halo(state, p) {
  clearTimeout(haloT);
  const cur = app.dataset.halo;
  if (state === 'idle' && (cur === 'ok' || cur === 'work' || cur === 'settle')) {
    if (cur !== 'settle') { app.dataset.halo = 'settle'; app.style.setProperty('--hp', '0'); }
    haloT = setTimeout(() => { if (app.dataset.halo === 'settle') app.dataset.halo = 'idle'; }, HALO_SETTLE_MS);
    return;
  }
  app.dataset.halo = state;
  if (p !== undefined) app.style.setProperty('--hp', Math.max(0, Math.min(100, p)).toFixed(2));
}

/* ------------------------------------------------------------ preferences */
const mqDark = matchMedia('(prefers-color-scheme: dark)');
const mqReduce = matchMedia('(prefers-reduced-motion: reduce)');
const reduced = () => root.dataset.motion === 'reduced';
function applyPrefs() {
  root.dataset.theme = S.set.theme === 'system' ? (mqDark.matches ? 'dark' : 'light') : S.set.theme;
  root.dataset.motion = (S.set.motion === 'system' ? mqReduce.matches : S.set.motion === 'reduced') ? 'reduced' : 'full';
  root.dataset.size = S.set.size;
  requestAnimationFrame(() => requestAnimationFrame(() => { refreshIndicators(true); placeRing(document.activeElement, true); }));
}
mqDark.addEventListener('change', applyPrefs);
mqReduce.addEventListener('change', applyPrefs);

/* ----------------------------------------------------------------- motion */
function spec(kind) { return (reduced() ? M.reduced[kind] || M.reduced.fade : M[kind]) || M.reduced.fade; }
function play(el, s, dir = 1) {
  if (!el || !el.animate || !s) return Promise.resolve(null);
  const a = el.animate(s.frames(dir, el), { duration: s.ms, easing: s.ease || 'linear', delay: s.delay || 0, fill: 'both' });
  return a.finished.then(() => a, () => null);
}
const gens = {};
async function swap(oldEl, newEl, kind, dir, group) {
  const g = gens[kind] = (gens[kind] || 0) + 1;
  group.forEach(el => { if (el !== oldEl && el !== newEl) { el.getAnimations().forEach(a => a.cancel()); el.hidden = true; el.inert = false; } });
  [oldEl, newEl].forEach(el => el && el.getAnimations().forEach(a => a.cancel()));
  newEl.hidden = false; newEl.inert = false;
  if (!oldEl || oldEl === newEl) return;
  oldEl.inert = true;
  const s = spec(kind);
  const [ao, ai] = await Promise.all([play(oldEl, s.out, dir), play(newEl, s.in, dir)]);
  if (gens[kind] !== g) return;
  oldEl.hidden = true; oldEl.inert = false;
  if (ao) ao.cancel(); if (ai) ai.cancel();
}

/* ------------------------------------------------------------- indicators */
function moveIndicator(box, active, instant) {
  if (!box) return;
  const ind = $(':scope > .indicator', box);
  if (!ind) return;
  if (!active || !box.getClientRects().length) { ind.style.opacity = active ? '' : '0'; return; }
  const b = box.getBoundingClientRect(), a = active.getBoundingClientRect();
  const sx = b.width / box.offsetWidth || 1, sy = b.height / box.offsetHeight || 1;
  if (instant) ind.classList.add('no-anim');
  else if (Math.abs(parseFloat(ind.style.getPropertyValue('--iy')) - (a.top - b.top) / sy) > 2) {
    ind.classList.add('gliding'); clearTimeout(ind._glide);
    ind._glide = setTimeout(() => ind.classList.remove('gliding'), 160);
  }
  ind.style.opacity = '';
  ind.style.setProperty('--ix', ((a.left - b.left) / sx) + 'px');
  ind.style.setProperty('--iy', ((a.top - b.top) / sy) + 'px');
  ind.style.setProperty('--iw', (a.width / sx) + 'px');
  ind.style.setProperty('--ih', (a.height / sy) + 'px');
  if (instant) { void ind.offsetWidth; ind.classList.remove('no-anim'); }
}
function refreshIndicators(instant, scope = document) {
  if (scope === document) {
    moveIndicator($('.nav'), $('.nav [aria-current="page"]'), instant);
    moveIndicator($('.tabs'), $('.tabs [aria-selected="true"]'), instant);
    moveIndicator($('.step-rail'), $('.step-rail [aria-current="step"]'), instant);
  }
  $$('.seg', scope).forEach(seg => moveIndicator(seg, $('[aria-pressed="true"]', seg), instant));
}

/* ------------------------------------------------------------- focus ring */
const ring = $('.focus-ring');
let ringHeld = false, ringOn = false, ringTimer = 0;
function remPx() { return parseFloat(getComputedStyle(root).fontSize) || 16; }
function placeRing(el, instant) {
  const show = !!el && el !== document.body && S.input !== 'mouse' && !ringHeld && el.getClientRects().length > 0;
  if (!show) { ring.classList.remove('on'); ringOn = false; return; }
  const r = el.getBoundingClientRect();
  const pad = el.matches('[data-ring="inset"]') ? 0 : el.matches('.tab') ? remPx() * .06 : remPx() * (parseFloat(getComputedStyle(root).getPropertyValue('--ring-pad')) || .25);
  const brs = getComputedStyle(el).borderTopLeftRadius;
  const er = brs.endsWith('%') ? Math.min(r.width, r.height) * parseFloat(brs) / 100 : parseFloat(brs) || 0;
  const jump = instant || !ringOn;
  if (jump) ring.classList.add('no-anim');
  const lean = el.matches('.play-orb, .slider, .acc-head, .mock-toggle, .mock-panel *') ? '' : ' skewX(-12deg)';
  ring.style.transform = `translate(${r.left - pad}px, ${r.top - pad}px)${lean}`;
  ring.style.width = (r.width + pad * 2) + 'px';
  ring.style.height = (r.height + pad * 2) + 'px';
  ring.style.borderRadius = (lean ? remPx() * .34 : Math.min(er + pad, (r.height + pad * 2) / 2)) + 'px';
  if (jump) { void ring.offsetWidth; ring.classList.remove('no-anim'); }
  ring.classList.add('on'); ringOn = true;
}
function holdRing(on) { ringHeld = on; if (on) { ring.classList.remove('on'); ringOn = false; } }
document.addEventListener('focusin', e => {
  placeRing(e.target);
  clearTimeout(ringTimer);
  ringTimer = setTimeout(() => placeRing(document.activeElement), 230); // follow focus "lift" scale
  const t = e.target;
  if (t.matches('.tab') && S.input !== 'mouse') setTab(t.dataset.tab);
  if (t.matches('.nav-item') && S.input !== 'mouse' && S.mode === 'main' && !S.sheetOpen && !S.launchOpen) go(t.dataset.goto);
});
document.addEventListener('scroll', () => placeRing(document.activeElement, true), true);
addEventListener('resize', () => { refreshIndicators(true); placeRing(document.activeElement, true); });

/* ------------------------------------------------------------- input mode */
const padPill = $('[data-bind="padPill"]');
function setInput(m) {
  if (S.input === m) return;
  S.input = m; root.dataset.input = m;
  renderHints();
  placeRing(document.activeElement, true);
}
addEventListener('pointermove', e => { if (e.pointerType === 'mouse' && Math.abs(e.movementX) + Math.abs(e.movementY) > 3) setInput('mouse'); }, { passive: true });
addEventListener('pointerdown', () => setInput('mouse'));

/* ----------------------------------------------------- spatial navigation */
const FOCUSABLE = 'button:not([disabled]):not([tabindex="-1"]), [data-focus][tabindex="0"]';
function layers() {
  if (S.mockOpen) return [$('.mock-panel')];
  if (S.launchOpen) return [$('.launch')];
  if (S.sheetOpen) return [$('.sheet')];
  return S.mode === 'setup' ? [$('.setup')] : [$('.topbar'), $('.main')];
}
function focusables() {
  return layers().flatMap(l => $$(FOCUSABLE, l)).filter(el =>
    el.getClientRects().length > 0 && !el.closest('[inert]') && !el.closest('[hidden]') && el.getAttribute('aria-disabled') !== 'true');
}
function focusEl(el) {
  if (!el) return;
  el.focus({ preventScroll: true });
  placeRing(el);   // do not rely on the focus event: it does not fire while the window is not in front
  clearTimeout(ringTimer); ringTimer = setTimeout(() => placeRing(document.activeElement), 400);   // settle after anything sliding in
  el.scrollIntoView({ block: 'nearest', inline: 'nearest', behavior: reduced() ? 'auto' : 'smooth' });
}
function focusFirst(scope) {
  const list = focusables().filter(el => scope.contains(el));
  focusEl(list.find(el => el.matches('[data-autofocus]')) || list[0]);
}
function move(dx, dy) {
  const cur = document.activeElement, list = focusables();
  if (!list.includes(cur)) { focusEl(list.find(el => el.matches('[data-autofocus]')) || list[0]); return; }
  const a = cur.getBoundingClientRect();
  const acx = a.left + a.width / 2, acy = a.top + a.height / 2;
  let best = null, bestScore = Infinity;
  for (const el of list) {
    if (el === cur) continue;
    if (dx && cur.matches('.nav-item, .tab') && el.matches('.nav-item, .tab')) continue; // sideways never moves within the menu
    const b = el.getBoundingClientRect();
    const bcx = b.left + b.width / 2, bcy = b.top + b.height / 2;
    let primary, ortho;
    if (dx) {
      if (dx > 0 ? bcx <= acx + 1 : bcx >= acx - 1) continue;
      primary = Math.max(0, dx > 0 ? b.left - a.right : a.left - b.right);
      ortho = Math.max(0, Math.max(a.top, b.top) - Math.min(a.bottom, b.bottom));
      if (ortho > 0) ortho += Math.abs(bcy - acy) * .25;
    } else {
      if (dy > 0 ? bcy <= acy + 1 : bcy >= acy - 1) continue;
      primary = Math.max(0, dy > 0 ? b.top - a.bottom : a.top - b.bottom);
      ortho = Math.max(0, Math.max(a.left, b.left) - Math.min(a.right, b.right));
      if (ortho > 0) ortho += Math.abs(bcx - acx) * .1;
      else ortho = Math.abs(b.left - a.left) * .02; // prefer the item that lines up
    }
    const score = primary + ortho * 3;
    if (score < bestScore) { bestScore = score; best = el; }
  }
  if (best && best.matches('.nav-item, .tab') && !cur.matches('.nav-item, .tab'))
    best = (S.page === 'settings' ? $('.tab[aria-selected="true"]') : $('.nav-item[aria-current="page"]')) || best;
  if (best) { focusEl(best); tick('move'); } else nudge(cur, dx, dy);
}
function nudge(el, dx, dy) { // edge feedback: a tiny bump, no movement in reduced mode
  if (reduced() || !el.animate) return;
  const x = dx * remPx() * .2, y = dy * remPx() * .2;
  el.animate([{ transform: 'translate(0, 0)' }, { transform: `translate(${x}px, ${y}px)` }, { transform: 'translate(0, 0)' }], { duration: 180, easing: 'ease-out' });
}
function ping(x, y) {
  const p = document.createElement('span');
  p.className = 'ping'; p.style.left = x + 'px'; p.style.top = y + 'px';
  app.append(p); p.addEventListener('animationend', () => p.remove());
}
function flashPress(el) {
  el.classList.add('pressed'); setTimeout(() => el.classList.remove('pressed'), 130);
  const r = el.getBoundingClientRect(); ping(r.left + r.width / 2, r.top + r.height / 2);
}
document.addEventListener('pointerdown', e => { if (e.target.closest('button:not(:disabled), [data-focus], .slider-track')) { ping(e.clientX, e.clientY); tick('select'); } });

/* A soft light trails the pointer; sparks drift along the seam's direction. */
const glow = $('.cursor-glow');
let gx = innerWidth / 2, gy = innerHeight / 2, tx = gx, ty = gy, glowRaf = 0;
function stepGlow() {
  gx += (tx - gx) * .13; gy += (ty - gy) * .13;
  glow.style.transform = `translate(${gx}px, ${gy}px)`;
  glowRaf = Math.abs(tx - gx) + Math.abs(ty - gy) > .5 ? requestAnimationFrame(stepGlow) : 0;
}
addEventListener('pointermove', e => {
  if (e.pointerType !== 'mouse') return;
  tx = e.clientX; ty = e.clientY; glow.classList.add('on');
  if (!glowRaf) glowRaf = requestAnimationFrame(stepGlow);
}, { passive: true });
document.documentElement.addEventListener('pointerleave', () => glow.classList.remove('on'));
function makeMotes(n) {
  const host = $('.drift'), colours = ['var(--art-1)', 'var(--art-2)', 'var(--art-3)'];
  for (let i = 0; i < n; i++) {
    const m = document.createElement('span');
    m.className = 'mote';
    const dur = 16 + Math.random() * 22;
    m.style.cssText = `left:${(-12 + Math.random() * 105).toFixed(1)}%;height:${(2 + Math.random() * 7).toFixed(1)}rem;--c:${colours[i % 3]};--dur:${dur.toFixed(1)}s;--delay:${(-Math.random() * dur).toFixed(1)}s`;
    host.append(m);
  }
}

function dpad(dx, dy) {
  if (MD.lifted && S.page === 'mods') { modsDpad(dy); return; }
  const el = document.activeElement;
  if (el && dx && el.matches('.slider')) return stepSlider(el, dx);
  if (el && dx && el.matches('.cycler')) return cycle(el, dx);
  move(dx, dy);
}
function activate() {
  const el = document.activeElement;
  if (!el || el === document.body) { move(0, 1); return; }
  if (el.matches('.slider')) return;
  if (el.matches('.cycler')) return cycle(el, 1);
  flashPress(el); tick('select'); el.click();
}

/* ------------------------------------------------------------------- keys */
let capture = null;
addEventListener('keydown', e => {
  if (capture) { e.preventDefault(); if (!e.repeat) finishRebind(e); return; }
  if (modsKey(e)) return;
  const k = e.key;
  if (k === '`') { e.preventDefault(); if (!LIVE || LIVE.dev) toggleMock(); return; }
  const arrows = { ArrowUp: [0, -1], ArrowDown: [0, 1], ArrowLeft: [-1, 0], ArrowRight: [1, 0] };
  if (arrows[k]) { e.preventDefault(); setInput('keyboard'); dpad(...arrows[k]); return; }
  if (k === 'Enter' || k === ' ') {
    setInput('keyboard');
    const el = document.activeElement;
    if (!el || el === document.body || !el.matches('button')) { e.preventDefault(); activate(); }
    else flashPress(el);
    return;
  }
  if (k === 'Escape' || k === 'Backspace') { e.preventDefault(); setInput('keyboard'); back(); return; }
  if (k === 'q' || k === 'Q' || k === 'PageUp') { e.preventDefault(); setInput('keyboard'); bumper(-1); return; }
  if (k === 'e' || k === 'E' || k === 'PageDown') { e.preventDefault(); setInput('keyboard'); bumper(1); return; }
  if (k === 'Tab') setInput('keyboard');
});

function back() {
  tick('back');
  if (capture) return cancelRebind();
  if (padTest) return setPadTest(false);
  if (!S.mockOpen && modsBack()) return;
  if (S.mockOpen) return toggleMock(false);
  if (S.launchOpen) return closeLaunch();
  if (S.sheetOpen) { if (!S.busy) closeSheet(); return; }
  if (S.mode === 'setup') { if (!S.busy && S.step > 0 && S.step < 4) showStep(S.step - 1); return; }
  if (S.page !== 'home') go('home');
}
function bumper(d) {
  if (S.mode === 'main' && !S.sheetOpen && !S.launchOpen && !S.mockOpen) {
    if (S.page === 'settings') { cycleTab(d); return; }
    const order = PAGES, i = order.indexOf(S.page);
    go(order[(i + d + order.length) % order.length]);
  }
}

/* ------------------------------------------------------ controller test */
/* While testing, the controller drives only the picture. Holding B for
   0.8 s (or Esc, or the button) ends it. */
let padTest = false, bHeld = false, holdStart = 0, holdRaf = 0;
function setPadTest(on) {
  if (padTest === on) return;
  padTest = on; holdStart = 0; bHeld = false;
  if (hostBridge) hostBridge.postMessage('controller test ' + (on ? 'on' : 'off'));
  const btn = $('[data-pad-test]');
  btn.classList.toggle('testing', on); btn.style.setProperty('--hold', '0');
  $('.pad-test-label').textContent = on ? 'Hold B to finish' : 'Test';
  $('.pad-card').classList.toggle('testing', on);
  renderHints();
  requestAnimationFrame(() => placeRing(document.activeElement, true));
}
function holdTick(now) {
  holdRaf = 0;
  if (!padTest) return;
  const btn = $('[data-pad-test]');
  if (!bHeld) { holdStart = 0; btn.style.setProperty('--hold', '0'); return; }
  if (!holdStart) holdStart = now;
  const f = Math.min(1, (now - holdStart) / 800);
  btn.style.setProperty('--hold', f.toFixed(3));
  if (f >= 1) { setPadTest(false); return; }
  holdRaf = requestAnimationFrame(holdTick);
}
function padTestState(bDown) { bHeld = bDown; if (padTest && !holdRaf) holdRaf = requestAnimationFrame(holdTick); }
document.addEventListener('click', e => { if (e.target.closest('[data-pad-test]')) setPadTest(!padTest); });

/* --------------------------------------------------------- host bridge */
/* In the real launcher window the host reads the controller natively and
   posts events here; the browser's own Gamepad API is unreliable in WebView2. */
const hostBridge = window.chrome && window.chrome.webview;
let hostPad = false;
function hostState(ev) {
  const [, mask, lx, ly, rx, ry, lt, rt] = ev.split(':');
  const m = parseInt(mask, 16);
  const bits = [0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200, 0, 0, 0x20, 0x10, 0x40, 0x80, 1, 2, 4, 8];
  const buttons = bits.map(b => ({ pressed: !!(m & b), value: m & b ? 1 : 0 }));
  buttons[6] = { pressed: +lt > 2, value: +lt / 15 };
  buttons[7] = { pressed: +rt > 2, value: +rt / 15 };
  return { buttons, axes: [+lx / 16, -ly / 16, +rx / 16, -ry / 16] };
}
function hostEvent(ev) {
  if (!ev) return;
  if (ev.startsWith('pad:')) { hostPad = ev === 'pad:1'; updatePadUI(); return; }
  if (ev.startsWith('win:')) { root.dataset.win = ev.slice(4); return; }
  if (ev.startsWith('eng:')) { let o = null; try { o = JSON.parse(ev.slice(4)); } catch (e) { return; } if (LIVE) onEngine(o); return; }
  if (ev.startsWith('s:')) { const st = hostState(ev); drawPad(st); if (padTest) setInput('gamepad'); padTestState(st.buttons[1].pressed); return; }
  if (padTest) return;
  if (capture) { if (ev === 'b') cancelRebind(); return; }   // a key is awaited: the controller only cancels
  setInput('gamepad');
  if (ev === 'up') dpad(0, -1); else if (ev === 'down') dpad(0, 1);
  else if (ev === 'left') dpad(-1, 0); else if (ev === 'right') dpad(1, 0);
  else if (ev === 'a') activate(); else if (ev === 'b') back();
  else if (ev === 'lb') bumper(-1); else if (ev === 'rb') bumper(1);
  else if (ev === 'x' || ev === 'y') modsPad(ev);
  else if (ev === 'start' && S.mode === 'main' && S.page === 'home' && !S.sheetOpen && !S.launchOpen) openLaunch();
}
if (hostBridge) {
  root.dataset.host = '1';
  document.addEventListener('click', e => { const w = e.target.closest('[data-win]'); if (w) hostBridge.postMessage('win:' + w.dataset.win); });
  hostBridge.addEventListener('message', e => String(e.data).split(String.fromCharCode(10)).forEach(hostEvent));
}

/* -------------------------------------------------------------- gamepad */
let padRaf = 0;
const prevBtn = {}, held = {};
function pads() { return navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(Boolean) : []; }
function pollPads(t) {
  if (hostBridge) { padRaf = 0; return; }
  const p = pads()[0];
  if (!p) { padRaf = 0; updatePadUI(); return; }
  const b = i => !!(p.buttons[i] && p.buttons[i].pressed);
  if (padTest) {
    for (let i = 0; i < p.buttons.length; i++) prevBtn[i] = b(i);
    drawPad(p); padTestState(b(1));
    padRaf = requestAnimationFrame(pollPads); return;
  }
  const ax = p.axes || [];
  const dirs = { up: b(12) || ax[1] < -.55, down: b(13) || ax[1] > .55, left: b(14) || ax[0] < -.55, right: b(15) || ax[0] > .55 };
  let any = false;
  for (const [name, on] of Object.entries(dirs)) {
    if (on) {
      any = true;
      if (!held[name]) { held[name] = t + 380; fireDir(name); }
      else if (t >= held[name]) { held[name] = t + 110; fireDir(name); }
    } else held[name] = 0;
  }
  const edge = i => { const now = b(i), was = prevBtn[i]; prevBtn[i] = now; if (now) any = true; return now && !was; };
  const A = edge(0), B = edge(1), X = edge(2), Y = edge(3), LB = edge(4), RB = edge(5), START = edge(9);
  for (let i = 0; i < p.buttons.length; i++) if (![0, 1, 2, 3, 4, 5, 9].includes(i)) { if (b(i)) any = true; prevBtn[i] = b(i); }
  if (any) setInput('gamepad');
  if (A) activate();
  if (B) back();
  if (X) modsPad('x');
  if (Y) modsPad('y');
  if (LB) bumper(-1);
  if (RB) bumper(1);
  if (START && S.mode === 'main' && S.page === 'home' && !S.sheetOpen && !S.launchOpen) openLaunch();
  drawPad(p);
  padRaf = requestAnimationFrame(pollPads);
}
function fireDir(n) { dpad(n === 'left' ? -1 : n === 'right' ? 1 : 0, n === 'up' ? -1 : n === 'down' ? 1 : 0); }
addEventListener('gamepadconnected', () => { if (!padRaf) padRaf = requestAnimationFrame(pollPads); updatePadUI(); });
addEventListener('gamepaddisconnected', updatePadUI);
const padSvg = $('.pad');
function drawPad(p) {
  $$('[data-b]', padSvg).forEach(el => {
    const i = +el.dataset.b, btn = p && p.buttons[i];
    el.classList.toggle('down', !!(btn && (btn.pressed || btn.value > .3)));
    if (el.dataset.axis !== undefined && p) {
      const ai = +el.dataset.axis, x = (p.axes[ai] || 0) * 7, y = (p.axes[ai + 1] || 0) * 7;
      el.style.transform = `translate(${x}px, ${y}px)`;
    }
  });
}
function updatePadUI() {
  const real = hostPad || pads().length > 0, on = real || S.set.padSim;
  const name = real ? 'Controller 1' : 'Controller 1 (pretend)';
  bind('padPill', on ? 'Controller connected' : 'Keyboard');
  bind('padSummary', on ? name + ', player 1' : 'Keyboard');
  bind('padCaption', real ? 'Controller 1' : on ? 'Pretend controller' : 'Press a button on your controller.');
  padSvg.classList.toggle('idle', !on);
  if (!real) drawPad(null);
}

/* ------------------------------------------------------------------ hints */
const GL = {
  keyboard: { move: ['\u2191\u2193\u2190\u2192'], a: ['Enter'], b: ['Esc'], tabs: ['Q', 'E'], play: ['Enter'], order: ['Alt', '\u2191\u2193'], x: ['\u2191\u2193'], y: ['Del'] },
  gamepad: { move: ['\u271A'], a: ['A'], b: ['B'], tabs: ['LB', 'RB'], play: ['\u2630'], order: ['X'], x: ['\u271A'], y: ['Y'] }
};
function glyphs(keys, kind) {
  return keys.map(k => `<span class="glyph${kind === 'gamepad' ? ' glyph-pad' + (k.length > 1 ? ' glyph-wide' : '') : ''}">${k}</span>`).join('');
}
function renderHints() {
  const kind = S.input === 'gamepad' || (S.set.padSim && S.input !== 'keyboard') ? 'gamepad' : 'keyboard';
  const g = GL[kind];
  const items = [[g.move, 'Move'], [g.a, 'Select']];
  if (!(S.mode === 'main' && S.page === 'home' && !S.sheetOpen)) items.push([g.b, S.sheetOpen ? 'Close' : 'Back']);
  if (S.mode === 'main' && !S.sheetOpen) items.push([g.tabs, S.page === 'settings' ? 'Switch section' : 'Switch page']);
  if (S.mode === 'main' && S.page === 'home' && kind === 'gamepad' && !S.sheetOpen) items.push([g.play, 'Play']);
  if (S.mode === 'main' && S.page === 'mods' && !S.sheetOpen) {
    items.splice(1, 1, [g.a, 'On or off'], [g.order, 'Order'], [g.y, 'Remove']);
    if (MD.lifted) items.splice(0, items.length, [g.x, 'Move'], [g.a, 'Place'], [g.b, 'Cancel']);
  }
  if (S.launchOpen) items.splice(0, items.length, [g.b, 'Back']);
  if (padTest) items.splice(0, items.length, kind === 'gamepad' ? [g.b, 'Hold to finish'] : [g.b, 'Finish']);
  const html = items.map(([k, label]) => `<span class="hint">${glyphs(k, kind)}<span>${label}</span></span>`).join('');
  const list = $('.hint-list');
  if (list.innerHTML === html) return;
  list.innerHTML = html;
  play(list, spec('fade').in, 1);
}

/* ------------------------------------------------------------ data binding */
function bind(key, text) { $$(`[data-bind="${key}"]`).forEach(el => { el.textContent = text; }); }
function summaries() {
  padSvg.dataset.turbo = S.set.preset === 'original' ? 'ls' : 'lb';
  bind('presetDesc', S.set.preset === 'original' ? 'Third turbo on left stick click' : 'Third turbo on LB');
  bind('resLabel', { 1: '640 \u00d7 480', 2: '1280 \u00d7 960', 3: '1920 \u00d7 1440', 4: '2560 \u00d7 1920', fit: 'Native' }[S.set.res]);
  bind('aspectDesc', S.chosen.has('aspect') ? '' : 'Matches the display');
  bind('resDesc', lowGpu() && !S.chosen.has('res') ? 'Set for integrated graphics' : 'Multiples of 640 × 480');
  // Menus and HUD only differ from 4:3 when the shape is wider.
  const flat = S.set.aspect === '4:3';
  const r2d = $('[data-row="aspect2d"]');
  if (r2d) { r2d.classList.toggle('row-off', flat); $$('button', r2d).forEach(b => { b.disabled = flat; }); }
  // The controller rows only act with the four-player layer on (GB_PADS=1).
  if (LIVE) {
    const padsOff = !S.set.pads;
    $$('[data-pad-row]').forEach(r => {
      r.classList.toggle('row-off', padsOff);
      $$('button', r).forEach(b => { b.disabled = padsOff; });
      $$('.slider, .cycler', r).forEach(el => { if (padsOff) el.setAttribute('aria-disabled', 'true'); else el.removeAttribute('aria-disabled'); });
    });
  }
  // Cheats: the rows act only with the master switch on; the whole page waits
  // for the game's side in the live launcher (CHEATS_READY).
  // Mouse in menus: the row is hidden until the feature is ready to offer.
  $$('[data-mouse-row]').forEach(r => { r.hidden = !MOUSE_MENUS_READY; });
  const cheatsBuilt = !LIVE || CHEATS_READY;
  $$('[data-cheats-master], [data-unlock-row]').forEach(r => {
    r.classList.toggle('row-off', !cheatsBuilt);
    $$('button', r).forEach(b => { b.disabled = !cheatsBuilt; });
  });
  $$('[data-cheat-row]').forEach(r => {
    const off = !cheatsBuilt || !S.set.cheats;
    r.classList.toggle('row-off', off);
    $$('button', r).forEach(b => { b.disabled = off; });
  });
  renderKeys();
  // The commentary level only matters with commentary on.
  const comOff = !S.set.commentary;
  $$('[data-commentary-row]').forEach(r => {
    r.classList.toggle('row-off', comOff);
    $$('.slider', r).forEach(el => { if (comOff) el.setAttribute('aria-disabled', 'true'); else el.removeAttribute('aria-disabled'); });
  });
}
/* A default that depends on another setting (the shape on the chosen display)
   is worked out again when that setting changes. */
function refreshDefaults() {
  applyDefaults();
  $$('.seg[data-setting]').forEach(renderSeg);
  $$('.toggle[data-setting]').forEach(t => t.setAttribute('aria-checked', String(!!S.set[t.dataset.setting])));
  drawPreview(false);
}

/* --------------------------------------------------------------- controls */
const CHEAT_PAIRS = {
  cheat_easy_2_pointers: 'cheat_hard_2_pointers', cheat_hard_2_pointers: 'cheat_easy_2_pointers',
  cheat_wnba_ball: 'cheat_alternate_ball', cheat_alternate_ball: 'cheat_wnba_ball'
};
/* The game's side of the cheats is built. Set to false to show the page dimmed
   and pass nothing. */
const CHEATS_READY = true;
/* Mouse in menus (GB_MOUSE_MENUS) is offered. False: its row on Controls is
   hidden. host.ps1's $script:MouseMenusReady must say the same, or Play passes
   nothing whatever the saved setting says. */
const MOUSE_MENUS_READY = false;
function setSetting(key, value) {
  S.set[key] = value;
  chose(key);
  if (['theme', 'motion', 'size'].includes(key)) {
    $$(`.seg[data-setting="${key}"]`).forEach(renderSeg);
    applyPrefs();
  }
  if (key === 'res') drawPreview(true);
  if (key === 'mods') modsMaster();
  if (key === 'mockMods') modsInit([]);
  if (key === 'padSim') { updatePadUI(); if (value) setInput('gamepad'); renderHints(); }
  // Two pairs of cheats share one game setting each: choosing one clears the other.
  if (value === true && CHEAT_PAIRS[key]) {
    const other = CHEAT_PAIRS[key];
    if (S.set[other]) { S.set[other] = false; chose(other); const t = $(`.toggle[data-setting="${other}"]`); if (t) t.setAttribute('aria-checked', 'false'); }
  }
  summaries(); save();
}
function renderSeg(seg) {
  const v = String(S.set[seg.dataset.setting]);
  $$('button', seg).forEach(b => b.setAttribute('aria-pressed', String(b.dataset.value === v)));
  moveIndicator(seg, $('[aria-pressed="true"]', seg));
}
document.addEventListener('click', e => {
  const segBtn = e.target.closest('.seg > button');
  if (segBtn) { const seg = segBtn.parentElement; setSetting(seg.dataset.setting, segBtn.dataset.value); renderSeg(seg); return; }
  const tg = e.target.closest('.toggle');
  if (tg) { const on = tg.getAttribute('aria-checked') !== 'true'; tg.setAttribute('aria-checked', String(on)); setSetting(tg.dataset.setting, on); return; }
  const arrow = e.target.closest('.cyc-arrow');
  if (arrow) { const c = arrow.closest('.cycler'); c.focus({ preventScroll: true }); cycle(c, arrow.classList.contains('cyc-prev') ? -1 : 1); return; }
});

function renderCycler(c, dir) {
  const opts = JSON.parse(c.dataset.options), key = c.dataset.setting;
  const val = $('.cyc-value', c);
  val.textContent = opts[S.set[key]] || opts[0];
  c.setAttribute('aria-valuetext', val.textContent);
  if (dir) play(val, spec('cycle').in, dir);
}
function cycle(c, d) {
  if (c.getAttribute('aria-disabled') === 'true') { nudge(c, d, 0); return; }
  const opts = JSON.parse(c.dataset.options), key = c.dataset.setting;
  const arrow = $(d < 0 ? '.cyc-prev' : '.cyc-next', c);
  arrow.classList.add(d < 0 ? 'bump-l' : 'bump-r');
  setTimeout(() => arrow.classList.remove('bump-l', 'bump-r'), 140);
  S.set[key] = (S.set[key] + d + opts.length) % opts.length;
  chose(key);
  renderCycler(c, d); tick('move');
  if (key === 'monitor') refreshDefaults();
  summaries(); save();
}

function renderSlider(sl) {
  const min = +sl.dataset.min, max = +sl.dataset.max, v = S.set[sl.dataset.setting];
  sl.style.setProperty('--pct', ((v - min) / (max - min) * 100) + '%');
  sl.setAttribute('aria-valuenow', v);
  $('.slider-value', sl).textContent = v + (sl.dataset.unit || '');
}
function stepSlider(sl, d) {
  if (sl.getAttribute('aria-disabled') === 'true') { nudge(sl, d, 0); return; }
  const key = sl.dataset.setting, step = +sl.dataset.step;
  const v = clamp(S.set[key] + d * step, +sl.dataset.min, +sl.dataset.max);
  if (v === S.set[key]) { nudge($('.slider-knob', sl), d, 0); return; }
  S.set[key] = v; chose(key); renderSlider(sl); tick('move'); save();
}
$$('.slider').forEach(sl => {
  const track = $('.slider-track', sl);
  const fromX = x => {
    if (sl.getAttribute('aria-disabled') === 'true') return;
    const r = track.getBoundingClientRect(), min = +sl.dataset.min, max = +sl.dataset.max, step = +sl.dataset.step;
    const v = min + clamp((x - r.left) / r.width, 0, 1) * (max - min);
    S.set[sl.dataset.setting] = clamp(Math.round(v / step) * step, min, max); chose(sl.dataset.setting); renderSlider(sl); save();
  };
  track.addEventListener('pointerdown', e => { sl.focus({ preventScroll: true }); sl.classList.add('dragging'); track.setPointerCapture(e.pointerId); fromX(e.clientX); });
  track.addEventListener('pointermove', e => { if (sl.classList.contains('dragging')) fromX(e.clientX); });
  const end = () => sl.classList.remove('dragging');
  track.addEventListener('pointerup', end); track.addEventListener('pointercancel', end);
});

/* Keyboard keys. Each game button's key, for the player the keyboard drives;
   the host keeps them in the player's controls.ini (key_a = K ...). Only keys
   that differ from the defaults are written. The names are the game's own.
   The defaults must match gb_pad_keys_defaults (port\src\gb_pad_logic.c).
   The turbos are three rows that say how many: the game counts turbos held,
   never which, so the "2 turbos" key presses both triggers and the "3 turbos"
   key both triggers and the left stick click (TURBO_CTLS below). */
const KEY_COLS = [
  [['a', 'A', 'K'], ['b', 'B', 'L'], ['x', 'X', 'J'], ['y', 'Y', 'I'], ['black', 'Black', 'E'], ['white', 'White', 'Q'],
   ['turbo1', '1 turbo', 'lshift'], ['turbo2', '2 turbos', 'space'], ['turbo3', '3 turbos', 'semicolon'],
   ['start', 'Start', 'enter'], ['back', 'Back', 'backspace, escape'], ['right_thumb', 'Right stick click', 'U']],
  [['dpad_up', 'D-pad up', 'up'], ['dpad_down', 'D-pad down', 'down'], ['dpad_left', 'D-pad left', 'left'], ['dpad_right', 'D-pad right', 'right'],
   ['left_stick_up', 'Left stick up', 'W, numpad8'], ['left_stick_down', 'Left stick down', 'S, numpad2'], ['left_stick_left', 'Left stick left', 'A, numpad4'], ['left_stick_right', 'Left stick right', 'D, numpad6'],
   ['right_stick_up', 'Right stick up', 'T'], ['right_stick_down', 'Right stick down', 'G'], ['right_stick_left', 'Right stick left', 'F'], ['right_stick_right', 'Right stick right', 'H']]
];
/* Which turbo keys each control of the file lists: index 0 is the 1-turbo key. */
const TURBO_CTLS = { left_trigger: [0, 1, 2], right_trigger: [1, 2], left_thumb: [2] };
const TURBO_DEF = ['lshift', 'space', 'semicolon'];
const KEY_ROWS = KEY_COLS.flat();
const K = { players: { 1: {}, 2: {}, 3: {}, 4: {} }, labels: {}, built: false };
const KEY_WORDS = { escape: 'Esc', enter: 'Enter', backspace: 'Backspace', tab: 'Tab', shift: 'Shift', ctrl: 'Ctrl', space: 'Space', capslock: 'Caps Lock', pause: 'Pause',
  pageup: 'Page Up', pagedown: 'Page Down', end: 'End', home: 'Home', left: 'Left arrow', up: 'Up arrow', right: 'Right arrow', down: 'Down arrow', insert: 'Insert', delete: 'Delete',
  numpadmultiply: 'Keypad *', numpadadd: 'Keypad +', numpadsubtract: 'Keypad -', numpaddecimal: 'Keypad .', numpaddivide: 'Keypad /',
  semicolon: ';', equals: '=', comma: ',', minus: '-', period: '.', slash: '/', grave: '`', leftbracket: '[', backslash: '\\', rightbracket: ']', quote: "'",
  lshift: 'Left Shift', rshift: 'Right Shift', lctrl: 'Left Ctrl', rctrl: 'Right Ctrl', alt: 'Alt', lalt: 'Left Alt', ralt: 'Right Alt' };
const KEY_ALIAS = { return: 'enter', esc: 'escape', control: 'ctrl' };
const keyCanon = n => { const t = String(n).trim(); const l = t.toLowerCase(); return t.length === 1 ? t.toUpperCase() : (KEY_ALIAS[l] || l); };
const keyList = v => String(v).split(',').map(keyCanon).filter(k => k && k !== 'none');
/* Keys are bound by their place on the keyboard: a name is the US key at that
   place (port\src\gb_pad_logic.c), and the label shown is what the player's
   own keyboard prints there (from the host, or from the key just pressed). */
const POS_KEYS = new Set([...'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', 'minus', 'equals', 'leftbracket', 'rightbracket', 'semicolon', 'quote', 'grave', 'backslash', 'comma', 'period', 'slash']);
function keyLabel(n) {
  if (POS_KEYS.has(n) && K.labels[n]) return K.labels[n];
  const m = /^numpad(\d)$/.exec(n); return m ? `Keypad ${m[1]}` : KEY_WORDS[n] || (/^f\d+$/.test(n) ? n.toUpperCase() : n);
}
/* Shift held on either side also counts as the Shift of that side. */
const keysOverlap = (a, b) => a === b || ['shift', 'ctrl', 'alt'].some(w => (a === w && (b === 'l' + w || b === 'r' + w)) || (b === w && (a === 'l' + w || a === 'r' + w)));
const keysPlayer = () => (S.set.pads ? S.set.kbPlayer : 1);
const fileKeys = (ctl, def, p) => keyList((K.players[p] || {})[ctl] !== undefined ? K.players[p][ctl] : def);
/* The three turbo keys, read back from the file's three controls. */
function turboKeys(p = keysPlayer()) {
  const lt = fileKeys('left_trigger', TURBO_DEF.join(', '), p), rt = fileKeys('right_trigger', TURBO_DEF.slice(1).join(', '), p);
  const th = fileKeys('left_thumb', TURBO_DEF[2], p);
  const t3 = th.find(k => rt.includes(k) && lt.includes(k)) || th[0] || '';
  const t2 = rt.find(k => k !== t3 && lt.includes(k)) || rt.find(k => k !== t3) || '';
  const t1 = lt.find(k => k !== t2 && k !== t3) || '';
  return [t1, t2, t3];
}
function setTurbo(p, i, name) {
  const t = turboKeys(p); t[i] = name;
  const same = t.every((k, j) => keyCanon(k) === keyCanon(TURBO_DEF[j]));
  for (const [ctl, idx] of Object.entries(TURBO_CTLS)) {
    if (same) delete K.players[p][ctl];
    else K.players[p][ctl] = idx.map(j => t[j]).filter(Boolean).join(', ') || 'none';
  }
}
const keysOf = (ctl, def, p = keysPlayer()) => {
  const m = /^turbo([123])$/.exec(ctl);
  if (m) { const k = turboKeys(p)[m[1] - 1]; return k ? [k] : []; }
  return fileKeys(ctl, def, p);
};
function renderKeys() {
  const grid = $('[data-keys-grid]');
  if (!K.built) {
    grid.innerHTML = KEY_COLS.map(col => `<div class="rows">${col.map(([c, label]) => `<div class="row"><div class="row-text"><div class="row-label">${label}</div></div><div class="row-control"><button class="btn btn-small btn-key" data-rebind="${c}"><span class="keycap"></span><span class="rebind-label">Change</span></button></div></div>`).join('')}</div>`).join('');
    K.built = true;
  }
  const p = keysPlayer();
  bind('keysFor', p ? `Keys for player ${p}` : 'The keyboard drives no player');
  $$('[data-keys-grid] .row, [data-keys-head]').forEach(r => r.classList.toggle('row-off', !p));
  $$('[data-rebind], [data-keys-reset]').forEach(b => { b.disabled = !p; });
  for (const [c, , def] of KEY_ROWS) {
    const b = $(`[data-rebind="${c}"]`);
    if (b !== capture) $('.keycap', b).textContent = keysOf(c, def).map(keyLabel).join(' or ') || 'None';
  }
}
/* The key's place (event.code), not its label, so the same key on every layout. */
const CODE_KEYS = { Backspace: 'backspace', Tab: 'tab', Enter: 'enter', ShiftLeft: 'lshift', ShiftRight: 'rshift', ControlLeft: 'lctrl', ControlRight: 'rctrl',
  AltLeft: 'alt', AltRight: 'alt', Pause: 'pause', CapsLock: 'capslock', Escape: 'escape', Space: 'space', PageUp: 'pageup', PageDown: 'pagedown', End: 'end', Home: 'home',
  ArrowLeft: 'left', ArrowUp: 'up', ArrowRight: 'right', ArrowDown: 'down', Insert: 'insert', Delete: 'delete',
  Semicolon: 'semicolon', Equal: 'equals', Comma: 'comma', Minus: 'minus', Period: 'period', Slash: 'slash', Backquote: 'grave',
  BracketLeft: 'leftbracket', Backslash: 'backslash', BracketRight: 'rightbracket', Quote: 'quote' };
function keyFromEvent(e) {
  const c = e.code || ''; let m;
  if ((m = /^Key([A-Z])$/.exec(c)) || (m = /^Digit(\d)$/.exec(c))) return m[1];
  if ((m = /^F(\d{1,2})$/.exec(c))) return +m[1] <= 12 ? 'f' + m[1] : '';
  if (CODE_KEYS[c]) return CODE_KEYS[c];
  if (c && !c.startsWith('Numpad')) return '';      // a key the game has no name for (the extra key left of Z, media keys)
  // The keypad keeps Windows' own codes (Num Lock decides digits or arrows), as the game reads them.
  const k = e.keyCode;
  if ((k >= 65 && k <= 90) || (k >= 48 && k <= 57)) return String.fromCharCode(k);
  if (k >= 96 && k <= 105) return 'numpad' + (k - 96);
  if (k >= 112 && k <= 123) return 'f' + (k - 111);
  return { 8: 'backspace', 9: 'tab', 13: 'enter', 16: 'shift', 17: 'ctrl', 18: 'alt', 19: 'pause', 20: 'capslock', 32: 'space', 33: 'pageup', 34: 'pagedown', 35: 'end', 36: 'home',
    37: 'left', 38: 'up', 39: 'right', 40: 'down', 45: 'insert', 46: 'delete', 106: 'numpadmultiply', 107: 'numpadadd', 109: 'numpadsubtract', 110: 'numpaddecimal', 111: 'numpaddivide',
    186: 'semicolon', 187: 'equals', 188: 'comma', 189: 'minus', 190: 'period', 191: 'slash', 192: 'grave', 219: 'leftbracket', 220: 'backslash', 221: 'rightbracket', 222: 'quote' }[k] || '';
}
const KEY_TAKEN = { f11: 'F11 switches fullscreen in the game.', f12: 'F12 takes a screenshot in the game.', alt: 'Alt belongs to Windows.' };
function cancelRebind() {
  if (!capture) return;
  const b = capture; capture = null; b.classList.remove('listening');
  $('.rebind-label', b).textContent = 'Change'; bind('keysMsg', ''); renderKeys();
}
function saveKeys() { if (LIVE) send('live:keys:' + JSON.stringify({ player: keysPlayer(), keys: K.players[keysPlayer()] })); }
function finishRebind(e) {
  const name = keyCanon(keyFromEvent(e));
  if (POS_KEYS.has(name) && e.key && e.key.length === 1 && e.key.trim()) K.labels[name] = e.key.toUpperCase();
  const refuse = why => { bind('keysMsg', `${why} Press another key.`); tick('back'); };
  if (!name) return refuse('That key cannot be used.');
  if (KEY_TAKEN[name]) return refuse(KEY_TAKEN[name]);
  const ctl = capture.dataset.rebind, row = KEY_ROWS.find(r => r[0] === ctl);
  const other = KEY_ROWS.find(([c, , def]) => c !== ctl && keysOf(c, def).some(k => keysOverlap(k, name)));
  if (other) return refuse(`${keyLabel(name)} is already ${other[1]}.`);
  const p = keysPlayer(), turbo = /^turbo([123])$/.exec(ctl);
  if (turbo) setTurbo(p, turbo[1] - 1, name);
  else if (keyCanon(row[2]) === name && !row[2].includes(',')) delete K.players[p][ctl]; else K.players[p][ctl] = name;
  const b = capture; cancelRebind();
  play($('.keycap', b), spec('cycle').in, 1); tick('select');
  saveKeys();
}
function onKeys(o) {
  if (o.ok === false) { toast(o.text || 'The keys could not be saved', 'info'); return; }
  if (o.labels) K.labels = Object.assign({}, o.labels);
  if (o.players) {
    for (const p of [1, 2, 3, 4]) {
      K.players[p] = Object.assign({}, o.players[p] || o.players[String(p)] || {});
      if (o.letterKeyed && o.letterKeyed[p] && o.placeOf) letterDefaults(p, o.placeOf);
    }
    renderKeys();
  }
}
/* A player whose file was written by letter (before keys went by place) keeps
   the whole old keyboard, as the game reads it: the defaults the file does not
   list keep their letters, at those letters' places on this keyboard. */
function letterDefaults(p, placeOf) {
  const defs = KEY_ROWS.filter(([c]) => !/^turbo/.test(c)).map(([c, , d]) => [c, d])
    .concat(Object.entries(TURBO_CTLS).map(([c, idx]) => [c, idx.map(j => TURBO_DEF[j]).join(', ')]));
  for (const [ctl, def] of defs) {
    if (K.players[p][ctl] !== undefined) continue;
    const was = keyList(def), now = was.map(k => keyCanon(placeOf[k] || k));
    if (now.join(', ') !== was.join(', ')) K.players[p][ctl] = now.join(', ');
  }
}
document.addEventListener('click', e => {
  if (e.target.closest('[data-keys-reset]')) { cancelRebind(); K.players[keysPlayer()] = {}; renderKeys(); saveKeys(); toast('Default keys restored', 'ok'); return; }
  const b = e.target.closest('[data-rebind]');
  if (!b || b.disabled) return;
  if (capture === b) return;
  cancelRebind();
  capture = b; b.classList.add('listening');
  $('.rebind-label', b).textContent = 'Press a key'; bind('keysMsg', '');
});
document.addEventListener('pointerdown', e => { if (capture && !e.target.closest('[data-rebind]')) cancelRebind(); });

/* Accordion */
document.addEventListener('click', e => {
  const h = e.target.closest('.acc-head');
  if (!h) return;
  const body = h.nextElementSibling, open = h.getAttribute('aria-expanded') !== 'true';
  h.setAttribute('aria-expanded', String(open));
  if (open) {
    body.hidden = false;
    const hgt = body.scrollHeight;
    play(body, spec('expand').in, hgt);
  } else {
    play(body, spec('expand').out, body.scrollHeight).then(() => { if (h.getAttribute('aria-expanded') === 'false') body.hidden = true; });
  }
});

/* Toasts */
function toast(text, kind = 'info') {
  const t = document.createElement('div');
  t.className = 'toast toast-' + kind; t.setAttribute('role', 'status'); t.textContent = text;
  $('.toasts').prepend(t);
  play(t, spec('toast').in, 1);
  setTimeout(() => play(t, spec('toast').out, 1).then(() => t.remove()), 3400);
}
document.addEventListener('click', e => { const b = e.target.closest('[data-toast]'); if (b && !(LIVE && b.dataset.toast.startsWith('Mockup'))) toast(b.dataset.toast); });

/* Resolution preview: an abstract scene drawn at the chosen pixel count */
const cv = $('.preview-canvas');
function drawPreview(animate) {
  const scale = S.set.res === 'fit' ? 8 : +S.set.res * 1.5;
  const w = Math.round(64 * scale), h = Math.round(48 * scale);
  cv.width = w; cv.height = h;
  const c = cv.getContext('2d');
  const css = getComputedStyle(root);
  const acc = css.getPropertyValue('--accent').trim() || '#7c8cff', acc2 = css.getPropertyValue('--art-2').trim() || '#40e0d0';
  const g = c.createLinearGradient(0, 0, 0, h); g.addColorStop(0, '#0d1424'); g.addColorStop(1, '#1d2a44');
  c.fillStyle = g; c.fillRect(0, 0, w, h);
  c.save(); c.translate(w * .68, h * .36);
  c.strokeStyle = acc; c.lineWidth = w * .012;
  for (let i = 0; i < 4; i++) { c.beginPath(); c.arc(0, 0, w * (.08 + i * .045), 0, Math.PI * 2); c.globalAlpha = 1 - i * .2; c.stroke(); }
  c.restore(); c.globalAlpha = 1;
  c.fillStyle = acc2; c.beginPath(); c.moveTo(0, h * .78); c.lineTo(w * .55, h * .58); c.lineTo(w, h * .7); c.lineTo(w, h); c.lineTo(0, h); c.fill();
  c.strokeStyle = 'rgba(255,255,255,.55)'; c.lineWidth = Math.max(1, w * .004);
  for (let i = -6; i < 14; i++) { c.beginPath(); c.moveTo(i * w * .09, h); c.lineTo(i * w * .09 + w * .25, h * .62); c.stroke(); }
  c.fillStyle = '#fff'; c.font = `700 ${Math.round(h * .16)}px ${css.getPropertyValue('--font-display') || 'sans-serif'}`;
  c.fillText('Aa 123', w * .07, h * .3);
  if (animate) play(cv, spec('fade').in, 1);
}

/* Rows follow each other in from the seam side after a change of section. */
function cascade(scope) {
  $$('.row, .ver, .acc, .tool, .upd-top, .pick-tile', scope).filter(el => el.getClientRects().length).forEach((el, i) => {
    // .upd-top holds things placed on the ring: a transform would make it their
    // containing block and move them while it runs, so it only fades in.
    const slide = !el.matches('.upd-top');
    el.animate(slide ? [{ opacity: 0, transform: 'translateX(-1rem)' }, { opacity: 1, transform: 'none' }] : [{ opacity: 0 }, { opacity: 1 }],
      { duration: 340, delay: 110 + Math.min(i, 9) * 38, easing: 'cubic-bezier(.05, .7, .1, 1)', fill: 'backwards' });
  });
}

/* Play sends a few quiet rings out of the halo ring itself. Deliberately
   calm: an earlier bright, fast version was far too much. */
function splash() {
  const el = document.createElement('div');
  el.className = 'splash';
  el.innerHTML = '<i class="shock"></i><i class="shock"></i><i class="shock"></i>';
  app.append(el);
  setTimeout(() => el.remove(), 5600);
}

/* A bead of light runs along the seam in the direction of travel. */
function pulse(dir) {
  const s = $('.seam');
  s.classList.remove('pulse', 'pulse-up'); void s.offsetWidth;
  s.classList.add(dir < 0 ? 'pulse-up' : 'pulse');
}
/* While the menu unfolds or folds, keep the gliding markers and the focus ring on their targets. */
let followUntil = 0;
function followLayout() {
  const running = performance.now() < followUntil;
  followUntil = performance.now() + 750;
  if (running) return;
  const tick = () => {
    moveIndicator($('.nav'), $('.nav [aria-current="page"]'));
    moveIndicator($('.tabs'), $('.tabs [aria-selected="true"]'));
    if (!ringHeld) placeRing(document.activeElement);
    if (performance.now() < followUntil) requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

/* ------------------------------------------------------------- navigation */
const PAGES = ['home', 'settings', 'mods', 'updates', 'about'];
const TABS = ['display', 'audio', 'controls', 'keyboard', 'game', 'cheats', 'advanced'];
async function go(page, opts = {}) {
  if (opts.tab) setTab(opts.tab, page !== S.page);
  if (page === S.page) { if (opts.tab) focusEl($(`.tab[data-tab="${opts.tab}"]`)); return; }
  const oldEl = $(`[data-page="${S.page}"]`), newEl = $(`[data-page="${page}"]`);
  const dir = Math.sign(PAGES.indexOf(page) - PAGES.indexOf(S.page)) || 1;
  setPadTest(false);
  S.page = page; app.dataset.view = page;
  if (hostBridge) hostBridge.postMessage('view ' + page);
  if (page === 'mods' && LIVE && !MD.installing && !MD.incoming) send('live:modsget');
  if (page !== 'mods' && MD.lifted) drop(true);
  $('.subnav').inert = page !== 'settings';
  pulse(dir); followLayout();
  const navPage = opts.nav || page;
  $$('.nav-item').forEach(n => n.toggleAttribute('aria-current', false));
  $(`.nav-item[data-goto="${navPage}"]`).setAttribute('aria-current', 'page');
  moveIndicator($('.nav'), $('.nav [aria-current="page"]'));
  renderHints();
  const act = document.activeElement;
  const fromNav = !!act && act.matches('.nav-item, .tab');
  // focus in the menu always sits on the section being shown
  if (fromNav && (act.matches('.tab') ? page !== 'settings' : act.dataset.goto !== navPage)) $(`.nav-item[data-goto="${navPage}"]`).focus({ preventScroll: true });
  holdRing(!fromNav);
  const p = swap(oldEl, newEl, 'page', dir, $$('.page'));
  cascade(newEl);
  requestAnimationFrame(() => refreshIndicators(true, newEl));
  if (page === 'settings') requestAnimationFrame(() => moveIndicator($('.tabs'), $('.tabs [aria-selected="true"]'), true));
  if (!fromNav) focusFirst(newEl);
  await p;
  holdRing(false);
  refreshIndicators(true, newEl);
  if (page === 'settings') moveIndicator($('.tabs'), $('.tabs [aria-selected="true"]'), true);
  placeRing(document.activeElement, !fromNav);
}
function setTab(name, instant) {
  if (name === S.tab) return;
  const dir = Math.sign(TABS.indexOf(name) - TABS.indexOf(S.tab)) || 1;
  const oldP = $(`[data-panel="${S.tab}"]`), newP = $(`[data-panel="${name}"]`);
  setPadTest(false); cancelRebind();
  if (name === 'keyboard' && LIVE) send('live:keysget');
  S.tab = name; if (!instant) pulse(dir);
  $$('.tab').forEach(t => t.setAttribute('aria-selected', String(t.dataset.tab === name)));
  moveIndicator($('.tabs'), $('.tabs [aria-selected="true"]'), instant);
  if (instant) { oldP.hidden = true; newP.hidden = false; refreshIndicators(true, newP); return; }
  swap(oldP, newP, 'tab', dir, $$('.tab-panel'));
  cascade(newP);
  requestAnimationFrame(() => refreshIndicators(true, newP));
  if (name === 'display') drawPreview(false);
}
function cycleTab(d) {
  const n = TABS[(TABS.indexOf(S.tab) + d + TABS.length) % TABS.length];
  setTab(n); focusEl($(`.tab[data-tab="${n}"]`));
}
document.addEventListener('click', e => {
  const t = e.target.closest('.tab');
  if (t) { setTab(t.dataset.tab); return; }
  const g = e.target.closest('[data-goto]');
  if (g) go(g.dataset.goto, { tab: g.dataset.gotoTab });
});

/* --------------------------------------------------------- mode switching */
async function setMode(mode, step = 0) {
  if (S.mode === mode) { if (mode === 'setup') showStep(step); return; }
  const outEl = mode === 'main' ? $('.setup') : $('.main');
  holdRing(true);
  await play(outEl, spec('mode').out, 1);
  outEl.getAnimations().forEach(a => a.cancel());
  S.mode = mode; app.dataset.mode = mode; halo('idle');
  renderHints();
  const inEl = mode === 'main' ? $('.main') : $('.setup');
  if (mode === 'setup') showStep(step, true);
  else { $$('.page').forEach(p => { p.hidden = p.dataset.page !== S.page; }); }
  refreshIndicators(true);
  const pIn = play(inEl, spec('mode').in, 1);
  if (mode === 'main') { play($('.topbar'), spec('mode').in, 1).then(a => a && a.cancel()); }
  focusFirst(mode === 'main' ? $(`[data-page="${S.page}"]`) : $(`[data-step-panel="${S.step}"]`));
  const a = await pIn; if (a) a.cancel();
  holdRing(false);
  refreshIndicators(true);
  placeRing(document.activeElement, true);
}

/* ------------------------------------------------------------------ setup */
const steps = $$('.step');
async function showStep(i, instant) {
  const oldEl = $(`[data-step-panel="${S.step}"]`), newEl = $(`[data-step-panel="${i}"]`);
  const dir = Math.sign(i - S.step) || 1;
  const prev = S.step;
  S.step = i; app.dataset.step = i; pulse(dir);
  if (hostBridge) hostBridge.postMessage('step ' + i);
  $$('.step-rail li').forEach(li => {
    const n = +li.dataset.rail;
    li.classList.toggle('done', n < i);
    li.toggleAttribute('aria-current', false);
    if (n === i) li.setAttribute('aria-current', 'step');
  });
  moveIndicator($('.step-rail'), $('.step-rail [aria-current="step"]'), instant);
  renderHints();
  if (instant || prev === i) { steps.forEach(s => { s.hidden = s !== newEl; }); enterStep(i); focusFirst(newEl); return; }
  holdRing(true);
  const p = swap(oldEl, newEl, 'step', dir, steps);
  cascade(newEl);
  focusFirst(newEl);
  enterStep(i);
  await p;
  holdRing(false); placeRing(document.activeElement, true);
}
function enterStep(i) {
  if (LIVE && LIVE.auto && i >= 1) setTimeout(autoStep, 300);
  // Back on step 0 because of an error: keep its alert; "Check again" runs the checks.
  if (i === 0) { if (LIVE && L.holdChecks) L.holdChecks = false; else runChecks(); }
  if (i === 3 && !S.busy && !buildDone) runBuild();
  if (i === 4) { S.built = true; save(); halo('ok', 100); const m = $('.done-mark'); m.classList.remove('drawn'); void m.getBoundingClientRect(); requestAnimationFrame(() => m.classList.add('drawn')); }
}
document.addEventListener('click', e => {
  if (e.target.closest('[data-next]')) { if (S.step === 0 && sacBlocked) return; showStep(S.step + 1); }
  else if (e.target.closest('[data-prev]')) showStep(S.step - 1);
  else if (e.target.closest('[data-finish]')) setMode('main');
  else if (e.target.closest('[data-start-setup]')) { buildDone = false; resetBuild(); setMode('setup', +e.target.closest('[data-start-setup]').dataset.startSetup); }
});

function alertHtml(kind, title, body, actions = []) {
  return `<div class="alert ${kind === 'warn' ? 'alert-warn' : ''}" role="alert"><span class="alert-icon">!</span><div class="alert-title">${title}</div><div class="alert-body">${body}</div>` +
    (actions.length ? `<div class="alert-actions">${actions.map(([attr, label, primary]) => `<button class="btn btn-small${primary ? ' btn-primary' : ''}" ${attr}>${label}</button>`).join('')}</div>` : '') + '</div>';
}
function showAlert(step, html) {
  const slot = $(`[data-alert="${step}"]`);
  slot.innerHTML = html;
  if (html) play(slot.firstElementChild, spec('alert').in, 1);
}

/* Step 0: checks of this PC */
let sacBlocked = false;
async function runChecks() {
  if (LIVE) return liveChecks();
  const sac = consume('sac');
  sacBlocked = false;
  const rows = $$('.check');
  rows.forEach(r => { r.className = 'check pending'; });
  bind('sacLabel', 'Smart App Control');
  showAlert(0, '');
  for (const r of rows) {
    await sleep(reduced() ? 120 : 260);
    if (r.dataset.check === 'sac') {
      bind('sacLabel', sac ? 'Smart App Control is on' : 'Smart App Control is off');
      r.className = 'check ' + (sac ? 'bad' : 'ok');
    } else r.className = 'check ok';
  }
  if (sac) {
    sacBlocked = true; halo('error');
    $('[data-step-panel="0"] [data-next]').disabled = true;
    showAlert(0, alertHtml('err', 'Smart App Control is on',
      'Windows blocks the game while it is on. Turn it off in Windows Security, then check again.',
      [['data-recheck', 'Check again', true]]));
    focusEl($('[data-recheck]'));
  } else { halo('idle'); $('[data-step-panel="0"] [data-next]').disabled = false; }
}
document.addEventListener('click', e => { if (e.target.closest('[data-recheck]') && LIVE) { liveChecks(); return; } if (e.target.closest('[data-recheck]')) { toast('Mockup: pretending it was switched off.'); runChecks().then(() => focusFirst($('[data-step-panel="0"]'))); } });

/* Step 1: the disc */
const HASH_OK = '08389AE31309FACD89EC7A1057CEC32C4308D734D0B8AD9151437AA511CBDCFE';
const HASH_BAD = '5D1F0C7AB2E94468F0C3B19A77D2E60CC8814F0A9D3B2E75C61A0F94E2D7B381';
let verifying = 0;
async function pick(kind) {
  if (LIVE) return livePick(kind);
  const run = ++verifying; halo('idle');
  const panel = $('[data-step-panel="1"]');
  const next = $('[data-next]', panel);
  next.disabled = true;
  showAlert(1, '');
  const compressed = consume('compressed');
  const sc = ['game', 'release', 'modified'].find(consume) || 'ok';
  bind('fileName', kind === 'folder' ? 'D:\\Dumps\\my-disc (extracted)' : compressed ? 'D:\\Dumps\\my-disc.cso' : 'D:\\Dumps\\my-disc.iso');
  bind('fileMeta', kind === 'folder' ? '355 files, 2.1 GB' : compressed ? 'Compressed image, 3.9 GB' : 'Disc image, 7.3 GB');
  const pickEl = $('.pick', panel), ver = $('.verify', panel);
  if (!pickEl.hidden) { await play(pickEl, spec('fade').out, 1); pickEl.hidden = true; }
  ver.hidden = false; play(ver, spec('alert').in, 1);
  const line = $('.verify-line', panel); line.className = 'verify-line';
  bind('hash', ''); bind('verifyStatus', 'Opening');
  focusEl($('[data-repick]'));
  await sleep(500); if (run !== verifying) return;
  if (compressed) {
    line.className = 'verify-line bad'; halo('error'); bind('verifyStatus', 'This kind of image cannot be read');
    showAlert(1, alertHtml('err', 'Compressed images are not supported',
      'Convert it to a plain <span class="mono">.iso</span>, then choose it again.',
      [['data-repick', 'Choose another image', true]]));
    focusEl($('[data-alert="1"] [data-repick]')); return;
  }
  bind('verifyStatus', 'Reading default.xbe');
  await sleep(500); if (run !== verifying) return;
  bind('verifyStatus', 'Fingerprinting');
  const target = sc === 'ok' ? HASH_OK : HASH_BAD;
  const hv = $('[data-bind="hash"]');
  for (let n = 0; n <= 64; n += reduced() ? 64 : 4) {
    hv.textContent = target.slice(0, n) + (n < 64 ? '\u2026' : '');
    await sleep(22); if (run !== verifying) return;
  }
  if (sc === 'ok') {
    line.className = 'verify-line ok'; bind('verifyStatus', 'Supported release');
    next.disabled = false; focusEl(next); return;
  }
  line.className = 'verify-line bad'; halo('error');
  const msgs = {
    game: ['Wrong game', 'This image is a different game.'],
    release: ['Wrong release', 'This is the European release. Only the USA release is supported.'],
    modified: ['Image does not match', `<span class="mono">default.xbe</span> differs from the supported release.<br>Expected <span class="mono">${HASH_OK.slice(0, 16)}\u2026</span>, found <span class="mono">${HASH_BAD.slice(0, 16)}\u2026</span>`]
  }[sc];
  bind('verifyStatus', 'Not the supported release');
  showAlert(1, alertHtml('err', msgs[0], msgs[1], [['data-repick', 'Choose another image', true], ['data-toast="Mockup: the project\u2019s help page on making an image of your own disc."', 'How to dump your disc']]));
  focusEl($('[data-alert="1"] [data-repick]'));
}
document.addEventListener('click', e => {
  const p = e.target.closest('[data-pick]');
  if (p) { pick(p.dataset.pick); return; }
  if (e.target.closest('[data-repick]')) {
    verifying++; halo('idle');
    const panel = $('[data-step-panel="1"]');
    showAlert(1, '');
    $('.verify', panel).hidden = true;
    const pk = $('.pick', panel); pk.hidden = false; play(pk, spec('alert').in, 1);
    $('[data-next]', panel).disabled = true;
    focusFirst(pk);
  }
});

/* Step 2: tools */
let toolsDone = false;
async function download() {
  if (LIVE) return liveSetup();
  const panel = $('[data-step-panel="2"]');
  const dl = $('[data-download]', panel), next = $('[data-next]', panel);
  showAlert(2, '');
  if (consume('space')) {
    halo('error');
    showAlert(2, alertHtml('err', 'Not enough disk space', '4.0 GB needed, 1.2 GB free on C:.',
      [['data-download', 'Check again', true], ['data-toast="Mockup: a folder picker would open here."', 'Choose another folder']]));
    focusEl($('[data-alert="2"] [data-download]')); return;
  }
  const av = consume('antivirus');
  S.busy = true; dl.disabled = true; dl.textContent = 'Downloading';
  const tools = $$('.tool', panel);
  const sizes = [12.6, 1.3, 191, 56.3, 0.3];
  tools.forEach(t => { t.className = 'tool'; $('.bar-fill', t).style.setProperty('--p', '0%'); $('.tool-state', t).textContent = ''; });
  const speed = S.set.speed === 'fast' ? 34 : 14; // MB per tick
  const prog = sizes.map(() => 0);
  let doneCount = 0;
  await new Promise(resolve => {
    const tick = () => {
      tools.forEach((t, i) => {
        if (prog[i] >= 1) return;
        const share = speed / (5 - doneCount) * (0.7 + Math.random() * .6);
        prog[i] = Math.min(1, prog[i] + share / sizes[i]);
        $('.bar-fill', t).style.setProperty('--p', (prog[i] * 100).toFixed(1) + '%');
        $('.tool-state', t).textContent = prog[i] < 1 ? Math.round(prog[i] * 100) + '%' : 'Checking';
        if (prog[i] >= 1) {
          doneCount++;
          setTimeout(() => {
            const bad = av && t.dataset.tool === 'clang';
            t.className = 'tool ' + (bad ? 'bad' : 'ok');
            $('.tool-state', t).textContent = bad ? 'Removed' : 'Verified';
          }, 350);
        }
      });
      halo('work', prog.reduce((a, v, i) => a + v * sizes[i], 0) / 261.5 * 100);
      if (doneCount < 5) setTimeout(tick, 120); else setTimeout(resolve, 500);
    };
    tick();
  });
  S.busy = false;
  if (av) {
    halo('error');
    dl.disabled = false; dl.textContent = 'Download 191 MB';
    showAlert(2, alertHtml('err', 'clang.exe was removed',
      'Your antivirus deleted it after the download. Restore it, or allow the tools folder, then try again.',
      [['data-download', 'Try again', true]]));
    focusEl($('[data-alert="2"] [data-download]')); return;
  }
  halo('idle'); toolsDone = true; dl.hidden = true; next.hidden = false; focusEl(next);
}
document.addEventListener('click', e => { if (e.target.closest('[data-download]')) { if (!S.busy) download(); } });

/* Step 3: build */
const STAGES = [40, 117, 3, 65, 240, 6];
const TOTAL = STAGES.reduce((a, b) => a + b, 0);
let buildDone = false, buildRun = 0;
const logEl = $('.log');
let logLines = 0;
function log(text, cls) {
  const line = document.createElement('span');
  if (cls) line.className = cls;
  line.textContent = text + '\n';
  logEl.append(line); logLines++;
  if (logEl.childNodes.length > 500) logEl.firstChild.remove();
  bind('logCount', logLines + ' lines');
  logEl.scrollTop = logEl.scrollHeight;
}
function fmt(s) { s = Math.max(0, Math.round(s)); return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); }
function resetBuild() {
  $$('.stage[data-stage]').forEach(st => { st.className = 'stage'; $('.stage-time', st).textContent = ''; });
  logEl.textContent = ''; logLines = 0;
  bind('buildPct', '0'); halo('idle', 0);
  bind('buildStage', 'Building'); bind('buildEta', 'about 8 minutes left'); bind('compileDetail', 'Compile'); showAlert(3, '');
  $('[data-retry]').hidden = true;
}
async function runBuild() {
  if (LIVE) return liveSetup();
  const run = ++buildRun;
  resetBuild();
  const fail = consume('buildfail');
  S.busy = true;
  const rate = S.set.speed === 'fast' ? TOTAL / 20 : TOTAL / 60; // simulated seconds per real second
  const names = ['Unpacking', 'Reading the code', 'Identifying', 'Translating', 'Compiling', 'Linking'];
  let sim = 0;
  log('gamebreaker-build 0.3.0', 'l-dim');
  log('tools: python 3.14 (embeddable), capstone 5.0.9, clang (llvm-mingw), cmake, ninja', 'l-dim');
  log(`disc: default.xbe sha256 ${HASH_OK.slice(0, 12)}\u2026 OK (supported release)`, 'l-ok');
  for (let i = 0; i < STAGES.length; i++) {
    const st = $(`.stage[data-stage="${i}"]`);
    st.className = 'stage active';
    bind('buildStage', names[i]);
    const startSim = sim;
    const startLog = ['unpack: 355 files', 'disasm: scanning 7 code sections', 'func_id: matching library signatures', 'lift: translating functions to C', 'compile: 138 files on 16 cores', 'link: gamebreaker.exe'][i];
    log(startLog);
    let lastFile = 0;
    while (sim < startSim + STAGES[i]) {
      await sleep(80);
      if (run !== buildRun) return;
      sim = Math.min(startSim + STAGES[i], sim + rate * .08);
      const frac = (sim - startSim) / STAGES[i];
      if (i === 4) {
        const f = Math.floor(frac * 138);
        bind('compileDetail', `Compile ${f} of 138`);
        while (lastFile < f) { lastFile++; if (lastFile % 3 === 0 || lastFile > 130) log(`  [${String(lastFile).padStart(3)}/138] ${lastFile <= 70 ? 'recomp_' + String(lastFile).padStart(4, '0') + '.c' : 'runtime/' + ['kernel', 'nv2a', 'apu', 'input', 'video'][lastFile % 5] + '_' + lastFile + '.c'}`, 'l-dim'); }
        if (fail && f >= 97) {
          st.className = 'stage bad';
          log('error: the compiler ran out of memory on recomp_0041.c (exit code 1)', 'l-err');
          log('build stopped. Nothing that was built before was changed.', 'l-err');
          bind('buildStage', 'Stopped'); bind('buildEta', 'while compiling'); halo('error');
          S.busy = false;
          showAlert(3, alertHtml('err', 'The compiler ran out of memory',
            'Close other programs and try again.',
            [['data-retry', 'Try again', true]]));
          focusEl($('[data-alert="3"] [data-retry]'));
          return;
        }
      }
      if (i === 1 && Math.random() < .08) log(`  disasm: ${Math.round(frac * 16482).toLocaleString('en-US')} functions so far`, 'l-dim');
      if (i === 3 && Math.random() < .1) log(`  lift: ${Math.round(frac * 16498).toLocaleString('en-US')} / 16,498`, 'l-dim');
      const pct = Math.floor(sim / TOTAL * 100);
      bind('buildPct', String(pct)); halo('work', sim / TOTAL * 100);
      const mins = Math.max(1, Math.ceil((TOTAL - sim) / 60));
      bind('buildEta', `about ${mins} ${mins === 1 ? 'minute' : 'minutes'} left`);
    }
    st.className = 'stage done';
    $('.stage-time', st).textContent = STAGES[i] >= 60 ? fmt(STAGES[i]) : STAGES[i] + ' s';
    if (i === 1) log('  disasm: 16,482 functions', 'l-ok');
    if (i === 3) log('  lift: 16,498 / 16,498 functions, 0 failed, 67 chunks', 'l-ok');
    if (i === 4) bind('compileDetail', 'Compile');
  }
  log(`done in ${Math.floor(TOTAL / 60)} min ${TOTAL % 60} s. Game program ready.`, 'l-ok');
  bind('buildPct', '100'); bind('buildStage', 'Built'); bind('buildEta', ''); halo('ok', 100);
  bind('doneLine', `Built in ${Math.floor(TOTAL / 60)} min ${TOTAL % 60} s`);
  S.busy = false; buildDone = true;
  await sleep(700);
  if (run === buildRun && S.mode === 'setup' && S.step === 3) showStep(4);
}
document.addEventListener('click', e => {
  if (e.target.closest('[data-retry]')) { if (LIVE) liveSetup(); else runBuild(); focusEl($('[data-log-toggle]')); return; }
  const lt = e.target.closest('[data-log-toggle]');
  if (lt) {
    const dr = $('.log-drawer'), open = dr.hidden;
    app.dataset.log = open ? '1' : '0';
    $$('[data-log-toggle]').forEach(b => { b.setAttribute('aria-expanded', String(open)); b.textContent = open ? 'Hide log' : 'Show log'; });
    if (open) { dr.hidden = false; play(dr, spec('drawer').in, 1); logEl.scrollTop = logEl.scrollHeight; }
    else play(dr, spec('drawer').out, 1).then(a => { dr.hidden = true; if (a) a.cancel(); });
  }
});

/* ------------------------------------------------------------ update sheet */
const sheet = $('.sheet'), scrim = $('.scrim');
let sheetReturn = null;
async function openSheet() {
  if (LIVE) { if (U.latest && !U.active) { fillUpdateSheet(); return showSheet('info'); } if (!U.latest) toast('Up to date', 'ok'); return; }
  if (S.updated) { toast('Up to date', 'ok'); return; }
  return showSheet('info');
}
/* The same sheet holds the update, the backup list and its confirmation, and
   the screen after a fatal error in the game. */
function sheetState(name) { $$('.sheet-state').forEach(s => { s.hidden = s.dataset.sheet !== name; }); focusFirst(sheet); }
let sheetFrom = null;
async function showSheet(name) {
  if (S.sheetOpen) { sheetState(name); return; }
  sheetReturn = document.activeElement;
  // The sheet opens inside the halo ring, which only sits mid-window on Play;
  // from another page go there, and come back when it closes. The menu keeps
  // marking the page the sheet was opened from.
  sheetFrom = null;
  if (S.page !== 'home') { sheetFrom = { page: S.page, tab: S.page === 'settings' ? S.tab : null }; await go('home', { nav: S.page }); }
  S.sheetOpen = true; app.dataset.sheet = '1'; renderHints();
  $$('.sheet-state').forEach(s => { s.hidden = s.dataset.sheet !== name; });
  scrim.hidden = false; sheet.hidden = false;
  $('.topbar').inert = true; $('.main').inert = true;
  play(scrim, spec('scrim').in, 1);
  holdRing(true);
  const a = await play(sheet, spec('sheet').in, 1);
  if (a) a.cancel();
  holdRing(false);
  focusFirst(sheet);
}
async function closeSheet() {
  S.sheetOpen = false; app.dataset.sheet = '0'; renderHints();
  holdRing(true);
  play(scrim, spec('scrim').out, 1);
  await play(sheet, spec('sheet').out, 1);
  sheet.hidden = true; scrim.hidden = true;
  sheet.getAnimations().forEach(a => a.cancel()); scrim.getAnimations().forEach(a => a.cancel());
  $('.topbar').inert = false; $('.main').inert = false;
  holdRing(false);
  if (sheetFrom) { const f = sheetFrom; sheetFrom = null; await go(f.page, f.tab ? { tab: f.tab } : {}); }
  if (sheetReturn && sheetReturn.isConnected && sheetReturn.getClientRects().length) focusEl(sheetReturn); else focusFirst($(`[data-page="${S.page}"]`));
}
async function runUpdate() {
  if (LIVE) return liveUpdate();
  S.busy = true; holdRing(true);
  const info = $('[data-sheet="info"]'), prog = $('[data-sheet="progress"]');
  await play(info, spec('fade').out, 1); info.hidden = true;
  prog.hidden = false; play(prog, spec('fade').in, 1);
  focusFirst(prog);
  const parts = [['Downloading', 900], ['Checking files', 600], ['Replacing files', 500], ['Rebuilding', 1600]];
  let done = 0; const total = parts.reduce((a, p) => a + p[1], 0);
  $$('[data-ustage]').forEach(s => { s.className = 'stage'; });
  for (let i = 0; i < parts.length; i++) {
    const st = $(`[data-ustage="${i}"]`); st.className = 'stage active';
    bind('updStage', parts[i][0]);
    const t0 = performance.now();
    while (performance.now() - t0 < parts[i][1]) {
      await sleep(60);
      halo('work', (done + Math.min(parts[i][1], performance.now() - t0)) / total * 100);
    }
    done += parts[i][1]; st.className = 'stage done';
  }
  bind('updStage', 'Done');
  await sleep(400);
  S.busy = false; S.updated = true;
  bind('version', '0.3.1');
  bind('verNewBtn', 'Installed'); halo('idle');
  $('[data-bind="verNewBtn"]').disabled = true;
  $('[data-rollback]').hidden = false;
  $('.nav-dot').classList.add('gone');
  $('#update-card').hidden = true;
  await closeSheet();
  toast('Updated to 0.3.1', 'ok');
}
document.addEventListener('click', e => {
  if (e.target.closest('[data-open-update]')) openSheet();
  else if (e.target.closest('[data-sheet-close]')) closeSheet();
  else if (e.target.closest('[data-update-now]')) runUpdate();
  else if (e.target === scrim && !S.busy) closeSheet();
  else if (e.target.closest('[data-rollback]')) toast('Mockup: 0.3.0 would start next time; 0.3.1 is kept.');
  else if (e.target.closest('[data-check-updates]') && LIVE) { if (!U.active) { U.manual = true; send('live:upcheck'); } }
  else if (e.target.closest('[data-check-updates]')) {
    const b = e.target.closest('[data-check-updates]'); b.disabled = true; b.textContent = 'Checking'; halo('spin');
    setTimeout(() => { b.disabled = false; b.textContent = 'Check now'; halo('idle'); if (S.updated) toast('Up to date', 'ok'); else openSheet(); }, 1100);
  }
});

/* ------------------------------------------------------------------ launch */
const launch = $('.launch');
let launchReturn = null, launchRun = 0;
async function openLaunch() {
  if (L.moving) { toast('Moving the game folder', 'info'); return; }
  const run = ++launchRun;
  if (S.mode === 'setup') await setMode('main');
  if (S.page !== 'home') await go('home');
  if (run !== launchRun) return;
  launchReturn = $('#play-btn');
  S.launchOpen = true; app.dataset.sheet = '1'; app.dataset.launch = '1'; renderHints();
  launch.hidden = false;
  bind('launchTitle', 'Starting'); bind('launchSub', ''); halo('spin');
  app.classList.add('thrum'); setTimeout(() => app.classList.remove('thrum'), 720);
  splash(); playTone();
  if (hostBridge) hostBridge.postMessage('play');
  if (LIVE && hostBridge) {
    // "Unlock everything" applies to this one start only (GB_UNLOCK_ALL) and is never saved.
    const payload = Object.assign({}, S.set, L.unlockOnce ? { unlockAll: true } : {});
    L.unlockOnce = false;
    hostBridge.postMessage('live:play:' + JSON.stringify(payload));
  }
  holdRing(true);
  const a = await play(launch, spec('launch').in, 1); if (a) a.cancel();
  await sleep(1300); if (run !== launchRun) return;
  halo('idle'); bind('launchTitle', 'Playing'); bind('launchSub', '');
  holdRing(false);
  focusFirst(launch);
}
async function closeLaunch() {
  launchRun++;
  S.launchOpen = false; renderHints();
  holdRing(true);
  await play(launch, spec('launch').out, 1);
  launch.hidden = true; launch.getAnimations().forEach(a => a.cancel());
  holdRing(false);
  app.dataset.sheet = '0'; app.dataset.launch = '0'; halo('idle'); focusEl($('#play-btn'));
}
document.addEventListener('click', e => {
  if (e.target.closest('#play-btn') || e.target.closest('[data-play]')) openLaunch();
  else if (e.target.closest('[data-launch-close]')) closeLaunch();
});

/* ------------------------------------------------------------- mockup bar */
const mockPanel = $('.mock-panel'), mockToggle = $('.mock-toggle');
let mockReturn = null;
function toggleMock(force) {
  const open = force === undefined ? mockPanel.hidden : force;
  if (open === !mockPanel.hidden) return;
  S.mockOpen = open; mockToggle.setAttribute('aria-expanded', String(open));
  if (open) {
    mockReturn = document.activeElement;
    mockPanel.hidden = false; play(mockPanel, spec('alert').in, 1);
    requestAnimationFrame(() => { refreshIndicators(true, mockPanel); focusFirst(mockPanel); });
  } else {
    mockPanel.hidden = true;
    if (mockReturn && mockReturn.isConnected && mockReturn.getClientRects().length) focusEl(mockReturn); else focusFirst(layers()[layers().length - 1]);
  }
}
mockToggle.addEventListener('click', () => toggleMock());
document.addEventListener('click', e => {
  const m = e.target.closest('[data-mock]');
  if (!m) return;
  toggleMock(false);
  if (m.dataset.mock === 'home') {
    S.mode === 'main' ? go('home') : setMode('main');
  } else {
    buildDone = false; toolsDone = false; buildRun++; resetBuild(); S.built = false; save();
    const p1 = $('[data-step-panel="1"]');
    $('.pick', p1).hidden = false; $('.verify', p1).hidden = true; $('[data-next]', p1).disabled = true; showAlert(1, '');
    const p2 = $('[data-step-panel="2"]');
    $('[data-download]', p2).hidden = false; $('[data-download]', p2).disabled = false; $('[data-download]', p2).textContent = 'Download 262 MB'; $('[data-next]', p2).hidden = true; showAlert(2, '');
    $$('.tool', p2).forEach(t => { t.className = 'tool'; $('.bar-fill', t).style.setProperty('--p', '0%'); $('.tool-state', t).textContent = ''; });
    S.step = 0; setMode('setup', 0);
  }
});


/* ------------------------------------------------------------------- live */
/* The real launcher. The host (launcher\prototype\host.ps1 -Live) runs the
   engine and forwards its events (setup.ps1 -Events) as 'eng:<json>':
   check, step, tool, xbe, lift, log, error, done, and the host's own
   picked, folder, exit, copied. Each failure has a code; ERR below turns it
   into the designed alert with one next step. */
const send = msg => { if (hostBridge) hostBridge.postMessage(msg); };
const GB = n => (n / 1073741824).toFixed(1) + ' GB';
const MB = n => (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + ' MB';
const L = { checking: false, checksOk: false, verified: false, running: false, phase: '', stage: -1, stageT0: 0,
  done: [], t0: 0, tools: {}, toolBytes: 0, errorShown: false, picked: null };

/* expected seconds per build stage, measured on the development PC */
const LSTAGE = [3, 122, 3, 71, 20, 1];
const LTOTAL = LSTAGE.reduce((a, b) => a + b, 0);
const SUB2STAGE = { parse: 1, disasm: 1, func_id: 2, abi: 2, lift: 3 };
const TOOLROW = { python: 'python', capstone: 'capstone', 'llvm-mingw': 'clang', cmake: 'cmake', ninja: 'ninja' };

const ERR = {
  sac: ['Windows blocked the game', null, [['data-recheck', 'Check again', true]]],
  space: ['Not enough disk space', null, [['data-recheck', 'Check again', true], ['data-change-folder', 'Choose another folder']]],
  path: ['Choose another folder', 'The compiler cannot work in a folder whose path has letters outside plain English. A short path such as C:\\Gamebreaker works.', [['data-change-folder', 'Choose another folder', true]]],
  longpath: ['Choose another folder', 'This folder\'s path is too long for the build. A short path such as C:\\Gamebreaker works.', [['data-change-folder', 'Choose another folder', true]]],
  '32bit': ['This Windows is 32-bit', 'The game needs 64-bit Windows.', []],
  oldwin: ['This Windows is too old', 'The oldest supported version is Windows 10 version 1809.', []],
  running: ['The game is running', 'Close it, then check again.', [['data-recheck', 'Check again', true]]],
  busy: ['Setup is already running', 'Another setup is working in this folder. Wait for it to finish.', []],
  compressed: ['Compressed images are not supported', 'Convert it to a plain <span class="mono">.iso</span>, then choose it again.', [['data-repick', 'Choose another image', true]]],
  not_disc: ['Not an Xbox disc image', 'Choose an uncompressed <span class="mono">.iso</span>, or a folder with the extracted disc. Split images are not supported.', [['data-repick', 'Choose another image', true]]],
  no_xbe: ['No default.xbe', 'This image or folder has no <span class="mono">default.xbe</span>.', [['data-repick', 'Choose another image', true]]],
  no_disc: ['Disc image not found', 'Choose it again.', [['data-repick', 'Choose another image', true]]],
  not_xbe: ['Not an Xbox program', '<span class="mono">default.xbe</span> is not an Xbox executable.', [['data-repick', 'Choose another image', true]]],
  xbe_game: ['Wrong game', 'This image is a different game.', [['data-repick', 'Choose another image', true]]],
  xbe_release: ['Wrong release', null, [['data-repick', 'Choose another image', true]]],
  xbe_modified: ['Image does not match', null, [['data-repick', 'Choose another image', true]]],
  download: ['Download failed', null, [['data-retry', 'Try again', true]]],
  hash: ['Download did not match', null, [['data-retry', 'Try again', true]]],
  antivirus: ['A tool was removed', 'Antivirus software may have deleted it. Allow the install folder, then try again.', [['data-retry', 'Try again', true]]],
  unpack: ['Unpacking failed', null, [['data-retry', 'Try again', true], ['data-copylog', 'Copy log']]],
  patch: ['The toolkit patch does not apply', null, [['data-copylog', 'Copy log', true]]],
  lift: ['Translating to C failed', null, [['data-retry', 'Try again', true], ['data-copylog', 'Copy log']]],
  compile: ['The compile failed', null, [['data-retry', 'Try again', true], ['data-copylog', 'Copy log']]],
  no_game: ['The game is not built', 'Run setup from Settings, Advanced, Rebuild.', []]
};
const esc = t => String(t).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
function errBody(o) {
  const t = String(o.text || '');
  if (o.code === 'space' && o.need) return `${GB(o.need)} needed, ${GB(o.free)} free on ${esc(o.drive)}.`;
  if (o.code === 'sac') return esc(t.split('\n')[1] || t);     // the title already says what happened
  if (o.code === 'xbe_release') { const m = /region: ([^)]+)\)/.exec(t); return `This is the ${esc(m ? m[1] : 'other')} release. Only the USA release is supported.`; }
  if (o.code === 'xbe_modified') {
    const m = /Found +SHA-256 ([0-9A-F]{64})/.exec(t);
    return `<span class="mono">default.xbe</span> differs from the supported release.<br>Expected <span class="mono">${HASH_OK.slice(0, 16)}\u2026</span>, found <span class="mono">${m ? m[1].slice(0, 16) + '\u2026' : 'another file'}</span>`;
  }
  return esc(t.split('\n')[0]);
}
function liveAlert(step, o) {
  const e = ERR[o.code] || [o.step === 'start' ? 'Setup could not start' : 'Setup stopped', null, [['data-retry', 'Try again', true], ['data-copylog', 'Copy log']]];
  showAlert(step, alertHtml('err', e[0], e[1] || errBody(o), e[2]));
  const first = $(`[data-alert="${step}"] button`);
  if (first) focusEl(first);
}

/* Step 0: this PC */
function liveChecks() {
  L.checking = true; L.checksOk = false; L.errorShown = false;
  const rows = $$('.check');
  rows.forEach(r => { r.className = 'check pending'; });
  bind('sacLabel', 'Smart App Control');
  showAlert(0, '');
  $('[data-step-panel="0"] [data-next]').disabled = true;
  halo('spin');
  send('live:check');
}
function onCheck(o) {
  const r = $(`.check[data-check="${o.key}"]`);
  if (!r) return;
  r.className = 'check ' + (o.ok ? 'ok' : 'bad');
  const label = $('.check-label', r);
  if (o.key === 'sac') {
    bind('sacLabel', { off: 'Smart App Control is off', on: 'Smart App Control is on', evaluation: 'Smart App Control is in evaluation mode', none: 'Smart App Control is not on this PC' }[o.state] || 'Smart App Control');
    // Said before the download, so a stop after it is not a surprise (test first, 2026-11-16).
    const tested = o.state === 'on' || o.state === 'evaluation';
    bind('sacNote', tested ? 'After the download, setup checks that Windows lets the game run on this PC.' : '');
    $('[data-bind="sacNote"]').hidden = !tested;
  }
  else if (o.text) label.textContent = o.text;
}

/* Step 1: the disc, checked before anything is downloaded */
function livePick(kind) {
  verifying++; halo('idle');
  const panel = $('[data-step-panel="1"]');
  $('[data-next]', panel).disabled = true;
  showAlert(1, ''); L.verified = false; L.errorShown = false;
  send('live:pick:' + kind);
}
async function onPicked(o) {
  const panel = $('[data-step-panel="1"]');
  if (!o.path) { focusFirst($('.pick', panel)); return; }
  L.picked = o;
  bind('fileName', o.path);
  const pickEl = $('.pick', panel), ver = $('.verify', panel);
  if (!pickEl.hidden) { await play(pickEl, spec('fade').out, 1); pickEl.hidden = true; }
  ver.hidden = false; play(ver, spec('alert').in, 1);
  $('.verify-line', panel).className = 'verify-line';
  bind('hash', ''); bind('verifyStatus', 'Reading default.xbe');
  halo('spin');
  focusEl($('[data-repick]'));
}
function onXbe(o) {
  const panel = $('[data-step-panel="1"]');
  $('.verify-line', panel).className = 'verify-line ok';
  bind('hash', o.sha); bind('verifyStatus', 'Supported release');
  halo('idle'); L.verified = true;
  const next = $('[data-next]', panel); next.disabled = false; focusEl(next);
}

/* Steps 2 and 3: one engine run. Tools first (step 2), then the build (step 3). */
function renderTools() {
  const tl = (LIVE.tools || []);
  let missing = 0;
  for (const t of tl) {
    const row = $(`.tool[data-tool="${TOOLROW[t.name] || t.name}"]`);
    if (!row) continue;
    $('.tool-size', row).textContent = MB(t.size);
    L.tools[t.name] = { size: t.size, got: t.present ? t.size : 0, present: t.present };
    if (t.present) { row.className = 'tool ok'; $('.bar-fill', row).style.setProperty('--p', '100%'); $('.tool-state', row).textContent = 'Present'; }
    else missing += t.size;
  }
  const dl = $('[data-download]');
  dl.textContent = missing ? 'Download ' + MB(missing) : 'Continue';
}
function liveSetup() {
  if (L.running) return;
  L.running = true; L.errorShown = false; L.done = []; L.stage = -1; L.t0 = performance.now();
  S.busy = true;
  showAlert(2, ''); showAlert(3, '');
  $('[data-retry]').hidden = true;
  if (S.step === 2) $('[data-download]').hidden = false;
  $$('.stage[data-stage]').forEach(st => { st.className = 'stage'; $('.stage-time', st).textContent = ''; });
  bind('buildPct', '0'); bind('buildStage', 'Building'); bind('buildEta', ''); bind('compileDetail', 'Compile');
  const dl = $('[data-download]'); dl.disabled = true;
  if (S.step === 2) dl.textContent = 'Downloading';
  halo('work', 0);
  send('live:setup');
  clearInterval(L.timer); L.timer = setInterval(liveTick, 250);
}
function setStage(i, state) {
  const st = $(`.stage[data-stage="${i}"]`);
  if (!st) return;
  if (state === 'active') {
    for (let k = 0; k < i; k++) if (!L.done.includes(k)) setStage(k, 'done');
    if (L.stage !== i) { L.stage = i; L.stageT0 = performance.now(); }
    st.className = 'stage active';
    bind('buildStage', ['Unpacking', 'Reading the code', 'Identifying', 'Translating', 'Compiling', 'Linking'][i]);
  } else if (state === 'done' && !L.done.includes(i)) {
    L.done.push(i); st.className = 'stage done';
    const secs = (performance.now() - (L.stage === i ? L.stageT0 : performance.now())) / 1000;
    $('.stage-time', st).textContent = secs >= 60 ? fmt(secs) : (secs >= 1 ? Math.round(secs) + ' s' : '');
  }
}
function liveTick() {
  if (!L.running || L.phase !== 'build') return;
  let sim = 0;
  for (let k = 0; k < LSTAGE.length; k++) {
    if (L.done.includes(k)) sim += LSTAGE[k];
    else if (k === L.stage) sim += Math.min(LSTAGE[k] * .95, (performance.now() - L.stageT0) / 1000);
  }
  const pct = Math.min(99, Math.floor(sim / LTOTAL * 100));
  bind('buildPct', String(pct)); halo('work', sim / LTOTAL * 100);
  const left = Math.max(0, LTOTAL - sim);
  bind('buildEta', left > 90 ? `about ${Math.ceil(left / 60)} minutes left` : left > 5 ? 'about a minute left' : '');
}
function onTool(o) {
  const t = L.tools[o.name] || (L.tools[o.name] = { size: o.size || 0, got: 0 });
  const row = $(`.tool[data-tool="${TOOLROW[o.name] || o.name}"]`);
  if (!row) return;
  const bar = $('.bar-fill', row), state = $('.tool-state', row);
  if (o.state === 'present') { t.got = t.size; row.className = 'tool ok'; bar.style.setProperty('--p', '100%'); state.textContent = 'Present'; }
  else if (o.state === 'download' || o.state === 'progress') {
    t.got = o.got || 0; row.className = 'tool';
    const p = t.size ? Math.min(100, t.got / t.size * 100) : 0;
    bar.style.setProperty('--p', p.toFixed(1) + '%'); state.textContent = Math.round(p) + '%';
  } else if (o.state === 'unpack') { t.got = t.size; bar.style.setProperty('--p', '100%'); state.textContent = 'Unpacking'; }
  else if (o.state === 'done') { row.className = 'tool ok'; state.textContent = 'Verified'; }
  const all = Object.values(L.tools), size = all.reduce((a, v) => a + (v.size || 0), 0), got = all.reduce((a, v) => a + (v.got || 0), 0);
  if (size && S.step === 2) halo('work', got / size * 100);
}
function onStep(o) {
  if (o.name === 'tools' && o.state === 'done') {
    const dl = $('[data-download]'); dl.hidden = true;
    L.phase = 'build';
    if (S.mode === 'setup' && S.step === 2) setTimeout(() => { if (S.step === 2) showStep(3); }, 600);
  }
  if (o.name === 'toolkit' && o.state === 'start') { L.phase = 'build'; setStage(0, 'active'); }
  if (o.name === 'disc') { if (o.state === 'start') setStage(0, 'active'); else setStage(0, 'done'); }
  if (o.name === 'lift' && o.state === 'done') { [1, 2, 3].forEach(k => setStage(k, 'done')); }
  if (o.name === 'compile') {
    if (o.state === 'start') { [0, 1, 2, 3].forEach(k => setStage(k, 'done')); setStage(4, 'active'); }
    else { setStage(4, 'done'); setStage(5, 'active'); setStage(5, 'done'); }
  }
}
function onLift(o) {
  const st = SUB2STAGE[o.sub];
  if (st === undefined) return;
  if (o.state === 'start') setStage(st, 'active');
  else if (o.sub === 'disasm' || o.sub === 'abi' || o.sub === 'lift') setStage(st, 'done');
}
function onDone(o) {
  if (o.mode === 'check') { L.checking = false; L.checksOk = !L.errorShown; if (L.checksOk) { halo('idle'); const nx = $('[data-step-panel="0"] [data-next]'); nx.disabled = false; if (S.mode === 'setup' && S.step === 0) focusEl(nx); if (LIVE.auto) autoStep(); } return; }
  if (o.mode === 'verify') { if (LIVE.auto) autoStep(); return; }
  clearInterval(L.timer);
  L.running = false; S.busy = false; L.phase = '';
  for (let k = 0; k < LSTAGE.length; k++) setStage(k, 'done');
  bind('buildPct', '100'); bind('buildStage', 'Built'); bind('buildEta', ''); halo('ok', 100);
  const secs = Math.round(o.secs || 0);
  bind('doneLine', o.changed ? `Built in ${Math.floor(secs / 60)} min ${secs % 60} s` : 'Already built');
  LIVE.installed = true; S.built = true;
  log(`done in ${secs} s`, 'l-ok');
  setTimeout(() => { if (S.mode === 'setup' && (S.step === 3 || S.step === 2)) showStep(4); }, 700);
}
function onError(o) {
  L.errorShown = true;
  if (o.step === 'checks' || o.code === 'sac' || o.code === 'space') {
    if (L.running) {                 // Smart App Control's test program runs during setup, after the download
      clearInterval(L.timer); L.running = false; S.busy = false; L.phase = '';
      const dl = $('[data-download]'); dl.hidden = false; dl.disabled = false; renderTools();
    }
    halo('error'); liveAlert(0, o); $('[data-step-panel="0"] [data-next]').disabled = true;
    const r = o.code === 'sac' ? $('.check[data-check="sac"]') : o.code === 'space' ? $('.check[data-check="space"]') : null;
    if (r) r.className = 'check bad';
    if (S.mode === 'setup' && S.step !== 0) { L.holdChecks = true; showStep(0); }
    return;
  }
  if (o.step === 'disc' && !L.running) {        // the check before any download
    const panel = $('[data-step-panel="1"]');
    $('.verify-line', panel).className = 'verify-line bad'; halo('error');
    bind('verifyStatus', /^xbe_/.test(o.code) ? 'Not the supported release' : 'This image cannot be read');
    const m = /Found +SHA-256 ([0-9A-F]{64})/.exec(o.text || ''); if (m) bind('hash', m[1]);
    liveAlert(1, o); return;
  }
  if (o.code === 'no_game') { closeLaunch(); toast(ERR.no_game[0], 'info'); return; }
  if (o.code === 'running' && o.step === 'play') { closeLaunch(); toast('The game is already running', 'info'); return; }
  if (o.code === 'start' && o.step === 'play') {        // the program would not start; nothing to rebuild on its own
    closeLaunch(); toast(`The game did not start: ${o.text || 'unknown reason'}. Check files in Settings, Advanced finds what is wrong.`, 'info'); return;
  }
  clearInterval(L.timer);
  L.running = false; S.busy = false;
  if (L.stage >= 0) { const st = $(`.stage[data-stage="${L.stage}"]`); if (st) st.className = 'stage bad'; }
  halo('error');
  bind('buildStage', 'Stopped');
  log(String(o.text || '').split('\n')[0], 'l-err');
  const atTools = o.step === 'tools' || (S.mode === 'setup' && S.step === 2);
  if (atTools) { const dl = $('[data-download]'); dl.hidden = true; dl.disabled = false; renderTools(); }   // the alert's Try again does the same
  if (S.mode !== 'setup') setMode('setup', atTools ? 2 : 3);
  liveAlert(atTools ? 2 : 3, o);
}
function onExit(o) {
  if (o.kind === 'setup' && L.running && o.code !== 0 && !L.errorShown) onError({ step: 'setup', code: '', text: `Setup ended with exit code ${o.code}.` });
  if (o.kind === 'check' && o.code !== 0 && !L.errorShown) onError({ step: 'checks', code: '', text: `The checks ended with exit code ${o.code}.` });
  if (o.kind === 'verify' && o.code !== 0 && !L.errorShown) onError({ step: 'disc', code: '', text: `Reading the disc ended with exit code ${o.code}.` });
  if (o.kind === 'modinstall' && MD.installing) installFailed('Install failed.');   // the install ended without its own answer
}
function onEngine(o) {
  if (U.active && ['step', 'tool', 'lift', 'log', 'error', 'done', 'xbe', 'check'].includes(o.ev)) { onUpdateEngine(o); return; }
  if (o.ev === 'error' && (o.step === 'checkfiles' || o.step === 'move')) {
    const cf = $('[data-check-files]'); cf.disabled = false; cf.textContent = 'Check files';
    const mb = $('[data-move-folder]'); mb.disabled = false; mb.textContent = 'Move'; L.moving = false;
    toast(o.text, 'info'); return;
  }
  if (o.ev === 'error' && o.step === 'update') {          // a check that failed
    const chk = $('[data-check-updates]'); chk.disabled = false; chk.textContent = 'Check now';
    bind('updStatus', o.code === 'offline' ? 'Update feed not reachable' : o.code === 'signature' ? 'Update refused: the list is not signed by this project' : 'Update check failed');
    if (U.manual || o.code === 'signature') toast(o.code === 'offline' ? 'Update feed not reachable' : o.text, 'info');
    U.manual = false; return;
  }
  switch (o.ev) {
    case 'check': onCheck(o); break;
    case 'step': onStep(o); break;
    case 'tool': onTool(o); break;
    case 'xbe': onXbe(o); break;
    case 'lift': onLift(o); break;
    case 'log': log(o.text); break;
    case 'error': onError(o); break;
    case 'done': onDone(o); break;
    case 'exit': onExit(o); break;
    case 'picked': onPicked(o); break;
    case 'folder':
      bind('installFolder', o.path); LIVE.folder = o.path; LIVE.installed = !!o.installed;
      if (o.tools) { LIVE.tools = o.tools; renderTools(); }
      if (o.changed && S.mode === 'setup') liveChecks();
      break;
    case 'keys': onKeys(o); break;
    case 'copied': toast(o.ok ? 'Log copied' : 'No setup log yet', o.ok ? 'ok' : 'info'); break;
    case 'backups': onBackups(o); break;
    case 'restored': if (S.sheetOpen) closeSheet(); toast(o.ok ? 'Save restored' : o.text, o.ok ? 'ok' : 'info'); break;
    case 'saves': if (!o.ok) toast(o.text, 'info'); break;
    case 'update': onUpdate(o); break;
    case 'checkfiles': onCheckFiles(o); break;
    case 'movepick':
      if (!o.to) break;
      L.moveTo = o.to; bind('moveTo', o.to); showSheet('move'); break;
    case 'move': onMove(o); break;
    case 'mods': case 'modinspect': case 'modprogress': case 'modfail': case 'modremoved': case 'modorder': case 'modnote': onModsEvent(o); break;
  }
}

/* ------------------------------------------------------ check files, move */
function onCheckFiles(o) {
  if (o.state !== 'done') return;
  const b = $('[data-check-files]'); b.disabled = false; b.textContent = 'Check files';
  if (!o.problems) { toast('Every file matches', 'ok'); return; }
  bind('checkTitle', o.problems === 1 ? '1 problem found' : `${o.problems} problems found`);
  $('[data-check-list]').innerHTML = (o.list || []).map(t => `<li>${esc(t)}</li>`).join('');
  bind('checkFix', o.fix === 'release' ? 'Unpack the release zip again, then check again.'
    : o.fix === 'disc' ? 'Set the game up again from your disc image.'
    : 'Rebuild downloads and builds what is missing.');
  $('[data-check-rebuild]').hidden = o.fix !== 'rebuild';
  showSheet('checked');
}
function onMove(o) {
  const mb = $('[data-move-folder]');
  if (o.state === 'copying' && o.bytes) mb.textContent = o.got ? `Moving ${Math.round(o.got / o.bytes * 100)} %` : 'Moving';
  if (o.state === 'done') { L.moving = false; mb.disabled = false; mb.textContent = 'Move'; toast('Moved', 'ok'); }
}

/* ----------------------------------------------------------------- updates */
/* The host runs setup\update.ps1 (check, install, rollback, finish) and, after
   restarting on the new files, setup.ps1 to rebuild what changed. The feed is
   a setting in the release (setup\release.txt); without one the page says
   "Not connected". */
const U = { latest: null, active: false, building: false, manual: false, ring: 0 };
function sizeText(b) { return b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(b / 1024))} KB`; }
function initUpdates() {
  const upd = LIVE.update || {};
  const chk = $('[data-check-updates]');
  if (!upd.feed) { $('.versions').hidden = true; chk.disabled = true; chk.textContent = 'Not connected'; return; }
  renderVersions();
  const j = upd.journal;
  if (j && j.state === 'applied') setTimeout(() => resumeUpdate(j), 900);
  else if (j && (j.state === 'applying' || j.state === 'building')) { U.active = true; setTimeout(() => { send('live:uprollback'); }, 900); }
  else if (j && j.state === 'rolledback' && !j.shown) setTimeout(() => showUpdateFailed(j.reason, j.from), 1400);
  else if (j && j.state === 'done' && !j.shown) setTimeout(() => toast(`Updated to ${j.to}`, 'ok'), 1400);
  // The daily check waits until the game is installed (the first run has its own steps).
  else if (S.set.updates && upd.due && LIVE.installed) setTimeout(() => send('live:upcheck'), 2500);
}
function renderVersions() {
  const v = $('.versions'); v.hidden = false;
  const notes = n => (n || []).filter(Boolean).map(t => `<li>${esc(t)}</li>`).join('');
  let html = '';
  if (U.latest) html += `<li class="ver ver-new"><span class="ver-num">${esc(U.latest.latest)}</span><ul class="ver-notes">${notes(U.latest.notes)}</ul><button class="btn btn-small" data-open-update>Update</button></li>`;
  html += `<li class="ver"><span class="ver-num">${esc(LIVE.version || 'dev')}</span><ul class="ver-notes">${notes((LIVE.update || {}).notes)}</ul></li>`;
  v.innerHTML = html;
}
function fillUpdateSheet() {
  const o = U.latest;
  bind('updTitle', `Update to ${o.latest}`);
  $('[data-upd-notes]').innerHTML = (o.notes || []).filter(Boolean).map(t => `<li>${esc(t)}</li>`).join('');
  const build = o.rebuild === 'full' ? ', then a rebuild of about 4 minutes' : o.rebuild === 'quick' ? ', then a rebuild of under a minute' : '';
  bind('updCost', `${sizeText(o.bytes || 0)}${build}`);
}
function setUStage(i, label) {
  $$('[data-ustage]').forEach(s => { const n = +s.dataset.ustage; s.className = n < i ? 'stage done' : n === i ? 'stage active' : 'stage'; });
  if (label) bind('updStage', label);
}
async function openProgress() {
  if (!S.sheetOpen) await showSheet('progress'); else sheetState('progress');
}
async function liveUpdate() {
  U.active = true; S.busy = true;
  await openProgress();
  setUStage(0, 'Downloading'); halo('work', 0);
  send('live:upinstall');
}
async function resumeUpdate(j) {
  U.active = true; U.building = true; S.busy = true;
  await openProgress();
  setUStage(3, 'Rebuilding'); halo('spin');
  send('live:upresume');
}
async function showUpdateFailed(reason, from) {
  U.active = false; U.building = false; S.busy = false; halo('idle');
  bind('upFailText', reason || 'The update could not finish.');
  bind('upFailBack', from ? `Version ${from} is back in place.` : '');
  if (S.sheetOpen) sheetState('upfail'); else await showSheet('upfail');
}
function onUpdate(o) {
  const chk = $('[data-check-updates]');
  switch (o.state) {
    case 'checking': chk.disabled = true; chk.textContent = 'Checking'; break;
    case 'current':
      chk.disabled = false; chk.textContent = 'Check now'; U.latest = null; renderVersions();
      bind('updStatus', 'Up to date');
      if (U.manual) toast('Up to date', 'ok');
      U.manual = false; break;
    case 'available':
      chk.disabled = false; chk.textContent = 'Check now';
      U.latest = o; renderVersions(); bind('updStatus', '');
      $('.nav-dot').classList.remove('gone');
      const card = $('#update-card'); card.textContent = `Update to ${o.latest}`; card.hidden = false;
      if (U.manual) openSheet();
      U.manual = false; break;
    case 'off': chk.disabled = true; chk.textContent = 'Not connected'; break;
    case 'downloading': setUStage(0, 'Downloading'); if (o.size) halo('work', o.got / o.size * 60); break;
    case 'verifying': setUStage(1, 'Checking files'); halo('work', 65); break;
    case 'replacing': setUStage(2, 'Replacing files'); halo('work', 75); break;
    case 'applied': setUStage(3, 'Restarting'); halo('work', 80); break;
    case 'rolledback': if (!o.launcher) showUpdateFailed(o.reason, o.version); else bind('updStage', 'Restarting'); break;
    case 'done':
      U.active = false; U.building = false; S.busy = false; halo('idle');
      bind('version', o.version); LIVE.version = o.version; U.latest = null; renderVersions();
      $('#update-card').hidden = true; $('.nav-dot').classList.add('gone');
      setUStage(4, 'Done');
      (S.sheetOpen ? closeSheet() : Promise.resolve()).then(() => toast(`Updated to ${o.version}`, 'ok'));
      break;
  }
}
/* While an update runs, setup's and update's own events belong to it, not to the first-run screens. */
function onUpdateEngine(o) {
  if (o.ev === 'step' && o.state === 'start') bind('updStage', 'Rebuilding');
  else if (o.ev === 'lift' && o.state === 'start') bind('updStage', 'Rebuilding: lifting');
  else if (o.ev === 'step' && o.name === 'compile' && o.state === 'start') bind('updStage', 'Rebuilding: compiling');
  else if (o.ev === 'log') log(o.text);
  else if (o.ev === 'error') {
    U.lastError = o.text;
    // Nothing was replaced yet (a bad download, the game running, offline): say so at once.
    if (o.step === 'update' && !U.building && o.code !== 'busy') showUpdateFailed(`${o.text}`, '');
  }
}

/* Tests only (host -Auto): walk the first run, and Play if asked. */
let autoBusy = false;
function autoStep() {
  if (!LIVE || !LIVE.auto || autoBusy) return;
  autoBusy = true;
  setTimeout(() => {
    autoBusy = false;
    if (S.mode === 'setup' && S.step === 0 && L.checksOk) showStep(1);
    else if (S.mode === 'setup' && S.step === 1 && !L.verified && !L.picked) livePick('file');
    else if (S.mode === 'setup' && S.step === 1 && L.verified) showStep(2);
    else if (S.mode === 'setup' && S.step === 2 && !L.running && /setup/.test(LIVE.auto)) liveSetup();
    else if (S.mode === 'setup' && S.step === 4) { if (/play/.test(LIVE.auto)) openLaunch(); else setMode('main'); }
    else if (S.mode === 'main' && S.page === 'home' && /play/.test(LIVE.auto) && !S.launchOpen) openLaunch();
  }, 1200);
}

function initLive() {
  app.dataset.live = '1'; if (LIVE.dev) app.dataset.dev = '1';
  bind('installFolder', LIVE.folder || '');
  bind('version', LIVE.version || 'dev');
  bind('setupLead', 'Builds the game from your disc image. About 5 minutes after the download.');
  $('#update-card').hidden = true;
  $('.nav-dot').classList.add('gone');
  initUpdates();
  // Displays as the game counts them (Windows' order), the primary first as "Primary display".
  const disp = ['Primary display'].concat((LIVE.displays || []).map((d, i) => `Display ${i + 1}, ${d.w} \u00d7 ${d.h}${d.primary ? ', primary' : ''}`));
  const mc = $('.cycler[data-setting="monitor"]'); mc.dataset.options = JSON.stringify(disp);
  if (S.set.monitor >= disp.length) { S.set.monitor = 0; S.chosen.delete('monitor'); }
  renderCycler(mc);
  refreshDefaults(); summaries();
  // Settings that cannot act in this release: shown, switched off.
  const off = [];
  if (!(LIVE.update && LIVE.update.feed)) {
    S.set.updates = false;                                // no feed in this release: shown off, not pretended on
    const ut = $('.toggle[data-setting="updates"]'); if (ut) ut.setAttribute('aria-checked', 'false');
    off.push('updates');
  }
  for (const k of off) {
    const el = $(`[data-setting="${k}"]:not(.mock-panel *)`); if (!el) continue;
    const row = el.closest('.row'); if (row) row.classList.add('row-off');
    if (el.matches('button')) el.disabled = true;
    else { el.setAttribute('aria-disabled', 'true'); $$('button', el).forEach(b => { b.disabled = true; }); }
  }
  setTimeout(() => send('live:keysget'), 1500);
  for (const sel of ['[data-toast^="Mockup: a folder"]:not([data-change-folder]):not([data-move-folder])']) {
    $$(sel).forEach(b => { b.disabled = true; });
  }
  renderTools();
  if (LIVE.installed && /play/.test(LIVE.auto || '') && !LIVE.lastGame) setTimeout(autoStep, 2500);
  // After the game: its own fatal error (found in its log by the host; the exit
  // code says nothing), or an early exit with an error code.
  if (LIVE.lastGame && LIVE.lastGame.fatal)
    setTimeout(() => { bind('crashText', LIVE.lastGame.fatal); showSheet('crash'); }, 1600);
  else if (LIVE.lastGame && LIVE.lastGame.code && LIVE.lastGame.secs < 15)
    setTimeout(() => toast(`The game closed after ${LIVE.lastGame.secs} s (exit code ${LIVE.lastGame.code})`, 'info'), 1600);
}
/* Saves: open the folder, restore a dated backup (saves\backups\yyyy-mm-dd_hhmmss). */
function backupLabel(name) {
  const m = /^(\d{4})-(\d\d)-(\d\d)_(\d\d)(\d\d)(\d\d)(_before-unlock|_mods-changed)?$/.exec(name);
  if (!m) return name;
  const d = new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
  const when = d.toLocaleString(undefined, { day: 'numeric', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
  if (m[7] === '_before-unlock') return `${when}, before unlock`;
  if (m[7] === '_mods-changed') return `${when}, before mods changed`;
  return when;
}
function onBackups(o) {
  const list = (o.list || []);
  if (!list.length) { toast(o.text || 'No backups yet', 'info'); return; }
  const host = $('[data-backups]');
  host.innerHTML = list.map(n => `<button class="btn btn-small" data-backup="${esc(n)}">${esc(backupLabel(n))}</button>`).join('');
  showSheet('restore');
}
document.addEventListener('click', e => {
  if (!LIVE) { if (e.target.closest('[data-change-folder]')) toast('Mockup: a folder picker would open here.'); return; }
  if (e.target.closest('[data-change-folder]')) { if (!S.busy) send('live:folder'); return; }
  if (e.target.closest('[data-copylog]')) { send('live:copylog'); return; }
  if (e.target.closest('[data-open-saves]')) { send('live:opensaves'); return; }
  if (e.target.closest('[data-open-shots]')) { send('live:openshots'); return; }
  if (e.target.closest('[data-open-dump]')) { send('live:opendump'); return; }
  if (e.target.closest('[data-restore]')) { send('live:backups'); return; }
  const bk = e.target.closest('[data-backup]');
  if (bk) { L.restoreName = bk.dataset.backup; bind('restoreTitle', 'Restore the save from ' + backupLabel(L.restoreName)); sheetState('confirm'); return; }
  if (e.target.closest('[data-restore-go]')) { if (L.restoreName) send('live:restore:' + L.restoreName); return; }
  if (e.target.closest('[data-unlock-all]')) { showSheet('unlock'); return; }
  // Settings, Advanced: Check files (setup\check_files.ps1) and Move (setup\move_install.ps1).
  const cf = e.target.closest('[data-check-files]');
  if (cf) { if (!S.busy && !L.moving) { cf.disabled = true; cf.textContent = 'Checking'; send('live:checkfiles'); } return; }
  if (e.target.closest('[data-check-rebuild]')) { closeSheet().then(() => { const b = $('[data-start-setup="3"]'); if (b) b.click(); }); return; }
  if (e.target.closest('[data-move-folder]')) { if (!S.busy && !L.moving) send('live:movepick'); return; }
  if (e.target.closest('[data-move-go]')) {
    if (L.moveTo) { L.moving = true; const mb = $('[data-move-folder]'); mb.disabled = true; mb.textContent = 'Moving'; send('live:movego:' + L.moveTo); }
    closeSheet(); return;
  }
  if (e.target.closest('[data-unlock-go]')) { L.unlockOnce = true; closeSheet().then(() => openLaunch()); return; }
});

/*@MODS@*/

/* ------------------------------------------------------------------- init */
function setAway() { if (document.hasFocus()) delete root.dataset.away; else root.dataset.away = '1'; }
addEventListener('blur', setAway); addEventListener('focus', setAway);

function init() {
  setAway();
  // A remembered value that is no longer offered (a shape removed, say) falls back to the default.
  $$('.seg[data-setting]:not(.mock-panel *)').forEach(seg => {
    const k = seg.dataset.setting, vals = $$('button', seg).map(b => b.dataset.value);
    if (PERSIST.includes(k) && !vals.includes(String(S.set[k]))) { S.chosen.delete(k); S.set[k] = defaultOf(k); if (!vals.includes(String(S.set[k]))) S.set[k] = vals[0]; }
  });
  applyPrefs();
  $$('.seg').forEach(renderSeg);
  $$('.cycler').forEach(c => renderCycler(c));
  $$('.slider').forEach(renderSlider);
  $$('.toggle[data-setting]').forEach(t => t.setAttribute('aria-checked', String(!!S.set[t.dataset.setting])));
  summaries(); updatePadUI(); drawPreview(false); makeMotes(16);
  // The address can name where to open, for testing: #home, #controls,light ...
  const tokens = location.hash.replace('#', '').split(',').filter(Boolean);
  if (tokens.includes('light') || tokens.includes('dark')) { S.set.theme = tokens.includes('light') ? 'light' : 'dark'; applyPrefs(); $$('.seg[data-setting="theme"]').forEach(renderSeg); }
  const startTab = tokens.find(t => TABS.includes(t));
  if (tokens.includes('padtest')) setTimeout(() => setPadTest(true), 1200);
  if (startTab) { $$('.tab').forEach(t => t.setAttribute('aria-selected', String(t.dataset.tab === startTab))); $$('.tab-panel').forEach(pn => { pn.hidden = pn.dataset.panel !== startTab; }); S.tab = startTab; }
  if (LIVE) initLive();
  modsInit(tokens);
  const sc = tokens.find(t => t.startsWith('sc:'));
  if (sc && SCEN.includes(sc.slice(3))) S.set.scenario = SCEN.indexOf(sc.slice(3));
  const stepTok = tokens.find(t => t.startsWith('step:'));
  const built = LIVE ? !!LIVE.installed : S.built;
  const start = startTab ? 'settings' : (tokens.find(t => t === 'home' || PAGES.includes(t)) || (built && !stepTok && !tokens.includes('setup') ? 'home' : ''));
  if (start === 'home' || PAGES.includes(start)) { S.mode = 'main'; app.dataset.mode = 'main'; S.page = PAGES.includes(start) ? start : 'home'; $$('.page').forEach(p => { p.hidden = p.dataset.page !== S.page; }); $$('.nav-item').forEach(n => n.toggleAttribute('aria-current', n.dataset.goto === S.page)); $(`.nav-item[data-goto="${S.page}"]`).setAttribute('aria-current', 'page'); }
  else if (stepTok) { const n = clamp(+stepTok.slice(5) || 0, 0, 4); steps.forEach((s, i) => { s.hidden = i !== n; }); setTimeout(() => showStep(n, true), 0); }
  else { steps.forEach((s, i) => { s.hidden = i !== 0; }); runChecks(); }
  root.dataset.input = S.input; app.dataset.view = S.page; $('.subnav').inert = S.page !== 'settings';
  renderHints();
  requestAnimationFrame(() => {
    refreshIndicators(true);
    focusFirst(S.mode === 'main' ? $(`[data-page="${S.page}"]`) : $('[data-step-panel="0"]'));
    holdRing(true);
    app.classList.add('boot');
    setTimeout(() => { app.classList.remove('boot'); holdRing(false); refreshIndicators(true); placeRing(document.activeElement, true); }, 1400);
  });
  if (pads().length && !padRaf) padRaf = requestAnimationFrame(pollPads);
  if (hostBridge) hostBridge.postMessage(`ready ${innerWidth}x${innerHeight} dpr ${devicePixelRatio}`);
  document.fonts && document.fonts.ready.then(() => refreshIndicators(true));
}
init();
})();
