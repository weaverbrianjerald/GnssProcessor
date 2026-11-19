'use strict';
/*
 * Live network monitor (prototype): the page.
 *
 * The server (scripts/netview/live/server.py) sends the run's description once (/meta) and then one record per epoch
 * (/stream, server-sent events; value lists in meta).  The page keeps the records of a rolling window and redraws at
 * most four times a second, however fast records arrive:
 *   tiles      the network at the cursor (the newest epoch while "follow now" is on), each with its trend
 *   matrices   receivers x window and satellites x window for a chosen metric, worst rows first
 *   map        the receivers coloured by the receivers' metric, a red ring after an alarm
 *   feed       what happened, newest first, by family
 *   selected   every value of one receiver or satellite over the window, and its events
 * A click on a matrix cell, a map point or a feed row selects that receiver or satellite and moves the cursor there.
 */

const LV = {
  meta: null, recs: [], cursor: -1, follow: true, rxMetric: 'code', satMetric: 'code', sel: null, events: [],
  famOn: null, dirty: true, lastDraw: 0, status: null, order: {}, orderAt: {}, arrivals: [], es: null, seqMap: 'turbo',
};
const C = {};
const COLOR_KEYS = ['bg', 'panel', 'ink', 'muted', 'line', 'soft', 'tree', 'held', 'refused', 'float', 'release', 'slip',
  'gap', 'alarm', 'coderef', 'other', 'iono', 'sel', 'cursor', 'grid'];

// ============================================================================================ helpers

function readColors() { const cs = getComputedStyle(document.documentElement); for (const k of COLOR_KEYS) C[k] = cs.getPropertyValue('--' + k).trim(); }
function esc(s) { return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
function fmt(v, d = 2) { return v != null && isFinite(v) ? Number(v).toFixed(d) : 'n/a'; }
function fmtLen(v) {
  if (v == null || !isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  return a < 0.01 ? (v * 1000).toFixed(1) + ' mm' : a < 1 ? (v * 100).toFixed(1) + ' cm' : v.toFixed(2) + ' m';
}
function fmtField(f, v) {
  if (v == null || !isFinite(v)) return 'n/a';
  if (f[0] === 'held') return Math.round(100 * v) + ' %';
  if (f[3] === 'm') return fmtLen(v);
  if (f[2] === 'count' || f[0] === 'nsat' || f[0] === 'nrx') return String(Math.round(v));
  return fmt(v, 2);
}
function gpsClock(ms) { return new Date(Date.UTC(1980, 0, 6) + ms).toISOString().slice(11, 19); }
function gpsDate(ms) { return new Date(Date.UTC(1980, 0, 6) + ms).toISOString().slice(0, 10); }
function fitCanvas(cv) {
  const dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h, dpr };
}
/** Colour of a value: viridis over [lo, hi] for magnitudes and counts (0 and empty: none), vik about 0 for signed values. */
function rgbOf(kind, range, v) {
  if (v == null || !isFinite(v)) return null;
  if (kind === 'count' && v <= 0) return null;
  const table = window.NV_ASSETS.colormaps[kind === 'vik' ? 'vik' : LV.seqMap] || window.NV_ASSETS.colormaps.viridis;
  let t = kind === 'vik' ? (Math.max(-1, Math.min(1, v / (range[1] || 1))) + 1) / 2 : (v - range[0]) / ((range[1] - range[0]) || 1);
  t = Math.max(0, Math.min(1, t));
  const x = t * (table.length - 1), i = Math.min(table.length - 2, Math.floor(x)), u = x - i;
  return [0, 1, 2].map(k => Math.round(255 * (table[i][k] * (1 - u) + table[i + 1][k] * u)));
}
function cssOf(rgb) { return rgb ? 'rgb(' + rgb.join(',') + ')' : null; }
function tip(html, ev) {
  const t = document.getElementById('tip');
  if (!html) { t.hidden = true; return; }
  t.innerHTML = html; t.hidden = false;
  t.style.left = Math.min(ev.clientX + 14, window.innerWidth - t.offsetWidth - 8) + 'px';
  t.style.top = Math.min(ev.clientY + 14, window.innerHeight - t.offsetHeight - 8) + 'px';
}

// ============================================================================================ data

const M_ = () => LV.meta;
function curRec() { return LV.recs.length ? LV.recs[LV.cursor < 0 ? LV.recs.length - 1 : Math.min(LV.cursor, LV.recs.length - 1)] : null; }
function curIndex() { return LV.cursor < 0 ? LV.recs.length - 1 : Math.min(LV.cursor, LV.recs.length - 1); }
function fieldIndex(kind, key) { return M_()[kind + '_fields'].findIndex(f => f[0] === key); }
/** The colour scale of a quantity: its own kind, unless the server found a signed quantity nearly one-signed (then a magnitude
 *  scale from 0, its lowest colour holding the values below zero). */
function scaleOf(kind, f) {
  const sk = (M_().scale_kinds || {})[kind + '.' + f[0]];
  return { kind: sk ? 'viridis' : f[2], range: M_().ranges[kind + '.' + f[0]] || [0, 1], lo: sk ? sk.lo_label : null, hi: sk ? sk.hi_label : null };
}
/** Value number `fi` of row `row` of a record (null when the record has no such row: a satellite first seen later). */
function cell(rec, kind, row, fi) { const a = rec[kind][row]; return a ? a[fi] : null; }
/** Appends the list entries a record or a snapshot brings (an engine run's satellites and event words grow), and
 *  takes its colour ranges; an entry already present is skipped, so the lists stay in the server's order. */
function applyAdd(add) {
  const m = M_();
  const lists = { sats: m.sats, tiers: m.events.tiers, names: m.events.names, actions: m.events.actions, dispositions: m.dispositions };
  for (const [k, list] of Object.entries(lists)) for (const x of add[k] || []) if (!list.includes(x)) list.push(x);
  for (const f of add.families || []) if (!m.families.some(g => g.key === f.key)) m.families.push(f);   // the fixing's families
  if (add.stations && !m.stations.length) m.stations = add.stations;   // an engine's header read after the page opened
  if (add.ranges) m.ranges = add.ranges;
  if (add.scale_kinds) m.scale_kinds = add.scale_kinds;
  if (add.step_s) m.step_s = add.step_s;
  if (add.stations || add.sats) LV.order = {};          // new rows: the cached row orders are stale
  if (add.stations || add.sats || add.step_s) setSubtitle();
}
function setSubtitle() {
  const m = M_();
  document.getElementById('run-sub').textContent = m.stations.length + ' receivers, ' + m.sats.length + ' satellites, one epoch every ' + m.step_s + ' s; window ' +
    m.window + ' epochs';
}
function netValue(rec, key) { const i = M_().net_fields.findIndex(f => f[0] === key); return i < 0 || !rec ? null : rec.net[i]; }
function famKeyOf(ev) { const f = M_().events.families[ev[1]]; return f ? f[0] : 'other'; }
function famLabelOf(key) { const f = M_().events.families.find(x => x[0] === key); return f ? f[1] : key; }
function famColor(key) { return { slip: C.slip, channel: C.gap, fixing: C.held, datum: C.tree, qc: C.alarm, atmosphere: C.iono, satellite: C.refused }[key] || C.other; }
function eventWords(ev) {
  const E = M_().events, tier = E.tiers[ev[2]], name = E.names[ev[3]], act = E.actions[ev[4]];
  if (tier === 'RINGFIX' && name === 'release') return 'released: ' + (RELEASE_TEXT[act] || act);
  return EVENT_TEXT[tier + '/' + name] || (tier + ' ' + name + (act ? ' (' + act + ')' : ''));
}
function isAlarm(ev) { const l = M_().lanes[ev[0]]; return l && l.key === 'alarm'; }
/** Rows that follow another event as its bookkeeping (which bands a slip response covers, the wet-delay carry reset
 *  by a slip, tree links kept, keys declared, the phase reference re-realized): hidden from the feed unless asked. */
const BOOKKEEPING = new Set(['T0_band_scope/common_band', 'ZWD/slip_latch', 'ZWD/first_fold', 'CHAIN/kept', 'CHAIN/declared_keys',
  'PHASE_DATUM/eq34_reref', 'SCC/companion_open', 'SCC/companion_close']);
const QUIET_ACTIONS = new Set(['diagnostic', 'coincident', 'report']);   // record without acting (live/telemetry.py)
function isBookkeeping(ev) { const E = M_().events; return BOOKKEEPING.has(E.tiers[ev[2]] + '/' + E.names[ev[3]]) || QUIET_ACTIONS.has(E.actions[ev[4]]); }

function addRecord(rec) {
  if (rec.add) applyAdd(rec.add);
  LV.recs.push(rec);
  const extra = LV.recs.length - M_().window;
  if (extra > 0) {
    LV.recs.splice(0, extra);
    if (LV.cursor >= 0) LV.cursor = Math.max(0, LV.cursor - extra);
  }
  for (const ev of rec.ev) LV.events.push({ e: rec.e, ms: rec.ms, ev, fresh: performance.now() });
  if (LV.events.length > 4000) LV.events.splice(0, LV.events.length - 4000);
  const now = performance.now();
  LV.arrivals.push(now);
  LV.lastArrival = now;
  while (LV.arrivals.length && now - LV.arrivals[0] > 5000) LV.arrivals.shift();
  LV.dirty = true;
}

function onSnapshot(data) {
  LV.status = data.status;
  LV.recs = [];
  LV.events = [];
  LV.cursor = -1;
  LV.order = {};
  if (data.lists) applyAdd(data.lists);
  for (const r of data.records) addRecord(r);
  for (const x of LV.events) x.fresh = -Infinity;            // the window that came at once is not news
  LV.arrivals = [];
  document.getElementById('follow').checked = LV.follow = true;
  syncControls();
  LV.dirty = true;
}

function connect() {
  if (LV.es) LV.es.close();
  const es = new EventSource('/stream');
  LV.es = es;
  es.addEventListener('snapshot', ev => { onSnapshot(JSON.parse(ev.data)); setStatus(); });
  es.addEventListener('record', ev => { addRecord(JSON.parse(ev.data)); });
  es.onopen = () => setStatus();
  es.onerror = () => { document.getElementById('status').className = 'pill off'; document.getElementById('status').textContent = 'reconnecting'; };
}

async function control(params) {
  const r = await fetch('/control?' + new URLSearchParams(params).toString());
  LV.status = await r.json();
  syncControls();
  setStatus();
}

// ============================================================================================ drawing

/** Redraw on a timer, at most 2.5 times a second whatever the arrival rate (a background tab slows it further). */
function scheduleDraw() {
  setInterval(() => {
    if (!LV.dirty) return;
    LV.dirty = false;
    LV.lastDraw = performance.now();
    try { drawAll(); } catch (err) { showError(err); }
  }, 400);
}
function showError(err) {
  const n = document.getElementById('note');
  n.innerHTML = '<span style="color:var(--release)">Something on this page failed: ' + esc(err.message) + '</span>';
  console.error(err);
}

function drawAll() {
  if (!LV.meta || !LV.recs.length) return;
  drawClock();
  drawTiles();
  drawMatrix('rx');
  drawMatrix('sat');
  drawMap();
  drawDetail();
  drawFeed();
}

function drawClock() {
  const rec = curRec(), n = LV.recs.length;
  document.getElementById('clock').textContent = gpsDate(rec.ms) + ' ' + gpsClock(rec.ms) + ' GPS time, epoch ' + (rec.e + 1) + (M_().n_ep ? ' of ' + M_().n_ep : '');
  const cur = document.getElementById('cursor');
  cur.max = n - 1;
  cur.value = curIndex();
  const first = LV.recs[0], last = LV.recs[n - 1];
  const span = LV.arrivals.length > 1 ? (LV.arrivals[LV.arrivals.length - 1] - LV.arrivals[0]) / 1000 : 0;
  const rate = span > 0.5 ? (LV.arrivals.length - 1) / span : 0;
  const quiet = LV.lastArrival ? (performance.now() - LV.lastArrival) / 1000 : 0;   // an engine that stopped or stalls
  document.getElementById('window-label').textContent = 'window ' + gpsClock(first.ms) + ' to ' + gpsClock(last.ms) + ' (' + n + ' epochs)' +
    (rate ? ', arriving ' + rate.toFixed(1) + ' epochs/s' : '') +
    (M_().mode === 'engine' && quiet > 10 ? ', no new epoch for ' + (quiet < 120 ? Math.round(quiet) + ' s' : Math.round(quiet / 60) + ' min') : '');
  const jmp = document.getElementById('jump');
  if (+jmp.max < last.e) jmp.max = last.e;            // an engine run grows
  if (LV.status && !LV.jumping) jmp.value = last.e;
}

function series(kind, row, fi) {
  const y = new Float32Array(LV.recs.length);
  for (let c = 0; c < LV.recs.length; c++) { const v = cell(LV.recs[c], kind, row, fi); y[c] = v == null ? NaN : v; }
  return y;
}
function netSeries(key) {
  const i = M_().net_fields.findIndex(f => f[0] === key);
  return Float32Array.from(LV.recs, r => (r.net[i] == null ? NaN : r.net[i]));
}
/** A small line over the window on a canvas, with the cursor. */
function spark(cv, y, color, range) {
  const { ctx, w, h } = fitCanvas(cv);
  ctx.clearRect(0, 0, w, h);
  let lo = Infinity, hi = -Infinity;
  for (const v of y) if (isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; }
  if (range) { lo = range[0]; hi = range[1]; }
  if (!isFinite(lo)) return;
  if (hi - lo < 1e-12) { hi += 0.5; lo -= 0.5; }
  const X = c => (y.length > 1 ? c / (y.length - 1) : 0) * (w - 2) + 1, Y = v => h - 2 - (v - lo) / (hi - lo) * (h - 4);
  ctx.strokeStyle = color; ctx.lineWidth = 1.2; ctx.beginPath();
  let pen = false;
  for (let c = 0; c < y.length; c++) { if (!isFinite(y[c])) { pen = false; continue; } if (pen) ctx.lineTo(X(c), Y(y[c])); else ctx.moveTo(X(c), Y(y[c])); pen = true; }
  ctx.stroke();
  const x = X(curIndex());
  ctx.strokeStyle = C.cursor; ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
}

function drawTiles() {
  const box = document.getElementById('tiles-box'), rec = curRec(), m = M_();
  const per10 = Math.max(1, Math.round(600 / m.step_s)), lo = Math.max(0, curIndex() - per10 + 1);
  let slips = 0, alarms = 0, qc = 0;
  const si = fieldIndex('rx', 'slip'), qi = fieldIndex('rx', 'qc');
  for (let c = lo; c <= curIndex(); c++) {
    const r = LV.recs[c];
    for (const row of r.rx) { slips += row[si] || 0; qc += row[qi] || 0; }
    for (const ev of r.ev) if (isAlarm(ev)) alarms++;
  }
  const disp = netValue(rec, 'disp');
  let withheld = 0;
  for (const r of LV.recs) { const d = netValue(r, 'disp'); if (d != null && d !== 0) withheld++; }
  const items = [
    { k: 'This epoch', v: disp == null ? 'n/a' : esc((m.dispositions[disp] || '?').replace('_', ' ')), s: withheld ? withheld + ' not emitted in the window' : 'every epoch of the window emitted', cls: disp === 0 ? 'ok' : 'bad' },
    { k: 'Post-fit sigma0, code', v: fmt(netValue(rec, 's0c'), 2), s: '1 = as weighted', cls: 'info', spark: [netSeries('s0c'), C.held] },
    { k: 'Receivers in the solution', v: fmt(netValue(rec, 'npres'), 0) + ' <small>of ' + m.stations.length + '</small>', cls: netValue(rec, 'npres') < m.stations.length - 1 ? 'warn' : 'ok', spark: [netSeries('npres'), C.ink] },
    { k: 'Satellites with a clock', v: fmt(netValue(rec, 'nclk'), 0), cls: 'info', spark: [netSeries('nclk'), C.ink] },
  ];
  m.families.forEach((f, i) => {
    if (!f.fixing || !rec.fam[i]) return;
    const y = Float32Array.from(LV.recs, r => (r.fam[i] && r.fam[i][0] ? r.fam[i][1] / r.fam[i][0] : NaN));
    items.push({ k: 'Held, ' + f.label, v: rec.fam[i][0] ? Math.round(100 * rec.fam[i][1] / rec.fam[i][0]) + ' %' : 'n/a', s: rec.fam[i][1] + ' of ' + rec.fam[i][0] + ' links', cls: 'info', spark: [y, C.held, [0, 1]] });
  });
  items.push({ k: 'Cycle slips, last 10 minutes', v: String(slips), s: 'quality-test flags ' + qc, cls: 'info' });
  items.push({ k: 'Alarms, last 10 minutes', v: String(alarms), cls: alarms ? 'bad' : 'ok' });
  const u = netValue(rec, 'user3d');
  if (u != null) items.push({ k: 'Held-out user, 3D error', v: fmtLen(u), cls: u < 0.05 ? 'ok' : u < 0.2 ? 'warn' : 'bad', spark: [netSeries('user3d'), C.ink] });
  let msep = null;
  for (let c = curIndex(); c >= 0 && msep == null; c--) msep = netValue(LV.recs[c], 'msep');
  if (msep != null) items.push({ k: 'Time per epoch', v: fmt(msep / 1000, 2) + ' <small>s</small>', s: 'of the engine', cls: 'info' });
  box.innerHTML = '<div class="tiles">' + items.map((it, n) => '<div class="tile ' + (it.cls || '') + '"><div class="k">' + esc(it.k) + '</div><div class="v">' + it.v + '</div>' +
    (it.s ? '<div class="s">' + esc(it.s) + '</div>' : '') + (it.spark ? '<canvas data-spark="' + n + '"></canvas>' : '') + '</div>').join('') + '</div>';
  items.forEach((it, n) => { if (it.spark) spark(box.querySelector('canvas[data-spark="' + n + '"]'), it.spark[0], it.spark[1], it.spark[2]); });
}

// --- matrices

function rowOrder(kind, fi) {
  const key = kind + ':' + fi, n = LV.recs.length;
  if (LV.order[key] && LV.orderAt[key] > n - 30 && LV.orderAt[key] <= n) return LV.order[key];
  const f = M_()[kind + '_fields'][fi], names = kind === 'rx' ? M_().stations.map(s => s.name) : M_().sats;
  const nr = names.length, score = new Float64Array(nr), cnt = new Int32Array(nr);
  for (const r of LV.recs) for (let i = 0; i < nr; i++) { const v = cell(r, kind, i, fi); if (v != null && isFinite(v)) { score[i] += v; cnt[i]++; } }
  const worseLow = ['nsat', 'nrx', 'held'].includes(f[0]), signed = f[2] === 'vik';
  let idx = Array.from({ length: nr }, (_x, i) => i).filter(i => cnt[i] > 0 || f[2] === 'count');
  if (signed) idx.sort((a, b) => names[a].localeCompare(names[b]));
  else idx.sort((a, b) => {
    const x = f[2] === 'count' ? score[a] : score[a] / (cnt[a] || 1), y = f[2] === 'count' ? score[b] : score[b] / (cnt[b] || 1);
    return worseLow ? x - y : y - x;
  });
  if (f[2] === 'count') idx = idx.filter(i => score[i] > 0).concat(idx.filter(i => score[i] <= 0));
  LV.order[key] = idx;
  LV.orderAt[key] = n;
  return idx;
}

function drawMatrix(kind) {
  const m = M_(), cv = document.getElementById(kind + '-matrix');
  const key = kind === 'rx' ? LV.rxMetric : LV.satMetric, fi = fieldIndex(kind, key), f = m[kind + '_fields'][fi];
  const names = kind === 'rx' ? m.stations.map(s => s.name) : m.sats;
  const order = rowOrder(kind, fi), nr = order.length, n = LV.recs.length;
  const rowH = nr > 80 ? 6 : nr > 45 ? 8 : 11, labelW = 92;
  const hNeed = nr * rowH + 18;
  if (Math.abs(cv.clientHeight - hNeed) > 1) cv.style.height = hNeed + 'px';
  const { ctx, w, h, dpr } = fitCanvas(cv);
  if (!w || !h) return;
  ctx.clearRect(0, 0, w, h);
  const sc = scaleOf(kind, f), range = sc.range;
  const ncol = Math.max(1, Math.min(n, Math.round((w - labelW - 4) * dpr)));
  const img = new ImageData(ncol, Math.max(1, nr));
  const bg = hexRgb(C.soft) || [235, 235, 230];
  for (let r = 0; r < nr; r++) {
    const row = order[r];
    for (let c = 0; c < ncol; c++) {
      const c0 = Math.floor(c * n / ncol), c1 = Math.max(c0 + 1, Math.floor((c + 1) * n / ncol));
      let v = NaN;
      if (c1 - c0 === 1) { const x = cell(LV.recs[c0], kind, row, fi); v = x == null ? NaN : x; }
      else {
        let acc = 0, k = 0, mx = -Infinity;
        for (let q = c0; q < c1; q++) { const x = cell(LV.recs[q], kind, row, fi); if (x != null && isFinite(x)) { acc += x; k++; if (x > mx) mx = x; } }
        v = k ? (f[2] === 'count' ? mx : acc / k) : NaN;
      }
      const col = rgbOf(sc.kind, range, v) || bg, o = (r * ncol + c) * 4;
      img.data[o] = col[0]; img.data[o + 1] = col[1]; img.data[o + 2] = col[2]; img.data[o + 3] = 255;
    }
  }
  const tmp = document.createElement('canvas');
  tmp.width = ncol; tmp.height = Math.max(1, nr);
  tmp.getContext('2d').putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(tmp, labelW, 0, w - labelW - 4, nr * rowH);
  ctx.font = Math.min(11, rowH + 1) + 'px system-ui, sans-serif';
  ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
  const every = rowH < 8 ? Math.ceil(8 / rowH) : 1;
  for (let r = 0; r < nr; r += every) {
    const sel = LV.sel && LV.sel.kind === kind && LV.sel.i === order[r];
    ctx.fillStyle = sel ? C.release : C.ink;
    ctx.fillText(names[order[r]], labelW - 4, r * rowH + rowH / 2);
  }
  // time marks: start, quarters, end of the window
  ctx.fillStyle = C.muted; ctx.textAlign = 'center'; ctx.textBaseline = 'top'; ctx.font = '10.5px system-ui, sans-serif';
  for (const q of [0, 0.25, 0.5, 0.75, 1]) {
    const c = Math.round(q * (n - 1)), x = labelW + (n > 1 ? c / (n - 1) : 0) * (w - labelW - 4);
    ctx.fillText(gpsClock(LV.recs[c].ms).slice(0, 5), Math.min(w - 16, Math.max(labelW + 14, x)), nr * rowH + 4);
  }
  const x = labelW + (n > 1 ? curIndex() / (n - 1) : 0) * (w - labelW - 4);
  ctx.strokeStyle = C.cursor; ctx.lineWidth = 1.4; ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, nr * rowH); ctx.stroke();
  if (LV.sel && LV.sel.kind === kind) {
    const r = order.indexOf(LV.sel.i);
    if (r >= 0) { ctx.strokeStyle = C.sel; ctx.lineWidth = 2; ctx.strokeRect(labelW, r * rowH, w - labelW - 4, rowH); }
  }
  cv._hit = (mx, my) => {
    const r = Math.floor(my / rowH);
    if (r < 0 || r >= nr || mx < labelW) return null;
    const c = Math.round(Math.max(0, Math.min(1, (mx - labelW) / (w - labelW - 4))) * (n - 1));
    return { row: order[r], c };
  };
  document.getElementById(kind + '-sub').textContent = f[1] + (f[2] === 'vik' ? ', by name' : ', worst first') + '; a click selects';
  const keyBox = document.getElementById(kind + '-key');
  const table = window.NV_ASSETS.colormaps[sc.kind === 'vik' ? 'vik' : LV.seqMap] || window.NV_ASSETS.colormaps.viridis;
  const stops = table.map((c, i) => 'rgb(' + c.map(v => Math.round(255 * v)).join(',') + ') ' + Math.round(100 * i / (table.length - 1)) + '%').join(',');
  keyBox.innerHTML = '<span>' + esc(f[1]) + '</span><span>' + esc(sc.lo || fmtField(f, sc.kind === 'vik' ? -range[1] : range[0])) + '</span><span class="ramp" style="background:linear-gradient(90deg,' + stops +
    ')"></span><span>' + esc(sc.hi || fmtField(f, range[1])) + '</span>';
}
function hexRgb(c) { return c && c.startsWith('#') ? [parseInt(c.slice(1, 3), 16), parseInt(c.slice(3, 5), 16), parseInt(c.slice(5, 7), 16)] : null; }

// --- map

function drawMap() {
  const cv = document.getElementById('map'), m = M_();
  const { ctx, w, h } = fitCanvas(cv);
  if (!w || !h) return;
  ctx.clearRect(0, 0, w, h);
  const s = Math.min((w - 8) / 360, (h - 8) / 180), x0 = (w - 360 * s) / 2, y0 = (h - 180 * s) / 2;
  const XY = (la, lo) => [x0 + (lo + 180) * s, y0 + (90 - la) * s];
  ctx.fillStyle = C.panel; ctx.fillRect(x0, y0, 360 * s, 180 * s);
  const co = window.NV_ASSETS.coastline;
  if (co) {
    ctx.strokeStyle = C.line; ctx.lineWidth = 0.8; ctx.beginPath();
    let pen = false;
    for (let i = 0; i < co.lon.length; i++) {
      if (co.lon[i] == null || co.lat[i] == null) { pen = false; continue; }
      const [x, y] = XY(co.lat[i], co.lon[i]);
      if (pen) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      pen = true;
    }
    ctx.stroke();
  }
  const fi = fieldIndex('rx', LV.rxMetric), f = m.rx_fields[fi], sc = scaleOf('rx', f), range = sc.range, rec = curRec();
  const per10 = Math.max(1, Math.round(600 / m.step_s)), lo = Math.max(0, curIndex() - per10 + 1);
  const alarmed = new Set();
  for (let c = lo; c <= curIndex(); c++) for (const ev of LV.recs[c].ev) if (isAlarm(ev) && ev[5] >= 0) alarmed.add(ev[5]);
  cv._pts = [];
  m.stations.forEach((st, j) => {
    if (st.lat == null || st.lon == null) return;
    const [x, y] = XY(st.lat, st.lon);
    const sel = LV.sel && LV.sel.kind === 'rx' && LV.sel.i === j;
    ctx.fillStyle = cssOf(rgbOf(sc.kind, range, cell(rec, 'rx', j, fi))) || C.float;
    ctx.strokeStyle = sel ? C.sel : C.ink; ctx.lineWidth = sel ? 2.5 : 0.8;
    ctx.beginPath(); ctx.arc(x, y, sel ? 6 : 4.5, 0, 2 * Math.PI); ctx.fill(); ctx.stroke();
    if (alarmed.has(j)) { ctx.strokeStyle = C.alarm; ctx.lineWidth = 2; ctx.beginPath(); ctx.arc(x, y, 8, 0, 2 * Math.PI); ctx.stroke(); }
    cv._pts.push([x, y, j]);
  });
}

// --- the selected receiver or satellite

function drawDetail() {
  const box = document.getElementById('detail'), m = M_();
  if (!LV.sel) { box.innerHTML = '<div class="hint">Nothing selected yet.</div>'; return; }
  const kind = LV.sel.kind, i = LV.sel.i, qs = m[kind + '_fields'], rec = curRec();
  const name = kind === 'rx' ? m.stations[i].name : m.sats[i];
  const path = kind === 'rx' ? 'receiver/' + encodeURIComponent(name) : 'satellite/' + encodeURIComponent(name);
  const evs = [];
  for (let n = LV.events.length - 1; n >= 0 && evs.length < 12; n--) {
    const x = LV.events[n];
    if (x.e > rec.e) continue;
    if ((kind === 'rx' && x.ev[5] === i) || (kind === 'sat' && x.ev[6] === i)) evs.push(x);
  }
  box.innerHTML = '<div class="hdr">' + esc(name) + ' <span class="muted" style="font-weight:400;font-size:12px">' + (kind === 'rx' ? 'receiver' : 'satellite') + '</span></div>' +
    (m.archive ? '<div class="hint"><a class="act" target="_blank" href="/archive#/' + path + '?t=' + rec.e + '">open its page in the archived dashboard at this time</a></div>' : '') +
    qs.map((f, k) => '<div class="sparkrow"><span class="lab" title="' + esc(f[1]) + '">' + esc(f[1]) + '</span><canvas class="spark" data-k="' + k + '"></canvas><span class="val">' +
      esc(fmtField(f, cell(rec, kind, i, k))) + '</span></div>').join('') +
    '<h3 style="margin-top:10px">Its events</h3><div class="evs">' + (evs.map(x => '<div><span class="muted">' + gpsClock(x.ms) + '</span> ' + esc(eventWords(x.ev)) +
      (kind === 'rx' && x.ev[6] >= 0 ? ' <b>' + esc(m.sats[x.ev[6]]) + '</b>' : '') + (kind === 'sat' && x.ev[5] >= 0 ? ' <b>' + esc(m.stations[x.ev[5]].name) + '</b>' : '') + '</div>').join('') ||
      '<div class="hint">none in the window</div>') + '</div>';
  qs.forEach((f, k) => spark(box.querySelector('canvas[data-k="' + k + '"]'), series(kind, i, k), f[2] === 'count' ? C.slip : C.held,
    f[0] === 'held' ? [0, 1] : null));
}

// --- the feed

function drawFeed() {
  const m = M_(), box = document.getElementById('feed'), chips = document.getElementById('feed-chips');
  if (!LV.famOn) LV.famOn = new Set(m.events.families.map(f => f[0]).filter(k => !['datum', 'channel', 'other'].includes(k)));
  const counts = {};
  let nBook = 0;
  for (const x of LV.events) {
    if (isBookkeeping(x.ev)) { nBook++; if (!LV.showBook) continue; }
    const k = famKeyOf(x.ev); counts[k] = (counts[k] || 0) + 1;
  }
  chips.innerHTML = m.events.families.filter(f => counts[f[0]]).map(f => '<label class="chip"><input type="checkbox" data-f="' + f[0] + '"' + (LV.famOn.has(f[0]) ? ' checked' : '') +
    '><span class="lane-dot" style="background:' + famColor(f[0]) + '"></span>' + esc(f[1]) + ' <span class="muted">' + counts[f[0]] + '</span></label>').join('') +
    (nBook ? '<label class="chip" title="rows that follow another event: which bands a slip response covers, the wet-delay carry reset, tree links kept"><input type="checkbox" data-book="1"' +
      (LV.showBook ? ' checked' : '') + '>bookkeeping rows <span class="muted">' + nBook + '</span></label>' : '');
  const rows = [], now = performance.now(), upTo = curRec().e;
  for (let n = LV.events.length - 1; n >= 0 && rows.length < 250; n--) {
    const x = LV.events[n];
    if (x.e > upTo || !LV.famOn.has(famKeyOf(x.ev)) || (!LV.showBook && isBookkeeping(x.ev))) continue;
    const j = x.ev[5], k = x.ev[6];
    rows.push('<div data-n="' + n + '" class="' + (now - x.fresh < 3000 ? 'fresh' : '') + '"><span class="lane-dot" style="background:' + famColor(famKeyOf(x.ev)) + '"></span><span class="t">' +
      gpsClock(x.ms) + '</span>' + esc(eventWords(x.ev)) + (j >= 0 ? ' <b>' + esc(m.stations[j].name) + '</b>' : '') + (k >= 0 ? ' <b>' + esc(m.sats[k]) + '</b>' : '') +
      (x.ev[7] ? '<div class="raw">' + esc(x.ev[7].replace(/wire/gi, 'stream')) + '</div>' : '') + '</div>');
  }
  box.innerHTML = rows.join('') || '<div class="hint">nothing in the window for these families</div>';
}

function setStatus() {
  const s = LV.status, el = document.getElementById('status');
  if (!s) return;
  const xr = s.speed * s.step_s;
  el.className = 'pill ' + (s.paused ? 'paused' : 'on');
  el.textContent = s.paused ? 'paused' : s.mode === 'engine' ? 'following the engine' : 'replaying at ' + (xr >= 10 ? Math.round(xr) : xr.toFixed(1)) + 'x real time';
  document.getElementById('pause').innerHTML = s.paused ? '&#9654;' : '&#10074;&#10074;';
}
function syncControls() {
  const s = LV.status;
  if (!s) return;
  const sp = document.getElementById('speed');
  if (![...sp.options].some(o => +o.value === s.speed)) sp.insertAdjacentHTML('beforeend', '<option value="' + s.speed + '">' + s.speed + ' epochs/s</option>');
  sp.value = s.speed;
  const j = document.getElementById('jump');
  j.max = s.n_ep - 1;
}

// ============================================================================================ interaction and start

function select(kind, i, col) {
  LV.sel = { kind, i };
  if (col != null) { LV.cursor = col; LV.follow = false; document.getElementById('follow').checked = false; }
  LV.dirty = true;
}

function bindControls() {
  for (const kind of ['rx', 'sat']) {
    const seg = document.getElementById(kind + '-metric');
    seg.innerHTML = M_()[kind + '_fields'].map(f => '<button data-k="' + f[0] + '">' + esc(f[1]) + '</button>').join('');
    const mark = () => seg.querySelectorAll('button').forEach(b => b.classList.toggle('on', b.dataset.k === (kind === 'rx' ? LV.rxMetric : LV.satMetric)));
    mark();
    seg.addEventListener('click', ev => {
      const b = ev.target.closest('button');
      if (!b) return;
      if (kind === 'rx') LV.rxMetric = b.dataset.k; else LV.satMetric = b.dataset.k;
      mark();
      LV.dirty = true;
    });
    const cv = document.getElementById(kind + '-matrix');
    cv.addEventListener('click', ev => { const r = cv.getBoundingClientRect(), hit = cv._hit && cv._hit(ev.clientX - r.left, ev.clientY - r.top); if (hit) select(kind, hit.row, hit.c); });
    cv.addEventListener('mousemove', ev => {
      const r = cv.getBoundingClientRect(), hit = cv._hit && cv._hit(ev.clientX - r.left, ev.clientY - r.top);
      if (!hit) { tip(null); return; }
      const fi = fieldIndex(kind, kind === 'rx' ? LV.rxMetric : LV.satMetric), f = M_()[kind + '_fields'][fi], rec = LV.recs[hit.c];
      tip('<b>' + esc(kind === 'rx' ? M_().stations[hit.row].name : M_().sats[hit.row]) + '</b> ' + gpsClock(rec.ms) + '<br>' + esc(f[1]) + ': ' + esc(fmtField(f, cell(rec, kind, hit.row, fi))), ev);
    });
    cv.addEventListener('mouseleave', () => tip(null));
  }
  const map = document.getElementById('map');
  const near = ev => { const r = map.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top; let best = null, bd = 12; for (const p of map._pts || []) { const d = Math.hypot(p[0] - x, p[1] - y); if (d < bd) { bd = d; best = p[2]; } } return best; };
  map.addEventListener('click', ev => { const j = near(ev); if (j != null) select('rx', j); });
  map.addEventListener('mousemove', ev => {
    const j = near(ev);
    if (j == null) { tip(null); return; }
    const fi = fieldIndex('rx', LV.rxMetric), f = M_().rx_fields[fi];
    tip('<b>' + esc(M_().stations[j].name) + '</b><br>' + esc(f[1]) + ': ' + esc(fmtField(f, cell(curRec(), 'rx', j, fi))), ev);
  });
  map.addEventListener('mouseleave', () => tip(null));
  document.getElementById('feed').addEventListener('click', ev => {
    const d = ev.target.closest('[data-n]');
    if (!d) return;
    const x = LV.events[+d.dataset.n];
    const col = LV.recs.findIndex(r => r.e === x.e);
    if (x.ev[5] >= 0) select('rx', x.ev[5], col >= 0 ? col : null);
    else if (x.ev[6] >= 0) select('sat', x.ev[6], col >= 0 ? col : null);
  });
  document.getElementById('feed-chips').addEventListener('change', ev => {
    if (ev.target.dataset.book) { LV.showBook = ev.target.checked; LV.dirty = true; return; }
    const k = ev.target.dataset.f;
    if (!k) return;
    if (ev.target.checked) LV.famOn.add(k); else LV.famOn.delete(k);
    LV.dirty = true;
  });
  document.getElementById('follow').addEventListener('change', ev => { LV.follow = ev.target.checked; if (LV.follow) LV.cursor = -1; LV.dirty = true; });
  document.getElementById('cursor').addEventListener('input', ev => {
    const v = +ev.target.value;
    LV.cursor = v >= LV.recs.length - 1 ? -1 : v;
    LV.follow = LV.cursor < 0;
    document.getElementById('follow').checked = LV.follow;
    LV.dirty = true;
  });
  const sm = document.getElementById('seqmap');
  try { const v = localStorage.getItem('netview.seqmap'); if (v === 'turbo' || v === 'viridis') LV.seqMap = v; } catch (_e) { /* no storage */ }
  sm.value = LV.seqMap;
  sm.addEventListener('change', () => { LV.seqMap = sm.value; try { localStorage.setItem('netview.seqmap', LV.seqMap); } catch (_e) { /* no storage */ } LV.dirty = true; });
  document.getElementById('pause').addEventListener('click', () => control({ pause: LV.status && LV.status.paused ? 0 : 1 }));
  document.getElementById('speed').addEventListener('change', ev => control({ speed: ev.target.value }));
  const jump = document.getElementById('jump');
  jump.addEventListener('input', () => { LV.jumping = true; });
  jump.addEventListener('change', () => { LV.jumping = false; control({ jump: jump.value }); });
  window.addEventListener('resize', () => { LV.dirty = true; });
}

async function start() {
  readColors();
  const r = await fetch('/meta');
  LV.meta = await r.json();
  LV.status = LV.meta.status;
  const m = LV.meta;
  document.getElementById('run-name').textContent = 'Live network monitor: ' + m.run;
  setSubtitle();
  document.getElementById('note').innerHTML = m.mode === 'replay' ? 'Replay of a finished run: the server releases one epoch after another as a live engine would send it. ' +
    'The rate sets how many epochs arrive per second; "follow now" keeps the cursor on the newest.' :
    m.mode === 'engine' ? 'Following the engine: each epoch appears when the engine has written it to its live record (' + esc(m.source || 'network_live.jsonl') + '). ' +
    'The rate sets how fast the window catches up after a jump back; "follow now" keeps the cursor on the newest.' : '';
  document.getElementById('archive').hidden = !m.archive;
  bindControls();
  syncControls();
  setStatus();
  if (new URLSearchParams(location.search).has('still')) {      // one look at the window, no stream (checks, screenshots)
    const s = await fetch('/snapshot');
    onSnapshot(await s.json());
    drawAll();
    return;
  }
  connect();
  scheduleDraw();
  if (m.mode === 'engine') setInterval(() => { LV.dirty = true; }, 5000);   // the quiet time keeps counting
}

window.addEventListener('error', ev => showError(ev.error || new Error(ev.message)));
start().catch(showError);
