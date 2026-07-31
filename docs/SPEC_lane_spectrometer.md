# SPEC — Lane-ladder spectrometer + MW-integer fold (network defect-localization campaign, `sd_ewl` S1–S3)

Handoff. Read `docs/SPEC_sd_ewl_prereduce.md` first (increment 1 — BUILT, float layer
falsified with data, stays OFF), then `docs/SPEC_sd_ewl_integer_hold.md` (increment 2 —
UNBUILT). This spec (a) re-purposes the MW/EWL integer machinery as a **diagnostic
campaign** for the network's non-integer-DD defect, (b) puts a **read-only instrument
BEFORE any fold**, and (c) **amends** the integer-hold spec in four places (§6 —
authoritative over the older text where they conflict). The production zero-q hold
(increment 2c) is **NOT part of this campaign** and stays deferred.

Status: **BUILT 2026-07-29** — S1 instrument, the anchor trace, and increments 2a+2b all
landed default-OFF; `gnss_tests` green at 948 cases / 150 suites. Implementation record,
deviations and the measured findings are in **§12** (read it before running anything: two
of the spec's own premises did not survive measurement). S3 remains unbuilt and 2c stays
deferred.

---

## 1. The question this campaign answers

Symptom (measured, closed trail — `grade_ddfrac.py` era with Cards 1d+1e live, i.e. an
HONEST instrument, σ 0.119 cyc): DD float fractions are statistically **UNIFORM** at
3.3 h (mean|f| 0.245, circR 0.052 vs uniform 0.25/0) and **no NLs are fixable at 3 h**
on three independent substrates. Real model bias remains. Open suspect classes, in
current order: (1) iono representation, (2) eq34 / per-(sat,signal) anchor datum,
(3) refloat churn.

Trusted reference: the **between-station DD of the MW statistic**. Receiver AND
satellite MW biases cancel exactly (same satellite pair on both sides), leaving a pure
integer × λ. Measured on the EWL band pairs: DD med|f| 0.019–0.029 cyc, circR
0.96–0.98, **unmoved through the DOY-129 detonation**. This is ground truth the
estimator never touched.

Strategy — two uses of that truth, strictly ordered:
- **S1 (spectrometer, read-only):** project the network's own float DDs onto the lane
  ladder and compare against the MW integers. The rungs' different error sensitivities
  make this a **spectrometer for the defect's magnitude and type**. No fold, no
  estimation risk, mostly no C++.
- **S2/S3 (forcing function):** fold the fixed integers and watch where the tension
  goes — first as a single-epoch **virtual fold** (S2), then, only if S2 is ambiguous,
  a short live fold in diagnostic trim (S3).

## 2. Why compare-before-fold (the WL-1 lesson — do not skip S1)

The soft version of "fold it in and see" was run (WL-1 float layer, 50 sta A/B +
`debug_r_scale` sweep): the tension did NOT settle into a legible residual — it exited
through the QC machinery (D5 flags 3.48×, refloats 205/ep, ~55σ innovations into a
zero-q carry, convergence sign flipped), and the post-mortem explicitly could not
separate mechanisms. In a system with slip re-taps, RAIM refloats, and adaptive gates,
injected tension becomes **churn**, and churn attributes worse than the original
symptom. A read-only projection gives attribution first; the fold then confirms.

## 3. Increment S1 — the lane-ladder spectrometer (scripts only)

### 3.1 Physics: the sensitivity ladder

Three error classes reach a float ambiguity estimate N̂_j (cycles):
**dispersive** (iono; enters band j as f₁²·δI₁/(c·f_j)), **non-dispersive metre-level**
(geometry/tropo/clock/orbit leakage; enters as δρ/λ_j), and **per-signal
cycle-structured** (phase-bias / datum realization; arbitrary per signal, NO ladder
structure). The lane combinations compress the first two by fixed, known ratios:

| combination | λ (m) | ∂frac/∂(DD iono I₁) cyc/m | ∂frac/∂(range-like) cyc/m | frac ≈ uniform when \|err\| ≳ |
|---|---|---|---|---|
| L1 per-band | 0.190 | 5.26 | 5.26 | ~0.08 m iono |
| L2 per-band | 0.244 | 6.74 | 4.10 | ~0.06 m |
| L5/E5a per-band | 0.255 | 7.04 | 3.92 | ~0.06 m |
| G WL(1,2) | 0.862 | 1.49 | 1.16 | ~0.27 m |
| E WL(1,5) | 0.751 | 1.78 | 1.33 | ~0.22 m |
| G EWL(2,5) | 5.861 | 0.293 | 0.171 | ~1.4 m |
| E EWL(7,5) | 9.768 | 0.179 | 0.102 | ~2.2 m |

How to read it:
- The rung where state-side fractions transition **uniform → concentrated** brackets
  the dispersive/metre-level error magnitude between that rung's threshold and the
  next one's.
- The **state-vs-MW EWL disagreement is a per-arc error gauge**: implied dispersive
  DD error [m] ≈ frac_EWL/0.293 (G), unambiguous to ±λ/2 ≈ ±1.7 m equivalent.
  The WL rung cross-checks at ~5× the sensitivity.
- **Uniform at EVERY rung including EWL** — while the measurement-side MW DD is
  clean — convicts the **cycle-structured per-signal class** (datum/anchor/bias
  separation): no plausible dispersive magnitude uniformizes the EWL rung without
  being grotesquely visible elsewhere.
- Honest limit: the ladder spans WL **directions** only; the NL direction (where the
  user-visible symptom lives) is orthogonal. Localization here is by magnitude
  bracketing + class elimination, not by probing NL directly.

### 3.2 Inputs (all existing; one optional trace addition)

- **State side:** `logs/network_dd_fractional.csv` (knob `network.trace_dd_fractional`;
  header at `src/core/logging/NetworkRunTraces.cpp:204`):
  `week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,frac_cyc,dd_sigma_cyc`.
  Cost controls are LOAD-BEARING: arm `trace_dd_fractional_max_pairs` (unlimited made a
  305-sta epoch take ~45 min) and `trace_dd_fractional_epoch_stride` (2 is fine —
  fraction statistics don't need every epoch).
- **Measurement side:** raw RINEX via the parser in `scripts/mw_census_allpairs.py`
  (per-(station, sat, signal-pair) MW series; star-basis DD machinery already there).
- **Segmentation:** series-intrinsic — split an arc window at any adjacent-epoch
  |Δdd_cyc| > 0.5 (refloat/re-level jumps split the window). `network_slip_raw.csv` /
  events traces are optional corroboration, not required inputs.

### 3.3 The grader — NEW `scripts/lane_spectrometer.py`

1. **Re-reference within signal (THE TRAP OF THIS SPEC).** `ref_sat` is
   per-(constellation, **signal**) and may DIFFER between signals. Never join `dd_cyc`
   across signals directly. Within each (epoch, cons, signal, sta_a, sta_b), form
   re-referenced DDs between arbitrary satellite pairs — `dd(sat_i) − dd(sat_j)`
   (ref_sat cancels only within one signal at one epoch) — THEN combine across
   signals on common (sat_i, sat_j, sta_a, sta_b, epoch) keys:
   `N_lane = ddᵣ(sigA) − ddᵣ(sigB)` [cycles].
2. **Arc-mean grading (the R6 method).** DD point estimates wander 1.5–4 cyc/ep;
   pooled instantaneous fractions measure churn, not bias. Grade **arc means** over
   segmented windows of ≥10 epochs; report the pooled numbers alongside for
   contrast, and the window-length distribution as evidence of gradeability.
3. **Measurement side.** Same channels, same windows: DD-of-star MW per sat-pair /
   sta-pair. EWL DDs round instantly; WL(1,2) needs n≥10 and carries a measured
   2–4 % systematic wrong-by-one floor (plateaus ~0.96, NOT √n; invisible to
   variance gates) — **population-level conclusions only** at the WL rung, never
   per-arc.
4. **Statistics per (rung × constellation × channel-family × partition):** med|f|,
   P(<0.1), circR against the uniform benchmarks (0.25 / 0.20 / ~0 — never report
   mean|frac| alone), decile table, and the state-vs-MW EWL disagreement histogram
   with its implied-error axis in metres. Propagate an indicative lane σ as
   √(σ_A²+σ_B²) from `dd_sigma_cyc` (cross-signal covariance is not in the trace —
   label it indicative).
5. **Partitions:**
   - **Anchor partition.** Only meaningful when the graded run used the prioritized
     per-(sat,signal) anchor election (check the run's config; with the legacy
     single-pivot datum the partition degenerates and exonerates that mechanism by
     construction). Split DD sat-pairs into same-anchor vs cross-anchor; the known
     blocker predicts cross-anchor pairs non-integer. Anchor identity source:
     the `networkIsGaugePinned_` logic (`src/core/filter/NetworkDdFrac.cpp:97`); if
     the anchor STATION is not derivable from existing traces, add a one-shot
     `network_phase_anchor.csv` (week,tow,cons,signal,sat,anchor_sta) behind a new
     trace-only knob `network.trace_phase_anchor` (default OFF ⇒ byte-identical).
   - **Baseline buckets** (<400 km / 400–1500 / >1500): free cross-check; per-band
     fractions are already measured baseline-independent (corr +0.009) — a lane-rung
     departure from that would itself be news.
   - **Convention screens.** GPS lanes involving L2W/L2S: grade mod 1.0 AND mod 0.5
     (semi-codeless half-cycle artifact is real: stable L2S arcs cluster at ~0.34 cyc,
     circR 0.40) — report separately, never average in. Galileo X-attribute families:
     report per family (L6X/L7X med|f| 0.131 off-integer is a known alignment offset,
     not a defect signal).

### 3.4 The decision matrix (a run without a matrix row is not a verdict)

| state-side observation | verdict | consequence |
|---|---|---|
| per-band uniform; WL concentrated; EWL clean | dispersive error, 0.06–0.27 m window | **iono representation confirmed dominant.** Magnitude gauge = frac_WL/1.49 m. Iono-2's owed ≥3 h A/B is the right next purchase; this grader becomes its acceptance instrument. |
| per-band AND WL uniform; EWL concentrated | dispersive, 0.27–1.4 m (field-scale) | iono representation at SH-field scale; cross-check with the free-iono discriminator before building. |
| ALL rungs uniform incl. EWL (measurement MW clean) | cycle-structured per-signal defect | read the anchor partition: cross ≫ same ⇒ the anchor-election datum split (known open blocker). Else bias-separation / leveling class — escalate to the eq34/datum card. |
| anchor partition splits at any rung | datum mechanism contributes independently | fix/disarm the election before re-grading anything else. |
| state-vs-MW EWL offset ≈ per-arc constant ≫ noise | instrument first | suspect channel identity, half-cycle, or the ref_sat join bug before physics; then integer-datum lump accounting (§6 A2). |
| windows too short to grade (refloat-dominated) | churn-gated (R6's third state) | D5/refloat load remains the gate; window-length distribution is the evidence. Do not force a class verdict. |

### 3.5 Data: existing runs first, else one run

Step 0: inventory `runs/` and stored `logs/` for a ≥3 h, ≥50-sta lineage that has
`network_dd_fractional.csv` (Cards 1d+1e era or later). If none survives, run ONE:
50 sta GPS+Gal, 30 s, ≥3 h, Release, standard armed set, derived from the
`_golden0729_net50_30ep_wl1off.json` ancestry, with `trace_dd_fractional` +
`max_pairs` (~2000) + `epoch_stride 2`, `state_snapshot_path` armed at cadence (S2
input + crash insurance), `cold_clock_seed` on, half-epoch start-offset back-off.
Grade the final 60 min only. 10-ep smoke first (staged cadence). Renamed exe under
concurrent sessions. Never Debug.

## 4. Increment S2 — the virtual fold (single-epoch paired resume A/B)

Prereq: increments **2a + 2b** of `SPEC_sd_ewl_integer_hold.md` as amended by §6.
The hold (2c) is NOT required — a virtual fold folds once.

1. From the S1 run's ~t+90 min snapshot, resume TWO one-epoch legs, same build,
   identical config except `sd_ewl.fix_enable`.
2. **Validity first:** the OFF leg must be byte-identical to a plain control resume
   (the A/B's own validity check, as WL-1's bit-identical epochs 1–10 were). `sd_ewl`
   adds rows only — no carry membership, no fingerprint entry. Verify the knob is
   absent from the fingerprint block with the two-tool rule before trusting the pair.
3. Read **Δx per state class** (sat clocks / PB / CB / SH coefficients / per-LOS dev /
   ZWD / local ambiguities): the between-leg state delta IS the single-epoch
   constrained-update redistribution — exact, per constraint, with no dynamics and no
   QC feedback. Prediction from the row algebra
   (`ApplicationNetworkAssembly.cpp:509`): PB columns move first — per-satellite
   leverage +24.00/−23.00 (G), +39.33/−38.33 (E) vs code ±0.51/0.49.
4. Read postfit redistribution (`trace_postfit_keyed`, σ̂0) and the product delta on
   SD/DD-graded quantities only (invariant #5).

Deliverable: a table of which state families absorbed the WL-direction tension, at
what magnitude, vs S1's bracketing. If S1 and S2 agree on a class, the campaign
verdict is closed — S3 is optional confirmation, not a requirement.

## 5. Increment S3 — live fold, diagnostic trim (only if S2 is ambiguous)

- **Per-epoch re-decision** (Route B every epoch), hold OFF — no 2c machinery.
  Decision flips are themselves diagnostic: counter `dd_flip`.
- QC stays armed (RAIM directive) but fully counted: ON/OFF legs × 30–60 ep from the
  same snapshot (the warm-resume seam is common-mode across the pair, so differential
  reads survive it). Read D5/refloat/RAIM-adapt deltas, SH field motion
  (`sh_dev`/`vtec` traces), clock spread, and **re-run the S1 grader on both legs** —
  the spectrometer is the fold's grader (does folding the WL direction concentrate
  the per-band fractions?).
- Budgeted failure mode: if the ON leg's D5 churn explodes (the WL-1 signature),
  STOP and fall back to S2's exact answer — that outcome convicts tension↔QC
  coupling, not a defect class.
- σ honesty: `sigma_floor_m` stays **1.0 m** (per-sample scale) for fixed rows — §6 A1.

## 6. AMENDMENTS to `SPEC_sd_ewl_integer_hold.md` (authoritative)

**A1 — fold-cadence noise model.** `MwSlipDetector::arcZdMean` is arc-CUMULATIVE:
consecutive-epoch rows share n−1 of n samples (ρ ≈ 1 − 1/2n). Refolding
`var_of_mean` as white every epoch over-claims information ~√k, and the over-claim
grows exactly as fixed-row residuals drop to code-noise scale (the convicted
σ-overconfidence pattern: SH pseudo-obs refolded white; D2/local-carry double
marginal). For ANY repeated fold (S3 and a future 2c): keep `sigma_floor_m` at
**per-SAMPLE scale (1.0 m — the increment-1 ladder's measured honest point)**;
folding the current mean at per-sample weight accretes ≈1 sample of information per
epoch, asymptotically honest with zero detector change. Production v2: expose the
per-epoch MW sample in `MwArcZdMean` (same bookkeeping class as
`exists`/`code1`/`code2`) and fold the sample — the zero-q hold then lets the filter
do the averaging optimally.

**A2 — the DD→SD integer datum (2a→2b construction, previously unstated).** Route B
yields DD integers. Per connected component (measured: one per constellation), elect
a reference station r0 deterministically (most resolved arcs, tie-break ascending
name), define N_SD(r0) ≡ 0, and propagate spanning-tree DD fixes to every station's
SD integer; count `dd_flip` on any re-decision that changes a propagated value. The
bias combination the network then observes is the true B^s−B^p **plus
λ·N_SD_true(r0)** — a per-(sat-pair, component) constant re-datum, i.e. the classical
WL-FCB modulo-λ definition. Harmless in-run and to any consumer integer decision;
**production arming must route fix-onset through the per-λ disc contract (PB gate
v2)** or the consumer wire sees a PB step.

**A3 — grading additions.** (i) Consumer replay (~33 s/arm) as a secondary gate —
the constrained B_EWL combinations are exactly what the consumer WL cascade
consumes; its WL fix rate is the truest "convergence" read for a PPP-RTK service.
(ii) A cross-station **fixed-row residual watchdog**: per (sat-pair), each station's
fixed-row residual vs the multi-station bias estimate. Majority redundancy convicts
the constant offsets that per-arc Welford σ structurally cannot. Calibrate its ROC on
EWL rows (truth near-certain) — it is REQUIRED before any WL(1,2) hold is ever
considered (2–4 % systematic wrong-by-one floor; half-cycle hazards).

**A4 — scope.** This campaign uses the EWL rungs only: G(2,5), E(7,5). WL(1,2) is
decision/population-diagnostic only. The hold (2c) stays deferred until the campaign
verdict; if the verdict is iono-representation, 2c competes with Iono-2 for the next
build slot on S2/S3's measured payoff, not on promise.

## 7. Where the code goes

| concern | location |
|---|---|
| S1 grader | NEW `scripts/lane_spectrometer.py` (imports the `mw_census_allpairs.py` parser; grading conventions from `grade_ddfrac.py`) |
| anchor one-shot trace (only if underivable) | emit in `src/core/filter/NetworkDdFrac.cpp`; stream in `logging/NetworkRunTraces` (`network_phase_anchor.csv`); knob `network.trace_phase_anchor` in `GnssConfigTypes.hpp` + `GnssConfigLoadNetwork.cpp` (trace-only, default OFF) |
| 2a decision seam (S2/S3 prereq) | per the integer-hold spec §6 table: NEW `filter/NetworkSdEwlAr.{hpp,cpp}`, manual `CMakeLists.txt` entry |
| held/propagated integers, `dd_flip`, per-component r0 | `app/NetworkRunContext` (`NetworkCarryState`/`NetworkServiceCounters`) — never function-locals |
| run-end readout | `src/core/filter/NetworkTeardown.cpp` |
| configs | `_spectro_*` derivatives of the `_golden0729_*` lineage (`config/test_suite/`) |

## 8. Counters (invariant #4 — or it didn't happen)

S1 script prints its own denominators: rows read, pairs re-referenced, joined,
dropped_ref_mismatch, dropped_screen (L2W / X-family), windows graded vs too-short.
C++ side inherits the integer-hold §7 counter set plus: `dd_flip`,
`sd_ref_reelected`, `rows_fixed_folded`, and (if built) anchor-trace row count.
Every S2/S3 leg's run-end line must name the dominant skip cause when
`rows_fixed == 0`.

## 9. Gates

1. **OFF byte-identity** for every new C++ knob (trace knob, 2a/2b knobs), same-build
   only; clear `logs/` per arm (several service CSVs are APPEND-mode; `products.txt`
   differs in its `G` timestamp record).
2. `gnss_tests` green; new suites in NEW files (suite-per-file rule): ref_sat
   re-referencing algebra (exact cancellation), cross-signal join keying, DD→SD datum
   propagation + flip counting, A1 weight accounting.
3. S1 verdict stated as a row of §3.4's matrix, with the window-length distribution
   attached. No matrix row ⇒ no verdict ⇒ not done.
4. S2 validity = OFF-leg byte-identity, then the Δx-per-class table.
5. Convergence-speed claims remain non-gradeable on ≤30-ep fixtures (integer-hold
   gate 4 stands unchanged).

## 10. Traps already paid for — carry them

- **ref_sat is per-(const,signal)** — cross-signal joins without within-signal
  re-referencing are poison (this spec's own trap; unit-test it).
- Tracer cost: `max_pairs` armed always (unlimited measured ~45 min/epoch at 305 sta);
  stride 2.
- Pathological `x_lin` tail (p99 1349 m, max 1.85e6 m): keep `max_abs_z_m` armed;
  `cold_clock_seed` on in the lineage.
- `phaseBiasColumn`/`codeBiasColumn`/`clockColumn` THROW (`index_.at()`) — increment 1
  wraps them; keep the wraps (961 station-epochs died once).
- Byte-identical A/B ⇒ the feature never ran (Card-1e lesson) — check counters, not
  absence of crashes.
- Stale-Release trap: verify exe mtime > source mtime before trusting any run.
- WL(1,2): population statistics only — plateau 0.96, constant per-arc error,
  variance gates structurally blind.
- Concurrent sessions: LNK1104 ⇒ relink a renamed exe; pre-kill name-scoped WER
  zombies before each launch.
- Debug is never a data run (50–100× Eigen penalty); compile checks + `gnss_tests`
  only.

## 11. Run recipes

```
# S1 (only if no suitable stored run): 10-ep smoke, then the 3h+ leg
build/x64-Release/gnss_processor.exe config/test_suite/_spectro_smoke10.json
build/x64-Release/gnss_processor.exe config/test_suite/_spectro_50sta_3h.json
python scripts/lane_spectrometer.py runs/spectro_3h [tail_seconds=3600]

# S2: paired one-epoch resume from the ~t+90min snapshot
build/x64-Release/gnss_processor.exe config/test_suite/_spectro_vfold_off.json
build/x64-Release/gnss_processor.exe config/test_suite/_spectro_vfold_on.json
```

Keep `eigen_threads`/`parallel_max_threads` at the baseline's 8 so any spectrum
comparison stays single-treatment. Timestamps/dates in configs, never derived.

---

## 12. Implementation record — 2026-07-29

Everything below is measured or verified in-tree. Where it contradicts §1–§11, this
section is authoritative (§6 remains authoritative over the older integer-hold text).

### 12.1 What landed

| piece | where | state |
|---|---|---|
| S1 grader | `scripts/lane_spectrometer.py` (+ `--selftest`) | built; 20 self-checks green |
| MW parser reuse | `scripts/mw_census_allpairs.py` → new `parse_station_tow(path, sod_lo, sod_hi, n_epochs)`; census body moved under a `__main__` guard so importing costs nothing; `parse_station` delegates (CLI unchanged) | built |
| anchor trace | `network.trace_phase_anchor` → `logs/network_phase_anchor.csv`; emitted in `NetworkDdFrac.cpp` off the gauge-pin predicate, outside the DD-frac stride gate | built, default OFF |
| 2a integer decision | NEW `filter/NetworkSdEwlAr.{hpp,cpp}` + `CMakeLists` entry; called from the epoch loop inside the existing `SB_PREASM` bracket | built, default OFF |
| 2a pure core | `sdEwlBands`, `sdEwlRoundingFfr`, `sdEwlFlipCheck`, `sdEwlPropagate` — extracted so the datum algebra is unit-testable without a detector | built |
| 2b fixed fold | `Application::networkAppendSdEwlRows_` | built, default OFF |
| carried state / counters | `ctx.carry.sd_ewl_{fix,r0,prev}`, `ctx.counters.sdewl_*`, `NetStation::SdEwlCounters::{rows_fixed,rows_float,groups_fixed,fix_pair_mismatch,fix_lone_arc}` | built |
| run-end readout | `NetworkTeardown.cpp` — two lines (2a decision, 2b fold) + a dominant-cause line when `rows_fixed == 0` | built |
| traces | `network_sd_ewl_ar.csv` (new, armed only with `trace && fix_enable`); `network_sd_ewl.csv` gains `n_fix_cyc,fixed,grp_fixed` **only** when `fix_enable` — armed-only header, so an increment-1 artifact set is unchanged | built |
| tests | NEW `tests/test_sd_ewl_integer.cpp` — `SdEwl{Datum,Ffr,Flip,FixedRow,BandTable,FixConfig}Test`, 22 cases | green |
| configs | `_spectro_smoke10`, `_spectro_50sta_3h`, `_spectro_byteid_off`, and `scripts/spectro_make_vfold.py` for the S2 legs | built |

### 12.2 Premises that did NOT survive measurement

**(a) §3.5 step 0 — no stored run is gradeable.** Every stored lineage with
`network_dd_fractional.csv` over >= 3 h was measured: the median adjacent-epoch DD step is
**1.2-11.6 cycles per epoch** (`t7h_offset2h_6h` 1.22, `t7d_off_12h` 5.23,
`t7e_pbcbfloor_12h` 5.49, `t7g_lowfloor10_12h` 6.14, `t7g_fullfloor_12h` 7.39,
`t7b_joint_timers_12h` 7.13, `t7d_tu_12h` 8.71, `t7f_shmarg_12h` 9.07,
`t6_gg_freeiono_lowfloor_12h` 11.64) against the spec's own 0.5 cyc window-splitting
threshold, so 0.0-0.2 % of windows reach the 10-epoch minimum. Not a `ref_sat`-flip
artifact: the same step on the *re-referenced satellite-pair* series (reference-free by
construction) is 1.4-12.5 cyc, and each tail window carries exactly one `ref_sat` per
(cons, signal). => a fresh run is required, and **§3.4's last row (churn-gated) is a live
possibility for the fresh one too** — which is why the grader measures and reports the
churn ladder *before* it reports any class verdict.

**(b) §3.3/§3.5 anchor partition — the anchor map is ONE-TO-MANY.** Under the graded
lineage's posture (`phase_datum_eq34` + `phase_datum_prioritized`) the election that runs
is `networkElectChainDatum`, which pins **spanning-tree edges**: `pinned_amb_targets`
receives one entry per tree edge at its receiver endpoint, so a satellite is pinned at
every station incident to it in the tree. Measured on the 10-epoch smoke: **up to 10
stations per (cons, signal, satellite)**. So "the anchor station" is not well defined and
the spec's binary same/cross split needs generalizing. The grader uses **pin-set
intersection** (same-anchor iff the two satellites' pin sets share a station), which asks
the same question and degrades correctly — on the legacy single-pivot datum every set is
`{pivot}`, every pair is same-anchor, and the partition exonerates the mechanism by
construction exactly as §3.5 says. The trace emits the whole pin set, one row per pin.

### 12.3 Amendments this build makes to §6

**A1 is implemented as TWO floors, because it is two different quantities.**
`sd_ewl.fix_sigma_floor_m` (default **1.0 m**, the measured per-sample honest point) is
used as (i) a variance-**of-the-mean** floor `sigma^2/n` for the FFR **decision** — the
served `var_of_mean_m2` shrinks as 1/n and measures 13-60x optimistic against the rows'
own residual spread, and an optimistic sigma inflates FFR into buying wrong fixes; and
(ii) a **flat per-sample** floor for the fixed **row**, which is A1's actual prescription
(refolding a variance-of-mean as white every epoch over-claims information as sqrt(k)).
Conflating them would either reject every mature arc or over-weight every fixed row.

**A2 gains a frame-extension rule the spec did not state.** `N_SD(r0) == 0` plus
spanning-tree propagation only ever relays satellites the *parent already holds*, so a
satellite invisible at r0 could never receive an integer — at a 50-station global roster
r0 sees roughly a third of a constellation, so that would forfeit two thirds of the arcs.
A satellite is therefore framed (value 0, counted as a datum, ungated) at the **first
station that reaches it**. This is legitimate and preserves A2 exactly: the frame constant
is per *satellite*, so wherever it is defined the residual offset in any SD difference
stays one constant per (satellite pair), identical at every station reading it.
`SdEwlDatumTest.PropagationIsTransitiveAlongAChain` is the test that found the gap.

**A NEW exactness precondition, not in §6: CHANNEL IDENTITY.** `B_EWL^s` is a per-signal
combination of the satellite phase and code biases, so Route B's `B^s - B^p` cancellation
holds only if **both** stations' MW arcs sit on the same two tracking channels. They need
not: increment 1 measured 41 % of resolved arcs with two candidate pairs, with both
`L7X/L5X` (1242) and `L7Q/L5Q` (1034) winning at different satellites. 2a therefore elects
a **network-common channel pair** per (constellation, satellite) by station consensus
(tie-break ascending pair name) and excludes stations on any other pair. Measured cost on
the smoke: **3222 (station, satellite) arcs excluded vs 2727 admitted** — more than half.
That is the price of exactness, and it means the spec's headline co-visibility figure
("67 distinct GPS SD pairs, 80 Galileo, one connected component each") is optimistic
under channel matching. The row builder tries the decision's pair first and counts
`fix_pair_mismatch` if it settles elsewhere (measured 0).

**Fixed and float rows coexist per row but in SEPARATE whitened groups.** §4 of the
integer-hold spec asks for per-row coexistence; a single group cannot deliver it.
`R = diag + var_p*11^T` is whitened by a dense `L^-1`, which would re-populate a fixed
row's deliberately zeroed local columns out of its float siblings, and the pivot arc's
ambiguity columns appear in *every* row of a group — so one mixed group would
simultaneously remove and estimate the pivot ambiguity. Two groups, each with its own
pivot and its own common-mode term, keeps both halves exact and makes "a fixed row
carries no local ambiguity coefficients" true of the whitened block, not just of `H`.
A constellation with exactly one fixed arc has nothing to difference it against; it is
demoted to the float group and counted (`fix_lone_arc`).

**Invariant #1 has a wired response even without 2c.** 2c is not built and no decision is
ever reused — the integers are re-derived every epoch. That does *not* by itself escape
invariant #1: a fixed row writes a lambda-quantized value (5.861 / 9.768 m) into satellite
PB/CB columns running at `q = 1e-12`/`1e-10`, whose measured response to a step is
tau ~ 3.8 h with the discontinuity counter silent throughout. So an integer that CHANGES
between epochs injects exactly that failure. The response, in this same increment:
`sdEwlFlipCheck` classifies each arc against the previous epoch — `Fresh` / `Stable` /
`Restart` (the detector's lagged sample count FELL: slip, gap or re-acquisition, so the
true integer legitimately changed; memory replaced, no flip charged) / `Flip` (a different
integer on a continuous arc: counted as `dd_flip` and the fix **withheld for that arc
that epoch**, because on a flip epoch neither value is known to be right). `Fresh` folds,
which is what keeps the S2 single-epoch fold from being byte-identical. The r0 reference
is kept **sticky** and its re-election counted separately (`sd_ref_reelected`): only r0
sets the SD frame — the spanning tree and each station's link satellite cancel in the
between-satellite difference — so tree churn needs no defending and a reference change
does.

### 12.4 Deviation from §4: the S2 legs are ~6 epochs, not 1

The MW arc accumulator is **not** in the state snapshot, so a resumed leg starts with zero
MW samples and needs `min_arc_epochs` lagged samples before any row can build. A literal
one-epoch leg builds ZERO rows and the pair comes out byte-identical — the Card-1e trap.
`scripts/spectro_make_vfold.py` therefore emits legs of ~6 epochs at
`min_arc_epochs = 2`, and the graded epoch is the FIRST with `rows_fixed > 0`. That is
still an exact single-epoch read: every earlier epoch is bit-identical across the legs
(no rows exist yet), so the legs enter the graded epoch from the same state and the delta
is the constrained update alone — and those identical leading epochs are the pair's own
validity check. §4.2's literal "the OFF leg must be byte-identical to a plain control
resume" cannot hold for a leg with the float layer armed (measured +0.51 % ev1), so the
generator emits **four** legs: `ctl` (sd_ewl off) -> `off` (float) isolates the float
layer, `off` -> `on` isolates the integer, and `inert` (fix armed with an unreachable FFR
gate) must be byte-identical to `off` — the only test that the new branch does nothing
when it does not fire.

### 12.5 Gate 9.2 coverage, honestly

`gnss_tests` covers the DD->SD datum propagation, the flip classification, the A1 weight
accounting, the fixed-row design/frame, the band table and the config contract (22 cases,
new file, new suite names). The first two items of gate 9.2 — **ref_sat re-referencing
algebra and cross-signal join keying** — live only in the Python grader; there is no C++
analogue to test. They are covered by `lane_spectrometer.py --selftest`, which proves the
exact cancellation, the virtual-reference member, the join across *differing* references,
**and that the naive join is wrong** (so nobody simplifies the re-referencing back out).
That is a real test, but it is not in `gnss_tests` — recorded here rather than implied.

---

## 13. S1 VERDICT — measured 2026-07-29, `runs/spectro_3h`

50 stations GPS+Galileo, 30 s, cold start 00:00 GPS DOY-129, 3.5 h, standard armed set,
`mw_wl` off, `sd_ewl` off (the producer graded AS IT IS). Graded window **02:30–03:30**
(the final 60 min, deliberately short of the 04:00 onset). Full output:
`runs/spectro_3h/spectrometer.txt`; per-window dump `runs/spectro_3h/lane_windows.csv`.
18 082 DD rows in the tail → 30 376 re-referenced pairs → 1250 lane channels → **856
graded arc-mean windows** (2.5 % of 34 626 candidate windows).

### 13.1 The instrument validates itself

The measurement side reproduces this document's own §1 ground truth **independently**,
from raw RINEX, with no estimator involved: MW DD fractional quality scales monotonically
with λ and lands exactly on the quoted numbers at the EWL rung —

| rung | λ (m) | MW DD med\|f\| | circR |
|---|---|---|---|
| E WL(1,5) | 0.751 | 0.222 | 0.083 |
| E WL(6,5) | 2.931 | 0.101 | 0.634 |
| E EWL(6,8) | 3.448 | 0.075 | 0.749 |
| E EWL(6,7) | 4.186 | 0.071 | 0.785 |
| **E EWL(7,5)** | **9.768** | **0.022** | **0.970** |

§1 states 0.019–0.029 cyc / circR 0.96–0.98 on the EWL band pairs. Confirmed. The WL rung
being uniform in the MW column is not a defect: a per-epoch WL MW is noise-dominated,
which is exactly why §3.3.3 restricts the WL rung to population statistics.

### 13.2 The ladder found its transition

Per-rung verdict on the ARC MEANS, ordered by λ:

| rung | λ (m) | verdict | med\|f\| | P(<0.1) | circR |
|---|---|---|---|---|---|
| E L1 / G L1 | 0.190 | churn-gated | — | — | — |
| G WL(1,2) | 0.862 | churn-gated | — | — | — |
| E WL(1,5) / (1,8) / (1,7) | 0.75–0.81 | partial | 0.21–0.23 | ~0.26 | 0.19–0.21 |
| E WL(1,6) | 1.011 | UNIFORM | 0.29 | 0.24 | 0.14 |
| E WL(6,5) | 2.931 | UNIFORM | 0.25 | 0.28 | 0.15 |
| E EWL(6,8) | 3.448 | partial | 0.20 | 0.25 | 0.19 |
| E EWL(6,7) | 4.186 | partial | 0.18 | 0.29 | 0.25 |
| **E EWL(7,5)** | **9.768** | **CONCENTRATED** | **0.136** | **0.401** | **0.423** |
| E EWL(7,8) | 19.537 | CONCENTRATED | 0.073 | 0.62 | 0.65 |
| E EWL(8,5) | 19.537 | CONCENTRATED | 0.084 | 0.59 | 0.61 |

The uniform → concentrated transition sits between **λ 4.19 m (partial)** and **λ 9.77 m
(CONCENTRATED)**. Reading the §3.1 table's uniformizing column at those two rungs brackets
the dispersive error magnitude at **~1.0–2.2 m** (range-like equivalent 1.7–3.9 m).

Two checks that the transition is physics and not an artifact: on E EWL(7,5) the `Q/Q` and
`X/X` channel families give **identical** med|f| 0.136 (42 and 95 arcs), so it is not a
tracking-attribute effect; and the state-vs-MW whole-cycle offset is **zero for 81.6 % of
EWL(7,5) arcs** (median 0, p90 1 cyc) while the WL rungs carry whole-cycle offsets on
70–100 % of arcs. The EWL rung is the one where the network's own float DD is on the right
lattice member, exactly as the ladder predicts.

### 13.3 The verdict row: ROW 2 **and** ROW 4

**ROW 2** — per-band and WL rungs uniform-or-ungradeable while the EWL rungs are
concentrated ⇒ *dispersive error at SH-field scale*, here bracketed at 1.0–2.2 m rather
than §3.4's 0.27–1.4 m guess. §3.4's consequence stands: cross-check with the free-iono
discriminator before building anything.

**ROW 4 also fires, and this is the new information.** The anchor partition splits
consistently, in the direction the open blocker predicts, on all three EWL rungs:

| rung | same-anchor med\|f\| (circR) | cross-anchor med\|f\| (circR) | n same / cross |
|---|---|---|---|
| E EWL(7,5) | **0.095** (0.513) | **0.155** (0.351) | 61 / 76 |
| E EWL(7,8) | 0.058 (0.708) | 0.084 (0.611) | 62 / 79 |
| E EWL(8,5) | 0.076 (0.685) | 0.095 (0.540) | 65 / 86 |

Cross-anchor pairs are 1.25–1.63× worse in med|f| and 1.16–1.46× worse in circR, on three
independent rungs with 60–90 arcs each. Under the eq34 chain datum "cross-anchor" means
the two satellites' spanning-tree pin SETS are disjoint (§12.2b). Per §3.4 row 4 the
consequence is explicit: **the datum mechanism contributes independently, and it should be
addressed before re-grading anything else.** Note the WL rungs show no consistent split
(cross-anchor is sometimes better) — consistent with those rungs being saturated.

### 13.4 What is NOT concluded

* **No per-band verdict exists.** L1/L2/L5 and G WL(1,2) are churn-gated: their median
  adjacent-epoch DD step is 1.5–3.6 cyc against the 0.5 cyc splitting threshold, so no
  window can form. §3.4's last row applies *to those rungs*, and the 2.5 % overall
  gradeable fraction is the evidence. The per-rung gate is why the EWL verdict survives:
  a single global churn gate would have thrown the whole run away.
* **The sub-cycle bias columns do not agree across rungs, and that IS the finding** —
  see §13.6, which scores both hypotheses against the correct null. Read the saturation
  flags with them: most WL rungs sit AT the uniform value, so their implied metres are a
  floor, not a measurement.
* **GPS EWL(2,5) is effectively unmeasured** — 6 step samples and 1 co-registered arc in
  the tail, because only the L5-capable GPS fleet qualifies. Every EWL statement above is
  Galileo. A GPS EWL verdict needs a longer lineage or a GPS-heavier roster.

### 13.5 Convergence: 30 min is NOT enough for this quantity

Same run, four disjoint 30-minute windows, same grader:

| window | G L1 DD churn (cyc/ep) | churn-as-one-error, best ratio | gradeable windows |
|---|---|---|---|
| 00:30–01:00 | 3.24 | 3.91× | 1.5 % |
| 01:00–01:30 | 2.04 | 2.60× | 1.7 % |
| 02:00–02:30 | 2.05 | 2.57× | 1.9 % |
| 03:00–03:30 | **1.52** | **1.82×** | **3.3 %** |

Position/clock convergence stabilizes far earlier, but the DD ambiguity churn — the
quantity this campaign grades — halves between the 30-minute mark and 3.5 h, the gradeable
fraction doubles, and the ladder only becomes explainable as a single error magnitude
(ratio < 2×) in the final window. So §3.5's "≥ 3 h" is justified by measurement, not
convention, and a 1 h S1 run would have produced a churn-gated non-verdict. Future S1
re-runs should keep ≥ 3 h; a 30-minute grading window inside it is sufficient.

### 13.6 The class test, scored against the right null — CORRECTED

An earlier pass judged the cross-rung metre columns against a fixed 2.0x threshold and
reported "fits neither ladder". That was wrong, and the correction matters because it
flips the churn verdict. A structureless per-signal error does not produce a "large"
ratio in some absolute sense — it produces a ratio equal to the rungs' **sensitivity
ratio**, because the implied metres are just the fraction over a sensitivity that varies
by that factor. Over the rungs here that null is 39-103x. So the statistic is how much of
the null a single-magnitude hypothesis explains,
`1 - log(observed)/log(null)`, not whether it clears a fixed bar. Judged that way,
`2.48x` across a `39x` null is 75 % agreement, not a failure.

**The epoch-to-epoch CHURN is ONE dispersive error of ~0.26-0.31 m.**

| pool | dispersive span | ratio | null | explains | range-like | explains |
|---|---|---|---|---|---|---|
| SPEC-table rungs (8) | 0.213-0.529 m | 2.48x | 39x | **75 %** | 0.286-0.872 m (3.05x) | 72 % |
| all well-sampled (18) | 0.212-0.529 m | 2.49x | 80x | **79 %** | 0.281-0.872 m (3.11x) | 76 % |

That is the answer to the obvious objection — *why would the network churn phase
ambiguities at the cycle level when a user filter snaps to stable ambiguities in 15-30
minutes?* It is not an artifact of immature or refloated arcs: conditioned on arc age,
the median re-referenced pair step is 2.37 cyc at ages 0-1 and still **1.81 cyc at ages
20-40** (n = 13 440), with 82 % of steps over 0.5 cyc. It is a real ~0.3 m per-epoch
DISPERSIVE motion, and it appears in the DD precisely because a between-station,
between-satellite DD cancels the per-station and per-satellite gauges but NOT anything
varying per (station, satellite) — the slant-iono residual above all. A user filter takes
its satellite biases fixed from a product and has no global SH field trading against 50
stations' ambiguities; this producer co-estimates both. On L1 that 0.3 m is 1.6 cyc; on
E EWL(7,5) it is 0.045 cyc. Same error, read through the ladder.

**The residual per-arc BIAS, after the churn is averaged out, is NOT dispersive.** The
state-vs-MW sub-cycle bias explains only 20 % of its null (10.9x of a 20x null on the
dispersive ladder, 14.6x of 26x on the range-like one) — no lane structure, i.e. the
per-signal cycle-structured signature. Read together with §13.3's anchor split, that is
the same conclusion from two independent statistics on the same arcs: **the churn is
iono representation, the residual bias is datum/bias separation.** Those are the two
suspect classes §1 listed, and this run separates them rather than choosing between them.

---

## 14. S2 VERDICT — the virtual fold, measured 2026-07-29

`runs/spectro_vf6/`, four legs resumed from the S1 snapshot (week 2313, tow 270060,
fingerprint 68a662a415533831), 7 epochs each, `min_arc_epochs = 2`, graded at the first
epoch with a folded fixed row (tow 270150). Output: `runs/spectro_vf6/vfold.txt`.

### 14.1 Validity — and the two code defects it caught

| check | result |
|---|---|
| `off` vs `inert` (fix armed, gate unreachable) | **24 artifacts byte-identical, 0 DIFFER.** The only differences are the two sd_ewl TRACE files, which are armed-only by construction (one gains three columns, the other exists only when armed). |
| datum identical between `off` and `on` at the graded epoch | **YES** — gauge-pin sets match, so the delta is a redistribution in a COMMON frame, not a frame change (standing invariant #5). |
| `ctl` vs `off`, `off` vs `on` | 15 artifacts differ each — the treatments are live, not inert. |

Getting that first row to pass took **two real fixes**, and the arm is the only reason
either was found:

1. **Datum-valued integers are ungated by construction.** The reference station's own
   satellites, each station's link satellite and a satellite framed on first sighting
   carry no rounding decision, so no FFR or |frac| setting can reject them. A group built
   only from those asserts a hard constraint with ZERO integer information while still
   removing a real local degree of freedom. Now `SdEwlFix::measured` marks them and a
   fixed group requires at least one MEASURED integer (`fix_datum_only` counts the
   demotions). A MIXED group is correct and wanted — the datum value is the reference the
   measurement is relative to.
2. **The integer must not be allowed to choose the channel pair.** The first version
   tried the decision's pair FIRST in the row builder's candidate search, on the correct
   reasoning that a fixed row must fold on the pair its integer was decided on. But that
   let the integer path change which pair the FLOAT path elects, and therefore change the
   solve even when nothing was ultimately fixed. The dependency now runs the other way:
   the row elects its pair exactly as increment 1 does and the fix is used only if it was
   decided on THAT pair (`fix_pair_mismatch`, measured 0 — 2a searches the same
   candidates in the same ascending order).

A third finding, about the arm itself: an FFR gate cannot express "impossible". At
`fix_min_ffr = 0.99999999` the arm still folded 6 fixed rows from 26 measured integers,
because a long arc's variance-of-the-mean makes its rounding arbitrarily confident. The
inert arm therefore uses `fix_max_frac_cyc = 1e-9`; the generator records why.

### 14.2 The Δx-per-class table — the row-algebra prediction CONFIRMED

De-medianed per-constellation |Δ| medians at the graded epoch (the datum-invariant
column; a whole-constellation common mode is a frame shift the product absorbs):

| state class | FLOAT layer (ctl→off) | **THE INTEGER (off→on)** |
|---|---|---|
| satellite PB | 0.0243 m | **0.0234 m** |
| satellite CB | 0.0102 m | **0.0014 m** |
| **PB / CB ratio** | **2.4x** | **16.7x** |
| satellite clock | 0.558 m | 0.241 m |
| SH-VTEC | 0.053 TECU | 0.018 TECU |
| ZWD | 1.22 m | 0.084 m |

The row algebra predicts the phase-bias columns move first: per-satellite PB leverage is
+24.00/−23.00 (GPS) and +39.33/−38.33 (Galileo) against ±0.51/0.49 on the code-bias
columns. Measured: the FLOAT layer spreads its effect almost evenly between PB and CB
(2.4x), while THE INTEGER concentrates it on PB by **16.7x** and moves CB **7x LESS than
the float layer does** (0.0014 vs 0.0102 m). That is exactly `SPEC_sd_ewl_integer_hold`
§2's mechanism, measured rather than argued: with `∇N_EWL` floating, the row's information
splits between a station-local nuisance with no prior and the shared bias, and the
nuisance absorbs nearly all of it; fixing the integer removes the local degree of freedom
instead of estimating it and hands the row weight to the bias.

Counters at the graded epoch: `on` folded 7 fixed rows of 306 built (45 measured FIXED
integers, 61 LINK, 13 R0, 175 NO_LINK, 0 FLIP); `off` folded 0 of 368. Post-fit σ̂0 over
stations: phase 0.085 (ctl) → 0.393 (float) → 0.323 (fixed) — the float layer degrades
phase σ̂0 4.6x and the integer fold recovers ~18 % of that degradation. Code σ̂0 29.10 →
27.90 → 29.77.

### 14.3 Campaign verdict

S1 and S2 agree on the class, which per §4 closes the campaign verdict and makes S3
optional confirmation rather than a requirement:

* the **churn** is iono representation — one ~0.3 m dispersive DD motion per epoch,
  75-79 % ladder-explained, unchanged by arc maturity;
* the **residual per-arc bias** is cycle-structured per-signal (20 % explained), and the
  anchor partition splits cross-vs-same on all three EWL rungs — the datum mechanism
  contributes independently (§3.4 row 4);
* the **integer fold works as designed**: it reaches the satellite phase biases 16.7x
  harder than the code biases and 7x more selectively than the float layer, in a common
  datum, with an inert arm that is byte-identical to float.

What that does NOT license: arming `sd_ewl.fix_enable` in production. Gate 3 of
`SPEC_sd_ewl_integer_hold` (the 50-sta 30-epoch ev1 field verdict) is still owed, the
A2 re-datum still has to route fix-onset through the per-λ disc contract before any
consumer sees it, and the campaign measured 7 fixed rows per epoch out of ~300 built —
a yield question the channel-consensus rule (over half the arcs excluded) now owns.
