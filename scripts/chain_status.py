#!/usr/bin/env python3
"""One-screen status for the DOY-129 continuous network run.

    python scripts/chain_status.py [--runroot C:/GnssProcessor/runs/day129_cont]

Reports, per segment: snapshot position, epochs emitted, warm-resume verdict,
throughput, and the rms_clock_m stability series from network_trace.csv.
"""
import argparse
import csv
import glob
import os
import re
import statistics

DAY_ANCHOR_TOW = 259200.0   # 2024-05-08 00:00:00 GPS
END_TOW = 345570.0          # 23:59:30 — the day's last epoch
EPOCH_S = 30.0


def tow_to_hms(tow):
    s = int(round(tow - DAY_ANCHOR_TOW))
    return f"{s // 3600:02d}:{(s % 3600) // 60:02d}:{s % 60:02d}"


def snap_header(path):
    m = n = None
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("M,"):
                    m = line.rstrip("\n").split(",")
                elif line.startswith("N,"):
                    n = line.rstrip("\n").split(",")
                    break
    except OSError:
        return None
    if not m or not n:
        return None
    return {"tow": float(m[2]), "n_emitted": int(n[1]), "n_skipped": int(n[2]),
            "gate_withheld": int(n[3]), "runaway": int(n[5])}


def trace_stats(path):
    """rms_clock_m series: the primary stability signal."""
    if not os.path.exists(path):
        return None
    rows = []
    try:
        with open(path, newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    rows.append((float(r["tow"]), float(r["rms_clock_m"]),
                                 int(r["n_present"]), int(r["gate_ok"]), int(r["solved"])))
                except (ValueError, KeyError, TypeError):
                    continue
    except OSError:
        return None
    if not rows:
        return None
    rms = [r[1] for r in rows]
    # Mask the first 20 epochs: the measured warm-resume transient decays from
    # ~162 m to the floor over ~20 epochs. Stats on the masked tail only.
    tail = rms[20:] if len(rms) > 20 else rms
    return {
        "n": len(rows),
        "first_rms": rms[0],
        "settled_rms": rms[20] if len(rms) > 20 else None,
        "last_rms": rms[-1],
        "tail_mean": statistics.mean(tail) if tail else None,
        "tail_p90": (statistics.quantiles(tail, n=10)[8] if len(tail) >= 10 else None),
        "tail_max": max(tail) if tail else None,
        "n_present_min": min(r[2] for r in rows),
        "n_present_mode": statistics.mode([r[2] for r in rows]),
        "not_ok": sum(1 for r in rows if r[3] == 0 or r[4] == 0),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runroot", default=r"C:/GnssProcessor/runs/day129_cont")
    args = ap.parse_args()

    segs = sorted(glob.glob(os.path.join(args.runroot, "seg*")))
    if not segs:
        print("no segments yet")
        return 0

    print(f"{'seg':<6}{'epochs':>8}{'reached':>11}{'age':>7}{'carriedDD':>11}"
          f"{'rms1':>9}{'rms@20':>9}{'rmsLast':>9}{'tailMean':>10}{'nPres':>7}{'bad':>5}")
    print("-" * 92)

    latest = None
    for seg in segs:
        name = os.path.basename(seg)
        h = snap_header(os.path.join(seg, "snap.txt"))
        log = os.path.join(seg, "logs", "main_log.log")
        age = dd = "-"
        if os.path.exists(log):
            with open(log, "r", encoding="utf-8", errors="replace") as fh:
                head = fh.read(400000)
            ma = re.search(r"snapshot age ([0-9.]+) s", head)
            md = re.search(r"restored invariant DD carry \((\d+) carried DD", head)
            if ma:
                age = ma.group(1)
            if md:
                dd = md.group(1)
        t = trace_stats(os.path.join(seg, "logs", "network_trace.csv"))
        if h:
            latest = h
        def fmt(key, spec):
            if not t or t.get(key) is None:
                return "-"
            return format(t[key], spec)

        n_ep = t["n"] if t else 0
        reached = tow_to_hms(h["tow"]) if h else "-"
        n_pres = str(t["n_present_mode"]) if t else "-"
        bad = t["not_ok"] if t else 0
        print(f"{name:<6}{n_ep:>8}{reached:>11}{age:>7}{dd:>11}"
              f"{fmt('first_rms', '.1f'):>9}"
              f"{fmt('settled_rms', '.2f'):>9}"
              f"{fmt('last_rms', '.2f'):>9}"
              f"{fmt('tail_mean', '.2f'):>10}"
              f"{n_pres:>7}{bad:>5}")

    if latest:
        done = latest["tow"] - DAY_ANCHOR_TOW
        remain = max(0.0, END_TOW - latest["tow"])
        print()
        print(f"day position   {tow_to_hms(latest['tow'])} of 23:59:30"
              f"   ({100.0 * done / 86370.0:.1f}% of day)")
        print(f"epochs left    {int(remain / EPOCH_S)}"
              f"   (~{remain / EPOCH_S * 36.024 / 3600.0:.1f} h at 36.0 s/epoch)")
        print(f"counters       n_emitted={latest['n_emitted']} n_skipped={latest['n_skipped']} "
              f"gate_withheld={latest['gate_withheld']} runaway={latest['runaway']}")
        print(f"status         {'COMPLETE' if latest['tow'] >= END_TOW else 'RUNNING'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
