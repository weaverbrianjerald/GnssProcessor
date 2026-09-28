"""Network discontinuity-counter and clock-drift (C1) consistency viewer.

Two independent half-sources, either of which alone produces a useful figure
(the viewer skips only when both are absent):

**Half B: discontinuity-counter timelines** (from ``network_solution.csv``).
Cumulative step plots of the per-satellite ``disc_counter`` (the running count
of stream discontinuities the consumer turns into ambiguity resets), one
:class:`Axes` per product kind (``CLK`` / ``PB`` / ``CB``).  Bump epochs are
classified with a CSV-only heuristic and marked: a "datum" bump (a clock-pivot
handover or datum re-reference) is one where >=80% of a constellation's
satellites present that epoch step together; anything sparser is "per-sat" (an
isolated satellite discontinuity).  The ``CLK`` ``disc_counter`` is identically
zero unless the producer ran with ``network.emit_clock_disc`` (default off); an
all-zero clock axis renders as a flat trace plus an informational note, not as
an error.

**Half A: C1 clock-drift consistency** (from the ``network_products*.txt``
product file).  For each satellite carrying >=2 clock (``C``) records, the
emitted clock offset ``c0`` is finite-differenced over epochs adjacent in that
satellite's own time (``fd = Δc0 / Δt``): a withheld or missing epoch opens a
real time gap, so the difference is never taken across a row-index neighbour
that is a different epoch, and pairs further apart than the reset gap are
dropped.  Per constellation, a tile overlays the producer-emitted drift
(``c1``, the ``C`` record's 4th field) as a line against the finite-difference
``fd`` as dots, plus a residual (``c1 - fd``) axis carrying a ``+/-2e-4 m/s``
guide band.

Physics note
------------
``c1`` (the two-state clock model's ``x1``) is the drift carried *into* the
epoch, whereas ``fd`` = ``Δc0/Δt`` is the *realised* rate *across* the interval
that ends at the epoch.  ``c1`` and ``fd`` are therefore offset by one epoch in
their time basis, so a perfect model does not give an identically-zero
``c1 - fd`` residual; the criterion is that the residual sits inside the
``+/-2e-4 m/s`` guide band, not that it is zero.

Consistency checks encoded (per the R6 pass criteria):
  * ``C1==0`` is encoded by omission: a **3-field** ``C`` record (no 4th ``c1``
    field).  On the first epoch after a per-satellite gap longer than
    ``network.clock_model_max_gap_s`` the drift state is reset, so a consistent
    producer writes a 3-field ``C`` record there.  A 4-field (non-zero ``c1``)
    ``C`` record on the first post-gap epoch is a **stale-C1** failure and is
    flagged.  The viewer does not read the config; it uses a fixed 120 s gap
    (``_DEFAULT_MAX_GAP_S``).
  * A finite difference is never taken across an epoch flagged
    ``datum_changed=1`` (scoped to the affected constellations when the E-line's
    field-8 ``datum_consts`` set is present): a datum re-reference is a step in
    ``c0``, not a physical drift.
  * If no ``C`` record in the whole file carries a 4th field (``clock_model``
    off), Half A degrades to an informational note rather than an empty tile.

Epochs are parsed through the L8 product-file module (``gnssviz.product_file``)
only if it exposes one of the entry points ``_try_l8_parser`` looks for;
otherwise a minimal inline record parser reads the ``E`` and ``C`` records this
viewer needs.  The current module exposes ``parse_product_file``, which the
adapter does not look for, so the inline parser is the one that runs.
"""

from __future__ import annotations

import glob
import os
from typing import Dict, List, Optional, Tuple

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, Scatter, TextBox
from ..identifiers import prn_num, system_of
from ..io import add_utc, gps_to_datetime, load_trace, require_cols
from ..palettes import color_by_prn, system_color
from .base import Viewer, ViewerResult, ViewerSkip

# Constellation render order (kept local, mirroring the other network viewers'
# private helpers).
_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")

# Product-file drift-consistency guide band: |c1 - fd| within this passes.
_GUIDE_BAND_M_S = 2.0e-4
# Per-satellite max gap for the drift-reset check; a fixed stand-in for
# network.clock_model_max_gap_s, which the viewer does not read.
_DEFAULT_MAX_GAP_S = 120.0
# Fraction of a constellation that must step at one epoch to call it a datum bump.
_DATUM_FRACTION = 0.80

_KINDS = (("CLK", "Sat clock disc-counter"),
          ("PB", "Phase-bias disc-counter"),
          ("CB", "Code-bias disc-counter"))


# =========================================================================== #
#  Public build
# =========================================================================== #
def build_network_discontinuities(ctx) -> ViewerResult:
    """Build the discontinuity-counter + C1-consistency viewer (two half-sources)."""
    figures: List[Figure] = []
    notes: List[str] = []

    # --- Half B: disc-counter timelines from network_solution.csv --------- #
    have_solution = ctx.has_data("network_solution.csv")
    if have_solution:
        b_figs, b_note = _build_counter_half(ctx)
        figures.extend(b_figs)
        if b_note:
            notes.append(b_note)
    else:
        notes.append("network_solution.csv absent — disc-counter timelines skipped.")

    # --- Half A: C1 consistency from the product file --------------------- #
    prod_path = _find_product_file(ctx)
    if prod_path is not None:
        a_figs, a_note = _build_c1_half(prod_path)
        figures.extend(a_figs)
        if a_note:
            notes.append(a_note)
    else:
        notes.append("no network_products*.txt under log_dir "
                     "(path may be absolute per config) — C1-honesty view skipped.")

    if not have_solution and prod_path is None:
        raise ViewerSkip("neither network_solution.csv nor a network_products*.txt "
                         "product file is present")

    # At least one source was present (otherwise the viewer skipped above).  If
    # it produced no drawable figure (for example a product file whose C records
    # are all 3-field: clock_model off, so there is no C1 to check), the result
    # degrades to an informational note (a summary-only ViewerResult) rather than
    # skipping the whole card.
    return ViewerResult(figures=figures, summary="\n".join(notes))


# =========================================================================== #
#  Half B -- disc-counter timelines (network_solution.csv)
# =========================================================================== #
def _build_counter_half(ctx) -> Tuple[List[Figure], str]:
    df = load_trace(ctx.path("network_solution.csv"))
    require_cols(df, ("week", "tow", "sat", "kind", "signal", "value_m",
                      "disc_counter"), "network_solution.csv")
    if df.empty:
        return [], "network_solution.csv has no rows."

    df = df.copy()
    df["sat"] = df["sat"].astype(str)
    df["kind"] = df["kind"].astype(str)
    df["signal"] = df["signal"].fillna("").astype(str)
    df["system"] = df["sat"].map(system_of)
    df = add_utc(df)
    df["disc_counter"] = df["disc_counter"].astype(float)

    sat_color = color_by_prn(df["sat"].tolist())

    axes_list: List[Axes] = []
    lines_note: List[str] = []
    n_datum = n_persat = 0
    for kind, kind_label in _KINDS:
        d = df[df["kind"] == kind]
        ax = Axes(title=kind_label, ylabel="cumulative disc count",
                  xlabel="GPS Time", legend=Legend(loc="upper left"))
        if d.empty:
            ax.traces = [TextBox(x=0.5, y=0.5, text=f"no {kind} rows",
                                 ha="center", va="center", boxed=True)]
            axes_list.append(ax)
            continue

        # One cumulative-step line per series (sat, or sat|signal for PB/CB).
        if kind == "CLK":
            keys = d["sat"]
        else:
            keys = d["sat"] + "|" + d["signal"]
        traces: list = []
        max_ctr = 0.0
        for key in _ordered_keys(keys.unique()):
            sub = d[keys == key].sort_values("utc")
            t = sub["utc"].to_numpy("datetime64[ns]")
            c = sub["disc_counter"].to_numpy(float)
            sat = str(sub["sat"].iloc[0])
            traces.append(Line(x=t, y=c, color=sat_color.get(sat, (0.3, 0.3, 0.3)),
                               width=0.9, style="-", show_in_legend=False))
            if c.size:
                max_ctr = max(max_ctr, float(np.nanmax(c)))

        # Classify + glyph bump epochs (a step in the per-series counter).
        bumps = _classify_bumps(d, keys)
        nd = int(sum(1 for b in bumps if b[2] == "datum"))
        npv = int(sum(1 for b in bumps if b[2] == "per-sat"))
        n_datum += nd
        n_persat += npv
        if bumps:
            # Place glyphs slightly above the top of the step traces so they read
            # as event markers rather than overplotting the lines.
            ylevel = (max_ctr if max_ctr > 0 else 1.0) * 1.06
            datum_t = [b[0] for b in bumps if b[2] == "datum"]
            persat_t = [b[0] for b in bumps if b[2] == "per-sat"]
            if datum_t:
                traces.append(Scatter(x=np.array(datum_t),
                                      y=np.full(len(datum_t), ylevel),
                                      marker="D", size=42, color=(0.85, 0.16, 0.16),
                                      edgecolor=(0.2, 0.0, 0.0), linewidth=0.6,
                                      label=f"datum bump ({len(datum_t)})"))
            if persat_t:
                traces.append(Scatter(x=np.array(persat_t),
                                      y=np.full(len(persat_t), ylevel),
                                      marker="v", size=26, color=(0.13, 0.40, 0.70),
                                      edgecolor=(0.0, 0.0, 0.2), linewidth=0.5,
                                      label=f"per-sat bump ({len(persat_t)})"))
        elif kind == "CLK":
            # All-zero clock counters are expected unless emit_clock_disc is on.
            lines_note.append("CLK disc-counter all-zero "
                              "(network.emit_clock_disc off — expected).")
        ax.traces = traces
        axes_list.append(ax)

    fig = Figure(name="Network disc-counters",
                 title="Network discontinuity counters (consumer ambiguity resets)",
                 axes=axes_list, width=12.0, height=9.5, link_x=True)

    note = (f"Disc-counter timelines: {n_datum} datum bump(s), "
            f"{n_persat} per-sat bump(s) across CLK/PB/CB.")
    if lines_note:
        note += " " + " ".join(lines_note)
    return [fig], note


def _classify_bumps(d, keys) -> List[Tuple[object, str, str]]:
    """Return ``[(utc, system, 'datum'|'per-sat'), ...]`` for every bump epoch.

    A *bump* for a series is an epoch where its ``disc_counter`` strictly
    exceeds the previous epoch's (a stream discontinuity).  Per constellation
    per epoch, if >= ``_DATUM_FRACTION`` of the satellites *present that epoch*
    bump, the whole event is labelled "datum" (a clock-pivot handover / datum
    re-reference); otherwise each bumping series is labelled "per-sat".  The
    label is emitted once per (epoch, system, class) so glyphs do not stack.
    """
    out: List[Tuple[object, str, str]] = []
    work = d.copy()
    work["_key"] = keys.values
    # Per-series step flag (True where the counter increased vs the prior epoch).
    work = work.sort_values(["_key", "utc"])
    step = np.zeros(len(work), dtype=bool)
    ck = work["_key"].to_numpy()
    cc = work["disc_counter"].to_numpy(float)
    for i in range(1, len(work)):
        if ck[i] == ck[i - 1] and cc[i] > cc[i - 1]:
            step[i] = True
    work["_step"] = step

    # Satellites present per (system, epoch): the denominator for the datum test.
    for (sys_name, utc), grp in work.groupby(["system", "utc"]):
        present_sats = grp["sat"].nunique()
        bump_sats = grp.loc[grp["_step"], "sat"].nunique()
        if bump_sats == 0:
            continue
        is_datum = present_sats > 0 and (bump_sats / present_sats) >= _DATUM_FRACTION
        out.append((utc, str(sys_name), "datum" if is_datum else "per-sat"))
    return out


# =========================================================================== #
#  Half A: C1 consistency (product file)
# =========================================================================== #
def _build_c1_half(prod_path: str) -> Tuple[List[Figure], str]:
    epochs = _parse_product_epochs(prod_path)
    if not epochs:
        return [], f"product file {os.path.basename(prod_path)} has no epochs."

    max_gap_s = _DEFAULT_MAX_GAP_S

    # Per-satellite ordered (t_sec, c0, has_c1, c1, datum_changed) records.
    # A datum change scoped to specific constellations applies only to those
    # (E-line field-8 datum_consts); an empty set is a global datum change.
    per_sat: Dict[str, List[Tuple[float, float, bool, float, bool]]] = {}
    any_c1_field = False
    for ep in epochs:
        t_sec = ep["t_sec"]
        for sat, (c0, has_c1, c1) in ep["clocks"].items():
            if has_c1:
                any_c1_field = True
            dc = _datum_changed_for(ep, sat)
            per_sat.setdefault(sat, []).append((t_sec, c0, has_c1, c1, dc))

    if not any_c1_field:
        return [], ("product file carries no 4-field C records "
                    "(network.clock_model off) — C1-honesty view not applicable.")

    # Build per-sat finite-difference / c1 / residual + stale-C1 flags.
    sat_series: Dict[str, dict] = {}
    n_stale = 0
    stale_examples: List[str] = []
    for sat, recs in per_sat.items():
        recs.sort(key=lambda r: r[0])
        if len(recs) < 2:
            continue
        t = np.array([r[0] for r in recs], float)
        c0 = np.array([r[1] for r in recs], float)
        has_c1 = np.array([r[2] for r in recs], bool)
        c1 = np.array([r[3] for r in recs], float)
        dchg = np.array([r[4] for r in recs], bool)

        # Finite difference on pairs adjacent in time; drop the pair when the
        # gap exceeds max_gap_s or either endpoint's epoch was a datum change.
        fd_t: List[float] = []
        fd_v: List[float] = []
        for i in range(1, len(recs)):
            dt = t[i] - t[i - 1]
            if dt <= 0 or dt > max_gap_s:
                continue
            if dchg[i] or dchg[i - 1]:
                continue
            fd_t.append(t[i])
            fd_v.append((c0[i] - c0[i - 1]) / dt)

        # Stale-C1 check: the first post-gap epoch resets the drift state, so a
        # consistent producer writes a 3-field C record there (has_c1 False).
        # A 4-field non-zero-c1 record there is a stale carry.
        stale_t: List[float] = []
        for i in range(1, len(recs)):
            gap = t[i] - t[i - 1]
            if gap > max_gap_s and has_c1[i] and abs(c1[i]) > 0.0:
                stale_t.append(t[i])
                n_stale += 1
                if len(stale_examples) < 6:
                    stale_examples.append(sat)

        sat_series[sat] = {
            "t": t, "c1": np.where(has_c1, c1, np.nan),
            "fd_t": np.array(fd_t, float), "fd_v": np.array(fd_v, float),
            "stale_t": np.array(stale_t, float),
        }

    if not sat_series:
        return [], ("product file has no satellite with >=2 clock records "
                    "for a finite difference.")

    sat_color = color_by_prn(list(sat_series.keys()))
    by_sys: Dict[str, List[str]] = {}
    for sat in sat_series:
        by_sys.setdefault(system_of(sat), []).append(sat)

    figures: List[Figure] = []
    for sys_name in _SYS_ORDER:
        sats = by_sys.get(sys_name)
        if not sats:
            continue
        sats = sorted(sats, key=lambda s: (prn_num(s) is None, prn_num(s) or 0, s))

        ax_rate = Axes(title=f"{sys_name} — emitted c1 vs finite-difference Δc0/Δt",
                       ylabel="clock rate (m/s)", xlabel="",
                       legend=Legend(loc="upper left"))
        ax_res = Axes(title="residual (c1 − fd)  [criterion: inside ±2e-4 m/s band]",
                      ylabel="residual (m/s)", xlabel="GPS Time",
                      legend=Legend(loc="upper left"))
        rate_traces: list = []
        res_traces: list = []
        res_lo: List[float] = []
        res_hi: List[float] = []
        res_min = np.inf
        res_max = -np.inf
        all_stale_t: List[float] = []

        for sat in sats:
            s = sat_series[sat]
            col = sat_color.get(sat, (0.3, 0.3, 0.3))
            t_dt = gps_to_datetime(np.zeros_like(s["t"]), s["t"]).to_numpy("datetime64[ns]")
            rate_traces.append(Line(x=t_dt, y=s["c1"], color=col, width=0.9,
                                    label=sat, show_in_legend=len(sats) <= 12))
            if s["fd_t"].size:
                fd_dt = gps_to_datetime(np.zeros_like(s["fd_t"]),
                                        s["fd_t"]).to_numpy("datetime64[ns]")
                rate_traces.append(Scatter(x=fd_dt, y=s["fd_v"], color=col,
                                           marker=".", size=14, alpha=0.8,
                                           show_in_legend=False))
                # Residual (c1 - fd) evaluated at the fd epochs (fd_t are a
                # subset of the sat's epoch times, so c1 aligns by lookup).
                idx = np.searchsorted(s["t"], s["fd_t"])
                idx = np.clip(idx, 0, s["t"].size - 1)
                res = s["c1"][idx] - s["fd_v"]
                res_traces.append(Scatter(x=fd_dt, y=res, color=col, marker="o",
                                          size=12, show_in_legend=False))
                finite = res[np.isfinite(res)]
                if finite.size:
                    res_min = min(res_min, float(finite.min()))
                    res_max = max(res_max, float(finite.max()))
            if s["stale_t"].size:
                all_stale_t.extend(s["stale_t"].tolist())

        # ±band on the residual axis (drawn as an HLine pair; a Band needs an
        # x-grid, and the residual x is a datetime scatter so guide HLines read
        # cleaner and never depend on point ordering).
        res_traces.append(HLine(y=+_GUIDE_BAND_M_S, color=(0.4, 0.4, 0.4),
                                style="--", label="±2e-4 m/s guide"))
        res_traces.append(HLine(y=-_GUIDE_BAND_M_S, color=(0.4, 0.4, 0.4),
                                style="--"))
        if all_stale_t:
            st_dt = gps_to_datetime(np.zeros(len(all_stale_t)),
                                    np.array(all_stale_t, float)).to_numpy("datetime64[ns]")
            res_traces.append(Scatter(x=st_dt, y=np.zeros(len(all_stale_t)),
                                      marker="x", size=48, color=(0.85, 0.16, 0.16),
                                      linewidth=1.4,
                                      label=f"stale C1 ({len(all_stale_t)})"))
        ax_rate.traces = rate_traces
        ax_res.traces = res_traces

        figures.append(Figure(
            name=f"C1 honesty — {sys_name}",
            title=f"Network clock-drift honesty (C1) — {sys_name}",
            subtitle="c1 is the drift carried INTO the epoch; fd is the realised "
                     "Δc0/Δt across it — a one-epoch basis offset is inherent, so "
                     "the ±band (not zero residual) is the criterion.",
            axes=[ax_rate, ax_res], width=12.0, height=7.5, link_x=True))

    note = (f"C1 honesty: {len(sat_series)} sat(s) finite-differenced; "
            f"{n_stale} stale-C1 event(s)"
            + (f" ({', '.join(sorted(set(stale_examples)))})" if stale_examples else "")
            + f"; gap/reset threshold {max_gap_s:.0f} s.")
    return figures, note


# --------------------------------------------------------------------------- #
#  Product-file location + parsing
# --------------------------------------------------------------------------- #
def _find_product_file(ctx) -> Optional[str]:
    """First ``network_products*.txt`` under the run's log dir (lexicographic).

    The product file's path may be configured to an absolute location outside
    the log dir; this viewer auto-discovers only within ``ctx.log_dir`` and
    notes the miss otherwise (it does not assume ``ctx.path('network_products.txt')``).
    Empty files are skipped.
    """
    hits = sorted(glob.glob(os.path.join(ctx.log_dir, "network_products*.txt")))
    for h in hits:
        try:
            if os.path.getsize(h) > 0:
                return h
        except OSError:
            continue
    return None


def _parse_product_epochs(path: str) -> List[dict]:
    """Parse E/C epochs from a network product file.

    Prefers the L8 parser (:mod:`gnssviz.product_file`) when it is available and
    exposes what this viewer needs; otherwise falls back to a minimal inline
    record parser reading exactly the ``E`` (epoch + datum flags) and ``C``
    (clock c0 [, c1]) records.  ``G`` (generation time) and every other tag are
    ignored here.

    Returns a list of dicts, one per epoch, each::

        {"t_sec": float,                 # GPS seconds (week*604800 + tow)
         "datum_changed": bool,
         "datum_consts": set[str],       # affected constellation letters ('' set => global)
         "clocks": {sat: (c0, has_c1, c1)}}
    """
    try:
        from .. import product_file as _pf  # noqa: F401  (L8, optional)
        parsed = _try_l8_parser(_pf, path)
        if parsed is not None:
            return parsed
    except ImportError:
        pass
    return _parse_product_epochs_inline(path)


def _try_l8_parser(pf_module, path: str) -> Optional[List[dict]]:
    """Adapt the L8 parser's output to this viewer's epoch-dict shape.

    The adapter is conservative: it engages only when ``product_file`` exposes a
    ``parse_epochs`` / ``read_epochs`` / ``read`` / ``parse`` callable that
    yields row/record objects carrying the needed E/C fields.  With no such
    callable, or on any shape mismatch, it returns ``None`` and the caller falls
    back to the inline parser.  The current ``product_file`` module exposes
    ``parse_product_file`` instead, so this adapter returns ``None``.
    """
    fn = None
    for name in ("parse_epochs", "read_epochs", "read", "parse"):
        fn = getattr(pf_module, name, None)
        if callable(fn):
            break
    if fn is None:
        return None
    try:
        records = fn(path)
    except Exception:
        return None
    try:
        return _adapt_l8_records(records)
    except Exception:
        return None


def _adapt_l8_records(records) -> Optional[List[dict]]:
    """Best-effort adaptation of arbitrary L8 record objects → epoch dicts.

    Supports either a sequence of epoch-like objects (with ``week``/``tow`` and a
    ``clocks``/``sats`` mapping) or a flat sequence of clock rows.  Returns
    ``None`` when the shape is unrecognised (→ inline fallback).
    """
    def _get(obj, *names):
        for n in names:
            if isinstance(obj, dict) and n in obj:
                return obj[n]
            if hasattr(obj, n):
                return getattr(obj, n)
        return None

    recs = list(records)
    if not recs:
        return []

    epochs: List[dict] = []
    for r in recs:
        week = _get(r, "week")
        tow = _get(r, "tow")
        clocks_src = _get(r, "clocks", "sats", "clock")
        if week is None or tow is None or clocks_src is None:
            return None  # not an epoch-shaped record → give up, use inline
        t_sec = float(week) * 604800.0 + float(tow)
        dchg = bool(_get(r, "datum_changed") or False)
        dc_raw = _get(r, "datum_consts")
        dconsts = set(str(x) for x in dc_raw) if dc_raw else set()
        clocks: Dict[str, Tuple[float, bool, float]] = {}
        items = (clocks_src.items() if isinstance(clocks_src, dict)
                 else [( _get(s, "sat"), s) for s in clocks_src])
        for sat, s in items:
            if sat is None:
                continue
            c0 = _get(s, "clock_c0_m", "c0", "value_m")
            if c0 is None:
                continue
            c1 = _get(s, "clock_c1_m_s", "c1")
            has_c1 = c1 is not None
            clocks[str(sat)] = (float(c0), bool(has_c1), float(c1) if has_c1 else 0.0)
        epochs.append({"t_sec": t_sec, "datum_changed": dchg,
                       "datum_consts": dconsts, "clocks": clocks})
    return epochs


def _parse_product_epochs_inline(path: str) -> List[dict]:
    """Minimal inline E/C record parser (fallback when the L8 adapter does not engage)."""
    epochs: List[dict] = []
    cur: Optional[dict] = None
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                line = raw.rstrip("\r\n")
                if not line or line[0] == "#":
                    continue
                f = line.split(",")
                tag = f[0]
                if tag == "E":
                    if len(f) < 6:
                        cur = None
                        continue
                    try:
                        t_sec = float(f[1]) * 604800.0 + float(f[2])
                    except ValueError:
                        cur = None
                        continue
                    dchg = (len(f) > 6 and f[6].strip() not in ("", "0"))
                    dconsts = set(ch for ch in f[7]) if len(f) > 7 and f[7] else set()
                    cur = {"t_sec": t_sec, "datum_changed": dchg,
                           "datum_consts": dconsts, "clocks": {}}
                    epochs.append(cur)
                elif tag == "C":
                    if cur is None or len(f) < 3:
                        continue
                    try:
                        c0 = float(f[2])
                    except ValueError:
                        continue
                    has_c1 = len(f) > 3 and f[3].strip() != ""
                    c1 = 0.0
                    if has_c1:
                        try:
                            c1 = float(f[3])
                        except ValueError:
                            has_c1 = False
                    cur["clocks"][f[1]] = (c0, has_c1, c1)
                # E/C are all this viewer needs; G/B/P/U/R/S ignored.
    except OSError:
        return []
    return epochs


def _datum_changed_for(ep: dict, sat: str) -> bool:
    """Whether ``sat``'s constellation had a datum change this epoch.

    An epoch flagged ``datum_changed`` with a non-empty ``datum_consts`` set
    applies only to satellites whose leading constellation letter is in that set
    (A2 per-constellation scoping).  An empty set is a global datum change
    (files without A2 scoping, or runs with scoping off) and applies to every
    satellite.
    """
    if not ep["datum_changed"]:
        return False
    consts = ep["datum_consts"]
    if not consts:
        return True
    return bool(sat) and sat[0] in consts


# --------------------------------------------------------------------------- #
def _ordered_keys(keys):
    """Order series keys by numeric PRN (chars 2-3), unparseable last."""
    def k(s):
        s = str(s)
        p = prn_num(s)
        return (0, p, s) if p is not None else (1, 0, s)
    return sorted((str(x) for x in keys), key=k)


viewer = Viewer(name="network_discontinuities",
                title="Network discontinuity counters & C1 clock-drift honesty",
                required_csvs=(),
                build=build_network_discontinuities)
