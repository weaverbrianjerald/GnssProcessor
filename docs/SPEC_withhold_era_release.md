# SPEC — A4 withhold-era release policy (`network.raim_withhold_adapt_escalate` + `raim_withhold_free_erase`)

**Session 2026-08-21.  Status: DERIVED + ADVERSARY-REVIEWED (design v2 — the
v1 recipe-commit was killed in review, §2-R); implementation + validation this
session; results in §7.  Owner of QUEUE_principled_replacements item 7's A4
half and of the §13 open-valve row "RAIM withhold / re-entry"; the SH-gate
(emission-only) half of item 7 is NOT this card.**

Repro pair and evidence: `runs/fld63_relevel4h_{off,on}` (commit `65ba362`,
`SPEC_slip_relevel_walk.md` §8.4) — the ON leg's 29-epoch A4 era.  Doctrine
inputs: the Jul-28 adapt-and-EMIT directive, the DOY-129 D-G rulings
(`SPEC_day129_frame_guards.md` Parts C–F), standing invariant #7.

## §1 The defect, named exactly (all measured on the repro pair)

A4 (`network.raim_withhold_sigma0`) is a whole-epoch skip: the decision sits
inside `networkSolveGlue` (`NetworkSolveGlue.cpp:1887`) and everything
textually below the driver's SkipEpoch consume (`ApplicationNetwork.cpp:2045`)
never runs — install, leveling harvest, post-fit RAIM/T4, clock QC, SH health,
stash.  Its stated purpose is carry protection ("the contaminated solve never
enters the carry", `rt_service_operations.md` §3) — correct, and kept.  The
un-designed fallout, in four measured pieces:

1. **The within-epoch tier is BUDGET-limited during the era, and its verdicts
   die with the epoch.**  On all 29 era epochs `n_adapt` sat pinned at the
   hard cap `kMaxAdapt = 5` (`NetworkSolveGlue.cpp:1135`) — i.e. iteration 5
   still had culprits clearing the k=6 gate (an early gate-exhaustion break
   would have left `n_adapt < 5`).  The recipe moved through FOUR plateaus
   (268890 {G13,G14 + 2 arcs}, 269070 +G20, 269400 excl-switch, 269520
   {G14,G19 + new arcs}, each ≥ 2 epochs — ⚠ not "one frozen recipe", the
   §8.4 shorthand overstated this), G13's peel moved its clock 48 m inside
   the scratch re-solve, and every peel was discarded with the withheld
   epoch.  Nothing latches across epochs by design
   (`ApplicationNetwork.cpp:1868-1874`).
2. **The frozen zero-q leveling carry is what ramps σ̂₀.**  `sigma0_adapt` has
   no cross-epoch memory; what persists is the carry it is measured against.
   During the era the stash is skipped, so `value_abs`/`var`/`last_seen_s`
   freeze while assembly re-injects at `var_eff = var + pl_q_amb·gap` with
   `pl_q_amb = 0` — stale levels at undiminished stiffness against a moving
   field: σ̂₀_first 22.9 → 48.5 over the era.
3. **The era ends only at the invariant-#1 cliff.**  Exit at 269760 was NOT a
   recovery: `gap > phase_leveling_max_gap_s` (900 s) mass-expired the entire
   leveling carry (pl prires rows 753 → 0), the solve went effectively free
   (σ̂₀ = the 1.0 floor exactly — `NetworkSolveGlue.cpp:1319-1322`), and the
   fleet re-leveled from one epoch of code with the worst prior-tension epoch
   of the whole run (2.75e17).  The 32 subsequent σ̂₀ = 1.0000 epochs are the
   floor clamp on the fresh self-consistent carry.
4. **A slip detected DURING the era loses its response** — the QUEUE-9 walk
   mechanism re-opened through the withhold path: detection runs pre-solve
   and latches, PASS-B consumes the free latch against a solve that is then
   discarded, the harvest half is skipped, the latch is cleared at the next
   PASS-A (`ApplicationNetworkAssembly.cpp:1495`), the pending-reseed mark
   was consumed in PASS-B — the next accepted epoch injects the stale
   PRE-SLIP carry at converged weight.  The 269400 mid-era event (σ̂₀ step
   +7.4) is this, live.  (The Card-4 DD funnel shares the hole; the bridge
   corroboration test also degenerates during an era — both recorded, both
   bounded by short eras rather than separately mechanized.)
5. **What actually ends an era on the healthy path is an EMIT.**  OFF's own
   4-epoch era ended when the 5-peel budget finally landed σ̂₀ 27.8 → 4.26
   under the 6.0 ceiling at 273390: the epoch EMITTED, and the full
   downstream healing ran — post-fit T4 committed 140+104+55 refloats and
   the fleet harvest re-stashed; σ̂₀ 4.26 → 1.20 in three epochs.  ON
   absorbed OFF's killer event the same way (emitted through it, refloats
   live).  The deadlocked and healthy paths differ in whether the epoch
   reaches the emit — NOT in which carries get erased (§2-R).

## §2 Derivation — the constraint set

- **C1 (carry protection, kept):** no value-class update from a withheld
  epoch — no posterior install, no harvest, no stash.  Untouched.
- **C2 (attack the measured limitation):** §1.1's cap-pinned `n_adapt` is the
  measured binding constraint on the era's own epochs; §1.5 is the measured
  exit mechanism.  The design must convert era epochs into EMITTED epochs by
  letting the sanctioned adaptation finish, not build a parallel response
  channel.
- **C3 (D-G1 MUST-1, disposition/verdict):** the σ̂₀ statistic, the k=6
  culprit gate, the ceiling, and the withhold decision logic are computed
  identically.  A deeper peel budget is more of the already-sanctioned
  response, applied only inside an era; a landed adaptation emitting is the
  doctrine's PRIMARY outcome, not a disposition override.
- **C4 (invariant #7, both edges):** era entry/exit become first-class events
  with counters and identities, emitted at the single decision site.
- **C5 (single-trip hygiene):** the first withheld epoch of an era runs the
  stock budget (both legs' 268620 single-trip stays byte-identical in
  behavior); escalation engages from the second consecutive withheld epoch.
- **C6 (bounded, non-destructive):** the escalated peels live in the scratch
  re-solve only.  An exhausted escalated budget withholds exactly as today —
  the failure mode of the new mechanism is the status quo.
- **C7 (era continuity guard):** era state advances/exits only at epochs that
  reach the A4 decision; bypass paths (datum_failed, gate_withheld,
  solve_failed, PASS-A-head skips, decimation) neither advance nor exit it.
  A TOW-gap guard (`now − wh_last_tow > 1.5 × rate` ⇒ era reset) prevents an
  hours-stale era from escalating an unrelated future event.

### §2-R Rejected by adversarial review (design v1 — do not rebuild)

v1 committed the tier's final recipe (culprit carry erases) to
`ctx.carry.local_carry` on persistent-culprit era epochs.  Killed on four
independent grounds, each verified in source or data:

1. **It cannot work by the era's own numbers**: the scratch re-solve already
   REMOVED the culprits, and the post-peel σ̂₀ was still 9.3–26.5 on all 29
   epochs — committing the same removals durably cannot land the next epoch
   under the ceiling.  The healthy exits it imitated were powered by the
   emit's full downstream (299 T4 refloats + fleet re-stash), not by culprit
   erasure alone.
2. **Unbounded blast radius under a stable scapegoat**: a common-mode/datum
   fault yields a stable recipe (persistence measures the FAULT's stability,
   not identification correctness); at v1 defaults the commit could erase up
   to 2 520 amb-entries (3.3× the measured 753-row carried fleet) of healthy
   zero-q carries — the §1.3 cliff re-implemented as policy.
3. **Scope defect**: `drop_sats`/`drop_arcs` leave scope at
   `NetworkSolveGlue.cpp:1789-1793` (deliberately, with dangling-alias
   retirement) — v1 as written referenced dead names; a `:1788` snapshot
   would carry an EMPTY recipe on the re-solve-exception path (:1685-1696
   clears the sets).
4. **Cap accounting**: commit-epochs ≠ evidence (data-borne culprits re-enter
   the recipe from fresh data and burn the cap on no-op erases).

The one salvage: if v2's stop rule fires (below), the next card is the
common-mode/datum-staleness mechanism — not a return to v1.

## §3 The design (v2)

Two independent default-OFF knobs, parsed beside the A4 knobs, both
inert-disarmed (warn) when `raim_withhold_sigma0` is 0.  Both are
fingerprint-EXEMPT — forward-only response class, the `relevel_free_epoch`
exemption family (8th instance; nothing serialized changes meaning).

**(a) `network.raim_withhold_adapt_escalate`** (int, default **0** = off;
accepted range ≥ `kMaxAdapt`, values in (0, 5] rejected-to-0 with warn).
When the era-continuity state says the PREVIOUS epoch was A4-withheld
(`wh_era_len ≥ 1`, C5/C7), the within-epoch adaptation loop's budget for
THIS epoch becomes the escalated value instead of `kMaxAdapt = 5`.  Nothing
else changes: same culprit scoring, same k-gate, same re-solve, same σ̂₀,
same ceiling compare.  If the escalated budget lands σ̂₀ ≤ ceiling the epoch
proceeds down the NORMAL emitted path (install, harvest, post-fit RAIM — the
§1.5 healing); if not, it withholds exactly as today.  Counters:
`n_wh_escalated_epochs`, `n_wh_escalate_landed` (escalated epoch that went on
to the accepted path), `n_wh_escalate_exhausted`.  Pre-registered arm value:
**12** (roughly kMaxAdapt + the observed 4-plateau recipe depth; NOT tuned
further this session).

**(b) `network.raim_withhold_free_erase`** (bool, default **false**;
additionally inert-disarmed without `cycle_slip.relevel_free_epoch`).  At the
A4 withhold branch (after the epoch-status/H/flight-recorder block, before
`SkipEpoch`): for every `(i, amb_key)` in `ctx.carry.relevel.free_ambs`
(detector-freed THIS epoch — a pre-solve, withhold-independent verdict),
`StateKeys::parseAmbiguityKey(amb_key)` → erase
`ctx.carry.local_carry[{i, sat, signal}]`, so carry-absence makes the free
re-level durable to the next accepted epoch (the §1.4 hole; erase-not-mark is
what is load-bearing — the pending-reseed mark was already consumed).
Bridged bands are already removed from `free_ambs` before this point, so a
delta-bridged carry can never be erased.  Counter `n_wh_free_erases`.

**Era state + edges** (`NetworkCarryState`: `wh_era_len`, `wh_last_tow`;
maintained ONLY at the A4 decision site): on a withheld decide with
`wh_era_len == 0` → event `SIGMA0/era_enter` + `n_wh_era_entries`; every
withheld decide advances `wh_era_len`/`wh_last_tow`; on a non-withheld decide
with `wh_era_len > 0` → event `SIGMA0/era_exit` carrying the era length and
the escalation/free-erase totals, teardown max-length stat, reset.  The C7
TOW-gap guard resets a stale era before the decision is evaluated.  Note the
adjacent recorder (`networkRhsTripEdge`, `:1888`) tracks the ceiling BREACH
era (carve-out-independent) — a deliberately different edge; the counters are
named `n_wh_*` to keep the two populations unconfusable.  All events and
counters are emitted only when at least one of the two knobs is armed (the
events CSV is in the byte-cert set).

**Declared limitations** (open-valve discipline, counters over silence):
`per_const_withhold` epochs count as era EXITS (the carve-out published), so
a per-constellation deadlock never escalates — declared, GPS-only lineage
unaffected, revisit with a multi-GNSS repro (`n_wh_perconst_exits` counts
them).  Station-culprit handling is unchanged from stock (exclusion,
epoch-local).  Committed-arc retap-streak interaction: none (v2 erases only
detector-freed arcs, which the retap path never owns).

## §4 Pre-registered validation gates (BEFORE the arms run)

Byte-identity: rerun `_fld63_relevel4h_{off,on}.json` UNCHANGED on the new
exe — products + the five solve CSVs (events included) byte-identical to the
`65ba362` runs (both knobs default-OFF and absent from the configs).  Suite
green with the new loader tests.

The armed arm (`_fld63_relevel4h_on` + `raim_withhold_sigma0` untouched +
`raim_withhold_adapt_escalate: 12` + `raim_withhold_free_erase: true`,
single knob-set over the certified pair):

- **G1 (the era)**: the 268890 era collapses 29 → **≤ 6** withheld epochs;
  no era in the run exceeds 6.  The expected mechanism is
  `n_wh_escalate_landed ≥ 1` inside it (an escalated EMIT, §1.5).
- **G2 (availability)**: `n_emitted ≥ 474`/483; total σ̂₀ withholds ≤ 12.
- **G3 (quality holds)**: settled-tail median post-fit phase σ̂₀ ≤ 1.35
  (armed-OFF: 1.274); T4 attribution-ARC rate ≤ 2.0e-2; PB disc marks
  equal-class (7).  An escalated-emit epoch is an `emitted_contaminated`
  class epoch — its quality is graded by these settled-tail aggregates, not
  per-epoch.
- **G4 (no cliff)**: no epoch with the fleet-expiry signature (pl prires
  rows = 0 while n_present > 50); run-max `prior_tension_ratio` < 1e6
  (armed-OFF hit 2.75e17 at the cliff).
- **G5 (verdict untouched, C3 check)**: every epoch up to and including the
  first era's SECOND epoch (the first escalated one) matches the certified
  ON run on `sigma0_first` and disposition-of-entry; divergence is legal
  only from the first escalated epoch onward.
- **G6 (counters, invariant #4)**: era enter/exit events present with
  lengths; `n_wh_escalated_epochs > 0`; `n_wh_free_erases` reported;
  escalate-landed + escalate-exhausted sum to the escalated count.
- **Stop rule**: if the escalated budget exhausts era-long
  (`n_wh_escalate_exhausted` ≈ era length, era length not collapsed), the
  budget-limited model is falsified — the residual is common-mode (the §2-R
  successor card's territory).  STOP, record, do not raise the budget and do
  not resurrect v1.

## §5 Implementation map

`GnssConfig.hpp` (2 getters), `GnssConfigLoadNetwork.cpp` (parse + 3
interlocks at the A4 parse site / end-of-section per the Card-1g order rule),
`NetworkRunContext.hpp` (`wh_era_len`, `wh_last_tow`, knob snapshot, 7
counters), `NetworkSolveGlue.cpp` (era-continuity resolve at the top of the
within-epoch block; budget swap at the loop bound; free-erase + era advance
in the withhold branch; era exit on the non-withheld decide),
`NetworkTeardown.cpp` (counter line).  Tests:
`tests/test_withhold_release_config.cpp` — defaults, the reject-to-0 band,
the three interlocks, counter-struct zero-init.

## §6 Filed collateral (not this card)

- `carry_driver.beginEpoch` re-inflates carried Pᶻ with a GROWING dt across
  an era (arithmetic-series over-inflation; `carry_prev_tow` freezes) —
  inert at default `qz ≤ 0`, real on armed-q lineages.
- The SH-gate half of item 7 (emission-only withhold with a distorted
  release statistic) — different mechanism, D-G rulings already written.
- The σ̂₀ = 1.0000 floor signature is explained (§1.3) — the §8.4
  "unexplained readout" closes by derivation, no code change.
- Stock `kMaxAdapt = 5` as a NON-era limitation (did the 268620 single trip
  itself need a deeper budget?) — out of scope; era-gated escalation only.

## §7 Results (2026-08-21, same session)

Provenance: exe MD5 E8F81856… (source ≡ snapshot, hashed at copy), suite green
with the 11-case `WithholdReleaseConfig` contract (one loader-behavior lesson
baked in: `relevel_free_epoch` itself disarms without `phase_leveling`, and the
erase half correctly cascades down — now pinned by
`FreeEraseDisarmsWhenLevelingDisarmsTheLatch`).  **OFF-path byte-cert PASS**:
knob-verbatim clones `_fld63_wh_bytecert_{off,on}.json` (product path
redirected — the 8b §5 overwrite trap avoided) reproduce the `65ba362`
reference runs byte-identically (products + events + closed-loop + solution +
ionosphere + epoch-status; `network_zwd.csv` absent on this posture).  Armed
arm `runs/fld63_relevel4h_wh` (`_fld63_relevel4h_wh.json`: escalate 12 +
free_erase over the certified ON leg).

| gate | pre-registered | measured | verdict |
|---|---|---|---|
| G1 era collapse | 29 → ≤ 6; none > 6 | **29 → 1; three 1-epoch eras, all `escalate_landed`** (268650: 12-peel lands σ̂₀ 3.29; 268920: 2.40; 273540: 2.44) | **PASS, decisive** |
| G2 availability | ≥ 474/483; ≤ 12 withholds | **478/483, 3 withholds** — beats BOTH references (armed-OFF 451/30, control-OFF 476/5); the 269400-class second event absorbed withhold-free | **PASS** |
| G3 settled σ̂₀ | ≤ 1.35 | **2.42 — FAIL** (attribution below) | **FAIL as registered** |
| G4 no cliff | run-max tension < 1e6; no pl-expiry | gate MIS-SPECIFIED — the 2.7e16 max is at TOW 259380, ignition epoch 7, INSIDE the byte-identical prefix (the baseline violates the gate as written).  Intent: post-settle max **4.8e5**, fleet pl-expiry epochs **0** (only the 7 ignition epochs have zero pl rows) | **intent PASS** |
| G5 verdict untouched | rows ≡ until first escalated epoch | **315/315 rows byte-equal before 268650; the shared 268620 entry row identical** | **PASS (C3 proven)** |
| G6 counters/events | both edges, closure | era enter/exit ×3 with identities; **3 escalated = 3 landed + 0 exhausted**; free-erase counter live at 0 (no detector-freed amb coincided with the 3 withheld epochs — a population statement, not a miswire); perconst-exits 0 | **PASS** |
| stop rule | exhausted ≈ era length ⇒ common-mode | **did not fire** — 0 exhausted | budget-limited model **CONFIRMED by intervention** |

**The G3 finding (the honest cost, hour-by-hour medians of emitted pf phase
σ̂₀):** h1/h2 identical across arms (1.389/2.166 — shared prefix); h3 wh 1.906
vs armed-OFF 1.608 vs control 2.091; h4 wh **3.343** vs armed-OFF 1.204 vs
control 1.934.  Two effects compound: (1) armed-OFF's reference number is
CLIFF-FLATTERED — its 29 sick epochs were withheld out of the median and its
post-cliff fleet sat 32 epochs at the σ̂₀ floor on mass-re-leveled carries, so
1.274 is not a fair bar; but (2) wh also runs hotter than CONTROL (2.42 vs
2.08), so the flattery does not explain everything: **the contaminated and
event-re-leveled carry population that armed-OFF's cliff amputated persists on
the wh arm and pays out slowly under T4 attrition** (T4 ARC rate 1.93e-2,
between the two references; the wh arm even shows its own 5-epoch σ̂₀-floor
stretch at 269010–269130 — the 12 dropped culprits plus T4 refloats
re-leveling a large sub-fleet without any expiry).  The cliff, for all its
brutality, was performing real work: amputating the stressed zero-q carries.
The escalation removes the amputation without replacing the therapy — the
residual quality cost is the SAME mid-arc zero-q representation defect that
owns the 8(b) plateau (`SPEC_iono_carry_8b.md` §6.6), surfacing on a third
instrument.

**Disposition: mechanism VALIDATED, promotion REFUSED.**  The deadlock is
dead (G1/G2/G5/G6), the budget-limited model is confirmed, and the knobs are
safe (verdict-untouched, byte-certified OFF) — but the pre-registered quality
gate failed, so `raim_withhold_adapt_escalate` + `raim_withhold_free_erase`
stay **default-OFF research knobs**; no promotion-ledger entry beyond
UNVALIDATED→SCOPED-research.  Arming them is the right call ONLY where
availability outranks settled σ̂₀ (the RT/service posture — rt_service_
operations' own doctrine), and that call belongs to a service-posture card
with its own consumer-side grading, not to this one.  The free-erase half
remains unexercised by this fixture (counter 0 — the §1.4 hole needs an era
long enough to contain a detector slip, which the escalation now prevents);
it stays for the day a slip lands ON the (now single) withheld epoch.

## §8 The successor card's PREMISE fails its own audit — the retirement census (2026-08-22)

§7 refused promotion on G3 and attributed the residual to a therapy the cliff
had been performing: *"The cliff, for all its brutality, was performing real
work: amputating the stressed zero-q carries.  The escalation removes the
amputation without replacing it."*  The obvious successor card is a targeted,
bounded replacement for that amputation.  Working it surfaced three independent
defects in the premise, each of which has to be closed before ANY mechanism is
justified.  This increment therefore builds the instrument, not the mechanism.

### §8.1 Every G3 number is on a RETIRED binary

`runs/fld63_relevel4h_{off,on,wh}` were produced 2026-08-21 at 00:05 / 00:08 /
09:17.  The carrier-phase wind-up **SIGN fix** landed the same evening
(`4f49fa2` 21:26 + consumer-gap closure `0d25aa0` 21:40) and is documented
solve-affecting on field data (H-RMS 3.18 → 1.30 cm).  Under standing invariant
#6, `wh`'s 2.42 versus anything measured on the post-fix build is a cross-build
comparison and inadmissible.  **The post-fix escalated arm has never been run.**

The post-fix control DOES exist — `runs/fld63_relevel4h_ctlw`
(`config/test_suite/_fld63_relevel4h_ctlw.json`, completed 2026-08-22 08:57),
knob-verbatim against the certified ON leg.  On the current build that same
config yields **453 emitted / 28 withheld, eras of 20 and 7 epochs, settled-tail
median emitted post-fit phase σ̂₀ 2.090, and ZERO leveling-fleet-expiry epochs.**
The deadlock reproduces — the fixture survives the binary change — but **the
cliff does not occur at all**, so the arm whose behaviour the therapy premise is
built on is not reproducible on the build any successor would run.

### §8.2 The old reference is DISQUALIFIED, not merely flattered

§7 already called armed-OFF's 1.274 "cliff-flattered".  It is worse than that.
Settled tail (emitted), `fld63_relevel4h_on`: **11 of 211 epochs (5.2 %) sit at
σ̂₀_pf_phase < 0.10**, and 162 epochs report σ̂₀ ≡ 1.0000 exactly; the off / wh /
ctlw legs have **0.0 %** below 0.10.  The cliff epoch itself emits at σ̂₀ 1.0000
with **σ̂₀_pf_phase = 0.0000 on 2 372 post-fit rows** — a post-fit phase residual
of identically zero is the free-ambiguity signature, i.e. the statistic went
**vacuous**, not good.  Excluding only the sub-0.10 epochs moves 1.274 → 1.346,
so the distortion is a sustained ~1 h post-cliff regime, not a few outliers.

### §8.3 σ̂₀_pf_phase is blind to the cost of a mass re-level

CLAUDE.md invariant #1's re-coupling clause already says a one-epoch code
re-level walks the level *"with every internal instrument silent"*.  On an
availability-matched paired contrast of the SH `a₀₀` channel over the settled
tail, the arm G3 called BEST carries the LARGEST level drift and the arm G3
FAILED roughly halves it.  ⚠ That contrast is itself cross-build (§8.1) and
`a₀₀` is diagnostic-class, so it is an arm-ranking hint, **not** a graded
result — it is recorded here as a reason to distrust a single-instrument verdict,
not as a verdict.  The general point stands on its own: **G3 graded a
level-affecting intervention on the one channel that cannot see level error.**

### §8.4 What the "cliff" actually is — and what landed

The obvious suspect is the post-harvest prune
`if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = local_carry.erase(it);`
(`NetworkRunContext.cpp`).  **It is not the cliff, and it cannot be.**  The
harvest refreshes `last_seen_s` for every arc it stashes and runs immediately
before the prune in the same block, so the prune's candidate set is by
construction DISJOINT from the arcs observed this epoch: it can only retire arcs
that were not harvested at all — set satellites, absent stations, harvest
failures.  It records **departures**, and it is structurally incapable of
amputating a carry that is being observed.

The actual mechanism is one seam earlier and erases nothing:

```
if (gap > pl_max_gap_s) continue;      // ApplicationNetworkAssembly, the inject
```

The carried prior is simply **not injected**.  The arc solves FREE for that
epoch and the harvest then re-stashes it from one epoch of code.  That is
exactly the observable §1.3 reported — `pl prires rows 753 → 0`, since a prires
row exists only where a prior folded — and it is why nothing in the carry map
looks amputated afterwards.  During a withhold era the epoch returns `SkipEpoch`
textually above the leveling seam, so `last_seen_s` freezes while `pl_now_s`
advances and the whole fleet crosses the threshold on ONE epoch.  At 900 s / 30 s
that is era length 30; ON's era was 29 and the 30th epoch crossed, while ctlw's
20-epoch era never reached it and exited by EMIT.

**Calling a 30-epoch timer "therapy" requires evidence that it selects on
quality.  It selects on elapsed time**, and what it does to the selected arcs is
force a one-epoch code re-level — which is the standing invariant-#1
re-coupling mechanism, i.e. the known *walk* generator, not a known cure.

Landed instead of a mechanism — the **leveling-carry retirement census**
(`n_cr_*` on `NetworkServiceCounters` plus one `SlipQcCounters` field; always on,
pure counters, no CSV row, no solve effect), reported as **four families that are
never summed**, because their units are not commensurable:

| family | what it costs | channels |
|---|---|---|
| **(B) THE CLIFF** — inject-side stale skip | nothing erased; one epoch of FREE solve, then a code re-level | `n_cr_inject_stale` (+ epochs, max, week/TOW, **distinct stations**) |
| **(A) departures** — permanent | the arc is gone | `n_cr_gap_prune` (+ epochs, max, week/TOW, fleet-before), `n_cr_sh_conv_flush` (whole-fleet one-shot at SH convergence — the largest single removal in the producer), `n_cr_zwd_gap_prune`, `n_cr_rpb_gap_prune` |
| **(C) one-solve refloats** — quality-selected | one solve, next epoch | `n_cr_raim_attrib` (T4), `n_cr_raim_consensus` (T3) |
| **(D) one-solve frame skips** | one solve of continuity; the same epoch re-stashes any still-tracked arc | `n_cr_eq34_reref`, `n_cr_phase_datum_reref`, `datum_slip_carry_erases` |
| **response** | — | `n_wh_free_erases` |

Three details that decide whether a reading is trustworthy:

- **(C) is a run-constant either/or, not a split.**  `raim_attribution` is a
  config read, so exactly one of the two channels is reachable for a whole run;
  the other's zero means "path not armed", never "path found nothing".  The
  run-end line prints which arm is live.
- **The attribution handle is week + TOW**, not continuous GPS seconds, so it
  pastes straight into a trace filter.  The cliff channel additionally records
  the **number of distinct stations** in its worst epoch — a fleet-wide era and
  a single station dropping out produce similar counts and completely different
  conclusions.
- **The fleet-before denominator is taken after the harvest**, so the prune's
  percentage is `departed / (departed + refreshed-live)` — availability, not
  carry health.  It is not a "fraction amputated" and the run-end line says so.

### §8.5 The ordered next steps (none of them a new mechanism)

1. **Attribute the event.** Re-run the certified ON config on the current exe
   with the census live and read the four families at the cliff epoch.  The
   timer reading predicts a single-epoch `n_cr_inject_stale` near the carried
   fleet, spanning MANY stations, with family (A) small — the carry is not
   erased, only bypassed.  If instead a quality channel (C) dominates, the
   therapy premise gets its first actual support.  If (A) carries the mass, the
   arcs genuinely departed and neither reading holds.
2. **Re-run the escalated arm post-fix** (`escalate: 12` + `free_erase` over
   ctlw), pinning the exe hash AND every `config_extends[].hash_fnv1a64` from
   the run manifest — §7's byte-cert pinned the exe only, and the config CHAIN
   is what demonstrably drifted between arms.  Grade it against **ctlw's
   measured 2.090**, which is the only same-build reference that exists.
3. **Only then** decide whether a targeted retirement is needed.  If it is, the
   per-entry statistic already exists and is correctly sited — the standardized
   prior-residual row `(e, fold_var, post_var) → (r, s, t)` built one statement
   before the carry is overwritten — but note it has **no knee and no small
   tail**: ~18.5 % of rows sit above 4σ̂₀ every epoch, so a 4σ̂₀ retirement
   erases ~139 entries EVERY epoch against the cliff's 753 ONCE, roughly 40× the
   amortized blast radius.  Every threshold on that ladder is a knob, not a
   physics boundary.  **§2-R's objection 2 applies to any successor that keys
   off it.**

**Disposition unchanged from §7** (`raim_withhold_adapt_escalate` +
`raim_withhold_free_erase` remain default-OFF research knobs); what changes is
that the successor card is **BLOCKED on §8.5 steps 1–2**, and the "cliff as
therapy" reading is downgraded from a finding to an untested hypothesis with a
disqualified evidence base.

## §9 §8.5 step 1 EXECUTED — the cliff does NOT reproduce, and the timer model is confirmed

`runs/_wd/whattrib` (`_fld63_wh_attrib4h.json` = the certified ON config,
knob-verbatim, product path redirected; exe `92FC13A7…`, post-wind-up-fix).

**Availability and eras (current binary):** **453 emitted / 28 withheld**, eras of
**1, 20 and 7** epochs.  The 20-epoch era runs TOW 269190 → 269760.

**The §1.3 cliff does not occur.**  20 epochs × 30 s = 600 s never reaches the
900 s `phase_leveling_max_gap_s` threshold, so the fleet never crosses it; the era
ends at 269760 by **EMIT**, exactly as §1.5 describes the healthy exit.  This is a
direct confirmation of §8.4's timer model — the crossing requires a ≥ 30-epoch
era, and 20 < 30.

**The retirement census attributes the epoch precisely.**  Its worst inject-side
stale event is at **week 2313 TOW 269760.0 — the very epoch §1.3 names** — but it
is **21 carried priors across 2 stations**, against an 853-entry carry.  By this
card's own pre-registered reading, a count concentrated on two station indices is
a station dropout, **not** a fleet-wide mass re-level.  The whole-run cliff
channel is 139 skips over 11 epochs.

**And the cliff was never a major retirement channel on this posture:**

| family | count (4 h, 61 sta) |
|---|---|
| [C] T4 attribution refloats | **6 564** |
| [D] eq34 chain-datum re-reference | **6 107** |
| [A] departures (900 s prune) | 528 (worst 24 of 853 = 2.8 %) |
| [B] **the cliff** (inject-side stale) | **139** (worst 21, 2 stations) |
| [A] SH-convergence flush | 0 |
| [D] phase-datum / datum-slip re-reference | 0 / 0 |
| [RESPONSE] A4 free-erase | 0 |

T4 attribution and the eq34 re-reference each retire ~47× what the cliff does.
Any claim that the cliff was performing load-bearing therapy has to explain how a
channel two orders of magnitude smaller than T4 — and quality-blind — was doing
the work.

**Method note:** `_fld63_relevel4h_ctlw` and `_fld63_relevel4h_on` were confirmed
to differ ONLY in output paths, a snapshot path and `trace_dd_fractional`, i.e.
they are the same solve posture.  The census arm and the attribution arm therefore
returned byte-identical counters, which is a determinism cross-check rather than a
coincidence.

**Consequence for the successor card:** §8's premise is not merely unsupported, its
motivating EVENT is absent on the current binary.  A targeted-retirement mechanism
has nothing to target here.  The remaining live question is §8.5 step 2 — whether
the escalation's quality cost is real once there is no cliff to remove.

### §9.1 The honest reference for that test

Same exe, same posture, measured: settled tail (TOW ≥ 266400, emitted only)
median `sigma0_pf_phase` = **2.0902** over 213 epochs, with **zero** vacuous
(< 0.10) epochs in that tail — 19 such epochs exist in the run but all fall in
ignition.  This is the bar §7 should have graded against; its 1.274 came from the
cliff-flattered leg (§8.2).

## §10 §8.5 step 2 EXECUTED — G3 does NOT reproduce; the promotion refusal is re-opened

`runs/_wd/whescal` (`_fld63_wh_escal_postfix.json` = `_fld63_relevel4h_wh.json`
knob-verbatim, product path redirected; same exe `92FC13A7…` as the §9 reference,
so this is a same-build pair — the comparison §7 could not make).

| | ON reference (§9) | ESCALATED | §7's pre-fix figures |
|---|---|---|---|
| emitted / withheld | 453 / 28 | **476 / 5** | 451 / 30 → 478 / 3 |
| eras | 1, 20, 7 | **five 1-epoch** | 29 → three 1-epoch |
| escalated → landed / exhausted | — | **5 → 5 / 0** | 3 → 3 / 0 |
| free-erase count | — | **2** | 0 (unexercised) |
| settled median σ̂₀_pf_phase | 2.0902 | 2.2117 | 1.274 → 2.42 |

**Gate (1): PASS, decisively.** The 20-epoch era collapses to isolated
single-epoch withholds; total withholds 28 → 5; availability +23 epochs (+5.1 %).
Every escalated epoch LANDED (5/5, 0 exhausted), so the budget-limited model is
confirmed a second time, on a different binary.  The stop rule did not fire.  The
free-erase half — recorded in §7 as unexercised because the escalation prevented
any era long enough to contain a detector slip — **fired twice here**, so that
channel is no longer unvalidated wiring.

**Gate (2): §7's quality objection DOES NOT REPRODUCE.**  The whole-population
medians (2.0902 → 2.2117, +5.8 %) compare DIFFERENT POPULATIONS — the escalated
arm emits 23 epochs the reference withheld — which is the same flattery error
§8.2 identified, running in the opposite direction.  Availability-matched, on the
**213 settled epochs both arms emitted**:

```
ON  median sigma0_pf_phase = 2.0902
ESC median sigma0_pf_phase = 2.1168
paired median delta        = +0.0000
```

**A paired median delta of exactly zero.**  On the epochs that were already being
emitted, the escalation changes nothing — consistent with §7's own G5 proof that
the verdict is byte-untouched until the first escalated epoch, and with the C5
single-trip rule.  The entire whole-population shift is the arithmetic of adding
the 23 recovered epochs, whose own median σ̂₀ is **3.6781** (max 6.2207): they are
the sick epochs, exactly as the `emitted_contaminated` class in §3 anticipated.

⇒ **§7's attribution was wrong, and its verdict rested on it.**  §7 read 2.42 vs
1.274 as the escalation degrading the settled solution because it had removed the
cliff's therapy.  On the current binary there is no cliff (§9), and the
availability-matched cost to the healthy population is zero.  The 2.42 was an
artifact of a retired binary graded against a cliff-flattered reference.

### §10.1 What this does and does NOT license

**Does:** re-open the promotion question.  The measured trade on this posture is
**+23 epochs of product (+5.1 % availability) at zero cost to the epochs already
being emitted**, with the recovered epochs themselves noisy (median σ̂₀ 3.68 vs
2.09).  That is a consumer-side decision — gap versus degraded epoch — and it is
the RT/service-posture call §7's disposition already named, now with the
"it also poisons the good epochs" objection removed.

**Does NOT:** license a blanket promotion.  Three limits, stated:

1. **σ̂₀_pf_phase is level-blind** (§8.3, and invariant #1's own re-coupling
   clause).  This pair was not graded on a level-aware channel, so "zero paired
   cost" means zero on THIS instrument, not zero.  A level contrast on the
   same-build pair is the owed measurement.
2. **One 4 h run, one posture** — the same limitation §7 had.
3. The **23 recovered epochs are genuinely worse**, and a consumer that cannot
   tell an `emitted_contaminated` epoch from a clean one inherits that silently.
   The class exists in the epoch-status record; whether the consumer reads it is
   a separate contract question.

**Disposition: UNVALIDATED→SCOPED-research stands, but the stated REASON changes.**
The knobs are not held back by a measured quality cost — that cost did not
reproduce.  They are held back pending (1) a level-aware grade and (3) the
consumer contract.  The §2-R successor mechanism remains DEAD: with no cliff and
no measured therapy, there is nothing for it to replace.
## §11 §10.1 limit (1) EXECUTED — the LEVEL-aware grade (2026-08-23)

Analysis-only, on the existing same-build pair (`runs/_wd/whattrib` /
`runs/_wd/whescal` — the arms' own CWDs; the `runs/fld63_wh_*` dirs hold only
products).  Both arms already carried `logs/network_vtec_sh.csv` (a00 = the
`n=0,m=0,trig=C` row; 81 rows/emitted epoch — row counts 453×81 and 476×81
match the dispositions exactly), so no synthesis was needed, and
`network_ionosphere.csv` was not touched (frame-broken, amendment 4).  GIM was
used DIAGNOSTICS-ONLY per amendment 6; the grade is the paired arm-vs-arm
contrast.

**Amendment-6 compliance**: 4 h run ≥ the 2 h convergence budget; the contrast
is confined to the settled tail TOW ≥ 266400 (241 ep = 2.00 h wall; 213
commonly-emitted).  Paired deltas are EXACTLY 0.0 on all 74 settled common
epochs before the first post-shared-withhold divergence at TOW 268650 — G5
byte-identity independently confirmed on the level channel — so the informative
window is the 139 post-divergence common epochs (1.38 h; a property of when the
first withhold fell, not of run length).

### §11.1 The level gate on commonly-emitted epochs: PASS

Paired a00 (esc − att, TECU): all-213 median **+0.0000** (mean −0.110,
p16/p84 −1.415/+0.971); post-divergence-139 median **−0.222**
(delta-of-medians −0.546), p16/p84 −2.430/+1.948, halves −0.72/+0.16 —
bounded breathing, no ramp.  Context: each arm's own 30-s |Δa00| median is
0.49 TECU (p95 3.3–4.2), and the −0.55 TECU shift is ~10× under the posture's
documented −4…−15 wander band.  σ̂₀ cross-check reproduces §10 exactly
(2.0902 / 2.1168 / paired +0.0000).  **§10.1 limit (1) is discharged: "zero
paired cost" now holds on a level-aware channel at ~0.5 TECU resolution.**

### §11.2 The recovered epochs SPLIT — and era-3 is the new finding

- **Era-2's 16 (TOW 269220–269760): level-CLEAN.**  d_local vs the 5 nearest
  clean emitted neighbors each side: median −0.26, max |1.37| TECU — inside
  the clean-epoch baseline (p16/p84 −0.80/+0.82).  σ̂₀_pf_phase decays
  4.44 → 1.06 across the era (median 3.6781 = §10's number).
- **Era-3's 7 (TOW 273270–273450): level-FAIL, and INSTRUMENT-SILENT.**
  Emitted a00 22.11–33.63 vs a local trend of ~43.6 — **d_local −9.96 to
  −21.48 TECU**, a dip-and-recover of −17/+14 TECU inside 4 minutes,
  physically impossible as global-mean ionosphere.  In the ESC arm these
  epochs **never tripped**: pre-fit σ0 2.0–3.0 (below the 3.0 trigger),
  n_adapt 0–1, σ̂₀_pf_phase 2.89–4.23, formal a00 std unchanged (0.6235 vs
  0.6234), no datum change — while the ATT arm at the same TOWs read σ0
  **14.5–20.2**, exhausted 5 adaptations, and withheld.  The era-2 escalation
  changed the trajectory such that the era-3 event expressed as an UNDETECTED
  EMITTED LEVEL EXCURSION instead of a detected withhold — §8.3's
  level-blindness made concrete.
- **σ̂₀ ANTI-RANKS level health**: the five worst-level epochs (−21.5…−14.1)
  carry σ̂₀ 3.10–4.19, all BELOW era-2's level-clean 269430 (σ̂₀ 6.22).  A
  consumer downweighting by σ̂₀ keeps the poisoned epochs and drops the clean
  ones.
- **A NEW classification gap**: the 7 era-3 epochs are contaminated but carry
  no `emitted_contaminated` class — that flag rides the trigger, and they
  never tripped.  The consumer contract cannot be satisfied by σ̂₀ thresholds
  OR by the class flag as built.
- Post-era: paired deltas +4.83, +3.28, +3.95, −3.56, −8.85 over the final 5
  epochs, the att arm's a00 climbing 38.7 → 54.6 in 2 min — the run ends
  150 s after era-3, MID-EVENT.  Re-convergence is unobserved.

### §11.3 Disposition

**The level grade is no longer the blocker; the consumer contract is — with a
concrete, measured failure mode.**  Promotion of `escalate=12 + free_erase`
stays blocked on §10.1 limit (3), now sharpened: the recovered class is not
merely "worse by σ̂₀ 3.68" — it can be level-toxic at −20 TECU while silent on
every armed instrument, and σ̂₀ mis-orders the two classes.  One limb is NOT
closable on this pair: era-3's post-era re-convergence (and its cost) needs a
window extending ≥1 h past TOW 273600, or a later start.  Gate (1) does not.
Scratch analysis: `wh_level_grade.py` (session scratchpad; numbers above are
the record).


### §11.4 The 6 h pair EXECUTED — era-3 answered, and two findings §11 did not predict (2026-08-23)

`_fld63_wh_attrib6h` / `_fld63_wh_escal6h` (knob-verbatim 6 h clones, same-build
pair on the §7.8-certified exe; `runs/_wd/whattrib6h` / `whescal6h`).
**Hours 0–4 reproduce the 4 h record EXACTLY across the build change** — era
TOWs identical (1/20/7), σ0 identical to six decimals (33.378601 at 268620),
a00 identical at matched settled TOWs, the same +23 recovered epochs.

**Era structure (6 h):** ATT (reference, 720 epochs TOW 259200-280770, 661 emitted / 59 sigma0_withheld): era-1 268620 (1 ep, sigma0 33.38), era-2 269190-269760 (20 ep, sigma0 27.6-30.5), era-3 273270-273450 (7 ep, sigma0 14.51-20.21, all 5 adaptations exhausted), era-4 275400 (1 ep, sigma0 7.78), era-5 277260-278130 (30 ep, sigma0 7.2 peaking 23.8). ESC (683 emitted / 37 withheld, 8 era_enter/8 era_exit per run summary): five 1-ep eras in hours 0-4 (268620, 269190, 269250, 269310, 269370 — each exits 'escalate_landed' after exactly 1 withheld epoch), then 275070 (1), 276150 (1), and era-8 278220-279090 (30 ep, n_adapt climbing 3->12, 29 escalation-exhausted epochs). HOURS 0-4 REPRODUCE THE 4 H RECORD EXACTLY despite the cross-build: identical era TOWs (1/20/7 at 268620 / 269190-269760 / 273270-273450), identical sigma0 to 6 decimals (33.378601 at 268620; 14.513363 at 273270), identical a00 at matched settled TOWs (37.2403 +/- 0.6253 at 266400 in both builds), and the same +23 recovered epochs in hours 0-4 (16 of era-2's 20 + all 7 of era-3) — zero cross-build drift detected. HOURS 4-6 NEW: the reference's +31 withholds are 275400 plus a 30-ep storm era 277260-278130; the escal arm postpones the storm era by 960 s (it emits ATT's whole era-5 window by adapting 4-5x/epoch, 28/30 of those epochs T2-flagged emitted_contaminated) but then takes its OWN 30-ep era at 278220-279090 that even 12 re-solves cannot land. ERA-3 RECURS at exactly 273270-273450 in the reference and is again fully emitted-through (never a withhold) in the escal arm. EXIT MODE OF BOTH 30-EP ERAS IS THE 900 s CLIFF, NOT RECOVERY: 30 ep x 30 s = phase_leveling_max_gap_s = 900, and both run summaries log the section-1.3 mass re-level at the exit epoch (ATT: 535 carried priors inject-skipped across 61 stations at TOW 278160; ESC: 556 across 61 stations at 279120); the first post-era epoch emits with sigma0 pinned at exactly 1.0000 and sigma0_pf_phase 0.000-0.02 (zero-redundancy fresh-relevel signature), sigma0 staying ~1.0 for ~15 min after. The ESC era_exit row's 'escalate_landed / 30 withheld' is coincident with — and actually caused by — the carry expiry.

**Era-3 re-convergence:** ERA-3 ANSWER: yes, both arms re-converge, fast, but by different paths and with a persistent low-level decorrelation afterward. ESC arm (emitted through the event): the dip bottoms at 22.11 TECU (TOW 273360, d_local -21.48) and is already back on trend INSIDE the era window — 273480 (one epoch after era end) reads 43.58 vs local trend 43.6-44.7 (d_local -1.10); one +5.6 breathing spike at 273540, formal a00 std flat at 0.588-0.594 throughout. ATT arm (withheld through the event): resumes at 273480 still LOW (38.75, own-arm d_local -6.62), climbs 38.75 -> 54.59 over 5 epochs — the 4 h record's 38.7->54.6 climb was the front half of an under-then-overshoot — peaking at d_local +7.71 at 273600, then snaps back to trend at 273630 (46.96, d_local -0.97). So ATT re-convergence: ~6 epochs (3 min) with a -6.6/+7.7 TECU under/overshoot; ESC: <=1 epoch, no overshoot beyond its +/-2 breathing. PAIRED DELTA (esc-att, common emitted): first sustained |delta|<1.5 TECU (5 consecutive ep) at TOW 274020 — 19 ep (9.5 min) after era end; but agreement does not re-lock to the hours-2-4 regime: a second +5..+7.5 divergence appears 274290-274500 with neither arm withholding. HOURS 4-6 PAIRED STATS (177 common emitted, all nonzero): median -2.54, mean -1.17, p16/p84 -7.89/+6.15, min/max -14.7/+12.6; per-30-min medians +4.76 / -2.22 / +0.16 / -7.39 — event-driven decorrelation, not a monotone drift. The -7.4 tail (n=56 epochs after both resets, TOW>=279120: median -7.39, p16/p84 -10.9/-3.6) is dominated by the ASYNCHRONOUS mass re-levels (ATT re-levels at 278160, ESC at 279120): each reset re-seeds all zero-q levels from one epoch of code and the two arms land ~7 TECU apart (ATT tail ~45-46, ESC ~38-39, pre-storm trend ~40-44; which is right is unadjudicated — no external reference used per method rules). Cost framing: the 4-6 h paired divergence is not attributable to escalation as a level 'cost' — it is trajectory decorrelation from divergent era/reset histories in an active field, and the per-arm 30 s |dA00| breathing itself doubles in hours 4-6 (ATT median 0.49->0.81, p95 3.1->4.9; ESC 0.49->0.66, p95 4.2->3.8).

**Recovered-epoch classes (6 h):** 54 recovered (esc emitted where att withheld; run totals 683 vs 661 emitted = net +22 after the 32 anti-recovered). ESC-arm d_local (5 nearest clean emitted neighbors each side, dispute epochs excluded; clean-epoch baseline n=144: median +0.005, p16/p84 -0.91/+0.64, p2.5/97.5 -2.69/+2.35, max |5.98|). CLASSES: (a) era-2's 16 (269220-269760): LEVEL-CLEAN — d_local median -0.4, max |1.37|, sigma0 decaying 4.14->1.09, sigma0_pf_phase 6.22->1.06; reproduces the 4 h record. (b) era-3's 7 (273270-273450): LEVEL-TOXIC AND STILL CONSUMER-SILENT — d_local -9.96..-21.48 TECU (numerically identical to the 4 h record), final sigma0 2.27-2.97 (below the 3.0 trigger), n_adapt 0-1, sigma0_pf_phase 2.89-4.23, formal a00 std unchanged (0.588-0.594), ZERO emitted_contaminated rows. One sharpening vs the 4 h 'never tripped' wording: the within-epoch T2 tier did act 3 times (we_refloat_sat G17 10.83->2.27 at 273270; we_refloat_arc G12 at 273330 and 273450) and those refloats ABSORBED the trigger — measure/refloat rows only, nothing a consumer sees; the A4 trigger never crossed and the class flag never set. (c) 275400 single: clean (+1.48). (d) era-5's 30 (277260-278130): 28 clean-to-marginal (d_local within +/-3.2) plus 2 toxic-by-threshold — 277380 (-7.01) and 277410 (-6.39) — but this class is NOT silent: n_adapt 4-5 on nearly every epoch, final sigma0 3.0-5.8, and 28/30 carry T2 emitted_contaminated flags (277380 flagged; 277410 is one of the 2 unflagged). ANTI-RECOVERED (32: att emitted where esc withheld) — the new symmetry finding: the REFERENCE arm silently emits its own level-toxic epochs. 276150: ATT d_local -11.42 at sigma0 2.73, n_adapt 2 — an instrument-silent toxic emission in the STOCK arm, same signature as era-3. And ATT's post-reset epochs 278490-278610 run d_local +5.7..+12.7 with sigma0 pinned at/near the exact-1.0 zero-redundancy sentinel and n_adapt 0 — the mass re-level transient is level-hot and structurally invisible to sigma0. T2 totals: ATT 52 emitted_contaminated rows, ESC 72.

**Level gate re-stated:** Re-run per section-11.1 method (paired esc-att a00 on commonly-emitted settled epochs, TOW>=266400; GIM untouched; network_ionosphere.csv untouched). HOURS 2-4 (212 common): median +0.0000, p16/p84 -1.380/+0.974 — reproduces section 11.1's PASS numbers (record: +0.0000, -1.415/+0.971) with byte-zero deltas on all pre-268650 epochs (first nonzero paired delta at TOW 268650, same divergence epoch as the 4 h record). FULL 6 h SETTLED TAIL (389 common): median +0.0000, mean -0.57, p16/p84 -4.58/+3.95, p05/p95 -9.4/+7.1, halves medians +0.0000/-2.23; post-divergence 315: median -0.78, p16/p84 -5.23/+5.10. Paired sigma0_pf delta median +0.0000 (ATT settled median 3.124, ESC 3.586). VERDICT: the hours-2-4 gate PASSES identically, but the section-11.1 'zero paired cost at ~0.5 TECU resolution' claim DOES NOT EXTEND to hours 4-6: the paired contrast widens to +/-5-8 TECU with a -7.4 TECU tail offset after the two asynchronous 900-s mass re-levels. However the widening is symmetric-cause (storm + divergent reset timing), sits inside the posture's documented -4..-15 wander band and inside each arm's own doubled breathing, shows no monotone ramp, and the reference arm cannot serve as a level truth there (it resets FIRST and its post-reset epochs are themselves +12.7 TECU hot at sigma0=1.0). The gate as an escalation-cost instrument is CONFOUNDED once era histories diverge enough to move reset timing; on the windows where the arms share an era history it still reads zero cost.

**What changes §11's reading:**
1. **Withholding through era-3 was WORSE for the level than emitting through
   it** — the reference arm resumed −6.6 low and overshot +7.7 over 6 epochs
   while the escalation arm was back on trend inside one epoch.  The
   adapt-and-EMIT doctrine gets direct level-channel support.
2. **Instrument-silent level toxicity is PRODUCER-WIDE, not an escalation
   cost**: the stock reference arm silently emitted −11.4 TECU at σ0 2.73
   (TOW 276150) and its post-reset epochs ran +5.7…+12.7 TECU hot at the
   σ0 = 1.0 zero-redundancy sentinel.  The §12 channel is owed by the
   producer, not by the escalation knob.
3. **The 900-s cliff is BACK in hours 4–6** — both arms' 30-epoch storm eras
   exit by carry expiry (mass re-level of 535/556 priors), not recovery, and
   the post-reset transient is itself level-hot and σ0-invisible.  §9's
   "cliff does not reproduce" was a property of the 4 h window, not of the
   posture.
4. Era-3's silence sharpens: the within-epoch T2 tier DID act (3 refloats
   that ABSORBED the trigger) — measure/refloat rows only, nothing a
   consumer sees.

## §12 The WITHHOLD CONSUMER CONTRACT — the level-health channel (`network.emit_level_health`)

**Session 2026-08-23.  Status: DERIVED (this card); implementation + validation
pre-registered below.  Owner of §10.1 limit (3) / the §11.3 blocker.  Evidence
base: the same-build 6 h pair (`runs/_wd/whattrib` / `runs/_wd/whescal`, exe
`92FC13A7…`), whose recovered-epoch split, anti-ranking measurement, and clean-
epoch baseline are this card's design inputs and its labeled truth set.**

### §12.1 The contract gap, named exactly (all measured on the 6 h pair)

A consumer of this product today has NO channel that ranks level health:

1. **σ̂₀ ANTI-RANKS it.**  The five worst-level epochs (−21.5…−14.1 TECU)
   carry σ̂₀ 3.10–4.19, all BELOW era-2's level-clean 269430 (σ̂₀ 6.22).  A
   consumer downweighting by σ̂₀ keeps the poisoned epochs and drops clean ones.
2. **The `emitted_contaminated` class rides the trigger AND lives only in the
   events trace** (`NetworkRaim.cpp:889` — a T2 event row, gated in practice
   on the events CSV being open).  Era-3's 7 toxic epochs never crossed the
   3.0 trigger (final σ̂₀ 2.27–2.97; the within-epoch T2 refloats ABSORBED it
   — 3 `we_refloat_*` actions, measure/refloat rows only), so they carry no
   class anywhere, and even the classified epochs are invisible to a product
   consumer.  Two defects: trigger-blind AND trace-gated.
3. **Every other armed instrument is silent through a −21 TECU excursion**:
   formal a00 std unchanged (0.588–0.594), no datum flag, n_adapt 0–1.
4. **The blindness is POSTURE-WIDE, not an escalation defect**: the stock
   reference arm silently emitted its own toxic epoch (276150, d_local
   −11.42, σ̂₀ 2.73), and both arms' post-mass-re-level epochs run +5.7…+12.7
   TECU hot with σ̂₀ pinned at the exact-1.0 zero-redundancy sentinel and no
   marking of any kind (the §11-caveat open limb).

Constraint set carried in (each pre-paid): GIM/external data is diagnostics-
only — the instrument must be SELF-referenced; the level VALUE stays untracked
(LV-6) — this card FLAGS and must not fix, and must not touch the rx-CB
carry's memory in either direction (both falsified); consumer SUPERSEDES never
adds (M4 v2); never gate a DECLARATION on a QC/trace knob (legend card);
emit-side additions fingerprint-SAFE or explicitly fingerprinted; adapt-and-
EMIT — a contract that blanket-rejects the recovered class re-creates the
withhold (16 of 23 recovered epochs were level-clean and must stay usable);
invariant #7 (both edges, counters, identities); invariant #4 (armed-but-inert
must be diagnosable at run end).

### §12.2 The mechanism — one channel, three classes, causal + retro-confirm

**What is measured.**  After each ACCEPTED solve, the producer reads its own
co-estimated global-mean ionosphere `a00 = x_s(vtecCoeffColumn(0,0,'C',L1))`
— the identical quantity `network_vtec_sh.csv` records and §11 graded on —
and forms the CAUSAL self-trend deviation

    d_causal(t) = a00(t) − median{ a00(t_i) }

over the last `K = level_health_ref_epochs` (default 5) EMITTED epochs whose
own class was `ok`, with content age ≤ `level_health_ref_max_age_s` (default
900 s).  Self-exclusion of flagged epochs is load-bearing: without it the
reference tracks the dip and the recovery false-flags at +14 TECU; with it
the era-3 dip is measured against the pre-era trend, exactly as §11.2's
centered d_local was.  Fewer than K valid entries ⇒ INDETERMINATE (never a
guess, never a block).  Cost: one median over ≤ 5 doubles per epoch — no
O(dim) work, no solve interaction.

**The classes** (an epoch carries a possibly-empty flag SET; `ok` = none):

- **`c` — trigger contamination.**  The EXISTING criterion verbatim
  (`raim_within_epoch` armed && post-adaptation σ̂₀ > `raim_sigma0_trigger`,
  `NetworkRaim.cpp:884`), promoted from trace-event-only to the product
  channel.  The T2 event row is unchanged; the class is set on the ctx
  scratch UNCONDITIONALLY of any trace stream.  Covers §12.1's known class.
- **`s` — level-suspect.**  |d_causal| > `level_health_dev_tecu` (default
  **6.0** TECU) with a valid reference.  Threshold provenance: above the
  144-epoch clean-emitted baseline's max |d_local| = 5.98 (p97.5 = 2.69) and
  1.7× UNDER the era-3 toxic floor (9.96) — margin both ways; ≈ 7.5× the
  clean p16/p84 half-width (±0.8); above each arm's own 30 s breathing p95
  (3.1–4.9) even in the doubled hours-4-6 regime.  Two-sided: the post-reset
  excursion is POSITIVE (+12.7).
- **`r` — fresh-re-level transient.**  Set at the mass re-level edge — this
  epoch's inject-side stale-skip count ≥ 50 % of carried leveling priors
  (the §1.3 signature; measured 535/61-sta and 556/61-sta at exactly the two
  era-exit epochs) — and HELD for `level_health_relevel_hold_s` (default
  900 s, the measured σ̂₀≈1.0 zero-redundancy transient).  This gives the
  900-s cliff edge its consumer MARK (the §11-caveat open limb); the wired
  RESPONSE at that edge remains a separate owed card — this card only flags.
- **`na` — indeterminate**: reference warming/starved, or no SH co-estimation
  in the posture.  `r`, `c`, `s`, and `na` epochs are all excluded from the
  reference.  ⚠ Declared consequence: a long all-`c` era (era-5: 28/30) can
  starve the reference into `na` — honest indeterminacy, never dip-tracking.

**What is emitted, at which edge.**  A trailing key=value token on the H
heartbeat line of every EMITTED epoch (withheld H lines carry none — a
withheld epoch delivers nothing to class):

    H,<week>,<tow>,<status>,<sigma0>,<n_sats>,<withheld>,lvl=<flags>[:<dev>]

`<flags>` ∈ concatenations of `c`/`s`/`r`, or `ok`, or `na`; `<dev>` = signed
d_causal ("%.2f" TECU), present whenever computed (including `ok`) so a
consumer may apply its own tighter threshold.  Verified forward-compatible:
the current reader parses H fields f[1..6] positionally and ignores trailing
fields (`NetworkProductIO.cpp:237-243`); the '#' legend is extended (enforced
by `HeaderLegendDeclaresEveryEmittedField`).  The S contract line gains

    lvl=<thr_tecu>:<live_classes>        e.g.  lvl=6.0:csr

declaring the threshold AND which class letters are live in this run (with
`raim_within_epoch` off, `c` is structurally absent and the declaration says
`sr`) — so "no c epochs" and "c not monitored" are distinguishable, and the
declaration is gated ONLY on the emit knob, never on a QC/trace knob (legend-
card rule).  `NetworkProducerStatus` gains optional `level`/`level_dev`
fields (empty default; `parseState`-style unknown-tolerance preserved), and
the consumer-replay H-rider forwards them.

**Retroactive limb (the causal-vs-centered answer, stated).**  At emit time
the producer knows: a00(t), the trailing clean reference, σ̂₀, n_adapt, the
T2 verdict, and the re-level edge (the inject-skip census runs in assembly,
BEFORE emit).  It cannot know whether a sub-threshold drift is an onset, nor
whether a flag is the field legitimately moving — those need future epochs.
So: `N = level_health_confirm_epochs` (default 5; 0 disables the limb)
emitted epochs later, the CENTERED verdict (5 clean neighbors each side —
§11.2's d_local exactly) re-evaluates the `s` decision; where it differs
from what was emitted, the next H line carries a supersession token

    lvlretro=<tow>:<class>[;<tow>:<class>...]

draining a pending queue with the `ura_retro_bump` mark==land discipline
(`NetworkSolPrep.cpp:644-672`): a correction is serialized only when it
lands on an actual emit.  Retro semantics are ACCEPTABLE to this contract by
precedent — `ura_retro_bump` already flags a past epoch's contamination at
the next emit, and M4 v2's supersession is the contract's update primitive
(the consumer REPLACES its stored class for that TOW, never accumulates).
The two consumer postures are stated explicitly: a hard-RT consumer operates
on the causal token alone — which on the labeled truth set catches 7/7 with
LAG 0 (era-3's first epoch is already −9.96) — accepting the un-measured
miss class (a slow sub-threshold onset; no example exists on this pair); a
consumer integrating longer than N·rate (150 s at 30 s) honors `lvlretro`.
The retro limb exists to BOUND the causal limb's miss class, not to carry
the detection.

**Events + counters (invariants #7/#4).**  An `s`-flagged stretch is an era
at both edges: `LVL/era_enter` (dev at entry, reference value + age) and
`LVL/era_exit` (length, min/max dev, class mix) into the events trace when
open — the product tokens are the consumer-side record regardless.  Run-end
teardown line prints the full census: `n_lvl_ok`, `n_lvl_suspect`,
`n_lvl_contam`, `n_lvl_relevel`, `n_lvl_na`, `n_lvl_retro_upgrade`,
`n_lvl_retro_downgrade`, `n_lvl_era_entries`, `lvl_era_max_len`.
Armed-but-inert is diagnosable: all-`na` with n_emitted matching `n_lvl_na`
(plus the loader warn) = no SH model / never-warm; token census vs counter
mismatch = mis-wiring.

**What this card does NOT do** (scope fence, each constraint-traceable): no
withhold, no deweight, no adaptation trigger — flag-only, so the no-deadlock
property is structural; no level fix and no rx-CB carry-memory change (LV-6 /
amendment-6 corollary — the per-epoch level AUTHORITY is the separate open
card); no GIM anywhere in-line; no per-station granularity (a00 is the global
mean — regional excursions cancelling in the mean are invisible; declared).

### §12.3 Config surface

| knob | type | default | notes |
|---|---|---|---|
| `network.emit_level_health` | bool | **false** | master; auto-enables `emit_epoch_status_record` (the R4/`ura_retro_bump` auto-enable precedent — the H line is the carrier) |
| `network.level_health_dev_tecu` | double | 6.0 | `s` threshold, two-sided |
| `network.level_health_ref_epochs` | int | 5 | K, trailing clean-reference size |
| `network.level_health_ref_max_age_s` | double | 900.0 | reference content-age horizon; overlaps the 900 s cliff by design — post-cliff epochs go `r`/`na`, never silently re-referenced |
| `network.level_health_confirm_epochs` | int | 5 | N; 0 = causal-only |
| `network.level_health_relevel_hold_s` | double | 900.0 | `r` hold after the mass-re-level edge |

Loader: warn-not-interlock when SH co-estimation is absent (channel runs
all-`na`) and when `raim_within_epoch` is off (`c` structurally absent; the
S-line declaration reflects it).  Summary toggle-log line per convention.

**Fingerprint class: fingerprint-SAFE** (measure+emit only — the trace-class
family).  The solve is untouched (VB-2 certifies byte-identical solve CSVs
armed-vs-not); `LevelHealthState` (ring buffer, retro queue, era scratch,
the T2 per-epoch flag) lives in `NetworkRunContext`, is NOT serialized into
snapshots, and a warm resume re-warms in K epochs, visible as K `na` tokens.
Legal on a warm resume.  ⚠ The product FILE differs when armed (tokens +
S declaration + legend) — same class of product delta as arming the H
heartbeat itself; arm only when consumers reading the file are current (the
existing RTA-10 rule extends verbatim).

### §12.4 The consumer contract (normative)

1. `lvl` is the ONLY level-health ranking.  σ̂₀ MUST NOT be used to rank
   level health — measured anti-ranking on this product class.
2. `ok` epochs are full-service, INCLUDING recovered ones (16 of 23 measured
   clean) — a consumer that rejects the recovered class wholesale is out of
   contract (it re-creates the withhold).
3. `s` / `c` / `r` epochs: response is the consumer's risk policy; the
   contract mandates only that iono-sensitive long-memory state MUST NOT
   integrate an `s`-class epoch unaccounted.  `<dev>` is provided for
   consumers applying tighter thresholds.
4. `lvlretro` SUPERSEDES the named TOW's causal class (replace, never add).
   Consumers with integration commitment > N·rate honor it; hard-RT
   consumers run causal-only with the documented miss class.
5. Unknown trailing tokens are ignored (existing grammar rule); pre-RTA-10
   and current readers parse the file unchanged.

### §12.5 Pre-registered validation (BEFORE any build) — the 6 h pair is the truth set

**Phase A — offline classifier replay** (`scripts/wh_level_flag_replay.py`
over the recorded `logs/network_vtec_sh.csv` + dispositions + events of BOTH
arms; the classifier is pure arithmetic over recorded series, so it runs
before a line of C++; gates on the CAUSAL statistic, centered only in VA-7):

- **VA-1 (sensitivity, HARD)**: era-3's 7 ESC-emitted epochs (TOW
  273270–273450) all class `s` causally — **7/7**.  Predicted margin: min
  |d_local| 9.96 vs thr 6.0.
- **VA-2 (specificity, HARD)**: era-2's 16 recovered epochs (269220–269760)
  — **zero** `s`.  Predicted: max |d_local| 1.37.
- **VA-3 (clean baseline)**: ≤ 2 causal `s` false-flags per arm over each
  arm's settled clean-emitted population (centered baseline max 5.98 < 6.0
  predicts 0; the causal-vs-centered difference is what is under test).
  Every flag individually adjudicated in the report.
- **VA-4 (symmetry / stock arm)**: ATT 276150 (−11.42) flags `s`; ATT
  post-reset 278490–278610 carry `r` (330–450 s < the 900 s hold), `s`
  additionally where |d| > 6 with a valid reference.
- **VA-5 (known-class unification)**: every epoch with a T2
  `emitted_contaminated` event carries `c` — ESC 72/72, ATT 52/52; era-5
  shows `c` on 28/30.
- **VA-6 (era-5 marginals, REPORT-ONLY)**: 277380/277410 (−7.0/−6.4)
  expected `s` at 6.0 — recorded, not gated (labels soft: the era-5
  d_local bracket spans 15–30 min, the coarse-reference caveat).
- **VA-7 (retro consistency)**: N=5 centered confirm yields ZERO downgrades
  on era-3 and ZERO upgrades on era-2; total retro corrections reported.
- **VA-8 (latency, REPORT)**: first-flag epoch == first toxic epoch for
  era-3 (predicted lag 0); dev at first flag reported.
- **VA-9 (indeterminate census)**: every `na` attributable to warmup,
  reference starvation, or the post-900s hole — no unexplained `na`.

Failure handling, pre-registered: one fallback threshold only (6.0 → 8.0);
K/max_age iterate IN THIS SPEC with the change recorded.  If no setting
passes VA-1 AND VA-2 simultaneously, the causal limb is FALSIFIED → STOP,
record, the retro-only variant becomes the candidate, promotion stays
blocked.  No silent tuning.

**Phase B — wired** (after build, suite green incl. the new config +
product-IO round-trip + legend tests):

- **VB-1 (structural byte-identity, invariant #6)**: both 6 h configs rerun
  knob-absent on the new exe — products + five solve CSVs byte-identical to
  the recorded pair.
- **VB-2 (measure-only)**: armed rerun — all solve CSVs byte-identical to
  VB-1; products differ ONLY in `lvl`/`lvlretro` tokens, the S `lvl=`
  declaration, and the legend line.
- **VB-3 (wiring == replay)**: the armed rerun's per-epoch token census
  equals Phase A's classifier output EXACTLY.
- **VB-4 (counters, invariant #4)**: teardown totals == token census;
  era events paired enter/exit.

### §12.6 The promotion gate this unblocks

§11.3's blocker (§10.1 limit 3) closes when Phase A + Phase B pass.  Then
`raim_withhold_adapt_escalate: 12` + `raim_withhold_free_erase: true`
promote as a SCOPED LEDGER ENTRY (CONFIG_promotion_ledger), **jointly and
only jointly** with `emit_level_health: true` (+ its auto-enabled H
heartbeat) — the recovered class's toxic tail is consumer-distinguishable
exactly when the channel is on the wire, so the escalation never ships
without it.  No loader-default flips (fingerprint discipline; the standing
no-blanket-promotion rule).  New network configs derive from a base arming
the triple explicitly.  Re-blocking condition: a field lineage where the
channel's false-flag rate exceeds VA-3's bound, or a toxic class that
evades ALL THREE flags (would also revive deferred item 4 below).

### §12.7 Deferred alternatives — with revival conditions

1. **Make σ̂₀ level-aware** — DEAD on LV-6 (the fold does not open; corr
   +0.681).  Revive only if the per-epoch level-authority card lands.
2. **Level-triggered withhold / blanket recovered-class rejection** — never
   (re-creates the withhold; 16/23 clean).  A level-triggered ADAPTATION is
   a lawful successor ONLY after the level-authority card exists (it would
   touch the level mechanism, out of this card's fence).
3. **GIM-referenced monitor** — prohibited in-line (standing directive);
   stays the offline diagnostic.  Revive only if the directive changes.
4. **WE-absorption suspicion** (count within-epoch refloats as a `c`-adjacent
   signal — era-3's trigger was absorbed by 3 `we_refloat_*` actions) —
   counterfactual and unmeasurable cleanly today.  Revive if a toxic class
   ever evades `s`, `c`, AND `r` simultaneously.
5. **Formal-std channel** — measured flat (0.588–0.594) through −21 TECU;
   structurally blind to a stiff carry.  No revival short of the carry
   variance model changing (carry-aware covariance card territory).
6. **Per-station level flags** — deferred until a consumer needs regional
   selectivity AND the LV-6 tier-2-median suspect is resolved; a00 stays
   the global-mean instrument until then.
7. **The wired RESPONSE at the 900-s mass-re-level edge** (re-validate /
   re-seed the re-coupled levels — invariant #1/#7) — NOT this card; this
   card ships the edge's consumer MARK (`r`) and its census, which is that
   card's motivating instrument.  Eligible immediately.

### §12.8 Implementation map

`GnssConfig.hpp` (6 getters + members), `GnssConfigLoadNetwork.cpp` (parse
beside the RTA-10 block at :4866; auto-enable + 2 warns + summary line),
`NetworkRunContext.hpp` (`LevelHealthState`: a00 ring buffer, retro queue,
era scratch, per-epoch `t2_tripped` + `relevel_edge` flags; knob snapshot; 9
counters), `NetworkRaim.cpp:884` (set `t2_tripped` beside the existing T2
event — trace-independent), assembly inject-skip census → `relevel_edge`
(PASS-B, `ApplicationNetworkAssembly.cpp`), class computation + retro drain
in `NetworkSolPrep.cpp` (beside the `ura_retro_bump` drain it imitates),
token attach in `NetworkProductIO.cpp::writeEpochStatus` (+ legend + S line
+ `NetworkProducerStatus.level`/`level_dev` + parser), H-rider forward in
`ApplicationConsumerReplay.cpp`, teardown line in `NetworkTeardown.cpp`.
Tests: `tests/test_level_health_config.cpp` (defaults, auto-enable, warns,
counter zero-init) + `test_network_product_io` extensions (H token
round-trip, unknown-token tolerance, legend completeness).  New `.cpp`
seams, if any, get manual `CMakeLists.txt` entries.


### §12.9 Phase A EXECUTED (2026-08-23) — PASS on the era-3 class, and two spec defects caught before any C++

`scripts/wh_level_flag_replay.py` over the recorded 6 h pair (the card's own
pre-registered zero-C++ gate):

**PASS — the sharp-excursion class:** escalation arm, causal limb alone,
defaults (K=5, 900 s, 6.0 TECU): **era-3 toxic 7/7 flagged `s`**
(d −9.0…−20.5) and **era-2 clean 16/16 passed**.  Bonus: it also catches
277410 — the era-5 toxic epoch the T2 class MISSED — and the post-era
recovery overshoots (+7.6/+6.6, two-sided as designed).  Flag census:
ESC 16/683 `s` (2.3 %), ATT 52/661 (7.9 % — an eventful tail).

**DEFECT 1 (spec text, fixed in place): the `na` self-exclusion was a
bootstrap deadlock.**  As first written, `na` epochs were excluded from the
reference — so the reference could never warm (first replay: 1160 `na`,
0 `ok`).  Amended: WARMING epochs with no `c`/`r`/`s` flag seed the
reference; the load-bearing exclusion is `c`/`r`/`s` only.

**DEFECT 2 (mechanism, resolves an open design question): the causal limb
alone MISSES the GRADUAL-slide class, and the retro-confirm limb is therefore
REQUIRED, not optional.**  The reference arm's own toxic epoch 276150
(centered d_local −11.4) is a 7-epoch slide (a00 35.2 → 23.3, each step
under the 6.0 per-epoch threshold) followed by a one-epoch snap back.  In the
causal replay the slide epochs either enter the reference (poisoning it) or
are c-excluded (starving it → 276150 lands `na`), and the RECOVERY gets
flagged +10…+13 instead of the trough.  The centered/retro measure catches
276150 trivially — so the `lvlretro` supersession limb is the mandatory
detector for slides, and a consumer reading only the causal token must be
told (S-line declaration) that slides confirm at +N.  The 52-flag ATT census
also confirms the post-reset transients flag correctly (`r`+`s` overlap).

Verdict: the card proceeds to Phase B implementation with the two amendments;
the promotion ledger entry (escalate=12 + free_erase JOINTLY with
emit_level_health) stands as written.
