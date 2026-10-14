# PRM3: card PRM phase 3, the event responses of the carried parameters

**STATUS**

- **id:** PRM3
- **topic:** network-vehicle
- **status:** closed
- **summary:** 69 response sites declared (0 errors) and the gap policies unified, byte-identical; carry_rebase_complete inert to mm, not recommended. Closed: the held emission under a shift is card IRD's, the erase family NOM's.
- **source:** [PRM](PRM_parameter_registry_2026-09-29.md) sections 4.5 and 8 (this file continues that card, which is near the 60 KB bar); [RF-1 design](RF-1_float_ambiguities_design.md) section 1.5
- **knobs / posture:** phase 3a and step 1 trace class only (run-start and run-end log lines, manifest records `param_model.on` and `param_responses`); step 2 adds `network.carry_rebase_complete` (default off, not recommended)
- **hashes / run dirs:** exe `gnss_processor_prm3a.exe` `0e815e61c291ec6b`; tags `p3ah6` (6 h goldens), `p3a` (program, scalar, diag, 2 h), `p3ax` (clv, eq34, dho, 1 h), `p3axr` (relevel, 2.1 h) under `runs/_rap_prm_*`; later gates on `gnss_processor_prm3b.exe`, `prm3c`, `prm3d` (sections 5, 7, 8)
- **dates in the text:** 2026-10-02, 2026-10-03

**Outcome.** Question: can every carried parameter's response to a slip, gap, handover, decouple or readmit be declared, counted and implemented once per policy? Phase 3a declared 69 sites (0 declaration errors, 16 harness arms byte-identical); step 1 unified the eight gap prunes; step 2's `network.carry_rebase_complete` moves the owed references inert or millimetric, not recommended. Closed: the held emission under a phase-bias shift passes to [IRD](IRD_integer_recovering_datum_2026-10-13.md) (IRD-7), the erase family to [NOM](NOM_network_object_model_2026-10-08.md)'s entities; the band-5 hold's return count is owed, the clock-monitor and oscillator counts moot. Status reviewed 2026-10-13.

## 1. The build

`include/filter/NetworkResponses.hpp` declares 69 response sites (`ResponseSite`), one row per (kind,
event, policy, code site): the event (`ParamEvent`: slip, gap, handover, decouple, readmit), the
policy (`ResponsePolicy`, 18 values, in `filter/NetworkParamRegistry.hpp`), the unit of a count, and
its source (an increment at the site into `prm_resp`, or the fold's disposition tally of phase 1b).
`responseSiteArmed` states when each site can act on a run, from the knob snapshot and the
configuration. The datum mechanisms of phase 2 are the Stransform responses, counted from the
traversal's moved values per kind (`CarryMovedTally`, now with the border kinds). The registry
builds `ParamSpec::on` (armed policies per event) from the table; it is printed at run start
(`Network (param responses)`) and written into `param_model`. The run end writes each armed or
responding row with its count, the Stransform rows, the free-epoch latch per cause
(`networkResponseLatchTally`, before the response reads the latch), and the check: a site that
responded on a run where it is not declared armed is a declaration error.

The sites were found by an inventory of the code (six readers, two critics), placed in two rounds,
each reviewed for behaviour changes, races and miscounts. Excluded by design (stated in the header):
the q rule's time update, numerical guards on a non-positive or non-finite prior, the coupling terms'
re-attribution (card CTU's line), and marks that only re-seed a linearization point.

## 2. RF-1 section 1.5 on the float vehicle

The four ambiguity reset paths, read again on the goldens (6 h field):

| path | on the float vehicle | rows, field 6 h |
| --- | --- | --- |
| pending-reseed | still dead: PASS B's measurement pre-pass consumes the mark, which only re-seeds x_lin (a solve no-op) | `amb_pending_reseed_skip` 0 |
| free-epoch latch | ends in a Drop (the serial drop pass); the inject then folds the arc at birth strength, so the old "inject-site skips" counter counts folds | `amb_drop_latch_slip` 306, `amb_drop_latch_reentry` 130 |
| T4 / T3 | a Drop, not an erase; T3 is unreachable while the attribution acts | `amb_raim_drop` 48 |
| gap cliff | the stale-gap drop; every such arc on the goldens was absent and erased by the prune in the same epoch, so the drop is a no-op ahead of the erase | `amb_drop_stale_gap` 1248, `amb_gap_prune` 1251 |

The latch, now traced per cause: 480 latched key-epochs over 190 epochs, 446 with a carried entry,
132 with more than one cause; by cause MW 26, LLI 261, IF-TD 129, wind-up 52, gap re-entry 144. The
other responses on the field: the anchor re-reference erased 138 arcs, the gross screen 5; the clock
anchor 33 decouples and 32 re-couplings; READMIT parked and seeded 4 clocks and 16 code biases; 23
phase-bias re-references; one handover S-transform of 31 clocks. Sim 6 h: 10 latch drops, 1302
stale-gap drops and prunes, 203 re-reference erases, 30 re-couplings.

Declared on the goldens: the ambiguity drops on a slip and erases on a handover and a gross
declaration; the satellite clock is marginalized and parked across a gap, seeded on return,
loosened and re-coupled by the anchor, S-transformed or retired at a handover; the receiver code bias
is erased on a step and skipped and pruned on a gap; the wet delay, ionosphere, receiver clock,
receiver phase bias and phase colour declare nothing (not carried).

## 3. The gate

`prm_ab.py cmp --all-csv`: `b0h6`/`p3ah6`, `b0`/`p3a`, `x0`/`p3ax`, `x0r`/`p3axr` all IDENTICAL. The
run end of every arm reports 0 declaration errors (31 to 35 of 69 sites armed). Unit suite 2781
passed, 1 fixture-gated skip; suite `ParamResponses` (7 cases: the table, the registry's masks, the
goldens' declared posture, the Stransform declaration, the per-kind traversal tally, the latch tally,
the count sources). Two run-end lines that described T4 as an erase now state the drop on the float
vehicle.

## 4. Found by the read, not changed (owed to phase 3b or their own cards)

- The stale-gap drop of absent arcs is a counted no-op before the prune.
- `relevel_free_marked` counts attempts (two admitted MW candidates per satellite); the latch tally
  counts keys.
- An idempotent drop under `amb_float_drop_decouple_always` changes the instance (regression and
  block row cleared); the drop rows now count it.
- The Q3 anchor commits its fold state before the solve; a failed solve skips the guard and loses
  the gap decouple edge (rare path).
- Uncounted: the extra-band hold's return response, the oscillator's finite-screen re-seed, the
  re-couplings of a satellite restored LOOSE after a warm resume (the snapshot does not carry the
  history bit). The declaration review of the 69 arming conditions was stopped before it finished;
  the run-time check covers the 16 harness postures only.

## 5. The declaration review, completed (2026-10-03)

The review stopped in section 4 was resumed and finished: every row's arming condition traced against
the gates on the path to its increments (the knob snapshot, the vehicle, the routes, the loader's
interlocks). Three conditions were too narrow, each a false declaration error on a posture the harness
does not run: the phase-bias and code-bias reinit rows under the clock model's fault test with no
clock monitor; the eq34 shift under the seed-round re-host with the shift knob off; and the clock
oscillator's moves at the ephemeris re-level, which the Stransform rows did not count (they now count
`clock_osc` with `prev_carry`, and a moved border kind outside the mechanism's declared kinds is a
declaration error too). The legacy unscoped handover, which retires every carried border entry by
skipping the fold, is now tallied as Retired. About thirty conditions were too broad (rows armed where
nothing can act: the local ambiguity under `carry_invariant`, a clock under the clock model, a code bias
without satellite code-bias estimation, a datum change under an anchor-free clock datum) and are
narrowed. `kResponseSites` is tied to the enum by a static assertion; the registry test now rebuilds
the masks from the table and compares both ways, and a new case pins the gates the goldens do not
exercise. The PASS B skip rows count the fold refusals offered to a solve, including an epoch the A4
ceiling later withholds (stated in the table).

Gate: exe `gnss_processor_prm3b.exe` `cc68a5125f13f2e7`, tags `q3ah6`, `q3a`, `q3ax`, `q3axr`:
byte-identical on all 16 arms (`--all-csv`), 0 declaration errors on every arm; suite `ParamResponses`
8 cases.

Still uncounted (owed): the hard clock refloat (`clock_monitor_reinit_sigma_m` <= 0 skips the stash,
an erase of the border entry), the extra-band hold's return, the oscillator's finite-screen re-seed.

## 6. Phase 3b, pre-registered (2026-10-03)

One implementation per policy, then the owed responses behind knobs, in this order:

1. Byte-identical: each local kind's gap horizon declared in the registry (`ParamSpec::gap_horizon_s`,
   from the knobs that set it today), and the gap policies through one implementation each: the eight
   gap prunes through `carryGapPrune`, the inject gap skips through `carryPastHorizon`. Gate: the 16
   harness arms byte-identical, every response count equal.
2. Behaviour-changing, one default-off knob each, graded on the posture that exercises it (2 h, both
   beds, against its own reference; the level and consumer bars of card CTU section 10): the ephemeris
   re-level moving every coupling map and the two baselines it skips (card CLV section 6.4 item 6; the
   receiver code-bias coupling terms are live on the goldens' route), and the phase-bias shifts (eq34,
   the declared handover) moving the coupling maps, `pb_level_prev`, `pb_hold` and the gross screen's
   held ambiguity.
3. Then the erase family: whether one EraseAndRebirth (an arc's local carry with its line of sight's
   ionosphere and phase colour) replaces the per-site subsets.

## 7. Phase 3b step 1, the gap policies through one implementation (2026-10-03)

`ParamSpec::gap_horizon_s` declares each local kind's gap horizon from the knob its sites read (the
ambiguity, wet delay and receiver phase bias `phase_leveling_max_gap_s`; the phase colour, the receiver
clock and the slant ionosphere their own, the ionosphere by the carry in force, the registry refusing
both ionosphere carries at once; the receiver code bias the one constant `kRxCbMaxGapS`, which replaces
six literals and two local copies of 900 s); the border kinds declare none (their gap response is
READMIT's park). The eight gap prunes go through `carryGapPrune` (in order, the site's side effects in
a callback before each erase) on the declared horizon, and the inject gap skips through
`carryPastHorizon`, the sites' own strict comparison; the fold tests that also refuse a negative or
NaN gap are kept as they were. The model table gains a `gap_s` column and `param_model` a
`gap_horizon_s` field (trace class: the run log and manifest change, not the gate's files).

Gate: exe `gnss_processor_prm3c.exe` `460e4a652e0e91b9`, tags `r3ah6`, `r3a`, `r3ax`, `r3axr`:
byte-identical on all 16 arms, 0 declaration errors, the gap response counts equal to phase 3a's
(ambiguity prunes 1251 field / 1302 sim on the 6 h goldens). Unit suite 2784 passed, 1 fixture skip.

## 8. Phase 3b step 2, the owed references moved (2026-10-03)

`network.carry_rebase_complete` (default off; fingerprinted when armed; INERT and disarmed when no re-base
mechanism is armed): each re-base mechanism also moves the sites its exemption table marks owed
(`carryMechanismOwedSites`): card CLV the stash's pivot and the phase-colour coupling; the ephemeris
re-level the stash's pivot, two baselines and nine coupling maps; the eq34 shift and the declared
handover the phase-bias baseline, the gross screen's held ambiguity, the remaining coupling maps (and
the pin bridge for the handover). Deviations from section 6, recorded: one knob for the four
mechanisms (each harness posture arms exactly one, so attribution holds per posture); the phase hold's
held emission (`pb_hold`) stays exempt from both phase-bias shifts, with or without the knob (a held
emitted value needs its own design; reason `kPbHoldEmit`, a manifest-only text change with the knob
off), and its unmoved held families at a shift are now counted (`prm_pbhold_unmoved_at_shift`).

Gate (exe `gnss_processor_prm3d.exe` `80db53d9733d3f2f`): knob off byte-identical on all 16 arms
(tags `s3ah6`, `s3a`, `s3ax`, `s3axr`), 0 declaration errors; unit suite 2791 passed, 1 fixture skip.

Knob on, against knob off on the same exe (tags `k3x2`, 2 h, and `k3r6`, the re-level 6 h across the
02:00 and 04:00 cut-overs), both beds; no arm INERT, every mechanism ran:

| mechanism | owed references moved | outputs |
| --- | --- | --- |
| card CLV | none moved (the pivot is re-derived at every kept stash; no phase colour) | byte-identical |
| ephemeris re-level (6 h) | the pivot 2 / 3, `pb_hold_prev_clock` 60 / 85; 0 receiver code-bias terms (their regression carries no clock term) | byte-identical |
| declared handover | 91 receiver code-bias terms, `pb_level_prev` 1 (field) | byte-identical |
| eq34 shift | 24 489 / 10 341 receiver code-bias terms, `pb_level_prev` 257 / 188 | field: consumer within 0.1 mm, emitted clock within 5.7 mm and phase bias within 7.6 mm from 1 h on; sim identical |

The read: the owed exemptions are inert or at the millimetre level on these postures. Card CTU's exact
update forms its rows on the carried values, not the terms' stored `s_prev`, so a regression's
reference matters only on the stale (card 1d) route; and the receiver code bias regresses on no
clock. The knob stays default off and is not recommended; the complete walks remain available. The
held emission under a phase-bias shift (257 / 188 family-epochs on the eq34 posture) is the one open
item of this step. Phase 3b step 3 (the erase family) is parked.
