#!/usr/bin/env python3
"""Read a planted-coordinate arm against its control (card PLANT).

  arm   <control run dir> <arm run dir> [--station S48000SIM] [--consumer SZZ000SIM] [--json out.json]
  table <control run dir> <arm run dir> [<arm run dir> ...]  [--json out.json]
  render <table.json> [--hour 2]                     one line per arm and section from a `table` JSON
  card   <table.json> [--hour 2]                     the card's three tables (markdown) from a `table` JSON

Every number is a difference against the same-exe control on the same epochs, or a
datum-invariant quantity of the arm alone (card CLAUDE.md, grading rules):

  products   per-epoch RMS over satellites of (arm - control) after removing the per-epoch common mean
             (satellite clock; code bias per constellation and signal; phase bias per constellation
             and signal), in mm, from `logs/network_solution.csv` (the emitted products)
  consumer   the held-out consumer's 3D error against truth by hour (p50) and the per-epoch vector
             shift between arm and control, from `logs/closed_loop_trace.csv`
  station    the planted station's post-fit RMS (`network_pf_keyed.csv`), its rank among the stations,
             its wet delay and receiver clock against the control, the share of its rows absent or
             deweighted by 100x against the control (`network_pf_rows.csv`)
  screens    events naming the station by tier (`network_events.csv`), the station-congruence rows
             (`network_congruence.csv`), RAIM rows (`network_raim.csv`, `network_raim_attrib.csv`),
             the epoch dispositions (`network_epoch_status.csv`), the roster in the solve
             (`network_run_summary.csv`) and the run-end text (`stdout.txt`): lines naming the
             station and run-end lines whose numbers moved against the control

Hours are counted from the first emitted epoch (the sim golden starts at 00:30:15, so hour 1
is the start artifact and ignition; settled claims come from hour 2).
"""
import argparse
import json
import math
import os
import re
import sys

import numpy as np
import pandas as pd

H = 3600.0
MM = 1000.0


# ----------------------------------------------------------------------------- helpers
def csv(rd, name, **kw):
    p = f"{rd}/logs/{name}"
    return pd.read_csv(p, **kw) if os.path.exists(p) and os.path.getsize(p) > 0 else None


CAP = [10 ** 6]   # last hour index; the epoch at exactly the run length (241st of a 2 h arm) belongs to the last hour


def hour_of(tow, t0):
    return np.minimum(((np.asarray(tow, dtype=float) - t0) // H).astype(int), CAP[0])


def fnum(x, nd=1):
    if x is None or (isinstance(x, float) and not math.isfinite(x)):
        return "-"
    return f"{x:.{nd}f}"


def med(a):
    a = np.asarray(a, dtype=float)
    a = a[np.isfinite(a)]
    return float(np.median(a)) if a.size else float("nan")


def p95(a):
    a = np.asarray(a, dtype=float)
    a = a[np.isfinite(a)]
    return float(np.percentile(a, 95)) if a.size else float("nan")


def read_text(p):
    return open(p, encoding="utf-8", errors="replace").read().split("\n") if os.path.exists(p) else []


def run_meta(rd):
    ex = read_text(rd + "/exitcode.txt")
    meta = {"dir": rd, "exit": ex[0] if ex else "?", "wall_s": float(ex[1]) if len(ex) > 1 and ex[1] else float("nan")}
    if os.path.exists(rd + "/rapid.json"):
        meta["plant"] = json.load(open(rd + "/rapid.json")).get("plant")
    return meta


# ----------------------------------------------------------------------------- consumer
def consumer(ctl, arm):
    a, c = csv(arm, "closed_loop_trace.csv"), csv(ctl, "closed_loop_trace.csv")
    t0 = float(c["tow"].min())
    out = {"n_epochs_arm": int(len(a)), "n_epochs_ctl": int(len(c))}
    m = a.merge(c, on="tow", suffixes=("_a", "_c"))
    m["h"] = hour_of(m["tow"], t0)
    for h, g in m.groupby("h"):
        d = np.sqrt((g.east_m_a - g.east_m_c) ** 2 + (g.north_m_a - g.north_m_c) ** 2 + (g.up_m_a - g.up_m_c) ** 2)
        out[f"h{h + 1}"] = {
            "n": int(len(g)),
            "p50_arm_mm": MM * med(g.pos_err_3d_m_a), "p50_ctl_mm": MM * med(g.pos_err_3d_m_c),
            "dp50_mm": MM * (med(g.pos_err_3d_m_a) - med(g.pos_err_3d_m_c)),
            "shift_med_mm": MM * float(np.median(d)), "shift_max_mm": MM * float(np.max(d)),
            "dE_mean_mm": MM * float(np.mean(g.east_m_a - g.east_m_c)),
            "dN_mean_mm": MM * float(np.mean(g.north_m_a - g.north_m_c)),
            "dU_mean_mm": MM * float(np.mean(g.up_m_a - g.up_m_c)),
            "E_mean_arm_mm": MM * float(np.mean(g.east_m_a)), "N_mean_arm_mm": MM * float(np.mean(g.north_m_a)),
            "U_mean_arm_mm": MM * float(np.mean(g.up_m_a)),
            "p50_arm_nobias_mm": MM * med(g.pos_err_3d_m_a),
        }
    # last 30 minutes (60 epochs), the most settled reading a 2 h arm has
    tail = m[m["tow"] >= m["tow"].max() - 1800.0 + 1e-6]
    d = np.sqrt((tail.east_m_a - tail.east_m_c) ** 2 + (tail.north_m_a - tail.north_m_c) ** 2 + (tail.up_m_a - tail.up_m_c) ** 2)
    out["last30"] = {"n": int(len(tail)), "p50_arm_mm": MM * med(tail.pos_err_3d_m_a), "p50_ctl_mm": MM * med(tail.pos_err_3d_m_c),
                     "dp50_mm": MM * (med(tail.pos_err_3d_m_a) - med(tail.pos_err_3d_m_c)),
                     "shift_med_mm": MM * float(np.median(d)),
                     "dE_mean_mm": MM * float(np.mean(tail.east_m_a - tail.east_m_c)),
                     "dN_mean_mm": MM * float(np.mean(tail.north_m_a - tail.north_m_c)),
                     "dU_mean_mm": MM * float(np.mean(tail.up_m_a - tail.up_m_c))}
    return out


# ----------------------------------------------------------------------------- products
def products(ctl, arm, station):
    a, c = csv(arm, "network_solution.csv"), csv(ctl, "network_solution.csv")
    t0 = float(c["tow"].min())
    m = a.merge(c, on=["tow", "sat", "kind", "signal"], how="outer", suffixes=("_a", "_c"), indicator=True)
    out = {"rows_arm_only": int((m["_merge"] == "left_only").sum()), "rows_ctl_only": int((m["_merge"] == "right_only").sum())}
    m = m[m["_merge"] == "both"].copy()
    m["d"] = m["value_m_a"] - m["value_m_c"]
    m["con"] = m["sat"].str[0]
    m["signal"] = m["signal"].fillna("")
    m["h"] = hour_of(m["tow"], t0)
    out["disc_counter_changed_rows"] = int((m["disc_counter_a"] != m["disc_counter_c"]).sum())

    def per_epoch_rms(df, keys):
        g = df.groupby(keys + ["tow"])["d"]
        dm = df["d"] - g.transform("mean")
        rms = (dm ** 2).groupby([df[k] for k in keys] + [df["tow"]]).mean().pow(0.5)
        return rms

    for kind in ("CLK", "CB", "PB"):
        df = m[m["kind"] == kind]
        if df.empty:
            continue
        keys = ["kind"] if kind == "CLK" else ["con", "signal"]
        if kind == "CLK":
            df = df.assign(kind=kind)
        rms = per_epoch_rms(df, keys).reset_index(name="rms")
        # collapse the (constellation, signal) groups of CB and PB by taking the largest group each epoch
        e = rms.groupby("tow")["rms"].max().reset_index()
        e["h"] = hour_of(e["tow"], t0)
        out[kind] = {f"h{h + 1}": {"rms_med_mm": MM * med(g.rms), "rms_p95_mm": MM * p95(g.rms), "rms_max_mm": MM * float(g.rms.max())}
                     for h, g in e.groupby("h")}
        out[kind]["last30_med_mm"] = MM * med(e[e["tow"] >= e["tow"].max() - 1800.0 + 1e-6].rms)
        out[kind]["max_mm"] = MM * float(e.rms.max())
        if kind == "CLK":
            # the satellites the planted station tracks at that epoch against the others (the contamination is regional)
            pr = csv(ctl, "network_pf_rows.csv", usecols=["tow", "station", "sat"])
            if pr is not None:
                seen = pr[pr.station == station][["tow", "sat"]].drop_duplicates().assign(seen=1)
                dd = df.merge(seen, on=["tow", "sat"], how="left")
                dd["seen"] = dd["seen"].fillna(0).astype(int)
                dd["dm"] = dd["d"] - dd.groupby("tow")["d"].transform("mean")
                for flag, name in ((1, "tracked"), (0, "untracked")):
                    gg = dd[dd.seen == flag]
                    rr = (gg["dm"] ** 2).groupby(gg["tow"]).mean().pow(0.5)
                    hh = hour_of(rr.index.values, t0)
                    out["CLK"][f"rms_{name}_by_hour_mm"] = {f"h{h + 1}": MM * med(rr.values[hh == h]) for h in np.unique(hh)}
                out["CLK"]["n_tracked_med"] = float(dd.groupby("tow")["seen"].sum().median())
                out["CLK"]["n_sats_med"] = float(dd.groupby("tow")["sat"].count().median())
            # the same per constellation, and the common mean itself (a datum shift, absorbed by a consumer clock)
            for con, g in df.groupby("con"):
                r = per_epoch_rms(g.assign(kind=kind), ["kind"]).reset_index(name="rms")
                out["CLK"][f"rms_{con}_med_mm"] = MM * med(r.rms)
            cm = df.groupby("tow")["d"].mean().reset_index()
            cm["h"] = hour_of(cm["tow"], t0)
            out["CLK"]["common_mean_med_mm"] = MM * med(cm["d"])
            out["CLK"]["common_mean_absmax_mm"] = MM * float(cm["d"].abs().max())
    return out


# ----------------------------------------------------------------------------- station
def station_block(ctl, arm, st):
    out = {}
    ka, kc = csv(arm, "network_pf_keyed.csv"), csv(ctl, "network_pf_keyed.csv")
    t0 = float(csv(ctl, "closed_loop_trace.csv")["tow"].min())
    if ka is not None and kc is not None:
        for tag, k in (("arm", ka), ("ctl", kc)):
            s = k[(k["kind"] == "STA")].copy()
            s["h"] = hour_of(s["tow"], t0)
            res = {}
            for h, g in s.groupby("h"):
                one = g[g["key"] == st]
                others = g[g["key"] != st]
                # per-station hour median of the rms, then the rank of the planted one
                pm_code = others.groupby("key")["rms_code"].median()
                pm_phase = others.groupby("key")["rms_phase"].median()
                r = {"rms_code_mm": MM * med(one.rms_code), "rms_phase_mm": MM * med(one.rms_phase),
                     "max_code_m": float(one.max_code.max()), "max_phase_m": float(one.max_phase.max()),
                     "others_code_med_mm": MM * float(pm_code.median()), "others_code_max_mm": MM * float(pm_code.max()),
                     "others_phase_med_mm": MM * float(pm_phase.median()), "others_phase_max_mm": MM * float(pm_phase.max())}
                r["code_rank"] = int((pm_code > med(one.rms_code)).sum()) + 1
                r["phase_rank"] = int((pm_phase > med(one.rms_phase)).sum()) + 1
                res[f"h{h + 1}"] = r
            out["pf_" + tag] = res
    za, zc = csv(arm, "network_zwd.csv"), csv(ctl, "network_zwd.csv")
    if za is not None and zc is not None:
        m = za[za.station == st].merge(zc[zc.station == st], on="tow", suffixes=("_a", "_c"))
        m["h"] = hour_of(m["tow"], t0)
        out["zwd"] = {f"h{h + 1}": {"dzwd_mean_mm": MM * float((g.zwd_m_a - g.zwd_m_c).mean()), "zwd_arm_mm": MM * float(g.zwd_m_a.mean()),
                                     "zwd_ctl_mm": MM * float(g.zwd_m_c.mean())} for h, g in m.groupby("h")}
    ra, rc = csv(arm, "network_rx_states.csv"), csv(ctl, "network_rx_states.csv")
    if ra is not None and rc is not None:
        a = ra[(ra.station == st) & (ra.kind == "CLK")]
        c = rc[(rc.station == st) & (rc.kind == "CLK")]
        m = a.merge(c, on="tow", suffixes=("_a", "_c"))
        m["h"] = hour_of(m["tow"], t0)
        out["rxclk"] = {f"h{h + 1}": {"dclk_mean_mm": MM * float((g.value_m_a - g.value_m_c).mean())} for h, g in m.groupby("h")}
        # the common (all-station) receiver clock shift, to separate the planted station's own part
        a = ra[ra.kind == "CLK"].merge(rc[rc.kind == "CLK"], on=["tow", "station"], suffixes=("_a", "_c"))
        a["d"] = a.value_m_a - a.value_m_c
        a["h"] = hour_of(a["tow"], t0)
        res = {}
        for h, g in a.groupby("h"):
            own = g[g.station == st].d.mean()
            oth = g[g.station != st].groupby("station").d.mean()
            res[f"h{h + 1}"] = {"own_minus_others_median_mm": MM * float(own - oth.median()),
                                "others_spread_mm": MM * float(oth.std())}
        out["rxclk_rel"] = res
    # rows absent or deweighted
    pa = csv(arm, "network_pf_rows.csv", usecols=["tow", "station", "sat", "signal", "kind", "resid_m", "w"])
    pc = csv(ctl, "network_pf_rows.csv", usecols=["tow", "station", "sat", "signal", "kind", "resid_m", "w"])
    if pa is not None and pc is not None:
        pa, pc = pa[pa.station == st], pc[pc.station == st]
        m = pc.merge(pa, on=["tow", "sat", "signal", "kind"], how="left", suffixes=("_c", "_a"))
        m["h"] = hour_of(m["tow"], t0)
        res = {}
        for h, g in m.groupby("h"):
            n = len(g)
            absent = int(g["w_a"].isna().sum())
            low = int(((g["w_a"] < 0.01 * g["w_c"]) & g["w_a"].notna()).sum())
            code = g[g.kind == "code"]
            res[f"h{h + 1}"] = {"rows_ctl": n, "absent": absent, "deweighted_100x": low,
                                "frac_out": (absent + low) / n if n else float("nan"),
                                "code_resid_rms_arm_mm": MM * float(np.sqrt(np.nanmean(code.resid_m_a ** 2))) if len(code) else float("nan"),
                                "code_resid_rms_ctl_mm": MM * float(np.sqrt(np.nanmean(code.resid_m_c ** 2))) if len(code) else float("nan"),
                                "code_resid_mean_arm_mm": MM * float(np.nanmean(code.resid_m_a)) if len(code) else float("nan")}
        out["rows"] = res
    return out


# ----------------------------------------------------------------------------- screens
def screens(ctl, arm, st):
    out = {}
    t0 = float(csv(ctl, "closed_loop_trace.csv")["tow"].min())
    ea, ec = csv(arm, "network_events.csv"), csv(ctl, "network_events.csv")

    def tally(e):
        if e is None:
            return {}, {}
        e = e.copy()
        e["key"] = e["tier"].astype(str) + "/" + e["event"].astype(str)
        net = e.groupby("key").size().to_dict()
        named = e[e["station"].astype(str) == st]
        det = e["detail"].astype(str).str.contains(st, regex=False) & (e["station"].astype(str) != st)
        mine = {}
        for k, g in named.groupby("key"):
            mine[k] = {"n": int(len(g)), "first_min": float((g["tow"].min() - t0) / 60.0)}
        for k, g in e[det].groupby("key"):
            mine.setdefault(k + " (in detail)", {"n": int(len(g)), "first_min": float((g["tow"].min() - t0) / 60.0)})
        return net, mine

    na, ma = tally(ea)
    nc, mc = tally(ec)
    out["events_named_station"] = {k: {"arm": ma.get(k, {}).get("n", 0), "ctl": mc.get(k, {}).get("n", 0),
                                       "first_min_arm": ma.get(k, {}).get("first_min")} for k in sorted(set(ma) | set(mc))}
    out["events_network_delta"] = {k: {"arm": na.get(k, 0), "ctl": nc.get(k, 0)} for k in sorted(set(na) | set(nc)) if na.get(k, 0) != nc.get(k, 0)}
    # who else is named: stations named by GROSSLOS and CONGRUENCE events, arm against control
    if ea is not None:
        for tier in ("GROSSLOS", "CONGRUENCE", "MDB", "PREFIT", "T4_attrib", "RAIM"):
            sel = ea[ea["tier"].astype(str) == tier]
            if len(sel):
                cnt = sel.groupby("station", dropna=True).size().sort_values(ascending=False)
                out.setdefault("tier_top_stations", {})[tier] = {"top": [(s, int(n)) for s, n in cnt.head(4).items()],
                                                                 "planted_rank": int((cnt > cnt.get(st, 0)).sum()) + 1 if st in cnt else None,
                                                                 "planted_n": int(cnt.get(st, 0)), "n_stations": int(len(cnt))}
    # who the gross line-of-sight screen declares: the planted station, or others (a poisoned network blames the wrong ones)
    for tag, e in (("arm", ea), ("ctl", ec)):
        if e is None:
            continue
        dec = e[(e["tier"].astype(str) == "GROSSLOS") & e["event"].astype(str).isin(["gross_los_cold_declare", "gross_los_declare"])]
        cnt = dec.groupby("station").size()
        out.setdefault("gross_declared", {})[tag] = {
            "planted": int(cnt.get(st, 0)), "others": int(cnt.drop(st, errors="ignore").sum()),
            "n_other_stations": int((cnt.drop(st, errors="ignore") > 0).sum()),
            "first_min_planted": float((dec[dec.station == st].tow.min() - t0) / 60.0) if (dec.station == st).any() else None,
            "first_min_others": float((dec[dec.station != st].tow.min() - t0) / 60.0) if (dec.station != st).any() else None,
            "standdown_planted": int(((e["tier"].astype(str) == "GROSSLOS") & e["event"].astype(str).str.contains("standdown") & (e["station"].astype(str) == st)).sum()),
            "emit_disc": int(((e["tier"].astype(str) == "GROSSLOS") & (e["event"].astype(str) == "gross_los_emit_disc")).sum()),
            "emit_withheld": int(((e["tier"].astype(str) == "GROSSLOS") & (e["event"].astype(str) == "gross_los_emit_withheld")).sum()),
            "retro_bumps": int(((e["tier"].astype(str) == "RETRO")).sum())}
    # congruence rows
    ca, cc = csv(arm, "network_congruence.csv"), csv(ctl, "network_congruence.csv")
    cg = {}
    for tag, c in (("arm", ca), ("ctl", cc)):
        if c is None:
            continue
        c = c.copy()
        c["h"] = hour_of(c["tow"], t0)
        res = {}
        for h, g in c.groupby("h"):
            one = g[g.station == st]
            others = g[g.station != st]
            om = others.groupby("station")["t"].median()
            r = {"n_sweeps": int(len(one)), "t_med": med(one.t), "t_max": float(one.t.max()) if len(one) else float("nan"),
                 "n_over4": int((one.t > 4.0).sum()),
                 "frac_over4": float((one.t > 4.0).mean()) if len(one) else float("nan"),
                 "dE_cm": 100 * med(one.d_e), "dN_cm": 100 * med(one.d_n), "dU_cm": 100 * med(one.d_u),
                 "mdb_E_m": med(one.mdb_e), "mdb_U_m": med(one.mdb_u),
                 "others_t_med_p50": float(om.median()), "others_t_med_p95": float(np.percentile(om, 95)) if len(om) else float("nan"),
                 "others_t_med_max": float(om.max()) if len(om) else float("nan"),
                 "rank_by_t_med": int((om > med(one.t)).sum()) + 1}
            res[f"h{h + 1}"] = r
        one = c[c.station == st].sort_values("tow")
        tt = one.t.values
        over = tt > 4.0
        first = float((one.tow.values[over][0] - t0) / 60.0) if over.any() else None
        # the sweep from which every later sweep is over the report line
        pers = None
        for i in range(len(tt)):
            if over[i:].all():
                pers = float((one.tow.values[i] - t0) / 60.0)
                break
        res["summary"] = {"n_sweeps": int(len(tt)), "n_over4": int(over.sum()), "first_over4_min": first, "persistent_from_min": pers,
                          "sweep_stride_min": float(np.median(np.diff(one.tow.values)) / 60.0) if len(tt) > 1 else None}
        cg[tag] = res
    out["congruence"] = cg
    # RAIM rows
    ra, rc = csv(arm, "network_raim.csv"), csv(ctl, "network_raim.csv")
    out["raim"] = {"rows_station_arm": int((ra.station == st).sum()) if ra is not None else None,
                   "rows_station_ctl": int((rc.station == st).sum()) if rc is not None else None,
                   "spiked_station_arm": int(((ra.station == st) & (ra.spiked == 1)).sum()) if ra is not None else None,
                   "spiked_station_ctl": int(((rc.station == st) & (rc.spiked == 1)).sum()) if rc is not None else None,
                   "rows_arm": int(len(ra)) if ra is not None else None, "rows_ctl": int(len(rc)) if rc is not None else None,
                   "spiked_arm": int((ra.spiked == 1).sum()) if ra is not None else None,
                   "spiked_ctl": int((rc.spiked == 1).sum()) if rc is not None else None,
                   "refloated_arm": int((ra.refloated == 1).sum()) if ra is not None else None,
                   "refloated_ctl": int((rc.refloated == 1).sum()) if rc is not None else None}
    aa, ac = csv(arm, "network_raim_attrib.csv"), csv(ctl, "network_raim_attrib.csv")
    out["raim_attrib"] = {"station_rows_arm": int(((aa.dim == "STA") & (aa.entity == st)).sum()) if aa is not None else None,
                          "station_rows_ctl": int(((ac.dim == "STA") & (ac.entity == st)).sum()) if ac is not None else None,
                          "station_acted_arm": int(((aa.dim == "STA") & (aa.entity == st) & (aa.acted == 1)).sum()) if aa is not None else None,
                          "acted_arm": int((aa.acted == 1).sum()) if aa is not None else None,
                          "acted_ctl": int((ac.acted == 1).sum()) if ac is not None else None,
                          "station_effect_max_arm": float(aa[(aa.dim == "STA") & (aa.entity == st)].effect.max()) if aa is not None and ((aa.dim == "STA") & (aa.entity == st)).any() else None,
                          "station_effect_max_ctl": float(ac[(ac.dim == "STA") & (ac.entity == st)].effect.max()) if ac is not None and ((ac.dim == "STA") & (ac.entity == st)).any() else None}
    # RAIM and attribution detail at the planted station: when, how often, how loud, against the next station
    det = {}
    for tag, rr, aa_ in (("arm", ra, aa), ("ctl", rc, ac)):
        d = {}
        if rr is not None:
            one = rr[rr.station == st]
            d["spiked_first_min"] = float((one.tow.min() - t0) / 60.0) if len(one) else None
            d["spiked_by_hour"] = {f"h{h + 1}": int((hour_of(one.tow, t0) == h).sum()) for h in range(CAP[0] + 1)} if len(one) else {}
            d["s_max_norm_med"] = med(one.s_max_norm) if len(one) else None
            d["s_max_norm_max"] = float(one.s_max_norm.max()) if len(one) else None
            d["n_sta_spiked_max"] = int(one.n_sta_spiked.max()) if len(one) else None
            d["spiked_rows_other_stations"] = int((rr.station != st).sum())
        if aa_ is not None:
            sta = aa_[aa_.dim == "STA"]
            cnt = sta.groupby("entity").size().sort_values(ascending=False)
            one = sta[sta.entity == st]
            d["attrib_sta_rows"] = int(len(one))
            d["attrib_first_min"] = float((one.tow.min() - t0) / 60.0) if len(one) else None
            d["attrib_prominence_max"] = float(one.prominence.max()) if len(one) else None
            d["attrib_effect_max"] = float(one.effect.max()) if len(one) else None
            d["attrib_rank"] = int((cnt > cnt.get(st, 0)).sum()) + 1 if st in cnt else None
            d["attrib_next_station_rows"] = int(cnt.drop(st, errors="ignore").max()) if len(cnt.drop(st, errors="ignore")) else 0
            big = one[one.prominence > 10.0]
            d["attrib_rows_over10"] = int(len(big))
            d["attrib_first_over10_min"] = float((big.tow.min() - t0) / 60.0) if len(big) else None
            d["attrib_by_hour"] = {f"h{h + 1}": int((hour_of(one.tow, t0) == h).sum()) for h in range(CAP[0] + 1)}
        det[tag] = d
    out["raim_detail"] = det
    # epoch status
    sa, sc = csv(arm, "network_epoch_status.csv"), csv(ctl, "network_epoch_status.csv")
    es = {}
    for tag, s in (("arm", sa), ("ctl", sc)):
        if s is None:
            continue
        s = s.copy()
        s["h"] = hour_of(s["tow"], t0)
        es[tag] = {"dispositions": s["disposition"].value_counts().to_dict(),
                   "by_hour": {f"h{h + 1}": {"sigma0_pf_med": med(g.sigma0_pf), "sigma0_pf_code_med": med(g.sigma0_pf_code),
                                              "n_refloat_sats_sum": int(g.n_refloat_sats.sum()), "n_slip_refloats_sum": int(g.n_slip_refloats.sum()),
                                              "n_present_min": int(g.n_present.min())} for h, g in s.groupby("h")}}
    out["epoch_status"] = es
    rs, rc2 = csv(arm, "network_run_summary.csv"), csv(ctl, "network_run_summary.csv")
    out["run_summary"] = {"arm": rs.iloc[0].to_dict() if rs is not None else None, "ctl": rc2.iloc[0].to_dict() if rc2 is not None else None}
    return out


# ----------------------------------------------------------------------------- stdout
LEVEL = re.compile(r"\[(info|warning|error|critical|debug)\]")


def template(line):
    body = line.split("] ", 3)[-1] if line.startswith("[") else line
    return re.sub(r"[-+]?\d+(\.\d+)?([eE][-+]?\d+)?", "#", body)


def stdout_block(ctl, arm, st):
    la, lc = read_text(arm + "/stdout.txt"), read_text(ctl + "/stdout.txt")
    out = {}

    def named(lines):
        res = {}
        for ln in lines:
            if st in ln:
                m = LEVEL.search(ln)
                key = (m.group(1) if m else "?", template(ln)[:300])
                res.setdefault(key, [0, ln])
                res[key][0] += 1
        return res

    na, nc = named(la), named(lc)
    new = [(k[0], v[0], v[1].split("] ", 3)[-1][:400]) for k, v in na.items() if k not in nc]
    out["lines_naming_station_arm"] = int(sum(v[0] for v in na.values()))
    out["lines_naming_station_ctl"] = int(sum(v[0] for v in nc.values()))
    out["new_lines_naming_station"] = new
    out["warning_lines_arm"] = int(sum(1 for ln in la if "[warning]" in ln))
    out["warning_lines_ctl"] = int(sum(1 for ln in lc if "[warning]" in ln))
    out["error_lines_arm"] = int(sum(1 for ln in la if "[error]" in ln or "[critical]" in ln))
    out["warnings_naming_station_arm"] = [ln.split("] ", 3)[-1][:300] for ln in la if "[warning]" in ln and st in ln][:10]

    # run-end lines whose numbers moved against the control
    def runend(lines):
        k = next((i for i, ln in enumerate(lines) if "Network[run-end]" in ln), None)
        return lines[k:] if k is not None else []

    ra, rc = runend(la), runend(lc)
    grp_a, grp_c = {}, {}
    for ln in ra:
        grp_a.setdefault(template(ln), []).append(ln.split("] ", 3)[-1])
    for ln in rc:
        grp_c.setdefault(template(ln), []).append(ln.split("] ", 3)[-1])
    changed, only_arm, only_ctl = [], [], []
    for t, lines in grp_a.items():
        cl = grp_c.get(t)
        if cl is None:
            only_arm += [(t, l) for l in lines]
            continue
        for i, l in enumerate(lines):
            if i < len(cl) and l != cl[i]:
                changed.append((cl[i], l))
    for t, lines in grp_c.items():
        if t not in grp_a:
            only_ctl += [(t, l) for l in lines]
    # a line whose COUNTS (integers) moved; real-valued drift in a worst-case magnitude is not a count
    timing = re.compile(r"ms| ms |wall|timing|recomputes|loop_ms|\d+\.\d+ s|elapsed|ms/|ms \(")
    ints = re.compile(r"(?<![\d.eE+-])\d+(?![\d.])")
    out["runend_counts_moved"] = [{"ctl": c[:420], "arm": a[:420]} for c, a in changed
                                  if not timing.search(c) and ints.findall(c) != ints.findall(a)]
    out["runend_lines_real_drift"] = int(sum(1 for c, a in changed if not timing.search(c) and ints.findall(c) == ints.findall(a)))
    out["runend_only_arm"] = [l[:420] for _, l in only_arm if "congruence) S" not in l][:40]
    out["runend_only_ctl"] = [l[:420] for _, l in only_ctl if "congruence) S" not in l][:40]
    # the run-end counters the questions name
    def grab(lines, pat, grp=1):
        for ln in lines:
            m = re.search(pat, ln)
            if m:
                return m.group(grp)
        return None

    for tag, lines in (("arm", la), ("ctl", lc)):
        out.setdefault("counters", {})[tag] = {
            "gross_los_over_bar": grab(lines, r"gross-LOS screen\): (\d+) LOS-epoch\(s\) over the"),
            "gross_cold_over_bound": grab(lines, r"semaphore|screened inside the maturity horizon \(\d+ emitted epochs\), (\d+) over the bound"),
            "gross_declarations": grab(lines, r"gross-LOS screen\): (\d+) declaration\(s\), \d+ release"),
            "gross_cold_declarations": grab(lines, r"cold bound\): (\d+) declaration\(s\)"),
            "gross_station_declarations": grab(lines, r"B4 station_tier = \w+\): (\d+) STATION declaration"),
            "t2_whole_sat_refloats": grab(lines, r"(\d+) whole-sat refloat"),
            "t2_station_exclusions": grab(lines, r"isolated-arc refloat\(s\), (\d+) station exclusion"),
            "sigma0_withheld": grab(lines, r"(\d+) by the A4"),
            "pd_dropped": grab(lines, r"n_sta_pd_dropped (\d+) total"),
            "congruence_reported": grab(lines, r"(\d+) reported, \d+ event"),
            "mdb_events": grab(lines, r"\((\d+) event\(s\) emitted, \d+ suppressed by the 20-pass"),
            "prefit_exceedances": grab(lines, r"prefit census INC-1\): (\d+) row-cell exceedance"),
            "admission_over_bound": grab(lines, r"measurement admission CENSUS\): (\d+) channel-epoch"),
        }
    # the whole congruence table: the run-level statistic per station, and the planted station's place in it
    row = re.compile(r"station congruence\) (\S+)/CLEAN: n = (\d+).*?delta = E ([+-][\d.]+)\+-([\d.]+)  N ([+-][\d.]+)\+-([\d.]+)  U ([+-][\d.]+)\+-([\d.]+) cm.*?geo\(t\) = ([\d.eE+-]+), (\d+) over")
    for tag, lines in (("arm", la), ("ctl", lc)):
        tab = {}
        for ln in lines:
            m = row.search(ln)
            if m:
                tab[m.group(1)] = {"n": int(m.group(2)), "dE": float(m.group(3)), "dN": float(m.group(5)), "dU": float(m.group(7)),
                                   "madU": float(m.group(8)), "geo": float(m.group(9)), "over4": int(m.group(10))}
        if st in tab:
            others = sorted((v["geo"] for k, v in tab.items() if k != st), reverse=True)
            out.setdefault("congruence_table", {})[tag] = {
                "planted": tab[st], "geo_rank": 1 + sum(1 for v in others if v > tab[st]["geo"]),
                "others_geo_max": others[0], "others_geo_median": float(np.median(others)), "n_stations": len(tab),
                "others_over4_median": float(np.median([v["over4"] for k, v in tab.items() if k != st])),
                "others_over4_max": max(v["over4"] for k, v in tab.items() if k != st)}
    # the congruence run-end row of the station
    for tag, lines in (("arm", la), ("ctl", lc)):
        for ln in lines:
            if f"station congruence) {st}/CLEAN: n =" in ln:
                out.setdefault("congruence_row", {})[tag] = ln.split("] ", 3)[-1][:500]
    return out


# ----------------------------------------------------------------------------- render
def ev_sum(r, prefix, which="arm"):
    d = r["screens"]["events_named_station"]
    return sum(v[which] for k, v in d.items() if k.startswith(prefix) and "(in detail)" not in k)


def ev_first(r, prefix):
    d = r["screens"]["events_named_station"]
    f = [v["first_min_arm"] for k, v in d.items() if k.startswith(prefix) and "(in detail)" not in k and v["arm"] > v["ctl"] and v["first_min_arm"] is not None]
    return min(f) if f else None


def g(d, *path, default=None):
    for p in path:
        if d is None or p not in d:
            return default
        d = d[p]
    return d


def render(res, hour):
    hk, h1 = f"h{hour}", "h1"
    tags = list(res)
    out = []
    out.append("SCREENS at the planted station (events and rows against the control; first = minutes from the first epoch)")
    out.append("arm | gross-LOS declared (first) | LOS refloats | MDB events | cong t h1 / h%d (rank of 60) | cong dE,dN,dU cm h%d | rows out h1 / h%d | RAIM spiked | warnings naming | n in solve min | other collateral events" % (hour, hour, hour))
    for t in tags:
        r = res[t]
        cong = g(r, "screens", "congruence", "arm") or {}
        cc = g(r, "screens", "congruence", "ctl") or {}
        rows = g(r, "station", "rows") or {}
        coll = {k: v for k, v in (g(r, "screens", "events_network_delta") or {}).items()
                if not k.startswith(("GROSSLOS", "CONGRUENCE", "PRIORRES", "MDB"))}
        out.append(" | ".join([
            t,
            f"{ev_sum(r, 'GROSSLOS/gross_los_cold_declare') + ev_sum(r, 'GROSSLOS/gross_los_declare')} ({fnum(ev_first(r, 'GROSSLOS/gross_los_cold_declare'))})",
            str(ev_sum(r, "GROSSLOS/gross_los_cold_refloat")),
            f"{ev_sum(r, 'MDB')} (ctl {ev_sum(r, 'MDB', 'ctl')})",
            f"{fnum(g(cong, h1, 't_med'), 2)} / {fnum(g(cong, hk, 't_med'), 2)} ({g(cong, hk, 'rank_by_t_med')}; ctl {g(cc, hk, 'rank_by_t_med')})",
            f"{fnum(g(cong, hk, 'dE_cm'))}, {fnum(g(cong, hk, 'dN_cm'))}, {fnum(g(cong, hk, 'dU_cm'))}",
            f"{fnum(100 * g(rows, h1, 'frac_out', default=float('nan')), 0)}% / {fnum(100 * g(rows, hk, 'frac_out', default=float('nan')), 0)}%",
            f"{g(r, 'screens', 'raim', 'spiked_station_arm')}",
            f"{len(g(r, 'stdout', 'warnings_naming_station_arm') or [])}",
            str(g(r, "screens", "run_summary", "arm", "n_sta_in_solve_min")),
            ", ".join(f"{k.split('/')[-1]} {v['arm']}/{v['ctl']}" for k, v in coll.items())[:120]]))
    out.append("")
    out.append("PRODUCTS (mm, per-epoch RMS over satellites of arm minus control after removing the common mean; median over the hour) and CONSUMER")
    out.append("arm | CLK h1 / h%d | CLK max | CB max | PB h%d | PB max | consumer p50 ctl h1,h%d | arm h1,h%d | shift median h%d | dE,dN,dU mean h%d" % (hour, hour, hour, hour, hour, hour))
    for t in tags:
        r = res[t]
        c = r["consumer"]
        pr = r["products"]
        out.append(" | ".join([
            t,
            f"{fnum(g(pr, 'CLK', h1, 'rms_med_mm'), 2)} / {fnum(g(pr, 'CLK', hk, 'rms_med_mm'), 2)}",
            fnum(g(pr, "CLK", "max_mm"), 1), fnum(g(pr, "CB", "max_mm"), 2),
            fnum(g(pr, "PB", hk, "rms_med_mm"), 2), fnum(g(pr, "PB", "max_mm"), 1),
            f"{fnum(g(c, h1, 'p50_ctl_mm'), 1)}, {fnum(g(c, hk, 'p50_ctl_mm'), 1)}",
            f"{fnum(g(c, h1, 'p50_arm_mm'), 1)}, {fnum(g(c, hk, 'p50_arm_mm'), 1)}",
            fnum(g(c, hk, "shift_med_mm"), 1),
            f"{fnum(g(c, hk, 'dE_mean_mm'), 1)}, {fnum(g(c, hk, 'dN_mean_mm'), 1)}, {fnum(g(c, hk, 'dU_mean_mm'), 1)}"]))
    out.append("")
    out.append("PLANTED STATION itself (post-fit RMS against the control and its rank among 60; wet delay and receiver clock against the control, mm)")
    out.append("arm | code RMS h%d arm / ctl (rank) | phase RMS h%d arm / ctl (rank) | dZWD h1, h%d | dClk h1, h%d" % (hour, hour, hour, hour))
    for t in tags:
        r = res[t]
        pa, pc = g(r, "station", "pf_arm"), g(r, "station", "pf_ctl")
        out.append(" | ".join([
            t,
            f"{fnum(g(pa, hk, 'rms_code_mm'), 0)} / {fnum(g(pc, hk, 'rms_code_mm'), 0)} ({g(pa, hk, 'code_rank')})",
            f"{fnum(g(pa, hk, 'rms_phase_mm'), 1)} / {fnum(g(pc, hk, 'rms_phase_mm'), 1)} ({g(pa, hk, 'phase_rank')})",
            f"{fnum(g(r, 'station', 'zwd', h1, 'dzwd_mean_mm'), 1)}, {fnum(g(r, 'station', 'zwd', hk, 'dzwd_mean_mm'), 1)}",
            f"{fnum(g(r, 'station', 'rxclk_rel', h1, 'own_minus_others_median_mm'), 1)}, {fnum(g(r, 'station', 'rxclk_rel', hk, 'own_minus_others_median_mm'), 1)}"]))
    return "\n".join(out)


def label(tag):
    m = re.match(r"_rap_plant_([eu])(\d+(?:cm|m|km))_s(\d)", tag)
    return (m.group(1).upper(), m.group(2), m.group(3)) if m else (tag, "", "")


SIZE_ORDER = {"1cm": 1, "10cm": 2, "1m": 3, "3m": 4, "10m": 5, "1km": 6}


def sig(x, nd=3):
    """A number to a few significant digits, for tables spanning 0.04 mm to 80 m."""
    if x is None or (isinstance(x, float) and not math.isfinite(x)):
        return "-"
    if abs(x) < 1e-3:
        return "0"
    if abs(x) >= 100:
        return f"{x:.0f}"
    return f"{x:.{max(nd - int(math.floor(math.log10(abs(x)))) - 1, 0)}f}"


def persistent_text(sm):
    """Minute from which every later sweep is over t = 4; '-' when fewer than three sweeps follow (the last one is not persistence)."""
    p, n, stride = g(sm, "persistent_from_min"), g(sm, "n_sweeps"), g(sm, "sweep_stride_min")
    if p is None or not n or not stride:
        return "-"
    after = ((n - 1) * stride - p) / stride + 1
    return fnum(p, 0) if after >= 3 else "-"


def card_tables(res, hour):
    hk, h1 = f"h{hour}", "h1"
    rows = sorted(((label(t), t) for t in res if label(t)[1]), key=lambda x: (SIZE_ORDER.get(x[0][1], 9), x[0][0]))
    a = ["| size | dir | wire: satellite clock RMS, h1 / h%d (mm) | phase bias RMS h%d (mm) | code bias max (mm) | consumer 3D p50, h1 / h%d (mm), control %s / %s | consumer shift, median h%d (mm) | consumer mean shift E / N / U, h%d (mm) |" % (hour, hour, hour, sig(g(res[rows[0][1]], "consumer", h1, "p50_ctl_mm")), sig(g(res[rows[0][1]], "consumer", hk, "p50_ctl_mm")), hour, hour),
         "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    b = ["| size | dir | congruence geo(t) at the station (largest of the other 59) | sweeps over t > 4 (of all) | persistent over 4 from (min) | CONGRUENCE events naming it, arm / control | attribution rows at it / next station, first with prominence over 10 (min) | RAIM spike rows at it, first (min) | gross-LOS declarations: at it / at others (stations) | its rows deweighted or absent h%d (%%) | warning naming it |" % (hour,),
         "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    c = ["| size | dir | wet delay change h%d (mm) | receiver clock change h%d (mm) | own post-fit code RMS, arm / control (mm), rank of 60 | own post-fit phase RMS, arm / control (mm), rank of 60 |" % (hour, hour), "| --- | --- | --- | --- | --- | --- |"]
    for (d, sz, _), t in rows:
        r = res[t]
        cn, pr, sc = r["consumer"], r["products"], r["screens"]
        a.append("| %s | %s | %s / %s | %s | %s | %s / %s | %s | %s / %s / %s |" % (
            sz, d, sig(g(pr, "CLK", h1, "rms_med_mm")), sig(g(pr, "CLK", hk, "rms_med_mm")), sig(g(pr, "PB", hk, "rms_med_mm")),
            sig(g(pr, "CB", "max_mm")), sig(g(cn, h1, "p50_arm_mm")), sig(g(cn, hk, "p50_arm_mm")),
            sig(g(cn, hk, "shift_med_mm")), sig(g(cn, hk, "dE_mean_mm")), sig(g(cn, hk, "dN_mean_mm")), sig(g(cn, hk, "dU_mean_mm"))))
        ct = g(r, "stdout", "congruence_table", "arm") or {}
        sm = g(sc, "congruence", "arm", "summary") or {}
        rd = g(sc, "raim_detail", "arm") or {}
        gd = g(sc, "gross_declared", "arm") or {}
        rows_h = g(r, "station", "rows", hk) or {}
        warn = len(g(r, "stdout", "warnings_naming_station_arm") or [])
        b.append("| %s | %s | %s (%s) | %s | %s | %s | %s / %s, %s | %s, %s | %s / %s (%s) | %s | %s |" % (
            sz, d, sig(g(ct, "planted", "geo")), sig(g(ct, "others_geo_max")),
            "%s of %s" % (g(sm, "n_over4"), g(sm, "n_sweeps")), persistent_text(sm),
            "%d / %d" % (ev_sum(r, "CONGRUENCE"), ev_sum(r, "CONGRUENCE", "ctl")),
            g(rd, "attrib_sta_rows"), g(rd, "attrib_next_station_rows"), fnum(g(rd, "attrib_first_over10_min"), 1) if g(rd, "attrib_first_over10_min") is not None else "-",
            len(g(rd, "spiked_by_hour") or {}) and sum((g(rd, "spiked_by_hour") or {}).values()), fnum(g(rd, "spiked_first_min"), 1) if g(rd, "spiked_first_min") is not None else "-",
            g(gd, "planted"), g(gd, "others"), g(gd, "n_other_stations"),
            fnum(100 * g(rows_h, "frac_out", default=float("nan")), 0), warn))
        pa, pc = g(r, "station", "pf_arm"), g(r, "station", "pf_ctl")
        c.append("| %s | %s | %s | %s | %s / %s (%s) | %s / %s (%s) |" % (
            sz, d, sig(g(r, "station", "zwd", hk, "dzwd_mean_mm")), sig(g(r, "station", "rxclk_rel", hk, "own_minus_others_median_mm")),
            sig(g(pa, hk, "rms_code_mm")), sig(g(pc, hk, "rms_code_mm")), g(pa, hk, "code_rank"),
            sig(g(pa, hk, "rms_phase_mm")), sig(g(pc, hk, "rms_phase_mm")), g(pa, hk, "phase_rank")))
    # D: by hour, for the arms long enough to have settled hours
    d = []
    long_ = [(k, t) for k, t in rows if g(res[t], "consumer", "h6") is not None]
    if long_:
        d = ["| size | dir | quantity | " + " | ".join(f"h{h}" for h in range(1, 7)) + " |", "| --- | --- | --- | " + " | ".join("---" for _ in range(6)) + " |"]
        for (dr, sz, _), t in long_:
            r = res[t]
            cn = r["consumer"]
            d.append(f"| {sz} | {dr} | consumer 3D p50, arm / control (mm) | " + " | ".join(f"{sig(g(cn, f'h{h}', 'p50_arm_mm'))} / {sig(g(cn, f'h{h}', 'p50_ctl_mm'))}" for h in range(1, 7)) + " |")
            d.append(f"| {sz} | {dr} | consumer shift, median (mm) | " + " | ".join(sig(g(cn, f"h{h}", "shift_med_mm")) for h in range(1, 7)) + " |")
            d.append(f"| {sz} | {dr} | satellite clock RMS (mm) | " + " | ".join(sig(g(r, "products", "CLK", f"h{h}", "rms_med_mm")) for h in range(1, 7)) + " |")
            d.append(f"| {sz} | {dr} | phase bias RMS (mm) | " + " | ".join(sig(g(r, "products", "PB", f"h{h}", "rms_med_mm")) for h in range(1, 7)) + " |")
            cg = g(r, "screens", "congruence", "arm") or {}
            d.append(f"| {sz} | {dr} | congruence median t at the station (rank of 60) | " + " | ".join(f"{sig(g(cg, f'h{h}', 't_med'))} ({g(cg, f'h{h}', 'rank_by_t_med')})" for h in range(1, 7)) + " |")
            ab = g(r, "screens", "raim_detail", "arm", "attrib_by_hour") or {}
            d.append(f"| {sz} | {dr} | RAIM attribution rows at the station | " + " | ".join(str(ab.get(f"h{h}", "-")) for h in range(1, 7)) + " |")
            d.append(f"| {sz} | {dr} | own post-fit phase RMS, arm / control (mm) | " + " | ".join(f"{sig(g(r, 'station', 'pf_arm', f'h{h}', 'rms_phase_mm'))} / {sig(g(r, 'station', 'pf_ctl', f'h{h}', 'rms_phase_mm'))}" for h in range(1, 7)) + " |")
    out = ["A. What the planted size does to the wire and to the held-out consumer"] + a + ["", "B. Which screen reacts, and whether it names the station"] + b + ["", "C. What the planted station absorbs"] + c
    if d:
        out += ["", "D. By hour (6 h arms)"] + d
    return "\n".join(out)


# ----------------------------------------------------------------------------- driver
def read_arm(ctl, arm, st, cons):
    tr = csv(ctl, "closed_loop_trace.csv")
    CAP[0] = max(int(math.ceil((float(tr["tow"].max()) - float(tr["tow"].min())) / H - 1e-9)) - 1, 0)
    r = {"meta": run_meta(arm), "ctl_meta": run_meta(ctl)}
    r["consumer"] = consumer(ctl, arm)
    r["products"] = products(ctl, arm, st)
    r["station"] = station_block(ctl, arm, st)
    r["screens"] = screens(ctl, arm, st)
    r["stdout"] = stdout_block(ctl, arm, st)
    return r


def clean(o):
    if isinstance(o, dict):
        return {str(k): clean(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [clean(v) for v in o]
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.floating, float)):
        return None if not math.isfinite(float(o)) else float(o)
    return o


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("arm")
    a.add_argument("ctl")
    a.add_argument("arm")
    a.add_argument("--station", default="S48000SIM")
    a.add_argument("--consumer", default="SZZ000SIM")
    a.add_argument("--json")
    cd = sub.add_parser("card")
    cd.add_argument("json")
    cd.add_argument("--hour", type=int, default=2)
    rd = sub.add_parser("render")
    rd.add_argument("json")
    rd.add_argument("--hour", type=int, default=2)
    t = sub.add_parser("table")
    t.add_argument("ctl")
    t.add_argument("arms", nargs="+")
    t.add_argument("--station", default="S48000SIM")
    t.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "arm":
        r = clean(read_arm(args.ctl, args.arm, args.station, args.consumer))
        txt = json.dumps(r, indent=1)
        if args.json:
            open(args.json, "w").write(txt)
        else:
            print(txt)
    elif args.cmd == "card":
        print(card_tables(json.load(open(args.json)), args.hour))
    elif args.cmd == "render":
        print(render(json.load(open(args.json)), args.hour))
    else:
        res = {os.path.basename(x): clean(read_arm(args.ctl, x, args.station, "")) for x in args.arms}
        if args.json:
            json.dump(res, open(args.json, "w"), indent=1)
        print(f"{len(res)} arm(s) read")


if __name__ == "__main__":
    main()
