#!/usr/bin/env python3
"""Field-gate grader for the RAIM pool re-host card (docs/SPEC_raim_pool_rehost.md).

The card's whole point is that a QC tier read ARMED and screened nothing, so the
**denominators are the deliverable** (SPEC §5, standing invariant #4).  This
grader reads them off a finished run directory and, given two run dirs, answers
the three questions the field gates ask:

  1. **Is the screen live?**  ``sigma0`` in ``logs/network_epoch_status.csv`` is
     the per-epoch post-fit RAIM variance factor written straight out of
     ``NetworkRaim.cpp`` (``sigma0 = max(raim_sigma0_floor, 1.4826*median|r|)``
     when the pool is non-empty, and the bare ``1.0`` initializer when it is
     empty).  On a ``carry_invariant`` lineage without the re-host it is
     **identically 1.0** for every warm epoch -- that constant IS the defect,
     visible on the instrument.  ``0.0`` marks an epoch where the seam never ran
     at all (pre-``carry_warm``).

     ⚠ σ̂₀ is FLOORED at ``raim_sigma0_floor`` (default 1.0), so the value 1.0
     alone cannot distinguish "empty pool" from "clean epoch".  The run-end
     ``pool_cells`` tally is the authority on liveness; this script reports the
     fraction of warm epochs strictly above the floor as the practical signal.

  2. **Does the R1 emission coupling fire?**  σ̂₀ feeds (a) the A4 withhold
     (``raim_withhold_sigma0``) -- read off the epoch ``disposition`` column --
     and (b) ``ura_sigma0_ewma``, which scales the emitted URA one epoch later.
     Both sit OUTSIDE the refloat knob, so a "measure-only" arm is not
     emission-neutral.  URA is read from the ``U`` records of ``products.txt``.

  3. **Did the estimate move?**  The screen alone must not.  ``--records``
     compares the ``C``/``B``/``P`` record streams of two ``products.txt`` files
     byte-for-byte per record kind and reports the first differing epoch, so
     "identical except where a withhold fired" is checkable rather than asserted.

Also tallies T3/T4 event rates from ``logs/network_events.csv`` (the T4
attribution tier is structurally ~0 on this lineage today -- the re-host is meant
to make it live, so the count is reported, never assumed) and the run-end
``Network (RAIM pool re-host)`` telemetry block from ``stdout.txt``.

Usage:
  python scripts/raim_gate_grade.py <run_dir> [--json out.json]
  python scripts/raim_gate_grade.py <leg_dir> --vs <ctl_dir> [--records] [--json out.json]
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Run-end telemetry lines, keyed by the tally they carry.  Matched against
# stdout.txt rather than recomputed: these are the counters invariant #4 names.
_TELEM = [
    ("epochs_pool_cells",
     r"RAIM pool re-host\): (\d+) seam pass\(es\), (\d+) station-epoch\(s\) with a "
     r"live census, (\d+) ambiguity cell\(s\) screened"),
    ("sigma0",
     r"RAIM pool re-host\): .?.?0 over (\d+) live-pool epoch\(s\) .{0,4} mean "
     r"([-\d.]+), max ([-\d.]+)"),
    ("flagged",
     r"RAIM pool re-host\): (\d+) satellite-epoch\(s\) consensus-flagged, (\d+) "
     r"isolated arc-epoch\(s\) attribution-flagged"),
    ("refloats",
     r"RAIM pool re-host\): (\d+) arc\(s\) queued into the deferred funnel "
     r"and (\d+) delivered"),
]


def _pct(vals, q):
    """Percentile by nearest-rank on a sorted copy (no numpy dependency)."""
    if not vals:
        return None
    s = sorted(vals)
    k = min(len(s) - 1, max(0, int(round(q / 100.0 * (len(s) - 1)))))
    return s[k]


def read_stdout(run: Path) -> dict:
    """Pull the run-end re-host telemetry + the loader posture lines."""
    out: dict = {"dead_wiring": False, "zero_opportunity": False,
                 "withhold_warned": False, "posture": None, "inert_warned": False}
    p = run / "stdout.txt"
    if not p.is_file():
        return out
    txt = p.read_text(encoding="utf-8", errors="replace")
    for key, pat in _TELEM:
        m = re.search(pat, txt)
        if not m:
            continue
        g = [float(x) if "." in x else int(x) for x in m.groups()]
        if key == "epochs_pool_cells":
            out["epochs"], out["pool_stations"], out["pool_cells"] = g
        elif key == "sigma0":
            out["sigma0_n"], out["sigma0_mean"], out["sigma0_max"] = g
        elif key == "flagged":
            out["sats_flagged"], out["arcs_flagged"] = g
        elif key == "refloats":
            out["refloats_queued"], out["refloats_drained"] = g
    out["dead_wiring"] = "DEAD WIRING" in txt
    out["zero_opportunity"] = "ZERO OPPORTUNITY" in txt
    out["withhold_warned"] = "can begin WITHHOLDING epochs" in txt
    out["inert_warned"] = "post-fit RAIM is INERT" in txt
    if "RAIM POOL RE-HOST): post-fit RAIM re-hosted" in txt:
        out["posture"] = "rehosted"
    elif "post-fit RAIM is INERT" in txt:
        out["posture"] = "inert"
    elif "Increment 3 / T2" in txt:
        out["posture"] = "legacy_pl_amb"
    return out


def read_epoch_status(run: Path, floor: float = 1.0) -> dict:
    """σ̂₀ distribution + A4 withhold dispositions, per epoch."""
    p = run / "logs" / "network_epoch_status.csv"
    res: dict = {"n_epochs": 0}
    if not p.is_file():
        return res
    s0, disp, withheld_tow, warm = [], Counter(), [], 0
    d5, retap, births, arcref = [], [], [], []
    with p.open(newline="") as fh:
        for row in csv.DictReader(fh):
            res["n_epochs"] += 1
            disp[row["disposition"]] += 1
            v = float(row["sigma0"])
            s0.append(v)
            if v > 0.0:
                warm += 1
            if row["disposition"] != "emitted":
                withheld_tow.append((float(row["tow"]), row["disposition"], v))
            for key, acc in (("n_d5_flags", d5), ("n_d5_retaps", retap),
                             ("n_chord_births", births), ("n_arc_refloat", arcref)):
                if row.get(key) not in (None, ""):
                    acc.append(float(row[key]))
    live = [v for v in s0 if v > floor]          # strictly above the σ̂₀ floor
    seam = [v for v in s0 if v > 0.0]            # seam ran at all
    res.update(
        disposition=dict(disp),
        n_warm_epochs=warm,
        n_withheld=sum(n for d, n in disp.items() if d != "emitted"),
        withheld=withheld_tow[:50],
        sigma0_unique=len(set(seam)),
        sigma0_above_floor=len(live),
        sigma0_p50=_pct(seam, 50), sigma0_p99=_pct(seam, 99),
        sigma0_max=max(seam) if seam else None,
        sigma0_mean=statistics.fmean(seam) if seam else None,
    )
    for name, acc in (("d5_flags", d5), ("d5_retaps", retap),
                      ("chord_births", births), ("arc_refloat", arcref)):
        if acc:
            res[f"{name}_total"] = sum(acc)
            res[f"{name}_per_ep"] = sum(acc) / len(acc)
    return res


def read_events(run: Path) -> dict:
    """T3 / T4 (and every other tier's) event counts from the QC event trace."""
    p = run / "logs" / "network_events.csv"
    if not p.is_file():
        return {}
    tiers, t34 = Counter(), Counter()
    n = 0
    with p.open(newline="") as fh:
        for row in csv.DictReader(fh):
            n += 1
            t = row.get("tier", "")
            tiers[t] += 1
            if t.startswith(("T3", "T4")):
                t34[f"{t}/{row.get('event','')}"] += 1
    return {"n_event_rows": n, "tiers": dict(tiers.most_common(20)),
            "t3_t4": dict(t34.most_common(30)),
            "n_t3": sum(v for k, v in tiers.items() if k.startswith("T3")),
            "n_t4": sum(v for k, v in tiers.items() if k.startswith("T4"))}


def read_dd(run: Path) -> dict:
    """Chord LIFETIME distribution + ``dd_sigma_cyc`` from the DD-fraction trace.

    R4's predicted failure mode for the refloat actuator is CHURN: indiscriminate
    membership exclusion from ``A_old`` pins mean chord lifetime low and inflates
    ``dd_sigma_cyc`` (the D5 v4 record: 4.3 epochs / ~3.9 cyc).  So both are read
    off ``logs/network_dd_fractional.csv`` and compared leg-vs-leg.

    ⚠ A chord is keyed on ``(signal, sat_i, sta_a, sta_b)`` and NOT on ``ref_sat``:
    the trace's reference satellite rotates on its own (SPEC_amb_iono §6.12/§6.13),
    so folding it into the identity would count reference rotations as chord
    deaths and manufacture churn that the estimator never saw.  Lifetime is the
    run length in CONSECUTIVE traced epochs; a chord seen in N separate runs
    contributes N lifetimes.
    """
    p = run / "logs" / "network_dd_fractional.csv"
    if not p.is_file():
        return {}
    epochs: list[str] = []
    seen: dict[tuple, int] = {}          # chord -> index of its last epoch
    runlen: dict[tuple, int] = {}        # chord -> current run length
    lifetimes: list[int] = []
    sig: list[float] = []
    frac: list[float] = []
    ep_index: dict[str, int] = {}
    with p.open(newline="") as fh:
        for row in csv.DictReader(fh):
            tow = row["tow"]
            if tow not in ep_index:
                ep_index[tow] = len(ep_index)
                epochs.append(tow)
            i = ep_index[tow]
            key = (row["signal"], row["sat_i"], row["sta_a"], row["sta_b"])
            if key in seen and seen[key] == i - 1:
                runlen[key] += 1
            else:
                if key in runlen:
                    lifetimes.append(runlen[key])
                runlen[key] = 1
            seen[key] = i
            try:
                sig.append(float(row["dd_sigma_cyc"]))
                frac.append(abs(float(row["frac_cyc"])))
            except (ValueError, KeyError):
                pass
    lifetimes.extend(runlen.values())
    return {
        "n_epochs_traced": len(epochs), "n_rows": len(sig),
        "n_chord_segments": len(lifetimes),
        "lifetime_mean": statistics.fmean(lifetimes) if lifetimes else None,
        "lifetime_p50": _pct(lifetimes, 50), "lifetime_p99": _pct(lifetimes, 99),
        "lifetime_max": max(lifetimes) if lifetimes else None,
        "dd_sigma_cyc_p50": _pct(sig, 50), "dd_sigma_cyc_p99": _pct(sig, 99),
        "dd_sigma_cyc_mean": statistics.fmean(sig) if sig else None,
        "abs_frac_cyc_p50": _pct(frac, 50),
    }


def read_products(path: Path) -> dict:
    """Split products.txt into per-record-kind streams + the U (URA) table.

    The v2 product stream is epoch-blocked: ``E,week,tow,...`` opens an epoch and
    every following record belongs to it until the next ``E``.  ``U,sat,ura_m``
    therefore carries no time of its own, so it is keyed on the OPEN epoch's tow
    -- (tow, sat) -- and two runs can be differenced per satellite per epoch
    without assuming row order.  ``H,week,tow,status,sigma0,n_pub,withheld`` is
    the emission heartbeat (its sigma0 is the prefit-census one under
    ``hsig=pc``, not RAIM's).
    """
    kinds: dict[str, list[str]] = defaultdict(list)
    ura: dict[tuple, float] = {}
    heartbeat: list[tuple] = []
    if not path.is_file():
        return {"kinds": {}, "ura": {}, "heartbeat": []}
    tow = ""
    with path.open(encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            ln = ln.rstrip("\n")
            if not ln or ln.startswith("#"):
                continue
            tok = ln.split(",")
            kind = tok[0].strip()
            kinds[kind].append(ln)
            if kind == "E" and len(tok) >= 3:
                tow = tok[2].strip()
            elif kind == "U" and len(tok) >= 3:
                try:
                    ura[(tow, tok[1].strip())] = float(tok[2])
                except ValueError:
                    pass
            elif kind == "H" and len(tok) >= 7:
                heartbeat.append((tok[2].strip(), tok[3].strip(), tok[4].strip(),
                                  tok[5].strip(), tok[6].strip()))
    return {"kinds": {k: v for k, v in kinds.items()}, "ura": ura,
            "heartbeat": heartbeat}


def compare_records(a: Path, b: Path, kinds=("C", "B", "P")) -> dict:
    """Per-record-kind byte comparison of two products.txt files."""
    pa, pb = read_products(a), read_products(b)
    out = {}
    for k in sorted(set(pa["kinds"]) | set(pb["kinds"])):
        la, lb = pa["kinds"].get(k, []), pb["kinds"].get(k, [])
        same = la == lb
        first = None
        if not same:
            for i, (x, y) in enumerate(zip(la, lb)):
                if x != y:
                    first = {"index": i, "a": x[:200], "b": y[:200]}
                    break
            if first is None:
                first = {"index": min(len(la), len(lb)), "a": "<len>", "b": "<len>"}
        out[k] = {"n_a": len(la), "n_b": len(lb), "identical": same,
                  "first_diff": first, "graded": k in kinds}
    # URA delta per satellite (leg - ctl)
    ua, ub = pa["ura"], pb["ura"]
    common = sorted(set(ua) & set(ub))
    d = [ub[k] - ua[k] for k in common]
    per_sat = defaultdict(list)
    for k in common:
        per_sat[k[1]].append(ub[k] - ua[k])
    out["_ura"] = {
        "n_common": len(common), "n_a": len(ua), "n_b": len(ub),
        "delta_p50": _pct(d, 50), "delta_p99": _pct(d, 99),
        "delta_max": max(d) if d else None, "delta_min": min(d) if d else None,
        "n_nonzero": sum(1 for x in d if x != 0.0),
        "ctl_p50": _pct([ua[k] for k in common], 50),
        "leg_p50": _pct([ub[k] for k in common], 50),
        "per_sat_p50_max": max(((s, _pct(v, 50)) for s, v in per_sat.items()),
                               key=lambda t: abs(t[1] or 0.0), default=None),
        "per_sat_p50": {s: _pct(v, 50) for s, v in sorted(per_sat.items())},
    }
    # Emission heartbeat: the H record's status / withheld columns, per epoch.
    ha = {h[0]: h for h in pa.get("heartbeat", [])}
    hb = {h[0]: h for h in pb.get("heartbeat", [])}
    diff_h = [(t, ha[t], hb[t]) for t in sorted(set(ha) & set(hb)) if ha[t] != hb[t]]
    out["_heartbeat"] = {"n_ctl": len(ha), "n_leg": len(hb),
                         "n_differing": len(diff_h), "first": diff_h[:10],
                         "ctl_withheld": sum(1 for h in ha.values() if h[4] not in ("-", "")),
                         "leg_withheld": sum(1 for h in hb.values() if h[4] not in ("-", ""))}
    return out


def grade(run: Path, dd: bool = False) -> dict:
    res = {"run": str(run), "telemetry": read_stdout(run),
           "epoch_status": read_epoch_status(run), "events": read_events(run)}
    if dd:
        res["dd"] = read_dd(run)
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rundir", type=Path)
    ap.add_argument("--vs", type=Path, default=None,
                    help="control run dir to compare against (leg - ctl)")
    ap.add_argument("--records", action="store_true",
                    help="byte-compare the products.txt C/B/P record streams")
    ap.add_argument("--dd", action="store_true",
                    help="chord lifetime + dd_sigma_cyc from network_dd_fractional.csv (R4 churn)")
    ap.add_argument("--json", type=Path, default=None)
    a = ap.parse_args()

    res = grade(a.rundir, dd=a.dd)
    if a.vs:
        res["ctl"] = grade(a.vs, dd=a.dd)
        if a.records:
            res["records"] = compare_records(a.vs / "products.txt",
                                             a.rundir / "products.txt")

    t = res["telemetry"]
    es = res["epoch_status"]
    print(f"== {a.rundir}")
    print(f"   posture            : {t.get('posture')}"
          f"  (dead_wiring={t.get('dead_wiring')}, "
          f"zero_opportunity={t.get('zero_opportunity')})")
    print(f"   re-host telemetry  : epochs={t.get('epochs')} "
          f"pool_stations={t.get('pool_stations')} pool_cells={t.get('pool_cells')}")
    print(f"   sigma0 (run-end)   : n={t.get('sigma0_n')} mean={t.get('sigma0_mean')} "
          f"max={t.get('sigma0_max')}")
    print(f"   sigma0 (per-epoch) : n_warm={es.get('n_warm_epochs')} "
          f"uniq={es.get('sigma0_unique')} above_floor={es.get('sigma0_above_floor')} "
          f"p50={es.get('sigma0_p50')} p99={es.get('sigma0_p99')} max={es.get('sigma0_max')}")
    print(f"   flagged            : sats={t.get('sats_flagged')} arcs={t.get('arcs_flagged')} "
          f"queued={t.get('refloats_queued')} drained={t.get('refloats_drained')}")
    print(f"   A4 withhold        : {es.get('n_withheld')} withheld of "
          f"{es.get('n_epochs')} -- {es.get('disposition')}")
    for tow, disp, s0 in (es.get("withheld") or [])[:20]:
        print(f"       withheld tow={tow:.0f} disposition={disp} sigma0={s0}")
    ev = res["events"]
    print(f"   events             : rows={ev.get('n_event_rows')} "
          f"T3={ev.get('n_t3')} T4={ev.get('n_t4')}")
    if es.get("d5_flags_total") is not None:
        print(f"   per-epoch rates    : d5_flags={es.get('d5_flags_per_ep'):.3f} "
              f"retaps={es.get('d5_retaps_per_ep'):.3f} "
              f"births={es.get('chord_births_per_ep'):.3f} "
              f"arc_refloat={es.get('arc_refloat_per_ep'):.3f}")
    if res.get("dd"):
        d = res["dd"]
        c = (res.get("ctl") or {}).get("dd") or {}
        def _rel(k):
            if c.get(k) in (None, 0) or d.get(k) is None:
                return ""
            return f"  ({100.0 * (d[k] / c[k] - 1.0):+.1f}% vs ctl)"
        print(f"   dd chords          : rows={d['n_rows']} segments={d['n_chord_segments']} "
              f"epochs={d['n_epochs_traced']}")
        print(f"   chord lifetime     : mean={d['lifetime_mean']:.2f}{_rel('lifetime_mean')} "
              f"p50={d['lifetime_p50']} p99={d['lifetime_p99']} max={d['lifetime_max']}")
        print(f"   dd_sigma_cyc       : p50={d['dd_sigma_cyc_p50']:.4f}{_rel('dd_sigma_cyc_p50')} "
              f"p99={d['dd_sigma_cyc_p99']:.4f} mean={d['dd_sigma_cyc_mean']:.4f}")
        print(f"   |frac_cyc| p50     : {d['abs_frac_cyc_p50']:.4f}{_rel('abs_frac_cyc_p50')}")
    if "records" in res:
        print("   products records   :")
        for k, v in res["records"].items():
            if k == "_ura":
                print(f"       U/URA: n={v['n_common']} nonzero={v['n_nonzero']} "
                      f"ctl_p50={v['ctl_p50']} leg_p50={v['leg_p50']} "
                      f"d_p50={v['delta_p50']} d_p99={v['delta_p99']} "
                      f"d_min={v['delta_min']} d_max={v['delta_max']}")
                continue
            if k == "_heartbeat":
                print(f"       H: n_ctl={v['n_ctl']} n_leg={v['n_leg']} "
                      f"differing={v['n_differing']} "
                      f"withheld ctl={v['ctl_withheld']} leg={v['leg_withheld']}")
                for t, a_, b_ in v["first"]:
                    print(f"           tow={t}  ctl={a_}  leg={b_}")
                continue
            mark = "IDENTICAL" if v["identical"] else "DIFFERS"
            print(f"       {k}: {mark}  n_ctl={v['n_a']} n_leg={v['n_b']}"
                  + (f"  first@{v['first_diff']['index']}" if not v["identical"] else ""))
    if a.json:
        a.json.write_text(json.dumps(res, indent=2, default=str), encoding="utf-8")
        print(f"   wrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
