#!/usr/bin/env python3
"""RF-IGN — read the IGNITION-BORN ZERO-Q LEVEL evidence out of a network run.

Card: docs/refound/RF-IGN_ignition_born_levels_2026-09-15.md

The question this reads for is standing invariant #1: after RF-PD-fix
(``network.station_prior_before_pd``) the RF-0g field golden emits from epoch 1
instead of from epoch 42, so ``carry_warm`` -- which is
``phase_leveling && n_emitted >= network.phase_leveling_warmup_epochs`` and
counts EMITTED epochs, not loop epochs (ApplicationNetwork.cpp:1515) -- opens
70 epochs earlier, and the phase-leveling zero-q level carry is therefore BORN
out of a colder network (the whole harvest sits inside
``if (carry_warm)`` at NetworkRunContext.cpp:4112).

REPORT-ONLY.  Nothing here writes into a run; every number is read off the
trace files the run already wrote.  No plotting.

Sections, matching the card:

  (a) birth-TOW distribution of the carried level / ambiguity records.
      Read from ``network_amb_state.csv`` (``event == birth``) when
      ``network.trace_amb_state`` was armed; otherwise the trace is ABSENT and
      the section falls back to the two proxies that need no extra knob --
      the ``carry_warm`` crossing TOW reconstructed from
      ``network_epoch_status.csv`` dispositions, and the first
      ``network_prior_residual.csv`` ``pl`` row (the first epoch a leveling
      prior actually FOLDED, which is one harvest after the first stash).

  (b) the ``pl`` (phase-leveling level) prior-residual family per HOUR:
      sigma0 and the per-epoch worst |t|, p50/p90, plus the folded row count.

  (c) the per-hour census of every event that could REFRESH a stale level
      (re-level / refloat / re-tap / T4 / anchor handover).

  (d) ``network_run_summary.csv`` plus the run-end leveling / carry counters
      scraped out of ``logs/main_log*.log``.

  (e) ``--window LO HI`` lists the events inside a TOW window and prints the
      consumer's per-5-minute p50, for naming the epoch an error steps.

Usage::

    python scripts/rf_ign_level_read.py runs/_rf0g_field6 runs/_rfpdfix_rf0g_field6
    python scripts/rf_ign_level_read.py runs/_rfpdfix_field6_both --window 277000 280800
    python scripts/rf_ign_level_read.py runs/A runs/B --sections abc
"""

from __future__ import annotations

import argparse
import collections
import csv
import glob
import os
import re
import sys

csv.field_size_limit(1 << 24)

# The ignition window this card is about: TOW 259200 (epoch 1) .. 261630, the
# stretch the OFF golden withheld.  Overridable on the command line.
IGN_LO_DEFAULT = 259200.0
IGN_HI_DEFAULT = 261630.0

# Events that can REFRESH / retire a carried zero-q level.  (tier, event).
REFRESH_EVENTS = [
    ("T4_attrib", "attrib_arc"),
    ("T0_gap", "arc_resume"),
    ("T0_mw_detect", "mw_wl_slip"),
    ("T0_mw_detect", "lli_slip"),
    ("T1_if_slip", "if_slip"),
    ("T1_if_slip", "refsat_reset"),
    ("T0_band_scope", "common_band"),
    ("T2_within_epoch", "we_refloat_arc"),
    ("T2_within_epoch", "we_refloat_sat"),
    ("T2_within_epoch", "we_exclude_station"),
    ("PHASE_ANCHOR", "anchor_handover"),
    ("PHASE_ANCHOR", "anchor_slip_handover"),
    ("RELEVEL", "relevel_free"),
    # NOT a level refresh -- the observability gate's soft-anchor heal on a
    # near-null SHARED phase-bias column.  Carried here because its ON/OFF
    # contrast is the availability half of the same story (it is what the
    # ignition withholds looked like), and it must not be read as a re-tap.
    ("GATE", "phase_heal"),
]

# Run-end tail lines worth quoting verbatim (substring match on the message).
TAIL_PATTERNS = [
    "slip re-level response",
    "leveling-carry retirement census",
    "PHASE_ANCHOR carry reref",
    "rx-CB carry)",
    "slip-QC card E / MW-detect",
    "prior-residual census) 'pl'/RESPONDED: worst",
    "constraint-aware global",
]


# ---------------------------------------------------------------- utilities


def pct(vals, q):
    """Nearest-rank percentile of a list (no numpy dependency)."""
    v = sorted(x for x in vals if x is not None)
    if not v:
        return float("nan")
    k = int(round((q / 100.0) * (len(v) - 1)))
    return v[max(0, min(len(v) - 1, k))]


def fnum(x, default=None):
    try:
        f = float(x)
    except (TypeError, ValueError):
        return default
    return f


def hour_of(tow, tow0):
    return int((tow - tow0) // 3600.0) + 1


def log_path(run, name):
    return os.path.join(run, "logs", name)


def have(run, name):
    return os.path.isfile(log_path(run, name))


def read_rows(run, name):
    with open(log_path(run, name), newline="", encoding="utf-8", errors="replace") as f:
        # network_amb_state.csv leads with a '#' provenance banner.
        first = f.readline()
        if not first.startswith("#"):
            f.seek(0)
        for row in csv.DictReader(f):
            yield row


def main_logs(run):
    """main_log*.log in rotation order, oldest first (.N ... then the live one)."""
    d = os.path.join(run, "logs")
    rot = sorted(
        glob.glob(os.path.join(d, "main_log.*.log")),
        key=lambda p: int(re.search(r"main_log\.(\d+)\.log$", p).group(1)),
        reverse=True,
    )
    live = os.path.join(d, "main_log.log")
    return rot + ([live] if os.path.isfile(live) else [])


# ------------------------------------------------------------------ (a)/(b)


def dispositions(run):
    """[(tow, disposition)] from network_epoch_status.csv, loop order."""
    if not have(run, "network_epoch_status.csv"):
        return []
    return [
        (fnum(r["tow"]), r["disposition"])
        for r in read_rows(run, "network_epoch_status.csv")
    ]


def carry_warm_crossing(disp, pl_warmup):
    """The first TOW at which carry_warm holds.

    carry_warm = phase_leveling && n_emitted >= pl_warmup, and n_emitted
    EXCLUDES the current epoch (ApplicationNetwork.cpp:1508/1515) -- so the
    count is taken over the epochs strictly before each row.
    """
    n = 0
    for tow, d in disp:
        if n >= pl_warmup:
            return tow, n
        if d == "emitted":
            n += 1
    return None, n


def section_a(run, ign_lo, ign_hi, pl_warmup):
    print("  (a) BIRTH of the carried level / ambiguity records")
    disp = dispositions(run)
    if disp:
        tow0 = disp[0][0]
        n_em = sum(1 for _, d in disp if d == "emitted")
        first_em = next((t for t, d in disp if d == "emitted"), None)
        cw, _ = carry_warm_crossing(disp, pl_warmup)
        cw_ep = None if cw is None else int(round((cw - tow0) / 30.0)) + 1
        print(
            f"      epochs {len(disp)}  emitted {n_em}  withheld {len(disp) - n_em}"
            f"   first emitted TOW {first_em}"
        )
        print(
            f"      carry_warm (n_emitted >= pl_warmup={pl_warmup}) first holds at"
            f" TOW {cw}  (loop epoch {cw_ep})  <= first level HARVEST"
        )

    if have(run, "network_amb_state.csv"):
        first_tow = {}
        births = []
        for r in read_rows(run, "network_amb_state.csv"):
            tow = fnum(r.get("tow"))
            key = (r.get("station"), r.get("sat"), r.get("signal"))
            if tow is None:
                continue
            if key not in first_tow:
                first_tow[key] = tow
            if r.get("event") == "birth":
                births.append(tow)
        n_ign = sum(1 for t in births if ign_lo <= t <= ign_hi)
        print(
            f"      network_amb_state.csv: {len(births)} birth row(s);"
            f" first birth TOW {min(births) if births else None}"
        )
        print(
            f"         born in ignition [{ign_lo:.0f},{ign_hi:.0f}]: {n_ign}"
            f"   born after: {len(births) - n_ign}"
        )
        ft = sorted(first_tow.values())
        n_ign_ft = sum(1 for t in ft if ign_lo <= t <= ign_hi)
        print(
            f"         distinct (sta,sat,sig) entries {len(ft)};"
            f" first-seen in ignition {n_ign_ft}, after {len(ft) - n_ign_ft}"
        )
    else:
        print(
            "      network_amb_state.csv ABSENT (network.trace_amb_state unarmed)"
            " -- birth TOWs are NOT recoverable for this arm; use the"
            " carry_warm crossing above and the pl first-fold below."
        )

    if have(run, "network_prior_residual.csv"):
        pl = [
            fnum(r["tow"])
            for r in read_rows(run, "network_prior_residual.csv")
            if r["family"] == "pl"
        ]
        print(
            f"      first 'pl' prior-residual (first leveling prior to FOLD):"
            f" TOW {min(pl) if pl else None}   rows {len(pl)}"
        )


def section_b(run, tow0):
    print("  (b) 'pl' prior-residual family per HOUR  (sigma0, per-epoch worst |t|)")
    if not have(run, "network_prior_residual.csv"):
        print("      network_prior_residual.csv ABSENT")
        return
    per = collections.defaultdict(lambda: {"s": [], "t": [], "n": [], "f": []})
    for r in read_rows(run, "network_prior_residual.csv"):
        if r["family"] != "pl" or r["label"] != "CLEAN":
            continue
        tow = fnum(r["tow"])
        if tow is None:
            continue
        b = per[hour_of(tow, tow0)]
        b["s"].append(fnum(r.get("sigma0")))
        b["t"].append(fnum(r.get("max_abs_t")))
        b["n"].append(fnum(r.get("n_rows")))
        b["f"].append(fnum(r.get("n_gt_4")))
    print("      hour |  n_ep |  rows p50 | sigma0 p50 | sigma0 p90 |  |t| p50 |  |t| p90 | n>4s p50")
    for h in sorted(per):
        b = per[h]
        print(
            f"      {h:4d} | {len(b['s']):5d} | {pct(b['n'], 50):9.0f} |"
            f" {pct(b['s'], 50):10.4f} | {pct(b['s'], 90):10.4f} |"
            f" {pct(b['t'], 50):8.2f} | {pct(b['t'], 90):8.2f} | {pct(b['f'], 50):8.0f}"
        )


# ---------------------------------------------------------------------- (c)


def section_c(run, tow0):
    print("  (c) level-REFRESH events per HOUR (+ the GATE/phase_heal availability row)")
    if not have(run, "network_events.csv"):
        print("      network_events.csv ABSENT")
        return
    per = collections.defaultdict(collections.Counter)
    tot = collections.Counter()
    want = set(REFRESH_EVENTS)
    for r in read_rows(run, "network_events.csv"):
        k = (r["tier"], r["event"])
        if k not in want:
            continue
        tow = fnum(r["tow"])
        if tow is None:
            continue
        per[hour_of(tow, tow0)][k] += 1
        tot[k] += 1
    if not tot:
        print("      none of the refresh events fired")
        return
    keys = [k for k in REFRESH_EVENTS if tot[k]]
    hdr = "      event".ljust(38) + "".join(f"{('h%d' % h):>7}" for h in sorted(per)) + f"{'TOTAL':>8}"
    print(hdr)
    for k in keys:
        lbl = f"      {k[0]}/{k[1]}".ljust(38)
        print(lbl + "".join(f"{per[h][k]:7d}" for h in sorted(per)) + f"{tot[k]:8d}")


# ---------------------------------------------------------------------- (d)


def section_d(run):
    print("  (d) run summary + run-end leveling / carry counters")
    if have(run, "network_run_summary.csv"):
        with open(log_path(run, "network_run_summary.csv"), encoding="utf-8") as f:
            lines = [ln.rstrip("\n") for ln in f if ln.strip()]
        if len(lines) >= 2:
            for k, v in zip(lines[0].split(","), lines[1].split(",")):
                print(f"      {k} = {v}")
    n_rej = 0
    tails = []
    for p in main_logs(run):
        with open(p, encoding="utf-8", errors="replace") as f:
            for ln in f:
                if "addStation rejected" in ln:
                    n_rej += 1
                for pat in TAIL_PATTERNS:
                    if pat in ln:
                        tails.append(ln.rstrip())
                        break
    print(f"      addStation rejected (all rotations) = {n_rej}")
    for t in tails:
        msg = t.split("] ", 3)[-1]
        print(f"      | {msg[:400]}")


# ---------------------------------------------------------------------- (e)


def consumer_bins(run, bin_s, lo=None, hi=None):
    """[(bin_start_tow, n, p50, p90)] of pos_err_3d_m from closed_loop_trace.csv."""
    if not have(run, "closed_loop_trace.csv"):
        return []
    per = collections.defaultdict(list)
    tow0 = None
    for r in read_rows(run, "closed_loop_trace.csv"):
        tow = fnum(r.get("tow"))
        e = fnum(r.get("pos_err_3d_m"))
        if tow is None or e is None:
            continue
        if tow0 is None:
            tow0 = tow
        if lo is not None and not (lo <= tow <= hi):
            continue
        per[int((tow - tow0) // bin_s)].append((tow, e))
    out = []
    for b in sorted(per):
        vals = [e for _, e in per[b]]
        out.append((min(t for t, _ in per[b]), len(vals), pct(vals, 50), pct(vals, 90)))
    return out


def section_e(run, lo, hi):
    print(f"  (e) window TOW [{lo:.0f}, {hi:.0f}]")
    print("      consumer per-5-min p50 / p90 (m):")
    for start, n, p50, p90 in consumer_bins(run, 300.0, lo, hi):
        print(f"        TOW {start:9.0f}  n={n:3d}  p50 {p50:8.3f}  p90 {p90:8.3f}")
    if not have(run, "network_events.csv"):
        return
    print("      events in window (tier/event -> count, and every non-bulk row):")
    cnt = collections.Counter()
    rows = []
    bulk = {("PRIORRES", "prior_residual"), ("MDB", "mdb_exceedance"),
            ("CONGRUENCE", "coordinate_offset"), ("REDUNDANCY", "uncheckable_hot_cell")}
    for r in read_rows(run, "network_events.csv"):
        tow = fnum(r["tow"])
        if tow is None or not (lo <= tow <= hi):
            continue
        k = (r["tier"], r["event"])
        cnt[k] += 1
        if k not in bulk:
            rows.append((tow, k, r.get("sat", ""), r.get("station", ""),
                         r.get("statistic", ""), r.get("action", ""),
                         (r.get("detail", "") or "")[:90]))
    for k, v in sorted(cnt.items(), key=lambda x: -x[1]):
        print(f"        {k[0]}/{k[1]:<24} {v}")
    print("      non-bulk rows:")
    for tow, k, sat, sta, stat, act, det in rows:
        print(f"        {tow:9.0f} {k[0]}/{k[1]:<22} {sat:<4} {sta:<12} "
              f"{stat:<12} {act:<18} {det}")


# --------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--sections", default="abcd",
                    help="which of a,b,c,d to print (default abcd)")
    ap.add_argument("--window", nargs=2, type=float, metavar=("LO", "HI"),
                    help="also run section (e) over this TOW window")
    ap.add_argument("--pl-warmup", type=int, default=6,
                    help="network.phase_leveling_warmup_epochs of the run (default 6)")
    ap.add_argument("--ign", nargs=2, type=float,
                    default=[IGN_LO_DEFAULT, IGN_HI_DEFAULT],
                    metavar=("LO", "HI"))
    ap.add_argument("--bins", type=float, default=0.0,
                    help="if > 0, also print consumer p50 in bins of this many seconds")
    a = ap.parse_args()

    # The run-end tail carries sigma-hat / Delta glyphs; a cp1252 console would
    # raise on them mid-report.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    for run in a.runs:
        if not os.path.isdir(run):
            print(f"!! {run}: not a directory", file=sys.stderr)
            continue
        print(f"=== {run}")
        disp = dispositions(run)
        tow0 = disp[0][0] if disp else 0.0
        if "a" in a.sections:
            section_a(run, a.ign[0], a.ign[1], a.pl_warmup)
        if "b" in a.sections:
            section_b(run, tow0)
        if "c" in a.sections:
            section_c(run, tow0)
        if "d" in a.sections:
            section_d(run)
        if a.bins > 0:
            print(f"  consumer p50/p90 in {a.bins:.0f} s bins:")
            for start, n, p50, p90 in consumer_bins(run, a.bins):
                print(f"      TOW {start:9.0f}  n={n:3d}  p50 {p50:8.3f}  p90 {p90:8.3f}")
        if a.window:
            section_e(run, a.window[0], a.window[1])
        print()


if __name__ == "__main__":
    main()
