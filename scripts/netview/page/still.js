'use strict';
/*
 * Network dashboard: stills.  One part of one view drawn alone on a white page, as a picture for a poster or a slide.
 *
 * A view's address with still=PART shows only that part, without the page around it:
 *     #/network?t=12:00&l=map&still=links&w=772&h=400
 * PART is  links     the network picture of #/network (rings by hops, or the world map with l=map);
 *          timeline  the event lanes, on any view (a view of one receiver or satellite adds its own lane);
 *          tiles     the view's key figures, as data in the report (no picture);
 *          or the title of one of the view's panels, or its beginning ("Receivers over time", "Wet delay").
 * The part is laid over the window at w x h CSS pixels (the whole window when they are absent) and its drawing takes
 * the height its legend leaves, so the picture needs no cropping.  How large the type is against the picture is the
 * browser's magnification: netview.py still sets the window to w x h and the device scale to the magnification.
 *
 * What the page keeps in buttons comes from the address here (STILL_CHOICES), next to the page's own t, f, b, l, s:
 *     key=0 no legend, title=1 keep the panel's title, cursor=0 no time cursor,
 *     names=0|sat, held=0, float=0 (the network picture), m=METRIC (a matrix, by its button's key),
 *     iono=vertical|slant, colours=turbo|viridis.
 * The page leaves a report for the command in <script id="nv-still-report">: what was drawn and when, the sizes of
 * the type in it, where its time axis lies, or why it could not be drawn.  Words that mean something only to the
 * page's makers are replaced in a still (STILL_WORDS); the page itself keeps them.
 */

const STILL = { on: false, q: null, type: null };

/** Words of the page that are its makers' own, and what a still says instead: [pattern, plain words]. */
const STILL_WORDS = [
  [/Held-out user receiver/g, 'Test receiver'],
];
function stillWords(text) {
  for (const [re, plain] of STILL_WORDS) text = text.replace(re, plain);
  return text;
}

/** The choices a view keeps in memory behind its buttons and boxes, set from the address: a still has no buttons. */
const STILL_CHOICES = {
  m: v => { OV.rxMetric = OV.satMetric = RXV.metric = SATV.metric = v; },
  iono: v => { ATV.iono = v === 'slant' ? 'slant' : 'vertical'; },
  names: v => { S.showLabels = v === 'sat' ? 'sat' : v !== '0'; },
  held: v => { S.showHeld = v !== '0'; },
  float: v => { S.showFloat = v !== '0'; },
  colours: v => { if (window.NV_ASSETS.colormaps[v]) S.seqMap = v; },
};

/** Before the first view is drawn: the page hides, the light colours and the address's choices are taken. */
function stillPrepare(q) {
  STILL.on = true; STILL.q = q;
  document.documentElement.classList.add('still');
  document.documentElement.dataset.theme = 'light';
  readColors();
  if (q.get('cursor') === '0') C.cursor = 'transparent';
  for (const [k, take] of Object.entries(STILL_CHOICES)) if (q.has(k)) take(q.get(k));
  stillWatchCanvasText();
}

/** Every text a canvas draws in a still passes here: plain words go in, its size is noted for the report. */
function stillWatchCanvasText() {
  const proto = CanvasRenderingContext2D.prototype, draw = proto.fillText;
  proto.fillText = function (text, ...rest) {
    text = stillWords(String(text));
    const px = /([\d.]+)px/.exec(this.font);
    if (STILL.type && px && text.trim()) STILL.type.push(+px[1]);
    return draw.call(this, text, ...rest);
  };
}

/** After the first view is drawn: find the part, lay it over the window, draw it at that size and leave the report. */
function stillCompose() {
  const q = STILL.q, name = q.get('still'), view = viewEl(R.name);
  try {
    const failed = view.querySelector(':scope > .error');
    if (failed) throw new Error(failed.textContent);
    if (name === 'tiles') { stillReport(Object.assign(stillFacts(name), { tiles: stillTiles(view) })); return; }
    const part = stillPart(view, name);
    stillCheckMetric(part, name, q.get('m'));
    stillLayOut(part, q);
    const notes = stillDraw(part);
    stillReport(Object.assign(stillFacts(name, part), { title: stillTitle(part), size: [part.clientWidth, part.clientHeight],
      type_px: stillTypeSizes(part), axis: stillAxis(part), notes }));
  } catch (err) {
    stillFail(err.message);
  }
}

/** A metric named in the address must be one of the matrix's buttons: the page would quietly show its first. */
function stillCheckMetric(part, name, m) {
  const buttons = Array.from(part.querySelectorAll('.seg button[data-m]'));
  if (!m || !buttons.length || buttons.some(b => b.dataset.m === m && b.classList.contains('on'))) return;
  throw new Error('"' + name + '" has no metric "' + m + '"; its metrics: ' + buttons.map(b => b.dataset.m + ' (' + b.textContent + ')').join(', '));
}

/** The part over the window: its size from the address, its legend and title as asked, its words plain. */
function stillLayOut(part, q) {
  const root = document.documentElement, w = +q.get('w'), h = +q.get('h');
  root.classList.toggle('still-nokey', q.get('key') === '0');
  root.classList.toggle('still-title', q.get('title') === '1');
  if (w > 0 && h > 0) { root.style.setProperty('--still-w', w + 'px'); root.style.setProperty('--still-h', h + 'px'); }
  part.classList.add('still-part');
  const walk = document.createTreeWalker(part, NodeFilter.SHOW_TEXT);      // titles and legends; canvas text is replaced as it is drawn
  for (let n = walk.nextNode(); n; n = walk.nextNode()) {
    const plain = stillWords(n.nodeValue);
    if (plain !== n.nodeValue) n.nodeValue = plain;
  }
}

/** Draw the part at its size, now and whenever a still opened by hand changes its window; returns notes for the report. */
function stillDraw(part) {
  const tl = document.getElementById('timeline'), lanes = part.contains(tl) ? timelineLanes().length : 0;
  const draw = () => {
    if (lanes) TL.laneH = (tl.clientHeight - TL.top - TL.foot) / lanes;      // the lanes share the height above the foot
    STILL.type = [];
    redrawAll();
  };
  draw();
  window.addEventListener('resize', draw);
  return lanes && TL.laneH < 11 ? ['the lanes are thinner than their labels: the timeline needs ' +
    Math.ceil(TL.top + TL.foot + 11 * lanes) + ' px of height'] : [];
}

/** What every report says: the view, the part, the cursor's time and the choices in force. */
function stillFacts(name, part) {
  const cf = curFamily(), metric = part && part.querySelector('.seg button.on[data-m]');
  const shows = { signals: cf ? famLabel(cf.f) : '', tree: cf ? cf.sig : '', layout: S.layout, colours: S.seqMap };
  if (metric) shows.metric = metric.textContent;
  return { ok: true, view: [R.name].concat(R.args).join('/'), part: name, run: M.H.run, date: gpsDate(0).toISOString().slice(0, 10),
    time: clockOf(S.ep), epoch: S.ep, epochs: M.nEp, receivers: M.nSta, satellites: M.nSat, shows };
}

/** The titled panels of a view: [{title, el}]. */
function stillPanels(view) {
  return Array.from(view.querySelectorAll('.panel, figure'), el => ({ el, title: stillTitle(el) })).filter(p => p.title);
}
/** A panel's title without the note that follows it in grey. */
function stillTitle(el) {
  const h = el.querySelector(':scope > h3, :scope > figcaption');
  return h && h.firstChild ? h.firstChild.textContent.trim() : '';
}

/** The element a part name stands for on the shown view; a name that fits no panel, or several, is refused. */
function stillPart(view, name) {
  if (name === 'timeline') return document.getElementById('timebar');
  if (name === 'links' && R.name === 'network') return document.getElementById('graph-pane');
  const panels = stillPanels(view), want = name.toLowerCase();
  for (const fits of [t => t === want, t => t.startsWith(want), t => t.includes(want)]) {
    const hits = panels.filter(p => fits(p.title.toLowerCase()));
    if (hits.length === 1) return hits[0].el;
    if (hits.length > 1) throw new Error('"' + name + '" fits ' + hits.length + ' parts of the view ' + R.name + ': ' + hits.map(p => p.title).join('; '));
  }
  const parts = ['timeline', 'tiles'].concat(R.name === 'network' ? ['links'] : [], panels.map(p => p.title));
  throw new Error('no part "' + name + '" on the view ' + R.name + '; its parts: ' + parts.join('; '));
}

/** The view's key figures as data: [{label, value, unit, note, level}], in plain words. */
function stillTiles(view) {
  const box = view.querySelector('.tiles');
  if (!box) throw new Error('the view ' + R.name + ' has no key figures');
  const text = x => (x ? stillWords(x.textContent.replace(/\s+/g, ' ').trim()) : '');
  return Array.from(box.querySelectorAll('.tile'), t => {
    const v = t.querySelector('.v'), unit = v && v.querySelector('small');
    return { label: text(t.querySelector('.k')), value: text(v && v.firstChild), unit: text(unit), note: text(t.querySelector('.s')),
      level: ['ok', 'warn', 'bad', 'info'].find(c => t.classList.contains(c)) || '' };
  });
}

/** Smallest and largest type in the picture, CSS pixels: canvas text drawn since the layout, and the part's shown text. */
function stillTypeSizes(part) {
  const sizes = STILL.type.slice();
  const walk = document.createTreeWalker(part, NodeFilter.SHOW_TEXT);
  for (let n = walk.nextNode(); n; n = walk.nextNode()) {
    const box = n.parentElement;
    if (n.nodeValue.trim() && box.getClientRects().length) sizes.push(parseFloat(getComputedStyle(box).fontSize));
  }
  return sizes.length ? [Math.min(...sizes), Math.max(...sizes)] : null;
}

/** Where the part's time axis lies, as fractions of the picture's width (to line stills up on one axis), or null. */
function stillAxis(part) {
  const tl = document.getElementById('timeline');
  const chart = CHARTS.find(c => c.xSpan && part.contains(c.cv));
  const cv = part.contains(tl) ? tl : chart ? chart.cv : null;
  if (!cv) return null;
  const span = cv === tl ? [tlX(0, tl.clientWidth), tlX(M.nEp - 1, tl.clientWidth)] : chart.xSpan();
  const box = part.getBoundingClientRect(), left = cv.getBoundingClientRect().left - box.left;
  return span.map(x => +((left + x) / box.width).toFixed(5));
}

/** The report the command reads from the page's markup; the data blocks leave the markup, which the command prints. */
function stillReport(report) {
  for (const el of document.querySelectorAll('script[type="application/octet-stream"]')) el.remove();
  const s = document.createElement('script');
  s.type = 'application/json'; s.id = 'nv-still-report';
  s.textContent = JSON.stringify(report).replace(/</g, '\\u003c');
  document.body.appendChild(s);
}

/** A still that cannot be drawn says so in the picture and in the report, never with an empty page. */
function stillFail(message) {
  const box = document.createElement('div');
  box.className = 'still-error';
  box.textContent = 'This still could not be drawn: ' + message;
  document.body.appendChild(box);
  stillReport({ ok: false, error: message });
}
