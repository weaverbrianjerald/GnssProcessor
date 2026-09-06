"""Per-satellite phase-ambiguity LOCKTIME strip viewer -- the port of
``view_ambiguity_locktime.m``.

How long (minutes) each carrier-phase ambiguity has held since it was last
(re)initialized.  Styled after the shared (satellite x epoch) strip renderer:
one figure per constellation, one tile per phase signal, cells coloured by
locktime.  Fresh resets render at the dark/low end of ``viridis`` (salient
against the background); mature locks recede to the bright/high end -- so a
cycle-slip / outlier path that resets ambiguities too often paints the strip
with hot stripes, while a healthy run is long cool sweeps.

WHY THIS IS RECONSTRUCTED FROM STATE, NOT FROM THE SLIP TRACES
-------------------------------------------------------------
An ambiguity is reinitialized by TWO independent code paths:

  (1) the cycle-slip pipeline (MwSlipDetector / TurboEdit ->
      ``CycleSlipManager::resetAmbiguityBssdAware`` ->
      ``StateManager::resetAmbiguity``), and
  (2) the Kalman filter's post-fit measurement-residual outlier check
      (``KalmanFilter::handlePhaseOutlier``), which fires with no slip flag.

Reconstructing resets from ``mwwl_trace`` / ``turbo_edit_trace`` alone would
SILENTLY MISS every KF-driven reset.  BOTH paths do the identical covariance
surgery ``P.row(i)=0; P.col(i)=0; P(i,i)=reset_std^2``, so within an arc
(ambiguities carry ~no process noise, variance monotonically non-increasing)
a meaningful ``std_m`` up-step is unambiguously a reinitialization.  But the
converse is FALSE (measured on the exam hero run, 2026-08-31):
``logAmbiguities`` writes the POST-update state, and in a converged filter
(position/clock/tropo already cm-tight) the same epoch's phase re-pins a
freshly reset ambiguity to ~cm, so the surgery often never surfaces in the
logged sigma at all -- 38 of that run's 43 injected slips left no up-step,
and the 5 that did log the raw reset sigma collapsed back within an epoch,
which is exactly the bridge hold-and-restore signature (below) and was
mis-held as "lock continuous".  Resets are therefore reconstructed from the
UNION of two sources:

  (a) the ``std_m`` reinflation heuristic -- still the only witness of
      KF post-fit resets, and of everything in logs that predate the slip
      traces; and
  (b) slip / LLI flags gathered by ``_load_slip_events``.  A detector slip
      (``is_slip`` / TurboEdit flag) is HARD evidence: the slip pipeline
      resets, so the epoch forces an in-arc reset on every ambiguity of that
      satellite and is never eligible for the bridge classification.  A
      receiver LLI flag (``is_lli``) is SOFT: it forces a reset only when no
      hold-and-restore signature is present, so a ``turbo_edit_bridge_lli``
      hold that genuinely restores its snapshot still renders as type 3.
      Consecutive flagged epochs collapse to ONE reset (MW flags persist for
      several epochs after a hit).

(A covariance-preserving cycle-slip REPAIR shifts ``value_m`` but NOT
``std_m`` and raises no detector flag -- correctly NOT counted as a locktime
reset, because arc continuity is preserved.)

Reset taxonomy reconstructed per (satellite, signal) series:

* **arc-start** (type 1) : first appearance, or reacquisition after a time gap
  (a rising/setting satellite -- expected, not pathological).
* **in-arc reset** (type 2) : a std reinflation mid-arc, or a detector slip /
  LLI flag at this epoch (union rule above) -- the diagnostic of interest.
  Optionally classified by source:

    - ``slip`` -- a slip flag exists for this sat near this epoch in
      ``mwwl_trace`` / ``turbo_edit_trace``,
    - ``kf``   -- no slip flag -> KF post-fit residual reset.

* **bridge held** (type 3) : a soft-LLI gap hold (``turbo_edit_bridge_lli``)
  whose live state parks at the reset sigma but is then RESTORED to the
  pre-gap snapshot + dN*lambda by the bridge repair.  Lock is logically
  continuous -- locktime keeps counting, and the event is marked with a
  triangle, not a reset.  A hold that TIMES OUT never restores and remains a
  true in-arc reset.

Markers overlaid on the strip: a circle for a slip-driven reset, an x (magenta)
for a non-slip reset (KF residual / hold timeout), and a triangle (green) for a
bridge hold+repair (lock continuous, not a reset).

Content switch ``plot_value`` (kwarg, MATLAB default ``'locktime'``):
  * ``'locktime'`` : minutes since last (re)init (PRIMARY -- reset diagnostic),
    capped at ``cap_minutes`` (20), viridis (fresh = dark);
  * ``'std'``      : ambiguity std (m) -- the raw reinflation signal itself, a
    cross-check that the locktime resets are real; capped at ``std_cap_m``
    (0.60), viridis reversed (high sigma = dark, salient).

Expected CSV header (``TraceLogger::logAmbiguities``):
    ``week,tow_s,key,value_m,std_m``   (key = ``"<sat>_<signal>"``).
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import GPS_EPOCH, add_utc, load_trace, require_cols
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult

# Reset-detection parameters, faithful to the C++ covariance surgery (mirrors
# the MATLAB ``resetParams`` struct one-for-one).
_REL_FACTOR = 1.5       # std(i) / std(i-1) ratio trigger
_ABS_JUMP_M = 0.10      # std(i) - std(i-1) absolute trigger (m)
_MIN_RESET_STD = 0.05   # ignore sub-floor jitter in deep convergence
_PIVOT_STD = 1e-5       # std <= this is a frozen BSSD pivot (m)
_GAP_FACTOR = 3.0       # dt > gapFactor*median(dt) => reacquisition
_HOLD_CAP_S = 90.0      # look-ahead for a snapshot restore (C++ cap 60 s)
_RESTORE_FACTOR = 2.0   # restored sigma <= this x pre-hold sigma => bridged

_CLASSIFY_WINDOW_S = 5.0  # a reset within +/- this many s of a slip flag = slip

# Canonical system order (matches the strip renderer / MATLAB sysOrder).
_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")


def _system_of(sat: str) -> str:
    """Leading-letter -> system name (matches the MATLAB startsWith ladder)."""
    if not sat:
        return "Unknown"
    return {
        "G": "GPS", "E": "Galileo", "R": "GLONASS",
        "C": "BeiDou", "J": "QZSS", "S": "SBAS",
    }.get(sat[0], "Unknown")


def _reconstruct_locktime(tsec: np.ndarray, stdv: np.ndarray,
                          slip_mask: np.ndarray | None = None,
                          lli_mask: np.ndarray | None = None):
    """Per (satellite, signal) arc -> (lockSec, resetType).

    Derives seconds-since-reinit and a reset taxonomy from the ambiguity std
    series plus the detector flags (module docstring, union rule).
    ``resetType``: 0 continuing, 1 arc-start (init/reacquire), 2 in-arc reset,
    3 bridge-held span (soft-LLI hold restored by a snapshot + dN*lambda
    repair -- locktime CONTINUES across it).  ``tsec`` must be ascending
    (seconds, arc-relative); ``stdv`` the matching std (m).

    ``slip_mask`` / ``lli_mask`` (optional bool arrays, same length): True
    where a detector slip / receiver LLI flag coincides with this sample.  A
    slip sample forces a type-2 reset regardless of the sigma series and is
    never bridge-eligible; an LLI sample forces one only when the sigma shows
    no hold-and-restore signature.  Consecutive flagged samples collapse to
    one reset at the run's first sample.

    Port of the MATLAB ``reconstructLocktime`` local function, extended with
    the flag union (the MATLAB tool predates the exam slip traces).
    """
    n = tsec.size
    lock_sec = np.zeros(n)
    reset_type = np.zeros(n, dtype=int)
    if n == 0:
        return lock_sec, reset_type
    reset_type[0] = 1                       # first appearance = arc start

    slip_mask = (np.zeros(n, bool) if slip_mask is None
                 else np.asarray(slip_mask, bool))
    lli_mask = (np.zeros(n, bool) if lli_mask is None
                else np.asarray(lli_mask, bool))
    # One physical event = one reset: only the FIRST sample of a consecutive
    # flagged run fires (MW keeps flagging for several epochs after a hit).
    any_mask = slip_mask | lli_mask
    new_event = any_mask & ~np.concatenate(([False], any_mask[:-1]))
    force_hard = new_event & slip_mask       # detector slip: never a bridge
    force_soft = new_event & ~slip_mask      # LLI only: bridge-eligible

    dt = np.diff(tsec)
    pos = dt[dt > 0]
    med_dt = float(np.median(pos)) if pos.size else 1.0
    if not np.isfinite(med_dt) or med_dt <= 0:
        med_dt = 1.0

    eps = np.finfo(float).eps
    i = 1
    while i < n:
        gap = tsec[i] - tsec[i - 1]
        s0 = stdv[i - 1]
        s1 = stdv[i]

        is_gap = gap > _GAP_FACTOR * med_dt

        # Mid-arc covariance reinflation (== a C++ resetAmbiguity /
        # handlePhaseOutlier did P(i,i)=reset_std^2 this epoch).
        d_std = s1 - s0
        ratio = (s1 / max(s0, eps)) if s0 > 0 else 0.0
        reinfl = (s1 > _MIN_RESET_STD) and (
            (d_std > _ABS_JUMP_M)
            or (s0 > _MIN_RESET_STD and ratio > _REL_FACTOR)
        )

        # Former BSSD pivot losing the pivot: frozen (std~0) -> re-estimated.
        former_pivot = (s0 <= _PIVOT_STD) and (s1 > _MIN_RESET_STD)

        if is_gap:
            reset_type[i] = 1
            lock_sec[i] = 0.0
            i += 1
        elif reinfl or former_pivot or force_hard[i] or force_soft[i]:
            # Reinflation: bridged hold-and-restore, or a true reset?  A
            # soft-LLI bridge repair RESTORES the pre-hold sigma in one step
            # within the hold cap; scan ahead for that signature.  (formerPivot
            # spans are excluded -- a pivot's pre-event sigma is 0, so "restore
            # to pre-hold sigma" is undefined.  A detector-slip sample is
            # excluded too: the slip pipeline reset the state, and a fresh
            # ambiguity's one-epoch re-pin in a converged filter is sigma-
            # indistinguishable from a snapshot restore.)
            j = 0
            if reinfl and s0 > _PIVOT_STD and not force_hard[i]:
                k = i + 1
                while k < n and (tsec[k] - tsec[i - 1]) <= _HOLD_CAP_S:
                    if (stdv[k] <= _RESTORE_FACTOR * s0
                            and stdv[k - 1] > _RESTORE_FACTOR * s0):
                        j = k                # single-step restore epoch
                        break
                    k += 1
            if j > 0:
                # Held span i..j-1: lock logically continuous.  Locktime keeps
                # accumulating through the hold and the restore.
                for m in range(i, j + 1):
                    lock_sec[m] = lock_sec[m - 1] + (tsec[m] - tsec[m - 1])
                reset_type[i:j] = 3          # parked/held samples
                i = j + 1
            else:
                reset_type[i] = 2
                lock_sec[i] = 0.0
                i += 1
        else:
            lock_sec[i] = lock_sec[i - 1] + gap
            i += 1

    return lock_sec, reset_type


def _load_slip_events(ctx) -> pd.DataFrame:
    """Gather (satellite, utc, kind) slip flags from MW + TurboEdit traces.

    ``kind`` is ``"slip"`` for detector slips (``is_slip`` / TurboEdit
    ``mw_flagged``/``gf_flagged``) and ``"lli"`` for receiver LLI flags
    (``is_lli``) -- the hard/soft split the reconstruction needs (module
    docstring).  Returns an empty frame (columns ``satellite``, ``utc``,
    ``kind``) if neither trace is present.  Port of the MATLAB
    ``loadSlipEvents`` extended with the LLI channel.
    """
    frames = []

    mw_path = ctx.path("mwwl_trace.csv")
    try:
        m = load_trace(mw_path)
        if {"satellite", "week", "tow_s", "is_slip"}.issubset(m.columns):
            is_slip = pd.to_numeric(m["is_slip"], errors="coerce").fillna(0.0)
            kinds = ((is_slip.to_numpy() == 1, "slip"),)
            if "is_lli" in m.columns:
                is_lli = pd.to_numeric(m["is_lli"], errors="coerce").fillna(0.0)
                kinds += ((is_lli.to_numpy() == 1, "lli"),)
            for sel, kind in kinds:
                if sel.any():
                    utc = GPS_EPOCH + pd.to_timedelta(
                        m["week"].to_numpy(float)[sel] * 7 * 86400.0
                        + m["tow_s"].to_numpy(float)[sel], unit="s")
                    frames.append(pd.DataFrame(
                        {"satellite": m["satellite"].astype(str).to_numpy()[sel],
                         "utc": utc, "kind": kind}))
    except (OSError, ValueError):
        pass

    te_path = ctx.path("turbo_edit_trace.csv")
    try:
        e = load_trace(te_path)
        need = {"satellite", "week", "tow_s", "mw_flagged", "gf_flagged"}
        if need.issubset(e.columns):
            mw_f = pd.to_numeric(e["mw_flagged"], errors="coerce").fillna(0.0)
            gf_f = pd.to_numeric(e["gf_flagged"], errors="coerce").fillna(0.0)
            flagged = (mw_f.to_numpy() == 1) | (gf_f.to_numpy() == 1)
            # Drop "none" resolutions (a GF false alarm that resolved to no
            # slip never touched the state).
            if "action" in e.columns:
                flagged = flagged & (e["action"].astype(str).to_numpy() != "none")
            if flagged.any():
                utc = GPS_EPOCH + pd.to_timedelta(
                    e["week"].to_numpy(float)[flagged] * 7 * 86400.0
                    + e["tow_s"].to_numpy(float)[flagged], unit="s")
                frames.append(pd.DataFrame(
                    {"satellite": e["satellite"].astype(str).to_numpy()[flagged],
                     "utc": utc, "kind": "slip"}))
    except (OSError, ValueError):
        pass

    if not frames:
        return pd.DataFrame({"satellite": pd.Series(dtype=str),
                             "utc": pd.Series(dtype="datetime64[ns]"),
                             "kind": pd.Series(dtype=str)})
    ev = pd.concat(frames, ignore_index=True)
    # A row flagged both ways keeps the HARD kind ("lli" sorts after "slip").
    ev = ev.sort_values(["satellite", "utc", "kind"], kind="mergesort")
    return ev.drop_duplicates(subset=["satellite", "utc"]).reset_index(drop=True)


def build_ambiguity_locktime(
    ctx,
    plot_value: str = "locktime",
    cap_minutes: float = 20.0,
    std_cap_m: float = 0.60,
    show_reset_markers: bool = True,
    classify_reset_source: bool = True,
    colormap_mode: str = "viridis",
) -> ViewerResult:
    """Build the ambiguity-locktime strip figures + summary.

    Kwargs default to the MATLAB content-switch defaults.
    """
    df = load_trace(ctx.path("ambiguity_trace.csv"))
    require_cols(df, ("week", "tow_s", "key", "value_m", "std_m"),
                 "ambiguity_trace.csv")
    df = df.copy()
    df["week"] = pd.to_numeric(df["week"], errors="coerce")
    df["tow_s"] = pd.to_numeric(df["tow_s"], errors="coerce")
    df["value_m"] = pd.to_numeric(df["value_m"], errors="coerce")
    df["std_m"] = pd.to_numeric(df["std_m"], errors="coerce")

    # --- Parse key -> satellite + signal (split on FIRST underscore) ---
    key = df["key"].astype(str)
    sat = key.str.split("_", n=1).str[0]
    sig = key.str.split("_", n=1).str[1]
    has_us = key.str.contains("_", regex=False)
    df["satellite"] = sat.where(has_us, "")
    df["signal"] = sig.where(has_us, "").fillna("")
    # phase ambiguities only
    df = df[(df["signal"] != "") & df["signal"].str.startswith("L")].copy()
    if df.empty:
        raise ValueError("No phase (L*) ambiguities found in ambiguity_trace.csv")

    df["system"] = df["satellite"].map(_system_of)

    # GPS time axis (trace files carry pure GPS time).
    df = add_utc(df)

    # --- De-duplicate (key, epoch): keep LAST (post-relinearization) row ---
    df = df.sort_values(["key", "utc"], kind="mergesort").reset_index(drop=True)
    n_before = len(df)
    df = df.drop_duplicates(subset=["key", "utc"], keep="last").reset_index(drop=True)
    n_dropped = n_before - len(df)

    # --- Slip / LLI events from the detector traces -----------------------
    # These force resets during reconstruction (union rule, module docstring)
    # AND classify reset sources further down -- loaded once for both.
    slip_events = _load_slip_events(ctx)
    win_ns = int(_CLASSIFY_WINDOW_S * 1e9)
    ev_by_sat = {}
    if not slip_events.empty:
        for s, g in slip_events.groupby(slip_events["satellite"].astype(str)):
            ts = g["utc"].to_numpy("datetime64[ns]").astype("int64")
            order = np.argsort(ts)
            ev_by_sat[str(s)] = (
                ts[order],
                (g["kind"].astype(str).to_numpy() == "slip")[order])

    def _event_masks(sat: str, utc_ns: np.ndarray):
        """(slip_mask, lli_mask) for one key's sample times, or (None, None)."""
        ev = ev_by_sat.get(sat)
        if ev is None:
            return None, None
        ts, hard = ev
        pos = np.searchsorted(ts, utc_ns)
        lo = np.clip(pos - 1, 0, ts.size - 1)
        hi = np.clip(pos, 0, ts.size - 1)
        d_lo = np.abs(utc_ns - ts[lo])
        d_hi = np.abs(ts[hi] - utc_ns)
        nearest = np.where(d_lo <= d_hi, lo, hi)
        within = np.minimum(d_lo, d_hi) <= win_ns
        is_hard = hard[nearest] & within
        return is_hard, within & ~is_hard

    # --- Reconstruct locktime + reset taxonomy per key ---
    df["lockMin"] = np.nan
    df["resetType"] = 0
    lock_min = df["lockMin"].to_numpy(copy=True)
    reset_type = df["resetType"].to_numpy(copy=True)
    for _, ridx in df.groupby("key", sort=False).indices.items():
        ridx = np.asarray(ridx)
        t0 = df["utc"].iloc[ridx[0]]
        tsec = (df["utc"].iloc[ridx] - t0).dt.total_seconds().to_numpy()
        stdv = df["std_m"].iloc[ridx].to_numpy(dtype=float)
        utc_ns = (df["utc"].iloc[ridx].to_numpy("datetime64[ns]")
                  .astype("int64"))
        sm, lm = _event_masks(str(df["satellite"].iloc[ridx[0]]), utc_ns)
        ls, rt = _reconstruct_locktime(tsec, stdv, slip_mask=sm, lli_mask=lm)
        lock_min[ridx] = ls / 60.0
        reset_type[ridx] = rt
    df["lockMin"] = lock_min
    df["resetType"] = reset_type

    # Survived-lock duration: accumulated locktime one epoch earlier.
    df["survivedMin"] = np.nan
    reset_pos = np.flatnonzero(df["resetType"].to_numpy() == 2)
    if reset_pos.size:
        surv = df["survivedMin"].to_numpy(copy=True)
        surv[reset_pos] = df["lockMin"].to_numpy()[reset_pos - 1]
        df["survivedMin"] = surv

    # Bridge-held span starts: a type-3 row whose predecessor is a different
    # key or not itself type-3 (one marker per hold-and-restore event).
    rt_arr = df["resetType"].to_numpy()
    key_arr = df["key"].to_numpy()
    prev_diff_key = np.concatenate(([True], key_arr[1:] != key_arr[:-1]))
    prev_not_held = np.concatenate(([True], rt_arr[:-1] != 3))
    df["heldStart"] = (rt_arr == 3) & (prev_diff_key | prev_not_held)

    # --- Optional: classify each in-arc reset as slip- vs KF-driven ---
    # (Reuses the events loaded for the reconstruction masks above.)
    df["resetSrc"] = ""
    do_classify = classify_reset_source and not slip_events.empty
    if do_classify:
        src = df["resetSrc"].to_numpy(copy=True).astype(object)
        # Force ns here as well: ev_by_sat's times are ns, while the frame's own
        # datetime64 resolution is whatever pandas inferred (us on pandas 3.x), so
        # a raw .astype("int64") on a scalar would compare us against ns.
        utc_ns_all = df["utc"].to_numpy("datetime64[ns]").astype("int64")
        for ii in reset_pos:
            ev = ev_by_sat.get(str(df["satellite"].iloc[ii]))
            t0 = int(utc_ns_all[ii])
            if ev is not None and np.any(np.abs(ev[0] - t0) <= win_ns):
                src[ii] = "slip"
            else:
                src[ii] = "kf"
        df["resetSrc"] = src

    # --- Resolve render quantity, caxis, colormap ---
    pv = plot_value.lower()
    if pv == "locktime":
        value_col = "lockMin"
        value_lbl = "Locktime (min since last reinit)"
        caxis = [0.0, cap_minutes]
        cmap_name = colormap_mode          # fresh = dark/low end
    elif pv == "std":
        value_col = "std_m"
        value_lbl = "Ambiguity sigma (m)"
        caxis = [0.0, std_cap_m]
        cmap_name = colormap_mode + "_r"   # high sigma = dark end
    else:
        raise ValueError("plot_value must be 'locktime' or 'std'")

    # --- Markers frame for the strip renderer (one row per glyph event) ---
    markers = None
    marker_styles = None
    if show_reset_markers:
        rows = []
        rs = df[df["resetType"].to_numpy() == 2]
        for _, e in rs.iterrows():
            s = e["resetSrc"]
            kind = "slip" if s == "slip" else ("kf" if s == "kf" else "reset")
            rows.append((e["system"], e["satellite"], e["signal"], e["utc"], kind))
        hs = df[df["heldStart"].to_numpy()]
        for _, e in hs.iterrows():
            rows.append((e["system"], e["satellite"], e["signal"], e["utc"], "held"))
        if rows:
            markers = pd.DataFrame(
                rows, columns=["system", "satellite", "facet", "utc", "kind"])
        # Glyph styling mirrors the MATLAB plot specs.
        marker_styles = {
            "slip": {"marker": "o", "color": (0.0, 0.0, 0.0), "size": 6.0},
            "kf": {"marker": "x", "color": (0.85, 0.0, 0.85), "size": 8.0},
            "reset": {"marker": "+", "color": (0.1, 0.1, 0.1), "size": 7.0},
            "held": {"marker": "^", "color": (0.0, 0.55, 0.25), "size": 6.0},
        }

    # --- Long-form table for the shared strip renderer ---
    val = df[value_col].to_numpy(dtype=float)
    melted = pd.DataFrame({
        "system": df["system"].astype(str),
        "satellite": df["satellite"].astype(str),
        "facet": df["signal"].astype(str),
        "utc": df["utc"],
        "value": val,
    })
    melted = melted[np.isfinite(melted["value"].to_numpy(float))]

    subtitle = ("fresh reset = dark (low)   mature lock = bright (high)   "
                "(o slip-driven reset,  x non-slip reset (KF residual / hold "
                "timeout),  ^ bridge hold+repair: lock continuous)")

    figs = build_sat_epoch_strip(
        melted,
        mode="continuous",
        colormap=cmap_name,
        caxis=caxis,
        fig_title=f"Ambiguity {value_lbl}",
        value_label=value_lbl,
        subtitle=subtitle,
        fig_tag="ambiguity_locktime",
        sys_order=_SYS_ORDER,
        markers=markers,
        marker_styles=marker_styles,
        # One facet per constellation but ~a couple dozen satellite ROWS in it:
        # the multi-facet default (1.75) crushes the strip to ~3 in.
        facet_height=7.0,
    )

    summary = _summary(df, n_dropped, do_classify)
    return ViewerResult(figures=figs, summary=summary)


def _summary(df: pd.DataFrame, n_dropped: int, do_classify: bool) -> str:
    """Plain-text stats block mirroring the MATLAB showSummary fprintf set."""
    epochs = np.sort(pd.unique(df["utc"]))
    n_epochs = epochs.size
    lines = []
    if n_dropped:
        lines.append(f"Dropped {n_dropped} duplicate (key,epoch) row(s) (kept last).")
    lines.append("=== Ambiguity Locktime Summary ===")
    lines.append(f"File        : ambiguity_trace.csv")
    span = pd.Timestamp(epochs[-1]) - pd.Timestamp(epochs[0]) if n_epochs else None
    lines.append(f"Epochs      : {n_epochs}   Span (GPS): "
                 f"{pd.Timestamp(epochs[0])} -> {pd.Timestamp(epochs[-1])}")
    span_hr = span.total_seconds() / 3600.0 if span is not None else np.nan
    if not (span_hr > 0):
        span_hr = np.nan

    rt = df["resetType"].to_numpy()
    held = df["heldStart"].to_numpy()
    tot_reset = int(np.sum(rt == 2))
    tot_slip = int(np.sum((rt == 2) & (df["resetSrc"].to_numpy() == "slip")))
    tot_kf = int(np.sum((rt == 2) & (df["resetSrc"].to_numpy() == "kf")))
    tot_held = int(np.sum(held))
    if do_classify:
        lines.append(f"In-arc resets: {tot_reset} total  (slip-driven {tot_slip}, "
                     f"non-slip {tot_kf} -- KF residual or soft-LLI hold timeout)")
    else:
        lines.append(f"In-arc resets: {tot_reset} total")
    lines.append(f"Bridge hold+repair events: {tot_held} (soft-LLI snapshot "
                 f"restored -- lock counted CONTINUOUS)")

    lines.append("")
    lines.append(f"{'System':<9} {'Signal':<6} {'N':>6} {'resets':>8} "
                 f"{'rst/hr':>8} {'mean surv':>10} {'min surv':>9}")
    lines.append("-" * 64)
    sys_present = [s for s in _SYS_ORDER if (df["system"] == s).any()]
    for sys_name in sys_present:
        t_sys = df[df["system"] == sys_name]
        sigs = sorted(s for s in t_sys["signal"].unique() if s)
        for sig in sigs:
            sub = t_sys[t_sys["signal"] == sig]
            if sub.empty:
                continue
            n_r = int(np.sum(sub["resetType"].to_numpy() == 2))
            surv = sub["survivedMin"].to_numpy()[sub["resetType"].to_numpy() == 2]
            surv = surv[np.isfinite(surv)]
            if surv.size == 0:
                mean_surv = np.nan
                min_surv = np.nan
            else:
                mean_surv = float(np.mean(surv))
                min_surv = float(np.min(surv))
            rst_hr = n_r / span_hr if np.isfinite(span_hr) else np.nan
            lines.append(f"{sys_name:<9} {sig:<6} {len(sub):>6d} {n_r:>8d} "
                         f"{rst_hr:>8.2f} {mean_surv:>9.1f}m {min_surv:>8.2f}m")

    # Loudest offenders: satellites with the most in-arc resets.
    rs = df[df["resetType"].to_numpy() == 2]
    if not rs.empty:
        cnt = (rs.groupby(["satellite", "signal"]).size()
               .sort_values(ascending=False))
        lines.append("")
        lines.append("Top reset offenders (satellite/signal):")
        for (sat, sig), c in cnt.head(10).items():
            lines.append(f"  {sat} {sig:<4} : {int(c)} in-arc reset(s)")

    return "\n".join(lines)


viewer = Viewer(name="ambiguity_locktime", title="Ambiguity locktime strip",
                required_csvs=("ambiguity_trace.csv",),
                build=build_ambiguity_locktime)
