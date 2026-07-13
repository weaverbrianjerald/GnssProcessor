#!/usr/bin/env python3
"""Self-contained per-epoch QC dashboard for any network producer run.

Reduces the run's standard trace CSVs to time-series and writes ONE static
HTML file (inline SVG charts, hover tooltips, light/dark aware) — no server,
no dependencies; open it directly in a browser.

Usage:
    python scripts/qc_dashboard.py <run_dir> [-o out.html]

<run_dir> is the directory holding logs/ (e.g. runs/eq34_full_3h or
build/x64-Release/run_allon_1h). Output defaults to <run_dir>/qc_dashboard.html.

Panels appear only when their source CSV exists:
    network_trace.csv           solve health (clock RMS, stations, pins)
    network_dd_fractional.csv   DD-step + |frac| histogram (needs the
                                network.trace_dd_fractional toggle armed)
    network_vtec_sh_health.csv  iono field health (a00, slant resid, >3sigma)
    network_solution.csv        wire discontinuities (disc_counter deltas)
"""
import argparse, csv, json, os, statistics as st, sys
from collections import defaultdict


def _f(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def _rows(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='') as fh:
        return list(csv.DictReader(fh))


def reduce_run(logs):
    out = {}
    tows = []

    tr = []
    for r in _rows(os.path.join(logs, 'network_trace.csv')):
        e = dict(tow=_f(r.get('tow')), n_present=_f(r.get('n_present')),
                 rms_clock=_f(r.get('rms_clock_m')), max_clock=_f(r.get('max_clock_m')),
                 pins=_f(r.get('n_pivot_pins')))
        if e['tow'] is not None:
            tr.append(e); tows.append(e['tow'])
    out['trace'] = tr

    sh = []
    for r in _rows(os.path.join(logs, 'network_vtec_sh_health.csv')):
        tw, nl = _f(r.get('tow')), _f(r.get('n_los')) or 1
        if tw is None:
            continue
        sh.append(dict(tow=tw, a00=_f(r.get('a00_tecu')),
                       resid_rms=_f(r.get('slant_resid_rms_m')),
                       frac_gt3=(_f(r.get('n_los_gt3sigma')) or 0) / nl))
        tows.append(tw)
    out['sh'] = sh

    series, frac_by_tow = defaultdict(list), defaultdict(list)
    for r in _rows(os.path.join(logs, 'network_dd_fractional.csv')):
        tw, dc, fr = _f(r.get('tow')), _f(r.get('dd_cyc')), _f(r.get('frac_cyc'))
        if tw is None or dc is None:
            continue
        series[(r.get('cons'), r.get('signal'), r.get('sat_i'), r.get('ref_sat'),
                r.get('sta_a'), r.get('sta_b'))].append((tw, dc))
        if fr is not None:
            frac_by_tow[tw].append(abs(fr))
        tows.append(tw)
    dts = sorted({round(b - a, 3) for p in series.values() if len(p) > 1
                  for a, b in zip(sorted(x for x, _ in p), sorted(x for x, _ in p)[1:]) if b > a})
    dt0 = dts[0] if dts else 30.0
    step_by_tow = defaultdict(list)
    for pts in series.values():
        pts.sort()
        for (t0, d0), (t1, d1) in zip(pts, pts[1:]):
            if 0 < t1 - t0 <= dt0 * 1.05:
                step_by_tow[t1].append(abs(d1 - d0))
    out['dd'] = [dict(tow=t, step_med=st.median(v),
                      step_p90=sorted(v)[int(0.9 * len(v))] if len(v) > 9 else max(v))
                 for t, v in sorted(step_by_tow.items())]
    out['frac'] = [dict(tow=t, med=st.median(v)) for t, v in sorted(frac_by_tow.items())]
    allf = [x for v in frac_by_tow.values() for x in v]
    hist = [0] * 10
    for x in allf:
        hist[min(9, int(x / 0.05))] += 1
    out['frac_hist'] = dict(bins=[i * 0.05 for i in range(10)], counts=hist, n=len(allf))

    prev, disc_by_tow, keys_by_tow = {}, defaultdict(int), defaultdict(int)
    for r in _rows(os.path.join(logs, 'network_solution.csv')):
        tw, dc = _f(r.get('tow')), _f(r.get('disc_counter')) or 0
        if tw is None:
            continue
        k = (r.get('sat'), r.get('kind'), r.get('signal'))
        keys_by_tow[tw] += 1
        if k in prev and dc > prev[k]:
            disc_by_tow[tw] += int(dc - prev[k])
        prev[k] = dc
        tows.append(tw)
    out['disc'] = [dict(tow=t, discs=disc_by_tow.get(t, 0)) for t in sorted(keys_by_tow)]

    out['tow0'] = min(tows) if tows else 0.0
    out['dt_s'] = dt0
    return out


TEMPLATE = r'''<meta charset="utf-8">
<title>__TITLE__</title>
<style>
  :root{
    --page:#f9f9f7; --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
    --grid:#e1e0d9; --axis:#c3c2b7; --ring:rgba(11,11,11,.10);
    --s1:#2a78d6; --s2:#eda100; --crit:#d03b3b;
    --mono:"Cascadia Mono",Consolas,ui-monospace,monospace;
  }
  @media (prefers-color-scheme: dark){ :root{
    --page:#0d0d0d; --surface:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7; --muted:#898781;
    --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
    --s1:#3987e5; --s2:#c98500; --crit:#d03b3b;
  }}
  :root[data-theme="light"]{
    --page:#f9f9f7; --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
    --grid:#e1e0d9; --axis:#c3c2b7; --ring:rgba(11,11,11,.10);
    --s1:#2a78d6; --s2:#eda100; --crit:#d03b3b;
  }
  :root[data-theme="dark"]{
    --page:#0d0d0d; --surface:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7; --muted:#898781;
    --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
    --s1:#3987e5; --s2:#c98500; --crit:#d03b3b;
  }
  html,body{background:var(--page);}
  body{font:14px/1.5 system-ui,"Segoe UI",sans-serif;color:var(--ink);margin:0;}
  .wrap{max-width:1080px;margin:0 auto;padding:28px 20px 60px;}
  header.top h1{font-size:20px;font-weight:600;margin:0 0 4px;text-wrap:balance;}
  .runmeta{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0 0;}
  .chip{font:11px/1 var(--mono);color:var(--ink2);border:1px solid var(--ring);
        border-radius:3px;padding:5px 8px;background:var(--surface);}
  .tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin:22px 0 8px;}
  .tile{background:var(--surface);border:1px solid var(--ring);border-radius:4px;padding:12px 14px;}
  .tile .k{font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);}
  .tile .v{font-size:22px;font-weight:600;margin-top:2px;}
  section{margin-top:30px;}
  section>h2{font-size:15px;font-weight:600;margin:0 0 2px;}
  section>p.read{margin:0 0 12px;color:var(--ink2);max-width:72ch;}
  .card{background:var(--surface);border:1px solid var(--ring);border-radius:4px;
        padding:14px 14px 8px;overflow-x:auto;position:relative;}
  .row3{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:12px;}
  .card h3{font-size:12px;font-weight:600;margin:0 0 2px;}
  .card .sub{font:11px var(--mono);color:var(--muted);margin:0 0 6px;}
  svg{display:block;} svg text{font:10.5px var(--mono);fill:var(--muted);}
  svg .glab{font:10px var(--mono);fill:var(--ink2);}
  .legend{display:flex;gap:14px;margin:4px 2px 6px;font:11px var(--mono);color:var(--ink2);}
  .legend i{display:inline-block;width:10px;height:3px;border-radius:2px;vertical-align:middle;margin-right:5px;}
  #tip{position:fixed;pointer-events:none;z-index:9;background:var(--surface);color:var(--ink);
       border:1px solid var(--ring);border-radius:4px;box-shadow:0 2px 8px rgba(0,0,0,.18);
       font:11px var(--mono);padding:6px 8px;display:none;white-space:nowrap;}
  footer{margin-top:34px;font:11px var(--mono);color:var(--muted);}
</style>

<div class="wrap">
<header class="top">
  <h1>__TITLE__</h1>
  <div class="runmeta">__CHIPS__</div>
</header>

<div class="tiles">
  <div class="tile"><div class="k">Epochs graded</div><div class="v" id="t_ep">—</div></div>
  <div class="tile"><div class="k">Stations present</div><div class="v" id="t_nsta">—</div></div>
  <div class="tile"><div class="k">Wire discs / run</div><div class="v" id="t_disc">—</div></div>
  <div class="tile"><div class="k">DD-step first→last</div><div class="v" id="t_dd">—</div></div>
  <div class="tile"><div class="k">med|frac|</div><div class="v" id="t_frac">—</div></div>
  <div class="tile"><div class="k">a00 range</div><div class="v" id="t_a00">—</div></div>
</div>

<section id="s_dd">
  <h2>1 · Carried-DD step per epoch</h2>
  <p class="read">Epoch-adjacent |ΔDD| across fixed LOS series (datum-invariant). Flat and low
  = the frame holds; climbing from ignition = frame churn. Dashed line: the 50-station
  em10+D5v2 reference class (2.35 cyc/ep). Everything left of the 10-epoch marker is what a
  smoke run shows.</p>
  <div class="card"><div class="legend"><span><i style="background:var(--s1)"></i>median</span>
  <span><i style="background:var(--s2)"></i>p90</span></div><div id="c_dd"></div></div>
</section>

<section id="s_disc">
  <h2>2 · Wire discontinuities per epoch</h2>
  <p class="read">Sum of per-key <span style="font-family:var(--mono)">disc_counter</span>
  increments in network_solution.csv — each is the emitter's jump test firing on a
  clock/phase-bias key. Sustained high counts mean the emitted frame re-levels constantly
  even if nothing else looks wrong.</p>
  <div class="card"><div id="c_disc"></div></div>
</section>

<section id="s_sh">
  <h2>3 · Ionosphere field health</h2>
  <p class="read">a00 should stay physical (tens of TECU) and in band. The slant residuals
  measure field⇄per-LOS disagreement — chronic tension shows here first, and misfit the
  field cannot absorb is squeezed into the ambiguity/bias frame.</p>
  <div class="row3">
    <div class="card"><h3>a00</h3><div class="sub">TECU</div><div id="c_a00"></div></div>
    <div class="card"><h3>slant residual RMS</h3><div class="sub">m (vs per-LOS iono)</div><div id="c_resid"></div></div>
    <div class="card"><h3>LOS beyond 3σ</h3><div class="sub">%</div><div id="c_gt3"></div></div>
  </div>
</section>

<section id="s_solve">
  <h2>4 · Solve health</h2>
  <p class="read">What the QC layer watches. If these are flat while panels 1–2 churn, the
  misfit lives in states (iono⇄ambiguity), not observation residuals — post-fit gates will
  be blind to it.</p>
  <div class="row3">
    <div class="card"><h3>clock RMS</h3><div class="sub">m, wire clocks</div><div id="c_clk"></div></div>
    <div class="card"><h3>stations present</h3><div class="sub">count</div><div id="c_nsta"></div></div>
    <div class="card"><h3>pivot pins</h3><div class="sub">count</div><div id="c_pins"></div></div>
  </div>
</section>

<section id="s_hist">
  <h2>5 · Fraction distribution</h2>
  <p class="read">|frac(DD)| over all LOS-epoch samples, 0.05-cycle bins, against the uniform
  expectation (dashed). Flat = no integer structure (AR futile); mass piled near 0 = integers
  exist in this frame.</p>
  <div class="card"><div id="c_hist"></div></div>
</section>

<footer>__FOOTER__ · panels appear only when their source CSV exists (DD panels need
network.trace_dd_fractional) · generated by scripts/qc_dashboard.py</footer>
</div>
<div id="tip"></div>

<script type="application/json" id="data">__DATA__</script>
<script>
const D = JSON.parse(document.getElementById('data').textContent);
const TOW0 = D.tow0, mins = t => (t - TOW0) / 60;
const css = v => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
const tip = document.getElementById('tip');

function niceTicks(lo, hi, n) {
  const span = hi - lo || 1, step0 = span / Math.max(1, n);
  const mag = Math.pow(10, Math.floor(Math.log10(step0)));
  const step = [1, 2, 2.5, 5, 10].map(m => m * mag).find(s => span / s <= n) || mag * 10;
  const t = []; for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) t.push(v);
  return t;
}
const fmt = v => Math.abs(v) >= 1000 ? Math.round(v).toLocaleString() : +v.toFixed(Math.abs(v) < 10 ? 2 : 1);

function panel(el, series, o = {}) {
  series = series.filter(s => s.pts.length);
  if (!series.length) { const sec = document.getElementById(el).closest('section');
    if (sec) sec.style.display = 'none'; return; }
  const node = document.getElementById(el);
  const W = o.w || node.parentElement.clientWidth - 8 || 480, H = o.h || 190;
  const M = { l: 46, r: 14, t: 10, b: 26 };
  const xs = series.flatMap(s => s.pts.map(p => p[0])), ys = series.flatMap(s => s.pts.map(p => p[1]));
  if (o.refY !== undefined) ys.push(o.refY);
  let ylo = o.yDomain ? o.yDomain[0] : (o.y0 ? 0 : Math.min(...ys)), yhi = o.yDomain ? o.yDomain[1] : Math.max(...ys);
  if (!o.yDomain) { const pad = (yhi - ylo) * 0.08 || 1; yhi += pad; if (!o.y0) ylo -= pad; }
  const xlo = Math.min(...xs), xhi = Math.max(...xs);
  const X = x => M.l + (x - xlo) / (xhi - xlo || 1) * (W - M.l - M.r);
  const Y = y => H - M.b - (y - ylo) / (yhi - ylo || 1) * (H - M.t - M.b);
  const NS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`); svg.setAttribute('width', '100%');
  svg.style.minWidth = '300px';
  const add = (tag, at, parent) => { const e = document.createElementNS(NS, tag);
    for (const k in at) e.setAttribute(k, at[k]); (parent || svg).appendChild(e); return e; };
  niceTicks(ylo, yhi, 4).forEach(v => {
    add('line', { x1: M.l, x2: W - M.r, y1: Y(v), y2: Y(v), stroke: css('--grid'), 'stroke-width': 1 });
    add('text', { x: M.l - 6, y: Y(v) + 3.5, 'text-anchor': 'end' }).textContent = fmt(v);
  });
  niceTicks(xlo, xhi, 6).forEach(v => {
    add('text', { x: X(v), y: H - 8, 'text-anchor': 'middle' }).textContent = fmt(v);
  });
  add('text', { x: W - M.r, y: H - 8, 'text-anchor': 'end', class: 'glab' }).textContent = o.xLabel || 'min';
  add('line', { x1: M.l, x2: W - M.r, y1: Y(ylo), y2: Y(ylo), stroke: css('--axis'), 'stroke-width': 1 });
  if (o.refY !== undefined && o.refY <= yhi) {
    add('line', { x1: M.l, x2: W - M.r, y1: Y(o.refY), y2: Y(o.refY), stroke: css('--muted'),
                  'stroke-width': 1, 'stroke-dasharray': '4 4' });
    if (o.refLabel) add('text', { x: M.l + 4, y: Y(o.refY) - 4, class: 'glab' }).textContent = o.refLabel;
  }
  if (o.mark !== undefined && o.mark > xlo && o.mark < xhi) {
    add('line', { x1: X(o.mark), x2: X(o.mark), y1: M.t, y2: H - M.b, stroke: css('--muted'),
                  'stroke-width': 1, 'stroke-dasharray': '2 3' });
    add('text', { x: X(o.mark) + 4, y: M.t + 9, class: 'glab' }).textContent = o.markLabel || '';
  }
  series.forEach(s => {
    if (o.bar) {
      const bw = Math.max(2, (W - M.l - M.r) / s.pts.length - 2);
      s.pts.forEach(p => add('rect', { x: X(p[0]) - bw / 2, y: Y(p[1]), width: bw,
        height: Math.max(0, Y(ylo) - Y(p[1])), fill: s.color, rx: 1.5 }));
    } else {
      add('path', { d: s.pts.map((p, i) => (i ? 'L' : 'M') + X(p[0]).toFixed(1) + ' ' + Y(p[1]).toFixed(1)).join(''),
        fill: 'none', stroke: s.color, 'stroke-width': s.width || 2, 'stroke-linejoin': 'round' });
      const last = s.pts[s.pts.length - 1];
      add('circle', { cx: X(last[0]), cy: Y(last[1]), r: 3, fill: s.color });
    }
  });
  const cross = add('line', { y1: M.t, y2: H - M.b, stroke: css('--axis'), 'stroke-width': 1, opacity: 0 });
  svg.addEventListener('pointermove', ev => {
    const r = svg.getBoundingClientRect();
    const px = (ev.clientX - r.left) * W / r.width;
    const xv = xlo + (px - M.l) / (W - M.l - M.r) * (xhi - xlo);
    const s0 = series[0];
    let best = 0, bd = 1e18;
    s0.pts.forEach((p, i) => { const d = Math.abs(p[0] - xv); if (d < bd) { bd = d; best = i; } });
    const x0 = s0.pts[best][0];
    cross.setAttribute('x1', X(x0)); cross.setAttribute('x2', X(x0)); cross.setAttribute('opacity', 0.7);
    tip.style.display = 'block';
    tip.style.left = Math.min(ev.clientX + 14, innerWidth - 170) + 'px';
    tip.style.top = (ev.clientY + 12) + 'px';
    tip.innerHTML = `t = ${fmt(x0)} ${o.xLabel || 'min'}` + series.map(s => {
      const p = s.pts.find(q => q[0] === x0) || s.pts[best];
      return `<br><span style="color:${s.color}">●</span> ${s.label}: <b>${fmt(p[1])}</b>${o.unit ? ' ' + o.unit : ''}`;
    }).join('');
  });
  svg.addEventListener('pointerleave', () => { tip.style.display = 'none'; cross.setAttribute('opacity', 0); });
  node.appendChild(svg);
}

const s1 = css('--s1'), s2 = css('--s2'), crit = css('--crit');
const set = (id, txt) => { document.getElementById(id).textContent = txt; };

const dd = D.dd.filter(d => d.step_med != null);
panel('c_dd', [
  { pts: dd.map(d => [mins(d.tow), d.step_med]), color: s1, label: 'median' },
  { pts: dd.map(d => [mins(d.tow), d.step_p90]), color: s2, label: 'p90', width: 1.5 },
], { y0: true, h: 240, refY: 2.35, refLabel: '50-sta em10+D5v2 ref (2.35)', unit: 'cyc/ep',
     mark: (D.dt_s * 10) / 60, markLabel: '10 epochs' });

panel('c_disc', [{ pts: D.disc.filter(d => d.discs != null).slice(1).map(d => [mins(d.tow), d.discs]),
  color: s1, label: 'disc events' }], { y0: true, bar: true, h: 200, unit: '/ep' });

const sh = D.sh.filter(d => d.a00 != null);
panel('c_a00', [{ pts: sh.map(d => [mins(d.tow), d.a00]), color: s1, label: 'a00' }],
  { h: 160, unit: 'TECU' });
panel('c_resid', [{ pts: sh.filter(d => d.resid_rms != null).map(d => [mins(d.tow), d.resid_rms]),
  color: crit, label: 'RMS' }], { y0: true, h: 160, unit: 'm' });
panel('c_gt3', [{ pts: sh.filter(d => d.frac_gt3 != null).map(d => [mins(d.tow), +(d.frac_gt3 * 100).toFixed(1)]),
  color: crit, label: '>3σ' }], { h: 160, yDomain: [0, 100], unit: '%' });

const tr = D.trace.filter(d => d.rms_clock != null);
panel('c_clk', [{ pts: tr.map(d => [mins(d.tow), d.rms_clock]), color: s1, label: 'rms' }],
  { y0: true, h: 160, unit: 'm' });
const ns = tr.filter(d => d.n_present != null);
panel('c_nsta', [{ pts: ns.map(d => [mins(d.tow), d.n_present]), color: s1, label: 'present' }],
  { h: 160 });
panel('c_pins', [{ pts: tr.filter(d => d.pins != null).map(d => [mins(d.tow), d.pins]), color: s1, label: 'pins' }],
  { h: 160 });

const hb = D.frac_hist;
if (hb.n > 0)
  panel('c_hist', [{ pts: hb.bins.map((b, i) => [b + 0.025, hb.counts[i]]), color: s1, label: 'count' }],
    { y0: true, bar: true, h: 200, refY: hb.n / 10, refLabel: 'uniform expectation',
      xLabel: '|frac| cyc', unit: '' });
else { const sec = document.getElementById('s_hist'); if (sec) sec.style.display = 'none'; }

const nEp = Math.max(D.trace.length, D.sh.length, D.disc.length);
set('t_ep', nEp || '—');
set('t_disc', D.disc.length ? D.disc.reduce((a, d) => a + (d.discs || 0), 0).toLocaleString() : '—');
set('t_dd', dd.length ? fmt(dd[0].step_med) + ' → ' + fmt(dd[dd.length - 1].step_med) : '—');
const medf = D.frac.map(f => f.med).sort((a, b) => a - b);
set('t_frac', medf.length ? fmt(medf[medf.length >> 1]) : '—');
set('t_a00', sh.length ? fmt(Math.min(...sh.map(d => d.a00))) + '–' + fmt(Math.max(...sh.map(d => d.a00))) : '—');
set('t_nsta', ns.length ? fmt(Math.min(...ns.map(d => d.n_present))) + '–' + fmt(Math.max(...ns.map(d => d.n_present))) : '—');
</script>
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('run_dir')
    ap.add_argument('-o', '--out', default=None)
    args = ap.parse_args()

    logs = os.path.join(args.run_dir, 'logs')
    if not os.path.isdir(logs):
        logs = args.run_dir                        # run_dir may itself be the logs dir
    data = reduce_run(logs)
    if not any(data[k] for k in ('trace', 'sh', 'dd', 'disc')):
        sys.exit(f'no recognized trace CSVs under {logs}')

    name = os.path.basename(os.path.abspath(args.run_dir).rstrip('/\\'))
    span_min = 0.0
    for k in ('trace', 'sh', 'disc'):
        if data[k]:
            span_min = max(span_min, (data[k][-1]['tow'] - data['tow0']) / 60.0)
    chips = [f'<span class="chip">{name}</span>',
             f'<span class="chip">tow0 {data["tow0"]:.0f} · {data["dt_s"]:.0f} s sampling · '
             f'{span_min:.0f} min span</span>']
    html = (TEMPLATE
            .replace('__TITLE__', f'{name} — QC time-series')
            .replace('__CHIPS__', '\n    '.join(chips))
            .replace('__FOOTER__', f'source: {os.path.abspath(logs)}')
            .replace('__DATA__', json.dumps(data)))

    out = args.out or os.path.join(args.run_dir, 'qc_dashboard.html')
    with open(out, 'w', encoding='utf-8') as fh:
        fh.write(html)
    print(f'wrote {out}  ({len(html)//1024} KB) — open directly in a browser')


if __name__ == '__main__':
    main()
