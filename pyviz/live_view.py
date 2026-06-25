#!/usr/bin/env python
"""Serve a live, self-updating GnssProcessor dashboard over HTTP.

Run this while a processor run is writing trace CSVs (post-processing or the
real-time engine).  The page reloads only when new data lands.

Examples
--------
    python live_view.py                       # http://127.0.0.1:8765 , plotly
    python live_view.py D:\\run\\logs --port 9000
    python live_view.py --only state dops satellite_status measurement_residuals
    python live_view.py --backend matplotlib  # static PNG dashboard
"""

from __future__ import annotations

import argparse
import os
import sys
import threading
import warnings
import webbrowser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_dir", nargs="?", default=None)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--interval", type=float, default=5.0, help="poll seconds")
    ap.add_argument("--only", nargs="*", default=None,
                    help="viewer subset (default: a curated live set)")
    ap.add_argument("--all", action="store_true", help="render every viewer (heavier)")
    ap.add_argument("--backend", default="plotly", help="plotly (default) | matplotlib")
    ap.add_argument("--no-open", action="store_true", help="don't open a browser")
    args = ap.parse_args(argv)

    warnings.filterwarnings("ignore", message="The 'generic' unit for NumPy timedelta")

    from gnssviz import TraceContext
    from gnssviz.backends import get_backend
    from gnssviz.realtime import LiveDashboard

    # Curated default live set (fast, high-signal; excludes the multi-MB slip
    # traces -- add them with --only or --all). --all / --only override.
    default_live = ["state", "dops", "satellite_status", "measurement_residuals",
                    "ionosphere", "ambiguities"]
    only = None if args.all else (args.only if args.only else default_live)

    ctx = TraceContext(args.log_dir)
    dash = LiveDashboard(ctx, get_backend(args.backend), only=only,
                         poll_interval=args.interval)

    if not args.no_open:
        url = f"http://{args.host}:{args.port}/"
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()

    dash.serve(host=args.host, port=args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
