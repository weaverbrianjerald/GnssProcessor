# SPEC — MW-seeded wide-lane layer (M5 card "WL-1")

**Status: Phase A BUILT + unit-tested + verified (Jul-15 2026), then FAILED its own §7.5 rung and DEMOTED to a default-OFF diagnostic** (`network.mw_wl.enable`) by `SPEC_ewl_wl_cascade_card.md` §3.0.2 — A/B forensics only. **Phase B was never implemented** (config surface parsed + warned inert; the keys are DEPRECATED, §4). Phase D (§3.5) was INVERTED to the front of the ladder by WL-2. Root cause of the §7.5 failure is F6, attributed in **§15**. Absorbed the implementation handoff 2026-08-15 (was `docs/HANDOFF_mw_wl_card.md`, now `docs/attic/`) — §12–§15 are that record.

> **Family disposition banner.** The whole MW/WL lane family — WL-1 (this card), WL-2, WL-3, UWL-1, LF-1 — is under the standing verdict **"MW/WL as conditioning — do NOT arm; net-harmful"** (CLAUDE.md), reinforced by the AGU-26 amendment (lane OFF is the speed call at BOTH 63 and 305 stations, 1.30× and 12×) and by the DOY-129 ≈83 %-wrong-integer measurement (`SPEC_ewl_wl_cascade_card.md` §12.7). A RECORD, not an open build order; arming state lives in `docs/CONFIG_promotion_ledger.md`.
> **Provenance.** Every `runs/<name>` path cited below resolves under `D:\GnssProcessor_archive\2026-08-12\runs\` (archived off C: 2026-08-12).

**One sentence:** feed the MW detector's per-arc Welford means, double-differenced over the Card-4 fundamental-cycle chord basis, into the solve as rank-K pseudo-observation blocks on cross-band ambiguity columns — giving the network the per-arc code-coherence accumulator it structurally lacks, and making the wide-lane integers reachable in tens of epochs (Phase A float, Phase B fix).

---

## 1. Motivation — the Jul-15 evidence chain (do not relitigate)

- Estimator-free MW census from raw RINEX (30 ep, 305 sta, C2W/L2W star DDs): **circR = 0.62, spread 0.16 cyc = the code-noise prediction** — WL integers are in the data, 1/√N-clean. `scripts/mw_census.py`.
- KF-implied WL (dd-frac trace, L1−L2 per pair): **uniform (circR 0.001–0.11) in EVERY configuration** — SH on/off, IF on/off, 30/60/300 s, `code_bias_q` and `ionosphere_q` A/Bs (both proven null: no receiver-CB states exist on the network path; the iono-q change gave a byte-identical epoch-5 NEQ).
- Root cause: locals (per-LOS iono, ZWD, rclk, RPB) are epoch-fresh in the 3card lineage; the only cross-epoch transports are the invariant DD carry (phase side) + border carry. Long-horizon CODE coherence — what determines N_WL — has no container. Memory `project_wl_structural_gap_jul15`; `docs/CONFIG_promotion_ledger.md §A1`.
- Consequence for AR ordering: WL is the earliest-converging integer functional (iono-insensitive, code-noise-limited, σ≈0.5 cyc/√N_ep); per-band N₁/N₂ separation is iono-limited (hours). Classical cascade: WL fixed ⇒ NL is a single integer per pair at λ_eff ≈ 10.7 cm on mm-precision IF phase ⇒ N₁, N₂ recovered exactly, per band. WL-1 is the first rung; it does NOT replace the uncombined estimator — it adds missing information INTO it.

## 2. Estimability (why this is S-system-legal)

Undifferenced MW = λ_WL·N_WL + b_WL,r + b_WLˢ + ε — datum-contaminated; never used directly. The card constrains ONLY closed-cycle (DD) functionals: for a fundamental cycle C in the (station,sat) arc graph, Σ_C ±MW̄_arc = λ_WL·(∇ΔN₁ − ∇ΔN₂) + Σ_C ±ε̄_arc — both bias lumps cancel IDENTICALLY (each receiver and satellite appears with +/− once per cycle). ∇ΔN_WL is an integer combination of the canonical Odijk estimable set — the SAME invariance class as the Card-4 carried chords. Gauge transformations (clock shifts, PB re-levels, pivot handovers) have zero component along these rows ⇒ the W5 null-space gate must be provably untouched (FIREWALL test, §7). BIA-lessness of the network MW is IRRELEVANT in this form (biases cancel in the cycle; the absolute WL integer of a single arc is never needed).

**No pivot, no second S-system (conceptual guard):** the WL rows define NO datum — nothing is pinned, no reference satellite/station is privileged. The spanning tree is a BASIS choice for the cycle space (any tree spans the same invariant subspace; tree swaps re-express without information change), NOT an S-basis choice. The constraints are S-system-invariant MEASUREMENTS applied inside the network's ONE S-system (eq34 et al.), valid unchanged under any datum the network runs. Classical BSSD/BRSD pivots eliminate states (a datum act); this card differences derived arc STATISTICS (a measurement act).

> ⚠️ **FRAME CORRECTION — this section's original corollary is MEASURED WRONG on the network path** (implementation finding, Jul-15; full text §13.1). The corollary read: *"read the detector's RAW undifferenced MwArcStats — never its level-mode / pivot-referenced outputs."* There ARE no raw undifferenced stats to read: the per-station `MwSlipDetector` is built with `setCorrections` and runs **BIA-corrected and SINGLE-DIFFERENCED** against a per-(constellation,signal-pair) MW pivot elected by elevation. `arcZdMean` reconstructs a **ZD-consistent** value (SD + pivot re-added) with a **conservative** variance. **The estimability argument above is unaffected** — everything in §2 up to the corollary stands verbatim, and `SPEC_ewl_wl_cascade_card.md` §2's "identical invariance class to WL-1 §2" remains correct.

## 3. Mechanism

### 3.1 Source statistics (per station, no new math)

`MwSlipDetector::MwArcStats` maintains per-arc {mean, M2 (Welford), count} in meters (`MwSlipDetector.hpp` ~464, stale). The layer reads them through `arcZdMean(sat, sig1, sig2) -> {mean_m, var_of_mean_m2, n, bia_corrected}` — the ZD-consistent reconstruction of §2's frame correction, NOT raw stats. The detector's own arc lifecycle (slip flag ⇒ stats reset; gap ⇒ new arc) IS the reset wiring — variance of a fresh arc is honest-huge.

**Construction gate change (landed):** detectors previously existed only under `networkPhaseLeveling() && (turbo_edit || mw_detect)` (`ApplicationNetwork` ~1130, stale). Relaxed to `... || networkMwWlSeed()` so the card works in the 3card lineage (phase_leveling stays OFF). OFF stays byte-identical: detectors are observation-side only, and the PASS-A refloat collection is explicitly `mw_detect`-gated so `mw_wl` never feeds the slip funnel.

### 3.2 Rows — ride the Card-4 chord basis (cross-band blocks)

For each carried chord (a fundamental cycle, per (const, signal-group) graph), the Card-4 driver already owns the cycle arc list with signs. The WL row for a cycle couples the L1-band and L2-band ambiguity columns of the SAME station/sat cycle:

  row terms: for each arc (r,s) in the cycle with sign c:
    (+c/λ₁ on station r's L1 amb column of sat s) and
    (−c/λ₂ on station r's L2 amb column of sat s)          [columns in meters]
  z (cycles) = Σ_C ±MW̄_arc / λ_WL
  R (cycles²) = Σ_C var_of_mean / λ_WL²  (+ a floor, §4)

`NetworkNormalEquations::DdPriorBlock::Term{station, local_col, coeff}` rows are ALREADY general signed combos across stations (`NetworkNormalEquations.hpp` ~349, stale) and `foldReducedDdPriors` folds arbitrary rank-K blocks. The epoch's WL rows are packaged as NEW `DdPriorBlock` instances appended to the same `dd_priors_` fold path. NO kernel change was needed. **Fold-composition caveat: §13.2** (WL blocks share L1 columns with the carry blocks and compose INEXACTLY).

**Band/channel discipline:** build a WL row ONLY when every arc in the cycle has both bands present on a consistent signal pair at that station, and the MW stats were computed on the SAME (C/L) channels the estimator uses for that station (channel mixing leaves DCB residue — the mw_census lesson). Skip otherwise; count skips. Mixed satellite-bias-frame cycles are also skipped (§13.3 F3).

### 3.3 Phase A — float WL constraint (the card's core)

Every epoch, for every eligible chord-cycle with n ≥ `min_arc_epochs` on all arcs: append the WL pseudo-obs block. That is all. The joint solve tightens the WL direction; per-band DD posteriors inherit; the carried z's stop wandering along WL; D5 flag rate should DROP (its innovations lose their largest component). No integers are asserted in Phase A. *(Measured: it did not work — the D5 rate ROSE instead, §14/§15.)*

**Double-count guard (required, LANDED):** the current epoch's P1/P2/L1/L2 are in the solve AND inside a same-epoch arc mean — a 1/n information double-count. Inject the LAGGED arc statistics (mean/var/n as of the END of the PREVIOUS epoch, i.e. snapshot the stats before this epoch's addMeasurement), making the pseudo-obs independent of the current epoch's measurement noise. Residual temporal correlation (multipath) is what `sigma_floor_cyc` prices; the M2 LS-VCE monitor is the honesty check. The realization is `MwArcStats::{mean,M2,count}_lag`, snapshotted at the top of the per-epoch update; `arcZdMean` serves ONLY the lagged fields; both constant-shift re-reference sites shift `mean_lag` alongside `mean` — §13.3 F1. *(This contract is what `include/slip/MwSlipDetector.hpp:473`, `:813` and `src/core/slip/MwSlipDetector.cpp:185`, `:2179` cite as "SPEC_mw_wl_card §3.3".)*

### 3.4 Phase B — WL integer fixing (NEVER IMPLEMENTED)

Design was: when a cycle's constrained WL posterior passes the FFR gate (σ_WL < `fix_sigma_cyc`, |frac| < `fix_frac_cyc`, defaults 0.10/0.15), replace the pseudo-obs with the rounded integer at `fix_weight_sigma_cyc` (hard-ish row, NOT an exact constraint — leaves a rollback path), and un-fix immediately when any member arc slips/resets (standing invariant #1). **Config surface only ever landed** (parsed + mutex-warned inert; `fix_enable=true` logs a loud INERT warning, mirroring the `carry_invariant_innov_refold` precedent). SUPERSEDED: `SPEC_ewl_wl_cascade_card.md` replaced this posterior-gated surface with statistic-gated fixing (§4 deprecations).

### 3.5 Phase D — EWL/L5 cascade (INVERTED to the FRONT of the ladder)

> **SUPERSEDED.** `SPEC_ewl_wl_cascade_card.md` (WL-2) explicitly *"inverts `SPEC_mw_wl_card.md` §3.5 (Phase D 'EWL cascade, after B') to the FRONT of the ladder"* — the EWL runs FIRST, because its lane is uncarried and its rows win without a fight, whereas the WL lane (both bands carried) cannot be entered by rows at all (§15). The physics below stands; only the ORDER was wrong.

For the fleet subset tracking L5, the extra-wide lane (L2−L5, λ_EWL = c/(f₂−f₅) ≈ 5.861 m) has single-epoch DD σ ≈ 0.05–0.12 cyc from code noise alone — **one-epoch rounding is safe** (P(wrong) ≪ 0.1 %), no averaging window needed. The fixed EWL then (a) cross-checks the MW WL (three-band closure: EWL + WL constrain the same integer lattice), and (b) gives the L5 subset **the full near-instant ladder 5.86 m → 0.86 m → 0.107 m**. Same estimability argument (closed cycles, biases cancel); same channel discipline per band. Caveat: L5X receiver-side ATX fallback inflates L5 PHASE residuals on some stations (known, not a bug) — irrelevant to the EWL integer content, but keep L5 out of NL rows in v1.

## 4. Config surface (all `network.mw_wl.*`, loader in `loadNetworkSection_`)

| key | default | status | meaning |
|---|---|---|---|
| `enable` | false | LIVE (diagnostic only) | Phase A master. OFF = byte-identical (no detectors built for the card, no blocks). Demoted to a default-OFF A/B-forensics diagnostic by WL-2 §3.0.2; the loader warns if armed together with `wl_inject_enable`. |
| `min_arc_epochs` | 10 | LIVE | Lagged Welford n per arc before its cycles are eligible. |
| `sigma_floor_cyc` | 0.05 | LIVE | R floor per row (multipath honesty). |
| `max_rows_per_epoch` | 0 | LIVE | 0 = unlimited; else lowest-R rows first (perf guard; count drops). |
| `f6_dump` | false | LIVE, log-only | §15 one-shot per-arc/per-row attribution dump → `logs/network_mw_wl_f6.csv`. Does NOT change the solve. Row/block caps `f6_max_rows` (8) / `f6_all_blocks` (false) were added later — `SPEC_ewl_wl_cascade_card.md` §12.7. |
| `debug_r_scale` | — | probe-only, loud warn, NOT production | Row-stiffness multiplier used for the Jul-15 authority probe (`SPEC_ewl_wl_cascade_card.md` §1). |
| `fix_enable` | false | **DEPRECATED** | Phase-B posterior-gated master — never implemented. Loader warns: REPLACED by WL-2's statistic-gated keys. |
| `fix_sigma_cyc` / `fix_frac_cyc` | 0.10 / 0.15 | **DEPRECATED** | Phase-B FFR gate — never implemented; same loader warning. |
| `fix_weight_sigma_cyc` | 0.01 | REUSED | Originally the Phase-B fixed-row σ; WL-2 reuses this key as its hold-row σ (`SPEC_ewl_wl_cascade_card.md` §3.3). |

New toggles are added to the summary log line + `isLoggerEnabled` per the CLAUDE.md convention.

## 5. Lifecycle / slip response (invariant #1 pre-flight)

- Phase A rows are RE-DERIVED each epoch from live detector state — nothing is carried by the card itself; there is no stiff prior to go stale. Arc slip ⇒ detector resets stats ⇒ next epoch's row has honest-huge R (or is skipped by `min_arc_epochs`). Gap ⇒ same via the detector's arc lifecycle.
- Phase B fixes WOULD be stiff assertions ⇒ they required an explicit un-fix on member-arc slip plus a D5-style innovation check on fixed rows. Never built; do not arm `fix_enable` (inert and warned).
- Coordinate with Card-4: a chord refloat (D5/coarse) does NOT invalidate the MW stats (the detector arc may be clean while the carry re-taps) — the WL row persists. A DETECTOR slip invalidates both (already funneled via `carry_slipped_ambs`).

## 6. Telemetry (invariant #4 — counters or it didn't happen)

Run-end line: `Network (M5/WL-1 MW-seeded wide-lane): N WL row(s) folded over M primary cycle(s) … mean R … skips — {non-primary, missing L2 band, no MW arc (channel/pivot), young arc, bad λ, mixed frame}`. Config echo carries the `mw_wl_*` fields. **Grading stays `grade_ddfrac.py` + `scripts/mw_census.py` as the external truth** — do not add `mw_wl` fields to the dd-frac flow unless needed. **Accepted-install-only counter rule:** per-build counters accumulate into run totals only on the ACCEPTED install (the last build before the accepted solve wins), so a RAIM re-solve cannot double-count. WL-2/WL-3/UWL-1 inherit this rule.

## 7. Validation outcomes (Fable, Jul-15; the plan collapsed to results)

| rung | result |
|---|---|
| 7.1 Byte-identity OFF (same-build) | **PASS** — all CSVs bit-exact vs the pre-card exe; ON ≡ OFF through ep 10 (surgically inert until eligible). §14. |
| 7.2 FIREWALL (unit, blocking): fold a WL block, assert `assertNullSpaceRank` nullity UNCHANGED | Property holds structurally (the WL rows are gauge-free DDs on local columns and touch no shared border column) — inherited from the kernel's gate-4 DatumFreedom. §13.2. |
| 7.3 Unit battery (row signs, λ scaling, channel discipline, R propagation, eligibility counters) | **PASS** — 15/15 including the F2/F3/F5 gates named in §13. |
| 7.4 Falsifier (injected slip, force-fire) | Not reached — the card failed 7.5 first and Phase B was never built. |
| 7.5 Rung run (50 sta, KF-WL circR vs the MW census; instrumented with `neq_export_epoch` + `scripts/neq_svd.py`, where the WL-direction marginals had to visibly tighten) | **FAIL.** D5 1628 vs OFF 369; KF-WL circR 0.11–0.23 vs OFF 0.18–0.31 — the constraint pulled AWAY from integers, and the marginals did not tighten. Root cause F6, §15. |
| 7.6 Graded 3 h @30 s (305 sta) vs the R5 em10d8 control | Never run (7.5 failed). |
| 7.7 Phase B after Phase A grades clean | Never reached. |

## 8. Perf notes

Rows add rank-K_WL to the existing fold: K_WL ≤ #carried chords per band pair (~2-3k at 305 sta) but each row is 8 terms — the fold's per-station reduction absorbs them like DD priors. Measured small. If the fold grows, `max_rows_per_epoch` is the guard (count what's dropped — no silent caps). The netar Phase-2 probe has `ils_max_dim` (default 0; 100 at scale) — REQUIRED for any fixability logging at 305 sta. *(The lane stack's real perf wall arrived later and is a different mechanism — `SPEC_lane_carry_card.md` Appendix LF-1.)*

## 9. Deferrals (triaged per invariant #3)

- **Field-inert:** GLONASS (FDMA — excluded like netar); cross-day arc spillover (bounded by detector max-gap); Welford-vs-MAD R cross-check (sigma_floor covers).
- **Field-blocking if skipped:** none for Phase A. Phase B without the un-fix wiring is NOT field-runnable (invariant #1) — and it was never built, so `fix_enable` is inert.

## 10. Verified anchors (Jul-15 tree — ⚠ LINE NUMBERS ARE STALE)

⚠️ **`src/core/app/ApplicationNetwork.cpp` was decomposed (INC-0..12) from ~14.7k lines to ~2.4k of thin epoch-loop orchestration.** Every `ApplicationNetwork.cpp` anchor recorded here is DEAD — the detector-construction gate `ApplicationNetwork.cpp:1134` (the `setCorrections` site) / `~1126-1139` / relax site `~1130`, the WL wiring `~6562-6693`, the dump block `~8455` — and the ones below 2400 point at unrelated code. The WL wiring now lives in `src/core/filter/NetworkMwWlGlue.cpp`, `NetworkMwWlLayer.cpp`, `NetworkDdPriorBuild.cpp` and `NetworkInstallCarry.cpp`. Prefer the anchors in `SPEC_ewl_wl_cascade_card.md` §12.2 — re-verified 2026-07-28.

Structural anchors still true by symbol (re-grep, never trust the number): `MwSlipDetector::MwArcStats{mean, M2, count}` + the `_lag` snapshot fields — `include/slip/MwSlipDetector.hpp`; `DdPriorBlock::Term{station, local_col, coeff}` general rows + the rank-K fold `foldReducedDdPriors` — `include/filter/NetworkNormalEquations.hpp`; cycle arc lists + signs — `NetworkInvariantCarryDriver`; h2local (StateManager idx → local col) at the epoch call sites (`asm_res[i].h2local`), per-band ambiguity columns via `StateManager::getAmbiguities()` + `StateKeys::parseAmbiguityKey`.

## 11. Open questions (ANSWERED — see §12.2 for Q1/Q2/Q3)

1. Cycle basis reuse vs rebuild; is `carry_invariant` required? → **§12.2 Q1.**
2. Where the MW stats are read (parallel safety). → **§12.2 Q2.**
3. Signal-pair selection when a station tracks >2 GPS bands (L5). → **§12.2 Q3.**
4. **Loose vs tight coupling (STUDY):** this SPEC is the loosely-coupled form (detector stats as lagged pseudo-obs) — zero new states, arc lifecycle = reset wiring, hot path untouched. The tightly-coupled alternative (per-arc WL states, or a per-LOS iono LocalCarry so the estimator accumulates code coherence natively) is statistically purer but re-opens the carried-state obligations. Pre-registered criterion: if the LS-VCE honesty check on the WL rows shows sustained σ̂ ≫ 1, go tight. **ANSWERED BY MEASUREMENT:** the tripwire fired (σ̂₀ 1.86 vs 1.42 control in the fold-on shape) and the tight form was built as WL-3 (`SPEC_lane_carry_card.md` §1).
5. **Phase C (product-side extension, post-B):** with DD WL integers fixed, the satellite WL bias (UPD) network becomes estimable up to the standard per-component datum — the network could then EMIT WL phase-bias products, still fully self-contained (no .BIA anywhere). Out of scope; MOOT while Phase B is unbuilt and the family is do-not-arm.

---

# Implementation record (Phase A, Opus + Fable, Jul-15 2026)

*§12–§15 were `docs/HANDOFF_mw_wl_card.md`, absorbed 2026-08-15. Section numbers here are NEW; the HANDOFF's own labels are carried as aliases so a label grep still lands — **HANDOFF §1/§2 → §12, §3 → §13.1, §4 → §13.2, §6b → §13.3, §6c → §14, §6d → §15**, with F1–F6 verbatim throughout. HANDOFF §5 (Phase B deferred) is §3.4; HANDOFF §7 (Fable's next steps) is §7.*

## 12. What landed  *(HANDOFF §1, §2)*

### 12.1 Files  *(HANDOFF §1)*

| File | Change |
|---|---|
| `include/filter/NetworkMwWlLayer.hpp` / `src/core/filter/NetworkMwWlLayer.cpp` | **NEW** pure numeric leaf. From the carry driver's chord-basis `DdPriorBlock`s + a per-arc resolver it emits WL pseudo-obs `DdPriorBlock`s (`+c/λ1` on L1 col, `−c/λ2` on L2 col; `z = Σ c·MW̄/λ_WL`; `Pz = diag(R)`, `R = max(Σ c²·var/λ_WL², σ_floor²)`). Eligibility classification + counters + the `max_rows_per_epoch` cap. |
| `include/slip/MwSlipDetector.hpp` / `.cpp` | **NEW** `arcZdMean(sat,sig1,sig2)` accessor → a **ZD-consistent** per-arc Welford summary (§13.1). |
| `include/config/GnssConfigTypes.hpp` | **NEW** `NetworkConfig::MwWlConfig` nested struct + `mw_wl` member. |
| `include/config/GnssConfig.hpp` | `networkMwWlSeed()` getter. |
| config loader | Parse `network.mw_wl.*`; validate (mutex: requires `carry_invariant`; Phase-B `fix_*` parsed but warned NOT-implemented); armed-ON summary log; clamps. |
| `src/core/app/ApplicationNetwork.cpp` (pre-decomposition) | Detector-construction gate relaxed to build under `mw_wl` (phase-leveling OFF); PASS-A refloat collection explicitly `mw_detect`-gated; `buildCarryDdPriors` builds the reverse/primary-pair maps + resolver and appends the WL blocks; run-end telemetry; config JSON echo. |
| `CMakeLists.txt` | `NetworkMwWlLayer.cpp` + `tests/test_network_mw_wl_layer.cpp`. |
| `tests/test_network_mw_wl_layer.cpp` | **NEW** 12 gates on the row math / eligibility / cap / grouping. |
| `tests/test_codebias_network_config.cpp` | `MwWlSeedContract` — the mutex + parse + clamp contract. |

Build: `gnss_core` clean (Debug + Release). Suite at the time: **736 pass / 1 pre-existing skip / 0 fail**, then **738/1/0** after the §13.3 review fixes. *(Dated historical records — the suite is now 1360 cases / 196 suites; not a current gate figure.)*

### 12.2 Answers to §11 Q1/Q2/Q3  *(HANDOFF §2)*

**Q1 — cycle basis reuse vs rebuild; carry_invariant required?** REUSE (one graph truth). The WL layer consumes the carry driver's *returned* `DdPriorBlock.rows` directly — each row is a fundamental cycle already expressed as signed `Term{est_station,local_col,coeff}` on the L1-band columns (chord + tree path). No independent cycle basis is built. **v1 REQUIRES `carry_invariant` ON** — mutex-checked at load (fail-loud), consistent with the other coupled toggles.

**Q2 — where the MW stats are read (parallel safety).** Read SERIALLY in `buildCarryDdPriors` (post-PASS-A, pre-solve), off the FINAL carry blocks, via the station-local `st.mw_slip->arcZdMean(...)` (const, no shared write). No hazard under `parallel_stations` — the parallel region is PASS-A geometry/detect; the WL build runs in the serial seam like the carry fold.

**Q3 — signal-pair selection when a station tracks >2 GPS bands (L5).** v1 = the estimator's PRIMARY L1/L2 pair only: the two lowest DISTINCT bands present with a valid column (ties lexicographic for determinism). A cycle whose carry-group signal is not that primary sig1 → `skip_not_primary` (so an L2/L5/secondary-L1 group never double-folds a WL row). L5/EWL cascade deferred — later inverted to the front by WL-2.

## 13. Measured deviations from this SPEC

### 13.1 SPEC §2/§3.1 frame premise — WRONG on the network path  *(HANDOFF §3)*

**§2 assumed the per-arc MW means are raw undifferenced (ZD). They are NOT.** The per-station `MwSlipDetector` (built with `setCorrections`, `ApplicationNetwork.cpp:1134` — stale anchor, §10) runs BIA-corrected and **single-differenced against a per-(constellation,signal-pair) MW pivot** chosen per station by elevation (non-pivot arc mean = `MW_bia(sat) − MW_bia(pivot)`; pivot arc mean = `MW_bia(pivot)`, stored undifferenced).

Why the card still works: a **closed fundamental cycle balances each station's signs**, so the per-station pivot term cancels within the cycle regardless of which pivot each station picked — *except* that a cycle arc that IS a station's own pivot is stored ZD while its siblings are SD. Resolution: `arcZdMean` reconstructs a **ZD-consistent** per-`(station,sat)` value (`SD + pivot re-added = MW_bia(sat)`; a pivot / no-BIA arc is already ZD), so every arc enters the layer in one frame and the cycle then cancels receiver bias (per-station balanced signs) and satellite bias (per-satellite balanced signs, uniform per sat by the station-independent product). The reconstructed variance is **conservative** (SD var + pivot var — the common pivot variance actually cancels in the cycle, so R is a safe upper bound).

**The verify triplet the handoff set** (a)/(b)/(c), only (a) and (c) discharged: (a) the KF-WL circR climbs toward the `scripts/mw_census.py` value as arcs mature (§7.5) — a sign/frame error would push it the wrong way; **(b) ⚠ STILL OWED — run at least one config that loads NO BIA product (raw-ZD path, `bia_corrected=false` everywhere) and one that does (SD-pivot path); both should behave**; (c) the falsifier (§7.4) confirms an injected slip widens/drops the affected rows and asserts no wrong integer.

### 13.2 Fold-composition caveat (SPEC §3.2)  *(HANDOFF §4)*

The WL blocks are appended to the SAME `foldReducedDdPriors` call as the carry blocks and share the L1 ambiguity columns. `foldReducedDdPriors` folds each block against the **pristine** per-station `N_rr`, so two blocks sharing local columns compose **inexactly** (drops the cross term — the documented "two post-reduce Woodbury folds don't compose" property; the Card-4 phase-C review CONFIRMED it). **Within** a WL block the rank-K fold is joint (exact) — WL rows of one carry component are grouped into ONE block, so their mutual coupling is exact; different components are column-disjoint (exact). Only the **WL↔carry** coupling is approximate. The emitted PRODUCT stays datum-invariant regardless: the WL rows are gauge-free DDs on local columns (touch no shared border column), so the W5 null space is untouched — the FIREWALL property is inherited structurally from the kernel's gate-4 DatumFreedom. The exact fix would be a per-(const, band-pair) **JOINT block** (carry-L1 + carry-L2 + WL rows folded together). ⚠ **§15 proves that alone would NOT have cured F6.**

### 13.3 Fable review findings F1–F4 (Jul-15 eve; F1–F3 applied, F4 OPEN)  *(HANDOFF §6b)*

Review verdict: architecture + every documented decision endorsed (basis reuse, serial-seam read, observation-side-only discipline, per-component grouping, accept-install counters, Phase-B deferral, and the §13.1 ZD-consistency correction).

- **F1 (was an UNdocumented §3.3 violation): lagged arc statistics.** `detectLargeMwResets` updates the Welford stats in PASS-A, so a same-epoch `arcZdMean` read included the current epoch — the 1/n double-count §3.3 requires avoiding (it would also bias the LS-VCE σ̂ reading that decides §11 Q4). Fix: `MwArcStats::{mean,M2,count}_lag` snapshot taken at the top of the per-epoch update; `arcZdMean` serves ONLY the lagged fields; **both** constant-shift re-reference sites (BSSD pivot S-transform, bias-commit re-anchor) shift `mean_lag` alongside `mean` so the lagged value stays in the arc's current frame.
- **F2: exact shared-arc covariance within a WL block.** Cycles of one component share tree-path arcs; a diagonal Pz double-counted their MW noise. Now `Pz(i,j) = Σ_shared c_i·c_j·var/λ_WL²` (per-arc contribution lists + sorted-merge intersection). ⚠ **Numerical trap: the σ floor stays DIAGONAL-ONLY so Pz remains PSD.** Gates: `RowsGroupedByCarryBlock` (sharpened), `SharedArcCovarianceExact`.
- **F3: mixed satellite-bias-frame guard.** Cycle-level satellite-bias cancellation requires one frame per cycle; arcs now carry `bia_corrected` through `MwArcZdMean`/`ArcData` and mixed cycles are skipped + counted (`skip_mixed_frame`, in the run-end line). Dormant on the self-contained path; guards the BIA era. Gate: `MixedBiasFrameSkipped`.
- **F4 (dormant path, ⚠ STILL OPEN): pivot-switch mid-arc in the SD reconstruction.** If the per-station MW pivot switches while an SD arc runs, the Welford mean mixes references. Verify the S-transform re-reference covers the stats used by `arcZdMean` before any BIA-era use. **Owed by whoever first arms this path with a .BIA loaded** — together with §13.1's owed two-arm no-BIA/BIA A/B.

## 14. Verification record (Fable, Jul-15 eve) — §7.1 PASS, §7.5 FAIL  *(HANDOFF §6c)*

Harness: 50 sta, GPS-only, 30 s, em15, 15 ep, SH-off/IF-off/serial shape (`_wlver_50sta_15ep_{off,on}.json`), `min_arc_epochs=10` ⇒ rows arm at ep 11.

**PASS (§7.1):** byte-identity OFF (all CSVs bit-exact vs the pre-card exe) AND ON≡OFF through ep 10 (surgically inert until eligible). Telemetry exact: **849 rows / 4797 cycles / σ ≈ 0.222 cyc** (the predicted band). Unit battery 15/15.

**FAIL (§7.5): the folded WL target was WRONG; D5 correctly contained it.**

- **F5 (FOUND + FIXED same-day): correction-frame re-expression missing.** The kernel solves CORRECTIONS about x_lin (no KF write-back); MW̄ is ABSOLUTE. Un-subtracted, the target displaced the per-band DDs by **med 131–156 cyc** at arming (≈ C·x_lin). Fix: `ArcData::{x_lin1,x_lin2}` via a `lin_of` map in the resolver; `z_corr = Σc·MW̄/λ_WL − Σ[(c/λ1)x_lin1 − (c/λ2)x_lin2]`; unit gate `LinearizationFrameSubtraction` (exact hand value).
- **F6 (residual after F5):** arming displacement **med 49–57 cyc** decaying to ~6 by ep 15; **D5 1628 (vs OFF 369)**; KF-WL circR **0.11–0.23 vs OFF 0.18–0.31** — the constraint still pulled AWAY from integers. Attributed in §15.

## 15. F6 ATTRIBUTED — stiff per-band carry dominance, NOT a target bug  *(HANDOFF §6d)*

*(The card's terminal verdict. `SPEC_ewl_wl_cascade_card.md` cites it as the F6 evidence; memory `mw-wl-f6-fix-directions` carries the probe result.)*

Instrument: the one-shot dump `network.mw_wl.f6_dump` (log-only, default-OFF → `logs/network_mw_wl_f6.csv`), build-time capture + post-solve reconstruction. Ran the §14 15-ep 50-sta ON config; the dump fired at the first arming epoch (ep 11, tow 259500) and telemetry reproduced §14 EXACTLY (849 rows / 4797 cycles / σ≈0.222, D5 1628).

**What the dump proves (rules OUT the suspect tree):**

- `z_mw` is as-integer-as-MW-gets: frac ±0.08–0.48 across the 8 dumped rows — the honest MW code-noise band at n=10 (matches `mw_census.py` circR≈0.6, NOT tighter). The measured WL target is FINE. **Not a z_MW / cycle-closure / arcZdMean bug.**
- `z_corr == zhat_block` to 4 dp on every row — the layer arithmetic (F5 subtraction, λ scaling, signs) is exact. **Not a convention or lin-frame bug.**
- **The real defect: in EVERY cycle exactly ONE arc — the eq34 CHORD — reconstructs to a non-zero WL; all tree-path arcs reconstruct to `x_abs ≈ 0` (they are the datum tree pins).** So `recon_abs` (the estimator's KF WL over the cycle) = the chord's CODE-SEEDED FLOAT WL, and the WL fold leaves it there: on the clean rows the innovation ≈ `recon_abs − z_mw` (row0 4.10/4.25, row2 6.53/6.70, row3 23.5/23.3, row7 35.9/36.1). **The WL constraint does not move the WL toward `z_mw`.**

**Mechanism (why the fold is overridden, not why the target is wrong).** The carry runs a SEPARATE per-(const,signal) block, so `∇ΔN₁` (L1 group) and `∇ΔN₂` (L2 group) are EACH time-linked stiffly (q_z = 1e-4 ⇒ a tight zero-q carry). The chord's L1 column is held by the L1 carry and its L2 column by the L2 carry, each to **σ ≈ 0.06–0.09 cyc**. Two independent per-band DDs pin `WL = ∇ΔN₁ − ∇ΔN₂` to **≈ 0.09 cyc** — TIGHTER than the MW pseudo-obs (**σ ≈ 0.22 cyc**) — but to the WRONG (code-seeded) value. The appended WL block is a *separate* post-reduce Woodbury fold sharing those columns, so per §13.2 it drops the cross term and, being ~3× looser, cannot overturn the carries. Its only residual effect is conflicting pressure that trips the D5 innovation gate on the carries → re-tap churn (**369 → 1628**) → the slow, noisy 50 → 6 cyc "decay" (D5-mediated absorption, not clean tightening), and the dd-frac circR drifts the wrong way.

**Consequence for the fix — the §13.2 joint block ALONE will NOT cure it.** Folding carry-L1 + carry-L2 + WL jointly still lets the two σ≈0.06 per-band carries over-determine the WL direction to 0.09 cyc; a 0.22-cyc measurement stays outvoted. The cure must stop the per-band carries from stiffly propagating the *garbage* WL (standing invariant #1: a stiff time-link past a correction warps the solution — here the "correction" is the MW WL the carry never received). Candidate directions:

- **(A) MW-seed the WL at chord birth** — split the MW WL into the L1/L2 carried seeds at birth so `∇ΔN₁−∇ΔN₂` starts MW-consistent, then the stiff carry HOLDS the right WL. Requires cross-group coordination at birth. *(Later ruled UNNECESSARY by the `debug_r_scale` probe — a loose lane converges from garbage seeds the moment a strong measurement arms; `SPEC_ewl_wl_cascade_card.md` §1.)*
- **(B) Anisotropic carry** — carry the (NL, WL) rotation per chord and give the WL direction a loose q so the MW pseudo-obs owns it. *(Probe-endorsed; became WL-3, `SPEC_lane_carry_card.md`.)*
- **(C) WL-informed re-tap** — treat a confident MW-vs-carried-WL disagreement as a WL slip and re-tap the chord's carry from the MW WL (reuses D5 plumbing). *(Became WL-2's inject primitive with the refloat replaced by a lane-targeted value edit.)*

**Secondary (separate, latent) finding — station-1 x_lin.** Station index 1 carries pathological ambiguity LINEARIZATION points (**x_lin ≈ 256540 m ≈ 256 km**, uniform across G01/G10/G15/G16/G27). They cancel in the invariant DD (so the solve survives) but make the WL row's `z_lin` a difference of two ~3e5 values and render its correction-frame innovation ill-conditioned for station-1 cycles (row 5: innov 17.3 vs true gap 1.0). Not the F6 cause. **ROOT-CAUSED and FIXED later:** it is the cold ambiguity seed subtracting a clock term that is zero at the first network epoch — cured by `cold_clock_seed`, which cut `x_lin1_m` from 269 352 m to 57 m at station 1 (`SPEC_ewl_wl_cascade_card.md` §12.6).
