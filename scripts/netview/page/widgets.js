'use strict';
/*
 * Network dashboard: the drawing pieces the layers share.
 *   page parts   view heading, panels, key-figure tiles, small formatting helpers
 *   matrix       rows (receivers, satellites, lines of sight) x epochs, one coloured cell per value
 *   table        sortable rows, a click opens the row's page
 *   sky plot     azimuth / elevation tracks of the satellites seen from one receiver
 *   arc chart    rows x epochs of tracked spans, with event marks
 *   world map    points (receivers, sub-satellite points) coloured by a value
 *   bar chart    values over categories (elevation bins, histogram bins, hours)
 * Every time-based widget joins CHARTS (core.js), so the time cursor moves on all of them together; each takes the
 * size the layout gives its canvas (a matrix's rows fill its height) and says where its time axis lies (xSpan).
 */

// ============================================================================================ page parts

function el(tag, cls, html) {
  const x = document.createElement(tag);
  if (cls) x.className = cls;
  if (html != null) x.innerHTML = html;
  return x;
}
function viewHead(parent, title, sub) {
  const h = el('div', 'vh', '<h2>' + esc(title) + '</h2>' + (sub ? '<span class="sub">' + sub + '</span>' : ''));
  parent.appendChild(h);
  return h;
}
function panel(parent, title, sub) {
  const p = el('div', 'panel');
  if (title) p.appendChild(el('h3', null, esc(title) + (sub ? ' <span class="muted">' + sub + '</span>' : '')));
  parent.appendChild(p);
  return p;
}
function canvasIn(parent, cls) { const c = el('canvas', cls || 'chart'); parent.appendChild(c); return c; }
function keyRow(parent, items) {
  const k = el('div', 'chart-key', items.map(([c, t]) => '<span><i style="background:' + c + '"></i>' + esc(t) + '</span>').join(''));
  parent.appendChild(k);
  return k;
}
/** A note naming what a view cannot show and the key that writes it. */
function missingNote(parent, layer) {
  const m = missingParts(layer);
  if (m.length) hint(parent, 'Not in this run: ' + missingHtml(m) + '.');
}

/** A length in metres with a unit chosen for its size (mm below 1 cm, cm below 1 m). */
function fmtLen(v, digits) {
  if (!isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  if (a < 0.01) return (v * 1000).toFixed(digits == null ? 1 : digits) + ' mm';
  if (a < 1) return (v * 100).toFixed(digits == null ? 1 : digits) + ' cm';
  return v.toFixed(digits == null ? 2 : digits) + ' m';
}
function fmtCount(v) { return isFinite(v) ? Math.round(v).toLocaleString() : 'n/a'; }
function fmtPct(v) { return isFinite(v) ? Math.round(100 * v) + ' %' : 'n/a'; }
/** Typed-array helpers: one row of a [rows, epochs] array, its median and the value at the cursor. */
function rowOf(a, r, n) { return a ? a.subarray(r * (n || M.nEp), (r + 1) * (n || M.nEp)) : null; }
function medianOf(a) {
  const v = [];
  for (let i = 0; i < a.length; i++) if (isFinite(a[i])) v.push(a[i]);
  if (!v.length) return NaN;
  v.sort((p, q) => p - q);
  return v[Math.floor(v.length / 2)];
}
function maxOf(a) { let m = -Infinity; for (let i = 0; i < a.length; i++) if (a[i] > m) m = a[i]; return m === -Infinity ? NaN : m; }
function sumOf(a) { let s = 0; for (let i = 0; i < a.length; i++) if (isFinite(a[i])) s += a[i]; return s; }
function asFloat(a, scale, sentinel) {
  const y = new Float32Array(a.length);
  for (let i = 0; i < a.length; i++) y[i] = a[i] === sentinel ? NaN : a[i] / scale;
  return y;
}
/** The epochs of one hour of the run, for "by hour" figures. */
function hourBins() {
  const per = Math.max(1, Math.round(3600000 / M.step));
  const out = [];
  for (let e0 = 0; e0 < M.nEp; e0 += per) out.push([e0, Math.min(M.nEp, e0 + per)]);
  return out;
}

// ============================================================================================ tiles

/**
 * Key-figure tiles.  items: [{k: label, v: value (html), s: line below, cls: ok|warn|bad|info, href, spark:
 * {y, color, yRange}}]; a tile with an href opens it on a click.
 */
function tiles(parent, items) {
  const box = el('div', 'tiles');
  for (const it of items) {
    const t = el('div', 'tile' + (it.cls ? ' ' + it.cls : ''), '<div class="k" title="' + esc(it.title || it.k) + '">' + esc(it.k) +
      '</div><div class="v">' + it.v + '</div>' + (it.s ? '<div class="s" title="' + esc(it.s.replace(/<[^>]+>/g, '')) + '">' + it.s + '</div>' : ''));
    if (it.href) { t.dataset.href = it.href; t.addEventListener('click', () => { location.hash = it.href; }); }
    box.appendChild(t);
    if (it.spark && it.spark.y) {
      const cv = el('canvas');
      t.appendChild(cv);
      makeChart(cv, { spark: true, series: [{ y: it.spark.y, color: it.spark.color || C.held, width: 1.2, step: it.spark.step }],
        yRange: it.spark.yRange, zeroBased: it.spark.zeroBased, robust: true, valueAt: it.spark.valueAt });
    }
  }
  parent.appendChild(box);
  return box;
}

// ============================================================================================ colour scales

/**
 * A colour scale: {kind: 'viridis' | 'vik' | 'count', lo, hi, label}.  vik is centred on 0 (signed values),
 * viridis spans lo..hi (magnitudes); 'count' is viridis from 0 with absent cells blank.
 */
function scaleColor(sc, v) {
  if (!isFinite(v)) return null;
  if (sc.kind === 'vik') return vik(Math.max(-1, Math.min(1, v / (sc.hi || 1))));
  if (sc.kind === 'count' && v <= 0) return null;
  return seqColor((v - sc.lo) / ((sc.hi - sc.lo) || 1));
}
function scaleRgb(sc, v) {
  if (!isFinite(v)) return null;
  const table = sc.kind === 'vik' ? window.NV_ASSETS.colormaps.vik : seqTable();
  let t = sc.kind === 'vik' ? (Math.max(-1, Math.min(1, v / (sc.hi || 1))) + 1) / 2 : (v - sc.lo) / ((sc.hi - sc.lo) || 1);
  if (sc.kind === 'count' && v <= 0) return null;
  t = Math.max(0, Math.min(1, t));
  const x = t * (table.length - 1), i = Math.min(table.length - 2, Math.floor(x)), u = x - i;
  return [0, 1, 2].map(k => Math.round(255 * (table[i][k] * (1 - u) + table[i + 1][k] * u)));
}
/** Robust range of values (1st to 99th percentile of the finite ones), for a colour scale. */
function robustSpan(values, symmetric, floorHi) {
  const v = [];
  const step = Math.max(1, Math.floor(values.length / 200000));
  for (let i = 0; i < values.length; i += step) if (isFinite(values[i])) v.push(values[i]);
  if (!v.length) return [0, 1];
  v.sort((p, q) => p - q);
  const lo = v[Math.floor(0.01 * (v.length - 1))], hi = v[Math.ceil(0.99 * (v.length - 1))];
  if (symmetric) { const m = Math.max(Math.abs(lo), Math.abs(hi), floorHi || 1e-9); return [-m, m]; }
  return [lo, Math.max(hi, lo + (floorHi || 1e-9))];
}
/**
 * The colour scale of a signed quantity.  vik about zero when both signs matter; when nearly every value has one sign
 * (the wet delay correction is mostly positive), viridis from 0 to the 99th percentile, its lowest colour holding
 * every value below zero (the key says "<0"), so the range the values use gets the whole scale.
 */
function signedScale(values, label) {
  const v = [];
  const step = Math.max(1, Math.floor(values.length / 200000));
  for (let i = 0; i < values.length; i += step) if (isFinite(values[i])) v.push(values[i]);
  if (!v.length) return { kind: 'vik', lo: -1, hi: 1, label };
  v.sort((p, q) => p - q);
  const p1 = v[Math.floor(0.01 * (v.length - 1))], p99 = v[Math.ceil(0.99 * (v.length - 1))];
  if (p99 > 0 && p1 >= -0.15 * p99) return { kind: 'viridis', lo: 0, hi: p99, label, loLabel: '<0' };
  if (p1 < 0 && p99 <= -0.15 * p1) return { kind: 'viridis', lo: p1, hi: 0, label, hiLabel: '>0' };
  const m = Math.max(Math.abs(p1), Math.abs(p99), 1e-9);
  return { kind: 'vik', lo: -m, hi: m, label };
}

function scaleKey(parent, sc, fmtv) {
  const f = fmtv || shortNum;
  const table = sc.kind === 'vik' ? window.NV_ASSETS.colormaps.vik : seqTable();
  const stops = table.map((c, i) => 'rgb(' + c.map(x => Math.round(255 * x)).join(',') + ') ' + Math.round(100 * i / (table.length - 1)) + '%');
  const lo = sc.kind === 'vik' ? -sc.hi : sc.lo;
  const k = el('div', 'ckey', '<span>' + esc(sc.label || '') + '</span><span>' + esc(sc.loLabel || f(lo)) + '</span><span class="ramp" style="background:linear-gradient(90deg,' +
    stops.join(',') + ')"></span><span>' + esc(sc.hiLabel || f(sc.hi)) + '</span>');
  parent.appendChild(k);
  return k;
}

// ============================================================================================ matrix

/** Below the rows of a matrix or an arc chart: the hour labels. */
const ROWS_FOOT = 20;
/** The height of one row: the rows fill the canvas above the hour labels, whatever height the layout gives it. */
function rowsHeight(cv, nRows) { return (cv.clientHeight - ROWS_FOOT) / Math.max(1, nRows); }

/**
 * Rows x epochs, one coloured cell per value.  spec: {rows: [label], value: (r, e) -> number | values: [rows*nEp],
 * scale: {kind, lo, hi, label}, rowH, labelW, onClick(r, e), text(r, e) -> tooltip line, marks: [{r, e, color}]}.
 */
function matrix(parent, spec) {
  const box = el('div', 'matrix');
  parent.appendChild(box);
  const cv = el('canvas');
  box.appendChild(cv);
  const labelW = spec.labelW || 96;
  cv.style.height = (spec.rows.length * (spec.rowH || (spec.rows.length > 80 ? 6 : spec.rows.length > 40 ? 9 : 12)) + ROWS_FOOT) + 'px';
  const m = { cv, spec, labelW, img: null, key: '' };
  const cell = (x, y) => {
    const w = cv.clientWidth, rowH = rowsHeight(cv, spec.rows.length);
    const r = Math.floor(y / rowH);
    if (r < 0 || r >= spec.rows.length || x < labelW) return null;
    const e = Math.round(Math.max(0, Math.min(1, (x - labelW) / (w - labelW - 6))) * (M.nEp - 1));
    return { r, e };
  };
  m.draw = () => {
    const { ctx, w, h } = fitCanvas(cv);
    if (!w || !h) return;
    const key = w + 'x' + h, rowH = rowsHeight(cv, spec.rows.length);
    if (!m.img || m.key !== key) { m.img = renderMatrix(spec, w, h, rowH, labelW); m.key = key; }
    ctx.clearRect(0, 0, w, h);
    ctx.drawImage(m.img, 0, 0, w, h);
    const x = labelW + (M.nEp > 1 ? S.ep / (M.nEp - 1) : 0) * (w - labelW - 6);
    ctx.strokeStyle = C.cursor; ctx.lineWidth = 1.2;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, spec.rows.length * rowH); ctx.stroke();
    if (spec.selRow != null) {
      ctx.strokeStyle = C.sel; ctx.lineWidth = 2;
      ctx.strokeRect(labelW, spec.selRow * rowH, w - labelW - 6, rowH);
    }
  };
  m.redraw = () => { m.img = null; m.draw(); };
  m.xSpan = () => [labelW, cv.clientWidth - 6];
  cv.addEventListener('mousemove', ev => {
    const r = cv.getBoundingClientRect(), c = cell(ev.clientX - r.left, ev.clientY - r.top);
    if (!c) { floatTip(null); return; }
    const v = spec.value ? spec.value(c.r, c.e) : spec.values[c.r * M.nEp + c.e];
    floatTip('<b>' + esc(spec.rows[c.r]) + '</b> ' + clockOf(c.e) + '<br>' + (spec.text ? spec.text(c.r, c.e, v) : esc(fmt(v, 3))), ev);
  });
  cv.addEventListener('mouseleave', () => floatTip(null));
  cv.addEventListener('click', ev => {
    const r = cv.getBoundingClientRect(), c = cell(ev.clientX - r.left, ev.clientY - r.top);
    if (!c) return;
    if (spec.onClick) spec.onClick(c.r, c.e); else setEp(c.e);
  });
  CHARTS.push(m);
  m.draw();
  return m;
}

function renderMatrix(spec, w, h, rowH, labelW) {
  const dpr = window.devicePixelRatio || 1;
  const off = document.createElement('canvas');
  off.width = Math.round(w * dpr); off.height = Math.round(h * dpr);
  const ctx = off.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const nr = spec.rows.length, ne = M.nEp;
  // the cells: one pixel per (column, row) in an image, drawn scaled without smoothing.  When the run has more
  // epochs than the plot has pixels (a long run), each column takes its epochs together: the largest for counts (an
  // isolated slip stays visible), the mean of the finite values otherwise
  const ncol = Math.max(1, Math.min(ne, Math.round((w - labelW - 6) * dpr)));
  const img = new ImageData(ncol, nr);
  const sc = spec.scale, isCount = sc.kind === 'count';
  const bg = C.soft.startsWith('#') ? [parseInt(C.soft.slice(1, 3), 16), parseInt(C.soft.slice(3, 5), 16), parseInt(C.soft.slice(5, 7), 16)] : [235, 235, 230];
  const val = (r, e) => (spec.value ? spec.value(r, e) : spec.values[r * ne + e]);
  for (let r = 0; r < nr; r++) {
    for (let c = 0; c < ncol; c++) {
      let v;
      if (ncol === ne) v = val(r, c);
      else {
        const e0 = Math.floor(c * ne / ncol), e1 = Math.max(e0 + 1, Math.floor((c + 1) * ne / ncol));
        let acc = 0, n = 0, mx = -Infinity;
        for (let e = e0; e < e1; e++) { const x = val(r, e); if (isFinite(x)) { acc += x; n++; if (x > mx) mx = x; } }
        v = n ? (isCount ? mx : acc / n) : NaN;
      }
      const col = scaleRgb(sc, v) || bg;
      const o = (r * ncol + c) * 4;
      img.data[o] = col[0]; img.data[o + 1] = col[1]; img.data[o + 2] = col[2]; img.data[o + 3] = 255;
    }
  }
  const tmp = document.createElement('canvas');
  tmp.width = ncol; tmp.height = nr;
  tmp.getContext('2d').putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(tmp, labelW, 0, w - labelW - 6, nr * rowH);
  for (const mk of spec.marks || []) {
    ctx.fillStyle = mk.color;
    const x = labelW + (ne > 1 ? mk.e / (ne - 1) : 0) * (w - labelW - 6);
    ctx.fillRect(x - 1, mk.r * rowH, 2, rowH);
  }
  ctx.font = Math.min(11, rowH + 1) + 'px system-ui, sans-serif';
  ctx.textBaseline = 'middle'; ctx.textAlign = 'right'; ctx.fillStyle = C.ink;
  const every = rowH < 8 ? Math.ceil(8 / rowH) : 1;
  // rows thinner than 4 px carry no names: they could not be read (the page's own rows are 6 px or more)
  if (rowH >= 4) for (let r = 0; r < nr; r += every) ctx.fillText(spec.rows[r], labelW - 4, r * rowH + rowH / 2);
  ctx.fillStyle = C.muted; ctx.textAlign = 'center'; ctx.textBaseline = 'top'; ctx.font = '10.5px system-ui, sans-serif';
  const ticks = hourTicks(), stepT = ticks.length > 10 ? 2 : 1;
  ticks.forEach((tk, i) => {
    if (i % stepT) return;
    const x = labelW + (ne > 1 ? tk.e / (ne - 1) : 0) * (w - labelW - 6);
    ctx.fillText(tk.label, x, nr * rowH + 4);
  });
  return off;
}

/** The tooltip that follows the pointer over matrices, maps and plots. */
function floatTip(html, ev) {
  let tip = document.getElementById('floattip');
  if (!tip) { tip = el('div', 'floattip'); tip.id = 'floattip'; document.body.appendChild(tip); }
  if (!html) { tip.hidden = true; return; }
  tip.innerHTML = html;
  tip.hidden = false;
  const x = Math.min(ev.clientX + 14, window.innerWidth - tip.offsetWidth - 8);
  const y = Math.min(ev.clientY + 14, window.innerHeight - tip.offsetHeight - 8);
  tip.style.left = x + 'px'; tip.style.top = y + 'px';
}

// ============================================================================================ table

/**
 * A sortable table.  spec: {cols: [{key, label, num, fmt(v, row), title, color(v, row)}], rows: [object],
 * sort: key, desc: bool, onRow(row), maxHeight}.  A click on a header sorts, a click on a row calls onRow.
 */
function dataTable(parent, spec) {
  const wrap = el('div', 'tablewrap');
  if (spec.maxHeight) wrap.style.maxHeight = spec.maxHeight + 'px';
  parent.appendChild(wrap);
  const t = { spec, wrap, sort: spec.sort || spec.cols[0].key, desc: spec.desc != null ? spec.desc : true };
  t.render = () => {
    const rows = spec.rows.slice();
    const col = spec.cols.find(c => c.key === t.sort);
    const val = r => { const v = col && col.sortValue ? col.sortValue(r) : r[t.sort]; return v == null || (typeof v === 'number' && !isFinite(v)) ? null : v; };
    rows.sort((a, b) => {
      const x = val(a), y = val(b);
      if (x == null && y == null) return 0;
      if (x == null) return 1;
      if (y == null) return -1;
      const c = typeof x === 'string' ? x.localeCompare(y) : x - y;
      return t.desc ? -c : c;
    });
    const head = '<tr>' + spec.cols.map(c => '<th data-k="' + c.key + '"' + (c.num ? ' class="num"' : '') + (c.title ? ' title="' + esc(c.title) + '"' : '') + '>' +
      esc(c.label) + (t.sort === c.key ? ' <span class="arrow">' + (t.desc ? '&darr;' : '&uarr;') + '</span>' : '') + '</th>').join('') + '</tr>';
    const body = rows.map((r, i) => '<tr data-i="' + i + '">' + spec.cols.map(c => {
      const v = r[c.key];
      const txt = c.fmt ? c.fmt(v, r) : (v == null ? '' : typeof v === 'number' ? (isFinite(v) ? (Number.isInteger(v) ? v.toLocaleString() : shortNum(v)) : '') : esc(v));
      const bg = c.color ? c.color(v, r) : null;
      return '<td' + (c.num ? ' class="num"' : '') + (bg ? ' style="background:' + bg + ';color:' + textOn(bg) + '"' : '') + '>' + txt + '</td>';
    }).join('') + '</tr>').join('');
    wrap.innerHTML = '<table class="dtable"><thead>' + head + '</thead><tbody>' + body + '</tbody></table>';
    wrap.querySelectorAll('th').forEach(th => th.addEventListener('click', () => {
      const k = th.dataset.k;
      if (t.sort === k) t.desc = !t.desc; else { t.sort = k; t.desc = true; }
      t.render();
    }));
    wrap.querySelectorAll('tbody tr').forEach(tr => tr.addEventListener('click', ev => {
      if (ev.target.closest('a')) return;
      if (spec.onRow) spec.onRow(rows[+tr.dataset.i]);
    }));
  };
  t.render();
  return t;
}
/** Black or white text on a coloured cell, by the colour's lightness. */
function textOn(rgb) {
  const m = /rgba?\((\d+),\s*(\d+),\s*(\d+)/.exec(rgb);
  if (!m) return C.ink;
  const l = 0.299 * m[1] + 0.587 * m[2] + 0.114 * m[3];
  return l > 140 ? '#111' : '#fff';
}

// ============================================================================================ sky plot

/**
 * Azimuth / elevation tracks.  spec: {tracks: [{label, az: Float32Array (deg), el: Float32Array (deg), color, value
 * (Float32Array, optional; coloured with scale)}], scale, onClick(i)}.  The dot of each track is at the cursor epoch.
 */
function skyPlot(parent, spec) {
  const cv = el('canvas', 'sky');
  parent.appendChild(cv);
  const sp = { cv, spec, img: null, key: '' };
  const xy = (R, cx, cy, az, elv) => {
    const r = R * (90 - elv) / 90, a = az * Math.PI / 180;
    return [cx + r * Math.sin(a), cy - r * Math.cos(a)];
  };
  const geom = () => { const w = cv.clientWidth, h = cv.clientHeight, R = Math.min(w, h) / 2 - 18; return { w, h, R, cx: w / 2, cy: h / 2 }; };
  sp.draw = () => {
    const { ctx, w, h } = fitCanvas(cv);
    if (!w || !h) return;
    const g = geom(), key = w + 'x' + h;
    if (!sp.img || sp.key !== key) {
      const dpr = window.devicePixelRatio || 1;
      const off = document.createElement('canvas');
      off.width = Math.round(w * dpr); off.height = Math.round(h * dpr);
      const c = off.getContext('2d');
      c.setTransform(dpr, 0, 0, dpr, 0, 0);
      c.strokeStyle = C.line; c.fillStyle = C.muted; c.font = '10.5px system-ui, sans-serif'; c.textAlign = 'center'; c.textBaseline = 'middle';
      for (const elv of [0, 30, 60]) { c.beginPath(); c.arc(g.cx, g.cy, g.R * (90 - elv) / 90, 0, 2 * Math.PI); c.stroke(); }
      for (let az = 0; az < 360; az += 30) {
        const [x, y] = xy(g.R, g.cx, g.cy, az, 0);
        c.beginPath(); c.moveTo(g.cx, g.cy); c.lineTo(x, y); c.stroke();
      }
      [['N', 0], ['E', 90], ['S', 180], ['W', 270]].forEach(([t, az]) => { const [x, y] = xy(g.R + 10, g.cx, g.cy, az, 0); c.fillText(t, x, y); });
      c.fillText('30', g.cx + 4, g.cy - g.R * 60 / 90 - 6); c.fillText('60', g.cx + 4, g.cy - g.R * 30 / 90 - 6);
      // a track is drawn through one point every 5 minutes (and the ends of each arc): the angles are stored in
      // whole degrees, and a point per epoch would draw their rounding as a zigzag
      const stride = Math.max(1, Math.round(300000 / M.step));
      for (const tr of spec.tracks) {
        c.lineWidth = 1.6;
        let lastE = -1, px = 0, py = 0;
        for (let e = 0; e < M.nEp; e++) {
          const a = tr.az[e], v = tr.el[e];
          if (!isFinite(a) || !isFinite(v)) { lastE = -1; continue; }
          const arcEnd = e + 1 >= M.nEp || !isFinite(tr.az[e + 1]) || !isFinite(tr.el[e + 1]);
          if (lastE >= 0 && e - lastE < stride && !arcEnd) continue;
          const [x, y] = xy(g.R, g.cx, g.cy, a, v);
          if (lastE >= 0) {
            c.strokeStyle = tr.value && spec.scale ? (scaleColor(spec.scale, tr.value[e]) || withAlpha(tr.color, 0.5)) : withAlpha(tr.color, 0.6);
            c.beginPath(); c.moveTo(px, py); c.lineTo(x, y); c.stroke();
          }
          lastE = e; px = x; py = y;
        }
      }
      sp.img = off; sp.key = key;
    }
    ctx.clearRect(0, 0, w, h);
    ctx.drawImage(sp.img, 0, 0, w, h);
    ctx.font = '10.5px system-ui, sans-serif'; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    spec.tracks.forEach((tr, i) => {
      const a = tr.az[S.ep], v = tr.el[S.ep];
      if (!isFinite(a) || !isFinite(v)) return;
      const [x, y] = xy(g.R, g.cx, g.cy, a, v);
      ctx.fillStyle = tr.color; ctx.strokeStyle = C.ink; ctx.lineWidth = i === spec.sel ? 2.5 : 1;
      ctx.beginPath(); ctx.arc(x, y, i === spec.sel ? 6 : 4.5, 0, 2 * Math.PI); ctx.fill(); ctx.stroke();
      ctx.fillStyle = C.ink; ctx.fillText(tr.label, x + 7, y);
    });
  };
  const nearest = ev => {
    const r = cv.getBoundingClientRect(), g = geom(), mx = ev.clientX - r.left, my = ev.clientY - r.top;
    let best = -1, bd = 14;
    spec.tracks.forEach((tr, i) => {
      const a = tr.az[S.ep], v = tr.el[S.ep];
      if (!isFinite(a) || !isFinite(v)) return;
      const [x, y] = xy(g.R, g.cx, g.cy, a, v), d = Math.hypot(x - mx, y - my);
      if (d < bd) { bd = d; best = i; }
    });
    return best;
  };
  cv.addEventListener('mousemove', ev => {
    const i = nearest(ev);
    cv.style.cursor = i >= 0 ? 'pointer' : 'crosshair';
    if (i < 0) { floatTip(null); return; }
    const tr = spec.tracks[i];
    floatTip('<b>' + esc(tr.label) + '</b> azimuth ' + fmt(tr.az[S.ep], 0) + ', elevation ' + fmt(tr.el[S.ep], 0) + ' degrees' + (tr.tip ? '<br>' + tr.tip(S.ep) : ''), ev);
  });
  cv.addEventListener('mouseleave', () => floatTip(null));
  cv.addEventListener('click', ev => { const i = nearest(ev); if (i >= 0 && spec.onClick) spec.onClick(i); });
  CHARTS.push(sp);
  sp.draw();
  return sp;
}

// ============================================================================================ arc chart

/**
 * Rows x epochs of tracked spans with event marks.  spec: {rows: [{label, present: (e) -> bool, color: (e) -> css
 * colour | null, marks: [{e, color}]}], rowH, labelW, onClick(r, e), text(r, e)}.
 */
function arcChart(parent, spec) {
  const box = el('div', 'arcs');
  parent.appendChild(box);
  const cv = el('canvas');
  box.appendChild(cv);
  const labelW = spec.labelW || 86;
  cv.style.height = (spec.rows.length * (spec.rowH || 11) + ROWS_FOOT) + 'px';
  const ac = { cv, spec, img: null, key: '' };
  const X = (w, e) => labelW + (M.nEp > 1 ? e / (M.nEp - 1) : 0) * (w - labelW - 6);
  ac.xSpan = () => [X(cv.clientWidth, 0), X(cv.clientWidth, M.nEp - 1)];
  ac.draw = () => {
    const { ctx, w, h } = fitCanvas(cv);
    if (!w || !h) return;
    const key = w + 'x' + h, rowH = rowsHeight(cv, spec.rows.length);
    if (!ac.img || ac.key !== key) {
      const dpr = window.devicePixelRatio || 1;
      const off = document.createElement('canvas');
      off.width = Math.round(w * dpr); off.height = Math.round(h * dpr);
      const c = off.getContext('2d');
      c.setTransform(dpr, 0, 0, dpr, 0, 0);
      c.font = '10.5px system-ui, sans-serif'; c.textBaseline = 'middle';
      spec.rows.forEach((row, r) => {
        const y0 = r * rowH;
        if (r % 2) { c.fillStyle = C.soft; c.fillRect(labelW, y0, w - labelW - 6, rowH); }
        c.fillStyle = C.ink; c.textAlign = 'right'; c.fillText(row.label, labelW - 4, y0 + rowH / 2);
        const dx = Math.max(1, (w - labelW - 6) / Math.max(1, M.nEp - 1));
        for (let e = 0; e < M.nEp; e++) {
          if (!row.present(e)) continue;
          c.fillStyle = (row.color && row.color(e)) || withAlpha(C.held, 0.55);
          c.fillRect(X(w, e) - dx / 2, y0 + 2, dx + 0.6, rowH - 4);
        }
        for (const mk of row.marks || []) {
          c.fillStyle = mk.color;
          c.fillRect(X(w, mk.e) - 1, y0, 2.2, rowH);
        }
      });
      c.fillStyle = C.muted; c.textAlign = 'center'; c.textBaseline = 'top';
      const ticks = hourTicks(), stepT = ticks.length > 10 ? 2 : 1;
      ticks.forEach((tk, i) => { if (!(i % stepT)) c.fillText(tk.label, X(w, tk.e), spec.rows.length * rowH + 4); });
      ac.img = off; ac.key = key;
    }
    ctx.clearRect(0, 0, w, h);
    ctx.drawImage(ac.img, 0, 0, w, h);
    const x = X(w, S.ep);
    ctx.strokeStyle = C.cursor; ctx.lineWidth = 1.2;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, spec.rows.length * rowH); ctx.stroke();
  };
  const at = ev => {
    const r = cv.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top, w = cv.clientWidth;
    const row = Math.floor(y / rowsHeight(cv, spec.rows.length));
    if (row < 0 || row >= spec.rows.length) return null;
    const e = Math.round(Math.max(0, Math.min(1, (x - labelW) / (w - labelW - 6))) * (M.nEp - 1));
    return { r: row, e };
  };
  cv.addEventListener('mousemove', ev => {
    const a = at(ev);
    if (!a) { floatTip(null); return; }
    floatTip('<b>' + esc(spec.rows[a.r].label) + '</b> ' + clockOf(a.e) + (spec.text ? '<br>' + spec.text(a.r, a.e) : ''), ev);
  });
  cv.addEventListener('mouseleave', () => floatTip(null));
  cv.addEventListener('click', ev => { const a = at(ev); if (a && spec.onClick) spec.onClick(a.r, a.e); });
  CHARTS.push(ac);
  ac.draw();
  return ac;
}

// ============================================================================================ world map

/**
 * Points on a world map.  spec: {points: [{label, lat, lon, value, ring, sel}], scale, onClick(i), text(i),
 * height}; points with a NaN value are drawn grey.  value may be a function of the epoch (redrawn with time).
 */
function worldMap(parent, spec) {
  const box = el('div', 'mapbox');
  parent.appendChild(box);
  const cv = el('canvas');
  if (spec.height) cv.style.height = spec.height + 'px';
  box.appendChild(cv);
  const mp = { cv, spec, img: null, key: '' };
  const rect = (w, h) => {
    const aw = w - 8, ah = h - 8, s = Math.min(aw / 360, ah / 180);
    return { x0: (w - 360 * s) / 2, y0: (h - 180 * s) / 2, s };
  };
  const XY = (rc, lat, lon) => [rc.x0 + (lon + 180) * rc.s, rc.y0 + (90 - lat) * rc.s];
  mp.draw = () => {
    const { ctx, w, h } = fitCanvas(cv);
    if (!w || !h) return;
    const rc = rect(w, h), key = w + 'x' + h;
    if (!mp.img || mp.key !== key) {
      const dpr = window.devicePixelRatio || 1;
      const off = document.createElement('canvas');
      off.width = Math.round(w * dpr); off.height = Math.round(h * dpr);
      const c = off.getContext('2d');
      c.setTransform(dpr, 0, 0, dpr, 0, 0);
      c.fillStyle = C.panel; c.fillRect(rc.x0, rc.y0, 360 * rc.s, 180 * rc.s);
      c.strokeStyle = C.line; c.lineWidth = 0.8;
      const co = window.NV_ASSETS.coastline;
      if (co) {
        c.beginPath();
        let pen = false;
        for (let i = 0; i < co.lon.length; i++) {
          const lo = co.lon[i], la = co.lat[i];
          if (lo == null || la == null) { pen = false; continue; }
          const [x, y] = XY(rc, la, lo);
          if (pen) c.lineTo(x, y); else c.moveTo(x, y);
          pen = true;
        }
        c.stroke();
      }
      c.strokeStyle = C.grid;
      for (let la = -60; la <= 60; la += 30) { const [x0, y] = XY(rc, la, -180), [x1] = XY(rc, la, 180); c.beginPath(); c.moveTo(x0, y); c.lineTo(x1, y); c.stroke(); }
      mp.img = off; mp.key = key;
    }
    ctx.clearRect(0, 0, w, h);
    ctx.drawImage(mp.img, 0, 0, w, h);
    ctx.font = '10px system-ui, sans-serif'; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    const pts = typeof spec.points === 'function' ? spec.points(S.ep) : spec.points;
    mp.last = pts.map(p => {
      if (!isFinite(p.lat) || !isFinite(p.lon)) return null;
      const [x, y] = XY(rc, p.lat, p.lon);
      const v = typeof p.value === 'function' ? p.value(S.ep) : p.value;
      ctx.fillStyle = scaleColor(spec.scale, v) || C.float;
      ctx.strokeStyle = p.sel ? C.sel : C.ink; ctx.lineWidth = p.sel ? 2.5 : 0.8;
      const rr = p.r || (p.sel ? 6 : 4.5);
      ctx.beginPath();
      if (p.shape === 'diamond') { ctx.moveTo(x, y - rr); ctx.lineTo(x + rr, y); ctx.lineTo(x, y + rr); ctx.lineTo(x - rr, y); ctx.closePath(); }
      else ctx.arc(x, y, rr, 0, 2 * Math.PI);
      ctx.fill(); ctx.stroke();
      if (p.ring) { ctx.strokeStyle = C.alarm; ctx.lineWidth = 2; ctx.beginPath(); ctx.arc(x, y, rr + 3, 0, 2 * Math.PI); ctx.stroke(); }
      if (spec.labels) { ctx.fillStyle = C.ink; ctx.fillText(p.label, x + rr + 2, y); }
      return [x, y];
    });
  };
  const nearest = ev => {
    if (!mp.last) return -1;
    const r = cv.getBoundingClientRect(), mx = ev.clientX - r.left, my = ev.clientY - r.top;
    let best = -1, bd = 12;
    mp.last.forEach((q, i) => { if (q) { const d = Math.hypot(q[0] - mx, q[1] - my); if (d < bd) { bd = d; best = i; } } });
    return best;
  };
  cv.addEventListener('mousemove', ev => {
    const i = nearest(ev);
    cv.style.cursor = i >= 0 ? 'pointer' : 'default';
    const pts = typeof spec.points === 'function' ? spec.points(S.ep) : spec.points;
    if (i < 0) { floatTip(null); return; }
    floatTip('<b>' + esc(pts[i].label) + '</b>' + (spec.text ? '<br>' + spec.text(i) : ''), ev);
  });
  cv.addEventListener('mouseleave', () => floatTip(null));
  cv.addEventListener('click', ev => { const i = nearest(ev); if (i >= 0 && spec.onClick) spec.onClick(i); });
  mp.redraw = () => { mp.img = null; mp.draw(); };
  CHARTS.push(mp);
  mp.draw();
  return mp;
}

// ============================================================================================ bar chart

/**
 * Values over categories (not over time).  spec: {labels: [string], series: [{v: [number], color, line}],
 * yLabel, yFmt, logY, stacked}.  Static; a tooltip names the category and the values.
 */
function barChart(parent, spec, cls) {
  const cv = canvasIn(parent, cls || 'chart');
  const n = spec.labels.length;
  const draw = () => {
    const { ctx, w, h } = fitCanvas(cv);
    if (!w || !h) return;
    const l = 50, r = 8, t = 6, b = 18, pw = w - l - r, ph = h - t - b;
    let lo = 0, hi = 0;
    for (let i = 0; i < n; i++) {
      let acc = 0;
      for (const s of spec.series) { const v = s.v[i]; if (!isFinite(v)) continue; if (spec.stacked && !s.line) { acc += v; hi = Math.max(hi, acc); } else { hi = Math.max(hi, v); lo = Math.min(lo, v); } }
    }
    if (spec.yMax != null) hi = spec.yMax;
    if (!(hi > lo)) hi = lo + 1;
    const Y = v => t + (1 - (v - lo) / (hi - lo)) * ph;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = C.panel; ctx.fillRect(l, t, pw, ph);
    ctx.font = '10.5px system-ui, sans-serif'; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (const v of niceTicks(lo, hi, 3)) {
      ctx.strokeStyle = C.grid; ctx.beginPath(); ctx.moveTo(l, Y(v)); ctx.lineTo(w - r, Y(v)); ctx.stroke();
      ctx.fillStyle = C.muted; ctx.fillText((spec.yFmt || shortNum)(v), l - 4, Y(v));
    }
    const bw = pw / n;
    const bars = spec.series.filter(s => !s.line);
    for (let i = 0; i < n; i++) {
      let acc = 0;
      bars.forEach((s, k) => {
        const v = s.v[i];
        if (!isFinite(v) || v === 0) return;
        ctx.fillStyle = s.color;
        if (spec.stacked) { ctx.fillRect(l + i * bw + 1, Y(acc + v), Math.max(1, bw - 2), Y(acc) - Y(acc + v)); acc += v; }
        else { const sw = (bw - 2) / bars.length; ctx.fillRect(l + i * bw + 1 + k * sw, Y(Math.max(0, v)), Math.max(1, sw), Math.abs(Y(v) - Y(0))); }
      });
    }
    for (const s of spec.series.filter(x => x.line)) {
      ctx.strokeStyle = s.color; ctx.lineWidth = 1.6; ctx.setLineDash(s.dash || []); ctx.beginPath();
      let pen = false;
      for (let i = 0; i < n; i++) { const v = s.v[i]; if (!isFinite(v)) { pen = false; continue; } const x = l + (i + 0.5) * bw; if (pen) ctx.lineTo(x, Y(v)); else ctx.moveTo(x, Y(v)); pen = true; }
      ctx.stroke(); ctx.setLineDash([]);
    }
    ctx.fillStyle = C.muted; ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    const every = Math.max(1, Math.ceil(n / Math.max(1, pw / 34)));
    for (let i = 0; i < n; i += every) ctx.fillText(spec.labels[i], l + (i + 0.5) * bw, h - b + 3);
    ctx.strokeStyle = C.line; ctx.strokeRect(l + 0.5, t + 0.5, pw - 1, ph - 1);
  };
  cv.addEventListener('mousemove', ev => {
    const rc = cv.getBoundingClientRect(), x = ev.clientX - rc.left, w = cv.clientWidth;
    const i = Math.floor((x - 50) / ((w - 58) / n));
    if (i < 0 || i >= n) { floatTip(null); return; }
    floatTip('<b>' + esc(spec.labels[i]) + '</b><br>' + spec.series.map(s => esc(s.name || '') + ' ' + (spec.tipFmt || shortNum)(s.v[i])).join('<br>'), ev);
  });
  cv.addEventListener('mouseleave', () => floatTip(null));
  const obj = { cv, draw };
  CHARTS.push(obj);
  draw();
  return obj;
}
