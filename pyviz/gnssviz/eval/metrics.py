"""Truth-referenced position error from a ``state_trace.csv``.

Reduces a run's per-epoch ECEF position to a small, comparable scorecard:
ENU bias / RMS / scatter over a post-convergence window, plus a
time-to-threshold convergence metric.  ENU is computed *about the truth
coordinate*, so the E/N/U series are the position **error** directly.

Reuses :mod:`gnssviz.geodesy` (``ecef_to_enu`` / ``ecef_to_lla``) and
:mod:`gnssviz.io` (``load_trace`` / ``add_utc`` / ``first_col``) so the sign
conventions match the existing ``state`` viewer exactly.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional, Tuple

import numpy as np
import pandas as pd

from ..geodesy import ecef_to_enu, ecef_to_lla
from ..io import GPS_EPOCH, add_utc, first_col, load_trace


# ---------------------------------------------------------------------------
# Epoch helpers
# ---------------------------------------------------------------------------

def _decimal_year_from_timestamp(ts: pd.Timestamp) -> float:
    year = int(ts.year)
    start = pd.Timestamp(year=year, month=1, day=1)
    nxt = pd.Timestamp(year=year + 1, month=1, day=1)
    return year + (ts - start) / (nxt - start)


def gps_to_decimal_year(week, tow) -> float:
    """GPS week + time-of-week (s) -> decimal year (GPS time scale)."""
    secs = float(week) * 7.0 * 86400.0 + float(tow)
    return _decimal_year_from_timestamp(GPS_EPOCH + pd.to_timedelta(secs, unit="s"))


def doy_to_decimal_year(year: int, doy: int, second_of_day: float = 43200.0) -> float:
    """Calendar ``(year, day-of-year)`` -> decimal year.

    ``second_of_day`` defaults to mid-day (43200 s); the propagation is linear
    in time so the within-day choice is sub-mm.
    """
    leap = (year % 4 == 0 and (year % 100 != 0 or year % 400 == 0))
    diy = 366.0 if leap else 365.0
    return year + ((doy - 1) + second_of_day / 86400.0) / diy


# ---------------------------------------------------------------------------
# Scorecard
# ---------------------------------------------------------------------------

@dataclass
class PositionMetrics:
    """Truth-referenced position scorecard for one run.

    All ENU quantities are metres, in the order ``(East, North, Up)``.
    ``bias`` is the mean error (accuracy), ``std`` the scatter about that mean
    (precision), ``rms`` the root-mean-square error about truth
    (``sqrt(bias**2 + std**2)``).
    """

    station: str
    source: str
    frame: str
    truth_ecef: np.ndarray
    n_epochs: int
    n_converged: int
    warmup_s: float
    bias_enu: Tuple[float, float, float]
    std_enu: Tuple[float, float, float]
    rms_enu: Tuple[float, float, float]
    rms_3d: float
    rms_horizontal: float
    final_enu: Tuple[float, float, float]
    time_to_threshold_s: Optional[float]
    threshold_m: float
    used_fallback_window: bool = False
    notes: str = ""

    def to_dict(self) -> dict:
        d = {
            "station": self.station,
            "source": self.source,
            "frame": self.frame,
            "truth_ecef_m": [float(v) for v in self.truth_ecef],
            "n_epochs": self.n_epochs,
            "n_converged": self.n_converged,
            "warmup_s": self.warmup_s,
            "bias_enu_m": list(self.bias_enu),
            "std_enu_m": list(self.std_enu),
            "rms_enu_m": list(self.rms_enu),
            "rms_3d_m": self.rms_3d,
            "rms_horizontal_m": self.rms_horizontal,
            "final_enu_m": list(self.final_enu),
            "time_to_threshold_s": self.time_to_threshold_s,
            "threshold_m": self.threshold_m,
            "used_fallback_window": self.used_fallback_window,
        }
        if self.notes:
            d["notes"] = self.notes
        return d

    def one_line(self) -> str:
        """Compact human row (cm)."""
        be, bn, bu = (v * 100 for v in self.bias_enu)
        ttc = "n/a" if self.time_to_threshold_s is None else f"{self.time_to_threshold_s:6.0f}s"
        return (f"{self.station:<10} {self.source:<14} "
                f"bias[E{be:+6.1f} N{bn:+6.1f} U{bu:+6.1f}]cm  "
                f"3D-RMS {self.rms_3d * 100:6.1f}cm  "
                f"H-RMS {self.rms_horizontal * 100:6.1f}cm  "
                f"conv {ttc}  (n={self.n_converged}/{self.n_epochs})")


@dataclass
class ErrorSeries:
    """Per-epoch ENU position error (about truth) vs elapsed time.

    The plottable counterpart of :class:`PositionMetrics`: where the metrics
    reduce a run to a single scorecard, this keeps the full time series so a
    report can draw the convergence curve.  ENU is in metres about the truth
    coordinate, so ``e/n/u`` are the position **error** directly (same sign
    convention as :func:`evaluate_state_trace` and the ``state`` viewer).
    """

    elapsed_s: np.ndarray            # seconds since the first epoch
    e: np.ndarray
    n: np.ndarray
    u: np.ndarray

    @property
    def err_3d(self) -> np.ndarray:
        return np.sqrt(self.e ** 2 + self.n ** 2 + self.u ** 2)

    @property
    def err_horizontal(self) -> np.ndarray:
        return np.sqrt(self.e ** 2 + self.n ** 2)


def _project_enu(df: pd.DataFrame, truth: np.ndarray):
    """ENU error (about ``truth``) + elapsed seconds for a loaded, utc-stamped trace.

    Pure projection -- ``df`` must already carry the position columns and a
    ``utc`` column (no validation / early-exit here).  Shared by
    :func:`evaluate_state_trace` and :func:`enu_error_series` so both use the
    identical sign convention and time origin.
    """
    x = first_col(df, "x_m", "x").to_numpy(float)
    y = first_col(df, "y_m", "y").to_numpy(float)
    z = first_col(df, "z_m", "z").to_numpy(float)

    lat0, lon0, h0 = (float(np.ravel(v)[0]) for v in ecef_to_lla(*truth))
    e, n, u = ecef_to_enu(x, y, z, lat0, lon0, h0)

    t = df["utc"].to_numpy()
    elapsed = (t - t[0]).astype("timedelta64[ms]").astype(float) / 1000.0
    return elapsed, np.asarray(e, float), np.asarray(n, float), np.asarray(u, float)


def enu_error_series(state_csv_path: str, truth_ecef) -> Optional[ErrorSeries]:
    """Best-effort ENU error time series for plotting a run's convergence.

    Unlike :func:`evaluate_state_trace` (which returns a degenerate all-NaN
    metric carrying a reason), this returns ``None`` on any structural problem
    -- trace unreadable, no epochs, missing position column, or missing time
    columns -- because its only caller (the suite report's convergence figures)
    simply skips a cell it cannot plot.
    """
    truth = np.asarray(truth_ecef, dtype=float).reshape(3)
    try:
        df = load_trace(state_csv_path)
    except Exception:                                          # noqa: BLE001
        return None
    if len(df) == 0 or first_col(df, "x_m", "x") is None:
        return None
    try:
        df = add_utc(df)
    except ValueError:
        return None
    elapsed, e, n, u = _project_enu(df, truth)
    return ErrorSeries(elapsed_s=elapsed, e=e, n=n, u=u)


def _stat_block(e, n, u, mask) -> dict:
    em, nm, um = e[mask], n[mask], u[mask]
    bias = (float(np.mean(em)), float(np.mean(nm)), float(np.mean(um)))
    # Sample std about the mean; a single sample -> 0 (matches the state viewer).
    def _sd(v):
        return float(np.std(v, ddof=1)) if v.size > 1 else 0.0
    std = (_sd(em), _sd(nm), _sd(um))
    rms = (float(np.sqrt(np.mean(em ** 2))),
           float(np.sqrt(np.mean(nm ** 2))),
           float(np.sqrt(np.mean(um ** 2))))
    rms_3d = float(np.sqrt(np.mean(em ** 2 + nm ** 2 + um ** 2)))
    rms_h = float(np.sqrt(np.mean(em ** 2 + nm ** 2)))
    return {"bias": bias, "std": std, "rms": rms, "rms_3d": rms_3d, "rms_h": rms_h}


def _nan_metric(station, source, frame, truth, warmup_s, threshold_m,
                n_epochs, note, used_fallback=False) -> "PositionMetrics":
    """A degenerate scorecard (all-NaN) carrying a reason in ``notes``."""
    nan3 = (float("nan"),) * 3
    return PositionMetrics(
        station=station, source=source, frame=frame, truth_ecef=truth,
        n_epochs=n_epochs, n_converged=0, warmup_s=warmup_s,
        bias_enu=nan3, std_enu=nan3, rms_enu=nan3, rms_3d=float("nan"),
        rms_horizontal=float("nan"), final_enu=nan3,
        time_to_threshold_s=None, threshold_m=threshold_m,
        used_fallback_window=used_fallback, notes=note)


def evaluate_state_trace(state_csv_path: str,
                         truth_ecef,
                         *,
                         station: str = "",
                         source: str = "",
                         frame: str = "",
                         warmup_s: float = 600.0,
                         threshold_m: float = 0.10) -> PositionMetrics:
    """Score one ``state_trace.csv`` against a truth ECEF coordinate.

    Parameters
    ----------
    state_csv_path
        Path to the run's ``state_trace.csv`` (columns ``week,tow_s,x_m,y_m,z_m``).
    truth_ecef
        Length-3 ECEF truth in metres (e.g. ``TruthCoord.ecef``).
    warmup_s
        Epochs within this many seconds of the first epoch are excluded from the
        converged statistics (filter convergence transient).  If that leaves no
        samples, the last 25 % of finite epochs is used instead and
        ``used_fallback_window`` is set.
    threshold_m
        3D-error threshold for the time-to-convergence metric.

    Returns
    -------
    PositionMetrics
    """
    truth = np.asarray(truth_ecef, dtype=float).reshape(3)
    try:
        df = load_trace(state_csv_path)
    except Exception as exc:                                   # noqa: BLE001
        return _nan_metric(station, source, frame, truth, warmup_s, threshold_m,
                           0, f"trace unreadable: {exc}")
    if len(df) == 0 or first_col(df, "x_m", "x") is None:
        return _nan_metric(station, source, frame, truth, warmup_s, threshold_m,
                           int(len(df)), "no epochs / missing position column")
    try:
        df = add_utc(df)
    except ValueError as exc:
        return _nan_metric(station, source, frame, truth, warmup_s, threshold_m,
                           int(len(df)), f"trace lacks time columns: {exc}")

    elapsed, e, n, u = _project_enu(df, truth)

    finite = np.isfinite(e) & np.isfinite(n) & np.isfinite(u)
    conv = finite & (elapsed >= warmup_s)
    used_fallback = False
    if not conv.any():
        used_fallback = True
        idx = np.where(finite)[0]
        if idx.size:
            cut = idx[max(0, int(np.floor(idx.size * 0.75)) - 1)]
            conv = finite & (np.arange(len(e)) >= cut)
        else:
            conv = finite

    n_epochs = int(len(e))
    n_conv = int(conv.sum())
    if n_conv == 0:
        return _nan_metric(station, source, frame, truth, warmup_s, threshold_m,
                           n_epochs, "no finite position samples", used_fallback)

    sb = _stat_block(e, n, u, conv)

    # Time to first reach the 3D threshold (and stay below for the rest of the
    # run) -- a convergence-time proxy that ignores brief early dips.
    err3d = np.sqrt(e ** 2 + n ** 2 + u ** 2)
    ttc = None
    fin_idx = np.where(finite)[0]
    for i in fin_idx:
        tail = err3d[i:][finite[i:]]
        if tail.size and np.all(tail < threshold_m):
            ttc = float(elapsed[i])
            break

    last = np.where(conv)[0][-1]
    final_enu = (float(e[last]), float(n[last]), float(u[last]))

    return PositionMetrics(
        station=station, source=source, frame=frame, truth_ecef=truth,
        n_epochs=n_epochs, n_converged=n_conv, warmup_s=warmup_s,
        bias_enu=sb["bias"], std_enu=sb["std"], rms_enu=sb["rms"],
        rms_3d=sb["rms_3d"], rms_horizontal=sb["rms_h"], final_enu=final_enu,
        time_to_threshold_s=ttc, threshold_m=threshold_m,
        used_fallback_window=used_fallback)
