'use strict';
/*
 * Network dashboard: the satellites.
 *   #/satellites       every satellite: key figures, the sub-satellite points on a world map, a table, and
 *                      satellites x time for one metric
 *   #/satellite/SAT    one satellite: key figures, the receivers that track it (arcs with events), its ground
 *                      track, the emitted clock and biases with their jumps and discontinuities, its code-bias
 *                      states, the clock anchor guard, its post-fit residuals, its hops on the trees
 * Per-satellite quantities come from the satellites block (scripts/netview/layers/satellites.py) and the core block
 * (products, jumps, sub-satellite points, trees).
 */

// ============================================================================================ derived per satellite

function satDerived() {
  if (M.satd) return M.satd;
  const nt = M.nSat, ne = M.nEp, A = M.A;
  const d = { held: new Float32Array(nt * ne), links: new Float32Array(nt * ne), frac: new Float32Array(nt * ne), jumps: new Float32Array(nt * ne),
    tot: { slip: new Int32Array(nt), qc: new Int32Array(nt), alarm: new Int32Array(nt), whole: new Int32Array(nt), iono: new Int32Array(nt), frac: new Int32Array(nt) } };
  for (let i = 0; i < M.nEv; i++) {
    const k = A.ev_sat[i];
    if (k < 0) continue;
    const c = evClass(i);
    if (c) d.tot[c][k]++;
  }
  if (M.hasLinks) {
    for (let l = 0; l < M.nLinks; l++) {
      if (!M.H.families[A.link_fam[l]].fixing) continue;
      const k = A.link_sat[l], s = A.link_start[l], n = A.link_count[l];
      for (let r = s; r < s + n; r++) {
        const o = k * ne + A.ce_ep[r];
        d.links[o]++;
        if (A.ce_state[r] === ST.held) d.held[o]++;
      }
    }
  }
  d.share = new Float32Array(nt * ne);
  for (let i = 0; i < nt * ne; i++) d.share[i] = d.links[i] ? d.held[i] / d.links[i] : NaN;
  if (A.jp_ep) for (let i = 0; i < A.jp_ep.length; i++) {
    const k = A.jp_sat[i], c = A.jp_cls[i];
    if (!c) continue;
    d.jumps[k * ne + A.jp_ep[i]]++;
    if (c === 3) d.frac[k * ne + A.jp_ep[i]]++;
    d.tot[['', 'whole', 'iono', 'frac'][c]][k]++;
  }
  M.satd = d;
  return d;
}

function satMetrics() {
  const SB = B('satellites'), A = SB ? SB.A : {}, d = satDerived(), ne = M.nEp;
  const row = (a, k) => a.subarray(k * ne, (k + 1) * ne);
  const out = [];
  if (A.sp_c_rms) out.push({ key: 'code', label: 'post-fit code RMS', kind: 'viridis', row: k => row(A.sp_c_rms, k), fmt: v => fmtLen(v), worse: 1 });
  if (A.sp_p_rms) out.push({ key: 'phase', label: 'post-fit phase RMS', kind: 'viridis', row: k => row(A.sp_p_rms, k), fmt: v => fmtLen(v), worse: 1 });
  if (A.sp_nrx) out.push({ key: 'nrx', label: 'receivers with residuals', kind: 'viridis', row: k => Float32Array.from(row(A.sp_nrx, k), v => v || NaN), fmt: v => fmt(v, 0), worse: -1 });
  if (A.ag) {
    const wi = SB.H.ag_fields.findIndex(f => f[0] === 'w_clk');
    if (wi >= 0) out.push({ key: 'anchor', label: 'clock anchor test (|w|, sigma)', kind: 'viridis', row: k => Float32Array.from(A.ag.subarray((wi * M.nSat + k) * ne, (wi * M.nSat + k + 1) * ne), Math.abs), fmt: v => fmt(v, 2), worse: 1 });
  }
  if (M.A.jp_ep) out.push({ key: 'frac', label: 'fraction jumps of the correction level', kind: 'count', row: k => row(d.frac, k), fmt: v => fmt(v, 0), worse: 1, sparse: true });
  if (M.hasFixing) out.push({ key: 'held', label: 'share of its links held', kind: 'viridis', row: k => row(d.share, k), fmt: v => fmtPct(v), worse: -1, range: [0, 1] });
  if (M.hasTree) {
    const cf = curFamily();
    if (cf) out.push({ key: 'hops', label: 'hops on the tree of ' + cf.sig, kind: 'viridis', row: k => Float32Array.from({ length: ne }, (_x, e) => { const v = treeDepth(cf.b, M.nSta + k, e); return v >= 0 ? v : NaN; }), fmt: v => fmt(v, 0), worse: 1 });
  }
  return out;
}

// ============================================================================================ #/satellites

const SATV = { metric: 'code' };

function renderSatellitesView(root) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const SB = B('satellites'), A = SB ? SB.A : {}, d = satDerived(), ne = M.nEp;
  viewHead(pad, 'Satellites', M.nSat + ' satellites; click a row, a point or a cell to open its page');
  missingNote(pad, 'satellites');
  const metrics = satMetrics();
  if (!metrics.find(m => m.key === SATV.metric)) SATV.metric = metrics[0] ? metrics[0].key : '';
  const trip = SB && SB.H.ag_decisions ? SB.H.ag_decisions.indexOf('trip') : -1;
  let trips = 0;
  if (A.ag_dec && trip >= 0) for (const v of A.ag_dec) if (v === trip) trips++;
  tiles(pad, [
    { k: 'Satellites with residuals now', v: A.sp_nrx ? String(Array.from({ length: M.nSat }, (_x, k) => A.sp_nrx[k * ne + S.ep]).filter(v => v > 0).length) : 'n/a', cls: 'info' },
    { k: 'Median post-fit code RMS now', v: fmtLen(medianOf(Array.from({ length: M.nSat }, (_x, k) => A.sp_c_rms ? A.sp_c_rms[k * ne + S.ep] : NaN))), cls: 'info' },
    { k: 'Jumps of the correction level', v: fmtCount(sumOf(d.tot.whole) + sumOf(d.tot.iono) + sumOf(d.tot.frac)), s: 'whole ' + sumOf(d.tot.whole) + ', ionosphere-like ' + sumOf(d.tot.iono) + ', fraction ' + sumOf(d.tot.frac), cls: sumOf(d.tot.frac) ? 'warn' : 'ok' },
    { k: 'Clock anchor guard trips', v: String(trips), s: 'satellite-epochs', cls: trips ? 'warn' : 'ok' },
    { k: 'Discontinuity steps emitted', v: A.dc_ep ? fmtCount(A.dc_ep.length) : 'n/a', s: 'a user resets at each', cls: 'info', href: hrefOf(['products']) },
  ]);
  const ctl = el('div', 'row', '<label>Colour by</label><span class="seg">' + metrics.map(m => '<button data-m="' + m.key + '" class="' + (m.key === SATV.metric ? 'on' : '') + '">' + esc(m.label) + '</button>').join('') + '</span>');
  pad.appendChild(ctl);
  ctl.querySelectorAll('button').forEach(b => b.addEventListener('click', () => { SATV.metric = b.dataset.m; renderSatellitesView(root); }));
  const metric = metrics.find(m => m.key === SATV.metric);
  const g = el('div', 'grid2');
  pad.appendChild(g);
  const pm = panel(g, 'Where they are', 'sub-satellite points at the cursor; circles GPS, diamonds Galileo');
  let sc = null;
  if (metric && M.A.sat_lat) {
    sc = metricScale(metric, M.nSat, metric.row);
    SATV.map = worldMap(pm, {
      height: 330, scale: sc, labels: true,
      points: e => M.H.sats.map((s, k) => ({ label: s, lat: M.A.sat_lat[k * ne + e], lon: M.A.sat_lon[k * ne + e], value: metric.row(k)[e], shape: s[0] === 'E' ? 'diamond' : 'circle', r: 4 })),
      text: k => esc(metric.label) + ': ' + esc(metric.fmt(metric.row(k)[S.ep])), onClick: k => openNode(M.nSta + k),
    });
    scaleKey(pm, sc, metric.fmt);
  }
  const pt = panel(g, 'Table', 'values at the cursor and over the run');
  const cf = curFamily();
  const rows = M.H.sats.map((s, k) => {
    const r = { name: s, k, sys: sysName(s[0]) };
    r.nrx = A.sp_nrx ? A.sp_nrx[k * ne + S.ep] || NaN : NaN;
    r.hops = cf && cf.f.sys === s[0] ? treeDepth(cf.b, M.nSta + k, S.ep) : NaN;
    if (!(r.hops >= 0)) r.hops = NaN;
    r.held = d.share[k * ne + S.ep];
    r.code = A.sp_c_rms ? medianOf(rowOf(A.sp_c_rms, k)) : NaN;
    r.phase = A.sp_p_rms ? medianOf(rowOf(A.sp_p_rms, k)) : NaN;
    r.whole = d.tot.whole[k]; r.iono = d.tot.iono[k]; r.frac = d.tot.frac[k];
    let t = 0;
    if (A.ag_dec && trip >= 0) for (let e = 0; e < ne; e++) if (A.ag_dec[k * ne + e] === trip) t++;
    r.trips = A.ag_dec ? t : NaN;
    r.disc = A.dc_ep ? A.dc_sat.filter(x => x === k).length : NaN;
    return r;
  });
  SATV.table = dataTable(pt, {
    maxHeight: 360, sort: 'code', desc: true, rows, onRow: r => openNode(M.nSta + r.k),
    cols: [
      { key: 'name', label: 'Satellite', fmt: (v, r) => satLink(r.k) },
      { key: 'sys', label: 'System' },
      { key: 'nrx', label: 'Receivers', num: true, fmt: v => isFinite(v) ? v : '' },
      { key: 'hops', label: 'Hops', num: true, fmt: v => isFinite(v) ? v : '' },
      { key: 'held', label: 'Held now', num: true, fmt: v => fmtPct(v) },
      { key: 'code', label: 'Code RMS (run)', num: true, fmt: v => fmtLen(v) },
      { key: 'phase', label: 'Phase RMS (run)', num: true, fmt: v => fmtLen(v) },
      { key: 'whole', label: 'Whole jumps', num: true },
      { key: 'iono', label: 'Iono-like jumps', num: true },
      { key: 'frac', label: 'Fraction jumps', num: true },
      { key: 'trips', label: 'Anchor trips', num: true, fmt: v => isFinite(v) ? v : '' },
      { key: 'disc', label: 'Discontinuities', num: true, fmt: v => isFinite(v) ? v : '' },
    ].filter(c => rows.some(r => r[c.key] != null && (typeof r[c.key] !== 'number' || isFinite(r[c.key])))),
  });
  if (metric) {
    const pmx = panel(pad, 'Satellites over time', esc(metric.label) + ', worst first');
    const order = orderRows(M.nSat, metric.row, metric, false, M.H.sats).filter(k => metric.sparse || hasFinite(metric.row(k)));
    matrix(pmx, { rows: order.map(k => M.H.sats[k]), scale: sc || metricScale(metric, M.nSat, metric.row), value: (r, e) => metric.row(order[r])[e],
      text: (r, e, v) => esc(metric.label) + ': ' + esc(metric.fmt(v)), onClick: (r, e) => { S.ep = e; openNode(M.nSta + order[r]); } });
  }
}

registerView('satellites', {
  title: 'Satellites', blocks: ['satellites'], children: ['satellite'],
  init() {},
  show() { renderSatellitesView(viewEl('satellites')); },
  time() { if (SATV.map) SATV.map.draw(); },
  badge() { return M.nSat + ''; },
});

// ============================================================================================ #/satellite/SAT

const SV = { k: -1, nowTiles: null };

function renderSatellitePage(root, k) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const name = M.H.sats[k], node = M.nSta + k, ne = M.nEp;
  const SB = B('satellites'), A = SB ? SB.A : {}, d = satDerived();
  viewHead(pad, name, sysName(name[0]) + ' satellite &middot; <a class="act" href="' + hrefOf(['network'], { s: 'node:' + name }) + '">its place in the network</a>');
  const box = el('div');
  pad.appendChild(box);
  const famOf = M.H.families.findIndex(f => f.sys === name[0] && f.fixing);
  const f = M.H.families[famOf >= 0 ? famOf : M.H.families.findIndex(x => x.sys === name[0])];
  const nowTiles = () => {
    box.innerHTML = '';
    const items = [
      { k: 'Receivers with residuals', v: A.sp_nrx ? String(A.sp_nrx[k * ne + S.ep]) : 'n/a', cls: 'info' },
      { k: 'Post-fit code RMS', v: fmtLen(A.sp_c_rms ? A.sp_c_rms[k * ne + S.ep] : NaN), s: 'run median ' + fmtLen(A.sp_c_rms ? medianOf(rowOf(A.sp_c_rms, k)) : NaN), cls: 'info' },
      { k: 'Links held now', v: fmtPct(d.share[k * ne + S.ep]), s: d.held[k * ne + S.ep] + ' of ' + d.links[k * ne + S.ep], cls: 'info' },
    ];
    if (f) for (const [b, sig] of [[f.band1, f.b1], [f.band2, f.b2]]) {
      const h = treeDepth(b, node, S.ep);
      items.push({ k: 'Hops on ' + sig, v: h >= 0 ? String(h) : 'not on it', cls: 'info' });
    }
    if (M.A.sat_clk) items.push({ k: 'Emitted clock', v: fmt(M.A.sat_clk[k * ne + S.ep], 3) + ' <small>m</small>', cls: 'info' });
    items.push({ k: 'Jumps over the run', v: String(d.tot.whole[k] + d.tot.iono[k] + d.tot.frac[k]), s: 'fraction ' + d.tot.frac[k], cls: d.tot.frac[k] ? 'warn' : 'ok' });
    tiles(box, items);
  };
  nowTiles();
  SV.nowTiles = nowTiles;
  const g = el('div', 'grid2');
  pad.appendChild(g);
  // receivers that track it
  const rxs = [];
  for (let j = 0; j < M.nSta; j++) {
    const o = (j * M.nSat + k) * ne;
    for (let e = 0; e < ne; e++) if (M.A.los_el[o + e] !== 255) { rxs.push(j); break; }
  }
  const pa = panel(g, 'Receivers that track it', rxs.length + ' receivers; colour: elevation; marks: slips, gaps, holds, releases');
  const evBy = new Map();
  for (const i of M.evBySat[k]) {
    const j = M.A.ev_sta[i];
    if (j < 0) continue;
    if (!evBy.has(j)) evBy.set(j, []);
    evBy.get(j).push(i);
  }
  const markColor = i => {
    const c = evClass(i), lane = M.H.lanes[M.A.ev_lane[i]].key;
    return c === 'slip' ? C.slip : lane === 'gap' ? C.gap : lane === 'hold' ? C.held : lane === 'release' ? C.release : c === 'alarm' ? C.alarm : null;
  };
  const elScale = { kind: 'viridis', lo: 0, hi: 90 };
  arcChart(pa, {
    rows: rxs.map(j => ({ label: shortSta(M.names[j]), present: e => M.A.los_el[(j * M.nSat + k) * ne + e] !== 255,
      color: e => scaleColor(elScale, M.A.los_el[(j * M.nSat + k) * ne + e]),
      marks: (evBy.get(j) || []).map(i => ({ e: M.A.ev_ep[i], color: markColor(i) })).filter(m => m.color) })),
    rowH: rxs.length > 40 ? 8 : 10, labelW: 52,
    text: (r, e) => M.names[rxs[r]] + ', elevation ' + (M.A.los_el[(rxs[r] * M.nSat + k) * ne + e] === 255 ? 'not tracked' : M.A.los_el[(rxs[r] * M.nSat + k) * ne + e] + ' degrees') +
      (evBy.get(rxs[r]) || []).filter(i => M.A.ev_ep[i] === e).map(i => '<br>' + esc(eventText(i))).join(''),
    onClick: (r, e) => { S.ep = e; openLink(rxs[r], k); },
  });
  keyRow(pa, [[C.slip, 'slip'], [C.gap, 'gap or return'], [C.held, 'held'], [C.release, 'released'], [C.alarm, 'alarm']]);
  const pm = panel(g, 'Ground track', 'its sub-satellite point over the run; the receivers that track it at the cursor');
  if (M.A.sat_lat) {
    const track = [];
    for (let e = 0; e < ne; e += Math.max(1, Math.round(ne / 240))) track.push({ label: name + ' ' + clockOf(e), lat: M.A.sat_lat[k * ne + e], lon: M.A.sat_lon[k * ne + e], value: e, r: 2 });
    const tsc = { kind: 'viridis', lo: 0, hi: ne - 1 };
    SV.map = worldMap(pm, { height: 300, scale: tsc,
      points: e => track.concat([{ label: name + ' now', lat: M.A.sat_lat[k * ne + e], lon: M.A.sat_lon[k * ne + e], value: e, r: 7, sel: true, shape: 'diamond' }])
        .concat(rxs.filter(j => M.A.los_el[(j * M.nSat + k) * ne + e] !== 255).map(j => ({ label: M.names[j], lat: M.lat[j], lon: M.lon[j], value: NaN, r: 3.5 }))),
      text: () => '' });
    hint(pm, 'Track colour: time over the run (viridis, dark at the start). Grey dots: receivers tracking it at the cursor.');
  }
  // series
  const g2 = el('div', 'grid2');
  pad.appendChild(g2);
  const series = (title, spec, keys) => { const p = panel(g2, title); makeChart(canvasIn(p), spec); if (keys) keyRow(p, keys); return p; };
  const discTicks = item => A.dc_ep ? Array.from(A.dc_ep).map((e, i) => (A.dc_sat[i] === k && (item == null || SB.H.dc_items[A.dc_item[i]].startsWith(item)) ? { e, color: C.release } : null)).filter(Boolean) : [];
  if (M.A.sat_clk) series('Emitted clock (m)', { series: [{ y: rowOf(M.A.sat_clk, k), color: C.ink }], robust: true, ticks: discTicks('CLK'), valueAt: e => fmt(M.A.sat_clk[k * ne + e], 3) + ' m' }, [[C.release, 'discontinuity step']]);
  const pbs = (M.H.phase_signals || []).map((sg, i) => [sg, i]).filter(([sg]) => { const y = satSeries('pb', k, sg); return y && hasFinite(y); });
  if (pbs.length) {
    const cols = pbs.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, pbs.length > 1 ? 0.8 * i / (pbs.length - 1) : 0.3));
    series('Emitted phase biases (m)', { series: pbs.map(([sg], i) => ({ y: satSeries('pb', k, sg), color: cols[i] })), robust: true, ticks: discTicks('PB'),
      valueAt: e => pbs.map(([sg]) => sg + ' ' + fmt(satSeries('pb', k, sg)[e], 3)).join(', ') }, pbs.map(([sg], i) => [cols[i], sg]));
  }
  if (A.sb) {
    const sigs = SB.H.sb_signals.map((sg, i) => [sg, i]).filter(([, i]) => hasFinite(A.sb.subarray((i * M.nSat + k) * ne, (i * M.nSat + k + 1) * ne)));
    const cols = sigs.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, sigs.length > 1 ? 0.8 * i / (sigs.length - 1) : 0.3));
    if (sigs.length) series('Code biases as estimated (m), with sigma', {
      series: sigs.map(([, i], n) => ({ y: A.sb.subarray((i * M.nSat + k) * ne, (i * M.nSat + k + 1) * ne), color: cols[n] })),
      bands: sigs.map(([, i], n) => { const y = A.sb.subarray((i * M.nSat + k) * ne, (i * M.nSat + k + 1) * ne), s = A.sb_sd.subarray((i * M.nSat + k) * ne, (i * M.nSat + k + 1) * ne); return { lo: y.map((v, e) => v - s[e]), hi: y.map((v, e) => v + s[e]), color: withAlpha(cols[n], 0.15) }; }),
      robust: true, ticks: discTicks('CB'),
      valueAt: e => sigs.map(([sg, i]) => sg + ' ' + fmt(A.sb[(i * M.nSat + k) * ne + e], 3) + (A.sb_pin[(i * M.nSat + k) * ne + e] ? ' (pin)' : '')).join(', '),
    }, sigs.map(([sg], n) => [cols[n], sg]));
  }
  if (A.ag) {
    const fi = nm => SB.H.ag_fields.findIndex(x => x[0] === nm);
    const w = A.ag.subarray((fi('w_clk') * M.nSat + k) * ne, (fi('w_clk') * M.nSat + k + 1) * ne);
    const trip = SB.H.ag_decisions.indexOf('trip'), out = SB.H.ag_decisions.indexOf('out');
    series('Clock anchor guard: test of the clock innovation (sigma)', { series: [{ y: w, color: C.ink }], symmetric: true, robust: true,
      dots: Array.from({ length: ne }, (_x, e) => { const dc = A.ag_dec[k * ne + e]; return dc === trip || dc === out ? { e, y: w[e], color: dc === trip ? C.alarm : C.refused, r: 3 } : null; }).filter(Boolean),
      valueAt: e => 'test ' + fmt(w[e], 2) + ', decision ' + (A.ag_dec[k * ne + e] === 255 ? 'none' : SB.H.ag_decisions[A.ag_dec[k * ne + e]]) }, [[C.alarm, 'trip'], [C.refused, 'loose and failing']]);
  }
  if (A.sp_c_rms) series('Post-fit residuals over its receivers (m)', { series: [{ y: rowOf(A.sp_c_rms, k), color: C.held, width: 1.4 }, { y: rowOf(A.sp_c_max, k), color: C.float }], zeroBased: true, robust: true,
    valueAt: e => 'code RMS ' + fmtLen(A.sp_c_rms[k * ne + e]) + ', phase RMS ' + fmtLen(A.sp_p_rms[k * ne + e]) }, [[C.held, 'code RMS'], [C.float, 'code largest']]);
  if (f) { const p = panel(g2, 'Hops and the correction level'); hopsChart(p, node, f); const cfi = M.H.families.indexOf(f); satLevelChart(p, k, { f, fi: cfi, b: f.band1, bo: f.band2, sig: f.b1 }); }
  const pe = panel(pad, 'Events of this satellite', M.evBySat[k].length + ' rows; click one to open its line of sight at that time');
  eventList(pe, M.evBySat[k]);
}

registerView('satellite', {
  title: 'Satellite', blocks: ['satellites'],
  init() {},
  selection(args) { const k = M.H.sats.indexOf(args[0]); return k >= 0 ? { kind: 'sat', node: M.nSta + k } : null; },
  crumbs(args) { return [['Satellites', ['satellites']], [args[0] || '', ['satellite', args[0] || '']]]; },
  show(args) {
    const k = M.H.sats.indexOf(args[0]);
    const root = viewEl('satellite');
    if (k < 0) { root.innerHTML = '<div class="pad">No satellite named ' + esc(args[0] || '') + ' in this run.</div>'; return; }
    SV.k = k;
    renderSatellitePage(root, k);
  },
  time() { if (SV.nowTiles) SV.nowTiles(); if (SV.map) SV.map.draw(); },
});
