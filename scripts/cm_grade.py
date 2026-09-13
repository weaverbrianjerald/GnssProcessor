#!/usr/bin/env python3
"""Card CM grader (SPEC_event_harness s24.8, CODE-MODEL CONSISTENCY): the five reads per arm on the
LAST HOUR of a 2 h cold arm (k = 121..240, k = 1 at the first emitted epoch), the six-row table and
the frozen bars against the control.

    python scripts/cm_grade.py [--runs C:/GnssProcessor/runs] [--arms a0,a1,a2,a3,a4,a5]
        [--tow0 259200] [--k-lo 121] [--k-hi 240] [--skip-census] [--skip-attrib] [--json out.json]

Reads (the s24.8 numbering):
  (i)   logs/network_pf_keyed.csv kind STA: per-station MEDIAN over the hour of rms_code (and s0_code),
        then p50 / p90 / max across stations                                   bar: p50 < 1.0 m
  (ii)  logs/network_zwd.csv: per-station MEDIAN zwd_m over the hour; the count with |ZWD| > 0.5 m and
        the worst three                                                        bar: zero stations
  (iii) scripts/wire_vs_code_census.py on the arm's products.txt restricted to the hour (--window-run):
        within-arc rms of D_phase and D_code on DECLARED arcs (tracking arcs and the rotating-set-immune
        diffmedian common mode beside them) and the raw clock contrast LEVEL p50   bar: D_phase < 0.5 m
  (iv)  scripts/pf_rows_attrib.py --k-lo 121 --k-hi 240 --tow0 <tow0>: the share of the pooled code
        sum of squares the per-LOS MEANS carry, and the implied (noise-corrected) dispersive fraction
  (v)   availability (emitted / skipped / withheld, whole run and the hour from network_epoch_status.csv)
        and the run-end counters (prires cb / rxcb, block-Baarda G, the rx-CB carry line, the last W6-cb
        line, the P1-4 post-fit sigma0, the s24.6 row count)
Verification per arm from the producer's own voice: the knob's effect lines and the lines that differ from
A0's logs/gnss_config.log, any unknown-knob warning, the trace armed, and the snapshot / cold-start lines.

Bars (frozen): an arm NAMES the mechanism when (i) p50 < 1.0 m AND (ii) zero stations beyond 0.5 m AND
(iii) D_phase declared within-arc rms < 0.5 m — all three; any bar HALVED vs A0 = a contributor; nothing
halved = exonerated.  Availability must not fall more than 5 % below A0's (a pass by withholding is not a
pass).  The grader prints the numbers and the labels; the manager decides.
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable or "python"

ARM_KNOBS = {
    "a0": ("control (golden + trace)", []),
    "a1": ("network.carry_rx_code_bias: false", ["carry_rx_code_bias", "rx-CB carry", "rx-CB", "receiver code-bias"]),
    "a2": ("network.estimate_sat_code_bias: false + model_corrections.enable_code_phase_bias: true",
           ["(W6)", "enable_code_phase_bias", "code_phase_bias", ".BIA", "OSB", "estimate_sat_biases",
            "promote_sat_code_bias", "estimate_ifb"]),
    "a2x": ("EXT-CB: external_sat_code_bias true (+ estimate_sat_code_bias false, enable_code_phase_bias true)",
            ["(EXT-CB)", "(W6)", "external_sat_code_bias", "enable_code_phase_bias", ".BIA", "OSB", "estimate_ifb"]),
    "a3": ("network.estimate_iono_sh: false",
           ["estimate_iono_sh", "SH-iono", "SH-VTEC", "SH health", "sh_health", "consume_network_iono", "Iono-2"]),
    "a4": ("filter.process_noise.sat_code_bias_q: 1e-4", ["sat_code_bias_q", "sat_code_bias", "code-bias q", "code bias q"]),
    "a5": ("network.cb_kaula_prior: false", ["Kaula", "kaula"]),
}

RUN_END = [
    ("complete", r"Network producer complete: (\d+) epochs emitted, (\d+) skipped \((\d+) of those withheld by the observability gate, (\d+) by the A4[^)]*\)[^\d]*(\d+) synced"),
    ("prires_cb_s0", r"\(prior-residual census\) family 'cb': (\d+) row\(s\) over (\d+) epoch\(s\); \S+ = ([\d.]+)"),
    ("prires_cb_clean", r"'cb'/CLEAN: n = (\d+) .*?>4:(\d+) \(([\d.]+)%\)"),
    ("prires_rxcb_s0", r"\(prior-residual census\) family 'rxcb': (\d+) row\(s\) over (\d+) epoch\(s\); \S+ = ([\d.]+)"),
    ("prires_rxcb_clean", r"'rxcb'/CLEAN: n = (\d+) .*?>4:(\d+) \(([\d.]+)%\)"),
    ("bb_G_clean", r"\(block-Baarda census\) 'G'/CLEAN: n = (\d+) block\(s\) / (\d+) row\(s\) .*?geo\(t_B\) = (\d+(?:\.\d+)?)"),
    ("bb_G_sigma0", r"\(block-Baarda census\) 'G': sigma0 = (\d+(?:\.\d+)?)"),
    ("rxcb_carry", r"\(rx-CB carry\): (\d+) receiver code-bias prior\(s\) folded, (\d+) withheld by the .*?, (\d+) harvest\(s\); (\d+) carried at run end.*?(\d+) station\(s\) withheld > 90 %.*?: ?(.*)$"),
    ("zwd_trace", r"per-station ZWD trace ENABLED"),
    ("w6cb", r"Network\[W6-cb\]: nshared=(\d+), n_cb=(\d+), folded (\d+)"),
    ("p14", r"\(P1-4 post-fit \S+\): (\d+) emitted epoch\(s\) pooled .*?\S+0 ([\d.]+) all / ([\d.]+) code / ([\d.]+) phase"),
    ("rows", r"\(\S+24\.6 post-fit rows\): (\d+) SIGNED per-row post-fit residual\(s\) written to network_pf_rows.csv \((\d+) code \+ (\d+) phase"),
    ("sh_gate", r"\(Phase 2\.2 SH health gate\): (\d+) out-of-band EPOCH\(s\), (\d+) engagement\(s\), (\d+) emission\(s\) withheld"),
    ("admission", r"\(measurement admission RESPONSE\): (\d+) channel-epoch\(s\) withheld"),
    ("t2", r"\(T2 within-epoch responses\): (\d+) whole-sat refloat\(s\), (\d+) isolated-arc refloat\(s\), (\d+) station exclusion\(s\)[^;]*; (\d+) epoch\(s\) withheld at the A4"),
]


# ------------------------------------------------------------------ helpers
def pct(a, p):
    a = np.asarray(a, dtype=float)
    return float(np.percentile(a, p)) if a.size else float("nan")


def fmt(x, p=3, w=0):
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return "-".rjust(w) if w else "-"
    s = f"{x:.{p}f}"
    return s.rjust(w) if w else s


def read_text(path):
    if not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def read_lines(path):
    return read_text(path).splitlines()


def per_station_median(path, kind_filter, key_col, val_cols, t_lo, t_hi):
    """CSV -> {val_col: {station: median over rows with t_lo <= tow <= t_hi}}, plus the epoch set."""
    acc = {c: defaultdict(list) for c in val_cols}
    tows = set()
    if not os.path.isfile(path):
        return None, tows
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.DictReader(fh)
        for r in rd:
            try:
                tow = float(r["tow"])
            except (KeyError, ValueError):
                continue
            if tow < t_lo - 1e-3 or tow > t_hi + 1e-3:
                continue
            if kind_filter and r.get("kind") != kind_filter:
                continue
            tows.add(round(tow, 3))
            for c in val_cols:
                try:
                    acc[c][r[key_col]].append(float(r[c]))
                except (KeyError, ValueError):
                    pass
    out = {c: {s: float(np.median(v)) for s, v in acc[c].items() if v} for c in val_cols}
    return out, tows


def epoch_status(path, t_lo, t_hi):
    """network_epoch_status.csv -> disposition counts (whole run, window)."""
    whole, win = defaultdict(int), defaultdict(int)
    if not os.path.isfile(path):
        return whole, win
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                tow = float(r["tow"])
            except (KeyError, ValueError):
                continue
            d = r.get("disposition", "?")
            whole[d] += 1
            if t_lo - 1e-3 <= tow <= t_hi + 1e-3:
                win[d] += 1
    return whole, win


def strip_paths(line, arm):
    """Normalize the run-dir name inside logged paths so only real config differences survive the diff."""
    return re.sub(r"_cm_a\d(?:_[A-Za-z0-9]+)?", "_cm_ARM", line)


def config_log_diff(lines0, linesN, arm0, armN):
    """Lines of armN's gnss_config.log absent from arm0's (and vice versa), run-dir strings normalized."""
    s0 = set(strip_paths(l, arm0) for l in lines0)
    sN = set(strip_paths(l, armN) for l in linesN)
    only_n = [l for l in linesN if strip_paths(l, armN) not in s0]
    only_0 = [l for l in lines0 if strip_paths(l, arm0) not in sN]
    return only_n, only_0


def short(line, n=260):
    line = re.sub(r"^\[[^\]]*\]\s*", "", line.strip())
    return line if len(line) <= n else line[:n] + " …"


def run_end_counters(stdout_lines):
    out = {}
    for name, rx in RUN_END:
        last = None
        for l in stdout_lines:
            m = re.search(rx, l)
            if m:
                last = m
        out[name] = list(last.groups()) if last else None
    return out


# ------------------------------------------------------------------ census / attrib
def census(armdir, runs_root, t_lo, n_ep, force=False):
    """scripts/wire_vs_code_census.py on the arm restricted to the hour; returns the hour-1 numbers."""
    label = os.path.basename(armdir.rstrip("/\\"))
    out_dir = os.path.join(armdir, "census")
    summ = os.path.join(out_dir, f"win_{label}_summary.json")
    seed_json = os.path.join(armdir, "seed.json")
    if not os.path.isfile(seed_json):
        # the census reads <seed>/seed.json for the CODE product identity (files.internal_data_root +
        # data_download.{year,doy,product_source,product_type}); the arm's own config carries the same facts
        for cand in ("arm.json", "leg.json", "smoke.json"):
            if os.path.isfile(os.path.join(armdir, cand)):
                shutil.copyfile(os.path.join(armdir, cand), seed_json)
                break
    if force or not os.path.isfile(summ):
        cmd = [PY, os.path.join(REPO, "scripts", "wire_vs_code_census.py"), "--project", os.path.dirname(runs_root.rstrip("/\\")),
               "--seed", os.path.relpath(armdir, os.path.dirname(runs_root.rstrip("/\\"))).replace("\\", "/"),
               "--run", os.path.relpath(armdir, os.path.dirname(runs_root.rstrip("/\\"))).replace("\\", "/"),
               "--window-run", os.path.relpath(armdir, os.path.dirname(runs_root.rstrip("/\\"))).replace("\\", "/"),
               "--window-tow", str(int(t_lo)), "--window-n", str(int(n_ep)), "--out", out_dir]
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "census_stdout.txt"), "w", encoding="utf-8", errors="replace") as fh:
            rc = subprocess.call(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=REPO)
        if rc != 0 or not os.path.isfile(summ):
            print(f"  !! census failed for {label} (rc {rc}); see {out_dir}/census_stdout.txt")
            return None
    with open(summ, encoding="utf-8") as fh:
        S = json.load(fh)

    def tab(method, kind, tag, col):
        t = S.get(str((method, kind, tag)), {}).get("table")
        if not t:
            return float("nan")
        return float(t[0][col])   # the window run has one hour (hours=[0])

    def level(kind, col):
        t = S.get(str(("level", kind)))
        return float(t[0][col]) if t else float("nan")

    res = dict(
        n_epochs=S.get("n_epochs"), cnt=S.get("cnt"), missing_bia=S.get("missing_bia"),
        phase_decl_rms=tab("median", "phase", "declared", 4), phase_decl_p50=tab("median", "phase", "declared", 2),
        phase_track_rms=tab("median", "phase", "tracking", 4), phase_decl_rms_diffmedian=tab("diffmedian", "phase", "declared", 4),
        code_decl_rms=tab("median", "code", "declared", 4), code_track_rms=tab("median", "code", "tracking", 4),
        code_decl_rms_diffmedian=tab("diffmedian", "code", "declared", 4),
        clk_decl_rms=tab("median", "clk", "declared", 4), pb_decl_rms=tab("median", "pb", "declared", 4),
        cb_decl_rms=tab("median", "cb", "declared", 4),
        clk_level_p50=level("clk", 2), clk_level_p95=level("clk", 3),
        phase_level_p50=level("phase", 2), code_level_p50=level("code", 2),
        phase_decl_arcs=S.get(str(("median", "phase", "declared")), {}).get("n_arcs"),
        phase_decl_steps=S.get(str(("median", "phase", "declared")), {}).get("arcs_with_step"),
    )
    # the full-run census (hours 0 and 1) for the ignition-vs-settled read
    full = os.path.join(out_dir, f"{label}_summary.json")
    if os.path.isfile(full):
        with open(full, encoding="utf-8") as fh:
            F = json.load(fh)
        t = F.get(str(("median", "phase", "declared")), {}).get("table") or []
        res["phase_decl_rms_by_hour"] = [float(r[4]) for r in t]
        t = F.get(str(("median", "code", "declared")), {}).get("table") or []
        res["code_decl_rms_by_hour"] = [float(r[4]) for r in t]
        t = F.get(str(("level", "clk"))) or []
        res["clk_level_p50_by_hour"] = [float(r[2]) for r in t]
    return res


def attrib(armdir, tow0, k_lo, k_hi, force=False):
    js = os.path.join(armdir, "pf_rows_attrib_last_hour.json")
    if force or not os.path.isfile(js):
        cmd = [PY, os.path.join(REPO, "scripts", "pf_rows_attrib.py"), armdir, "--k-lo", str(k_lo), "--k-hi", str(k_hi),
               "--tow0", str(int(tow0)), "--json", js]
        with open(os.path.join(armdir, "pf_rows_attrib_last_hour.txt"), "w", encoding="utf-8", errors="replace") as fh:
            rc = subprocess.call(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=REPO)
        if rc != 0 or not os.path.isfile(js):
            print(f"  !! pf_rows_attrib failed for {armdir} (rc {rc}); see pf_rows_attrib_last_hour.txt")
            return None
    with open(js, encoding="utf-8") as fh:
        A = json.load(fh)
    i1 = A.get("item1_code") or {}
    i6 = A.get("item6_code") or {}
    p1 = A.get("item1_phase") or {}
    marg = i6.get("marginal") or {}
    nested = i6.get("nested") or []
    cand = i6.get("a2_candidates") or {}
    iono_cand = cand.get("ionosphere: implied dispersive (noise-corrected; shape-admitted only)")
    return dict(
        # the s24.6 "per-LOS dispersive" figure is the NESTED gamma-direction row (row 6 of the nested
        # sequence); the implied (shape-corrected) share is reported beside it with its admission flag
        disp_nested=(nested[5][1] if len(nested) > 5 else None),
        iono_shape_admitted=(iono_cand is not None and isinstance(iono_cand, (int, float)) and np.isfinite(iono_cand)),
        n_rows=i1.get("n_rows"), n_los=i1.get("n_los"), pooled_rms=i1.get("pooled_rms"),
        level_share=i1.get("share_means_pct"), abs_mean_p50=i1.get("abs_mean_p50"), abs_mean_p90=i1.get("abs_mean_p90"),
        sd_p50=i1.get("sd_p50"),
        disp_corrected=i6.get("implied_dispersive_corrected"), disp_raw=i6.get("implied_dispersive_raw"),
        between_over_common=i6.get("between_over_common"), pure_dispersive_ratio=i6.get("pure_dispersive_ratio"),
        gamma_share_3=(i6.get("shape") or {}).get("gamma_share_3"),
        sta_sig_const=marg.get("(station,signal) constants  [rx code bias row]"),
        sat_sig_const=marg.get("(satellite,signal) constants  [satellite constant row]"),
        los_common_nested=(nested[4][1] if len(nested) > 4 else None),
        a2_best=i6.get("a2_best"), a2_best_share=i6.get("a2_best_share"),
        phase_pooled_rms=p1.get("pooled_rms"),
        per_signal={k: v.get("rms") for k, v in (i1.get("per_signal") or {}).items()},
    )


# ------------------------------------------------------------------ per arm
def grade_arm(arm, armdir, a, ref_cfg_lines=None):
    base_arm = arm.split("_")[0]          # "a0_nozwd" grades as a0 (the trace_zwd byte-identity twin)
    R = dict(arm=arm, knob=ARM_KNOBS.get(base_arm, (arm, []))[0], dir=armdir)
    t_lo = a.tow0 + 30.0 * (a.k_lo - 1)
    t_hi = a.tow0 + 30.0 * (a.k_hi - 1)
    logs = os.path.join(armdir, "logs")
    ec = read_text(os.path.join(armdir, "exitcode.txt")).split()
    R["exit"] = ec[0] if ec else None
    R["wall_s"] = float(ec[1]) if len(ec) > 1 else None
    stdout = read_lines(os.path.join(armdir, "stdout.txt"))
    cfg = read_lines(os.path.join(logs, "gnss_config.log"))

    # -- verification from the producer's own voice
    V = {}
    # NB the loader is silent on unknown JSON keys (feedback_branch_exe_unknown_knob_silent); this catches only
    # an explicit refusal / unrecognized-value line, and the per-knob effect lines below are the real check
    V["unknown_knob"] = [short(l) for l in cfg if re.search(r"unknown (key|knob|option|field|config|value)|unrecogni[sz]ed|refus", l, re.I)]
    V["trace_rows"] = [short(l, 160) for l in cfg if "trace_postfit_rows" in l]
    V["trace_zwd"] = [short(l, 160) for l in cfg if "ZWD trace ENABLED" in l]
    V["first_epoch"] = None
    for l in stdout:
        m = re.search(r"emitted epoch 1 \u2014|emitted epoch 1 -|TOW (\d+\.\d) emitted epoch 1\b", l)
        if m:
            V["first_epoch"] = short(l, 200)
            break
    V["snapshot"] = [short(l, 240) for l in stdout
                     if re.search(r"snapshot", l, re.I) and re.search(r"cold|warm|resum|restor|no snapshot|not found|absent|missing|fresh|generation", l, re.I)
                     and "[gnss_config]" not in l][:6]
    V["knob_lines"] = []
    for tok in ARM_KNOBS.get(base_arm, (arm, []))[1]:
        for l in cfg:
            if tok in l and short(l) not in V["knob_lines"]:
                V["knob_lines"].append(short(l))
    V["knob_lines"] = V["knob_lines"][:12]
    if ref_cfg_lines is not None and arm != "a0":
        only_n, only_0 = config_log_diff(ref_cfg_lines, cfg, "a0", arm)
        V["cfg_only_in_arm"] = [short(l) for l in only_n][:20]
        V["cfg_only_in_a0"] = [short(l) for l in only_0][:20]
    R["verify"] = V

    # -- (v) availability + run-end counters
    C = run_end_counters(stdout)
    R["counters"] = C
    whole, win = epoch_status(os.path.join(logs, "network_epoch_status.csv"), t_lo, t_hi)
    R["status_whole"] = dict(whole)
    R["status_window"] = dict(win)
    if C.get("complete"):
        em, sk, wg, wa, sy = (int(x) for x in C["complete"])
        R["avail"] = dict(emitted=em, skipped=sk, withheld_gate=wg, withheld_a4=wa, synced=sy,
                          frac=em / sy if sy else float("nan"))
    else:
        R["avail"] = None

    # -- (i) per-station code residual rms, last hour
    med, tows = per_station_median(os.path.join(logs, "network_pf_keyed.csv"), "STA", "key",
                                   ["rms_code", "s0_code", "rms_phase"], t_lo, t_hi)
    if med:
        rc = np.array(list(med["rms_code"].values()))
        R["i"] = dict(n_sta=int(rc.size), n_epochs=len(tows), p50=pct(rc, 50), p90=pct(rc, 90), max=float(rc.max()) if rc.size else float("nan"),
                      s0_p50=pct(list(med["s0_code"].values()), 50), phase_p50=pct(list(med["rms_phase"].values()), 50),
                      worst=sorted(med["rms_code"].items(), key=lambda kv: -kv[1])[:3],
                      best=sorted(med["rms_code"].items(), key=lambda kv: kv[1])[:3],
                      per_station=med["rms_code"])
    else:
        R["i"] = None

    # -- (ii) ZWD, last hour
    medz, towz = per_station_median(os.path.join(logs, "network_zwd.csv"), None, "station", ["zwd_m"], t_lo, t_hi)
    if medz and medz["zwd_m"]:
        z = medz["zwd_m"]
        az = np.abs(np.array(list(z.values())))
        R["ii"] = dict(n_sta=len(z), n_epochs=len(towz), n_over_0p5=int((az > 0.5).sum()), n_over_5=int((az > 5.0).sum()),
                       abs_p50=pct(az, 50), abs_max=float(az.max()),
                       worst=sorted(z.items(), key=lambda kv: -abs(kv[1]))[:3], per_station=z)
    else:
        R["ii"] = None

    # -- (iii) wire-vs-CODE census on the hour
    R["iii"] = None if a.skip_census else census(armdir, a.runs, t_lo, a.k_hi - a.k_lo + 1, force=a.force)
    # -- (iv) per-LOS attribution on the hour
    R["iv"] = None if a.skip_attrib else attrib(armdir, a.tow0, a.k_lo, a.k_hi, force=a.force)
    return R


# ------------------------------------------------------------------ report
def bars(R, R0):
    i = R.get("i") or {}
    ii = R.get("ii") or {}
    iii = R.get("iii") or {}
    b1 = i.get("p50"); b2 = ii.get("n_over_0p5"); b3 = iii.get("phase_decl_rms")
    passes = dict(i=(b1 is not None and np.isfinite(b1) and b1 < 1.0),
                  ii=(b2 is not None and b2 == 0),
                  iii=(b3 is not None and np.isfinite(b3) and b3 < 0.5))
    halved = {}
    if R0 is not R:
        i0 = R0.get("i") or {}; ii0 = R0.get("ii") or {}; iii0 = R0.get("iii") or {}
        for name, v, v0 in (("i", b1, i0.get("p50")), ("ii", b2, ii0.get("n_over_0p5")), ("iii", b3, iii0.get("phase_decl_rms"))):
            halved[name] = (v is not None and v0 is not None and np.isfinite(float(v)) and np.isfinite(float(v0))
                            and v0 > 0 and v <= 0.5 * v0)
    av, av0 = R.get("avail"), R0.get("avail")
    avail_ok = None
    if av and av0 and np.isfinite(av["frac"]) and np.isfinite(av0["frac"]):
        avail_ok = av["frac"] >= av0["frac"] - 0.05
    names = all(passes.values())
    contributor = any(halved.values())
    label = "NAMES (all three bars)" if names else ("contributor (a bar halved vs A0)" if contributor else "exonerated (no bar moved)")
    if R is R0:
        label = "control"
    if avail_ok is False:
        label += " — AVAILABILITY FELL > 5 % vs A0 (a pass by withholding is not a pass)"
    return dict(passes=passes, halved=halved, avail_ok=avail_ok, label=label)


def print_report(results, a):
    order = [r["arm"] for r in results]
    R0 = results[0]
    print("\n" + "=" * 118)
    print(f"CARD CM — six cold 2 h arms, LAST HOUR k = {a.k_lo}..{a.k_hi} (tow {a.tow0 + 30 * (a.k_lo - 1):.0f}..{a.tow0 + 30 * (a.k_hi - 1):.0f})")
    print("=" * 118)
    for R in results:
        print(f"\n--- {R['arm']}: {R['knob']}   [{R['dir']}]  exit {R['exit']}  wall {fmt(R['wall_s'], 0)} s")
        V = R["verify"]
        print(f"  trace armed: {V['trace_rows'][0] if V['trace_rows'] else 'NO trace_postfit_rows LINE'}")
        print(f"  zwd trace: {V['trace_zwd'][0] if V.get('trace_zwd') else 'NO ZWD-trace LINE'}")
        print(f"  first emitted epoch: {V['first_epoch'] or 'NO epoch-1 LINE'}")
        print(f"  unknown-knob / refusal lines: {V['unknown_knob'] if V['unknown_knob'] else 'none'}")
        print(f"  snapshot / cold-start lines: " + (" | ".join(V["snapshot"]) if V["snapshot"] else "none matched"))
        if V.get("knob_lines"):
            print("  knob effect lines (gnss_config.log):")
            for l in V["knob_lines"]:
                print(f"    - {l}")
        if V.get("cfg_only_in_arm") is not None:
            print(f"  gnss_config.log lines only in this arm ({len(V['cfg_only_in_arm'])}):")
            for l in V["cfg_only_in_arm"]:
                print(f"    + {l}")
            print(f"  gnss_config.log lines only in a0 ({len(V['cfg_only_in_a0'])}):")
            for l in V["cfg_only_in_a0"]:
                print(f"    - {l}")
        av = R.get("avail")
        if av:
            print(f"  (v) availability: {av['emitted']} emitted / {av['skipped']} skipped ({av['withheld_gate']} gate, {av['withheld_a4']} A4) of {av['synced']} synced"
                  f" = {100 * av['frac']:.1f} %; hour dispositions {R['status_window']}; whole run {R['status_whole']}")
        C = R["counters"]
        def g(name, idx=None):
            v = C.get(name)
            if v is None:
                return "-"
            return v[idx] if idx is not None else v
        print(f"      prires cb: n {g('prires_cb_s0', 0)} rows, σ̂₀ {g('prires_cb_s0', 2)}, CLEAN n {g('prires_cb_clean', 0)} >4σ̂₀ {g('prires_cb_clean', 1)} ({g('prires_cb_clean', 2)} %)"
              f" | rxcb: σ̂₀ {g('prires_rxcb_s0', 2)}, CLEAN n {g('prires_rxcb_clean', 0)} >4 {g('prires_rxcb_clean', 1)} ({g('prires_rxcb_clean', 2)} %)")
        print(f"      block-Baarda G: {g('bb_G_clean', 0)} blocks / {g('bb_G_clean', 1)} rows, geo(t_B) {g('bb_G_clean', 2)}, sigma0 {g('bb_G_sigma0', 0)}"
              f" | W6-cb (last): nshared {g('w6cb', 0)} n_cb {g('w6cb', 1)} folded {g('w6cb', 2)}")
        rx = C.get("rxcb_carry")
        print(f"      rx-CB carry: " + (f"{rx[0]} folded, {rx[1]} withheld (innovation gate), {rx[2]} harvests, {rx[3]} carried at run end; {rx[4]} station(s) withheld > 90 %: {rx[5].strip()}" if rx else "- (no line)"))
        print(f"      P1-4 post-fit σ̂0 last epoch: all {g('p14', 1)} / code {g('p14', 2)} / phase {g('p14', 3)} | rows written {g('rows', 0)} ({g('rows', 1)} code + {g('rows', 2)} phase)"
              f" | SH gate withheld {g('sh_gate', 2)} | admission withheld {g('admission', 0)} | T2 A4-withheld {g('t2', 3)}")
        i = R.get("i")
        if i:
            print(f"  (i) code rms per station (median over {i['n_epochs']} epochs, {i['n_sta']} sta): p50 {fmt(i['p50'])}  p90 {fmt(i['p90'])}  max {fmt(i['max'])} m"
                  f"  [s0_code p50 {fmt(i['s0_p50'])}; phase rms p50 {fmt(i['phase_p50'], 4)}]  worst: " + ", ".join(f"{s[:4]} {v:.2f}" for s, v in i["worst"])
                  + "  best: " + ", ".join(f"{s[:4]} {v:.2f}" for s, v in i["best"]))
        else:
            print("  (i) no network_pf_keyed.csv STA rows in the window")
        ii = R.get("ii")
        if ii:
            print(f"  (ii) ZWD per station (median over {ii['n_epochs']} epochs, {ii['n_sta']} sta): |ZWD| > 0.5 m at {ii['n_over_0p5']} station(s) (> 5 m: {ii['n_over_5']}); |ZWD| p50 {fmt(ii['abs_p50'])} max {fmt(ii['abs_max'])} m"
                  f"; worst: " + ", ".join(f"{s[:4]} {v:+.2f}" for s, v in ii["worst"]))
        else:
            print("  (ii) no network_zwd.csv rows in the window")
        c = R.get("iii")
        if c:
            print(f"  (iii) wire-vs-CODE census, hour ({c['n_epochs']} epochs): D_phase within-arc rms declared {fmt(c['phase_decl_rms'])} (tracking {fmt(c['phase_track_rms'])}; diffmedian cm {fmt(c['phase_decl_rms_diffmedian'])}) m"
                  f"; D_code declared {fmt(c['code_decl_rms'])} (tracking {fmt(c['code_track_rms'])}; diffmedian {fmt(c['code_decl_rms_diffmedian'])}) m"
                  f"; raw clock contrast LEVEL p50 {fmt(c['clk_level_p50'])} p95 {fmt(c['clk_level_p95'])} m, within-arc rms {fmt(c['clk_decl_rms'])} m"
                  f"; pb {fmt(c['pb_decl_rms'])} cb {fmt(c['cb_decl_rms'])} m; declared phase arcs {c['phase_decl_arcs']} ({c['phase_decl_steps']} with a step > 0.25 m)")
            if c.get("phase_decl_rms_by_hour"):
                print(f"        by hour (full 2 h census): D_phase decl {[round(x, 3) for x in c['phase_decl_rms_by_hour']]}, D_code decl {[round(x, 3) for x in c['code_decl_rms_by_hour']]}, clk level p50 {[round(x, 3) for x in c['clk_level_p50_by_hour']]}")
            if c.get("missing_bia"):
                print(f"        rows outside the census (no CODE OSB): {c['missing_bia']}; counters {c['cnt']}")
        t = R.get("iv")
        if t:
            print(f"  (iv) per-LOS attribution, hour: {t['n_los']} LOS-signal series / {t['n_rows']} code rows, pooled code rms {fmt(t['pooled_rms'])} m; per-LOS MEAN share of SS {fmt(t['level_share'], 1)} %"
                  f" (|mean| p50 {fmt(t['abs_mean_p50'])} p90 {fmt(t['abs_mean_p90'])} m; rms about mean p50 {fmt(t['sd_p50'])} m); nested per-LOS dispersive {fmt(t['disp_nested'], 1)} %"
                  f" (implied shape-corrected {fmt(t['disp_corrected'], 1)} %, {'shape-ADMITTED' if t['iono_shape_admitted'] else 'shape-REFUTED'}:"
                  f" between/common {fmt(t['between_over_common'])} vs pure-dispersive {fmt(t['pure_dispersive_ratio'])}; 3-signal γ share {fmt(t['gamma_share_3'], 2)});"
                  f" (sta,sig) constants {fmt(t['sta_sig_const'], 1)} %, (sat,sig) {fmt(t['sat_sig_const'], 1)} %, nested LOS signal-common {fmt(t['los_common_nested'], 1)} %; A2 best row: {t['a2_best']} at {fmt(t['a2_best_share'], 1)} %"
                  f"; phase pooled rms {fmt(t['phase_pooled_rms'], 4)} m; per-signal code rms {{{', '.join(f'{k} {fmt(v, 2)}' for k, v in sorted(t['per_signal'].items()))}}}")
        B = bars(R, R0)
        R["bars"] = B
        print(f"  BARS: (i) p50<1.0 {'PASS' if B['passes']['i'] else 'fail'} | (ii) zero>0.5 {'PASS' if B['passes']['ii'] else 'fail'} | (iii) D_phase<0.5 {'PASS' if B['passes']['iii'] else 'fail'}"
              + ("" if R is R0 else f" | halved vs A0: {[k for k, v in B['halved'].items() if v] or 'none'} | availability ok: {B['avail_ok']}")
              + f"  =>  {B['label']}")

    # -- the six-row table
    print("\n" + "-" * 118)
    print("SIX-ROW TABLE (last hour): (i) code rms p50/p90/max m | (ii) #sta |ZWD|>0.5 (worst 3) | (iii) D_phase / D_code declared within-arc rms, clk LEVEL p50 m | (iv) LOS-mean SS share %, nested dispersive % | (v) emitted/synced, withheld")
    print("-" * 118)
    print(f"{'arm':4s} {'knob':52s} {'(i) p50/p90/max':>19s} {'(ii) n>0.5 (worst)':>28s} {'(iii) Dph/Dcd/clk':>20s} {'(iv) lvl%/disp%':>15s} {'(v) avail':>16s}  verdict")
    for R in results:
        i = R.get("i") or {}; ii = R.get("ii") or {}; c = R.get("iii") or {}; t = R.get("iv") or {}; av = R.get("avail") or {}
        w3 = ", ".join(f"{s[:4]} {v:+.1f}" for s, v in (ii.get("worst") or [])[:3])
        print(f"{R['arm']:4s} {R['knob'][:52]:52s} {fmt(i.get('p50'), 2, 5)}/{fmt(i.get('p90'), 2, 5)}/{fmt(i.get('max'), 2, 6)} "
              f"{str(ii.get('n_over_0p5', '-')):>4s} ({w3})".ljust(29) + " "
              f"{fmt(c.get('phase_decl_rms'), 2, 5)}/{fmt(c.get('code_decl_rms'), 2, 5)}/{fmt(c.get('clk_level_p50'), 2, 6)} "
              f"{fmt(t.get('level_share'), 1, 6)}/{fmt(t.get('disp_nested'), 1, 6)} "
              f"{str(av.get('emitted', '-')):>5s}/{str(av.get('synced', '-')):>5s} w{str((av.get('withheld_gate') or 0) + (av.get('withheld_a4') or 0)) if av else '-':>3s}  {R['bars']['label']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default="C:/GnssProcessor/runs")
    ap.add_argument("--arms", default="a0,a1,a2,a3,a4,a5")
    ap.add_argument("--prefix", default="_cm_")
    ap.add_argument("--tow0", type=float, default=259200.0, help="tow of k = 1 (DOY-129 00:00:00 = 259200)")
    ap.add_argument("--k-lo", type=int, default=121)
    ap.add_argument("--k-hi", type=int, default=240)
    ap.add_argument("--skip-census", action="store_true")
    ap.add_argument("--skip-attrib", action="store_true")
    ap.add_argument("--force", action="store_true", help="re-run the census / attrib even if their outputs exist")
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    arms = [s for s in a.arms.split(",") if s]
    results = []
    ref_cfg = None
    for arm in arms:
        armdir = os.path.join(a.runs, f"{a.prefix}{arm}")
        if not os.path.isdir(armdir):
            print(f"!! {armdir} missing — skipped")
            continue
        if arm.split("_")[0] == "a0" and ref_cfg is None:
            ref_cfg = read_lines(os.path.join(armdir, "logs", "gnss_config.log"))
        print(f"grading {arm} ({armdir}) …", flush=True)
        results.append(grade_arm(arm, armdir, a, ref_cfg))
    if not results:
        sys.exit("nothing graded")
    print_report(results, a)
    if a.json:
        def conv(o):
            if isinstance(o, (np.floating,)):
                return float(o)
            if isinstance(o, (np.integer,)):
                return int(o)
            if isinstance(o, np.ndarray):
                return o.tolist()
            if isinstance(o, float) and not np.isfinite(o):
                return None
            return str(o)
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(results, fh, indent=1, default=conv)
        print(f"\njson -> {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
