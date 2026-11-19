'use strict';
/*
 * Network dashboard: the receivers.
 *   #/receivers        every receiver: key figures, a world map and a table, and receivers x time for one metric
 *   #/receiver/NAME    one receiver: key figures, sky plot, arcs of its satellites with their events, its series
 *                      (post-fit residuals, satellites, wet delay, clock, code biases, coordinate test, residual test)
 * The per-receiver quantities come from the receivers block (scripts/netview/layers/receivers.py), the wet delay and
 * clock from the core block, the azimuths of the sky plot from the line-of-sight block.
 */

// ============================================================================================ derived per receiver

const SLIP_TIERS = new Set(['T0_mw_detect', 'T1_gf_jump', 'T1_if_slip']);
const QUIET_ACTIONS = new Set(['diagnostic', 'coincident']);

/** Event classes the layers count: a slip detection, a quality-test flag, an alarm (a check that acted). */
function evClass(i) {
  const A = M.A, E = M.H.events;
  const tier = E.tiers[A.ev_tier[i]];
  if (SLIP_TIERS.has(tier)) return QUIET_ACTIONS.has(E.actions[A.ev_action[i]]) ? '' : 'slip';
  if (M.H.lanes[A.ev_lane[i]].key === 'alarm') return 'alarm';
  const fam = E.families && E.families[E.tier_family[A.ev_tier[i]]];
  return fam && fam[0] === 'qc' ? 'qc' : '';
}

/** Counts per receiver and epoch of slips, quality flags and alarms; held and float links per receiver. Cached. */
function rxDerived() {
  if (M.rxd) return M.rxd;
  const ns = M.nSta, ne = M.nEp, A = M.A;
  const d = { slip: new Float32Array(ns * ne), qc: new Float32Array(ns * ne), alarm: new Float32Array(ns * ne),
    held: new Float32Array(ns * ne), links: new Float32Array(ns * ne), tot: { slip: new Int32Array(ns), qc: new Int32Array(ns), alarm: new Int32Array(ns) } };
  for (let i = 0; i < M.nEv; i++) {
    const j = A.ev_sta[i];
    if (j < 0) continue;
    const c = evClass(i);
    if (!c) continue;
    d[c][j * ne + A.ev_ep[i]]++;
    d.tot[c][j]++;
  }
  if (M.hasLinks) {
    for (let l = 0; l < M.nLinks; l++) {
      if (!M.H.families[A.link_fam[l]].fixing) continue;
      const j = A.link_sta[l], s = A.link_start[l], n = A.link_count[l];
      for (let r = s; r < s + n; r++) {
        const o = j * ne + A.ce_ep[r];
        d.links[o]++;
        if (A.ce_state[r] === ST.held) d.held[o]++;
      }
    }
  }
  d.share = new Float32Array(ns * ne);
  for (let i = 0; i < ns * ne; i++) d.share[i] = d.links[i] ? d.held[i] / d.links[i] : NaN;
  M.rxd = d;
  return d;
}

/**
 * The receiver metrics a reader can colour a map or a matrix with: {key, label, kind (scale), row(j) -> series
 * over epochs, fmt(v), worse: +1 when larger is worse, -1 when smaller is worse, 0 for signed values}.
 */
function rxMetrics() {
  const RB = B('receivers'), A = RB ? RB.A : {}, d = rxDerived(), ne = M.nEp;
  const out = [];
  const row = (a, j) => a.subarray(j * ne, (j + 1) * ne);
  if (A.rp_c_rms) out.push({ key: 'code', label: 'post-fit code RMS', kind: 'viridis', row: j => row(A.rp_c_rms, j), fmt: v => fmtLen(v), worse: 1 });
  if (A.rp_p_rms) out.push({ key: 'phase', label: 'post-fit phase RMS', kind: 'viridis', row: j => row(A.rp_p_rms, j), fmt: v => fmtLen(v), worse: 1 });
  if (A.pk_c_s0) out.push({ key: 'zcode', label: 'normalized code residual scale', kind: 'viridis', row: j => row(A.pk_c_s0, j), fmt: v => fmt(v, 2), worse: 1 });
  if (A.rp_nsat) out.push({ key: 'nsat', label: 'satellites with residuals', kind: 'viridis', row: j => Float32Array.from(row(A.rp_nsat, j), v => v || NaN), fmt: v => fmt(v, 0), worse: -1 });
  if (A.sr_max) out.push({ key: 'raim', label: 'largest normalized residual (residual test)', kind: 'viridis', row: j => row(A.sr_max, j), fmt: v => fmt(v, 1), worse: 1 });
  out.push({ key: 'slip', label: 'cycle slips detected', kind: 'count', row: j => row(d.slip, j), fmt: v => fmt(v, 0), worse: 1, sparse: true });
  out.push({ key: 'qc', label: 'quality-test flags', kind: 'count', row: j => row(d.qc, j), fmt: v => fmt(v, 0), worse: 1, sparse: true });
  if (M.hasFixing) out.push({ key: 'held', label: 'share of its links held at whole numbers', kind: 'viridis', row: j => row(d.share, j), fmt: v => fmtPct(v), worse: -1, range: [0, 1] });
  if (M.A.rx_zwd) out.push({ key: 'zwd', label: 'wet delay (correction to the model)', kind: 'vik', row: j => row(M.A.rx_zwd, j), fmt: v => fmtLen(v), worse: 0 });
  if (M.A.rx_clk_sd) out.push({ key: 'clksd', label: 'receiver clock sigma', kind: 'viridis', row: j => row(M.A.rx_clk_sd, j), fmt: v => fmtLen(v), worse: 1 });
  return out;
}

/** A metric's colour scale over every receiver (robust range; vik symmetric about zero). */
function metricScale(metric, nRows, rowFn) {
  if (metric.range) return { kind: metric.kind, lo: metric.range[0], hi: metric.range[1], label: metric.label };
  const all = new Float32Array(nRows * M.nEp);
  for (let r = 0; r < nRows; r++) all.set(rowFn(r), r * M.nEp);
  if (metric.kind === 'count') { let mx = 0; for (const v of all) if (v > mx) mx = v; return { kind: 'count', lo: 0, hi: Math.max(1, mx), label: metric.label }; }
  if (metric.kind === 'vik') return signedScale(all, metric.label);
  const [lo, hi] = robustSpan(all, false);
  return { kind: metric.kind, lo, hi, label: metric.label };
}

/** Rows ordered for a matrix: worst first by the metric's run median (or total for counts), or by name. */
function orderRows(n, rowFn, metric, byName, names) {
  const idx = Array.from({ length: n }, (_x, i) => i);
  if (byName || !metric.worse) return idx.sort((a, b) => names[a].localeCompare(names[b]));
  const score = idx.map(i => metric.sparse ? sumOf(rowFn(i)) : medianOf(rowFn(i)));
  return idx.sort((a, b) => {
    const x = score[a], y = score[b];
    if (!isFinite(x)) return 1;
    if (!isFinite(y)) return -1;
    return metric.worse > 0 ? y - x : x - y;
  });
}

// ============================================================================================ #/receivers

const RXV = { metric: 'code', byName: false };

function renderReceivers(root) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const metrics = rxMetrics();
  if (!metrics.find(m => m.key === RXV.metric)) RXV.metric = metrics[0] ? metrics[0].key : '';
  const d = rxDerived(), RB = B('receivers'), A = RB ? RB.A : {};
  viewHead(pad, 'Receivers', M.nSta + ' receivers; click a row, a point or a cell to open its page');
  missingNote(pad, 'receivers');
  const now = S.ep, ne = M.nEp;
  const colNow = a => { const v = []; for (let j = 0; j < M.nSta; j++) v.push(a ? a[j * ne + now] : NaN); return v; };
  const es = B('epoch') && B('epoch').A.es, esf = B('epoch') && B('epoch').H.es_fields;
  const nPresent = es ? es[esf.findIndex(f => f[0] === 'n_present') * ne + now] : NaN;
  tiles(pad, [
    { k: 'Receivers in the solution now', v: isFinite(nPresent) ? nPresent + ' <small>of ' + M.nSta + '</small>' : 'n/a', cls: 'info' },
    { k: 'Median post-fit code RMS now', v: fmtLen(medianOf(colNow(A.rp_c_rms))), cls: 'info' },
    { k: 'Median post-fit phase RMS now', v: fmtLen(medianOf(colNow(A.rp_p_rms))), cls: 'info' },
    { k: 'Slips detected over the run', v: fmtCount(sumOf(d.tot.slip)), s: 'on ' + d.tot.slip.filter(x => x > 0).length + ' receivers', cls: 'info', href: hrefOf(['slips']) },
    { k: 'Quality-test flags over the run', v: fmtCount(sumOf(d.tot.qc)), s: 'outlier, detectability and coordinate tests', cls: 'info', href: hrefOf(['stochastic']) },
  ]);
  const ctl = el('div', 'row');
  ctl.innerHTML = '<label>Colour by</label><span class="seg">' + metrics.map(m => '<button data-m="' + m.key + '" class="' + (m.key === RXV.metric ? 'on' : '') + '">' + esc(m.label) + '</button>').join('') + '</span>';
  pad.appendChild(ctl);
  ctl.querySelectorAll('button').forEach(b => b.addEventListener('click', () => { RXV.metric = b.dataset.m; renderReceivers(root); }));
  const metric = metrics.find(m => m.key === RXV.metric);
  const g = el('div', 'grid2');
  pad.appendChild(g);
  const pm = panel(g, 'Where they are', 'colour: ' + (metric ? metric.label : '') + ' at the cursor');
  let sc = null;
  if (metric) {
    sc = metricScale(metric, M.nSta, metric.row);
    const map = worldMap(pm, {
      height: 330, scale: sc,
      points: () => M.H.stations.map((s, j) => ({ label: s.name, lat: s.lat, lon: s.lon, value: metric.row(j)[S.ep], ring: d.alarm[j * ne + S.ep] > 0 })),
      text: j => esc(metric.label) + ': ' + esc(metric.fmt(metric.row(j)[S.ep])), onClick: j => openNode(j),
    });
    RXV.map = map;
    scaleKey(pm, sc, metric.fmt);
  }
  const pt = panel(g, 'Table', 'values at the cursor and over the run; click a header to sort');
  const hopsBand = curFamily() ? curFamily().b : -1;
  const rows = M.H.stations.map((s, j) => {
    const r = { name: s.name, role: s.role, j };
    r.hops = hopsBand >= 0 ? treeDepth(hopsBand, j, now) : NaN;
    if (r.hops < 0) r.hops = NaN;
    r.nsat = A.rp_nsat ? A.rp_nsat[j * ne + now] : NaN;
    r.code = A.rp_c_rms ? A.rp_c_rms[j * ne + now] : NaN;
    r.codeMed = A.rp_c_rms ? medianOf(rowOf(A.rp_c_rms, j)) : NaN;
    r.phaseMed = A.rp_p_rms ? medianOf(rowOf(A.rp_p_rms, j)) : NaN;
    r.slips = d.tot.slip[j]; r.qc = d.tot.qc[j]; r.alarms = d.tot.alarm[j];
    r.held = d.share[j * ne + now];
    r.zwd = M.A.rx_zwd ? M.A.rx_zwd[j * ne + now] : NaN;
    if (A.cg) {
      let mx = NaN;
      const nf = B('receivers').H.cg_fields.length;
      for (let e = 0; e < ne; e++) {
        const de = A.cg[(0 * M.nSta + j) * ne + e], dn = A.cg[(1 * M.nSta + j) * ne + e], du = A.cg[(2 * M.nSta + j) * ne + e];
        if (isFinite(de)) { const h = Math.hypot(de, dn, du); if (!(h <= mx)) mx = h; }
      }
      r.coord = nf ? mx : NaN;
    }
    return r;
  });
  const colScale = (key, kind) => {
    const v = rows.map(r => r[key]).filter(isFinite);
    if (!v.length) return null;
    const s = { kind: kind || 'viridis', lo: Math.min(...v), hi: Math.max(...v) };
    return x => (isFinite(x) ? scaleColor(s, x) : null);
  };
  RXV.table = dataTable(pt, {
    maxHeight: 360, sort: 'codeMed', desc: true,
    cols: [
      { key: 'name', label: 'Receiver', fmt: (v, r) => staLink(r.j) },
      { key: 'hops', label: 'Hops', num: true, title: 'hops from the reference receiver on the tree of the chosen signal', fmt: v => isFinite(v) ? v : '' },
      { key: 'nsat', label: 'Satellites', num: true, title: 'satellites with post-fit residuals at the cursor', fmt: v => isFinite(v) ? v : '' },
      { key: 'code', label: 'Code RMS', num: true, title: 'post-fit code residual RMS at the cursor', fmt: v => fmtLen(v) },
      { key: 'codeMed', label: 'Code RMS (run)', num: true, title: 'median over the run', fmt: v => fmtLen(v), color: colScale('codeMed') },
      { key: 'phaseMed', label: 'Phase RMS (run)', num: true, fmt: v => fmtLen(v), color: colScale('phaseMed') },
      { key: 'slips', label: 'Slips', num: true, title: 'slips detected over the run', color: colScale('slips') },
      { key: 'qc', label: 'Test flags', num: true, title: 'quality-test flags over the run' },
      { key: 'held', label: 'Held now', num: true, title: 'share of its links held at whole numbers at the cursor', fmt: v => fmtPct(v) },
      { key: 'zwd', label: 'Wet delay', num: true, fmt: v => fmtLen(v) },
      { key: 'coord', label: 'Coordinate offset', num: true, title: 'largest 3D offset of the held coordinate found by the coordinate test', fmt: v => fmtLen(v) },
    ].filter(c => rows.some(r => r[c.key] != null && (typeof r[c.key] !== 'number' || isFinite(r[c.key])))),
    rows, onRow: r => openNode(r.j),
  });
  if (metric) {
    const pmx = panel(pad, 'Receivers over time', esc(metric.label) + (RXV.byName ? ', by name' : ', worst first'));
    const ord = el('div', 'row', '<label><input type="checkbox" id="rx-byname"' + (RXV.byName ? ' checked' : '') + '> order by name</label>');
    pmx.appendChild(ord);
    ord.querySelector('input').addEventListener('change', ev => { RXV.byName = ev.target.checked; renderReceivers(root); });
    const names = M.H.stations.map(s => s.name);
    const order = orderRows(M.nSta, metric.row, metric, RXV.byName, names);
    matrix(pmx, {
      rows: order.map(j => names[j]), scale: sc, value: (r, e) => metric.row(order[r])[e],
      text: (r, e, v) => esc(metric.label) + ': ' + esc(metric.fmt(v)), onClick: (r, e) => { S.ep = e; openNode(order[r]); },
    });
    scaleKey(pmx, sc, metric.fmt);
  }
}

registerView('receivers', {
  title: 'Receivers', blocks: ['receivers', 'epoch'], children: ['receiver'],
  init() {},
  show() { renderReceivers(viewEl('receivers')); },
  time() {
    if (RXV.map) RXV.map.draw();
  },
  badge() { return M.nSta + ''; },
});

// ============================================================================================ #/receiver/NAME

function renderReceiver(root, j) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const name = M.names[j], st = M.H.stations[j], ne = M.nEp;
  const RB = B('receivers'), A = RB ? RB.A : {}, d = rxDerived();
  viewHead(pad, name, esc(st.role || 'receiver') + (isFinite(st.lat) ? ', ' + fmt(st.lat, 2) + ' N ' + fmt(st.lon, 2) + ' E' : '') +
    ' &middot; ' + linkHtml(['network'], 'see it in the network', '') + ' &middot; <a class="act" href="' + hrefOf(['network'], { s: 'node:' + name }) + '">its tree position</a>');
  const at = a => (a ? a[j * ne + S.ep] : NaN);
  const cf = curFamily();
  const box = el('div');
  pad.appendChild(box);
  const nowTiles = () => {
    const hops = cf ? treeDepth(cf.b, j, S.ep) : -1;
    box.innerHTML = '';
    tiles(box, [
      { k: 'Satellites with residuals', v: isFinite(at(A.rp_nsat)) ? String(at(A.rp_nsat)) : 'n/a', cls: 'info' },
      { k: 'Post-fit code RMS', v: fmtLen(at(A.rp_c_rms)), s: 'run median ' + fmtLen(A.rp_c_rms ? medianOf(rowOf(A.rp_c_rms, j)) : NaN), cls: 'info' },
      { k: 'Post-fit phase RMS', v: fmtLen(at(A.rp_p_rms)), s: 'run median ' + fmtLen(A.rp_p_rms ? medianOf(rowOf(A.rp_p_rms, j)) : NaN), cls: 'info' },
      { k: 'Wet delay', v: fmtLen(at(M.A.rx_zwd)), s: 'sigma ' + fmtLen(at(M.A.rx_zwd_sd)), cls: 'info' },
      { k: 'Hops on ' + (cf ? cf.sig : 'the tree'), v: hops >= 0 ? String(hops) : 'not on it', cls: 'info', href: hrefOf(['network'], { s: 'node:' + name }) },
      { k: 'Links held now', v: fmtPct(d.share[j * ne + S.ep]), s: d.held[j * ne + S.ep] + ' of ' + d.links[j * ne + S.ep] + ' links', cls: 'info' },
      { k: 'Slips over the run', v: String(d.tot.slip[j]), cls: d.tot.slip[j] ? 'warn' : 'ok' },
      { k: 'Quality-test flags', v: String(d.tot.qc[j]), s: 'alarms ' + d.tot.alarm[j], cls: d.tot.alarm[j] ? 'bad' : 'ok' },
    ]);
  };
  nowTiles();
  RV.nowTiles = nowTiles;
  // satellites of this receiver: sky plot and arcs
  const L = B('los');
  const sats = [];
  for (let k = 0; k < M.nSat; k++) {
    const o = (j * M.nSat + k) * ne;
    let any = false;
    for (let e = 0; e < ne && !any; e++) any = M.A.los_el[o + e] !== 255;
    if (any) sats.push(k);
  }
  const g = el('div', 'grid2');
  pad.appendChild(g);
  const ps = panel(g, 'Sky plot', 'tracks over the run, dots at the cursor; click a satellite to open the line of sight');
  const losIndex = L ? losIndexMap() : null;
  const tracks = sats.map(k => {
    const el_ = new Float32Array(ne), az = new Float32Array(ne).fill(NaN);
    const o = (j * M.nSat + k) * ne;
    for (let e = 0; e < ne; e++) { const v = M.A.los_el[o + e]; el_[e] = v === 255 ? NaN : v; }
    const li = losIndex ? losIndex.get(j * M.nSat + k) : undefined;
    if (li != null && L.A.laz) for (let e = 0; e < ne; e++) { const v = L.A.laz[li * ne + e]; az[e] = v === 255 ? NaN : v * 360 / 254; }
    return { label: M.H.sats[k], az, el: el_, color: sysColor(M.H.sats[k][0]) };
  });
  if (L && L.A.laz) {
    skyPlot(ps, { tracks, onClick: i => openLink(j, sats[i]) });
    keyRow(ps, [...new Set(sats.map(k => M.H.sats[k][0]))].map(c => [sysColor(c), sysName(c)]));
  } else hint(ps, 'Azimuths need the line-of-sight block (network.trace_ionosphere).');
  const pa = panel(g, 'Arcs of its satellites', 'colour: elevation; marks: slips, gaps and returns, holds, releases');
  const evBy = new Map();
  for (const i of M.evBySta[j]) {
    const k = M.A.ev_sat[i];
    if (k < 0) continue;
    if (!evBy.has(k)) evBy.set(k, []);
    evBy.get(k).push(i);
  }
  const markColor = i => {
    const c = evClass(i), lane = M.H.lanes[M.A.ev_lane[i]].key;
    if (c === 'slip') return C.slip;
    if (lane === 'gap') return C.gap;
    if (lane === 'hold') return C.held;
    if (lane === 'release') return C.release;
    if (c === 'alarm') return C.alarm;
    return null;
  };
  const elScale = { kind: 'viridis', lo: 0, hi: 90 };
  arcChart(pa, {
    rows: sats.map(k => ({
      label: M.H.sats[k],
      present: e => M.A.los_el[(j * M.nSat + k) * ne + e] !== 255,
      color: e => scaleColor(elScale, M.A.los_el[(j * M.nSat + k) * ne + e]),
      marks: (evBy.get(k) || []).map(i => ({ e: M.A.ev_ep[i], color: markColor(i) })).filter(m => m.color),
    })),
    rowH: sats.length > 30 ? 9 : 11,
    text: (r, e) => 'elevation ' + (M.A.los_el[(j * M.nSat + sats[r]) * ne + e] === 255 ? 'not tracked' : M.A.los_el[(j * M.nSat + sats[r]) * ne + e] + ' degrees') +
      (evBy.get(sats[r]) || []).filter(i => M.A.ev_ep[i] === e).map(i => '<br>' + esc(eventText(i))).join(''),
    onClick: (r, e) => { S.ep = e; openLink(j, sats[r]); },
  });
  keyRow(pa, [[C.slip, 'slip'], [C.gap, 'gap or return'], [C.held, 'held'], [C.release, 'released'], [C.alarm, 'alarm']]);
  // series
  const g2 = el('div', 'grid2');
  pad.appendChild(g2);
  const series = (title, spec, keys) => { const p = panel(g2, title); makeChart(canvasIn(p), spec); if (keys) keyRow(p, keys); return p; };
  if (A.rp_c_rms) series('Post-fit code residuals (m)', { series: [{ y: rowOf(A.rp_c_rms, j), color: C.held, width: 1.4 }, { y: rowOf(A.rp_c_max, j), color: C.float }], zeroBased: true, robust: true,
    valueAt: e => 'RMS ' + fmtLen(A.rp_c_rms[j * ne + e]) + ', largest ' + fmtLen(A.rp_c_max[j * ne + e]) }, [[C.held, 'RMS'], [C.float, 'largest']]);
  if (A.rp_p_rms) series('Post-fit phase residuals (mm)', { series: [{ y: scaled(rowOf(A.rp_p_rms, j), 1000), color: C.held, width: 1.4 }, { y: scaled(rowOf(A.rp_p_max, j), 1000), color: C.float }], zeroBased: true, robust: true,
    valueAt: e => 'RMS ' + fmtLen(A.rp_p_rms[j * ne + e]) + ', largest ' + fmtLen(A.rp_p_max[j * ne + e]) }, [[C.held, 'RMS'], [C.float, 'largest']]);
  if (A.pk_c_s0) series('Normalized residual scale (1 = as weighted)', { series: [{ y: rowOf(A.pk_c_s0, j), color: C.held, width: 1.4 }, { y: rowOf(A.pk_p_s0, j), color: C.coderef }], zeroBased: true, robust: true,
    hlines: [{ y: 1, color: C.muted, inRange: true }], valueAt: e => 'code ' + fmt(A.pk_c_s0[j * ne + e], 2) + ', phase ' + fmt(A.pk_p_s0[j * ne + e], 2) }, [[C.held, 'code'], [C.coderef, 'phase']]);
  if (A.rp_nsat) series('Satellites with residuals', { series: [{ y: Float32Array.from(rowOf(A.rp_nsat, j), v => v || NaN), color: C.ink, step: true }], zeroBased: true, valueAt: e => A.rp_nsat[j * ne + e] + ' satellites' });
  if (M.A.rx_zwd) {
    const z = rowOf(M.A.rx_zwd, j), zs = rowOf(M.A.rx_zwd_sd, j);
    series('Wet delay, correction to the model (m), with its sigma', { series: [{ y: z, color: C.iono, width: 1.5 }], bands: [{ lo: z.map((v, e) => v - zs[e]), hi: z.map((v, e) => v + zs[e]), color: withAlpha(C.iono, 0.18) }], robust: true,
      valueAt: e => fmtLen(z[e]) + ' +- ' + fmtLen(zs[e]) });
  }
  if (M.A.rx_clk) series('Receiver clock (m)', { series: [{ y: rowOf(M.A.rx_clk, j), color: C.ink }], robust: true, valueAt: e => fmt(M.A.rx_clk[j * ne + e], 3) + ' m' });
  if (A.rxs) {
    const keys = RB.H.rxs_keys;
    const cols = keys.map((_k, i) => rampColor(window.NV_ASSETS.colormaps.viridis, keys.length > 1 ? 0.85 * i / (keys.length - 1) : 0.3));
    series('Receiver code biases and inter-system bias (m)', { series: keys.map((_k, i) => ({ y: A.rxs.subarray((i * M.nSta + j) * ne, (i * M.nSta + j + 1) * ne), color: cols[i] })), robust: true,
      valueAt: e => keys.map((k, i) => k.join(' ') + ' ' + fmt(A.rxs[(i * M.nSta + j) * ne + e], 3)).join(', ') }, keys.map((k, i) => [cols[i], k.join(' ').replace('_', ' ')]));
  }
  if (A.cg) {
    const f = i => A.cg.subarray((i * M.nSta + j) * ne, (i * M.nSta + j + 1) * ne);
    series('Coordinate test: offset of the held coordinate (m)', { series: [{ y: f(0), color: vik(-0.8), connect: true }, { y: f(1), color: vik(0.8), connect: true }, { y: f(2), color: C.ink, width: 1.4, connect: true }], symmetric: true,
      dots: Array.from(f(4)).map((v, e) => v > 0 ? { e, y: f(2)[e], color: C.alarm, r: 3 } : null).filter(Boolean),
      valueAt: e => 'east ' + fmtLen(f(0)[e]) + ', north ' + fmtLen(f(1)[e]) + ', up ' + fmtLen(f(2)[e]) + ', test ' + fmt(f(3)[e], 2) }, [[vik(-0.8), 'east'], [vik(0.8), 'north'], [C.ink, 'up'], [C.alarm, 'reported']]);
  }
  if (A.sr_max) series('Residual test: largest normalized residual of its satellites', { series: [{ y: rowOf(A.sr_max, j), color: C.alarm }], zeroBased: true, robust: true, hlines: [{ y: 3, color: C.muted, inRange: true }],
    valueAt: e => fmt(A.sr_max[j * ne + e], 1) + ' (' + A.sr_n3[j * ne + e] + ' above 3)' });
  if (M.hasFixing) series('Its links that are not tree links', { series: [{ y: rowOf(d.links, j), color: C.float, step: true }, { y: rowOf(d.held, j), color: C.held, width: 1.5, step: true }], zeroBased: true,
    valueAt: e => d.held[j * ne + e] + ' held of ' + d.links[j * ne + e] }, [[C.float, 'links'], [C.held, 'held']]);
  const pe = panel(pad, 'Events of this receiver', M.evBySta[j].length + ' rows; click one to open its line of sight at that time');
  eventList(pe, M.evBySta[j]);
}
function scaled(a, f) { return Float32Array.from(a, v => v * f); }
function sysColor(c) { return { G: C.held, E: C.coderef, C: C.refused, J: C.slip, R: C.release }[c] || C.float; }

/** line of sight index by (receiver * nSat + satellite), from the los block. */
function losIndexMap() {
  const L = B('los');
  if (!L) return null;
  if (L._map) return L._map;
  const m = new Map();
  for (let i = 0; i < L.A.los_sta.length; i++) m.set(L.A.los_sta[i] * M.nSat + L.A.los_sat[i], i);
  L._map = m;
  return m;
}

const RV = { j: -1, nowTiles: null };
registerView('receiver', {
  title: 'Receiver', blocks: ['receivers', 'los'],
  init() {},
  selection(args) { const n = M.nodeIndex.get(args[0]); return n != null && n < M.nSta ? { kind: 'sta', node: n } : null; },
  crumbs(args) { return [['Receivers', ['receivers']], [args[0] || '', ['receiver', args[0] || '']]]; },
  show(args) {
    const n = M.nodeIndex.get(args[0]);
    const root = viewEl('receiver');
    if (n == null || n >= M.nSta) { root.innerHTML = '<div class="pad">No receiver named ' + esc(args[0] || '') + ' in this run.</div>'; return; }
    RV.j = n;
    renderReceiver(root, n);
  },
  time() { if (RV.nowTiles) RV.nowTiles(); },
});
