#!/usr/bin/env python
"""Generate an offline HTML report (or show figures) for a GnssProcessor run.

Examples
--------
    python run_report.py                         # default logs dir -> gnss_report.html
    python run_report.py D:\\run\\logs -o out.html
    python run_report.py --only state dops       # subset of viewers
    python run_report.py --show                  # interactive windows instead of a report

Exam dashboard (GnssSimulator answer key)
-----------------------------------------
    python run_report.py <run_logs> --truth-dir <truth_dir> -o dashboard.html \\
        --backend plotly --plotly-js inline

``--truth-dir`` binds the simulator's answer-key folder (``truth_*.csv`` +
``scenario_summary.json``); the truth-referenced ``exam_*`` viewers then light up
(and, without it, auto-skip).  A ``verify_report.json`` (auto-detected under the
run's log dir, or ``--verify-report``) adds the coloured PASS/FAIL gate banner.
``--plotly-js inline`` embeds plotly.js so the interactive report opens offline.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import sys
from functools import partial

# Make the bundled gnssviz package importable when run as a loose script.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _resolve_verify_report(log_dir: str, explicit):
    """Resolve the verify_report.json path: explicit flag, else auto-detect."""
    if explicit:
        return explicit if os.path.exists(explicit) else None
    auto = os.path.join(log_dir, "verify_report.json")
    return auto if os.path.exists(auto) else None


def _registry_for(std_residuals: bool):
    """The dispatch registry, optionally with standardized residual panels on.

    The ``measurement_residuals`` viewer binds ``show_standardized=False`` by
    default (raw Code/Phase only).  For an exam report we flip it on so the
    storm-window standardized-residual inflation (~1.4→2.2σ, the Part-D exhibit)
    is visible — done by swapping just that viewer's ``build`` partial, leaving
    the shared REGISTRY untouched.
    """
    from gnssviz.viewers import REGISTRY
    if not std_residuals:
        return REGISTRY
    from gnssviz.viewers.measurement_residuals import build_measurement_residuals
    out = []
    for v in REGISTRY:
        if v.name == "measurement_residuals":
            v = dataclasses.replace(
                v, build=partial(build_measurement_residuals, show_standardized=True))
        out.append(v)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_dir", nargs="?", default=None,
                    help="trace folder (default: first existing GnssProcessor logs dir)")
    ap.add_argument("-o", "--output", default=None,
                    help="output HTML path (default: <log_dir>/gnss_report.html)")
    ap.add_argument("--only", nargs="*", default=None,
                    help="only run these viewer names (e.g. state dops)")
    ap.add_argument("--exclude", nargs="*", default=None,
                    help="drop these viewer names from the report (applied after --only)")
    ap.add_argument("--all", action="store_true",
                    help="also run viewers that are off by default (e.g. residual_strip)")
    ap.add_argument("--backend", default="matplotlib")
    ap.add_argument("--network-config", default=None,
                    help="network run config JSON (station positions/roles for the "
                         "worldmap + iono pierce-point viewers)")
    ap.add_argument("--data-root", default=None,
                    help="data root searched for an IONEX GIM (ionosphere backdrop)")
    ap.add_argument("--truth-dir", default=None,
                    help="GnssSimulator answer-key folder (truth_*.csv + "
                         "scenario_summary.json); lights up the exam_* viewers")
    ap.add_argument("--verify-report", default=None,
                    help="verify_report.json for the PASS/FAIL banner + scorecard "
                         "(default: auto-detect <log_dir>/verify_report.json)")
    ap.add_argument("--plotly-js", default="cdn", choices=("cdn", "inline"),
                    help="plotly.js source for --backend plotly: 'cdn' (default, "
                         "needs network) or 'inline' (embed, fully offline)")
    ap.add_argument("--std-residuals", dest="std_residuals", action="store_true",
                    default=None,
                    help="force the standardized-residual panels on (default: on "
                         "whenever --truth-dir is given)")
    ap.add_argument("--no-std-residuals", dest="std_residuals", action="store_false",
                    help="force the standardized-residual panels off even for an exam run")
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
                       data_root=args.data_root, truth_dir=args.truth_dir)

    # Exam mode: bind the verify report + default the standardized panels on.
    is_exam = bool(args.truth_dir)
    verify_path = _resolve_verify_report(ctx.log_dir, args.verify_report)
    ctx.verify_report = verify_path
    std_residuals = args.std_residuals if args.std_residuals is not None else is_exam

    print(f"=== gnssviz: {ctx.log_dir} ===")
    if is_exam:
        print(f"    truth dir: {ctx.truth_dir}")
        print(f"    verify report: {verify_path or '(none found)'}")
    registry = _registry_for(std_residuals)
    if args.exclude:
        drop = set(args.exclude)
        registry = [v for v in registry if v.name not in drop]
    out = run_viewers(ctx, only=args.only, include_disabled=args.all,
                      registry=registry)
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
        title = ("GnssSimulator exam run report" if is_exam
                 else "GnssProcessor run report")
        # Verifier PASS/FAIL banner + per-check chip row (exam runs with a report).
        extra_html = ""
        if is_exam and verify_path:
            from gnssviz.viewers.exam_common import verify_report, verifier_banner_html
            extra_html = verifier_banner_html(verify_report(ctx), verify_path)
        # The exam dashboard is a polished artifact: suppress the "Skipped viewers"
        # plumbing list (viewers that auto-skip for lack of their inputs are expected,
        # not something the reader should see).
        report_skipped = [] if is_exam else out.skipped
        write_html_report(output,
                          [(v.title, r) for (v, r) in out.ran],
                          backend, log_dir=ctx.log_dir, skipped=report_skipped,
                          dpi=args.dpi, title=title, extra_html=extra_html,
                          plotly_js=args.plotly_js)
        print(f"\nWrote {output}")
        if args.open:
            import webbrowser
            webbrowser.open(f"file://{os.path.abspath(output)}")

    print(f"=== {len(out.ran)} ran, {len(out.skipped)} skipped, "
          f"{len(out.failed)} failed ===")
    return 1 if out.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
