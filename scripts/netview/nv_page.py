"""Writes the page: one HTML file that opens from file:// with no server and no network access.

Each data block is a small binary container, gzip-compressed and base64-encoded into a script tag of its own:
    'NVB1' | u32 header length | header JSON (UTF-8) | padding to 8 | arrays, each aligned to 8 bytes
The page decompresses it with the browser's DecompressionStream and takes typed-array views of the arrays
(little-endian, as numpy writes them on x86), so nothing large is parsed as text.

The page's script, style and markup live beside this file in page/ and are inlined here.  The colour maps are the
viridis (magnitudes) and vik (signed values) anchor tables of pyviz/gnssviz/colormaps.py.
"""
from __future__ import annotations

import base64
import gzip
import importlib.util
import json
import math
import os
import re
import struct
import urllib.parse
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

#: numpy dtype -> name of the JavaScript typed array that views it.
JS_TYPES = {
    np.dtype('int8'): 'Int8Array', np.dtype('uint8'): 'Uint8Array', np.dtype('int16'): 'Int16Array',
    np.dtype('uint16'): 'Uint16Array', np.dtype('int32'): 'Int32Array', np.dtype('uint32'): 'Uint32Array',
    np.dtype('float32'): 'Float32Array', np.dtype('float64'): 'Float64Array',
}


def _clean(v):
    """JSON-safe copy of a header value: NaN and infinities become null, numpy scalars plain numbers."""
    if isinstance(v, dict):
        return {str(k): _clean(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_clean(x) for x in v]
    if isinstance(v, (np.integer,)):
        return int(v)
    if isinstance(v, (float, np.floating)):
        return float(v) if math.isfinite(v) else None
    return v


def pack(header: dict, arrays: dict) -> bytes:
    """The binary container of the block (uncompressed)."""
    entries, blobs = [], bytearray()
    for name, a in arrays.items():
        a = np.ascontiguousarray(a)
        if a.dtype == np.int64:
            a = a.astype(np.int32)
        if a.dtype not in JS_TYPES:
            raise TypeError(f'array {name}: dtype {a.dtype} has no typed-array view')
        blobs += b'\0' * ((-len(blobs)) % 8)
        entries.append({'name': name, 'type': JS_TYPES[a.dtype], 'shape': list(a.shape), 'offset': len(blobs),
                        'length': int(a.size)})
        blobs += a.astype(a.dtype.newbyteorder('<'), copy=False).tobytes()
    head = dict(header)
    head['arrays'] = entries
    hj = json.dumps(_clean(head), separators=(',', ':'), allow_nan=False, ensure_ascii=False).encode('utf-8')
    out = bytearray(b'NVB1' + struct.pack('<I', len(hj)) + hj)
    out += b'\0' * ((-len(out)) % 8)
    return bytes(out + blobs)


#: typed-array name -> numpy dtype (the inverse of JS_TYPES).
NP_TYPES = {v: k for k, v in JS_TYPES.items()}


def unpack(raw: bytes) -> tuple[dict, dict]:
    """(header, {name: numpy array}) of a block container written by pack()."""
    if raw[:4] != b'NVB1':
        raise ValueError('not a network view data block')
    n = struct.unpack('<I', raw[4:8])[0]
    head = json.loads(raw[8:8 + n].decode('utf-8'))
    base = 8 + n
    base += (-base) % 8
    arrays = {}
    for a in head.pop('arrays'):
        dt = NP_TYPES[a['type']].newbyteorder('<')
        start = base + a['offset']
        arrays[a['name']] = np.frombuffer(raw, dt, count=a['length'], offset=start).reshape(a['shape'])
    return head, arrays


def read_page(path: Path) -> dict:
    """Every block of a built page: {name: (header, arrays)} ('core' for the block decoded at open)."""
    html = Path(path).read_text(encoding='utf-8')
    out = {}
    for m in re.finditer(r'<script id="(nv-data|nv-block-[a-z]+)" type="application/octet-stream" data-encoding="(gzip|raw)">'
                         r'([A-Za-z0-9+/=]*)</script>', html):
        raw = base64.b64decode(m.group(3))
        if m.group(2) == 'gzip':
            raw = gzip.decompress(raw)
        name = 'core' if m.group(1) == 'nv-data' else m.group(1)[len('nv-block-'):]
        out[name] = unpack(raw)
    if 'core' not in out:
        raise ValueError(f'{path}: not a page written by netview.py build')
    return out


def colormaps():
    """viridis, vik and turbo anchor tables from pyviz/gnssviz/colormaps.py (loaded by path: the package's __init__ pulls
    in its viewers, which this tool does not need)."""
    p = REPO / 'pyviz' / 'gnssviz' / 'colormaps.py'
    spec = importlib.util.spec_from_file_location('nv_colormaps', p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return {'viridis': np.round(mod._VIRIDIS, 4).tolist(), 'vik': np.round(mod._VIK, 4).tolist(),
            'turbo': np.round(mod._TURBO, 4).tolist()}


def coastline():
    """The vendored Natural Earth 1:110m coastline of pyviz (public domain), rounded to 0.1 degree."""
    p = REPO / 'pyviz' / 'gnssviz' / 'assets' / 'coastline_110m.json'
    if not p.exists():
        return None
    j = json.loads(p.read_text(encoding='utf-8'))
    r = lambda v: None if v is None else round(v, 1)                       # noqa: E731
    return {'lon': [r(v) for v in j['lon']], 'lat': [r(v) for v in j['lat']]}


#: The page's style and script files, joined in this order (a file not yet written is skipped).
CSS_FILES = ['netview.css', 'dashboard.css']
JS_FILES = ['core.js', 'widgets.js', 'eventtext.js', 'network.js', 'overview.js', 'receivers.js', 'satellites.js',
            'los.js', 'slips.js', 'stochastic.js', 'atmosphere.js', 'products.js', 'events.js', 'main.js']
#: The live monitor's page (served by live/server.py): its own script, the dashboard's style and event words.
LIVE_CSS_FILES = ['netview.css', 'dashboard.css', 'live.css']
LIVE_JS_FILES = ['eventtext.js', 'live.js']


def live_page_html(run: str) -> str:
    """The live monitor's page as one HTML text (style, script and colour maps inlined)."""
    page = (HERE / 'page' / 'live.html').read_text(encoding='utf-8')
    assets = json.dumps({'colormaps': colormaps(), 'coastline': coastline()}, separators=(',', ':'))
    fill = {'CSS': _joined(LIVE_CSS_FILES), 'ASSETS': f'window.NV_ASSETS = {assets};', 'JS': _joined(LIVE_JS_FILES),
            'TITLE': _html_escape(f'Live network monitor {run}')}
    return re.sub(r'\{\{(CSS|ASSETS|JS|TITLE)\}\}', lambda m: fill[m.group(1)], page)


def _joined(names: list[str]) -> str:
    parts = []
    for n in names:
        p = HERE / 'page' / n
        if p.exists():
            parts.append(f'/* ---- {n} ---- */\n' + p.read_text(encoding='utf-8'))
    return '\n'.join(parts)


def encode_block(header: dict, arrays: dict, compress: bool = True) -> tuple[str, int]:
    """(base64 text, raw size) of one block."""
    raw = pack(header, arrays)
    data = gzip.compress(raw, compresslevel=6, mtime=0) if compress else raw
    return base64.b64encode(data).decode('ascii'), len(raw)


def write_page(path: Path, blocks: dict, compress: bool = True) -> dict:
    """Write the HTML file from {block name: (header, arrays)} ('core' is decoded at open, the others when a view
    first needs them); returns the sizes (bytes) of each block raw and encoded, and of the page."""
    sizes = {}
    core_b64, raw = encode_block(*blocks['core'], compress=compress)
    sizes['core'] = {'raw': raw, 'encoded': len(core_b64)}
    tags = []
    for name, (h, a) in blocks.items():
        if name == 'core':
            continue
        b64, raw = encode_block(h, a, compress=compress)
        sizes[name] = {'raw': raw, 'encoded': len(b64)}
        tags.append(f'<script id="nv-block-{name}" type="application/octet-stream" '
                    f'data-encoding="{"gzip" if compress else "raw"}">{b64}</script>')
    page = (HERE / 'page' / 'netview.html').read_text(encoding='utf-8')
    assets = json.dumps({'colormaps': colormaps(), 'coastline': coastline()}, separators=(',', ':'))
    header = blocks['core'][0]
    fill = {'CSS': _joined(CSS_FILES), 'ASSETS': f'window.NV_ASSETS = {assets};', 'JS': _joined(JS_FILES),
            'TITLE': _html_escape(f'Network dashboard {header.get("run", "")}'),
            'ENCODING': 'gzip' if compress else 'raw', 'DATA': core_b64, 'BLOCKS': '\n'.join(tags)}
    # one pass over the template only, so text inserted for one token is never scanned for another
    html = re.sub(r'\{\{(CSS|ASSETS|JS|TITLE|ENCODING|DATA|BLOCKS)\}\}', lambda m: fill[m.group(1)], page)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(html, encoding='utf-8', newline='\n')
    sizes['page'] = path.stat().st_size
    return sizes


def _html_escape(s: str) -> str:
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


COMPARE_PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Network view comparison</title>
<style>
  :root { color-scheme: light dark; }
  html, body { margin: 0; height: 100%; background: #fbfbf9; font: 13px system-ui, sans-serif; }
  @media (prefers-color-scheme: dark) { html, body { background: #171716; color: #e9e8e3; } }
  .bar { padding: 4px 12px; color: #6b6b66; }
  .wrap { display: flex; height: calc(100vh - 26px); }
  iframe { flex: 1; min-width: 0; height: 100%; border: 0; border-left: 2px solid #d9d8d2; }
  iframe:first-child { border-left: 0; }
  @media (max-width: 900px) { .wrap { flex-direction: column; height: auto; } iframe { height: 100vh; border-left: 0; } }
</style>
</head>
<body>
<div class="bar">Left: {{A_NAME}} &nbsp;|&nbsp; right: {{B_NAME}}. Time, signals, layout and the selected link or node follow
each other; each side keeps its own tree.</div>
<div class="wrap">
  <iframe id="a" src="{{A_SRC}}" title="{{A_NAME}}"></iframe>
  <iframe id="b" src="{{B_SRC}}" title="{{B_NAME}}"></iframe>
</div>
<script>
  const frames = [document.getElementById('a'), document.getElementById('b')];
  window.addEventListener('message', ev => {
    if (!ev.data || ev.data.nv !== 'sync') return;
    for (const f of frames) if (f.contentWindow !== ev.source) f.contentWindow.postMessage(ev.data, '*');
  });
</script>
</body>
</html>
"""


def write_compare(path: Path, page_a: Path, page_b: Path) -> None:
    """A page that shows two network view pages side by side with their time and selection linked."""
    def src(p: Path) -> str:
        rel = Path(os.path.relpath(p.resolve(), path.resolve().parent)).as_posix()
        return urllib.parse.quote(rel)

    fill = {'A_SRC': src(page_a), 'B_SRC': src(page_b), 'A_NAME': _html_escape(page_a.stem),
            'B_NAME': _html_escape(page_b.stem)}
    html = re.sub(r'\{\{(A_SRC|B_SRC|A_NAME|B_NAME)\}\}', lambda m: fill[m.group(1)], COMPARE_PAGE)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(html, encoding='utf-8', newline='\n')
