# A4 withhold — the withhold-era release policy — the defect named exactly, the v2 design, the pre-registered gates and the first results

**STATUS**

- **id:** A4 withhold
- **topic:** raim-qc
- **status:** closed
- **summary:** the withhold-era release policy — the defect named exactly, the v2 design, the pre-registered gates and the first results
- **dates in the text:** 2026-08-21
- **knobs / posture:** `network.raim_withhold_adapt_escalate`, `network.raim_withhold_sigma0`, `network.raim_withhold_free_erase`
- **hashes named in the text** (commit / exe / byte-cert): `65ba362`
- **run dirs named in the text:** `runs/fld63_relevel4h_`, `runs/fld63_relevel4h_wh`
- **source:** `docs/archive/SPEC_withhold_era_release.md` — (preamble), §1, §2, §3, §4, §5, §6, §7

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_withhold_era_release.md -->
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

