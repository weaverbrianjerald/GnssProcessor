"""Real-time dashboard: serve a live, in-place-updating report over HTTP.

Point it at the log folder of a *running* GnssProcessor (post-processing writing
growing CSV traces, or the real-time engine streaming).  The page polls a cheap
``/status`` token; when it changes it fetches ``/update`` and patches each figure
*in place* (Plotly ``react`` / image swap), with no full-page reload, so zoom/pan
survive and there is no flicker.

Two efficiency layers keep it cheap even with large traces:
  * **per-viewer change detection**: a viewer is only re-built and re-rendered
    when one of *its* trace CSVs changes, so a 20 MB ``mwwl_trace.csv`` update
    never re-renders the unrelated state / DOP panels;
  * **token-gated polling**: nothing is rendered while the data is idle.

Backend-agnostic: the same viewers and :class:`Figure` specs drive the live and
the offline reports; the plotly backend (default here) makes it interactive,
while the matplotlib backend degrades to live image swaps.  Stdlib-only HTTP.
"""

from __future__ import annotations

import html
import json
import os
import threading
from functools import partial
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import List, Optional, Sequence

from .backends.base import Backend
from .report import _CSS
from .trace_paths import TraceContext
from .viewers import REGISTRY


class LiveDashboard:
    """Renders the report on demand, re-rendering only the viewers that changed."""

    def __init__(self, ctx: TraceContext, backend: Optional[Backend], *,
                 only: Optional[Sequence[str]] = None,
                 title: str = "GnssProcessor live dashboard",
                 poll_interval: float = 5.0, dpi: int = 110):
        self.ctx = ctx
        self.backend = backend
        self.only = set(only) if only else None
        self.title = title
        self.poll_interval = poll_interval
        self.dpi = dpi
        self._lock = threading.Lock()
        self._cache = {}    # viewer name -> (viewer_token, section dict)

    # ------------------------------------------------------------------ #
    def data_token(self) -> str:
        """Change token over the CSVs the rendered viewers consume, not every CSV.

        Scoping to the active (``only``-filtered) viewers' ``required_csvs``,
        rather than every CSV in the folder, means an idle or excluded trace
        (e.g. a 20 MB ``mwwl_trace.csv`` still being written but not rendered)
        does not advance the token and trigger needless ``/update`` fetches.
        """
        newest = 0.0
        for viewer in REGISTRY:
            if self.only is not None and viewer.name not in self.only:
                continue
            for c in viewer.required_csvs:
                try:
                    newest = max(newest, os.path.getmtime(self.ctx.path(c)))
                except OSError:
                    pass
        return f"{newest:.3f}"

    def _viewer_token(self, viewer) -> str:
        newest = 0.0
        for c in viewer.required_csvs:
            try:
                newest = max(newest, os.path.getmtime(self.ctx.path(c)))
            except OSError:
                pass
        return f"{newest:.3f}"

    def _build(self) -> List[dict]:
        """Ordered section dicts; re-renders only viewers whose CSVs changed."""
        sections: List[dict] = []
        for viewer in REGISTRY:
            if self.only is not None and viewer.name not in self.only:
                continue
            if not all(self.ctx.has_data(c) for c in viewer.required_csvs):
                self._cache.pop(viewer.name, None)
                continue
            tok = self._viewer_token(viewer)
            cached = self._cache.get(viewer.name)
            if cached and cached[0] == tok:
                sections.append(cached[1])
                continue
            try:
                res = viewer.build(self.ctx)
            except Exception:
                self._cache.pop(viewer.name, None)
                continue
            figs = []
            for i, fig in enumerate(res.figures):
                rendered = self.backend.render(fig)
                try:
                    payload = self.backend.live_payload(rendered, dpi=self.dpi)
                finally:
                    self.backend.close(rendered)
                figs.append({"id": f"{viewer.name}__{i}", "payload": payload})
            section = {"viewer": viewer.name, "title": viewer.title,
                       "summary": res.summary, "figs": figs}
            self._cache[viewer.name] = (tok, section)
            sections.append(section)
        return sections

    # ------------------------------------------------------------------ #
    def live_page(self) -> str:
        with self._lock:
            sections = self._build()
            token = self.data_token()
        return _render_page(sections, token, self.backend, self.title, self.poll_interval)

    def update_payload(self) -> dict:
        with self._lock:
            sections = self._build()
            token = self.data_token()
        figs = [f for s in sections for f in s["figs"]]
        return {
            "token": token,
            "ids": [f["id"] for f in figs],
            "figs": figs,
            # Every rendered section, including empty ones, so the client can
            # clear a summary that went away, not just set ones that are present.
            "summaries": {s["viewer"]: (s["summary"] or "") for s in sections},
        }

    # ------------------------------------------------------------------ #
    def serve(self, host: str = "127.0.0.1", port: int = 8765) -> None:
        httpd = ThreadingHTTPServer((host, port), partial(_Handler, self))
        print(f"gnssviz live dashboard: http://{host}:{port}/  (Ctrl-C to stop)")
        print(f"  watching {self.ctx.log_dir}  (poll {self.poll_interval:g}s, "
              f"backend={self.backend.name})")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopping live dashboard")
        finally:
            httpd.server_close()


# --------------------------------------------------------------------------- #
def _render_page(sections, token, backend, title, poll_interval) -> str:
    head_js = ('<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>'
               if getattr(backend, "needs_js", False) else "")
    parts: List[str] = [
        "<!DOCTYPE html><html><head><meta charset='utf-8'>",
        f"<title>{html.escape(title)}</title>{head_js}<style>{_CSS}</style></head><body>",
        f"<h1>{html.escape(title)}</h1>",
        f"<div class='meta'>Live &#9679; updates in place every {poll_interval:g}s "
        f"&nbsp;|&nbsp; <span id='lasttick'></span></div>",
        "<div class='toc'><b>Sections</b><ul>",
    ]
    for i, s in enumerate(sections):
        parts.append(f"<li><a href='#sec{i}'>{html.escape(s['title'])}</a></li>")
    parts.append("</ul></div>")

    init_figs = []
    for i, s in enumerate(sections):
        parts.append(f"<h2 id='sec{i}'>{html.escape(s['title'])}</h2>")
        # Always emit the <pre> (hidden when empty) so live updates can populate
        # or clear it without a structural reload.
        disp = "" if s["summary"] else " style='display:none'"
        parts.append(f"<pre id='summary_{html.escape(s['viewer'])}'{disp}>"
                     f"{html.escape(s['summary'] or '')}</pre>")
        for f in s["figs"]:
            parts.append(f"<div class='fig' id='{f['id']}'></div>")
            init_figs.append(f)

    parts.append("<script>\n" + _LIVE_JS
                 .replace("__FIGS__", json.dumps(init_figs))
                 .replace("__TOKEN__", json.dumps(token))
                 .replace("__POLL__", str(int(poll_interval * 1000)))
                 + "\n</script></body></html>")
    return "".join(parts)


# Client: render initial figures, then poll /status and patch /update in place.
_LIVE_JS = r"""
const FIGS = __FIGS__;
let TOKEN = __TOKEN__;
const POLL = __POLL__;
function paint(id, p) {
  const el = document.getElementById(id);
  if (!el) return false;
  if (p.kind === 'plotly') {
    Plotly.react(el, p.data, p.layout, {responsive: true});
  } else {
    el.innerHTML = '<img src="' + p.src + '">';
  }
  return true;
}
FIGS.forEach(f => paint(f.id, f.payload));
function stamp() {
  const e = document.getElementById('lasttick');
  if (e) e.textContent = 'updated ' + new Date().toLocaleTimeString();
}
stamp();
async function poll() {
  try {
    const s = await (await fetch('status', {cache: 'no-store'})).json();
    if (!s.token || s.token === TOKEN) return;
    const u = await (await fetch('update', {cache: 'no-store'})).json();
    const have = new Set(FIGS.map(f => f.id));
    if (u.ids.length !== FIGS.length || !u.ids.every(id => have.has(id))) {
      location.reload();            // structure changed (viewer/figure added or removed)
      return;
    }
    u.figs.forEach(f => paint(f.id, f.payload));
    if (u.summaries) {
      for (const v in u.summaries) {
        const el = document.getElementById('summary_' + v);
        if (el) { el.textContent = u.summaries[v]; el.style.display = u.summaries[v] ? '' : 'none'; }
      }
    }
    TOKEN = u.token;
    stamp();
  } catch (e) { /* keep polling */ }
}
setInterval(poll, POLL);
"""


# --------------------------------------------------------------------------- #
class _Handler(BaseHTTPRequestHandler):
    def __init__(self, dashboard: LiveDashboard, *args, **kwargs):
        self.dashboard = dashboard
        super().__init__(*args, **kwargs)

    def log_message(self, *_args):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0].rstrip("/") or "/"
        if path == "/status":
            self._send(200, "application/json",
                       json.dumps({"token": self.dashboard.data_token()}).encode())
        elif path == "/update":
            try:
                body = json.dumps(self.dashboard.update_payload()).encode("utf-8")
            except Exception as exc:
                self._send(500, "application/json",
                           json.dumps({"error": str(exc)}).encode())
                return
            self._send(200, "application/json", body)
        elif path == "/":
            try:
                body = self.dashboard.live_page().encode("utf-8")
            except Exception as exc:
                body = (f"<html><body><h1>render error</h1><pre>{html.escape(str(exc))}"
                        "</pre><script>setTimeout(()=>location.reload(),5000)</script>"
                        "</body></html>").encode()
                self._send(500, "text/html", body)
                return
            self._send(200, "text/html", body)
        else:
            self._send(404, "text/plain", b"not found")

    def _send(self, code: int, ctype: str, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)
