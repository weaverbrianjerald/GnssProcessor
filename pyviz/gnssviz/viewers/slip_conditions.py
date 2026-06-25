"""Slip-condition exhibits viewer -- the port of ``view_slip_conditions.m``.

Demonstrates the four Melbourne-Wubbena (MW) x geometry-free (GF) cycle-slip
detection conditions on real data, as joint four-panel exhibits.  It merges the
example-figure machinery of the MW wide-lane viewer (``mwwl_trace.csv``) and the
TurboEdit GF TD2 viewer (``turbo_edit_trace.csv``) keyed on the SAME
``(satellite, signal-pair, epoch)`` so the two detectors are read side by side.

The 2x2 conditions (each detector's OWN gate decides "detected"):
  1. MW detected, GF stable   -- WL/code step the phase-GF doesn't see
  2. MW stable,  GF detected   -- e.g. ``dN1=dN2`` (WL unchanged, GF jumps)
  3. MW stable,  GF stable     -- clean control window
  4. MW detected, GF detected  -- unambiguous dual-detector slip

* "MW detected" == ``mwwl_trace.is_slip == 1`` (``|dev_active|/threshold > 1``).
* "GF detected" == ``turbo_edit_trace.gf_flagged == 1`` (``|TD2|/thresh > 1``,
  past the anti-echo cooldown).

These are the two DETECTORS' decisions.  ``turbo_edit``'s internal ``mw_flagged``
is NOT used for classification: it fires on sub-threshold MW wiggle
(``is_slip=0``) and would mislabel a real MW detection as "stable".

Events are formed by clustering MW and GF flags on the same ``(sat, pair)`` that
fall within ``cluster_win_s`` of each other -- the two detectors typically fire
an epoch or two apart on the same physical slip and must be counted as ONE event
to land in condition 4.

Each exhibit is a four-panel figure (rendered as a vertical stack, since the
backend lays axes out top-to-bottom rather than in a 2x2 grid -- the closest
available layout to the MATLAB ``tiledlayout(2,2)``):

* MW raw wide-lane + reference levels
* MW active deviation vs +/- threshold
* GF raw geometry-free phase (+ repaired overlay)
* GF second time-difference (TD2) vs +/- threshold

with the slip epoch starred, a dotted detection epoch line, and a solid
action-coloured line at the resolved-action epoch.  The figure title carries the
condition label / sat / pair / epoch and the MW & GF detection statistics and the
resolved TurboEdit action (matching the MATLAB ``title`` / ``subtitle``).
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace
from ..figures import Axes, Figure, Legend, Line, Scatter, TextBox, VLine
from .base import Viewer, ViewerResult

# --- Line colours (RGB, matching the MATLAB plot specs) ---
_C_RAW = (0.0, 0.35, 0.75)        # MW/GF raw + active deviation (blue, '-o')
_C_HATCH = (0.85, 0.55, 0.0)      # Hatch mean (orange)
_C_LOCAL = (0.2, 0.6, 0.2)        # local median (green)
_C_THRESH = (0.85, 0.0, 0.0)      # +/- threshold (red dashed)
_C_REPAIR = (0.75, 0.0, 0.1)      # repaired GF overlay (dark red dashed)
_C_STAR = (1.0, 0.85, 0.0)        # flagged-sample star fill (yellow)
_C_DETECT = (0.15, 0.15, 0.15)    # detection-epoch dotted vline (dark grey)

# Action-coloured vline (matching ``actionColor``).
_ACTION_COLORS = {
    "repair": (0.00, 0.60, 0.20),
    "reset": (0.85, 0.00, 0.85),
    "defer": (0.90, 0.50, 0.00),
    "moot": (0.50, 0.50, 0.50),
    "code_event": (0.20, 0.40, 0.70),
}
_ACTION_DEFAULT = (0.30, 0.30, 0.30)

# Action-row selection priority (real outcome over the 'none' default).
_ACTION_PRIORITY = ("repair", "reset", "defer", "moot", "code_event", "none")

# Condition labels (1/2/4 from clustering; 3 from clean windows; 5 = repair).
_COND_LABELS = {
    1: "1: MW detected, GF stable",
    2: "2: MW stable, GF detected",
    3: "3: MW stable, GF stable",
    4: "4: MW detected, GF detected",
    5: "5: Repair (TE action)",
}


def _action_color(act: str):
    return _ACTION_COLORS.get(str(act), _ACTION_DEFAULT)


def _mround(x: float) -> int:
    """MATLAB ``round`` (half away from zero)."""
    return int(np.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


# --------------------------------------------------------------------------- #
#  Data loading
# --------------------------------------------------------------------------- #
def _load_mw(path: str) -> pd.DataFrame:
    Tmw = load_trace(path)
    need = ["week", "tow_s", "satellite", "system", "sig1", "sig2",
            "mw_cycles", "dev_from_mean_cycles", "threshold_cycles"]
    miss = [c for c in need if c not in Tmw.columns]
    if miss:
        raise ValueError(f"mwwl_trace.csv missing: {', '.join(miss)}")

    Tmw = Tmw.copy()
    Tmw["satellite"] = Tmw["satellite"].astype(str)
    Tmw["system"] = Tmw["system"].astype(str)
    Tmw["sig1"] = Tmw["sig1"].astype(str)
    Tmw["sig2"] = Tmw["sig2"].astype(str)
    Tmw["pair"] = Tmw["sig1"] + "/" + Tmw["sig2"]
    Tmw = add_utc(Tmw)

    for c in ("mw_cycles", "mw_mean_cycles", "local_ref_cycles",
              "dev_from_mean_cycles", "threshold_cycles", "active_dev_cycles",
              "is_slip", "arc_count"):
        if c in Tmw.columns:
            Tmw[c] = pd.to_numeric(Tmw[c], errors="coerce")

    if "is_slip" not in Tmw.columns:
        raise ValueError("mwwl_trace.csv has no is_slip column -- cannot "
                         "classify MW detections.")
    Tmw["is_slip"] = Tmw["is_slip"].fillna(0.0)
    if "arc_count" not in Tmw.columns:
        Tmw["arc_count"] = 2.0    # unknown -> treat as in-arc

    # Active deviation the MW detector gated.  Direct path (active_dev_cycles);
    # fall back to dev_from_mean on the NaN (init/LLI) rows.
    if "active_dev_cycles" in Tmw.columns:
        da = Tmw["active_dev_cycles"].to_numpy(float)
        nf = ~np.isfinite(da)
        da[nf] = Tmw["dev_from_mean_cycles"].to_numpy(float)[nf]
    else:
        da = Tmw["dev_from_mean_cycles"].to_numpy(float)
    Tmw["dev_active_cycles"] = da
    return Tmw


def _load_te(path: str) -> pd.DataFrame:
    Tte = load_trace(path)
    need = ["week", "tow_s", "satellite", "system", "sig1", "sig2",
            "gf_m", "td2_gf_m", "thresh_m", "gf_flagged", "action"]
    miss = [c for c in need if c not in Tte.columns]
    if miss:
        raise ValueError(f"turbo_edit_trace.csv missing: {', '.join(miss)}")

    Tte = Tte.copy()
    Tte["satellite"] = Tte["satellite"].astype(str)
    Tte["system"] = Tte["system"].astype(str)
    Tte["sig1"] = Tte["sig1"].astype(str)
    Tte["sig2"] = Tte["sig2"].astype(str)
    Tte["pair"] = Tte["sig1"] + "/" + Tte["sig2"]
    Tte = add_utc(Tte)

    for c in ("gf_m", "td2_gf_m", "thresh_m", "gf_flagged", "mw_flagged",
              "dN1", "dN2", "frac", "lambda1_m", "lambda2_m"):
        if c in Tte.columns:
            Tte[c] = pd.to_numeric(Tte[c], errors="coerce")

    Tte["gf_flagged"] = Tte["gf_flagged"].fillna(0.0)
    # -1 sentinel = immature MAD -> NaN
    th = Tte["thresh_m"].to_numpy(float)
    th[th < 0] = np.nan
    Tte["thresh_m"] = th
    if "action" not in Tte.columns:
        Tte["action"] = ""
    Tte["action"] = Tte["action"].astype(str)
    return Tte


# --------------------------------------------------------------------------- #
#  Event clustering / classification
# --------------------------------------------------------------------------- #
def _cluster_events(E: pd.DataFrame, cluster_win_s: float) -> list:
    """Cluster flag rows per (sat,pair) into events.

    Flags within ``cluster_win_s`` seconds of each other on the same (sat,pair)
    belong to one event.  Returns a list of dicts (sat, pair, system, t0, hasMW,
    hasGF, rank, inArc).
    """
    out: list = []
    if E.empty:
        return out
    for (sat, pair), sub in E.groupby(["sat", "pair"], sort=False):
        sub = sub.sort_values("utc").reset_index(drop=True)
        utc = sub["utc"].to_numpy()
        dt = np.diff(utc).astype("timedelta64[ns]").astype(float) / 1e9
        new_cluster = np.concatenate(([True], dt > cluster_win_s))
        cid = np.cumsum(new_cluster)
        for c in range(1, int(cid.max()) + 1):
            rows = sub[cid == c]
            mwR = rows[rows["src"] == "mw"]
            gfR = rows[rows["src"] == "gf"]
            hasMW = not mwR.empty
            hasGF = not gfR.empty
            rmw, tmw = 0.0, None
            if hasMW:
                im = mwR["ratio"].to_numpy(float).argmax()
                rmw = float(mwR["ratio"].to_numpy(float)[im])
                tmw = mwR["utc"].to_numpy()[im]
            rgf, tgf = 0.0, None
            if hasGF:
                ig = gfR["ratio"].to_numpy(float).argmax()
                rgf = float(gfR["ratio"].to_numpy(float)[ig])
                tgf = gfR["utc"].to_numpy()[ig]
            # t0 anchors on the detector that fired; for a dual-detector event
            # prefer the MW step (sharper in time).
            t0 = tmw if hasMW else tgf
            in_arc = bool(hasMW and (mwR["arc"].to_numpy(float) > 1).any())
            out.append({
                "sat": rows["sat"].iloc[0], "pair": rows["pair"].iloc[0],
                "system": rows["system"].iloc[0], "t0": pd.Timestamp(t0),
                "hasMW": hasMW, "hasGF": hasGF, "rank": max(rmw, rgf),
                "inArc": in_arc,
            })
    return out


def _pick_distinct_sat(cand: list, n: int) -> list:
    """Greedily take highest-ranked candidates, preferring a new satellite each
    pick, then fill any remainder in rank order."""
    pick: list = []
    used: set = set()
    for c in cand:
        if len(pick) >= n:
            break
        if c["sat"] not in used:
            pick.append(c)
            used.add(c["sat"])
    if len(pick) < n:
        for c in cand:
            if len(pick) >= n:
                break
            if c not in pick:
                pick.append(c)
    return pick


def _sample_clean_windows(Tmw, Tte, mwSlip, gfFlag, window_s, n, avoid_sats) -> list:
    """Pick (sat,pair) mid-arc epochs where NEITHER detector fires within
    +/- window_s, preferring sats not already used and with data in both
    traces.  Returns a list (possibly shorter than n)."""
    clean: list = []
    keys = (Tte["satellite"] + "|" + Tte["pair"])
    cnt = keys.value_counts()        # most-populated first
    used = set(str(s) for s in avoid_sats)
    win = np.timedelta64(int(window_s), "s")
    for uk in cnt.index:
        if len(clean) >= n:
            break
        sat, pair = str(uk).split("|", 1)
        if sat in used:
            continue
        teRows = Tte[(Tte["satellite"] == sat) & (Tte["pair"] == pair)]
        mwRows = Tmw[(Tmw["satellite"] == sat) & (Tmw["pair"] == pair)]
        if len(teRows) < 30 or len(mwRows) < 30:
            continue
        teRows = teRows.sort_values("utc").reset_index(drop=True)
        # mid-arc (MATLAB round of height/2, 1-based -> 0-based)
        mid = _mround(len(teRows) / 2.0) - 1
        mid = min(max(mid, 0), len(teRows) - 1)
        t0 = teRows["utc"].iloc[mid]
        mwBad = bool((((mwSlip["utc"] - t0).abs() <= win)
                      & (mwSlip["satellite"] == sat)
                      & (mwSlip["pair"] == pair)).any())
        gfBad = bool((((gfFlag["utc"] - t0).abs() <= win)
                      & (gfFlag["satellite"] == sat)
                      & (gfFlag["pair"] == pair)).any())
        if mwBad or gfBad:
            continue
        clean.append({"sat": sat, "pair": pair,
                      "system": teRows["system"].iloc[0], "t0": t0})
        used.add(sat)
    return clean


# --------------------------------------------------------------------------- #
#  Gap-broken line helper
# --------------------------------------------------------------------------- #
def _insert_gap_breaks(t, y, gap_factor):
    """Insert NaT/NaN at gaps > gap_factor*median(dt) so the line breaks."""
    t = pd.to_datetime(np.asarray(t))
    y = np.asarray(y, dtype=float)
    if t.size < 2:
        return t, y
    order = np.argsort(t.values if hasattr(t, "values") else t)
    tS = t[order]
    yS = y[order]
    dt = np.diff(tS).astype("timedelta64[ns]").astype(float) / 1e9
    md = np.median(dt)
    if not np.isfinite(md) or md <= 0:
        md = 1.0
    gaps = np.flatnonzero(dt > gap_factor * md)
    if gaps.size == 0:
        return tS, yS
    t_out, y_out = [], []
    r0 = 0
    for gi in gaps:
        t_out.extend(list(tS[r0:gi + 1]))
        y_out.extend(list(yS[r0:gi + 1]))
        t_out.append(np.datetime64("NaT"))
        y_out.append(np.nan)
        r0 = gi + 1
    t_out.extend(list(tS[r0:]))
    y_out.extend(list(yS[r0:]))
    return pd.to_datetime(np.array(t_out)), np.array(y_out, dtype=float)


def _gb_line(t, y, gap_factor, style, color, label, show_in_legend=True):
    tt, yy = _insert_gap_breaks(t, y, gap_factor)
    return Line(x=tt, y=yy, color=color, width=0.9, style=style,
                label=label, show_in_legend=show_in_legend)


def _star_flags(W, flag_times, col):
    """Star the flagged samples of column ``col`` within window ``W``."""
    if W.empty or len(flag_times) == 0:
        return None
    xs, ys = [], []
    wutc = W["utc"].to_numpy()
    vals = W[col].to_numpy(float)
    for ft in flag_times:
        r = np.flatnonzero(wutc == np.datetime64(ft))
        if r.size:
            xs.append(W["utc"].iloc[int(r[0])])
            ys.append(vals[int(r[0])])
    if not xs:
        return None
    return Scatter(x=xs, y=ys, color=_C_STAR, size=90.0, marker="*",
                   show_in_legend=False)


def _median_dt(utc_arr):
    u = np.unique(np.asarray(utc_arr))
    if u.size < 2:
        return 1.0
    d = np.diff(np.sort(u)).astype("timedelta64[ns]").astype(float) / 1e9
    d = d[d > 0]
    if d.size == 0:
        return 1.0
    md = float(np.median(d))
    return md if (np.isfinite(md) and md > 0) else 1.0


def _pick_flag_row(utc, flag_mask, t0):
    """Index of the flagged row nearest t0; if none flagged, the nearest row."""
    utc = np.asarray(utc)
    t0 = np.datetime64(pd.Timestamp(t0))
    if np.any(flag_mask):
        fi = np.flatnonzero(flag_mask)
        j = np.abs((utc[fi] - t0).astype("timedelta64[ns]").astype(float)).argmin()
        return int(fi[j])
    return int(np.abs((utc - t0).astype("timedelta64[ns]").astype(float)).argmin())


def _pick_action_row(actions, utc, t0):
    """Index of the resolved-action row nearest t0, preferring a real outcome."""
    actions = np.asarray(actions, dtype=object)
    utc = np.asarray(utc)
    t0 = np.datetime64(pd.Timestamp(t0))
    for p in _ACTION_PRIORITY:
        c = np.flatnonzero(actions == p)
        if c.size:
            j = np.abs((utc[c] - t0).astype("timedelta64[ns]").astype(float)).argmin()
            return int(c[j])
    return int(np.abs((utc - t0).astype("timedelta64[ns]").astype(float)).argmin())


# --------------------------------------------------------------------------- #
#  Exhibit construction (one 4-axis figure per selected event)
# --------------------------------------------------------------------------- #
def _epoch_vlines(t_det, t_act, act, do_label):
    """Dotted detection-epoch line + solid action-coloured line (when a real
    action lands a measurable lag after detection)."""
    out = [VLine(x=t_det, color=_C_DETECT, style=":", width=1.0)]
    if (t_act is not None and not pd.isna(t_act) and act not in ("none", "")
            and abs((pd.Timestamp(t_act) - pd.Timestamp(t_det)).total_seconds()) > 0.001):
        col = _action_color(act)
        out.append(VLine(x=t_act, color=col, style="-", width=1.3,
                         label=(str(act) if do_label else None)))
    return out


def _build_exhibit(Tmw, Tte, ev, window_s, gap_factor) -> Figure:
    sat, pair, t0 = ev["sat"], ev["pair"], pd.Timestamp(ev["t0"])
    win = np.timedelta64(int(window_s), "s")

    Wmw = Tmw[(Tmw["satellite"] == sat) & (Tmw["pair"] == pair)
              & ((Tmw["utc"] - t0).abs() <= win)].sort_values("utc").reset_index(drop=True)
    Wte = Tte[(Tte["satellite"] == sat) & (Tte["pair"] == pair)
              & ((Tte["utc"] - t0).abs() <= win)].sort_values("utc").reset_index(drop=True)

    # MW detection statistic.
    mw_str = "MW: n/a"
    if not Wmw.empty:
        im = _pick_flag_row(Wmw["utc"], Wmw["is_slip"].to_numpy() == 1, t0)
        dv = float(Wmw["dev_active_cycles"].to_numpy(float)[im])
        th = float(Wmw["threshold_cycles"].to_numpy(float)[im])
        mw_str = (f"MW |dev|/thr={abs(dv)/th:.1f} "
                  f"(dev={dv:+.2f}, thr={th:.2f} cyc)") if th not in (0.0,) and np.isfinite(th) \
            else f"MW |dev|/thr=nan (dev={dv:+.2f}, thr={th:.2f} cyc)"

    # GF detection statistic.
    gf_str = "GF: n/a"
    if not Wte.empty:
        ig = _pick_flag_row(Wte["utc"], Wte["gf_flagged"].to_numpy() == 1, t0)
        td = float(Wte["td2_gf_m"].to_numpy(float)[ig])
        tt = float(Wte["thresh_m"].to_numpy(float)[ig])
        gf_str = f"GF |TD2|/thr={abs(td)/tt:.1f} (TD2={td:+.3f}, thr={tt:.3f} m)" \
            if (np.isfinite(tt) and tt != 0) \
            else f"GF |TD2|/thr=nan (TD2={td:+.3f}, thr={tt:.3f} m)"

    # Resolved TurboEdit action (prefer a real outcome over none/code_event).
    act, t_act = "", None
    dN1 = dN2 = l1 = l2 = np.nan
    if not Wte.empty:
        ia = _pick_action_row(Wte["action"].to_numpy(object), Wte["utc"], t0)
        act = str(Wte["action"].iloc[ia])
        t_act = pd.Timestamp(Wte["utc"].iloc[ia])
        if "dN1" in Wte.columns:
            dN1 = float(Wte["dN1"].iloc[ia])
            dN2 = float(Wte["dN2"].iloc[ia])
        if "lambda1_m" in Wte.columns:
            l1 = float(Wte["lambda1_m"].iloc[ia])
            l2 = float(Wte["lambda2_m"].iloc[ia])

    # Detection epoch (earliest flag) and detect->action lag in epochs.
    mw_flag_t = Wmw.loc[Wmw["is_slip"] == 1, "utc"] if not Wmw.empty else pd.Series([], dtype="datetime64[ns]")
    gf_flag_t = Wte.loc[Wte["gf_flagged"] == 1, "utc"] if not Wte.empty else pd.Series([], dtype="datetime64[ns]")
    det_times = pd.concat([mw_flag_t, gf_flag_t])
    t_detect = pd.Timestamp(det_times.min()) if not det_times.empty else t0

    act_str = "n/a"
    lag_str = ""
    if act != "":
        if (t_act is not None and not pd.isna(t_act) and act != "none"
                and t_act > t_detect):
            all_utc = np.concatenate([Wmw["utc"].to_numpy(), Wte["utc"].to_numpy()]) \
                if (not Wmw.empty or not Wte.empty) else np.array([], dtype="datetime64[ns]")
            md = _median_dt(all_utc)
            lag_ep = _mround((pd.Timestamp(t_act) - t_detect).total_seconds() / md)
            lag_str = f"  +{lag_ep}ep lag"
        if np.isfinite(dN1) and (dN1 != 0 or dN2 != 0):
            act_str = f"{act} (dN1={_mround(dN1):+d}, dN2={_mround(dN2):+d}){lag_str}"
        else:
            act_str = f"{act}{lag_str}"

    title = (f"Condition {ev['condLabel']}   |   {sat} {pair} @ {t0}")
    subtitle = f"{mw_str}     {gf_str}     TE: action={act_str}"

    mw_flag_times = list(mw_flag_t.to_numpy())
    gf_flag_times = list(gf_flag_t.to_numpy())

    # --- Axis 1: MW raw wide-lane + reference levels ---
    ax1 = Axes(title="MW raw wide-lane", ylabel="MW WL (cyc)",
               legend=Legend(loc="best"))
    if not Wmw.empty:
        ax1.traces.append(_gb_line(Wmw["utc"], Wmw["mw_cycles"], gap_factor,
                                   "-o", _C_RAW, "MW raw"))
        if "mw_mean_cycles" in Wmw.columns:
            ax1.traces.append(_gb_line(Wmw["utc"], Wmw["mw_mean_cycles"],
                                       gap_factor, "-", _C_HATCH, "Hatch mean"))
        if "local_ref_cycles" in Wmw.columns:
            ax1.traces.append(_gb_line(Wmw["utc"], Wmw["local_ref_cycles"],
                                       gap_factor, "-", _C_LOCAL, "local median"))
        star = _star_flags(Wmw, mw_flag_times, "mw_cycles")
        if star is not None:
            ax1.traces.append(star)
    else:
        ax1.traces.append(TextBox(x=0.5, y=0.5, text="no MW data", ha="center"))
    ax1.traces.extend(_epoch_vlines(t_detect, t_act, act, do_label=True))

    # --- Axis 2: MW gate (active deviation vs +/- threshold) ---
    ax2 = Axes(title="MW active deviation vs gate", xlabel="GPS Time",
               ylabel="dev_active (cyc)")
    if not Wmw.empty:
        ax2.traces.append(_gb_line(Wmw["utc"], Wmw["dev_active_cycles"],
                                   gap_factor, "-o", _C_RAW, "dev_active"))
        ax2.traces.append(_gb_line(Wmw["utc"], Wmw["threshold_cycles"],
                                   gap_factor, "--", _C_THRESH, "+/- threshold"))
        ax2.traces.append(_gb_line(Wmw["utc"], -Wmw["threshold_cycles"].to_numpy(float),
                                   gap_factor, "--", _C_THRESH, None,
                                   show_in_legend=False))
        star = _star_flags(Wmw, mw_flag_times, "dev_active_cycles")
        if star is not None:
            ax2.traces.append(star)
    ax2.traces.extend(_epoch_vlines(t_detect, t_act, act, do_label=False))

    # --- Axis 3: GF raw + repaired overlay ---
    ax3 = Axes(title="GF raw geometry-free phase", ylabel="GF (m)",
               legend=Legend(loc="best"))
    if not Wte.empty:
        ax3.traces.append(_gb_line(Wte["utc"], Wte["gf_m"], gap_factor,
                                   "-o", _C_RAW, "GF raw"))
        if (act == "repair" and np.isfinite(dN1) and np.isfinite(l1)
                and (dN1 != 0 or dN2 != 0)):
            step_m = l1 * dN1 - l2 * dN2
            post = Wte["utc"] >= t_detect
            ax3.traces.append(_gb_line(
                Wte.loc[post, "utc"], Wte.loc[post, "gf_m"].to_numpy(float) - step_m,
                gap_factor, "--", _C_REPAIR, f"repaired: GF-({step_m:+.3f} m)"))
        star = _star_flags(Wte, gf_flag_times, "gf_m")
        if star is not None:
            ax3.traces.append(star)
    else:
        ax3.traces.append(TextBox(x=0.5, y=0.5, text="no GF data", ha="center"))
    ax3.traces.extend(_epoch_vlines(t_detect, t_act, act, do_label=False))

    # --- Axis 4: GF gate (TD2 vs +/- threshold) ---
    ax4 = Axes(title="GF second time-difference vs gate", xlabel="GPS Time",
               ylabel="TD2_GF (m)")
    if not Wte.empty:
        ax4.traces.append(_gb_line(Wte["utc"], Wte["td2_gf_m"], gap_factor,
                                   "-o", _C_RAW, "TD2_GF"))
        ax4.traces.append(_gb_line(Wte["utc"], Wte["thresh_m"], gap_factor,
                                   "--", _C_THRESH, "+/- threshold"))
        ax4.traces.append(_gb_line(Wte["utc"], -Wte["thresh_m"].to_numpy(float),
                                   gap_factor, "--", _C_THRESH, None,
                                   show_in_legend=False))
        star = _star_flags(Wte, gf_flag_times, "td2_gf_m")
        if star is not None:
            ax4.traces.append(star)
    ax4.traces.extend(_epoch_vlines(t_detect, t_act, act, do_label=False))

    # x-limits anchored on the MW window (matching the MATLAB linkaxes + xlim).
    if not Wmw.empty:
        xl = (pd.Timestamp(Wmw["utc"].min()), pd.Timestamp(Wmw["utc"].max()))
        for a in (ax1, ax2, ax3, ax4):
            a.xlim = xl

    return Figure(name=f"Slip condition {ev['condNum']}", title=title,
                  subtitle=subtitle, axes=[ax1, ax2, ax3, ax4],
                  width=11.0, height=10.0)


# --------------------------------------------------------------------------- #
#  Top-level build
# --------------------------------------------------------------------------- #
def build_slip_conditions(ctx, n_per_condition: int = 2, n_repair_examples: int = 1,
                          window_sec: int = 60, cluster_win_s: float = 5.0,
                          gap_factor: float = 3.0, prefer_in_arc: bool = True) -> ViewerResult:
    """Build the slip-condition exhibits.

    Parameters default to the MATLAB script's config block:
    ``nPerCondition=2``, ``nRepairExamples=1``, ``windowSec=60``,
    ``clusterWinS=5``, ``gapFactor=3``, ``preferInArc=true``.
    """
    Tmw = _load_mw(ctx.path("mwwl_trace.csv"))
    Tte = _load_te(ctx.path("turbo_edit_trace.csv"))

    # --- Per-detector firing event rows ---
    mwSlip = Tmw[Tmw["is_slip"] == 1]
    gfFlag = Tte[Tte["gf_flagged"] == 1]

    mw_ratio = np.abs(mwSlip["dev_active_cycles"].to_numpy(float)) \
        / mwSlip["threshold_cycles"].to_numpy(float)
    mwEv = pd.DataFrame({
        "sat": mwSlip["satellite"].to_numpy(), "pair": mwSlip["pair"].to_numpy(),
        "system": mwSlip["system"].to_numpy(), "utc": mwSlip["utc"].to_numpy(),
        "ratio": mw_ratio, "arc": mwSlip["arc_count"].to_numpy(float),
        "src": np.repeat("mw", len(mwSlip)),
    })
    gf_ratio = np.abs(gfFlag["td2_gf_m"].to_numpy(float)) \
        / gfFlag["thresh_m"].to_numpy(float)
    gfEv = pd.DataFrame({
        "sat": gfFlag["satellite"].to_numpy(), "pair": gfFlag["pair"].to_numpy(),
        "system": gfFlag["system"].to_numpy(), "utc": gfFlag["utc"].to_numpy(),
        "ratio": gf_ratio, "arc": np.full(len(gfFlag), np.nan),
        "src": np.repeat("gf", len(gfFlag)),
    })
    E = pd.concat([mwEv, gfEv], ignore_index=True)
    E["ratio"] = E["ratio"].to_numpy(float)
    E.loc[~np.isfinite(E["ratio"]), "ratio"] = 0.0

    # --- Cluster + classify into conditions 1/2/4 ---
    C = _cluster_events(E, cluster_win_s)
    for c in C:
        if c["hasMW"] and c["hasGF"]:
            c["condNum"] = 4
        elif c["hasMW"] and not c["hasGF"]:
            c["condNum"] = 1
        elif (not c["hasMW"]) and c["hasGF"]:
            c["condNum"] = 2
        else:
            c["condNum"] = 0

    selected: list = []
    log = ["=== Slip-condition inventory ==="]
    for cn in (1, 2, 4):
        cand = [c for c in C if c["condNum"] == cn]
        if not cand:
            log.append(f"Condition {_COND_LABELS[cn]} : 0 found")
            continue
        # Sort: in-arc first (if preferred), then by exceedance strength.
        if prefer_in_arc:
            cand = sorted(cand, key=lambda x: (-int(x["inArc"]), -x["rank"]))
        else:
            cand = sorted(cand, key=lambda x: -x["rank"])
        pick = _pick_distinct_sat(cand, n_per_condition)
        log.append(f"Condition {_COND_LABELS[cn]} : {len(cand)} found, "
                   f"showing {len(pick)}")
        for e in pick:
            selected.append({"sat": e["sat"], "pair": e["pair"],
                             "system": e["system"], "t0": e["t0"], "condNum": cn,
                             "condLabel": _COND_LABELS[cn], "rank": e["rank"]})

    # --- Condition 3: clean windows (both detectors quiet) ---
    avoid = [s["sat"] for s in selected]
    clean = _sample_clean_windows(Tmw, Tte, mwSlip, gfFlag, window_sec,
                                  n_per_condition, avoid)
    log.append(f"Condition 3: MW stable, GF stable : {len(clean)} clean "
               f"window(s) sampled")
    for k in clean:
        selected.append({"sat": k["sat"], "pair": k["pair"], "system": k["system"],
                         "t0": k["t0"], "condNum": 3,
                         "condLabel": _COND_LABELS[3], "rank": 0.0})

    # --- Repair examples (action == "repair") ---
    repMask = Tte["action"] == "repair"
    if repMask.any():
        rr = Tte[repMask]
        rep_ratio = np.abs(rr["td2_gf_m"].to_numpy(float)) / rr["thresh_m"].to_numpy(float)
        repEv = pd.DataFrame({
            "sat": rr["satellite"].to_numpy(), "pair": rr["pair"].to_numpy(),
            "system": rr["system"].to_numpy(), "utc": rr["utc"].to_numpy(),
            "ratio": rep_ratio, "arc": np.full(len(rr), np.nan),
            "src": np.repeat("gf", len(rr)),
        })
        repEv.loc[~np.isfinite(repEv["ratio"]), "ratio"] = 0.0
        Crep = _cluster_events(repEv, cluster_win_s)
        Crep = sorted(Crep, key=lambda x: -x["rank"])
        n_show = min(n_repair_examples, len(Crep))
        log.append(f'Repair (TE action="repair") : {len(Crep)} found, '
                   f"showing {n_show}")
        for e in Crep[:n_show]:
            selected.append({"sat": e["sat"], "pair": e["pair"],
                             "system": e["system"], "t0": e["t0"], "condNum": 5,
                             "condLabel": _COND_LABELS[5], "rank": e["rank"]})
    else:
        log.append('Repair (TE action="repair") : 0 found')

    # --- Render exhibits (sorted by condition number) ---
    figures = []
    if not selected:
        log.append("\nNo events to display.")
    else:
        selected = sorted(selected, key=lambda s: s["condNum"])
        for s in selected:
            figures.append(_build_exhibit(Tmw, Tte, s, window_sec, gap_factor))
        log.append(f"\nRendered {len(selected)} condition exhibit(s).")

    return ViewerResult(figures=figures, summary="\n".join(log))


viewer = Viewer(name="slip_conditions", title="MW x GF slip-condition exhibits",
                required_csvs=("mwwl_trace.csv", "turbo_edit_trace.csv"),
                build=build_slip_conditions)
