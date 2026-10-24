# LASER-STUDY: NGS LASER theory mapped onto the network QC instruments (source study of 2026-10-08)

**STATUS**

- **id:** LASER-STUDY
- **topic:** raim-qc
- **status:** reference
- **summary:** Source study of the NGS LASER guide (v2.7.0) and source (v3.1): placement lesson for datum transforms, the adoption map of the measure-only instruments, the one-way IF-TD slip test finding, two LASER defects and the textbook-audit verdicts.
- **dates in the text:** 2026-10-07 to 2026-10-08
- **source:** agent memory note `project_laser_theory_mapping`, copied 2026-10-24 (the study was never filed as a card; its report stayed in a session scratchpad)

**Outcome.** Headline: the network IF-TD slip test is one-way (sim 439 refloats per 6 h against 0, field 468 against 69); the fix is a two-way median-polish centring, untested. `carry_vce_apply` amplifies observation-side mis-scale 70 to 110 times and needs an identifiability gate. The note also records the adoption map of the measure-only instruments (prior-residual census, global sigma0, redundancy census, MDB, block Baarda, congruence screen), two defects found in the LASER source for the NGS group and the textbook-audit verdicts.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: NGS LASER theory mapped onto the network QC instruments (source study of 2026-10-08)

These results were recorded only in the agent memory note `project_laser_theory_mapping` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: the S2H harvest and its oracle test `tests/test_rx_cb_harvest_demean.cpp` were deleted on 2026-10-14 (commit 15b927c); the one-way IF-TD test is still unfixed (`src/core/network/slip/SlipQc.cpp` takes one network median per pair), and the status of the two LASER defects reported for the NGS group is unknown after 2026-10-08. The code anchors named below now live under `src/core/network/slip/SlipQc.cpp` and `network/qc/Mdb.*`.

Guide: `\\depot\cce_projects\gcap\Software\LASER\NGS_LASER_Guide_v2.7.0.pdf` (Snow/Yang/Whitacre,
Aug 2024; slide 5-x = PDF p. 312+x; 5-40..5-67 is the standard GMM/LESS core, 5-68..5-103 the
theory mined). LASER source (v3.1) at `C:\LASER`: the 5-86 S-transform is in
`src/PGS/LSA/CoreLsa.cpp` `applySTransformation` (S = I − Eᵀ(D₀Eᵀ)⁻¹D₀), plus a big-system variant
that never forms S: per disjoint component, subtract the mean correction over the CONTROL points,
post-solve. Golden oracle `C:\LASER\test\GPS3710_test_PMINOLESS`, read (skip-gated) by
`tests/test_rx_cb_harvest_demean.cpp`. LCF side: memory note `project_ngs_laser_lcf`.

**Placement lesson (dissolved an "architecturally blocked" objection):** solve in any convenient
datum, then re-datum the SOLUTION. The transform belongs at the HARVEST, not in the fold: O(n), no
cross-station prior rows, no frozen pin, no lag loop.

Adoption map (cards' STATUS checked 2026-10-07). The measure-only instruments (prires census,
global σ̂₀, redundancy census, MDB, block Baarda, congruence screen) are all BUILT, solve
byte-identical, and armed on the refound goldens as instruments; their field / power gates are mostly
OWED (see each card).
- Prior-residual census (5-74). Not-folded rows need fold_var + post_var, or the ε clamp inflates t
  about ×3000 and the positive control passes vacuously.
- Exact S-transform (5-86/87): harvest half built as card S2H (memory note `archive_lessons_carry_fold`),
  VALIDATED as frame hygiene through 3 h, MISSED the 12 h level bar, default OFF; the handover /
  vanish-return edge was still open as of 2026-08-15.
- Constraint-aware σ̂₀² (5-74/85; memory note `archive_lessons_raim_sigma0`): n − rkA = n − m + l for
  MINIMAL sets (pins exempt); NON-minimal priors (carry, KP CB rows) need the terms.
- MDB on the network path (5-97): ported by SHARING (`include/filter/BaardaReliability.hpp` used by
  `KalmanFilter` and `filter/NetworkMdb.*`); ledger verdict UNVALIDATED (converged-window gates owed).
- BlockFormal 3D Baarda (5-101): BUILT, unit-gated (memory note `project_block_baarda_card`).
- `carry_vce_apply` as the 5-93..95 variance-factor iteration: BUILT, default OFF (goldens arm only
  the `carry_vce` measurement). Skip BIQUUE (5-96): dies on transient negatives and agrees anyway.

**Durable result: a bigger network does not catch smaller faults per row.** 63 → 303 stations: median
GPS code-row MDB 2.97 → 2.82 m formal, 9.78 → 8.67 m scaled (about 5 %), while phase uncheckability
gets WORSE, 79.2 % → 84.7 %. rᵢ is LOCAL (own-station geometry and the shared parameters it must
support grow together); scale wins in the SHARED parameters only. ⚠ Measured cold, inside ignition
(FT63 11-20 epochs, AGU305 31 epochs), on the `carry_invariant` lineage. Phase uncheckability is a
LINEAGE property: 79.2 % with `carry_invariant` vs 10.8 % with it off + phase leveling, same day.

MDB surprises: σ̂₀,phase DEFLATES (5.7e-12 to 0.107), so `mdb_scaled < mdb_formal` falsely reads as a
tighter guarantee; tagged `*_deflating`, never clamped. Spec errors caught in build (detail in the
cards): r-floor 1e-9, not 1e-3 (1e-3 hides stiff carries); the folded bit must come from each ACTUAL
injector; χ²_b geometric-mean rescale for b > 1 blocks (a χ²₁ constant biases σ̂₀ about 1.8× at b = 8);
MDB σ̂₀ from the post-fit pass, not census/M2 (a carry prior is not a row dispersion); a shared helper
plus independent-oracle tests (file parity is a tautology).

2026-08-04 cross-card finding, scoped: on that era's armed set (`carry_invariant`, no
`phase_leveling`) the whole post-fit tier was inert (postFitRaim gated by `carry_warm`, Pass-1 walks an
emptied `pl_amb`). Since the 2026-09-15 rebase the goldens arm `phase_leveling: true`,
`carry_invariant: false` (verified 2026-10-07). RAIM-pool re-host for `carry_invariant` chains:
memory note `project_raim_pool_rehost`.

CLOSED, do not re-open from LASER: datum choice at emit (MINOLESS / ensemble / s2z / bca, all A/B'd
dead, common-mode absorbable); BIQUUE; sparse-solver envy (LASER's 296k-parameter 3.5 h run is a
static one-shot with no per-epoch carry; memory note `project_network_kf_vs_information_form` stands).
Free riders not taken: D̂{σ̂₀²} = 2σ̂₀⁴/r (5-59) for σ̂₀ error bars; bordered KKT exact constraints
(5-69) only if `w = 1e10` pin conditioning bites again (indefinite KKT vs PD-assuming carry).

**Snow "Adjustment Computations" (2026-01-05 ed.) audit, 2026-10-07** (5 agents, 134 items; not filed as a
card). Core adheres (covariance law, NEQ addition + exact Schur, minimal datum + rank gate, marginalize, DLM/Joseph).
Live gaps, all already documented: RAIM |v|sqrt(w) with no sqrt(r_i), floored MAD sigma0, fixed bars (B2 field arm
owed); dropped cross-row time-update covariance (WSX); white phase vs measured colour (LGM); diagonal handover
S-transform kernel; consumer post-fit gate on sqrt(R) not sqrt(Sr). Two off-path defects found and FIXED the same
day (uncommitted at writing): (1) full-R w-test used Sr^-1 for R^-1; now `KalmanFilter::residualQcKernel` (static,
tested vs a batch GMM oracle in `tests/test_residual_qc_wtest.cpp`); (2) `carry_vce_apply` target lacked the
redundancy division; now `robust_var_r / hand_fold_var_r` (new geo channel of ê²/r, rows with r <= 1e-3 or r < 0
excluded). `stochastic_qc.tex` corrected; record in `docs/SPEC_carry_vce_apply.md` §12.

**Source-level study, 2026-10-08** (LASER PGS 3.2.0 + Aug-2026 "Task 0" at
`D:\GnssProcessorSupport\reference_software\laser\LASER`, literature beside it in `..\Literature`; 9 readers + 5
verifiers, not filed as a card; report in that session's scratchpad `laser_study/LASER_study_report.md`). Verified results:
- HEADLINE: the network IF-TD slip test is ONE-WAY (subtracts the a priori sat clock, differences within a station only,
  one network median per pair: `include/slip/IfPhaseSlipDetector.hpp:24-31`, `src/core/slip/NetworkSlipQc.cpp:1446`). On
  the broadcast bed a sat's a priori clock step refloats it network-wide: slip-free sim `_rap_nbp_s6n` 439 refloats/6 h
  vs 0 on the golden; field `_rap_nbp_f6` 468 vs 69 (recounted from `network_events.csv`). Fix = two-way
  `medianPolish2Way` centring (LASER loop-space idea). May explain part of NBP's broadcast cost: untested hypothesis.
- `carry_vce_apply`: prior-only factors at r ~ 0.01 amplify obs-side mis-scale ~70-110x (LASER never estimates Q0
  components alone); needs an identifiability gate. The "b = 3.7" figure is an old-lineage sim number.
- Block-Baarda census INERT on the float vehicle (34014/34029 field, 35221/35221 sim blocks singular); rank-gate
  margin 1.7-2.0 decades at 1e-11 (window ~[1e-13, 5e-12]); field primary code white scatter 0.11-0.13 m vs 0.30 stated
  (instrument only; applying a code VC is the code analogue of the falsified phase cut).
- Dead: actuator on carried ambiguity priors (repeats D5/ALC, MDB 64 m); exact constraints / KKT for the 1e10 pins (0
  PD drops); sqrt(tr N/m) row scaling (pins never reach the gate spectrum); single-epoch IF code misclosure screen (gross
  rows are carry iono); Delaunay DD bases (NetworkZStore exists). Soft W6 gauge rows' carried 1/w term is real but mm-level.
- LASER defects for NGS: A1 observed-point rows at column `parameterOrder` not `3*parameterOrder`
  (`LSA/Net3DLsa.cpp:4246-4253`); constrained-point test uses Qxi not Q0 - Qxi (`LSA/CoreLsa.cpp:3333-3352`); 5 medium.
  Not yet raised with the maintainers as of 2026-10-08.

Record: `docs/cards/raim-qc/{PRIOR-RESIDUAL-CENSUS,REDUNDANCY-CENSUS,MDB-RIDER-A,GLOBAL-SIGMA0-RIDER,
BLOCK-BAARDA-RIDER-B,CONGRUENCE-RIDER-C}.md`, `docs/cards/code-bias/S2H-HARVEST.md`,
`docs/SPEC_datum_graph_census.md`.
