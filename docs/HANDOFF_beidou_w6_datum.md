# HANDOFF — BeiDou W6 datum: gate calibration + phase-anchor completion ("BD-DATUM")

**Status:** Cards AN + DG + PA + H + KP IMPLEMENTED, default-OFF, uncommitted
(for user review).  Implements `docs/SPEC_beidou_w6_datum.md`.  Built 2026-07-22.
KP was CONDITIONAL; its trigger fired on the armed 305 arm's CODE-dominated
residual (§4b), so it is built.  **PA4 milestone MET: the 305 BeiDou W6 storm
config now emits 6/6 processed epochs (§9), closing the original zero-epoch
blocker.**  The KP injection-site deviation (§5) and the ship decision are
reserved to the user; the §6.4 graded emit leg is ON COMMAND ONLY.

---

## 0. The reconciliation this report must open with

Per the spec's §0.1 mandate: **never judge an arm by its raw below-cutoff
count.** The below-cutoff population is four classes with four different owners,
and two of them move in opposite directions under the same change.

| class | measured (per dump, 305 arms) | nature | owner |
|---|---|---|---|
| declared clock gauges (`CLK\|G/E/C`) | 3 | expected, pinned by datum | none (correct) |
| sparse-code confounds (pure-`CB`, `CB+CLK+PB`) | 3.3 control → 0 co-gated | **genuinely null** (ev ≈ 0 relative) — no gate value can clear them | CB-ADM quarantine + phase co-gate (predecessor card) |
| weak-but-real BeiDou families | ≈ 21.5 co-gated (`CB+PB`, ev ~1e-3) | **observable, miscounted** by a single relative cutoff | **Card DG** — gate calibration |
| deep phase orphans (pure-`PB`) | 4.0 co-gated | genuinely null — admitted thin signals with no reachable ambiguity anchor | **Card PA** — drop-and-heal |

This is why control (22.5/dump) → co-gated (33.0/dump) is an **improvement**: it
traded 3.3/dump of genuinely-null code confounds for weak-but-real directions a
calibrated cutoff can clear, and exposed the 4.0/dump phase-orphan class that
was previously hidden inside mixed lumps.  Every number in this report is
re-derivable with `python scripts/characterize_storm.py` (Card AN).

**Units.** The estimability dump is budget-latched at 6 dumps per run
(`spectrum_diag_budget`), so "135 directions" (across dumps) and "22.5
directions" (per dump) are the same measurement.  Every figure below carries its
unit; the predecessor HANDOFF's unit-mixed tables now carry an erratum (§4, H3).

---

## 1. What was built

| File | Change |
|---|---|
| `scripts/characterize_storm.py` | **NEW** — Card AN storm analyzer (CSV-preferred, log fallback, arm-vs-arm diff, `--cross-check`, `--json`) |
| `include/filter/NetworkEstimator.hpp` | `setRankGateTol` / `rankGateTol` + `rank_gate_tol_` member; `EstimabilityDump` / `EstimabilityDirection` result types; `dumpEstimabilitySpectrum` returns the dump; `kNullSpaceTol` re-documented as the DEFAULT |
| `src/core/filter/NetworkEstimator.cpp` | dump reads the RESOLVED tolerance and fills the machine-readable mirror from the same locals |
| `include/filter/NetworkSbasis.hpp` / `.cpp` | `assertNullSpaceRank` gains an optional `int* observed_out` (written before the equality test, so the count survives the throw); **KP** `buildCodeBiasKaulaPriors` static |
| `include/filter/NetworkPhaseDatum.hpp` / `.cpp` | heal reads `est.rankGateTol()`; new `PhaseHealEvent` + optional `healed_out` attribution channel |
| `include/config/GnssConfig.hpp` | `network_rank_gate_tol_` member + getter (DG); `network_cb_kaula_prior_` / `network_cb_kaula_sigma_m_` + getters (KP); both with binding placement rules |
| `src/core/config/GnssConfigLoadNetwork.cpp` | DG parse + validate (> 0, NaN-safe) + clamp at 1e-16 + armed log; KP parse + σ>0 validate + armed/inert log |
| `include/app/NetworkRunContext.hpp` | DG6 counters (`n_gate_evaluated`, `n_gate_withheld_under/over`, `nullspace_obs_min/max`); PA2 `n_heal_attempt_*`; H2 `n_cbadm_phase_orphan_key_epochs`; KP `n_kp_epochs/cols` + `kp_margin_logged` |
| `include/logging/NetworkRunTraces.hpp` / `.cpp` | `network_estimability.csv` stream + `EPOCH` / `DIR` / `RUN` row writers (RUN carries DG6 + PA2 + H2 counts) |
| `src/core/app/ApplicationNetwork.cpp` | `est.setRankGateTol(...)`; both gate call sites use the resolved value + observed out-param; CSV rows; DG6 counters + armed-only run-end line; PA2 `phase_heal` events + attempt counters + run-end line; H2 orphan count; **KP** prior fold + K2 margin log + run-end line |
| `src/core/app/ApplicationNetworkAssembly.cpp` | H1 stale-comment fix; H2 orphan-window counter at the phase registration loop |
| `tests/test_network_rank_gate.cpp` | **NEW** — 14 additive tests (DG + PA + H2-transform + KP) |
| `CMakeLists.txt` | + the new test file (explicit list) |
| `config/test_suite/_maxnet_305_gpsgalbds_d15_5ep_datum.json` | **NEW** — DG + PA armed |
| `config/test_suite/_maxnet_305_gpsgalbds_d15_5ep_datum_dgonly.json` | **NEW** — DG isolated (deviation, §6) |
| `config/test_suite/_maxnet_305_gpsgalbds_d15_5ep_datum_kp.json` | **NEW** — DG + PA + KP armed |
| `docs/HANDOFF_cb_admission_card.md` | H3 units erratum |
| `docs/network_decomposition_addendum_2026-07-16.md` | enumerated-artifact-delta table (the estim arm's new CSV) |

**Commit-time note (SPEC §7):** `ApplicationNetwork.cpp`,
`ApplicationNetworkAssembly.cpp`, `GnssConfig.hpp`, `GnssConfigLoadNetwork.cpp`,
`NetworkRunContext.hpp`, `NetworkSbasis.*`, `NetworkEstimator.*`,
`NetworkPhaseDatum.*` and `NetworkRunTraces.*` also carry hunks from concurrent
uncommitted sibling cards (slip-QC, invariant-carry, Iono-2, decomposition).
BD-DATUM's hunks are the ones described above; commit with hunk-level staging, do
not sweep the whole file.

### New config keys (section `network`)

```
rank_gate_tol    : double, default 1e-9   (== NetworkEstimator::kNullSpaceTol)   [DG]
cb_kaula_prior   : bool,   default false                                          [KP]
cb_kaula_sigma_m : double, default 10.0   (≈ 33 ns; INERT unless cb_kaula_prior)  [KP]
```

Validated `> 0` (NaN lands here too, deliberately: a NaN cutoff makes every
`ev < cutoff` false and would report a rank-0 null space on a healthy system);
clamped up at `1e-16` with a warning, because a cutoff below double's relative
resolution against λmax counts NOTHING as null — an OFF switch wearing a tuning
value's clothes.

---

## 2. Card DG — the mechanism, and why it is a calibration parameter

The gate asserts that the number of eigenvalues below `tol·max(λmax,1)` **equals
exactly** the declared S-basis dimension, and throws in **both** directions.
"Null" is therefore a *placement decision in a spectral gap*, and the gap moves
with the constellation mix.  It has moved once already: the GPS+Galileo era
needed 1e-6 → 1e-9 because 1e-6 sat above the weak-but-real directions.  Adding
BeiDou repeats it exactly.

**DG2 — one resolved value, three consumers.**  The gate, the estimability dump
and the prioritized drop-and-heal now all read `NetworkEstimator::rankGateTol()`,
set once next to the other estimator knobs.  This is load-bearing, not tidiness:
under a shared compile constant an *armed* gate would leave the heal scanning at
the old cutoff, soft-anchoring (σ = 1 m prior) the weak-but-real families the
calibration had just declared REAL.  That is precisely the near-cutoff fragility
that ordered gate-calibration ahead of the Kaula prior (spec §0.3).
`tests/test_network_rank_gate.cpp::HealFollowsTheResolvedToleranceNotTheDefault`
pins it.

**The class boundary the card rests on.**  A structurally absent column has
eigenvalue *exactly* 0 and is therefore below **any** positive cutoff.  No
calibration can clear a genuine null — "calibrate until it emits" is not an
available shortcut.  Encoded as
`GenuinelyNullDirectionSurvivesEveryTolerance` (gate throws at 1e-9, 1e-12,
1e-18 **and** 1e-30; the heal resolves it at all four).

**DG3 placement, measured (co-gated 305 arm, `maxnet305_cogate3`):**

| quantity | measured | margin vs the armed cutoff |
|---|---|---|
| λmax | 6.022e+06 | — |
| gauge depth (max \|ev\| over pure-`CLK` directions) | 1.884e-08 | cutoff is **2.5 decades above** |
| weak-real floor (min ev in the ≥ 1e-3 band) | 1.034e-03 | cutoff is **2.2 decades below** |
| **armed `rank_gate_tol` = 1e-12 ⇒ cutoff** | **6.022e-06** | both margins ≥ 2 decades ✔ |

The analyzer's own geometric mid-gap proposal from the same spectrum is
`rank_gate_tol 7.33e-13` (cutoff 4.414e-06) — the same decade as the spec's
1e-12, which is therefore **confirmed against a measured dump**, not guessed.

**Predicted DG falsifier outcome (from the analyzer, before the run).**  At that
cutoff, of the 33.0 below-cutoff directions per dump, **8.0 per dump would
remain**: 3.0 pure-`CLK` (the declared gauges), 4.0 pure-`PB` (Card PA's), 0.7
`CB+CLK`, 0.3 `CLK+PB`.  So **DG alone clears the weak-real layer but still
withholds** — exactly what the spec anticipates ("DG's falsifier is the census
shift, not emission"), and PA owns the remainder.

---

## 3. Gate results

### 3.1 Unit + suite (Debug)

- **14 new `NetworkRankGateTest` cases green**: default-is-the-constant (incl.
  survival across `beginEpoch`/`reset`), both-direction throw, armed
  reclassification, observed-count on both paths, dump↔gate cutoff+count
  agreement at three tolerances, genuine-null-at-every-tolerance, heal follows
  the resolved tolerance, attribution channel is observational, config default,
  config validate/clamp, the H2 phase→code companion-key transform, and KP's
  prior construction / lift / K2 margin.
- **Full suite 879 / 878 passed / 1 skipped — ZERO regressions.**  Additive count
  is **+14 against the PRE-CARD TREE STATE** (865/864/1), not against the last
  commit: the tree carries several uncommitted sibling cards, so a
  commit-relative count would be inflated (the discipline `SPEC_inc8b` R15 sets).

### 3.2 OFF byte-identity — 10-arm golden matrix (Release)

Run THREE times across binary revisions (initial DG/PA/H build; + PA2 attempt
counters + widened CSV; + KP + corrected K2 placement).  IDENTICAL verdict all
three.

| arm | verdict | attribution |
|---|---|---|
| alloff, allon, jw, wlinj, snap, obs | **PASS byte-identical** | — |
| `estim` | 0 content diffs, **+1 artifact** | **BD-DATUM DG5**: `logs/network_estimability.csv` — exactly the delta enumerated in the decomposition addendum.  The WARN text is unchanged. |
| `consumer` | 1 content diff | **NOT BD-DATUM** — the CB-ADM predecessor's uncommitted W6 log-text fix ("GPS+Galileo" → "GPS + Galileo + BeiDou", `GnssConfigLoadNetwork.cpp`) |
| `cpllive`, `cpljw` | 1 content diff each | **NOT BD-DATUM** — the concurrent slip-QC card's unconditional `Network[MW detect] … detected (carry cold; diagnostic)` line in `src/core/slip/NetworkSlipQc.cpp`, a file BD-DATUM never touches |

**Zero content diffs attributable to BD-DATUM across all ten arms, on the final
KP binary.**  The two non-BD-DATUM diffs were bisected to their originating
uncommitted sibling cards by diffing the exact log line against
`git show HEAD:<file>`; the goldens predate both.  Every new toggle (DG, KP) is
OFF in every `_dcmp_` arm, so the whole calibration/prior surface is exercised
only on the OFF path here.

The `estim` arm's CSV is well-formed (now 17 fields per row after the DG6/PA2
widening: 6 `EPOCH`, 18 `DIR`, 1 `RUN`) and its `RUN` record proves the DG6/PA2
plumbing is live on a *healthy* config — `rank_gate_tol=1e-09, 11 epochs
evaluated, 11 emitted, 0 withheld-under, 0 withheld-over, obs_min=1 obs_max=1,
heal_attempt_epochs=0, cbadm_phase_orphan_key_epochs=0` against `declared_dim=1`.
Standing invariant #4 satisfied: the counters exist and move.

### 3.3 Card AN cross-check (A4)

`python scripts/characterize_storm.py <archived control log> --cross-check`
re-derives the recorded CB-ADM control-arm numbers from the archived log:
**18 of 18 oracle values PASS** — 135 directions across 6 dumps; BeiDou 123 /
Galileo 6 / GPS 6; `CB+PB` 98 / pure-`CLK` 16 / `CB+CLK+PB` 13 / pure-`CB` 7 /
`CLK+PB` 1; 118 of 135 involving a `CB|` component; culprits C06 71, C13 68,
C29 53, C16 49, C34 43, C36 32, C38 31.  The CB-ADM arm's table is likewise
reproduced exactly (151 + 89 + 18 = 258).

The analyzer also reads the new `network_estimability.csv` end-to-end and
correctly **refuses** to propose a calibration on a healthy config ("no
weak-but-real layer to rescue, so calibration has nothing to do here") — the
DG3 guard working in the direction that matters.

---

## 4. Card H — hygiene

- **H1** — the stale comment at the code-bias registration site claimed the phase
  side is "CO-GATED to them PER-(sat,signal) below".  It is not: the co-gate lives
  upstream at phase-bias **state allocation**
  (`MeasurementModel::ensureNetworkSharedStates`) and keys off the run-latched
  **census-admitted** set, not the narrower per-epoch registered subset.  Rewritten
  to say exactly that, with both reasons (the level, because a registration-site
  gate breaks the "every `shared_phase_key` is a registered column" invariant and
  crashed the ON path; and the granularity ruling below).
- **H2** — the granularity seam ruling is now **measurable**.  New run-end counter
  `n_cbadm_phase_orphan_key_epochs` counts (key, epoch) pairs where an ADMITTED
  code key was per-epoch-withheld (< 2 present stations) while its same-signal
  phase column stayed shared.  Computed at the phase registration loop — the only
  site that sees the census-admitted set, the per-epoch registered set and the
  shared phase set together.  Reported in the CB-ADM run-end line and in the CSV
  `RUN` record.

  **Measured 0 on BOTH the 305-station arm and the 50-station sparse arm — and
  that is a weak result, stated as such.**  0 is the expected value, but it is
  also exactly what a MISWIRED counter reports, and the first version WAS
  miswired: it compared the phase key's signal (`L2X`) against the census's code
  keys (`C2X`), so it could never have fired.  Since no available config produces
  a nonzero count, the wiring is proven by a unit test pinning the
  `L<band><attr>` → `C<band><attr>` companion transform against the one
  `MeasurementModel::ensureNetworkSharedStates` applies — **not** by a field
  number.  A genuinely sparse tier is still owed for a true positive control; a
  run showing it nonzero **and** coincident with withholds is what reopens the
  granularity ruling.
- **H3** — units erratum appended to `docs/HANDOFF_cb_admission_card.md` §4, with
  the per-dump vs across-dumps mapping table, the "never compare the dump's
  `N ev<cutoff` with a withhold line's `observed M`" rule (they differ because the
  withhold message reports the POST-heal re-gate: measured 22 vs 14 on the same
  epoch at the same cutoff), and the "never judge by the raw total" rule.

---

## 4b. The 305-station result — DG passes, PA closes its class, ONE direction remains

All arms: Release, renamed exe, `_maxnet_305_gpsgalbds_d15_5ep_*`, 6 recorded
dumps each, declared S-basis dimension 3.

| arm | tolerance | below cutoff / dump | class decomposition / dump | gate |
|---|---|---|---|---|
| `cbadm` (co-gated baseline) | 1e-9 (default) | **33.0** | `CB+PB` 21.5, pure-`PB` 4.0, pure-`CB` 3.5, pure-`CLK` 3.0, `CB+CLK` 0.7, `CLK+PB` 0.3 | 0 emitted, 6 withheld UNDER |
| `datum` (DG 1e-12 + PA min_stations 2) | **1e-12** | **8.0** | pure-`PB` 4.0, pure-`CLK` **3.0**, `CB+CLK` 0.7, `CLK+PB` 0.3 | 0 emitted, 6 withheld UNDER, **0 OVER** |

**DG's falsifier PASSES.**  The weak-but-real layer is gone: `CB+PB` 21.5 → **0**
and pure-`CB` 3.5 → **0** per dump; the analyzer reports "nothing in the ≥ 1e-3
band" for the armed arm, i.e. the `[1e-3, cutoff)` bin is EMPTY.  The three
declared gauges are still counted (pure-`CLK` = exactly 3.0/dump) and
`n_gate_withheld_over` is **0**, so the calibration did not overshoot into the
gauge depth — the over-specified trap the placement rule guards against did not
fire.  Observed near-null count is a flat min 8 / max 8 across the run.

**PA closes its class completely.**  The eight below-cutoff directions of every
dump decompose exactly:

| # | ev | class | heal reach |
|---|---|---|---|
| 1 | 8.671e-09 | `CLK\|E*` common mode | — **declared Galileo gauge** |
| 2 | 3.038e-09 | `CLK\|G*` common mode | — **declared GPS gauge** |
| 3 | -2.737e-09 | `CLK\|C*` common mode | — **declared BeiDou gauge** |
| 4-7 | -8.4e-12 … -1.9e-10 | pure-`PB` (`C22/C21/C30/C29 \| L1I`) | **span ≈ 0.99 ⇒ healed** |
| 8 | 5.358e-10 | `CLK\|C22` + `CB\|C21\|{C5P,C5X,C8X,C7D}` | **zero PB weight ⇒ heal CANNOT reach it** |

The drop-and-heal anchors all four phase orphans, taking the gate from 8 to
**4 observed vs 3 declared** on every epoch (`observed 4` in the withhold line).

**PA4 therefore FAILS by exactly one direction, and that direction is
CODE-dominated** — which is PA3's stated disposition branch: "CB-dominated ⇒ a
genuine admitted-code deficiency — feed Card KP's trigger."

### A telemetry defect this run exposed (and PA2 fixed)

The pre-existing `n_healed_epochs` / `n_healed_cols` count only heals that
**recovered** the gate.  On the co-gated arm the heal fired on all 6 epochs and
recovered none, so the run-end summary reported `0 healed` while the log carried
6 `drop-and-heal failed` lines — a live mechanism reading as dead wiring, the
exact standing-invariant-#4 failure PA2 exists to prevent.  Added
`n_heal_attempt_epochs` / `n_heal_attempt_cols`, counted before the re-gate can
throw.  Verified on the 50-station arm: **"drop-and-heal FIRED on 15 epoch(s),
soft-anchoring 45 near-null phase-bias column(s); 0 epoch(s) were RECOVERED"** —
the same run that would previously have printed nothing at all.  Attempts ≫
recoveries is PA5's insufficiency signature, now readable directly.

---

## 5. Card KP — trigger FIRED, card BUILT (default-OFF)

**Trigger (a) fires on measured data**: the PA3 disposition above identifies the
single residual direction as `CLK|C22` + four `CB|C21|*` components with **zero**
phase-bias weight.  The heal anchors PB columns only, so this direction is
structurally beyond it; and it is not clearable by any tolerance, because a
genuinely null direction is null at every positive cutoff (pinned by
`GenuinelyNullDirectionSurvivesEveryTolerance`).  That is a genuine
admitted-CODE deficiency — exactly KP's stated trigger.

Built accordingly:

- **K1** — `NetworkSbasis::buildCodeBiasKaulaPriors`: one zero-valued prior per
  REGISTERED (= admitted, covered) satellite code-bias column, `info = 1/σ²`.
  New toggles `network.cb_kaula_prior` (default false) and
  `network.cb_kaula_sigma_m` (default **10 m ≈ 33 ns**, the loose end of the
  physical OSB bound).  Folded after the tight gauge rows and before the gate;
  mirrored into `cap_priors` so the RAIM within-epoch replay solves the same
  datum.  σ ≤ 0 is refused at both the loader and the seam (it would mean
  `info = ∞`, a hard pin on every OSB wearing a "loose prior" label).
- **K2** — the anti-fragility margin is **computed from the epoch's own λmax and
  logged once**, not asserted: `log10(info / cutoff)` must be ≥ 2.0, else a loud
  WARN naming it as the configuration CB-ADM already rejected.  On the storm arm
  the cutoff is 6.022e-06 and `info` is 1e-2 ⇒ **3.2 decades**; the residual
  direction's own CB weights (≈0.145 × 4 signals) lift it by ≈8e-4 ⇒ **2.1
  decades**.  Both clear the bar.
- **K3** — `declared_dimension` is unchanged **by construction**, not by audit:
  priors go through the diagonal information channel, not the datum rows.

**What KP does to the PRODUCT, stated plainly.**  Along the direction it lifts,
the emitted OSB combination becomes essentially prior-driven (≈0) with a
posterior σ of order the prior's 10 m — which the M4 contract's `sigc` carries,
so a consumer deweights it correctly.  Every well-observed OSB is untouched
(their data information exceeds 1e-2 by orders of magnitude).  This is the
honest reading of KP: it buys EMISSION of the rest of the epoch at the cost of
one weakly-determined combination being reported as bounded-and-uncertain rather
than withheld along with everything else.  It is also precisely KP's own trigger
(b) — "sigmas too large to be useful downstream" — turned into a measurable
question, and it can only be answered once the arm emits and the products are
graded (the §6.4 leg, on the user's command).

**Deviation from K1's stated site, with rationale.**  The card names
`buildCodeBiasDatumRows`.  That function's output is folded at
`kCodeBiasGaugeWeight` — a TIGHT datum weight, which is the point of a gauge row.
A Kaula prior is deliberately LOOSE, so stacking it there would either mis-weight
it or force a second weight channel through a row order the header documents as
byte-identity-bearing.  The prior is therefore a sibling static on the same class,
folded through `NetworkEstimator::addSharedPrior` — the channel the phase heal
already uses — which also makes K3 hold structurally.  **Flagged for the user's
ruling** per the card discipline.

---

## 6. Deviations from the spec (stated, per the card discipline)

1. **PA's `phase_datum_prioritized` arming is a no-op on this arm, so it was not
   set.**  The spec says to arm `network.phase_datum_prioritized = true`.
   Measured: the knob the heal actually reads is
   `NetworkKnobs::phase_datum_prioritized = prioritized || ensemble || eq34`
   (`NetworkRunContext.cpp`), and the storm arm already sets
   `phase_datum_eq34 = true` — so the heal is **already live** there (the archived
   control/co-gated logs contain `drop-and-heal failed` lines, which only that path
   emits).  Setting the raw toggle as well would only trip the loader's
   "SUPERSEDES network.phase_datum_prioritized" warning.  `phase_datum_min_stations`
   WAS off (1) and is armed to 2 as specified; it is read by
   `networkBuildPhaseDatumCandidates`, which runs on the eq34 path too.
2. **DG6's run-end log line is gated on the tolerance being ARMED**, not on
   `trace_estimability`.  DG5 is a MUST that says a trace-armed run's only artifact
   delta is the new file; a run-end line firing under `trace_estimability` would
   have added a second delta on the golden `estim` arm.  The same numbers still
   reach every trace-armed run through the CSV's trailing `RUN` record, so the
   control arm is not left dark.
3. **A third harness config, `_maxnet_305_gpsgalbds_d15_5ep_datum_dgonly.json`,
   was staged** (cbadm arm + `rank_gate_tol` only).  The spec's single `_datum`
   arm bundles DG and PA, which would confound DG's census-shift falsifier with
   PA's coverage gate.  The DG-only arm isolates it at the cost of one extra
   ~17-minute run.
4. **`assertNullSpaceRank` gained an optional out-parameter.**  DG4 says the gate
   semantics are untouched — they are (exact equality, both-direction throw,
   relative cutoff, declared-dim computation all unchanged); the out-param is
   written before the equality test and read by nothing inside the function.  It
   exists because DG6's under/over split cannot be recovered from the exception
   text without parsing prose.

---

## 7. Standing-invariant pre-flight

1. **Stiff/zero-q event response** — no new time-linked quantity is introduced.
   The tolerance is a per-run constant; the heal's σ = 1 m anchor is unchanged in
   value and now fires on a strictly *smaller* set under an armed tolerance.
2. **Release-only data runs** — every arm below ran
   `build/x64-Release/gnss_processor_bddatum.exe` (renamed copy; name-scoped
   zombie pre-kill).  Debug was used only for `gnss_tests`.
3. **Deferrals triaged** — Card KP: NO LONGER deferred (its trigger fired on the
   armed 305 arm's CODE-dominated residual, §4b); built default-OFF.  PA5 pdp
   anchor-election threading: deferred by the spec's own reserved ruling (collides
   with decomposition 8b); only unlocked if PA+KP together still fail to emit.
   Graded emit leg (§6.4 of the spec): **field-blocking for the BeiDou product
   claim** — this card does not assert BeiDou product quality, only emission; the
   graded leg is ON COMMAND ONLY.
4. **Run-end counters are the ON-path telemetry** — DG6 counters + the CSV `RUN`
   record + H2's orphan counter; all verified non-trivially populated on the
   `estim` arm.
5. **Grade datum-invariant quantities only** — this card grades a CENSUS
   (eigen-spectrum classes), which is datum-invariant by construction.  No wire
   steps are graded here; the §6.4 graded leg is the user's call.
6. **Default-OFF + structural byte-identity** — proven by the 10-arm matrix
   (§3.2) with every diff attributed.

---

## 8. What IS and is NOT answered

**Answered by the 305-station A/B (§4b):**
- **PA3 disposition** — the residual after DG + PA is 3 declared gauges + 4
  healed pure-`PB` + **one CODE-dominated direction** (`CLK|C22` + `CB|C21|*`,
  zero phase weight).  That is the "CB-dominated ⇒ genuine admitted-code
  deficiency ⇒ Card KP" branch, on data.
- **Card KP trigger** — fired (trigger a).  KP built; on the armed+KP arm the
  pre-heal census drops **8 → 7** (the CODE direction is lifted), leaving 3
  gauges + 4 pure-`PB` for the heal.
- **DG falsifier** — PASSED: the weak-real band is emptied, gauges still counted,
  0 over-specified.

**Answered — PA4 EMITS (§9):** DG + PA + KP take the gate to exactly 3 and the
arm emits 6/6.  No lever beyond KP was invented; the emit is the spec's own
four-piece path.

**Open, and explicitly the user's call:**
- **KP ship decision** — the `addSharedPrior` vs gauge-row deviation (§5) is
  reserved to the user.
- **The §6.4 graded emit leg** — BeiDou clock sanity vs the Jun-29 divergence,
  GPS/Gal quality unchanged, held-out consumer float — ON COMMAND ONLY, and only
  meaningful once PA4 emits.
- **H2 positive control** — a genuinely sparse tier (§4, H2).

---

## 9. Measured 305-station outcomes (per arm)

All arms Release, renamed exe, 6 recorded dumps, declared dim 3.  Below-cutoff
directions per dump, class-decomposed (the raw total is never the verdict).

| arm | cfg delta vs cbadm | below/dump | classes /dump | gate |
|---|---|---|---|---|
| `cbadm` | — (baseline) | 33.0 | `CB+PB` 21.5, pure-`PB` 4.0, pure-`CB` 3.5, pure-`CLK` 3.0, `CB+CLK` 0.7 | 0 emit / 6 withheld UNDER |
| `dgonly` | + `rank_gate_tol` 1e-12 ONLY | 8.0 | pure-`PB` 4.0, pure-`CLK` 3.0, `CB+CLK` 0.7, `CLK+PB` 0.3 | 0 emit / 6 UNDER, **0 OVER**; obs min/max 8/8 — DG's census shift ISOLATED from PA |
| `datum` | + `phase_datum_min_stations` 2 (DG + PA) | 8.0 | (same census as dgonly) | 0 emit / 6 UNDER, 0 OVER; heal fires, recovers 0 (residual = 1 CODE dir) |
| `datum_kp` | + `cb_kaula_prior` σ 10 m (DG + PA + KP) | 7.0 (pre-heal) | pure-`PB` 4.0, pure-`CLK` 3.0 (`CB+CLK` dir LIFTED) | **6 emitted / 0 skipped** — heal recovered all 6 (24 cols); DG 6/0/0; K2 margin 3.2 decades; H2 orphan 0 |

### ★ PA4 MILESTONE — the 305 BeiDou W6 storm config EMITS

`datum_kp` run-end, verbatim:

```
Network producer complete: 6 epochs emitted, 0 skipped
    (0 withheld by the observability gate, 0 by the A4 σ̂0 ceiling) — of 8 synced.
Network (Card DG): rank gate evaluated on 6 epoch(s) at tol=1e-12 —
    6 emitted, 0 withheld UNDER-specified, 0 withheld OVER-specified.
    Observed near-null count over the run: min 7, max 7.
Network (Card KP): prior info 1.000e-02 vs rank-gate cutoff 6.022e-06 —
    margin 3.2 decades ABOVE the cutoff (K2 requires >= 2.0). 691 admitted OSB columns.
Network (Card KP): prior folded on 6 epoch(s), 4146 column-epoch(s) at sigma 10 m.
Network (Card PA): drop-and-heal FIRED on 6 epoch(s), soft-anchoring 24 near-null
    phase-bias column(s); 6 epoch(s) were RECOVERED by it (24 column(s)).
Network (CB-ADM): 903 admitted, 134 quarantined, 14 late. Card H2 orphan window: 0.
```

This is the original blocker's close-out: the config that emitted **zero epochs**
for the entire CB-ADM investigation now emits every epoch it processes.  The
mechanism is the FOUR-piece path exactly as the spec's §0.1 reconciliation
predicted — CB-ADM census + co-gate (removes the true code nulls) → DG (removes
the weak-real layer, 33 → 8) → KP (lifts the one CODE-dominated residual, 8 → 7)
→ PA heal (anchors the 4 pure-`PB` orphans, 7 → 3 = declared).  Each piece is
necessary; none alone emits.

**Reading:** DG removed the weak-real layer (33 → 8) without touching the gauges
(`n_gate_withheld_over` = 0 on every arm — the over-specified trap never fired).
The `dgonly` arm isolates that: DG alone gives the identical 8.0/dump census as
`datum` (the `phase_datum_min_stations` bump does not change the census on this
eq34-path arm), so the 33 → 8 shift is DG's and DG's alone — exactly the spec's
"DG's falsifier is the census shift, not emission."  PA's heal reaches the 4
pure-`PB` orphans (span ≈ 0.99) but not the CODE-dominated residual (zero phase
weight), leaving both `dgonly` and `datum` one direction short — hence they
withhold (heal recovers 0), while `datum_kp`, with that direction lifted by KP,
recovers all 6 and emits.  No lever beyond KP was invented; the emit came from
the spec's own four-piece path.
