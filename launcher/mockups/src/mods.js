/* ===================================================================== MODS
   The mods on this PC, as plates on a beam of light. Load order runs top to
   bottom and the first mod on the beam wins a clash (the game's own rule,
   port/src/gb_mods.c). The page writes only through the host: the order and
   on/off as mods\load_order.txt, installs and removals, and the "mods"
   setting, which becomes GB_MODS=0 when off. This file is joined into
   core.js by build.py and shares its scope. */
const MD = { quiet: false, list: [], sel: null, lifted: null, liftFrom: -1, incoming: null, installing: false, loaded: false, drag: null, gameFiles: true };
const MOD_C = 2 * Math.PI * 46;           // the lens ring's circumference (r = 46 in a 100 box)
const plural = (n, one, many) => `${n} ${n === 1 ? one : (many || one + 's')}`;

/* A mod with no icon.png gets a sigil from its name: two leaning bars (the
   mark's break) on a colour of its own, with its initials. */
function hashStr(s) { let h = 2166136261; for (const c of String(s)) { h ^= c.charCodeAt(0); h = Math.imul(h, 16777619); } return h >>> 0; }
function modInitials(name) {
  const w = String(name).replace(/[_\-.]+/g, ' ').trim().split(/\s+/).filter(Boolean);
  return ((w[0] || '?')[0] + (w.length > 1 ? w[1][0] : (w[0] || '')[1] || '')).toUpperCase();
}
function sigil(name) {
  const h = hashStr(name), a = h % 360, b = (a + 35 + (h >>> 9) % 70) % 360;
  const svg = `<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'><defs><linearGradient id='g' x1='0' y1='0' x2='1' y2='1'><stop offset='0' stop-color='hsl(${a},62%,42%)'/><stop offset='1' stop-color='hsl(${b},58%,22%)'/></linearGradient></defs>` +
    `<rect width='64' height='64' fill='url(#g)'/><path d='M4 20a6 6 0 0 1 6-6h14L4 40z' fill='rgba(255,255,255,.16)'/><path d='M60 44a6 6 0 0 1-6 6H40l20-26z' fill='rgba(255,255,255,.16)'/>` +
    `<text x='32' y='40' text-anchor='middle' font-family='Segoe UI,sans-serif' font-weight='700' font-size='20' fill='#fff' fill-opacity='.92'>${esc(modInitials(name))}</text></svg>`;
  return 'data:image/svg+xml;utf8,' + encodeURIComponent(svg);
}
const modIcon = m => m.icon || sigil(m.name || m.folder);

/* Which files each mod really gives the game: walking the beam from the top,
   the first mod with a file keeps it. Mods that are off take part in nothing.
   Two files clash when they reach the same thing in the game: the host gives
   each file a key (the disc path; an archive entry by its number, whether the
   mod named it by name or number; a line of text by its entry and line). */
const fileKey = (m, i) => (m.keys && m.keys[i]) || m.files[i].toLowerCase();
function computeClashes() {
  const owner = new Map();
  for (const m of MD.list) { m.lost = []; m.lostTo = {}; m.lostFrom = {}; }
  for (const m of MD.list) {
    if (!m.on) continue;
    const miss = new Set((m.missing || []).map(f => f.toLowerCase()));
    const take = (f, k) => {
      if (miss.has(f.toLowerCase())) return;                 // an entry the archive lacks reaches nothing
      const w = owner.get(k);
      if (!w) { owner.set(k, m); return; }
      m.lost.push(f); m.lostFrom[f.toLowerCase()] = w.name;
      // Two pictures of one mod with one fingerprint: the first in folder order is used; no arc to itself.
      if (w !== m) m.lostTo[w.folder] = (m.lostTo[w.folder] || 0) + 1;
    };
    m.files.forEach((f, i) => take(f, fileKey(m, i)));
    // Pictures clash on their fingerprint, whatever their labels.
    (m.textures || []).forEach((f, i) => take(f, (m.textureKeys && m.textureKeys[i]) || 'tex:' + f.toLowerCase()));
  }
}
const modBy = f => MD.list.find(m => m.folder === f);

/* ------------------------------------------------------------ rendering */
const stackEl = $('.stack'), lensEl = $('.lens'), modsPage = $('.page-mods'), arcsEl = $('.clash-arcs');
function plateHtml(m, i) {
  const meta = [m.version ? `<span>${esc(m.version)}</span>` : '', m.author ? `<span>${esc(m.author)}</span>` : '',
    `<span class="plate-size">${plural(m.files.length, 'file')}${m.textures && m.textures.length ? ', ' + plural(m.textures.length, 'picture') : ''}${m.size ? ', ' + sizeText(m.size) : ''}</span>`].join('');
  const lost = m.on && m.lost && m.lost.length, miss = m.missing ? m.missing.length : 0;
  const side = [miss ? `<span class="plate-warn">${miss} not found</span>` : '',
    lost ? `<span class="plate-clash">${lost === m.files.length + (m.textures || []).length ? 'All overridden' : lost + ' overridden'}</span>` : '',
    m.bad ? `<span class="plate-warn">${esc(m.bad)}</span>` : ''].join('');
  return `<li class="plate" data-focus tabindex="0" data-folder="${esc(m.folder)}" data-on="${m.on ? 1 : 0}"${i === 0 ? ' data-autofocus' : ''}
    aria-label="${esc(m.name)}, ${m.on ? 'on' : 'off'}, place ${i + 1}">
    <span class="plate-contact" data-mod-toggle title="On or off"></span>
    <span class="plate-index">${String(i + 1).padStart(2, '0')}</span>
    <img class="plate-icon" alt="" src="${modIcon(m)}">
    <span class="plate-text"><span class="plate-name">${esc(m.name)}</span><span class="plate-meta">${meta}</span></span>
    <span class="plate-side">${side}</span>
    <span class="plate-fill"></span>
  </li>`;
}
function incomingHtml(o) {
  return `<li class="plate incoming${o.ok ? '' : ' refused'}" data-focus tabindex="0" data-incoming data-on="1">
    <span class="plate-contact"></span>
    <span class="plate-index">${o.ok ? 'new' : ''}</span>
    <img class="plate-icon" alt="" src="${o.ok ? modIcon(o) : sigil(o.zip || 'zip')}">
    <span class="plate-text"><span class="plate-name">${esc(o.ok ? o.name : o.zip || 'This file')}</span><span class="plate-meta">${o.ok ? [o.version, o.author].filter(Boolean).map(t => `<span>${esc(t)}</span>`).join('') : '<span>Not installed</span>'}</span></span>
    <span class="plate-fill"></span>
  </li>`;
}
function renderMods(opts = {}) {
  computeClashes();
  const keep = document.activeElement && document.activeElement.closest && document.activeElement.closest('.plate');
  const keepFolder = keep ? keep.dataset.folder : null;
  const first = opts.flip ? flipFirst() : null;
  stackEl.innerHTML = MD.list.map(plateHtml).join('') + (MD.incoming ? incomingHtml(MD.incoming) : '');
  const on = MD.list.filter(m => m.on).length;
  modsPage.dataset.count = String(MD.list.length);
  modsPage.dataset.master = S.set.mods ? 'on' : 'off';
  bind('modsCount', !MD.list.length ? '' : !S.set.mods ? 'All off' : `${on} of ${MD.list.length} on`);
  bind('modsNav', S.set.mods && on ? String(on) : '');
  const slot = $('.slot', modsPage);
  slot.hidden = MD.list.length > 0 && !MD.incoming && app.dataset.drop !== '1' ? true : !!MD.incoming;
  bind('slotTitle', MD.list.length ? 'Drop to install' : 'No mods');
  bind('slotText', MD.list.length ? '' : 'Drop a .zip here');
  if (first) flipLast(first);
  requestAnimationFrame(drawArcs);
  MD.quiet = true;                 // putting focus back is not a new selection
  if (keepFolder) { const el = plateEl(keepFolder); if (el) { el.focus({ preventScroll: true }); placeRing(el, true); } }
  else if (keep && keep.matches('[data-incoming]')) { const el = $('[data-incoming]', stackEl); if (el) el.focus({ preventScroll: true }); }
  MD.quiet = false;
  if (!MD.incoming && MD.sel && !modBy(MD.sel)) MD.sel = MD.list.length ? MD.list[0].folder : null;
  if (!MD.sel && MD.list.length && !MD.incoming) MD.sel = MD.list[0].folder;
  renderLens();
}
const plateEl = f => $$('.plate', stackEl).find(p => p.dataset.folder === f);
/* plates glide to their new places (first, last, invert, play) */
function flipFirst() { const r = {}; $$('.plate[data-folder]', stackEl).forEach(p => { r[p.dataset.folder] = p.getBoundingClientRect().top; }); return r; }
function flipLast(first) {
  $$('.plate[data-folder]', stackEl).forEach(p => {
    const was = first[p.dataset.folder]; if (was === undefined) return;
    const d = was - p.getBoundingClientRect().top; if (Math.abs(d) < 1) return;
    p.animate([{ translate: `0 ${d}px` }, { translate: '0 0' }], { duration: 260, easing: 'cubic-bezier(.2, .9, .25, 1)' });
  });
}
/* Clash arcs: from the plate that keeps a file round the beam to the one that loses it. */
function drawArcs() {
  const wrap = $('.stack-wrap', modsPage);
  arcsEl.setAttribute('height', String(wrap.scrollHeight));
  const rem = remPx(), beamX = 1.1 * rem, out = .1 * rem;
  const y = el => el.offsetTop + stackEl.offsetTop + el.offsetHeight / 2;
  let html = '';
  for (const m of MD.list) {
    if (!m.on) continue;
    for (const [wf, n] of Object.entries(m.lostTo)) {
      const a = plateEl(wf), b = plateEl(m.folder); if (!a || !b) continue;
      const y1 = y(a), y2 = y(b), bulge = Math.min(beamX - out, 6 + (y2 - y1) * .08);
      html += `<path d="M${beamX} ${y1} C${beamX - bulge * 1.6} ${y1}, ${beamX - bulge * 1.6} ${y2}, ${beamX} ${y2}"/>` +
        `<circle cx="${beamX}" cy="${y1}" r="2.2"/><circle cx="${beamX}" cy="${y2}" r="2.2"/>` +
        `<text x="${beamX - bulge * 1.35 - 2}" y="${(y1 + y2) / 2 + 3}" text-anchor="end">${n}</text>`;
    }
  }
  arcsEl.innerHTML = html;
}
/* Markdown, read safely: everything is escaped; headings, lists and paragraphs keep their shape. */
function readmeHtml(text) {
  const out = []; let list = false, para = [];
  const flush = () => { if (para.length) { out.push(`<p>${para.join(' ')}</p>`); para = []; } };
  const inline = s => esc(s).replace(/\*\*(.+?)\*\*/g, '<b>$1</b>').replace(/`([^`]+)`/g, '<span class="mono">$1</span>').replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1');
  for (const raw of String(text).split(/\r?\n/).slice(0, 400)) {
    const l = raw.trimEnd();
    if (/^\s*([-*+]|\d+\.)\s+/.test(l)) { flush(); if (!list) { out.push('<ul>'); list = true; } out.push(`<li>${inline(l.replace(/^\s*([-*+]|\d+\.)\s+/, ''))}</li>`); continue; }
    if (list) { out.push('</ul>'); list = false; }
    if (/^#{1,6}\s/.test(l)) { flush(); out.push(`<h4>${inline(l.replace(/^#+\s*/, ''))}</h4>`); continue; }
    if (!l.trim() || /^(-{3,}|={3,}|<!--)/.test(l.trim())) { flush(); continue; }
    if (/^\s*<[^>]+>\s*$/.test(l)) continue;      // bare HTML tags (images, centring) carry nothing readable
    para.push(inline(l.trim()));
  }
  flush(); if (list) out.push('</ul>');
  return out.join('');
}
function ringHtml(m, used, lost) {
  const u = used * MOD_C, l = lost * MOD_C;
  return `<div class="lens-ring"><svg viewBox="0 0 100 100" aria-hidden="true"><circle class="r-track" cx="50" cy="50" r="46"/>` +
    `<circle class="r-used" cx="50" cy="50" r="46" stroke-dasharray="${u} ${MOD_C}"/>` +
    `<circle class="r-lost" cx="50" cy="50" r="46" stroke-dasharray="${l} ${MOD_C}" stroke-dashoffset="${-u}"/></svg>` +
    `<img alt="" src="${modIcon(m)}"></div>`;
}
function renderLens() {
  if (MD.incoming) return renderIncomingLens();
  const m = MD.sel && modBy(MD.sel);
  lensEl.hidden = !m;
  if (!m) { lensEl.innerHTML = ''; return; }
  const idx = MD.list.indexOf(m), n = m.files.length + (m.textures || []).length, lost = m.on ? m.lost.length : 0;
  const used = n ? Math.max(0, n - lost - (m.on ? (m.missing || []).length : 0)) / n : 0;   // entries the game did not find reach it no more than lost ones
  const lostSet = new Set(m.lost.map(f => f.toLowerCase())), missSet = new Set((m.missing || []).map(f => f.toLowerCase()));
  const why = new Map((m.refused || []).map(r => [String(r.file).toLowerCase(), r.why]));
  const fileRows = m.files.slice(0, 300).map((f, i) => {
    const lf = f.toLowerCase(), note = m.notes && m.notes[i] ? ` <span class="f-note">${esc(m.notes[i])}</span>` : '';
    if (missSet.has(lf)) return `<span class="f-unused" title="Not read by the game">${esc(f)} <span class="f-note">not in the archive</span></span>`;
    if (why.has(lf)) return `<span class="f-unused" title="${esc(why.get(lf))}">${esc(f)} <span class="f-note">not used</span></span>`;
    if (!lostSet.has(lf)) return `<span>${esc(f)}${note}</span>`;
    return `<span class="f-lost">${esc(f)}${note} <span class="f-note">from ${esc(m.lostFrom[lf] || '')}</span></span>`;
  }).join('') + (m.files.length > 300 ? `<span class="f-note">and ${m.files.length - 300} more</span>` : '') +
    (m.textures || []).slice(0, 300).map(f => {
      const lf = f.toLowerCase();
      if (why.has(lf)) return `<span class="f-unused" title="${esc(why.get(lf))}">${esc(f)} <span class="f-note">not used</span></span>`;
      if (lostSet.has(lf)) return `<span class="f-lost">${esc(f)} <span class="f-note">picture</span> <span class="f-note">from ${esc(m.lostFrom[lf] || '')}</span></span>`;
      return `<span>${esc(f)} <span class="f-note">picture</span></span>`;
    }).join('') +
    (m.unused || []).slice(0, 40).map(f => `<span class="f-unused" title="Not read by the game">${esc(f)}</span>`).join('');
  lensEl.dataset.on = m.on && S.set.mods ? '1' : '0';
  lensEl.innerHTML = `
    <div class="lens-top">${ringHtml(m, used, m.on ? lost / (n || 1) : 0)}
      <div><div class="lens-name">${esc(m.name)}</div>
        <div class="lens-meta">${[m.version, m.author].filter(Boolean).map(esc).join(', ')}</div></div></div>
    <div class="lens-actions">
      <button class="btn btn-small" data-mod-toggle-btn>${m.on ? 'Turn off' : 'Turn on'}</button>
      <button class="btn btn-small btn-quiet" data-mod-open>Open folder</button>
      <button class="btn btn-small btn-quiet btn-danger" data-mod-remove>Remove</button>
    </div>
    ${m.desc ? `<p class="lens-desc">${esc(m.desc)}</p>` : ''}
    <div class="lens-sec"><div class="lens-h">Files</div><div class="files">${fileRows}</div></div>
    ${m.readme ? `<div class="lens-sec"><div class="lens-h">Readme</div><div class="readme">${readmeHtml(m.readme)}</div></div>` : ''}`;
}
function renderIncomingLens() {
  const o = MD.incoming; lensEl.hidden = false; lensEl.dataset.on = '1';
  if (!o.ok) {
    lensEl.innerHTML = `<div class="lens-refusal"><div class="lens-name">${esc(o.reason)}</div></div>
      <div class="lens-actions"><button class="btn btn-small btn-primary" data-mod-dismiss data-autofocus>Close</button></div>`;
    return;
  }
  const rep = o.replaces && modBy(o.replaces.folder);
  lensEl.innerHTML = `
    <div class="lens-top">${ringHtml(o, MD.installing ? (o.p || 0) / 100 : 0, 0)}
      <div><div class="lens-name">${esc(o.name)}</div>
        <div class="lens-meta">${[o.version, o.author].filter(Boolean).map(esc).join(', ')}</div>
        ${rep && rep.version ? `<div class="lens-state">Installed: ${esc(rep.version)}</div>` : ''}</div></div>
    <div class="lens-actions">
      <button class="btn btn-small btn-primary" data-mod-install-go data-autofocus ${MD.installing ? 'disabled' : ''}>${MD.installing ? 'Installing' : rep ? 'Replace' : 'Install'}</button>
      <button class="btn btn-small btn-quiet" data-mod-dismiss ${MD.installing ? 'disabled' : ''}>Cancel</button>
    </div>
    ${o.desc ? `<p class="lens-desc">${esc(o.desc)}</p>` : ''}
    <div class="lens-sec"><div class="lens-h">${plural(o.files.length, 'file')}${o.textures && o.textures.length ? ', ' + plural(o.textures.length, 'picture') : ''}, ${sizeText(o.size || 0)}</div>
      <div class="files">${o.files.slice(0, 300).map((f, i) => (o.missing || []).includes(f)
        ? `<span class="f-unused" title="Not read by the game">${esc(f)} <span class="f-note">not in the archive</span></span>`
        : `<span>${esc(f)}${o.notes && o.notes[i] ? ` <span class="f-note">${esc(o.notes[i])}</span>` : ''}</span>`).join('')}${(o.textures || []).slice(0, 300).map(f => `<span>${esc(f)} <span class="f-note">picture</span></span>`).join('')}${(o.unused || []).slice(0, 40).map(f => `<span class="f-unused" title="Not read by the game">${esc(f)}</span>`).join('')}</div></div>
    ${o.readme ? `<div class="lens-sec"><div class="lens-h">Readme</div><div class="readme">${readmeHtml(o.readme)}</div></div>` : ''}`;
}

/* ------------------------------------------------------------- actions */
function saveOrder() {
  if (LIVE) send('live:modsorder:' + JSON.stringify(MD.list.map(m => ({ folder: m.folder, on: !!m.on }))));
}
function toggleMod(folder) {
  const m = modBy(folder); if (!m || MD.lifted) return;
  m.on = !m.on; MD.sel = folder;
  renderMods(); saveOrder();
  const el = plateEl(folder);
  if (el && m.on) { el.classList.add('lit'); setTimeout(() => el.classList.remove('lit'), 1100); }
  tick('select');
}
function moveMod(folder, d) {
  const i = MD.list.findIndex(m => m.folder === folder), j = i + d;
  if (i < 0 || j < 0 || j >= MD.list.length) { const el = plateEl(folder); if (el) nudge(el, 0, d); return false; }
  const [m] = MD.list.splice(i, 1); MD.list.splice(j, 0, m);
  renderMods({ flip: true });
  const el = plateEl(folder); if (el) { if (MD.lifted === folder) el.classList.add('lifted'); focusEl(el); }
  tick('move');
  return true;
}
function lift(folder) {
  MD.lifted = folder; MD.liftFrom = MD.list.findIndex(m => m.folder === folder);
  const el = plateEl(folder); if (el) el.classList.add('lifted');
  renderHints(); tick('select');
}
function drop(cancel) {
  const f = MD.lifted; if (!f) return;
  MD.lifted = null;
  if (cancel) { const i = MD.list.findIndex(m => m.folder === f); if (i !== MD.liftFrom) { const [m] = MD.list.splice(i, 1); MD.list.splice(MD.liftFrom, 0, m); } renderMods({ flip: true }); }
  else { const el = plateEl(f); if (el) el.classList.remove('lifted'); if (MD.list.findIndex(m => m.folder === f) !== MD.liftFrom) saveOrder(); renderMods(); }
  const el = plateEl(f); if (el) focusEl(el);
  renderHints(); tick(cancel ? 'back' : 'select');
}
function askRemove(folder) {
  const m = modBy(folder); if (!m) return;
  MD.removing = folder; bind('modRemoveTitle', `Remove ${m.name}?`); showSheet('modremove');
}
function removed(folder) {
  const el = plateEl(folder), i = MD.list.findIndex(m => m.folder === folder);
  const finish = () => {
    if (i >= 0) MD.list.splice(i, 1);
    if (MD.sel === folder) MD.sel = MD.list.length ? MD.list[Math.min(i, MD.list.length - 1)].folder : null;
    renderMods({ flip: true });
    const next = MD.sel && plateEl(MD.sel); focusEl(next || $('[data-mods-install]'));
  };
  if (!el) return finish();
  el.animate([{ opacity: 1, transform: getComputedStyle(el).transform }, { opacity: 0, transform: 'translateX(3rem)' }], { duration: 320, easing: 'cubic-bezier(.2,0,0,1)', fill: 'forwards' }).finished.then(finish);
}
function installPick() {
  if (MD.installing) return;
  if (LIVE) { send('live:modspick'); return; }
  mockInspect();
}
function showIncoming(o) {
  MD.incoming = o; MD.installing = false;
  if (S.page !== 'mods') go('mods');
  renderMods();
  halo(o.ok ? 'idle' : 'error');
  const target = $('[data-mod-install-go]', lensEl) || $('[data-mod-dismiss]', lensEl);
  setTimeout(() => focusEl(target), 60);
  const pl = $('[data-incoming]', stackEl); if (pl) pl.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
}
function dismissIncoming() {
  if (MD.installing) return;
  MD.incoming = null; halo('idle'); renderMods();
  if (LIVE) send('live:modsforget');
  const el = MD.sel && plateEl(MD.sel); focusEl(el || $('[data-mods-install]'));
}
function installGo() {
  const o = MD.incoming; if (!o || !o.ok || MD.installing) return;
  MD.installing = true; o.p = 0; renderLens(); halo('work', 0);
  if (LIVE) send('live:modsinstall:' + o.token); else mockInstall(o);
}
function installProgress(p) {
  const o = MD.incoming; if (!o) return;
  o.p = clamp(p, 0, 100);
  const pl = $('[data-incoming]', stackEl); if (pl) pl.style.setProperty('--p', o.p.toFixed(1));
  halo('work', o.p);
  const used = $('.r-used', lensEl); if (used) used.setAttribute('stroke-dasharray', `${o.p / 100 * MOD_C} ${MOD_C}`);
}
function installed(list, folder) {
  MD.installing = false; MD.incoming = null; MD.sel = folder;
  if (list) MD.list = list;
  renderMods(); halo('ok', 100); setTimeout(() => { if (!MD.incoming && !S.busy) halo('idle'); }, 800);
  const el = plateEl(folder);
  if (el) { el.classList.add('lit'); setTimeout(() => el.classList.remove('lit'), 1100); focusEl(el); }
  tick('select');
}
function installFailed(reason) {
  const o = MD.incoming; MD.installing = false;
  MD.incoming = { ok: false, zip: o ? o.zip : '', reason };
  renderMods(); halo('error');
  setTimeout(() => focusEl($('[data-mod-dismiss]', lensEl)), 60);
}
function modsMaster() { renderMods(); }

/* ------------------------------------------------------------- input */
document.addEventListener('focusin', e => {
  const p = e.target.closest && e.target.closest('.plate[data-folder]');
  if (p && !MD.quiet && !MD.incoming && MD.sel !== p.dataset.folder) { MD.sel = p.dataset.folder; renderLens(); }
});
document.addEventListener('click', e => {
  if (e.target.closest('[data-mods-install]')) { installPick(); return; }
  if (e.target.closest('[data-mods-folder]')) { if (LIVE) send('live:modsopen'); else toast('Mockup: the mods folder would open here.'); return; }
  if (e.target.closest('[data-mod-install-go]')) { installGo(); return; }
  if (e.target.closest('[data-mod-dismiss]')) { dismissIncoming(); return; }
  if (e.target.closest('[data-mod-toggle-btn]')) { if (MD.sel) toggleMod(MD.sel); return; }
  if (e.target.closest('[data-mod-open]')) { if (LIVE) send('live:modsopen:' + MD.sel); else toast('Mockup: the mod’s folder would open here.'); return; }
  if (e.target.closest('[data-mod-remove]')) { if (MD.sel) askRemove(MD.sel); return; }
  if (e.target.closest('[data-mod-remove-go]')) {
    const f = MD.removing; closeSheet();
    if (!f) return;
    if (LIVE) send('live:modsremove:' + f); else removed(f);
    return;
  }
  const plate = e.target.closest('.plate[data-folder]');
  if (!plate || MD.drag && MD.drag.moved) return;
  if (MD.lifted) { drop(false); return; }
  // Enter, Space and the controller's A arrive as a click with no pointer (detail 0): on or off.
  // With the mouse, the contact on the beam switches it, and a click elsewhere selects it.
  if (e.detail === 0 || e.target.closest('[data-mod-toggle]')) toggleMod(plate.dataset.folder);
});
document.addEventListener('dblclick', e => { const p = e.target.closest('.plate[data-folder]'); if (p && !e.target.closest('[data-mod-toggle]')) toggleMod(p.dataset.folder); });

/* Keys on the page: Alt+Up/Down move a mod, Delete removes it. A lifted
   plate (controller X) follows Up/Down until A, X or B. */
function modsKey(e) {
  if (S.page !== 'mods' || S.mode !== 'main' || S.sheetOpen || S.launchOpen || S.mockOpen) return false;
  const p = document.activeElement && document.activeElement.closest && document.activeElement.closest('.plate[data-folder]');
  if (MD.lifted) {
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') { e.preventDefault(); setInput('keyboard'); moveMod(MD.lifted, e.key === 'ArrowUp' ? -1 : 1); return true; }
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); drop(false); return true; }
    if (e.key === 'Escape' || e.key === 'Backspace') { e.preventDefault(); drop(true); return true; }
    return false;
  }
  if (!p) return false;
  if (e.altKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
    e.preventDefault(); setInput('keyboard');
    if (moveMod(p.dataset.folder, e.key === 'ArrowUp' ? -1 : 1)) saveOrder();
    return true;
  }
  if (e.key === 'Delete') { e.preventDefault(); setInput('keyboard'); askRemove(p.dataset.folder); return true; }
  return false;
}
function modsPad(btn) {
  if (S.page !== 'mods' || S.mode !== 'main' || S.sheetOpen || S.launchOpen || S.mockOpen) return;
  setInput('gamepad');
  const p = document.activeElement && document.activeElement.closest && document.activeElement.closest('.plate[data-folder]');
  if (btn === 'x') { if (MD.lifted) drop(false); else if (p) lift(p.dataset.folder); }
  if (btn === 'y' && p && !MD.lifted) askRemove(p.dataset.folder);
}
function modsDpad(dy) { if (!MD.lifted || !dy) return false; moveMod(MD.lifted, dy); return true; }
function modsBack() {
  if (S.page !== 'mods' || S.sheetOpen) return false;
  if (MD.lifted) { drop(true); return true; }
  if (MD.incoming && !MD.installing) { dismissIncoming(); return true; }
  return false;
}

/* Dragging a plate with the mouse: the others make way as it passes their middles. */
stackEl.addEventListener('pointerdown', e => {
  const p = e.target.closest('.plate[data-folder]');
  if (!p || e.button !== 0 || e.target.closest('[data-mod-toggle]') || MD.lifted || MD.incoming) return;
  const plates = $$('.plate[data-folder]', stackEl);
  MD.drag = { el: p, folder: p.dataset.folder, y0: e.clientY, moved: false, from: plates.indexOf(p), to: plates.indexOf(p),
    mids: plates.map(q => { const r = q.getBoundingClientRect(); return r.top + r.height / 2; }), step: plates.length > 1 ? plates[1].getBoundingClientRect().top - plates[0].getBoundingClientRect().top : 0, plates };
});
addEventListener('pointermove', e => {
  const d = MD.drag; if (!d) return;
  const dy = e.clientY - d.y0;
  if (!d.moved && Math.abs(dy) < 6) return;
  if (!d.moved) { d.moved = true; d.el.setPointerCapture && d.el.setPointerCapture(e.pointerId); d.el.classList.add('dragging', 'lifted'); stackEl.classList.add('reordering'); }
  const y = d.mids[d.from] + dy;
  let to = d.mids.findIndex(m => y < m); if (to < 0) to = d.mids.length - 1;
  if (to > d.from && y < d.mids[to]) to = Math.max(d.from, to - 1);
  d.to = clamp(to, 0, d.plates.length - 1);
  d.el.style.translate = `0 ${dy}px`;
  d.plates.forEach((q, i) => {
    if (q === d.el) return;
    const s = (d.from < d.to && i > d.from && i <= d.to) ? -d.step : (d.from > d.to && i >= d.to && i < d.from) ? d.step : 0;
    q.style.translate = s ? `0 ${s}px` : '';
  });
});
addEventListener('pointerup', () => {
  const d = MD.drag; if (!d) return;
  MD.drag = d.moved ? Object.assign(d, { done: true }) : null;
  if (!d.moved) return;
  setTimeout(() => { MD.drag = null; }, 0);   // the click that follows a drag is not a selection
  const r = d.el.getBoundingClientRect().top;
  stackEl.classList.remove('reordering');
  d.plates.forEach(q => { q.style.translate = ''; });
  d.el.classList.remove('dragging', 'lifted');
  if (d.to !== d.from) { const [m] = MD.list.splice(d.from, 1); MD.list.splice(d.to, 0, m); saveOrder(); }
  MD.sel = d.folder; renderMods();
  const el = plateEl(d.folder);
  if (el) { const delta = r - el.getBoundingClientRect().top; el.animate([{ translate: `0 ${delta}px` }, { translate: '0 0' }], { duration: 220, easing: 'cubic-bezier(.2,.9,.25,1)' }); focusEl(el); }
  tick('select');
});

/* A .zip dropped anywhere on the window: shown on the Mods page first. */
let dragDepth = 0;
const hasFiles = e => e.dataTransfer && Array.from(e.dataTransfer.types || []).includes('Files');
addEventListener('dragenter', e => {
  if (!hasFiles(e)) return; e.preventDefault();
  if (S.mode !== 'main' || S.sheetOpen || S.launchOpen || MD.installing) return;
  if (dragDepth++ === 0) { app.dataset.drop = '1'; if (S.page !== 'mods') go('mods'); renderMods(); }
});
addEventListener('dragover', e => { if (!hasFiles(e)) return; e.preventDefault(); e.dataTransfer.dropEffect = S.mode === 'main' && !MD.installing ? 'copy' : 'none'; });
addEventListener('dragleave', e => { if (!hasFiles(e)) return; if (--dragDepth <= 0) { dragDepth = 0; delete app.dataset.drop; renderMods(); } });
addEventListener('drop', e => {
  e.preventDefault();             // never let the window open the file
  dragDepth = 0; delete app.dataset.drop;
  if (S.mode !== 'main' || S.sheetOpen || S.launchOpen || MD.installing) { renderMods(); return; }
  const files = Array.from((e.dataTransfer && e.dataTransfer.files) || []);
  if (!files.length) { renderMods(); return; }
  if (files.length > 1) toast('One file at a time', 'info');
  const f = files[0];
  if (LIVE && hostBridge && hostBridge.postMessageWithAdditionalObjects) { hostBridge.postMessageWithAdditionalObjects('live:modsdrop', [f]); renderMods(); return; }
  if (LIVE) { showIncoming({ ok: false, zip: f.name, reason: 'Dropping does not work here. Use Install from file.' }); return; }
  mockInspect(f.name);
});

/* ------------------------------------------------------------- host */
function onModsEvent(o) {
  switch (o.ev) {
    case 'mods':
      MD.loaded = true; MD.gameFiles = o.gameFiles !== false;
      MD.list = (o.list || []).map(normMod);
      if (o.error) toast(o.error, 'info');
      if (o.folder && MD.installing) installed(MD.list, o.folder); else renderMods();
      break;
    case 'modinspect':
      if (o.cancelled) break;
      showIncoming(o.ok ? normMod(Object.assign({ on: true }, o)) : { ok: false, zip: o.zip, reason: o.reason });
      break;
    case 'modprogress': installProgress(o.p); break;
    case 'modfail': installFailed(o.reason); break;
    case 'modremoved': if (o.ok) removed(o.folder); else toast(o.reason, 'info'); break;
    case 'modorder': if (!o.ok) toast(o.reason, 'info'); break;
    case 'modnote': toast(o.text, 'info'); break;
  }
}
function normMod(o) {
  const m = Object.assign({ files: [], unused: [], missing: [], on: true }, o);
  for (const k of ['files', 'keys', 'notes', 'unused', 'missing', 'textures', 'textureKeys', 'refused']) m[k] = Array.from(m[k] || []);
  m.name = m.name || m.folder; return m;
}

/* ------------------------------------------------------------- mockup */
/* Sample mods for the design: names, versions and paths are made up for the
   page; the paths follow the disc's folders. Pictures are drawn here. */
function mockArt(kind, h1, h2) {
  const bg = `<rect width='64' height='64' fill='hsl(${h1},45%,14%)'/>`;
  const shapes = {
    rings: `<circle cx='38' cy='30' r='18' fill='none' stroke='hsl(${h2},80%,62%)' stroke-width='3'/><circle cx='38' cy='30' r='10' fill='none' stroke='hsl(${h1},80%,62%)' stroke-width='3' opacity='.7'/><path d='M0 52l64-14v26H0z' fill='hsl(${h2},60%,30%)'/>`,
    bars: [0, 1, 2, 3, 4].map(i => `<rect x='${8 + i * 10}' y='${44 - i * 6}' width='6' height='${12 + i * 6}' fill='hsl(${h2 + i * 8},75%,${50 + i * 4}%)'/>`).join(''),
    sun: `<circle cx='32' cy='40' r='16' fill='hsl(${h2},90%,58%)'/><path d='M0 44h64v20H0z' fill='hsl(${h1},50%,10%)'/>` + [0, 1, 2].map(i => `<path d='M0 ${48 + i * 5}h64' stroke='hsl(${h2},80%,55%)' stroke-width='1.5' opacity='${.7 - i * .2}'/>`).join(''),
    grid: [0, 1, 2, 3].map(i => `<path d='M${i * 18 - 6} 64L${i * 18 + 14} 30' stroke='hsl(${h2},70%,60%)' stroke-width='1.6'/>`).join('') + `<path d='M0 46h64M0 56h64' stroke='hsl(${h2},70%,60%)' stroke-width='1.2' opacity='.6'/><text x='10' y='24' font-family='Segoe UI' font-weight='800' font-size='15' fill='hsl(${h2},90%,70%)'>21</text>`
  };
  return 'data:image/svg+xml;utf8,' + encodeURIComponent(`<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>${bg}${shapes[kind]}</svg>`);
}
const MOCK_MODS = () => [
  { folder: 'nightowl-Midnight_HUD', name: 'Midnight HUD', version: '1.2.0', author: 'nightowl', on: true, size: 1460000, manifest: true,
    desc: 'The match HUD in deep blue with sharper digits.', icon: mockArt('rings', 230, 195),
    files: ['fe/HUD.XSH', 'fe/OFFIND.xsh', 'fe/HUDFONT.xsh'],
    readme: '# Midnight HUD\nA darker match HUD.\n\n## Changes\n- Score panel in deep blue\n- Sharper digits on the shot clock\n- The player marker matches\n\nMade from my own disc dump.' },
  { folder: 'courtside-Retro_Scoreboard', name: 'Retro Scoreboard', version: '2.0.1', author: 'courtside', on: true, size: 820000, manifest: true,
    desc: 'An old arena scoreboard look for the score and clock.', icon: mockArt('bars', 20, 35),
    files: ['fe/HUD.XSH', 'fe/SCORE.xsh'], readme: 'Retro Scoreboard\n\nPut it below other HUD mods if you only want the clock.' },
  { folder: 'oldschool-Classic_Rosters', name: 'Classic Rosters', version: '1.0.0', author: 'oldschool', on: true, size: 5120000, manifest: true,
    desc: 'Rosters and ratings edited by hand.', icon: mockArt('grid', 280, 300),
    files: ['database/xdbplyr.adf', 'database/xdbteam.adf', 'xf/xf000002/417.xsh'], missing: ['xf/xf000002/417.xsh'] },
  { folder: 'skyline-Sunset_Rooftop', name: 'Sunset Rooftop', version: '0.9.2', author: 'skyline', on: false, size: 38400000, manifest: true,
    desc: 'The rooftop court at sunset.', icon: mockArt('sun', 260, 28), files: ['xf/court07.viv'], unused: ['preview/sunset.jpg'] },
  { folder: 'my_ball_texture', name: 'my_ball_texture', version: '', author: '', on: true, size: 65536, manifest: false, files: ['fe/BALL.xsh'] }
];
const MOCK_ZIPS = [
  () => ({ ok: true, token: 'm1', zip: 'neonline-Street_Lights-1.1.0.zip', folder: 'neonline-Street_Lights', name: 'Street Lights', version: '1.1.0', author: 'neonline', manifest: true,
    desc: 'Brighter court lights for night games.', icon: mockArt('rings', 300, 55), size: 2240000, files: ['fe/LIGHTS.xsh', 'fe/HUD.XSH', 'courtfx/glow.fx'],
    readme: '# Street Lights\nNight courts with brighter lamps.\n\n- Works with any HUD mod\n- Put it last if you use Midnight HUD' }),
  () => ({ ok: true, token: 'm2', zip: 'nightowl-Midnight_HUD-1.3.0.zip', folder: 'nightowl-Midnight_HUD', name: 'Midnight HUD', version: '1.3.0', author: 'nightowl', manifest: true,
    desc: 'The match HUD in deep blue with sharper digits.', icon: mockArt('rings', 230, 195), size: 1480000, files: ['fe/HUD.XSH', 'fe/OFFIND.xsh', 'fe/HUDFONT.xsh'],
    replaces: { folder: 'nightowl-Midnight_HUD' } }),
  () => ({ ok: true, token: 'm3', zip: 'green rims.zip', folder: 'green_rims', name: 'green_rims', manifest: false, size: 48000, files: ['fe/RIM.xsh'] }),
  () => ({ ok: false, zip: 'super_mod_installer.zip', reason: 'Contains programs.' }),
  () => ({ ok: false, zip: 'weird.zip', reason: 'Unsafe file path.' }),
  () => ({ ok: false, zip: 'photos.zip', reason: 'No files the game reads.' })
];
function mockInspect(name) {
  if (name && !/\.zip$/i.test(name)) { showIncoming({ ok: false, zip: name, reason: 'Not a .zip file.' }); return; }
  const i = clamp(+S.set.mockZip || 0, 0, MOCK_ZIPS.length - 1);
  const o = MOCK_ZIPS[i]();
  if (name) o.zip = name;
  showIncoming(o.ok ? normMod(o) : o);
}
function mockInstall(o) {
  const t0 = performance.now(), dur = 1800;
  const stepIt = () => {
    if (MD.incoming !== o) return;
    const p = Math.min(100, (performance.now() - t0) / dur * 100);
    installProgress(p);
    if (p < 100) { requestAnimationFrame(stepIt); return; }
    const rep = o.replaces && modBy(o.replaces.folder);
    const m = normMod(Object.assign({}, o, { on: rep ? rep.on : true }));
    delete m.token; delete m.replaces; delete m.p;
    if (rep) MD.list[MD.list.indexOf(rep)] = m; else MD.list.push(m);
    installed(null, m.folder);
  };
  requestAnimationFrame(stepIt);
}
function modsInit(tokens) {
  if (LIVE) { renderMods(); setTimeout(() => send('live:modsget'), 700); return; }   // after the host has the page
  const empty = tokens.includes('mods:empty') || S.set.mockMods === 'none';
  MD.list = empty ? [] : MOCK_MODS().map(normMod);
  if (tokens.includes('mods:off')) S.set.mods = false;
  renderMods();
  const t = tokens.find(x => /^mods:/.test(x));
  if (!t) return;
  setTimeout(() => {
    if (S.page !== 'mods') return;
    if (t === 'mods:clash') focusEl(plateEl('courtside-Retro_Scoreboard'));
    if (t === 'mods:details') focusEl(plateEl('nightowl-Midnight_HUD'));
    if (t === 'mods:incoming') { S.set.mockZip = 0; mockInspect(); }
    if (t === 'mods:installing') { S.set.mockZip = 0; mockInspect(); MD.installing = true; renderLens(); installProgress(58); }
    if (t === 'mods:refused') { S.set.mockZip = 3; mockInspect(); }
    if (t === 'mods:drop') { app.dataset.drop = '1'; renderMods(); }
    if (t === 'mods:lifted') { const f = 'oldschool-Classic_Rosters'; focusEl(plateEl(f)); lift(f); moveMod(f, -1); }
  }, 1600);
}
