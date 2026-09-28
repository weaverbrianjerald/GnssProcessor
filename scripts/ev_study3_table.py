#!/usr/bin/env python3
"""Event harness study 3: the band-scoped response summary table (§Z).

Same reduction as ``ev_study1_table.py`` (which it reuses wholesale), over the
Study-3 matrix: every A/B pair differs only in

    network.raim_erase_band_scope        all -> min_keep_one
    network.cycle_slip.refloat_band_scope all -> implicated

so a row pair reads as the production knobs' effect on one event.  The OFF arms
are the §6/§9 legs already on disk; nothing is re-run at settings that exist.

    python scripts/ev_study3_table.py              # the A/B table
    python scripts/ev_study3_table.py --bands      # + the per-band breakdown

``--bands`` re-grades the all-band-slip pair once per band (``event_grade.py
--signal``), which is where the mechanism is visible: the kept band's level
should behave like a band that was never freed.
"""
import argparse
import json
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)

from ev_study1_table import grade, iono_series, qc_reflags, f, EPOCH_S  # noqa: E402

# (label, OFF legdir, ON legdir, station, sat)
PAIRS = [
    ("arc1 T4 erase  14.2deg", "evleg_erase",    "evleg_z_a1erase_on", "S21000SIM", "G10"),
    ("arc2 T4 erase  81.3deg", "evleg_s2_erase", "evleg_z_a2erase_on", "S50000SIM", "G31"),
    ("arc3 T4 erase  13.8deg", "evleg_s3_erase", "evleg_z_a3erase_on", "S40000SIM", "G31"),
    ("arc1 slip x3 (all band)", "evleg_z_slip3_off", "evleg_z_slip3_on", "S21000SIM", "G10"),
    ("arc1 slip x1 (control) ", "evleg_slip",     "evleg_z_slip1_on",  "S21000SIM", "G10"),
]

BAND_PAIR = ("evleg_z_slip3_off", "evleg_z_slip3_on")
BANDS = ("L1C", "L2W", "L5Q")


def row(legdir, null, scratch, sta, sat, signal=None):
    tag = (os.path.basename(legdir) + "__vs_" + os.path.basename(null)
           + (("_" + signal) if signal else ""))
    out = os.path.join(scratch, tag + ".json")
    cmd = [sys.executable, os.path.join(_HERE, "event_grade.py"), legdir,
           "--null", null, "--fixture", "C:/GnssProcessor/runs/simnet60_quiet_wu93_24h",
           "--pre", "10", "--post", "50", "--json", out]
    if signal:
        cmd += ["--signal", signal]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        print(r.stdout.decode("utf-8", "replace"))
        raise SystemExit(f"grade failed for {legdir}")
    with open(out, encoding="utf-8") as fh:
        res = json.load(fh)
    d = {int(k): v for k, v in (res.get("lvl_diff") or {}).items() if v is not None}
    zw = {int(k): v for k, v in (res.get("zwd_diff") or {}).items() if v is not None}
    cn = {int(k): v for k, v in (res.get("cons_diff") or {}).items() if v is not None}
    pk_k = max(d, key=lambda k: abs(d[k])) if d else None
    floor = [abs(d[k]) for k in d if 14 <= k <= 45]
    zpk_k = max(zw, key=lambda k: abs(zw[k])) if zw else None
    cpk_k = max(cn, key=lambda k: abs(cn[k])) if cn else None
    t0 = res.get("t0")
    ie, inl = iono_series(legdir, sta, sat), iono_series(null, sta, sat)
    di = None
    if t0 is not None:
        t45 = t0 + 45 * EPOCH_S
        for t in ie:
            if abs(t - t45) < 0.5 and t in inl:
                di = ie[t] - inl[t]
    nq, tiers = qc_reflags(res)
    return dict(peak=(d.get(pk_k) if pk_k is not None else None), peak_ep=pk_k,
                e14=d.get(14), e30=d.get(30), e45=d.get(45),
                floor=(sum(floor) / len(floor) if floor else None),
                zwd_pk=(zw.get(zpk_k) if zpk_k is not None else None),
                iono45=di, reflags=nq, tiers=tiers,
                cons_pk=(cn.get(cpk_k) if cpk_k is not None else None))


def line(label, r):
    return (f"{label:24s} {f(abs(r['peak']) if r['peak'] is not None else None):>8s} "
            f"{(f'{r['peak_ep']:+d}' if r['peak_ep'] is not None else '-'):>4s} "
            f"{f(abs(r['e14']) if r['e14'] is not None else None):>7s} "
            f"{f(abs(r['e45']) if r['e45'] is not None else None):>7s} "
            f"{f(r['floor']):>7s} "
            f"{f((r['zwd_pk'] or 0) * 1000.0, 1):>9s} "
            f"{f(r['iono45'], 4):>9s} {r['reflags']:>6d} "
            f"{f(r['cons_pk']):>8s}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--null", default="runs/evleg_null",
                    help="the OFF arms' control leg (knobs off, no event)")
    # The ON arms need their own null: the detector scope is not inert on the
    # quiet bed, it acted on 20 arcs in 61 epochs of the null-ON leg, because MW
    # fires on two pairs of the same satellite in one epoch and their union covers
    # the arc.  Grading an ON event leg against the OFF null therefore measures the
    # event plus that background (it showed up as a pre-event |e| peak at ep −1 and
    # an identical +0.021 m consumer offset on every ON row).  Each arm is graded
    # against the null of its own posture, so each cancels its own background.
    ap.add_argument("--null-on", default="runs/evleg_z_null_on",
                    help="the ON arms' control leg (knobs on, no event)")
    ap.add_argument("--bands", action="store_true")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    null = args.null if os.path.isabs(args.null) else os.path.join(REPO, args.null)
    null_on = (args.null_on if os.path.isabs(args.null_on)
               else os.path.join(REPO, args.null_on))
    scratch = os.path.join(REPO, "runs", "_study3_json")
    os.makedirs(scratch, exist_ok=True)

    hdr = (f"{'leg':24s} {'peak|e|':>8s} {'@ep':>4s} {'+14':>7s} {'+45':>7s} "
           f"{'floor':>7s} {'ZWDpk_mm':>9s} {'dIono+45':>9s} {'T4/T3':>6s} {'cons_pk':>8s}")
    print(hdr)
    print("-" * len(hdr))
    rows = []
    for label, off, on, sta, sat in PAIRS:
        ro = row(os.path.join(REPO, "runs", off), null, scratch, sta, sat)
        rn = row(os.path.join(REPO, "runs", on), null_on, scratch, sta, sat)
        print(line(label + " OFF", ro))
        print(line(label + " ON ", rn))
        gain = (ro["floor"] / rn["floor"]) if (ro["floor"] and rn["floor"]) else None
        print(f"{'':24s} {'':8s} {'':4s} {'':7s} {'':7s} "
              f"{('x' + f(gain, 2)) if gain else '-':>7s}  <- floor OFF/ON")
        rows.append(dict(label=label, off_leg=off, on_leg=on, off=ro, on=rn, gain=gain))

    if args.bands:
        print()
        off, on = BAND_PAIR
        print(f"per-band floor (+14..+45), all-band injected slip, {off} vs {on}")
        print(f"{'band':6s} {'OFF floor':>10s} {'ON floor':>10s} {'OFF/ON':>8s}")
        bands = []
        for b in BANDS:
            ro = row(os.path.join(REPO, "runs", off), null, scratch,
                     "S21000SIM", "G10", signal=b)
            rn = row(os.path.join(REPO, "runs", on), null_on, scratch,
                     "S21000SIM", "G10", signal=b)
            g = (ro["floor"] / rn["floor"]) if (ro["floor"] and rn["floor"]) else None
            print(f"{b:6s} {f(ro['floor']):>10s} {f(rn['floor']):>10s} "
                  f"{(('x' + f(g, 2)) if g else '-'):>8s}")
            bands.append(dict(band=b, off=ro["floor"], on=rn["floor"], gain=g))
        rows.append(dict(label="per-band", bands=bands))

    out = args.out or os.path.join(scratch, "study3_table.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(rows, fh, indent=2)
    print(f"\njson: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
