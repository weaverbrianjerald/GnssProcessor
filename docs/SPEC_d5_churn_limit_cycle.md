# SPEC — the D5 chord-churn limit cycle (v4 re-tap-only, the algebra dump, the slip-aware election)

**Status**: BUILT + unit-gated + 10-epoch smoke PASS 2026-08-02; 3 h graded legs
recorded in §7. All three knobs default-OFF, OFF structurally byte-identical.
**Memory context**: `project_chord_churn_d5_limit_cycle` (the decomposition this
card acts on), `project_m1_card4_d5_innovation` (the D5 gate v1/v2/v3),
`project_network_ddfrac_rootcause_shvtec` and `project_lane_spectrometer_campaign`
(the "churn IS iono representation" verdict this ends up pointing back at).
**Regime**: 63 stations, DOY-129, GPS, 30 s, cold, `carry_invariant` + eq34 +
q_z=1e-4 + the full lane stack — i.e. `_ft63_24h_conv.json`, the config the
decomposition was measured on.

---

## 1. What the decomposition found (do not re-derive; `runs/ft63_24h_conv`)

The headline "113× slip fan-out" (525 009 slip refloats / 4 646 raw slip events)
is a denominator artifact — it divides the D5 loop's OUTPUT by only the raw-event
INPUT. The true split:

- **513 482 (97.8 %) are D5 innovation-gate flags.** Each flag re-taps its chord
  at install AND funnel-marks its arc, so the same chord is killed again next
  epoch. `slip_kills ≈ flag rows`.
- **11 527 ≈ 4 646 raw events × 2.5 DDs/event** — both bands of the slipped arc's
  own chord. No topological cascade.
- Mean chord lifetime **4.3 epochs**; `dd_sigma_cyc` pinned ~3.9 cyc; `med|frac_cyc|`
  uniform at 0.25 at every baseline, including 15 m co-located pairs.

And the flag population is neither slips nor noise: 1.5 % raw-step coincidence,
per-chord `std_innov` FLAT for hours (WIND/G03 at 230.3σ drifting to 185σ over
~7 h), 98.9 % of inter-flag gaps one epoch, 224 chords = 50 % of all flags,
chronicity tracking per-group graph sparsity and overlapping the iono-deweight
gate's chronic >3σ SH-misfit roster.

## 2. ⛔ The "cycle support 1 ⇒ the funnel protects nothing" premise is FALSE for a real slip

The decomposition's proposed fix was to remove the D5→funnel feed-back outright,
reasoning that a CHORD arc lies in exactly one fundamental cycle — its own — so
the membership exclusion can only kill the DD `installPosterior` just reset.

The topology half is right and is now pinned by a test
(`SlippedChordArcRefloatsFreshVsContaminatingControl`, GATE 8: `slip_kills` delta
is exactly 1). **The functional half is wrong.** Measured in
`D5FunnelRefloatCompletesResetAndCostsTheCarry` (GATE 10b), two arms with
identical noise and gate, differing only in whether flags are fed back:

| | re-converges to the post-slip integer | slip_kills |
|---|---|---|
| funnel fed back (today) | **yes**, |Δ| ≈ 0.006 cyc | ≥ 1 |
| feed-back suppressed | **no**, |Δ| = **0.48 cyc** | 0 |

The in-epoch reset moves the chord by the POST-FIT innovation `v`, which the
fold's `−GᵀΣG` border term has already shrunk; the reset then re-establishes a
stiff carry at `σ̂₀²·Cov(v)_ii` and locks the remainder in. **The refloat is what
COMPLETES a real slip's reset.** Removing it wholesale would blunt standing
invariant #1 on precisely the population D5 exists to catch — the fine slips the
coarse MW/LLI/IF-TD layer is deliberately relaxed against.

So the churn is real and the fix is real, but the fix must be scoped by EVIDENCE,
not by topology. That is what §3 builds.

## 2b. ★ THE ROOT CAUSE, found by the instrument in §3b: the gate tests BORN chords

`scripts/grade_d5_algebra.py` on the 3 h baseline leg (`runs/churn_base`, 63 sta, DOY-129):

```
BORN vs CARRIED  (born := sqrt(var_v) >= 900 m, the diffuse fresh-chord prior)
  born      32062   95.34 %   med|v| 19221.550 m   med sqrt(var_v) 1000.000 m   med std_innov 19.23
  carried    1566    4.66 %   med|v|     7.058 m   med sqrt(var_v)    0.645 m   med std_innov  9.07
```

**95.3 % of all D5 flags are on chords BORN that epoch**, at exactly
`√var_v = 1000.000 m` — i.e. `√fresh_prior_var` = √1e6, the diffuse prior — with a median
`|v|` of 19.2 km. That `v` is the correction frame's distance from `x_lin`, not a physical
DD disagreement.

The gate's statistic is `|v_i| > k·σ̂₀·√Cov(v)_ii`, where `Cov(v) = M⁻¹ − GᵀΣG` is the H0
covariance of the innovation **about the carried prior**. A born chord has no carried
prior. There is no H0, so the ratio is not a hypothesis test; the "reset" it triggers is a
no-op, because a diffuse chord's posterior ALREADY is the data-only tap-off; and the flag's
one lasting consequence is the funnel entry, which re-borns the chord next epoch, where it
flags again. Duty on the worst chords: URUM00CHN/G22 **0.99**, and 27 chords are
born-flagged on more than half of all flagged epochs.

**That closes the loop the decomposition described.** A chord re-born every epoch has by
construction never accumulated anything — which is exactly why `dd_sigma_cyc` cannot leave
its pin and `med|frac_cyc|` stays uniform at 0.25. It also explains the two facts the
decomposition found puzzling: per-chord `std_innov` FLAT for hours (the x_lin offset is
static, so the ratio is), and `frac(v/λ)` uniform (an x_lin distance has no reason to be
near an integer).

**D5 v2 already knew this.** It excluded born chords from the σ̂₀ accumulator with the note
*"a born chord's diffuse Cov(v) would tank σ̂₀²"*. The same exclusion was never applied to
the gate DECISION — the two are three lines apart in `installPosterior`.

⇒ `network.carry_invariant_innov_carried_only` (§3d). It is upstream of both v3 and v4,
which suppress this same population for incidental reasons (a born chord's arc usually
reads measured-and-flat), and it is the one of the three defensible as a *correctness* fix
rather than a policy choice.

## 3. What was built

### 3a. `network.carry_invariant_innov_retap_only` (D5 v4)

A D5 flag whose arc the raw observables **MEASURED as continuous** re-taps its
chord in-epoch as before, and its arc is no longer excluded from the next epoch's
`A_old` membership. Same fail-safe tri-state as the v3 corroboration gate:

| raw verdict | response |
|---|---|
| stepped | re-tap **and** refloat (unchanged) |
| measured and flat | re-tap, **keep the carry** |
| no evidence | re-tap **and** refloat (unchanged) |

Plus a current-epoch re-check: an arc a raw detector flags THIS epoch refloats
regardless of what was measured at flag time.

**The funnel INSERT is untouched.** Every side consumer — the Iono-2 v3k erase
classifier, the WL-3 MW-row slip-skip, anchor hygiene, the PB emit filter, Card
G's bridge classifier — reads the same union it always read. Only the
carry-membership test changes.

Implementation: `NetworkInstallCarry.cpp` tags the flat subset into
`ctx.carry.carry_innov_pending_flat` at the funnel-insert site (the one place
holding the flags, the reverse map and `slip_qc.epoch` at once, and where the WL-3
pair partner must be tagged too or the suppression is silently half-on);
`ApplicationNetwork.cpp` drains it in lockstep with `carry_innov_pending`;
`NetworkDdPriorBuild.cpp`'s `ambSlipped` consults it. Counter:
`ctx.carry.carry_innov_kill_suppressed`, reported at run end with a zero-tripwire.

### 3b. The algebra dump — six columns on `network_d5_flags.csv`

`std_innov` alone cannot say whether a standing 230σ means `v` is large or
`Cov(v)` is small, and those want different fixes. Added, under the existing
`network.mw_wl.d5_dump`: `zhat, v, var_v, sigma0, z_post, dw_w`.

`dw_w` is the station's SH-constraint deweight weight
(`ctx.carry.topo_prev_w`, this epoch, blank without `network.trace_topology`) —
carried so the measured overlap between the chronic-flagger roster and the
deweight gate's chronic-misfit roster is gradeable from one file.

⚠️ `zhat` / `z_post` are CORRECTION-frame values about `x_lin`, so differencing
`z_post` across epochs mixes real movement with x_lin re-seeds (which are exact
solve no-ops — `project_network_xlin_invariance`). `scripts/grade_d5_algebra.py`
therefore builds every statistic from `v` and `var_v` and never from a `z_post`
difference.

### 3d. `network.carry_invariant_innov_carried_only` (D5 v5) — the root fix

Three lines in `installPosterior`: reuse the SAME `chord_carried` mask the σ̂₀ accumulator
already uses to exclude born chords from calibration, and apply it to the gate decision.
Counter `innov_born_skipped`, incremented BEFORE the threshold test so it is the whole born
population, not just the part that would have flagged.

Gated by `SkipsBornChordsUnderCarriedOnly` (GATE 10c), which uses the first epoch of a run
— entirely born chords — as the probe: the unrestricted gate flags there, carried-only
flags nothing, the DD after the epoch is unchanged to 1e-6 (skipping the gate cannot move
the solve, because the flag's in-epoch action is what a diffuse chord's posterior already
is), and a genuinely carried chord with an unsignalled 12-cycle jump still flags.

### 3c. `network.phase_datum_eq34_slip_aware_election` (Card 3/4 correctness)

A slipped arc in the spanning TREE poisons every fundamental cycle through it
(GATE 9: a tree-edge slip refloats all 3 dependent chords); a slipped CHORD costs
one DD. The election is the only place that decides which a slipped arc becomes,
and it decided without looking: the sticky-tree seed re-seats every live incumbent
unconditionally, and the incumbent queue drains ahead of the new-edge walk.

Three effects, all fail-safe:
1. a slipped incumbent loses its sticky seed;
2. a slipped arc is refused in the new-edge walk's FIRST pass, on the same terms
   as INC-1's cold host, and admitted in the second;
3. it ranks below every clean arc in the I4 longevity order.

**(2) is load-bearing and was found by the test, not by inspection.** The first
implementation put slip-avoidance only in (1) and (3) — and `arcLife` is read ONLY
under `phase_datum_eq34_longevity_election`, which the field config does not arm.
The result de-seeded the incumbent and then handed the edge straight back through
the walk: a datum handover paid for nothing. Avoiding a slipped tree edge is a
CORRECTNESS preference, not a longevity heuristic, so it gets its own
second-chance flag rather than riding one.

Counters `eq34_slip_{deseeded,demoted,readmitted}`. `readmitted` is the honest
one: when the slipped arc is a component's only route to a satellite the
completion re-admits it and the mass refloat happens anyway — correct (the
topology forced it), but it must be visible, or a nonzero de-seed count reads as
"no slipped arc is in the tree" when one still is.

## 4. Arms

`scripts/churn_run_arms.ps1` (Release-only, renamed exe, wipes `logs/` AND every
snapshot first — the producer writes `state_snapshot_path` at each hour boundary,
so a re-run would otherwise warm-resume off its own last output and read as a
clean cold start).

| arm | config | treatment |
|---|---|---|
| `base` | `_ft63_churn_base.json` | D5 v2, exactly as `ft63_24h_conv` ran |
| `v3` | `_ft63_churn_v3.json` | `+carry_invariant_innov_corroborate` |
| `v4` | `_ft63_churn_v4.json` | `+carry_invariant_innov_retap_only` |
| `base2` | `_ft63_churn_base2.json` | base again, on the LATER exe — see below |
| `v5` | `_ft63_churn_v5.json` | `+carry_invariant_innov_carried_only` |

All four treatments act on the same population and exactly one is armed per arm;
arming two makes the arm unreadable (v3 suppresses the flat flags one level
earlier, so with v3 on, v4 is inert by construction — the loader warns).

v3 vs v4 is the interesting pair: same population, **opposite** response. v3
leaves the DD alone; v4 re-centres it on the data but keeps its identity. If the
innovation is SH-iono / conditioning tension rather than a slip, v3 holds the DD
stiff against it and the tension must land elsewhere, while v4 lets the DD absorb
it without ratcheting.

`base2` exists because base/v3/v4 ran on one exe and v5 needs a later one.
Standing invariant #6 forbids reading a cross-build delta as a treatment effect,
so `base2` re-runs the base treatment on the later build: it makes v5-vs-base a
same-build comparison AND discharges the OFF-path byte-identity gate the two new
knobs owe. If `base2 != base` anywhere, the default-OFF claim is false and every
ON-arm number here is unattributable.

`-Smoke` runs the 300 s variants (staged-cadence directive: smoke first).

## 5. Grading

All datum-invariant (standing invariant #5). Read `network_epoch_status.csv`
**by column NAME** — the indices around `n_arc_refloat`(17) / `n_d5_flags`(18) /
`n_d5_retaps`(19) / `n_chord_births`(20) / `n_slip_refloats`(21) are off by one.

1. `n_chord_births` / `n_slip_refloats` per epoch — the churn itself;
2. `dd_sigma_cyc` — does it leave the ~3.9 cyc pin (`scripts/grade_ddfrac_perhour.py`);
3. `med|frac_cyc|` — does it leave 0.25 (uniform = zero integer information);
4. **`total_IF` spread / `worst_med` — the NO-HARM check. A churn win that moves
   this is not a win.**
5. `scripts/grade_d5_algebra.py` on `logs/network_d5_flags.csv` for the mechanism.

## 6. 10-epoch smoke (2026-08-02) — both mechanisms fire, and land on the same churn

| arm | D5 flags | suppressed | slip refloats | chord births |
|---|---|---|---|---|
| base | 534 | — | **748** | **1629** |
| v3 | 3 | 858 / 861 trips (99.7 %) | **147** | **1028** |
| v4 | 499 | 507 membership kills | **147** | **1028** |

Two different mechanisms converge on **exactly** the same refloat and birth counts
— 147 refloats, the raw-detector-driven remainder — while differing by 166× in how
many DDs they re-tap. That is what the decomposition predicts if the D5 population
is a limit cycle and nothing else: remove it at either edge and the same 601
refloats disappear.

10 epochs from cold is ignition and no convergence verdict may be read from it.

`scripts/grade_d5_algebra.py` on the base smoke, for the mechanism (ignition, so
treat as provisional):

```
median |v|          9.9548 m      <- the disagreement
median sqrt(var_v)  0.7859 m      <- the formal claim about it
INTEGER TEST on v/lambda:  median |frac| 0.265   (uniform = 0.25)
                           within 0.10 cyc of integer  19.7 %  (chance = 20 %)
raw corroboration:  stepped 0.37 % / measured-and-flat 99.25 % / no evidence 0.37 %
```

**The fractional part of `v/λ` is indistinguishable from uniform.** A cycle slip
displaces a DD by an INTEGER number of cycles, so this is the direct test that the
re-tap population is not integer-scale, independent of what the MW/GF detector
said — and it agrees with the 1.5 % raw-step coincidence from a completely
different instrument. Threshold tuning cannot fix a continuous misfit; the misfit
source has to.

## 7. ⛔ 3 h graded legs — THE CHURN IS A SYMPTOM. Removing it changes nothing.

Release, same day, cold, 360 epochs each. `scripts/grade_churn_arms.py`.

| arm | births/ep | refloats/ep | D5 flags/ep | chord lifetime | med\|frac\| | dd_sigma_cyc | med\|dDD\| cyc/ep | σ̂0_code |
|---|---|---|---|---|---|---|---|---|
| base | 105.8 | 96.6 | 93.4 | ~8 ep | 0.2492 | 8.710 | 2.674 | 2.974 |
| base2 | 105.8 | 96.6 | 93.4 | ~8 ep | 0.2492 | 8.710 | 2.674 | 2.974 |
| v3 | 14.9 | 5.6 | 1.4 | **~54 ep** | 0.2523 | 8.610 | 2.284 | 3.051 |
| v4 | 14.6 | 5.3 | 23.1 | **~56 ep** | 0.2442 | 8.620 | 2.175 | 3.042 |
| v5 (defective guard) | 37.9 | 28.6 | 24.5 | ~21 ep | 0.2458 | 8.634 | 2.496 | 2.994 |
| **v5b** (corrected) | 18.9 | 9.6 | 4.8 | **~43 ep** | 0.2464 | 8.634 | 2.491 | 2.994 |

(chord lifetime ≈ 811 carried DDs / births per epoch. DD columns are the LAST HOUR
only — cold ignition must not enter a convergence verdict. `base2` reproducing `base` to
every printed digit is the byte-identity gate showing up in the grading table.)

`total_IF` at hr2, the converged tail (`scripts/chain_stability.py`):

| arm | spread_med | spread_p90 | worst_med | clk_std |
|---|---|---|---|---|
| base | 7.871 | 8.636 | 20.93 | 4.95 |
| v3 | 7.938 | 8.552 | 21.82 | 5.06 |
| v4 | **7.680** | **8.319** | 20.95 | 4.98 |
| v5b | 7.837 | 8.636 | **20.75** | 4.96 |

**The card's central hypothesis is falsified.** Its stated payoff was: *"mean chord
lifetime 4.3 ep … Fixing the loop (without touching iono) should lift lifetime to O(50)
ep; then med|f| on the stable basis becomes a clean iono diagnostic instead of uniform
hash."* The antecedent was delivered **in full, four times over, by four mechanically
independent treatments** — v3 suppressed 99.0 % of gate trips, v4 suppressed 10 496
membership kills, v5b removed 100 % of the untestable-prior population — cutting refloats
90-94 % and lifting chord lifetime from ~8 to 43-56 epochs, squarely in the predicted
O(50). The consequent did not follow in any arm: `med|frac_cyc|` stayed uniform (0.2492 →
0.2523 / 0.2442 / 0.2464), `dd_sigma_cyc` did not leave its pin (8.71 → 8.61 / 8.62 /
8.63). `total_IF` moved ≲1 % in both directions — neither harm nor benefit.

The four differ in *mechanism* as sharply as they agree in *outcome*: v3 suppresses the
flag, v4 suppresses only its funnel consequence, v5b never evaluates the untestable
population at all. They differ 16× in how many DDs they re-tap (1.4 vs 23.1 flags/ep) and
land on the same churn and the same null. That is not a scoping artifact.

**⇒ The churn was a SYMPTOM, not the cause of the uniform DD fractions.** This inverts the
causal direction the lane-spectrometer campaign's "churn IS iono representation" verdict
was read in: iono representation is upstream of BOTH.

It also reframes what `dd_sigma_cyc ≈ 8.6 cyc` means. With DDs now genuinely accumulating
over ~55 epochs and the variance still not falling, **the DD information is not there to be
had** — it was never being thrown away by resets. Any further work on integer recovery has
to add information (iono representation, constellation, cadence), not conserve it.

### ⚠️ The v5 guard shipped wrong, and its own instrument caught it

v5 keyed the skip on the driver's `chord_carried` mask, on the reasoning that it is the
same signal D5 v2 uses to keep born chords out of the σ̂₀ accumulator. That reasoning is
wrong: **`chord_carried` is TOPOLOGICAL** (cycle ⊆ A_old, no arc slipped) and says nothing
about whether the carried prior holds information. A chord carried from a predecessor that
was itself diffuse inherits a diffuse *projected* prior and passes it.

Measured on `runs/churn_v5`: after skipping 13 643 rows, **7 082 of the 8 807 surviving
flags (80.4 %) were still at √Cov(v) = 1000 m** — still untestable. The guard caught about
two thirds of its target while the loader line and the header claimed it caught the
population.

Corrected to skip on `!chord_carried` **OR** `Cov(v)_ii ≥ ½·fresh_prior_var`. `Cov(v)` is
the PRIOR-predictive variance, so a value near `fresh_prior_var` means `Pᶻ_ii` is diffuse
whatever the topology says; half is a deliberately loose bar, since one epoch of real data
collapses a DD variance by orders. Result on `runs/churn_v5b`: **1 725 flags, 0.0 %
diffuse-prior** — exactly the informative-prior population and nothing else. GATE 10c
gained a regression assertion that no surviving flag has a near-diffuse prior.

The defective arm is kept on disk as `runs/churn_v5` deliberately: the 80.4 % is the
evidence for why the topological test is not the informational one.

### ★ What the null makes visible: the REAL D5 population, and a correction to §1

Filtering both arms to informative-prior flags only (`√var_v < 900 m`):

| | flags | signals | top stations | on a deweighted sta | max chord duty |
|---|---|---|---|---|---|
| base2 | 1 566 / 33 628 | L1C 1003, L1W 440, L2X 123 | OAFA, YKRO, GUAM, VOIM, SAVO, SVTL | 13.4 % | 0.067 |
| v5b | 1 725 / 1 725 | L1C 1008, L1W 515, L2X 202 | OAFA, YKRO, VOIM, GUAM, INEG, SGOC | 15.8 % | 0.125 |

**The real population is ~1 600 flags per 3 h leg (≈4.5/ep), on a stable roster, at LOW
duty (≤0.12), and it is essentially UNCHANGED by any treatment.** Everything the four knobs
removed was the born-chord loop. Median `|v|` 6.47 m = 34 L1 cycles at 8.3σ, `frac(v/λ)`
still uniform (0.264; 19.2 % near-integer vs 20 % chance) and 96.5 % measured-and-flat — so
the "model tension, not slips" verdict survives on the population that actually tests a
hypothesis, which is the stronger form of that claim.

**This CORRECTS §1's chronicity finding, which conflated two different things:**

- The **duty-0.99 chronic chords — WIND, MRO1, URUM, ULAB, CAS1** — were the born-chord
  loop, an artifact. Their "std_innov 230σ FLAT for hours" is a static x_lin-frame offset
  measured against a diffuse prior; nothing physical is drifting. They do not appear in the
  informative population at all.
- The **sparse-group signal is REAL and survives**: OAFA / YKRO / VOIM / GUAM / INEG /
  SGOC, with **L2X over-represented relative to its station count** (202 flags from a
  3-station group vs 1008 from the dense L1C group) — exactly §1's "chronicity tracks
  per-group graph sparsity", now measured on a population where it means something.

So the roster to study is **not** the one the decomposition named. That is worth stating
plainly, because the named stations were about to become the target of the follow-on work.

### Slip-aware election — wiring smoke (10 ep, `runs/churn_elect_smoke`)

```
21 slipped incumbent(s) DE-SEEDED, 0 arcLife demotion(s), 3 slipped arc(s) became a
tree edge ANYWAY because the component had no other route to the vertex.
chord births 1629 -> 1477 (-9.3 %)   slip refloats 748 -> 596 (-20.3 %)
carried DDs at run end 798 -> 798 (unchanged: edges MOVED, tree rank preserved)
```

Re-admitted / de-seeded = **3/21 = 0.14** — on this graph the completion almost always had
an alternative, so the policy (not the topology) is the binding constraint here, and 18
tree-edge mass kills became rank-1 chord kills. 0 arcLife demotions is expected and
correct: `phase_datum_eq34_longevity_election` is off in this config, which is exactly why
the walk-side refusal (§3c item 2) had to exist.

⚠️ This smoke found a **bug in this card's own loader guard**: the "armed without
`carry_invariant`" advisory was placed in the parse block that reads the knob, but
`network_carry_invariant_` is parsed a few lines LATER, so it fired on a config that plainly
arms `carry_invariant`. Moved to the cross-knob validation pass, where every flag is
settled. A loader advisory that lies is worse than none — it teaches a reader to ignore the
whole class.

### ⛔ Slip-aware election — 3 h field arm (`runs/churn_elect`): MECHANISM WORKS, NET-HARMFUL

| | de-seeded | re-admitted | ratio | births/ep | refloats/ep |
|---|---|---|---|---|---|
| elect | **102** | **9** | **0.088** | 58.3 (vs 105.8) | 50.0 (vs 96.6) |

The policy is doing exactly what it was built to do: the graph had an alternative route
**91 %** of the time, so 93 tree-edge mass kills became rank-1 chord kills, and chord
refloats fell 48 %. The `readmitted/deseeded` question §3c posed is answered — the
topology is NOT the binding constraint on this network.

**And it is net-harmful anyway.** `total_IF` at hr2 against the same-build baseline:

| | spread_med | spread_p90 | worst_med | clk_std |
|---|---|---|---|---|
| base2 | 7.871 | 8.636 | 20.93 | 4.95 |
| elect | **9.104** (+15.7 %) | **9.727** (+12.6 %) | **23.35** (+11.6 %) | **5.94** (+20 %) |

That is an order more movement than the ≤1 % band every D5 arm on this leg sat in, in the
wrong direction, on the project's designated no-harm quantity.

**Mechanism of the harm: de-seating an incumbent IS a datum handover.** The arm paid 102
phase-datum chain handovers the baseline never paid. Under `carry_invariant` the DDs are
invariant to that, which is why the churn improved — but `total_IF` is a clock + code-bias
quantity in the EMITTED frame, and chain-datum churn is precisely the hazard
`project_ppprtk_literature_pack` records as unsolved in the literature and amplified by
stiff carries. The card traded a graph-theoretic win for a datum-stability cost and the
cost is larger. Standing invariant #7's shape exactly: the mechanism changes which
information flows into which states, and only one of its two edges was being reasoned about.

⚠️ **Do NOT read this arm's DD columns.** `med|frac|` 0.2500 and `dd_sigma_cyc` 7.433 look
like an improvement and are a POOL-SIZE artifact: the arm traces **115 distinct
station-pairs at 81 rows/epoch against the baseline's 80 pairs at 29** (more DDs survive to
maturity, so more pass the trace's eligibility). That pool growth is itself evidence the
mechanism worked — and it makes the DD statistics non-comparable, exactly the confound
`project_net305_attribution_rerun` records.

**Verdict: KEEP OFF.** The prediction in this card's own §8 — "a correctness improvement
independent of AR, so it should not be deleted" — is falsified as an *arming*
recommendation. Keep the code (it is correct, gated, and the counters make the trade
visible), keep it default-OFF, and treat the de-seed as an event that owes a datum-side
response before it could ever be armed.

### Byte-identity gate (invariant #6) — PASS

`compare_network_run.py compare runs/churn_base runs/churn_base2` reports exactly **2**
content diffs, both pure config provenance: `network_run_manifest.json`'s `config_extends`
chain and `run.log`'s config-inheritance line, each naming the deliberately
differently-named `_ft63_churn_base2.json`. Independently checksummed:
`network_epoch_status.csv`, `network_dd_fractional.csv`, `network_d5_flags.csv`,
`network_solution.csv`, `network_carry_vce.csv`, `network_events.csv` and `products.txt`
are md5-IDENTICAL across the two builds; `state_snapshot.bin` is identical in length and
passes the tool's field-level `wall_utc` mask. So the later build's OFF path — carrying the
v5 gate and the slip-aware election — is inert, and the v5-vs-base comparison is
same-build-legal.

## 8. Verdict, and what it means for the queue

**Keep all four knobs default-OFF. None has earned a recommendation.** v4 is marginally
best on `total_IF spread_med` (7.680 vs 7.871) and `med|dDD|` (2.175 vs 2.674); v5b is best
on `worst_med` (20.75 vs 20.93). All of it is inside noise. They are cheap, correct, and
worth having armed if a future card needs a quiet carry — but arming them buys nothing
measurable today, and the honest ledger verdict for each is UNVALIDATED-with-evidence, not
RECOMMEND.

**Do not spend more effort reducing churn for AR's sake.** The question the card was built
to answer is now answered from four directions. The follow-on that mattered — *"then
med|f| on the stable basis becomes a clean iono diagnostic"* — is dead: the basis was
stabilised to 43-56 epoch lifetimes and `med|frac_cyc|` did not move off uniform.

**What the null actually says.** With DDs genuinely accumulating over ~50 epochs and
`dd_sigma_cyc` still flat at 8.6 cyc, the DD information is **not there to be conserved**.
Integer recovery needs information ADDED — iono representation, constellations, cadence —
not information protected. That is the same conclusion
`project_why_non_integer_rootcause` reached from the physics side (NL is 99.3 % iono,
needs 0.117 TECU), and this card is the independent estimation-side confirmation. It also
inverts how the lane-spectrometer "churn IS iono representation" verdict should be read:
iono representation is upstream of BOTH the churn and the uniform fractions.

### Still genuinely open

- ~~The slip-aware election has no field grading~~ — **CLOSED, NET-HARMFUL** (see §7). It
  works mechanically (102 de-seeded / 9 re-admitted) and costs `total_IF` spread +15.7 %.
  Keep OFF. The open question it leaves is a real one: **a de-seed is a datum handover and
  currently has no datum-side response.** If tree-edge mass kills ever become worth
  removing, that response — not the election policy — is the thing to build. Cross onto
  `SPEC_deweight_release_hygiene.md`, which is the same shape of problem.
- Neither of the two remaining `carry_slipped_ambs` consumers was re-examined for whether
  it WANTS the D5-only entries it currently gets (the v3k erase classifier already splits
  them out; anchor hygiene and the PB emit filter do not).
- **The chronic-flagger roster is still unexplained and is now the interesting object.**
  URUM / CAS1 / MRO1 / ULAB / WIND at duty up to 0.99, overlapping the iono-deweight gate's
  chronic >3σ SH-misfit roster. With the churn removed, whatever those stations are doing
  is no longer hidden behind 33 628 flags/leg — `runs/churn_v5b`'s 1 725 informative-prior
  flags are a clean population to study. Cross onto
  `project_network_ddfrac_rootcause_shvtec`.
