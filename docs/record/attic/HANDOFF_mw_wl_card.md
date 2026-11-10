# HANDOFF — MW-seeded wide-lane layer (M5 card WL-1), Phase A

**Status:** Phase A IMPLEMENTED + built (Debug & Release) + unit-tested (Opus, Jul-15 2026).
Default-OFF, byte-identical when off.  Verification battery (SPEC §7) is Fable's next.
Implements `docs/SPEC_mw_wl_card.md`.

---

## 1. What landed (files)

| File | Change |
|---|---|
| `include/filter/NetworkMwWlLayer.hpp` / `src/core/filter/NetworkMwWlLayer.cpp` | **NEW** pure numeric leaf.  From the carry driver's chord-basis `DdPriorBlock`s + a per-arc resolver it emits WL pseudo-obs `DdPriorBlock`s (`+c/λ1` on L1 col, `−c/λ2` on L2 col; `z = Σ c·MW̄/λ_WL`; `Pz = diag(R)`, `R = max(Σ c²·var/λ_WL², σ_floor²)`).  Eligibility classification + counters + the `max_rows_per_epoch` cap. |
| `include/slip/MwSlipDetector.hpp` / `.cpp` | **NEW** `arcZdMean(sat,sig1,sig2)` accessor → a **ZD-consistent** per-arc Welford summary (see §3). |
| `include/config/GnssConfigTypes.hpp` | **NEW** `NetworkConfig::MwWlConfig` nested struct + `mw_wl` member. |
| `include/config/GnssConfig.hpp` | `networkMwWlSeed()` getter. |
| `src/core/config/GnssConfigLoad.cpp` | Parse `network.mw_wl.*`; validate (mutex: requires `carry_invariant`; Phase-B `fix_*` parsed but warned NOT-implemented); armed-ON summary log; clamps. |
| `src/core/app/ApplicationNetwork.cpp` | Detector-construction gate relaxed to build under `mw_wl` (phase-leveling OFF); PASS-A refloat collection explicitly `mw_detect`-gated (so `mw_wl` is observation-side only); `buildCarryDdPriors` builds the reverse/primary-pair maps + resolver and appends the WL blocks; run-end telemetry; config JSON echo. |
| `CMakeLists.txt` | `NetworkMwWlLayer.cpp` + `tests/test_network_mw_wl_layer.cpp`. |
| `tests/test_network_mw_wl_layer.cpp` | **NEW** 12 gates on the row math / eligibility / cap / grouping. |
| `tests/test_codebias_network_config.cpp` | `MwWlSeedContract` — the mutex + parse + clamp contract. |

Build: `gnss_core` clean (Debug + Release).  Suite: **736 pass / 1 pre-existing skip / 0 fail**.

---

## 2. Answers to SPEC §11 (open questions)

**Q1 — cycle basis reuse vs rebuild; carry_invariant required?**
REUSE (one graph truth).  The WL layer consumes the carry driver's *returned* `DdPriorBlock.rows`
directly — each row is a fundamental cycle already expressed as signed `Term{est_station,local_col,coeff}`
on the L1-band columns (chord + tree path).  No independent cycle basis is built.  **v1 REQUIRES
`carry_invariant` ON** — mutex-checked at load (fail-loud, `GnssConfigLoad.cpp`), consistent with the
other coupled toggles.

**Q2 — where the MW stats are read (parallel safety).**
Read SERIALLY in `buildCarryDdPriors` (post-PASS-A, pre-solve), off the FINAL carry blocks, via the
station-local `st.mw_slip->arcZdMean(...)` (const, no shared write).  No hazard under
`parallel_stations` — the parallel region is PASS-A geometry/detect; the WL build runs in the serial
seam like the carry fold.

**Q3 — signal-pair selection when a station tracks >2 GPS bands (L5).**
v1 = the estimator's PRIMARY L1/L2 pair only: the two lowest DISTINCT bands present with a valid column
(ties lexicographic for determinism).  A cycle whose carry-group signal is not that primary sig1 →
`skip_not_primary` (so an L2/L5/secondary-L1 group never double-folds a WL row).  L5/EWL cascade deferred.

---

## 3. ⚠️ SPEC §2 frame correction (important — verify empirically)

**SPEC §2 assumed the per-arc MW means are raw undifferenced (ZD).  They are NOT on the network path.**
The per-station `MwSlipDetector` (built with `setCorrections`, `ApplicationNetwork.cpp:1134`) runs
BIA-corrected and **single-differenced against a per-(constellation,signal-pair) MW pivot** chosen per
station by elevation (non-pivot arc mean = `MW_bia(sat) − MW_bia(pivot)`; pivot arc mean = `MW_bia(pivot)`,
stored undifferenced).

Why the card still works: a **closed fundamental cycle balances each station's signs**, so the
per-station pivot term cancels within the cycle regardless of which pivot each station picked — *except*
that a cycle arc that IS a station's own pivot is stored ZD while its siblings are SD.  Resolution:
`arcZdMean` reconstructs a **ZD-consistent** per-`(station,sat)` value (`SD + pivot re-added = MW_bia(sat)`;
a pivot / no-BIA arc is already ZD), so every arc enters the layer in one frame and the cycle then cancels
receiver bias (per-station balanced signs) and satellite bias (per-satellite balanced signs, uniform per
sat by the station-independent product).  The reconstructed variance is **conservative** (SD var + pivot
var — the common pivot variance actually cancels in the cycle, so R is a safe upper bound).

**Fable, verify:** (a) the KF-WL circR climbs toward the `scripts/mw_census.py` value as arcs mature
(SPEC §7.5) — a sign/frame error would push it the wrong way; (b) run at least one config that loads NO
BIA product (raw-ZD path, `bia_corrected=false` everywhere) and one that does (SD-pivot path) — both
should behave; (c) the falsifier (SPEC §7.4) confirms an injected slip widens/drops the affected rows
and asserts no wrong integer.

## 4. ⚠️ Fold composition caveat (SPEC §3.2 — the Phase-A grading question)

The WL blocks are appended to the SAME `foldReducedDdPriors` call as the carry blocks and share the L1
ambiguity columns.  `foldReducedDdPriors` folds each block against the **pristine** per-station `N_rr`,
so two blocks sharing local columns compose **inexactly** (drops the cross term — the documented
"two post-reduce Woodbury folds don't compose" property; `NetworkNormalEquations.hpp` `foldReducedDdPriors`
warning, and the Card-4 phase-C review CONFIRMED finding).

- **Within** a WL block the rank-K fold is joint (exact) — WL rows of one carry component are grouped into
  ONE block, so their mutual coupling is exact; different components are column-disjoint (exact).  Only
  the **WL↔carry** coupling is approximate.
- The emitted PRODUCT stays datum-invariant regardless: the WL rows are gauge-free DDs on local columns
  (touch no shared border column), so the W5 null space is untouched — the FIREWALL property is inherited
  structurally from the kernel's gate-4 DatumFreedom (SPEC §7.2 should confirm mechanically).
- For a Phase-A **float** pre-conditioner (over-tightening along WL is roughly the intended effect) this
  is defensible; the empirical size is the SPEC §7.6 grading question.  If it matters, the exact fix is a
  per-(const, band-pair) JOINT block (carry-L1 + carry-L2 + WL rows folded together) — a bigger change to
  the carry-driver grouping + install, out of v1 scope.

## 5. Phase B (WL integer fixing) — DEFERRED

Config surface only (`fix_enable`/`fix_sigma_cyc`/`fix_frac_cyc`/`fix_weight_sigma_cyc`, parsed +
mutex-warned).  The fixing mechanism + the mandatory un-fix-on-slip wiring (standing invariant #1: no
stiff integer assertion without a wired + force-fire-tested slip response) is a **follow-up increment**,
per SPEC §7.7 ("Phase B only after Phase A grades clean").  `fix_enable=true` today logs a loud INERT
warning (mirrors the `carry_invariant_innov_refold` precedent).  **Do not arm before SPEC §7.4's
force-fire passes.**

## 6. Telemetry (invariant #4)

Run-end line `Network (M5/WL-1 MW-seeded wide-lane): N WL row(s) folded over M primary cycle(s) …
mean R … skips — {non-primary, missing L2 band, no MW arc (channel/pivot), young arc, bad λ}`.
Per-build counters are accumulated into run totals only on the ACCEPTED install (the last build before
the accepted solve wins; no RAIM-re-solve double-count).  Config echo carries `mw_wl_*` fields.

## 6b. Fable review outcome (Jul-15 eve) — APPROVED, three findings applied

Review verdict: architecture + every documented decision endorsed (basis reuse,
serial-seam read, observation-side-only discipline, per-component grouping,
accept-install counters, Phase-B deferral, and the §3 ZD-consistency correction
of the SPEC's wrong raw-ZD assumption).  Three findings, all applied same-day
(suite 738/1/0 after):

- **F1 (was an UNdocumented SPEC §3.3 violation): lagged arc statistics.**
  `detectLargeMwResets` updates the Welford stats in PASS-A, so a same-epoch
  `arcZdMean` read included the current epoch — the 1/n double-count the SPEC
  requires avoiding (it would also bias the LS-VCE σ̂ reading that decides §11
  Q4).  Fix: `MwArcStats::{mean,M2,count}_lag` snapshot taken at the top of the
  per-epoch update; `arcZdMean` serves ONLY the lagged fields; both constant-
  shift re-reference sites (BSSD pivot S-transform, bias-commit re-anchor)
  shift `mean_lag` alongside `mean` so the lagged value stays in the arc's
  current frame.
- **F2: exact shared-arc covariance within a WL block.**  Cycles of one
  component share tree-path arcs; diagonal Pz double-counted their MW noise.
  Now Pz(i,j) = Σ_shared c_i·c_j·var/λ_WL² (per-arc contribution lists +
  sorted-merge intersection; σ floor stays diagonal-only so Pz remains PSD).
  Gates: `RowsGroupedByCarryBlock` (sharpened), `SharedArcCovarianceExact`.
- **F3: mixed satellite-bias-frame guard.**  Cycle-level satellite-bias
  cancellation requires one frame per cycle; arcs now carry `bia_corrected`
  through `MwArcZdMean`/`ArcData` and mixed cycles are skipped +
  counted (`skip_mixed_frame`, in the run-end line).  Dormant on the
  self-contained path; guards the BIA era.  Gate: `MixedBiasFrameSkipped`.
- **F4 (dormant-path, OPEN): pivot-switch mid-arc in the SD reconstruction** —
  if the per-station MW pivot switches while an SD arc runs, the Welford mean
  mixes references; verify the S-transform re-reference covers the stats used
  by `arcZdMean` before any BIA-era use.  Tracked for the battery.

## 6c. Fable verification run (Jul-15 eve) — §7.1 PASS, §7.5 FAIL: F5 applied, F6 OPEN → OPUS

Simple case per user: 50 sta, GPS-only, 30 s, em15, 15 ep, SH-off/IF-off/serial shape
(`_wlver_50sta_15ep_{off,on}.json`), `min_arc_epochs=10` ⇒ rows arm at ep 11.

**PASS:** byte-identity OFF (all CSVs bit-exact vs the pre-card exe) AND ON≡OFF through
ep 10 (surgically inert until eligible).  Telemetry exact: 849 rows / 4797 cycles /
σ≈0.222 cyc (the predicted band).  Unit battery 15/15.

**FAIL (§7.5): the folded WL target is WRONG; D5 correctly contains it.**
- **F5 (FOUND + FIXED same-day): correction-frame re-expression missing.**  The kernel
  solves CORRECTIONS about x_lin (no KF write-back); MW̄ is ABSOLUTE.  Un-subtracted,
  the target displaced the per-band DDs by med 131–156 cyc at arming (≈ C·x_lin).
  Fix in tree: `ArcData::{x_lin1,x_lin2}` via a `lin_of` map in the resolver;
  `z_corr = Σc·MW̄/λ_WL − Σ[(c/λ1)x_lin1 − (c/λ2)x_lin2]`; unit gate
  `LinearizationFrameSubtraction` (exact hand value).
- **F6 (OPEN): residual ~50-cycle target error after F5.**  Arming displacement
  med 49–57 cyc decaying to ~6 by ep 15; D5 1628 (vs OFF 369); KF-WL circR
  0.11–0.23 vs OFF 0.18–0.31 — the constraint still pulls AWAY from integers.

**F6 diagnostic plan (build a one-shot log-only dump, then attribute):**
At the first arming epoch, for the first ~8 WL rows log per-arc
(station, sat, sig1, sig2, c, MW̄, x_lin1, x_lin2) + row aggregates
z_MW = Σc·MW̄/λ_WL, z_lin = Σ[(c/λ1)x_lin1 − (c/λ2)x_lin2], z_corr = z_MW − z_lin,
AND the pre-solve reconstructed row value from the previous posterior
(netar-style: Σ[(c/λ1)(x_lin1 + x_r(col1)) − (c/λ2)(x_lin2 + x_r(col2))]) plus the
kernel's own `ddPriorInnovation` on the WL block.  Attribution:
(a) reconstructed_abs ≈ z_MW but z_corr ≠ posterior_corr ⇒ convention bug (sign of
    the subtraction, or the fold's innovation direction) — check first, cheapest;
(b) reconstructed_abs − z_MW ≈ the ~50 cyc ⇒ a lumped term does NOT cancel in the
    state frame — group the discrepancy by station/sat/signal-pair; prime suspects:
    (i) x_lin fetch defect in the NEW `lin_of` wiring (verify dumped x_lin values are
        plausible amb seeds — a wrong-index fetch is exactly 10–100 m scale),
    (ii) mixed band-2 channels across stations in one cycle (station A pairs L1C/L2W,
         station B L1C/L2X ⇒ sat's b_L2W−b_L2X does not cancel — test by filtering
         rows to uniform sig2; magnitude argues against 50 cyc but cheap to kill),
    (iii) eq34 tree-pin interaction (cycle rows traverse PINNED tree arcs whose
         per-band pin values are code-seeded reals);
(c) rows with non-±1 coeffs (re-expressed cycles) — dump separately, verify the
    per-vertex balance assumption actually holds on them.
Keep `mw_wl` default-OFF throughout (it is); D5 has contained both defects, but do
not grade anything ON until the dump attributes F6 and a fix passes this same
15-ep A/B (OFF-level D5 + circR lifting ≥ OFF).

## 6d. F6 ATTRIBUTED (Opus, Jul-15 eve) — root cause = stiff per-band carry dominance, NOT a target bug

Built the one-shot dump (`network.mw_wl.f6_dump`, log-only default-OFF →
`logs/network_mw_wl_f6.csv`; wiring in `ApplicationNetwork.cpp` build-time capture +
post-solve reconstruction).  Ran the §6c 15-ep 50-sta ON config; the dump fired at the
first arming epoch (ep 11, tow 259500) — telemetry reproduced §6c EXACTLY (849 rows /
4797 cycles / σ≈0.222, D5 1628).  Dump saved to scratchpad `network_mw_wl_f6_ep11.csv`.

**What the dump proves (rules OUT the §6c suspect tree (a)/(b-i)/(b-ii)):**
- `z_mw` is as-integer-as-MW-gets: frac ±0.08–0.48 across the 8 rows — the honest
  MW code-noise band at n=10 (matches the `mw_census.py` circR≈0.6, NOT tighter).  The
  measured WL target is FINE.  **Not a z_MW / cycle-closure / arcZdMean bug.**
- `z_corr == zhat_block` to 4 dp on every row — the layer arithmetic (F5 subtraction,
  λ scaling, signs) is exact.  **Not a convention (a) or lin-frame (b-i) bug.**
- **The real defect (b-iii, sharpened): in EVERY cycle exactly ONE arc — the eq34 CHORD —
  reconstructs to a non-zero WL; all tree-path arcs reconstruct to `x_abs≈0` (they are
  the datum tree pins).**  So `recon_abs` (the estimator's KF WL over the cycle) = the
  chord's CODE-SEEDED FLOAT WL, and the WL fold leaves it there: on the clean rows the
  innovation ≈ `recon_abs − z_mw` (row0 4.10/4.25, row2 6.53/6.70, row3 23.5/23.3,
  row7 35.9/36.1).  **The WL constraint does not move the WL toward `z_mw`.**

**Mechanism (why the fold is overridden, not why the target is wrong).**  The carry
runs a SEPARATE per-(const,signal) block, so `∇ΔN₁` (L1 group) and `∇ΔN₂` (L2 group) are
EACH time-linked stiffly (q_z=1e-4 ⇒ a tight zero-q carry).  The chord's L1 column is held
by the L1 carry and its L2 column by the L2 carry, each to σ≈0.06–0.09 cyc.  Two
independent per-band DDs pin `WL = ∇ΔN₁−∇ΔN₂` to ≈0.09 cyc — TIGHTER than the WL pseudo-obs
(σ≈0.22 cyc) — but to the WRONG (code-seeded) value.  The appended WL block is a *separate*
post-reduce Woodbury fold sharing those columns, so per §4 it drops the cross term and,
being ~3× looser, cannot overturn the carries.  Its only residual effect is conflicting
pressure that trips the D5 innovation gate on the carries → re-tap churn (369→1628) → the
slow, noisy 50→6 cyc "decay" (D5-mediated absorption, not clean tightening), and the
dd-frac circR drifts the wrong way.

**Consequence for the fix — the §4 joint block ALONE will NOT cure it.**  Folding
carry-L1 + carry-L2 + WL jointly still lets the two σ≈0.06 per-band carries over-determine
the WL direction to 0.09 cyc; a 0.22-cyc measurement stays outvoted.  The cure must stop
the per-band carries from stiffly propagating the *garbage* WL (Standing Invariant #1: a
stiff time-link past a correction warps the solution — here the "correction" is the MW WL
the carry never received).  Candidate directions:
  (A) **MW-seed the WL at chord birth** — when a chord is born, split the MW WL into the
      L1/L2 carried seeds so `∇ΔN₁−∇ΔN₂` starts MW-consistent, then the stiff carry HOLDS
      the right WL (uses MW to seed, not to fight).  Requires cross-group coordination at
      birth (the carry groups are independent today).  *Most aligned with SPEC §1 + Inv #1.*
  (B) **Anisotropic carry** — carry the (NL, WL) rotation per chord and give the WL
      direction a loose q so the MW pseudo-obs owns it (bigger: cross-band carry coupling).
  (C) **WL-informed re-tap** — treat a confident MW-vs-carried-WL disagreement as a WL
      slip and re-tap the chord's carry from the MW WL (reuses D5 plumbing; a cleaner,
      gated version of the churn that already happens).

**Secondary (separate, latent) finding:** station index 1 carries pathological ambiguity
LINEARIZATION points (`x_lin≈256540 m`≈256 km, uniform across G01/G10/G15/G16/G27).  They
cancel in the invariant DD (so the solve survives) but make the WL row's `z_lin` a
difference of two ~3e5 values and render its correction-frame innovation ill-conditioned
for station-1 cycles (row 5: innov 17.3 vs true gap 1.0).  Not the F6 cause, but worth a
look — a 256 km ambiguity seed points at a bad a priori for that station.

## 7. Fable's next steps (SPEC §7, in order)

1. Byte-identity OFF (same-build).  2. FIREWALL unit (fold a WL block; `assertNullSpaceRank` unchanged).
3. Extend the unit battery — notably the `MwSlipDetector::arcZdMean` ZD reconstruction (feed crafted
observations through `detectLargeMwResets`, assert SD+pivot re-add == raw ZD; §3 above).  4. Falsifier
FIRST (10-ep smoke, 50 sta).  5. Rung runs (KF-WL circR vs `mw_census.py`; `neq_export_epoch` +
`neq_svd.py`).  6. Graded 3 h @30 s (305 sta, em10) vs the R5 em10d8 control.  7. Phase B only after A
grades clean.
