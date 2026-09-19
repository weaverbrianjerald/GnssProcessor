# sif_propagate

Standalone, dependency-free propagation of **NGS Station Information File (SIF)**
station coordinates to a target epoch, with propagated uncertainties.

Given a station list, a target epoch, and one or more SIF JSON files, it emits
each station's ITRF ECEF coordinate and covariance **at that epoch** as CSV or
JSON. It is a faithful Python re-implementation of GnssProcessor's
`resolveStationCoordinate` / `StationInfoFile`, so any project can consume SIF
survey-grade coordinates without linking the C++ engine.

A SIF is NGS's multi-year CORS/IGS solution: per-station ITRF coordinates +
velocities + seasonal (periodic) motion + post-seismic deformation, all referred
to the SIF reference frame/epoch (typically **ITRF2020 @ 2020.00**). This tool
propagates that model to whatever epoch you ask for.

## Requirements

- Python **3.7+**. Standard library only — no numpy, no third-party packages.
- `sif_propagate.py` is a single self-contained file: copy it into another
  project as-is.

## The coordinate function

For the OCCUPATION window covering the requested epoch `t` (or the nearest
non-flagged window if `t` is outside every window):

```
x(t) = x_ref + v·(t − t_ref)                      linear (velocity) term
             + Σ periodic(t)                       annual + semiannual seasonal
             + Σ post_seismic(t)                    log/exp relaxation per E/N/U
```

with `t_ref` the SIF reference epoch (e.g. 2020.0). The position covariance is
propagated from the OCCUPATION 6×6 coordinate/velocity covariance:

```
Cxx(t) = Cxx + dt·(Cxv + Cxvᵀ) + dt²·Cvv,          dt = t − t_ref
```

Periodic phase reference is the METADATA reference epoch; post-seismic terms are
summed in the local ENU frame for events before `t`, then rotated to ECEF.
Toggle the nonlinear terms with `--no-periodic` / `--no-psd` (both on by default,
matching the C++ network-roster path).

### Occupation selection

A SIF station carries one OCCUPATION per equipment era, each with its **own**
reference coordinate, velocity and 6×6 covariance; the breaks are equipment
changes and the offsets that come with them. Which occupation you evaluate is a
choice, and `--occupation` makes it explicit:

| mode | meaning |
|---|---|
| `covering` (default) | the occupation whose window contains the epoch, else the nearest by window midpoint. **The coordinate that was valid for data collected at that epoch** — this is what the C++ engine does. |
| `latest` | the last non-`DO_NOT_USE` occupation by window start — the *current* solution for the mark, propagated (linearly, and optionally with the nonlinear terms) back to the requested epoch. |

**Why `latest` exists: it is the NGS datasheet convention.** A datasheet's
NAD83(2011) epoch-2010.00 position is produced from the **current** occupation
propagated linearly back to 2010.0 with **no periodic term**, then run through
the epoch-matched ITRF2020→NAD83(2011) 14-parameter transformation
(EPSG:9988→6317 evaluated at *t* = 2010.0). Reproducing a published value means
reproducing that recipe, not asking "what was the coordinate in 2010".

`--datasheet` is the preset: `--occupation latest` **+** `--no-periodic`.
Post-seismic terms stay **on** (pass `--no-psd` to drop them; the stations
checked here carry none). It does **not** apply the Helmert — it stops at
ITRF2020 ECEF at your epoch. The closure through the transformation is
demonstrated end-to-end in `corv_worked_example.py` in this folder.

Measured on CORV00USA (Corvallis, OR) against datasheet **AH2488**, whose
monument position is lat 44 35 07.91300 N, lon 123 18 16.51681 W,
*h* = 105.953 m, NAD83(2011) epoch 2010.00:

- The covering occupation at 2010.0 is the 1998-04-26 … 2019-08-03 window; the
  latest starts 2019-08-04. They differ by **32.8 mm**, almost entirely **Up**
  (E +1.6, N −4.2, **U +32.4 mm**) — the 2019-07 antenna swap.
- Default (`covering`) lands **32.8 mm** from the datasheet. `--datasheet`
  closes to **0.4 mm**. CHZZ00USA (datasheet **AJ6961**, latest occupation from
  2022-11-30) closes to **0.6 mm**.

```bash
python sif_propagate.py CORV00USA --epoch 2010.0 --datasheet
```

> ⚠ **Differencing resolved coordinates across an occupation break is not a
> velocity.** The jump between modes (or between two epochs straddling a break)
> is an equipment offset plus a change of reference coordinate — it contains no
> site motion. For site velocity read the occupation's `VELOCITY` directly.

> **The C++ `resolveStationCoordinate` implements `covering` only**, deliberately:
> the engine wants the coordinate valid for the data epoch it is processing. The
> `latest` mode exists in this tool for reproducing published legacy coordinates,
> and the default is byte-identical to the C++ path so the oracle tests still
> hold.

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

Identifiers may be a **9-char IGS long name** (`CIT100USA`), a **4-char marker**
(`CIT1`, case-insensitive), or a **DOMES number** (`40400S201`) — resolved in
that order, exactly like the C++ `find()`. Pass them positionally and/or via
`--stations-file` (whitespace/newline list, `#` comments allowed, or a JSON
array). Unknown stations appear in the output with `found=false` and a warning
on stderr — never a silent zero.

### SIF files

`--sif PATH [PATH ...]` names the SIF JSON file(s). If omitted, the tool tries
`$SIF_PATHS` (os-pathsep-separated), then auto-discovers — searching the script
directory, its parents, and the CWD — the first of `sif/*.json` (this project's
layout) or `data/static/sif/*.json` (the GnssProcessor tree layout). So dropping
the SIF JSON files in a `sif/` folder next to `sif_propagate.py` is enough to run
with no `--sif`. Multiple files accumulate (IGS + NCN); first-wins on duplicate
identifiers.

## Output

CSV columns (JSON carries the same fields plus the full 3×3 `covariance_ecef`):

| column | meaning |
|---|---|
| `requested` / `found` / `valid` | input key; whether it matched a record; whether a usable occupation was found |
| `id` / `domes` / `description` | resolved station identity |
| `reference_frame` / `epoch_year` | SIF frame (e.g. ITRF2020); decimal-year the coordinate was propagated to |
| `x` `y` `z` | ECEF position at the epoch [m] |
| `lat_deg` `lon_deg` `height_m` | geodetic on the **WGS84** ellipsoid (see note below) |
| `sigma_x` `sigma_y` `sigma_z` | ECEF 1-σ uncertainties [m] |
| `sigma_e` `sigma_n` `sigma_u` | local East/North/Up 1-σ uncertainties [m] |
| `in_window` | true if the epoch fell inside an occupation window (false ⇒ extrapolated to the nearest one) |
| `window_start` / `window_end` | the used occupation window (blank end = open-ended) |
| `source` | SIF file the record came from |
| `occupation_mode` | which selection mode produced the row (`covering` / `latest`); **appended last**, so the historical column order is unchanged |

**Precision.** CSV floats are written with per-column precision so nothing
meaningful is truncated: angular columns (`lat_deg`, `lon_deg`) get 11 decimal
places (~1 µm at the surface), metric columns (`x/y/z`, `height_m`, sigmas) get 6
(µm) — both far below GNSS precision. JSON carries full double precision.

**Geodetic datum.** `lat_deg/lon_deg/height_m` are on the **WGS84** ellipsoid
(a = 6378137.0 m, 1/f = 298.257223563). This is exactly the ellipsoid
GnssProcessor's own ECEF↔LLH uses, so the tool stays consistent with the engine;
WGS84 vs GRS80/ITRF differ only in the last digits of the flattening (sub-mm in
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

# NGS datasheet recipe: current occupation, linear only
ds = resolve_station_coordinate(reg.find("CORV00USA"), 2010.0,
                                with_periodic=False, occupation="latest")
print(ds.ecef, ds.window, ds.in_window, ds.occupation_mode)
```

`in_window` / `window` always describe the occupation **actually used**, so a
`latest` result at an old epoch correctly reports `in_window=False`.

## Validation

`test_sif_propagate.py` checks the port three ways and exits non-zero on any
failure:

1. **Oracle** — full resolved ECEF + propagated 3×3 covariance for 12
   (station, epoch) cases captured directly from the C++
   `resolveStationCoordinate`. Covers seasonal-only (CIT100USA), heavy
   post-seismic (ANTC00CHL, ~0.5 m nonlinear term), multi-occupation with a
   zero-cross-covariance window (AB0200USA), and a no-nonlinear-term epoch
   (AMC400USA). **Realised agreement: ≤ 1e-9 m on position, ≤ 5e-13 m² on
   covariance.**
2. **Anchors** — the exact parsing/unit-normalisation numbers asserted in
   `tests/test_station_info.cpp`.
3. **Self-checks** — `find()` key resolution, `dt=0 ⇒ Cxx`, covariance symmetry,
   ENU↔ECEF round-trip, and the calendar/DOY time conversions.
4. **Occupation modes** — CORV00USA `latest` + no-periodic at 2010.0 reproduces
   the expected ITRF2020 XYZ to ≤ 0.2 mm; `covering` vs `latest` differ by
   30–35 mm and Up-dominated; a single-occupation station (AMC400USA) is
   bit-identical in both modes; the default keyword is still `covering`.

```bash
python test_sif_propagate.py     # needs data/static/sif/*.json (or $SIF_PATHS)
```

## Source of truth

The propagation math mirrors, line for line:

- `src/core/products/StationInfoFile.cpp` — SIF parse, unit normalisation,
  occupation selection, `resolveStationCoordinate`.
- `include/common/StationInfo.hpp` — data model and unit conventions.
- `src/core/math/Coordinates.cpp` — `ecefToLlh`, `enuToEcefMatrix` (WGS84).

If the C++ coordinate function changes, re-capture the oracle block in
`test_sif_propagate.py` and re-run it.
