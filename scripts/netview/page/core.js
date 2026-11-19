'use strict';
/*
 * Network dashboard: the shared core of the page script.
 *
 * Every quantity is computed by the build step (scripts/netview/nv_build.py and scripts/netview/layers/); the page
 * selects and draws.  The page script is several files joined in order by nv_page.py:
 *   core.js     decode, model of the core block, state and colours, the time bar and its event lanes, small charts,
 *               the legend, the router (blocks, views, rail, breadcrumb) and the start;
 *   widgets.js  the drawing pieces the layers share (matrix of entity x time, table, sky plot, arc chart, ...);
 *   network.js  the network links view (the trees, held and float links, their detail);
 *   one file per layer (overview.js, receivers.js, ...), each registering its views; main.js starts the page.
 * Sections of this file: 1 decode, 2 model, 3 state and colours, 5 time bar and timeline, 6 small charts,
 * 9 legend and missing parts, 10 blocks, views and the router, 11 interaction and start.
 */

// ============================================================================================ 1. decode

function b64ToBytes(s) {
  if (Uint8Array.fromBase64) return Uint8Array.fromBase64(s);
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/** A data block in its script tag: {H: header, A: {name: typed array view}}; the tag's text is freed. */
async function decodeBlockEl(el) {
  let bytes = b64ToBytes(el.textContent.trim());
  el.textContent = '';
  if (el.dataset.encoding === 'gzip') {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
    bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  }
  const magic = String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
  if (magic !== 'NVB1') throw new Error('not a network view data block');
  const hlen = new DataView(bytes.buffer, bytes.byteOffset).getUint32(4, true);
  const H = JSON.parse(new TextDecoder().decode(bytes.subarray(8, 8 + hlen)));
  let base = 8 + hlen;
  base += (8 - base % 8) % 8;
  const A = {};
  for (const a of H.arrays) {
    const Ctor = window[a.type];
    A[a.name] = new Ctor(bytes.buffer, bytes.byteOffset + base + a.offset, a.length);
    A[a.name].shape = a.shape;
  }
  return { H, A };
}

/**
 * Series stored as steps between epochs (header 'delta': names; NAME_dz: 1 per row coded so) are added back up in
 * place, once, when the block opens.  The sentinel (the type's minimum) marks an absent epoch and is skipped.
 */
function undeltaBlock(b) {
  for (const name of (b.H.delta || [])) {
    const a = b.A[name], flags = b.A[name + '_dz'];
    if (!a || !flags) continue;
    const n = a.shape[a.shape.length - 1], sentinel = a instanceof Int32Array ? -2147483648 : -32768;
    for (let r = 0; r < flags.length; r++) {
      if (!flags[r]) continue;
      let acc = 0, first = true;
      for (let i = r * n, end = r * n + n; i < end; i++) {
        const v = a[i];
        if (v === sentinel) continue;
        acc = first ? v : acc + v;
        first = false;
        a[i] = acc;
      }
    }
  }
}

// ============================================================================================ 2. model

const INT32_MIN = -2147483648;
const ST = {
  float: 0, not_candidate: 1, no_float: 2, window: 3, sigma: 4, fraction: 5, lattice: 6, persist: 7, odds: 8,
  cap: 9, wl_fraction: 10, code_event: 11, refused: 12, decided: 13, held: 14, would_hold: 15,
};
const M = {};

function buildModel(H, A) {
  M.H = H; M.A = A;
  M.nEp = H.n_ep; M.nSta = H.stations.length; M.nSat = H.sats.length; M.nNode = M.nSta + M.nSat;
  M.nBand = H.bands.length; M.nFam = H.families.length;
  M.names = H.stations.map(s => s.name).concat(H.sats);
  M.nodeIndex = new Map(M.names.map((n, i) => [n, i]));
  M.lat = Float64Array.from(H.stations, s => (s.lat == null ? NaN : s.lat));
  M.lon = Float64Array.from(H.stations, s => (s.lon == null ? NaN : s.lon));
  M.frac = H.frac_scale;
  M.step = M.nEp > 1 ? (A.t_ms[M.nEp - 1] - A.t_ms[0]) / (M.nEp - 1) : 30000;
  M.hasTree = !!A.tree_parent;
  M.hasLinks = !!A.link_fam && A.link_fam.length > 0;
  M.hasFixing = H.families.some(f => f.fixing);
  M.maxDepthCache = {};
  M.famEpCache = {};
  if (M.hasLinks) indexLinks();
  indexEvents();
  indexJumps();
}

function indexLinks() {
  const A = M.A, nl = A.link_fam.length;
  M.nLinks = nl;
  M.rowLink = new Int32Array(A.ce_ep.length);
  M.linkKey = new Map();
  M.famLinkFirst = new Int32Array(M.nFam).fill(-1);
  M.famLinkEnd = new Int32Array(M.nFam).fill(-1);
  for (let l = 0; l < nl; l++) {
    const s = A.link_start[l], c = A.link_count[l];
    M.rowLink.fill(l, s, s + c);
    const f = A.link_fam[l];
    M.linkKey.set(f + '|' + A.link_sta[l] + '|' + A.link_sat[l], l);
    if (M.famLinkFirst[f] < 0) M.famLinkFirst[f] = l;
    M.famLinkEnd[f] = l + 1;
  }
}

function indexEvents() {
  const A = M.A, n = A.ev_ep ? A.ev_ep.length : 0, nl = M.H.lanes.length;
  M.nEv = n;
  M.evOff = new Int32Array(M.nEp + 1);
  M.laneCount = Array.from({ length: nl }, () => new Int32Array(M.nEp));
  M.evBySta = Array.from({ length: M.nSta }, () => []);
  M.evBySat = Array.from({ length: M.nSat }, () => []);
  for (let i = 0; i < n; i++) {
    const e = A.ev_ep[i];
    M.evOff[e + 1]++;
    M.laneCount[A.ev_lane[i]][e]++;
    if (A.ev_sta[i] >= 0) M.evBySta[A.ev_sta[i]].push(i);
    if (A.ev_sat[i] >= 0) M.evBySat[A.ev_sat[i]].push(i);
  }
  for (let e = 0; e < M.nEp; e++) M.evOff[e + 1] += M.evOff[e];
}

function indexJumps() {
  M.jumps = new Map();
  const A = M.A;
  if (!A.jp_ep) return;
  for (let i = 0; i < A.jp_ep.length; i++) {
    const k = A.jp_fam[i] + '|' + A.jp_sat[i];
    if (!M.jumps.has(k)) M.jumps.set(k, []);
    M.jumps.get(k).push(i);
  }
}

// --- time

function gpsDate(e) {
  const ms = Date.UTC(1980, 0, 6) + (M.H.week0 * 604800000) + M.H.tow0_ms + M.A.t_ms[e];
  return new Date(ms);
}
function pad2(v) { return String(v).padStart(2, '0'); }
function clockOf(e) {
  const d = gpsDate(e);
  return pad2(d.getUTCHours()) + ':' + pad2(d.getUTCMinutes()) + ':' + pad2(d.getUTCSeconds());
}
function elapsedOf(e) {
  const m = Math.round(M.A.t_ms[e] / 60000);
  return Math.floor(m / 60) + ' h ' + pad2(m % 60) + ' min';
}
function dayOfYear(d) {
  return Math.floor((Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()) - Date.UTC(d.getUTCFullYear(), 0, 1)) / 864e5) + 1;
}
/** GPS time of epoch e in milliseconds since the GPS origin (the key two pages share when they are compared). */
function absMs(e) { return M.H.week0 * 604800000 + M.H.tow0_ms + M.A.t_ms[e]; }
/** The epoch nearest to an absolute GPS time, or -1 when it lies outside this run by more than one step. */
function epochAtMs(ms) {
  const rel = ms - M.H.week0 * 604800000 - M.H.tow0_ms, t = M.A.t_ms;
  if (rel < t[0] - M.step || rel > t[M.nEp - 1] + M.step) return -1;
  let lo = 0, hi = M.nEp - 1;
  while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (t[mid] <= rel) lo = mid; else hi = mid; }
  return Math.abs(t[hi] - rel) < Math.abs(t[lo] - rel) ? hi : lo;
}
/** Ticks at whole GPS hours (every second hour on long runs): [{e, label}]. */
function hourTicks() {
  if (M.ticks) return M.ticks;
  const H = 3600000, base = Date.UTC(1980, 0, 6) + M.H.week0 * 604800000 + M.H.tow0_ms;
  const t0 = base + M.A.t_ms[0], t1 = base + M.A.t_ms[M.nEp - 1];
  const step = (t1 - t0) / H > 14 ? 2 : 1;
  M.ticks = [];
  for (let h = Math.ceil(t0 / (step * H)) * step * H; h <= t1; h += step * H) {
    const e = epochAtMs(h - Date.UTC(1980, 0, 6));
    if (e >= 0) M.ticks.push({ e, label: pad2(new Date(h).getUTCHours()) + ':00' });
  }
  return M.ticks;
}
function durationText(nEpochs) {
  const m = Math.round(nEpochs * M.step / 60000);
  return m >= 60 ? Math.floor(m / 60) + ' h ' + pad2(m % 60) + ' min' : m + ' min';
}

// --- tree

function tIdx(b, n, e) { return (b * M.nNode + n) * M.nEp + e; }
function treeParent(b, n, e) { return M.hasTree && b >= 0 ? M.A.tree_parent[tIdx(b, n, e)] : -1; }
function treeDepth(b, n, e) { return M.hasTree && b >= 0 ? M.A.tree_depth[tIdx(b, n, e)] : -1; }
function treeTarget(b, n, e) {
  if (!M.hasTree || b < 0) return null;
  const v = M.A.tree_target[tIdx(b, n, e)];
  return v === INT32_MIN ? null : v;
}
function treeRow(b, n, e) { return M.hasTree && b >= 0 ? M.A.tree_row[tIdx(b, n, e)] : 0; }
/** Is the link (receiver node r, satellite node s) a tree link of signal b at epoch e? */
function isTreeLink(b, r, s, e) { return treeParent(b, s, e) === r || treeParent(b, r, e) === s; }
function maxDepth(b) {
  if (b < 0 || !M.hasTree) return 1;
  if (M.maxDepthCache[b] == null) {
    let m = 1;
    const base = b * M.nNode * M.nEp, d = M.A.tree_depth;
    for (let i = 0; i < M.nNode * M.nEp; i++) if (d[base + i] > m) m = d[base + i];
    M.maxDepthCache[b] = m;
  }
  return M.maxDepthCache[b];
}
/** Epochs at which the tree of signal b moved node n (a new parent, a return, a split or merge, a slipped link). */
function treeMoves(b, n) {
  const out = [];
  if (!M.hasTree || b < 0) return out;
  const kept = M.H.tree_rows.indexOf('kept'), root = M.H.tree_rows.indexOf('reference receiver');
  for (let e = 0; e < M.nEp; e++) {
    const r = treeRow(b, n, e);
    if (r !== 0 && r !== kept && r !== root) out.push(e);
  }
  return out;
}
/** What the tree did to node n at epoch e, in plain words, or '' when it kept it. */
function treeChangeText(b, n, e) {
  const r = treeRow(b, n, e), label = M.H.tree_rows[r] || '';
  return label === 'kept' || label === 'not in the tree' || label === 'reference receiver' ? '' : label;
}
/** The tree path from node n to its reference receiver (list of nodes), signal b, epoch e. */
function pathToRoot(b, n, e) {
  const out = [n];
  for (let k = 0; k < 64; k++) {
    const p = treeParent(b, out[out.length - 1], e);
    if (p < 0) break;
    out.push(p);
  }
  return out;
}

// --- links that are not tree links

function linkOf(f, sta, sat) {
  if (!M.hasLinks) return -1;
  const v = M.linkKey.get(f + '|' + sta + '|' + sat);
  return v == null ? -1 : v;
}
function linkRowAt(l, e) {
  const A = M.A;
  let lo = A.link_start[l], hi = lo + A.link_count[l] - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1, v = A.ce_ep[mid];
    if (v === e) return mid;
    if (v < e) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}
/** Rows of family f present at epoch e (a subarray of row indices). */
function famRowsAt(f, e) {
  if (!M.hasLinks || M.famLinkFirst[f] < 0) return new Int32Array(0);
  let c = M.famEpCache[f];
  if (!c) {
    const A = M.A, off = new Int32Array(M.nEp + 1);
    const r0 = A.link_start[M.famLinkFirst[f]];
    const lastL = M.famLinkEnd[f] - 1;
    const r1 = A.link_start[lastL] + A.link_count[lastL];
    for (let r = r0; r < r1; r++) off[A.ce_ep[r] + 1]++;
    for (let i = 0; i < M.nEp; i++) off[i + 1] += off[i];
    const rows = new Int32Array(r1 - r0), fill = off.slice(0, M.nEp);
    for (let r = r0; r < r1; r++) rows[fill[A.ce_ep[r]]++] = r;
    c = M.famEpCache[f] = { off, rows };
  }
  return c.rows.subarray(c.off[e], c.off[e + 1]);
}
function sigmaOf(code) {
  if (!code) return NaN;
  const [lo, hi] = M.H.sigma_code;
  return Math.pow(10, lo + code * (hi - lo) / 255);
}
/** Everything a row says: floats, distances from the whole numbers in use, sigmas, state, held integers. */
function rowInfo(r) {
  const A = M.A, f = M.frac;
  const wlInt = A.ce_wlint[r], nInt = A.ce_nint[r];
  const n1 = nInt + A.ce_dn1[r], wl = wlInt + A.ce_dwl[r];
  return {
    state: A.ce_state[r], ring: A.ce_ring[r],
    wlFloat: wlInt + A.ce_wlfrac[r] / f, nFloat: nInt + A.ce_nfrac[r] / f,
    wlDist: A.ce_wlfrac[r] / f - A.ce_dwl[r], nDist: A.ce_nfrac[r] / f - A.ce_dn1[r],
    sWl: sigmaOf(A.ce_swl[r]), sIf: sigmaOf(A.ce_sif[r]),
    n1, wl, n2: n1 - wl, res: A.ce_res[r],
  };
}
function stateKind(code) {
  if (code === ST.held) return 'held';
  if (code === ST.decided || code === ST.would_hold) return 'decided';
  if (code === ST.float) return 'float';
  return 'refused';
}

// --- products and receivers

function satSeries(name, sat, sig) {
  const A = M.A;
  if (name === 'clk') return A.sat_clk ? A.sat_clk.subarray(sat * M.nEp, (sat + 1) * M.nEp) : null;
  const tab = name === 'pb' ? M.H.phase_signals : M.H.code_signals;
  const arr = name === 'pb' ? A.sat_pb : A.sat_cb;
  const k = tab.indexOf(sig);
  if (!arr || k < 0) return null;
  const o = (k * M.nSat + sat) * M.nEp;
  return arr.subarray(o, o + M.nEp);
}
function satLevel(f, band, sat) {
  const a = M.A.sat_level;
  if (!a) return null;
  const o = ((f * 2 + band) * M.nSat + sat) * M.nEp;
  return a.subarray(o, o + M.nEp);
}
/** Elevation (degrees, NaN when not tracked) of satellite sat seen from receiver sta, over the run. */
function losElevation(sta, sat) {
  const a = M.A.los_el;
  if (!a) return null;
  const o = (sta * M.nSat + sat) * M.nEp, y = new Float32Array(M.nEp);
  let any = false;
  for (let e = 0; e < M.nEp; e++) { const v = a[o + e]; y[e] = v === 255 ? NaN : v; any = any || v !== 255; }
  return any ? y : null;
}
function rxSeries(name, sta, band) {
  const a = M.A[name];
  if (!a) return null;
  const o = (band == null ? sta : band * M.nSta + sta) * M.nEp;
  return a.subarray(o, o + M.nEp);
}

// ============================================================================================ 3. state and colours

const S = {
  ep: 0, fam: 0, bandSel: 0, layout: 'rings', showFloat: true, showHeld: true, showLabels: true,
  sel: null, hover: null, playing: false, speed: 5, showOther: false, seqMap: 'turbo',
};
const C = {};
const COLOR_KEYS = ['bg', 'panel', 'ink', 'muted', 'line', 'soft', 'tree', 'held', 'decided', 'refused', 'float',
  'release', 'slip', 'gap', 'alarm', 'coderef', 'other', 'whole', 'iono', 'frac', 'sel', 'root', 'cursor', 'grid'];
const LANE_COLOR = { hold: 'held', release: 'release', tree: 'tree', slip: 'slip', gap: 'gap', alarm: 'alarm',
  coderef: 'coderef', other: 'other' };

function readColors() {
  const cs = getComputedStyle(document.documentElement);
  for (const k of COLOR_KEYS) C[k] = cs.getPropertyValue('--' + k).trim();
}
function rampColor(table, t) {
  if (!isFinite(t)) return C.soft;
  t = Math.max(0, Math.min(1, t));
  const x = t * (table.length - 1), i = Math.min(table.length - 2, Math.floor(x)), u = x - i;
  const c = [0, 1, 2].map(k => Math.round(255 * (table[i][k] * (1 - u) + table[i + 1][k] * u)));
  return 'rgb(' + c.join(',') + ')';
}
function viridis(t) { return rampColor(window.NV_ASSETS.colormaps.viridis, t); }
/** The colour map of magnitudes and counts (matrices, maps, node colours): turbo, the author's choice for residuals and
 *  similar (2026-11-17), or viridis; the reader's pick is kept in the browser.  Signed values keep vik. */
function seqTable() { return window.NV_ASSETS.colormaps[S.seqMap] || window.NV_ASSETS.colormaps.viridis; }
function seqColor(t) { return rampColor(seqTable(), t); }
function loadSeqMap() { try { const v = localStorage.getItem('netview.seqmap'); if (v === 'turbo' || v === 'viridis') S.seqMap = v; } catch (_e) { /* no storage: the default stays */ } }
function saveSeqMap() { try { localStorage.setItem('netview.seqmap', S.seqMap); } catch (_e) { /* no storage */ } }
function vik(t) { return rampColor(window.NV_ASSETS.colormaps.vik, (t + 1) / 2); }
function withAlpha(color, a) {
  if (color.startsWith('#')) {
    const n = parseInt(color.slice(1), 16);
    return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a + ')';
  }
  if (color.startsWith('rgb(')) return color.replace('rgb(', 'rgba(').replace(')', ',' + a + ')');
  return color;
}
function laneColor(lane) { return C[LANE_COLOR[M.H.lanes[lane].key]] || C.other; }

function curFamily() {
  const f = M.H.families[S.fam];
  if (!f) return null;
  return { f, fi: S.fam, b: S.bandSel ? f.band2 : f.band1, bo: S.bandSel ? f.band1 : f.band2,
    sig: S.bandSel ? f.b2 : f.b1 };
}
function sysName(c) { return { G: 'GPS', E: 'Galileo', C: 'BeiDou', J: 'QZSS', R: 'GLONASS' }[c] || c; }
function famLabel(f) { return sysName(f.sys) + ' ' + f.b1 + ' + ' + f.b2; }
function shortSta(name) { return name.slice(0, 4); }
function esc(s) { return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
function fmt(v, d = 3) { return isFinite(v) ? v.toFixed(d) : 'n/a'; }
function fmtSigned(v, d = 3) { return isFinite(v) ? (v >= 0 ? '+' : '') + v.toFixed(d) : 'n/a'; }


// ============================================================================================ 5. time bar and timeline

const TL = { labelW: 128, laneH: 11, top: 3, cache: null };

function timelineLanes() {
  const lanes = M.H.lanes.map((l, i) => ({ label: l.label, lane: i, color: laneColor(i), counts: M.laneCount[i] }));
  const sel = selectionEvents();
  if (sel) lanes.unshift({ label: 'Selected', lane: -1, color: C.sel, counts: sel.counts, selected: true });
  return lanes;
}

function drawTimeline() {
  const cv = document.getElementById('timeline');
  const lanes = timelineLanes();
  const hNeeded = TL.top + lanes.length * TL.laneH + 38;
  if (Math.abs(cv.clientHeight - hNeeded) > 1) {
    cv.style.height = hNeeded + 'px';         // a lane came or went: the view below takes the difference
    viewResized();
  }
  const { ctx, w, h } = fitCanvas(cv);
  if (!w || !h) return;                       // not laid out yet (a background tab); the resize handler redraws
  const key = [w, h, S.fam, selKey(S.sel), lanes.length].join('|');
  if (!TL.cache || TL.cache.key !== key) TL.cache = { key, img: renderTimelineStatic(lanes, w, h) };
  ctx.clearRect(0, 0, w, h);
  ctx.drawImage(TL.cache.img, 0, 0, w, h);
  const x = tlX(S.ep, w);
  ctx.strokeStyle = C.cursor; ctx.lineWidth = 1.5;
  ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
}
function tlX(e, w) { return TL.labelW + (M.nEp > 1 ? e / (M.nEp - 1) : 0) * (w - TL.labelW - 8); }
function tlEp(x, w) { return Math.round(Math.max(0, Math.min(1, (x - TL.labelW) / (w - TL.labelW - 8))) * (M.nEp - 1)); }

function renderTimelineStatic(lanes, w, h) {
  const dpr = window.devicePixelRatio || 1;
  const off = document.createElement('canvas');
  off.width = Math.round(w * dpr); off.height = Math.round(h * dpr);
  const ctx = off.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.font = '11px system-ui, sans-serif'; ctx.textBaseline = 'middle';
  lanes.forEach((ln, k) => {
    const y0 = TL.top + k * TL.laneH;
    ctx.fillStyle = k % 2 ? C.panel : C.soft;
    ctx.fillRect(TL.labelW, y0, w - TL.labelW - 8, TL.laneH);
    ctx.fillStyle = C.ink; ctx.textAlign = 'right';
    let total = 0, max = 0;
    for (let e = 0; e < M.nEp; e++) { total += ln.counts[e]; if (ln.counts[e] > max) max = ln.counts[e]; }
    ctx.fillText(ln.label + ' ' + total, TL.labelW - 6, y0 + TL.laneH / 2);
    if (!max) return;
    ctx.fillStyle = ln.color;
    const lm = Math.log1p(max);
    for (let e = 0; e < M.nEp; e++) {
      const c = ln.counts[e];
      if (!c) continue;
      const hh = Math.max(3, (TL.laneH - 2) * Math.log1p(c) / lm);
      const x = tlX(e, w);
      ctx.fillRect(x - 0.75, y0 + TL.laneH - 1 - hh, 1.5, hh);
    }
  });
  const yAx = TL.top + lanes.length * TL.laneH;
  drawHeldShareStrip(ctx, w, yAx + 2, 18);
  ctx.fillStyle = C.muted; ctx.textAlign = 'center'; ctx.textBaseline = 'top';
  for (const tk of hourTicks()) {
    const x = tlX(tk.e, w);
    ctx.fillText(tk.label, x, yAx + 22);
    ctx.fillStyle = C.line; ctx.fillRect(x, TL.top, 1, lanes.length * TL.laneH); ctx.fillStyle = C.muted;
  }
  return off;
}

function drawHeldShareStrip(ctx, w, y0, hh) {
  const cf = curFamily();
  ctx.fillStyle = C.muted; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
  ctx.font = '11px system-ui, sans-serif';
  if (!cf || !M.A.sum_links) return;
  const nf = M.nFam, ne = M.nEp, sl = M.A.sum_links;
  const label = cf.f.fixing ? 'Held share' : 'Links (count)';
  ctx.fillText(label, TL.labelW - 6, y0 + hh / 2);
  ctx.fillStyle = C.soft; ctx.fillRect(TL.labelW, y0, w - TL.labelW - 8, hh);
  let max = 1;
  if (!cf.f.fixing) for (let e = 0; e < ne; e++) max = Math.max(max, sl[cf.fi * ne + e]);
  ctx.fillStyle = withAlpha(cf.f.fixing ? C.held : C.float, 0.75);
  for (let e = 0; e < ne; e++) {
    const n = sl[cf.fi * ne + e], held = sl[(nf + cf.fi) * ne + e];
    const v = cf.f.fixing ? (n ? held / n : 0) : n / max;
    if (!v) continue;
    const x = tlX(e, w);
    ctx.fillRect(x - 0.75, y0 + hh * (1 - v), 1.6, hh * v);
  }
}

function selKey(sel) {
  if (!sel) return '';
  if (sel.kind === 'link') return 'link:' + sel.sta + ':' + sel.sat;
  if (sel.kind === 'events') return 'events:' + sel.ep + ':' + sel.lane;
  return sel.kind + ':' + sel.node;
}

/** Events of the selected link or node: per-epoch counts and the event indices. */
function selectionEvents() {
  const sel = S.sel;
  if (!sel || !M.nEv) return null;
  if (sel._ev) return sel._ev;
  let idx = [];
  const A = M.A;
  if (sel.kind === 'link') idx = M.evBySta[sel.sta].filter(i => A.ev_sat[i] === sel.sat);
  else if (sel.kind === 'sta') idx = M.evBySta[sel.node];
  else if (sel.kind === 'sat') idx = M.evBySat[sel.node - M.nSta];
  const counts = new Int32Array(M.nEp);
  for (const i of idx) counts[A.ev_ep[i]]++;
  sel._ev = { idx, counts };
  return sel._ev;
}

function updateTimeLabel() {
  document.getElementById('time-label').textContent =
    clockOf(S.ep) + ' GPS time, ' + elapsedOf(S.ep) + ' into the run (epoch ' + (S.ep + 1) + ' of ' + M.nEp + ')';
}

// ============================================================================================ 6. small charts

/**
 * A chart over the run's epochs on a canvas, its static part cached.  spec: {series: [{y, color, width, dash,
 * step}], bands: [{lo, hi, color}], hlines: [{y, color, dash}], dots: [{e, y, color, r}], ticks: [{e, color}],
 * strip: {codes, colors}, bars: [{e0, e1, stack: [{v, color}]}], yRange, symmetric, yFmt, title}.
 */
const CHARTS = [];

/** Size a canvas's backing store to its laid-out size and the screen's pixel ratio; returns {ctx, w, h} in CSS px. */
function fitCanvas(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
    cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
  }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h };
}

function makeChart(canvas, spec) {
  const ch = { cv: canvas, spec, img: null, key: '' };
  canvas.addEventListener('click', ev => {
    const r = canvas.getBoundingClientRect();
    setEp(chartEp(ch, ev.clientX - r.left));
  });
  canvas.addEventListener('mousemove', ev => {
    const r = canvas.getBoundingClientRect();
    const e = chartEp(ch, ev.clientX - r.left);
    canvas.title = clockOf(e) + (spec.valueAt ? '  ' + spec.valueAt(e) : '');
  });
  CHARTS.push(ch);
  drawChart(ch);
  return ch;
}
function chartBox(w, h, spec) {
  if (spec.spark) return { l: 1, r: 1, t: 2, b: 2, w, h };
  return { l: 50, r: 8, t: spec.title ? 15 : 5, b: spec.noAxis ? 4 : 15, w, h };
}
function chartX(bx, e) { return bx.l + (M.nEp > 1 ? e / (M.nEp - 1) : 0) * (bx.w - bx.l - bx.r); }
function chartEp(ch, x) {
  const bx = chartBox(ch.cv.clientWidth, ch.cv.clientHeight, ch.spec);
  return Math.round(Math.max(0, Math.min(1, (x - bx.l) / (bx.w - bx.l - bx.r))) * (M.nEp - 1));
}
function drawChart(ch) {
  if (!ch.cv.isConnected) return;
  if (ch.draw) { ch.draw(); return; }        // a widget with its own drawing (matrix, arc chart, sky plot, map)
  const { ctx, w, h } = fitCanvas(ch.cv);
  if (!w || !h) return;
  const key = w + 'x' + h;
  if (!ch.img || ch.key !== key) { ch.img = renderChartStatic(ch.spec, w, h); ch.key = key; }
  ctx.clearRect(0, 0, w, h);
  ctx.drawImage(ch.img, 0, 0, w, h);
  const bx = chartBox(w, h, ch.spec), x = chartX(bx, S.ep);
  ctx.strokeStyle = C.cursor; ctx.lineWidth = 1.2;
  ctx.beginPath(); ctx.moveTo(x, bx.t); ctx.lineTo(x, h - bx.b); ctx.stroke();
}
function drawAllCharts() {
  for (let i = CHARTS.length - 1; i >= 0; i--) {
    if (!CHARTS[i].cv.isConnected) CHARTS.splice(i, 1); else drawChart(CHARTS[i]);
  }
}
function niceTicks(lo, hi, n) {
  const span = hi - lo;
  if (!(span > 0)) return [lo];
  const raw = span / n, mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const st = [1, 2, 2.5, 5, 10].map(m => m * mag).find(s => s >= raw) || 10 * mag;
  const out = [];
  for (let v = Math.ceil(lo / st) * st; v <= hi + 1e-12; v += st) out.push(Math.abs(v) < 1e-12 ? 0 : v);
  return out;
}
/** y range from the 1st to the 99th percentile of the series (a start-up transient does not flatten the rest). */
function robustRange(spec) {
  const v = [];
  for (const s of spec.series || []) for (let e = 0; e < s.y.length; e++) if (isFinite(s.y[e])) v.push(s.y[e]);
  if (v.length < 20) return null;
  v.sort((p, q) => p - q);
  const lo = v[Math.floor(0.01 * (v.length - 1))], hi = v[Math.ceil(0.99 * (v.length - 1))];
  const pad = Math.max((hi - lo) * 0.12, Math.abs(hi) * 1e-6, 1e-6);
  return [lo - pad, hi + pad];
}

function seriesRange(spec) {
  if (spec.robust) { const r = robustRange(spec); if (r) return r; }
  let lo = Infinity, hi = -Infinity;
  const take = v => { if (isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } };
  for (const s of spec.series || []) for (let e = 0; e < s.y.length; e++) take(s.y[e]);
  for (const b of spec.bands || []) for (let e = 0; e < b.lo.length; e++) { take(b.lo[e]); take(b.hi[e]); }
  for (const d of spec.dots || []) take(d.y);
  for (const b of spec.bars || []) { let t = 0; for (const s of b.stack) t += s.v; take(t); take(0); }
  for (const l of spec.hlines || []) if (l.inRange) take(l.y);
  if (!isFinite(lo)) return [0, 1];
  if (spec.symmetric) { const m = Math.max(Math.abs(lo), Math.abs(hi), spec.minHalf || 0); return [-m * 1.08, m * 1.08]; }
  if (spec.zeroBased) lo = Math.min(lo, 0);
  if (hi - lo < (spec.minSpan || 1e-9)) { const c = (hi + lo) / 2, s = (spec.minSpan || 1) / 2; lo = c - s; hi = c + s; }
  const pad = (hi - lo) * 0.07;
  return [spec.zeroBased && lo === 0 ? 0 : lo - pad, hi + pad];
}
function renderChartStatic(spec, w, h) {
  const dpr = window.devicePixelRatio || 1;
  const off = document.createElement('canvas');
  off.width = Math.round(w * dpr); off.height = Math.round(h * dpr);
  const ctx = off.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const bx = chartBox(w, h, spec);
  const [y0, y1] = spec.yRange || seriesRange(spec);
  const Y = v => bx.t + (1 - (v - y0) / (y1 - y0)) * (h - bx.t - bx.b);
  const X = e => chartX(bx, e);
  ctx.font = '10.5px system-ui, sans-serif';
  if (spec.title) { ctx.fillStyle = C.ink; ctx.textAlign = 'left'; ctx.textBaseline = 'top'; ctx.fillText(spec.title, 2, 1); }
  ctx.fillStyle = C.panel; ctx.fillRect(bx.l, bx.t, w - bx.l - bx.r, h - bx.t - bx.b);
  // grid and y ticks
  if (!spec.strip && !spec.spark) {
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (const v of niceTicks(y0, y1, spec.nTicks || 3)) {
      const y = Y(v);
      ctx.strokeStyle = C.grid; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(bx.l, y); ctx.lineTo(w - bx.r, y); ctx.stroke();
      ctx.fillStyle = C.muted; ctx.fillText((spec.yFmt || shortNum)(v), bx.l - 4, y);
    }
  }
  // x ticks (hours)
  if (!spec.noAxis && !spec.spark) {
    ctx.textAlign = 'center'; ctx.textBaseline = 'top'; ctx.fillStyle = C.muted;
    const ticks = hourTicks(), every = ticks.length > 8 ? 2 : 1;
    ticks.forEach((tk, i) => { if (i % every === 0) ctx.fillText(tk.label, X(tk.e), h - bx.b + 2); });
  }
  if (spec.strip) {
    const { codes, colors } = spec.strip;
    for (let e = 0; e < M.nEp; e++) {
      const c = colors[codes[e]];
      if (!c) continue;
      ctx.fillStyle = c;
      ctx.fillRect(X(e) - 0.5, bx.t + 2, Math.max(1, X(e + 1) - X(e)) + 0.5, h - bx.t - bx.b - 4);
    }
  }
  for (const b of spec.bars || []) {
    let acc = 0;
    const xa = X(b.e0) + 1, xb = X(b.e1) - 1;
    for (const s of b.stack) {
      if (!(s.v > 0)) continue;
      ctx.fillStyle = s.color;
      ctx.fillRect(xa, Y(acc + s.v), Math.max(1, xb - xa), Y(acc) - Y(acc + s.v));
      acc += s.v;
    }
    if (b.label) {
      ctx.fillStyle = C.muted; ctx.textAlign = 'center'; ctx.textBaseline = 'bottom'; ctx.font = '9px system-ui, sans-serif';
      ctx.fillText(b.label, (xa + xb) / 2, Y(acc) - 1);
      ctx.font = '10.5px system-ui, sans-serif';
    }
  }
  for (const b of spec.bands || []) {
    ctx.fillStyle = b.color;
    for (let e = 0; e < M.nEp; e++) {
      const lo = b.lo[e], hi = b.hi[e];
      if (!isFinite(lo) || !isFinite(hi)) continue;
      const ya = Y(Math.min(y1, hi)), yb = Y(Math.max(y0, lo));
      ctx.fillRect(X(e) - 0.6, ya, Math.max(1.2, X(e + 1) - X(e)), yb - ya);
    }
  }
  for (const l of spec.hlines || []) {
    if (l.y < y0 || l.y > y1) continue;
    ctx.strokeStyle = l.color || C.muted; ctx.lineWidth = 1; ctx.setLineDash(l.dash || [4, 3]);
    ctx.beginPath(); ctx.moveTo(bx.l, Y(l.y)); ctx.lineTo(w - bx.r, Y(l.y)); ctx.stroke(); ctx.setLineDash([]);
  }
  ctx.save();
  ctx.beginPath(); ctx.rect(bx.l, bx.t, w - bx.l - bx.r, h - bx.t - bx.b); ctx.clip();
  for (const s of spec.series || []) {
    ctx.strokeStyle = s.color; ctx.lineWidth = s.width || 1.3; ctx.setLineDash(s.dash || []);
    ctx.beginPath();
    let pen = false, prevY = 0;
    for (let e = 0; e < s.y.length; e++) {
      const v = s.y[e];
      if (!isFinite(v)) { if (!s.connect) pen = false; continue; }   // connect: a sparse series joins its points
      const x = X(e), y = Y(v);
      if (!pen) { ctx.moveTo(x, y); pen = true; }
      else if (s.step) { ctx.lineTo(x, prevY); ctx.lineTo(x, y); }
      else ctx.lineTo(x, y);
      prevY = y;
    }
    ctx.stroke(); ctx.setLineDash([]);
  }
  for (const d of spec.dots || []) {
    if (!isFinite(d.y)) continue;
    ctx.fillStyle = d.color; ctx.beginPath(); ctx.arc(X(d.e), Y(Math.max(y0, Math.min(y1, d.y))), d.r || 3, 0, 2 * Math.PI); ctx.fill();
  }
  ctx.restore();
  for (const t of spec.ticks || []) {
    ctx.fillStyle = t.color; ctx.fillRect(X(t.e) - 1, bx.t, 2, 6);
  }
  if (!spec.spark) { ctx.strokeStyle = C.line; ctx.lineWidth = 1; ctx.strokeRect(bx.l + 0.5, bx.t + 0.5, w - bx.l - bx.r - 1, h - bx.t - bx.b - 1); }
  if (spec.empty) {
    ctx.fillStyle = C.muted; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText(spec.empty, (bx.l + w - bx.r) / 2, (bx.t + h - bx.b) / 2);
  }
  return off;
}
function shortNum(v) {
  const a = Math.abs(v);
  if (a === 0) return '0';
  if (a >= 1000) return v.toFixed(0);
  if (a >= 10) return v.toFixed(a >= 100 ? 0 : 1);
  if (a >= 1) return v.toFixed(2).replace(/0$/, '');
  return v.toPrecision(2);
}
function chartEl(parent, title, cls, keyItems) {
  const h3 = document.createElement('h3');
  h3.textContent = title;
  parent.appendChild(h3);
  const cv = document.createElement('canvas');
  cv.className = 'chart' + (cls ? ' ' + cls : '');
  parent.appendChild(cv);
  if (keyItems && keyItems.length) {
    const k = document.createElement('div');
    k.className = 'chart-key';
    k.innerHTML = keyItems.map(([c, t]) => '<span><i style="background:' + c + '"></i>' + esc(t) + '</span>').join('');
    parent.appendChild(k);
  }
  return cv;
}
function hint(parent, html) {
  const p = document.createElement('div');
  p.className = 'hint';
  p.innerHTML = html;
  parent.appendChild(p);
  return p;
}


// ============================================================================================ 9. legend and missing parts

const LEGEND = [
  ['Layers', 'The list on the left: Overview (key figures and what needs a look), Network links (the trees and the whole numbers), Receivers, Satellites, Cycle slips, Stochastic model, Atmosphere, Products and user check, Events. The badge beside a layer is its count.'],
  ['Going deeper', 'A click on a tile opens its layer; a click on a receiver or satellite (a table row, a map point, a matrix cell) opens its page at that time; on those pages a click on an arc or a sky-plot dot opens the line of sight (one receiver and one satellite). The path at the top leads back; so does the browser\'s back button.'],
  ['Time', 'One cursor for every view: the slider, the play button, the arrow keys (Shift: ten epochs) or a click on any chart. The lanes below the slider count the events of the run; a click on a lane jumps to its nearest event.'],
  ['Reference receiver', 'The receiver at the centre of a signal\'s tree. Its receiver phase bias is fixed; every other receiver and satellite of the tree is tied to it.'],
  ['Tree link', 'A receiver-satellite link that the tree uses to tie a node to the reference receiver. Its ambiguity is a whole number by definition (the chain pins it). Each signal has its own tree; the dashed links are tree links of the other signal of the pair only.'],
  ['Held link', 'A link that is not a tree link and whose whole numbers on both signals the network fixing holds (a tight prior in the network solve). It is released at a slip, a gap, a change of the tree around it, a re-opened float or a residual above 3 cm.'],
  ['Float link', 'A link whose ambiguity is estimated as a real number. On signals with fixing, orange links are floats the fixing has not (yet) held; the detail of a link says which test it failed.'],
  ['Hop', 'The number of tree links between the reference receiver and a node. Satellites sit at odd hops (1, 3, 5, ...), receivers at even hops. The rings of the picture are the hops of the chosen signal\'s tree.'],
  ['Ring of a link', 'The larger hop count of its two ends. The fixing works outward ring by ring.'],
  ['Wide lane', 'The difference of the two signals\' ambiguities, in cycles of the wide lane (86 cm for GPS L1/L2, 75 cm for Galileo E1/E5a). Its float reaches a whole number earlier than each signal\'s.'],
  ['Narrow lane', 'The ionosphere-free combination of the two ambiguities with the wide lane fixed, in narrow-lane cycles (about 11 cm). It is the quantity that is held last; its distance from a whole number and its sigma decide.'],
  ['Jump', 'A step of a satellite\'s emitted correction level between two epochs, after the step all satellites share is removed. Whole: both signals move by whole cycles (a re-pin). Ionosphere-like: the ionosphere-free level is unchanged. Fraction: the ionosphere-free level moves by a part of a cycle, which a user with a float solution feels.'],
  ['Node colour', 'The share of a node\'s links towards the reference receiver (to nodes of fewer hops) that are whole numbers: tree links or held links. Yellow: every path inward is made of whole numbers.'],
  ['Alarm', 'A residual check that flagged or re-opened something: the post-fit residual test, a re-opened carried float, a gross error on a line of sight, a satellite clock leaving its anchor.'],
  ['Code reference', 'Events of the code biases that define the clock\'s reference (a new code signal of a satellite, a re-seed, a move of the clock to another code pair).'],
  ['Post-fit residual', 'Observed minus computed after the epoch\'s network solve, per receiver, satellite and signal. Code residuals are decimetres to metres, phase residuals millimetres.'],
  ['Sigma0', 'The square root of the weighted sum of squared post-fit residuals over the redundancy: 1 when the weights match the scatter of the data, above 1 when the data scatter more than the weights assume.'],
  ['Variance factor', 'An estimate, per class of observation (constellation and signal), of how far the a priori variances are from the scatter of the residuals. A factor of 1 means the class is weighted right.'],
  ['Melbourne-Wubbena (MW)', 'The wide-lane combination of phase and code. It is free of geometry, clock and ionosphere, so a step in it is a slip of the wide lane.'],
  ['Geometry-free (GF)', 'The difference of the two phases in metres. It follows the ionosphere smoothly; a step in it is a slip of one signal.'],
  ['Wet delay (ZWD)', 'The zenith wet tropospheric delay estimated per receiver, as a correction to the a priori model.'],
];

function renderLegend() {
  document.getElementById('legend-list').innerHTML = LEGEND.map(([t, d]) => '<dt>' + esc(t) + '</dt><dd>' + esc(d) + '</dd>').join('');
}

/** Parts of the page whose trace is absent, per layer, with the file and the key that writes it. */
function missingParts(layer) {
  return (M.H.missing || []).filter(m => !layer || (m.layer || 'network') === layer);
}
function missingHtml(list) {
  return list.map(m => esc(m.part) + ' (<code>' + esc(m.file) + '</code>, key <code>' + esc(m.key) + '</code>)').join('; ');
}

/** The banner above the views: only what concerns every view (each view names its own missing parts). */
function renderMissing() {
  const box = document.getElementById('missing');
  const notes = [];
  if (!M.hasFixing && M.hasLinks) notes.push('No fixing in this run (<code>network.ring_fix.enable</code> off): every link that is not a tree link is a float.');
  if (!notes.length) return;
  box.innerHTML = notes.join('<br>');
  box.hidden = false;
}

// ============================================================================================ 10. blocks, views and the router

/*
 * The page's data is split into blocks: the core block (decoded at open) and one block per group of layers
 * (decoded the first time a view needs it, then kept).  A view is registered by its own file with
 *   registerView(name, {title, rail, blocks: [...], init(el), show(args, query), time(), resize(),
 *                        crumbs(args) -> [[label, path]], timelineEvents(e, lane)})
 * and reached by the address #/name/arg1/arg2?t=epoch&...; the browser's back button walks the views.
 */

const BLOCKS = {};
function blockEl(name) { return document.getElementById('nv-block-' + name); }
function hasBlock(name) { return name === 'core' || !!BLOCKS[name] || !!blockEl(name); }
function needBlock(name) {
  if (!BLOCKS[name]) {
    const el = blockEl(name);
    const t0 = performance.now();
    BLOCKS[name] = el ? decodeBlockEl(el).then(b => {
      undeltaBlock(b);
      BLOCKS['_' + name] = b;
      (window.NV_BLOCK_MS = window.NV_BLOCK_MS || {})[name] = Math.round(performance.now() - t0);
      return b;
    }) : Promise.resolve(null);
  }
  return BLOCKS[name];
}
/** A block already decoded (a view's blocks are awaited before it is shown), or null. */
function B(name) { return name === 'core' ? { H: M.H, A: M.A } : BLOCKS['_' + name] || null; }

const VIEWS = {};
/** The rail: the views a reader picks from, in order. Pages of one receiver, satellite or line of sight are reached from them. */
const RAIL = [
  ['overview', 'Overview'], ['network', 'Network links'], ['receivers', 'Receivers'], ['satellites', 'Satellites'],
  ['slips', 'Cycle slips'], ['stochastic', 'Stochastic model'], ['atmosphere', 'Atmosphere'],
  ['products', 'Products and user check'], ['events', 'Events'],
];
const R = { name: null, view: null, args: [], seq: 0, query: new URLSearchParams() };

function registerView(name, spec) { VIEWS[name] = Object.assign({ name, blocks: [], inited: false }, spec); }

function viewEl(name) {
  let el = document.getElementById('view-' + name);
  if (!el) {
    el = document.createElement('section');
    el.className = 'view';
    el.id = 'view-' + name;
    el.hidden = true;
    document.getElementById('views').appendChild(el);
  }
  return el;
}

function parseHash() {
  let h = location.hash.slice(1);
  if (!h.startsWith('/')) return { path: h ? ['network'] : [], q: new URLSearchParams(h) };   // addresses of the single view
  const qi = h.indexOf('?');
  const path = (qi >= 0 ? h.slice(1, qi) : h.slice(1)).split('/').filter(Boolean).map(decodeURIComponent);
  return { path, q: new URLSearchParams(qi >= 0 ? h.slice(qi + 1) : '') };
}

/** The query kept across views: time, signals, tree, layout (the network view adds its selection). */
function baseQuery(extra) {
  const p = new URLSearchParams();
  p.set('t', S.ep); p.set('f', S.fam); p.set('b', S.bandSel);
  if (S.layout !== 'rings') p.set('l', S.layout);
  for (const [k, v] of Object.entries(extra || {})) if (v != null && v !== '') p.set(k, v);
  return p;
}
function hrefOf(path, extra) { return '#/' + path.map(encodeURIComponent).join('/') + '?' + baseQuery(extra).toString(); }
function go(path, extra) { location.hash = hrefOf(path, extra); }
function openNode(n) {
  if (n == null || n < 0) return;
  go(n < M.nSta ? ['receiver', M.names[n]] : ['satellite', M.names[n]]);
}
function openLink(sta, sat) { go(['los', M.names[sta], M.H.sats[sat]]); }
function linkHtml(path, label, cls) { return '<a class="act' + (cls ? ' ' + cls : '') + '" href="' + hrefOf(path) + '">' + esc(label) + '</a>'; }
function staLink(j) { return j >= 0 && j < M.nSta ? linkHtml(['receiver', M.names[j]], M.names[j]) : ''; }
function satLink(k) { return k >= 0 && k < M.nSat ? linkHtml(['satellite', M.H.sats[k]], M.H.sats[k]) : ''; }
function losLink(j, k, label) { return linkHtml(['los', M.names[j], M.H.sats[k]], label || (shortSta(M.names[j]) + ' - ' + M.H.sats[k])); }

function loadQuery(q) {
  if (q.has('f') && +q.get('f') < M.nFam) S.fam = +q.get('f');
  if (q.has('b')) S.bandSel = +q.get('b') ? 1 : 0;
  if (q.has('t')) S.ep = Math.max(0, Math.min(M.nEp - 1, +q.get('t')));
  S.layout = q.get('l') === 'map' ? 'map' : 'rings';
}

async function route() {
  const { path, q } = parseHash();
  const name = VIEWS[path[0]] ? path[0] : 'overview';
  const view = VIEWS[name], args = path.slice(1), seq = ++R.seq;
  const prevEp = S.ep;
  loadQuery(q);
  R.query = q;
  for (const el of document.querySelectorAll('#views > .view')) el.hidden = true;
  const el = viewEl(name);
  el.hidden = false;
  R.name = name; R.view = view; R.args = args;
  renderRail();
  renderCrumbs(view, args);
  const missingBlocks = view.blocks.filter(b => !hasBlock(b));
  if (view.blocks.some(b => !BLOCKS['_' + b] && hasBlock(b))) {
    el.insertAdjacentHTML('afterbegin', '<div class="opening">Opening this part of the run&hellip;</div>');
  }
  await Promise.all(view.blocks.map(needBlock));
  if (seq !== R.seq) return;                                   // another view was asked for meanwhile
  const op = el.querySelector(':scope > .opening');
  if (op) op.remove();
  try {
    if (!view.inited) { view.init(el, missingBlocks); view.inited = true; }
    S.sel = view.selection ? view.selection(args, q) : null;
    TL.cache = null;
    view.show(args, q);
  } catch (err) {
    el.insertAdjacentHTML('afterbegin', '<div class="error">This view failed: ' + esc(err.message) + '</div>');
    console.error(err);
  }
  document.getElementById('slider').value = S.ep;
  updateTimeLabel();
  drawTimeline();
  drawAllCharts();
  if (prevEp !== S.ep && view.time) view.time();
  broadcast();
}

function renderRail() {
  const rail = document.getElementById('rail');
  rail.innerHTML = RAIL.map(([name, label]) => {
    const v = VIEWS[name];
    if (!v) return '';
    const on = R.name === name || (v.children || []).includes(R.name);
    const badge = v.badge ? v.badge() : '';
    return '<a class="rail-item' + (on ? ' on' : '') + '" href="' + hrefOf([name]) + '"><span>' + esc(label) + '</span>' +
      (badge ? '<span class="badge">' + esc(badge) + '</span>' : '') + '</a>';
  }).join('');
}

function renderCrumbs(view, args) {
  const items = [['Overview', ['overview']]];
  if (view.crumbs) items.push(...view.crumbs(args));
  else if (view.name !== 'overview') items.push([view.title || view.name, [view.name]]);
  document.getElementById('crumbs').innerHTML = items.map(([label, path], i) =>
    i === items.length - 1 ? '<b>' + esc(label) + '</b>' : '<a class="act" href="' + hrefOf(path) + '">' + esc(label) + '</a>').join(' <span class="sep">&rsaquo;</span> ');
}

function viewResized() { if (R.view && R.view.resize) R.view.resize(); }

// ============================================================================================ 11. interaction and start

function setEp(e) {
  e = Math.max(0, Math.min(M.nEp - 1, e | 0));
  if (e === S.ep) return;
  S.ep = e;
  document.getElementById('slider').value = e;
  refreshTime();
}
function refreshTime() {
  updateTimeLabel();
  drawTimeline();
  if (R.view && R.view.time) R.view.time();
  drawAllCharts();
  saveHash();
}

function play() {
  S.playing = !S.playing;
  document.getElementById('play').innerHTML = S.playing ? '&#10074;&#10074;' : '&#9654;';
  if (!S.playing) return;
  let last = performance.now(), acc = 0;
  const tick = now => {
    if (!S.playing) return;
    acc += (now - last) / 1000 * S.speed;
    last = now;
    if (acc >= 1) {
      const n = Math.floor(acc);
      acc -= n;
      if (S.ep + n >= M.nEp - 1) { setEp(M.nEp - 1); play(); return; }
      setEp(S.ep + n);
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

/** Keep the address current without adding a step to the browser's history (time moves, selections change). */
function saveHash() {
  const extra = R.view && R.view.query ? R.view.query() : {};
  const h = '#/' + [R.name || 'overview'].concat(R.args).map(encodeURIComponent).join('/') + '?' + baseQuery(extra).toString();
  try { history.replaceState(null, '', h); } catch (_e) { /* file:// may refuse; harmless */ }
  broadcast();
}

// --- two pages side by side (netview.py compare): each page tells its frame what it shows, the frame tells the other

const SYNC = { inFrame: window.parent !== window, applying: false };
function broadcast() {
  if (!SYNC.inFrame || SYNC.applying || !M.H) return;
  parent.postMessage({ nv: 'sync', ms: absMs(S.ep), path: [R.name || 'overview'].concat(R.args),
    fam: M.H.families[S.fam] && M.H.families[S.fam].key, band: S.bandSel, layout: S.layout,
    extra: R.view && R.view.query ? R.view.query() : {} }, '*');
}
function applySync(d) {
  SYNC.applying = true;
  try {
    const fi = M.H.families.findIndex(f => f.key === d.fam);
    if (fi >= 0) S.fam = fi;
    if (d.band != null) S.bandSel = d.band;
    if (d.layout) S.layout = d.layout;
    const e = epochAtMs(d.ms);
    if (e >= 0) S.ep = e;
    const want = hrefOf(d.path || ['overview'], d.extra);
    if (want !== location.hash) location.hash = want;          // route() runs on the hash change
    else refreshTime();
  } finally { SYNC.applying = false; }
}
window.addEventListener('message', ev => { if (ev.data && ev.data.nv === 'sync' && M.H) applySync(ev.data); });

function wireCommon() {
  const slider = document.getElementById('slider');
  slider.max = M.nEp - 1;
  slider.addEventListener('input', () => setEp(+slider.value));
  document.getElementById('play').addEventListener('click', play);
  document.getElementById('speed').addEventListener('change', ev => { S.speed = +ev.target.value; });
  const dl = document.getElementById('names');
  dl.innerHTML = M.names.map(n => '<option value="' + esc(n) + '">').join('');
  const search = document.getElementById('search');
  search.addEventListener('change', () => {
    const v = search.value.trim().toUpperCase();
    let n = M.nodeIndex.get(v);
    if (n == null) n = M.names.findIndex(x => x.toUpperCase().startsWith(v));
    if (n == null || n < 0) return;
    if (R.name === 'network') selectNode(n); else openNode(n);
    search.value = '';
  });
  const sm = document.getElementById('seqmap');
  sm.value = S.seqMap;
  sm.addEventListener('change', () => {
    S.seqMap = sm.value;
    saveSeqMap();
    for (const c of CHARTS) c.img = null;
    if (R.view && R.view.recolor) R.view.recolor(); else if (R.view) R.view.show(R.args, R.query);
    drawAllCharts();
  });
  document.getElementById('legend-btn').addEventListener('click', () => { document.getElementById('legend').hidden = false; });
  document.getElementById('legend-close').addEventListener('click', () => { document.getElementById('legend').hidden = true; });
  document.getElementById('legend').addEventListener('click', ev => { if (ev.target.id === 'legend') ev.target.hidden = true; });
  const tl = document.getElementById('timeline');
  tl.addEventListener('click', ev => {
    const r = tl.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top;
    const lanes = timelineLanes(), k = Math.floor((y - TL.top) / TL.laneH);
    let e = tlEp(x, r.width);
    if (k >= 0 && k < lanes.length && x >= TL.labelW) {
      const c = lanes[k].counts, span = Math.max(2, Math.round(6 / (r.width - TL.labelW) * M.nEp));
      let best = -1;
      for (let d = 0; d <= span; d++) {
        if (e - d >= 0 && c[e - d]) { best = e - d; break; }
        if (e + d < M.nEp && c[e + d]) { best = e + d; break; }
      }
      if (best >= 0) {
        setEp(best);
        if (!lanes[k].selected) {
          if (R.view && R.view.timelineEvents) R.view.timelineEvents(best, lanes[k].lane);
          else go(['events'], { e: best, lane: lanes[k].lane });
        }
        return;
      }
    }
    setEp(e);
  });
  tl.addEventListener('mousemove', ev => {
    const r = tl.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top;
    const lanes = timelineLanes(), k = Math.floor((y - TL.top) / TL.laneH), e = tlEp(x, r.width);
    tl.title = clockOf(e) + (k >= 0 && k < lanes.length ? '  ' + lanes[k].label + ': ' + lanes[k].counts[e] + ' at this epoch' : '');
  });
  document.addEventListener('keydown', ev => {
    if (ev.target.tagName === 'INPUT' && ev.target.type !== 'range' || ev.target.tagName === 'SELECT') return;
    if (ev.key === 'ArrowRight') { setEp(S.ep + (ev.shiftKey ? 10 : 1)); ev.preventDefault(); }
    else if (ev.key === 'ArrowLeft') { setEp(S.ep - (ev.shiftKey ? 10 : 1)); ev.preventDefault(); }
    else if (ev.key === ' ') { play(); ev.preventDefault(); }
    else if (ev.key === 'Home') setEp(0);
    else if (ev.key === 'End') setEp(M.nEp - 1);
    else if (ev.key === 'Escape') {
      if (!document.getElementById('legend').hidden) document.getElementById('legend').hidden = true;
      else if (R.view && R.view.escape) R.view.escape();
    }
  });
  let rt = null;
  window.addEventListener('resize', () => {
    clearTimeout(rt);
    rt = setTimeout(() => { TL.cache = null; for (const c of CHARTS) c.img = null; viewResized(); drawTimeline(); drawAllCharts(); }, 120);
  });
  const mq = window.matchMedia('(prefers-color-scheme: dark)');
  if (mq.addEventListener) mq.addEventListener('change', () => {
    readColors(); TL.cache = null; for (const c of CHARTS) c.img = null;
    for (const v of Object.values(VIEWS)) if (v.inited && v.recolor) v.recolor();
    drawTimeline(); drawAllCharts();
  });
}

async function start() {
  const t0 = performance.now();
  const { H, A } = await decodeBlockEl(document.getElementById('nv-data'));
  buildModel(H, A);
  BLOCKS.core = Promise.resolve({ H, A });
  readColors();
  loadSeqMap();
  S.fam = defaultFamily();
  loadQuery(parseHash().q);
  document.getElementById('run-name').textContent = 'Network dashboard: ' + H.run;
  const d = gpsDate(0);
  document.getElementById('run-sub').textContent = d.toISOString().slice(0, 10) + ' (day ' + dayOfYear(d) + '), ' +
    M.nSta + ' receivers, ' + M.nSat + ' satellites, ' + durationText(M.nEp) + '; built ' + H.built;
  document.getElementById('loading').hidden = true;
  document.getElementById('app').hidden = false;
  wireCommon();
  renderLegend();
  renderMissing();
  document.getElementById('slider').value = S.ep;
  window.addEventListener('hashchange', () => { route(); });
  await route();
  window.NV = { M, S, G, R, B, VIEWS };       // for the comparison page and for checks from the console
  window.NV_OPEN_MS = Math.round(performance.now() - t0);
  console.log('network dashboard: first view ready ' + Math.round(performance.now()) + ' ms after the page began loading (' +
    window.NV_OPEN_MS + ' ms of it decoding and drawing); blocks decoded (ms): ' + JSON.stringify(window.NV_BLOCK_MS || {}));
}
