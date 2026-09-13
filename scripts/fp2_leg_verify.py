#!/usr/bin/env python3
"""FP-2 per-leg PROVENANCE verification — the checks a gate table cannot do.

    python scripts/fp2_leg_verify.py [--runs C:/GnssProcessor/runs] [--prefix fp2]
                                     [--json out.json]

The §20 / §22.7 gates grade NUMBERS.  Before any number is legal to read, each
leg has to be shown to be the leg it claims to be, and each PIN/OFF pair has to
differ in the four knobs and nothing else.  That is this script.  Invariant #4:
these are counters, not the absence of a crash.

Per leg:
  * exit code 0 and the wall time;
  * NO ``COLD START`` anywhere in stdout (a leg that cold-started measures the
    bed, not the event) and the RTB-7 WARM RESUME line's restored counts;
  * 61 emitted epochs in the leg's own window (``emitted epoch`` lines) and the
    run-end ``epochs emitted / skipped / withheld`` triple;
  * the RS-8 canonical-signal restore and RS-9 rx-CB frame restore lines ON
    (the harness arms every RS restore; an INERT one silently changes the seam);
  * the four posture ARMED lines PRESENT on a ``_pin`` leg and ABSENT on an
    ``_off`` leg — the knob is proved by the producer's own voice, never by the
    config alone (a branch exe drops an unknown knob silently);
  * ``leg_meta.json``'s recorded ``extra`` and event.

Per PIN/OFF pair: a flattened ``leg.json`` diff, which must be exactly the four
knobs plus the per-leg output paths (plus the injection block on an event leg,
which is identical in both arms and therefore must NOT appear in the diff).
"""
import argparse
import json
import os
import re
import sys

# The producer's run-end clauses carry U+21D2 / U+03C3 / em-dashes; a cp1252
# console would abort the report mid-table on the first one.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):                        # pragma: no cover
    pass

EVENTS = ("null", "e1", "e2", "e3g24", "e3g25", "e5")
ARMS = ("off", "pin")
PIN_KNOBS = ("common_band_scope", "if_phase_td_pairs",
             "common_band_ref_arc", "if_phase_td_ref_arm_all")

# The producer's OWN log text for each armed knob (substring match).
ARMED_LINES = {
    "common_band_scope":       "Network (common-band scope): ARMED",
    "if_phase_td_pairs":       "Network (IF-TD pairs): ALL",
    "common_band_ref_arc":     "reference-arc clause): ARMED",
    "if_phase_td_ref_arm_all": "IF-TD reference arm on ALL chains ARMED",
}
RS_LINES = {
    "rs8_canonical": "(RS-8 canonical restore): ON",
    "rs9_rxcb_lin":  "(RS-9 rx-CB frame restore): ON",
}
ALLOWED_PATH_KEYS = ("network.product_output_path", "network.state_snapshot_path")


def flat(d, p=""):
    out = {}
    for k, v in d.items():
        kk = f"{p}.{k}" if p else k
        if isinstance(v, dict):
            out.update(flat(v, kk))
        else:
            out[kk] = v
    return out


def read_leg(legdir):
    r = {"dir": legdir, "exists": os.path.isdir(legdir), "problems": []}
    if not r["exists"]:
        r["problems"].append("leg directory missing")
        return r
    ec = os.path.join(legdir, "exitcode.txt")
    if os.path.isfile(ec):
        parts = open(ec, encoding="utf-8").read().split()
        r["rc"] = int(parts[0])
        r["wall_s"] = float(parts[1]) if len(parts) > 1 else None
        if r["rc"] != 0:
            r["problems"].append(f"exit code {r['rc']}")
    else:
        r["problems"].append("no exitcode.txt (leg did not finish)")
        return r

    so = os.path.join(legdir, "stdout.txt")
    txt = open(so, encoding="utf-8", errors="replace").read() if os.path.isfile(so) else ""
    if not txt:
        r["problems"].append("no stdout.txt")
        return r

    r["cold_start"] = "COLD START" in txt
    if r["cold_start"]:
        r["problems"].append("COLD START present")

    m = re.search(r"WARM RESUME from '([^']+)'", txt)
    r["warm_resume"] = bool(m)
    if not m:
        r["problems"].append("no WARM RESUME line")
    m = re.search(r"n_emitted (\d+) \(carry stays warm\)", txt)
    r["resume_n_emitted"] = int(m.group(1)) if m else None

    # The leg's OWN emitted-epoch count.  The "emitted epoch N" log line rides
    # health_interval_s (6 of them in a 61-epoch leg), so it is NOT the counter;
    # the products file's E records and the consumer trace rows are.
    pp = os.path.join(legdir, "products.txt")
    r["product_epochs"] = (len(set(re.findall(r"^E,[^,]*,([0-9.]+)", open(
        pp, encoding="utf-8", errors="replace").read(), re.M)))
        if os.path.isfile(pp) else 0)
    cp = os.path.join(legdir, "logs", "closed_loop_trace.csv")
    r["consumer_rows"] = (sum(1 for _ in open(cp, encoding="utf-8",
                                              errors="replace")) - 1
                          if os.path.isfile(cp) else 0)
    r["emitted_epochs"] = r["product_epochs"]
    for nm, v in (("product epochs", r["product_epochs"]),
                  ("consumer rows", r["consumer_rows"])):
        if v != 61:
            r["problems"].append(f"{v} {nm} (want 61)")

    m = re.search(r"Network producer complete: (\d+) epochs emitted, (\d+) skipped "
                  r"\((\d+) of those withheld[^)]*\) — of (\d+) synced", txt)
    if m:
        tot, skipped, withheld, synced = (int(g) for g in m.groups())
        r["run_end"] = {"emitted_total": tot, "skipped": skipped,
                        "withheld": withheld, "synced_total": synced}
        if withheld:
            r["problems"].append(f"{withheld} withheld epoch(s)")
        if r["resume_n_emitted"] is not None and tot - r["resume_n_emitted"] != 61:
            r["problems"].append(f"run-end emitted delta {tot - r['resume_n_emitted']} (want 61)")
    else:
        r["problems"].append("no 'Network producer complete' run-end line")

    r["armed"] = {k: (v in txt) for k, v in ARMED_LINES.items()}
    r["rs_restores"] = {k: (v in txt) for k, v in RS_LINES.items()}
    for k, ok in r["rs_restores"].items():
        if not ok:
            r["problems"].append(f"{k} restore line ABSENT")

    want_pin = legdir.endswith("_pin")
    for k, present in r["armed"].items():
        if present != want_pin:
            r["problems"].append(
                f"{k} ARMED line {'present on an OFF leg' if present else 'ABSENT on a PIN leg'}")

    mp = os.path.join(legdir, "leg_meta.json")
    if os.path.isfile(mp):
        meta = json.load(open(mp, encoding="utf-8"))
        r["meta_event"] = meta.get("event")
        r["meta_extra"] = meta.get("extra")
        r["meta_seed"] = meta.get("seed_run")
        r["meta_fingerprint"] = meta.get("fingerprint")
        r["meta_snapshot_sha256"] = meta.get("snapshot_sha256")
        extra_net = (meta.get("extra") or {}).get("network", {})
        cs = extra_net.get("cycle_slip", {})
        for k in PIN_KNOBS:
            if want_pin and k not in cs:
                r["problems"].append(f"leg_meta extra does not record {k}")
            if (not want_pin) and k in cs:
                r["problems"].append(f"OFF leg_meta extra records {k}")
    else:
        r["problems"].append("no leg_meta.json")

    # Run-end clauses: the ON-path telemetry (invariant #4 — counters, not the
    # absence of crashes).  Each is the producer's own last word on one knob.
    r["clauses"] = {}
    # ⚠ take the LAST match: the loader prints an ARMED banner with the same
    # §-prefix at startup, and it is not the counter line.
    for tag, pat in (("scope", r"Network \(common-band scope\): verdicts .*"),
                     ("pairs", r"Network \(IF-TD pairs \[[^\]]+\]\): .*"),
                     ("ref_arm", r"Network \(§22\.6 REF-FAN / IF-TD reference arm "
                                 r"per CHAIN\):[^\n]*"),
                     ("mw_admit", r"Network \(MW detect-only admission\): .*")):
        m = re.findall(pat, txt)
        if m:
            r["clauses"][tag] = " ".join(m[-1].split())

    for f in ("products.txt", os.path.join("logs", "closed_loop_trace.csv")):
        p = os.path.join(legdir, f)
        r.setdefault("artifacts", {})[f] = os.path.getsize(p) if os.path.isfile(p) else None
        if not os.path.isfile(p):
            r["problems"].append(f"missing {f}")
    return r


def pair_diff(runs, prefix, key):
    off = os.path.join(runs, f"evleg_{prefix}_{key}_off", "leg.json")
    pin = os.path.join(runs, f"evleg_{prefix}_{key}_pin", "leg.json")
    if not (os.path.isfile(off) and os.path.isfile(pin)):
        return {"err": "missing leg.json"}
    a, b = flat(json.load(open(off))), flat(json.load(open(pin)))
    diffs, unexpected = {}, []
    for k in sorted(set(a) | set(b)):
        va, vb = a.get(k, "<MISSING>"), b.get(k, "<MISSING>")
        if va != vb:
            diffs[k] = [va, vb]
            if k in ALLOWED_PATH_KEYS:
                continue
            if k.startswith("network.cycle_slip.") and k.rsplit(".", 1)[-1] in PIN_KNOBS:
                continue
            unexpected.append(k)
    # the injection block must be byte-equal across the arms (it is the event)
    inj = {k: a[k] for k in a if k.startswith("network.inject")}
    inj_b = {k: b[k] for k in b if k.startswith("network.inject")}
    if inj != inj_b:
        unexpected.append("network.inject.* differs between arms")
    return {"n_diffs": len(diffs), "keys": sorted(diffs), "unexpected": unexpected,
            "inject_keys": sorted(inj)}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default=r"C:\GnssProcessor\runs")
    ap.add_argument("--prefix", default="fp2")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    res = {"runs": a.runs, "prefix": a.prefix, "legs": {}, "pairs": {}}
    print(f"=== FP-2 leg verification ({a.prefix}) ===\n")
    hdr = (f"{'leg':<22}{'rc':>3}{'wall':>8}{'prod':>6}{'cons':>6}{'wthl':>6}"
           f"{'cold':>6}{'RS8/9':>7}  armed(scope/pairs/refarc/refarm)")
    print(hdr)
    print("-" * len(hdr))
    allprob = []
    for key in EVENTS:
        for arm in ARMS:
            name = f"{a.prefix}_{key}_{arm}"
            r = read_leg(os.path.join(a.runs, f"evleg_{name}"))
            res["legs"][name] = r
            armed = r.get("armed", {})
            flags = "/".join("Y" if armed.get(k) else "." for k in PIN_KNOBS)
            rs = "/".join("Y" if r.get("rs_restores", {}).get(k) else "."
                          for k in RS_LINES)
            print(f"{name:<22}{r.get('rc', '?'):>3}{r.get('wall_s') or 0:>8.0f}"
                  f"{r.get('product_epochs', 0):>6}{r.get('consumer_rows', 0):>6}"
                  f"{(r.get('run_end') or {}).get('withheld', '?'):>6}"
                  f"{'YES' if r.get('cold_start') else 'no':>6}{rs:>7}  {flags}")
            for p in r["problems"]:
                allprob.append(f"{name}: {p}")

    print("\n=== PIN/OFF config pair diffs ===")
    for key in EVENTS:
        d = pair_diff(a.runs, a.prefix, key)
        res["pairs"][key] = d
        if "err" in d:
            print(f"  {key:<8} {d['err']}")
            allprob.append(f"{key}: pair diff {d['err']}")
            continue
        mark = "OK " if not d["unexpected"] else "BAD"
        print(f"  {mark} {key:<8} {d['n_diffs']} differing key(s); "
              f"inject keys {len(d['inject_keys'])} (identical in both arms)")
        for k in d["keys"]:
            print(f"          {k}")
        for u in d["unexpected"]:
            allprob.append(f"{key}: UNEXPECTED pair diff {u}")

    print("\n=== RUN-END CLAUSES (the two nulls; the ON-path telemetry) ===")
    for arm in ARMS:
        nm = f"{a.prefix}_null_{arm}"
        print(f"\n  --- {nm} ---")
        for tag, line in (res["legs"].get(nm, {}).get("clauses") or {}).items():
            print(f"    [{tag}] {line}")

    print("\n=== PROBLEMS ===")
    if allprob:
        for p in allprob:
            print("  " + p)
    else:
        print("  none — every leg is warm, 61/61, the postures speak for themselves, "
              "and every pair differs in the four knobs only")
    res["problems"] = allprob
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=1, default=str)
        print(f"\nwrote {a.json}")
    return 1 if allprob else 0


if __name__ == "__main__":
    sys.exit(main())
