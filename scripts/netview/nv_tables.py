"""Fast reads of a run's trace tables, and the maps from names and times to the page's indices.

The dashboard reads about forty traces, several of them millions of rows (`network_pf_rows.csv` is 2.8 million on a
6 h run of 56 receivers).  They are read with pyarrow's multi-threaded CSV reader, with the column types given, and
turned into numpy arrays; names become indices through a dictionary encoding, so no per-row Python runs.

    t = read(logs, 'network_zwd.csv', {'week': I64, 'tow': F64, 'station': STR, 'zwd_m': F64})
    e = epochs(t, ix)                 # epoch index of every row (-1 when the epoch is not one of the run's)
    j = names(t, 'station', ix.sta)   # receiver index of every row (-1 when unknown)
    v = num(t, 'zwd_m')               # float64 array, NaN where the field is empty

`read` returns None when the file is absent and an empty table when it holds a header only; a requested column the
file lacks is reported and left out (`missing_columns`).
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.csv as pcsv

WEEK_MS = 604800000

I64 = pa.int64()
F64 = pa.float64()
STR = pa.string()


def _comment_lines(path: Path) -> int:
    """Number of '#' lines at the top of the file (network_amb_state.csv starts with a description)."""
    n = 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if not line.startswith('#'):
                break
            n += 1
    return n


def header_of(path: Path) -> list[str]:
    """The column names of a CSV file (after any '#' lines)."""
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if not line.startswith('#'):
                return [c.strip() for c in line.rstrip('\r\n').split(',')]
    return []


def read(logs: Path, name: str, columns: dict | None = None) -> pa.Table | None:
    """The table logs/name with the given {column: pyarrow type} (all columns, types inferred, when None)."""
    path = logs / name
    if not path.exists():
        return None
    have = header_of(path)
    ro = pcsv.ReadOptions(skip_rows=_comment_lines(path), block_size=1 << 24)
    if columns is None:
        co = pcsv.ConvertOptions(strings_can_be_null=True)
    else:
        cols = {c: t for c, t in columns.items() if c in have}
        if not cols:
            return pa.table({})
        co = pcsv.ConvertOptions(include_columns=list(cols), column_types=cols, strings_can_be_null=True)
    try:
        t = pcsv.read_csv(path, read_options=ro, convert_options=co)
    except pa.ArrowInvalid as err:                       # a header-only or truncated file: nothing to show
        if 'Empty CSV file' in str(err):
            return pa.table({})
        raise
    t = t.replace_schema_metadata({'missing_columns': ','.join(c for c in (columns or {}) if c not in have)})
    return t


def missing_columns(t: pa.Table | None) -> list[str]:
    if t is None or t.schema.metadata is None:
        return []
    v = t.schema.metadata.get(b'missing_columns', b'').decode()
    return [c for c in v.split(',') if c]


def has(t: pa.Table | None, *cols: str) -> bool:
    """The table exists, has rows and has every named column."""
    return t is not None and t.num_rows > 0 and all(c in t.column_names for c in cols)


def _np_chunk(c: pa.Array) -> np.ndarray:
    bufs = c.buffers()
    vals = np.frombuffer(bufs[1], np.float64, count=c.offset + len(c))[c.offset:].copy()
    if c.null_count:
        bits = np.unpackbits(np.frombuffer(bufs[0], np.uint8), bitorder='little')[c.offset:c.offset + len(c)]
        vals[bits == 0] = np.nan
    return vals


def _np(a) -> np.ndarray:
    """A pyarrow (chunked) array as float64 numpy, nulls NaN, read straight from its buffers.  pyarrow's own
    conversions with nulls (to_numpy, fill_null) import pandas, which takes over a minute on a loaded machine."""
    a = pc.cast(a, pa.float64())
    if isinstance(a, pa.ChunkedArray):
        return np.concatenate([_np_chunk(c) for c in a.chunks]) if a.num_chunks else np.zeros(0)
    return _np_chunk(a)


def num(t: pa.Table, col: str, dtype=np.float64) -> np.ndarray:
    """A numeric column as numpy (empty fields NaN for floats, 0 for integers)."""
    v = _np(t.column(col))
    if np.issubdtype(np.dtype(dtype), np.floating):
        return v.astype(dtype, copy=False)
    return np.nan_to_num(v, nan=0.0).astype(dtype)


def strings(t: pa.Table, col: str) -> tuple[np.ndarray, list[str]]:
    """A text column as (codes int32, values): codes index values; empty fields get -1."""
    a = t.column(col).combine_chunks()
    if not pa.types.is_dictionary(a.type):
        a = a.dictionary_encode()
    idx = _np(a.indices)
    codes = np.where(np.isnan(idx), -1, idx).astype(np.int32)
    return codes, [str(v) for v in a.dictionary.to_pylist()]


def names(t: pa.Table, col: str, index: dict, unknown: int = -1) -> np.ndarray:
    """Map a text column through a {name: index} dict; unknown or empty names get `unknown`."""
    codes, values = strings(t, col)
    lut = np.array([index.get(v, unknown) for v in values] + [unknown], np.int32)
    return lut[codes]                                    # code -1 picks the trailing `unknown`


def t_ms(t: pa.Table) -> np.ndarray:
    """GPS time of every row in integer milliseconds (week * WEEK_MS + round(tow * 1000))."""
    w = num(t, 'week', np.int64)
    s = num(t, 'tow', np.float64)
    return w * WEEK_MS + np.round(s * 1000.0).astype(np.int64)


def epochs(t: pa.Table, ix) -> np.ndarray:
    """Epoch index of every row (-1 when its time is not one of the run's epochs)."""
    ms = t_ms(t)
    grid = np.asarray(ix.t_ms, np.int64)
    e = np.searchsorted(grid, ms)
    e = np.minimum(e, len(grid) - 1)
    ok = grid[e] == ms
    return np.where(ok, e, -1).astype(np.int64)


def dense(shape, fill=np.nan, dtype=np.float32) -> np.ndarray:
    return np.full(shape, fill, dtype)


def scatter(out: np.ndarray, idx: tuple, values: np.ndarray, ok: np.ndarray | None = None) -> np.ndarray:
    """out[idx] = values where every index is valid (and ok); later rows win."""
    keep = np.ones(len(values), bool) if ok is None else ok.copy()
    for i in idx:
        keep &= i >= 0
    out[tuple(i[keep] for i in idx)] = values[keep]
    return out


def quantize(v: np.ndarray, scale: float, dtype=np.int16, sentinel: int | None = None) -> np.ndarray:
    """round(v * scale) clipped to the dtype; NaN becomes the sentinel (the dtype's minimum by default)."""
    info = np.iinfo(dtype)
    s = info.min if sentinel is None else sentinel
    q = np.round(np.nan_to_num(v * scale, nan=0.0, posinf=info.max, neginf=info.min + 1))
    q = np.clip(q, info.min + 1, info.max).astype(dtype)
    q[~np.isfinite(v)] = s
    return q


def delta_rows(q: np.ndarray, sentinel: int) -> tuple[np.ndarray, np.ndarray]:
    """Differences along the last axis of the entries that are not the sentinel (the first such entry of a row stays
    as it is), for series that change little between epochs and so compress far better as steps.  A row whose steps
    would not fit the dtype is left as it is.  Returns (coded array, u8 flag per row: 1 = coded as steps); the page
    adds the steps back when it opens the block."""
    q2 = q.reshape(-1, q.shape[-1])
    out = q2.copy()
    flags = np.zeros(q2.shape[0], np.uint8)
    info = np.iinfo(q.dtype)
    for r in np.nonzero((q2 != sentinel).sum(axis=1) >= 2)[0]:
        m = q2[r] != sentinel
        v = q2[r][m].astype(np.int64)
        d = np.diff(v)
        if d.min() <= info.min or d.max() > info.max:
            continue
        v[1:] = d
        out[r, m] = v.astype(q.dtype)
        flags[r] = 1
    return out.reshape(q.shape), flags.reshape(q.shape[:-1])


def log_code(v: np.ndarray, lo: float, hi: float) -> np.ndarray:
    """A positive quantity on a log scale in u8: 1..255 spans 10**lo..10**hi, 0 = absent."""
    with np.errstate(divide='ignore', invalid='ignore'):
        x = (np.log10(v) - lo) / (hi - lo)
    q = np.clip(np.round(1 + x * 254), 1, 255)
    q[~np.isfinite(x)] = 0
    return q.astype(np.uint8)
