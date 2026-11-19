'use strict';
/*
 * Network dashboard: the atmosphere (#/atmosphere).
 * The wet delay of every receiver (a map at the cursor, receivers x time, all series) and the ionosphere: the slant
 * delay of every line of sight placed at its pierce point on a shell 350 km high, and per receiver the median of its
 * satellites' vertical-equivalent delay over time.
 */

const IONO_SHELL_M = 350000, EARTH_R = 6371000;

/** Pierce point (lat, lon in degrees) of a line of sight at the shell, and the slant-to-vertical factor. */
function piercePoint(latDeg, lonDeg, azDeg, elDeg) {
  const la = latDeg * Math.PI / 180, lo = lonDeg * Math.PI / 180, az = azDeg * Math.PI / 180, elv = elDeg * Math.PI / 180;
  const psi = Math.PI / 2 - elv - Math.asin(EARTH_R / (EARTH_R + IONO_SHELL_M) * Math.cos(elv));
  const lap = Math.asin(Math.sin(la) * Math.cos(psi) + Math.cos(la) * Math.sin(psi) * Math.cos(az));
  const lop = lo + Math.asin(Math.sin(psi) * Math.sin(az) / Math.cos(lap));
  const z = Math.asin(EARTH_R / (EARTH_R + IONO_SHELL_M) * Math.cos(elv));
  return { lat: lap * 180 / Math.PI, lon: ((lop * 180 / Math.PI + 540) % 360) - 180, vert: Math.cos(z) };
}

/** Per receiver and epoch: the median vertical-equivalent ionosphere over its satellites (m). Cached. */
function rxVerticalIono() {
  const L = B('los');
  if (!L || !L.A.lion) return null;
  if (L._viono) return L._viono;
  const ne = M.nEp, out = new Float32Array(M.nSta * ne).fill(NaN), sc = L.H.scales.lion;
  const bySta = Array.from({ length: M.nSta }, () => []);
  for (let li = 0; li < L.A.los_sta.length; li++) bySta[L.A.los_sta[li]].push(li);
  const vals = [];
  for (let j = 0; j < M.nSta; j++) {
    for (let e = 0; e < ne; e++) {
      vals.length = 0;
      for (const li of bySta[j]) {
        const q = L.A.lion[li * ne + e], k = L.A.los_sat[li], elv = M.A.los_el[(j * M.nSat + k) * ne + e];
        if (q === -32768 || elv === 255 || elv < 10) continue;
        const z = Math.asin(EARTH_R / (EARTH_R + IONO_SHELL_M) * Math.cos(elv * Math.PI / 180));
        vals.push(q / sc * Math.cos(z));
      }
      if (vals.length) { vals.sort((a, b) => a - b); out[j * ne + e] = vals[vals.length >> 1]; }
    }
  }
  L._viono = out;
  return out;
}

const ATV = { iono: 'vertical' };

function renderAtmosphere(root) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  const ne = M.nEp, L = B('los');
  viewHead(pad, 'Atmosphere', 'wet delay per receiver; ionosphere per line of sight, slant or vertical');
  missingNote(pad, 'atmosphere');
  const zwd = M.A.rx_zwd, vio = rxVerticalIono();
  const col = (a, e) => Array.from({ length: M.nSta }, (_x, j) => a ? a[j * ne + e] : NaN);
  const box = el('div');
  pad.appendChild(box);
  const nowTiles = () => {
    box.innerHTML = '';
    const items = [];
    if (zwd) { const v = col(zwd, S.ep).filter(isFinite); items.push({ k: 'Wet delay, median correction now', v: fmtLen(medianOf(v)), s: 'from ' + fmtLen(Math.min(...v)) + ' to ' + fmtLen(Math.max(...v)), cls: 'info' }); }
    if (vio) { const v = col(vio, S.ep).filter(isFinite); items.push({ k: 'Vertical ionosphere, median now', v: fmtLen(medianOf(v)), s: 'from ' + fmtLen(Math.min(...v)) + ' to ' + fmtLen(Math.max(...v)), cls: 'info' }); }
    tiles(box, items);
  };
  nowTiles();
  ATV.nowTiles = nowTiles;
  const g = el('div', 'grid2');
  pad.appendChild(g);
  if (zwd) {
    const p = panel(g, 'Wet delay at the cursor', 'correction to the model per receiver');
    const sc = signedScale(zwd, 'wet delay');
    ATV.zmap = worldMap(p, { height: 320, scale: sc, points: () => M.H.stations.map((s, j) => ({ label: s.name, lat: s.lat, lon: s.lon, value: zwd[j * ne + S.ep] })),
      text: j => fmtLen(zwd[j * ne + S.ep]) + ' +- ' + fmtLen(M.A.rx_zwd_sd[j * ne + S.ep]), onClick: j => openNode(j) });
    scaleKey(p, sc, v => fmtLen(v));
  }
  if (L && L.A.laz && L.A.lion) {
    const vert = ATV.iono === 'vertical';
    const p = panel(g, (vert ? 'Vertical' : 'Slant') + ' ionosphere at the cursor', 'each line of sight at its pierce point (shell at 350 km), above 10 degrees' +
      (vert ? '; slant x cos(zenith at the shell)' : ''));
    const ctl = el('div', 'row', '<span class="seg"><button data-i="vertical" class="' + (vert ? 'on' : '') + '">vertical</button><button data-i="slant" class="' + (vert ? '' : 'on') + '">slant</button></span>');
    p.appendChild(ctl);
    ctl.querySelectorAll('button').forEach(b => b.addEventListener('click', () => { ATV.iono = b.dataset.i; renderAtmosphere(root); }));
    const factor = elv => (vert ? Math.cos(Math.asin(EARTH_R / (EARTH_R + IONO_SHELL_M) * Math.cos(elv * Math.PI / 180))) : 1);
    const v = [];
    for (let li = 0; li < L.A.los_sta.length; li += 3) {
      const j = L.A.los_sta[li], k = L.A.los_sat[li];
      for (let e = 0; e < ne; e += 7) {
        const q = L.A.lion[li * ne + e], elv = M.A.los_el[(j * M.nSat + k) * ne + e];
        if (q !== -32768 && elv !== 255 && elv >= 10) v.push(q / L.H.scales.lion * factor(elv));
      }
    }
    const [lo, hi] = robustSpan(Float32Array.from(v), false);
    const sc = { kind: 'viridis', lo: Math.max(0, lo), hi, label: (vert ? 'vertical' : 'slant') + ' ionosphere', loLabel: lo < 0 ? '<0' : null };
    ATV.imap = worldMap(p, { height: 320, scale: sc, points: e => {
      const pts = [];
      for (let li = 0; li < L.A.los_sta.length; li++) {
        const j = L.A.los_sta[li], k = L.A.los_sat[li], q = L.A.lion[li * ne + e], az = L.A.laz[li * ne + e], elv = M.A.los_el[(j * M.nSat + k) * ne + e];
        if (q === -32768 || az === 255 || elv === 255 || elv < 10 || !isFinite(M.lat[j])) continue;
        const pp = piercePoint(M.lat[j], M.lon[j], az * 360 / 254, elv);
        const slant = q / L.H.scales.lion;
        pts.push({ label: M.names[j] + ' - ' + M.H.sats[k], lat: pp.lat, lon: pp.lon, value: vert ? slant * pp.vert : slant, slant, vertical: slant * pp.vert, r: 2.6, j, k });
      }
      ATV.ipts = pts;
      return pts;
    }, text: i => { const q = ATV.ipts && ATV.ipts[i]; return q ? 'vertical ' + fmtLen(q.vertical) + ', slant ' + fmtLen(q.slant) : ''; },
    onClick: i => { const q = ATV.ipts && ATV.ipts[i]; if (q) openLink(q.j, q.k); } });
    scaleKey(p, sc, x => fmtLen(x));
  }
  if (zwd) {
    const p = panel(pad, 'Wet delay over time', 'receivers x time, ordered by name; a click opens the receiver');
    const names = M.H.stations.map(s => s.name);
    const order = names.map((_n, j) => j).sort((a, b) => names[a].localeCompare(names[b]));
    const sc = signedScale(zwd, 'wet delay');
    matrix(p, { rows: order.map(j => names[j]), scale: sc, value: (r, e) => zwd[order[r] * ne + e], text: (r, e, v) => fmtLen(v), onClick: (r, e) => { S.ep = e; openNode(order[r]); } });
    scaleKey(p, sc, v => fmtLen(v));
  }
  if (vio) {
    const p = panel(pad, 'Vertical ionosphere over time', 'per receiver, the median over its satellites above 10 degrees of slant x cos(zenith at the shell)');
    const names = M.H.stations.map(s => s.name);
    const order = names.map((_n, j) => j).sort((a, b) => (M.lat[b] || 0) - (M.lat[a] || 0));
    const [lo, hi] = robustSpan(vio, false);
    const sc = { kind: 'viridis', lo: Math.max(0, lo), hi, label: 'vertical ionosphere' };
    matrix(p, { rows: order.map(j => names[j] + ' ' + fmt(M.lat[j], 0)), scale: sc, value: (r, e) => vio[order[r] * ne + e], text: (r, e, v) => fmtLen(v), onClick: (r, e) => { S.ep = e; openNode(order[r]); }, labelW: 120 });
    scaleKey(p, sc, v => fmtLen(v));
    hint(p, 'Rows ordered from north to south (the number after each name is its latitude).');
  }
}

registerView('atmosphere', {
  title: 'Atmosphere', blocks: ['los'],
  init() {},
  show() { renderAtmosphere(viewEl('atmosphere')); },
  time() { if (ATV.nowTiles) ATV.nowTiles(); if (ATV.zmap) ATV.zmap.draw(); if (ATV.imap) ATV.imap.draw(); },
});
