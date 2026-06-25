"""Self-contained HTML report assembly.

Renders each viewer's :class:`Figure` specs through a backend to PNG bytes,
base64-embeds them, and writes a single standalone ``.html`` file -- the offline
"summary report after each run" deliverable.  No templating dependency: the HTML
is built with plain string formatting so the suite stays installable with just
numpy/pandas/matplotlib.
"""

from __future__ import annotations

import datetime as _dt
import html
from typing import List, Optional, Sequence, Tuple

from .backends.base import Backend
from .viewers.base import ViewerResult

_CSS = """
:root { color-scheme: light dark; }
body { font-family: -apple-system, Segoe UI, Roboto, sans-serif; margin: 0 auto;
       max-width: 1500px; padding: 24px; background: #fafafa; color: #1a1a1a; }
h1 { font-size: 1.6rem; border-bottom: 2px solid #ccc; padding-bottom: 8px; }
h2 { font-size: 1.2rem; margin-top: 2.2rem; color: #1b3a5c;
     border-left: 4px solid #1b3a5c; padding-left: 10px; }
.meta { color: #555; font-size: 0.85rem; margin-bottom: 1.5rem; }
.meta code { background: #eee; padding: 1px 5px; border-radius: 3px; }
pre { background: #f0f0f0; border: 1px solid #ddd; border-radius: 4px;
      padding: 10px 14px; font-size: 0.82rem; overflow-x: auto; }
.fig { margin: 14px 0; text-align: center; }
.fig img { max-width: 100%; height: auto; border: 1px solid #ddd;
           background: white; border-radius: 4px; }
.toc { background: #fff; border: 1px solid #ddd; border-radius: 6px; padding: 10px 18px; }
.toc a { color: #1b3a5c; text-decoration: none; }
.skip { color: #999; font-style: italic; }
"""


def build_html_report(
    sections: Sequence[Tuple[str, ViewerResult]],
    backend: Backend,
    *,
    log_dir: str = "",
    title: str = "GnssProcessor run report",
    skipped: Optional[Sequence[Tuple[str, str]]] = None,
    dpi: int = 110,
    live_token: Optional[str] = None,
    poll_interval: float = 5.0,
) -> str:
    """Assemble the report HTML from ``(section_title, ViewerResult)`` pairs.

    When ``live_token`` is set, a small poller is embedded that re-fetches
    ``/status`` every ``poll_interval`` seconds and reloads the page when the
    token changes -- the basis of the real-time dashboard (no reload while the
    data is idle, one reload per new-data tick).
    """
    now = _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    # Interactive backends (plotly) need their JS runtime loaded once per doc.
    head_js = ('<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>'
               if getattr(backend, "needs_js", False) else "")
    poller = ""
    if live_token is not None:
        poller = (
            "<script>const _T=%r;setInterval(async()=>{try{"
            "const r=await fetch('status',{cache:'no-store'});const j=await r.json();"
            "if(j.token && j.token!==_T) location.reload();}catch(e){}},%d);</script>"
            % (live_token, int(poll_interval * 1000)))
    live_badge = (f" &nbsp;|&nbsp; <span style='color:#2a7'>&#9679; live "
                  f"(every {poll_interval:g}s)</span>" if live_token is not None else "")
    parts: List[str] = [
        "<!DOCTYPE html><html><head><meta charset='utf-8'>",
        f"<title>{html.escape(title)}</title>{head_js}{poller}<style>{_CSS}</style></head><body>",
        f"<h1>{html.escape(title)}</h1>",
        f"<div class='meta'>Trace folder: <code>{html.escape(log_dir)}</code>"
        f" &nbsp;|&nbsp; generated {now}{live_badge}</div>",
    ]

    # Table of contents.
    parts.append("<div class='toc'><b>Sections</b><ul>")
    for i, (sec, _res) in enumerate(sections):
        parts.append(f"<li><a href='#sec{i}'>{html.escape(sec)}</a></li>")
    parts.append("</ul></div>")

    for i, (sec, res) in enumerate(sections):
        parts.append(f"<h2 id='sec{i}'>{html.escape(sec)}</h2>")
        if res.summary:
            parts.append(f"<pre>{html.escape(res.summary)}</pre>")
        for fig in res.figures:
            rendered = backend.render(fig)
            try:
                parts.append(backend.to_html(rendered, dpi=dpi))
            finally:
                backend.close(rendered)
        if not res.figures and not res.summary:
            parts.append("<p class='skip'>(no figures produced)</p>")

    if skipped:
        parts.append("<h2>Skipped viewers</h2><ul>")
        for name, reason in skipped:
            parts.append(f"<li class='skip'>{html.escape(name)} &mdash; "
                         f"{html.escape(reason)}</li>")
        parts.append("</ul>")

    parts.append("</body></html>")
    return "".join(parts)


def write_html_report(path: str, *args, **kwargs) -> str:
    """Build the report and write it to ``path``; returns ``path``."""
    htmltext = build_html_report(*args, **kwargs)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(htmltext)
    return path
