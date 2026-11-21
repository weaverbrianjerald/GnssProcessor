"""Stills: parts of a built page written as pictures by a headless browser (netview.py still).

The page draws the still (page/still.js): one part of one view alone on a white page, at a size given in its address.
This module asks for it.  A copy of the page is made with this checkout's page code around the page's data
(nv_page.repage), so a page built before stills existed can be used and the page itself is never changed.  Each still
is one run of headless Edge on that copy:

    msedge --headless=new --window-size=W,H --force-device-scale-factor=D --virtual-time-budget=B
           --screenshot=<png> --dump-dom  file:///<copy>#/<view>?still=<part>&w=W&h=H&...

W x H is the layout of the part in CSS pixels and D the device pixels per CSS pixel, so the picture is W*D x H*D pixels
and needs no cropping.  The type scale of a still is the magnification: the part is laid out at 1/S of the asked size
and drawn S times larger, so type, lines and marks all grow by S against the picture.  The printed markup
(--dump-dom) carries the page's report (what was drawn, the type sizes, the time axis, or the reason for a failure); a
still without a report of success is a failure and its picture is removed.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

import nv_page

#: Where Edge is installed (the first that exists is used; NETVIEW_BROWSER names another Chromium browser).
BROWSER_PATHS = [
    Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft' / 'Edge' / 'Application' / 'msedge.exe',
    Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'Microsoft' / 'Edge' / 'Application' / 'msedge.exe',
]
REPORT = re.compile(r'<script type="application/json" id="nv-still-report">(.*?)</script>', re.S)


@dataclass
class Still:
    """One still: a part of a view, when, how large, and where it is written."""
    panel: str                                   # '<view path>/<part>': network/links, receiver/GCGO00USA/Wet delay
    out: Path                                    # .png: the picture; .json: the page's report only (the tiles' data)
    t: str | None = None                         # the cursor: an epoch index or a GPS time of day HH:MM
    size: tuple[int, int] = (1200, 600)          # the picture in CSS pixels at type scale 1
    type_scale: float = 1.0                      # magnification of type, lines and marks against the picture
    scale: float = 2.0                           # picture pixels per CSS pixel of size
    choices: dict = field(default_factory=dict)  # further address parameters: l=map, key=0, m=held, ...
    placed_width: float | None = None            # inches the picture is wide where it is placed (type size in points)

    @property
    def is_picture(self) -> bool:
        return self.out.suffix.lower() != '.json'

    def layout(self) -> tuple[int, int, float]:
        """(width, height) the part is laid out in, CSS pixels, and the device pixels per CSS pixel."""
        w, h = (max(1, round(v / self.type_scale)) for v in self.size)
        return w, h, self.type_scale * self.scale

    def address(self, page_uri: str) -> str:
        """The address of the still on a page."""
        path, _, part = self.panel.rpartition('/')
        q = {}
        if self.t is not None:
            q['t'] = self.t
        q['still'] = part
        if self.is_picture:
            q['w'], q['h'] = self.layout()[:2]
        q.update(self.choices)
        return (page_uri + '#/' + '/'.join(urllib.parse.quote(c, safe='') for c in path.split('/') if c) + '?' +
                urllib.parse.urlencode(q, quote_via=urllib.parse.quote))


def find_browser() -> Path | None:
    named = os.environ.get('NETVIEW_BROWSER')
    return next((p for p in ([Path(named)] if named else []) + BROWSER_PATHS if p.is_file()), None)


def browser_args(browser: Path, profile: Path, still: Still, url: str, budget_ms: int) -> list[str]:
    """The command of one still.  The profile folder is the job's own, so stills run side by side."""
    w, h, device = still.layout()
    args = [str(browser), '--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', '--log-level=3',
            f'--user-data-dir={profile}', f'--window-size={w},{h}', f'--force-device-scale-factor={device:g}',
            f'--virtual-time-budget={budget_ms}']
    if still.is_picture:
        args.append(f'--screenshot={still.out}')
    return args + ['--dump-dom', url]


def read_report(markup: str) -> dict | None:
    """The report the page left in its markup, or None when it left none."""
    m = REPORT.search(markup)
    return json.loads(m.group(1)) if m else None


def png_size(path: Path) -> tuple[int, int]:
    with open(path, 'rb') as f:
        head = f.read(24)
    if head[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG file')
    return struct.unpack('>II', head[16:24])


def _shoot(browser: Path, work: Path, page_uri: str, k: int, still: Still, budget_ms: int) -> dict:
    """One still: run the browser, read the report, check the picture.  Returns the report with the file's facts."""
    still.out.parent.mkdir(parents=True, exist_ok=True)
    if still.out.exists():
        still.out.unlink()
    w, h, device = still.layout()
    report = None
    for attempt in (1, 2):                 # no report (the page was not ready, the browser hung): once more, with twice the time
        cmd = browser_args(browser, work / f'profile_{k}_{attempt}', still, still.address(page_uri), attempt * budget_ms)
        try:
            done = subprocess.run(cmd, capture_output=True, timeout=attempt * (60 + 2 * budget_ms / 1000))
        except subprocess.TimeoutExpired:
            continue
        report = read_report(done.stdout.decode('utf-8', 'replace'))
        if report is not None:
            break
    if report is None:
        report = {'ok': False, 'error': 'the page left no report: the browser did not finish, or the page did not open '
                                        'within the time budget (raise --budget)'}
    if report['ok'] and still.is_picture and 'tiles' in report:
        report = {'ok': False, 'error': 'the part "tiles" is data, not a picture: write it to a .json file'}
    if report['ok'] and still.is_picture and not still.out.is_file():
        report = {'ok': False, 'error': 'the browser wrote no picture'}
    report.update(panel=still.panel, file=str(still.out), tries=attempt)
    if not report['ok']:
        still.out.unlink(missing_ok=True)                      # the picture of a failure shows its message: not kept
        return report
    if still.is_picture:
        report.update(pixels=list(png_size(still.out)), layout=[w, h], magnified=device)
        if still.placed_width and report.get('type_px'):
            report['placed_width_in'] = still.placed_width
            report['type_pt'] = [round(px * still.placed_width * 72 / w, 1) for px in report['type_px']]
    else:
        still.out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    return report


def run(page: Path, stills: list[Still], parallel: int = 4, budget_ms: int = 45000, log=print) -> list[dict]:
    """Write the stills of one built page; returns one report per still, in order (report['ok'] False for a failure)."""
    browser = find_browser()
    if browser is None:
        raise RuntimeError('Microsoft Edge was not found (set NETVIEW_BROWSER to a Chromium browser)')
    work = Path(tempfile.mkdtemp(prefix='netview_still_'))
    try:
        copy = work / 'page.html'
        nv_page.repage(page, copy)
        with ThreadPoolExecutor(max_workers=max(1, parallel)) as pool:
            jobs = [pool.submit(_shoot, browser, work, copy.as_uri(), k, s, budget_ms) for k, s in enumerate(stills)]
            reports = []
            for job in jobs:
                reports.append(job.result())
                log(describe(reports[-1]))
        return reports
    finally:
        _remove(work)


def _remove(folder: Path) -> None:
    """Remove the work folder; the browser's helpers may hold its profile for a moment after it exits."""
    for _ in range(20):
        shutil.rmtree(folder, ignore_errors=True)
        if not folder.exists():
            return
        time.sleep(0.5)
    print(f'  note: the work folder {folder} could not be removed')


def describe(r: dict) -> str:
    """One line about a still for the terminal."""
    name = Path(r['file']).name
    if not r['ok']:
        return f'{name}: FAILED: {r["error"]}'
    what = f'{r["panel"]} at {r["time"]}'
    if 'tiles' in r:
        return f'{name}: {what}, {len(r["tiles"])} key figures as data'
    if 'pixels' not in r:
        return f'{name}: {what}, the report only'
    line = (f'{name}: {r["pixels"][0]} x {r["pixels"][1]} px; {what}; laid out {r["layout"][0]} x {r["layout"][1]} px, '
            f'magnified {r["magnified"]:g}')
    if r.get('type_px'):
        lo, hi = r['type_px']
        line += f'; type {lo:g} to {hi:g} px'
        if 'type_pt' in r:
            line += f' = {r["type_pt"][0]:g} to {r["type_pt"][1]:g} pt at {r["placed_width_in"]:g} in wide'
    else:
        line += '; no type in it'
    notes = r.get('notes', []) + (['drawn on the second try (the first left no report)'] if r.get('tries') == 2 else [])
    return line + ''.join(f'\n  note: {n}' for n in notes)


def parse_size(text: str) -> tuple[int, int]:
    m = re.fullmatch(r'(\d+)[xX](\d+)', text.strip())
    if not m:
        raise ValueError(f'size {text!r}: expected WIDTHxHEIGHT in CSS pixels, for example 1930x1000')
    return int(m.group(1)), int(m.group(2))


def load_batch(path: Path, out_dir: Path | None = None) -> tuple[Path | None, list[Still]]:
    """(page or None, stills) of a batch file:
        {"page": "<page.html>", "defaults": {"t": "12:00", "type_scale": 2.5, "scale": 3},
         "stills": [{"file": "links_map.png", "panel": "network/links", "size": [1930, 1000], "placed_width": 19.3,
                     "set": {"l": "map", "key": 0}}, ...]}
    Each still takes the defaults it does not set; files are written to out_dir (default: the batch file's folder)."""
    spec = json.loads(Path(path).read_text(encoding='utf-8'))
    base = Path(out_dir) if out_dir else Path(path).resolve().parent
    known = {'file', 'panel', 't', 'size', 'type_scale', 'scale', 'set', 'placed_width'}
    stills = []
    for k, entry in enumerate(spec.get('stills', [])):
        e = {**spec.get('defaults', {}), **entry}
        if set(e) - known or not {'file', 'panel'} <= set(e):
            raise ValueError(f'{path}: still {k + 1}: needs "file" and "panel"; knows {sorted(known)}; has {sorted(e)}')
        still = Still(panel=e['panel'], out=base / e['file'], t=None if e.get('t') is None else str(e['t']),
                      choices={**spec.get('defaults', {}).get('set', {}), **entry.get('set', {})},
                      placed_width=e.get('placed_width'))
        if 'size' in e:
            still.size = (int(e['size'][0]), int(e['size'][1]))
        still.type_scale = float(e.get('type_scale', still.type_scale))
        still.scale = float(e.get('scale', still.scale))
        stills.append(still)
    return (Path(spec['page']) if spec.get('page') else None), stills
