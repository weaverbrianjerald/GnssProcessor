# SPEC — Lane-ladder spectrometer + MW-integer fold (network defect-localization campaign, `sd_ewl` S1–S3)

Handoff. Read `docs/SPEC_sd_ewl_prereduce.md` first — it is the whole `network.sd_ewl.*` card now (increment 1 float layer, falsified with data and OFF; increment 2a+2b integer path, built and OFF; increment 2c deferred). This spec (a) re-purposes the MW/EWL integer machinery as a **diagnostic campaign** for the network's non-integer-DD defect, (b) puts a **read-only instrument BEFORE any fold**, and (c) records the campaign verdicts (§13, §14). Status: **BUILT 2026-07-29** — S1 instrument, the anchor trace, and increments 2a+2b all landed default-OFF. S3 remains unbuilt and 2c stays deferred.

> ⚠️ **RELOCATION NOTICE (2026-08-15).** The sd_ewl-MECHANISM halves of this document — **§6 (amendments A1–A4)**, **§12.1** (build manifest), **§12.3** (build-time refinements), **§14.1/§14.2/§14.3** (the validity arm's defects, the Δx table, the licence limit) — have MOVED into `docs/SPEC_sd_ewl_prereduce.md`, the single authoritative home for the mechanism. They survive below as pointers plus their headline facts. Authoritative HERE: the **instrument** (§3, §4) and the **campaign verdicts** (§13, §14). Retired companions named below by their original titles now live under `docs/attic/` (`SPEC_sd_ewl_integer_hold.md`, `HANDOFF_mw_wl_card.md`, `REPORT_mwwl_conditioning_day129.md`, `HANDOFF_lane_carry_card.md`, `SPEC_lane_fold_perf_card.md`) — titles are quoted verbatim so a code-comment grep still lands, but the live path is `docs/attic/<name>`.
> **Provenance.** Every `runs/<name>` path below (`runs/spectro_3h`, `runs/spectro_3h/spectrometer.txt`, `runs/spectro_3h/lane_windows.csv`, `runs/spectro_vf6/`, `runs/spectro_vf6/vfold.txt`) resolves under `D:\GnssProcessor_archive\2026-08-12\runs\` (archived off C: 2026-08-12).

---

## 1. The question this campaign answers

Symptom (measured, closed trail — `grade_ddfrac.py` era with Cards 1d+1e live, i.e. an HONEST instrument, σ 0.119 cyc): DD float fractions are statistically **UNIFORM** at 3.3 h (mean|f| 0.245, circR 0.052 vs uniform 0.25/0) and **no NLs are fixable at 3 h** on three independent substrates. Real model bias remains. Open suspect classes, in current order: (1) iono representation, (2) eq34 / per-(sat,signal) anchor datum, (3) refloat churn.

Trusted reference: the **between-station DD of the MW statistic**. Receiver AND satellite MW biases cancel exactly (same satellite pair on both sides), leaving a pure integer × λ. Measured on the EWL band pairs: DD med|f| **0.019–0.029 cyc**, circR **0.96–0.98**, **unmoved through the DOY-129 detonation**. Ground truth the estimator never touched.

Strategy — two uses of that truth, strictly ordered: **S1 (spectrometer, read-only)** projects the network's own float DDs onto the lane ladder and compares against the MW integers; the rungs' different error sensitivities make this a **spectrometer for the defect's magnitude and type** (no fold, no estimation risk, mostly no C++). **S2/S3 (forcing function)** folds the fixed integers and watches where the tension goes — first as a single-epoch **virtual fold** (S2), then, only if S2 is ambiguous, a short live fold in diagnostic trim (S3).

## 2. Why compare-before-fold (the WL-1 lesson — do not skip S1)

The soft version of "fold it in and see" was run (WL-1 float layer, 50 sta A/B + `debug_r_scale` sweep) and the tension did NOT settle into a legible residual — it exited through the QC machinery (D5 flags 3.48×, refloats 205/ep, ~55σ innovations into a zero-q carry, convergence sign flipped), and the post-mortem explicitly could not separate mechanisms. **In a system with slip re-taps, RAIM refloats and adaptive gates, injected tension becomes CHURN, and churn attributes worse than the original symptom.** A read-only projection gives attribution first; the fold then confirms. *(Full record: `docs/SPEC_mw_wl_card.md` §14–§15.)*

## 3. Increment S1 — the lane-ladder spectrometer (scripts only)

### 3.1 Physics: the sensitivity ladder

Three error classes reach a float ambiguity estimate N̂_j (cycles): **dispersive** (iono; enters band j as f₁²·δI₁/(c·f_j)), **non-dispersive metre-level** (geometry/tropo/clock/orbit leakage; enters as δρ/λ_j), and **per-signal cycle-structured** (phase-bias / datum realization; arbitrary per signal, NO ladder structure). The lane combinations compress the first two by fixed, known ratios:

| combination | λ (m) | ∂frac/∂(DD iono I₁) cyc/m | ∂frac/∂(range-like) cyc/m | frac ≈ uniform when \|err\| ≳ |
|---|---|---|---|---|
| L1 per-band | 0.190 | 5.26 | 5.26 | ~0.08 m iono |
| L2 per-band | 0.244 | 6.74 | 4.10 | ~0.06 m |
| L5/E5a per-band | 0.255 | 7.04 | 3.92 | ~0.06 m |
| G WL(1,2) | 0.862 | 1.49 | 1.16 | ~0.27 m |
| E WL(1,5) | 0.751 | 1.78 | 1.33 | ~0.22 m |
| G EWL(2,5) | 5.861 | 0.293 | 0.171 | ~1.4 m |
| E EWL(7,5) | 9.768 | 0.179 | 0.102 | ~2.2 m |

How to read it: the rung where state-side fractions transition **uniform → concentrated** brackets the dispersive/metre-level error magnitude between that rung's threshold and the next one's. The **state-vs-MW EWL disagreement is a per-arc error gauge**: implied dispersive DD error [m] ≈ frac_EWL/0.293 (G), unambiguous to ±λ/2 ≈ ±1.7 m equivalent; the WL rung cross-checks at ~5× the sensitivity. **Uniform at EVERY rung including EWL** — while the measurement-side MW DD is clean — convicts the **cycle-structured per-signal class** (datum/anchor/bias separation): no plausible dispersive magnitude uniformizes the EWL rung without being grotesquely visible elsewhere. Honest limit: the ladder spans WL **directions** only; the NL direction (where the user-visible symptom lives) is orthogonal, so localization is by magnitude bracketing + class elimination, not by probing NL directly.

### 3.2 Inputs (all existing; one optional trace addition)

- **State side:** `logs/network_dd_fractional.csv` (knob `network.trace_dd_fractional`; header at `src/core/logging/NetworkRunTraces.cpp:230` — re-grepped 2026-08-15; the card's original `NetworkRunTraces.cpp:204` is stale): `week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,frac_cyc,dd_sigma_cyc`. Cost controls are LOAD-BEARING: arm `trace_dd_fractional_max_pairs` (unlimited made a 305-sta epoch take ~45 min) and `trace_dd_fractional_epoch_stride` (2 is fine).
- **Measurement side:** raw RINEX via the parser in `scripts/mw_census_allpairs.py` (per-(station, sat, signal-pair) MW series; star-basis DD machinery already there).
- **Segmentation:** series-intrinsic — split an arc window at any adjacent-epoch |Δdd_cyc| > 0.5. `network_slip_raw.csv` / events traces are optional corroboration, not required inputs.

### 3.3 The grader — `scripts/lane_spectrometer.py`

1. **Re-reference within signal (THE TRAP OF THIS SPEC).** `ref_sat` is per-(constellation, **signal**) and may DIFFER between signals. Never join `dd_cyc` across signals directly. Within each (epoch, cons, signal, sta_a, sta_b), form re-referenced DDs between arbitrary satellite pairs — `dd(sat_i) − dd(sat_j)` (ref_sat cancels only within one signal at one epoch) — THEN combine across signals on common (sat_i, sat_j, sta_a, sta_b, epoch) keys: `N_lane = ddᵣ(sigA) − ddᵣ(sigB)` [cycles].
2. **Arc-mean grading (the R6 method).** DD point estimates wander 1.5–4 cyc/ep; pooled instantaneous fractions measure churn, not bias. Grade **arc means** over segmented windows of ≥10 epochs; report the pooled numbers alongside for contrast, and the window-length distribution as evidence of gradeability.
3. **Measurement side.** Same channels, same windows: DD-of-star MW per sat-pair / sta-pair. EWL DDs round instantly; WL(1,2) needs n≥10 and carries a measured **2–4 % systematic wrong-by-one floor (plateaus ~0.96, NOT √n; invisible to variance gates)** — **population-level conclusions only** at the WL rung, never per-arc.
4. **Statistics per (rung × constellation × channel-family × partition):** med|f|, P(<0.1), circR against the uniform benchmarks (0.25 / 0.20 / ~0 — never report mean|frac| alone), decile table, and the state-vs-MW EWL disagreement histogram with its implied-error axis in metres. Propagate an indicative lane σ as √(σ_A²+σ_B²) from `dd_sigma_cyc` (cross-signal covariance is not in the trace — label it indicative).
5. **Partitions:**
   - **Anchor partition.** Only meaningful when the graded run used the prioritized per-(sat,signal) anchor election (with the legacy single-pivot datum the partition degenerates and exonerates that mechanism by construction). Split DD sat-pairs into same-anchor vs cross-anchor; the known blocker predicts cross-anchor pairs non-integer. Anchor identity source: the `networkIsGaugePinned_` logic (`src/core/filter/NetworkDdFrac.cpp:59` — re-grepped 2026-08-15; the card's original `NetworkDdFrac.cpp:97` is stale); the one-shot trace `network_phase_anchor.csv` (week,tow,cons,signal,sat,anchor_sta) is behind `network.trace_phase_anchor` (default OFF ⇒ byte-identical). ⚠ **The anchor map is ONE-TO-MANY — §12.2(b); the grader uses pin-set intersection, not a binary same/cross split.**
   - **Baseline buckets** (<400 km / 400–1500 / >1500): free cross-check; per-band fractions are already measured baseline-independent (corr +0.009) — a lane-rung departure from that would itself be news.
   - **Convention screens.** GPS lanes involving L2W/L2S: grade mod 1.0 AND mod 0.5 (the semi-codeless half-cycle artifact is real: stable L2S arcs cluster at ~0.34 cyc, circR 0.40) — report separately, never average in. Galileo X-attribute families: report per family (L6X/L7X med|f| 0.131 off-integer is a known alignment offset, not a defect signal).

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

Step 0: inventory `runs/` and stored `logs/` for a ≥3 h, ≥50-sta lineage that has `network_dd_fractional.csv` (Cards 1d+1e era or later). ⚠ **Measured: no stored run qualifies — §12.2(a).** So run ONE: 50 sta GPS+Gal, 30 s, ≥3 h, Release, standard armed set, derived from the `_golden0729_net50_30ep_wl1off.json` ancestry, with `trace_dd_fractional` + `max_pairs` (~2000) + `epoch_stride 2`, `state_snapshot_path` armed at cadence (S2 input + crash insurance), `cold_clock_seed` on, half-epoch start-offset back-off. Grade the final 60 min only. 10-ep smoke first (staged cadence). Renamed exe under concurrent sessions. Never Debug. **≥3 h is justified by measurement, not convention — §13.5.**

## 4. Increment S2 — the virtual fold (paired resume A/B)

Prereq: increments **2a + 2b** of the sd_ewl card as amended (now `docs/SPEC_sd_ewl_prereduce.md` §8–§10). The hold (2c) is NOT required — a virtual fold folds once.

1. From the S1 run's ~t+90 min snapshot, resume paired legs, same build, identical config except `sd_ewl.fix_enable`.
2. **Validity first:** the OFF leg must be byte-identical to a plain control resume. `sd_ewl` adds rows only — no carry membership, no fingerprint entry. Verify the knob is absent from the fingerprint block with the two-tool rule before trusting the pair. ⚠ **Refined by measurement into a FOUR-leg design — §12.4.**
3. Read **Δx per state class** (sat clocks / PB / CB / SH coefficients / per-LOS dev / ZWD / local ambiguities): the between-leg state delta IS the single-epoch constrained-update redistribution — exact, per constraint, with no dynamics and no QC feedback. Prediction from the row algebra (`ApplicationNetworkAssembly.cpp:625` — re-grepped 2026-08-15, the `pred_bias_m` coefficient site; the card's original `ApplicationNetworkAssembly.cpp:509` is stale): PB columns move first — per-satellite leverage +24.00/−23.00 (G), +39.33/−38.33 (E) vs code ±0.51/0.49.
4. Read postfit redistribution (`trace_postfit_keyed`, σ̂0) and the product delta on SD/DD-graded quantities only (invariant #5).

Deliverable: a table of which state families absorbed the WL-direction tension, at what magnitude, vs S1's bracketing. **If S1 and S2 agree on a class, the campaign verdict is closed — S3 is optional confirmation, not a requirement.** *(They did — §14.3.)*

## 5. Increment S3 — live fold, diagnostic trim — NOT BUILT, and now OPTIONAL

S1 and S2 agreed on the class (§14.3), so S3 became optional confirmation and was never built. The design constraints, if it is ever revived: per-epoch re-decision (Route B every epoch) with the hold OFF, decision flips counted as `dd_flip`; QC stays armed (RAIM directive) but fully counted, ON/OFF legs × 30–60 ep from the same snapshot (the warm-resume seam is common-mode across the pair, so differential reads survive it); **read-out list — D5/refloat/RAIM-adapt deltas, SH field motion (`sh_dev`/`vtec` traces), clock spread, and re-run the S1 grader on BOTH legs, because the spectrometer is the fold's own grader (does folding the WL direction concentrate the per-band fractions?)**; **budgeted failure mode — if the ON leg's D5 churn explodes (the WL-1 signature), STOP and fall back to S2's exact answer**, because that outcome convicts tension↔QC coupling, not a defect class; and `sigma_floor_m` stays **1.0 m** (per-sample scale) for fixed rows (§6 A1).

## 6. AMENDMENTS to `SPEC_sd_ewl_integer_hold.md` (authoritative) — ⚠ RELOCATED to `docs/SPEC_sd_ewl_prereduce.md` §10

**The full, authoritative A1–A4 text and its four build-time refinements now live in `docs/SPEC_sd_ewl_prereduce.md` §10.** They remain authoritative over any older integer-hold wording; only the address changed. (The integer-hold document itself is retired to `docs/attic/SPEC_sd_ewl_integer_hold.md`; the title above is quoted verbatim so code-comment greps still land.) Headlines, so a reader landing here from a code comment knows what they are:

- **A1 — fold-cadence noise model.** `arcZdMean` is arc-CUMULATIVE (consecutive rows share n−1 of n samples, ρ ≈ 1 − 1/2n), so refolding `var_of_mean` as white every epoch over-claims information ~√k. Keep `sigma_floor_m` at **PER-SAMPLE scale (1.0 m)**. → prereduce §10.1, and §10.5 for the **TWO floors** it is implemented as.
- **A2 — the DD→SD integer datum.** Per component elect r0 (most resolved arcs, tie-break ascending name), define N_SD(r0) ≡ 0, propagate spanning-tree DD fixes. The observed bias combination is `B^s−B^p` **plus λ·N_SD_true(r0)** — the classical WL-FCB modulo-λ re-datum. **Production arming MUST route fix-onset through the per-λ disc contract (PB gate v2).** → prereduce §10.2, plus §10.6's frame-extension rule.
- **A3 — grading additions.** Consumer replay (~33 s/arm) as a secondary gate; a cross-station fixed-row residual watchdog, ROC-calibrated on EWL rows, **REQUIRED before any WL(1,2) hold is ever considered**. → prereduce §10.3.
- **A4 — scope.** EWL rungs only: G(2,5), E(7,5). WL(1,2) is decision/population-diagnostic only. → prereduce §10.4.

Also relocated there: the **CHANNEL IDENTITY** exactness precondition (§10.7), the **separate whitened groups** rule (§10.8), and the **`sdEwlFlipCheck`** invariant-#1 response (§10.9).

## 7. Where the code goes

| concern | location |
|---|---|
| S1 grader | `scripts/lane_spectrometer.py` (imports the `mw_census_allpairs.py` parser; grading conventions from `grade_ddfrac.py`) |
| anchor one-shot trace | emitted in `src/core/filter/NetworkDdFrac.cpp`; stream in `logging/NetworkRunTraces` (`network_phase_anchor.csv`); knob `network.trace_phase_anchor` (trace-only, default OFF) |
| 2a decision seam (S2/S3 prereq) | `filter/NetworkSdEwlAr.{hpp,cpp}`, manual `CMakeLists.txt` entry — `docs/SPEC_sd_ewl_prereduce.md` §9.1 |
| held/propagated integers, `dd_flip`, per-component r0 | `app/NetworkRunContext` (`NetworkCarryState`/`NetworkServiceCounters`) — never function-locals |
| run-end readout | `src/core/filter/NetworkTeardown.cpp` |
| configs | `_spectro_*` derivatives of the `_golden0729_*` lineage (`config/test_suite/`) |

## 8. Counters (invariant #4 — or it didn't happen)

S1 script prints its own denominators: rows read, pairs re-referenced, joined, dropped_ref_mismatch, dropped_screen (L2W / X-family), windows graded vs too-short. C++ side inherits the sd_ewl counter set (`SPEC_sd_ewl_prereduce.md` §9.2) plus `dd_flip`, `sd_ref_reelected`, `rows_fixed_folded`, and the anchor-trace row count. Every S2/S3 leg's run-end line must name the dominant skip cause when `rows_fixed == 0`.

## 9. Gates

1. **OFF byte-identity** for every new C++ knob (trace knob, 2a/2b knobs), same-build only; clear `logs/` per arm (several service CSVs are APPEND-mode; `products.txt` differs in its `G` timestamp record).
2. `gnss_tests` green; new suites in NEW files (suite-per-file rule): ref_sat re-referencing algebra (exact cancellation), cross-signal join keying, DD→SD datum propagation + flip counting, A1 weight accounting. ⚠ **Coverage is honest but split — §12.5.** *(The "948 cases / 150 suites" figure this card originally quoted is a dated record of the suite at landing time; it is now 1360 cases / 196 suites.)*
3. S1 verdict stated as a row of §3.4's matrix, with the window-length distribution attached. **No matrix row ⇒ no verdict ⇒ not done.**
4. S2 validity = OFF-leg byte-identity, then the Δx-per-class table.
5. Convergence-speed claims remain non-gradeable on ≤30-ep fixtures (the sd_ewl card's gate 4 stands unchanged).

## 10. Traps already paid for — carry them

Two are OWNED here (this instrument's own); the rest are the sd_ewl card's and are recorded once in `docs/SPEC_sd_ewl_prereduce.md` §3 / §3b / §5b / §12 / §15 — do not re-state them, do not forget them.

- ⚠️ **OWNED: `ref_sat` is per-(const,signal)** — cross-signal joins without within-signal re-referencing are poison (unit-tested by `lane_spectrometer.py --selftest`, which also proves the naive join is WRONG so nobody simplifies the re-referencing back out).
- ⚠️ **OWNED: tracer cost** — `max_pairs` armed always (unlimited measured **~45 min/epoch at 305 sta**); stride 2.
- **WL(1,2): population statistics only** — plateau 0.96, constant per-arc error, variance gates structurally blind.
- **Byte-identical A/B ⇒ the feature never ran** (the Card-1e lesson) — check counters, not absence of crashes. This bit S2 literally: §12.4.
- Inherited (see the sd_ewl card): the `phaseBiasColumn`/`codeBiasColumn`/`clockColumn` THROW trap; the pathological `x_lin` tail and `max_abs_z_m` / `cold_clock_seed`; stale-Release mtime; LNK1104 renamed-exe + WER zombies; Debug is never a data run.

## 11. Run recipe

```
python scripts/lane_spectrometer.py <run_dir> [tail_seconds=3600]
python scripts/spectro_make_vfold.py …     # emits the four S2 legs (§12.4)
python scripts/spectro_grade_vfold.py …    # grades them
```

Producer arms are the `_spectro_*` configs: S1 `_spectro_smoke10`, `_spectro_50sta_3h`, `_spectro_byteid_off`; the four S2 legs `_spectro_vfold_ctl.json`, `_spectro_vfold_off.json`, `_spectro_vfold_on.json`, `_spectro_vfold_inert.json` (all four on disk under `config/test_suite/`; `spectro_make_vfold.py` regenerates them). Run Release-only per CLAUDE.md's Build section (`scripts/vs2026_build.ps1`). Keep `eigen_threads` / `parallel_max_threads` at the baseline's **8** so any spectrum comparison stays single-treatment. Timestamps/dates in configs, never derived.

---

## 12. Implementation record — 2026-07-29

Everything below is measured or verified in-tree. Where it contradicts §1–§11, this section is authoritative.

### 12.1 What landed

**The sd_ewl-mechanism rows of this manifest have MOVED to `docs/SPEC_sd_ewl_prereduce.md` §10.10** (the 2a seam and its pure core `sdEwlBands`/`sdEwlRoundingFfr`/`sdEwlFlipCheck`/`sdEwlPropagate`, the 2b fold, the carried state `ctx.carry.sd_ewl_{fix,r0,prev}` / `ctx.counters.sdewl_*` / `NetStation::SdEwlCounters`, the `NetworkTeardown` readout, the `network_sd_ewl_ar.csv` + armed-only `network_sd_ewl.csv` columns, and `tests/test_sd_ewl_integer.cpp`'s 22 cases). What landed on the INSTRUMENT side, which stays here:

| piece | where | state |
|---|---|---|
| S1 grader | `scripts/lane_spectrometer.py` (+ `--selftest`) | built; 20 self-checks green |
| MW parser reuse | `scripts/mw_census_allpairs.py` → new `parse_station_tow(path, sod_lo, sod_hi, n_epochs)`; census body moved under a `__main__` guard so importing costs nothing; `parse_station` delegates (CLI unchanged) | built |
| anchor trace | `network.trace_phase_anchor` → `logs/network_phase_anchor.csv`; emitted in `NetworkDdFrac.cpp` off the gauge-pin predicate, OUTSIDE the DD-frac stride gate | built, default OFF |
| S2 leg generator | `scripts/spectro_make_vfold.py` (+ `spectro_grade_vfold.py`) | built |
| configs | `_spectro_smoke10`, `_spectro_50sta_3h`, `_spectro_byteid_off`, `_spectro_vfold_{ctl,off,on,inert}` | built |

### 12.2 Premises that did NOT survive measurement

**(a) §3.5 step 0 — no stored run is gradeable.** Every stored lineage with `network_dd_fractional.csv` over ≥3 h was measured: the median adjacent-epoch DD step is **1.2–11.6 cycles per epoch** (`t7h_offset2h_6h` 1.22, `t7d_off_12h` 5.23, `t7e_pbcbfloor_12h` 5.49, `t7g_lowfloor10_12h` 6.14, `t7g_fullfloor_12h` 7.39, `t7b_joint_timers_12h` 7.13, `t7d_tu_12h` 8.71, `t7f_shmarg_12h` 9.07, `t6_gg_freeiono_lowfloor_12h` 11.64) against the spec's own 0.5 cyc window-splitting threshold, so **0.0–0.2 % of windows reach the 10-epoch minimum**. Not a `ref_sat`-flip artifact: the same step on the *re-referenced satellite-pair* series (reference-free by construction) is 1.4–12.5 cyc, and each tail window carries exactly one `ref_sat` per (cons, signal). ⇒ a fresh run is required, and **§3.4's last row (churn-gated) is a live possibility for the fresh one too** — which is why the grader measures and reports the churn ladder *before* any class verdict.

**(b) §3.3/§3.5 anchor partition — the anchor map is ONE-TO-MANY.** Under the graded lineage's posture (`phase_datum_eq34` + `phase_datum_prioritized`) the election that runs is `networkElectChainDatum`, which pins **spanning-tree edges**: `pinned_amb_targets` receives one entry per tree edge at its receiver endpoint, so a satellite is pinned at every station incident to it in the tree. Measured on the 10-epoch smoke: **up to 10 stations per (cons, signal, satellite)**. So "the anchor station" is not well defined and the spec's binary same/cross split needs generalizing. The grader uses **pin-set intersection** (same-anchor iff the two satellites' pin sets share a station), which asks the same question and degrades correctly — on the legacy single-pivot datum every set is `{pivot}`, every pair is same-anchor, and the partition exonerates the mechanism by construction exactly as §3.5 says. The trace emits the whole pin set, one row per pin.

### 12.3 Amendments this build makes to §6 — ⚠ RELOCATED

**Moved in full to `docs/SPEC_sd_ewl_prereduce.md` §10.5–§10.9.** The four refinements, named so a reader from a code comment can find them: **(i)** A1 is implemented as **TWO floors** — `sd_ewl.fix_sigma_floor_m` (1.0 m) as a variance-**of-the-mean** floor σ²/n for the FFR DECISION (the served `var_of_mean_m2` measures **13–60× optimistic**) AND as a flat per-sample floor for the fixed ROW (prereduce §10.5); **(ii)** A2's **frame-extension rule** — a satellite is framed at the FIRST station that reaches it, because r0 sees only about a third of a constellation at a 50-station global roster (§10.6); **(iii)** the NEW **CHANNEL IDENTITY** precondition — network-common channel pair by station consensus, measured cost **3222 arcs excluded vs 2727 admitted** (§10.7); **(iv)** fixed and float rows in **SEPARATE whitened groups** (§10.8). Plus the `sdEwlFlipCheck` invariant-#1 response (§10.9).

### 12.4 Deviation from §4: the S2 legs are ~6 epochs, not 1 — and there are FOUR of them

The MW arc accumulator is **not** in the state snapshot, so a resumed leg starts with zero MW samples and needs `min_arc_epochs` lagged samples before any row can build. **A literal one-epoch leg builds ZERO rows and the pair comes out byte-identical — the Card-1e trap.** `scripts/spectro_make_vfold.py` therefore emits legs of ~6 epochs at `min_arc_epochs = 2`, and the graded epoch is the FIRST with `rows_fixed > 0`. That is still an exact single-epoch read: every earlier epoch is bit-identical across the legs (no rows exist yet), so the legs enter the graded epoch from the same state and the delta is the constrained update alone — and those identical leading epochs are the pair's own validity check.

§4.2's literal "the OFF leg must be byte-identical to a plain control resume" cannot hold for a leg with the float layer armed (measured +0.51 % ev1), so the generator emits **four** legs: `ctl` (sd_ewl off) → `off` (float) **isolates the float layer**; `off` → `on` **isolates the integer**; `inert` (fix armed with an unreachable FFR gate) must be byte-identical to `off` — **the only test that the new branch does nothing when it does not fire.**

### 12.5 Gate 9.2 coverage, honestly

`gnss_tests` covers the DD→SD datum propagation, the flip classification, the A1 weight accounting, the fixed-row design/frame, the band table and the config contract (22 cases, new file, new suite names). The first two items of gate 9.2 — **ref_sat re-referencing algebra and cross-signal join keying** — live only in the Python grader; there is no C++ analogue to test. They are covered by `lane_spectrometer.py --selftest`, which proves the exact cancellation, the virtual-reference member, the join across *differing* references, **and that the naive join is wrong**. A real test, but **not in `gnss_tests`** — recorded here rather than implied.

---

## 13. S1 VERDICT — measured 2026-07-29, `runs/spectro_3h`

50 stations GPS+Galileo, 30 s, cold start 00:00 GPS DOY-129, 3.5 h, standard armed set, `mw_wl` off, `sd_ewl` off (the producer graded AS IT IS). Graded window **02:30–03:30** (the final 60 min, deliberately short of the 04:00 onset). Full output: `runs/spectro_3h/spectrometer.txt`; per-window dump `runs/spectro_3h/lane_windows.csv`. 18 082 DD rows in the tail → 30 376 re-referenced pairs → 1250 lane channels → **856 graded arc-mean windows** (2.5 % of 34 626 candidate windows).

### 13.1 The instrument validates itself

The measurement side reproduces §1's ground truth **independently**, from raw RINEX, with no estimator involved: MW DD fractional quality scales monotonically with λ and lands exactly on the quoted numbers at the EWL rung —

| rung | λ (m) | MW DD med\|f\| | circR |
|---|---|---|---|
| E WL(1,5) | 0.751 | 0.222 | 0.083 |
| E WL(6,5) | 2.931 | 0.101 | 0.634 |
| E EWL(6,8) | 3.448 | 0.075 | 0.749 |
| E EWL(6,7) | 4.186 | 0.071 | 0.785 |
| **E EWL(7,5)** | **9.768** | **0.022** | **0.970** |

§1 states 0.019–0.029 cyc / circR 0.96–0.98 on the EWL band pairs. Confirmed. The WL rung being uniform in the MW column is not a defect: a per-epoch WL MW is noise-dominated, which is exactly why §3.3.3 restricts the WL rung to population statistics.

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

The uniform → concentrated transition sits between **λ 4.19 m (partial)** and **λ 9.77 m (CONCENTRATED)**. Reading the §3.1 table's uniformizing column at those two rungs brackets the dispersive error magnitude at **~1.0–2.2 m** (range-like equivalent 1.7–3.9 m). Two checks that the transition is physics and not an artifact: on E EWL(7,5) the `Q/Q` and `X/X` channel families give **identical** med|f| 0.136 (42 and 95 arcs), so it is not a tracking-attribute effect; and the state-vs-MW whole-cycle offset is **zero for 81.6 % of EWL(7,5) arcs** (median 0, p90 1 cyc) while the WL rungs carry whole-cycle offsets on 70–100 % of arcs. The EWL rung is where the network's own float DD is on the right lattice member, exactly as the ladder predicts.

### 13.3 The verdict row: ROW 2 **and** ROW 4

**ROW 2** — per-band and WL rungs uniform-or-ungradeable while the EWL rungs are concentrated ⇒ *dispersive error at SH-field scale*, here bracketed at **1.0–2.2 m** rather than §3.4's 0.27–1.4 m guess. §3.4's consequence stands: cross-check with the free-iono discriminator before building anything.

**ROW 4 also fires, and this is the new information.** The anchor partition splits consistently, in the direction the open blocker predicts, on all three EWL rungs:

| rung | same-anchor med\|f\| (circR) | cross-anchor med\|f\| (circR) | n same / cross |
|---|---|---|---|
| E EWL(7,5) | **0.095** (0.513) | **0.155** (0.351) | 61 / 76 |
| E EWL(7,8) | 0.058 (0.708) | 0.084 (0.611) | 62 / 79 |
| E EWL(8,5) | 0.076 (0.685) | 0.095 (0.540) | 65 / 86 |

Cross-anchor pairs are **1.25–1.63× worse in med|f|** and 1.16–1.46× worse in circR, on three independent rungs with 60–90 arcs each. Under the eq34 chain datum "cross-anchor" means the two satellites' spanning-tree pin SETS are disjoint (§12.2b). Per §3.4 row 4 the consequence is explicit: **the datum mechanism contributes independently, and it should be addressed before re-grading anything else.** The WL rungs show no consistent split (cross-anchor is sometimes better) — consistent with those rungs being saturated.

### 13.4 What is NOT concluded

* **No per-band verdict exists.** L1/L2/L5 and G WL(1,2) are churn-gated: their median adjacent-epoch DD step is 1.5–3.6 cyc against the 0.5 cyc splitting threshold, so no window can form. §3.4's last row applies *to those rungs*, and the 2.5 % overall gradeable fraction is the evidence. **The per-rung gate is why the EWL verdict survives: a single global churn gate would have thrown the whole run away.**
* **The sub-cycle bias columns do not agree across rungs, and that IS the finding** — §13.6 scores both hypotheses against the correct null. Read the saturation flags with them: most WL rungs sit AT the uniform value, so their implied metres are a floor, not a measurement.
* **GPS EWL(2,5) is effectively unmeasured** — 6 step samples and 1 co-registered arc in the tail, because only the L5-capable GPS fleet qualifies. **Every EWL statement above is Galileo.** A GPS EWL verdict needs a longer lineage or a GPS-heavier roster.

### 13.5 Convergence: 30 min is NOT enough for this quantity

Same run, four disjoint 30-minute windows, same grader:

| window | G L1 DD churn (cyc/ep) | churn-as-one-error, best ratio | gradeable windows |
|---|---|---|---|
| 00:30–01:00 | 3.24 | 3.91× | 1.5 % |
| 01:00–01:30 | 2.04 | 2.60× | 1.7 % |
| 02:00–02:30 | 2.05 | 2.57× | 1.9 % |
| 03:00–03:30 | **1.52** | **1.82×** | **3.3 %** |

Position/clock convergence stabilizes far earlier, but the DD ambiguity churn — the quantity this campaign grades — **halves between the 30-minute mark and 3.5 h**, the gradeable fraction doubles, and the ladder only becomes explainable as a single error magnitude (ratio < 2×) in the final window. So §3.5's "≥ 3 h" is justified by measurement, not convention, and **a 1 h S1 run would have produced a churn-gated non-verdict.** Future S1 re-runs should keep ≥ 3 h; a 30-minute grading window inside it is sufficient.

### 13.6 The class test, scored against the right null — CORRECTED

An earlier pass judged the cross-rung metre columns against a fixed 2.0× threshold and reported "fits neither ladder". **That was wrong, and the correction matters because it flips the churn verdict.** A structureless per-signal error does not produce a "large" ratio in some absolute sense — it produces a ratio equal to the rungs' **sensitivity ratio**, because the implied metres are just the fraction over a sensitivity that varies by that factor. Over the rungs here that null is **39–103×**. So the statistic is how much of the null a single-magnitude hypothesis explains, `1 − log(observed)/log(null)`, not whether it clears a fixed bar. Judged that way, `2.48×` across a `39×` null is **75 % agreement**, not a failure.

**The epoch-to-epoch CHURN is ONE dispersive error of ~0.26–0.31 m.**

| pool | dispersive span | ratio | null | explains | range-like | explains |
|---|---|---|---|---|---|---|
| SPEC-table rungs (8) | 0.213–0.529 m | 2.48× | 39× | **75 %** | 0.286–0.872 m (3.05×) | 72 % |
| all well-sampled (18) | 0.212–0.529 m | 2.49× | 80× | **79 %** | 0.281–0.872 m (3.11×) | 76 % |

That answers the obvious objection — *why would the network churn phase ambiguities at the cycle level when a user filter snaps to stable ambiguities in 15–30 minutes?* It is not an artifact of immature or refloated arcs: conditioned on arc age, the median re-referenced pair step is **2.37 cyc at ages 0–1 and still 1.81 cyc at ages 20–40** (n = 13 440), with 82 % of steps over 0.5 cyc. It is a real ~0.3 m per-epoch DISPERSIVE motion, and it appears in the DD precisely because a between-station, between-satellite DD cancels the per-station and per-satellite gauges but **NOT anything varying per (station, satellite)** — the slant-iono residual above all. A user filter takes its satellite biases fixed from a product and has no global SH field trading against 50 stations' ambiguities; this producer co-estimates both. On L1 that 0.3 m is 1.6 cyc; on E EWL(7,5) it is 0.045 cyc. Same error, read through the ladder.

**The residual per-arc BIAS, after the churn is averaged out, is NOT dispersive.** The state-vs-MW sub-cycle bias explains only **20 %** of its null (10.9× of a 20× null on the dispersive ladder, 14.6× of 26× on the range-like one) — no lane structure, i.e. the per-signal cycle-structured signature. Read together with §13.3's anchor split, that is the same conclusion from two independent statistics on the same arcs: **the churn is iono representation, the residual bias is datum/bias separation.** Those are the two suspect classes §1 listed, and this run separates them rather than choosing between them.

---

## 14. S2 VERDICT — the virtual fold, measured 2026-07-29

`runs/spectro_vf6/`, four legs (§12.4) resumed from the S1 snapshot (week 2313, tow 270060, fingerprint 68a662a415533831), 7 epochs each, `min_arc_epochs = 2`, graded at the first epoch with a folded fixed row (tow 270150). Output: `runs/spectro_vf6/vfold.txt`.

### 14.1 Validity, and the defects it caught — ⚠ mechanism detail RELOCATED

| check | result |
|---|---|
| `off` vs `inert` (fix armed, gate unreachable) | **24 artifacts byte-identical, 0 DIFFER.** The only differences are the two sd_ewl TRACE files, armed-only by construction. |
| datum identical between `off` and `on` at the graded epoch | **YES** — gauge-pin sets match, so the delta is a redistribution in a COMMON frame, not a frame change (standing invariant #5). |
| `ctl` vs `off`, `off` vs `on` | 15 artifacts differ each — the treatments are live, not inert. |

Getting that first row to pass took **two real code fixes plus one finding about the arm itself**. **All three are recorded in full at `docs/SPEC_sd_ewl_prereduce.md` §11.1:** (1) datum-valued integers are ungated by construction — a fixed group now requires at least one MEASURED integer (`fix_datum_only`); (2) the integer must not be allowed to choose the channel pair — otherwise the integer path changes the FLOAT path's election and therefore the solve even when nothing is fixed (`fix_pair_mismatch`, measured 0); (3) **an FFR gate cannot express "impossible"** — at `fix_min_ffr = 0.99999999` the arm still folded 6 fixed rows from 26 measured integers, so the inert arm must use `fix_max_frac_cyc = 1e-9`.

### 14.2 The Δx-per-class table — ⚠ RELOCATED

**The full table and its row-algebra reading now live at `docs/SPEC_sd_ewl_prereduce.md` §11.2.** Headline, because §14.3 depends on it: the FLOAT layer spreads its effect almost evenly between satellite PB and CB (**PB/CB ratio 2.4×**) while THE INTEGER concentrates on PB by **16.7×** and moves CB **7× LESS** than the float layer does (0.0014 vs 0.0102 m) — the sd_ewl card's §8.1 rank mechanism, measured rather than argued.

### 14.3 Campaign verdict

S1 and S2 agree on the class, which per §4 closes the campaign verdict and makes S3 optional confirmation rather than a requirement: the **churn** is iono representation — one ~0.3 m dispersive DD motion per epoch, 75–79 % ladder-explained, unchanged by arc maturity; the **residual per-arc bias** is cycle-structured per-signal (20 % explained), and the anchor partition splits cross-vs-same on all three EWL rungs, so the datum mechanism contributes independently (§3.4 row 4); the **integer fold works as designed**, reaching the satellite phase biases 16.7× harder than the code biases and 7× more selectively than the float layer, in a common datum, with an inert arm byte-identical to float.

**What that does NOT license: arming `sd_ewl.fix_enable` in production.** Gate 3 (the 50-sta 30-epoch ev1 field verdict) is still owed, the A2 re-datum must route fix-onset through the per-λ disc contract before any consumer sees it, and the campaign measured **7 fixed rows per epoch out of ~300 built** — a yield question the channel-consensus rule (over half the arcs excluded) now owns. Full text: `docs/SPEC_sd_ewl_prereduce.md` §11.3.
