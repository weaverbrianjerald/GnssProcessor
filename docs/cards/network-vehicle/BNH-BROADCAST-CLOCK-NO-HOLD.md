# BNH: the reference setup on the broadcast clock without the phase-bias hold

**STATUS**

- **id:** BNH
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-22 on the author's order of that day ("use our current best configuration, simply replace CODE clocks with the broadcast"; "Yes, build it"). The loader refuses `network.apriori_source` `sp3_brdc_clock` unless the smoothed phase-bias hold is armed, and the reference files run the hold off since 2026-11-19. The design note (section 2) finds that the refusal's reason belongs to the float posture, that the broadcast terms reach the products and the user without the hold, and that the chain posture's existing responses cover the events the hold used to bridge. The change is a relaxed refusal under `network.phase_datum_eq34`; no new key, state or estimator feature.
- **dates in the text:** 2026-11-22
- **knobs / posture:** no new key. The loader accepts `network.apriori_source` `sp3_brdc_clock` with `network.emit_pb_hold` off when `network.phase_datum_eq34` is on; every other refusal stays. Target arm: `config/golden/golden_{sim,field}_refound.json` with `network.apriori_source` `sp3_brdc_clock`, `network.brdc_select` `ttom` and a data root without CLK files as the only changes.
- **code:** branch `net/brdc-no-hold-2026-11-22` from `int/2026-11-19` (`1bb4964`), worktree `D:/GnssProcessorSupport/worktrees/brdcnohold`.
- **run dirs:** `D:/GnssProcessorSupport/runs_nom/_rap_bnh_*`; report `D:/GnssProcessorSupport/gates/broadcast_reference/REPORT_build_no_hold.md`.
- **source:** the author's words of 2026-11-22, relayed by the managing session (`D:/GnssProcessorSupport/gates/broadcast_reference/ORDER_build_no_hold.md`); cards [NBP](../../record/refound/NBP_native_broadcast_clock_posture_2026-10-07.md), [BRD](../../record/refound/BRD_broadcast_referenced_level_2026-11-02.md), [BRD2](../../record/refound/BRD2_broadcast_remainder_hold_days_2026-11-03.md), [LAH](../../record/refound/LAH_level_aware_hold_2026-09-29.md), [SMH](../../record/refound/SMH_smoothed_hold_clone_2026-09-29.md).

**Outcome.** Pre-registered. The design note finds that the smoothed hold was required under the broadcast clock because the float posture needed a hold on its emitted phase biases and the phase-clock hold is valid only with a precise a priori clock. The chain posture gives the stream its integer frame without a hold. The broadcast terms (the per-ephemeris offset, the IODE stamp, the tracked sum) are written by the wire seam whether or not a hold runs, and the user applies the clock against the stamped record whether or not a hold runs. The build relaxes one refusal; readings I1 to I5 are fixed below before any run.

> Results are appended TO THIS FILE, below the text, under dated headings.

---

## 1. Question

Can today's reference setup (the eq34 chain as phase datum, network integer fixing ring by ring, the phase-bias hold off, the fixing differenced built-in user) run with the broadcast clock as its a priori satellite clock instead of the CODE final clock, with the clock source as the only change, so that the only external product of the network is the orbit?

## 2. Design note: what the hold did for a broadcast run, and what the chain setup has in its place

Read on `int/2026-11-19` (`1bb4964`).

**Why the loader requires the smoothed hold.** Card NBP built the broadcast posture on the float vehicle with the phase-bias hold, the posture of its day. A float consumer needs the emitted phase biases held per generation (card PBH: 0.0467 m of within-arc movement otherwise), and the hold that suits the exact update, card LAH's phase-clock hold, transfers the level step to the phase bias against the a priori clock, valid only when that clock is precise (1.191 m under the broadcast clock, card SCL section 3.1). So NBP required the other hold, card SMH's smoothed hold, and refused LAH's (`src/core/config/GnssConfigLoadNetwork.cpp`, the block after the smoothed hold's own refusals). The requirement says which hold a float broadcast run must use, not that the broadcast clock needs a hold. Since 2026-11-19 the chain gives the stream its integer frame and the hold is off in the reference files because it freezes the cold estimate (card IRDU stage B; the ledger row of `network.emit_pb_hold` excludes the chain from its regime).

Mechanism by mechanism:

1. **The user's clock across a change of a satellite's broadcast ephemeris.** The a priori is continuous through the per-ephemeris offset table, a_s(t) = brdc_k(t) + O_s,k, built from 00:00 of the observation day (`products/BrdcClockApriori`). The wire seam `networkBrdcWire` (`src/core/network/emit/BrdcWire.cpp`) writes C_wire = C_est + O_s,k with `iode=k`, so brdc_k(t) + C_wire = a_s(t) + C_est. Its only gate is `ctx.knobs.nbp_on`; it runs after the holds (each an immediate return when off) and before the store push and the product write (`src/core/network/emit/EmitStore.cpp`). The user applies the clock only against the stamped record (`src/core/corrections/GnssCorrections.cpp`, the gate on `clock_iode`). No hold takes part. Replacement needed: none. Test: section 3, I2 (a change of ephemeris between two epochs) and I3.
2. **The tracked sum on the products.** Under `network.apriori_clock_track` the same seam adds U to the clock record after the holds, whatever they did. Replacement needed: none. The track requires the two-way centring (`network.cycle_slip.if_phase_td_sat_centre`), which the reference files do not arm; it is an arm of the run session (a3), not part of the target. Test: I2 with and without the track.
3. **Datum changes and generations.** Under the hold a generation is the hold's interval; with the hold off, the emitter's datum-change marks (the clock re-anchor, `network.clock_anchor_mark`; the phase frame) and the chain's own events (event rows, carry shift, ring fix release) declare every change to the user, as they do on the CODE a priori today. The broadcast clock adds no generation of its own: an ephemeris switch is continuous by item 1 and is counted (`nbp_switches`), not declared. Replacement needed: none.
4. **The bridge while a clock is decoupled from its anchor.** The smoothed hold carried a clone that passed the clock's smoothed motion through an anchor decoupling (card SMH); card BRD2 found that bridge dropping centimetres of clock motion at the statistic decouplings, the DOY-132 remainder. With the hold off there is no bridge: the decoupled clock is estimated from the epoch's data, the decoupling and the re-coupling are marked (`network.clock_anchor_mark`, `network.clock_anchor_recouple_epochs` 4, `network.clock_anchor_resolve`), and the IF-TD satellite clock event (`network.cycle_slip.if_phase_td_sat_clock_event`) responds to a step. These are the reference files' responses on the CODE a priori today. What the broadcast clock changes is how often they fire: the time update's prior mean follows the broadcast increment (card BRD's second mechanism), so the anchor's statistic test sees the satellite's real departures from its broadcast polynomial. Replacement needed: none to load and run; the rate is read in I3 and I5, and the tracked a priori (the run session's a3) is the existing answer to the cause.

**Conclusion.** The change is a relaxed refusal, under the key that says the chain is the phase datum (`network.phase_datum_eq34`), plus tests. The products' broadcast terms need no new code. No new estimator feature, no new state, no new key.

## 3. The change

- `src/core/config/GnssConfigLoadNetwork.cpp`: under `sp3_brdc_clock` without the phase-clock hold on the track, the requirement of the smoothed hold is lifted when `network.phase_datum_eq34` is on and `network.emit_pb_hold` is off (the reference posture). A hold that is armed must still be the smoothed hold; the phase-clock hold stays refused; without the chain the smoothed hold stays required. A run-start line says which case applies.
- Nothing else in the engine. Every configuration that loads today takes the same path.

## 4. Readings, fixed before the first run (2026-11-22)

- **I1 identity.** The two reference files (`golden_sim_refound`, `golden_field_refound`), one frozen lineage (`golden_sim_rf0hv18`), the BeiDou lineage (`golden_field_bds_refound`), 2 h each, and one broadcast run with the hold on (the simulator reference with `sp3_brdc_clock`, `network.emit_pb_hold` and `network.emit_pb_hold_smoothed`, the run session's arm a1 posture) give byte-identical `products.txt` and identical trace files on the new engine and on its base (`int/2026-11-19` `1bb4964`), both Release from this worktree's build tree. Both sha256 are stated. Pass: every file identical, apart from wall-time and path fields.
- **I2 unit tests**, in `tests/network/test_brdc_clock_no_hold.cpp`: the loader accepts the broadcast clock with the hold off under the chain; it refuses it with the hold off without the chain, refuses the phase-clock hold and a plain hold under the chain, and still accepts the smoothed hold; on a hand-built epoch with the hold off, the wire seam writes C_wire = C_est + O with the stamped IODE so that brdc_k + C_wire reproduces a + C_est, without and with the track (C_wire = C_est + O + U); across a change of ephemeris between two epochs the stamp switches, the switch is counted and brdc_k + C_wire stays continuous. Pass: every case passes.
- **I3 events on the simulator with the hold off**, each in one 2 h arm of the target (simulator reference plus the broadcast clock) with one injected event of the harness, read at both edges from the run-end counters and the event traces against the same arm without the event:
  - a change of a satellite's broadcast ephemeris inside the run (natural, every satellite carries several in 2 h): the stamp-switch count, and the products' between-satellite clock (brdc_k + C_wire) across each switch against the epoch-to-epoch scatter of the same satellite away from switches;
  - a satellite clock step (`kind` `sat_clock_step`): the IF-TD satellite clock event and the anchor decoupling at the step epoch, the re-coupling after it, and the held links of that satellite released or kept as the chain decides;
  - a clock decoupled from its anchor by an outage longer than `network.clock_anchor_max_gap_s` and re-coupled at its return (`kind` `outage` of one satellite, one event per station so that every station loses it together): the decoupling and the re-coupling counted, the satellite's re-admission.
  Pass: each edge has its counter above zero in the event arm, and the arm emits every epoch. Reported, not graded: the truth audit's counts around each event.
- **I4** the full Debug suite through the gate. Pass: 0 failed.
- **I5 the first target run:** the simulator reference with the clock source as the only change (`network.apriori_source` `sp3_brdc_clock`, `network.brdc_select` `ttom`, a data root without the CLK file), 2 h, graded from 00:30:15. Reported for the run session's readings N1 to N6, not a pass or fail of the build: exit code, epochs emitted and withheld, the truth audit's held, right and wrong counts, the clock anchor's decoupling count beside the reference run's, the stamp-switch count.

## 5. What this card does not do

It arms nothing in a reference file and changes no loader default. The poster's arms on the three beds and their readings N1 to N7 belong to the run session (`D:/GnssProcessorSupport/gates/broadcast_reference/ORDER.md`).
