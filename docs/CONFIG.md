# GnssProcessor configuration reference

The runtime config is one of the JSON files in [`config/`](../config/).
Each scenario file is self-contained — no merging or layering.  Pick one
at the command line:

```
gnss_processor.exe config/<scenario>.json
```

Default when no argv is given: `config/ppp_rtk_internal.json`.

Field-level Doxygen lives in
[`include/config/GnssConfig.hpp`](../include/config/GnssConfig.hpp); this
document covers the higher-level "which knob does what" topics that
don't belong in source-level documentation.

---

## `data_source`

- **`internal`** — turnkey mode.  PathResolver globs the bundled `data/`
  tree (see `files.internal_data_root`) for SP3 / CLK / BIA / ERP / ATX /
  NC / brdc files and matches RINEX observations by station name.  No
  network access; `data_download.enabled` is ignored.  Used by tests and
  most demo runs.
- **`production`** — real runs.  PathResolver delegates to ProductResolver,
  which downloads any missing IGS long-name files from CDDIS into
  `data_download.data_root`.  Configure year / doy / station / country in
  the `data_download` block.

---

## `processing.mode`

| Value | Meaning |
| --- | --- |
| `PPP` | Single receiver, combined ionosphere-free, with external orbit/clock products. |
| `PPP-RTK` | Single receiver, uncombined, with phase clock + per-sat ionosphere. |
| `Baseline-Single` | DD baseline: one rover + one reference station. |
| `Baseline-Multi` | Star-topology multi-baseline: N rovers against one reference. |

The mode dictates which `BaselineConfig` / `RealTimeConfig` blocks are
required.  `isBaselineMode()` is the runtime predicate.

---

## `baseline_fixture`

Picks a named bundle in `include/config/BaselineFixtures.hpp`.  The loader
fills any blank field of the `baseline` section from the preset; values
you set in the config always win.  Set to `"Custom"` (or omit) to disable
the preset entirely.

| Preset | Stations | Baseline | Date | Source |
| --- | --- | --- | --- | --- |
| `IGS_Pasadena` | Caltech `CIT100USA` vs JPL `JPLM00USA` | ~10 km | 2024 DOY 129 | CDDIS (auto-fetched) |
| `OSU_Campus` | OSU `NARY` reference + `233` rover | ~340 m | 2023 DOY 321 | Local — drop into `data/2023-321/obs/<station>/` |

---

## `estimation` toggles (PPP-RTK bias handling)

Most are derived from the processing mode + signal mode; uncomment to
override.  Three strategies for handling missing satellite biases on
non-canonical signals (e.g. tracking L5Q when only L1C/L2W have BIA):

1. **`lock_signals_to_clock_ref`** — restrict to clock-reference bands
   only.  Too restrictive in practice; default off.
2. **`estimate_sat_biases`** *(recommended, default on for PPP-RTK)* —
   estimate per-satellite code biases for signals without BIA coverage.
   Phase ambiguities absorb satellite phase biases.
3. **`require_bia_for_signals`** — reject CODE measurements without BIA
   (phase rows are always kept; the ambiguity absorbs the residual sat
   phase bias).  Auto-set to `!estimate_sat_biases`.  Also used by the
   MW detector for integer widelane rounding.

---

## Baseline-mode knobs

### `baseline.use_iono_free`
Per-band uncombined DDs cancel the inter-receiver ionospheric delay only
on short baselines.  For >10–20 km baselines, set this true to collapse
each phase row into the dual-frequency ionosphere-free combination.
Cost: ~3× amplified code noise (variance scales by α²+β² ≈ 9.5 for
GPS L1+L2) and a non-integer ambiguity (AR auto-excluded for IF rows).
Sat-pairs without two shared bands are dropped in this mode.

### `baseline.estimate_differential_tropo`
Adds a single Δzwd state (= ZWD_rover − ZWD_reference) to the baseline
filter.  Wet delay is otherwise unmodeled at both stations and only
cancels in BRSD when both stations see nearly identical wet path delay
— typically violated above ~10 km of baseline, especially with altitude
differences.  H column for phase DDs becomes `(mf_w_i − mf_w_p)`.
Single-state model adequate up to ~100 km; beyond that you'd want
separate ZWDs per station.

### `baseline.enable_pivot_handover`
On a sticky-pivot change, applies an S-transform to re-reference the
existing DD ambiguities to the new pivot rather than erasing and
re-seeding them.  Preserves integer cycle information across handovers.
Default true; set false to revert to legacy reset-on-handover behavior
for testing.

### `baseline.signal_aliasing`
Pair signals across receivers at the band level when tracking
attributes differ (e.g. rover `C2W` vs reference `C2C`, both GPS L2).
Default true.  Set false for strict exact-RINEX-code matching.  The
sat-DCB residual on cross-attribute code rows is small (typically
<1 m) and is absorbed by the float ambiguity for phase rows.

---

## `between_satellite_sd.pivot_min_elevation`

Sticky-pivot retention threshold (degrees).  The current pivot stays in
place until it drops below this elevation; only then is a handover
considered.  Lower values = fewer handovers but lower-quality pivots
(more ionospheric delay residual, more multipath).  Typical published
values: 15–30°.  The codebase default is 25°; baseline configs ship
with that.  PPP-RTK's higher process noise can tolerate down to 15°
without ill effect.

---

## `filter.initial_std`

Each value is a **standard deviation**, not a variance.  Some scenario-
specific guidance:

| State | PPP | PPP-RTK | Baseline |
| --- | --- | --- | --- |
| `position` | 1000 m (truly unknown) | 100 m (modest prior, BSSD constrains fast) | 30 m (RINEX header is meter-accurate) |
| `ambiguity` | 1000 m | 100 m | 100 m (`innov_pre` seed is good) |
| `clock` | 1000 m | 1000 m | unused (DD cancels) |
| `troposphere` | 0.5 m | 0.5 m | unused (or Δzwd via baseline block) |
| `ionosphere` | 5 m | 5 m | unused |

Setting `initial_std.position` too high (e.g. 1 km on a known site) makes
the Kalman gain on position saturate at ~1 in the first measurement
update, dragging ambiguities along with the position update and producing
spuriously large initial innovations that the outlier handler then
rejects.

---

## `filter.process_noise`

Spectral densities (variance rate, units of m²/s for position).  Defaults
are tuned for static post-processing.  For kinematic runs bump
`position_q` to roughly the expected acceleration variance × dt — e.g.
1e2 (≈ 10 m/s² × 1 s) for vehicular dynamics.

---

## `data_download` (production mode only)

Used only when `data_source = "production"`.  In internal mode every field
is ignored.  `data_root` is the local cache directory; ProductResolver
creates `orbit/`, `clock/`, `dcb/`, `erp/`, `brdc/`, `rinex/`, `atx/`,
`nc/` subdirectories under it on first use.  `atx_path` / `nc_path` /
`crx2rnx_path` point at user-supplied static resources that
ProductResolver does not know how to download.

**ATX auto-refresh (honoured in *both* modes).**  `atx_auto_refresh`
(default `false`) is the one `data_download` field that is *not* ignored
in internal mode.  When set, the engine shells out to
`scripts/refresh_atx.py` at run start — after paths are resolved, before
the antenna model is loaded — to keep the IGS ANTEX (`igs20.atx`) current:
a conditional GET against `files.igs.org`, ANTEX-validated, the previous
file backed up (`igs20.atx.weekNNNN.bak`), then an atomic in-place replace.
It is best-effort and non-fatal: a network or validation failure logs a
warning and the run proceeds with the existing local ATX.
`atx_refresh_max_age_days` (default `7`) throttles the network check —
when the local file is younger than this the check is skipped (`0` =
always check).  `atx_refresh_url` overrides the source URL (empty = the
IGS default).  Off by default so runs stay reproducible; the same script
can be run by hand or on a schedule (`python scripts/refresh_atx.py`)
instead of, or in addition to, the per-run hook.

For baseline runs, the typical pattern is to download the reference
station via the `data_download.station` field and drop the rover in
manually — or run internal mode with the bootstrap script
(`scripts/bootstrap_internal_data.py`).

---

## `logging`

Each `enable_*` flag toggles one log channel.  Console output is
unaffected; flags only gate the per-channel `.log` and `.csv` files.
The same flag set is read for both spdlog channels and CSV trace
files; trace files (`enable_state_trace`, `enable_ambiguity_trace`,
etc.) are at the bottom of the section.
