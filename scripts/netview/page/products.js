'use strict';
/*
 * Network dashboard: the products and the user check (#/products).
 * What the network emitted and how a user did with it: epochs emitted or withheld, the emitted products and their
 * discontinuities, the datum of each constellation, the time per epoch, and the held-out user receiver (a receiver
 * outside the network that uses the products and is scored against its known position): its position error by
 * hour, its fixing and its inter-system bias.  Sources: the epoch block and the satellites block.
 */

function epochField(block, name) {
  const EB = B('epoch');
  if (!EB || !EB.A[block]) return null;
  const f = EB.H[block + '_fields'];
  const i = f ? f.findIndex(x => x[0] === name) : -1;
  return i < 0 ? null : EB.A[block].subarray(i * M.nEp, (i + 1) * M.nEp);
}

function renderProducts(root) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const EB = B('epoch'), SB = B('satellites'), ne = M.nEp;
  viewHead(pad, 'Products and user check', 'what the network emitted, and how the held-out user receiver did with it');
  missingNote(pad, 'products');
  const p3 = epochField('cl', 'pos_err_3d_m');
  const items = [];
  if (EB && EB.A.es_disp) {
    const counts = {};
    for (const v of EB.A.es_disp) { const k = v === 255 ? 'none' : EB.H.dispositions[v]; counts[k] = (counts[k] || 0) + 1; }
    items.push({ k: 'Epochs', v: (counts.emitted || 0) + ' <small>emitted of ' + ne + '</small>', s: Object.entries(counts).filter(([k]) => k !== 'emitted').map(([k, n]) => n + ' ' + k.replace('_', ' ')).join(', ') || 'none withheld', cls: counts.emitted === ne ? 'ok' : 'warn' });
  }
  if (SB && SB.A.dc_ep) items.push({ k: 'Discontinuity steps', v: fmtCount(SB.A.dc_ep.length), s: 'a user resets the quantity at each', cls: 'info' });
  if (p3) {
    const hm = hourBins().map(([a, b]) => medianOf(p3.subarray(a, b)));
    items.push({ k: 'Held-out user, 3D error (median of the last hour)', v: fmtLen(hm[hm.length - 1]), s: 'by hour: ' + hm.map(v => fmtLen(v, 0)).join(', '), cls: hm[hm.length - 1] < 0.05 ? 'ok' : hm[hm.length - 1] < 0.2 ? 'warn' : 'bad' });
    const ran = epochField('cl', 'ar_ran'), ok = epochField('cl', 'ar_success');
    if (ran) { const nr = sumOf(ran), no = sumOf(ok); items.push({ k: 'Held-out user, fixing accepted', v: nr ? fmtPct(no / nr) : 'did not run', s: no + ' of ' + nr + ' epochs where it ran', cls: 'info' }); }
  }
  const rs = EB && EB.H.run_summary;
  if (rs) items.push({ k: 'Time per epoch', v: fmt(rs.ms_per_epoch / 1000, 2) + ' <small>s</small>', s: rs.n_threads + ' threads; loop ' + durationText(Math.round(rs.loop_ms / M.step)), cls: 'info' });
  tiles(pad, items);
  const g = el('div', 'grid2');
  pad.appendChild(g);
  if (p3) {
    const st = EB.H.cl_station ? ' (' + esc(EB.H.cl_station) + ')' : '';
    const p = panel(g, 'Held-out user receiver: position error (m)' + st, 'estimate minus its known position; band: 3D sigma');
    const e_ = epochField('cl', 'east_m'), n_ = epochField('cl', 'north_m'), u_ = epochField('cl', 'up_m'), s3 = epochField('cl', 'sig_3d_m');
    makeChart(canvasIn(p, 'chart tall'), { series: [{ y: p3, color: C.ink, width: 1.5 }, { y: e_, color: vik(-0.8) }, { y: n_, color: vik(0.8) }, { y: u_, color: C.held }],
      bands: s3 ? [{ lo: s3.map(v => -v), hi: s3, color: withAlpha(C.float, 0.15) }] : [], symmetric: true, robust: true,
      valueAt: e => '3D ' + fmtLen(p3[e]) + ', east ' + fmtLen(e_[e]) + ', north ' + fmtLen(n_[e]) + ', up ' + fmtLen(u_[e]) });
    keyRow(p, [[C.ink, '3D'], [vik(-0.8), 'east'], [vik(0.8), 'north'], [C.held, 'up']]);
    const hb = hourBins();
    const rows = hb.map(([a, b], h) => ({ h, n: medianOf(n_.subarray(a, b)), e: medianOf(e_.subarray(a, b)), u: medianOf(u_.subarray(a, b)), d3: medianOf(p3.subarray(a, b)),
      rms: Math.sqrt(Array.from(p3.subarray(a, b)).filter(isFinite).reduce((s, v) => s + v * v, 0) / Math.max(1, Array.from(p3.subarray(a, b)).filter(isFinite).length)) }));
    dataTable(p, { sort: 'h', desc: false, rows, cols: [{ key: 'h', label: 'Hour', num: true }, { key: 'd3', label: '3D median', num: true, fmt: v => fmtLen(v) }, { key: 'rms', label: '3D RMS', num: true, fmt: v => fmtLen(v) },
      { key: 'e', label: 'East median', num: true, fmt: v => fmtLen(v) }, { key: 'n', label: 'North median', num: true, fmt: v => fmtLen(v) }, { key: 'u', label: 'Up median', num: true, fmt: v => fmtLen(v) }] });
    const nf = epochField('cl', 'num_fixed'), ratio = epochField('cl', 'ratio'), nsat = epochField('cl', 'n_sats');
    if (nf) {
      const q = panel(g, 'Held-out user receiver: satellites and fixing');
      makeChart(canvasIn(q), { series: [{ y: nsat, color: C.ink, step: true }, { y: nf, color: C.held, step: true }], zeroBased: true, valueAt: e => nsat[e] + ' satellites, ' + nf[e] + ' fixed' });
      keyRow(q, [[C.ink, 'satellites used'], [C.held, 'ambiguities fixed']]);
      if (ratio && hasFinite(ratio) && maxOf(ratio) > 0) makeChart(canvasIn(q), { series: [{ y: ratio, color: C.coderef }], zeroBased: true, robust: true, valueAt: e => 'ratio ' + fmt(ratio[e], 2) });
      if (EB.A.cli) {
        const sy = EB.H.cli_systems;
        makeChart(chartEl(q, 'Its inter-system bias (m)'), { series: sy.map((_s, i) => ({ y: EB.A.cli.subarray((i * 2) * ne, (i * 2 + 1) * ne), color: C.refused })), robust: true,
          valueAt: e => sy.map((s, i) => s + ' ' + fmt(EB.A.cli[(i * 2) * ne + e], 3)).join(', ') });
      }
    }
  }
  const np = epochField('nt', 'n_phase_bias'), ncs = esSeries('n_clock_sats');
  if (np || ncs) {
    const p = panel(g, 'Emitted per epoch');
    makeChart(canvasIn(p), { series: [np ? { y: np, color: C.held, step: true } : null, ncs ? { y: ncs, color: C.ink, step: true } : null].filter(Boolean), zeroBased: true,
      valueAt: e => (np ? np[e] + ' phase biases' : '') + (ncs ? ', ' + ncs[e] + ' clocks' : '') });
    keyRow(p, [[C.held, 'phase biases'], [C.ink, 'satellite clocks']]);
    if (EB && EB.A.es_disp) {
      const cols = [null, C.refused, C.release, C.alarm, C.alarm, C.gap, C.other];
      makeChart(chartEl(p, 'Epochs not emitted', 'strip'), { strip: { codes: Uint8Array.from(EB.A.es_disp, v => (v === 255 ? 6 : v)), colors: cols } });
    }
  }
  if (SB && SB.A.dc_ep) {
    const p = panel(g, 'Discontinuity steps per 10 minutes', 'by product; many satellites at once is a datum move');
    const items2 = SB.H.dc_items, kinds = [...new Set(items2.map(s => s.split(' ')[0]))];
    const cols = kinds.map((_k, i) => rampColor(window.NV_ASSETS.colormaps.viridis, kinds.length > 1 ? 0.8 * i / (kinds.length - 1) : 0.3));
    const bin = Math.max(1, Math.round(600000 / M.step)), bars = [];
    for (let e0 = 0; e0 < ne; e0 += bin) bars.push({ e0, e1: e0 + bin, stack: kinds.map((_k, i) => ({ v: 0, color: cols[i] })) });
    for (let i = 0; i < SB.A.dc_ep.length; i++) bars[Math.floor(SB.A.dc_ep[i] / bin)].stack[kinds.indexOf(items2[SB.A.dc_item[i]].split(' ')[0])].v++;
    makeChart(canvasIn(p), { bars, zeroBased: true });
    keyRow(p, kinds.map((k, i) => [cols[i], { CLK: 'clock', PB: 'phase bias', CB: 'code bias' }[k] || k]));
  }
  if (EB && EB.A.dg) {
    const p = panel(g, 'Datum per constellation', 'connected parts of the receiver-satellite graph and the datum dimension');
    const cons = EB.H.dg_cons, fi = n => EB.H.dg_fields.findIndex(f => f[0] === n);
    const series = [];
    cons.forEach((c, k) => {
      series.push({ y: EB.A.dg.subarray((k * EB.H.dg_fields.length + fi('n_comps')) * ne, (k * EB.H.dg_fields.length + fi('n_comps') + 1) * ne), color: k ? C.coderef : C.held, step: true, connect: true });
      series.push({ y: EB.A.dg.subarray((k * EB.H.dg_fields.length + fi('d')) * ne, (k * EB.H.dg_fields.length + fi('d') + 1) * ne), color: k ? C.coderef : C.held, step: true, dash: [4, 3], connect: true });
    });
    makeChart(canvasIn(p), { series, zeroBased: true, nTicks: 3 });
    keyRow(p, cons.map((c, k) => [k ? C.coderef : C.held, sysName(c) + ': parts (solid), dimension (dashed)']));
  }
  const ms = epochField('sh', 'ms_per_epoch_window');
  if (ms) {
    const p = panel(g, 'Time per epoch (s)', 'over each reporting window');
    makeChart(canvasIn(p), { series: [{ y: ms.map(v => v / 1000), color: C.ink, connect: true }], zeroBased: true, valueAt: e => fmt(ms[e] / 1000, 2) + ' s' });
  }
  if (rs) {
    const p = panel(pad, 'Run summary');
    p.insertAdjacentHTML('beforeend', '<table class="kv2">' + Object.entries(rs).map(([k, v]) => '<tr><td>' + esc(k.replace(/_/g, ' ')) + '</td><td class="num">' + esc(String(v)) + '</td></tr>').join('') + '</table>');
  }
}

registerView('products', {
  title: 'Products and user check', blocks: ['epoch', 'satellites'],
  init() {},
  show() { renderProducts(viewEl('products')); },
});
