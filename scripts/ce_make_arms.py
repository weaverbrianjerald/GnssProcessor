#!/usr/bin/env python3
"""Card CE arm generator (SPEC_event_harness s24.14): the CHAIN-ERA bisect on the LEVELING vehicle.

    python scripts/ce_make_arms.py [--base config/test_suite/_pv_b.json] [--runs C:/GnssProcessor/runs]
        [--prefix _cm_] [--arms c0,c1,c2,c3,c4,c5,c6,pva2] [--out-dir <dir>] [--force]

s24.11's classified posture diff (`runs/_cm_pvb/posture_diff.md`) split the 52 leaf keys separating the
flattened `_fldf3_pl2` chain from the field golden into VEHICLE 16 / GOLDEN-NEWER 13 / CHAIN-ERA 17 /
RUN-SCOPE 6.  PV-B took the VEHICLE rows and nothing else, and still left (i) 1.79 m / (ii) 13 / (iii)
0.95 m while PV-A's code side reads 0.436 m (s24.13's re-read after the P1-4 code-row fallback landed).
This script bisects the CHAIN-ERA class: each arm is the PV-B base plus ONE group of CHAIN-ERA keys,
**at the value the flattened pl2 chain has** (`config/test_suite/_pv_a.json` is that chain flattened, and
is the literal source for every value below), plus its own two run paths.

DIRECTION (read this before "fixing" c4 / c5): the arms move PV-B TOWARDS PV-A, so every value below is
pl2's, not the golden's.  s24.14's prose parenthesises c4 as "max_degree 15 + layer 2 + mean_vtec_prior
15 +/- 30" and c5 as "frequency_mode: triple" — those are the GOLDEN's values (what PV-B already runs),
so applying them would make both arms exact no-ops.  The governing sentence in both s24.14 and the
worker brief is "values as the flattened pl2 chain has them"; c0 is defined as "PV-A's posture on the
golden's newer pins", which is only true with pl2's values; and the generator's own assertion (a group's
diff = its keys + 2 paths) fails loudly on a no-op.  pl2 is deg-8 SINGLE layer with the retired
32.24 +/- 2 GIM pin, and DUAL frequency.  (`estimation.global_vtec_sh.max_degree` is 8 in BOTH configs —
posture_diff.md's note calls the golden "deg-15", but the flattened leaf is 8 on both sides, so it is not
a differing key and is not in any arm.)

The 17 CHAIN-ERA keys are partitioned c1..c6 with no key in two groups; c0 is their union.
`network.cb_kaula_prior: false` is in c0 only — it is already exonerated as s24.12's b3 — and the three
written-only keys (`apriori_source`, `clock_datum_broadcast_anchored`, `emit_pb_filter_jump_k`, all of
which pl2 writes at the value the loader defaults to anyway) are likewise c0-only.

pva2 is the exception: its base is `config/test_suite/_pv_a.json` itself, unchanged except for the two
run paths — PV-A re-run 2 h on the post-ZP exe so its (i) / (iv) are readable on the same last-hour
window as every other arm.

Each arm is written to `<runs>/<prefix><arm>/arm.json` AND `<runs>/<prefix><arm>/seed.json`
(scripts/cm_grade.py's wire-vs-CODE census reads `seed.json` for the CODE product identity), and the
leaf diff against the arm's base is printed and ASSERTED, so the "group keys + 2 paths" property is
verifiable from the console rather than trusted.

The loader is SILENT on unknown JSON keys (feedback_branch_exe_unknown_knob_silent), so a mistyped key
is a silent no-op; the spellings below are the loader's, taken from the flattened `_pv_a.json` leaves
(note `require_bia_for_signals` lives under `estimation`, not `network`, and `require_atx_coverage`
under `model_corrections`).  The run's `logs/gnss_config.log` effect line is the real check and
cm_grade.py prints it per arm.
"""
import argparse
import json
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The four CODE-OSB leaves of s24.14's c1, as the pl2 chain specifies them.  s24.13 found s24.12's b6
# mis-specified: without external_sat_code_bias + require_bia_for_signals the loader forces W6 back on.
# Neither _pv_a.json nor _pv_b.json writes any .BIA path or product key — the bias file is resolved by
# PathResolver from the internal data root, identically on both sides — so there is nothing extra to copy.
C1 = [("network.external_sat_code_bias", True),
      ("network.estimate_sat_code_bias", False),
      ("model_corrections.enable_code_phase_bias", True),
      ("estimation.require_bia_for_signals", True)]
C2 = [("network.rx_code_bias_datum", True)]
C3 = [("network.apriori_clock_anchor_sigma_m", 0.02)]
C4 = [("estimation.global_vtec_sh.layer2_max_degree", 0),
      ("estimation.global_vtec_sh.mean_vtec_prior_tecu", 32.24),
      ("estimation.global_vtec_sh.mean_vtec_prior_sigma_tecu", 2.0)]
C5 = [("processing.frequency_mode", "dual")]
C6 = [("model_corrections.require_atx_coverage", True),
      ("network.emit_pb_filter_meas_sigma_m", 0.1),
      ("network.mark_refsat_reset_disc", True)]
# c0-only: exonerated (b3) + the three written-only keys, so that c0 is the full 17-key union.
C0_EXTRA = [("network.cb_kaula_prior", False),
            ("network.apriori_source", "sp3"),
            ("network.clock_datum_broadcast_anchored", False),
            ("network.emit_pb_filter_jump_k", 4.0)]

# arm -> (description, [(dotted json path, value), ...], base override or "")
ARMS = {
    "c1": ("CODE OSB, correctly specified: external_sat_code_bias + estimate_sat_code_bias off + "
           "enable_code_phase_bias + require_bia_for_signals (the pl2 EXT-CB recipe; s24.12's b6 was "
           "mis-specified and the loader forced W6 back on)", C1, ""),
    "c2": ("network.rx_code_bias_datum: true — EXT63 pins the receiver code-bias datum", C2, ""),
    "c3": ("network.apriori_clock_anchor_sigma_m: 0.02 — EXT63 anchors the a-priori (SP3) clock at 2 cm; "
           "the golden leaves it free (0.0)", C3, ""),
    "c4": ("the SH representation as pl2: layer2_max_degree 4 -> 0 (single layer) + mean_vtec_prior "
           "15.0 +/- 30.0 -> 32.24 +/- 2.0 (the retired 00:00-UT GIM pin)", C4, ""),
    "c5": ("processing.frequency_mode: triple -> dual — the EXT63 DUAL-frequency lineage against the "
           "FT63 golden's GPS L1/L2/L5 triple", C5, ""),
    "c6": ("the CHAIN-ERA remainder: require_atx_coverage + emit_pb_filter_meas_sigma_m 0.05 -> 0.10 + "
           "mark_refsat_reset_disc", C6, ""),
    "c0": ("the UNION of ALL 17 CHAIN-ERA keys on the PV-B base = PV-A's posture on the golden's newer pins",
           C1 + C2 + C3 + C4 + C5 + C6 + C0_EXTRA, ""),
    "pva2": ("PV-A re-run 2 h on the post-ZP exe (config/test_suite/_pv_a.json unchanged; only the two "
             "run paths move) so its (i) / (iv) read on the same last-hour window",
             [], os.path.join(REPO, "config", "test_suite", "_pv_a.json")),
}


def dset(d, dotted, value):
    """Set a dotted json path, creating intermediate objects; returns the previous value or '<absent>'."""
    parts = dotted.split(".")
    cur = d
    for p in parts[:-1]:
        if p not in cur or not isinstance(cur[p], dict):
            cur[p] = {}
        cur = cur[p]
    prev = cur.get(parts[-1], "<absent>")
    cur[parts[-1]] = value
    return prev


def flat(d, p=""):
    out = {}
    if isinstance(d, dict):
        for k, v in d.items():
            out.update(flat(v, p + "." + k if p else k))
    elif isinstance(d, list):
        out[p] = "<%d entries>" % len(d) if p.endswith("stations") else json.dumps(d)
    else:
        out[p] = d
    return out


def leaf_diff(a, b):
    fa, fb = flat(a), flat(b)
    return [(k, fa.get(k, "<absent>"), fb.get(k, "<absent>"))
            for k in sorted(set(fa) | set(fb))
            if not k.startswith(("_comment", "_promotion_note", "_cm2", "_ce")) and fa.get(k, "<absent>") != fb.get(k, "<absent>")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=os.path.join(REPO, "config", "test_suite", "_pv_b.json"))
    ap.add_argument("--runs", default="C:/GnssProcessor/runs")
    ap.add_argument("--prefix", default="_cm_")
    ap.add_argument("--arms", default="c0,c1,c2,c3,c4,c5,c6,pva2")
    ap.add_argument("--out-dir", default="", help="also write <arm>.json here (default: run dir only)")
    ap.add_argument("--force", action="store_true", help="overwrite an existing arm.json")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    bad = 0
    for arm in [s for s in a.arms.split(",") if s]:
        if arm not in ARMS:
            sys.exit(f"unknown arm {arm}")
        desc, knobs, base_override = ARMS[arm]
        base_path = base_override or a.base
        with open(base_path, encoding="utf-8") as fh:
            base_txt = fh.read()
        base = json.loads(base_txt)

        cfg = json.loads(base_txt)
        rundir = os.path.join(a.runs, f"{a.prefix}{arm}").replace("\\", "/")
        for dotted, val in knobs:
            dset(cfg, dotted, val)
        dset(cfg, "network.product_output_path", f"{rundir}/products.txt")
        dset(cfg, "network.state_snapshot_path", f"{rundir}/state_snapshot.bin")
        cfg["_ce_arm"] = [
            f"CARD CE ARM {arm} (SPEC_event_harness s24.14, pre-registered).  Base: "
            f"{os.path.relpath(base_path, REPO).replace(os.sep, '/')}.",
            f"THE GROUP: {desc}",
            "Values are the flattened pl2 chain's (config/test_suite/_pv_a.json), not the golden's - the "
            "arms move PV-B TOWARDS PV-A.  Control = runs/_cm_pvb (not re-run).",
        ]

        os.makedirs(os.path.join(rundir, "logs"), exist_ok=True)
        dest = os.path.join(rundir, "arm.json")
        if os.path.isfile(dest) and not a.force:
            print(f"!! {dest} exists - pass --force to overwrite; skipped")
            continue
        with open(dest, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(cfg, indent=2))
        shutil.copyfile(dest, os.path.join(rundir, "seed.json"))
        if a.out_dir:
            os.makedirs(a.out_dir, exist_ok=True)
            shutil.copyfile(dest, os.path.join(a.out_dir, f"{arm}.json"))

        d = leaf_diff(base, cfg)
        print(f"\n{arm}: {desc}\n  base {os.path.relpath(base_path, REPO).replace(os.sep, '/')} -> {dest} (+ seed.json)")
        for k, old, new in d:
            print(f"     {k:<58s} {old} -> {new}")
        n_path = len([x for x in d if x[0].endswith(("product_output_path", "state_snapshot_path"))])
        n_knob = len(d) - n_path
        ok = (n_knob == len(knobs) and n_path == 2)
        if not ok:
            bad += 1
        print(f"  {'OK' if ok else '!! UNEXPECTED'}: {n_knob} knob leaf/leaves (expected {len(knobs)}) "
              f"+ {n_path} path leaves (expected 2)")

    if bad:
        print(f"\n!! {bad} arm(s) did not match the expected leaf diff", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
