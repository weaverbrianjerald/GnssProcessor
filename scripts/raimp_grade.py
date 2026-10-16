#!/usr/bin/env python3
"""RAIM POWER leg grader (docs/SPEC_raim_pool_rehost.md §7.2, the pre-registered P0-P3 bars,
plus the manager's P0' extension: n = 1 re-drawn at three more event epochs).

    python scripts/raimp_grade.py [--runs C:/GnssProcessor/runs] [--json out.json]
                                  [--batch-status runs/_raimp_batch_status.json ...]

Legs from the v8 field seed (ep0604, 60 epochs): ``raimp_null_screen`` and, for
n in {1, 2, 4}, ``raimp_n<n>_{off,screen,act}`` (the (n, n, n) equal-cycle slip on
L1C / L2W / L5X of G24 at GOPE00CZE at tow 277620, leg epoch k = 11); then the
P0' draws ``raimp_n1e<at_ep>_{off,act}`` for at_ep in {14, 18, 22} (event at leg
epoch k = at_ep + 1). The OFF arms' null is ``evleg_cert_null_v8`` (same posture,
no screen); the screen / act arms' null is ``raimp_null_screen`` (the actuator is
inert on a null, so it is the act posture's null too).

EPOCH AXIS. The brief's convention is ``k = 1 + (tow - 277320)/30``, the leg's
1-based epoch number. The brief quotes every window for the k = 11 event
("k 9..15", "sigma0 k 5..25", "wire +10..+50", "P2 +20..+50"); each is applied as
an offset from the leg's own event epoch kev, so a draw at kev = 19 reads the
same event-relative windows. dk = k - kev is printed beside k where the fp1
gate-table's event-relative read is reproduced.

What is read, and from which instrument (formats read from source, cited):

  VERIFY   fp2_leg_verify.py's checks: rc, wall, absence of ``COLD START``,
           presence of a ``WARM RESUME`` line, 61 product epochs / consumer
           rows, the run-end emitted triple, the RS-8 / RS-9 restore lines, the
           posture the producer itself reports (GnssConfigLoadNetwork.cpp:10362
           "RAIM POOL RE-HOST ... Refloat actuator is ARMED | OFF (measure-only)";
           :10374 "post-fit RAIM is INERT" on an OFF leg), leg_meta's recorded
           extra and event tow, and the flattened leg.json diff inside each
           triple / pair (the knob(s) and the two per-leg paths only; the inject
           block is byte-equal across the arms and matches the E3 shape plus the
           L2W event at the leg's own tow).
  CENSUS   detector rows at (GOPE00CZE, G24), k in [kev-2, kev+4]: T1 IF-TD
           (network_if_slip.csv plus T1_if_slip events), T0 MW / LLI / band-scope
           events, LLI bits (network_slip_raw.csv), the mw_slip_detector.log DET
           lines for G24 (station-blind by design), and D5 rows on the arc
           (network_d5_flags.csv: chord = (receiver u, satellite 1000000+j),
           DdPriorBuild.cpp:182; `station` is u, `amb_key` the arc).
           A D5 trip that the v3 corroboration gate suppresses never reaches the
           dump (InvariantCarryDriver.cpp:1646-1648 `continue`s before
           last_innov_flags_), so the refused outcome is visible only in the
           run-end "{} of {} gate trips SUPPRESSED" total (leg minus null,
           Teardown.cpp:5021) and in the tri-state `raw_stepped` of the
           rows that do land (0 = measured flat, i.e. the corroboration set;
           InstallCarry.cpp:152-170).
           The (1,2) IF-TD bar is max(k_mad·s_eff·elev_map, floor_frac·λ_IF(1,1))
           (SlipQc.cpp:1194-1197; if_phase_td_floor_frac default 1.0,
           GnssConfigLoadNetwork.cpp:4600) = 0.106953 m on this chain, equal to
           the step an equal-count n = 1 slip adds to the chain's dd, so the
           margin |dd| - thr at the event epoch is what decides P0 per draw.
  SIGMA0   three instruments, because the pre-registered column is not the
           re-hosted statistic: (a) network_epoch_status.csv `sigma0` is the
           within-epoch tier's `sigma0_adapt` (StashStatus.cpp:530, "0.0
           unless within-epoch RAIM ran"), pooled over R.pl_amb which is empty on
           this lineage, printed anyway because it is pre-registered; (b) the
           re-hosted σ̂0 (Raim.cpp:417-428) is tallied only at run end
           (mean / max over live-pool epochs) and feeds ura_sigma0_ewma
           (:435, alpha 0.1), which scales the emitted URA by max(1, ewma)
           (SolPrep.cpp:433); the per-epoch U-record ratio leg/twin (same
           solve) therefore reads the EWMA, and sigma0_k = (ewma_k - 0.9
           ewma_{k-1}) / 0.1 inverts it where both sides are above the clip
           (raim_gate_grade.py's inversion); (c) network_raim.csv rows (spiked
           cells only) and network_raim_attrib.csv rows carry σ̂0 per flagged
           cell. network_pf_keyed.csv STA / SAT rows (trace_postfit_keyed,
           FrameDiag.cpp) are the coarse post-fit residual read on the
           failure branch.
  T3/T4    every T3_* / T4_* event row per leg, with identity; raim_attribution
           is on for this lineage, so the T4 path drives the refloat and the T3
           consensus block (Raim.cpp:1180, `!attrib_act`) never emits.
  DRAIN    act legs: run-end "queued into the deferred funnel and delivered"
           (Teardown.cpp:868); the drain itself (ApplicationNetwork.cpp
           :1590-1596) logs no identity, so the identity is the T4_attrib row
           that queued it plus the next epoch's chord births / arc refloats.
  WIRE     products.txt G24 CLK / per-signal PB, event minus null at the same
           posture: max |d| over k kev-1..kev+39 (the brief's "+10..+50") and
           over dk 0..45 (the fp1 G-C read); worst other key; blast radius at the
           event epoch; the declared discontinuity load (C clock_disc / P
           phase_disc counters differing from the null).
  HASHES   sha256 of every products.txt; null_screen == cert_null_v8; screen ==
           off per n; act vs screen per record kind with the first differing
           epoch; the extension's act vs off likewise.
  CONSUMER closed_loop_trace.csv ENU event - null, reported only (SPEC_event_
           harness §24.1), printed against the consumer's own ignition curve (it
           cold-starts on every leg).
"""
import argparse
import csv
import hashlib
import json
import math
import os
import re
import statistics
import sys
from collections import defaultdict

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):                        # pragma: no cover
    pass

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, r"C:\GnssProcessor\scripts"):
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.insert(0, _p)

from event_field_grade import (_RT_KIND, consumer_series, read_events,   # noqa: E402
                               rt_events, withheld)
from event_grade import read_products                                  # noqa: E402
from raim_gate_grade import read_stdout as rehost_telemetry             # noqa: E402

EPOCH_S = 30.0
FIRST_TOW = 277320.00000810623
STATION, SAT = "GOPE00CZE", "G24"
BANDS = ("L1C", "L2W", "L5X")
NS = (1, 2, 4)
EXT_AT_EPS = (14, 18, 22)
CERT_NULL = "evleg_cert_null_v8"
NULL_SCREEN = "evleg_raimp_null_screen"
ALLOWED_PATH_KEYS = ("network.product_output_path", "network.state_snapshot_path")
KNOB_SCREEN = "network.raim_carry_invariant"
KNOB_ACT = "network.raim_carry_invariant_refloat"
URA_ALPHA = 0.1
N_EPOCHS = 61

ARMED_LINE = "Network producer (RAIM POOL RE-HOST): post-fit RAIM re-hosted"
ACT_ARMED = "Refloat actuator is ARMED"
ACT_OFF = "Refloat actuator is OFF (measure-only)"
INERT_LINE = "post-fit RAIM is INERT"
RS_LINES = {"rs8_canonical": "(RS-8 canonical restore): ON",
            "rs9_rxcb_lin": "(RS-9 rx-CB frame restore): ON"}


# ------------------------------------------------------------------ utilities
def kleg(tow):
    return 1 + int(round((float(tow) - FIRST_TOW) / EPOCH_S))


def tow_of(k):
    return FIRST_TOW + (k - 1) * EPOCH_S


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fnum(v, d=4, w=10):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "n/a".rjust(w)
    return f"{v:{w}.{d}f}"


def flat(d, p=""):
    out = {}
    for k, v in d.items():
        kk = f"{p}.{k}" if p else k
        if isinstance(v, dict):
            out.update(flat(v, kk))
        else:
            out[kk] = v
    return out


def csv_rows(path):
    if not os.path.isfile(path):
        return None
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        return list(csv.DictReader(fh))


def expected_inject(n, tow):
    return {"enable": True,
            "events": [{"kind": "slip", "station": STATION, "sat": SAT, "signal": s,
                        "delta_cycles": n, "tow": tow} for s in BANDS]}


def leg_table():
    """The matrix: dicts with name / arm / n / kev / null / twin / group."""
    legs = [{"name": "raimp_null_screen", "arm": "null_screen", "n": None, "kev": 11,
             "null": CERT_NULL, "twin": CERT_NULL, "group": "main"}]
    for n in NS:
        legs.append({"name": f"raimp_n{n}_off", "arm": "off", "n": n, "kev": 11,
                     "null": CERT_NULL, "twin": None, "group": "main"})
        legs.append({"name": f"raimp_n{n}_screen", "arm": "screen", "n": n, "kev": 11,
                     "null": NULL_SCREEN, "twin": f"evleg_raimp_n{n}_off", "group": "main"})
        legs.append({"name": f"raimp_n{n}_act", "arm": "act", "n": n, "kev": 11,
                     "null": NULL_SCREEN, "twin": f"evleg_raimp_n{n}_off", "group": "main"})
    for e in EXT_AT_EPS:
        legs.append({"name": f"raimp_n1e{e}_off", "arm": "off", "n": 1, "kev": e + 1,
                     "null": CERT_NULL, "twin": None, "group": "ext"})
        legs.append({"name": f"raimp_n1e{e}_act", "arm": "act", "n": 1, "kev": e + 1,
                     "null": NULL_SCREEN, "twin": f"evleg_raimp_n1e{e}_off", "group": "ext"})
    return legs


# ------------------------------------------------------------------- VERIFY
def verify_leg(legdir, arm, n, kev):
    """fp2_leg_verify.read_leg, re-targeted at the re-host posture lines."""
    r = {"dir": legdir, "arm": arm, "n": n, "kev": kev, "problems": []}
    if not os.path.isdir(legdir):
        r["problems"].append("leg directory missing")
        return r
    ec = os.path.join(legdir, "exitcode.txt")
    if not os.path.isfile(ec):
        r["problems"].append("no exitcode.txt (leg did not finish)")
        return r
    parts = open(ec, encoding="utf-8").read().split()
    r["rc"] = int(parts[0])
    r["wall_s"] = float(parts[1]) if len(parts) > 1 else None
    if r["rc"] != 0:
        r["problems"].append(f"exit code {r['rc']}")
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
    pp = os.path.join(legdir, "products.txt")
    r["product_epochs"] = (len(set(re.findall(r"^E,[^,]*,([0-9.]+)", open(
        pp, encoding="utf-8", errors="replace").read(), re.M))) if os.path.isfile(pp) else 0)
    r["products_sha256"] = sha256(pp) if os.path.isfile(pp) else None
    cp = os.path.join(legdir, "logs", "closed_loop_trace.csv")
    r["consumer_rows"] = (sum(1 for _ in open(cp, encoding="utf-8", errors="replace")) - 1
                          if os.path.isfile(cp) else 0)
    for nm, v in (("product epochs", r["product_epochs"]), ("consumer rows", r["consumer_rows"])):
        if v != N_EPOCHS:
            r["problems"].append(f"{v} {nm} (want {N_EPOCHS})")
    m = re.search(r"Network producer complete: (\d+) epochs emitted, (\d+) skipped "
                  r"\((\d+) of those withheld[^)]*\) — of (\d+) synced", txt)
    if m:
        tot, skipped, wh, synced = (int(g) for g in m.groups())
        r["run_end"] = {"emitted_total": tot, "skipped": skipped, "withheld": wh,
                        "synced_total": synced}
        if wh:
            r["problems"].append(f"{wh} withheld epoch(s)")
        if r["resume_n_emitted"] is not None and tot - r["resume_n_emitted"] != N_EPOCHS:
            r["problems"].append(f"run-end emitted delta {tot - r['resume_n_emitted']} (want {N_EPOCHS})")
    else:
        r["problems"].append("no 'Network producer complete' run-end line")
    r["rs_restores"] = {k: (v in txt) for k, v in RS_LINES.items()}
    for k, ok in r["rs_restores"].items():
        if not ok:
            r["problems"].append(f"{k} restore line ABSENT")
    # posture, in the producer's own voice
    r["armed_line"] = ARMED_LINE in txt
    r["act_armed_line"] = ACT_ARMED in txt
    r["act_off_line"] = ACT_OFF in txt
    r["inert_line"] = INERT_LINE in txt
    want_screen = arm in ("screen", "act", "null_screen")
    want_act = arm == "act"
    if r["armed_line"] != want_screen:
        r["problems"].append("RAIM POOL RE-HOST armed line "
                             + ("present on an OFF leg" if r["armed_line"] else "ABSENT on a screen/act leg"))
    if r["inert_line"] != (not want_screen):
        r["problems"].append("post-fit RAIM INERT line "
                             + ("present on a screen/act leg" if r["inert_line"] else "ABSENT on an OFF leg"))
    if want_screen and (r["act_armed_line"] != want_act or r["act_off_line"] == want_act):
        r["problems"].append(f"actuator line mismatch: ARMED={r['act_armed_line']} "
                             f"OFF={r['act_off_line']} (want ARMED={want_act})")
    # leg_meta extra + event
    mp = os.path.join(legdir, "leg_meta.json")
    if os.path.isfile(mp):
        meta = json.load(open(mp, encoding="utf-8"))
        r["meta_event"] = meta.get("event")
        r["meta_extra"] = meta.get("extra")
        r["meta_fingerprint"] = meta.get("fingerprint")
        r["meta_snapshot_sha256"] = meta.get("snapshot_sha256")
        r["meta_event_tow"] = meta.get("event_tow")
        r["meta_at_ep"] = meta.get("at_ep")
        net = (meta.get("extra") or {}).get("network", {})
        if want_screen and net.get("raim_carry_invariant") is not True:
            r["problems"].append("leg_meta extra lacks raim_carry_invariant: true")
        if (not want_screen) and "raim_carry_invariant" in net:
            r["problems"].append("OFF leg_meta extra records raim_carry_invariant")
        if want_act and net.get("raim_carry_invariant_refloat") is not True:
            r["problems"].append("act leg_meta extra lacks raim_carry_invariant_refloat: true")
        if (not want_act) and "raim_carry_invariant_refloat" in net:
            r["problems"].append("non-act leg_meta extra records raim_carry_invariant_refloat")
        if n is not None:
            want_tow = tow_of(kev)
            if net.get("inject") != expected_inject(n, want_tow):
                r["problems"].append("leg_meta extra inject block != expected (n,n,n) shape at the leg's tow")
            if meta.get("event_tow") != want_tow:
                r["problems"].append(f"leg_meta event_tow {meta.get('event_tow')!r} != {want_tow!r}")
            if meta.get("at_ep") != kev - 1:
                r["problems"].append(f"leg_meta at_ep {meta.get('at_ep')} != {kev - 1}")
        elif "inject" in net:
            r["problems"].append("null leg_meta extra carries an inject block")
    else:
        r["problems"].append("no leg_meta.json")
    # the ON-path telemetry (invariant #4)
    r["rehost"] = rehost_telemetry(__import__("pathlib").Path(legdir))
    r["rehost_lines"] = [" ".join(ln.split()) for ln in txt.splitlines()
                         if "RAIM pool re-host" in ln or "RAIM POOL RE-HOST" in ln]
    m = re.search(r"D5 fine-slip innovation gate\): (\d+) carried-DD innovation flag\(s\), "
                  r"(\d+) fresh re-tap\(s\)", txt)
    r["d5_flags_retaps"] = [int(m.group(1)), int(m.group(2))] if m else None
    m = re.search(r"D5 v3 corroboration gate\): (\d+) of (\d+) gate trips SUPPRESSED", txt)
    r["d5_suppressed_of_trips"] = [int(m.group(1)), int(m.group(2))] if m else None
    r["raim_attrib_lines"] = [" ".join(ln.split())[:300] for ln in txt.splitlines()
                              if "Network[RAIM-attrib]" in ln or "Network[RAIM]:" in ln]
    r["inject_lines"] = [" ".join(ln.split())[:200] for ln in txt.splitlines()
                         if "fault inject): " in ln and "armed" in ln]
    r["withheld"] = withheld(legdir)
    if want_screen and r["rehost"].get("pool_cells") in (None, 0):
        r["problems"].append("re-host telemetry: pool_cells missing or 0 (DEAD WIRING?)")
    return r


def pair_diff(a_path, b_path, allowed_knobs):
    if not (os.path.isfile(a_path) and os.path.isfile(b_path)):
        return {"err": "missing leg.json"}
    a, b = flat(json.load(open(a_path))), flat(json.load(open(b_path)))
    diffs, unexpected = {}, []
    for k in sorted(set(a) | set(b)):
        va, vb = a.get(k, "<MISSING>"), b.get(k, "<MISSING>")
        if va != vb:
            diffs[k] = [va, vb]
            if k in ALLOWED_PATH_KEYS or k in allowed_knobs:
                continue
            unexpected.append(k)
    inj_a = {k: a[k] for k in a if k.startswith("network.inject")}
    inj_b = {k: b[k] for k in b if k.startswith("network.inject")}
    if inj_a != inj_b:
        unexpected.append("network.inject.* differs between arms")
    return {"n_diffs": len(diffs), "keys": sorted(diffs), "unexpected": unexpected,
            "inject_keys": len(inj_a)}


# ------------------------------------------------------------------- CENSUS
def census(legdir, lo, hi):
    c = {"window": [lo, hi]}
    ev = read_events(legdir) or []
    at = []
    for r in ev:
        try:
            k = kleg(r["tow"])
        except (KeyError, ValueError):
            continue
        if r.get("station") == STATION and r.get("sat") == SAT and lo <= k <= hi:
            at.append((k, r.get("tier"), r.get("event"), r.get("statistic"),
                       r.get("threshold"), r.get("action"), (r.get("detail") or "")[:100]))
    c["events_at_arc"] = sorted(at)
    c["t1_rows"] = [x for x in at if x[1] == "T1_if_slip"]
    c["t0_rows"] = [x for x in at if x[1] == "T0_mw_detect" or x[1] == "T0_band_scope"]
    c["lli_event_rows"] = [x for x in at if x[2] == "lli_slip"]
    c["inject_rows"] = [x for x in at if x[1] == "INJECT"]
    c["other_rows"] = [x for x in at if x[1] not in ("T1_if_slip", "T0_mw_detect",
                                                     "T0_band_scope", "INJECT")]
    # IF-TD chain rows (every epoch, flagged or not); thr_m = max(k_mad*s_eff*emap, floor_m)
    ifr = csv_rows(os.path.join(legdir, "logs", "network_if_slip.csv")) or []
    c["if_td"] = sorted((kleg(r["tow"]), f"{r['sig1']}/{r['sig2']}", float(r["abs_dev_m"]),
                         float(r["thr_m"]), int(r["flagged"]), int(r["is_ref"]),
                         float(r.get("dd_m") or "nan"), float(r.get("floor_m") or "nan"),
                         float(r.get("s_eff_m") or "nan"))
                        for r in ifr if r["station"] == STATION and r["sat"] == SAT
                        and lo <= kleg(r["tow"]) <= hi)
    c["if_td_flagged"] = [x for x in c["if_td"] if x[4] == 1]
    # raw MW / GF / LLI on the primary pair
    raw = csv_rows(os.path.join(legdir, "logs", "network_slip_raw.csv")) or []
    c["raw"] = sorted((kleg(r["tow"]), f"{r['sig1']}/{r['sig2']}", float(r["mw_m"]),
                       float(r["gf_m"]), int(r["lli1"]), int(r["lli2"]))
                      for r in raw if r["station"] == STATION and r["sat"] == SAT
                      and lo <= kleg(r["tow"]) <= hi)
    c["lli_asserted"] = [x for x in c["raw"] if x[4] or x[5]]
    # MW log DET lines for G24 (station-blind), in the window
    mwp = os.path.join(legdir, "logs", "mw_slip_detector.log")
    det = []
    if os.path.isfile(mwp):
        pat = re.compile(r"^\[\s*(\d+),\s*([0-9.]+)")
        for ln in open(mwp, encoding="utf-8", errors="replace"):
            if f"MW slip detected for {SAT} " not in ln:
                continue
            m = pat.match(ln)
            if not m:
                continue
            k = kleg(m.group(2))
            if lo <= k <= hi:
                lam = re.search(r"lambda_wl=([\d.]+)", ln)
                dev = re.search(r"\|dev\|=([-\d.]+) m \(([-\d.]+) WL\)", ln)
                det.append((k, float(lam.group(1)) if lam else None,
                            float(dev.group(2)) if dev else None))
    c["mw_det_lines"] = det
    # Rows from the D5 detector: the arc itself, plus collateral at the station or on the satellite
    d5 = csv_rows(os.path.join(legdir, "logs", "network_d5_flags.csv")) or []
    arc, sta_other, sat_other = [], [], []
    for r in d5:
        k = kleg(r["tow"])
        if not (lo <= k <= hi):
            continue
        sat = r["amb_key"].split("_")[0] if r["amb_key"] else ""
        row = (k, r["station"], r["amb_key"], float(r["std_innov"] or "nan"),
               r["hold_chord"], r["injected_prev"], r["raw_stepped"], r["v"], r["z_post"],
               r["warmup_suppressed"])
        if r["station"] == STATION and sat == SAT:
            arc.append(row)
        elif r["station"] == STATION:
            sta_other.append(row)
        elif sat == SAT:
            sat_other.append(row)
    c["d5_arc"] = sorted(arc)
    c["d5_station_other"] = sorted(sta_other)
    c["d5_sat_other"] = sorted(sat_other)
    # per-epoch status counters in the window
    es = csv_rows(os.path.join(legdir, "logs", "network_epoch_status.csv")) or []
    c["status"] = {kleg(r["tow"]): {kk: r.get(kk) for kk in
                                    ("disposition", "sigma0", "n_d5_flags", "n_d5_retaps",
                                     "n_chord_births", "n_arc_refloat", "n_slip_refloats",
                                     "sigma0_pf_phase", "n_pf_rows")}
                   for r in es if lo <= kleg(r["tow"]) <= hi}
    c["n_detector_rows"] = (len(c["if_td_flagged"]) + len(c["t1_rows"]) + len(c["t0_rows"])
                            + len(c["lli_asserted"]))
    return c


def if_td_margin(c, kev):
    """(dd, thr, |dd| - thr) of the (1,2) chain row at the event epoch (None if absent)."""
    for k, p, d, t, f, r, dd, fl, se in c["if_td"]:
        if k == kev and p == "L1C/L2W":
            return {"dd_m": dd, "thr_m": t, "margin_m": d - t, "flagged": f}
    return None


# ------------------------------------------------------------------- SIGMA0
def sigma0_series(legdir, lo, hi):
    es = csv_rows(os.path.join(legdir, "logs", "network_epoch_status.csv")) or []
    return {kleg(r["tow"]): float(r["sigma0"]) for r in es if lo <= kleg(r["tow"]) <= hi}


def ura_by_epoch(path):
    """{k: {sat: ura}} from the U records (keyed on the open E epoch)."""
    out = defaultdict(dict)
    k = None
    if not os.path.isfile(path):
        return out
    for ln in open(path, encoding="utf-8", errors="replace"):
        f = ln.rstrip("\n").split(",")
        if f[0] == "E" and len(f) >= 3:
            try:
                k = kleg(f[2])
            except ValueError:
                k = None
        elif f[0] == "U" and k is not None and len(f) >= 3:
            try:
                out[k][f[1]] = float(f[2])
            except ValueError:
                pass
    return out


def ura_ratio_series(legdir, ctldir, lo=1, hi=N_EPOCHS, alpha=URA_ALPHA):
    """Per-epoch median U ratio leg/ctl (= max(1, ewma) when the solve is shared) + inversion."""
    a, b = ura_by_epoch(os.path.join(legdir, "products.txt")), \
        ura_by_epoch(os.path.join(ctldir, "products.txt"))
    ratio = {}
    for k in sorted(set(a) & set(b)):
        if not (lo <= k <= hi):
            continue
        rs = [a[k][s] / b[k][s] for s in a[k] if s in b[k] and b[k][s]]
        if rs:
            ratio[k] = (statistics.median(rs), max(rs) - min(rs), len(rs))
    implied = {}
    ks = sorted(ratio)
    for i in range(1, len(ks)):
        e1, e0 = ratio[ks[i]][0], ratio[ks[i - 1]][0]
        if e1 > 1.0 and e0 > 1.0:
            implied[ks[i]] = (e1 - (1.0 - alpha) * e0) / alpha
    return ratio, implied


def raim_rows(legdir, lo, hi):
    out = {"raim": [], "attrib": []}
    for r in csv_rows(os.path.join(legdir, "logs", "network_raim.csv")) or []:
        k = kleg(r["tow"])
        if lo <= k <= hi or (r["station"] == STATION and r["sat"] == SAT):
            out["raim"].append((k, r["sat"], r["station"], r["s_max_raw"], r["s_max_norm"],
                                r["sigma0"], r["spiked"], r["n_sta_spiked"], r["refloated"]))
    for r in csv_rows(os.path.join(legdir, "logs", "network_raim_attrib.csv")) or []:
        k = kleg(r["tow"])
        if lo <= k <= hi:
            out["attrib"].append((k, r["dim"], r["entity"], r["effect"], r["prominence"],
                                  r["n_cells"], r["overall"], r["sigma0"], r["acted"]))
    return out


def pf_keyed(legdir, lo, hi):
    rows = csv_rows(os.path.join(legdir, "logs", "network_pf_keyed.csv")) or []
    out = {"STA": {}, "SAT": {}}
    for r in rows:
        k = kleg(r["tow"])
        if not (lo <= k <= hi):
            continue
        if r["kind"] == "STA" and r["key"] == STATION:
            out["STA"][k] = (int(r["n_phase"]), float(r["s0_phase"]), float(r["rms_phase"]),
                             float(r["max_phase"]))
        elif r["kind"] == "SAT" and r["key"] == SAT:
            out["SAT"][k] = (int(r["n_phase"]), float(r["s0_phase"]), float(r["rms_phase"]),
                             float(r["max_phase"]))
    return out


def t34_rows(legdir):
    out = []
    for r in read_events(legdir) or []:
        t = r.get("tier", "")
        if t.startswith(("T3", "T4")):
            try:
                k = kleg(r["tow"])
            except (KeyError, ValueError):
                k = None
            out.append((k, t, r.get("event"), r.get("sat"), r.get("station"),
                        r.get("statistic"), r.get("threshold"), r.get("action"),
                        (r.get("detail") or "")[:80]))
    return sorted(out, key=lambda x: (x[0] if x[0] is not None else -1, x[1]))


# --------------------------------------------------------------------- WIRE
def wire(legdir, nulldir, klo, khi, sat=SAT):
    """G24 CLK / per-signal PB max |d| over leg epochs klo..khi, event - null."""
    A = read_products(os.path.join(legdir, "products.txt"))
    B = read_products(os.path.join(nulldir, "products.txt"))
    clk, pb, other = None, {}, (None, None, None)
    clk_at = None
    n = 0
    series = {}
    for t in sorted(set(A) & set(B)):
        k = kleg(t)
        if not (klo <= k <= khi):
            continue
        n += 1
        for key, va in A[t].items():
            vb = B[t].get(key)
            if vb is None:
                continue
            d = abs(va - vb)
            if key[0] == "C" and key[1] == sat:
                series[k] = d
                if clk is None or d > clk:
                    clk, clk_at = d, k
            elif key[0] == "P" and key[1] == sat:
                if d > pb.get(key[2], (0.0, None))[0]:
                    pb[key[2]] = (d, k)
            elif other[0] is None or d > other[0]:
                other = (d, "/".join(key), k)
    return {"clk": clk, "clk_at": clk_at, "pb": pb,
            "pb_max": max((v for v, _ in pb.values()), default=None),
            "other": {"v": other[0], "key": other[1], "at": other[2]},
            "n_epochs": n, "clk_series": series, "window": [klo, khi],
            "clk_mean": (statistics.fmean(series.values()) if series else None)}


def blast(legdir, nulldir, k, tol=0.5):
    A = read_products(os.path.join(legdir, "products.txt"))
    B = read_products(os.path.join(nulldir, "products.txt"))
    out = []
    for t in sorted(set(A) & set(B)):
        if kleg(t) != k:
            continue
        for key, va in A[t].items():
            vb = B[t].get(key)
            if vb is not None and abs(va - vb) > tol:
                out.append((round(abs(va - vb), 4), "/".join(key)))
    return sorted(out, reverse=True)


def records_diff(a_path, b_path):
    """Per record kind: identical?, first differing epoch (k) and record."""
    def load(p):
        kinds = defaultdict(list)
        k = None
        for ln in open(p, encoding="utf-8", errors="replace"):
            ln = ln.rstrip("\n")
            if not ln or ln.startswith("#"):
                continue
            f = ln.split(",")
            if f[0] == "E" and len(f) >= 3:
                try:
                    k = kleg(f[2])
                except ValueError:
                    k = None
            kinds[f[0]].append((k, ln))
        return kinds
    ka, kb = load(a_path), load(b_path)
    out = {}
    for kind in sorted(set(ka) | set(kb)):
        la, lb = ka.get(kind, []), kb.get(kind, [])
        same = [x[1] for x in la] == [x[1] for x in lb]
        first = None
        if not same:
            for i, (x, y) in enumerate(zip(la, lb)):
                if x[1] != y[1]:
                    first = {"k": x[0], "a": x[1][:120], "b": y[1][:120]}
                    break
            if first is None:
                first = {"k": (la or lb)[min(len(la), len(lb)) - 1][0] if (la or lb) else None,
                         "a": f"<len {len(la)}>", "b": f"<len {len(lb)}>"}
        out[kind] = {"identical": same, "n_a": len(la), "n_b": len(lb), "first": first}
    return out


def disc_delta(legdir, nulldir, lo, hi, kev):
    """Products C / P records whose discontinuity counter differs from the null.

    A refloat that re-realizes a satellite's phase-bias level declares itself on
    the wire through the P record's phase_disc (and a C record's clock_disc); the
    count of (k, sat, signal) records whose counter differs from the null's over
    the window is the declared re-realization load the event put on the consumer
    (the FP-2 6 h twin failed on this number). Also the per-epoch increment count
    (leg vs null) at kev-1..kev+3, showing where the response landed.
    """
    def load(p):
        out, k = {}, None
        for ln in open(p, encoding="utf-8", errors="replace"):
            f = ln.rstrip("\n").split(",")
            if f[0] == "E" and len(f) >= 3:
                try:
                    k = kleg(f[2])
                except ValueError:
                    k = None
            elif f[0] == "P" and len(f) >= 7 and f[6].isdigit():
                out[(k, f[1], f[2])] = int(f[6])
            elif f[0] == "C" and len(f) >= 5 and f[4].isdigit():
                out[(k, f[1], "CLK")] = int(f[4])
        return out
    a, b = load(os.path.join(legdir, "products.txt")), load(os.path.join(nulldir, "products.txt"))
    differing = [(kk, s, g) for (kk, s, g), v in a.items()
                 if kk is not None and lo <= kk <= hi and b.get((kk, s, g)) != v]
    first = min((x[0] for x in differing), default=None)
    inc = {}
    for k in range(kev - 1, kev + 4):
        ia = sum(1 for (kk, s, g), v in a.items() if kk == k and (k - 1, s, g) in a and v != a[(k - 1, s, g)])
        ib = sum(1 for (kk, s, g), v in b.items() if kk == k and (k - 1, s, g) in b and v != b[(k - 1, s, g)])
        inc[k] = (ia, ib)
    return {"n_differing": len(differing), "first_k": first,
            "first_records": sorted(differing)[:6], "increments": inc}


def consumer(legdir, nulldir, ev_tow, post_dk):
    a, ea = consumer_series(legdir, ev_tow)
    b, eb = consumer_series(nulldir, ev_tow)
    if ea or eb or not a or not b:
        return {"err": ea or eb or "no rows"}
    d3, abs3 = {}, {}
    for dk in a:
        if dk in b and 0 <= dk <= post_dk:
            d3[dk] = math.sqrt(sum((a[dk][i] - b[dk][i]) ** 2 for i in range(3)))
            abs3[dk] = math.sqrt(sum(a[dk][i] ** 2 for i in range(3)))
    if not d3:
        return {"err": "no common epochs"}
    mx = max((v, k) for k, v in d3.items())
    seg = [v for k, v in d3.items() if 30 <= k <= post_dk]
    ra, rb = rt_events(legdir, ev_tow, 0, post_dk), rt_events(nulldir, ev_tow, 0, post_dk)
    return {"max3d": mx[0], "max3d_at_dk": mx[1], "post_dk": post_dk,
            "mean3d_30_post": (sum(seg) / len(seg)) if seg else None, "n": len(d3),
            "d3_at": {dk: d3.get(dk) for dk in (0, 1, 2, 5, 10, 20, 30, 40, 50)},
            "abs3_at": {dk: abs3.get(dk) for dk in (0, 5, 20, 50)},
            "rt_resets_ev_minus_null": ({k: ra[k] - rb[k] for k, _ in _RT_KIND}
                                        if (ra is not None and rb is not None) else None)}


# --------------------------------------------------------------------- MAIN
def hdr(t, ch="="):
    print("\n" + ch * 100 + "\n" + t + "\n" + ch * 100)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default=r"C:\GnssProcessor\runs")
    ap.add_argument("--json", default=None)
    ap.add_argument("--batch-status", nargs="*", default=[])
    a = ap.parse_args()
    R = a.runs

    def L(name):
        return os.path.join(R, name)

    def ld_of(leg):
        return L(f"evleg_{leg['name']}")

    def done(d):
        return os.path.isfile(os.path.join(d, "exitcode.txt"))

    legs = leg_table()
    by_name = {lg["name"]: lg for lg in legs}
    res = {"runs": R, "legs": {}, "pairs": {}, "verdicts": {}}

    # ---------------------------------------------------------------- VERIFY
    hdr("1. PER-LEG VERIFICATION (fp2_leg_verify checks, re-host posture lines)")
    print(f"{'leg':<20}{'kev':>4}{'rc':>3}{'wall':>7}{'prod':>5}{'cons':>5}{'wthl':>5}{'cold':>5}"
          f"{'RS8/9':>7}{'armed':>7}{'act':>5}{'inert':>6}{'cells':>7}{'s0mean':>7}"
          f"{'s0max':>6}{'flag':>5}{'q/d':>5}  products sha256[:12]")
    print("-" * 140)
    allprob = []
    V = {}
    for lg in legs:
        v = verify_leg(ld_of(lg), lg["arm"], lg["n"], lg["kev"])
        V[lg["name"]] = v
        res["legs"][lg["name"]] = {"verify": v, "leg": lg}
        t = v.get("rehost") or {}
        print(f"{lg['name']:<20}{lg['kev']:>4}{v.get('rc', '?'):>3}{(v.get('wall_s') or 0):>7.0f}"
              f"{v.get('product_epochs', 0):>5}{v.get('consumer_rows', 0):>5}"
              f"{(v.get('run_end') or {}).get('withheld', '?'):>5}"
              f"{'YES' if v.get('cold_start') else 'no':>5}"
              f"{'/'.join('Y' if v.get('rs_restores', {}).get(k) else '.' for k in RS_LINES):>7}"
              f"{'Y' if v.get('armed_line') else '.':>7}"
              f"{'ARM' if v.get('act_armed_line') else ('off' if v.get('act_off_line') else '-'):>5}"
              f"{'Y' if v.get('inert_line') else '.':>6}"
              f"{str(t.get('pool_cells', '-')):>7}{str(t.get('sigma0_mean', '-')):>7}"
              f"{str(t.get('sigma0_max', '-')):>6}"
              f"{str(t.get('sats_flagged', '-')) + '/' + str(t.get('arcs_flagged', '-')):>5}"
              f"{str(t.get('refloats_queued', '-')) + '/' + str(t.get('refloats_drained', '-')):>5}"
              f"  {(v.get('products_sha256') or '')[:12]}")
        for p in v["problems"]:
            allprob.append(f"{lg['name']}: {p}")
    cert = verify_leg(L(CERT_NULL), "off", None, 11)
    res["cert_null"] = cert
    print(f"{'(cert_null_v8)':<20}{'':>4}{cert.get('rc', '?'):>3}{(cert.get('wall_s') or 0):>7.0f}"
          f"{cert.get('product_epochs', 0):>5}{cert.get('consumer_rows', 0):>5}"
          f"{'':>5}{'':>5}{'':>7}{'':>7}{'':>5}{'Y' if cert.get('inert_line') else '.':>6}"
          f"{'':>7}{'':>7}{'':>6}{'':>5}{'':>5}  {(cert.get('products_sha256') or '')[:12]}")

    print("\n  config pair diffs (flattened leg.json; allowed = the knob(s) + the two per-leg paths):")

    def pdiff(lbl, x, y, knobs):
        pd = pair_diff(x + "/leg.json", y + "/leg.json", knobs)
        res["pairs"][lbl] = pd
        if "err" in pd:
            print(f"    {lbl}: {pd['err']}")
            return
        print(f"    {'OK ' if not pd['unexpected'] else 'BAD'} {lbl}: {pd['n_diffs']} key(s): "
              f"{pd['keys']}  (inject keys {pd['inject_keys']}, identical)")
        for u in pd["unexpected"]:
            allprob.append(f"{lbl}: UNEXPECTED diff {u}")

    pdiff("cert_null_v8 -> null_screen", L(CERT_NULL), L(NULL_SCREEN), {KNOB_SCREEN})
    for n in NS:
        pdiff(f"n{n}_off -> n{n}_screen", L(f"evleg_raimp_n{n}_off"), L(f"evleg_raimp_n{n}_screen"), {KNOB_SCREEN})
        pdiff(f"n{n}_screen -> n{n}_act", L(f"evleg_raimp_n{n}_screen"), L(f"evleg_raimp_n{n}_act"), {KNOB_ACT})
    for e in EXT_AT_EPS:
        pdiff(f"n1e{e}_off -> n1e{e}_act", L(f"evleg_raimp_n1e{e}_off"), L(f"evleg_raimp_n1e{e}_act"),
              {KNOB_SCREEN, KNOB_ACT})
    for lg in legs:
        if lg["arm"] != "off":
            continue
        lj = ld_of(lg) + "/leg.json"
        if os.path.isfile(lj):
            inj = json.load(open(lj)).get("network", {}).get("inject")
            ok = inj == expected_inject(lg["n"], tow_of(lg["kev"]))
            print(f"    {'OK ' if ok else 'BAD'} {lg['name']}: leg.json inject == E3 shape + L2W, delta {lg['n']}, "
                  f"tow {tow_of(lg['kev'])!r} (k {lg['kev']})")
            if not ok:
                allprob.append(f"{lg['name']}: inject block != expected: {json.dumps(inj)[:200]}")

    print("\n  re-host run-end lines (the ON-path telemetry, invariant #4) and the injector's own count:")
    for lg in legs:
        v = V[lg["name"]]
        for ln in (v.get("rehost_lines") or [])[:6]:
            m = re.search(r"\] \[\w+\] (.*)", ln)
            print(f"    {lg['name']:<18} {(m.group(1) if m else ln)[:170]}")
        for ln in v.get("inject_lines") or []:
            m = re.search(r"\] \[\w+\] (.*)", ln)
            print(f"    {lg['name']:<18} {(m.group(1) if m else ln)[:170]}")

    # ---------------------------------------------------------------- CENSUS
    hdr("2. DETECTOR CENSUS at (GOPE00CZE, G24), k in [kev-2, kev+4] (the brief's 9..15 for kev = 11)")
    C = {}
    print(f"{'leg':<18}{'kev':>4}{'IFTD flg':>9}{'T1 ev':>7}{'T0 MW/BS':>9}{'LLI':>5}{'MW DET':>8}"
          f"{'D5 arc':>8}{'D5 sta':>8}{'D5 sat':>8}{'D5 flags/retaps':>17}{'D5 supp/trips':>15}"
          f"{'IF-TD margin@kev':>18}")
    print("-" * 140)
    for lg in legs:
        ld = ld_of(lg)
        if not done(ld):
            continue
        c = census(ld, lg["kev"] - 2, lg["kev"] + 4)
        c["p0prime"] = census(ld, lg["kev"] - 1, lg["kev"] + 5)["n_detector_rows"] if lg["n"] else None
        c["margin"] = if_td_margin(c, lg["kev"])
        C[lg["name"]] = c
        res["legs"][lg["name"]]["census"] = c
        v = V[lg["name"]]
        mg = c["margin"]
        print(f"{lg['name']:<18}{lg['kev']:>4}{len(c['if_td_flagged']):>9}{len(c['t1_rows']):>7}"
              f"{len(c['t0_rows']):>9}{len(c['lli_asserted']):>5}{len(c['mw_det_lines']):>8}"
              f"{len(c['d5_arc']):>8}{len(c['d5_station_other']):>8}{len(c['d5_sat_other']):>8}"
              f"{str(v.get('d5_flags_retaps')):>17}{str(v.get('d5_suppressed_of_trips')):>15}"
              f"{(fnum(mg['margin_m'], 4, 1).strip() + (' F' if mg['flagged'] else ' -')) if mg else 'n/a':>18}")
    cN = census(L(CERT_NULL), 9, 15)
    res["cert_null"]["census"] = cN
    print(f"{'(cert_null_v8)':<18}{'':>4}{len(cN['if_td_flagged']):>9}{len(cN['t1_rows']):>7}"
          f"{len(cN['t0_rows']):>9}{len(cN['lli_asserted']):>5}{len(cN['mw_det_lines']):>8}"
          f"{len(cN['d5_arc']):>8}{len(cN['d5_station_other']):>8}{len(cN['d5_sat_other']):>8}"
          f"{str(cert.get('d5_flags_retaps')):>17}{str(cert.get('d5_suppressed_of_trips')):>15}")
    null_raw = {}
    for nm in (CERT_NULL, NULL_SCREEN):
        if done(L(nm)):
            null_raw[nm] = {x[0]: x for x in census(L(nm), 1, N_EPOCHS)["raw"]}
    print("\n  rows (k, ...) per leg — IF-TD chain rows are printed for every epoch of the window:")
    for lg in legs:
        c = C.get(lg["name"])
        if not c:
            continue
        print(f"\n  --- {lg['name']} (kev {lg['kev']}) ---")
        print("    IF-TD (k, pair, dd_m signed, abs_dev/thr [floor, s_eff], flagged, is_ref): "
              + "; ".join(f"{k}:{p} {dd:+.4f} {d:.4f}/{t:.4f} [{fl:.4f}, {se:.4f}] f={f} r={r}"
                          for k, p, d, t, f, r, dd, fl, se in c["if_td"]))
        print("    raw (k, pair, mw_m, gf_m, lli1, lli2):             "
              + "; ".join(f"{k}: mw {mw:.3f} gf {gf:.3f} lli {l1}{l2}" for k, p, mw, gf, l1, l2 in c["raw"]))
        nraw = null_raw.get(lg["null"], {})
        if nraw:
            print("    raw DELTA vs null (k: dmw_m, dgf_m):               "
                  + "; ".join(f"{k}: {mw - nraw[k][2]:+.4f} {gf - nraw[k][3]:+.4f}"
                              for k, p, mw, gf, l1, l2 in c["raw"] if k in nraw))
        for row in c["events_at_arc"]:
            print(f"    event k={row[0]:>2} {row[1]:<14}{row[2]:<16}{row[5]:<10} stat={row[3]!s:>10} "
                  f"thr={row[4]!s:>9}  {row[6]}")
        if c["mw_det_lines"]:
            print("    MW DET lines for G24 (station-blind): "
                  + "; ".join(f"k={k} lam={lam} dev={dev} WL" for k, lam, dev in c["mw_det_lines"]))
        for row in c["d5_arc"]:
            print(f"    D5 ARC  k={row[0]:>2} {row[1]} {row[2]} std_innov={row[3]:.2f} hold={row[4]} "
                  f"inj_prev={row[5]} raw_stepped={row[6]!r} v={row[7]} z_post={row[8]} warm={row[9]}")
        for row in c["d5_station_other"]:
            print(f"    D5 sta  k={row[0]:>2} {row[1]} {row[2]} std_innov={row[3]:.2f} raw_stepped={row[6]!r}")
        for row in c["d5_sat_other"]:
            print(f"    D5 sat  k={row[0]:>2} {row[1]} {row[2]} std_innov={row[3]:.2f} raw_stepped={row[6]!r}")
        st = c["status"]
        print("    epoch status k: " + "; ".join(
            f"{k}: s0={st[k]['sigma0']} d5 {st[k]['n_d5_flags']}/{st[k]['n_d5_retaps']} "
            f"births {st[k]['n_chord_births']} arc_refl {st[k]['n_arc_refloat']} "
            f"slip_refl {st[k]['n_slip_refloats']}" for k in sorted(st)))

    # ---------------------------------------------------------------- SIGMA0
    hdr("3. SIGMA0 — (a) pre-registered column network_epoch_status.csv sigma0, k = kev-6..kev+14")
    print("   ⚠ this column is the WITHIN-EPOCH tier's sigma0_adapt (StashStatus.cpp:530),"
          " pooled over R.pl_amb — empty on this lineage — not the re-hosted screen's statistic.")
    S = {}
    print(f"{'leg':<18}{'kev':>4}  " + "".join(f"{d:>+6}" for d in range(-6, 15)) + f"{'spread':>8}")
    rows_s = [(CERT_NULL, L(CERT_NULL), 11)] + [(lg["name"], ld_of(lg), lg["kev"]) for lg in legs]
    for name, ld, kev in rows_s:
        if not done(ld):
            continue
        s = sigma0_series(ld, kev - 6, kev + 14)
        S[name] = s
        vals = [s[k] for k in range(kev - 6, kev + 15) if k in s]
        spread = statistics.pstdev(vals) if len(vals) > 1 else 0.0
        print(f"{name:<18}{kev:>4}  " + "".join(f"{s.get(kev + d, float('nan')):>6.3f}" for d in range(-6, 15))
              + f"{spread:>8.4f}")
    res["sigma0_column"] = S

    hdr("3. SIGMA0 — (b) the RE-HOSTED sigma0 read off the emitted URA: median U ratio leg/twin, "
        "and the inverted per-epoch sigma0 (alpha 0.1)", "-")
    print("   twin = the same-draw OFF leg (identical solve until a refloat drains); null_screen's twin = cert_null_v8.")
    U = {}
    for lg in legs:
        if not lg.get("twin"):
            continue
        ld, td = ld_of(lg), L(lg["twin"])
        if not (done(ld) and done(td)):
            continue
        ratio, implied = ura_ratio_series(ld, td)
        U[lg["name"]] = {"ratio": {k: v[0] for k, v in ratio.items()},
                         "spread_per_epoch_max": max((v[1] for v in ratio.values()), default=None),
                         "implied": implied}
        above = sorted(k for k, v in ratio.items() if v[0] > 1.0 + 1e-12)
        kev = lg["kev"]
        print(f"  {lg['name']:<18} ratio>1 at k={above[:12]}{'...' if len(above) > 12 else ''}  "
              f"max ratio {max((v[0] for v in ratio.values()), default=None)}  "
              f"per-epoch spread max {U[lg['name']]['spread_per_epoch_max']}")
        if ratio:
            print(f"      ratio k={kev - 6}..{kev + 14}: "
                  + " ".join(f"{ratio[k][0]:.4f}" if k in ratio else "  n/a" for k in range(kev - 6, kev + 15)))
        if implied:
            print("      implied sigma0: " + "; ".join(f"k={k}: {v:.3f}" for k, v in sorted(implied.items())[:20]))
    res["ura"] = U

    hdr("3. SIGMA0 — (c) flagged-cell traces (network_raim.csv / network_raim_attrib.csv, k kev-2..kev+4 or the arc)"
        " and the coarse post-fit read (network_pf_keyed.csv STA GOPE / SAT G24, phase pool)", "-")
    for lg in legs:
        ld = ld_of(lg)
        if not done(ld):
            continue
        kev = lg["kev"]
        rr = raim_rows(ld, kev - 2, kev + 4)
        pk = pf_keyed(ld, kev - 2, kev + 4)
        res["legs"][lg["name"]]["raim_rows"] = rr
        res["legs"][lg["name"]]["pf_keyed"] = pk
        print(f"  {lg['name']:<18} raim rows {len(rr['raim'])}  attrib rows {len(rr['attrib'])}")
        for row in rr["raim"][:12]:
            print(f"      raim   k={row[0]} {row[1]} {row[2]} s_raw={row[3]} s_norm={row[4]} sigma0={row[5]} "
                  f"spiked={row[6]} n_sta={row[7]} refloated={row[8]}")
        acted = [row for row in rr["attrib"] if row[8] not in ("0", "")]
        print(f"      attrib rows acted={len(acted)}; rows naming {STATION} or {SAT}: "
              + "; ".join(f"k={row[0]} {row[1]} {row[2]} prom={row[4]} acted={row[8]}" for row in rr["attrib"]
                          if STATION in row[2] or SAT in row[2]) )
        print("      pf STA GOPE (k: n, s0, rms, max): " + "; ".join(
            f"{k}: {v[0]} {v[1]:.4f} {v[2]:.4f} {v[3]:.4f}" for k, v in sorted(pk["STA"].items())))
        print("      pf SAT G24  (k: n, s0, rms, max): " + "; ".join(
            f"{k}: {v[0]} {v[1]:.4f} {v[2]:.4f} {v[3]:.4f}" for k, v in sorted(pk["SAT"].items())))
    pkN = pf_keyed(L(CERT_NULL), 1, N_EPOCHS)
    res["cert_null"]["pf_keyed"] = pkN
    print("  (cert_null_v8)     pf STA GOPE k 9..25: " + "; ".join(
        f"{k}: {v[0]} {v[1]:.4f} {v[2]:.4f} {v[3]:.4f}" for k, v in sorted(pkN["STA"].items()) if 9 <= k <= 25))

    # ---------------------------------------------------------------- T3/T4
    hdr("4. T3 / T4 EVENT ROWS with identity (every leg; * = names GOPE00CZE/G24 in k kev-1..kev+4)")
    for lg in legs:
        ld = ld_of(lg)
        if not done(ld):
            continue
        rows = t34_rows(ld)
        kev = lg["kev"]
        hits = [r for r in rows if r[3] == SAT and r[4] == STATION and r[0] is not None
                and kev - 1 <= r[0] <= kev + 4]
        res["legs"][lg["name"]]["t34"] = rows
        res["legs"][lg["name"]]["t34_arc_hits"] = hits
        print(f"  {lg['name']:<18} T3/T4 rows {len(rows)}   naming the arc in k {kev - 1}..{kev + 4}: {len(hits)}")
        for r in rows[:15]:
            mark = "*" if r in hits else " "
            print(f"     {mark} k={r[0]} {r[1]}/{r[2]} sat={r[3]} sta={r[4]} stat={r[5]} thr={r[6]} {r[7]} {r[8]}")
    rowsN = t34_rows(L(CERT_NULL))
    res["cert_null"]["t34"] = rowsN
    print(f"  {'(cert_null_v8)':<18} T3/T4 rows {len(rowsN)}")

    # ---------------------------------------------------------------- DRAIN
    hdr("5. ACT LEGS — the actuator's run-end counters and the drain identity")
    for lg in legs:
        if lg["arm"] != "act":
            continue
        v = V.get(lg["name"]) or {}
        t = v.get("rehost") or {}
        if not t:
            continue
        kev = lg["kev"]
        print(f"  {lg['name']:<18} refloats queued {t.get('refloats_queued')} / drained {t.get('refloats_drained')}   "
              f"sats flagged {t.get('sats_flagged')}  arcs flagged {t.get('arcs_flagged')}")
        for ln in v.get("raim_attrib_lines") or []:
            m = re.search(r"\] \[\w+\] (.*)", ln)
            print(f"      {(m.group(1) if m else ln)[:220]}")
        st = (C.get(lg["name"]) or {}).get("status") or {}
        twin = os.path.basename(lg["twin"] or "").replace("evleg_", "")
        so = (C.get(twin) or {}).get("status") or {}
        print(f"      chord births / arc refloats / slip refloats k {kev - 2}..{kev + 4}  act: " + "; ".join(
            f"{k}: {st[k]['n_chord_births']}/{st[k]['n_arc_refloat']}/{st[k]['n_slip_refloats']}" for k in sorted(st))
              + f"   {twin}: " + "; ".join(
            f"{k}: {so[k]['n_chord_births']}/{so[k]['n_arc_refloat']}/{so[k]['n_slip_refloats']}" for k in sorted(so)))

    # ----------------------------------------------------------------- WIRE
    hdr("6. WIRE — G24 CLK / PB, event MINUS null at the same posture (off vs cert_null_v8; "
        "screen/act vs raimp_null_screen)")
    W = {}
    print(f"{'leg':<18}{'window (k)':>12}{'CLK max|d|':>12}{'at k':>6}{'CLK mean':>10}{'PB max|d|':>11}"
          f"{'per-signal PB (max|d| @k)':<44}{'worst other':>13}  key @k")
    print("-" * 160)
    for lg in legs:
        ld, nd = ld_of(lg), L(lg["null"])
        if not (done(ld) and done(nd)):
            continue
        kev = lg["kev"]
        w1 = wire(ld, nd, kev - 1, min(kev + 39, N_EPOCHS))          # the brief: +10..+50
        w2 = wire(ld, nd, kev, min(kev + 45, N_EPOCHS))              # fp1 G-C: dk 0..45
        w3 = wire(ld, nd, kev + 9, min(kev + 39, N_EPOCHS))          # matches the P2 window, +20..+50
        bl = blast(ld, nd, kev)
        W[lg["name"]] = {"brief": w1, "gc": w2, "p2": w3, "blast_kev": bl}
        res["legs"][lg["name"]]["wire"] = {kk: {k: v for k, v in ww.items() if k != "clk_series"}
                                           for kk, ww in (("brief", w1), ("gc", w2), ("p2", w3))}
        res["legs"][lg["name"]]["wire"]["blast_kev"] = bl
        for lab, w in ((f"{w1['window'][0]}..{w1['window'][1]}", w1), (f"{w2['window'][0]}..{w2['window'][1]}", w2),
                       (f"{w3['window'][0]}..{w3['window'][1]}", w3)):
            pbs = ", ".join(f"{s} {v:.4f}@{k}" for s, (v, k) in sorted(w["pb"].items()))
            print(f"{lg['name']:<18}{lab:>12}{fnum(w['clk'], 5, 12)}{str(w['clk_at']):>6}{fnum(w['clk_mean'], 5, 10)}"
                  f"{fnum(w['pb_max'], 5, 11)} {pbs:<43}{fnum(w['other']['v'], 4, 13)}  {w['other']['key']} @{w['other']['at']}")
        print(f"{'':<18}{'blast k=' + str(kev):>12}  {len(bl)} key(s) > 0.5 m: "
              + (", ".join(f"{k} {v:.3f}" for v, k in bl[:6]) or "(none)"))

    # --------------------------------------------------------------- HASHES
    hdr("7. PRODUCTS.TXT HASHES and record-kind identity")
    H = {CERT_NULL: cert.get("products_sha256")}
    for lg in legs:
        H[lg["name"]] = V[lg["name"]].get("products_sha256")
        print(f"  {lg['name']:<20} {H[lg['name']]}")
    print(f"  {'(cert_null_v8)':<20} {H[CERT_NULL]}")
    res["hashes"] = H
    ident = {}
    print("\n  identities:")

    def check(lbl, x, y):
        if H.get(x) and H.get(y):
            same = H[x] == H[y]
            ident[lbl] = same
            print(f"    {'SAME' if same else 'DIFF'}  {lbl}")
            if not same:
                xd = L(x if x == CERT_NULL else f"evleg_{x}") + "/products.txt"
                yd = L(y if y == CERT_NULL else f"evleg_{y}") + "/products.txt"
                rd = records_diff(xd, yd)
                res.setdefault("records_diff", {})[lbl] = rd
                for kind, d in rd.items():
                    if not d["identical"]:
                        print(f"          {kind}: n {d['n_a']} vs {d['n_b']}, first diff at k={d['first']['k']}: "
                              f"{d['first']['a'][:70]} | {d['first']['b'][:70]}")
    check("raimp_null_screen == cert_null_v8", "raimp_null_screen", CERT_NULL)
    for n in NS:
        check(f"raimp_n{n}_screen == raimp_n{n}_off", f"raimp_n{n}_screen", f"raimp_n{n}_off")
        check(f"raimp_n{n}_act vs raimp_n{n}_screen", f"raimp_n{n}_act", f"raimp_n{n}_screen")
        check(f"raimp_n{n}_off vs cert_null_v8 (the event itself)", f"raimp_n{n}_off", CERT_NULL)
    for e in EXT_AT_EPS:
        check(f"raimp_n1e{e}_act vs raimp_n1e{e}_off", f"raimp_n1e{e}_act", f"raimp_n1e{e}_off")
        check(f"raimp_n1e{e}_off vs cert_null_v8 (the event itself)", f"raimp_n1e{e}_off", CERT_NULL)
    res["identities"] = ident

    # ------------------------------------------------------------- CONSUMER
    hdr("8. CONSUMER (closed_loop_trace ENU, event - null, de-biased) — REPORTED, never gated; "
        "and the declared wire discontinuity load")
    for lg in legs:
        ld, nd = ld_of(lg), L(lg["null"])
        if not (done(ld) and done(nd)):
            continue
        kev = lg["kev"]
        post = min(50, N_EPOCHS - kev)
        co = consumer(ld, nd, tow_of(kev), post)
        dd = disc_delta(ld, nd, kev - 2, N_EPOCHS, kev)
        res["legs"][lg["name"]]["consumer"] = co
        res["legs"][lg["name"]]["disc_delta"] = dd
        if "err" in co:
            print(f"  {lg['name']:<18} n/a ({co['err']})")
        else:
            print(f"  {lg['name']:<18} max|d3D| {co['max3d']:.4f} m @dk +{co['max3d_at_dk']}   "
                  f"mean|d3D| [dk +30,+{post}] {fnum(co['mean3d_30_post'], 4, 1).strip()} m   (n {co['n']})")
            print(f"{'':<20}|d3D| at dk 0/1/2/5/10/20/30/40/50: "
                  + " ".join(fnum(co['d3_at'][dk], 3, 1).strip() for dk in (0, 1, 2, 5, 10, 20, 30, 40, 50))
                  + "   leg's OWN |3D| at dk 0/5/20/50: "
                  + " ".join(fnum(co['abs3_at'][dk], 2, 1).strip() for dk in (0, 5, 20, 50))
                  + f"   rt resets ev-null {co['rt_resets_ev_minus_null']}")
        print(f"{'':<20}wire disc counters differing from the null (C clock_disc / P phase_disc, k {kev - 2}..{N_EPOCHS}): "
              f"{dd['n_differing']} record-epoch(s), first at k={dd['first_k']}; per-epoch increments leg/null "
              + " ".join(f"k{k}:{a_}/{b_}" for k, (a_, b_) in sorted(dd['increments'].items())))

    # ------------------------------------------------------------- VERDICTS
    hdr("9. PRE-REGISTERED BARS P0..P3 (the numbers; the manager decides)")
    ver = {}
    # ---- the P0 bar on the (n, n, n) matrix at kev = 11
    p0 = {}
    for n in NS:
        c = C.get(f"raimp_n{n}_off")
        if c:
            p0[n] = {"detector_rows": c["n_detector_rows"], "if_td_flagged": len(c["if_td_flagged"]),
                     "t1": len(c["t1_rows"]), "t0": len(c["t0_rows"]), "lli": len(c["lli_asserted"]),
                     "d5_arc_rows": len(c["d5_arc"]), "margin": c["margin"]}
    blind = [n for n in NS if n in p0 and p0[n]["detector_rows"] == 0]
    ver["P0"] = {"per_n": p0, "blind_ns": blind, "smallest_blind_n": (min(blind) if blind else None)}
    print("  P0 (blindness) — OFF arm detector rows at GOPE/G24 in k 9..15 (IF-TD flagged + T1 + T0 MW/band-scope + LLI):")
    for n in NS:
        if n in p0:
            mg = p0[n]["margin"] or {}
            print(f"     n={n}: {p0[n]['detector_rows']} rows  (IF-TD flagged {p0[n]['if_td_flagged']}, "
                  f"T1 ev {p0[n]['t1']}, T0 ev {p0[n]['t0']}, LLI {p0[n]['lli']}; D5 rows on the arc "
                  f"{p0[n]['d5_arc_rows']} [reported, not a detector]); IF-TD at k=11: dd "
                  f"{fnum(mg.get('dd_m'), 4, 1).strip()} thr {fnum(mg.get('thr_m'), 4, 1).strip()} margin "
                  f"{fnum(mg.get('margin_m'), 4, 1).strip()} m")
    if blind:
        print(f"     -> smallest blind n = {min(blind)}  (blind set {blind})")
    else:
        print("     -> NO blind n at kev = 11: every OFF arm was caught by the (1,2) IF-TD chain")
    # the floor + the predicted blind epochs for n = 1 from the null chain's own series
    ifn = csv_rows(os.path.join(L(CERT_NULL), "logs", "network_if_slip.csv")) or []
    chain = {kleg(r["tow"]): (float(r["dd_m"]), float(r["thr_m"]), float(r["floor_m"]))
             for r in ifn if r["station"] == STATION and r["sat"] == SAT and r["sig1"] == "L1C"}
    lam_if = chain[11][2] if 11 in chain else None
    pred = {}
    for n in NS:
        pred[n] = {k: round(abs(dd + n * lam_if) - thr, 5) for k, (dd, thr, fl) in sorted(chain.items())
                   if lam_if and 2 <= k <= 56}
    blind_k = {n: [k for k, m in pred[n].items() if m <= 0.0] for n in NS}
    ver["P0"]["if_td_floor_m"] = lam_if
    ver["P0"]["null_chain_dd"] = {k: v[0] for k, v in chain.items()}
    ver["P0"]["predicted_margin_n1"] = pred.get(1)
    ver["P0"]["if_td_blind_event_epochs"] = blind_k
    print(f"     (1,2) IF-TD bar at GOPE/G24 = its floor {lam_if} m = floor_frac 1.0 x lambda_IF(1,1); the (n,n,n) step "
          f"is n x that, so n = 1 sits AT the bar: caught iff the null chain's own dd at the event epoch has the step's sign.")
    for n in NS:
        print(f"     n={n}: event epochs where the null chain predicts |dd + n*lambda| <= thr (IF-TD-blind): "
              f"{blind_k[n] if blind_k[n] else 'none'}")
    if pred.get(1):
        print("     n=1 predicted margin |null_dd + lambda| - thr per event epoch k (m): "
              + " ".join(f"{k}:{m:+.4f}" for k, m in pred[1].items() if 5 <= k <= 30))

    # ---- the P0' extension: the draws
    hdr("9'. P0' EXTENSION — n = 1 re-drawn at event epochs k = 15 / 19 / 23 (--at-ep 14 / 18 / 22), "
        "graded on the FIRST draw whose OFF arm has zero detector rows in [kev-1, kev+5]", "-")
    draws = {}
    for e in EXT_AT_EPS:
        kev = e + 1
        c = C.get(f"raimp_n1e{e}_off")
        if not c:
            draws[e] = {"kev": kev, "missing": True}
            print(f"     at_ep {e} (k {kev}): OFF leg missing / unfinished")
            continue
        mg = c["margin"] or {}
        draws[e] = {"kev": kev, "detector_rows_p0prime": c["p0prime"], "detector_rows_k-2..+4": c["n_detector_rows"],
                    "if_td_flagged": len(c["if_td_flagged"]), "t1": len(c["t1_rows"]), "t0": len(c["t0_rows"]),
                    "lli": len(c["lli_asserted"]), "d5_arc_rows": len(c["d5_arc"]), "margin": mg,
                    "predicted_margin": (pred.get(1) or {}).get(kev),
                    "blind": c["p0prime"] == 0}
        d = draws[e]
        print(f"     at_ep {e} (k {kev}): OFF detector rows in [{kev - 1},{kev + 5}] = {d['detector_rows_p0prime']} "
              f"(IF-TD flagged {d['if_td_flagged']}, T1 {d['t1']}, T0 {d['t0']}, LLI {d['lli']}; D5 arc rows {d['d5_arc_rows']}); "
              f"IF-TD at k={kev}: dd {fnum(mg.get('dd_m'), 4, 1).strip()} thr {fnum(mg.get('thr_m'), 4, 1).strip()} "
              f"margin {fnum(mg.get('margin_m'), 4, 1).strip()} m (predicted from the null chain "
              f"{fnum(d['predicted_margin'], 4, 1).strip()})  ->  {'BLIND' if d['blind'] else 'CAUGHT'}")
    blind_draws = [e for e in EXT_AT_EPS if draws.get(e, {}).get("blind")]
    graded = blind_draws[0] if blind_draws else None
    ver["P0prime"] = {"draws": draws, "blind_draws": blind_draws, "graded_at_ep": graded}
    unfinished = [e for e in EXT_AT_EPS if draws.get(e, {}).get("missing")]
    if unfinished:
        print(f"     -> draws still unfinished: at_ep {unfinished} — P0' not decidable yet")
    elif graded is None:
        print("     -> every draw CAUGHT: the posture has no equal-count slip the chain misses at this "
              "noise level; RAIM's field opportunity on this lineage is empty by construction (per-draw margins above).")
    else:
        print(f"     -> first blind draw: at_ep {graded} (k {graded + 1}) — P1 / P2 / P3 graded on it below")

    # ---- the P1 bar: the screen's power, on the main matrix (screen arms, kev 11) and on the graded draw (act arm).
    def p1_read(name, kev):
        s = S.get(name) or {}
        null_s = S.get("raimp_null_screen") or {}
        nv = [null_s[k] for k in range(5, 26) if k in null_s]
        spread = statistics.pstdev(nv) if len(nv) > 1 else 0.0
        bar = 1.0 + 3.0 * spread
        s3 = [s[k] for k in (kev - 1, kev, kev + 1) if k in s]
        hits = (res["legs"].get(name) or {}).get("t34_arc_hits") or []
        u = U.get(name) or {}
        ur = u.get("ratio") or {}
        u4 = {k: ur[k] for k in (kev - 1, kev, kev + 1, kev + 2) if k in ur}
        imp = {k: v for k, v in (u.get("implied") or {}).items() if kev - 1 <= k <= kev + 4}
        rise = bool(s3) and max(s3) > bar
        rise_ura = any(v > 1.0 + 1e-12 for v in u4.values())
        return {"sigma0_col": s3, "bar": bar, "null_spread": spread, "rise_column": rise,
                "ura_ratio": u4, "ura_implied_sigma0": imp, "rise_ura": rise_ura,
                "t34_arc_hits": len(hits), "PASS": (rise or rise_ura) and bool(hits),
                "rehost_run_end": {k: (V.get(name, {}).get("rehost") or {}).get(k) for k in
                                   ("sigma0_n", "sigma0_mean", "sigma0_max", "sats_flagged", "arcs_flagged")}}
    p1 = {}
    for n in NS:
        if f"raimp_n{n}_screen" in S:
            p1[f"n{n}_screen"] = p1_read(f"raimp_n{n}_screen", 11)
    for e in EXT_AT_EPS:
        if f"raimp_n1e{e}_act" in S:
            p1[f"n1e{e}_act"] = p1_read(f"raimp_n1e{e}_act", e + 1)
    ver["P1"] = {"per_leg": p1, "graded": (f"n1e{graded}_act" if graded is not None else
                                            (f"n{min(blind)}_screen" if blind else None))}
    print("\n  P1 (power) — needs σ̂0 at kev-1..kev+1 above 1.0 + 3 x the null's spread (the pre-registered column, "
          "OR the URA-implied re-hosted σ̂0 leaving its clip) AND a T3/T4 row naming GOPE/G24 in kev-1..kev+4:")
    for key, q in p1.items():
        print(f"     {key:<12}: sigma0 col {q['sigma0_col']} (bar {q['bar']:.4f}) rise={q['rise_column']}; "
              f"URA ratio {q['ura_ratio']} rise={q['rise_ura']}; T3/T4 arc rows {q['t34_arc_hits']}; "
              f"run-end {q['rehost_run_end']}  ->  {'PASS' if q['PASS'] else 'FAIL'}"
              + ("   <- the graded leg" if key == ver["P1"]["graded"] else ""))
    # ---- the P2 bar: actuation (only where P1 passed)
    p2 = {}
    for lg in legs:
        if lg["arm"] != "act":
            continue
        t = (V.get(lg["name"]) or {}).get("rehost") or {}
        if not t:
            continue
        kev = lg["kev"]
        hits = (res["legs"].get(lg["name"]) or {}).get("t34_arc_hits") or []
        wa = (W.get(lg["name"]) or {}).get("p2") or {}
        # the comparator: the screen-only arm on the main matrix; the OFF arm (vs cert_null) on a draw
        comp_name = (f"raimp_n{lg['n']}_screen" if lg["group"] == "main" else lg["twin"].replace("evleg_", ""))
        wc = (W.get(comp_name) or {}).get("p2") or {}
        below = (wa.get("clk_mean") is not None and wc.get("clk_mean") is not None
                 and wa["clk_mean"] < wc["clk_mean"])
        key = lg["name"].replace("raimp_", "")
        p1key = (f"n{lg['n']}_screen" if lg["group"] == "main" else key)
        p2[key] = {"refloats_queued": t.get("refloats_queued"), "refloats_drained": t.get("refloats_drained"),
                   "arc_in_t34": len(hits), "act_clk_mean": wa.get("clk_mean"), "act_clk_max": wa.get("clk"),
                   "comparator": comp_name, "comp_clk_mean": wc.get("clk_mean"), "comp_clk_max": wc.get("clk"),
                   "window": wa.get("window"), "act_below_comparator": below,
                   "PASS": bool(t.get("refloats_queued")) and bool(hits) and below,
                   "applicable": bool((p1.get(p1key) or {}).get("PASS"))}
    ver["P2"] = p2
    print("\n  P2 (actuation; graded only where P1 PASSED) — queued >= 1, the arc in the drain with identity, the act "
          "arm's G24 wire error (mean |dCLK| over kev+9..kev+39 vs its null) below the comparator's (screen-only "
          "arm on the matrix; the OFF arm vs cert_null on a draw):")
    for key, q in p2.items():
        print(f"     {key:<10}: queued/drained {q['refloats_queued']}/{q['refloats_drained']}, arc T4 rows {q['arc_in_t34']}, "
              f"CLK mean act {fnum(q['act_clk_mean'], 5, 1).strip()} vs {q['comparator'].replace('evleg_', '')} "
              f"{fnum(q['comp_clk_mean'], 5, 1).strip()} over k {q['window']}  ->  "
              f"{('PASS' if q['PASS'] else 'FAIL') if q['applicable'] else 'n/a (P1 not PASS)'}")
    # ---- the P3 bar: the cost of the miss (off arm vs cert_null over +10..+50)
    p3 = {}
    for lg in legs:
        if lg["arm"] != "off":
            continue
        w = (W.get(lg["name"]) or {}).get("brief")
        if w:
            p3[lg["name"].replace("raimp_", "")] = {
                "kev": lg["kev"], "window": w["window"], "clk_max": w["clk"], "clk_at": w["clk_at"],
                "clk_mean": w["clk_mean"], "pb": {s: v for s, (v, k) in w["pb"].items()},
                "pb_max": w["pb_max"], "worst_other": w["other"], "caught": (C[lg["name"]]["n_detector_rows"] > 0)}
    ver["P3"] = p3
    print("\n  P3 (the cost of the miss, reported) — OFF arm G24 CLK / PB max |d| vs cert_null_v8 over kev-1..kev+39; "
          "on a CAUGHT draw this is the cost of the detected-and-refloated slip, not of a miss:")
    for key, q in p3.items():
        print(f"     {key:<8} (k {q['window'][0]}..{q['window'][1]}, {'caught' if q['caught'] else 'MISSED'}): "
              f"CLK max {fnum(q['clk_max'], 5, 1).strip()} m @k{q['clk_at']} mean {fnum(q['clk_mean'], 5, 1).strip()}; PB "
              + ", ".join(f"{s} {v:.4f}" for s, v in sorted(q["pb"].items()))
              + f"; worst other {fnum(q['worst_other']['v'], 4, 1).strip()} ({q['worst_other']['key']} @k{q['worst_other']['at']})")
    res["verdicts"] = ver

    # ------------------------------------------------------------- WALL / SHA
    hdr("10. WALL TIMES, HARNESS SHA, PROBLEMS")
    for lg in legs:
        v = V[lg["name"]]
        print(f"  {lg['name']:<20} wall {fnum(v.get('wall_s'), 1, 7).strip():>7} s   rc {v.get('rc', '?')}")
    shas = set()
    res["batch_status"] = []
    for bsp in a.batch_status:
        if not os.path.isfile(bsp):
            print(f"\n  batch status {bsp}: missing")
            continue
        bs = json.load(open(bsp, encoding="utf-8"))
        rec = {k: v for k, v in bs.items() if k != "legs"}
        rec["legs"] = [{k: v for k, v in l.items() if k not in ("harness_stdout", "harness_stderr", "cmd")}
                       for l in bs.get("legs", [])]
        res["batch_status"].append(rec)
        print(f"\n  batch {os.path.basename(bsp)}: started {bs.get('started')} finished {bs.get('finished')} "
              f"done={bs.get('done')} extension={bs.get('extension')}")
        print(f"    exe sha256        {bs.get('exe_sha256')}")
        print(f"    harness sha START {bs.get('harness_sha256_start')}")
        print(f"    harness sha END   {bs.get('harness_sha256_end')}")
        s = {l.get("harness_sha256_before") for l in bs.get("legs", []) if l.get("harness_sha256_before")}
        s |= {l.get("harness_sha256_after") for l in bs.get("legs", []) if l.get("harness_sha256_after")}
        s |= {bs.get("harness_sha256_start"), bs.get("harness_sha256_end")} - {None}
        shas |= s
        print(f"    harness sha per leg (before/after): {sorted(s)}")
    if shas:
        print(f"  harness sha over every batch: {sorted(shas)}  "
              f"{'STABLE' if len(shas) == 1 else 'MOVED — regenerate every pair straddling the move'}")
    print("\n  PROBLEMS:")
    if allprob:
        for p in allprob:
            print("    " + p)
    else:
        print("    none — every leg warm, 61/61, the postures speak for themselves, every pair differs in the knob(s) only")
    res["problems"] = allprob
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=1, default=str)
        print(f"\njson -> {a.json}")
    return 1 if allprob else 0


if __name__ == "__main__":
    sys.exit(main())
