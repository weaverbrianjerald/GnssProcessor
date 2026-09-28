#!/usr/bin/env python
"""Generate an N-run HTML comparison report for PPP-RTK network legs.

``run_compare.py`` lays run B against run A. A validation card is usually
graded over a *family* of legs, though (the DOY-129 reference day, the
frame-guards leg that re-ran the same window armed, the s2z realization arm),
and reading three pair-wise reports to answer one question is the friction this
driver removes. The **first** directory is the reference; every ratio and
agreement statistic is quoted against it.

Examples
--------
    # label auto-derived from the run dir (".../leg01/logs" -> "leg01")
    python run_compare_n.py runs/day129_ref/logs runs/day129_guards/leg01/logs

    # explicit labels, inline
    python run_compare_n.py runs/day129_guards/seed/logs=reference \\
                            runs/day129_guards/leg01/logs=guarded \\
                            runs/day129_s2z/leg01/logs=s2z -o abc.html

    # the RHS geography read at a known onset, in one command
    python run_compare_n.py A/logs B/logs --only keyed --onset-tow 334020

Comparators (``--only``)
------------------------
``disposition``  N disposition timelines on one union epoch axis + agreement
                 against the reference (which epochs a leg withheld that the
                 reference emitted, listed by time).
``tracks``       sigma0 / sigma0_pf / SH-VTEC a00 overlaid, each leg's withheld
                 epochs shaded in its own colour, vertical lines at the
                 structural frame events (CB excursion, code-datum reseed, pivot
                 handover, clock-basis change, SH-gate engage/release).
``events``       per-tier event counts and per-epoch rates, ratio'd against the
                 reference (>3x or new is called out).
``keyed``        network_pf_keyed.csv STA pools reduced to a robust-z against
                 each key's own pre-onset baseline and joined to the
                 network_stations.csv roster on a per-leg worldmap.
``scorecard``    all of the above, as one text panel.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import sys
from functools import partial

# Make the bundled gnssviz package importable when run as a loose script.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _split_spec(spec: str):
    """``"path"`` or ``"path=label"`` -> ``(path, label|None)``.

    ``=`` is the separator (never ``:``) so a Windows drive letter is safe; only
    the last ``=`` splits, so a path containing one still resolves.
    """
    if "=" in spec:
        path, _, label = spec.rpartition("=")
        if path and label:
            return path, label
    return spec, None


def _customized(args):
    """The comparator registry with the CLI's per-comparator knobs bound in.

    Comparators take only a ``RunSet``; the tunables (onset epoch, baseline
    length, shading) ride in as a bound ``partial`` on a copy of the registry
    entry, leaving the shared :data:`RUN_COMPARATORS` untouched -- the same
    pattern ``run_report.py`` uses for the standardized-residual panels.
    """
    from gnssviz.viewers.network_run_compare import RUN_COMPARATORS

    binds = {
        "tracks": dict(max_event_lines=args.max_event_lines,
                       shade_withheld=not args.no_shading),
        "events": dict(ratio_flag=args.ratio_flag),
        "keyed": dict(onset_tow=args.onset_tow, min_baseline=args.min_baseline,
                      z_clip=args.z_clip),
    }
    out = []
    for c in RUN_COMPARATORS:
        kw = binds.get(c.name)
        out.append(dataclasses.replace(c, build=partial(c.build, **kw)) if kw else c)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+",
                    help="trace folders, reference FIRST; each may carry an "
                         "inline label as PATH=LABEL")
    ap.add_argument("--labels", nargs="*", default=None,
                    help="explicit labels, one per directory (overrides PATH=LABEL)")
    ap.add_argument("-o", "--output", default=None,
                    help="output HTML path (default: <last dir>/compare_n_report.html)")
    ap.add_argument("--only", nargs="*", default=None,
                    help="only run these comparators "
                         "(disposition tracks events keyed scorecard)")
    ap.add_argument("--network-config", default=None,
                    help="network run config JSON — the station-position fallback "
                         "for the keyed worldmap when a leg emitted no "
                         "network_stations.csv roster")
    ap.add_argument("--onset-tow", type=float, default=None,
                    help="grade the keyed worldmap at this time-of-week (default: "
                         "auto — the reference leg's worst cross-STA s0_code epoch)")
    ap.add_argument("--min-baseline", type=int, default=10,
                    help="minimum pre-onset epochs a key needs for a robust-z "
                         "(default: 10)")
    ap.add_argument("--z-clip", type=float, default=8.0,
                    help="keyed-z colour-scale saturation (default: 8; the table "
                         "always prints the unclipped z)")
    ap.add_argument("--max-event-lines", type=int, default=60,
                    help="cap on frame-event vertical lines (default: 60; any "
                         "drop is reported, never silent)")
    ap.add_argument("--ratio-flag", type=float, default=3.0,
                    help="event-rate ratio that earns a call-out (default: 3.0)")
    ap.add_argument("--no-shading", action="store_true",
                    help="disable the per-leg withheld-epoch shading on the tracks")
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

    from gnssviz.backends import get_backend
    from gnssviz.report import write_html_report
    from gnssviz.viewers.network_run_compare import (RunSet, auto_label,
                                                     run_multi_comparators)

    paths, inline = zip(*(_split_spec(s) for s in args.dirs))
    if args.labels:
        if len(args.labels) != len(paths):
            ap.error(f"--labels has {len(args.labels)} entries for {len(paths)} dirs")
        labels = list(args.labels)
    elif any(inline):
        # Mixed: an inline label wins, anything unlabelled falls back to the same
        # run-dir rule the all-auto path uses (never the bare "logs" basename).
        labels = [lab or auto_label(p) for p, lab in zip(paths, inline)]
    else:
        labels = None                       # auto (run-dir basename)

    missing = [p for p in paths if not os.path.isdir(p)]
    if missing:
        ap.error("not a directory: " + ", ".join(missing))

    runs = RunSet.from_dirs(list(paths), labels,
                            network_config=args.network_config)
    print(f"=== gnssviz N-run compare ({len(runs.legs)} legs; "
          f"reference = {runs.reference.label}) ===")
    for leg in runs.legs:
        print(f"    {leg.label:<16} {leg.log_dir}")

    out = run_multi_comparators(runs, only=args.only, registry=_customized(args))
    backend = get_backend(args.backend)

    for name, reason in out.skipped:
        print(f"  --  {name:<14} skipped ({reason})")
    for name, err in out.failed:
        print(f"  XX  {name:<14} FAILED: {err}")
    for comp, res in out.ran:
        print(f"  ->  {comp.name:<14} {len(res.figures)} figure(s)")

    output = args.output or os.path.join(runs.legs[-1].log_dir,
                                         "compare_n_report.html")
    write_html_report(
        output,
        [(c.title, r) for (c, r) in out.ran],
        backend,
        log_dir=" | ".join(f"{leg.label}={leg.log_dir}" for leg in runs.legs),
        title="GnssProcessor N-run report: " + " vs ".join(runs.labels),
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
