#!/usr/bin/env python
"""Generate an A/B HTML comparison report for two GnssProcessor network runs.

A validation card ("floors OFF locks in at epoch 42; floors ON stays fixed") is a
statement about two runs. ``run_report.py`` summarizes one run; this driver lays
run **B** against run **A** over the same epochs and writes a single HTML report
built with the same :func:`gnssviz.report.write_html_report` assembler.

Examples
--------
    python run_compare.py D:\\runA\\logs D:\\runB\\logs
    python run_compare.py runA runB --label-a floorsOFF --label-b floorsON
    python run_compare.py runA runB -o ab.html --only cl epoch
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
    ap.add_argument("dir_a", help="run A (baseline) trace folder")
    ap.add_argument("dir_b", help="run B trace folder")
    ap.add_argument("--label-a", default="A", help="short label for run A (default: A)")
    ap.add_argument("--label-b", default="B", help="short label for run B (default: B)")
    ap.add_argument("-o", "--output", default=None,
                    help="output HTML path (default: <dir_b>/compare_report.html)")
    ap.add_argument("--only", nargs="*", default=None,
                    help="only run these comparator names (cl epoch flags zwd clock scorecard)")
    ap.add_argument("--backend", default="matplotlib")
    ap.add_argument("--dpi", type=int, default=110)
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

    from gnssviz.trace_paths import TraceContext
    from gnssviz.compare import ComparePair, run_comparators
    from gnssviz.backends import get_backend
    from gnssviz.report import write_html_report

    pair = ComparePair(
        a=TraceContext(args.dir_a), b=TraceContext(args.dir_b),
        label_a=args.label_a, label_b=args.label_b)
    print(f"=== gnssviz A/B: {args.label_a}={pair.a.log_dir}  "
          f"{args.label_b}={pair.b.log_dir} ===")

    out = run_comparators(pair, only=args.only)
    backend = get_backend(args.backend)

    for name, reason in out.skipped:
        print(f"  --  {name:<12} skipped ({reason})")
    for name, err in out.failed:
        print(f"  XX  {name:<12} FAILED: {err}")
    for comp, res in out.ran:
        print(f"  ->  {comp.name:<12} {len(res.figures)} figure(s)")

    output = args.output or os.path.join(pair.b.log_dir, "compare_report.html")
    write_html_report(
        output,
        [(c.title, r) for (c, r) in out.ran],
        backend,
        log_dir=f"{args.label_a}={pair.a.log_dir} | {args.label_b}={pair.b.log_dir}",
        title=f"GnssProcessor A/B report: {args.label_a} vs {args.label_b}",
        skipped=out.skipped,
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
