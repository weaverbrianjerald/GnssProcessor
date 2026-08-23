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
