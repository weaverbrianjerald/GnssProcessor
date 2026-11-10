# SPEC — SD EWL integer fix + zero-q hold (`network.sd_ewl.*` increment 2)

Handoff. Read `docs/SPEC_sd_ewl_prereduce.md` first — this builds on its increment 1
(float pseudo-observation, built, default-OFF, rows confirmed building).

Status: **2a + 2b BUILT 2026-07-29, default OFF (`network.sd_ewl.fix_enable`). 2c (the
zero-q hold) stays DEFERRED.** Route B was built as specified in §3 and the precondition
in §3 (Route A blocked on `bia_corrected = false`) is unchanged and untouched — Route B
needs no bias a-priori, which is why it was the one built.

Read `docs/SPEC_lane_spectrometer.md` **§12** before touching this card: it is the
implementation record and it AMENDS this document in five places. The one that changes the
algebra is new and not in §6 there either — Route B's `B^s − B^p` cancellation requires
the two stations' MW arcs to sit on the **same tracking-channel pair**, which increment 1
measured they often do not (41 % of arcs have two candidates; `L7X/L5X` and `L7Q/L5Q` each
win at different satellites). 2a therefore elects a network-common channel pair per
(constellation, satellite) and excludes the rest — measured cost 3222 excluded vs 2727
admitted arcs, i.e. over half — so §3's "67 distinct GPS SD pairs, 80 Galileo, one
connected component each" is optimistic under channel matching. The other four: the A1
double floor, the A2 frame-extension rule, fixed/float rows in SEPARATE whitened groups,
and the invariant-#1 flip response that a per-epoch re-decision still needs.

First measured behaviour (10-epoch smoke, 50 sta GPS+Gal, `min_arc_epochs = 2`): 2727 arcs
admitted, 1092 DD roundings, **1079 usable integers**, 34 rejected on |frac| and 493 on
FFR, 49 flips counted and vetoed, 0 reference re-elections; **174 fixed rows in 118
all-fixed whitened groups** beside 2353 float rows, 0 channel mismatches. The card is
alive; the gate-3 field verdict is still owed.

---

## 1. What you are building

Increment 1 folds one float pseudo-observation row per (station, satellite ≠ pivot) on the
constellation's extra-wide-lane band pair:

```
z_SD = MW̄(r,s) − MW̄(r,p) = λ_EWL·∇N_EWL + [B_EWL^s − B_EWL^p]
                              └ station-local ┘  └ shared border ┘
```

Increment 2 fixes `∇N_EWL` to an integer, holds it across epochs at **zero process
noise**, and thereby converts each row from a soft constraint on a *sum* into a **direct
observation of the shared satellite bias combination**:

```
[B_EWL^s − B_EWL^p] = z_SD − λ_EWL·∇N_EWL^fixed
```

Three sub-increments, in this order, no reordering: **2a obtain → 2b fix → 2c hold.**

## 2. Why the float layer is not enough (the argument in four lines)

`MW = φ_WL − P_NL` is a linear combination of `φ1, φ2, P1, P2` — observables the epoch's
normal equations already hold. A float MW row therefore adds **zero new rank** at that
epoch. Its only genuinely new content is (a) **integrality**, which increment 1 discards,
and (b) the arc's **temporal constancy**, which the Card-4 DD carry and the shared bias
carry already partly transmit with no cross-covariance in the fold. With `∇N_EWL` floating,
the row's information splits between a station-local nuisance with effectively no prior and
the shared bias — so the nuisance absorbs nearly all of it. Fixing the integer removes the
local degree of freedom rather than estimating it, and hands the whole row weight to the
bias. That is the mechanism; everything below is about earning the right to do it.

## 3. Increment 2a — obtain the integer (THE PRECONDITION)

Rounding `z_SD/λ_EWL` requires `B^s − B^p` removed first, which is what you are solving
for. Circular. Two ways out.

### Route A — round against the a-priori satellite bias

Needs no co-visibility. **Currently blocked, measured:** all 4138 EWL arcs on the
50-station fixture returned `bia_corrected = false` — `MwSlipDetector::computeSatMwBias`
cannot form the MW bias on the `(b2,b5)` pair, so the integer and the bias sit conflated in
one number. Unblocking means extending BIA MW-bias formation to the third band. Note the
inherited risk: an integer rounded against an a-priori is poisoned by errors in that
a-priori, permanently, for every downstream consumer.

### Route B — between-station DD of the SD EWL (PREFERRED)

```
[MW̄(A,s) − MW̄(A,p)] − [MW̄(B,s) − MW̄(B,p)] = λ_EWL·[(N_As−N_Ap) − (N_Bs−N_Bp)]
```

`B^s − B^p` cancels **exactly** (same satellite pair; each station's receiver term already
cancelled inside its own SD). What remains is a pure integer combination × λ_EWL — 5.861 m
GPS (2,5), 9.768 m Galileo (7,5), 19.537 m Galileo (8,5) — against decimetre averaged MW
noise. Roundable with **no bias knowledge at all**, which is why it is preferred: an
integer that uses no a-priori cannot be poisoned by one.

Cost: needs pairwise co-visibility. Measured on the fixture — abundant: **67 distinct GPS
SD pairs, 80 Galileo, one connected component each** over 17 GPS / 21 Galileo satellites.

**Decision criterion:** build Route B. Fall back to A only if B's measured fix rate on the
50-station fixture is below the FFR gate (below), and say so in the report.

**Validation before 2b:** FFR (not ADOP) on the rounded DD, per the standing AR criterion.
The existing decision tiers already fix at 100 % FFR with mean `R = 1e-4 cyc²` — that is
the bar to reproduce, on the DD-of-SD combination rather than the chord basis.

## 4. Increment 2b — fold the fixed integer

In `Application::networkAppendSdEwlRows_`
(`src/core/app/ApplicationNetworkAssembly.cpp`), when the row's `∇N_EWL` is fixed:

- subtract `λ_EWL·∇N_EWL^fixed` from the row's RHS;
- **zero the four local ambiguity coefficients** — the local DOF is removed, not
  constrained. Do NOT leave them in with a tight weight; that is a stiff prior on a
  gauge-adjacent local column, not a rank reduction.
- keep the eight shared PB/CB coefficients unchanged (SPEC_sd_ewl_prereduce §3);
- keep the correction frame (§3b there) — `pred_m` must drop its ambiguity terms in step
  with the design, or the row is folded in a mixed frame. **Both live in one place for
  this reason; keep them there.**

Rows whose integer is not fixed stay on the increment-1 float path — the two must coexist
per row, not per run.

## 5. Increment 2c — hold at zero process noise

An ambiguity is exactly constant along an unbroken arc, so `q = 0` is the physically
correct process noise, and holding it while the biases keep their `q_pb`/`q_cb` is what
makes every later epoch a fresh bias observation. This is also **standing invariant #1**
territory, which this codebase has paid for twice (phase-leveling zero-q carry needed a
slip detector retrofitted; Card-4 invariant DD carry's slip hole = **10.9 cyc/ep field
FAIL**, fixed to 2.4 only by the slip re-tap). The failure mode is a slow non-recovering
warp, not a crash — cf. `q_cb = 1e-10` giving a 49 m step with τ = 3.79 h.

**Non-negotiable design rules:**

1. **Hold ZD, difference at use.** Hold `N_EWL(r,s)` per (station, satellite)
   *undifferenced*; form the between-satellite difference at row-build time. Holding an
   SD value referenced to the station's pivot would make a **pivot re-election** a state
   event and drag the whole card into `NetworkChainDatum`'s ordering contract. Increment
   1's pivot election is per-epoch and stateless — keep it that way. This also halves the
   event surface (two arcs per held value, not four plus an election).
2. **Slip response wired in the SAME increment**, never a follow-up. A slip on either
   contributing arc drops the held integer (`resetSatelliteSignal` already gives
   per-(satellite, signal) granularity; PASS-A's slip funnel — `carry_slipped_ambs` —
   is the recording site, same place the Iono-2 SH-dev erase hooks in).
3. **Gap / re-acquisition → DROP, not carry.** Reset-over-repair.
4. **Every event class gets a counter and a run-end line.** A zero-q carry with no event
   counters is inert-and-invisible — that is how both prior instances shipped broken.
5. Grade on **datum-invariant / SD quantities only**.

## 6. Where the code goes

| concern | location |
|---|---|
| row build (float + fixed) | `Application::networkAppendSdEwlRows_`, `src/core/app/ApplicationNetworkAssembly.cpp` |
| declaration + Doxygen | `include/app/Application.hpp` |
| DD-of-SD integer decision (2a) | NEW seam `filter/NetworkSdEwlAr.{hpp,cpp}` — free function over `(NetworkRunContext&, const GnssConfig&, log, NetworkRunTraces&, …)`, called from the epoch loop; **add to `CMakeLists.txt` by hand** (explicit source list, no globbing) |
| held integers + event counters | `app/NetworkRunContext` (`NetworkCarryState` / `NetworkServiceCounters`) — never a function-local |
| config knobs | `SdEwlConfig` in `include/config/GnssConfigTypes.hpp`; parse + clamp + ON-summary in `src/core/config/GnssConfigLoadNetwork.cpp` |
| per-station tallies | `NetStation::SdEwlCounters`, `include/filter/NetworkRunTypes.hpp` |
| run-end readout | `src/core/filter/NetworkTeardown.cpp` |
| trace | `NetworkRunTraces::sd_ewl_trace` → `logs/network_sd_ewl.csv`; rows buffer in `StationAssembly::sd_ewl_trace`, drained SERIALLY in `networkPassB_` (produce is parallel — never touch a shared stream there) |
| statistic source | `MwSlipDetector::arcZdMean` — serves `exists`, `code1`, `code2`, `bia_corrected`, `count` (lagged) |
| tests | NEW file, new suite names (one suite per file); `tests/test_sd_ewl_prereduce.cpp` owns `SdEwl{Algebra,Whitening,PivotElection,Config,ArcSource,ChannelSearch}Test` |

New knobs (default OFF ⇒ byte-identical): `sd_ewl.fix_enable`, `sd_ewl.fix_min_ffr`,
`sd_ewl.fix_max_frac_cyc`, `sd_ewl.hold_enable`, `sd_ewl.hold_max_gap_s`.

## 7. Counters (invariant #4 — or it didn't happen)

`dd_pairs_seen`, `dd_fix_attempts`, `dd_fixed`, `dd_reject_frac`, `dd_reject_ffr`,
`rows_fixed`, `rows_float`, `held_alive`, `held_dropped_slip`, `held_dropped_gap`,
`held_dropped_pivotless`, `held_reused`. Run-end line must name the dominant cause when
`rows_fixed == 0`, as the increment-1 line does.

## 8. Gates

1. **OFF byte-identical.** All new knobs default false ⇒ artifacts identical to
   `runs/sdewl_on30/` (increment-1 ON) and to `runs/sdewl_byteid_off/` with `sd_ewl` off.
   Same-build comparison only. NB `products.txt` differs in its `G` generation-timestamp
   record and several service CSVs are APPEND-mode — clear `C:/GnssProcessor/logs` before
   each arm or a stale block reads as a difference.
2. `gnss_tests` green. Unit gates: the DD-of-SD cancellation of `B^s − B^p` (exact); the
   fixed-row design having **zero** local ambiguity coefficients; the correction frame
   still vanishing at the linearization point with the ambiguity terms dropped; each
   event class dropping the held value.
3. **Field, 50-sta GPS+Gal 30-ep fixture**, baseline `runs/golden_0729/net50_wl1off`
   (`ev1` = 0.1390, mean of last 10 epochs). Reference scale: the DD-chord fold bought
   **+0.6 %** (`singletop_base` 0.14135 → `singletop_fold` 0.14220).
   - PRIMARY: `ev1` lift ≫ 0.6 %, **surviving** the tail guard and an honest
     `sigma_floor_m` (see §9).
   - SECONDARY, and this is where increment 1 fails today: must NOT degrade
     `clock_std_m` (8.52 m), `worst_dev_m` (16.31 m), or D5 churn (1279 flags/run).
   - Fixed rows must show `resid_m` at the MW code-noise scale with **no per-satellite
     offset** — that is also the read on whether the BIA frame skew (§9) is real.
4. Convergence-speed claims are **not gradeable on this fixture**: 31 epochs at 30 s with
   `min_arc_epochs = 10` leaves a 21-epoch row window. Use a longer lineage or the UWL
   cold-fix path (whose λ floor deliberately excludes GPS's 5.861 m EWL).

## 9. Traps already paid for — do not re-learn

- `NetworkEstimator::phaseBiasColumn` / `codeBiasColumn` / `clockColumn` are `index_.at()`
  and **THROW**. A bare call in the parallel produce phase escapes to the produce loop's
  `catch`, marks the station `threw`, and **drops its whole block**. Cost: 961 station-epoch
  drops on a run whose counters read "0 rows built" — it looked like a clean inert control.
  Wrap them.
- A loop that aborts truncates its own counters. Always emit a denominator (`sat_seen`).
- The row must be in the **correction frame** `y = z − H·x_lin`. Raw `z_SD` runs to
  hundreds of metres (measured mean 203 m, max 4162 m) against a 0.13 m σ; folding raw gave
  `solveSharedConstrained: … not positive-definite` by epoch 11
  (`runs/sdewl_on30_RAWFRAME_INVALID/`).
- Shared PB/CB station states are allocated at 0 and never written back, so their `x_lin`
  term is zero *today* — compute it anyway rather than assume it.
- The MW arc key's tracking channel is the **detector's** choice. Search candidate
  `(band1, band2)` phase pairs in ascending order; do not elect one. Measured: 41 % of
  arcs had 2 candidates, and both `L7X/L5X` and `L7Q/L5Q` won at different satellites.
- Satellite CODE-bias columns come from `MwArcZdMean::code1/code2`, not from
  `"C" + band + phase attribute` — `matchChannel` falls back to the band's first code.
- **`arcZdMean` is NOT L1-referenced-only.** `collectUncombinedPairs` enumerates every band
  pair present; only 106 of 8308 visits had no EWL arc. The earlier "that pair is never
  created" reading was wrong.
- Increment 1's residuals: median 1.02 m, p90 3.50, p95 5.04, **p99 1349 m, max 1.85e6 m**
  — a 2.3 % pathological-`x_lin` tail, and a `sigma_floor_m` of 0.05 m that is 13–60×
  optimistic against the measured spread. Arm `max_abs_z_m` and set `sigma_floor_m` from
  measurement. `_sdewl_on30_guard.json` / `_sdewl_on30_guard_sig.json` are that ladder.
  **Ladder COMPLETE — §2's rank argument is confirmed by measurement.** ev1 vs the
  `net50_wl1off` baseline (0.139037): unguarded σ 0.05 m → **+6.78 %**; tail-capped at 10 m
  → +5.81 % (so the lift was *not* the 3.5 % pathological tail); honest σ 1.0 m →
  **+0.51 %**, with every secondary metric back to baseline or better (`clock_std_m` 8.475
  vs 8.520, `worst_dev_m` 16.157 vs 16.309, D5 1345 vs 1279). +0.51 % is real (paired,
  12/12 epochs, |mean|/se = 14.6) but is *below* the 0.6 % the DD-chord fold bought.
  **So the float layer's entire apparent value was over-weighting — ~13×.** Increment 1
  stays OFF; increment 2 is the only version of this card worth building, which is what
  §2 predicts on first principles. Full table in `SPEC_sd_ewl_prereduce.md` §7.4.
- **Open exposure:** all 4138 arcs came `bia_corrected = false`, so the arc carries the
  *total* satellite bias. If the estimator applied an a-priori OSB the detector did not,
  the row absorbs it as a per-satellite constant. `skip_mixed_frame` only checks the two
  arcs against each other, not against the estimator. Route B is immune; Route A is not.
- The previously **falsified** MW/WL integer tiers and WL-1 float layer were measured on
  the **DD chord basis**, where 80.7 % of each cycle's arcs are eq34 tree pins at absolute
  0. This card changes the basis. Do not cite that verdict as covering this one — and do
  not assume it doesn't, either. Measure.

## 10. Run recipe

```
build/x64-Release/gnss_processor.exe config/test_suite/<arm>.json
```

Release only (Debug is 50–100× slower — compile checks and `gnss_tests` only). Concurrent
sessions are normal: if `LNK1104` hits, relink a renamed exe rather than killing another
session's run. Arms derive from `_golden0729_net50_30ep_wl1off.json`; keep
`eigen_threads` / `parallel_max_threads` at the baseline's **8** so the spectrum comparison
stays single-treatment.
