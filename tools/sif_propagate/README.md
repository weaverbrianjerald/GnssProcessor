# sif_propagate

Standalone, dependency-free propagation of **NGS Station Information File (SIF)**
station coordinates to a target epoch, with propagated uncertainties.

Given a station list, a target epoch, and one or more SIF JSON files, the tool
emits each station's ITRF ECEF coordinate and covariance **at that epoch** as CSV
or JSON. It is a faithful Python re-implementation of GnssProcessor's
`resolveStationCoordinate` / `StationInfoFile`, so any project can consume SIF
survey-grade coordinates without linking the C++ engine.

A SIF is NGS's multi-year CORS/IGS solution: per-station ITRF coordinates +
velocities + seasonal (periodic) motion + post-seismic deformation, all referred
to the SIF reference frame/epoch (typically **ITRF2020 @ 2020.00**). The tool
propagates that model to the requested epoch.

## Requirements

- Python **3.7+**. Standard library only: no numpy, no third-party packages.
- `sif_propagate.py` is a single self-contained file and can be copied into
  another project as-is.

## The coordinate function

For the OCCUPATION window covering the requested epoch `t` (or the nearest
non-flagged window if `t` is outside every window, see
[Extrapolation diagnostics](#extrapolation-diagnostics)):

```
x(t) = x_ref + v·(t − t_ref)                      linear (velocity) term
             + Σ periodic(t)                       annual + semiannual seasonal
             + Σ post_seismic(t)                    log/exp relaxation per E/N/U
```

with `t_ref` the selected occupation's own `REFERENCE_EPOCH`, or the METADATA
reference epoch (default 2020.0) when the occupation carries none or a
non-numeric value. The position covariance is propagated from the OCCUPATION 6×6
coordinate/velocity covariance with the same `t_ref`:

```
Cxx(t) = Cxx + dt·(Cxv + Cxvᵀ) + dt²·Cvv,          dt = t − t_ref
```

Every occupation in the bundled IGS and NCN SIFs is at 2020.0 except four NCN
records derived from site logs (AZAZ00USA, AZOR00USA, AZPL00USA, FLSI00USA) at
2010.0. Assuming 2020.0 for those would misplace them by `|v|·10 yr`, 11.0 to
14.9 cm. All four are flagged `DO_NOT_USE` in the shipped NCN SIF, so the
selection rule below gives them no coordinate; the per-occupation epoch applies
as soon as such a record is usable.

The periodic phase reference stays the METADATA reference epoch of the block;
post-seismic terms are summed in the local ENU frame for events before `t`, then
rotated to ECEF. Toggle the nonlinear terms with `--no-periodic` / `--no-psd`
(both on by default, matching the C++ network-roster path).

### Occupation selection

A SIF station carries one OCCUPATION per equipment era, each with its **own**
reference coordinate, velocity and 6×6 covariance; the breaks are equipment
changes and the offsets that come with them. The occupation evaluated is a
choice, and `--occupation` makes it explicit:

| mode | meaning |
|---|---|
| `covering` (default) | the first non-`DO_NOT_USE` occupation whose window contains the epoch (`start <= t < end`), else the non-flagged occupation whose window midpoint is nearest (an open end capped at 9000). **The coordinate that was valid for data collected at that epoch**; this is what the C++ engine does. |
| `latest` | the last non-`DO_NOT_USE` occupation by window start: the *current* solution for the mark, propagated (linearly, and optionally with the nonlinear terms) back to the requested epoch. `in_window` describes the latest window; `extrapolated` keeps its `covering` meaning (see [Extrapolation diagnostics](#extrapolation-diagnostics)). |

**Why `latest` exists: it is the NGS datasheet convention.** A datasheet's
NAD83(2011) epoch-2010.00 position is produced from the **current** occupation
propagated linearly back to 2010.0 with **no periodic term**, then run through
the epoch-matched ITRF2020→NAD83(2011) 14-parameter transformation
(EPSG:9988→6317 evaluated at *t* = 2010.0). Reproducing a published value means
reproducing that recipe, not asking "what was the coordinate in 2010".

`--datasheet` is the preset: `--occupation latest` **+** `--no-periodic`.
Post-seismic terms stay **on** (`--no-psd` drops them; the stations checked here
carry none). It does **not** apply the Helmert: it stops at ITRF2020 ECEF at the
requested epoch. The closure through the transformation is demonstrated end to
end in `corv_worked_example.py` in this folder.

Measured on CORV00USA (Corvallis, OR) against datasheet **AH2488**, whose
monument position is lat 44 35 07.91300 N, lon 123 18 16.51681 W,
*h* = 105.953 m, NAD83(2011) epoch 2010.00:

- The covering occupation at 2010.0 is the 1998-04-26 … 2019-08-03 window; the
  latest starts 2019-08-04. They differ by **32.8 mm**, almost entirely **Up**
  (E +1.6, N −4.2, **U +32.4 mm**), the 2019-07 antenna swap.
- Default (`covering`) lands **32.8 mm** from the datasheet. `--datasheet`
  closes to **0.4 mm**. CHZZ00USA (datasheet **AJ6961**, latest occupation from
  2022-11-30) closes to **0.6 mm**.

```bash
python sif_propagate.py CORV00USA --epoch 2010.0 --datasheet
```

> ⚠ **Differencing resolved coordinates across an occupation break is not a
> velocity.** The jump between modes (or between two epochs straddling a break)
> is an equipment offset plus a change of reference coordinate; it contains no
> site motion. For site velocity, read the occupation's `VELOCITY` directly.

> **The C++ `resolveStationCoordinate` implements `covering` only**, deliberately:
> the engine wants the coordinate valid for the data epoch it is processing. The
> `latest` mode exists in this tool for reproducing published legacy coordinates,
> and the default is byte-identical to the C++ path so the oracle tests still
> hold.

### Extrapolation diagnostics

When no non-flagged window covers the epoch, the nearest occupation is still
evaluated (the coordinate is unchanged), and the result says so. 266 of the 597
IGS stations and 937 of the 2640 NCN stations end on a closed segment; at
2024.35, 1238 of the 3079 resolvable stations are extrapolated, 75 of them across
an antenna change and 68 across a receiver change.

| field | meaning |
|---|---|
| `extrapolated` | true iff no non-`DO_NOT_USE` occupation window covers `t` |
| `extrapolation_years` | signed, from the selected occupation: `t − end` (≥ 0) past the end, `t − start` (< 0) before the start, 0.0 when covered |
| `segment_start_year` / `segment_end_year` | window of the selected occupation (9999.0 = open in the library result) |
| `antenna_change_in_gap` | an ANTENNA window other than the first (by START) starts in the gap and differs from the preceding window in TYPE (whole string, radome included, trimmed) or SERIAL_NUMBER |
| `receiver_change_in_gap` | the same test on RECEIVER windows, TYPE only |

The gap is `[end − tol, t]` past the end and `[t, start + tol]` before the start,
with `tol` = 2 days (`EQUIPMENT_GAP_TOL_YR`) on the segment side, because a
logged change and the solution's segment boundary rarely coincide. A
record-only re-log (same TYPE and serial), a firmware-only receiver update and a
serial-only receiver swap are not changes. All diagnostics are false / 0.0 when
the epoch is covered. In `covering` mode the selected window covers `t` exactly
when some non-flagged window does. `latest` mode is a twin-only extension (the
C++ and pyviz ports implement `covering` only) and keeps the same definition: an
epoch covered by an earlier occupation is not extrapolated and draws no CLI
warning, although `in_window` is false for the latest window. When no window
covers `t`, the years and the gap are measured from the latest segment. A
datasheet run on CORV00USA or CHZZ00USA at 2010.0 is therefore not flagged.

Examples at 2024.35 (bundled SIFs):

- NIUM00NIU: last segment ends 2024-01-07, 0.331 yr before the epoch; the antenna
  changed TRM59800.00 → JAVRINGANT_DM on 2024-02-27. `antenna_change_in_gap`.
- CHUR00CAN: last segment ends 2024-02-13 23:59:30; the receiver changed
  TPS NET-G3A → SEPT POLARX5 at 23:43 the same day, inside the 2-day tolerance.
  `receiver_change_in_gap`.
- PALM00ATA: the antenna record starting 2024-04-15 repeats the previous TYPE and
  serial, so it is not a change.

The CLI prints one `# WARNING:` line to stderr per extrapolated station, stating
the years outside the segment and whether an antenna or receiver change lies in
the gap.

## Usage

```bash
# decimal-year epoch, CSV to stdout, auto-locate bundled SIFs
python sif_propagate.py CIT100USA ANTC00CHL --epoch 2024.36

# calendar-date epoch, explicit SIF files, JSON to a file
python sif_propagate.py --sif igs20_SIF.json ncn20_SIF.json \
    --date 2024-05-08 --format json --output out.json AMC400USA P041

# year:day-of-year epoch, station list from a file
python sif_propagate.py --stations-file roster.txt --doy 2024:129
```

### Epoch (choose one)

| Option              | Example              | Meaning                        |
|---------------------|----------------------|--------------------------------|
| `--epoch YEAR`      | `--epoch 2024.36`    | decimal year                   |
| `--date DATE`       | `--date 2024-05-08`  | calendar date (`YYYY-MM-DD[THH:MM:SS]`) |
| `--doy YYYY:DOY`    | `--doy 2024:129`     | year and day-of-year           |

Calendar→decimal-year conversion uses the same convention as the SIF loader
(day-of-year zero-based, leap-aware; timezone ignored).

### Stations

Identifiers are resolved in the same order as the C++ `find()`, trimmed and
case-insensitive:

1. **exact 9-char ID** (`CIT100USA`), returned unconditionally;
2. **exact DOMES number** (`40400S201`), returned unconditionally;
3. **4-char prefix candidate**: the station whose ID starts with the key's first
   four characters. A 4-char code is not unique across networks, so the
   candidate is vetted:
   - with an approximate position (library `approx_ecef`, three finite values
     with norm > 1e6 m): accepted iff within `MAX_PREFIX_DISTANCE_M` (10 km, 3D)
     of the candidate's reference position, which is its
     `APPROXIMATE_COORDINATES`, else the first non-`DO_NOT_USE` occupation's
     `COORDINATES`, else the first occupation's (a candidate with none of these
     usable is rejected);
   - without a position (the CLI): accepted iff the key is a bare 4-char marker
     (`CIT1`, `P389`). A longer key that is not an exact ID or DOMES is rejected.

GAMB00PYF (Gambier, French Polynesia) is in neither bundled SIF; its prefix
candidate GAMB00USA is 7,688 km away and is rejected. STHL00GBR is the old name
of STHL00SHN (same monument); its candidate is accepted when the approximate
position is supplied and rejected without one. Every rejection is logged at
WARNING level (logger `sif_propagate`; the CLI prints it to stderr) with the key,
the candidate ID and, when a position was supplied, the distance.

Pass identifiers positionally and/or via `--stations-file` (whitespace/newline
list, `#` comments allowed, or a JSON array). Unknown or rejected stations appear
in the output with `found=false` and a warning on stderr, never a silent zero.

### SIF files

`--sif PATH [PATH ...]` names the SIF JSON file(s). If omitted, the tool tries
`$SIF_PATHS` (os-pathsep-separated), then auto-discovers (searching the script
directory, its parents, and the CWD) the first of `sif/*.json` (this project's
layout) or `data/static/sif/*.json` (the GnssProcessor tree layout). A `sif/`
folder of SIF JSON files next to `sif_propagate.py` is therefore enough to run
without `--sif`. Multiple files accumulate (IGS + NCN); first wins on duplicate
identifiers.

## Output

CSV columns (JSON carries the same fields plus the full 3×3 `covariance_ecef`):

| column | meaning |
|---|---|
| `requested` / `found` / `valid` | input key; whether it matched a record; whether a usable occupation was found |
| `id` / `domes` / `description` | resolved station identity |
| `reference_frame` / `epoch_year` | SIF frame (e.g. ITRF2020); decimal year the coordinate was propagated to |
| `x` `y` `z` | ECEF position at the epoch [m] |
| `lat_deg` `lon_deg` `height_m` | geodetic on the **WGS84** ellipsoid (see note below) |
| `sigma_x` `sigma_y` `sigma_z` | ECEF 1-σ uncertainties [m] |
| `sigma_e` `sigma_n` `sigma_u` | local East/North/Up 1-σ uncertainties [m] |
| `in_window` | true if the epoch fell inside the used occupation window (false: extrapolated) |
| `window_start` / `window_end` | the used occupation window (blank end = open-ended) |
| `source` | SIF file the record came from |
| `occupation_mode` | which selection mode produced the row (`covering` / `latest`) |
| `extrapolated` | see [Extrapolation diagnostics](#extrapolation-diagnostics) |
| `extrapolation_years` | signed years outside the selected segment (0.0 when covered) |
| `segment_start_year` / `segment_end_year` | the selected occupation's window (blank / JSON null end = open-ended, as `window_end`) |
| `antenna_change_in_gap` / `receiver_change_in_gap` | equipment change between the segment and the epoch |

The columns through `occupation_mode` keep their historical order; the six
extrapolation columns are **appended after it**, so positional consumers of the
earlier layout are unaffected.

**Precision.** CSV floats are written with per-column precision so nothing
meaningful is truncated: angular columns (`lat_deg`, `lon_deg`) get 11 decimal
places (~1 µm at the surface); metric columns (`x/y/z`, `height_m`, sigmas) and
year columns get 6 (µm). Both are far below GNSS precision. JSON carries full
double precision.

**Geodetic datum.** `lat_deg/lon_deg/height_m` are on the **WGS84** ellipsoid
(a = 6378137.0 m, 1/f = 298.257223563). This is exactly the ellipsoid
GnssProcessor's own ECEF↔LLH uses, so the tool stays consistent with the engine;
WGS84 and GRS80/ITRF differ only in the last digits of the flattening (sub-mm in
latitude/height), so WGS84 is sufficient. The note is printed to the console
(stderr) on every run and embedded as `geodetic_note` in JSON output. The ECEF
`x/y/z` are the primary datum-exact quantity; LLH is a WGS84 convenience view.

## Library use

```python
from sif_propagate import SifRegistry, resolve_station_coordinate

reg = SifRegistry()
reg.load("igs20_SIF.json")
reg.load("ncn20_SIF.json")

st = reg.find("CIT100USA")          # or "cit1", or a DOMES number
rc = resolve_station_coordinate(st, 2024.36)   # with_periodic / with_psd default True
print(rc.ecef, rc.covariance)        # (x,y,z) [m], 3x3 [m^2]

# a renamed or longer key: confirm the 4-char prefix candidate with a position
st = reg.find("STHL00GBR", approx_ecef=(6104817.262, -605827.807, -1740738.684))

# extrapolation diagnostics
rc = resolve_station_coordinate(reg.find("NIUM00NIU"), 2024.35)
print(rc.extrapolated, rc.extrapolation_years, rc.antenna_change_in_gap)

# NGS datasheet recipe: current occupation, linear only
ds = resolve_station_coordinate(reg.find("CORV00USA"), 2010.0,
                                with_periodic=False, occupation="latest")
print(ds.ecef, ds.window, ds.in_window, ds.occupation_mode)
```

`in_window` / `window` always describe the occupation **actually used**, so a
`latest` result at an old epoch correctly reports `in_window=False`.
`format_extrapolation_warning()` renders the CLI's one-line warning for any
extrapolated result.

## Validation

`test_sif_propagate.py` checks the port in nine sections and exits non-zero on
any failure:

1. **Oracle**: full resolved ECEF + propagated 3×3 covariance for 12
   (station, epoch) cases captured directly from the C++
   `resolveStationCoordinate`. Covers seasonal-only (CIT100USA), heavy
   post-seismic (ANTC00CHL, ~0.5 m nonlinear term), multi-occupation with a
   zero-cross-covariance window (AB0200USA), and a no-nonlinear-term epoch
   (AMC400USA). **Realised agreement: ≤ 1e-9 m on position, ≤ 5e-13 m² on
   covariance.**
2. **Anchors**: the exact parsing/unit-normalisation numbers asserted in
   `tests/test_station_info.cpp`.
3. **Self-checks**: `find()` key resolution, `dt=0 ⇒ Cxx`, covariance symmetry,
   ENU↔ECEF round-trip, and the calendar/DOY time conversions.
4. **Occupation modes**: CORV00USA `latest` + no-periodic at 2010.0 reproduces
   the expected ITRF2020 XYZ to ≤ 0.2 mm; `covering` vs `latest` differ by
   30 to 35 mm and Up-dominated; a single-occupation station (AMC400USA) is
   bit-identical in both modes; the default keyword is still `covering`; the
   historical CSV column order is unchanged.
5. **Lookup** (synthetic SIF): exact ID and DOMES unconditional; a 9-char
   non-exact key rejected without a position; bare 4-char codes accepted; the
   10 km limit (inclusive, 3D); unusable positions treated as absent; the
   reference-position cascade (only the first non-flagged occupation is
   consulted before the first occupation; a small non-zero
   `APPROXIMATE_COORDINATES` falls through); one WARNING per rejection.
6. **Epoch** (synthetic SIF): a 2010.0 occupation in the linear term and the
   covariance, exact; 2020.0, absent, string and boolean `REFERENCE_EPOCH`; the
   METADATA fallback; the periodic phase still on the METADATA epoch.
7. **Extrapolation** (synthetic SIF): past the end and before the start, the
   2-day tolerance on the segment side and none on the `t` side, `t` at the
   segment start (covered) and end (extrapolated, 0.0 yr), record-only re-log,
   firmware-only and serial-only receiver updates, radome-only antenna change,
   the first window, a flagged window covering `t`, `latest` mode.
8. **CLI**: the stderr warnings (none for a `--datasheet` run at an epoch an
   earlier occupation covers) and the appended CSV / JSON columns.
9. **Real files** (each check skipped when its station is absent): GAMB00PYF,
   STHL00GBR, P389, `cit1`, the four 2010.0 records (AZAZ00USA with its flag
   cleared reproduces `coords + v·(2024.35 − 2010.0)` exactly), NIUM00NIU and
   CHUR00CAN at 2024.35.

```bash
python test_sif_propagate.py     # sections 1-4 and 9 need data/static/sif/*.json (or $SIF_PATHS)
```

## Source of truth

The propagation math mirrors, line for line:

- `src/core/products/StationInfoFile.cpp`: SIF parse, unit normalisation,
  station lookup, occupation selection, `resolveStationCoordinate`.
- `include/common/StationInfo.hpp`: data model and unit conventions.
- `src/core/math/Coordinates.cpp`: `ecefToLlh`, `enuToEcefMatrix` (WGS84).

If the C++ coordinate function changes, re-capture the oracle block in
`test_sif_propagate.py` and re-run it.
