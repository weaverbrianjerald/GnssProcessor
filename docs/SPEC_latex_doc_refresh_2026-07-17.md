# SPEC — LaTeX documentation refresh & new-doc plan (2026-07-17)

**Scope.** Which existing `docs/report/*.tex` chapters need updating, and which new
LaTeX documents are worth writing now vs. deferring, given that (a) most of the
project is stable enough to deserve a draft, and (b) the network processing is
still being reorganized (INC-0..8 decomposition, TU freeze through INC-12) and the
AR-oriented pieces are still being changed.

**Method.** Every chapter was audited against the source it documents, with a
skeptical verifier re-checking each high/medium finding against the tree at HEAD.
Verifier tally across re-checked findings: **74 CONFIRMED, 2 OVERSTATED, 0 REFUTED**
(the two overstatements are one nuance in `estimator_infrastructure.tex`, folded in
below). Undocumented subsystems were separately scouted for new-doc potential, with
each one's *code churn since 2026-07-01* and *default-flag state* used to decide
write-now vs. defer.

---

## 0. The gap in one number

Every `docs/report/*.tex` was last edited **2026-06-15 .. 2026-06-28**. Since the
last doc touch (`04c15ce`, Jun 28), **34 commits landed in `filter/` + `app/` and 1
in `docs/report/`**. The entire July network frontier — Card 4 invariant carry, the
MW-WL/EWL/UWL/lane-carry cascade, the D5 innovation gate, SH-VTEC, W6 code-bias
multiband, the emit/rebase/contract-v2 layer, GLONASS-in-network, and the
Setup/Assembly TU decomposition — postdates all of it.

The **guiding distinction** for this whole spec: the network *algebra* (S-basis,
arrowhead Schur, datum theory) does **not** churn just because its *translation unit
moved* in the decomposition. Settled algebra is documentable now even while the code
around it is reorganized. What must be deferred is the algebra that is *itself* still
being falsified-and-cured week to week (the AR carry / widelane frontier).

---

## Part A — Updates to existing chapters

Worst-first. "Effort" is prose-editing time; none of the Tier-2/3 items require
re-deriving an equation — the settled math verified correct almost everywhere.

### Tier 1 — Actively wrong: false status/claims that mislead a reader

These state that a *shipped, load-bearing* subsystem is unbuilt, or assert an
equation the code contradicts. Fix first; several are one-paragraph edits.

| Chapter | The false statement | Fix |
|---|---|---|
| [network.tex](report/network.tex) | §"Scope and status" (lines 46–63): runNetwork "is a scaffold … logs a *not yet implemented* error and returns non-zero." **The string does not exist**; runNetwork is the ~8,780-line live epoch driver — the most-developed subsystem in the repo. | Rewrite scope para; see also §net-sbasis W5 remark and the missing subsystems in Tier 2. |
| [pipeline_diagrams.tex:264](report/pipeline_diagrams.tex) | Fig. 5 caption: "the live runNetwork() epoch loop is staged." | Rewrite the one sentence; optionally add the closed-loop consumer branch to the figure. ~1 h. |
| [slip_detection.tex:28](report/slip_detection.tex) | "the geometry-free **and ionosphere-free phase** detectors … are staged — present in the tree but not compiled." **`IfPhaseSlipDetector.cpp` is compiled and armed on the network path** (`network.cycle_slip.if_phase_td`); only `GfSlipDetector.cpp` is out. Commit `39af08f` (Jul 14) armed it and its message even says it "fix[ed] stale STAGED/inert docs" — but left this `.tex`. | Split "compiled" from "registered", "user path" from "network path"; retitle §261 "(staged on user path; armed on network path)". Also fix the now-stale in-code comment `SlipDetectorRegistry.cpp:67`. |
| [site_displacement.tex:437](report/site_displacement.tex) | §sitedisp-apl: APL "is presently **absent**", `computeAtmosphericLoadingCorrection` is a "1 mm cosine stub … *uncalled*", `d_APL = 0`. **All false** — commit `04c15ce` deleted the stub and wired a real S1/S2 thermal-tide model into the modelled range; the named function no longer exists. | Rewrite from "known gap" → derivation of the implemented S1/S2 tide; rescope the honesty note to the still-real *non-tidal/synoptic* APL gap. |
| [ssr_corrections.tex:330](report/ssr_corrections.tex) | Ionosphere subsection: "there is no SSR path that writes the slant-iono state … the remaining unimplemented piece on both sides." **False since `6fa0cd1` (Jul 11)** — an in-process slant-iono channel exists (`pushSlantIono`/`getSlantIono`), producer emits, consumer folds the BLUP pseudo-obs, gated `closed_loop.consume_network_iono`. | Rewrite that paragraph; keep the (still-true) on-wire RTCM VTEC rejection point. |
| [frames_time.tex:135](report/frames_time.tex) | eq:frame-pole asserts `h = |z| − N(1−e²) = |z| − a(1−e²)`. The second equality is **wrong** (at the pole `N(1−e²) = b`, the polar radius); the chapter's *next sentence* states the correct value. ~21 km error if a reader trusts the equation. | One-term fix. Code is correct. |
| [realtime_engine.tex:249](report/realtime_engine.tex) | Time-promotion is "anchored on the rover's wall-clock GPS time" via `TimeUtils::nowContinuousGpsSeconds`. **Reversed by RTB-1(a) (`f44b3c6`, Jul 2)**: host clock consulted only at cold start, every later field promoted by nearest-continuation. Also cites `currentGpsWeekFromSystem`, **a deleted function**. | Rewrite the §rt-time anchor paragraph; drop the dead citation. |

### Tier 2 — Stale: settled math is fine, but a major landed subsystem is missing and citations have rotted

The equations verify correct; the drift is undocumented mechanisms + line-number
citation rot from July inserts. These are `substantial-revision` because they add a
new subsection, not because anything is wrong.

- **[state_vector.tex](report/state_vector.tex)** — 3 live state blocks added after
  the Jun-24 freeze are entirely absent: per-satellite **network clock** (`<sat>|SC`,
  `estimate_sat_clocks`), per-sat **iono-rate** (`İ^s`, `enable_iono_rate`), and the
  **receiver phase-bias split** (`<system>_RPB_<signal>`, `phase_datum_eq34`, a
  fresh-each-epoch no-carry local state). Plus one **inverted claim**: `resetAmbiguity`
  is described as "preserving cross-state correlations" — the code *decorrelates*
  (zeros row/col) and preserves the *value* (reset-not-repair). Also: ambiguity unit
  stated as "cycles" at :128 vs "metres" at :241 (code = metres); `clock_bias_q`
  10³→10⁴; `acceleration_psd`→`acceleration_psd_q`. ~2–4 h.
- **[estimator_infrastructure.tex](report/estimator_infrastructure.tex)** — the WLS
  seed section is wrong about what the WLS actually seeds: **clock only** (position
  comes from the RINEX header; ZWD and the boxed covariance eq. (7) are dead outputs
  with no consumer). *(Verifier nuance: the clock IS seeded on first-epoch/jump/gap —
  so soften "contradicted on all three counts" to position+ZWD+covariance, not clock.)*
  The **cold-clock seed** (`seedColdReceiverClock`, `network.cold_clock_seed`) is an
  undocumented 6th seed. The ambiguity seed §(e) points at the wrong function
  (`StateInitializer.cpp:255` → should be `StateManager::computeAmbiguitySeed`) and
  eq. (14) drops the phase-clock and slant-iono terms the code subtracts. "Sticky
  hysteresis" mis-described (absolute gate, no challenger comparison); the hard
  elevation gate is the real low-el mechanism, unmentioned. ~3–4 h.
- **[stochastic_qc.tex](report/stochastic_qc.tex)** — the entire network σ̂0 / RAIM
  layer is undocumented and uses *different* algebra: robust MAD σ̂0 =
  `max(floor, 1.4826·median|r|)` (not Ω/dof) that **drives control** (refloat /
  exclude / withhold / URA), plus a batch redundancy `r_i = 1 − w·aᵀCov(θ)a`. One
  false sentence attributes `NeqAccumulator` to the network (it's PPP-only). ~18
  `KalmanFilter.cpp` citations drifted ~+31 (`f44b3c6`). Scope eq:qc-mdb as KF-only. ~2–4 h.
- **[realtime_engine.tex](report/realtime_engine.tex)** — beyond the Tier-1 time
  anchor: the **SBF decoder subsystem** (`src/core/sbf/`, ~1440 lines, active
  first-class obs source) has no decode subsection though its RTCM/MSM twin gets full
  equation treatment; **MSM5** is now emitted (doc says it isn't); add 1033/1230 to
  the dispatch list. (SBF is a **section add**, tracked in Part B.)
- **[site_displacement.tex](report/site_displacement.tex)** — beyond the Tier-1 APL
  rewrite: the **SIF/FES2014b** OTL coefficient source (highest precedence, `+180°`
  horizontal phase convention) is undocumented; precedence is SIF→BLQ→EOT20→zero. And
  **~25 `GnssCorrections.cpp` citations** drifted non-uniformly (+126 pole block,
  +280 SET block) plus `atm_loading_corr`→`otl_corr`/`apl_corr` rename throughout.
  The tide physics itself verifies line-by-line. ~3–5 h.

### Tier 3 — Minor drift: light edits (settled math intact)

Each verified mathematically correct; edits are localized prose + citation
re-anchoring. Grouped, since the pattern repeats:

| Chapter | Essence of the drift | Effort |
|---|---|---|
| [atmosphere.tex](report/atmosphere.tex) | **SH-VTEC co-estimated field undocumented** (landed Jun 30, in this chapter's own dir) — but this is a *new-doc* call, see Part B; plus IONEX geometry moved to `computeIppGeometry`, ~15 citation re-points, one geodetic/geocentric naming nit. | 2–3 h |
| [kalman_smoother.tex](report/kalman_smoother.tex) | iono-rate CWNA coupling falsifies two "only/pure-random-walk" universals; Baarda data-snooping gate override undocumented; **smoother BSSD pivot-handover frame reconciliation** (~100 lines, `inverseReReferenceGroup`) absent. All citations still resolve. | ~½ day |
| [bssd_dd.tex](report/bssd_dd.tex) | GLONASS per-slot IFB **survives** BSSD (falsifies boxed eq:bssd-code, default-off flag); pivot policy prose wrong (coverage-tier not "highest elevation"; DD ranks on SNR); reset-and-reseed fallbacks + iono/sat-bias re-referencing + forced handover unmentioned. | 2–3 h |
| [ambiguity_resolution.tex](report/ambiguity_resolution.tex) | MLAMBDA kernels extracted to `filter/IlsCore.{hpp,cpp}` (`9627c6d`) → ~6 citations stale; the network producer's reuse via `ils::ilsFix` + stricter `network.ambiguity_resolution.*` gates undocumented; eq:pseudo missing `λ_k`; `wl_only_pseudo_var`/`wl_max_frac` absent from table. | 1–2 h |
| [product_evaluation.tex](report/product_evaluation.tex) | "precise & broadcast never mix" broken by RTB-4 orbit/clock source split (`use_precise_orbits`, default off); `ephCutoverShiftMetres` + median-spacing mixed-rate merge undocumented. Math perfect. | 1–2 h |
| [observation_equations.tex](report/observation_equations.tex) | transmission-time solve is single-step-from-pseudorange, not fixed-point iteration; `relinearize_geometry` not "enabled in all shipped configs" (6 network configs ship it off); IF/MW/GF scope over-broad for plain PPP. | Low |
| [notation.tex](report/notation.tex) | sat-clock glossed product-only (estimated in network); receiver-index list omits network/user modes; `f_1` undefined (= GPS L1 universally, 1.8% BDS error if misread); `k` bound twice (epoch vs FCN); **`β` vs `b̄` symbol collision** for sat phase bias (single-source-of-truth violation, see §C). | 1–2 h |
| [foundations.tex](report/foundations.tex) | Wrapper is sound; drift is entirely in the subfiled `state_vector.tex` (the 3 missing blocks above) and one `observation_equations.tex` default. | ~30 min |
| [observation_physics.tex](report/observation_physics.tex) | Cites non-existent `computeModelledObservable` (×3; real = `computeBaseModeledValue`); attitude inline-copy mis-attributed to `Antenna.cpp` (real = `GnssCorrections.cpp`); RTB-5 `skip_satellite_pco` note (defer to SSR doc). | ~20 min |
| [network_example.tex](report/network_example.tex) | Worked solution pins receiver clock `a_A≡0` labeled "shared-block gauge"; code pins a **satellite** clock. S-transform-equivalent, so recovery holds — relabel to `g¹≡0`. | ~15 min |

### Tier 4 — Current: leave as-is (trivial)

- **[imu_aiding.tex](report/imu_aiding.tex)** — chapter is materially accurate; the
  contradictions found are **stale code comments** (`ImuIntegrator.hpp` Somigliana
  Doxygen, three "inert" aiding comments, `KalmanFilter.cpp:1112` sign comment) where
  the `.tex` is the *correct* side. One real doc nit: sign of a dropped
  gravity-gradient term. ~1 h if the code comments are cleaned alongside.

### Special case — the generated appendix (a code fix, not a prose edit)

[generated_tables.tex](report/generated_tables.tex) wrapper prose is fine, but the
appendix is **materially incomplete and no regen fixes it**:

- `scripts/gen_report_tables.py` reads only `GnssConfigLoad.cpp` (line 54). The INC-1
  config split (`774fdba`, Jul 16) moved the network section loader into
  `GnssConfigLoadNetwork.cpp`, which now holds **121 `jval<bool>` flags** — the whole
  network frontier. Only **5** `network.*` rows currently reach the table.
  **Fix: add `GnssConfigLoadNetwork.cpp` to the generator's `SRC` (glob
  `GnssConfigLoad*.cpp`), read both TUs in `gen_config_toggles()`, update the
  caption/banner, then regen.**
- Two *benign* fragments (`config_toggles` GnssConfigLoad-visible part, `state_keys`)
  drifted with ordinary July additions and **self-heal on regen** — the `--check`
  guard already flags them.
- Bug to note: `--check` is not read-only (it writes fragments before comparing) and
  `docs/report/generated/` is gitignored/untracked, so there is no committed baseline
  for CI. If drift-guarding in CI is wanted, that needs fixing too.

---

## Part B — New LaTeX

Verdicts driven by *code churn since Jul-01* and *default-flag/validation state*, not
by size. Four buckets.

### B1 — Write now (settled algebra; code reorg doesn't touch the math)

The report's `\part{Network mode}` currently holds `network.tex` + `network_example.tex`.
The settled July network math is enough for **four new subfiles** plus **two
subsections** appended to `network.tex`. Proposed expanded structure:

| New file | Topic | Pages | Stability evidence |
|---|---|---|---|
| **network_datum.tex** | S-system datum theory: 3 clock/ISB datums (pin / sum-to-zero / broadcast-anchored), 3 ambiguity datums, the **null-space rank gate** (`assertNullSpaceRank`, eigen-count vs `declared_dimension`), estimability spectrum, **W6 satellite code-bias (OSB) datum** (IF-pin `a₁+a₂=1`, per-(const,signal) sum-to-zero, GF↔iono confounding). Absorbs+extends the existing §net-sbasis/§net-phasebias, which get trimmed to pointers. | 8–12 | S-basis code = 4 commits since Jul-01, **all verbatim INC-5..8 TU moves, byte-identical gated**; pin/s2z/broadcast + rank-gate algebra unchanged. All datum flags default-off; settled default is the legacy pin already in `network.tex`. **Canonical Teunissen/Odijk S-transform algebra.** |
| **network_iono_vtec.tex** | Co-estimated sun-fixed **SH-VTEC** field: normalized-Legendre basis, single-layer `STEC = M·VTEC`, the Option-A constraint row, common-mode-free `R = diag+σ²11ᵀ`, and the load-bearing **Kaula-Tikhonov datum** (a₀₀ vs absolute sat-DCB). | 6–9 | Kernel `SphericalHarmonicIonosphere.{hpp,cpp}` = **0 commits since Jul-01** (frozen Jun 30); NetworkEstimator diffs are TU-move + key plumbing. Validated Jun 30 (<0.5 TECU IONEX reproduction, orthonormality/partial tests). Default-off gated by the *AR frontier*, not by unstable math. **Scope to Stage-0/1 core**; BeiDou observability / temporal warm-start / closed-loop emission → short "open extensions" note only. |
| **network_qc.tex** | Robust network-relative QC: station-relative **iono-deweight IGG gate**, median/MAD + Tukey median-polish primitives, the **network IF-TD triple-difference** slip driver (cross-station pooling, empirical-Bayes shrinkage, refloat), the **D5 innovation gate** (exact H0 `Cov(v)=M⁻¹−GᵀΣG`), reset/refloat-over-repair discipline. | 10–14 | Every commit to the 4 code paths since Jul-01 is verbatim INC-2/INC-6, byte-identical. Deweight card committed+armed; RobustStats has 12 unit tests. **Open items are tuning/validation** (v2 adaptive ratchet over-fire, IF-TD `k_mad`, D5 em10 power) → explicit OPEN callouts inside the chapter, not omissions. |
| **network_product_emission.tex** | The emission-frame layer downstream of the internal datum: **NetworkEmitRebase** S-selection (`b_s → b_s + δ + n_s·λ` removal, robust median coordinate-descent, integer left on slipped/first-sighting keys — invariant #1), the phase-bias **jump/discontinuity test** (τ = f·λ), the **v1 continuity filter** (χ²₁ geometric-mean self-tuning R), and the **combined-correction stochastic model** (sigc/sigcc/rho/agek, Zhang 2021/Psychas 2024). | 7–9 | 5 headers, 8 commits since Jul-01 = additive feature landings, all unit-locked (`test_card4_emit_rebase`, `test_pb_emit_filter`, `test_network_emit`). **Document v1 only**; the PB-filter **v2 gate is excluded** — its dt-independent-σ premise was *measured false* Jul-15 (20 cm@30s vs 8.1 cm@300s), header walked back Jul-16. Wire format / snapshot → Doxygen. |
| **validation_grading.tex** | Project-wide validation *methodology*: the single-realization mandate (why raw estimated params are ungradeable), S-invariant graded quantities (DD-fractional integer-closeness: mean\|f\|, P(<0.1), circular resultant R), the DD-step optimism/calibration ratio, field-run gates + run-end counters. | 4–6 | `grade_ddfrac.py` = 2 commits since Jul-01 (statistic defs stable); `tests/` churn is decomposition + default-off byte-identical AR, none touching grading math. Rests on settled published geodesy + textbook circular stats. **Standalone** (cross-cutting: governs position and wire grading too, not just network DD); the document has no "how do we know it's right" capstone. |

Append to **network.tex** as subsections (not standalone — they build directly on
its already-typeset `addSharedInfo`/Schur machinery):

- **Full-covariance shared carry** (2–3 pp) — information-form KF on the shared
  border (`addSharedPriorDense`, `shared_carry_full_cov`): marginalization-not-
  conditioning, the exactness identity vs the batch solve, the diagonal-carry
  lock-in rationale. Settled+validated (landed at freeze `d003698`, no algebra churn
  since). Explicitly **scoped to exclude** the Card-4/WL carries that ride the same
  Woodbury folds.

> **Coordination note.** `network_datum.tex` (S-system datum) and the W6 code-bias
> leg are the *same* datum story — write W6 as sections **inside** `network_datum.tex`,
> not as a separate doc, so one datum narrative isn't fragmented across three files.
> All B1 network docs share the fix to `network.tex`'s stale scope para + §net-sbasis
> W5 remark — do that fix once, first.

### B2 — Sections in existing chapters (settled, but not a chapter's worth)

- **GLONASS FDMA** → new §in [observation_physics.tex](report/observation_physics.tex),
  2–3 pp. The FDMA algebra is settled (`cfa3080`, long ago) but two of its three legs
  (time system, per-slot IFB state) are **already** in `frames_time.tex` +
  `state_vector.tex`. Missing: per-FCN frequency/wavelength closed form, its
  propagation into IF/iono coefficients, the meters-ambiguity slip-safety invariant,
  and a consolidated float-only rationale. **The network-GLONASS producer at scale is
  the churning part → out of scope** (one-line deferral note).
- **SBF (Septentrio) observation decode** → new §in
  [realtime_engine.tex](report/realtime_engine.tex) after §rt-msm, ~1–1.5 pp. `src/core/sbf/`
  = **0 commits since Jul-01**, bit-exact validated. Only the Meas3Ranges (4109)
  differential reconstruction is report-worthy algebra — the direct analogue of the
  MSM range-reconstruction eqs already there. Everything else (SubframeAssembler ICD
  scaling, SignalMap, framing) → Doxygen; AsteRx-U = one-paragraph cross-ref.

### B3 — Defer (still churning: the AR frontier — do NOT document yet)

These are real estimation mathematics that belong in the reference *eventually*, but
their *math* (not just their TU) is still being falsified-and-cured. Deferring them is
the direct answer to "we're still changing … to achieve stable network processing with
AR." Each has a written trigger to promote.

| Topic | Why defer | Promote when |
|---|---|---|
| **Card-4 invariant carry** (`network_carry.tex`) | 6 days old; recent commits *add new math* (D5 v2 swapped Cov(v) for LS-VCE σ0; q_z flips 0↔1e-4). **Never field-validated** — C3c handoff says "NOT golden" (1.5–2.4 vs 0.085 target). Double-gated default-off. | Field falsifier reaches golden class OR carry committed default-capable **and** WL/D5 layers stop moving. *(The settled sub-kernel — DD cycle basis, unimodular Φ^z, tap-off/marginalization — is documentation-ready; outline captured for that moment.)* |
| **MW-WL/EWL/UWL/lane-carry cascade** (`network_widelane_carry.tex`, 8–12 pp) | One commit ever (`89fe588`, Jul 16 — a big drop *yesterday*, not maturity). All 9 SPEC/HANDOFF cards Jul 15–16; multiple sub-mechanisms falsified-then-cured in the last 48 h. Every gate default-off; validation is byte-identity + 15-ep circR, **not accuracy**. | First datum-invariant graded field run passes **and** the tier/carry architecture stops being falsified per-card. |
| **LS-VCE carry-q monitor** (→ future §in stochastic_qc.tex) | 1 commit ever, default-off, **apply path throws NOT IMPLEMENTED**; handoff says original premise "FALSIFIED", demoted to diagnostic. Primary estimator diverged from spec (geometric-mean vs LS-VCE) — which is authoritative is an open call. | Estimator choice fixed and (ideally) apply path validated. |

### B4 — Not LaTeX material (Doxygen / ops-markdown, regardless of churn)

Explicitly **not** for the mathematical reference — no state/functional/stochastic
model/datum/estimator of their own. Recording so they aren't re-proposed:

- **Data/product file management** (PathResolver / ProductResolver / DataDownloader /
  BaselineFixtures) — file location & acquisition; already in `CONFIG.md` + `CLAUDE.md`
  + thorough header Doxygen. Settled, just plumbing.
- **Network logging/trace architecture** (`NetworkRunTraces`, CSV schema) — serialization;
  the math it *prints* is defined in the estimator/QC chapters. Column semantics owned
  by `docs/rt_service_operations.md` + `docs/network_logviz_plan.md`.
- **Aiding-source abstraction** (`IAidingSource`/registry/manager) — composition layer,
  byte-identity-with-IMU by design. Its one substantive client (IMU error-state EKF) is
  *already* fully in `imu_aiding.tex`. *(Deferred half-page: a "row-contributing aiding
  pseudo-obs" subsection once ZUPT/NHC/baro is field-validated.)*

---

## C. Cross-cutting issues (fix once, everywhere)

1. **Notation single-source violation.** `network.tex:28` defines a file-local `β`
   for the estimable satellite phase bias, which `gnssreport.sty` (the declared
   single source of truth) assigns `b̄^s_σ` (honored by `state_vector`/`network_example`).
   Harmonize to `b̄` when touching `network.tex`.
2. **Line-number citation fragility is the dominant failure mode.** The single
   largest category of drift across *every* stale chapter is `file:line` citations
   invalidated by July inserts (site_displacement ~25, stochastic_qc ~18, frames_time
   ~17, atmosphere ~15, …). The math and the *function names* proved stable; the line
   numbers did not survive three weeks. **Recommendation: cite function/symbol names,
   not line numbers**, in the refresh — it removes ~⅓ of all findings permanently and
   stops the next month's edits from re-staling the doc.
3. **The generator drift-guard isn't wired to CI** and mutates the tree on `--check`
   against an untracked baseline (see §A special case). If drift-guarding is wanted,
   the generator needs the network-TU fix *and* a read-only `--check` against a
   committed baseline.
4. **Stale in-code comments surfaced as a side effect** (not doc work, but cheap to
   fix while adjacent): `SlipDetectorRegistry.cpp:67`, `Coordinates.cpp:22` (pole
   longitude), `ImuIntegrator.hpp:145` Somigliana, three aiding "inert" banners,
   `LiveEphemerisStore.cpp:513` GLONASS, `KalmanFilter.cpp:1112` sign.

---

## D. Recommended sequencing

1. **Tier-1 status fixes** (network scope para, pipeline Fig 5, slip status, APL,
   ssr iono, pole eq) — cheap, and they're the statements that actively mislead. Do
   the `network.tex` scope + §net-sbasis W5 fix here since every B1 network doc
   depends on it.
2. **Generator patch + regen** — mechanical, restores 121 network flags to the appendix.
3. **B1 write-now new docs**, in dependency order: `network_datum.tex` (+W6) first
   (the others cross-ref its datum), then `network_iono_vtec.tex`, `network_qc.tex`,
   `network_product_emission.tex`, the `network.tex` full-cov-carry subsection, and
   `validation_grading.tex` (independent, can slot anywhere).
4. **Tier-2 stale rewrites** + **B2 sections** (SBF, GLONASS) — pair each with its
   citation-name conversion (§C.2).
5. **Tier-3 light edits** — batch; mostly citation re-anchoring + one-paragraph adds.
6. **Defer B3** until the AR frontier settles; the promote-triggers are written above.

Net new LaTeX if all of B1+B2 land: **5 new subfiles + 3 in-chapter sections**,
~40–55 pages, all of it settled algebra whose TU-churn does not change the math.
