# RF-NS-b: the vehicle's observation loss is a per-station PD rejection (2026-09-15)

**STATUS**

- **id:** RF-NS-b
- **topic:** network-vehicle
- **status:** superseded
- **summary:** Nothing was absent: addStation's per-station PD test dropped a median of 29 (golden) and 42 (vehicle) of 59 stations per epoch; superseded by RF-PD-fix, which removed every rejection (armed in the goldens).
- **dates in the text:** 2026-09-15
- **knobs / posture:** the RF-0g golden and the RF-1 + RF-2b + RF-2d vehicle (6 h cold field); the PD test at `NetworkNormalEquations.cpp:476-484`; the readmission seed downstream of the gate (`NetworkSolveGlue.cpp:1159` against `:1048`); `network.amb_float_birth_var_m2` unset; pre-registered, NOT BUILT: (b1) the PD rejection fixed at its source, (b2) fix (a) v2 with the pinned column quarantined behind a column-occupancy accessor; instrument `scripts/rf_obs_loss_read.py`
- **hashes named in the text:** `network_if_slip.csv` sha256 `dbd4ac1d5818d655…`, identical in both runs
- **run dirs named in the text:** `runs/_rf0g_field6`, `runs/_rf2d_field6_both`, `runs/_rfns_field6_both`
- **source:** [RF-NS](RF-NS_null_space_read_2026-09-15.md) section 6 (fix (a) rejected; the absence framing corrected)

**Outcome.** Question: why do the returning `CB|Gxx|C1W` columns of RF-NS carry no information? Nothing was absent: G07, G30 and G04 stay tracked by 10 to 13 stations, but `addStation`'s per-station PD test drops a median of 29 (golden) and 42 (vehicle) of 59 stations, so the column is fed by at most one in-solve station and the withhold blocks its own seed. Superseded: [RF-PD](RF-PD_station_rejection_2026-09-15.md) named the null and [RF-PD-fix](RF-PD-fix_station_prior_before_pd_2026-09-15.md) removed every rejection (armed in the goldens); guard (b2) has no population. Status reviewed 2026-10-13.

**Status as recorded (2026-09-15):** open — **nothing was absent.**  The satellites at the centre of every mid-run withhold are TRACKED by 10–13
stations on every epoch of their supposed "absence" (`network_if_slip.csv`, whose **sha256 is identical between the two
runs** — `dbd4ac1d5818d655…` — so the observation stream entering the producer is bit-identical in both arms).  What
differs is how many of those stations reach the SOLVE.  `NetworkNormalEquations::addStation` rejects any station whose
local normal matrix `N_rr_r` fails a relative PD test, and drops it for the epoch: a median of **29 of 59 stations per
settled epoch in the RF-0g GOLDEN and 42 of 59 in the RF-1/2b/2d vehicle** (23 633 and 30 340 drops over 720 epochs, 49
distinct stations, six of them rejected on all 720).  G07 therefore reaches the reduced border at 6–7 stations in the
golden and at exactly **one** (DARW00AUS) in the vehicle; G30 the same; G04 at **two** in the golden and one in the
vehicle.  The `CB|Gxx|C1W` column RF-NS found carrying zero information is that: a column registered on a ≥ 2-**present**-
station count and then fed by ≤ 1 **in-solve** station.  The withhold begins on the exact epoch the column RETURNS to the
border (G07 274860, G30 279960, G04 264390 — the golden's own `TOPOLOGY col_added` + `READMIT seed_code_bias` epoch), and
the seed that would pin it lives DOWNSTREAM of the gate (`NetworkSolveGlue.cpp:1159` vs `:1048`), so the withhold
prevents its own remedy until coverage recovers on its own.  Instrument: `scripts/rf_obs_loss_read.py`.  Nothing built.

## 1. The read — tracked, dropped, in the solve

`scripts/rf_obs_loss_read.py <golden_run> <vehicle_run> --sat G07 [--tow-min …] [--tow-max …]`.  Three columns, three
sources, all trace rows:

- **trk** — stations with an observation row for that satellite in `network_if_slip.csv` (identical in both runs).
- **ndrop** — stations named by `addStation rejected … dropping it this epoch` in `main_log*.log` at that epoch (all
  satellites; it is a station-level drop).
- **in_solve** = trk − dropped, cross-checked against **pf** = distinct stations carrying a post-fit CODE row for that
  satellite in `network_pf_rows.csv`.  `in_solve` and `pf` agree exactly on every emitted epoch of the G07 and G30
  windows; `in_solve` under-reads by one where a station has code without the L1C/L2W phase pair `network_if_slip.csv`
  keys on (G04, and the recovery epochs), so **pf is the authority and `in_solve` the predictor**.

### 1.1 G07 — the 49-epoch hour-4 stretch

| TOW | trk | golden ndrop / in_solve / pf / disp | vehicle ndrop / in_solve / pf / disp |
|---|---|---|---|
| 273990 | 13 | 27 / 8 / **8** / emitted | 41 / 3 / **3** / emitted |
| 274080 | 12 | 27 / 7 / **7** / emitted | 41 / 2 / **2** / emitted |
| 274170 | 11 | 27 / 6 / **6** / emitted | 41 / 2 / **2** / emitted |
| **274260** | 10 | 26 / 6 / **6** / emitted | 41 / 1 / **1** / emitted |
| 274800 | 10 | 26 / 6 / **6** / emitted | 40 / 1 / **1** / emitted |
| 274830 | 10 | 26 / 6 / **6** / emitted | 40 / 1 / **1** / emitted |
| **274860** | 10 | 26 / 6 / **7** / emitted | 40 / 1 / **0** / **gate_withheld** |
| 274890 … 276300 | 10–11 | 26 / 7 / **7** / emitted | 39–41 / 1 / — / **gate_withheld** |
| **276330** | 11 | 26 / 7 / **7** / emitted | 40 / 1 / **2** / emitted |
| 276390 | 12 | 26 / 8 / **8** / emitted | 40 / 2 / **2** / emitted |

The satellite is tracked by 10–13 stations on EVERY epoch, in both arms, including all 49 withheld ones.  The vehicle's
count in the solve is **1** from 274260 to 276300 and DARW00AUS is the same station throughout (elevation 44.1° → 39.2°,
never near a mask).  The golden's count never falls below 6.

### 1.2 G30 — the 18-epoch hour-5 stretch

| TOW | trk | golden in_solve / pf / disp | vehicle in_solve / pf / disp |
|---|---|---|---|
| 279450 | 12 | 7 / **7** / emitted | 1 / **1** / emitted |
| **279510** | 11 | 6 / **6** / emitted | 1 / **1** / emitted |
| 279930 | 11 | 6 / **6** / emitted | 1 / **1** / emitted |
| **279960** | 11 | 6 / **7** / emitted | 1 / **0** / **gate_withheld** |
| 279990 … 280470 | 11–12 | 7 / **7** / emitted | 1 / — / **gate_withheld** |
| **280500** | 12 | 7 / **8** / emitted | 1 / **2** / emitted |
| 280620 | 13 | 8 / **8** / emitted | 2 / **2** / emitted |

Same shape, same numbers, different satellite.  Golden ndrop 25–26 per epoch, vehicle 37–40.

### 1.3 The onset epoch is the column's RETURN, and the golden proves it

The golden's `network_events.csv` carries, on G07:

```
274260.000  TOPOLOGY col_removed   label=CB:G07:C1W
274260.000  READMIT  park G07      D2 clock + code-bias carry parked at departure
274860.000  READMIT  seed_code_bias G07 C1W  value=-0.6989;var_eff=2.581e-05;anchored=0
274860.000  TOPOLOGY col_added     label=CB:G07:C1W
274860.000  READMIT  seed_held G07 C1W       kind=code_bias;e=0.0000;sqrtS=0.0000
```

**The park is IDENTICAL in both arms** — same epoch, same satellite, same text, and it fires from the same predicate
(`readmitColumnPresent`, "a DEPARTURE only if the column itself left the layout",
`src/core/app/NetworkRunContext.cpp:6523-6537`).  The two park predicates do NOT differ.  What differs is what happens
600 s later, when the column returns:

- **golden** — the column is back with 6 in-solve stations, the gate passes, `networkInjectD2CarryPrior` runs and seeds
  it the SAME epoch, `seed_held` reports `e = 0.0000`.  Gap invisible.
- **vehicle** — the column is back with 1 in-solve station, the null-space gate throws FIRST
  (`NetworkSolveGlue.cpp:925/971` → `:1048 gate_withheld` → `:1125 SkipEpoch`), and the epoch returns at
  `src/core/app/ApplicationNetwork.cpp:2481` **before** `networkInjectD2CarryPrior` at `NetworkSolveGlue.cpp:1159` ever
  runs.  The seed cannot fire on the epoch that needs it.  It fires at 276330 with `gap_ep=50`, the first epoch the
  satellite has 2 in-solve stations again.

The vehicle has no `TOPOLOGY col_added` at 274860 because that ledger is a delta against the last EMITTED epoch — RF-NS
§3 already recorded that the silent return is invisible to it.  The golden's `col_added` at exactly 274860, off the same
observation stream and the same registration predicate, is what dates the return in both arms.

## 2. The mechanism, with file:line

1. **Registration counts PRESENT stations, before any of them is dropped.**
   `src/core/app/ApplicationNetworkAssembly.cpp:2108-2117` — `for … if (!present[i]) continue; for (kv :
   sm.getSatBiasStates()) ++cb_station_count[kv.first];` then `if (cnt >= 2) keep_keys.insert(key);` and
   `est.registerCodeBias(...)` at `:2126`.  A registered column is a promise that ≥ 2 stations will feed it.
2. **The promise is broken later in the same epoch.**  The serial consume loop at
   `src/core/app/ApplicationNetworkAssembly.cpp:4410-4470` calls `est.addStation(...)` at `:4450`; on throw it logs
   `Network: station '<X>' addStation rejected (…); dropping it this epoch` and sets `present[i] = false`,
   `est_index_of[i]` stays −1.
3. **The throw is a relative PD test on the station's LOCAL block.**
   `src/core/filter/NetworkNormalEquations.cpp:476-484`: `if (d_min <= 0.0 || d_min < 1e-12 * d_ref)` over the LDLᵀ
   pivots of `N_rr_r`, `d_ref` = the MEDIAN |pivot| under `station_pd_median_scale_` — "station prior too weak or an
   unobservable local parameter".  Measured rejections (`scripts/rf_obs_loss_read.py`, over all 720 epochs of each run):

   | run | total drops | median/epoch (settled, TOW ≥ 261660) | min–max | distinct stations | rejected on all 720 ep |
   |---|---|---|---|---|---|
   | `_rf0g_field6` (golden) | **23 633** | **29 of 59** | 25–48 | 49 | KRGG00ATF, SAVO00BRA, WIND00NAM, MIZU00JPN, MRO100AUS, LMMF00MTQ |
   | `_rf2d_field6_both` (vehicle) | **30 340** | **42 of 59** | 37–48 | 49 | HKSL00HKG, WTZR00DEU, WTZS00DEU, GOPE00CZE, GOP600CZE, GRAZ00AUT |

   The typical pivot in the message is ±1e-10 against a max of 4.5e6 — a genuine null direction in the local block, not a
   conditioning nuisance.  **The RF-0g field golden is running as a ~30-station network, not a 59-station one**, and that
   is true before the vehicle is armed at all.
4. **The vehicle's only solve-affecting difference is the float ambiguity.**  `diff` of the two arms is
   `ambiguity_vehicle: float_kf`, `phase_datum_held_basis` + `phase_datum_held_cold_release` +
   `phase_datum_held_mark_cold`, `phase_anchor_flag_quiet_ep` / `phase_anchor_min_age_ep`, `trace_amb_state` and the run
   paths.  `float_kf` is the one that puts new columns in `N_rr_r` — one free ambiguity per (sat, signal) arc — which is
   exactly the "unobservable local parameter" the PD test names.  29 → 42 drops per epoch is that.
5. **A registered column fed by ≤ 1 in-solve station is a null direction.**  One station's `C1W` code row is absorbed by
   that station's own local receiver-code-bias / clock parameters in the Schur reduction, so the SHARED column receives
   no information — which is precisely RF-NS §3's census (`CB|G07|C1W = 1.00`, every other component 0.00, eigenvalue 0.0
   to ±7e-16, on all 49 epochs).  The sum-to-zero veteran row does not cover it either, because the park removed the
   satellite's carry and so its `jac_sats` membership (`src/core/filter/NetworkSbasis.cpp:1193-1197`).
6. **The gate then blocks its own remedy** (see §1.3): the seed is at `NetworkSolveGlue.cpp:1159`, the withhold return at
   `ApplicationNetwork.cpp:2481`, the gate at `NetworkSolveGlue.cpp:1048/1125`.

**What is still not directly instrumented:** that ≥ 2 IN-SOLVE stations is the exact estimability threshold, and the
per-epoch value of `cb_station_count[sat|signal]`.  Both need the occupancy accessor (b2) pre-registers; the evidence
here is that the withhold begins on the column's return epoch in the arm with 1 in-solve station and never begins in the
arm with 6, and ends on the first epoch with 2 (`pf` 0 → 2 at G07 276330, G30 280500, G04 264540 in the vehicle).  No
counter in either run reports column occupancy, so that last step is inference from coincidence, stated as such.

## 3. The G04 verdict — it is the SAME defect, not a genuine coverage loss

RF-NS read the golden's own 5-epoch hour-1 dip as the one instance of a real absence.  It is not:

| TOW | trk | golden ndrop / in_solve / pf / disp | vehicle ndrop / in_solve / pf / disp |
|---|---|---|---|
| 264360 | 10 | 39 / 2 / **2** / emitted | 43 / 1 / **1** / emitted |
| **264390** | 10 | 39 / 2 / **0** / **gate_withheld** | 43 / 1 / **0** / **gate_withheld** |
| 264420–264510 | 11 | 39 / 2 / — / **gate_withheld** | 43 / 1 / — / **gate_withheld** |
| **264540** | 11 | 39 / 2 / **3** / emitted | 43 / 1 / **2** / emitted |
| 264630 | 12 | 38 / 3 / **3** / emitted | 43 / 2 / **2** / emitted |

G04 is tracked by 10–12 stations throughout.  The golden reaches the solve with **2** and the vehicle with **1**, because
at hour 1 the golden's own PD rejection rate is at its worst — **38–39 of 59 stations per epoch**.  The dip closes on the
epoch the in-solve count reaches 3 (golden) / 2 (vehicle).  So the "genuine absence" class RF-NS §4.1(a) was written for
is **not instanced anywhere in these two runs**: all three stretches, on both arms, are the same station-level rejection.
(b2) below is therefore a guard for a class that is real in principle — a satellite genuinely tracked by < 2 stations —
and that this bed does not exhibit.

## 4. Pre-registered — NOT BUILT

### (b1) The station-level PD rejection — fix it at its source

**What changes.**  Not the PD test's threshold (a looser bar admits a genuinely singular block, which is what the test
was added to stop).  The defect is that an unobservable LOCAL column silently costs the station its whole epoch, when
what it should cost is that column.  Three candidates, to be ranked by a read before any build:

  1. **Pin the offending local column instead of dropping the station** — identify the null pivot's column from the LDLᵀ
     `vectorD()` already computed at `NetworkNormalEquations.cpp:456-470`, add a birth-variance prior row for it, refactor
     once, and admit the station.  Under `float_kf` that column is an ambiguity with no prior, so the pin is its birth
     prior: `network.amb_float_birth_var_m2` exists and is unset on this arm.
  2. **Drop the column** (Schur it out station-side as a sponge, the CB-ADM quarantine pattern already in
     `ApplicationNetworkAssembly.cpp:2130-2145`) and admit the station without it.
  3. **Admit the station at reduced rank** via a pseudo-inverse on the null direction, with the direction declared.

**Where.**  The decision belongs in `src/core/filter/NetworkNormalEquations.cpp::addStation` (it owns the factorization
and is the only place the null direction is visible) with the policy knob read from `NetworkKnobs`; the caller's drop
path at `ApplicationNetworkAssembly.cpp:4450-4470` stays as the backstop.  Default OFF, byte-identical when off.

**Both edges (invariant #7).**  DECOUPLING: today's drop is already an event in the log but NOT in `network_events.csv`
and NOT in a counter — the first deliverable is `TOPOLOGY sta_dropped` (station, epoch, min pivot, `d_ref`, the named
null column) plus `n_sta_pd_dropped` per epoch in `network_epoch_status.csv`, so the 29/42-per-epoch rate is visible
without grepping a rotated log.  RE-COUPLING: `TOPOLOGY sta_readmitted` with the parameter that was pinned/dropped and
its prior, so a station's return is never read as a first sighting.  Both land BEFORE the fix, as the measurement that
ranks the three candidates.

**Bars.**  (i) the vehicle field arm reaches **645/720 emitted** with **zero `gate_withheld` after TOW 261660** — the 75
ignition withholds are RF-NS §4.1(b)'s target, not this card's; (ii) the consumer's hours 3–6 no worse than
**0.84 / 0.65 / 1.37 / 0.60 m**; (iii) the sim arm **720/720** with WL ≥ 98 % kept; (iv) OFF is byte-identical on
`products.txt`, `logs/closed_loop_trace.csv` and `logs/network_epoch_status.csv`.  ⚠ The golden is in scope too — it
drops 29 stations per epoch — so (b1) ON must be graded on BOTH arms and its golden run is a posture change, not a
regression check.

### (b2) Fix (a) v2 — a target-0 pin for a column with no information, quarantined from the carry

RF-NS §4.1(a) with the three couplings RF-NS §6.2 found, cut:

- **Prerequisite:** a column-occupancy accessor on `NetworkEstimator` (`include/filter/NetworkEstimator.hpp`) — "did any
  observation row touch shared column c this epoch", off the pre-datum diagonal.  RF-NS §6.3: without it the census
  predicate cannot be written at all.
- **The pin:** one target-0 row at `kCodeBiasGaugeWeight` in the s2z block `NetworkSbasis.cpp:1181-1210`, for a
  registered code-bias column that no observation row and no datum row touches this epoch.
- **Quarantine (this is the whole difference from (a)):** the pinned column is EXCLUDED from
  `networkStashCarry` (`NetworkRunContext.cpp:6402-6417`), from veteran promotion into `jac_sats`
  (`NetworkSbasis.cpp:1193-1197`) and from park retirement (`NetworkRunContext.cpp:6512-6520`).  The park stays parked;
  the genuine `seed_code_bias` fires at the real return with its value and `var_eff` unchanged.
- **Both edges:** `CB_COVER uncovered` (sat, signal, epoch, the pin's weight) and `CB_COVER covered` at release, with
  `n_uncovered_cb_cols` per epoch in `network_epoch_status.csv` — expected 0 on a settled run, and an S-line token when
  the knob is ON.
- **Bar:** the G04 dip EMITTED on both arms (its 5 epochs), with the return seed byte-unchanged — the `seed_code_bias`
  value and `var_eff` at 264540 identical to today's events (golden −4.2219 / 8.103e-03, vehicle −3.7526 / 1.221e-02) —
  and every other epoch byte-identical.  ⚠ If (b1) lands first the G04 dip disappears with it and this bar has no
  population; (b2) then keeps its OFF-by-default guard status and is graded on a constructed case, not on this bed.

**Order.**  (b1) first and alone.  (b2) is a guard, not the fix; building it first would paper over a defect that is
costing the golden half its network on every epoch.

## 5. Run dirs, instruments

- `runs/_rf0g_field6` — the RF-0g golden, 640/720 emitted, 80 withheld (75 ignition + the 5 G04).
- `runs/_rf2d_field6_both` — the RF-1 + RF-2b + RF-2d vehicle, 573/720, 147 withheld (75 ignition + 49 G07 + 18 G30 +
  5 G04).  Unchanged by this card.
- `runs/_rfns_field6_both` — the same arm with RF-NS's re-armed eigen-census; solve byte-identical.
- `scripts/rf_obs_loss_read.py <golden_run> <vehicle_run> --sat <SAT> [--tow-min --tow-max] [--stride N] [--events]` —
  the grader: tracked / PD-dropped / in-solve / post-fit-code station counts per epoch for both arms side by side, with
  the dispositions, and the per-satellite event list on request.
- `scripts/rf_null_space_read.py` — RF-NS's eigen-census grader, unchanged.
- No build, no run: every number above is a count of rows in traces that already existed.
