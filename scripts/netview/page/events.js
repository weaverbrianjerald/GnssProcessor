'use strict';
/*
 * Network dashboard: the events.
 *   eventList(parent, indices)  the list every page uses: time, family, plain text, receiver and satellite links,
 *                               the raw row; a click moves the time there and opens the line of sight (or the
 *                               receiver or satellite) the event names
 *   #/events                    every event of the run: counts per 10 minutes by family, filters (family, tier,
 *                               name, time window) and the list
 */

function famKey(i) {
  const E = M.H.events, f = E.families && E.families[E.tier_family[M.A.ev_tier[i]]];
  return f ? f[0] : 'other';
}
function famColor(key) {
  return { slip: C.slip, channel: C.gap, fixing: C.held, datum: C.tree, qc: C.alarm, atmosphere: C.iono, satellite: C.refused }[key] || C.other;
}

/** A list of events (indices into the core block's event columns), newest filter state kept per list. */
function eventList(parent, idx, opts) {
  const o = Object.assign({ limit: 300, families: null }, opts || {});
  const wrap = el('div');
  parent.appendChild(wrap);
  const fams = (M.H.events.families || []).map(f => f[0]);
  const counts = {};
  for (const i of idx) { const k = famKey(i); counts[k] = (counts[k] || 0) + 1; }
  const on = new Set(o.families || fams.filter(k => k !== 'other' || !counts.slip));
  const draw = () => {
    const sel = idx.filter(i => on.has(famKey(i)));
    const chips = fams.filter(k => counts[k]).map(k => {
      const label = (M.H.events.families.find(f => f[0] === k) || [k, k])[1];
      return '<label class="chip"><input type="checkbox" data-f="' + k + '"' + (on.has(k) ? ' checked' : '') + '><span class="lane-dot" style="background:' + famColor(k) + '"></span>' +
        esc(label) + ' <span class="muted">' + counts[k] + '</span></label>';
    }).join('');
    const rows = sel.slice(0, o.limit).map(i => {
      const A = M.A, j = A.ev_sta[i], k = A.ev_sat[i];
      const who = [j >= 0 ? staLink(j) : '', k >= 0 ? satLink(k) : ''].filter(Boolean).join(' &middot; ');
      return '<div data-ev="' + i + '"><span class="lane-dot" style="background:' + famColor(famKey(i)) + '"></span><span class="t">' + clockOf(A.ev_ep[i]) +
        '</span>' + esc(eventText(i)) + (who ? ' ' + who : '') + '<div class="raw">' + esc(M.H.events.tiers[A.ev_tier[i]] + ' ' + M.H.events.names[A.ev_name[i]] + ' ' +
        M.H.events.actions[A.ev_action[i]] + ': ' + rawDetail(i)) + '</div></div>';
    }).join('');
    wrap.innerHTML = '<div class="chips">' + chips + '</div><div class="evlist">' + (rows || '<div class="hint">none</div>') +
      (sel.length > o.limit ? '<div class="hint"><span class="act" data-more="1">show ' + Math.min(sel.length - o.limit, 1000) + ' more</span> of ' + (sel.length - o.limit) + ' not listed</div>' : '') + '</div>';
    wrap.querySelectorAll('input[data-f]').forEach(x => x.addEventListener('change', () => { if (x.checked) on.add(x.dataset.f); else on.delete(x.dataset.f); draw(); }));
    const more = wrap.querySelector('[data-more]');
    if (more) more.addEventListener('click', () => { o.limit += 1000; draw(); });
    wrap.querySelectorAll('[data-ev]').forEach(x => x.addEventListener('click', ev => {
      if (ev.target.closest('a')) return;
      const i = +x.dataset.ev, A = M.A;
      S.ep = A.ev_ep[i];
      if (A.ev_sta[i] >= 0 && A.ev_sat[i] >= 0) openLink(A.ev_sta[i], A.ev_sat[i]);
      else if (A.ev_sat[i] >= 0) openNode(M.nSta + A.ev_sat[i]);
      else if (A.ev_sta[i] >= 0) openNode(A.ev_sta[i]);
      else setEp(A.ev_ep[i]);
    }));
  };
  draw();
  return wrap;
}

// ============================================================================================ #/events

const EVV = { window: 'all', text: '', tier: '' };

function renderEventsView(root, q) {
  root.innerHTML = '';
  const pad = el('div', 'pad');
  root.appendChild(pad);
  viewHead(pad, 'Events', fmtCount(M.nEv) + ' rows over the run' + (M.H.events_dropped && Object.keys(M.H.events_dropped).length ?
    ' (left out: ' + Object.entries(M.H.events_dropped).map(([k, v]) => v + ' ' + esc(k)).join(', ') + ')' : ''));
  missingNote(pad, 'events');
  if (!M.nEv) return;
  const fams = M.H.events.families || [];
  const p = panel(pad, 'Events per 10 minutes, by family');
  const bin = Math.max(1, Math.round(600000 / M.step)), bars = [];
  for (let e0 = 0; e0 < M.nEp; e0 += bin) {
    const v = fams.map(() => 0);
    for (let i = M.evOff[e0]; i < M.evOff[Math.min(M.nEp, e0 + bin)]; i++) v[M.H.events.tier_family[M.A.ev_tier[i]]]++;
    bars.push({ e0, e1: e0 + bin, stack: fams.map((f, k) => ({ v: v[k], color: famColor(f[0]) })) });
  }
  makeChart(canvasIn(p, 'chart tall'), { bars, zeroBased: true, nTicks: 4,
    valueAt: e => { const b = bars[Math.floor(e / bin)]; return b ? fams.map((f, k) => b.stack[k].v ? f[1] + ' ' + b.stack[k].v : '').filter(Boolean).join(', ') : ''; } });
  keyRow(p, fams.map(f => [famColor(f[0]), f[1]]));
  const pl = panel(pad, 'The list', 'filter by family, time and name; a click opens the line of sight, receiver or satellite at that time');
  const tiers = M.H.events.tiers.map((t, i) => [t, i]).sort((a, b) => a[0].localeCompare(b[0]));
  const ctl = el('div', 'row', '<label>Time <select id="ev-win"><option value="all">the whole run</option><option value="near">10 minutes around the cursor</option>' +
    '<option value="at">the cursor epoch</option></select></label><label>Check <select id="ev-tier"><option value="">every check</option>' +
    tiers.map(([t]) => '<option>' + esc(t) + '</option>').join('') + '</select></label><label>Name <input id="ev-text" placeholder="receiver or satellite" size="14"></label>');
  pl.appendChild(ctl);
  const listBox = el('div');
  pl.appendChild(listBox);
  const win = ctl.querySelector('#ev-win'), tierSel = ctl.querySelector('#ev-tier'), txt = ctl.querySelector('#ev-text');
  if (q && q.has('e')) EVV.window = 'at';
  win.value = EVV.window; tierSel.value = EVV.tier; txt.value = EVV.text;
  const fill = () => {
    EVV.window = win.value; EVV.tier = tierSel.value; EVV.text = txt.value.trim().toUpperCase();
    let lo = 0, hi = M.nEp;
    if (EVV.window === 'near') { const h = Math.round(300000 / M.step); lo = Math.max(0, S.ep - h); hi = Math.min(M.nEp, S.ep + h + 1); }
    if (EVV.window === 'at') { lo = S.ep; hi = S.ep + 1; }
    const tierIdx = EVV.tier ? M.H.events.tiers.indexOf(EVV.tier) : -1;
    const idx = [];
    for (let i = M.evOff[lo]; i < M.evOff[hi]; i++) {
      if (tierIdx >= 0 && M.A.ev_tier[i] !== tierIdx) continue;
      if (EVV.text) {
        const j = M.A.ev_sta[i], k = M.A.ev_sat[i];
        const n = (j >= 0 ? M.names[j] : '') + ' ' + (k >= 0 ? M.H.sats[k] : '');
        if (!n.toUpperCase().includes(EVV.text)) continue;
      }
      idx.push(i);
    }
    listBox.innerHTML = '';
    hint(listBox, idx.length + ' rows');
    eventList(listBox, idx, { limit: 400 });
  };
  win.addEventListener('change', fill); tierSel.addEventListener('change', fill);
  txt.addEventListener('change', fill);
  EVV.fill = fill;
  fill();
}

registerView('events', {
  title: 'Events', blocks: [],
  init() {},
  show(_args, q) { renderEventsView(viewEl("events"), q); },
  time() { if (EVV.window !== 'all' && EVV.fill) EVV.fill(); },
  badge() { return M.nEv ? fmtCount(M.nEv) : ''; },
});
