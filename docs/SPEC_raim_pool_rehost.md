# SPEC — RAIM pool re-host (post-fit RAIM on the Card-4 invariant-carry lineage)

**Status:** INC-0 built, default-OFF, compile-clean, unit-gated. Field gates 1–3 owed.
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

## 8. Promotion

Per the standing no-promotion directive: `network.raim` stays as-is, both new knobs stay default-OFF, and
the **632** shipped `carry_invariant` configs (§1.2 — `extends`-resolved, not the literal-grep 203)
keep their current (inert) behaviour until a validated arm.

## 9. Related

`docs/SPEC_deweight_release_hygiene.md` §7.2 — the `b_red` invariance proof; the deferral precedent this
card copies. `docs/SPEC_d5_churn_limit_cycle.md` — the churn failure mode of R4.
`docs/SPEC_pd_safe_dd_fold.md`, `NetworkDdPriorBuild.cpp:550-576` — the `A_old` membership-exclusion edge
the drain feeds. CLAUDE.md standing invariants #1, #3, #4, #5, #6, #7 — all pre-flighted in §3.4, §5, §6.
