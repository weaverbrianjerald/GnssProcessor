'use strict';
/*
 * Network dashboard: the stochastic model (#/stochastic).
 * Whether the weights match the scatter of the data, and what the tests found:
 *   sigma0 over time (post-fit, code and phase; the within-epoch residual test's own),
 *   variance factors per class of observation (1 = weighted right; the square root is the sigma ratio),
 *   residuals by elevation and by hour per signal, normalized-residual histograms against the normal curve,
 *   the outlier, detectability and carried-prior tests, the attribution of large residuals,
 *   and the model as configured: process noise and floors per parameter kind, and the responses to events.
 * Sources: the quality block (scripts/netview/layers/quality.py) and the epoch block (sigma0, run tables).
 */

const STV = { kind: 'code' };

function renderStochastic(root) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const QB = B('quality'), EB = B('epoch'), Q = QB ? QB.A : {}, QH = QB ? QB.H : {}, ne = M.nEp;
  viewHead(pad, 'Stochastic model', 'do the weights match the data, and what did the tests flag');
  missingNote(pad, 'stochastic');
  const s0 = esSeries('sigma0_pf'), s0c = esSeries('sigma0_pf_code'), s0p = esSeries('sigma0_pf_phase'), s0r = esSeries('sigma0');
  const items = [];
  if (s0c) items.push({ k: 'Post-fit sigma0, code', v: fmt(medianOf(s0c), 2), s: 'median over the run; 1 = as weighted', cls: 'info', spark: { y: s0c } });
  if (s0p) items.push({ k: 'Post-fit sigma0, phase', v: fmt(medianOf(s0p), 3), s: 'about 0 while ambiguities are free', cls: 'info', spark: { y: s0p } });
  if (Q.vcf_ef && QH.vcf_classes) {
    const last = QH.vcf_classes.map((c, i) => [c, Q.vcf_ef[i * Q.vcf_ep.length + Q.vcf_ep.length - 1]]).filter(([c, v]) => isFinite(v) && c.includes(' '));
    const off = last.filter(([, v]) => v > 2 || v < 0.5);
    items.push({ k: 'Variance factors far from 1', v: off.length + ' <small>of ' + last.length + ' classes</small>', s: off.slice(0, 4).map(([c, v]) => c + ' ' + fmt(v, 2)).join(', ') || 'all between 0.5 and 2', cls: off.length ? 'warn' : 'ok' });
  }
  if (Q.bb_n_gt_4) items.push({ k: 'Outlier test: blocks above 4', v: fmtCount(sumOf(Q.bb_n_gt_4)), s: 'receiver-satellite blocks over the run', cls: 'info' });
  if (Q.pr_n_gt_4) items.push({ k: 'Carried priors above 4 sigma', v: fmtCount(sumOf(Q.pr_n_gt_4)), cls: 'info' });
  if (Q.ra_ep) items.push({ k: 'Residual attributions', v: fmtCount(Q.ra_ep.length), s: 'acted on: ' + sumOf(Q.ra_acted), cls: sumOf(Q.ra_acted) ? 'warn' : 'info' });
  tiles(pad, items);
  const g = el('div', 'grid2');
  pad.appendChild(g);
  if (s0) {
    const p = panel(g, 'Sigma0 over time', '1 when the weights match the scatter');
    const trig = esSeries('sigma0_trigger'), ceil = esSeries('withhold_ceiling');
    makeChart(canvasIn(p, 'chart tall'), { series: [{ y: s0, color: C.ink, width: 1.3 }, { y: s0c, color: C.held }, { y: s0p, color: C.coderef }].concat(s0r && hasFinite(s0r) && maxOf(s0r) > 0 ? [{ y: s0r, color: C.alarm }] : []),
      hlines: [{ y: 1, color: C.muted, inRange: true }].concat(trig && isFinite(trig[0]) ? [{ y: trig[0], color: C.refused }] : []).concat(ceil && isFinite(ceil[0]) ? [{ y: ceil[0], color: C.release }] : []),
      zeroBased: true, robust: true, valueAt: e => 'all ' + fmt(s0[e], 3) + ', code ' + fmt(s0c[e], 3) + ', phase ' + fmt(s0p[e], 3) + (s0r ? ', residual test ' + fmt(s0r[e], 2) : '') });
    keyRow(p, [[C.ink, 'post-fit, all rows'], [C.held, 'code'], [C.coderef, 'phase'], [C.alarm, 'within-epoch residual test'], [C.refused, 're-solve trigger'], [C.release, 'withhold ceiling']]);
  }
  if (Q.vcf_ef) {
    const p = panel(g, 'Variance factors per class', 'running average of the redundancy-weighted factor; 1 = weighted right');
    const ctl = el('div', 'row', '<span class="seg"><button data-k="code" class="' + (STV.kind === 'code' ? 'on' : '') + '">code</button><button data-k="phase" class="' + (STV.kind === 'phase' ? 'on' : '') + '">phase</button></span>');
    p.appendChild(ctl);
    ctl.querySelectorAll('button').forEach(b => b.addEventListener('click', () => { STV.kind = b.dataset.k; renderStochastic(root); }));
    const nv = Q.vcf_ep.length;
    const cls = QH.vcf_classes.map((c, i) => [c, i]).filter(([c]) => { const parts = c.split(' '); return parts.length === 2 && (STV.kind === 'code' ? parts[1][0] === 'C' : parts[1][0] === 'L'); });
    const cols = cls.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, cls.length > 1 ? 0.9 * i / (cls.length - 1) : 0.3));
    const series = cls.map(([, i], n) => { const y = new Float32Array(ne).fill(NaN); for (let r = 0; r < nv; r++) y[Q.vcf_ep[r]] = Q.vcf_ef[i * nv + r]; return { y, color: cols[n], width: 1.3, connect: true }; });
    makeChart(canvasIn(p, 'chart tall'), { series, hlines: [{ y: 1, color: C.muted, inRange: true }], zeroBased: true, robust: true,
      valueAt: e => cls.map(([c], n) => isFinite(series[n].y[e]) ? c + ' ' + fmt(series[n].y[e], 2) : '').filter(Boolean).join(', ') });
    keyRow(p, cls.map(([c], n) => [cols[n], c]));
    const rows = QH.vcf_classes.map((c, i) => ({ c, n: Q.vcf_n[i * nv + nv - 1], rho: Q.vcf_rho[i * nv + nv - 1], f: Q.vcf_f[i * nv + nv - 1], ef: Q.vcf_ef[i * nv + nv - 1], g: Q.vcf_g[i * nv + nv - 1], id: Q.vcf_id[i * nv + nv - 1] }));
    dataTable(p, { maxHeight: 240, sort: 'ef', rows, cols: [{ key: 'c', label: 'Class' }, { key: 'n', label: 'Rows', num: true, fmt: v => fmtCount(v) },
      { key: 'rho', label: 'Redundancy share', num: true, fmt: v => fmt(v, 2) }, { key: 'f', label: 'Factor (last)', num: true, fmt: v => fmt(v, 2) },
      { key: 'ef', label: 'Running factor', num: true, fmt: v => fmt(v, 2), color: v => isFinite(v) ? vik(Math.max(-1, Math.min(1, Math.log2(v) / 2))) : null },
      { key: 'g', label: 'Robust factor', num: true, fmt: v => fmt(v, 2) }, { key: 'id', label: 'Estimable', fmt: v => v ? 'yes' : 'no' }] });
  }
  if (Q.eb_rms) {
    const sigs = QH.pf_signals, nb = Q.eb_rms.length / sigs.length;
    const labels = Array.from({ length: nb }, (_x, b) => (b * QH.el_bin) + '-' + ((b + 1) * QH.el_bin));
    for (const [title, isPhase, scale, unit] of [['Code residuals by elevation (m)', false, 1, 'm'], ['Phase residuals by elevation (mm)', true, 1000, 'mm']]) {
      const pick = sigs.map((s, i) => [s, i]).filter(([s]) => (s[0] === 'L') === isPhase);
      const cols = pick.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, pick.length > 1 ? 0.9 * i / (pick.length - 1) : 0.3));
      const p = panel(g, title, 'RMS per 5 degrees (solid) and the median a priori sigma (dashed)');
      barChart(p, { labels, series: pick.map(([s, i], n) => ({ name: s + ' RMS', v: Array.from(Q.eb_rms.subarray(i * nb, (i + 1) * nb), v => v * scale), color: cols[n], line: true }))
        .concat(pick.map(([s, i], n) => ({ name: s + ' sigma', v: Array.from(Q.eb_sd.subarray(i * nb, (i + 1) * nb), v => v * scale), color: withAlpha(cols[n], 0.6), line: true, dash: [4, 3] }))), tipFmt: v => shortNum(v) + ' ' + unit, logY: false }, 'chart tall');
      keyRow(p, pick.map(([s], n) => [cols[n], s]));
    }
    const p = panel(g, 'Normalized residuals by elevation', 'RMS of residual / sigma; 1 means the elevation model of the weights fits');
    const cols = sigs.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, sigs.length > 1 ? 0.9 * i / (sigs.length - 1) : 0.3));
    barChart(p, { labels, series: sigs.map((s, i) => ({ name: s, v: Array.from(Q.eb_zrms.subarray(i * nb, (i + 1) * nb)), color: cols[i], line: true })), tipFmt: v => fmt(v, 2) }, 'chart tall');
    keyRow(p, sigs.map((s, i) => [cols[i], s]));
  }
  if (Q.zh) {
    const sigs = QH.pf_signals, [lo, hi, st] = QH.z_hist, nz = Q.zh.length / sigs.length;
    const p = panel(g, 'Distribution of the normalized residuals', 'share per 0.25 sigma; grey: the normal curve. Wider than grey: weights too tight');
    const labels = Array.from({ length: nz }, (_x, b) => b === 0 ? '<' + lo : b === nz - 1 ? '>' + hi : fmt(lo + (b - 1) * st, 2));
    const cols = sigs.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, sigs.length > 1 ? 0.9 * i / (sigs.length - 1) : 0.3));
    const normal = labels.map((_x, b) => { if (b === 0 || b === nz - 1) return NaN; const z = lo + (b - 0.5) * st; return st * Math.exp(-z * z / 2) / Math.sqrt(2 * Math.PI); });
    const series = sigs.map((s, i) => { const row = Q.zh.subarray(i * nz, (i + 1) * nz); const tot = sumOf(row) || 1; return { name: s, v: Array.from(row, c => c / tot), color: cols[i], line: true }; });
    barChart(p, { labels, series: series.concat([{ name: 'normal', v: normal, color: C.muted, line: true }]), tipFmt: v => fmt(100 * v, 2) + ' %' }, 'chart tall');
    keyRow(p, sigs.map((s, i) => [cols[i], s]).concat([[C.muted, 'normal curve']]));
  }
  if (Q.hb_rms) {
    const sigs = QH.pf_signals, nh = Q.hb_rms.length / sigs.length;
    const p = panel(g, 'Normalized residuals by hour', 'RMS of residual / sigma per signal and hour of the run');
    const cols = sigs.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, sigs.length > 1 ? 0.9 * i / (sigs.length - 1) : 0.3));
    barChart(p, { labels: Array.from({ length: nh }, (_x, h) => 'hour ' + h), series: sigs.map((s, i) => ({ name: s, v: Array.from(Q.hb_zrms.subarray(i * nh, (i + 1) * nh)), color: cols[i] })), tipFmt: v => fmt(v, 2) }, 'chart');
    keyRow(p, sigs.map((s, i) => [cols[i], s]));
  }
  // tests
  const g2 = el('div', 'grid2');
  pad.appendChild(g2);
  const rowsSeries = (epArr, valArr, sel) => { const y = new Float32Array(ne).fill(NaN); for (let r = 0; r < epArr.length; r++) if (!sel || sel(r)) y[epArr[r]] = (isFinite(y[epArr[r]]) ? y[epArr[r]] : 0) + valArr[r]; return y; };
  if (Q.bb_ep) {
    const p = panel(g2, 'Outlier test of receiver-satellite blocks', 'blocks above 4 per check, by constellation; dots: largest test value');
    const fams = QH.bb_families, cols = fams.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, fams.length > 1 ? 0.75 * i / (fams.length - 1) : 0.3));
    makeChart(canvasIn(p), { series: fams.map((_f, fi) => ({ y: rowsSeries(Q.bb_ep, Q.bb_n_gt_4, r => Q.bb_fam[r] === fi), color: cols[fi], connect: true })), zeroBased: true,
      valueAt: e => fams.map((f, fi) => f + ' ' + fmt(rowsSeries(Q.bb_ep, Q.bb_n_gt_4, r => Q.bb_fam[r] === fi)[e], 0)).join(', ') });
    keyRow(p, fams.map((f, i) => [cols[i], f]));
    const worst = [];
    for (let r = 0; r < Q.bb_ep.length; r++) if (Q.bb_max_t[r] > 6) worst.push(r);
    worst.sort((a, b) => Q.bb_max_t[b] - Q.bb_max_t[a]);
    if (worst.length) hint(p, 'Largest: ' + worst.slice(0, 6).map(r => clockOf(Q.bb_ep[r]) + ' ' + (Q.bb_sta && Q.bb_sta[r] >= 0 ? staLink(Q.bb_sta[r]) : '') + ' ' + (Q.bb_sat && Q.bb_sat[r] >= 0 ? satLink(Q.bb_sat[r]) : '') + ' (' + fmt(Q.bb_max_t[r], 1) + ')').join('; '));
  }
  if (Q.mdb_ep) {
    const p = panel(g2, 'Smallest detectable errors', 'median and 95th percentile per class, scaled by sigma0 (m)');
    const cls = QH.mdb_classes.map((c, i) => [c, i]).filter(([c]) => c.includes('code'));
    const cols = cls.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, cls.length > 1 ? 0.75 * i / (cls.length - 1) : 0.3));
    const series = [];
    cls.forEach(([, ci], n) => QH.mdb_consts.forEach((cn, k) => { series.push({ y: rowsSeries(Q.mdb_ep, Q.mdb_mdb_scaled_med_m, r => Q.mdb_class[r] === ci && Q.mdb_const[r] === k), color: cols[n], connect: true, dash: k ? [4, 3] : null }); }));
    makeChart(canvasIn(p), { series, zeroBased: true, robust: true });
    keyRow(p, cls.map(([c], n) => [cols[n], c.replace('_', ' ')]).concat([[C.muted, 'solid ' + QH.mdb_consts[0] + (QH.mdb_consts[1] ? ', dashed ' + QH.mdb_consts[1] : '')]]));
  }
  if (Q.pr_ep) {
    const p = panel(g2, 'Tests of the carried priors', 'rows above 4 sigma per family');
    const fams = QH.pr_families, cols = fams.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, fams.length > 1 ? 0.85 * i / (fams.length - 1) : 0.3));
    makeChart(canvasIn(p), { series: fams.map((_f, fi) => ({ y: rowsSeries(Q.pr_ep, Q.pr_n_gt_4, r => Q.pr_fam[r] === fi), color: cols[fi], connect: true })), zeroBased: true });
    keyRow(p, fams.map((f, i) => [cols[i], f]));
  }
  if (Q.ra_ep) {
    const p = panel(g2, 'Attribution of large residuals', 'per 10 minutes by kind; a receiver, a satellite, a receiver-satellite arc or a cell of the carried priors');
    const dims = QH.ra_dims, cols = dims.map((_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, dims.length > 1 ? 0.85 * i / (dims.length - 1) : 0.3));
    const bin = Math.max(1, Math.round(600000 / M.step)), bars = [];
    for (let e0 = 0; e0 < ne; e0 += bin) bars.push({ e0, e1: e0 + bin, stack: dims.map((_d, di) => ({ v: 0, color: cols[di] })) });
    for (let r = 0; r < Q.ra_ep.length; r++) bars[Math.floor(Q.ra_ep[r] / bin)].stack[Q.ra_dim[r]].v++;
    makeChart(canvasIn(p), { bars, zeroBased: true });
    keyRow(p, dims.map((d, i) => [cols[i], { SAT: 'satellite', STA: 'receiver', ARC: 'receiver-satellite arc', PCELL: 'carried-prior cell', SIG: 'signal' }[d] || d]));
    const top = new Map();
    for (let r = 0; r < Q.ra_ep.length; r++) { const k = QH.ra_dims[Q.ra_dim[r]] + ' ' + QH.ra_entities[Q.ra_ent[r]]; top.set(k, (top.get(k) || 0) + 1); }
    hint(p, 'Most often: ' + [...top.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8).map(([k, n]) => esc(k) + ' (' + n + ')').join(', '));
  }
  // the model as configured
  const man = EB && EB.H.manifest;
  if (man && man.param_model && man.param_model.kinds) {
    const p = panel(pad, 'Parameters as modelled', 'process noise, its rule and the variance floor per parameter kind (the run manifest)');
    const rows = man.param_model.kinds.map(k => ({ kind: k.kind, est: k.estimated ? 'yes' : 'no', carried: k.carried ? 'yes' : 'no', q: k.q, units: k.q_units || '', rule: k.q_rule || '', tau: k.tau_s, floor: k.var_floor_m2 }));
    dataTable(p, { maxHeight: 300, sort: 'kind', desc: false, rows, cols: [{ key: 'kind', label: 'Kind' }, { key: 'est', label: 'Estimated' }, { key: 'carried', label: 'Carried' },
      { key: 'q', label: 'Process noise', num: true, fmt: v => v == null ? '' : Number(v).toExponential(2) }, { key: 'units', label: 'Units' }, { key: 'rule', label: 'Rule' },
      { key: 'tau', label: 'Correlation time (s)', num: true, fmt: v => v == null ? '' : shortNum(v) }, { key: 'floor', label: 'Variance floor', num: true, fmt: v => v == null ? '' : Number(v).toExponential(2) }] });
  }
  if (man && man.param_responses && man.param_responses.sites) {
    const p = panel(pad, 'Responses to events', 'what each carried parameter does at a slip, a gap or a return, and how often it did');
    const rows = man.param_responses.sites.map(s => ({ site: s.site, kind: s.kind, event: s.event, policy: s.policy, armed: s.armed ? 'yes' : 'no', count: s.count }));
    dataTable(p, { maxHeight: 300, sort: 'count', rows, cols: [{ key: 'kind', label: 'Kind' }, { key: 'event', label: 'Event' }, { key: 'policy', label: 'Response' },
      { key: 'armed', label: 'Armed' }, { key: 'count', label: 'Times', num: true, fmt: v => fmtCount(v) }, { key: 'site', label: 'Where' }] });
  }
  if (EB && EB.H.carry_vce && EB.H.carry_vce.length) {
    const p = panel(pad, 'Measured floors of the carried priors, by hour', 'the floor variance and rate the data show, against the configured floor');
    dataTable(p, { maxHeight: 260, sort: 'hour', desc: false, rows: EB.H.carry_vce.map(r => Object.assign({}, r)), cols: [{ key: 'hour', label: 'Hour', num: true }, { key: 'family', label: 'Family' },
      { key: 'const', label: 'System' }, { key: 'n_rows', label: 'Rows', num: true, fmt: v => fmtCount(v) }, { key: 'robust_floor_m2', label: 'Robust floor (m2)', num: true, fmt: v => v == null ? '' : Number(v).toExponential(2) },
      { key: 'mean_floor_m2', label: 'Mean floor (m2)', num: true, fmt: v => v == null ? '' : Number(v).toExponential(2) }, { key: 'rate_m2ps', label: 'Rate (m2/s)', num: true, fmt: v => v == null ? '' : Number(v).toExponential(2) },
      { key: 'cfg_floor_m2', label: 'Configured floor', num: true, fmt: v => v == null ? '' : Number(v).toExponential(2) }] });
  }
}

registerView('stochastic', {
  title: 'Stochastic model', blocks: ['quality', 'epoch'],
  init() {},
  show() { renderStochastic(viewEl('stochastic')); },
});
