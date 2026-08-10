# SPEC — PD-safe invariant-DD fold (the corrupted-carry fix)

**Status**: **BUILT, unit-gated, and FIELD-VALIDATED on the §7 `pdfix_on` 3 h leg
(2026-08-03).** INC-0/1/2 landed under `network.carry_invariant_pd_safe` (default
OFF, fingerprinted); §6's tests are `tests/test_pd_safe_fold.cpp`
(`PdSafeFoldTest`, 6 cases). **THE gate (§7.1 snapshot PSD audit) PASSES: 0 of 782
carried rows failing vs 686 (87.7 %) on the same-lineage control; mature DD steps
1.015/0.883/0.509 cyc/ep vs 2.749/2.326/1.509.** **§7's `pdfix_off` P0 arm also
PASSES at 3 h**: products md5 `697a4964…` identical to `runs/churn_base2`
(pre-edit exe), snapshot identical but its write timestamp, fingerprint hash
unchanged — invariant #6 holds at full scale, and the OFF arm's own INC-0 puts
the defect at **85.24 % of carried row-epochs inert over 349 of 360 epochs**.
Three costs to read before arming: total_IF hr2 moved +12.4 % (no clock-QC
degradation), §7.4's "flags will rise" prediction INVERTED (−45 %), and pd_safe
costs **+10.0 %** wall on the same-build comparison. See §10.
**§11 is a follow-on that landed the same day**: the σ̂₀ CLAMP VISIBILITY fix, which
turns §10's finding-2 (the D5 calibration silently ran as v1 for a campaign) from an
anecdote into an instrument — field-proven on the corrupt substrate at **339 of 360
installs floored (94.7 %)**, solve byte-identical. Solve-path edit ⇒ full
invariant-#6 discipline (default-OFF knob, structural OFF byte-identity,
same-build A/B).
**Memory context**: `project_dd_spillway_rootcause` (the Aug-2 night diagnosis
this card fixes), `project_chord_churn_d5_limit_cycle` (the symptom layer),
`project_lane_carry_card` / `project_lane_fold_perf_lf1` (the joint pair-block
shape that is the structural hazard).
**Regime for all arms**: 63 sta, DOY-129, GPS, 30 s, cold, the
`_ft63_churn_base.json` lineage — the configs and reference runs of the churn
card, so every number here is comparable to `runs/churn_*` and `runs/qz0*`.

---

## 1. The defect (measured, not inferred — do not re-derive)

The invariant-DD carry is a per-epoch cycle: project the carried posterior
into this epoch's chord basis (`buildDdPriors`: `Pᶻ = W·Pᶻ_old·Wᵀ` + fresh
diagonal `fresh_prior_var` = 1e6), fold it against the epoch data
(`foldReducedDdPriors`: `M = (Pᶻ+B)⁻¹`, Woodbury onto the reduced shared
system), read the posterior back and install it
(`installPosterior` ← `ddPriorPosteriorCovariance`:
`Pᶻ⁺ = Pᶻ − PᶻMPᶻ + (PᶻMGᵀ)Σ(GMPᶻ)`, NetworkNormalEquations.cpp:975).

Three facts, proven from snapshot CARRY sections (2026-08-02):

1. **The posterior subtraction is catastrophically cancelling on the giant
   joint pair block.** The [band-1; band-2] lane shape is born near-singular
   (same-chord band rows are near-duplicates through shared geometry: min eig
   4.6e-6 at ep0001 vs 1.8e-2 for per-band blocks). Mixed scales (1e-4 mature
   .. 1e6 fresh) + K=686 GEMM chains + `W·P·Wᵀ` integer re-expression push the
   hidden tight eigenvalues through zero between ep 2 and ep 122.
2. **Once indefinite, the fold's PD test trips and the block is silently
   frozen forever.** NetworkNormalEquations.cpp:884 (serial) / :734 (parallel):
   `ldlt.info() != Success || vectorD().minCoeff() <= 0.0  ⇒  M := 0` — no
   counter, no log, no event. Posterior = prior; B (1-10 m²) can never cover a
   −8e8 eigenvalue; the projection churns the indefinite matrix to negative
   diagonals (−2.9e7 m²) and super-fresh entries (+4.7e6 > 1e6).
3. **Blast radius**: 568/686 rows diffuse at 3 h = 78 % of the carried state,
   in the control and every arm of this lineage. The healthy small blocks
   (90 + 6 rows) accumulate to σ = 2.7 cm = 0.14 L1 cyc over 3 h through the
   SAME code — the architecture is correct; the arithmetic is not.

Downstream silences that kept this invisible (all get counters in INC-0):
the D5 gate's `if (!(var > 0.0)) continue` row skip
(NetworkInvariantCarryDriver.cpp, d5GateHalf/d5GateJoint), the v5 born-skip
absorbing the frozen-diffuse standing set (~448 rows/ep vs 21.5 real
births/ep), and the carry-blind covariance readback (companion card, §8).

## 2. Design principle

PD of the carried covariance is an ALGEBRAIC INVARIANT of the exact recursion
(independent chords ⇒ full-row-rank W ⇒ `W·PSD·Wᵀ` + fresh diagonal is PD; the
exact posterior of a PD prior is PD). It breaks only numerically. Therefore:

- make the arithmetic scale-following so violations shrink to round-off dust
  (INC-1a/1b);
- repair dust when it appears, and COUNT every repair (INC-1c);
- when repair is impossible, fail LOUDLY with a bounded, explicit reset —
  never a silent permanent decoupling (INC-2; standing invariant #7 demands
  both edges);
- and regardless of the knob, make the existing failure path visible (INC-0).

## 3. INC-0 — observability (always-on, solve-neutral; lands FIRST)

New driver/NEQ counters, surfaced per the existing counter pattern
(run-end line in `filter/NetworkTeardown`, per-epoch columns APPENDED to
`network_epoch_status.csv` — readers read by name):

| counter | site | meaning |
|---|---|---|
| `ddfold_degen_blocks` / `_rows` | both M:=0 sites (NNE :884, :734) | blocks/rows that folded inertly this epoch |
| `ddfold_min_pivot_rel` | same | worst `dmin/dmax` of the epoch (epoch_status column) |
| `d5_nonpos_var_rows` | d5GateHalf/Joint var≤0 skip | rows the gate could not test |

Logging: WARN on each block's healthy→degenerate TRANSITION (block index, K,
group, min pivot) and on recovery — not per epoch (a 360-epoch spam teaches
readers to ignore it). Run-end summary always.

Gate: solve byte-identity is structural (counters touch no math). Trace files
gain columns/lines — document in the compare-tool notes; the solve artifacts
(`network_solution.csv`, products, snapshot solve content) must be
md5-identical on `_ft63_churn_base.json` vs the pre-edit exe.

## 4. INC-1 — the numerical fix (knob `network.carry_invariant_pd_safe`, default OFF)

Everything in this section is under the knob — the product form is
algebraically identical but rounds differently, so the OFF path must stay
byte-identical (invariant #6). Loader: warn-if-set-without-`carry_invariant`
in the CROSS-KNOB VALIDATION PASS, not the parse block (the churn card's
loader-ordering trap). POSTURE ledger entry; fingerprinted (fresh convergence
only — never flip mid-chain; snapshots from OFF runs are additionally handled
by the restore guard, INC-2c).

**(a) Product-form posterior** — remove the cancellation at its source.
Identity: `Pᶻ − PᶻMPᶻ = PᶻM[(Pᶻ+B) − Pᶻ] = PᶻMB` exactly. Rewrite
`ddPriorPosteriorCovariance` (NNE :975) as

```
Pᶻ⁺ = sym(PᶻMB) + (PᶻMGᵀ)·Σ·(PᶻMGᵀ)ᵀ,   sym(X) = (X + Xᵀ)/2
```

Products follow the scale of the ANSWER (mature row: 1e-4·(1/B)·B ≈ 1e-4 from
same-scale operands) instead of subtracting 1e6-scale operands to reach 1e-4.
`B` must be available at readback: store it in `FoldedDdPrior` (extra K² per
block, ~3.8 MB at K=686 — bounded; do NOT recompute as `Minv − Pz`, which
re-subtracts).

**(b) Equilibrated, scale-relative fold factorization.** At both fold sites:
`S = diag(1/√max(diag(Pᶻ+B), ε))`, factor `Ã = S(Pᶻ+B)S` (all pivots O(1)),
test `dmin ≥ 1e-12·dmax` (the `requireFactorPd` constant, NNE :79 — replaces
the absolute `minCoeff() ≤ 0`), and unscale exactly: `M = S·Ã⁻¹·S`. Keep the
D5 cache consistent: `fd.Minv` stays `Pᶻ+B` (post-repair if (c) fired).

**(c) Repair backstop — counted, bounded, rare by construction.** If (b)'s
pivot test trips: `SelfAdjointEigenSolver` on `Ã` (precedent: 9 files already
use it), clamp eigenvalues below `1e-12·λmax` up to that floor, refactor,
count `ddfold_repairs` + record the worst clamped magnitude
(`ddfold_repair_worst`). WARN if a clamp ever exceeds 1e-6·λmax — dust is
hygiene, a large repair is a NEW bug and must scream, not be laundered.
Cost: eigh is O(K³), same class as the existing `ldlt.solve(Identity)`; it
runs only on trip, and after (a)+(b) trips should be dust-rate.

## 5. INC-2 — failure policy (both edges, invariant #7)

**(a) Degenerate-block rebirth.** If a block STILL cannot factor after (c)
(reachable in principle via a corrupt restored blob or a genuine new bug):
`M := 0` for this epoch (unchanged solve semantics — the fold stays inert and
index-aligned), the block is flagged, and `installPosterior` REBIRTHS it:
keep the means `ẑ` as seed, reset `Pᶻ` to the fresh diffuse diagonal. Next
epoch the diffuse prior takes the data-only tap-off — a whole-block mass
birth, one bounded epoch of information lost, exactly the semantics chord
births already have. Emit `DD_FOLD_REBIRTH` on `network_events.csv`
(trace_events stream) with block, group, K, min pivot; count
`ddfold_rebirths`. The funnel and slip consumers are NOT touched — a rebirth
is a prior reset, not a slip.

**(b) The decoupling edge is an event too**: the healthy→degenerate WARN of
INC-0 is the event for the M:=0 edge; (a) is the re-coupling edge. No silent
state remains reachable.

**(c) Restore guard.** `restoreCarry` (NetworkInvariantCarryDriver.cpp:186):
after parsing each block, run the (b)-style check; on failure, rebirth that
block at restore time (counter `ddfold_restore_rebirths`, WARN). Every
snapshot currently on disk carries the corrupt giant block — without this, a
warm resume re-imports the disease into a fixed build.

## 6. Tests (new file `tests/test_pd_safe_fold.cpp`; one suite per file)

1. `ProductFormEqualsSubtractionWellConditioned` — (a) ≡ legacy to 1e-9 rel on
   a benign block.
2. `MixedScaleLanePairStaysPd` — THE falsifier pair: synthetic 40-row block,
   fresh 1e6 + mature 1e-4 + near-duplicate row pairs (corr 1−1e-9), 500
   build→fold→install cycles with noisy data. Assert the LEGACY path goes
   indefinite (reproduces the disease — if this ever stops failing, the toy
   no longer models the field and must be re-derived) and the pd_safe path
   holds min-eig ≥ 0 with posteriors matching a long-double reference.
3. `EquilibratedInverseMatchesPlain` — (b) ≡ plain inverse to tol on a
   well-conditioned matrix; pivot test scale-invariance under Pᶻ → cPᶻ.
4. `DegenerateBlockRebirthsLoudly` — hand-built indefinite Pᶻ: fold is inert,
   counters/event fire, install rebirths, NEXT epoch's posterior equals the
   data-only tap-off (the GATE-10c pattern).
5. `RestoreGuardRebirthsCorruptBlob` — serialize an indefinite block, restore
   with the knob: rebirth + counter; without: legacy byte-path intact.
6. OFF-path structural byte-identity is asserted the usual way (knob absent ≡
   false ≡ pre-edit expressions; EXPECT_EQ against verbatim legacy forms where
   practical, per the nl_iono_free_row precedent).

## 7. Arms and grading (Release, renamed exe, smoke-first — standing cadence)

| arm | config | purpose |
|---|---|---|
| `pdfix_off` | `_ft63_churn_base.json` on the NEW exe | invariant-#6 gate: solve artifacts md5-identical to `runs/churn_base2` |
| `pdfix_on` | + `carry_invariant_pd_safe: true`, 3 h | the fix arm |
| `pdfix_qz0` / `pdfix_qz0_iofloat` | the Aug-2 arms + knob | re-ask the q_z / iono-float questions ON A WORKING CARRY |

Grading (tools exist):
1. **Snapshot PSD audit** — promote the session parser to
   `scripts/parse_carry_psd.py` (deliverable of this card): every block PD at
   ep0001/1h/2h/3h; diffuse rows ≈ current-epoch births only. THE gate.
2. `scripts/grade_dd_spillway.py` — mature per-key steps and the structure
   function. Success = the giant block's keys leave the 2 cyc/ep pin;
   prediction: gain-limited ~0.5-1.5 cyc/ep at qz=1e-4, < 0.3 and flattening
   SF at qz0. `med|frac|` movement is NOT promised at 3 h (see §1: block-0
   needed hours to reach 0.14 cyc; the iono physics still sets the pace).
3. `scripts/chain_stability.py` total_IF hr2 vs `churn_v5b` — the no-harm
   check, with eyes open: 78 % of the carry starts WORKING, so products may
   move well beyond the ±1 % churn-card band in either direction. A move is
   not an auto-fail; a clock-QC degradation is. Grade direction + magnitude +
   clock QC and treat any large move as its own investigation before arming
   anywhere else.
4. D5 behavior: flags will RISE (stiff priors now actually resist the iono
   walk — the tension becomes visible for the first time). Watch σ̂₀
   (2.5→? ), refloat rates, and whether the v3k erase/keep split and the
   funnel keep invariant #1 satisfied at the new operating point. A flag
   storm here is SIGNAL, not regression — it is the previously-hidden war.
5. Perf: `fold_minv_ms` / install timers delta < 25 % at 63 sta; repairs
   dust-rate (`ddfold_repairs` ≈ 0 steady-state).

## 8. Explicitly out of scope (companion / follow-on cards)

- **Honest covariance readback**: `jointConditionalCovariance` /
  `reconstructStationCovariance*` carry no dd-prior term (only the mean κ
  path exists, NNE :1111 vs :1218) — `dd_sigma_cyc` and the AR probe stay
  carry-blind until the rank-K correction card lands. This card's grading
  deliberately uses means-based statistics and the snapshot audit instead.
- Group-id → signal mapping in the snapshot + why no traced key rode block 0
  quiet (tracer pool vs carried-component coverage) — first validation item
  once counters exist.
- Whether pre-lane per-band giant blocks also corrupt (the golden wl1off
  lineage showed the same dd_sigma pin; no golden snapshot survives to check
  — answered for free by INC-0 counters on any future per-band run).
- Re-measure the POSTURE ledger's "qz 1e-4 beats zero-q" evidence line — it
  was graded on the corrupt substrate.
- The 305-sta tier: repair cost scales with the same K³ as the existing fold;
  wherever the dense fold is feasible the fix is feasible — but the giant-K
  growth at 305 is ALREADY the INC-C2 impossibility's territory; nothing here
  changes that boundary.

## 9. Why this is the right order

INC-0 alone would have converted three weeks of misattributed campaigns into
a one-line WARN on day one. It is solve-neutral, lands first, and stays on
forever. The numerics (INC-1) are the actual cure; the policy (INC-2) makes
the residual failure mode bounded and loud; the arms (§7) then re-open every
question this lineage graded on the corrupt substrate — churn, q_z posture,
tie strength, iono-float — on a carry that finally does what the algebra says.

## 10. Implementation record (2026-08-03)

### What landed

| increment | where |
|---|---|
| INC-0 counters + per-block PD reads | `NetworkNormalEquations::DdFoldStats`, `FoldedDdPrior::{degenerate,min_pivot_rel,repaired,repair_worst}`, `ddPriorBlockDegenerate/MinPivotRel` |
| INC-0 seam readout, transitions, events | `filter/NetworkInstallCarry.cpp` (reads the ACCEPTED solve's fold); run-end block in `filter/NetworkTeardown.cpp` |
| INC-0 trace columns | `ddfold_degen_blocks,ddfold_degen_rows,ddfold_min_pivot_rel` appended to `network_epoch_status.csv`; `DD_FOLD_*` rows on `network_events.csv` |
| INC-1a product form | `ddPriorPosteriorCovariance` — `sym(PᶻMB)`, with `B` cached in `FoldedDdPrior` under the knob only |
| INC-1b equilibration | `ddMiddleInversePdSafe` (both fold sites); the legacy body extracted verbatim into `inverseFromLdlt` / `ddMiddleInverseLegacy` |
| INC-1c repair | eigen-clamp in `ddMiddleInversePdSafe` |
| INC-2a rebirth | `NetworkInvariantCarryDriver::installPosterior` (all four block shapes), `Counters::ddfold_rebirth*`, `lastFoldRebirths()` |
| INC-2c restore guard | `carriedBlockIsPd` + the per-block check in `restoreCarry`; `resumeCarryRebirthBlocks()`; WARN at the resume site in `NetworkRunContext.cpp` |

### Two readings BEYOND this SPEC's literal text — both deliberate, both recorded

1. **A repair clamp above `1e-6·λmax` is REFUSED, not applied.** §4(c) requires a
   large clamp to "scream, not be laundered"; §5(a) names "a corrupt restored
   blob" as a reachable trigger for the rebirth. As literally written the
   eigen-clamp ALWAYS succeeds — a −2.9e7 eigenvalue is silently rewritten to
   `+1e-12·λmax` — so §4(c)'s prohibition has no mechanism and §5(a)'s trigger is
   unreachable by construction. Refusing an outsized clamp makes both true: dust
   is repaired and counted, and a genuinely broken block goes to the bounded,
   loud, counted rebirth instead of being made to look healthy.
2. **INC-0's counters and its three epoch-status columns are UNCONDITIONAL** (the
   redundancy / prefit-census precedent, not the measurement-admission one),
   because INC-0 is an always-on instrument rather than an armed card. So an OFF
   run's epoch-status SCHEMA changes while its SOLVE artifacts stay byte-identical
   — grade the solve artifacts, not the traces (§3 already says so).

### What the build measured

**INC-0 re-dated the defect.** The snapshot audit could only bracket "indefinite
between ep 2 and ep 122". The live counter is exact: on a **10-epoch cold** 63-sta
smoke the giant block (group 3, K=352, lead chord 1→1000013 / JPLM00USA) goes
DEGENERATE at **epoch 9**, min pivot `−2.914e-10`. The freeze is an IGNITION-phase
event, not a slow drift.

Smoke A/B, Release, 300 s, `_ft63_pdfix_{off,on}_smoke.json`:

| | OFF | ON |
|---|---|---|
| inert row-epochs | 1406 = **16.10 %** of the carried dimension | **0** |
| worst relative pivot | −1.078e-09 | +4.035e-08 |
| D5 rows the gate could not test | **666** | **0** |
| repairs / rebirths | — | 0 / 0 |

The **arithmetic alone** — INC-1a + INC-1b — closed it. INC-1c and INC-2a never
fired, exactly as §4(c) predicted ("after (a)+(b) trips should be dust-rate"). The
corollary matters: **both backstops are UNEXERCISED in the field**, and only the
unit gates cover them.

**Invariant #6 passed at smoke scale.** `runs/pdfix_off_smoke/products.txt` md5
`72a680d1e8afbcfbd52b04b43f1e18b3` is identical to the pre-edit exe's
`runs/churn_base_smoke/products.txt`; the snapshot matches 1703/1703 lines except
the ISO write timestamp; the fingerprint hash `7eb708fc76da6b80` is unchanged, so
OFF runs still resume off pre-card snapshots.

`scripts/parse_carry_psd.py` reproduces §1's diagnosis independently from
`runs/churn_base2`: normalized min-eig **3.43e-07** on the giant block at ep0001
against **1.37e-02** for the per-band block (§1's 4.6e-6 vs 1.8e-2), 304
non-positive variances by ep0122, and at 3 h **776 of 782 carried rows (99.2 %)**
in blocks that fail the gate, 65.3 % of rows standing diffuse.

### Adversarial review (2026-08-03, 4 dimensions x refutation panel)

22 findings raised, 20 refuted, **2 CONFIRMED** — both in this card's own INC-0
plumbing, both solve-neutral, both fixed before this record was written. They are
recorded because both were the same failure this card exists to end (an
instrument that reads clean while lying), and because the fix removed a whole
class of them:

1. **`have_ddfold` in `networkSolveGlue` was dead wiring.** The per-epoch values
   were parked in a `ctx.carry.ddfold_ep_tow` stamp written by the INSTALL seam —
   which runs AFTER the glue and only on an ACCEPTED epoch. So the branch meant to
   fill the columns on a WITHHELD row was unreachable by construction, and every
   withheld epoch blanked exactly the columns a reader most wants there (a
   degenerate carry block is a prime suspect for a withhold).
2. **The same stamp was written even when NO fold ran**, publishing the
   default-constructed `min_pivot_rel = 1.0` on a chordless epoch — a perfectly
   conditioned factorization that was never computed, and indistinguishable from
   the 1.0 a genuine single-chord block reports.

**Fix**: the stamp is gone. Both writers now read `est.ddFoldStats()` directly and
gate on `blocks > 0`. The kernel already resets it once per epoch in `beginEpoch`
and a within-epoch RAIM re-solve overwrites it, so the read is always the latest
fold and "no fold ran" is representable. Verified on the OFF smoke: the degenerate
epochs now publish `degen_blocks=1, degen_rows=704, min_pivot_rel=-2.914e-10`
where the stamp version published nothing.

The DD-fold's numbers differ from the redundancy/admission censuses precisely
because the kernel HOLDS them — those seams run pre-assembly and genuinely need a
park. Copying their stamp pattern was the mistake.

### §7 field result — `pdfix_on` 3 h leg, 2026-08-03 (OFF arm not re-run; see note)

`runs/pdfix_on`, 360 epochs, Release. Control is `runs/churn_base2` — the SAME
lineage produced by the PRE-EDIT exe, so it is a same-config OFF arm for every
gate below except md5 identity. (§7's `pdfix_off` was skipped by user direction
to get the result faster; the invariant-#6 md5 gate therefore still rests on the
300 s smoke, where it PASSED.)

**Run-end INC-0/1/2, 360 epochs: 0 degenerate blocks, 0 repairs, 0 rebirths, 0
untestable D5 rows, worst relative pivot +1.716e-10.** The arithmetic alone held
the invariant for the whole leg; INC-1c and INC-2a remain field-unexercised.

| gate | ON (`pdfix_on`) | control (`churn_base2`) | verdict |
|---|---|---|---|
| **1. PSD audit (THE gate)** | every block PD: min-eig **6.758e-05** (K=686), 1.359e-01, 9.567e-01; **0/782 rows failing** | **686/782 (87.7 %) failing**, 100 non-positive variances, min diag −3.992e+03 | **PASS** |
| 1b. diffuse ≈ births only | 73 rows vs 65.8 births/ep → ratio **1.11** | 511 vs 105.8 → ratio **4.83** | **PASS** |
| **2. spillway, mature steps** | **1.015 / 0.883 / 0.509** cyc/ep (age 25-50 / 50-100 / 100+) | 2.749 / 2.326 / 1.509 | **PASS** — §7.2 predicted 0.5-1.5 at q_z=1e-4 |
| 2b. structure function, lag 1 | 0.83 cyc | 2.24 cyc | −63 %; still GROWING with lag (drift remains) |
| 2c. `med\|frac\|` | 0.241-0.255 | 0.243-0.254 | unchanged — §7.2 explicitly did not promise movement at 3 h |
| 2d. worst station pair | 3.41 cyc/ep | 7.08 cyc/ep | −52 % |
| 2e. cross-band ratio | 1.270 | 1.286 | still the COMMON-CYCLE signature (1.2833) ⇒ residual is still iono representation |
| **3. total_IF (no-harm)** | hr2 spread_med **8.812 m**, worst_med 22.14, worst_max 23.89, clk_std **5.00**, 0 excursions, 684 disc bumps | 7.837 / 20.75 / 27.64 / 4.96 / 0 / 684 | **MOVED +12.4 %**; no clock-QC degradation ⇒ not a fail per §7.3 |
| 3b. convergence trend | 13.470 → 8.801 m, ratio **0.653** | 10.348 → 7.815, ratio 0.755 | starts WIDER, converges HARDER |
| **4. D5 response** | 18 559 flags, 23 675 births, 20 330 refloats, **σ̂₀ = 4.05×** | 33 628 / 38 105 / 34 760, **σ̂₀ = 1.00×** | flags **−45 %** |
| **5. perf** | **2481.4 ms/epoch** | *(see correction below)* | **+10.0 %** vs the same-build OFF arm — **PASS** (gate ≤25 %) |

**Three findings the card did not predict.**

1. **§7.4's prediction is INVERTED.** It expected D5 flags to RISE ("stiff priors
   now actually resist the iono walk — the tension becomes visible for the first
   time"). They FELL 45 %, and births/refloats with them (−38 % / −42 %). The
   reading that fits: the flags being counted on the corrupt substrate were
   overwhelmingly the born-chord limit cycle of
   `project_chord_churn_d5_limit_cycle` (95.3 % of flags sat on diffuse priors),
   not tension. Killing the freeze removed the chords that were being re-born to
   be re-flagged. The tension §7.4 anticipated may still be there; it is not
   visible above a churn population that shrank by nearly half.
2. **On the corrupt substrate the D5 v2 LS-VCE calibration was INERT.** The
   control's σ̂₀ is exactly 1.00× — the accumulator's seed. Its blob's raw
   σ̂₀² is 0.0099, and the driver clamps σ̂₀² to [1, cap] ("calibration only
   RAISES the threshold"), so the gate ran as the RAW FORMAL v1 test for the
   entire run. A frozen block's innovations are tiny against its diffuse Cov(v),
   which is precisely what drove σ̂₀² below 1. **Every conclusion about D5
   calibration drawn on this lineage was drawn from a gate running in v1 mode.**
   The ON arm lands at σ̂₀ = 4.05×, the direction and order the M2 monitor
   predicted (formal Cov(v) 10-30× optimistic).
3. ~~The fix is 42 % faster~~ — **WITHDRAWN, and the withdrawal is the point.**
   That number compared `pdfix_on` (new exe) against `churn_base2` (PRE-EDIT
   exe), i.e. across builds and machine states. The P0 leg settled it: `pdfix_off`
   runs the byte-IDENTICAL solve at **2255 ms/epoch** while `churn_base2` took
   4287, so 42 % of that gap is not the fix. On the legal same-build comparison
   pd_safe costs **+10.0 %** (2481 vs 2255) — inside the ≤25 % gate, but a COST,
   not a saving. §7's own rule ("no cross-build claim is legal until P0 passes")
   caught this; it was written for exactly this mistake.
   Note the counters are unaffected: `pdfix_off` and `churn_base2` agree exactly
   on flags (33 628), births (38 105), refloats (34 760), σ̂₀ (1.00) and carried
   population (792), so every NON-timing comparison in the table above stands.

**The one thing to investigate before arming anywhere else** (§7.3's own
instruction): total_IF hr2 spread_med moved +12.4 %, far outside the churn card's
±1 % band — as §7.3 warned it might, in either direction. It is NOT a clock-QC
degradation (clk_std flat at 5.00 vs 4.96, zero excursions, zero withholds, disc
bumps identical at 684), so by the card's stated rule it is not a fail. But the
shape is worth reading: the ON arm starts 28 % WIDER at hr0 (13.543 vs 10.600)
and converges harder (ratio 0.653 vs 0.755), i.e. the ignition phase now genuinely
re-earns information instead of freezing whatever it grabbed first. Whether it
crosses below the control given more time is a 6 h question, not a 3 h one.

### Grading-tool correction made during this leg

`parse_carry_psd.py`'s "no entry above the fresh prior" criterion was WRONG and
failed BOTH arms on the first run. A D5 re-tap installs σ̂₀²·Cov(v)_ii, and under
LS-VCE calibration σ̂₀ is both > 1 and TIME-VARYING (15-epoch forgetting
half-life), so entries legitimately exceed the 1e6 prior — the leg wrote 1.593e7
from an epoch where σ̂₀² was 15.93 and closed at 15.85, so no ceiling readable
from the final snapshot is even a valid bound. The check is now an ADVISORY
reported against the blob's own `S,` line; the GATE is PD + a positive finite
diagonal, which is what §7.1 actually specifies. With that corrected the two arms
separate cleanly: 0 % vs 87.7 % of rows failing.

### What is OWED

Everything in §7 above the smoke line. Nothing in this section is field evidence
for the FIX — it is field evidence for the DEFECT plus a demonstration that the
wiring fires and the OFF path is untouched. Specifically owed:

1. `pdfix_off` 3 h — md5 identity against `runs/churn_base2`. STILL OWED: the
   invariant-#6 gate currently rests on the 300 s smoke only.
2. ~~`pdfix_on` 3 h~~ — **DONE**, see above.
3. `pdfix_qz0` / `pdfix_qz0_iofloat` — the §7 re-asks on a working carry. Now
   sharper than when written: q_z posture was graded on a substrate where 78 % of
   the carry could not respond AND the D5 calibration was pinned at its seed.
4. A 6 h leg for the total_IF question — the 3 h trend (ratio 0.653 vs 0.755) is
   suggestive but does not settle whether the +12.4 % hr2 move closes.
5. Only then: the §8 follow-ons and the POSTURE ledger re-measure.

---

## 11. Follow-on — the σ̂₀ CLAMP VISIBILITY defect (2026-08-03, BUILT + FIELD-PROVEN)

§10 finding 2 recorded that the D5 v2 LS-VCE calibration ran **as v1 for a whole
campaign** while the run-end line reported a healthy-looking `σ̂₀ = 1.00×`. That
finding named a bug in an ESTIMATOR. The deeper bug is in the INSTRUMENT, and it is
the same failure this whole card exists to end: **a readout that cannot distinguish
"working" from "silently degraded".**

### The defect

`NetworkInvariantCarryDriver::innovInflationSigma0()` returned `1.0` from FOUR
distinct states and published only the number:

| state | cause | gate actually running |
|---|---|---|
| `Off` | `innov_calibrate` false | raw formal (v1), **by config** |
| `WarmUp` | `n_eff < 200` rows | raw formal (v1) |
| **`Floored`** | raw σ̂₀² **< 1**, clamped UP by `max(f2, 1.0)` | raw formal (v1) — **silently** |
| `Active` | raw σ̂₀² genuinely ≈ 1 | calibrated |

(plus `Capped`, raw σ̂₀² > `kInflationFactorCap`, clamped DOWN — visible in magnitude
but not as a clamp.) The clamp comment says "calibration only RAISES the threshold",
which is a defensible POLICY; nothing published whether it had bound. The raw value
existed only in the snapshot blob's `S,` line, recoverable only by someone who
already suspected the answer.

### What landed

| piece | where |
|---|---|
| `InflationState` enum + `InflationStatus` + `innovInflationStatus()` / `inflationStateName()` | `filter/NetworkInvariantCarryDriver.{hpp,cpp}` — `innovInflationSigma0()` now delegates, arithmetic unchanged |
| per-install state tally (`d5_calib_{epochs,off,warmup,floored,capped,active}_epochs`, `d5_calib_raw_{min,max}`) | `Counters`, tallied at the single `installPosterior` consult site |
| state-TRANSITION log (WARN on `Floored`/`Capped`, INFO otherwise), `NetworkCarryState::d5_calib_state_prev` | `filter/NetworkInstallCarry.cpp`, `app/NetworkRunContext.hpp` |
| run-end state histogram + raw-σ̂₀² range + the floor-clamp WARN | `filter/NetworkTeardown.cpp` |
| GATE 16 `Sigma0FloorClampIsVisibleNotSilent` | `tests/test_network_invariant_carry_driver.cpp` |

Three deliberate calls, each the same shape as INC-0's:

1. **The clamp is UNCHANGED.** Only its visibility. Whether σ̂₀² should be allowed
   below 1 is a separate policy question with a real argument on both sides; this
   card does not decide it, and conflating a policy change with an instrument fix
   would have made the field comparison unreadable.
2. **Always-on, not knob-gated** — an instrument, like INC-0's counters. The
   tallies run even with calibration OFF, so `d5_calib_off_epochs` distinguishes
   "v1 by config" from "v2 that never engaged".
3. **`WarmUp` does not WARN on transition.** It is expected and self-clearing at the
   start of every calibrated run, so warning would put a WARN on epoch 1 of every
   run and train readers to skip the line — the exact failure mode. A run that never
   LEAVES warm-up is caught by the run-end summary instead.

### Field proof — `runs/s0v_retro3h` (= `_ft63_pdfix_off.json`, 3 h, the CORRUPT substrate)

The 300 s smoke does **not** reproduce it (it ends `active` at σ̂₀ 2.87×) — the giant
block only freezes at epoch 9 and the diffuse-prior collapse of σ̂₀² takes ~20 min.
So the 3 h leg is the acceptance test, and it is unambiguous:

```
warmup → active  TOW 259230  raw σ̂₀² = 1.602
active → FLOORED TOW 259800  raw σ̂₀² = 0.9908        <-- epoch 21, the WARN
run-end: σ̂₀ = 1.00× in state 'floored'  (raw σ̂₀² = 0.007139, n_eff = 15004)
         over 360 installs: active 19 / floored 339 / warm-up 2 / capped 0
         RAW σ̂₀² the gate CONSULTED past warm-up: 0.004913 … 8.571
         WARN: floor clamp bound on 339 of 360 (94.7 %) — gate ran as RAW FORMAL
```

**94.7 % of the run.** The old line for this identical run was the single string
`final empirical inflation σ̂₀ = 1.00×`. The transition WARN lands at epoch 21 — 20
minutes into a 3 h leg — which is when a reader would have caught it.

The timeline also corroborates the mechanism §10 inferred: the giant block goes
degenerate at **epoch 9**, σ̂₀ floors at **epoch 21**. A frozen block's innovations
are tiny against its diffuse Cov(v); that is what drove raw σ̂₀² to 0.0049.

### Invariant #6 — PASSES at 3 h on the visibility exe

`runs/s0v_retro3h/products.txt` md5 **`697a4964077ad362fbd942416250d26a`** — identical
to both `runs/pdfix_off` (P0) and `runs/churn_base2` (pre-edit exe). The snapshot
differs in exactly ONE line, the ISO write timestamp; fingerprint `7eb708fc76da6b80`
unchanged. The ddfold counters reproduce to the digit (85.24 % inert, worst pivot
−0.00227049), so the whole run is bit-reproducible. Two 300 s smokes agree
independently: `4ad2ee16…` (qz0 substrate) and `72a680d1…` (off substrate).

### What this changes about the existing record

Nothing in §10 is withdrawn — finding 2 was right. What changes is the SCOPE of the
warning: **any** run on this lineage whose run-end line says σ̂₀ = 1.00× must now be
re-read as "unknown, probably floored" unless it was produced by an exe carrying this
instrument. That includes the whole `churn_*` family and every D5 threshold (`k`)
conclusion drawn against it. The POSTURE ledger entry conditioning `innov_k = 4` on
`innov_calibrate` (`GnssConfigNetworkPosture.cpp`) is graded on exactly that evidence
and is now a re-measure item.

## 12. Follow-on — the σ̂₀ SEED-REASON defect (2026-08-03, BUILT + SOLVE-NEUTRAL)

§11 fixed a readout that could not distinguish *working* from *silently degraded*.
The **resume** line one layer up had the same disease in its purest form: it did not
merely fail to name the cause, it **asserted one nothing had checked**.

### The defect

`NetworkTeardown.cpp` printed, on the `!resumeSigma0Seeded()` branch:

```
sigma0 NOT seeded (v1 blob / calibration off) — the gate runs raw-formal until its
warm-up count is met
```

unconditionally. `resumeSigma0Seeded()` is a bare bool, and **four** distinct
conditions drive it false:

| condition | what the reader should do | old text |
|---|---|---|
| `NotRestored` — no blob restored at all (cold) | nothing; there was no seed to take | **wrong** (both clauses) |
| `CalibrationOff` — `innov_calibrate` false ⇒ no accumulator; any `S,` line ignored | gate is v1 **by config**, not pending warm-up | half right |
| `NoSeedLine` — a blob restored but carried no `S,` line | lineage fact (v1/legacy blob) | the only case described |
| `Rejected` — `S,` line parsed but failed `σ̂₀²>0 && rows>0` (`NetworkInvariantCarryDriver.cpp:245`) | **writer-side defect** — the run that wrote the snapshot serialized a degenerate accumulator | mis-attributed to an old blob |

The trailing clause was wrong too: on `CalibrationOff` the gate is not "raw-formal
until its warm-up count is met" — it is raw-formal *forever*, and no warm-up is
pending because the accumulator does not exist.

**Reproduce** — any cold run on the `_ft63_pdfix_*` lineage, e.g.
`runs/pdfix_qz0_deg15/run.log:1801`, which prints `carry restored NO; sigma0 NOT
seeded (v1 blob / calibration off)` while line 84 of the same log says `D5 v2 LS-VCE
CALIBRATION ON`. Two false clauses on a line whose entire job (invariant #4) is to
make inert wiring visible.

### What landed

| piece | where |
|---|---|
| `ResumeSeedState` enum + `ResumeSeedStatus` + `resumeSigma0Status()` / `resumeSeedStateName()` | `filter/NetworkInvariantCarryDriver.{hpp,cpp}` |
| `saw_seed_line` / `resume_seed_{line_,sq_,rows_}` — the blob's *offer*, recorded next to whether it was taken | `restoreCarry` |
| per-state message + greppable `[tag]` on the RTB-7d resume line | `filter/NetworkTeardown.cpp` |
| GATE `ResumeSigma0SeedReasonIsNamedNotAsserted` | `tests/test_network_invariant_carry_driver.cpp` |

Same three calls as §11's:

1. **The seed arithmetic is UNCHANGED.** `resumeSigma0Status()` is a pure readout over
   state `restoreCarry` already had; it explains the bool and never re-decides it (the
   gate asserts `state == Seeded` ⇔ `resumeSigma0Seeded()` on every arm).
2. **The branch order is the causal chain, most-fundamental first**: with no blob there
   is nothing to seed *from*; with no accumulator nothing to seed *into* (and the gate
   is v1 by config whatever the blob said); only then does blob content decide.
3. **The rejected seed keeps its numbers.** `blob_sigma0_sq` / `blob_rows` stay
   reportable even when ignored — on `CalibrationOff` and `Rejected` the offer the run
   declined *is* the diagnosis.

### Invariant #6 — PASSES

`runs/seedreason_smoke` (`_ft63_pdfix_qz0_smoke.json`, 300 s, output paths redirected,
renamed Release exe `gnss_processor_seedreason.exe`): `products.txt` md5
**`4ad2ee1610557fede239c2535e0995e0`** — identical to the §11 qz0 substrate smoke. The
line now reads:

```
carry restored NO; sigma0 [cold] NOT seeded — COLD: no carry blob was restored this
run, so there was no σ̂₀ to seed from; ...
```

Unit suite 1129 cases / 174 suites, 1128 pass + the one fixture-gated skip.

### Known adjacent gap (NOT fixed here)

`NetworkRunContext.cpp:829` runs `validateCarryBinding` *after* a successful
`restoreCarry`, and on failure calls `carry_driver.reset()` — which does not clear the
resume telemetry. So a run whose blob restored but failed binding still reports `carry
restored YES` on this line while its carry cold-started. Different member, different
edge, and out of this fix's solve-neutral scope; recorded here so it is not re-derived.
