'use strict';
/*
 * Network dashboard: the overview (#/overview), the landing page.
 * Key figures of the run with their history (a click opens the layer behind each), receivers x time and
 * satellites x time for a chosen metric (a click on a cell opens that receiver or satellite at that time), and the
 * receivers and satellites that most need a look.
 */

const OV = { rxMetric: 'code', satMetric: 'code' };

function esSeries(name) {
  const EB = B('epoch');
  if (!EB || !EB.A.es) return null;
  const i = EB.H.es_fields.findIndex(f => f[0] === name);
  return i < 0 ? null : EB.A.es.subarray(i * M.nEp, (i + 1) * M.nEp);
}
function clSeries(name) {
  const EB = B('epoch');
  if (!EB || !EB.A.cl) return null;
  const i = EB.H.cl_fields.findIndex(f => f[0] === name);
  return i < 0 ? null : EB.A.cl.subarray(i * M.nEp, (i + 1) * M.nEp);
}

/** Median of a series over each hour of the run: [{h, v}]. */
function hourlyMedian(y) {
  return hourBins().map(([a, b], h) => ({ h, v: medianOf(y.subarray(a, b)) }));
}

function overviewTiles(box) {
  box.innerHTML = '';
  const ne = M.nEp, e = S.ep, items = [];
  const EB = B('epoch');
  if (EB && EB.A.es_disp) {
    let emitted = 0;
    for (let i = 0; i < ne; i++) if (EB.A.es_disp[i] === 0) emitted++;
    items.push({ k: 'Epochs emitted', v: emitted + ' <small>of ' + ne + '</small>', cls: emitted === ne ? 'ok' : 'warn', s: emitted === ne ? 'none withheld' : (ne - emitted) + ' withheld or failed', href: hrefOf(['products']) });
  }
  const s0c = esSeries('sigma0_pf_code');
  if (s0c) items.push({ k: 'Post-fit sigma0, code', v: fmt(s0c[e], 2), s: 'run median ' + fmt(medianOf(s0c), 2) + ' (1 = as weighted)', cls: 'info', spark: { y: s0c }, href: hrefOf(['stochastic']) });
  const npres = esSeries('n_present');
  if (npres) items.push({ k: 'Receivers in the solution', v: fmt(npres[e], 0) + ' <small>of ' + M.nSta + '</small>', cls: npres[e] < M.nSta - 1 ? 'warn' : 'ok', spark: { y: npres, step: true }, href: hrefOf(['receivers']) });
  const nsat = esSeries('n_clock_sats');
  if (nsat) items.push({ k: 'Satellites with a clock', v: fmt(nsat[e], 0), cls: 'info', spark: { y: nsat, step: true }, href: hrefOf(['satellites']) });
  const sl = M.A.sum_links;
  if (sl) M.H.families.forEach((f, fi) => {
    if (!f.fixing) return;
    const y = new Float32Array(ne);
    for (let i = 0; i < ne; i++) { const n = sl[fi * ne + i]; y[i] = n ? sl[(M.nFam + fi) * ne + i] / n : NaN; }
    items.push({ k: 'Held, ' + famLabel(f), v: fmtPct(y[e]), s: sl[(M.nFam + fi) * ne + e] + ' of ' + sl[fi * ne + e] + ' links that are not tree links', cls: 'info',
      spark: { y, yRange: [0, 1] }, href: hrefOf(['network'], { f: fi }) });
  });
  const d = rxDerived();
  const hours = Math.max(1 / 60, (ne - 1) * M.step / 3600000);
  const slipTotal = sumOf(d.tot.slip);
  const slipsPerEp = new Float32Array(ne);
  for (let j = 0; j < M.nSta; j++) for (let i = 0; i < ne; i++) slipsPerEp[i] += d.slip[j * ne + i];
  items.push({ k: 'Cycle slips detected', v: fmt(slipTotal / hours, 0) + ' <small>per hour</small>', s: fmtCount(slipTotal) + ' over the run', cls: 'info',
    spark: { y: smooth(slipsPerEp, Math.round(600000 / M.step)), zeroBased: true }, href: hrefOf(['slips']) });
  const qcTotal = sumOf(d.tot.qc), alarmTotal = sumOf(d.tot.alarm);
  items.push({ k: 'Quality-test flags', v: fmtCount(qcTotal), s: 'alarms that acted: ' + alarmTotal, cls: alarmTotal ? 'warn' : 'ok', href: hrefOf(['stochastic']) });
  const p3 = clSeries('pos_err_3d_m');
  if (p3) {
    const hm = hourlyMedian(p3), last = hm.length ? hm[hm.length - 1].v : NaN;
    items.push({ k: 'Held-out user receiver, 3D error', v: fmtLen(p3[e]), s: 'median by hour: ' + hm.map(x => fmtLen(x.v, 0)).join(' '), cls: last < 0.05 ? 'ok' : last < 0.2 ? 'warn' : 'bad',
      spark: { y: p3, zeroBased: true }, href: hrefOf(['products']) });
  }
  const rs = EB && EB.H.run_summary;
  if (rs) items.push({ k: 'Time per epoch', v: fmt(rs.ms_per_epoch / 1000, 2) + ' <small>s</small>', s: (rs.n_threads || '?') + ' threads, ' + fmtCount(rs.n_emitted) + ' epochs emitted', cls: 'info', href: hrefOf(['products']) });
  tiles(box, items);
}
/** Running mean over k epochs (for sparse counts drawn as a rate). */
function smooth(y, k) {
  const out = new Float32Array(y.length);
  let acc = 0;
  for (let i = 0; i < y.length; i++) {
    acc += y[i];
    if (i >= k) acc -= y[i - k];
    out[i] = acc / Math.min(k, i + 1);
  }
  return out;
}

function renderOverviewView(root) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const d0 = gpsDate(0);
  viewHead(pad, 'Overview', esc(M.H.run) + ': ' + d0.toISOString().slice(0, 10) + ' (day ' + dayOfYear(d0) + '), ' + durationText(M.nEp) + ', ' +
    M.nSta + ' receivers, ' + M.nSat + ' satellites. Tiles show the cursor epoch; a click opens the layer behind them.');
  const tb = el('div');
  pad.appendChild(tb);
  overviewTiles(tb);
  OV.tiles = tb;
  attentionPanel(pad);
  // receivers x time
  const rm = rxMetrics();
  if (rm.length) {
    if (!rm.find(m => m.key === OV.rxMetric)) OV.rxMetric = rm[0].key;
    const p = panel(pad, 'Receivers over time', 'worst first; a click on a cell opens the receiver at that time');
    metricMatrix(p, rm, OV.rxMetric, k => { OV.rxMetric = k; renderOverviewView(root); }, M.nSta, M.H.stations.map(s => s.name), (j, e) => { S.ep = e; openNode(j); });
  }
  const sm = typeof satMetrics === 'function' ? satMetrics() : [];
  if (sm.length) {
    if (!sm.find(m => m.key === OV.satMetric)) OV.satMetric = sm[0].key;
    const p = panel(pad, 'Satellites over time', 'worst first; a click on a cell opens the satellite at that time');
    metricMatrix(p, sm, OV.satMetric, k => { OV.satMetric = k; renderOverviewView(root); }, M.nSat, M.H.sats, (k, e) => { S.ep = e; openNode(M.nSta + k); });
  }
  missingNote(pad, 'products');
}

/** A metric picker and the matrix of its rows x epochs, worst rows first. */
function metricMatrix(parent, metrics, key, onPick, nRows, names, onCell) {
  const ctl = el('div', 'row', '<span class="seg">' + metrics.map(m => '<button data-m="' + m.key + '" class="' + (m.key === key ? 'on' : '') + '">' + esc(m.label) + '</button>').join('') + '</span>');
  parent.appendChild(ctl);
  ctl.querySelectorAll('button').forEach(b => b.addEventListener('click', () => onPick(b.dataset.m)));
  const metric = metrics.find(m => m.key === key);
  const sc = metricScale(metric, nRows, metric.row);
  const order = orderRows(nRows, metric.row, metric, false, names).filter(r => metric.sparse ? true : hasFinite(metric.row(r)));
  matrix(parent, { rows: order.map(r => names[r]), scale: sc, value: (r, e) => metric.row(order[r])[e],
    text: (r, e, v) => esc(metric.label) + ': ' + esc(metric.fmt(v)), onClick: (r, e) => onCell(order[r], e) });
  scaleKey(parent, sc, metric.fmt);
}

/** The receivers and satellites that most need a look: most slips, largest residuals, alarms, clock trips. */
function attentionPanel(parent) {
  const p = panel(parent, 'Worth a look', 'over the whole run');
  const box = el('div', 'attn');
  p.appendChild(box);
  const d = rxDerived(), RB = B('receivers'), SB = B('satellites');
  const lists = [];
  const top = (n, score, fmtv, link) => Array.from({ length: n }, (_x, i) => i).map(i => [i, score(i)]).filter(x => isFinite(x[1]) && x[1] > 0)
    .sort((a, b) => b[1] - a[1]).slice(0, 6).map(([i, v]) => '<li>' + link(i) + ' <span class="muted">' + fmtv(v) + '</span></li>').join('');
  lists.push(['Receivers with the most slips', top(M.nSta, j => d.tot.slip[j], v => v + ' slips', staLink)]);
  if (RB && RB.A.rp_c_rms) lists.push(['Receivers with the largest code residuals', top(M.nSta, j => medianOf(rowOf(RB.A.rp_c_rms, j)), v => fmtLen(v) + ' median RMS', staLink)]);
  lists.push(['Receivers with the most test flags', top(M.nSta, j => d.tot.qc[j] + 10 * d.tot.alarm[j], v => v + '', staLink)]);
  if (SB && SB.A.sp_c_rms) lists.push(['Satellites with the largest code residuals', top(M.nSat, k => medianOf(rowOf(SB.A.sp_c_rms, k)), v => fmtLen(v) + ' median RMS', satLink)]);
  if (SB && SB.A.ag_dec) {
    const trip = SB.H.ag_decisions.indexOf('trip');
    lists.push(['Satellites whose clock tripped its anchor guard', top(M.nSat, k => { let c = 0; for (let e = 0; e < M.nEp; e++) if (SB.A.ag_dec[k * M.nEp + e] === trip) c++; return c; }, v => v + ' epochs', satLink)]);
  }
  if (M.A.jp_ep) {
    const frac = new Float32Array(M.nSat);
    for (let i = 0; i < M.A.jp_ep.length; i++) if (M.A.jp_cls[i] === 3) frac[M.A.jp_sat[i]]++;
    lists.push(['Satellites with the most fraction jumps', top(M.nSat, k => frac[k], v => v + ' jumps', satLink)]);
  }
  box.innerHTML = lists.filter(l => l[1]).map(([t, li]) => '<div><b>' + esc(t) + '</b><ol>' + li + '</ol></div>').join('') || '<div class="hint">nothing stands out</div>';
}

registerView('overview', {
  title: 'Overview', blocks: ['epoch', 'receivers', 'satellites'],
  init() {},
  show() { renderOverviewView(viewEl('overview')); },
  time() { if (OV.tiles) overviewTiles(OV.tiles); },
});
