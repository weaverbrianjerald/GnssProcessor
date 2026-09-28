#!/usr/bin/env python3
"""Card CM (SPEC_event_harness s24.8, code-model consistency): write the six single-knob cold
arm configs from the field golden and the two trace-merge smoke configs from the cert4 smoke,
then print every arm's flattened diff against a0 (it must be the knob and the output paths only).

    python scripts/cm_make_arms.py [--golden config/golden/golden_field_gps_dual.json]
        [--runs C:/GnssProcessor/runs] [--smoke-from C:/GnssProcessor/runs/_cert4_smoke_base/smoke.json]
        [--only a0,a2] [--no-smoke] [--no-arms]

Every arm is the golden (already flat: the generator refuses a base with `extends`)
  plus processing.max_duration_s 7200 (240 epochs from the file start; network.start_offset_hours is
                                        left exactly as the golden has it)
  plus network.trace_postfit_rows true (the s24.6 per-row trace, trace-only)
  plus network.trace_zwd true (read (ii)'s network_zwd.csv; trace-only, harness-standard)
  plus one knob (the s24.8 table)
  plus its own output paths: every string value under runs/golden_field_gps_dual/ is re-pointed to
    runs/_cm_<arm>/ (product_output_path, state_snapshot_path); the producer is launched with
    cwd = that dir (scripts/ev_launch.ps1) so ./logs/ is per-arm as well.

The smokes: runs/_cert4_smoke_base/smoke.json re-pointed to runs/_cm_smoke_base (knobs off) and to
runs/_cm_smoke_rows with network.trace_postfit_rows true; products.txt must hash 87a33bf0... in both.
"""
import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ARMS = {
    "a0": {},
    "a1": {"network.carry_rx_code_bias": False},
    "a2": {"network.estimate_sat_code_bias": False,
           "model_corrections.enable_code_phase_bias": True},
    # a2 as written is inert on this lineage: the loader's SH coupling forces estimate_sat_code_bias back
    # on (GnssConfigLoadNetwork.cpp:3083-3085, "SH co-estimation is the W6 iono constraint") and the W6
    # chain then forces enable_code_phase_bias off (:3376), so the run's gnss_config.log prints the W6
    # enabled line and the products are the control's.  The loader's own posture for "SH iono with the
    # external .BIA" is EXT-CB (:3048-3056): network.external_sat_code_bias, which suppresses that force
    # (mutually exclusive with an explicit estimate_sat_code_bias: true, and it hard-errors on an explicit
    # enable_code_phase_bias: false); a2x is that realization of a2's intent, reported beside a2.
    "a2x": {"network.estimate_sat_code_bias": False,
            "network.external_sat_code_bias": True,
            "model_corrections.enable_code_phase_bias": True},
    "a3": {"network.estimate_iono_sh": False},
    "a4": {"filter.process_noise.sat_code_bias_q": 1e-4},
    "a5": {"network.cb_kaula_prior": False},
    # ---- the s24.8 extension (manager, 2026-09-13): the shared station block --------------------
    # a6 / a6b: the ZWD as a time-linked state, the diagonal B9 random-walk carry
    # (GnssConfigLoadNetwork.cpp:4647; :11958-11970, "ZWD carry, B9 ... reuses the phase-leveling
    # warmup / max-gap housekeeping; non-pivot stations only"). phase_leveling is pinned on the golden,
    # phase_leveling_local_block is not, so the block form is out of regime and the diagonal row is the
    # minimal posture the loader accepts.
    "a6":  {"network.carry_zwd": True, "network.zwd_carry_q": 1e-8},
    "a6b": {"network.carry_zwd": True, "network.zwd_carry_q": 1e-7},
    # a7: the ZWD soft constraint pseudo-observation toward the a-priori (GnssConfigLoad.cpp:1559-1564,
    # apriori 0.10 m).  Its getter (GnssConfig::getZwdConstraint) is consumed only by
    # RealTimeEngine.cpp / RealTimeEngineEstimate.cpp; nothing on the network path reads it, so this
    # arm is expected byte-identical to a0 (run as the proof of that, reported as dropped).
    "a7":  {"estimation.zwd_constraint.enabled": True, "estimation.zwd_constraint.sigma_m": 0.2},
    # a8lit: the golden with network.stations cut to three European stations and nothing else changed,
    # expected to be refused, since network.clock_datum_station is MBAR00UGA
    # (ApplicationNetworkSetup.cpp:200 "clock_datum_station '...' not in stations list").
    "a8lit": {"__stations__": ["WROC00POL", "GOP600CZE", "FFMJ00DEU"]},
    # a8: the same cut with the clock datum re-pointed at one of the three (the minimal change the
    # refusal forces; WROC00POL is the best-behaved station of the control).
    "a8":  {"__stations__": ["WROC00POL", "GOP600CZE", "FFMJ00DEU"],
            "network.clock_datum_station": "WROC00POL"},
    # ---- the s24.8 second extension (manager, 2026-09-13): the ionosphere representation ------------
    # a9: the PPP posture at the station level, CODE's OSB (EXT-CB), no SH field, per-LOS ionosphere free
    # at ionosphere_q.  The golden's estimation.ionosphere_weighting.enable stays as it is; whether the
    # network setup then loads the external GIM as a constraint is read from the run's own log.
    "a9":   {"network.estimate_iono_sh": False, "network.external_sat_code_bias": True,
             "network.estimate_sat_code_bias": False, "model_corrections.enable_code_phase_bias": True},
    # a10 / a10b: the SH<->slant representation-error steady-state sigma (T5v2 GM deviation), 5 -> 50 / 20 TECU
    "a10":  {"estimation.global_vtec_sh.representation_sigma_tecu": 50.0},
    "a10b": {"estimation.global_vtec_sh.representation_sigma_tecu": 20.0},
    # a11: the SH field's resolution, degree 8 -> 15 (layer 2: 4 -> 8)
    "a11":  {"estimation.global_vtec_sh.max_degree": 15, "estimation.global_vtec_sh.layer2_max_degree": 8},
}
DEFAULT_ARMS = ["a0", "a1", "a2", "a3", "a4", "a5"]


def cut_stations(cfg, names):
    """network.stations -> only the named entries, in the given order (arrays replace wholesale)."""
    have = {s.get("name"): s for s in cfg.get("network", {}).get("stations", [])}
    missing = [n for n in names if n not in have]
    if missing:
        sys.exit(f"FATAL: stations not in the golden's network.stations: {missing}")
    n_before = len(cfg["network"]["stations"])
    cfg["network"]["stations"] = [have[n] for n in names]
    return f"network.stations: {n_before} -> {len(names)} entries [{', '.join(names)}]"
COMMON = {"processing.max_duration_s": 7200.0, "network.trace_postfit_rows": True,
          # read (ii) needs logs/network_zwd.csv, which the golden does not arm; trace-only
          # (GnssConfigLoadNetwork.cpp:2924, "per-station ZWD trace ENABLED: network_zwd.csv logs each
          # station's estimated ZWD and datum-conditioned formal sigma each emitted epoch"), and the
          # event harness arms it on every leg.  Certified byte-identical on this build by the
          # _cm_a0_nozwd / _cm_a0 pair (products.txt hash).
          "network.trace_zwd": True}
GOLDEN_RUN_SUBDIR = "runs/golden_field_gps_dual"


def set_dotted(cfg, key, val):
    """cfg[a][b][c] = val for key 'a.b.c'; returns the previous value ('<absent>' if none)."""
    parts = key.split(".")
    d = cfg
    for p in parts[:-1]:
        if not isinstance(d.get(p), dict):
            d[p] = {}
        d = d[p]
    old = d.get(parts[-1], "<absent>")
    d[parts[-1]] = val
    return old


def repoint(obj, old, new):
    """Replace `old` inside every string value of a nested JSON object; returns the count."""
    n = 0
    if isinstance(obj, dict):
        items = list(obj.items())
    elif isinstance(obj, list):
        items = list(enumerate(obj))
    else:
        return 0
    for k, v in items:
        if isinstance(v, str):
            if old in v:
                obj[k] = v.replace(old, new)
                n += 1
        else:
            n += repoint(v, old, new)
    return n


def flatten(obj, prefix="", out=None):
    """Nested dict -> {'a.b.c': json-encoded leaf}; arrays are leaves (they replace wholesale)."""
    if out is None:
        out = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "_comment":
                continue
            flatten(v, prefix + k + ".", out)
    else:
        out[prefix[:-1]] = json.dumps(obj, sort_keys=True)
    return out


def diff_flat(base, other):
    keys = sorted(set(base) | set(other))
    return [(k, base.get(k, "<absent>"), other.get(k, "<absent>")) for k in keys
            if base.get(k) != other.get(k)]


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def dump(cfg, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=1)
        fh.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--golden", default=os.path.join(REPO, "config", "golden", "golden_field_gps_dual.json"))
    ap.add_argument("--runs", default="C:/GnssProcessor/runs")
    ap.add_argument("--smoke-from", default="C:/GnssProcessor/runs/_cert4_smoke_base/smoke.json")
    ap.add_argument("--only", default="", help="comma list of arms to write (default all six)")
    ap.add_argument("--no-smoke", action="store_true")
    ap.add_argument("--no-arms", action="store_true")
    a = ap.parse_args()

    if not a.no_arms:
        golden = load(a.golden)
        if "extends" in golden:
            sys.exit(f"FATAL: {a.golden} has an `extends` chain; flatten it first (make_golden_config.py output is flat)")
        want = [s for s in a.only.split(",") if s] or DEFAULT_ARMS   # a2x / a6.. only on request
        flats = {}
        for arm in want:
            knobs = dict(ARMS[arm])
            cfg = json.loads(json.dumps(golden))
            n_paths = repoint(cfg, GOLDEN_RUN_SUBDIR, f"runs/_cm_{arm}")
            changes = []
            stations = knobs.pop("__stations__", None)
            if stations:
                changes.append(cut_stations(cfg, stations))
            for k, v in {**COMMON, **knobs}.items():
                old = set_dotted(cfg, k, v)
                changes.append(f"{k}: {old} -> {v}")
            cfg.setdefault("_comment", []).append(
                f"card CM (SPEC_event_harness s24.8) arm {arm}: " + "; ".join(changes)
                + f"; {n_paths} output path(s) re-pointed to runs/_cm_{arm}/")
            path = os.path.join(a.runs, f"_cm_{arm}", "arm.json")
            dump(cfg, path)
            flats[arm] = flatten(cfg)
            print(f"{arm}: wrote {path}  [{n_paths} path(s) re-pointed]  " + "; ".join(changes))
        base = flats.get("a0") or flatten(json.loads(json.dumps(golden)))
        for arm, f in flats.items():
            if arm == "a0":
                continue
            d = diff_flat(base, f)
            print(f"\n{arm} vs a0: {len(d)} flattened key(s) differ")
            for k, b, v in d:
                print(f"  {k}: {b} -> {v}")
        if "a0" in flats:
            d = diff_flat(flatten(golden), flats["a0"])
            print(f"\na0 vs the golden: {len(d)} flattened key(s) differ")
            for k, b, v in d:
                print(f"  {k}: {b} -> {v}")

    if not a.no_smoke:
        smoke = load(a.smoke_from)
        src = os.path.basename(os.path.dirname(os.path.abspath(a.smoke_from)))
        flats = {}
        for name, extra in (("_cm_smoke_base", {}), ("_cm_smoke_rows", {"network.trace_postfit_rows": True})):
            cfg = json.loads(json.dumps(smoke))
            n_paths = repoint(cfg, f"runs/{src}", f"runs/{name}")
            changes = [f"{k}: {set_dotted(cfg, k, v)} -> {v}" for k, v in extra.items()]
            path = os.path.join(a.runs, name, "smoke.json")
            dump(cfg, path)
            flats[name] = flatten(cfg)
            print(f"\n{name}: wrote {path}  [{n_paths} path(s) re-pointed from runs/{src}]  " + "; ".join(changes))
        d = diff_flat(flats["_cm_smoke_base"], flats["_cm_smoke_rows"])
        print(f"_cm_smoke_rows vs _cm_smoke_base: {len(d)} flattened key(s) differ")
        for k, b, v in d:
            print(f"  {k}: {b} -> {v}")
        d = diff_flat(flatten(smoke), flats["_cm_smoke_base"])
        print(f"_cm_smoke_base vs {src}/smoke.json: {len(d)} flattened key(s) differ")
        for k, b, v in d:
            print(f"  {k}: {b} -> {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
