'use strict';
/*
 * Network dashboard: the network links view (#/network).  The tree of each signal drawn as rings of hops around its
 * reference receiver (or on a world map), the links held at whole numbers and the float links, and on the right the
 * detail of the selected link, satellite or receiver.  Sections: 4 graph, 7 detail panel, 8 summaries over the run,
 * 9 the view (controls, selection, registration).  Section numbers continue those of core.js.
 */

// ============================================================================================ 4. graph

const G = { nodes: [], links: [], pos: null };

/** The family's links at epoch e: tree links of both signals and the links that are not tree links. */
function collectGraph(e) {
  const cf = curFamily();
  const out = { cf, present: new Uint8Array(M.nNode), depth: new Int8Array(M.nNode).fill(-1),
    root: new Int16Array(M.nNode).fill(-1), links: [], roots: [] };
  if (!cf) return out;
  const { b, bo, fi } = cf;
  const sys = cf.f.sys;
  const famNode = n => n < M.nSta || M.names[n][0] === sys;
  for (let n = 0; n < M.nNode; n++) {
    if (!famNode(n)) continue;
    const p = treeParent(b, n, e);
    if (p !== -1) { out.present[n] = 1; out.depth[n] = treeDepth(b, n, e); if (p === -2) out.roots.push(n); }
    if (treeParent(bo, n, e) !== -1) out.present[n] = 1;
  }
  // tree links of the drawn signal, then those of the other signal only
  for (let n = M.nSta; n < M.nNode; n++) {
    if (!famNode(n)) continue;
    for (const [band, kind] of [[b, 'tree'], [bo, 'treeOther']]) {
      const p = treeParent(band, n, e);
      if (p >= 0) addTreeLink(out, kind, band, p, n, e, b, bo);
    }
  }
  for (let r = 0; r < M.nSta; r++) {
    for (const [band, kind] of [[b, 'tree'], [bo, 'treeOther']]) {
      const p = treeParent(band, r, e);
      if (p >= 0) addTreeLink(out, kind, band, r, p, e, b, bo);
    }
  }
  for (const r of famRowsAt(fi, e)) {
    const l = M.rowLink[r];
    const sta = M.A.link_sta[l], sat = M.nSta + M.A.link_sat[l];
    out.present[sta] = 1; out.present[sat] = 1;
    out.links.push({ a: sta, s: sat, kind: stateKind(M.A.ce_state[r]), row: r, link: l });
  }
  // component of each node (its reference receiver in the drawn signal)
  for (let n = 0; n < M.nNode; n++) {
    if (out.depth[n] < 0) continue;
    const path = pathToRoot(b, n, e);
    out.root[n] = path[path.length - 1];
  }
  inwardCounts(out);
  return out;
}

function addTreeLink(out, kind, band, sta, sat, e, b, bo) {
  if (kind === 'treeOther' && isTreeLink(b, sta, sat, e)) return;
  const both = kind === 'tree' && isTreeLink(bo, sta, sat, e);
  out.links.push({ a: sta, s: sat, kind, both, band, row: -1, link: -1 });
}

/** Per node: links towards the reference (to nodes of fewer hops) and how many of them are whole numbers. */
function inwardCounts(gr) {
  gr.inTotal = new Int16Array(M.nNode);
  gr.inWhole = new Int16Array(M.nNode);
  const whole = k => k === 'tree' || k === 'held' || k === 'decided';
  for (const lk of gr.links) {
    for (const [n, m] of [[lk.a, lk.s], [lk.s, lk.a]]) {
      const dn = gr.depth[n], dm = gr.depth[m];
      if (dn < 0 || dm < 0 || dm >= dn) continue;
      gr.inTotal[n]++;
      if (whole(lk.kind)) gr.inWhole[n]++;
    }
  }
}

function bearing(lat1, lon1, lat2, lon2) {
  const r = Math.PI / 180, p1 = lat1 * r, p2 = lat2 * r, dl = (lon2 - lon1) * r;
  return Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl));
}
function hashAngle(n) { return ((n * 2654435761) % 6283) / 1000; }

/** Spread sorted angles on a circle so that neighbours sit at least minSep apart (order kept). */
function spreadAngles(items, minSep) {
  const n = items.length;
  if (n < 2) return;
  items.sort((p, q) => p.ang - q.ang);
  minSep = Math.min(minSep, 2 * Math.PI / n * 0.92);
  for (let it = 0; it < 60; it++) {
    let moved = false;
    for (let i = 0; i < n; i++) {
      const j = (i + 1) % n;
      let gap = items[j].ang - items[i].ang;
      if (j === 0) gap += 2 * Math.PI;
      if (gap < minSep - 1e-9) {
        const d = (minSep - gap) / 2;
        items[i].ang -= d; items[j].ang += d; moved = true;
      }
    }
    if (!moved) break;
  }
}

function satCentroid(gr, s) {
  let x = 0, y = 0, z = 0, k = 0;
  for (const lk of gr.links) {
    if (lk.s !== s || !isFinite(M.lat[lk.a])) continue;
    const la = M.lat[lk.a] * Math.PI / 180, lo = M.lon[lk.a] * Math.PI / 180;
    x += Math.cos(la) * Math.cos(lo); y += Math.cos(la) * Math.sin(lo); z += Math.sin(la); k++;
  }
  if (!k) return null;
  return { lat: Math.atan2(z, Math.hypot(x, y)) * 180 / Math.PI, lon: Math.atan2(y, x) * 180 / Math.PI };
}

/**
 * The hops a ring picture shows for signal b: the depth that 99 % of the run's node-epochs do not exceed
 * (deeper nodes sit on its ring, labelled "or more"), so a rare deep excursion does not shrink every ring.
 */
function ringDepth(b) {
  if (b < 0 || !M.hasTree) return 1;
  M.ringDepthCache = M.ringDepthCache || {};
  if (M.ringDepthCache[b] == null) {
    const hist = new Int32Array(128), base = b * M.nNode * M.nEp, d = M.A.tree_depth;
    let n = 0;
    for (let i = 0; i < M.nNode * M.nEp; i++) { const v = d[base + i]; if (v > 0) { hist[v]++; n++; } }
    let acc = 0, k = 1;
    for (; k < 128; k++) { acc += hist[k]; if (acc >= 0.99 * n) break; }
    M.ringDepthCache[b] = Math.max(2, k);
  }
  return M.ringDepthCache[b];
}

/** Direction of every present node as seen from the reference receiver (radians from north). */
function nodeBearings(gr, main) {
  const rlat = main >= 0 && main < M.nSta ? M.lat[main] : NaN, rlon = main >= 0 && main < M.nSta ? M.lon[main] : NaN;
  const ang = new Float64Array(M.nNode).fill(NaN);
  for (let n = 0; n < M.nSta; n++) {
    if (!gr.present[n]) continue;
    ang[n] = isFinite(rlat) && isFinite(M.lat[n]) && n !== main ? bearing(rlat, rlon, M.lat[n], M.lon[n]) : hashAngle(n);
  }
  // a satellite looks the way of the receivers that see it
  const sx = new Float64Array(M.nNode), sy = new Float64Array(M.nNode);
  for (const lk of gr.links) {
    if (!isFinite(ang[lk.a]) || lk.a === main) continue;
    sx[lk.s] += Math.cos(ang[lk.a]); sy[lk.s] += Math.sin(ang[lk.a]);
  }
  for (let n = M.nSta; n < M.nNode; n++) if (gr.present[n]) ang[n] = (sx[n] || sy[n]) ? Math.atan2(sy[n], sx[n]) : hashAngle(n);
  return ang;
}

/**
 * Radial tree: the reference receiver at the centre, each node on the ring of its hops, and each subtree in a
 * wedge as wide as its number of leaves; children run clockwise in the order of their direction from the
 * reference receiver.  Tree links never cross.  Nodes off the main tree go on the outer ring.
 */
function layoutRings(gr, w, h) {
  const pos = new Array(M.nNode).fill(null);
  const cx = w / 2, cy = h / 2 + 6, R = Math.max(60, Math.min(w, h) / 2 - 30);
  const b = gr.cf ? gr.cf.b : -1;
  const Dh = ringDepth(b), D = Dh + 1;
  const size = new Map();
  for (let n = 0; n < M.nNode; n++) if (gr.root[n] >= 0) size.set(gr.root[n], (size.get(gr.root[n]) || 0) + 1);
  let main = -1, best = -1;
  for (const [rt, c] of size) if (c > best) { best = c; main = rt; }
  gr.mainRoot = main;
  const ang = nodeBearings(gr, main);
  const kids = Array.from({ length: M.nNode }, () => []);
  for (let n = 0; n < M.nNode; n++) {
    if (gr.root[n] !== main || n === main) continue;
    const p = treeParent(b, n, S.ep);
    if (p >= 0) kids[p].push(n);
  }
  const leaves = new Float64Array(M.nNode);
  const countLeaves = n => {
    let s = 0;
    for (const c of kids[n]) s += countLeaves(c);
    leaves[n] = Math.max(1, s);
    return leaves[n];
  };
  const placed = new Uint8Array(M.nNode);
  const place = (n, a0, a1) => {
    const a = (a0 + a1) / 2, d = Math.min(gr.depth[n], Dh), r = ringRadius(R, d, D);
    pos[n] = { x: cx + r * Math.sin(a), y: cy - r * Math.cos(a), ring: d, ang: a };
    placed[n] = 1;
    const ks = kids[n].slice().sort((p, q) => ang[p] - ang[q]);
    let t = a0;
    for (const c of ks) { const span = (a1 - a0) * leaves[c] / leaves[n]; place(c, t, t + span); t += span; }
  };
  if (main >= 0) {
    countLeaves(main);
    pos[main] = { x: cx, y: cy, ring: 0, ang: 0 };
    placed[main] = 1;
    const ks = kids[main].slice().sort((p, q) => ang[p] - ang[q]);
    let t = ks.length ? ang[ks[0]] - Math.PI * leaves[ks[0]] / leaves[main] : 0;
    for (const c of ks) { const span = 2 * Math.PI * leaves[c] / leaves[main]; place(c, t, t + span); t += span; }
  }
  const outer = [];
  for (let n = 0; n < M.nNode; n++) if (gr.present[n] && !placed[n]) outer.push({ n, ang: ang[n] });
  const ro = ringRadius(R, D, D);
  spreadAngles(outer, 15 / ro);
  for (const it of outer) pos[it.n] = { x: cx + ro * Math.sin(it.ang), y: cy - ro * Math.cos(it.ang), ring: D, ang: it.ang };
  G.ringGeom = { cx, cy, R, D, Dh, deeper: maxDepth(b) > Dh };
  return pos;
}

/** Radius of ring d (D = the outer ring): mildly compressed outward, where the deep rings hold few nodes. */
function ringRadius(R, d, D) { return R * Math.pow(d / D, 0.6); }

function mapRect(w, h) {
  const m = 14;
  let mw = w - 2 * m, mh = mw / 2;
  if (mh > h - 2 * m) { mh = h - 2 * m; mw = mh * 2; }
  return { x0: (w - mw) / 2, y0: (h - mh) / 2, w: mw, h: mh };
}
function mapXY(rc, lat, lon) { return { x: rc.x0 + (lon + 180) / 360 * rc.w, y: rc.y0 + (90 - lat) / 180 * rc.h }; }

function layoutMap(gr, w, h) {
  const rc = mapRect(w, h), pos = new Array(M.nNode).fill(null);
  for (let n = 0; n < M.nSta; n++) if (gr.present[n] && isFinite(M.lat[n])) pos[n] = mapXY(rc, M.lat[n], M.lon[n]);
  const sl = M.A.sat_lat, so = M.A.sat_lon;
  for (let n = M.nSta; n < M.nNode; n++) {
    if (!gr.present[n]) continue;
    const i = (n - M.nSta) * M.nEp + S.ep;
    const c = sl && isFinite(sl[i]) ? { lat: sl[i], lon: so[i] } : satCentroid(gr, n);
    if (c) pos[n] = mapXY(rc, c.lat, c.lon);
  }
  G.mapRect = rc;
  let main = -1, best = -1;
  const size = new Map();
  for (let n = 0; n < M.nNode; n++) if (gr.root[n] >= 0) size.set(gr.root[n], (size.get(gr.root[n]) || 0) + 1);
  for (const [rt, c] of size) if (c > best) { best = c; main = rt; }
  gr.mainRoot = main;
  return pos;
}

function releasesAt(e, fi) {
  const out = [];
  if (!M.nEv) return out;
  const A = M.A, rel = M.H.lanes.findIndex(l => l.key === 'release');
  const fam = M.H.families[fi];
  for (let i = M.evOff[e]; i < M.evOff[e + 1]; i++) {
    if (A.ev_lane[i] !== rel || A.ev_sta[i] < 0 || A.ev_sat[i] < 0) continue;
    if (M.H.sats[A.ev_sat[i]][0] !== fam.sys) continue;
    out.push({ a: A.ev_sta[i], s: M.nSta + A.ev_sat[i], cause: M.H.events.actions[A.ev_action[i]] });
  }
  return out;
}
function alarmNodesNear(e) {
  const set = new Set();
  if (!M.nEv) return set;
  const A = M.A, al = M.H.lanes.findIndex(l => l.key === 'alarm');
  for (let k = Math.max(0, e - 2); k <= e; k++) {
    for (let i = M.evOff[k]; i < M.evOff[k + 1]; i++) {
      if (A.ev_lane[i] !== al) continue;
      if (A.ev_sta[i] >= 0) set.add(A.ev_sta[i]);
      if (A.ev_sat[i] >= 0) set.add(M.nSta + A.ev_sat[i]);
    }
  }
  return set;
}

function drawGraph() {
  const cv = document.getElementById('graph');
  const { ctx, w, h } = fitCanvas(cv);
  if (!w || !h) return;
  ctx.clearRect(0, 0, w, h);
  G.nodes = []; G.links = [];
  if (!M.hasTree && !M.hasLinks) {
    ctx.fillStyle = C.muted; ctx.font = '14px system-ui, sans-serif'; ctx.textAlign = 'center';
    ctx.fillText('This run has no tree trace: the network cannot be drawn.', w / 2, h / 2 - 10);
    ctx.fillText('See the note above the network for the key that writes it.', w / 2, h / 2 + 12);
    return;
  }
  const gr = collectGraph(S.ep);
  const pos = S.layout === 'map' ? layoutMap(gr, w, h) : layoutRings(gr, w, h);
  G.gr = gr; G.pos = pos;
  if (S.layout === 'map') drawCoast(ctx); else drawRingGuides(ctx);
  const sel = S.sel;
  const selNodes = new Set();
  if (sel && (sel.kind === 'sta' || sel.kind === 'sat')) selNodes.add(sel.node);
  if (sel && sel.kind === 'link') { selNodes.add(sel.sta); selNodes.add(M.nSta + sel.sat); }
  const fade = sel && (sel.kind === 'sta' || sel.kind === 'sat');
  const order = { float: 0, refused: 1, treeOther: 2, held: 3, decided: 4, tree: 5 };
  const links = gr.links.slice().sort((p, q) => order[p.kind] - order[q.kind]);
  for (const lk of links) {
    const pa = pos[lk.a], ps = pos[lk.s];
    if (!pa || !ps) continue;
    if (!linkShown(lk)) continue;
    const touches = selNodes.has(lk.a) || selNodes.has(lk.s);
    const alpha = fade && !touches ? 0.12 : 1;
    styleLink(ctx, lk, alpha);
    ctx.beginPath(); ctx.moveTo(pa.x, pa.y); ctx.lineTo(ps.x, ps.y); ctx.stroke();
    ctx.setLineDash([]);
    G.links.push({ lk, x1: pa.x, y1: pa.y, x2: ps.x, y2: ps.y });
  }
  if (sel) drawSelection(ctx, gr, pos, sel);
  for (const rl of releasesAt(S.ep, S.fam)) {
    const pa = pos[rl.a], ps = pos[rl.s];
    if (!pa || !ps) continue;
    const mx = (pa.x + ps.x) / 2, my = (pa.y + ps.y) / 2;
    ctx.strokeStyle = C.release; ctx.lineWidth = 2.2;
    ctx.beginPath(); ctx.moveTo(mx - 5, my - 5); ctx.lineTo(mx + 5, my + 5); ctx.moveTo(mx + 5, my - 5); ctx.lineTo(mx - 5, my + 5); ctx.stroke();
  }
  const alarms = alarmNodesNear(S.ep);
  for (let n = 0; n < M.nNode; n++) {
    const p = pos[n];
    if (!p) continue;
    drawNode(ctx, gr, n, p, fade && !selNodes.has(n) && !linkedToSel(gr, n, selNodes), alarms.has(n));
    G.nodes.push({ n, x: p.x, y: p.y });
  }
  if (S.showLabels) drawLabels(ctx, gr, pos, fade, selNodes);
  updateGraphKey(gr);
}

/** The checkboxes: float links and held links can be hidden (links of the selected node always show). */
function linkShown(lk) {
  const sel = S.sel;
  if (sel && (sel.kind === 'sta' || sel.kind === 'sat') && (lk.a === sel.node || lk.s === sel.node)) return true;
  if (sel && sel.kind === 'link' && lk.a === sel.sta && lk.s === M.nSta + sel.sat) return true;
  if (lk.kind === 'float' || lk.kind === 'refused') return S.showFloat;
  if (lk.kind === 'held' || lk.kind === 'decided') return S.showHeld;
  return true;
}

function linkedToSel(gr, n, selNodes) {
  for (const lk of gr.links) if ((lk.a === n && selNodes.has(lk.s)) || (lk.s === n && selNodes.has(lk.a))) return true;
  return false;
}

function styleLink(ctx, lk, alpha) {
  let c = C.float, wd = 0.8, dash = [];
  switch (lk.kind) {
    case 'tree': c = C.tree; wd = 1.9; break;
    case 'treeOther': c = C.tree; wd = 1.1; dash = [5, 4]; break;
    case 'held': c = C.held; wd = 1.7; break;
    case 'decided': c = C.decided; wd = 2.6; break;
    case 'refused': c = C.refused; wd = 0.9; alpha *= 0.55; break;
    default: c = C.float; wd = 0.7; alpha *= 0.4;
  }
  ctx.strokeStyle = withAlpha(c, alpha);
  ctx.lineWidth = wd;
  ctx.setLineDash(dash);
}

function drawSelection(ctx, gr, pos, sel) {
  const cf = gr.cf;
  const paths = [];
  if (sel.kind === 'link') paths.push(pathToRoot(cf.b, sel.sta, S.ep), pathToRoot(cf.b, M.nSta + sel.sat, S.ep));
  if (sel.kind === 'sta' || sel.kind === 'sat') paths.push(pathToRoot(cf.b, sel.node, S.ep));
  ctx.strokeStyle = withAlpha(C.sel, 0.85); ctx.lineWidth = 5; ctx.lineCap = 'round';
  for (const p of paths) {
    ctx.beginPath();
    let started = false;
    for (const n of p) {
      const q = pos[n];
      if (!q) continue;
      if (!started) { ctx.moveTo(q.x, q.y); started = true; } else ctx.lineTo(q.x, q.y);
    }
    ctx.stroke();
  }
  if (sel.kind === 'link') {
    const pa = pos[sel.sta], ps = pos[M.nSta + sel.sat];
    if (pa && ps) {
      ctx.strokeStyle = C.sel; ctx.lineWidth = 7; ctx.globalAlpha = 0.55;
      ctx.beginPath(); ctx.moveTo(pa.x, pa.y); ctx.lineTo(ps.x, ps.y); ctx.stroke();
      ctx.globalAlpha = 1;
    }
  }
  ctx.lineCap = 'butt';
}

function drawNode(ctx, gr, n, p, faded, alarm) {
  const isSat = n >= M.nSta;
  const root = gr.depth[n] === 0;
  const share = gr.inTotal[n] ? gr.inWhole[n] / gr.inTotal[n] : NaN;
  const r = root ? 8 : 5.5;
  ctx.globalAlpha = faded ? 0.25 : 1;
  ctx.fillStyle = root ? C.root : (gr.inTotal[n] ? seqColor(share) : C.panel);
  ctx.strokeStyle = gr.depth[n] < 0 ? C.muted : C.ink;
  ctx.lineWidth = 1.1;
  ctx.setLineDash(gr.depth[n] < 0 || (gr.mainRoot >= 0 && gr.root[n] !== gr.mainRoot) ? [2, 2] : []);
  ctx.beginPath();
  if (!isSat) ctx.arc(p.x, p.y, r, 0, 2 * Math.PI);
  else if (M.names[n][0] === 'E') { ctx.moveTo(p.x, p.y - r - 1); ctx.lineTo(p.x + r + 1, p.y); ctx.lineTo(p.x, p.y + r + 1); ctx.lineTo(p.x - r - 1, p.y); ctx.closePath(); }
  else ctx.rect(p.x - r, p.y - r, 2 * r, 2 * r);
  ctx.fill(); ctx.stroke();
  ctx.setLineDash([]);
  if (root) { ctx.beginPath(); ctx.arc(p.x, p.y, r + 3.5, 0, 2 * Math.PI); ctx.stroke(); }
  if (alarm) {
    ctx.strokeStyle = C.alarm; ctx.lineWidth = 2.2;
    ctx.beginPath(); ctx.arc(p.x, p.y, r + 5, 0, 2 * Math.PI); ctx.stroke();
  }
  if (S.hover && S.hover.kind === 'node' && S.hover.node === n) {
    ctx.strokeStyle = C.sel; ctx.lineWidth = 2.5;
    ctx.beginPath(); ctx.arc(p.x, p.y, r + 3, 0, 2 * Math.PI); ctx.stroke();
  }
  ctx.globalAlpha = 1;
}

function drawLabels(ctx, gr, pos, fade, selNodes) {
  ctx.font = '10.5px system-ui, sans-serif';
  ctx.textBaseline = 'middle';
  for (let n = 0; n < M.nNode; n++) {
    const p = pos[n];
    if (!p) continue;
    if (S.showLabels === 'sat' && n < M.nSta && n !== gr.mainRoot) continue;   // a still's choice: satellites and the reference only
    const label = n < M.nSta && n !== gr.mainRoot ? shortSta(M.names[n]) : M.names[n];
    ctx.globalAlpha = fade && !selNodes.has(n) ? 0.3 : 0.9;
    ctx.fillStyle = n < M.nSta ? C.muted : C.ink;
    let dx = 9, dy = 0;
    if (S.layout !== 'map' && p.ring > 0) {
      const ux = Math.sin(p.ang), uy = -Math.cos(p.ang);
      dx = ux * 11; dy = uy * 11;
      ctx.textAlign = ux > 0.3 ? 'left' : ux < -0.3 ? 'right' : 'center';
    } else {                                     // a name that would run off the right edge goes to the left of its node
      const off = p.x + dx + ctx.measureText(label).width > ctx.canvas.clientWidth;
      ctx.textAlign = off ? 'right' : 'left';
      if (off) dx = -dx;
    }
    if (n === gr.mainRoot && S.layout !== 'map') { dx = 0; dy = 20; ctx.textAlign = 'center'; ctx.fillStyle = C.ink; }
    ctx.fillText(label, p.x + dx, p.y + dy);
  }
  ctx.globalAlpha = 1;
}

function drawRingGuides(ctx) {
  const g = G.ringGeom;
  if (!g) return;
  ctx.strokeStyle = C.grid; ctx.lineWidth = 1; ctx.fillStyle = C.muted;
  ctx.font = '10px system-ui, sans-serif'; ctx.textAlign = 'left'; ctx.textBaseline = 'bottom';
  for (let d = 1; d <= g.D; d++) {
    const r = ringRadius(g.R, d, g.D);
    ctx.setLineDash(d === g.D ? [3, 4] : []);
    ctx.beginPath(); ctx.arc(g.cx, g.cy, r, 0, 2 * Math.PI); ctx.stroke();
    const label = d === g.D ? 'off the main tree' : d + (d === 1 ? ' hop' : ' hops') + (d === g.Dh && g.deeper ? ' or more' : '');
    ctx.fillText(label, g.cx + 3, g.cy - r - 1);
  }
  ctx.setLineDash([]);
}

function drawCoast(ctx) {
  const rc = G.mapRect, cl = window.NV_ASSETS.coastline;
  ctx.fillStyle = C.soft; ctx.fillRect(rc.x0, rc.y0, rc.w, rc.h);
  if (!cl) return;
  ctx.strokeStyle = C.line; ctx.lineWidth = 0.8; ctx.beginPath();
  let pen = false;
  for (let i = 0; i < cl.lon.length; i++) {
    if (cl.lon[i] == null) { pen = false; continue; }
    const p = mapXY(rc, cl.lat[i], cl.lon[i]);
    if (!pen) { ctx.moveTo(p.x, p.y); pen = true; } else ctx.lineTo(p.x, p.y);
  }
  ctx.stroke();
}

function updateGraphKey(gr) {
  const el = document.getElementById('graph-key');
  const cf = gr.cf;
  if (!cf) { el.innerHTML = ''; return; }
  const counts = { tree: 0, treeOther: 0, held: 0, decided: 0, refused: 0, float: 0 };
  for (const lk of gr.links) counts[lk.kind]++;
  const ramp = [0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1].map(seqColor).join(',');
  let html = '<div><span class="sw" style="border-color:' + C.tree + '"></span>tree link of ' + esc(cf.sig) + ' (' + counts.tree + ')</div>' +
    '<div><span class="sw" style="border-color:' + C.tree + ';border-top-style:dashed"></span>tree link of the other signal only (' + counts.treeOther + ')</div>';
  if (cf.f.fixing) {
    html += '<div><span class="sw" style="border-color:' + C.held + '"></span>held at whole numbers (' + counts.held + ')' +
      (counts.decided ? ', decided now (' + counts.decided + ')' : '') + '</div>' +
      '<div><span class="sw" style="border-color:' + C.refused + '"></span>float, not yet held (' + counts.refused + ')</div>';
  } else {
    html += '<div><span class="sw" style="border-color:' + C.float + '"></span>float, no fixing on these signals (' + counts.float + ')</div>';
  }
  html += '<div>node colour: share of its links towards the reference that are whole numbers <span class="bar" style="background:linear-gradient(90deg,' + ramp + ')"></span>0 to 1</div>' +
    '<div>&#9675; receiver &nbsp; &#9633; GPS &nbsp; &#9671; Galileo &nbsp; red ring: alarm in the last minute &nbsp; &#10005;: released now</div>';
  el.innerHTML = html;
}

function hitTest(x, y) {
  let best = null, bd = 10;
  for (const nd of G.nodes) {
    const d = Math.hypot(nd.x - x, nd.y - y);
    if (d < bd) { bd = d; best = { kind: 'node', node: nd.n }; }
  }
  if (best) return best;
  bd = 4.5;
  for (const g of G.links) {
    const d = segDist(x, y, g.x1, g.y1, g.x2, g.y2);
    if (d < bd) { bd = d; best = { kind: 'link', lk: g.lk }; }
  }
  return best;
}
function segDist(px, py, x1, y1, x2, y2) {
  const dx = x2 - x1, dy = y2 - y1, L = dx * dx + dy * dy;
  let t = L ? ((px - x1) * dx + (py - y1) * dy) / L : 0;
  t = Math.max(0, Math.min(1, t));
  return Math.hypot(px - x1 - t * dx, py - y1 - t * dy);
}


// ============================================================================================ 7. detail panel

function eventText(i) {
  const A = M.A, E = M.H.events;
  const tier = E.tiers[A.ev_tier[i]], name = E.names[A.ev_name[i]], act = E.actions[A.ev_action[i]];
  let t = EVENT_TEXT[tier + '/' + name];
  if (tier === 'RINGFIX' && name === 'release') t = 'released: ' + (RELEASE_TEXT[act] || act);
  return t || (tier + ' ' + name + (act ? ' (' + act + ')' : ''));
}
function rawDetail(i) {
  const d = M.H.events.details[M.A.ev_detail[i]] || '';
  return d.replace(/wire/gi, 'stream');
}

function eventRows(idx, limit) {
  const A = M.A, other = M.H.lanes.findIndex(l => l.key === 'other');
  const nOther = idx.filter(i => A.ev_lane[i] === other).length;
  if (!S.showOther) idx = idx.filter(i => A.ev_lane[i] !== other);
  const toggle = nOther ? '<div class="hint"><span class="act" data-toggle-other="1">' + (S.showOther ? 'hide' : 'show') +
    ' the ' + nOther + ' rows of other checks</span> (outlier tests, bookkeeping, wet delay carry)</div>' : '';
  const rows = idx.slice(0, limit || 400).map(i => {
    const sta = A.ev_sta[i] >= 0 ? M.names[A.ev_sta[i]] : '';
    const sat = A.ev_sat[i] >= 0 ? M.H.sats[A.ev_sat[i]] : '';
    const band = A.ev_band[i] >= 0 ? M.H.bands[A.ev_band[i]].split(':')[1] : '';
    const who = [sta, sat].filter(Boolean).join(' - ') + (band ? ' ' + band : '');
    return '<div data-ev="' + i + '"><span class="lane-dot" style="background:' + laneColor(A.ev_lane[i]) + '"></span>' +
      '<span class="t">' + clockOf(A.ev_ep[i]) + '</span>' + esc(eventText(i)) + (who ? ' <b>' + esc(who) + '</b>' : '') +
      '<div class="raw">' + esc(M.H.events.tiers[A.ev_tier[i]] + ' ' + M.H.events.names[A.ev_name[i]] + ' ' +
      M.H.events.actions[A.ev_action[i]] + ': ' + rawDetail(i)) + '</div></div>';
  });
  const more = idx.length > (limit || 400) ? '<div class="hint">' + (idx.length - (limit || 400)) + ' more not listed</div>' : '';
  return toggle + '<div class="evlist">' + (rows.join('') || '<div class="hint">none</div>') + more + '</div>';
}

function renderDetail() {
  const el = document.getElementById('detail');
  el.innerHTML = '';
  const sel = S.sel;
  if (!sel) renderOverview(el);
  else if (sel.kind === 'link') renderLink(el, sel);
  else if (sel.kind === 'sat') renderSat(el, sel);
  else if (sel.kind === 'sta') renderSta(el, sel);
  else if (sel.kind === 'events') renderEvents(el, sel);
  const open = !sel ? '' : sel.kind === 'link' ? linkHtml(['los', M.names[sel.sta], M.H.sats[sel.sat]], 'Open the page of this line of sight')
    : sel.kind === 'sta' ? linkHtml(['receiver', M.names[sel.node]], 'Open the receiver page')
      : sel.kind === 'sat' ? linkHtml(['satellite', M.names[sel.node]], 'Open the satellite page') : '';
  if (open) el.insertAdjacentHTML('afterbegin', '<div class="openpage">' + open + ' &rsaquo;</div>');
  wireDetailClicks(el);
  updateDetailNow();
}

function nowBox(el) {
  const d = document.createElement('div');
  d.id = 'now-box';
  el.appendChild(d);
  return d;
}

function updateDetailNow() {
  const box = document.getElementById('now-box');
  if (!box) return;
  const sel = S.sel;
  let html = '';
  if (!sel) html = overviewNow();
  else if (sel.kind === 'link') html = linkNow(sel);
  else if (sel.kind === 'sat') html = satNow(sel);
  else if (sel.kind === 'sta') html = staNow(sel);
  box.innerHTML = html;
  wireDetailClicks(box);
}

// --- overview

function renderOverview(el) {
  const H = M.H, d = gpsDate(0);
  el.insertAdjacentHTML('beforeend', '<h2>' + esc(H.run) + '</h2><div class="sub">' +
    d.toISOString().slice(0, 10) + ' (day ' + dayOfYear(d) + ' of ' + d.getUTCFullYear() + '), ' + M.nEp + ' epochs over ' +
    durationText(M.nEp) + ', ' + M.nSta + ' receivers, ' + M.nSat + ' satellites</div>');
  nowBox(el);
  hint(el, 'Click a link, a satellite or a receiver in the network to see its history; click a lane of the timeline ' +
    'to jump to its events. Arrow keys step one epoch (with Shift ten), the space bar plays. The yellow path is the chain ' +
    'of tree links from the selected node to the reference receiver.');
  const fams = H.families.map((f, i) => '<tr><td><span class="act" data-fam="' + i + '">' + esc(famLabel(f)) + '</span></td><td>' +
    (f.fixing ? 'held (' + f.held_link_epochs.toLocaleString() + ' link-epochs held)' : 'floats only') + '</td></tr>').join('');
  el.insertAdjacentHTML('beforeend', '<h3>Signal families</h3><table>' + fams + '</table>');
  const cfg = H.config || {};
  if (cfg.ring_fix && Object.keys(cfg.ring_fix).length) {
    el.insertAdjacentHTML('beforeend', '<h3>Fixing settings (from ' + esc(cfg.file || 'the config') + ')</h3><table class="kv">' +
      Object.entries(cfg.ring_fix).map(([k, v]) => '<tr><td>' + esc(k) + '</td><td>' + esc(JSON.stringify(v)) + '</td></tr>').join('') +
      '</table>');
  }
  const au = H.audits || {};
  if (au.right || au.wrong) {
    hint(el, 'Held whole numbers checked against the integers of the run\'s audit file: ' + (au.right || 0).toLocaleString() +
      ' link-epochs right, ' + (au.wrong || 0).toLocaleString() + ' wrong' +
      (au.unlisted ? ', ' + au.unlisted.toLocaleString() + ' not in the file' : '') + '.');
  }
}

function overviewNow() {
  const sl = M.A.sum_links;
  if (!sl) return '';
  const ne = M.nEp, nf = M.nFam;
  const rows = M.H.families.map((f, i) => {
    const n = sl[i * ne + S.ep], held = sl[(nf + i) * ne + S.ep], ref = sl[(3 * nf + i) * ne + S.ep];
    return '<tr><td>' + esc(famLabel(f)) + '</td><td class="num">' + n + '</td><td class="num">' + (f.fixing ? held : '') +
      '</td><td class="num">' + (f.fixing ? (n ? Math.round(100 * held / n) + ' %' : '') : '') + '</td><td class="num">' +
      (f.fixing ? ref : '') + '</td></tr>';
  }).join('');
  return '<h3>At ' + clockOf(S.ep) + ': links that are not tree links</h3><table><tr><th>signals</th><th class="num">links</th>' +
    '<th class="num">held</th><th class="num">share</th><th class="num">not yet</th></tr>' + rows + '</table>';
}

// --- link

function linkStrip(sel) {
  const cf = curFamily(), A = M.A;
  const codes = new Uint8Array(M.nEp);
  const s = M.nSta + sel.sat;
  for (let e = 0; e < M.nEp; e++) {
    const t1 = isTreeLink(cf.f.band1, sel.sta, s, e), t2 = isTreeLink(cf.f.band2, sel.sta, s, e);
    if (t1 && t2) { codes[e] = 1; continue; }
    if (t1 || t2) { codes[e] = 2; continue; }
    const r = sel.link >= 0 ? linkRowAt(sel.link, e) : -1;
    if (r < 0) continue;
    const k = stateKind(A.ce_state[r]);
    codes[e] = k === 'held' ? 3 : k === 'decided' ? 4 : k === 'refused' ? 5 : 6;
  }
  return codes;
}
const STRIP_LABELS = ['absent', 'tree link of both signals', 'tree link of one signal', 'held', 'decided', 'float, not yet held', 'float'];
function stripColors() { return [null, C.tree, withAlpha(C.tree, 0.45), C.held, C.decided, C.refused, C.float]; }

function renderLink(el, sel) {
  const cf = curFamily();
  if (!cf) { el.insertAdjacentHTML('beforeend', '<div class="hint">No signal family with links in this run.</div>'); return; }
  const f = cf.f;
  const staName = M.names[sel.sta], satName = M.H.sats[sel.sat];
  el.insertAdjacentHTML('beforeend', '<h2>Link <span class="act" data-node="' + sel.sta + '">' + esc(staName) + '</span> - ' +
    '<span class="act" data-node="' + (M.nSta + sel.sat) + '">' + esc(satName) + '</span></h2><div class="sub">' +
    esc(famLabel(f)) + '</div>');
  nowBox(el);
  const codes = linkStrip(sel);
  const counts = [0, 0, 0, 0, 0, 0, 0];
  for (const c of codes) counts[c]++;
  makeChart(chartEl(el, 'Over the run', 'strip', STRIP_LABELS.slice(1).map((t, i) => [stripColors()[i + 1], t]).filter((_x, i) => counts[i + 1])),
    { strip: { codes, colors: stripColors() }, noAxis: false });
  hint(el, ['tree link', 'held', 'float'].map((t, k) => {
    const n = k === 0 ? counts[1] + counts[2] : k === 1 ? counts[3] + counts[4] : counts[5] + counts[6];
    return t + ' ' + durationText(n);
  }).join(', ') + '.');
  holdHistory(el, sel);
  if (sel.link >= 0) linkCharts(el, sel, f);
  treeTargetChart(el, sel, cf);
  const elev = losElevation(sel.sta, sel.sat);
  if (elev) makeChart(chartEl(el, 'Elevation of the satellite seen from the receiver (degrees)', '', [[C.muted, 'elevation']]),
    { series: [{ y: elev, color: C.muted, width: 1.4 }], yRange: [0, 90], nTicks: 3, valueAt: e => fmt(elev[e], 0) + ' degrees' });
  const ev = selectionEvents();
  el.insertAdjacentHTML('beforeend', '<h3>Events of this link (' + (ev ? ev.idx.length : 0) + ')</h3>' + eventRows(ev ? ev.idx : []));
}

function linkCharts(el, sel, f) {
  const A = M.A, ne = M.nEp;
  const wl = new Float32Array(ne).fill(NaN), nl = new Float32Array(ne).fill(NaN);
  const wlLo = new Float32Array(ne).fill(NaN), wlHi = new Float32Array(ne).fill(NaN);
  const nlLo = new Float32Array(ne).fill(NaN), nlHi = new Float32Array(ne).fill(NaN);
  const res = new Float32Array(ne).fill(NaN);
  const s0 = A.link_start[sel.link], c = A.link_count[sel.link];
  let anyRes = false;
  for (let r = s0; r < s0 + c; r++) {
    const e = A.ce_ep[r], q = rowInfo(r);
    wl[e] = q.wlDist; nl[e] = q.nDist;
    wlLo[e] = q.wlDist - q.sWl; wlHi[e] = q.wlDist + q.sWl;
    nlLo[e] = q.nDist - q.sIf; nlHi[e] = q.nDist + q.sIf;
    if (isFinite(q.res)) { res[e] = q.res; anyRes = true; }
  }
  const wlMax = (M.H.config && M.H.config.ring_fix && M.H.config.ring_fix.wl_frac_max) || 0.25;
  const band = withAlpha(C.held, 0.18);
  makeChart(chartEl(el, 'Wide lane: float minus its whole number (cycles), band = one sigma', '', [[C.held, 'float minus whole number'], [C.refused, 'limit ' + wlMax]]), {
    series: [{ y: wl, color: C.held, width: 1.3 }], bands: [{ lo: wlLo, hi: wlHi, color: band }],
    hlines: [{ y: 0, color: C.muted, dash: [] }, { y: wlMax, color: C.refused }, { y: -wlMax, color: C.refused }],
    symmetric: true, minHalf: 0.3, yRange: [-0.6, 0.6], valueAt: e => fmtSigned(wl[e]),
  });
  makeChart(chartEl(el, 'Narrow lane (ionosphere-free, wide lane fixed): float minus its whole number (cycles)', 'tall', [[C.held, 'float minus whole number'], [C.refused, 'limit 0.20']]), {
    series: [{ y: nl, color: C.held, width: 1.3 }], bands: [{ lo: nlLo, hi: nlHi, color: band }],
    hlines: [{ y: 0, color: C.muted, dash: [] }, { y: 0.2, color: C.refused }, { y: -0.2, color: C.refused }],
    yRange: [-0.6, 0.6], valueAt: e => fmtSigned(nl[e]),
  });
  if (anyRes) {
    makeChart(chartEl(el, 'Post-fit ionosphere-free phase residual while held (m)', '', [[C.held, 'residual'], [C.release, 'release bound 0.03 m']]), {
      series: [{ y: res, color: C.held }], hlines: [{ y: 0.03, color: C.release }, { y: -0.03, color: C.release }, { y: 0, color: C.muted, dash: [] }],
      symmetric: true, minHalf: 0.04, valueAt: e => fmtSigned(res[e], 4) + ' m',
    });
  }
  const rf = (M.H.config && M.H.config.ring_fix) || {};
  const gate = rf.gate === 'odds'
    ? 'the odds of the nearest whole number against the next exceed a million to one on the narrow and the wide lane' +
      (rf.sigma_scale && rf.sigma_scale !== 1 ? ' (sigma scaled by ' + rf.sigma_scale + ')' : '')
    : 'the narrow lane\'s sigma is at most 0.10 cycle';
  hint(el, 'While held the whole numbers are the held ones; otherwise the nearest. ' + (f.fixing ? 'The fixing holds a ' +
    'link when ' + gate + ', the narrow lane is within 0.20 cycle and the wide lane within ' + wlMax + ' of a whole ' +
    'number, with the same whole numbers for 10 epochs in a row and no event on the link in the last 10 min.' :
    'No fixing runs on these signals: the link stays a float.'));
}

/** The link's fixing history from its RINGFIX events: decided, released (with the cause), became a tree link,
 * carried the tree through a re-host; each release with how long the hold lasted. */
function holdHistory(el, sel) {
  const ev = selectionEvents();
  if (!ev) return;
  const A = M.A, E = M.H.events;
  const rows = [];
  let since = -1;
  for (const i of ev.idx) {
    if (E.tiers[A.ev_tier[i]] !== 'RINGFIX') continue;
    const name = E.names[A.ev_name[i]], act = E.actions[A.ev_action[i]], e = A.ev_ep[i];
    let what = eventText(i), extra = '';
    if (name === 'hold') since = e;
    if (name === 'release' || name === 'to_tree') {
      if (since >= 0) extra = 'held ' + durationText(e - since);
      since = -1;
    }
    if (name === 'release') what = 'released: ' + (RELEASE_TEXT[act] || act);
    rows.push('<tr><td><span class="act" data-ep="' + e + '">' + clockOf(e) + '</span></td><td>' + esc(what) + '</td><td>' + esc(extra) + '</td></tr>');
  }
  if (!rows.length) return;
  el.insertAdjacentHTML('beforeend', '<h3>Decided, held, released</h3><table>' + rows.join('') + '</table>');
}

function treeTargetChart(el, sel, cf) {
  const s = M.nSta + sel.sat, ne = M.nEp;
  const series = [];
  const key = [];
  [[cf.f.band1, cf.f.b1, C.tree], [cf.f.band2, cf.f.b2, C.held]].forEach(([b, name, color]) => {
    const y = new Float32Array(ne).fill(NaN);
    let any = false;
    for (let e = 0; e < ne; e++) {
      if (!isTreeLink(b, sel.sta, s, e)) continue;
      const child = treeParent(b, s, e) === sel.sta ? s : sel.sta;
      const t = treeTarget(b, child, e);
      if (t != null) { y[e] = t; any = true; }
    }
    if (any) { series.push({ y, color, step: true, width: 1.6 }); key.push([color, name]); }
  });
  if (!series.length) return;
  makeChart(chartEl(el, 'Whole number of the tree link (cycles)', '', key), { series, nTicks: 3, yFmt: v => v.toFixed(0) });
}

function linkNow(sel) {
  const cf = curFamily();
  if (!cf) return '';
  const f = cf.f, e = S.ep, s = M.nSta + sel.sat;
  const t1 = isTreeLink(f.band1, sel.sta, s, e), t2 = isTreeLink(f.band2, sel.sta, s, e);
  const rows = [];
  if (t1 || t2) {
    const parts = [];
    for (const [b, name, on] of [[f.band1, f.b1, t1], [f.band2, f.b2, t2]]) {
      if (!on) continue;
      const child = treeParent(b, s, e) === sel.sta ? s : sel.sta;
      const t = treeTarget(b, child, e);
      parts.push(name + ' ' + (t == null ? '?' : t));
    }
    rows.push(['now', '<span class="state-tree">tree link of ' + [t1 && f.b1, t2 && f.b2].filter(Boolean).join(' and ') + '</span>']);
    rows.push(['whole numbers', parts.join(', ')]);
  }
  const r = sel.link >= 0 ? linkRowAt(sel.link, e) : -1;
  if (r >= 0) {
    const q = rowInfo(r), k = stateKind(q.state);
    rows.push(['now', '<span class="state-' + k + '">' + esc(M.H.states[q.state]) + '</span>']);
    rows.push([k === 'held' || k === 'decided' ? 'held whole numbers' : 'nearest whole numbers',
      f.b1 + ' ' + q.n1 + ', ' + f.b2 + ' ' + q.n2 + ', wide lane ' + q.wl]);
    const pinned = k === 'held' ? ' <span class="state-float">(pinned by the hold)</span>' : '';
    rows.push(['wide-lane float', fmt(q.wlFloat, 3) + ' (' + fmtSigned(q.wlDist) + ' from ' + q.wl + ', sigma ' + fmt(q.sWl, 3) + ')' + pinned]);
    rows.push(['narrow-lane float', fmt(q.nFloat, 3) + ' (' + fmtSigned(q.nDist) + ' from ' + q.n1 + ', sigma ' + fmt(q.sIf, 3) + ')' + pinned]);
    if (q.ring) rows.push(['ring', q.ring + ' (the larger hop count of its two ends)']);
    if (isFinite(q.res)) rows.push(['residual', fmtSigned(q.res, 4) + ' m']);
  }
  if (!rows.length) rows.push(['now', 'not tracked at this epoch']);
  const elev = M.A.los_el ? M.A.los_el[(sel.sta * M.nSat + sel.sat) * M.nEp + e] : 255;
  if (elev !== 255) rows.push(['elevation', elev + ' degrees']);
  const dS = treeDepth(cf.b, sel.sta, e), dT = treeDepth(cf.b, s, e);
  rows.push(['hops (' + cf.sig + ')', M.names[sel.sta] + ' ' + (dS >= 0 ? dS : '-') + ', ' + M.H.sats[sel.sat] + ' ' + (dT >= 0 ? dT : '-')]);
  return '<h3>At ' + clockOf(e) + '</h3><table class="kv">' + rows.map(([k, v]) => '<tr><td>' + esc(k) + '</td><td>' + v + '</td></tr>').join('') + '</table>';
}

// --- satellite

function renderSat(el, sel) {
  const cf = curFamily(), f = cf ? cf.f : null, sat = sel.node - M.nSta, name = M.H.sats[sat];
  el.insertAdjacentHTML('beforeend', '<h2>Satellite ' + esc(name) + '</h2><div class="sub">' + esc(sysName(name[0])) +
    (!f || name[0] === f.sys ? '' : ' (not of the drawn signals: pick its family above)') + '</div>');
  nowBox(el);
  if (f && M.hasTree && name[0] === f.sys) hopsChart(el, sel.node, f);
  if (cf) satLevelChart(el, sat, cf);
  const clk = satSeries('clk', sat);
  if (clk) makeChart(chartEl(el, 'Clock correction emitted (m)', '', [[C.ink, 'clock']]), { series: [{ y: clk, color: C.ink }], robust: true, valueAt: e => fmt(clk[e], 4) + ' m' });
  biasChart(el, sat, 'pb', 'Phase bias emitted, change from its median (m)');
  biasChart(el, sat, 'cb', 'Code bias emitted, change from its median (m)');
  const ev = selectionEvents();
  el.insertAdjacentHTML('beforeend', '<h3>Events of this satellite (' + (ev ? ev.idx.length : 0) + ')</h3>' + eventRows(ev ? ev.idx : []));
}
/** Hops of a node on both signals of family f over the run, with a mark wherever the tree moved it. */
function hopsChart(el, node, f) {
  const ne = M.nEp, h1 = new Float32Array(ne).fill(NaN), h2 = new Float32Array(ne).fill(NaN);
  for (let e = 0; e < ne; e++) {
    const a = treeDepth(f.band1, node, e), b = treeDepth(f.band2, node, e);
    if (a >= 0) h1[e] = a;
    if (b >= 0) h2[e] = b + 0.12;                        // drawn a hair apart so equal hops stay visible
  }
  const moves = treeMoves(f.band1, node);
  makeChart(chartEl(el, 'Hops from the reference receiver; marks at the top: the tree moved it on ' + f.b1 + ' (' + moves.length + ')', '',
    [[C.tree, f.b1], [C.held, f.b2], [C.release, 'moved']]), {
    series: [{ y: h1, color: C.tree, step: true, width: 1.6 }, { y: h2, color: C.held, step: true, width: 1.2 }],
    ticks: moves.map(e => ({ e, color: C.release })),
    nTicks: 3, zeroBased: true, yFmt: v => v.toFixed(0),
    valueAt: e => f.b1 + ' ' + (isFinite(h1[e]) ? h1[e] : '-') + ', ' + M.H.tree_rows[treeRow(f.band1, node, e)],
  });
}
function hasFinite(a) { for (let i = 0; i < a.length; i++) if (isFinite(a[i])) return true; return false; }
function median(a) {
  const v = Array.from(a).filter(isFinite).sort((p, q) => p - q);
  return v.length ? v[v.length >> 1] : NaN;
}

/** Each signal's emitted bias less its median over the run: offsets of tens of metres would hide the moves. */
function biasChart(el, sat, kind, title) {
  const tab = kind === 'pb' ? M.H.phase_signals : M.H.code_signals;
  const sigs = tab.filter(s => s[0] && satSeries(kind, sat, s) && hasFinite(satSeries(kind, sat, s)));
  if (!sigs.length) return;
  const cols = sigs.map((_s, i) => rampColor(window.NV_ASSETS.colormaps.viridis, sigs.length > 1 ? 0.85 * i / (sigs.length - 1) : 0.3));
  const series = sigs.map((s, i) => {
    const y = satSeries(kind, sat, s), m = median(y);
    return { y: Float32Array.from(y, v => v - m), color: cols[i], med: m };
  });
  makeChart(chartEl(el, title, '', sigs.map((s, i) => [cols[i], s + ' (median ' + fmt(series[i].med, 3) + ')'])), {
    series, robust: true, valueAt: e => sigs.map((s, i) => s + ' ' + fmtSigned(series[i].y[e], 3)).join(', '),
  });
}

function satLevelChart(el, sat, cf) {
  const f = cf.f, fi = cf.fi;
  const l1 = satLevel(fi, 0, sat), l2 = satLevel(fi, 1, sat);
  if (!l1 || !hasFinite(l1)) return;
  const ne = M.nEp, nl = new Float32Array(ne).fill(NaN), wl = new Float32Array(ne).fill(NaN);
  for (let e = 0; e < ne; e++) {
    if (!isFinite(l1[e]) || !isFinite(l2[e])) continue;
    nl[e] = (f.f1 * l1[e] - f.f2 * l2[e]) / (f.f1 - f.f2);
    wl[e] = l1[e] - l2[e];
  }
  const A = M.A, jl = M.jumps.get(fi + '|' + sat) || [];
  const clsColor = [null, C.whole, C.iono, C.frac];
  const dots = jl.map(i => ({ e: A.jp_ep[i], y: nl[A.jp_ep[i]], color: clsColor[A.jp_cls[i]], r: 3.2 }));
  const n = [0, 0, 0, 0];
  for (const i of jl) n[A.jp_cls[i]]++;
  makeChart(chartEl(el, 'Correction level: the satellite\'s own steps added up (cycles)', 'tall',
    [[C.ink, 'narrow lane'], [C.muted, 'wide lane'], [C.whole, 'whole jump ' + n[1]], [C.iono, 'ionosphere-like ' + n[2]], [C.frac, 'fraction ' + n[3]]]), {
    series: [{ y: nl, color: C.ink, width: 1.3 }, { y: wl, color: C.muted, width: 1, dash: [3, 2] }], dots,
    valueAt: e => 'narrow ' + fmt(nl[e], 3) + ', wide ' + fmt(wl[e], 3),
  });
  hint(el, 'A jump is the step of the satellite\'s emitted correction level between two epochs after the step all ' +
    'satellites share is removed (tolerance ' + M.H.jump_tol + ' cycle): whole cycles on both signals (a re-pin), ' +
    'ionosphere-like (the ionosphere-free level unchanged) or a fraction of a cycle (the level a user holds moves).');
}

function satNow(sel) {
  const cf = curFamily(), e = S.ep, n = sel.node, name = M.names[n];
  const rows = [];
  if (!cf) return '<h3>At ' + clockOf(e) + '</h3><div class="hint">No signal family with a tree in this run.</div>';
  const f = cf.f;
  if (name[0] === f.sys && M.hasTree) {
    for (const [b, sig] of [[f.band1, f.b1], [f.band2, f.b2]]) {
      const d = treeDepth(b, n, e), p = treeParent(b, n, e), ch = treeChangeText(b, n, e);
      rows.push(['hops on ' + sig, d >= 0 ? d + (p >= 0 ? ', parent <span class="act" data-node="' + p + '">' + esc(M.names[p]) + '</span>' : '') +
        (ch ? ' <span class="state-refused">(' + esc(ch) + ' now)</span>' : '') : 'not on this tree']);
    }
    const gr = G.gr && G.gr.cf && G.gr.cf.fi === cf.fi ? G.gr : collectGraph(e);
    const tot = gr.inTotal[n], wh = gr.inWhole[n];
    rows.push(['links towards the reference', tot ? wh + ' of ' + tot + ' are whole numbers' : 'none']);
    rows.push(['tied to the reference by whole numbers only', tot ? (wh === tot ? 'yes' : 'no') : 'n/a']);
    const links = gr.links.filter(lk => lk.s === n).sort((p, q) => gr.depth[p.a] - gr.depth[q.a]);
    if (links.length) {
      rows.push(['links', links.map(lk => '<span class="act" data-link="' + lk.a + ',' + (n - M.nSta) + '">' +
        esc(shortSta(M.names[lk.a])) + '</span> <span class="state-' + kindClass(lk.kind) + '">' + kindWord(lk.kind) + '</span>').join(', ')]);
    }
  }
  const jl = M.jumps.get(cf.fi + '|' + (n - M.nSta)) || [];
  const A = M.A, here = jl.filter(i => A.jp_ep[i] === e);
  if (here.length) rows.push(['jump now', here.map(i => ['', 'whole', 'ionosphere-like', 'fraction'][A.jp_cls[i]] + ' (narrow lane ' + fmtSigned(A.jp_dn[i]) + ')').join(', ')]);
  return '<h3>At ' + clockOf(e) + '</h3><table class="kv">' + rows.map(([k, v]) => '<tr><td>' + esc(k) + '</td><td>' + v + '</td></tr>').join('') + '</table>';
}
function kindWord(k) { return { tree: 'tree', treeOther: 'tree (other signal)', held: 'held', decided: 'decided', refused: 'not yet', float: 'float' }[k] || k; }
function kindClass(k) { return k === 'treeOther' ? 'tree' : k === 'decided' ? 'held' : k; }

// --- receiver

function renderSta(el, sel) {
  const n = sel.node, st = M.H.stations[n];
  el.insertAdjacentHTML('beforeend', '<h2>Receiver ' + esc(st.name) + '</h2><div class="sub">' + esc(st.role || '') +
    (st.lat != null ? ', latitude ' + st.lat.toFixed(2) + ', longitude ' + st.lon.toFixed(2) : '') + '</div>');
  nowBox(el);
  if (curFamily() && M.hasTree) hopsChart(el, n, curFamily().f);
  const z = rxSeries('rx_zwd', n), zs = rxSeries('rx_zwd_sd', n);
  if (z) {
    const lo = z.map((v, i) => v - zs[i]), hi = z.map((v, i) => v + zs[i]);
    makeChart(chartEl(el, 'Wet delay at the zenith (m), band = one sigma; scale from the 1st to the 99th percentile', '', [[C.held, 'wet delay']]), {
      series: [{ y: z, color: C.held }], bands: [{ lo, hi, color: withAlpha(C.held, 0.15) }], valueAt: e => fmt(z[e], 4) + ' m',
      robust: true,
    });
  }
  const ck = rxSeries('rx_clk', n);
  if (ck) makeChart(chartEl(el, 'Receiver clock (m)', '', [[C.ink, 'clock']]), { series: [{ y: ck, color: C.ink }], robust: true, valueAt: e => fmt(ck[e], 3) + ' m' });
  if (M.A.rx_rpb && curFamily()) {
    const cf = curFamily();
    const series = [], key = [];
    [[cf.f.band1, cf.f.b1, C.tree], [cf.f.band2, cf.f.b2, C.held]].forEach(([b, name, color]) => {
      const y = rxSeries('rx_rpb', n, b);
      if (y && hasFinite(y)) { series.push({ y, color }); key.push([color, name]); }
    });
    if (series.length) makeChart(chartEl(el, 'Receiver phase bias (m)', '', key), { series, robust: true });
  } else if (!M.A.rx_rpb) hint(el, 'Receiver phase bias: not traced in this run (key network.trace_rpb_states).');
  const ev = selectionEvents();
  const A = M.A, al = M.H.lanes.findIndex(l => l.key === 'alarm');
  const alarms = ev ? ev.idx.filter(i => A.ev_lane[i] === al) : [];
  el.insertAdjacentHTML('beforeend', '<h3>Alarms at this receiver (' + alarms.length + ')</h3>' + eventRows(alarms, 100));
  el.insertAdjacentHTML('beforeend', '<h3>All events of this receiver (' + (ev ? ev.idx.length : 0) + ')</h3>' + eventRows(ev ? ev.idx : []));
}
function staNow(sel) {
  const cf = curFamily(), e = S.ep, n = sel.node;
  const rows = [];
  if (!cf) {
    const z = rxSeries('rx_zwd', n);
    if (z) rows.push(['wet delay', fmt(z[e], 4) + ' m']);
    return '<h3>At ' + clockOf(e) + '</h3><table class="kv">' + rows.map(([k, v]) => '<tr><td>' + esc(k) + '</td><td>' + v + '</td></tr>').join('') + '</table>';
  }
  const f = cf.f;
  if (M.hasTree) {
    for (const [b, sig] of [[f.band1, f.b1], [f.band2, f.b2]]) {
      const d = treeDepth(b, n, e), p = treeParent(b, n, e), ch = treeChangeText(b, n, e);
      rows.push(['hops on ' + sig, d >= 0 ? (p === -2 ? '0 (the reference receiver)' : d + (p >= 0 ? ', parent <span class="act" data-node="' + p + '">' + esc(M.names[p]) + '</span>' : '') +
        (ch ? ' <span class="state-refused">(' + esc(ch) + ' now)</span>' : '')) : 'not on this tree']);
    }
  }
  const z = rxSeries('rx_zwd', n);
  if (z) rows.push(['wet delay', fmt(z[e], 4) + ' m']);
  const gr = G.gr && G.gr.cf && G.gr.cf.fi === cf.fi ? G.gr : collectGraph(e);
  const links = gr.links.filter(lk => lk.a === n).sort((p, q) => gr.depth[p.s] - gr.depth[q.s]);
  if (links.length) {
    const cnt = {};
    for (const lk of links) cnt[lk.kind] = (cnt[lk.kind] || 0) + 1;
    rows.push(['links of ' + famLabel(f), Object.entries(cnt).map(([k, v]) => v + ' ' + kindWord(k)).join(', ')]);
    rows.push(['', links.map(lk => '<span class="act" data-link="' + n + ',' + (lk.s - M.nSta) + '">' + esc(M.names[lk.s]) +
      '</span> <span class="state-' + kindClass(lk.kind) + '">' + kindWord(lk.kind) + '</span>').join(', ')]);
  }
  return '<h3>At ' + clockOf(e) + '</h3><table class="kv">' + rows.map(([k, v]) => '<tr><td>' + esc(k) + '</td><td>' + v + '</td></tr>').join('') + '</table>';
}

// --- events of one epoch

function renderEvents(el, sel) {
  const A = M.A, e = sel.ep;
  const idx = [];
  for (let i = M.evOff[e]; i < M.evOff[e + 1]; i++) idx.push(i);
  if (sel.lane >= 0) idx.sort((p, q) => (A.ev_lane[q] === sel.lane) - (A.ev_lane[p] === sel.lane));
  el.insertAdjacentHTML('beforeend', '<h2>Events at ' + clockOf(e) + '</h2><div class="sub">' + idx.length +
    ' events at this epoch' + (sel.lane >= 0 ? ', ' + esc(M.H.lanes[sel.lane].label.toLowerCase()) + ' first' : '') +
    '. Click a row to select its link or node.</div>');
  el.insertAdjacentHTML('beforeend', eventRows(idx, 600));
}

function wireDetailClicks(root) {
  root.querySelectorAll('[data-node]').forEach(x => x.addEventListener('click', () => selectNode(+x.dataset.node)));
  root.querySelectorAll('[data-link]').forEach(x => x.addEventListener('click', () => {
    const [a, s] = x.dataset.link.split(',').map(Number);
    selectLink(a, s);
  }));
  root.querySelectorAll('[data-fam]').forEach(x => x.addEventListener('click', () => setFamily(+x.dataset.fam)));
  root.querySelectorAll('[data-toggle-other]').forEach(x => x.addEventListener('click', () => { S.showOther = !S.showOther; renderDetail(); }));
  root.querySelectorAll('[data-ep]').forEach(x => x.addEventListener('click', () => setEp(+x.dataset.ep)));
  root.querySelectorAll('[data-ev]').forEach(x => x.addEventListener('click', () => {
    const i = +x.dataset.ev, A = M.A;
    setEp(A.ev_ep[i]);
    if (A.ev_sta[i] >= 0 && A.ev_sat[i] >= 0) selectLink(A.ev_sta[i], A.ev_sat[i], A.ev_band[i]);
    else if (A.ev_sat[i] >= 0) selectNode(M.nSta + A.ev_sat[i]);
    else if (A.ev_sta[i] >= 0) selectNode(A.ev_sta[i]);
  }));
}

// ============================================================================================ 8. summaries over the run

function renderSummary() {
  const box = document.getElementById('summary-charts');
  box.innerHTML = '';
  const fig = (title, keyItems) => {
    const f = document.createElement('figure');
    f.innerHTML = '<figcaption>' + esc(title) + '</figcaption>';
    const cv = document.createElement('canvas');
    f.appendChild(cv);
    if (keyItems) {
      const k = document.createElement('div');
      k.className = 'chart-key';
      k.innerHTML = keyItems.map(([c, t]) => '<span><i style="background:' + c + '"></i>' + esc(t) + '</span>').join('');
      f.appendChild(k);
    }
    box.appendChild(f);
    return cv;
  };
  const sl = M.A.sum_links, ne = M.nEp, nf = M.nFam;
  if (sl && M.hasFixing) {
    const fams = M.H.families.map((f, i) => [f, i]).filter(([f]) => f.fixing);
    const cols = fams.map((_x, k) => rampColor(window.NV_ASSETS.colormaps.viridis, fams.length > 1 ? 0.8 * k / (fams.length - 1) : 0.25));
    const series = fams.map(([_f, i], k) => {
      const y = new Float32Array(ne);
      for (let e = 0; e < ne; e++) { const n = sl[i * ne + e]; y[e] = n ? 100 * sl[(nf + i) * ne + e] / n : NaN; }
      return { y, color: cols[k], width: 1.6 };
    });
    makeChart(fig('Share of the links that are not tree links held at whole numbers (%)', fams.map(([f], k) => [cols[k], famLabel(f)])),
      { series, yRange: [0, 100], nTicks: 4, valueAt: e => series.map(s => fmt(s.y[e], 0) + ' %').join(', ') });
  }
  const cf = curFamily();
  if (sl && cf) {
    const i = cf.fi, held = new Float32Array(ne), notyet = new Float32Array(ne), all = new Float32Array(ne);
    for (let e = 0; e < ne; e++) {
      all[e] = sl[i * ne + e]; held[e] = sl[(nf + i) * ne + e] + sl[(2 * nf + i) * ne + e]; notyet[e] = sl[(3 * nf + i) * ne + e];
    }
    const tree = new Float32Array(ne);
    if (M.hasTree) for (let e = 0; e < ne; e++) {
      let c = 0;
      for (let n = M.nSta; n < M.nNode; n++) if (treeParent(cf.f.band1, n, e) >= 0) c++;
      for (let n = 0; n < M.nSta; n++) if (treeParent(cf.f.band1, n, e) >= 0) c++;
      tree[e] = c;
    }
    makeChart(fig('Links of ' + famLabel(cf.f) + ' (count)', [[C.tree, 'tree links of ' + cf.f.b1], [C.float, 'not tree links'], [C.held, 'held'], [C.refused, 'not yet held']]), {
      series: [{ y: tree, color: C.tree }, { y: all, color: C.float }, { y: held, color: C.held, width: 1.6 }].concat(cf.f.fixing ? [{ y: notyet, color: C.refused }] : []),
      zeroBased: true, nTicks: 4, valueAt: e => 'tree ' + tree[e] + ', other ' + all[e] + ', held ' + held[e],
    });
  }
  jumpSummary(fig, cf);
  alarmSummary(fig);
}

function jumpSummary(fig, cf) {
  const tab = (M.H.jump_tables || []).find(t => t.family === (cf ? cf.fi : -1));
  if (!tab) return;
  const binEp = Math.round(3600000 / M.step), nh = tab.sat_hours.length;
  const hopsName = ['not on both trees', '1 hop', '3 hops', '5 hops', '7 hops or more'];
  const clsCol = [C.whole, C.iono, C.frac];
  const bars = [];
  for (let h = 0; h < nh; h++) {
    for (let k = 1; k <= 4; k++) {
      const sh = tab.sat_hours[h][k];
      if (!(sh > 0.2)) continue;
      const w = binEp / 4.6, e0 = h * binEp + (k - 1) * (binEp / 4.2) + 2;
      bars.push({ e0, e1: e0 + w, stack: tab.counts[h][k].map((c, j) => ({ v: c / sh, color: clsCol[j] })), label: ['', '1', '3', '5', '7+'][k] });
    }
  }
  makeChart(fig('Jumps of the corrections per satellite-hour, ' + famLabel(cf.f) + '; in each hour one bar per hops class (1, 3, 5, 7+)',
    [[C.whole, 'whole cycles'], [C.iono, 'ionosphere-like'], [C.frac, 'fraction of a cycle']]),
  { bars, zeroBased: true, nTicks: 4, valueAt: e => {
    const h = Math.floor(e / binEp);
    if (h >= nh) return '';
    return 'hour ' + h + ': ' + [1, 2, 3, 4].map(k => hopsName[k] + ' ' + (tab.sat_hours[h][k] > 0.2 ?
      tab.counts[h][k].map(c => (c / tab.sat_hours[h][k]).toFixed(1)).join('/') : '-')).join('; ');
  } });
}

/** Per 10 minutes: counts of the events of some lanes, or of one lane split by its action (the release cause). */
function binnedBars(groups) {
  const bin = Math.max(1, Math.round(600000 / M.step)), A = M.A, bars = [];
  for (let e0 = 0; e0 < M.nEp; e0 += bin) {
    const v = groups.map(() => 0);
    for (let i = M.evOff[e0]; i < M.evOff[Math.min(M.nEp, e0 + bin)]; i++) {
      const g = groups.findIndex(gr => gr.match(i, A));
      if (g >= 0) v[g]++;
    }
    bars.push({ e0, e1: e0 + bin, stack: groups.map((gr, j) => ({ v: v[j], color: gr.color })) });
  }
  return bars;
}

function alarmSummary(fig) {
  if (!M.nEv) return;
  const lane = k => M.H.lanes.findIndex(l => l.key === k);
  const rel = lane('release'), acts = M.H.events.actions;
  if (M.laneCount[rel].some(c => c > 0)) {
    const causes = [['slip', C.slip], ['refloat', C.alarm], ['gap', C.gap], ['chain', C.tree], ['monitor', C.coderef]];
    makeChart(fig('Releases of held links per 10 minutes, by cause', causes.map(([c, col]) => [col, RELEASE_TEXT[c] || c])), {
      bars: binnedBars(causes.map(([c, color]) => ({ color, match: (i, A) => A.ev_lane[i] === rel && acts[A.ev_action[i]] === c }))),
      zeroBased: true, nTicks: 4,
    });
  }
  const al = lane('alarm'), sl = lane('slip');
  makeChart(fig('Alarms and slips per 10 minutes', [[C.alarm, 'alarms'], [C.slip, 'slips']]), {
    bars: binnedBars([{ color: C.alarm, match: (i, A) => A.ev_lane[i] === al }, { color: C.slip, match: (i, A) => A.ev_lane[i] === sl }]),
    zeroBased: true, nTicks: 4,
  });
}


// ============================================================================================ 9. the view

function setFamily(fi) {
  S.fam = fi;
  document.getElementById('family').value = fi;
  fillBandSelect();
  const sys = M.H.families[fi].sys;
  const selSat = S.sel && (S.sel.kind === 'link' ? S.sel.sat : S.sel.kind === 'sat' ? S.sel.node - M.nSta : -1);
  if (selSat != null && selSat >= 0 && M.H.sats[selSat][0] !== sys) S.sel = null;     // not of these signals
  if (S.sel && S.sel.kind === 'link') {
    S.sel.link = linkOf(S.fam, S.sel.sta, S.sel.sat);
    S.sel._ev = null;
  }
  TL.cache = null;
  drawGraph(); drawTimeline(); renderDetail(); renderSummary(); saveHash();
}
function selectNode(n) {
  if (n == null || n < 0) return;
  S.sel = { kind: n < M.nSta ? 'sta' : 'sat', node: n };
  if (n >= M.nSta) {
    const sys = M.names[n][0];
    if (curFamily() && curFamily().f.sys !== sys) {
      const fi = M.H.families.findIndex(f => f.sys === sys);
      if (fi >= 0) { setFamilyQuiet(fi); }
    }
  }
  afterSelect();
}
function selectLink(sta, sat) {
  const sys = M.H.sats[sat][0];
  if (curFamily() && curFamily().f.sys !== sys) {
    const fi = M.H.families.findIndex(f => f.sys === sys && f.fixing);
    setFamilyQuiet(fi >= 0 ? fi : M.H.families.findIndex(f => f.sys === sys));
  }
  S.sel = { kind: 'link', sta, sat, link: linkOf(S.fam, sta, sat) };
  afterSelect();
}
function setFamilyQuiet(fi) {
  if (fi < 0) return;
  S.fam = fi;
  document.getElementById('family').value = fi;
  fillBandSelect();
  renderSummary();
}
function afterSelect() {
  TL.cache = null;
  renderDetail(); drawGraph(); drawTimeline(); saveHash();
}
function clearSelection() { S.sel = null; afterSelect(); }

function fillBandSelect() {
  const f = M.H.families[S.fam], sel = document.getElementById('band');
  sel.innerHTML = '';
  if (!f) return;
  [f.b1, f.b2].forEach((b, i) => sel.insertAdjacentHTML('beforeend', '<option value="' + i + '">' + esc(b) + '</option>'));
  sel.value = S.bandSel;
}

function defaultFamily() {
  const H = M.H;
  let best = -1, bestN = -1;
  H.families.forEach((f, i) => { if (f.held_link_epochs > bestN) { bestN = f.held_link_epochs; best = i; } });
  return best >= 0 ? best : 0;
}

/** The selection named in the address (s=link:RECEIVER:SAT or s=node:NAME). */
function selectionOfQuery(q) {
  const s = q.get('s');
  if (s && s.startsWith('link:')) {
    const [, a, b] = s.split(':');
    const sta = M.nodeIndex.get(a), sat = M.H.sats.indexOf(b);
    if (sta != null && sat >= 0) return { kind: 'link', sta, sat, link: linkOf(S.fam, sta, sat) };
  } else if (s && s.startsWith('node:')) {
    const n = M.nodeIndex.get(s.slice(5));
    if (n != null) return { kind: n < M.nSta ? 'sta' : 'sat', node: n };
  }
  return null;
}

function showTooltip(hit, x, y) {
  const tip = document.getElementById('tooltip');
  if (!hit) { tip.hidden = true; return; }
  const gr = G.gr, cf = gr.cf;
  let html = '';
  if (hit.kind === 'node') {
    const n = hit.node, d = gr.depth[n];
    html = '<b>' + esc(M.names[n]) + '</b> ' + (n < M.nSta ? 'receiver' : sysName(M.names[n][0]) + ' satellite') +
      '<br>' + (d === 0 ? 'reference receiver of ' + esc(cf.sig) : d > 0 ? d + ' hops on ' + esc(cf.sig) : 'not on the tree of ' + esc(cf.sig)) +
      (gr.inTotal[n] ? '<br>links towards the reference: ' + gr.inWhole[n] + ' of ' + gr.inTotal[n] + ' whole' : '') +
      '<br><span class="muted">double-click: open its page</span>';
  } else {
    const lk = hit.lk;
    html = '<b>' + esc(M.names[lk.a]) + ' - ' + esc(M.names[lk.s]) + '</b><br>' + esc(kindWord(lk.kind));
    if (lk.row >= 0) {
      const q = rowInfo(lk.row);
      html = '<b>' + esc(M.names[lk.a]) + ' - ' + esc(M.names[lk.s]) + '</b><br>' + esc(M.H.states[q.state]) +
        '<br>wide lane ' + fmtSigned(q.wlDist, 2) + ' from ' + q.wl + ' (sigma ' + fmt(q.sWl, 2) + ')' +
        '<br>narrow lane ' + fmtSigned(q.nDist, 2) + ' from ' + q.n1 + ' (sigma ' + fmt(q.sIf, 2) + ')' + (q.ring ? '<br>ring ' + q.ring : '');
    } else if (lk.kind === 'tree' || lk.kind === 'treeOther') {
      const child = treeParent(lk.band, lk.s, S.ep) === lk.a ? lk.s : lk.a;
      const t = treeTarget(lk.band, child, S.ep);
      html += ' of ' + esc(M.H.bands[lk.band].split(':')[1]) + (lk.both ? ' (both signals)' : '') + (t != null ? ', whole number ' + t : '');
    }
  }
  tip.innerHTML = html;
  tip.hidden = false;
  const pane = document.getElementById('graph-pane');
  tip.style.left = Math.min(x + 14, pane.clientWidth - 260) + 'px';
  tip.style.top = (y + 14) + 'px';
}

/** The network area and the detail panel fill the view's height (the summaries follow below). */
function fitMain() {
  const main = document.getElementById('main'), views = document.getElementById('views');
  if (window.matchMedia('(max-width: 760px)').matches) { main.style.height = ''; return; }
  const top = main.getBoundingClientRect().top - views.getBoundingClientRect().top + views.scrollTop;
  const h = Math.max(480, views.clientHeight - top - 8);
  main.style.height = h + 'px';
  document.getElementById('detail').style.maxHeight = h + 'px';
}

function initNetworkView() {
  const fam = document.getElementById('family');
  M.H.families.forEach((f, i) => fam.insertAdjacentHTML('beforeend', '<option value="' + i + '">' + esc(famLabel(f)) + (f.fixing ? ' (held)' : '') + '</option>'));
  fam.addEventListener('change', () => setFamily(+fam.value));
  if (!M.nFam) for (const id of ['family', 'band']) document.getElementById(id).parentElement.hidden = true;
  document.getElementById('band').addEventListener('change', ev => { S.bandSel = +ev.target.value; TL.cache = null; drawGraph(); updateDetailNow(); saveHash(); });
  document.getElementById('layout').addEventListener('change', ev => { S.layout = ev.target.value; drawGraph(); saveHash(); });
  document.getElementById('show-float').addEventListener('change', ev => { S.showFloat = ev.target.checked; drawGraph(); });
  document.getElementById('show-held').addEventListener('change', ev => { S.showHeld = ev.target.checked; drawGraph(); });
  document.getElementById('show-labels').addEventListener('change', ev => { S.showLabels = ev.target.checked; drawGraph(); });
  const cv = document.getElementById('graph');
  cv.addEventListener('mousemove', ev => {
    const r = cv.getBoundingClientRect();
    const hit = hitTest(ev.clientX - r.left, ev.clientY - r.top);
    const changed = JSON.stringify(hit && (hit.node != null ? hit.node : [hit.lk.a, hit.lk.s])) !== JSON.stringify(S.hover && (S.hover.node != null ? S.hover.node : [S.hover.lk.a, S.hover.lk.s]));
    S.hover = hit;
    showTooltip(hit, ev.clientX - r.left, ev.clientY - r.top);
    cv.style.cursor = hit ? 'pointer' : 'default';
    if (changed) drawGraph();
  });
  cv.addEventListener('mouseleave', () => { S.hover = null; showTooltip(null); drawGraph(); });
  cv.addEventListener('click', ev => {
    const r = cv.getBoundingClientRect();
    const hit = hitTest(ev.clientX - r.left, ev.clientY - r.top);
    if (!hit) { clearSelection(); return; }
    if (hit.kind === 'node') selectNode(hit.node);
    else selectLink(hit.lk.a, hit.lk.s - M.nSta);
  });
  cv.addEventListener('dblclick', ev => {
    const r = cv.getBoundingClientRect();
    const hit = hitTest(ev.clientX - r.left, ev.clientY - r.top);
    if (hit && hit.kind === 'node') openNode(hit.node);
    else if (hit) openLink(hit.lk.a, hit.lk.s - M.nSta);
  });
}

registerView('network', {
  title: 'Network links',
  init() { initNetworkView(); },
  selection(_args, q) { return selectionOfQuery(q); },
  show() {
    document.getElementById('family').value = S.fam;
    document.getElementById('layout').value = S.layout;
    const miss = missingParts('network'), nm = document.getElementById('net-missing');
    nm.hidden = !miss.length;
    nm.innerHTML = miss.length ? 'Not in this run: ' + missingHtml(miss) + '.' : '';
    fillBandSelect();
    fitMain();
    renderDetail();
    renderSummary();
    drawGraph();
  },
  time() { drawGraph(); updateDetailNow(); },
  resize() { fitMain(); drawGraph(); },
  recolor() { renderDetail(); renderSummary(); drawGraph(); },
  query() {
    const s = S.sel;
    return { s: s && s.kind === 'link' ? 'link:' + M.names[s.sta] + ':' + M.H.sats[s.sat]
      : s && (s.kind === 'sta' || s.kind === 'sat') ? 'node:' + M.names[s.node] : '' };
  },
  timelineEvents(e, lane) { S.sel = { kind: 'events', ep: e, lane }; TL.cache = null; renderDetail(); drawTimeline(); },
  escape() { clearSelection(); },
  badge() {
    const cf = curFamily(), sl = M.A.sum_links;
    if (!cf || !cf.f.fixing || !sl) return '';
    const n = sl[cf.fi * M.nEp + S.ep], held = sl[(M.nFam + cf.fi) * M.nEp + S.ep];
    return n ? Math.round(100 * held / n) + ' % held' : '';
  },
});
