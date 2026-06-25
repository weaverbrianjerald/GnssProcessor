"""CSV loading, column-name fallbacks, and GPS-time conversion.

The MATLAB viewers are riddled with ``try T.x_m; catch T.x; end`` ladders so one
script reads both the live ``state_trace.csv`` schema and the older / smoother
schema.  :func:`first_col` captures that pattern once.  Everything here returns
plain pandas / numpy objects; no plotting.
"""

from __future__ import annotations

from typing import Optional, Sequence

import numpy as np
import pandas as pd

# GPS time origin (1980-01-06 00:00:00).  Trace files carry *pure GPS time* so
# the resulting timestamps line up with the RINEX epochs (no UTC leap offset).
GPS_EPOCH = pd.Timestamp("1980-01-06 00:00:00")


def load_trace(path: str) -> pd.DataFrame:
    """Read a trace CSV into a DataFrame (string-typed text columns)."""
    return pd.read_csv(path)


def first_col(df: pd.DataFrame, *names: str) -> Optional[pd.Series]:
    """Return the first column present among ``names`` (``None`` if none).

    Mirrors the MATLAB ``try ... catch`` column ladders, e.g.
    ``first_col(df, 'x_m', 'x')``.
    """
    for n in names:
        if n in df.columns:
            return df[n]
    return None


def require_cols(df: pd.DataFrame, names: Sequence[str], who: str) -> None:
    """Raise a clear error if any required column is missing."""
    missing = [n for n in names if n not in df.columns]
    if missing:
        raise ValueError(f"{who}: missing required column(s): {', '.join(missing)}")


def gps_to_datetime(week, tow) -> pd.Series:
    """Convert GPS week + time-of-week (seconds) to a datetime on the GPS scale."""
    week = np.asarray(week, dtype=float)
    tow = np.asarray(tow, dtype=float)
    secs = week * 7.0 * 86400.0 + tow
    return GPS_EPOCH + pd.to_timedelta(secs, unit="s")


def add_utc(df: pd.DataFrame, sort: bool = False) -> pd.DataFrame:
    """Add a ``utc`` datetime column from ``week`` + (``tow_s`` | ``tow``).

    Returns a copy.  The column is named ``utc`` to match the MATLAB scripts even
    though it is GPS time, not UTC (the traces carry pure GPS time by design).
    """
    out = df.copy()
    week = first_col(out, "week")
    tow = first_col(out, "tow_s", "tow")
    if week is None or tow is None:
        raise ValueError("add_utc: trace lacks 'week' and/or 'tow_s'/'tow' columns")
    out["utc"] = gps_to_datetime(week, tow)
    if sort:
        out = out.sort_values("utc").reset_index(drop=True)
    return out
