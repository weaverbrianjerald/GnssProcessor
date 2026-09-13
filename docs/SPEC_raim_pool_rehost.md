# SPEC — RAIM pool re-host (post-fit RAIM on the Card-4 invariant-carry lineage)

**Status:** INC-0 built, default-OFF, unit-gated. Field gates 1–3 MEASURED 2026-09-13 (§7.1): the screen is live and neutral (RECOMMEND_SCOPED, pinned on the field golden), the actuator is untested for want of opportunity (KEEP_OFF); the power leg is owed.
**Date:** 2026-08-13
**Knobs:** `network.raim_carry_invariant` (screen, measure-only), `network.raim_carry_invariant_refloat`
(actuator). Neither fingerprinted (eq34-hygiene / dw-release precedent), so a same-snapshot A/B is legal.
**⚠ Anchor drift** (re-grepped at HEAD 2026-08-15): the 2026-08-13 text cited
`ApplicationNetworkAssembly.cpp:2520` / `ApplicationNetworkAssembly.cpp:2546` /
`ApplicationNetworkAssembly.cpp:2560` for `legacy_local_inject` / `do_amb_carry` / the `pl_amb` fill — now
`:2534` / `:2603` / `:2617`, with the new `qc_amb` fill at `:2565`; `NetworkRaim.cpp:203-207` for the
constraint-aware σ̂₀ data side — now the comment at `:93` and the computation at `:290-301`;
`NetworkRaim.cpp:513` / `NetworkRaim.cpp:569` for the two refloat loops' pivot skip — now `:593` / `:650`
(the Pass-1 skip is `:157`); `NetworkSolveGlue.cpp:1043` / `NetworkSolveGlue.cpp:1145` /
`NetworkSolveGlue.cpp:1158` for the within-epoch tier — now `:1110` (`within_epoch_armed && carry_warm`),
`:1219` (`R.pl_amb.empty() && !we_ci`), `:1232`; `GnssConfigLoadNetwork.cpp:7028` for the "RAIM-family
lesson" comment — now `:7253`; `GnssConfigLoadNetwork.cpp:6129-6135` for the eq34 hard load error — now
`:6187-6191`; `NetworkRunContext.hpp:1195-1200` for the deferral-pattern rationale — now `:1294-1304`
(beside `dw_release_pending` `:1299`, `raim_refloat_pending` `:1333`); `NetworkRunContext.cpp:2180` /
`NetworkRunContext.cpp:2296` / `NetworkRunContext.cpp:2269` for the `pl_amb` consumers — now `:2298-2314`.
`MeasurementModel.cpp:566`, `NetworkDdPriorBuild.cpp:550-576` and `NetworkRaim.cpp:191-193` are unchanged.

---

## 1. The defect

Post-fit network RAIM has been **structurally inert on every Card-4 (`network.carry_invariant`) lineage**
since Card 4 landed. Not mistuned — inert. It reported ARMED in the loader summary and screened nothing.
Two independent kills, either alone sufficient:

**KILL 1 — the seam is never called.**

```cpp
// src/core/app/ApplicationNetwork.cpp (SB_RAIM)
if (raim_on && carry_warm) {           // carry_warm = pl_on && (...)  [:1428]
    networkPostFitRaim(...);
}
```

`carry_warm` requires `network.phase_leveling`. A Card-4 config that leaves leveling off never executes
the seam. This is the field posture: `_ft63_24h_conv.json` carries `carry_invariant: true` and `raim:
true` and **no `phase_leveling` key** — only the decoy sub-knob `phase_leveling_max_gap_s: 900.0`, while
the master flag defaults false.

**KILL 2 — the residual pool is empty, even with leveling ON.** Every RAIM walk keys on
`StationAssembly::pl_amb`, whose sole fill site sits under `legacy_local_inject` (`carry_warm`) and then
`do_amb_carry = (phase_datum_prioritized || !st.is_pivot) && !carry_invariant`. Card 4's DD carry
supersedes the phase-leveling ambiguity carry as a solve input, so `do_amb_carry` is false and `pl_amb` is
never built. RAIM's Pass-1 station loop then bails at `if (R.pl_amb.empty() || ...) continue;`, leaving
`cells`, `amb_stats`, `r_pool` and `flagged` all empty, and both refloat loops iterating zero times.

### 1.1 Why this is a HOSTING defect, not a statistics defect

The enumeration RAIM needs — *which ambiguity columns are present, unpinned, and therefore checkable this
epoch* — is entirely carry-agnostic: it walks `getAmbiguities()`, skips gauge pins, maps through
`h2local`. Nothing in it depends on who owns the time-link. But it was **hosted on the phase-leveling
carry's own bookkeeping vector**, because that carry happened to need the same enumeration first. Two
different questions were sharing one data structure:

| question | nature | correct host |
|---|---|---|
| which columns get a carried prior injected? | CARRY | `pl_amb` |
| which columns are screenable by a post-fit residual test? | OBSERVABILITY | `qc_amb` (new) |

When the carry vehicle changed, the QC substrate vanished with it.

### 1.2 Blast radius

Everything riding the same dead walk on a Card-4 lineage: post-fit RAIM consensus + median-polish
attribution (`NetworkRaim.cpp`); the MDB reliability rider (`NetworkRaim.cpp:191-193`); the
constraint-aware global σ̂₀ data side; the L13 per-station residual trace; and — **sibling, NOT fixed
here** — the within-epoch tier, which has the identical dependency. CLAUDE.md already flags that as a
sanctioned open fix; deferred deliberately (§6 D1).

Shipped configs, **`extends`-RESOLVED** (`scripts/gnss_config_chain.py`, re-measured 2026-08-21):
**632** arm `carry_invariant` and **543** arm `raim` across all of `config/` (511 / 538 of those in
`config/test_suite/` alone); the whole `_ft63_*` field lineage arms both. Corroborates the stored
GOLDEN-0729 observation that armed QC tiers never fired.

⚠ The resolved count is the one that governs behaviour, and it is ~3× the literal one — a bare
`grep '"carry_invariant": true'` over `config/test_suite/` returns **203** (and **197** for `raim`),
because most lineages inherit the flag through an `extends` chain rather than restating it. The
earlier revision of this line quoted those literal figures as the population; 203 was reproducible
but counted only the configs that spell the knob out, and the companion `raim` figure (129) had
since drifted. Any "how many configs are affected" question here must be resolved, not grepped.

### 1.3 The codebase already knew

`GnssConfigLoadNetwork.cpp:7253` calls this **"the RAIM-family lesson applied before the run instead of
after it"** — but wired the resulting INERT guard only for `carry_vce_apply`, never for RAIM. There was no
loader warning for `raim + carry_invariant`. The IF-phase-TD block sixteen lines above already had exactly
the three-way `phase_leveling / carry_invariant / neither` branch RAIM lacked.

---

## 2. What does NOT actuate (and why the obvious fix is wrong)

Filling the pool makes RAIM *run*. It does not make its refloat *act*. The seam performs two mutations per
flagged arc, and on the Card-4 path **neither moves the solve**:

| what does not actuate |
|---|
| **(a) `local_carry.erase`** drops a phase-leveling carry entry. Its only solve-side consumer is the PASS-B inject, under the same `!carry_invariant` gate — and on this lineage the map is empty anyway. No-op. |
| **(b) `markAmbiguityPendingReseed`** *does* reach a live, ungated reader — `MeasurementModel.cpp:566` via `ensureRequiredStates`, which reseeds `x_lin`. But for the network that is **bookkeeping, not actuation**: the kernel solves corrections about the linearization point with no write-back, so the reduced shared RHS $b_{red} = u_s - N_{sr} N_{rr}^{-1} u_r$ is algebraically unchanged. Proved for the sibling mechanism in `SPEC_deweight_release_hygiene.md §7.2`. |
| **The only actuator that reaches the invariant carry is the C3c slip funnel** (`carry_slipped_ambs`), which membership-excludes the arc from `A_old` so its chord is re-tapped from a diffuse prior. |

### 2.1 Why the response must be deferred one epoch

RAIM detects **post-solve** at `SB_RAIM`. The funnel is a per-epoch driver local whose load-bearing
consumer, `buildCarryDdPriors`, already ran at `SB_DDBUILD`. So a flag raised at `SB_RAIM` has missed its
own consumer by several sub-blocks. This ordering problem has been solved twice already, and
`NetworkRunContext.hpp:1294-1304` states the rationale verbatim for the second case. RAIM is the **third**
instance:

| producer | detects at | queue | drains into |
|---|---|---|---|
| Card-4 D5 innovation gate | post-solve | `carry_innov_pending` | `carry_slipped_ambs` |
| iono-deweight release edge | `SB_T1A` | `dw_release_pending` | `carry_slipped_raw` |
| **post-fit RAIM (this card)** | `SB_RAIM` | `raim_refloat_pending` | `carry_slipped_ambs` |

**Provenance is load-bearing.** RAIM drains into the funnel proper and *not* into `carry_slipped_raw`. The
raw subset means a RAW-OBSERVABLE step, which both the Iono-2 v3k `d5_keep` erase classifier and the WL-3
MW-row slip-skip read. A post-fit residual consensus is estimation-side evidence — the same class as the
D5 innovation gate, which also drains into the funnel only. Draining RAIM into raw would silently change
the Iono-2 deviation-erase policy.

---

## 3. Design

### 3.1 The census (`qc_amb`)

`StationAssembly::qc_amb` — same `PlAmb` POD, same walk, built in PASS B **outside both carry gates**
(`ApplicationNetworkAssembly.cpp:2565`). `pl_amb` keeps its exact current contents (fill at `:2617`) and
becomes documented as the carry-eligible *subset*. A separate block rather than a relaxation of the two
existing gates, because `pl_amb` is read by the leveling harvest and the carry-VCE row feed
(`NetworkRunContext.cpp:2298-2314`), the sticky tree, and the within-epoch tier; widening it in place
would change carry behaviour on a `phase_leveling` + `carry_invariant` config — an invariant-#6 violation.

**The census is deliberately BROADER than `pl_amb`, and that is the point.** `pl_amb`'s gate skips the
*whole pivot station* on the legacy datum path (`do_amb_carry = (phase_datum_prioritized || !st.is_pivot)
&& ...`, `:2603`), because every one of that station's ambiguities is a gauge pin under that datum.
`qc_amb` applies only the per-`(sat, signal)` `networkIsGaugePinned_` test, so on a legacy-datum run it
also carries the pivot's non-anchored arcs. Correct separation of concerns: the census answers "what is
checkable", each consumer applies its own policy on top — and every current RAIM consumer already does
(the Pass-1 station loop `:157` and both refloat loops `:593` / `:650` carry their own
`(!phase_datum_prioritized && stations[i].is_pivot) → continue`; because that skip runs *before* the tally
increment, `pool_stations` / `pool_cells` count what was actually screened, not what the census offered).
A future consumer that wants pivot arcs must opt in explicitly rather than inherit a carry vehicle's datum
policy by accident — which is how the original defect happened.

### 3.2 Pool selection

One substitution point in `NetworkRaim.cpp` serves all five riders:

```cpp
auto ambPool = [&](const StationAssembly& R) -> const std::vector<PlAmb>& {
    return R.qc_amb.empty() ? R.pl_amb : R.qc_amb;
};
```

`qc_amb` is never allocated when the knob is off ⇒ a phase-leveling run is byte-identical.

### 3.3 Knob resolution — why it lives with the carry knobs

```cpp
k.raim_ci_on      = k.raim_on && k.carry_invariant && config_.networkRaimCarryInvariant();
k.raim_ci_refloat = k.raim_ci_on && config_.networkRaimCarryInvariantRefloat();
```

Resolved next to `k.carry_invariant`, **not** with the rest of the `raim_*` family, so that one bool
answers "is the re-hosted census live" for all three consumers — the driver's gate, the assembly's fill,
and the seam's selector. They must not be able to disagree.

`k.carry_invariant = phase_datum_eq34 && networkCarryInvariant()` also drags `eq34` into the conjunct.
**That is defensive redundancy, not a live discriminator** — `carry_invariant` without `eq34` is a *hard
load error* (`GnssConfigLoadNetwork.cpp:6187-6191` returns `false`; the DD cycle basis is defined only on
the eq34 chain datum), so no config that could exercise the difference reaches knob resolution. Kept for
symmetry with `carry_invariant_pd_safe` / `carry_aware_covariance` and because it is already correct if
that requirement is ever relaxed to a warning; `CarryInvariantWithoutEq34IsAHardLoadError`
(`tests/test_raim_carry_invariant_pool.cpp:225`) pins the error so such a relaxation fails loudly rather
than silently promoting the conjunct to load-bearing.

### 3.4 Lifetime bound (invariant #1)

`raim_refloat_pending` is keyed on `(station index, ambiguity key)`. If a station index were reused or a
key re-allocated between epochs, a stale pair would refloat an unrelated arc. Bounded to **exactly one
epoch** by an unconditional drain-and-clear at the driver head. The drain must stay unconditional even on
a datum-change or station-drop epoch — an early `continue` that skips it would break the bound. Not
serialized (the dw-release precedent): a warm resume starts with an empty queue.

---

## 4. Edits landed (INC-0)

| file | change |
|---|---|
| `include/config/GnssConfig.hpp` | two getters + two private members, full Doxygen |
| `src/core/config/GnssConfigLoadNetwork.cpp` | parse; three INERT guards; σ̂₀-withhold warning; three-way RAIM posture log |
| `include/app/NetworkRunContext.hpp` | `raim_ci_on` / `raim_ci_refloat` knobs; `raim_refloat_pending` queue; `raim_ci` tallies slot |
| `include/filter/NetworkRaim.hpp` | `NetworkRaimCiTallies` |
| `src/core/app/NetworkRunContext.cpp` | knob resolution beside `carry_invariant` |
| `include/filter/NetworkRunTypes.hpp` | `qc_amb` + re-documented `pl_amb` |
| `src/core/app/ApplicationNetworkAssembly.cpp` | census fill block, outside both carry gates |
| `src/core/filter/NetworkRaim.cpp` | `ambPool` selector at 4 sites; queue insert at both refloat loops; tallies |
| `src/core/app/ApplicationNetwork.cpp` | call-site gate widened; funnel drain beside its two siblings |
| `src/core/filter/NetworkTeardown.cpp` | run-end telemetry block |
| `tests/test_raim_carry_invariant_pool.cpp` | new suite |
| `CMakeLists.txt` | test source entry |

No new production `.cpp`: the census belongs in the assembly TU (it is PASS-B assembly output), the screen
in `NetworkRaim`, the drain at the driver head beside its siblings, the queue in `NetworkRunContext`.

---

## 5. Telemetry (invariant #4)

This card exists because a tier read ARMED and contributed nothing, so the **denominators are the
deliverable**. Four previously-identical-looking outcomes are now distinguishable:

| observation | meaning |
|---|---|
| `epochs == 0` | the seam never ran (call-site gate) |
| `epochs > 0`, `pool_cells == 0` | it ran, census EMPTY — **dead wiring, the original defect** (logged `warn`) |
| `pool_cells > 0`, `sats_flagged == 0` | screened real residuals, nothing bad — *zero opportunity, not zero capability* |
| `sats_flagged > 0`, `refloats_queued == 0` | flagged, actuator disarmed (logged `warn`) |

σ̂₀ mean/max are reported over live-pool epochs only. **This is the first measurement of σ̂₀ on this
lineage** — with the pool empty it never left its `1.0` initializer, so any previously-quoted RAIM
threshold here was calibrated against a constant.

---

## 6. Risks and deferrals (invariant #3 triage)

| id | risk / deferral |
|---|---|
| **R1** | **FIELD-BLOCKING. A "measure-only" arm is NOT emission-neutral.** Filling the pool makes σ̂₀ real, and σ̂₀ feeds (a) the A4 withhold `raim_withhold_sigma0: 6.0` — armed in the standard set — which can begin **withholding epochs**, and (b) `ura_sigma0_ewma`, so emitted URA moves. Neither sits behind the refloat knob. The loader now warns when the screen is armed with a live withhold. **Decide before gate 2, not during it:** either disarm the withhold for the first leg, or grade the leg as emission-affecting. |
| **R2** | **the statistic's POWER under Card 4 is unproven.** Under phase-leveling a carried ambiguity is pinned by a stiff prior, so a missed slip has nowhere to hide and spikes the post-fit residual — RAIM's stated mechanism. Under `carry_invariant` the local ambiguity column is **free each epoch** and the time-link lives in the DD chord's prior, so a slipped arc's tension may be absorbed locally and never reach the column's residual. **The re-host makes RAIM runnable, not necessarily powerful.** This is why the actuator is a separate knob and why gate 2 is mandatory. |
| **R3** | **refloat scope under eq34. UNRESOLVED.** Both refloat loops skip the pivot unless `phase_datum_prioritized` (`NetworkRaim.cpp:593`, `:650`). Whether that is set on the eq34/carry_invariant configs was not verified. If false, the refloat silently excludes the pivot station on exactly the lineage this card targets. **Check before gate 3.** |
| **R4** | **invariant #7, both edges.** The queued refloat is the DECOUPLING edge (`A_old` membership exclusion, `NetworkDdPriorBuild.cpp:550-576`); the RE-COUPLING edge is the chord's re-tap the following epoch. Both are counted. What bounds the decoupled arc is the one-epoch queue lifetime. The D5 v4 record is the cautionary case: indiscriminate membership exclusion pinned mean chord lifetime at 4.3 epochs and `dd_sigma_cyc` at ~3.9 cyc. **A RAIM refloat storm on a lineage where the statistic has no power would reproduce exactly that churn** — which is why R2 gates R3. |
| **D1** | **FIELD-INERT, deferred: the within-epoch tier.** `NetworkSolveGlue.cpp:1110` / `:1219` has the identical `carry_warm` + `pl_amb` dependency and the identical fix. Deliberately NOT bundled: that tier is solve-affecting *in-epoch* (it re-solves, dropping culprits) and mixes with the WE-CI code/iono pools, so its A/B is a different and harder gate. Land this first, then re-use `qc_amb` there with its own validation. |
| **D2** | **NOT RECOMMENDED, deferred: queue unification.** Four ad-hoc pending sets (`carry_innov_pending`, `carry_innov_pending_flat`, `dw_release_pending`, `raim_refloat_pending`) with three slightly different drain semantics is accretion, and one provenance-tagged network refloat-request queue is the correct end state. But it touches three landed cards' validated wiring and cannot be made byte-identical trivially. The fourth queue is added now in the established shape so the later merge is mechanical. |

---

## 7. Validation owed

| unit validation |
|---|
| **Unit** (landed, PASSING): `tests/test_raim_carry_invariant_pool.cpp`, 10 cases — defaults OFF; the legacy Card-4 posture unchanged; both arming postures; three INERT guards + cascade; the eq34 hard-load-error; the withhold left alone. Verified twice on 2026-08-13, Debug, VS 2026. **(1) Isolated worktree at `8e5b04b`** (needed because a concurrent session had `NetworkSbasis::buildCodeBiasDatumRows` mid-signature-change so the shared tree could not link `gnss_tests`): `RaimCarryInvariantPool` 10/10; full suite **1370 / 197, 0 FAILED, 17 SKIPPED** — all 17 fixture-gated (`FixtureSmokeTest`, `Gpt3Test`, `ShIonosphere`, `RinexNavParserFixture`), because a fresh worktree has 4 files under `data/` against the main tree's 388 (`data/` is gitignored pending `bootstrap_internal_data.py`). **(2) Shared tree, after the break cleared**, both workstreams present and fixtures bootstrapped: `RaimCarryInvariantPool` 10/10; full suite **1385 tests / 198 suites, 0 FAILED, 1 SKIPPED** (the documented `FixtureSmokeTest`). Baseline before this card was 1360 / 196; the delta is +10 cases / +1 suite from this file plus the concurrent family-GF work. |
| ⚠ To reproduce the isolated arm: `git worktree add --detach <dir> HEAD`, copy the 12 files from §4, configure with the bundled CMake 4.3.1 under the `CLAUDE.md` build wrapper, build `gnss_tests`. `include/app/NetworkRunContext.hpp` must be reconstructed as HEAD + this card's four hunks if another session has it open — copying it wholesale drags in their members and their un-copied dependencies. |
| **Still owed at unit level:** census contents equal the `pl_amb` a matched phase-leveling config would build; the drain lands in `carry_slipped_ambs` and **not** `carry_slipped_raw`; the drained pair excludes that arc from `A_old` membership (the only test that proves actuation *reaches* the carry). These need the assembly harness. |

| gate | content |
|---|---|
| **Field gate 1 — smoke (Release only, invariant #2).** | 10 epochs on an `_ft63`-derived config (carry_invariant ON, phase_leveling OFF, raim ON). Assert `raim_ci.epochs > 0`, `pool_cells > 0`, and that the OFF twin is byte-identical on `network_solution.csv` / `network_events.csv` / `network_vtec_sh_health.csv`. |
| **Field gate 2 — measure-only, ≥1 h.** | Record the σ̂₀ distribution (p50/p99) and the T3/T4 flag rate on this lineage for the first time. Explicitly check whether the A4 withhold starts firing and whether emitted URA moves (R1). |
| **Field gate 3 — refloat armed.** | Grade on datum-invariant / SD quantities only (invariant #5): `total_IF` on a ≥2 h window per the level-channel rule, plus chord lifetime and `dd_sigma_cyc` against the OFF leg — churn is the predicted failure mode (R4). |

---

### 7.1 Field gates 1–3 MEASURED (2026-09-13; worker on worktree-agent-ac909cc72761ca6f7, exe = main's producer `gnss_processor_rs7.exe` 23C628B3…, seed `runs/evseed_field6h_v5` ep0604; `scripts/raim_gate_grade.py`; manager grade)

**Pre-flight.**  (a) Neither knob is a warm-resume fingerprint token — gates 2–3 ran as legs from one snapshot
(all 8 warm-resumed under `state_snapshot_require`).  (b) **R3 is non-binding under eq34** — `NetworkRunContext.cpp:292`
resolves `k.phase_datum_prioritized = prioritized || ensemble || eq34`, so the `(!phase_datum_prioritized && is_pivot)`
skip at `NetworkRaim.cpp:201 / 822 / 1090 / 1184` never fires on this lineage: the pool counts **61 × N − ε** station-epochs
(7196 over 118 epochs; 14698 over 241), the pivot / clock-datum station MBAR00UGA included.  (The worker's first
reading of the raw getter said "MBAR skipped"; the leg's own arithmetic corrected it.  R3 still bites a
legacy-pivot-datum `carry_invariant` lineage.)  (c) **R1 splits:** the A4 withhold compares `sigma0_adapt`, which the
WITHIN-EPOCH tier publishes over `R.pl_amb` only (`NetworkSolveGlue.cpp:1310–1473`, `:1900`) — deferral D1 — so the
re-host CANNOT make the withhold fire on any lineage until D1 lands (the loader's R1 warning overstates that half; and
`network_epoch_status.csv`'s `sigma0` column is that within-epoch statistic, not this card's).  The URA path IS live
(`NetworkRaim.cpp:435 → NetworkSolPrep.cpp:433`, `emit_ura` armed); `ura_retro_bump` / `clock_monitor_couple_raim` off,
`ura_refloat_inflate` 1.0 (inert).

**Gate 1 PASS.**  OFF smoke `products.txt` `87a33bf0…` (the certified golden hash); ON: `20 seam pass(es), 1200
station-epoch(s) with a live census, 18163 ambiguity cell(s) screened` — not the DEAD WIRING warn; σ̂₀ mean / max
1.000 / 1.000, 0 satellites / 0 arcs flagged (`ZERO OPPORTUNITY`); **ON hash == OFF hash**, every record kind identical.

**Gate 2 — the screen is live and completely neutral** (116-epoch legs from ep0604; ⚠ all three carry `zwd_seam_reseed:
true`, which §21.21 of SPEC_event_harness has since shown is the wrong posture for this lineage — shared by the three
arms, so the A/B stands; the absolute ZWD / clock numbers are inflated exactly as RS-10 predicts):

| | raim_ctl | raim_screen | raim_screen_nowh |
|---|---|---|---|
| seam passes / pool station-epochs / cells | INERT (loader warn) | 118 / 7196 / **107 396** | 118 / 7196 / 107 396 |
| σ̂₀ mean / max over live-pool epochs | — | **1.000 / 1.000** | 1.000 / 1.000 |
| satellites / arcs flagged; T3 / T4 rows | — | **0 / 0; 0 / 0** | 0 / 0; 0 / 0 |
| A4 withheld epochs | 0 / 118 | **0 / 118** | 0 / 118 |
| URA Δ vs ctl (U records) | — | **0.000 m, 0 of 3487** | 0.000 |
| products sha256 | `a910f1be…` | `a910f1be…` | `a910f1be…` |
| wall | 316 s | 527 s (contention) | 332 s |

C / B / P byte-identical between the control and BOTH screen legs — the "differs only at withheld epochs" clause has an
empty list; `ev_seam_grade` identical on every channel.  Cost ≈ 15 s / 118 epochs ≈ 0.13 s per epoch (~5 %).  R2's
power question is answered in the negative-evidence direction: a network carrying metre-level ZWD and clock error
(from the misapplied reseed) still produced σ̂₀ ≡ 1.000 across 107 396 cells — exactly "the free ambiguity column
absorbs the tension locally" R2 predicts.

**Gate 3 — the actuator is UNTESTED, not dead.**  The first attempt was discarded: `scripts/event_leg.py` changed on
main between the two launches (the RS-10 conditional landed), so `raim3_ctl` carried `zwd_seam_reseed: true` and
`raim3_refloat` false — a two-knob A/B with a spurious 36-withhold difference (SPEC_event_harness §21.21; feedback:
harness defaults are shared state).  Gate 3b, both 240-epoch legs generated back-to-back off one harness (sha16
`e6071f43…`, `zwd_seam_reseed: false` on both, diff = the two card knobs + paths): 241 / 241 emitted on both; pool
14 698 station-epochs / **220 718 cells**; σ̂₀ 1.000 / 1.000; satellites / arcs flagged **0 / 0**; refloats queued /
drained **0 / 0**; chord lifetime mean 35.78 = 35.78; `dd_sigma_cyc` p50 3.9468 = 3.9468; D5 flags / re-taps / births per
epoch identical; `products.txt` **byte-identical** (`c1f6a6e6…`) — three 240-epoch legs (the discarded refloat leg too)
hash the same with the actuator on and off.  `total_IF` over the 2 h window: min 7.77 m at 40 min, final 10.01 m
(+29 %, still degrading — the level channel has not converged at 2 h on this lineage, invariant CLAUDE.md Amendment 6).
`refloats_queued == 0` with `sats_flagged == 0` is the ZERO-OPPORTUNITY state (§5), not the dead-actuator signature;
the pre-registered PASS shape (`refloats_queued > 0`) is not met for want of opportunity.  One non-reproducible
termination: `raim3b_refloat`'s first run died at ~epoch 177 with exit −1 and a clean log tail (an external kill —
four run-only workers shared the exe NAME `gnss_processor_rs7.exe`; evidence `runs/evleg_raim3b_refloat_crash1`); the
identical `leg.json` re-ran 241 / 241.

**Decision.**  `network.raim_carry_invariant` (the screen): **RECOMMEND_SCOPED** on `carry_invariant` lineages and
PINNED on `golden_field_gps_dual` — it makes post-fit RAIM run at all there (today: pool empty, σ̂₀ a constant, the
loader's own "INERT" warning) at ~5 % epoch cost with products byte-identical, and its run-end telemetry (§5) is the
first honest reading of what the tier sees; the "RAIM always on" directive is served by a live, neutral screen, not by
an inert one.  `network.raim_carry_invariant_refloat` (the actuator): **KEEP_OFF** — uncertified, zero opportunity in
2 h 58 min of field data.  **Owed, pre-registered:** the POWER leg — E3 (the equal-count slip at GOPE00CZE/G24, +10)
with the screen armed vs off from the v8 harness seed: does σ̂₀ leave its floor at +10, is G24's arc flagged (T3 / T4
rows > 0), and with the actuator armed does `refloats_queued` read > 0 with the arc drained into `carry_slipped_ambs`
(the §7 unit item that proves actuation reaches the carry) — the one measurement that turns "runs" into "screens".
D1 (re-host the within-epoch tier's pool) stays deferred and is the prerequisite for any withhold on this lineage.

### 7.2 The POWER leg, re-shaped before it is run: the E3 shape gives the screen no opportunity by construction (pre-registered 2026-09-14 ~00:45 local, manager; no leg read)

§7.1 named E3 (the equal-count L1C + L5X slip at GOPE00CZE/G24, +10) as the power leg.  Read against the epoch order it
cannot work: under the golden's OFF posture the (1,2) IF-TD chain sees the L1C half of E3 as a 3.1 m step and the two MW
pairs (1,5) / (2,5) see 29 WL cycles; the arc is detected and DRAINED (`free_ambs`) before `SB_RAIM` runs (the RAIM seam
sits after both slip drains), so at +10 the screen meets a FREE column — zero opportunity, whatever its power.  The same
holds for E1 (two MW pairs fire), E2, E5 (LLI).  A power measurement needs a slip every armed detector is blind to:

**The RAIM-only shape on this posture: EQUAL cycles on all three tracked bands — L1C, L2W and L5X each + n at the same
epoch (`n, n, n`).**  MW(1,2), MW(1,5) and MW(2,5) are identically zero (Δn = 0 on every pair); the (1,2) IF-TD step is
`(α₁λ₁ − α₂λ₂)·n = 0.107 n m` against an 8·MAD threshold (`if_phase_td_k_mad` 8.0); no LLI; GF is not armed on the network
path.  The D5 innovation gate WILL flag the slipped arc's chords (the DD steps by n·λ_b per band) but `carry_invariant_
innov_corroborate` refuses a flag without raw evidence, so the chord prior stays and the tension has one place to go —
the post-fit residual the re-hosted screen reads.  That hand-off (detector blind → D5 refused → RAIM) is exactly what the
"RAIM always on" directive asks the tier to catch.

**Legs** (from `evseed_field6h_v8` ep0604, 60 epochs, the harness defaults, exe = the certified `AF418A9F…` under a unique
name; harness `e6071f43…` must not move): `raimp_null_screen` (no event, `raim_carry_invariant: true`); for n ∈ {1, 2, 4}:
`raimp_n<n>_off` (seed posture — the screen absent, the loader's INERT line), `raimp_n<n>_screen` (+ `raim_carry_invariant:
true`), `raimp_n<n>_act` (+ `raim_carry_invariant_refloat: true`) — ten legs; the injection block is three `slip` events at
tow 277620 on G24 at GOPE00CZE, signals L1C / L2W / L5X, `delta_cycles` n each (the E3 shape with the third band added).

**Reads.**  Per leg at GOPE/G24, k ∈ [+9, +15]: T1 IF-TD rows, T0 MW rows, LLI, D5 rows on chords touching (GOPE, G24)
with their corroborated / held outcome — the DETECTOR CENSUS that decides whether the leg is a RAIM-only opportunity;
the screen legs' σ̂₀ series k = +5..+25 and the null's floor series; T3 / T4 rows with identity; on the act legs
`refloats_queued / drained` and the `carry_slipped_ambs` drain with identity; the wire (fp1_gate_table's G-C read, event −
null at the same posture): G24 CLK / PB |Δ| at +10..+50; `products.txt` hashes — `raimp_null_screen` byte-identical to
`cert_null_v8`, every screen leg byte-identical to its off leg (the A4 withhold is unreachable until D1; a difference is a
finding), act legs allowed to differ after +10 only where a refloat drained.

**Bars.**  P0 (blindness): the smallest n whose off arm has ZERO detector rows at GOPE/G24 in [+9, +15]; if n = 1 is
already caught by IF-TD, the posture has no RAIM-only slip of ≥ 1 cycle and the screen's field opportunity is EMPTY on
this lineage — a closing finding, not a failure of the leg.  P1 (power, on that n): the screen arm's σ̂₀ at +10..+12 rises
above 1.0 + 3× the null's own spread AND a T3 / T4 row names GOPE/G24 within +10..+15 ⇒ PASS (the screen sees an undrained
slip); σ̂₀ at its floor with the D5 flag refused ⇒ FAIL = §3's R2 absorption confirmed on a real slip (report the arc's
post-fit residual and the chord-prior residual, which then carry the finding).  P2 (actuation, only on P1 PASS):
`refloats_queued ≥ 1`, the arc in the drain with identity, and the act arm's G24 wire error over +20..+50 below the
screen-only arm's.  P3 (the cost of the miss, reported): the off arm's G24 CLK / PB error vs null over +10..+50 — what an
undetected (n, n, n) slip costs the products, the number that prices the screen.  Consumer channels are REPORTED, not
gated (SPEC_event_harness §24.1).

### 7.3 RESULTS — the POWER leg: an undrained equal-count slip is below every tier's resolution on this lineage, and it costs the wire nothing (2026-09-14 ~03:40 local; worker on worktree-agent-aa3b8bfe38edc9aa1 `19392c8`, merged; exe = the certified `AF418A9F…` as `gnss_processor_raimp.exe`; seed v8 ep0604; harness `e6071f43…` unmoved across 16 legs; `scripts/raimp_batch.py` + `scripts/raimp_grade.py`; manager grade)

**P0 — the (n, n, n) shape is caught for n ≥ 2 always, and for n = 1 by a coin flip.**  The (1,2) IF-TD bar is its physical
floor, `thr = max(k_mad·s_eff·map, floor_frac·λ_IF(1,1))` (`NetworkSlipQc.cpp:1194–1197`, `floor_frac` 1.0) = 0.106953 m = exactly
one equal-count cycle's IF step; the empirical term (8 × 3 mm × 1.2 ≈ 0.03 m) never reaches it.  So an n = 1 slip is caught iff
the chain's own noise at the event epoch shares the step's sign: the pre-registered k = 11 draw was caught by +6.0 mm (n = 2 by
+0.113, n = 4 by +0.327 m, each refloated at the event epoch — the E3 situation), and the null chain predicted 20 of 55
epochs blind for n = 1.  **P0′ (the manager's extension): three re-draws at k = 15 / 19 / 23 were all MISSED** — dd 0.1065 /
0.1057 / 0.1065 m against 0.1070 (margins −0.4 / −1.3 / −0.5 mm), zero T1 / T0 / LLI rows in [kev−1, kev+5], MW Δ ≡ 0 and
GF Δ = −0.0539·n m by construction.  The D5 innovation gate never TRIPPED on the arc either (suppressed totals equal the
null's) — on this lineage the chord priors sit at `dd_sigma_cyc` p50 ≈ 3.9 cycles (§7.1), so a one-cycle step is a quarter
of a chord σ.

**P1 — FAIL: the re-hosted screen saw nothing on the three undrained slips.**  Screen / act arms: σ̂₀ 1.000 / 1.000 over
54 518 cells and 61 seam passes, 0 satellites / 0 arcs flagged, 0 T3 / T4 rows, URA ratio ≡ 1.0000 at every epoch, act
byte-identical to off (`d4f34599…` / `6166c9d1…` / `aef07661…`); GOPE's post-fit phase pool at the blind epoch moved by
+3.7 mm s0 / −0.6 mm max against the null.  §3's R2 is confirmed on a real slip: the free local column takes the step and
the residual never sees it.  One structural qualifier the worker found in the snapshot: (GOPE00CZE, G24) is GOPE's **L5X
eq34 datum PIN** (`EQP,GOPE00CZE|G|L5X,G24`; L1C → G32, L2W → G12), and pinned keys are excluded from the pool at carry
build (`NetworkRaim.cpp:198–199`) — the L5X third of the slip cannot reach the screen by construction; the L1C / L2W thirds
were in the pool and left no footprint.  On the matrix legs (n = 1, 2, 4 caught) P1 reads FAIL for want of opportunity, as
§7.2 predicted.  **P2 — not applicable** (queued / drained 0 / 0 on all six act legs; the actuator has never fired in 16
legs).  **P3 — the cost of the miss is nil**: G24 clock max |Δ| vs null 0.0022 / 0.0017 / 0.0009 m (means ≤ 1.1 mm), phase
bias max 0.034 / 0.029 / 0.037 m growing toward the window end; the CAUGHT slips cost more (CLK 0.018–0.024, PB 0.064–0.070 m
— the response's own price).  Blast at kev: 0 keys > 0.5 m on all 16 legs; declared discontinuities at the events: 0.

**Provenance.**  Every leg 61 / 61, warm resume, 3 events armed / 3 fired, no cold start; config diffs exactly the knob(s)
+ paths; `raimp_null_screen` ≡ `cert_null_v8` (`e0d234ee…`); every screen ≡ its off leg; the harness sha before and after
each leg unchanged.  Two instrument traps recorded: `network_epoch_status.csv`'s `sigma0` column is the WITHIN-EPOCH tier's
`sigma0_adapt` (`NetworkStashStatus.cpp:530`, pooled over `pl_amb`, empty here) — it cannot see the re-hosted screen, contra
`raim_gate_grade.py`'s docstring (amended); and a D5 trip the v3 corroboration gate suppresses never reaches
`network_d5_flags.csv` (`NetworkInvariantCarryDriver.cpp:1646–1648`), so "refused" is visible only in the run-end totals.

**Decision.**  `network.raim_carry_invariant` stays RECOMMEND_SCOPED and pinned: live, neutral, ~5 % cost, and now with its
power measured — none, on the one slip shape the detectors miss, which is also harmless.  `network.raim_carry_invariant_refloat`
stays KEEP_OFF with the stronger record: zero opportunity in 16 legs INCLUDING three detector-blind slips.  The structural
reading: on a `carry_invariant` lineage whose chord priors are ~4 cycles wide, a one-cycle slip is below the resolution of
every tier — IF-TD by its own floor, D5 by the chord σ, post-fit RAIM by the free column — and the wire is indifferent to it;
the tiers' power is bounded by the carry's strength, not by their statistics.  No further power legs; D1 stays deferred (a
withhold remains unreachable), and the next RAIM question is the sim bed's stiffer chords, not the field's.

## 8. Promotion

Per the standing no-promotion directive: `network.raim` stays as-is, both new knobs stay default-OFF, and
the **632** shipped `carry_invariant` configs (§1.2 — `extends`-resolved, not the literal-grep 203)
keep their current (inert) behaviour until a validated arm.

## 9. Related

`docs/SPEC_deweight_release_hygiene.md` §7.2 — the `b_red` invariance proof; the deferral precedent this
card copies. `docs/SPEC_d5_churn_limit_cycle.md` — the churn failure mode of R4.
`docs/SPEC_pd_safe_dd_fold.md`, `NetworkDdPriorBuild.cpp:550-576` — the `A_old` membership-exclusion edge
the drain feeds. CLAUDE.md standing invariants #1, #3, #4, #5, #6, #7 — all pre-flighted in §3.4, §5, §6.
