# PBF: presence edges of a held phase bias at the consumer

**STATUS**

- **id:** PBF
- **topic:** network-vehicle
- **status:** closed
- **summary:** ARMED 2026-09-28 (ledger A13): the consumer answers both presence edges of a held phase bias; ARHT00ATA's hour-2 p50 on the CLV wire goes from 804 to 0.762 m, P2 missed as frozen.
- **source:** [CLV2](CLV2_clock_level_batch2_2026-09-25.md) section 4.6 item 1 (the read); [PBH](PBH_phase_bias_hold_2026-09-19.md) (the hold and its consumer seam); [CST](CST_consumer_sat_trace_2026-09-15.md) (the per-satellite consumer trace); [SID](SID_signal_identity_2026-09-22.md) (the L1W family's coverage: 18 of 58 stations)
- **knobs / posture:** new `processing.ssr_pb_hold_presence_flip` (bool, default false, byte-identical); new `network.closed_loop.pb_hold_presence_flip` (bool, default false, derived into the consumer, never reaches the producer's solve or its fingerprint); the consumer replay driver now honours `network.trace_consumer_sat` (measure-only); at pre-registration no golden, ledger row or loader default changed; ARMED 2026-09-28 (section 5): `network.closed_loop.pb_hold_presence_flip: true` in `config/refound/{field,sim}_gps_dual_rf0hv.json`, pinned in both goldens, ledger appendix A13 with a loader posture row; the loader default stays false
- **dates in the text:** 2026-09-28
- **hashes / run dirs:** main `3ca0626` plus this card's diff (uncommitted at pre-registration; built `ff36b1f`); armed with exe `gnss_processor_pbf2.exe` sha256 prefix `73c47ebd7eb610d0`, baseline pair `runs/_rf0hv11_{field,sim}6`; exe `gnss_processor_pbf.exe` sha256 `3a3c64ea88f8a750b166cc9158cbb61b244210539422dd5b35d08b43b7962ce1`; census keys `t3g0`, `t3b1` (the trace armed, knob absent), `p0g0` (knob off), `pfg0`, `pfb1` (knob on), replays `runs/_rap_msc_<site>_<key>_d`; wires `runs/_rap_clv2_f_g0` (the baseline wire) and `runs/_rap_clv2_f_b1` (card CLV2's batch-1 base)

**Outcome.** A held phase bias that appears on, or leaves, a phase row the consumer already uses went unanswered: the Kalman peel reset innocent satellites, and ARHT00ATA diverged on eight CLV and pre-CLV wires (hour-1 p50 above 400 m on 14 of 90 census keys). The knob answers the edges where they occur (ARHT00ATA 3, QUIN00USA 5) and leaves the 15 edge-free replays byte-identical: P0, P1 and P3 PASS; P2 FAILS its 0.20 m bar, the rest being card CLV2's phase-side cost. Armed, the 6 h baselines are unchanged. Owed ([OPEN_ITEMS](../status/OPEN_ITEMS.md)): the peel's masking of a bad phase row, the real-time gap tolerance, per-site census bars.

## 1. What the read found

The census replays graded only position, so the replay driver
(`ApplicationConsumerReplay.cpp`) was given card CST's per-satellite trace, written with the
in-process formatter and stamp. Measure-only: the 34 replays of the baseline and CLV wires with
the trace armed reproduce the earlier census `closed_loop_trace.csv` files byte for byte
(`rp1g0` and `c2b1`, 17 of 17 each).

ARHT00ATA (a C1W site, Antarctica, 9 to 13 satellites) diverges on the CLV wire: hour 1 / 2 p50
478.953 / 803.907 m against 0.690 / 0.067 m on the baseline wire. Of the 90 census keys on
disk before this card, 14 put ARHT00ATA's hour-1 p50 above 400 m (`clv`, `clvw`, `clvx9`, `clvx10`, `rp1clv`,
`rp1pp`, `rp1ppfull`, `rp1ppif`, `c2b1`, `c2a2`, `c2cr`, `c2a2cr`, and the pre-CLV `p2` and
`tB`: 436 to 530 m) and two more above 40 m (`clvx11`, `r6n`), which the census median never
showed.

The sequence at TOW 260130 (both wires carry it; the wires differ by at most 0.56 m in any
clock there, and by no step above 1 m in any record):

1. The G32 L1W phase bias is absent from the wire until TOW 260100 and present from TOW 260130
   at 92.409 m, held (generation 1). The raw G32 data are clean across the window (the
   geometry-free phase is smooth, Melbourne-Wubbena constant at -2.9 to -3.8 wide-lane cycles,
   L1W minus L1C constant at one cycle).
2. The consumer's G32 L1W ambiguity had absorbed the missing bias; the appearance steps the row
   by the whole bias. `ev_pb` is `none`: nothing answers it.
3. On the baseline wire the Kalman filter's post-fit peel resets G32's two ambiguities and the
   solution continues. On the CLV wire the first update pass moves the state 704 m, all 36
   standardized residuals exceed the threshold, and the peel resets G03, G06, G28 and G19 and
   excludes G32's code rows; the epoch commits nothing. At TOW 260160 the G32 L1W row, its
   ambiguity still tight, enters with a -104.6 m innovation and the update moves the position
   490 m and the receiver clock -270 m. The code rows are excluded as outliers from then on and
   the solution never recovers.

Why nothing answers the edge. `RealTimeEngine::reactToPbHold_` claims every key whose store
entry carries a hold generation, and the three downstream phase-bias reactions (reset, card
CC's carry, card PBT's translation) skip a claimed key entirely, including the A5 applied-value
flag. The seam's comment gives two premises, both false here: that a first sighting has no prior
state (the row was in use without the bias), and that the A5 edge is unreachable on the
closed-loop postures (the harness arms the staleness gate on its derived consumer). A key that
leaves the store is not claimed, but its A5 memory was never written while it was held, so the
departure is not answered either. Before card PBH the same edges took `reset_flip`.

Exposure in the 2 h census window, baseline wire: QUIN00USA 5 edges (G32 L1W twice each way,
G16 L1W once), ARHT00ATA 3, every other site 0 (the L1W phase-bias family is tracked at 18 of
58 roster stations, card SID). At QUIN00USA the peel happened to catch every edge.

Two side readings, neither a lever:

- The consumer re-seeds its receiver clock from a single-frequency code-only fit at every epoch
  (240 of 240 at every site): `stepTimeAndDetectGap` defaults the gap tolerance to 1 s when
  `processing.data_gap_tolerance_s` is 0, so every 30 s epoch reads as a gap (the documented
  meaning of 0 is "use the measurement rate", which the post-processing path implements as 1.5
  times the interval). The seed is 20 to 217 m off the converged clock. With the tolerance at
  45 s the seed fires once per replay and every site's hour-1 and hour-2 p50 moves by at most
  0.001 m (keys `gtg0`, `gtb1`), because the consumer's clock process noise (1000 m2/s) makes the
  prior irrelevant; ARHT00ATA still diverges. Hygiene only.
- The consumer's early epochs are fragile: a replay of the CLV wire's row values on the baseline
  wire's records (key `hbb`, identical rows up to the last bit, the baseline wire's counters and
  tokens) reproduces the CLV wire at 13 of 17 sites and flips ARHT00ATA to 0.507 / 0.080 m and
  QUIN00USA from 2.521 / 0.790 to 1.047 / 0.252 m. At sites with unanswered edges the census is
  close to a coin toss.

## 2. The design

`processing.ssr_pb_hold_presence_flip` (default false). Armed, a key the hold seam claimed skips
only its discontinuity-counter reaction, which the hold owns, and its A5 presence edge runs as
on an unheld key:

- the reset path (the golden posture's): `reset_flip` on the flipped signal only;
- card CC's carry: the ambiguity moves by the applied step (the whole bias), covariance kept;
- card PBT's translation: the edge only; any other epoch of a claimed key is recorded and not
  translated, because the hold seam already re-floated a generation change.

The edge needs the staleness gate (`processing.enable_ssr_bias_max_age`); the harness arms it on
its derived consumer, and the loader warns when the knob is armed without it. Run-end line on
every consumer path (flush, the in-process harness, the replay driver), silent unless armed:
edges answered, split into appearances and departures, and those the declared-roster rule of
card SID 5b left alone. The reset is the standing preference for a signal without a usable bias
(reset over repair); the carry is exact here because the step is the known applied value, and
it is available through card CC's knob.

## 3. The gates (2 h census replays on the two wires; bars frozen before the knob-on grades were read)

| gate | reading | bar |
| --- | --- | --- |
| P0 | knob off on the baseline wire (`p0g0`) against `t3g0`; the unit suite | `closed_loop_trace.csv` and `closed_loop_sat_trace.csv` byte-identical at 17 of 17; the full Release suite passes; the 4 new tests (`PbHoldPresenceTest`) pass |
| P1 | knob on, run-end line | the answered edges equal the edges the trace shows (ARHT00ATA 3, QUIN00USA 5 on the baseline wire), 0 elsewhere |
| P2 | knob on, the CLV wire (`pfb1` against `t3b1`) | ARHT00ATA hour-2 p50 at most 0.20 m |
| P3 | knob on, both wires | every site with 0 edges byte-identical to its knob-off replay; no site with edges worse than its knob-off replay by more than 0.05 m in hour 2 |

If P0 to P3 pass, the knob is a candidate for the author's arming decision; the in-process
consumer at WTZZ00DEU has 0 edges, so arming it leaves the golden baselines' consumer unchanged.

## 4. Result (2026-09-28, 2 h census replays)

Exe `gnss_processor_pbf.exe` (sha256
`3a3c64ea88f8a750b166cc9158cbb61b244210539422dd5b35d08b43b7962ce1`, main `3ca0626` plus this
card's diff). Every number is read from `runs/_rap_msc_<site>_<key>_d`.

| gate | reading | verdict |
| --- | --- | --- |
| P0 | `p0g0` against `t3g0`: both traces byte-identical at 17 of 17; full Release suite 2 672 passed, 1 skipped (the fixture-gated `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`); `PbHoldPresenceTest` 4 of 4 | PASS |
| P1 | the run-end line answers 3 edges at ARHT00ATA (2 appear, 1 vanish) and 5 at QUIN00USA (2 appear, 3 vanish) on both wires, 0 at the other 15 replays, each a `reset_flip` of the flipped signal only | PASS |
| P2 | ARHT00ATA on the CLV wire: 478.953 / 803.907 to 2.278 / 0.762 m (bar 0.20 m in hour 2) | FAIL as frozen |
| P3 | the 15 replays without an edge byte-identical on both wires; with edges, hour 2: QUIN00USA 0.083 to 0.076 m (baseline wire) and 0.790 to 0.584 m (CLV wire), ARHT00ATA 0.067 to 0.066 m (baseline wire) | PASS |

| replay, hour 1 / 2 p50 (m) | knob off | knob on |
| --- | --- | --- |
| ARHT00ATA, baseline wire | 0.690 / 0.067 | 0.682 / 0.066 |
| ARHT00ATA, CLV wire | 478.953 / 803.907 | 2.278 / 0.762 |
| QUIN00USA, baseline wire | 0.223 / 0.083 | 0.223 / 0.076 |
| QUIN00USA, CLV wire | 2.521 / 0.790 | 2.521 / 0.584 |

**Reading of P2.** The divergence is gone: the knob-on consumer on the CLV wire follows the
knob-off one to TOW 260100 (4.51 m at epoch 30, the same value), answers the G32 L1W appearance
at TOW 260130, and descends to 0.68 to 0.83 m by hour 2 instead of 480 m. What is left is not
the edge. ARHT00ATA's first epochs are already worse on the CLV wire than on the baseline wire
(16.7 m against 10.6 m at epoch 6, 13.5 against 1.6 at epoch 12), the pattern card CLV2's other
C1W sites show, and card CLV2 section 5 attributes that cost to the wire's phase rows. The bar
assumed the edge was the whole of ARHT00ATA's loss on that wire; it is the catastrophic part.

**Verdict: BUILT, default OFF, NOT ARMED; a candidate for the author's arming decision.** Armed,
it changes nothing at WTZZ00DEU and at 15 of 17 census sites in this window (no edge), and it
removes an unbounded consumer failure at the sites that track the L1W family (invariant #7:
both presence edges now have a response). Arming needs a ledger row (the consumer-side posture
of `network.closed_loop.*`) and a golden pin; not done here.

**Owed.**

1. The consumer's clock seed (section 1, side reading 1): `processing.data_gap_tolerance_s` 0
   means 1 s on the real-time path and 1.5 intervals on the post-processing path. Inert on the
   grade; a one-line alignment when the real-time engine is next touched.
2. The Kalman peel's masking (section 1 item 3): a single bad phase row moved the first update
   pass 704 m and every residual over the threshold; the peel then reset the wrong satellites.
   Any other unanswered step (an undeclared producer step, a missed slip) meets the same peel.
   A pre-update screen of the phase innovations against the previous epoch's state would catch
   a 92 m row before it spreads. Not built.
3. The census as a bar: at a site with an unanswered event the hour-1 grade flips on
   last-bit differences in the wire (section 1, side reading 2). Census bars should report the
   per-site list beside the median, and a site that diverges should be read, not medianed.

## 5. Armed (2026-09-28, the author's instruction)

`network.closed_loop.pb_hold_presence_flip: true` in both RF-0hv working bases with a dated
comment, a manifest pin on the frozen golden bases of 2026-09-22 (no second freeze; 32 pins),
the goldens regenerated (`make_golden_config.py --verify` PASS, 63 recommended knobs armed in
regime on both), ledger row RECOMMEND_SCOPED with appendix A13 (regime: the float vehicle, a
closed-loop consumer and the hold), and a loader posture row beside the hold's
(`config_posture_check.py --verify-loader`: 0 coverage gaps). Exe `gnss_processor_pbf2.exe`
(sha256 `73c47ebd7eb610d0dff775133fa5aac351dd8b38a8b5090645699a20f12d04f7`, main `ff36b1f` plus
the posture row).

Confirmation pair, 6 h cold on the live bases with the knob (`runs/_rf0hv11_{field,sim}6`):
0 presence edges at WTZZ00DEU and at SZZ000SIM; products (records), `closed_loop_trace.csv` and
`network_epoch_status.csv` byte-identical to `runs/_rap_owd_{field,sim}6f`, the post-OWD 6 h
references of the same code. Arming therefore changes no baseline number: WTZZ00DEU 0.560 0.059
0.028 0.030 0.039 0.048 m by hour, 611 epochs below a decimetre; the sim consumer as in
`runs/_rf0hv10_sim6`. (Against `runs/_rf0hv10_{field,sim}6` the records differ by card OWD's S
line and the closed loop by OWD's 0.1 mm consumer change, as card OWD recorded.) The consumers
that meet the edge are the replayed census sites that read the L1W phase-bias family.
