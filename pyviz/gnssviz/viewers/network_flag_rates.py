"""Network fault-QC flag-rate viewer: realized reject rate vs implied alpha.

Reads ``network_flag_rates.csv`` (the PPP-RTK network producer's per-QC-tier
aggregate fault-detection tallies) and compares, for every DIA/RAIM tier, the
realized flag rate against the implied false-alarm probability alpha that the
tier's threshold was calibrated to. A tier flagging far more (or far fewer)
than its implied alpha is mis-calibrated: too tight ejects good data, too
loose lets faults through.

Tiers (T1..T6, T3c) are the successive fault screens: chi-square / NIS gates,
per-satellite RAIM, shared-parameter DIA, and so on. ``T6`` carries no
calibrated threshold (``has_alpha==0``), so its ``implied_alpha`` is blank and
it is shown as a realized rate only, with no calibration ratio.

Two panels:

  1. Rate vs alpha: one row per tier on a shared log-x axis; a short segment
     joins the realized rate (filled) to the implied alpha (hollow) marker, so
     mis-calibration reads as a long segment. Tiers with no exposure this run
     (``opportunities==0``) draw hollow "no exposure" markers. Vertical guides
     mark the 1e-3 and 1e-6 alpha decades.
  2. Table: a monospace tier | opp | flags | rate | alpha | ratio read-out;
     when the companion ``network_run_summary.csv`` is present it appends the
     run's sync/emit/withhold counts, including ``withheld_frac`` (the sigma0
     withhold fraction, the V3 pass metric).

This is a run-aggregate view: the counts are totals over the whole run. The
per-event timing, which epoch each flag fired in, lives in the separate
per-epoch QC traces, not here.
"""

from __future__ import annotations

import math
from typing import List, Optional

from ..figures import Axes, Figure, Legend, Line, Scatter, TextBox, VLine
from ..io import load_trace, require_cols
from .base import Viewer, ViewerResult, ViewerSkip

# log-x needs strictly-positive x; a 0.0 rate/alpha is clamped to this floor.
_LOG_FLOOR = 1e-12

# Alpha decade guides drawn as vertical reference lines.
_GUIDES = (1e-3, 1e-6)

# Calibration is "OK" when realized rate is within [1/_BAND, _BAND] x implied alpha.
_BAND = 10.0

# Per-tier colours (deterministic, distinct); unknown tiers fall back to gray.
_TIER_COLORS = {
    "T1":  (0.15, 0.45, 0.85),   # blue
    "T2":  (0.20, 0.65, 0.30),   # green
    "T3":  (0.85, 0.45, 0.10),   # orange
    "T3c": (0.95, 0.70, 0.10),   # amber
    "T4":  (0.55, 0.30, 0.75),   # purple
    "T5":  (0.85, 0.20, 0.20),   # red
    "T6":  (0.40, 0.40, 0.40),   # gray (no calibrated alpha)
}
# Preferred display order (present tiers filtered against this).
_TIER_ORDER = ("T1", "T2", "T3", "T3c", "T4", "T5", "T6")


def _tier_color(tier: str):
    return _TIER_COLORS.get(str(tier), (0.5, 0.5, 0.5))


def _clamp_log(x: float) -> float:
    """Floor a (percentage) value to a small positive number for a log-x axis."""
    try:
        v = float(x)
    except (TypeError, ValueError):
        return _LOG_FLOOR
    if not math.isfinite(v) or v <= 0.0:
        return _LOG_FLOOR
    return v


def _has_alpha(row) -> bool:
    """Whether a row carries a calibrated implied-alpha (T6 does not)."""
    ha = row.get("has_alpha")
    try:
        if ha is not None and math.isfinite(float(ha)):
            return float(ha) > 0.0
    except (TypeError, ValueError):
        pass
    # Fall back to the presence of a finite implied_alpha value.
    a = row.get("implied_alpha")
    try:
        return a is not None and math.isfinite(float(a))
    except (TypeError, ValueError):
        return False


def build_network_flag_rates(ctx) -> ViewerResult:
    # The driver auto-skips when the required CSV is absent; guard here too so a
    # direct build() call (tests) raises ViewerSkip rather than a pandas error.
    if not ctx.has_data("network_flag_rates.csv"):
        raise ViewerSkip("network_flag_rates.csv absent or has no data rows")
    df = load_trace(ctx.path("network_flag_rates.csv"))
    require_cols(df, ("tier", "label", "opportunities", "flags", "rate", "k",
                      "implied_alpha", "has_alpha"), "network_flag_rates.csv")
    if df.empty:
        raise ViewerSkip("network_flag_rates.csv has no rows")

    df = df.copy()
    df["tier"] = df["tier"].astype(str)
    # Order rows: known tiers first (canonical order), then any extras as-seen.
    order_index = {t: i for i, t in enumerate(_TIER_ORDER)}
    df["_ord"] = [order_index.get(t, len(_TIER_ORDER) + i)
                  for i, t in enumerate(df["tier"])]
    df = df.sort_values("_ord").reset_index(drop=True)

    rows = [df.iloc[i].to_dict() for i in range(len(df))]

    # --- Axes 1: rate vs implied alpha, one row per tier ---
    ax_rate = Axes(title="Realized flag rate vs implied alpha (per QC tier)",
                   xlabel="rate / implied alpha (%, log scale)", ylabel="",
                   xscale="log", x_is_datetime=False, grid=True,
                   legend=Legend(loc="lower right"))
    traces: list = []

    # Vertical alpha-decade guides.
    for g in _GUIDES:
        traces.append(VLine(x=g, color=(0.70, 0.70, 0.70), style=":", width=1.0,
                            label=f"alpha={g:g}%"))

    n = len(rows)
    seen_labels = {"rate": False, "alpha": False, "noexp": False}
    for i, r in enumerate(rows):
        y = float(n - 1 - i)          # top row = first tier
        tier = str(r["tier"])
        col = _tier_color(tier)
        rate = _clamp_log(r.get("rate"))
        opp = _num(r.get("opportunities"))
        has_a = _has_alpha(r)
        no_exposure = (opp is not None and opp == 0.0)

        if has_a:
            alpha = _clamp_log(r.get("implied_alpha"))
            # 2-point segment joining realized rate to implied alpha at this row.
            traces.append(Line(x=[rate, alpha], y=[y, y], color=col, width=1.4,
                               show_in_legend=False))
            # Implied-alpha marker (hollow diamond).
            traces.append(Scatter(
                x=[alpha], y=[y], marker="D", size=42.0,
                color=(1.0, 1.0, 1.0), edgecolor=col, linewidth=1.2,
                label=("implied alpha" if not seen_labels["alpha"] else None),
                show_in_legend=not seen_labels["alpha"]))
            seen_labels["alpha"] = True

        # Realized-rate marker.  Hollow when the tier had no exposure this run.
        if no_exposure:
            traces.append(Scatter(
                x=[rate], y=[y], marker="o", size=48.0,
                color=(1.0, 1.0, 1.0), edgecolor=col, linewidth=1.2,
                label=("no exposure" if not seen_labels["noexp"] else None),
                show_in_legend=not seen_labels["noexp"]))
            seen_labels["noexp"] = True
        else:
            traces.append(Scatter(
                x=[rate], y=[y], marker="o", size=52.0,
                color=col, edgecolor=(0.15, 0.15, 0.15), linewidth=0.6,
                label=("realized rate" if not seen_labels["rate"] else None),
                show_in_legend=not seen_labels["rate"]))
            seen_labels["rate"] = True

        # Tier label at the left margin (data coords are log-x, so anchor the
        # text label in axes-fraction space instead).
        traces.append(TextBox(x=0.005, y=(y + 0.5) / max(n, 1),
                              text=tier, ha="left", va="center", mono=True,
                              bold=True, color=col, fontsize=8.5))

    ax_rate.ylim = (-0.6, n - 0.4)
    ax_rate.traces = traces

    # --- Axes 2: monospace table (+ optional run summary) ---
    table_text = _table_text(rows, ctx)
    ax_tab = Axes(title="Per-tier calibration + run summary", axis_off=True,
                  x_is_datetime=False, grid=False)
    ax_tab.traces = [TextBox(x=0.01, y=0.98, text=table_text, mono=True,
                             ha="left", va="top", fontsize=8.5)]

    fig = Figure(name="Network flag rates",
                 title="PPP-RTK network fault-QC flag rates",
                 axes=[ax_rate, ax_tab], width=12.0, height=8.0, link_x=False)

    return ViewerResult(figures=[fig], summary=_summary(rows, ctx))


# --------------------------------------------------------------------------- #
def _num(x) -> Optional[float]:
    try:
        v = float(x)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def _ratio(r) -> Optional[float]:
    """rate / implied_alpha, or None when alpha is absent (T6) or zero."""
    if not _has_alpha(r):
        return None
    rate = _num(r.get("rate"))
    alpha = _num(r.get("implied_alpha"))
    if rate is None or alpha is None or alpha == 0.0:
        return None
    return rate / alpha


def _fmt_e(x) -> str:
    v = _num(x)
    return f"{v:.2e}" if v is not None else "  --  "


def _fmt_int(x) -> str:
    v = _num(x)
    return f"{int(round(v))}" if v is not None else "--"


def _table_text(rows, ctx) -> str:
    out = ["Tier   Opp        Flags      Rate(%)     Alpha(%)    Ratio"]
    out.append("-" * 60)
    for r in rows:
        ratio = _ratio(r)
        ratio_s = f"{ratio:.2f}x" if ratio is not None else "  n/a"
        alpha_s = _fmt_e(r.get("implied_alpha")) if _has_alpha(r) else "  --  "
        out.append(f"{str(r['tier']):<6} "
                   f"{_fmt_int(r.get('opportunities')):<10} "
                   f"{_fmt_int(r.get('flags')):<10} "
                   f"{_fmt_e(r.get('rate')):<11} "
                   f"{alpha_s:<11} "
                   f"{ratio_s}")

    rs = _run_summary_rows(ctx)
    if rs is not None:
        out.append("")
        out.append("Run summary")
        out.append("-" * 60)
        for label, val in rs:
            out.append(f"{label:<22} {val}")
    return "\n".join(out)


def _run_summary_rows(ctx):
    """Return [(label, value_str), ...] from network_run_summary.csv, or None.

    Tolerates the file's absence entirely (companion is optional).
    """
    name = "network_run_summary.csv"
    try:
        if not ctx.has_data(name):
            return None
        df = load_trace(ctx.path(name))
    except (OSError, ValueError):
        return None
    if df.empty:
        return None
    r = df.iloc[0].to_dict()

    def g(k):
        return _num(r.get(k))

    rows = []
    for key, lab in (("n_synced", "n_synced"), ("n_emitted", "n_emitted"),
                     ("n_skipped", "n_skipped"),
                     ("n_gate_withheld", "n_gate_withheld"),
                     ("n_sigma0_withheld", "n_sigma0_withheld")):
        v = g(key)
        if v is not None:
            rows.append((lab, f"{int(round(v))}"))

    n_synced = g("n_synced")
    n_s0 = g("n_sigma0_withheld")
    if n_synced is not None and n_synced > 0 and n_s0 is not None:
        rows.append(("withheld_frac", f"{n_s0 / n_synced:.4f}"))

    for key, lab, fmt in (("loop_ms", "loop_ms", "{:.1f}"),
                          ("ms_per_epoch", "ms_per_epoch", "{:.2f}"),
                          ("n_stations", "n_stations", "{:.0f}"),
                          ("n_threads", "n_threads", "{:.0f}")):
        v = g(key)
        if v is not None:
            rows.append((lab, fmt.format(v)))

    par = r.get("parallel")
    if par is not None and str(par).strip() != "":
        rows.append(("parallel", str(par).strip()))

    return rows if rows else None


def _summary(rows, ctx) -> str:
    out = ["=== PPP-RTK Network Fault-QC Flag Rates ==="]
    out.append(f"Tiers reported : {len(rows)}")

    offenders = []
    for r in rows:
        tier = str(r["tier"])
        opp = _num(r.get("opportunities"))
        rate = _num(r.get("rate"))
        flags = _num(r.get("flags"))
        n_flags = int(round(flags)) if flags is not None else 0
        n_opp = int(round(opp)) if opp is not None else 0
        ratio = _ratio(r)

        if opp is not None and opp == 0.0:
            out.append(f"{tier}: no exposure this run (0 opportunities)")
            continue
        if not _has_alpha(r):
            rate_s = f"{rate:.2e}%" if rate is not None else "n/a"
            out.append(f"{tier}: {n_flags}/{n_opp} flagged (rate {rate_s}); "
                       f"no calibrated alpha (advisory only)")
            continue

        alpha = _num(r.get("implied_alpha"))
        rate_s = f"{rate:.2e}%" if rate is not None else "n/a"
        alpha_s = f"{alpha:.2e}%" if alpha is not None else "n/a"
        if ratio is None:
            out.append(f"{tier}: {n_flags}/{n_opp} flagged (rate {rate_s}); "
                       f"alpha {alpha_s}, ratio undefined")
            continue
        verdict = "OK" if (1.0 / _BAND) <= ratio <= _BAND else "MIS-CALIBRATED"
        out.append(f"{tier}: rate {rate_s} vs alpha {alpha_s} "
                   f"({ratio:.2f}x) -> {verdict}")
        if verdict != "OK":
            offenders.append((tier, ratio))

    out.append("")
    if offenders:
        parts = ", ".join(f"{t} ({rr:.1f}x)" for t, rr in offenders)
        out.append(f"OFF-BAND tiers (outside [{1.0 / _BAND:g}x, {_BAND:g}x] "
                   f"of implied alpha): {parts}")
    else:
        out.append(f"All tiers within [{1.0 / _BAND:g}x, {_BAND:g}x] "
                   f"of implied alpha.")

    rs = _run_summary_rows(ctx)
    if rs is not None:
        wf = next((v for lab, v in rs if lab == "withheld_frac"), None)
        if wf is not None:
            out.append(f"sigma0 withheld_frac : {wf}")

    return "\n".join(out)


viewer = Viewer(name="network_flag_rates",
                title="Network fault-QC flag rates (rate vs implied alpha)",
                required_csvs=("network_flag_rates.csv",),
                build=build_network_flag_rates)
