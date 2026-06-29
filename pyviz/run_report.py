#!/usr/bin/env python
"""Generate an offline HTML report (or show figures) for a GnssProcessor run.

Examples
--------
    python run_report.py                         # default logs dir -> gnss_report.html
    python run_report.py D:\\run\\logs -o out.html
    python run_report.py --only state dops       # subset of viewers
    python run_report.py --show                  # interactive windows instead of a report
"""

from __future__ import annotations

import argparse
import os
import sys

# Make the bundled gnssviz package importable when run as a loose script.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_dir", nargs="?", default=None,
                    help="trace folder (default: first existing GnssProcessor logs dir)")
    ap.add_argument("-o", "--output", default=None,
                    help="output HTML path (default: <log_dir>/gnss_report.html)")
    ap.add_argument("--only", nargs="*", default=None,
                    help="only run these viewer names (e.g. state dops)")
    ap.add_argument("--all", action="store_true",
                    help="also run viewers that are off by default (e.g. residual_strip)")
    ap.add_argument("--backend", default="matplotlib")
    ap.add_argument("--network-config", default=None,
                    help="network run config JSON (station positions/roles for the "
                         "worldmap + iono pierce-point viewers)")
    ap.add_argument("--data-root", default=None,
                    help="data root searched for an IONEX GIM (ionosphere backdrop)")
    ap.add_argument("--dpi", type=int, default=110)
    ap.add_argument("--show", action="store_true",
                    help="open interactive figure windows instead of writing a report")
    ap.add_argument("--open", action="store_true",
                    help="open the generated report in the default browser")
    args = ap.parse_args(argv)

    # Cosmetic: matplotlib 3.10 + numpy 2.5 emit a DeprecationWarning from
    # matplotlib.dates on every datetime axis; nothing the user can act on.
    import warnings
    warnings.filterwarnings(
        "ignore",
        message="The 'generic' unit for NumPy timedelta is deprecated",
        category=DeprecationWarning)

    if args.show:                       # pick a GUI backend before importing mpl
        os.environ.setdefault("MPLBACKEND", "TkAgg")

    from gnssviz import TraceContext, run_viewers
    from gnssviz.backends import get_backend

    ctx = TraceContext(args.log_dir, network_config=args.network_config,
                       data_root=args.data_root)
    print(f"=== gnssviz: {ctx.log_dir} ===")
    out = run_viewers(ctx, only=args.only, include_disabled=args.all)
    backend = get_backend(args.backend)

    for name, reason in out.skipped:
        print(f"  --  {name:<20} skipped ({reason})")
    for name, err in out.failed:
        print(f"  XX  {name:<20} FAILED: {err}")
    for viewer, res in out.ran:
        print(f"  ->  {viewer.name:<20} {len(res.figures)} figure(s)")

    if args.show:
        import matplotlib.pyplot as plt
        for _viewer, res in out.ran:
            for fig in res.figures:
                backend.render(fig)
        plt.show()
    else:
        from gnssviz.report import write_html_report
        output = args.output or os.path.join(ctx.log_dir, "gnss_report.html")
        write_html_report(output,
                          [(v.title, r) for (v, r) in out.ran],
                          backend, log_dir=ctx.log_dir, skipped=out.skipped,
                          dpi=args.dpi)
        print(f"\nWrote {output}")
        if args.open:
            import webbrowser
            webbrowser.open(f"file://{os.path.abspath(output)}")

    print(f"=== {len(out.ran)} ran, {len(out.skipped)} skipped, "
          f"{len(out.failed)} failed ===")
    return 1 if out.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
