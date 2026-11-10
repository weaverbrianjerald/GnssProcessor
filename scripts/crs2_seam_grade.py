#!/usr/bin/env python3
"""crs2_seam_grade.py (card CRS2): grade an event-harness leg against its seed at the seam.

The leg resumes the seed's snapshot and processes the epochs the seed also processed, so
the seed's own wire and consumer at the same epochs are the reference (card CRS section 7,
arm A0).  Per leg, this prints:

  wire      at the leg's first emitted epoch: the largest and the median absolute phase-bias
            difference leg minus seed over the (satellite, signal) keys both carry, the keys
            whose hold generation differs, the largest clock difference, and the same phase
            bias numbers four epochs later (a level, not a transient, reads the same twice);
  events    PB_HOLD gen_start rows with action `first` at the first epoch (a cold hold
            first-sights every key there), PB_FRAME resume_seam rows over the leg (the seam
            mark), and the first E record's datum flag;
  consumer  the held-out consumer's 3D error (pos_err_3d_m) at the first epoch, its median
            and its maximum over the leg's epochs, for the leg and for the seed at the same
            epochs, and the median of the per-epoch difference;
  run end   the consumer's hold-ledger line (first_sight / refloats) and the producer's
            CRS2 hold-restore line, both from stdout.txt; the consumer's initializer log
            (logs/state_initializer.log, when the leg enabled it) for `Initialized` lines.

    python scripts/crs2_seam_grade.py --seed <seed dir> --leg <leg dir> [--json out.json]

Card: docs/record/refound/CRS2_resume_hold_and_latches_2026-11-06.md.
"""
import argparse
import csv
import json
import math
import os
import re
import statistics as st
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from scenario_matrix import read_wire  # noqa: E402

LEVEL_CHECK_EPOCHS = 4


def read_e_flags(d):
    """products.txt E records: tow -> (datum_changed, constellation set)."""
    out = {}
    p = d + "/products.txt"
    if not os.path.exists(p):
        return out
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("E,"):
                # E,<week>,<tow>,<n_emitted>,<n_skipped>,<iod>,<datum_changed>,<consts>; the
                # constellation set is empty (and the field may be absent) when nothing changed.
                fs = line.rstrip("\n").split(",")
                if len(fs) >= 7:
                    out[int(round(float(fs[2])))] = (fs[6], fs[7] if len(fs) >= 8 else "")
    return out


def read_cons3d(d):
    """closed_loop_trace.csv: tow -> pos_err_3d_m."""
    out = {}
    p = d + "/logs/closed_loop_trace.csv"
    if not os.path.exists(p):
        return out
    for r in csv.DictReader(open(p, newline="")):
        try:
            out[int(round(float(r["tow"])))] = float(r["pos_err_3d_m"])
        except (KeyError, ValueError):
            pass
    return out


def read_events(d):
    p = d + "/logs/network_events.csv"
    if not os.path.exists(p):
        return []
    return list(csv.DictReader(open(p, newline="")))


def grep_file(path, needle, limit=3):
    if not os.path.exists(path):
        return []
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if needle in line:
                out.append(line.rstrip("\n"))
                if len(out) >= limit:
                    break
    return out


def wire_at(seed_w, leg_w, tow):
    if tow not in seed_w or tow not in leg_w:
        return None
    sp, lp = seed_w[tow]["P"], leg_w[tow]["P"]
    common = sorted(set(sp) & set(lp))
    if not common:
        return None
    d = {k: lp[k][0] - sp[k][0] for k in common}
    absd = [abs(v) for v in d.values()]
    worst = max(common, key=lambda k: abs(d[k]))
    gen_diff = [(k, sp[k][1], lp[k][1]) for k in common if sp[k][1] != lp[k][1]]
    sc, lc = seed_w[tow]["C"], leg_w[tow]["C"]
    cc = sorted(set(sc) & set(lc))
    dclk = max((abs(lc[s] - sc[s]) for s in cc), default=float("nan"))
    return {"tow": tow, "n_keys": len(common), "max_abs_dpb_m": max(absd),
            "median_abs_dpb_m": st.median(absd), "worst_key": "%s %s" % worst,
            "worst_dpb_m": d[worst], "gen_diff": gen_diff, "n_leg_only": len(set(lp) - set(sp)),
            "n_seed_only": len(set(sp) - set(lp)), "n_clk": len(cc), "max_abs_dclk_m": dclk}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--leg", required=True)
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    seed, leg = a.seed.replace("\\", "/").rstrip("/"), a.leg.replace("\\", "/").rstrip("/")

    seed_w, leg_w = read_wire(seed), read_wire(leg)
    tows = sorted(leg_w)
    if not tows:
        raise SystemExit(f"{leg}: no emitted epoch in products.txt")
    t0 = tows[0]
    res = {"leg": os.path.basename(leg), "seed": os.path.basename(seed), "first_tow": t0,
           "n_leg_epochs": len(tows)}
    res["wire_first"] = wire_at(seed_w, leg_w, t0)
    t4 = tows[min(LEVEL_CHECK_EPOCHS, len(tows) - 1)]
    res["wire_plus4"] = wire_at(seed_w, leg_w, t4)
    eflags = read_e_flags(leg)
    res["first_e_datum_changed"] = eflags.get(t0, ("?", "?"))[0]
    res["first_e_consts"] = eflags.get(t0, ("?", "?"))[1]

    ev = read_events(leg)
    res["pb_hold_first_at_t0"] = sum(1 for r in ev if r.get("tier") == "PB_HOLD"
                                    and r.get("event") == "gen_start" and r.get("action") == "first"
                                    and abs(float(r.get("tow", "nan") or "nan") - t0) < 0.5)
    res["pb_hold_gen_start_leg"] = sum(1 for r in ev if r.get("tier") == "PB_HOLD" and r.get("event") == "gen_start")
    res["pb_frame_resume_seam"] = [(r.get("sat"), r.get("statistic"), r.get("detail")) for r in ev
                                   if r.get("tier") == "PB_FRAME" and r.get("event") == "resume_seam"]
    res["pb_hold_smh_bridge_t0"] = sum(1 for r in ev if r.get("tier") == "PB_HOLD" and r.get("event") == "smh_bridge"
                                      and abs(float(r.get("tow", "nan") or "nan") - t0) < 0.5)

    cs, cl = read_cons3d(seed), read_cons3d(leg)
    ct = [t for t in tows if t in cl]
    both = [t for t in ct if t in cs]
    if ct:
        lv = [cl[t] for t in ct]
        res["cons_leg"] = {"first": cl[ct[0]], "median": st.median(lv), "max": max(lv), "n": len(ct)}
    if both:
        sv = [cs[t] for t in both]
        dv = [abs(cl[t] - cs[t]) for t in both]
        res["cons_seed_same_epochs"] = {"first": cs[both[0]], "median": st.median(sv), "max": max(sv), "n": len(both)}
        res["cons_abs_diff_median"] = st.median(dv)
        res["cons_abs_diff_max"] = max(dv)

    so = leg + "/stdout.txt"
    res["consumer_pbh_line"] = grep_file(so, "pb hold, consumer (PBH)", 1)
    res["crs2_restore_line"] = grep_file(so, "CRS2 hold restore", 2)
    res["rt_latch_line"] = grep_file(so, "initializer latch", 1)
    res["rt_resume_line"] = grep_file(so, "WARM RESUME applied", 1)
    res["initializer_lines"] = grep_file(leg + "/logs/state_initializer.log", "Initialized", 10)

    w, w4 = res["wire_first"], res["wire_plus4"]
    print(f"leg {res['leg']} vs seed {res['seed']}: first tow {t0}, {len(tows)} emitted epochs")
    if w:
        print(f"  wire @first   keys {w['n_keys']} (leg-only {w['n_leg_only']}, seed-only {w['n_seed_only']}): "
              f"max|dPB| {w['max_abs_dpb_m']:.4f} m ({w['worst_key']} {w['worst_dpb_m']:+.4f}), "
              f"median {w['median_abs_dpb_m']:.4f} m; gen differs on {len(w['gen_diff'])} key(s); "
              f"max|dClk| {w['max_abs_dclk_m']:.4f} m over {w['n_clk']}")
        if w["gen_diff"][:6]:
            print(f"                gen (seed->leg): {w['gen_diff'][:6]}")
    if w4:
        print(f"  wire @+{LEVEL_CHECK_EPOCHS}      max|dPB| {w4['max_abs_dpb_m']:.4f} m, median {w4['median_abs_dpb_m']:.4f} m")
    print(f"  events        PB_HOLD gen_start first @t0 {res['pb_hold_first_at_t0']}, gen_start over the leg "
          f"{res['pb_hold_gen_start_leg']}, smh_bridge @t0 {res['pb_hold_smh_bridge_t0']}, "
          f"PB_FRAME resume_seam {len(res['pb_frame_resume_seam'])}, first E datum_changed "
          f"{res['first_e_datum_changed']} ({res['first_e_consts']})")
    if "cons_leg" in res:
        c = res["cons_leg"]
        print(f"  consumer leg  first {c['first']:.4f}  median {c['median']:.4f}  max {c['max']:.4f} m over {c['n']}")
    if "cons_seed_same_epochs" in res:
        c = res["cons_seed_same_epochs"]
        print(f"  consumer seed first {c['first']:.4f}  median {c['median']:.4f}  max {c['max']:.4f} m "
              f"(same epochs); |leg-seed| median {res['cons_abs_diff_median']:.4f}, max {res['cons_abs_diff_max']:.4f} m")
    for k in ("rt_resume_line", "rt_latch_line", "crs2_restore_line", "consumer_pbh_line"):
        for ln in res[k]:
            print("  " + re.sub(r"^\[.*?\] \[\w+\] \[\w+\] ", "", ln)[:230])
    if res["initializer_lines"]:
        print(f"  initializer   {len(res['initializer_lines'])} Initialized line(s): " + res["initializer_lines"][0][:160])
    else:
        print("  initializer   no Initialized line (or the logger is off on this leg)")
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
