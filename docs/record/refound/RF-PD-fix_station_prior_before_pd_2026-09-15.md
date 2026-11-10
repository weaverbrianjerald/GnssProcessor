# RF-PD-fix: the station prior folded before the PD test (2026-09-15)

**STATUS**

- **id:** RF-PD-fix
- **topic:** network-vehicle
- **status:** closed
- **summary:** `station_prior_before_pd` cured the PD amputation (rejections to 0, 59 of 59 stations, 720 / 720 emitted) but failed the consumer bars on the leveling golden; closed, armed in the goldens since GRB.
- **dates in the text:** 2026-09-15; annotation 2026-10-01 (card RPD)
- **knobs / posture:** `network.station_prior_before_pd` (bool, default OFF; folds only the RXCB-DATUM rows, the carry rows stay after the test), armed with `network.trace_station_pd_null` on every arm; new `TOPOLOGY sta_readmitted` event and four appended `network_run_summary.csv` columns; RF-0g golden and RF-1 + RF-2b + RF-2d vehicle, field and sim, 6 h
- **hashes named in the text:** exe `gnss_processor_rfpdfix.exe` sha256[:16] `14578F851F0C22EC` (main `73e4bd1` + the card); OFF smoke `products.txt` `9ADD1D3AB9D70657`; Debug suite 2223 tests, 2222 passed, 1 skipped
- **run dirs named in the text:** `runs/_rfpdfix_smoke_off`, `runs/_rfpdfix_rf0g_field6`, `runs/_rfpdfix_field6_both`, `runs/_rfpdfix_rf0g_sim6`, `runs/_rfpdfix_sim6_both`
- **source:** [RF-PD](RF-PD_station_rejection_2026-09-15.md) section 6 (a)

**Outcome.** Question: does folding each station's RXCB-DATUM rows before `addStation`'s PD test cure RF-PD's amputation? PD rejections go to 0 on four beds, field stations in the solve rise to 59 of 59 and both field arms emit 720 / 720; consumer bars B2b, B3d and B4c FAIL (golden field about 6 times worse: [RF-IGN](RF-IGN_ignition_born_levels_2026-09-15.md)'s ignition-born level) and B3b misses on 9 epochs. Closed: in the goldens since [GRB](GRB_golden_rebase_2026-09-22.md), replay fixed by [RPD](RPD_replay_station_prior_2026-10-01.md); leftovers moot (`_rf0hv15_field6`: 0 PD drops). Status reviewed 2026-10-13.

**Status as recorded (2026-09-15):** open — **the amputation is cured and the consumer bar is FAILED, and those are two different findings.**  RF-PD §6 (a) is
built: the station's own RXCB-DATUM rows now enter `N_rr_r` / `u_r` before `NetworkNormalEquations::addStation` factorizes and applies
its pin-aware PD test, so the test judges the matrix the SOLVE uses.  On the RF-0g field posture the PD rejection is GONE — golden
23 633 → **0** and vehicle 30 340 → **0** over 720 epochs (the 9 residual `addStation` rejections in each arm are a DIFFERENT guard, the
pre-existing non-finite-input check, and they are 38 → 9 fewer than the source runs) — stations in the solve go from a median of
**28 of 59** (golden) and **17 of 59** (vehicle) to **59 of 59 on 711 of 720 epochs, 58 on the other 9**, the RXCB-DATUM fold count rises
17 809 → 42 463 and 9 834 → 42 469, and **both field arms emit 720 / 720** (golden was 640, vehicle 573 — every gate withhold is gone).
The vehicle's own RF-2d instrument improves 5×: IF-correction dispersion p90 **1.007 → 0.201 m**, unsignalled clock steps 24 → 0.  The
wire-vs-CODE census is better or flat in every family but one hour.  **And the golden consumer degrades ~6×** — p50 by hour
3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68 → **12.51 / 7.61 / 8.38 / 8.40 / 11.17 / 13.97 m**, failing bar B3 on all six hours; the vehicle
consumer fails B4 on four of six (5.02 / 2.11 / 0.97 / 1.05 / 0.45 / 3.65 against 4.75 / 3.00 / 0.84 / 0.65 / 1.37 / 0.60).  On the SIM
beds — both also affected (27 637 / 34 422 rejections at Step 0) — the rejections likewise go to zero, all 60 stations are in the solve on
every epoch, WL integers improve to **99.1 %**, and the consumer moves the OTHER way at ignition (golden sim hour 1 6.23 → 1.98 m) while
missing the ±0.05 m/hour bar in both directions.  The knob is
DEFAULT OFF, the OFF smoke is byte-identical on all three gated artifacts (`products.txt` `9add1d3ab9d70657`), and **nothing was armed on
a golden and no ledger row was written** — the promotion is the author's call and the consumer result says it is not automatic.  Both
edges of invariant #7 are wired: `TOPOLOGY sta_pd_dropped` (RF-PD, the decoupling edge) and the new `TOPOLOGY sta_readmitted` (this card,
the re-coupling edge; 11 re-admissions per field arm, longest gap 1 epoch).

## 1. The fix as built

### 1.1 The seam, and why the prior goes INTO `addStation`

`NetworkNormalEquations::addStation` gains an optional
`StationPrior { std::vector<Eigen::Index> cols; Eigen::VectorXd dNrr; Eigen::VectorXd dur; }` — the same triple `addStationPrior` takes
— folded into a LOCAL COPY of `N_rr_r` and of `u_r_r` before the LDLᵀ factorization and therefore before the PD test.

| file:line | what |
|---|---|
| `include/filter/NetworkNormalEquations.hpp:165-249` | `StationPrior` + the `addStation` / `addStationObservations` (×2) `prior` parameter, defaulted empty |
| `src/core/filter/NetworkNormalEquations.cpp:414-441` | prior validation — the `addStationPrior` contract verbatim (size, column range, `Λ ≥ 0`, finite `η`), run BEFORE the first mutation of anything |
| `src/core/filter/NetworkNormalEquations.cpp:466-483` | the fold into `N_rr_pre` (a copy taken ONLY when the prior is non-empty) and `N_rr_use`, the matrix `.compute()` and the PD test both read |
| `src/core/filter/NetworkNormalEquations.cpp:526` | the RF-PD eigen read moves to `N_rr_use`, so a residual rejection still names the block the test rejected |
| `src/core/filter/NetworkNormalEquations.cpp:565-574` | `st.u_r = u_r_r` then the prior's `η` — same additions, same base, same ascending order `addStationPrior` would have used |
| `src/core/filter/NetworkEstimator.{hpp:165-180,cpp:199-215}` | pass-through on both `addStation` overloads |
| `src/core/app/ApplicationNetworkAssembly.cpp:4079-4098` | the ROUTING: the RXCB-DATUM row goes to `inj_prior` when armed, to `inj` (its historical position) when not |
| `src/core/app/ApplicationNetworkAssembly.cpp:4212-4239` | the `inj_prior` finalize — sort ascending + coalesce duplicate columns, exactly the `inj` shape |
| `src/core/app/ApplicationNetworkAssembly.cpp:4585-4598` | the `est.addStation(..., StationPrior{...})` call site |
| `include/filter/NetworkRunTypes.hpp:1034-1046` | `StationAssembly::prior_cols / prior_dNrr / prior_dur` |
| `include/config/GnssConfigTypes.hpp:2334-2372`, `src/core/config/GnssConfigLoadNetwork.cpp:2340-2356`, `:3463-3472`, `src/core/logging/NetworkRunTraces.cpp:1541-1546` | the knob: field + Doxygen, parse + arming log, the deferred `rx_code_bias_datum` inertness cross-check, the armed-only manifest echo |

**The failure path was chosen by being already correct, not by being rebuilt.**  `addStation` builds and validates the whole local block
BEFORE it touches the shared border (`N_ss_ += N_ss_r` / `u_s_ += u_s_r` are the last two statements, deliberately — the comment at
`:416-433` records the global zero-emit this ordering was paid for).  A rejected station therefore leaves `N_ss_` / `u_s_` untouched and
`n_active_` unadvanced, and there is NOTHING to roll back.  A two-phase add + validate would have had to re-derive that property; putting
the prior inside the existing build-then-validate window inherits it.  The prior's own validation runs before the copy, so a malformed
prior throws on exactly the same path a malformed block does (unit-tested).

**Why the prior cannot ride the existing `addStationPrior` path.**  That method applies each `Λ_c` as `Eigen::LDLT::rankUpdate` on the
CACHED factor, and Eigen's rank-1 LDLᵀ update divides by the existing pivot `d_j`.  The blocks this card admits have `d_min` at
±1e-12 relative (RF-PD §4(5)) — a rank-1 update through them is meaningless.  A prior that must cure a rank defect has to be inside the
matrix that is factorized.

### 1.2 Double-apply: the routing is exclusive at the emplace

`R.carry_cols` is a COALESCED vector — one column can carry a phase-leveling row, an rx-CB carry row, an oracle-pin row AND the
RXCB-DATUM row summed together (`ApplicationNetworkAssembly.cpp:4173`'s comment: `addStationPrior` "does not sum" duplicate entries, which
is why they are coalesced in the first place).  Subtracting the datum's share back out of a summed entry is not exact in floating point,
so the fix does not subtract: the datum row is emplaced into **either** `inj_prior` **or** `inj`, never both, at its single build site
(`:4079-4098`).  Consequences, all checked:

- Knob OFF: the emplace is the original statement at the original point in the walk, `inj_prior` stays empty, `R.prior_*` stays empty,
  `addStation` takes no copy and folds nothing.  **Byte-identical** — proven by the OFF smoke (§3).
- Knob ON with the carry also armed: `R.carry_cols` no longer contains the datum rows, `addStationPrior` at `:4661` receives only the
  carry, and the gate's `!R.carry_cols.empty()` conjunct suppresses the call entirely on a datum-only posture.
- The positive check (invariant #4): `n_rxcb_gauges` — accumulated at `NetworkRunContext.cpp:5720-5727` from `R.pl_cb_datum_rows`, which
  is incremented exactly ONCE per row in both branches, over stations that reached the solve — goes **17 809 → 42 463** (golden) and
  **9 834 → 42 469** (vehicle).  The datum reaches 2.4× / 4.3× more station-epochs, and the count is a row count, not a fold count, so a
  double-apply would not show here; what shows here is that the mechanism is LIVE and that the rows follow the stations.

**Scope — datum rows only.**  Per RF-PD §6 (a)'s warning, the phase-leveling and rx-CB CARRY rows stay on the post-test path: folding a
carried value into the ADMISSION decision would let the carry's own staleness decide whether a station is in the solve, which is
invariant #7's decoupling edge and needs its own card.  Nothing else about the rejection moved — same median-scale test, same relative
bar, same drop, same RF-PD event.

### 1.3 Both edges (invariant #7) and the truthful count

The DECOUPLING edge was already instrumented by RF-PD (`TOPOLOGY sta_pd_dropped`, `n_sta_pd_dropped`).  This card adds the RE-COUPLING
edge and the count the whole defect hid behind:

- **`TOPOLOGY sta_readmitted`** — `ApplicationNetworkAssembly.cpp:4684-4728`, one row per station that re-enters the solve after ≥ 1
  absent consume epoch, carrying `statistic` = epochs absent and `detail` = `epochs_absent=N;prior_rows=K;pre_pd_prior=0|1`.  A station
  that has never been in the solve is a FIRST SIGHTING and emits nothing — the distinction invariant #7 names.  Event rows ride
  `network.trace_topology`; the counters are unconditional.  Skipped on an ABORTED epoch (a pivot rejection breaks the consume loop
  early), where a later station's "absence" would be an artifact of the break.
- **Counters** in `include/app/NetworkRunContext.hpp:2374-2393` (`NetworkServiceCounters`, per the INC-12 rule — no function-local):
  `sta_solve_track` (per station: `ever_in_solve`, `absent_streak`), `sta_in_solve_ep` / `sta_in_solve_epochs` /
  `sta_in_solve_min` / `sta_in_solve_min_tow`, `sta_readmit_events`, `sta_readmit_max_gap_ep`.
- **Run-end tail** — `src/core/filter/NetworkTeardown.cpp:363-390`, and UNCONDITIONAL unlike RF-PD's armed-only readout: an unarmed run
  has exactly the same need to know that `n_present` is not the number of stations in the solve, and the count is in no other artifact.
- **Run summary** — `src/core/logging/NetworkRunTraces.cpp:2117-2143`: four APPENDED columns on `network_run_summary.csv`,
  `n_sta_in_solve_min`, `n_sta_in_solve_med`, `n_sta_pd_dropped`, `n_sta_readmitted`.  The existing ten keep their order and meaning, so a
  positional reader of the old schema is unaffected.
- **No per-epoch trace column was added, and `network_epoch_status.csv`'s header is untouched** (it is one of the three byte-identity
  gates).  The per-epoch in-solve count is `n_present` minus the `sta_pd_dropped` rows at that TOW in `network_events.csv` — an UNGATED
  trace — which is what the per-hour tables in §5 are read from; the run-end tail and `network_run_summary.csv` carry the min/median.

## 2. Step 0 — are the SIM beds affected?

Counted over all `logs/main_log*.log` rotations, oldest first:

| source run | `addStation rejected` |
|---|---|
| `runs/_rf0g_sim6` | **27 637** |
| `runs/_rf2d_sim6_both` | **34 422** |
| `runs/_rf0g_field6` | 23 633 |
| `runs/_rf2d_field6_both` | 30 340 |

**Neither sim bed is clean** — both carry MORE rejections than their field twin.  The byte-identity form of bar B2 therefore does not
apply to either; B2 is graded in its outcome form (720/720 kept, consumer within 0.05 m/hour, vehicle-sim WL ≥ 98 %).

## 3. Build, hashes, identity

- Exe `build/x64-Release/gnss_processor_rfpdfix.exe` sha256[:16] **`14578F851F0C22EC`** (main `73e4bd1` + this card).
- Debug unit suite, verbatim: **`2223 tests from 293 test suites ran.`** / **`[  PASSED  ] 2222 tests.`** /
  **`[  SKIPPED ] 1 test`** — `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the standing bootstrap-gated skip.  That is the
  2216-passing baseline + this card's 6 cases in 1 new suite.
- **OFF smoke** `runs/_rfpdfix_smoke_off` (the `_rf2d_smoke_off` config, paths only), exit 0:

| artifact | ON-build, knob OFF | `runs/_rf2d_smoke_off` | |
|---|---|---|---|
| `products.txt` | `9ADD1D3AB9D70657` | `9ADD1D3AB9D70657` | ✔ the canonical hash |
| `logs/closed_loop_trace.csv` | `7476F0A064413FF4` | `7476F0A064413FF4` | ✔ |
| `logs/network_epoch_status.csv` | `A227A6CD9E44E7F0` | `A227A6CD9E44E7F0` | ✔ |

`"station_prior_before_pd"` is absent from the OFF run's `network_run_manifest.json`.

### 3.1 Unit test

`tests/test_neq_station_prior_before_pd.cpp`, suite `NeqStationPriorBeforePdTest` (new file, new suite name, explicit `CMakeLists.txt`
entry at line 338), 6 cases:

1. `SyntheticBlockCarriesTheCardsRankOneNull` — the fixture is RF-PD §5's mechanism in miniature (rows `C1/L1/C2/L2` per satellite over
   `{rx DCB, slant iono, N1, N2}`, `d₂ = γd₁`).  Asserts `n_eig = 1` and the null's mass ratio `(b²+c²)/a² = 1 + γ²` to 1e-6 — so the
   admission test below cannot pass on a merely ill-conditioned block.
2. `RejectedWithoutPriorAdmittedWithPriorBeforePd` — **the card**: the same rows are REJECTED with the prior empty, ADMITTED with it, and
   the post-test path cannot rescue the rejected station (`addStationPrior` throws `out_of_range` — it was never added).
3. `RejectsMalformedPriorAndLeavesTheBorderClean` — out-of-range column / negative Λ / non-finite η / size mismatch all throw, and the
   reduced shared border is exactly zero afterwards.
4. `AdmittedEitherWayGivesTheSameSystem` — for a block both orders admit: `N_sr` and `u_r` **bit-identical**; `N_rr`, the reduced shared
   normals/RHS and the reconstructed locals agree to 1e-12 relative (1e-10 for the back-substitution).  Not bit-exact BY CONSTRUCTION —
   `LDLT::rankUpdate` and a fresh `LDLT` of the same summed matrix are different algorithms — and the tolerance is taken against the
   OPERAND scale, because the reduced border is a small difference of large numbers and grading it against its own maximum would be
   grading cancellation noise (the test asserts the border is non-degenerate first).
5. `EmptyPriorIsTheLegacyCallBitForBit` — the OFF path equals the no-argument overload on every accessor.
6. `PriorOnZeroLocalStationThrows`.

## 4. The bars

Graders: `python scripts/rf2d_grade.py <runs>` (consumer p50/p90 by hour in metres, emitted/withheld, IF dispersion);
`python scripts/rf_pd_null_read.py <run>` + the `addStation rejected` count over `logs/main_log*.log`;
`python scripts/simnet_amb_integer.py runs/simnet60_quiet_wu93_24h --run <run>` for the sim WL;
`python scripts/wire_vs_code_census.py --run …` for the wire census.

| bar | pre-registered | measured | |
|---|---|---|---|
| **B1** OFF smoke byte-identical | `products.txt` = `9add1d3ab9d70657` | `9ADD1D3AB9D70657`, and `closed_loop_trace.csv` / `network_epoch_status.csv` byte-identical too | **PASS** |
| **B2a** sim beds 720/720 (outcome form — Step 0 found rejections on BOTH, so the byte-identity form does not apply) | 720/720 | **720 / 720** on both, 0 rejections, 60/60 in solve | **PASS** |
| **B2b** sim consumer within 0.05 m/hour | ±0.05 | golden Δ −4.25 / −1.54 / **+0.51** / **+0.32** / −0.24 / −0.31; vehicle Δ −1.33 / **+0.32** / **+0.16** / +0.02 / −0.09 / **+0.13** | **FAIL** (11 of 12 hours outside ±0.05, mixed sign; §4.1) |
| **B2c** vehicle-sim WL ≥ 98 % | ≥ 98 % | **99.1 %** (source, same read: 96.7 %) | **PASS** |
| **B3a** golden field: `addStation rejected` = 0 | 0 | **0 PD rejections**; 9 total, all the non-finite-input guard (`no_pd_record`) on GUAM/KIT3/MAJU/PARK — 38 → 9 fewer than the source | **PASS on the PD test; the literal total is 9, not 0** |
| **B3b** in-solve 59 on every emitted epoch | 59 | 59 on **711** of 720 emitted epochs, **58** on the 9 epochs of B3a | **FAIL (by 1 station on 9 epochs, from the non-PD guard)** |
| **B3c** golden emitted ≥ 640/720 | ≥ 640 | **720 / 720** (0 withheld; was 640) | **PASS** |
| **B3d** golden consumer p50 no hour worse by > 0.10 m | 3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68 | **12.51 / 7.61 / 8.38 / 8.40 / 11.17 / 13.97** | **FAIL, all six hours** (+8.74 / +6.53 / +7.10 / +7.15 / +9.39 / +11.29) |
| **B4a** vehicle field: rejections 0 | 0 | **0 PD rejections**; 9 total, the same four stations and the same guard | **PASS on the PD test; literal total 9** |
| **B4b** vehicle emitted ≥ 640/720 | ≥ 640 | **720 / 720** (was 573; all 147 gate withholds gone, including the G07/G30/G04 stretches) | **PASS** |
| **B4c** vehicle consumer p50 no hour worse by > 0.10 m | 4.75 / 3.00 / 0.84 / 0.65 / 1.37 / 0.60 | **5.02 / 2.11 / 0.97 / 1.05 / 0.45 / 3.65** | **FAIL, 4 of 6** (+0.27 / −0.89 / +0.13 / +0.40 / −0.92 / +3.05) |

### 4.1 B2 — the sim beds

| | golden sim source | `_rfpdfix_rf0g_sim6` | vehicle sim source | `_rfpdfix_sim6_both` |
|---|---|---|---|---|
| exit / wall | 0 / 142 s | 0 / 196 s | 0 / 178 s | 0 / 198 s |
| emitted | 720 / 720 | **720 / 720** | 720 / 720 | **720 / 720** |
| `addStation rejected` | 27 637 | **0** | 34 422 | **0** |
| `n_sta_in_solve` min/med/max | — | **60 / 60 / 60**, 0 re-admissions | — | **60 / 60 / 60**, 0 re-admissions |
| consumer p50 by hour (m) | 6.23 / 3.30 / 0.86 / 0.50 / 0.46 / 0.55 | **1.98 / 1.76 / 1.37 / 0.82 / 0.22 / 0.24** | 3.14 / 0.47 / 0.24 / 0.24 / 0.28 / 0.33 | **1.81 / 0.79 / 0.40 / 0.26 / 0.19 / 0.46** |
| consumer p90 by hour (m) | 8.47 / 4.46 / 1.22 / 0.74 / 0.86 / 0.79 | 5.36 / 1.85 / 1.63 / 1.29 / 0.57 / 0.52 | 5.72 / 0.93 / 0.29 / 0.29 / 0.45 / 0.38 | 5.41 / 1.23 / 0.48 / 0.30 / 0.62 / 0.53 |
| IF dispersion p50 / p90 | 0.050 / 0.156 | 0.152 / 0.459 | 0.022 / 0.043 | 0.014 / 0.051 |
| WL at own true integer (ref-free, last 25 %) | — | — | 96.7 % | **99.1 %** |

- **720 / 720 kept on both beds — PASS.**  The sim rejections go to zero and all 60 stations are in the solve on every epoch of both
  arms, with zero re-admissions (no station ever left).
- **vehicle-sim WL ≥ 98 % — PASS at 99.1 %**, on the ref-free last-25 % `round(e)=0` read RF-2b quotes; the source run scores 96.7 % on
  the identical read (all bands improve: WL med|e| 0.047 → 0.029 cyc, and the chord population goes from 540 to 48 816 rows because the
  DD trace only sees stations that are IN the solve — the source sim solved as a ~20-station network).
- **consumer within 0.05 m/hour — FAIL, literally, in both directions.**  Golden sim Δ (ON − source) = **−4.25 / −1.54 / +0.51 / +0.32 /
  −0.24 / −0.31**; vehicle sim Δ = **−1.33 / +0.32 / +0.16 / +0.02 / −0.09 / +0.13**.  Only vehicle hour 4 is inside ±0.05 m.  The sign
  is mixed and the ignition improvement is large (golden hour 1 6.23 → 1.98 m, hour 2 3.30 → 1.76), which is the opposite sign to the
  golden FIELD result and is the second thing §5.3's open question has to explain.

## 5. Report-only

### 5.1 Stations IN THE SOLVE, per hour (min / median / max of 59)

Read as `n_present` minus the `sta_pd_dropped` rows at that TOW.  OFF is read from `runs/_rfpd_*`, the RF-PD instrument runs the card
proved byte-identical to `_rf0g_field6` / `_rf2d_field6_both` on all three gated artifacts.

| arm | h1 | h2 | h3 | h4 | h5 | h6 | run |
|---|---|---|---|---|---|---|---|
| golden OFF | 11/13/17 | 18/20/23 | 23/26/28 | 28/30/31 | 31/33/33 | 33/34/34 | 11 / **28** / 34 |
| golden ON | 58/59/59 | 59/59/59 | 58/59/59 | 58/59/59 | 58/59/59 | 58/59/59 | 58 / **59** / 59 |
| vehicle OFF | 11/13/15 | 15/16/16 | 16/17/17 | 17/17/18 | 18/19/19 | 19/19/22 | 11 / **17** / 22 |
| vehicle ON | 58/59/59 | 59/59/59 | 58/59/59 | 58/59/59 | 58/59/59 | 58/59/59 | 58 / **59** / 59 |

The OFF golden's monotone hourly climb is the anchor roster filling (RF-PD §2); it is gone because survival no longer depends on an
anchor pin landing in the station's own block.  Re-admissions: **11** per field arm, longest gap **1** epoch — i.e. the remaining
absences are single-epoch and self-healing, which is the shape a drop SHOULD have.

### 5.2 Consumer p90 by hour (m), and the IF-correction dispersion

| arm | p90 by hour | IF dispersion p50 / p90 / max (m) | CLK_STEP |
|---|---|---|---|
| golden OFF | 5.30 / 3.54 / 1.64 / 1.35 / 2.64 / 3.86 | 0.175 / **0.512** / 1.50 | 25 |
| golden ON | 14.45 / 8.91 / 9.36 / 10.38 / 17.20 / 16.00 | 0.172 / **0.547** / 3.12 | 1 |
| vehicle OFF | 6.43 / 4.32 / 2.55 / 0.74 / 2.10 / 3.12 | 0.248 / **1.007** / 2.70 | 24 |
| vehicle ON | 5.46 / 3.61 / 1.27 / 2.39 / 0.94 / 5.35 | 0.055 / **0.201** / 0.68 | 0 |

RF-2d's bars were golden 0.512 and vehicle 1.007.  The golden is flat (0.547); **the vehicle improves 5×** and its unsignalled clock
steps go to zero — the wire's per-satellite consistency is better with the stations back, on the vehicle decisively.

### 5.3 Wire-vs-CODE census (`scripts/wire_vs_code_census.py`, 4 runs, ~4 min)

W2 (`_rfpdfix_rf0g_field6` / `_rf0g_field6` within-arc rms ratio, hours 2–5): phase tracking 0.569 / 0.631 / 0.927 / 0.947; phase
declared 0.744 / 0.141 / 0.645 / 0.592; code tracking 0.574 / 0.740 / 0.968 / 1.055; code declared 0.477 / 0.716 / 0.863 / 1.104.
**"Worse by > 10 % in hours: none"** on seven of the eight families; the eighth (code declared, cm = diffmedian) only at hour 5.
Arcs with a step > 0.25 m, tracking / declared: golden **230/303 and 370/1020 → 89/116 and 139/1165** (code), vehicle
**286/483 and 325/636 → 91/116 and 147/703**.

**This is the finding to carry forward: the wire got BETTER against CODE while the golden consumer got 6× worse.**  W2's contrast is
de-commoned per epoch (`D = X − median_s X`), so it is blind by construction to any per-epoch COMMON-MODE change — and the common mode is
exactly what this fix moves: the RXCB datum's realized common-mode eigenvalue is `1/σ²` regardless of station count, so realizing it over
59 receivers instead of 28 re-levels the receiver-DCB common mode, which by RF-PD §5's own algebra (`a = −d₁η`) trades one-for-one against
the SLANT IONO level — and RF-0b's posture requires the consumer to apply the emitted ionosphere and code biases AS A PAIR.  A level shift
there is invisible to W2 and lands in full on the consumer.  **That is a hypothesis consistent with the measurements, NOT a tested claim**
— it is the first thing to measure, and the instrument for it already exists (`scripts/gim_global_mean_series.py`, and CLAUDE.md's
amendment 6 corollary on the rx-CB carry's common-mode memory).  Nothing was tuned to chase it.

## 6. Runs

- `runs/_rfpdfix_smoke_off` — OFF smoke, exit 0, byte-identical (§3).
- `runs/_rfpdfix_rf0g_field6` — RF-0g golden + the knob, exit 0, 280 s.
- `runs/_rfpdfix_field6_both` — RF-1+2b+2d vehicle + the knob, exit 0, 281 s.
- `runs/_rfpdfix_rf0g_sim6`, `runs/_rfpdfix_sim6_both` — the two sim beds, §4.1.

Each arm is its source `arm.json` with ONLY the run path changed plus `station_prior_before_pd: true` and `trace_station_pd_null: true`
under `network` (`git diff`-equivalent: 2 added lines + 1 changed path per arm).  **The second knob is a deliberate deviation from
"the knob under test only"**: bar B3 asks for any residual rejection to be named with its `sta_pd_dropped` family, which is impossible
without it, and RF-PD §3.1 certified it solve-byte-identical on these exact two postures.  It is what produced the "all 9 residuals are
`no_pd_record`" reading.

## 7. What stays open

1. **The consumer.**  B2b, B3d and B4c FAIL.  The fix is not promotable on this evidence, and the split in §5.3 says where to look first
   — a common-mode / level re-realization, not per-satellite wire quality.  The sim beds sharpen the question rather than settle it: the
   same change that costs the golden FIELD consumer 6× makes the golden SIM consumer 3× BETTER at ignition, so whatever moves is not a
   property of the mechanism alone but of the bed it runs on (the sim has no real receiver DCB spread to re-level).  Untested.
2. **The 9 residual rejections** are the non-finite-input LDLᵀ guard, not the PD test: GUAM00GUM ×2, KIT300UZB ×2, MAJU00MHL ×3,
   PARK00AUS ×2, all `no_pd_record`.  They pre-date this card (38 in each source run) and are a separate defect — a NaN/Inf reaching
   `N_rr_r` from the assembly is its own card, and `addStation`'s message conflates it with the PD test.
3. **The 75 ignition withholds** RF-0g owes (carry-cold completion, RF-NS §4 fix (b)) — untouched here; note the golden now emits
   720/720, so the availability half of that item is answered by this card and the QUALITY half is not.
4. **RF-2c** — untouched.
5. **The carry rows are still judged after the test** (§1.2 scope): folding them early would put carry staleness into the admission
   decision, invariant #7's decoupling edge, and needs its own card.
6. **Ledger / golden promotion is the author's call** and nothing was written: no config under `config/golden/` was touched,
   `docs/CONFIG_promotion_ledger.{md,json}` were not touched, and the knob is default OFF in the loader.

## 8. Annotation, 2026-10-01 (card RPD)

The within-epoch RAIM replay (`resolve_dropping`, `src/core/filter/NetworkSolveGlue.cpp`) re-added
each station without the `StationPrior` of section 1.1, so with this knob armed no replay carried the
RXCB-DATUM rows: a station that needed them was rejected inside the replay (aborting it), and the
others re-solved without the datum. The arms of section 6 adapted on 14 epochs (13 on
`_rfpdfix_rf0g_field6`, 1 on `_rfpdfix_field6_both`, none rejected), each re-solved without the rows;
the primary solves and the PD census above are unaffected. Fixed in card
[RPD](RPD_replay_station_prior_2026-10-01.md).
