'use strict';
/*
 * Network dashboard: the cycle slips (#/slips).
 * How many slips each detector found and when, on which receivers and satellites, at which elevations, and how
 * often each test flags (with the false-alarm rate its threshold implies).  A click on a receiver or a cell opens
 * that receiver; the list at the end opens each slip's line of sight.
 * Detections are the rows of network_events.csv of the slip tests (loss of lock, Melbourne-Wubbena jump,
 * geometry-free jump, ionosphere-free time-difference test), each response counted once.
 */

const DETECTORS = [
  ['lli', 'loss of lock (receiver flag)', i => tierOf(i) === 'T0_mw_detect' && nameOf(i) === 'lli_slip'],
  ['mw', 'Melbourne-Wubbena jump', i => tierOf(i) === 'T0_mw_detect' && nameOf(i) !== 'lli_slip'],
  ['gf', 'geometry-free jump', i => tierOf(i) === 'T1_gf_jump'],
  ['if', 'ionosphere-free time-difference test', i => tierOf(i) === 'T1_if_slip'],
];
function tierOf(i) { return M.H.events.tiers[M.A.ev_tier[i]]; }
function nameOf(i) { return M.H.events.names[M.A.ev_name[i]]; }
function detectorOf(i) {
  if (evClass(i) !== 'slip') return -1;
  return DETECTORS.findIndex(d => d[2](i));
}
function detColor(k) { return [C.release, C.slip, C.iono, C.held][k] || C.other; }

function renderSlips(root) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const ne = M.nEp, A = M.A;
  const hours = Math.max(1 / 60, (ne - 1) * M.step / 3600000);
  const det = [], perDet = DETECTORS.map(() => 0), perSta = Array.from({ length: M.nSta }, () => DETECTORS.map(() => 0)),
    perSat = Array.from({ length: M.nSat }, () => DETECTORS.map(() => 0));
  let gaps = 0, windup = 0;
  for (let i = 0; i < M.nEv; i++) {
    const t = tierOf(i);
    if (t === 'T0_gap') gaps++;
    if (t === 'T0_windup') windup++;
    const k = detectorOf(i);
    if (k < 0) continue;
    det.push(i);
    perDet[k]++;
    if (A.ev_sta[i] >= 0) perSta[A.ev_sta[i]][k]++;
    if (A.ev_sat[i] >= 0) perSat[A.ev_sat[i]][k]++;
  }
  viewHead(pad, 'Cycle slips', fmtCount(det.length) + ' slips detected over ' + durationText(ne) + ' (' + fmt(det.length / hours, 0) + ' per hour)');
  missingNote(pad, 'slips');
  tiles(pad, DETECTORS.map((d, k) => ({ k: d[1], v: fmtCount(perDet[k]), s: fmt(perDet[k] / hours, 1) + ' per hour', cls: 'info' }))
    .concat([{ k: 'tracking resumed after a gap', v: fmtCount(gaps), s: 'not slips; the arc restarts', cls: 'info' },
      { k: 'wind-up restarted', v: fmtCount(windup), cls: 'info' }]));
  // per 10 minutes by detector
  const p1 = panel(pad, 'Slips per 10 minutes, by detector');
  const bin = Math.max(1, Math.round(600000 / M.step)), bars = [];
  for (let e0 = 0; e0 < ne; e0 += bin) {
    const v = DETECTORS.map(() => 0);
    for (let i = M.evOff[e0]; i < M.evOff[Math.min(ne, e0 + bin)]; i++) { const k = detectorOf(i); if (k >= 0) v[k]++; }
    bars.push({ e0, e1: e0 + bin, stack: v.map((x, k) => ({ v: x, color: detColor(k) })) });
  }
  makeChart(canvasIn(p1, 'chart tall'), { bars, zeroBased: true, nTicks: 4,
    valueAt: e => { const b = bars[Math.floor(e / bin)]; return b ? DETECTORS.map((d, k) => d[0] + ' ' + b.stack[k].v).join(', ') : ''; } });
  keyRow(p1, DETECTORS.map((d, k) => [detColor(k), d[1]]));
  // receivers x time
  const d = rxDerived();
  const p2 = panel(pad, 'Receivers over time', 'slips per epoch, most first; a click opens the receiver at that time');
  const names = M.H.stations.map(s => s.name);
  const order = Array.from({ length: M.nSta }, (_x, j) => j).filter(j => d.tot.slip[j] > 0).sort((a, b) => d.tot.slip[b] - d.tot.slip[a]);
  if (order.length < M.nSta) hint(p2, (M.nSta - order.length) + ' receivers had no slip and are left out.');
  let mx = 1;
  for (const v of d.slip) if (v > mx) mx = v;
  const sc = { kind: 'count', lo: 0, hi: mx, label: 'slips at the epoch' };
  matrix(p2, { rows: order.map(j => names[j] + ' (' + d.tot.slip[j] + ')'), scale: sc, value: (r, e) => d.slip[order[r] * ne + e] || NaN,
    text: (r, e, v) => (isFinite(v) ? v : 0) + ' slips', onClick: (r, e) => { S.ep = e; openNode(order[r]); }, labelW: 130 });
  scaleKey(p2, sc, v => v.toFixed(0));
  // tables
  const g = el('div', 'grid2');
  pad.appendChild(g);
  const RB = B('receivers');
  const pt = panel(g, 'By receiver', 'per satellite-hour of tracking: slips divided by the hours its satellites were tracked');
  const rows = names.map((n, j) => {
    let satEp = 0;
    if (RB && RB.A.rp_nsat) for (let e = 0; e < ne; e++) satEp += RB.A.rp_nsat[j * ne + e];
    const r = { name: n, j, total: perSta[j].reduce((a, b) => a + b, 0) };
    DETECTORS.forEach((dd, k) => { r[dd[0]] = perSta[j][k]; });
    r.rate = satEp ? r.total / (satEp * M.step / 3600000) : NaN;
    return r;
  });
  dataTable(pt, { maxHeight: 380, sort: 'total', rows, onRow: r => openNode(r.j),
    cols: [{ key: 'name', label: 'Receiver', fmt: (v, r) => staLink(r.j) }, { key: 'total', label: 'Slips', num: true }]
      .concat(DETECTORS.map(dd => ({ key: dd[0], label: dd[0].toUpperCase(), num: true, title: dd[1] })))
      .concat([{ key: 'rate', label: 'Per satellite-hour', num: true, fmt: v => fmt(v, 2) }]) });
  const ps = panel(g, 'By satellite');
  const srows = M.H.sats.map((n, k) => { const r = { name: n, k, total: perSat[k].reduce((a, b) => a + b, 0) }; DETECTORS.forEach((dd, q) => { r[dd[0]] = perSat[k][q]; }); return r; });
  dataTable(ps, { maxHeight: 380, sort: 'total', rows: srows, onRow: r => openNode(M.nSta + r.k),
    cols: [{ key: 'name', label: 'Satellite', fmt: (v, r) => satLink(r.k) }, { key: 'total', label: 'Slips', num: true }]
      .concat(DETECTORS.map(dd => ({ key: dd[0], label: dd[0].toUpperCase(), num: true, title: dd[1] }))) });
  // elevation
  const g2 = el('div', 'grid2');
  pad.appendChild(g2);
  const pe = panel(g2, 'Slips by elevation', 'bars: count per 5 degrees by detector; line: per hour of tracking at that elevation');
  const nb = 18, cnt = DETECTORS.map(() => new Float32Array(nb)), track = new Float32Array(nb);
  for (const i of det) {
    const j = A.ev_sta[i], k = A.ev_sat[i];
    if (j < 0 || k < 0) continue;
    const v = A.los_el[(j * M.nSat + k) * ne + A.ev_ep[i]];
    if (v === 255) continue;
    cnt[detectorOf(i)][Math.min(nb - 1, Math.floor(v / 5))]++;
  }
  for (let i = 0; i < A.los_el.length; i++) { const v = A.los_el[i]; if (v !== 255) track[Math.min(nb - 1, Math.floor(v / 5))]++; }
  const rate = Array.from(track, (t, b) => { const n = cnt.reduce((s, c) => s + c[b], 0); return t ? n / (t * M.step / 3600000) : NaN; });
  const maxCount = Math.max(1, ...Array.from({ length: nb }, (_x, b) => cnt.reduce((s, c) => s + c[b], 0)));
  const maxRate = Math.max(1e-9, ...rate.filter(isFinite));
  barChart(pe, { labels: Array.from({ length: nb }, (_x, b) => (b * 5) + '-' + (b * 5 + 5)), stacked: true,
    series: DETECTORS.map((dd, k) => ({ name: dd[1], v: Array.from(cnt[k]), color: detColor(k) })).concat([{ name: 'per tracking hour (scaled)', v: rate.map(r => r / maxRate * maxCount), color: C.ink, line: true }]),
    tipFmt: v => shortNum(v) }, 'chart tall');
  keyRow(pe, DETECTORS.map((dd, k) => [detColor(k), dd[1]]).concat([[C.ink, 'rate per tracking hour (scaled to the bars)']]));
  const EB = B('epoch');
  const pf = panel(g2, 'How often each test flags', 'opportunities, flags, the rate and the false-alarm rate its threshold implies');
  if (EB && EB.H.flag_rates && EB.H.flag_rates.length) {
    dataTable(pf, { maxHeight: 300, sort: 'flags', rows: EB.H.flag_rates.map(r => Object.assign({}, r)),
      cols: [{ key: 'label', label: 'Test' }, { key: 'opportunities', label: 'Tested', num: true, fmt: v => fmtCount(v) }, { key: 'flags', label: 'Flags', num: true, fmt: v => fmtCount(v) },
        { key: 'rate', label: 'Rate', num: true, fmt: v => v != null ? Number(v).toExponential(1) : '' }, { key: 'k', label: 'Threshold (sigma)', num: true },
        { key: 'implied_alpha', label: 'Implied false-alarm rate', num: true, fmt: v => v != null ? Number(v).toExponential(1) : '' }] });
  } else hint(pf, 'Not in this run: network_flag_rates.csv (key network.trace_flag_rates).');
  const pl = panel(pad, 'The slips', 'each detection; a click opens its line of sight at that time');
  eventList(pl, det, { families: ['slip'], limit: 300 });
}

registerView('slips', {
  title: 'Cycle slips', blocks: ['receivers', 'epoch'],
  init() {},
  show() { renderSlips(viewEl('slips')); },
  badge() { const d = rxDerived(); const n = sumOf(d.tot.slip); return n ? fmtCount(n) : ''; },
});
