'use strict';
/*
 * Network dashboard: one line of sight, receiver x satellite (#/los/RECEIVER/SAT), the deepest level.
 * Its elevation, the post-fit residual of every signal with its a priori sigma, the slip observables (MW, GF,
 * phase minus code, loss-of-lock flags) with the slips and gaps, the ionosphere-free time-difference test against
 * its threshold, the slant ionosphere, the carried float ambiguities, and the whole numbers of the link (held,
 * decided, released, wide and narrow lane), with every event of the pair.
 * Series come from the line-of-sight block (scripts/netview/layers/los.py) and the core block.
 */

const LV = { j: -1, k: -1, nowTiles: null };

/** The series of one line of sight from the los block, decoded to metres (NaN where absent). */
function losSeries(li) {
  const L = B('los');
  if (!L || li == null) return {};
  const A = L.A, H = L.H, ne = M.nEp, sc = H.scales || {};
  const row = (a, r, n) => a.subarray(r * (n || ne), (r + 1) * (n || ne));
  const out = {};
  const dec = (name, off) => {
    if (!A[name]) return null;
    const y = asFloat(row(A[name], li), sc[name], -32768);
    if (off && A[off]) { const o = A[off][li]; for (let e = 0; e < ne; e++) y[e] += o; }
    return y;
  };
  out.iono = dec('lion');
  out.mw = dec('lmw', 'lmw_off');
  out.gf = dec('lgf', 'lgf_off');
  out.cp = dec('lcp', 'lcp_off');
  out.td = dec('ltd');
  out.thr = dec('ltt');
  out.snorm = dec('lsn');
  if (A.ltf) out.tflag = row(A.ltf, li);
  if (A.llli) out.lli = row(A.llli, li);
  if (A.lres) {
    out.res = [];
    for (let s = 0; s < 5; s++) {
      const sig = A.lsig[li * 5 + s];
      if (sig === 255) continue;
      const r = asFloat(A.lres.subarray((li * 5 + s) * ne, (li * 5 + s + 1) * ne), H.res_scale[s], -32768);
      const sd = new Float32Array(ne);
      const [lo, hi] = H.sd_log;
      for (let e = 0; e < ne; e++) { const c = A.lsd[(li * 5 + s) * ne + e]; sd[e] = c ? Math.pow(10, lo + (c - 1) / 254 * (hi - lo)) : NaN; }
      out.res.push({ slot: s, signal: H.pf_signals[sig], phase: s >= 3, r, sd });
    }
  }
  if (A.lam) {
    out.amb = [];
    for (let s = 0; s < 2; s++) {
      const sig = A.lams[li * 2 + s];
      if (sig === 255) continue;
      const v = new Float32Array(ne), sd = new Float32Array(ne), base = A.lam_off[li * 2 + s];
      const [lo, hi] = H.sd_log;
      for (let e = 0; e < ne; e++) {
        const q = A.lam[(li * 2 + s) * ne + e];
        v[e] = q === -2147483648 ? NaN : q / sc.lam;
        const c = A.lav[(li * 2 + s) * ne + e];
        sd[e] = c ? Math.pow(10, lo + (c - 1) / 254 * (hi - lo)) : NaN;
      }
      out.amb.push({ slot: s, signal: H.amb_signals[sig], v, sd, base });
    }
    out.ambEvents = [];
    for (let i = 0; i < (A.ae_los ? A.ae_los.length : 0); i++) if (A.ae_los[i] === li) out.ambEvents.push({ e: A.ae_ep[i], slot: A.ae_slot[i], what: H.amb_events[A.ae_code[i]] });
  }
  return out;
}

function renderLos(root, j, k) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const sta = M.names[j], sat = M.H.sats[k], ne = M.nEp;
  const li = losIndexMap() ? losIndexMap().get(j * M.nSat + k) : undefined;
  const s = losSeries(li);
  viewHead(pad, sta + ' - ' + sat, 'line of sight: ' + staLink(j) + ' to ' + satLink(k) + ' &middot; <a class="act" href="' +
    hrefOf(['network'], { s: 'link:' + sta + ':' + sat }) + '">this link in the network</a>');
  if (li == null) hint(pad, 'This pair is in no line-of-sight trace of the run (no residuals, slip observables or ionosphere).');
  missingNote(pad, 'los');
  const elev = losElevation(j, k);
  const sel = S.sel;
  const evIdx = M.evBySta[j].filter(i => M.A.ev_sat[i] === k);
  const box = el('div');
  pad.appendChild(box);
  const linkState = e => {
    if (!sel || sel.kind !== 'link') return '';
    const cf = curFamily();
    if (!cf) return '';
    const node = M.nSta + k;
    if (isTreeLink(cf.f.band1, j, node, e) || isTreeLink(cf.f.band2, j, node, e)) return 'tree link';
    const r = sel.link >= 0 ? linkRowAt(sel.link, e) : -1;
    return r >= 0 ? M.H.states[rowInfo(r).state] : 'not in the solution';
  };
  const nowTiles = () => {
    box.innerHTML = '';
    const e = S.ep, items = [];
    items.push({ k: 'Elevation', v: elev && isFinite(elev[e]) ? fmt(elev[e], 0) + ' <small>degrees</small>' : 'not tracked', cls: 'info' });
    items.push({ k: 'Link state', v: '<span style="font-size:14px">' + esc(linkState(e) || 'n/a') + '</span>', cls: 'info' });
    if (s.res) for (const r of s.res.filter(x => x.slot === 0 || x.slot === 3)) items.push({ k: 'Post-fit residual ' + r.signal, v: fmtLen(r.r[e]), s: 'a priori sigma ' + fmtLen(r.sd[e]), cls: 'info' });
    if (s.iono) items.push({ k: 'Slant ionosphere', v: fmtLen(s.iono[e]), cls: 'info' });
    if (s.td) items.push({ k: 'Time-difference test', v: fmtLen(Math.abs(s.td[e])), s: 'threshold ' + fmtLen(s.thr[e]), cls: s.tflag && s.tflag[e] & 1 ? 'bad' : 'ok' });
    items.push({ k: 'Events of the pair', v: String(evIdx.length), cls: 'info' });
    tiles(box, items);
  };
  nowTiles();
  LV.nowTiles = nowTiles;
  const evTicks = test => evIdx.filter(i => test(i)).map(i => ({ e: M.A.ev_ep[i], color: evClass(i) === 'slip' ? C.slip : M.H.lanes[M.A.ev_lane[i]].key === 'gap' ? C.gap : C.release }));
  const slipTicks = evTicks(i => evClass(i) === 'slip' || M.H.lanes[M.A.ev_lane[i]].key === 'gap');
  const g = el('div', 'grid2');
  pad.appendChild(g);
  const chart = (title, spec, keys, parent) => { const p = parent || panel(g, title); makeChart(canvasIn(p), spec); if (keys) keyRow(p, keys); return p; };
  if (elev) chart('Elevation (degrees)', { series: [{ y: elev, color: C.muted, width: 1.4 }], yRange: [0, 90], nTicks: 3, ticks: slipTicks, valueAt: e => fmt(elev[e], 0) + ' degrees' },
    [[C.slip, 'slip'], [C.gap, 'gap or return']]);
  if (s.res && s.res.length) {
    const codes = s.res.filter(r => !r.phase), phases = s.res.filter(r => r.phase);
    const cols = n => Array.from({ length: n }, (_x, i) => rampColor(window.NV_ASSETS.colormaps.viridis, n > 1 ? 0.75 * i / (n - 1) : 0.25));
    if (codes.length) {
      const cc = cols(codes.length);
      chart('Post-fit code residuals (m); dashed: a priori sigma', { series: codes.map((r, i) => ({ y: r.r, color: cc[i], width: 1.2 })).concat(codes.map((r, i) => ({ y: r.sd, color: withAlpha(cc[i], 0.6), dash: [3, 3] }))).concat(codes.map((r, i) => ({ y: r.sd.map(v => -v), color: withAlpha(cc[i], 0.6), dash: [3, 3] }))),
        symmetric: true, robust: true, ticks: slipTicks, valueAt: e => codes.map(r => r.signal + ' ' + fmtLen(r.r[e]) + ' (sigma ' + fmtLen(r.sd[e]) + ')').join(', ') }, codes.map((r, i) => [cc[i], r.signal]));
    }
    if (phases.length) {
      const pc = cols(phases.length);
      chart('Post-fit phase residuals (mm); dashed: a priori sigma', { series: phases.map((r, i) => ({ y: scaled(r.r, 1000), color: pc[i], width: 1.2 })).concat(phases.map((r, i) => ({ y: scaled(r.sd, 1000), color: withAlpha(pc[i], 0.6), dash: [3, 3] }))).concat(phases.map((r, i) => ({ y: scaled(r.sd, -1000), color: withAlpha(pc[i], 0.6), dash: [3, 3] }))),
        symmetric: true, robust: true, ticks: slipTicks, valueAt: e => phases.map(r => r.signal + ' ' + fmtLen(r.r[e]) + ' (sigma ' + fmtLen(r.sd[e]) + ')').join(', ') }, phases.map((r, i) => [pc[i], r.signal]));
    }
  }
  const lliDots = (y) => s.lli ? Array.from(s.lli).map((b, e) => (b && isFinite(y[e]) ? { e, y: y[e], color: C.alarm, r: 3 } : null)).filter(Boolean) : [];
  if (s.mw) chart('Melbourne-Wubbena (m): a step is a slip of the wide lane', { series: [{ y: s.mw, color: C.ink }], robust: true, ticks: slipTicks, dots: lliDots(s.mw), valueAt: e => fmt(s.mw[e], 3) + ' m' },
    [[C.slip, 'slip detected'], [C.gap, 'gap or return'], [C.alarm, 'loss-of-lock flag']]);
  if (s.gf) chart('Geometry-free phase (m): follows the ionosphere; a step is a slip', { series: [{ y: s.gf, color: C.iono }], robust: true, ticks: slipTicks, dots: lliDots(s.gf), valueAt: e => fmt(s.gf[e], 4) + ' m' });
  if (s.cp) chart('Phase minus code on the first signal (m): code noise and multipath', { series: [{ y: s.cp, color: C.float }], robust: true, ticks: slipTicks, valueAt: e => fmt(s.cp[e], 3) + ' m' });
  if (s.td) {
    const dev = s.td.map(Math.abs);
    chart('Ionosphere-free time-difference test: deviation and threshold (m)', { series: [{ y: dev, color: C.ink }, { y: s.thr, color: C.refused, dash: [4, 3] }], zeroBased: true, robust: true,
      dots: Array.from(s.tflag).map((b, e) => (b & 1 ? { e, y: dev[e], color: C.alarm, r: 3.5 } : null)).filter(Boolean),
      valueAt: e => 'deviation ' + fmtLen(dev[e]) + ', threshold ' + fmtLen(s.thr[e]) + (s.tflag[e] & 1 ? ', flagged' : '') }, [[C.ink, 'deviation from the network median'], [C.refused, 'threshold'], [C.alarm, 'flagged']]);
  }
  if (s.iono) chart('Slant ionosphere (m)', { series: [{ y: s.iono, color: C.iono, width: 1.4 }], robust: true, ticks: slipTicks, valueAt: e => fmtLen(s.iono[e]) });
  if (s.snorm) chart('Residual test: normalized residual of this pair', { series: [{ y: s.snorm, color: C.alarm, connect: true }], zeroBased: true, hlines: [{ y: 3, color: C.muted, inRange: true }], valueAt: e => fmt(s.snorm[e], 1) });
  if (s.amb && s.amb.length) {
    const p = panel(g, 'Carried float ambiguities (m, less their median), band = one sigma');
    const ac = s.amb.map((_a, i) => rampColor(window.NV_ASSETS.colormaps.viridis, i ? 0.7 : 0.2));
    makeChart(canvasIn(p, 'chart tall'), { series: s.amb.map((a, i) => ({ y: a.v, color: ac[i], width: 1.3 })),
      bands: s.amb.map((a, i) => ({ lo: a.v.map((v, e) => v - a.sd[e]), hi: a.v.map((v, e) => v + a.sd[e]), color: withAlpha(ac[i], 0.15) })),
      ticks: s.ambEvents.map(x => ({ e: x.e, color: x.what.startsWith('birth') ? C.coderef : C.release })), robust: true,
      valueAt: e => s.amb.map(a => a.signal + ' ' + fmtLen(a.v[e]) + ' (sigma ' + fmtLen(a.sd[e]) + ')').join(', ') });
    keyRow(p, s.amb.map((a, i) => [ac[i], a.signal]).concat([[C.coderef, 'birth'], [C.release, 'dropped or erased']]));
    if (s.ambEvents.length) {
      const groups = new Map();
      for (const x of s.ambEvents) { const k = x.what.replace(/_/g, ' '); const g0 = groups.get(k) || { n: 0, first: x.e, last: x.e }; g0.n++; g0.last = x.e; groups.set(k, g0); }
      hint(p, [...groups.entries()].map(([k, g0]) => esc(k) + ' ' + g0.n + 'x (' + clockOf(g0.first) + (g0.last !== g0.first ? ' to ' + clockOf(g0.last) : '') + ')').join('; '));
    }
  }
  // the whole numbers of the link (the network view's link detail, without its header)
  if (sel && sel.kind === 'link' && M.hasLinks) {
    const p = panel(pad, 'Whole numbers of the link', esc(famLabel(curFamily().f)));
    const codes = linkStrip(sel);
    const counts = [0, 0, 0, 0, 0, 0, 0];
    for (const c of codes) counts[c]++;
    makeChart(chartEl(p, 'State over the run', 'strip', STRIP_LABELS.slice(1).map((t, i) => [stripColors()[i + 1], t]).filter((_x, i) => counts[i + 1])),
      { strip: { codes, colors: stripColors() }, noAxis: false });
    holdHistory(p, sel);
    if (sel.link >= 0) linkCharts(p, sel, curFamily().f);
    treeTargetChart(p, sel, curFamily());
    p.querySelectorAll('[data-ep]').forEach(x => x.addEventListener('click', () => setEp(+x.dataset.ep)));
  }
  const pe = panel(pad, 'Events of this pair', evIdx.length + ' rows');
  eventList(pe, evIdx);
}

registerView('los', {
  title: 'Line of sight', blocks: ['los'],
  init() {},
  selection(args) {
    const j = M.nodeIndex.get(args[0]), k = M.H.sats.indexOf(args[1]);
    if (j == null || j >= M.nSta || k < 0) return null;
    const sys = M.H.sats[k][0];
    if (!curFamily() || curFamily().f.sys !== sys) {
      const fi = M.H.families.findIndex(f => f.sys === sys && f.fixing);
      const fj = fi >= 0 ? fi : M.H.families.findIndex(f => f.sys === sys);
      if (fj >= 0) S.fam = fj;
    }
    return { kind: 'link', sta: j, sat: k, link: M.hasLinks ? linkOf(S.fam, j, k) : -1 };
  },
  crumbs(args) { return [['Receivers', ['receivers']], [args[0] || '', ['receiver', args[0] || '']], [(args[1] || ''), ['los', args[0] || '', args[1] || '']]]; },
  show(args) {
    const j = M.nodeIndex.get(args[0]), k = M.H.sats.indexOf(args[1]);
    const root = viewEl('los');
    if (j == null || j >= M.nSta || k < 0) { root.innerHTML = '<div class="pad">No line of sight ' + esc(args.join(' - ')) + ' in this run.</div>'; return; }
    LV.j = j; LV.k = k;
    renderLos(root, j, k);
  },
  time() { if (LV.nowTiles) LV.nowTiles(); },
});
