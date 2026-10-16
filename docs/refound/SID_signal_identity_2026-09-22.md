# SID: signal identity and arc lifecycle (SignalRoster and ArcLedger)

**STATUS**

- **id:** SID
- **topic:** network-vehicle
- **status:** closed
- **summary:** Phases 1 to 6b BUILT 2026-09-22: 1 to 5a leave solve and consumer unchanged; 5b a trade, OFF; 6a and 6b KEEP OFF; closed, card NOM carries the roster and ledger and the family ignition datum is owed.
- **source:** the author's question of 2026-09-22; cards [CSL](CSL_canonical_signal_latch_2026-09-20.md), [GRS](GRS_gap_reentry_per_signal_2026-09-20.md), [EV7](EV7_all_sat_clock_decouple_2026-09-21.md), [SCC](SCC_second_code_channel_2026-09-21.md), [GRK](GRK_gap_reentry_key_mismatch_2026-09-22.md) (the six defects of one shape); the audit report (`runs/_msc/sid_audit_report_2026-09-22.md`, 912 lines, outside the repository with the run directories; its tables are summarised here and its measurements are reproducible from the run directories named); the audit was read-only, and the author passed its recommended fixes to the build
- **knobs / posture:** `network.signal_roster` (phase 1), `network.roster_tracker` (phase 2), `network.ledger_reentry` with `network.ledger_reentry_no_verdict` (phase 3), `network.ledger_screens` and `network.ledger_anchor` (phase 4), `network.ledger_emit` and `network.closed_loop.roster_latch` (phase 5), `network.second_phase_channel` (phase 6a), `processing.channel_mode: all` on the network path with `network.closed_loop.channel_mode` (phase 6b), all built 2026-09-22, default OFF or single; the family ignition datum named as the next read in section 12. Posture: the RF-0hv working bases (baseline field `runs/_rf0hv8_field6`, sim `runs/_rf0hv7_sim6`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** `runs/_rf0hv8_field6` (all Part D numbers), `runs/_rap_k1_strip_l1c`, `runs/_rap_grk_g1_field_on`; traces `network_slip_raw.csv`, `network_amb_state.csv`, `network_events.csv`, `network_pf_rows.csv`, `closed_loop_sat_trace.csv`, `products.txt`; phases 1 to 6b main `6202cbc`, `2ea87bb`, `ff8b05b`, `c85e03c`, `66a1f12`, `1dc05b0`, `817367e`, exes `gnss_processor_sid1.exe` to `gnss_processor_sid6b.exe`, arms `runs/_rap_sid1_*` to `runs/_rap_sid6b_*`

**Outcome.** Question: do six defects share one missing layer (signal choice per band, arc edges with identity)? The roster and arc ledger reproduce the audit and GRK exactly, solve unchanged (phases 1 to 5a). Missed: 5b's no-site-worse bar (a trade, off); 6a's bars (the L1W hole is receiver tracking: 18 of 58 stations); 6b's field bars (`channel_mode: all` freezes C2L, a family without a canonical station). Closed: [NOM](NOM_network_object_model_2026-10-08.md) carries the roster and ledger; the family ignition datum and the emitter's identity-less edges are [owed](../status/OPEN_ITEMS.md); minor leftovers are moot. Status reviewed 2026-10-13.

## 1. The question and the reading it tests

The author: the ultimate goal is all satellites, frequencies and channels; the project keeps
having problems with which signals or combinations are checked or used, and with intermittent
drop behaviour. The manager's reading, tested by the audit: the estimator (uncombined, one
ambiguity and one phase bias per signal, receiver code biases per signal, S-basis datum) is
organised for every signal; what fails is the layer deciding which signal stands for a band at a
station, decided independently in several places, and the arc lifecycle (birth, gap, re-entry,
death of a (station, satellite, signal)) kept in many containers with no shared state machine.

## 2. Inventory (the audit's Parts A to C)

- **Selection rules: 128 sites in 38 files** (26 the station latch and its readers, 17
  measurement construction, 18 slip detection, 41 datum, pin and anchor elections, 16 emission and
  consumer, 10 assembly and allow list). At least six independent implementations of "the two
  lowest distinct bands" with different tie-break rules: `NetworkRawObservables.cpp` (band digit
  alone, then `ph.front()`), `NetworkSbasis.cpp` (band digit and name, distinct FREQUENCY),
  `NetworkFastClock.cpp` (`bandOrderKey` then code name), `NetworkDdPriorBuild.cpp` (band digit and
  name, then a census channel), `RealTimeEngineEstimate.cpp` (lexically smallest per band),
  `IonoVariabilityMonitor.cpp` (first phase per band, first two map entries). Nothing compares
  them. The tracker's comparator compares the band digit ALONE under `std::sort`, which is not
  stable, so the same-band order the whole GRK read depends on is formally unspecified (the
  toolchain happens to give lexical order).
- **The collision is already documented once, and worked around locally**:
  `NetworkRunContext.cpp` near line 4842 states that the raw pass names a band by the
  lexicographically first attribute (L1C) while the carry key uses the estimator's choice (L1W),
  and resolves it by band digit for the lane observatory alone.
- **Arc-keyed state: 29 containers** in `NetworkRunContext.hpp` match the audit's patterns (5 at
  signal level: `local_carry`, `lane_t4_pre`, `relevel_retap_streak`, `relevel_retapped_prev`,
  `relevel_pend`; 24 at satellite or station level); a wider sweep finds 44 plus 6 lifecycle
  holders outside the file. **31 have both edges silent**, among them `local_carry`'s gap prune
  (`NetworkRunContext.cpp` near line 5124, nothing emitted), `RawSlipHistory` (never erased, no
  forget horizon) and `StateManager::pruneAmbiguitiesExcept`, which destroys an ambiguity after
  ONE missed epoch with no window and no event: the largest silent death path in the system. The
  lifecycle vocabulary exists three times in three incompatible key spaces (`adm_arc` with birth,
  re-entry, resume, onset, heal; `pb_hold` with a generation counter; `gl_state` with declare,
  clear, expire) and is absent from the one container the producer depends on.
- **Information gates: 29**, of which 4 have both edges instrumented (measurement admission,
  gross LOS, pin bridge, phase-bias hold) and 2 have identity at neither edge: the priority latch
  (a signal excluded permanently with no event or counter) and the consumer's freshness flip
  (which corrections are applied changes with no event row).

## 3. Measured on the field baseline (`runs/_rf0hv8_field6`, 58 stations, 6 h)

| quantity | value | source |
| --- | --- | --- |
| stations tracking more than one band-1 phase / band-2 phase / band-1 code | 42 / 56 / 47 of 58 | RINEX headers |
| latch: band-1 phase L1C 40, L1W 18; band 2 L2W 58 | | `stdout.txt` |
| carried signal sets that disagree with the latch | 0 of 116 station-band cells | `network_amb_state.csv` |
| stations where the count rule and the priority rule pick differently | 6 (BOGT, KOKV, STHL, ULAB, URUM, WIND): card CSL's six | `stdout.txt` runner-up counts |
| **station-band cells where the tracker's signal is not the carried one** | **46 at 44 of 58 stations**: band 1 at all 18 L1W stations (100 percent of rows), band 2 at 28 stations (L2L or L2S before L2W; 63 to 85 percent of rows) | `network_slip_raw.csv` against `network_amb_state.csv` |
| gaps of 2 to 30 epochs at carried arcs | 195 at 131 arcs; 127 answered at the return epoch, 68 not | `network_amb_state.csv`, `network_events.csv` |
| band 1: mismatched cells answered / matched cells answered | **0 of 31** / 55 of 67 | same |
| band 2: mismatched / matched | 10 of 14 (the per-signal tier of card GRS answers them) / 62 of 83 | same |
| L1W row-gap re-entries answered | **0 of 30** (L1C 23 of 30, L2W 33 of 54) | same |
| **wire phase-bias family missing while a clock record exists** | **L1W 5 323 of 22 586 satellite-epochs (23.6 percent), 17 of 32 satellites**; L1C 113 (0.5 percent); L2W 0 | `products.txt` |
| carried stations per column-epoch, median | L1C 8, **L1W 4**, L2W 12; 21 of 32 L1W families below 3 stations in more than 10 percent of their epochs, four of them always (G01, G04, G16, G26) | `network_amb_state.csv` |
| code-bias families missing (after card SCC) | C1C 0, C1W 2, C2W 0 | `products.txt` |
| gate drop edges (card EV7) | 13, all on L1W keys; 3 017 gated column-epochs | `network_events.csv` |
| the consumer at WTZZ00DEU (tracks L1W, C1W, L2W, C2W) | 0 of 5 809 satellite-epochs without a matching wire record: it sees 8 satellites of the 24 the L1W family covers | `closed_loop_sat_trace.csv` |

Readings:

- The GRK defect is total where it applies (0 of 31) and its exposure is 18 stations, not 10: all
  58 stations report C1C, so the tracker names band 1 L1C at every L1W station; the ten of card
  GRK are the ones that had a shifted re-entry in the window. The emulation arm
  (`runs/_rap_k1_strip_l1c`) removed 10 of the 46 cells (8 band-1 stations and all 28 band-2 cells
  remain), so card GRK's frozen gate G1 ("ON equals the emulation") compared two different
  interventions: recorded in that card. The band-2 half is real but covered by the per-signal
  tier in 10 of 14 cases; no arm isolates its cost.
- **The L1W phase-bias hole is the phase-side twin of the defect card SCC fixed on the code side**
  (C1W 5 210 missing satellite-epochs to 2). A station contributes one phase per band, so 18
  stations carry the whole L1W family at a median of 4 stations per column; that is why the
  coverage gate has its 13 edges on L1W keys (card EV7), and it is the same class card MSC
  measured as code-bias dropouts of tens of metres at QUIN00USA. Only a mechanism that lets a
  station contribute more than one phase per band closes it (section 4, phase 6). It is also a
  candidate for the half-read gain of card CSL (18 stations moving INTO a family whose support is
  4 is where a marginal station is worth most): consistent, not proven.
- A measurement trap for future readers of `network_amb_state.csv`: the `drop:stale_gap` row is
  written 900 s after an arc's last observation; counting it as presence manufactures a gap of
  exactly 30 epochs at every pruned arc (426 phantom gaps here). Exclude `drop:stale_gap` and
  `erase:*`.

## 4. The design and the plan (the audit's Part E, adopted)

**SignalRoster**: one object per epoch, owned by `NetworkRunContext`, answering for every
(station, constellation, band): the TRACKED signals (raw, this epoch), the MODELLED phase and code
(the latch's choice; a set after phase 6), the DATUM signal the S-basis pins, the COMPANION codes
(card SCC) and the EXCLUDED signals with their reason. Decided ONCE per epoch by
`networkBuildSignalRoster` at the driver site immediately after the canonical-pair-admission seam
returns the epoch (`ApplicationNetwork.cpp` about line 1410, before `networkPassAHead_`), from
`PppModel::exportCanonicalSignals()`, the raw records and the config. 22 sites of the inventory
become readers (the tracker pair, the MW channel choice, the IF and GF pairs, the fast-clock
pair, the lane census, the consumer lookup by ROLE, the allow list as an `Excluded` reason, the
band-digit workaround deleted); the elections that pick an ELEMENT rather than a signal (reference
satellites, pivots, the eq-34 datum receiver, the anchor election, RAIM's band to spare) stay; the
IF pin pair becomes a roster WRITER (`datum_phase`, `datum_code`).

**ArcLedger**: one record per (station, satellite, signal), owned by `NetworkRunContext`, written
at the same point: states Absent, Tracking, Gap, Reentered, Dead; the epochs of each transition,
the gap length, a generation counter (increments at every birth), the roster role, a reason mask
at death. Events, tier `ARC`: `arc_birth`, `arc_gap`, `arc_reentry` (gap length, generation, the
slip tier's verdict, whether a carry was reachable), `arc_death` (reason), holdoff-throttled from
the start (about 1 200 live records per epoch at 58 stations, 6 000 at 305). `local_carry`,
`StateManager`'s ambiguity map, the anchor incumbents, the emitter baselines, `pb_hold`'s
generation and the consumer's freshness maps become READERS (the ledger owns WHEN, the carry owns
WHAT); `si_arc` and the one-epoch presence memories are REPLACED; the fully evented machines
(`gl_state`, `pin_bridge`) stay and write the reason mask.

**`channel_mode: all`** (the road to every channel): what refuses or assumes pairs today is the
SCC interlock, the `PppModel` widening (adds every attribute of a selected band, never a band),
the MW detector's mirrored channel mode, `resolveReceiverBias` keying the receiver code bias on
the band digit (C1C and C1W both `GPS_DCB`: an all-channel run asserts a zero inter-signal bias on
every extra channel, the class card B5D paid for), and six hard-coded "two lowest distinct bands"
sites plus two that assume exactly two. What the roster must carry: a SET of modelled phases per
band, each with its own ambiguity, phase-bias column and a receiver inter-signal PHASE state
(the twin of SCC's `<sys>_ISC_<signal>`), a set of modelled codes (SCC), and a DATUM designation
per family; the S-basis needs a same-band free companion for a second PHASE family (the code side
has `n_sat_gf_sameband_free`, the phase side nothing; card B5D built the band-5 level row). On
this roster it would add about 9 L1C-contributing stations to each L1W column and 4 the other
way: the coverage argument card SCC won on the code side.

**Phases** (each default OFF, byte-identical OFF; G0 form of cards EV7 and SCC; field bar = the
16-site census plus WTZZ00DEU; sim bed `runs/_rf0hv7_sim6`, one signal per band, so identity is
the expected sim result through phase 5):

| phase | what | bar | size (lines) |
| --- | --- | --- | --- |
| 1 observatory | roster and ledger built, read by nobody; counters of every disagreement class (roster against each reader site, ledger against each container); run-end table | solve byte-identical ARMED; the table reproduces section 3 (46 cells, 6 stations, 195 gaps, 68 unanswered, 13 edges); zero disagreements on the sim | 1 400 to 1 800 |
| 2 tracker reads the roster | card GRK's knob becomes "read `modelled_phase`"; the lexical sort and the unstable comparator disappear | gross code rows at or below 103, refloats not above the GRK arm, WTZZ00DEU within 0.005 m, census not worse | 150 to 250 |
| 3 carry and re-entry key on ledger arcs | `local_carry`'s prune, both re-entry tiers and `pruneAmbiguitiesExcept` consult the ledger; the prune emits `arc_death` | unanswered re-entries 68 toward the 12 genuinely unmeasurable; no epoch with 8 or more clock trips; census not worse; sim within 0.01 m. SOLVE-AFFECTING: graded as a change | 350 to 500 |
| 4 coverage gate and anchor election read the ledger | the EV7 drop edge becomes an `ARC` transition; a returning key is a re-entry, not a first sighting | 13 edges still handled, carried jumps below 0.5 m; the E4b relabel bed of card EV7 | 250 to 350 |
| 5 emitter hold, consumer latch and freshness read the roster | `pb_hold`'s generation derives from the ledger; freshness flips become events; the producer DECLARES its roster on the wire | card MSC's 50 dropouts each attributable to a named transition; QUIN00USA without an unexplained excursion | 300 to 400 |
| 6 `channel_mode: all` on the network path | a set of modelled phases per band, the inter-signal phase state, the phase-family level row, the pair sites reading the roster, the SCC interlock lifted | the L1W phase-bias family at 31 or more satellites per epoch (24 today) and at most 100 satellite-epochs without one (5 323 today): the G1 form card SCC used; WTZZ00DEU within 0.03 m; census median not worse; sim with a cloned second phase | 700 to 1 000 |

Phases 1, 2 and 4 are independent; 3 needs 1; 5 needs 1 and 3; 6 needs 1 and should follow 3.
Risks recorded by the audit: the roster must reproduce the latch exactly on its first pass
(phase 1's armed identity protects this); ledger scale (holdoff from the start, cost measured
against the 7.6 ms of `trace_prefit_census`); phase 3 changes which columns exist; the
warm-resume fingerprint from phase 3 on; phase 6's null direction (the ambiguities absorb a
constant per arc, so the datum work is the hard part).

## 5. Phase 1, pre-registered (the observatory)

- P1. Knob `network.signal_roster` (bool, default OFF). Armed: `networkBuildSignalRoster` fills
  `ctx.roster` and advances `ctx.arc_ledger` once per epoch at the site named in section 4;
  nothing reads either. The solve, the wire and every trace of the baseline are byte-identical
  armed and disarmed (invariant 6, both states).
- P2. Roster contents per (station, constellation, band) as in section 4, with the latch read
  from `PppModel::exportCanonicalSignals()` and the tracked set from the epoch's raw records. The
  code-follows-phase clause and the allow list are reproduced, not re-derived: the `Modelled`
  role must agree with the latch at every cell (counter `roster_latch_disagree`, bar 0).
- P3. Ledger transitions and horizons as in section 4; the horizon is the maximum of the readers'
  gap windows; the record is kept for the forget window after death; events `arc_birth`,
  `arc_gap`, `arc_reentry`, `arc_death` on tier `ARC`, throttled per station by the holdoff
  pattern of `NetworkSignalIntegrity.cpp`, with run-end counts per transition.
- P4. Disagreement counters, each a class with a name and a count in the run-end table, both
  knob states where the inputs exist OFF: tracker signal against `modelled_phase` per band
  (cells and rows); count rule against priority rule per station; `local_carry` presence against
  the ledger state (an entry inside the window whose arc is `Dead`, an arc `Tracking` without an
  entry); `StateManager` ambiguity presence against the ledger (a state destroyed while the arc
  is `Gap` inside the horizon); anchor incumbent against the ledger (a `Reentered` key treated as
  a first sighting); phase-bias family support per column-epoch (carried stations, the L1W median);
  wire family coverage (satellite-epochs with a clock and no bias record per family, from the
  emitter's own bookkeeping); gate drop edges on the ledger's `Gap` arcs.
- P5. Tests: `tests/test_signal_roster.cpp` (suite `SignalRosterTest`: the roster reproduces the
  priority latch and the count latch on synthetic headers; companions and exclusions carry their
  reason; a band the model does not carry is `Tracked` only) and `tests/test_arc_ledger.cpp`
  (suite `ArcLedgerTest`: the state machine over a synthetic presence sequence, the generation
  counter, the horizon, the forget window, the event count per transition). New `.cpp` files in
  `CMakeLists.txt`.
- P6. Run-end table in `NetworkTeardown.cpp`, manifest object `signal_roster` in
  `NetworkRunTraces.cpp`, a per-epoch trace `network_arc_ledger.csv` behind the knob (one row per
  transition, not per record).

Gates:

- G0. Knob absent: the three graded files identical on both beds.
- G1. Knob ON, field (`runs/_rf0hv8_field6/arm.json`, 6 h): the three graded files identical to
  the baseline; `roster_latch_disagree` 0; the table reads tracker mismatch cells 46 at 44
  stations (band 1 18, band 2 28), count-against-priority stations 6, gaps of 2 to 30 epochs at
  carried arcs 195 with 68 unanswered (within 5 percent each, the audit's numbers came from the
  traces and the ledger sees the same epochs), gate drop edges 13, L1W wire coverage missing
  5 323 satellite-epochs (within 1 percent), median carried support L1C 8, L1W 4, L2W 12.
- G2. Knob ON, sim (`runs/_rf0hv7_sim6/arm.json`): the three files identical; every disagreement
  class 0 except the gap and family-support classes, which report the sim's own values.
- G3. Cost: the epoch time of the field run within 3 percent of the baseline
  (`network_epoch_status.csv` or the section timers), the ledger's own section timer reported.
- B1. Byte identity is the bar; there is no consumer bar for an observatory.

Invariants pre-flight: 1 not applicable (no stiff quantity is added); 3 deferrals triaged at the
build; 4 the table is the telemetry, and an OFF run prints the classes whose inputs exist OFF;
6 byte identity in both knob states; 7 the ledger IS the edge instrument, so its own events are
the pre-flight for phases 3 to 5.

## 6. Phase 1 result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`; rapid mode, 2 h arms)

Main `6202cbc`. Knob `network.signal_roster` (default OFF). `include/app/SignalRoster.hpp`,
`src/core/app/SignalRoster.cpp`, `include/filter/ArcLedger.hpp`, `src/core/filter/ArcLedger.cpp`;
suites `SignalRosterTest` (10) and `ArcLedgerTest` (8); `gnss_tests` 2452 ran, 2451 passed, 1
known skip; exe `gnss_processor_sid1.exe` (`AB665086...`). Two driver calls, not one: the roster
and the ledger are built at the site of section 4 (after the canonical-pair admission, from the
epoch's `RinexObservation`, since `raw_observed` is filled downstream), and a counters-only
audit pass runs after the clock update, where the tracker's rows, the carry, the ambiguity
states, the anchor election and the emitted solution are all in scope. `PppModel` gained a
const allow-list getter and knob-gated retention of the canonical candidates (the count rule
against the priority rule cannot be read from the runner-up the model logs, which is computed
under the station's own rule). `datum_phase` and `datum_code` stay empty until phase 2 makes
the IF pin a roster writer. Horizon 3900 s (the largest reader window, `readmit_seed_horizon_s`);
forget window 2880 epochs; events throttled per (station, transition) over 30 epochs, births
suppressed at the first epoch; the trace `network_arc_ledger.csv` is complete. Section timer
`SB_ROSTER`. Not fingerprinted.

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knob absent, three files identical on both beds; the 2 h controls equal the 6 h baselines over the shared window | identical; `runs/_rap_sid1_g0_field4` equals `runs/_rf0hv8_field6` and `_sid1_g0_sim4` equals `_rf0hv7_sim6` on the first 241 consumer rows | PASS |
| G1 identity | field ON identical to its control | `runs/_rap_sid1_g1_field_on4`: all three files identical | PASS |
| G1 `roster_latch_disagree` | 0 | 0 | PASS |
| G1 tracker against carry | 46 cells at 44 stations, band 1 18 / band 2 28 | 46 of 116 at 44, 18 / 28 (the worker's own Python over the OFF traces: the same) | PASS, exact |
| G1 count against priority | 6 stations | 6 on the band-1 phase (22 differ on some band and type) | PASS, exact |
| G1 gaps and answers | 195 / 68 at 6 h | 51 carry-side gaps of 2 to 30 epochs in 2 h, 16 unanswered (Python 11: a different answer test) | population reproduced; the 6 h magnitude is not gradable at 2 h |
| G1 gate drop edges | 13 at 6 h | 4 in 2 h, all on a family with an arc in a Gap | rate consistent |
| G1 L1W wire coverage | 23.6 percent missing | 1822 of 7565 (24.1 percent); Python 1830 of 7597 | PASS on the rate |
| G1 carried support | L1C 8, L1W 4, L2W 12 | 8.5 / 4.0 / 13.0 (Python 8.0 / 4.0 / 12.0) | PASS |
| G2 | sim ON identical; identity classes 0 | identical; every identity class 0 (0 of 120 cells, 0 of 60 stations, 0 edges) | PASS |
| G3 | field loop within 3 percent | 84 918.6 to 85 179.9 ms over 240 epochs (+0.31 percent); the roster's own section 8.25 ms per epoch (build 5.41, audit 2.80), 2.3 percent of the loop; sim +3.6 percent of wall with the section at 2.5 percent | PASS |

Readings:

- The observatory reproduces the hand audit's identity population exactly and at a cost the
  loop does not see, so the hand audit is retired: the table is the instrument from here.
- **Structural, for phase 3:** the ledger's population is RAW-side (the arrival site is
  upstream of the elevation mask, the allow list and every admission screen), while
  `local_carry` and the ambiguity states are admitted-side. The two are reported separately
  and never added (raw-side 29 gaps with a live carry against 51 carry-side); classes (c) and
  (d) carry the same difference (84 256 modelled arcs Tracking without a carry entry, 52 851
  modelled arcs in a Gap whose state is already gone; on the sim 43 833 and 40 370). Before a
  reader keys on the ledger, the ledger needs an ADMITTED lifecycle beside the raw one (one more
  timestamp per record, `ep_last_admitted`, and the Gap test the carry readers use runs on it);
  the tracker (phase 2) reads the raw side, the carry, the prune and the re-entry response
  (phase 3) read the admitted side.
- The anchor election treated 4 returned families as first sightings in 2 h (class e), the
  EV7 class seen from the ledger; the ambiguity prune destroyed the state of 52 851 raw-side
  gapped arcs inside the horizon (class d), which is the silent death path of section 2
  quantified, on the raw population.
- The audit's "answered" test and the ledger's differ by 5 of 51 (the ledger reads
  `relevel.free_ambs` with the gap cause; the Python matched `reentry_relevel` events); phase 3
  fixes the definition when the response itself keys on the ledger.

Phase 2 (the tracker reads the roster) and phase 3 (carry, re-entry and the prune on the
admitted lifecycle) are next, in that order; phase 2 retires card GRK's knob by construction.

## 7. Phase 2 result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`; rapid mode, 2 h arms)

Main `2ea87bb`. Knob `network.roster_tracker` (default OFF; refused without `network.signal_roster`).
Armed, the arc-level tracker names each band by the roster's `modelled_phase` of the current
epoch (the station latch is the fall-back on a constellation's FIRST epoch, where the roster has
no modelled role yet: the latch is decided inside `computeModels`, downstream of the build site;
955 fall-backs and 60 lookups neither could answer, all in the first epoch), so card GRK's latch
lookup is superseded by construction. The S-basis IF pin is a roster writer: it is elected per
satellite and realized in `NetworkSolveGlue`, so the roster cell (station, constellation, band)
takes the per-band mode over the epoch's pinned satellites (27 838 cells with a datum code,
23 421 with a datum phase, 4 417 off-model where the pin names C1C at an L1W station, 0
ambiguous); a defect found at the gate (the datum seam keys a constellation by the satellite
id's first character, the roster by the system string, so the first armed run wrote no datum
cell) was fixed and pinned by a unit case. Each `ArcRecord` carries the ADMITTED lifecycle beside
the raw one, fed from the modelled row set after the elevation mask, the allow list and the
latch (the set `local_carry` and the ambiguity states are keyed from); `arc_reentry` events carry
both gap lengths; `network_arc_ledger.csv` gains the admitted columns and rows. Not
fingerprinted. `SignalRosterTest` 14 and `ArcLedgerTest` 13 cases; `gnss_tests` 2461 ran, 2460
passed, 1 known skip; exe `gnss_processor_sid2b.exe` (`B51822D2...`).

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knob absent, identical to the baselines over the shared window | `runs/_rap_sid2_g0_field2` equals `runs/_rf0hv9_field6` and `_sid2_g0_sim2` equals `_rf0hv9_sim6` | PASS |
| G1 | the roster reader byte-identical to the control, with and without card GRK's knob | `runs/_rap_sid2_g1_field2` and `_sid2_g1b_field2` (GRK's knob off): all three files identical; tracker mismatch 0 of 116 cells; unreachable re-entries 0 of 21 | PASS |
| G2 | sim identical | `runs/_rap_sid2_g2_sim2`: identical; identity classes 0 | PASS |
| G3 | the admitted lifecycle stated beside the raw one | (c) 23 915 modelled arcs Tracking without a carry entry (raw 84 256); (d) 41 543 in a Gap inside the horizon whose state is gone (raw 52 851); carry-side gaps of 2 to 30 epochs 49 with 37 answered and 12 not (raw-stamp reading 51 / 14; the worker's Python 51 / 13) | read |
| G4 | cost | field loop 98 510 to 101 506 ms over 241 epochs (+3.0 percent); roster section 2 635 ms (2.6 percent), 11.1 ms per epoch (build 5.6, audit 5.5) | read |

Readings: the reader reproduces GRK exactly, so the tracker no longer decides a signal; the
admitted lifecycle halves the raw-side gap population and is the one phase 3 keys on; the 12
unanswered carry-side re-entries of the 2 h window are phase 3's target population; the cost
rose from 8.3 to 11.1 ms per epoch with the admitted pass and is to be measured again when the
readers land (the audit pass becomes cheaper as containers turn into readers).

## 8. Phase 3 result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`; rapid mode, 2 h arms and one 6 h arm with the census)

Main `ff8b05b`. Knob `network.ledger_reentry` (default OFF; refused without `network.signal_roster`,
`network.roster_tracker` and `network.cycle_slip.gap_reentry_relevel`); sub-knob
`network.ledger_reentry_no_verdict` (`bridge`, the default, or `refloat`). Armed, the gap re-entry
response takes its population from the ledger's ADMITTED lifecycle (every arc whose admitted state
goes Gap to Reentered at the epoch, whose signal the roster calls Modelled, whose gap is inside
`phase_leveling_max_gap_s`) and looks each arc's verdict up in the raw tiers' results (the
per-signal resumption for the exact arc wins, else the satellite's row-level one with the LLI
band-scoped); the response key IS the ledger arc's key, so card GRK's unreachable count is 0 by
construction. An arc no raw tier saw has no verdict and follows the sub-knob. The leveling carry's
900 s prune emits `ARC arc_death reason=horizon` (buffered at the prune, evented in the audit pass:
the prune site has neither the traces nor the epoch); `StateManager::pruneAmbiguitiesExcept` takes
an optional sink and every pruned column whose admitted arc is in a gap inside the horizon is
marked, so the next `arc_reentry` carries `state_recreated`. The population is a query on
`ep_last_admitted` (the drain runs in PASS A; the machine advances after the clock update). The
"answered" split is read off the response's own record per arc. Fingerprinted, armed-only.
`LedgerReentryTest`, 13 cases; `gnss_tests` 2474 ran, 2473 passed, 1 known skip; exe
`gnss_processor_sid3.exe` (`41C04F7F...`). A defect of the build found and fixed before the graded
arms: the prune mark was drained before the admitted advance and never fired (507 pruned, 0 marked);
after the fix 507 of 507, 476 of 476 (sim), 1530 of 1530 (6 h).

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knobs absent, identical to the baselines over the shared window | identical on both beds | PASS |
| G1 | roster and tracker only: identical to phase 2's control | identical (the carry-death events and the prune marks change no solve; `arc_death` 94 to 168 in the events) | PASS |
| G2 | `ledger_reentry` (bridge) against G1: within 0.010 m per hour, no epoch with 8 or more trips | byte-identical; 100 arcs, 100 with a verdict (69 row-level, 31 per-signal: 57 shifted, 12 LLI, 31 clean), 53 freed / 31 bridged / 16 no carry; trips max 1 per epoch; unreachable 0 | PASS |
| G3 | `refloat` against G2 | byte-identical (the policy is inert while `gap_reentry_relevel_unmeasurable` is OFF, the base's value) | PASS, policy unexercised |
| G4 / G5 | sim within 0.010 m per hour | byte-identical; 30 arcs, 30 with a verdict, 20 freed / 8 bridged / 2 no carry | PASS |
| G6 | 6 h against `runs/_rf0hv9_field6`; census not worse | `runs/_rap_sid3_g6_field6` byte-identical on the three graded files over 6 h (0.560 0.059 0.028 0.030 0.036 0.045 m); 301 arcs, 214 with a verdict (140 row-level, 74 per-signal: 131 shifted, 33 LLI, 50 clean), **87 with none**; 147 freed / 137 bridged / 17 no carry; carry prune 1265, state prune 1530 marked; census key `sid3` against `grk` identical at 16 of 16 sites | PASS |

The refloat policy, isolated at 6 h with `gap_reentry_relevel_unmeasurable` armed
(`runs/_rap_sid3_g8_field6` against `_g9_field6`, which differ by the policy alone): 238 freed
against 151, the 87 no-verdict arcs exactly, and the wire and the consumer stay byte-identical (only
prior-residual diagnostics move on 19 of 720 epoch-status rows; 86 of the 87 never reach a live
carry at the inject). `_g9` against `_g6` (the sub-knob alone) moves the solve as it always did.

Readings:

- On this posture the ledger-driven response is behaviourally a no-op: it reproduces the
  raw-driven response arc for arc at 2 h and byte for byte over 6 h and 16 replayed sites, while
  making explicit what was inferred before (the population, the verdict source, the two silent
  prune edges). After card GRK there was no residual hole in the re-entry response on this bed:
  the audit's 68 unanswered were the 31 of GRK's class plus clean-verdict bridges and arcs with no
  carry to protect, which the unified split now names (6 h: 115 in the population, 85 freed, 11
  bridged by verdict, 1 bridged with no verdict, 4 no carry, 14 no carry entry).
- The no-verdict population (0 in 2 h, 87 of 301 in 6 h) is a screen-side decoupling: the modelled
  row of a signal disappears while `raw_observed` keeps it and the satellite stays modelled, so
  neither raw tier sees a break. The ledger's admitted reason mask carries only `horizon` today;
  the writers at each screen (mask, allow list, admission, gate) are phase 4's work, and attributing
  the 87 needs them. Freeing them costs nothing at the wire on 6 h of field data; grading `refloat`
  needs a longer window or the slipped sim bed.
- Phase 3's target as pre-registered ("68 unanswered toward the 12 genuinely unmeasurable") is met
  by the partition, not by a change of behaviour: 0 unmeasurable verdicts exist in 6 h; every
  arc the tiers see is answered by its verdict, and the arcs they do not see are named and counted.

Phases 4 (the coverage gate and the anchor election on the ledger, with the reason-mask writers)
and 5 (the emitter hold, the consumer latch and freshness on the roster; the roster declared on the
wire) follow; card RF-3's code-row QC is next in the author's plan.

## 9. Phase 4 result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`; 6 h arms where the EV7 edge sits)

Main `c85e03c`. Two knobs. 4a, `network.ledger_screens` (default OFF; refused without
`network.signal_roster`): writers at every screen that removes a modelled row, a satellite or a
station record the arcs and a reason bit (`mask`, `nomodel`, `allowlist`, `admission`, `readmit`,
`gate`, `gross`, `deweight`, `slip`, `horizon`; `unknown` when none fired), folded into the ledger's
admitted gaps by the audit pass; the anchor election is read against the ledger (a first sighting
whose family has arcs in Reentered or Gap is counted and evented with identity; each coverage-gate
drop edge reports its arcs' states and the generation test at the return; `anchor_arc_age` is
compared with the ledger's admitted age). 4b, `network.ledger_anchor` (default OFF; refused without
4a; fingerprinted armed-only): at the election's first-sighting branch, a family the coverage gate
dropped that returns inside the horizon with arcs of the SAME admitted generation restores its
previous incumbent as the candidate of record, subject to the existing usable-key, ban, hygiene and
held-basis rules (a refusal is counted); a new-generation return proceeds as today. New seam
`NetworkAnchorReentry`; `LedgerAnchorTest` 9 cases, `ArcLedgerTest` +9, `SignalRosterTest` +3;
`gnss_tests` 2495 ran, 2494 passed, 1 known skip; exe `gnss_processor_sid4.exe` (`1DE3CD38...`). A
defect of the build found and fixed before the graded arms: the reason expansion scanned past its
station and would have attributed neighbouring stations' gaps to the mask.

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knobs absent, identical on both beds | identical | PASS |
| G1 (4a) | byte-identical armed; the table | `runs/_rap_sid4_g1_field6` identical to `runs/_rf0hv9_field6` over 6 h, `_g1_sim2` identical to the sim base; 1 530 admitted gaps stamped: mask 1 212, nomodel 163, unknown 152, gate 13, admission 3; **phase 3's 87 no-verdict returns attributed completely: admission 61, nomodel 26, 0 unattributed**; 114 families elected without an incumbent, 19 with a returning arc (2 same-generation and 4 new-generation returns of a gate-dropped family, 13 without memory); the 13 gate-drop edges carry 26 Tracking, 13 Gap or Reentered, 40 Dead arcs, 0 with a carried jump above 0.5 m | PASS |
| G2 (4b) | field 6 h, stated either way; census not worse | `runs/_rap_sid4_g2_field6` byte-identical to the base and to G1 on the three files while EXERCISED: 1 restore (TOW 272430, G18 L1W, BOGT00COL: the station the coverage rank also picks), 1 same-generation return refused by the held-basis rule (TOW 261900, G32 L1W), 4 new-generation returns; trips max 1 per epoch; census key `sid4` against `grk` identical at 16 of 16 | PASS |
| G3 (card EV7's relabelled bed) | reproduces the class; graded ON against OFF | `runs/_rap_sid4_g3_sim_off` byte-identical to `runs/_rap_w5_sim_off` (10 edges, 4 with a jump above 0.5 m, 6 trips reproduced); `_g3_sim_on` byte-identical to it (0 restores, 1 refused, 1 new) | PASS |
| G4 | cost | the roster section 3.9 to 6.0 ms per epoch, 1.2 to 1.7 percent of the loop (phases 1 to 4 share the seam; the loop totals of the batch are confounded by four concurrent arms) | read |

Readings:

- The no-verdict population is a screen-side decoupling, now named: 61 of the 87 returns follow a
  measurement-admission withhold and 26 the model's own refusal (no product, no pair, eclipsing);
  none is unattributed. Nothing in the slip tiers can see either, and card RF-3 shows what the
  response's re-coupling does at those returns.
- The election treats 19 of 114 incumbent-less elections as first sightings although the family has
  a returning arc; the behaviour half restores the incumbent in the one same-generation case the
  rules allow, and on this posture the coverage rank elects the same station, so nothing moves. On
  card EV7's bed the one same-generation return is refused by the held-basis minimum-age rule. The
  class is real, exercised, and inert at 6 h on both beds: the knob stays built and OFF, and the
  first-sighting events are the instrument to watch on longer or busier beds.
- Writers that could not attach: the readmit park runs downstream of the audit pass and removes
  carries, not rows (field-inert, 0 gaps); `exclude_unmodeled_antenna_stations` is a setup-time
  decision with no per-epoch arc (not armed on either base). The allow-list writer is inert on both
  bases. The `anchor_arc_age` comparison has a zero denominator here (the hygiene seam is not armed
  on this posture): built, unverified. The 152 `unknown` gaps (9.9 percent) reach no return the
  response sees and are left for a later writer sweep.

Phase 5 (the emitter hold, the consumer latch and freshness on the roster; the roster declared on
the wire) is the last identity phase before `channel_mode: all`.

## 10. Phase 5 result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`; 2 h arms, one 6 h wire and two 16-site censuses)

Main `66a1f12`. Two knobs. 5a, `network.ledger_emit` (default OFF; refused without
`network.signal_roster`): the emitter's per-family bookkeeping is READ against the ledger (E1: the
phase-bias hold's generation against the ledger's admitted arc births at the same epoch, in both
directions; the emitter baselines' first sighting against the ledger's memory of the family), the
producer DECLARES its roster on the wire (E2: record `N,<constellation>,<band digit>[,ph=..][,co=..]
[,comp=..][,dph=..][,dco=..]`, one per constellation and band, the lists ordered by producer
station support with a lexical tie break, written as a block ahead of the epoch's records only when
the declaration changes; the legend comment extended, so every file this build writes carries one
more comment line in both knob states), and the consumer's side is counted (E3: its band latch
compared with the declaration; its freshness flips become events with identity, attributed at the
flip epoch as off-wire, undeclared or stale). 5b, `network.closed_loop.roster_latch` (consumer side,
default OFF, inert without a declaration on the wire): C1 the consumer's canonical signal per band
follows the declared primary when the consumer tracks it; C2 a fresh-to-stale flip of a family the
declaration does not carry is not a staleness event. New seams `NetworkLedgerEmit` and
`SignalDeclaration`; `LedgerEmitTest` 17 cases; `gnss_tests` 2523 ran, 2522 passed, 1 known skip;
exe `gnss_processor_sid5.exe` (`05C58FEC...`). A build defect found on the way: two headers
forward-declared `class NetworkRunContext` where the type is a `struct`, and MSVC mangled a free
function over it two ways by include order (a link failure in the new test TU); both now say
`struct`.

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knobs absent, identical on both beds | identical over the 2 h window (`runs/_rap_sid5_g0_field2` against `runs/_rf0hv10_field6`, `_g0_sim2` against `_rf0hv10_sim6`) | PASS |
| G1 (5a, 2 h) | traces identical; products identical after the N records | both beds: `closed_loop_trace.csv` and `network_epoch_status.csv` identical to G0; `products.txt` identical after 4 N records; the field declaration `GPS 1 ph=L1C+L1W co=C1C+C1W comp=C1W+C1C dco=C1C; GPS 2 ph=L2W co=C2W dco=C2W` (dph added at the second change); WTZZ00DEU's latch differs from the declared primary on band 1 (phase and code) | PASS |
| G2 (5a, 6 h wire and census) | identical to the baseline after the records; the census identical | `runs/_rap_sid5_g2_field6` identical to `runs/_rf0hv10_field6` on the three files after 4 N records and the legend line; census key `sid5` against `rf3gl2nb` identical at 16 of 16 (median count 348.0 both) | PASS |
| G3 (5b, 6 h census, key `sid5b`) | no site worse by more than 0.01 m on hours 2 to 6; QUIN00USA without an excursion above 5 m after hour 2 | the latch moved at 11 of 16 sites (C1W to C1C at all 11; L1W to L1C at ARHT, BADG, BAKE, QUIN, UTQI, WUH2, and at WTZZ00DEU); counts 5 better (QUIN00USA 583 to 611, BADG00RUS 185 to 249, POHN00FSM 0 to 43, BAKE00CAN 504 to 528, ARHT00ATA +3) and 3 worse (WUH200CHN 69 to 32, HARB00ZAF 358 to 339, DAV100ATA -6); hours 2 to 6 mean p50 WUH200CHN +0.021 m, BADG00RUS -0.026, QUIN00USA -0.010; per hour, WUH200CHN +0.047 (hour 3), DAV100ATA +0.017 (hour 2), POHN00FSM +0.014, HARB00ZAF +0.012; QUIN00USA's largest excursion after hour 1 0.97 to 0.10 m, none above 5 m; WTZZ00DEU 611 to 612, hours 2 to 6 +0.003 m | bar 1 **FAIL** (one site on the mean, four in some hour); bar 2 PASS |
| G4 | cost | loop +3.8 percent field (86 485 to 89 734 ms per pass of the summary's `loop_ms`), +7.3 percent sim; the phase-5 fold 3.3 ms per epoch at 6 h; the wire +169 bytes per run (the declaration changes twice) | read |

The 6 h table (`runs/_rap_sid5_g2_field6/stdout.txt`): 62 322 emitted families carried a hold
generation; 15 generation changes, 12 with an admitted arc birth and **3 without** (all G20 at TOW
262920 on L1C, L1W and L2W at once: a satellite-level emission gap longer than
`emit_pb_hold_max_gap_s` while the arcs stayed admitted, so the consumer is told of a new
generation that no arc birth caused); 1 165 arc births with no generation change, all of them one
more station joining a family the wire already carried (0 family returns); the emitter's phase
first-sighting test fired 96 times, all on a family with a returning arc, 88 of them the ledger's
own first admission at the same epoch and **8 genuine returns read as first sightings** (G32 260130,
G25 262500, G03 267390, G29 267510, G28 271500, G31 272280, G01 279420, G26 280350); the code row of
that test is dead wiring (the ledger's arcs are phase arcs); 116 of 125 274 roster cells (0.09
percent) were answered by the station latch because the cell had no modelled role yet at the
constellation's first epoch.

Readings:

- The census's freshness-flip population is **4** on the current wire, not card MSC's 50: one event
  (G25 C1W absent from the wire at TOW 259230, off-wire at ARHT00ATA and DAV100ATA, the return
  read as stale). Cards SCC, EV7, UPO and GRK removed the rest four wires ago; the phase-5 bar of
  section 4 ("the 50 dropouts each attributable") is met on what remains. The undeclared class is
  empty: the network declares both band-1 attributes, so no consumer tracks a signal the network
  carries no family for, and C2 never fires.
- 5b's whole effect is C1, and it is a trade: the declared primary is the network's majority
  signal (L1C at 40 stations against L1W at 18; C1C likewise), and following it helps the sites
  whose own rule had chosen the thinner family (QUIN00USA, BADG00RUS, POHN00FSM, BAKE00CAN) and
  hurts others (WUH200CHN, HARB00ZAF). Which family serves a given consumer is a product-selection
  question the declaration now makes visible and does not answer; the knob stays built and OFF.
  The hole itself (the L1W family at a median of 4 stations per column) is phase 6's.
- Two producer-side classes the observatory names for the first time: a hold generation restarted
  by an emission gap with no arc behind it (3), and a returning family read by the emitter as a
  first sighting (8). Both are re-coupling edges without identity (standing invariant 7); their
  consumer cost is unread.
- The declaration is written on change only (a consumer joining mid-file holds none until the
  next change); a periodic re-broadcast is owed with the seam work. A pre-card consumer reading
  this wire warns once on the unknown tag and parses every epoch (the reader's sticky flag,
  `LedgerEmitTest.AnUnknownRecordIsIgnoredByTheParser`).

Phase 6 (`channel_mode: all`) is split by the manager into 6a, the second PHASE channel on band 1
(the twin of card SCC, allow-listed, bounded: the L1W hole), and 6b, every band and channel with
the interlock lifted.

## 11. Phase 6a result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`): the second phase channel cannot close the L1W hole on this roster

Main `1dc05b0`. `network.second_phase_channel` (default OFF; `second_phase_signals` default
[L1C, L1W]; `second_phase_sigma_scale`): the phase twin of card SCC. A station contributes every
allow-listed band-1 phase it tracks besides its latched one, each an ordinary float ambiguity
joining the EXISTING satellite phase-bias family of that signal; no receiver inter-signal phase
state (section 4's risk: it would sit in the null); no datum row; the anchor election gains
candidates; the MW detector keeps the latched phase in mirror mode and admits the second under
`processing.mw_channel_mode: all`; the IF-TD detector's primary chain became "the first pair with
distinct frequencies" (two same-frequency phases would have starved it); refused with
`channel_mode: all` and on the leveling vehicle. The `rapid_arm.py` clone op now clones a PHASE
(cycles, a per-station and a per-satellite constant). `SecondPhaseChannelTest` 38 cases;
`gnss_tests` 2561 ran, 2560 passed, 1 known skip; exe `gnss_processor_sid6.exe` (`3524C8AD...`).

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | identical on both beds, with and without the roster | identical (`runs/_rap_sid6_g0_field2`, `_g0r_field2`, `_g0_sim2`) | PASS |
| G1 (2 h field) | the wire's L1W coverage against the base | `runs/_rap_sid6_g1_field2` against `_g0r_field2`: L1W 1 830 of 7 597 satellite-epochs missing, 24.0 satellites per epoch, IDENTICAL; L1C 0 missing both; the roster's `modelled_extra` filled at 18 stations, 36 120 second-phase cell-epochs, 0 fallback rows; the roster is an observatory (`_g1n_field2` identical to `_g1_field2`) | read |
| G2 (6 h field) | L1W family at 31 or more satellites per epoch and at most 100 missing satellite-epochs; WTZZ00DEU within 0.03 m; rows not above 213 / 32; wire not above 0.0041 / 0.0028 | `runs/_rap_sid6_g2_field6` against `runs/_rf0hv10_field6`: **L1W 5 323 missing and 24 satellites per epoch, unchanged** (counted directly from both `products.txt`: a clock record without an L1W phase-bias record); L1C 113 to 0; rows 280 / 41; wire 0.0054 / 0.0037 m; WTZZ00DEU 0.514 0.046 0.028 0.034 0.037 0.041 m (617 below a decimetre; never worse by more than 0.004 m); census key `sid6` against `rf3gl2nb`: counts 9 better 7 worse (STPM00SPM 581 to 621, HARB00ZAF 358 to 502; ASCG00SHN 180 to 0, SCRZ00BOL 398 to 243, QUIN00USA 583 to 385), median 348 to 279.5, hours 2 to 6 6 better 6 worse | coverage **FAIL** (unreachable, below); rows and wire FAIL; WTZZ PASS |
| G3 (sim, an L1W cloned at all 60 stations) | knob OFF identical; ON the L1W family complete, the consumer within 0.03 m | OFF identical over 2 h; ON (`runs/_rap_sid6_g3_sim_on`, 6 h): L1W 0 of 22 586 missing, 31 satellites per epoch, 60 stations, every L1W column existing only through the second phase; consumer 0.507 0.158 0.061 0.053 0.064 0.052 against 0.601 0.104 0.096 0.056 0.055 0.049 m (hour 2 +0.054) | mechanism PASS; consumer bar FAIL at hour 2 |
| G3b | a consumer on the second family alone converges (within 0.05 m from hour 3) | `runs/_rap_sid6_g3b_l1w` (the consumer tracking C1C, L1W, C2W, L2W on that wire): 0.447 0.163 0.067 0.059 0.069 0.056 m, within 0.014 m of the base from hour 4 | PASS |
| G4 | cost | loop +8.2 percent (2 h), +151 carried ambiguities per epoch; MW `all` +0.8 percent | read |

**The finding.** Only 18 of the 58 field stations report L1W at all (RINEX headers: 18 L1C+L1L,
16 L1C, 14 L1C+L1W+L1X, 6 L1C+L1X, 4 L1C+L1W), and every one of them already latches it under the
priority rule; the second phase this mechanism can add is therefore always L1C at those 18
stations, never L1W. The L1W hole (section 3: 5 323 satellite-epochs, a median of 4 stations per
column) is a RECEIVER-TRACKING limit, not a latch limit, and section 4's estimate of nine more
stations per L1W column counted the L1L and L1X trackers, which are other families. What the
extra L1C arcs cost on the field is measurable and not small (rows 213 to 280, wire 0.0041 to
0.0054 m, one more near-null direction found by the S-basis completion, a census that moves both
ways with ASCG00SHN losing every sub-decimetre epoch); the mechanism itself is sound on the truth
bed (a complete second family, a consumer converging on it alone). The knob stays built and OFF
on this roster. Slip tiers reaching a second-phase arc: LLI, IF-TD, MW under `all`; the per-signal
gap re-entry tier answered 0 of 53 re-openings (a defect to read if the knob is ever armed).

**What follows.** The consumers that latch L1W (ARHT00ATA, BADG00RUS, BAKE00CAN, QUIN00USA,
UTQI00USA, WUH200CHN and WTZZ00DEU in the census) are served by an 18-station family whichever
signal the other 40 stations track, and card EV7's coverage-gate edges are all on L1W keys. The
one mechanism that lets the 40 L1C stations support those consumers is a phase-bias family per
(satellite, BAND) rather than per (satellite, signal): L1C and L1W ride the same carrier, their
satellite-side difference is a per-satellite constant that a float ambiguity absorbs per arc at
every station and at the consumer, and the wire can keep its per-signal P records with the band's
value under each name. That is a model decision (a per-signal family is what integer resolution
on a mixed-signal network would need), pre-registered here as the design question of phase 6b
for the author: `network.phase_bias_family: signal | band`, default `signal`, graded on the
census with the L1W-latching sites as the target population and the Asian Up bias read before
and after. Phase 6b's other half (every band and channel, the interlock lifted) is unchanged.

## 12. Phase 6b result (2026-09-22; Opus build worker, every number re-graded by the manager from `runs/`): `channel_mode: all` is correct where a family has a canonical station, and a family without one freezes its satellite level at ignition

Main `817367e`. `processing.channel_mode: all` on the network path, the author's choice over the
band-level phase-bias family of section 11: the widening in `PppModel::selectUncombinedSignals` is
the one entry and SUBSUMES cards SCC and SID 6a on every band (their sets, sigma scales, MW guard
and telemetry serve every extra channel; the SCC refusal is an info line); every non-canonical
code of any band routes through `<system>_ISC_<signal>`; the row-level gap re-entry tier frees the
station's extra-channel cells on the bands that broke (the per-signal tier's zero of phase 6a was
an empty population, not an exclusion); `network.closed_loop.channel_mode` (default `single`)
keeps the consumer's model where it was; a WARN when a band-2-or-above companion is modelled with
`network.sat_isc_state` OFF. `ChannelModeAllTest` 24 cases; `gnss_tests` 2585 ran, 2584 passed,
1 known skip; exe `gnss_processor_sid6b.exe` (`E967DD74...`).

| gate | measured | verdict |
| --- | --- | --- |
| G0 | identical on both beds; the plain sim under `all` identical (one channel per band) | PASS |
| G1 (2 h field, `all` + `sat_isc_state` + `mw_channel_mode: single`) | 20 GPS families, 13 with extra rows (C1L 17 stations, C1X 20, C2L 24, C2S 4, C2X 28, L1L 17, L1X 20, L2L 24, L2S 4, L2X 28, beside C1C/C1W/C2W/L1C/L1W/L2W; the X and L2C channels enter through the widening although absent from the priority list); receiver ISC states 40 to 133; S-basis completion 4 to 7 realized against 1 declared; rows above 3 m / 10 m 113 / 6 to 8 179 / 2 180 in 2 h; the consumer at `single` tracked L1W, C1W, L2W, C2W as before | read |
| G2 (6 h field, `runs/_rap_sid6b_g2_field6`) | rows **213 / 32 to 31 595 / 5 620**, of which 26 824 are C2L; clock wire 0.0041 / 0.0028 to 0.0055 / 0.0035 m; WTZZ00DEU 0.608 0.063 0.030 0.032 0.039 0.045 m (594 below a decimetre); census key `sid6b` against `rf3gl2nb`: median count 348 to 259.5, hours 2 to 6 +0.014 m, counts 7 better 9 worse (QUIN00USA 583 to 239, BADG00RUS 185 to 22, ASCG00SHN 180 to 0); with `signal_min_station_fraction: 0.3` (C2S and L2S dropped everywhere) 296.5 and +0.005 m, WTZZ00DEU 0.606 0.152 0.056 0.012 0.020 0.019; the L1W family 5 323 missing, unchanged | every bar **FAIL** except WTZZ hours 2 to 6 |
| G3 (the phase-6a clone bed under `all`) | against `runs/_rap_sid6_g3_sim_on`: the same one extra family and row set; consumer within 0.0005 m per hour, C within 2.1 mm, P within 0.3 mm; the difference attributed to the gap-tier extension (74 arcs reached) | PASS |
| G4 | loop +155 percent, carried ambiguities +55 percent, wire +59 percent | read |

**The read (manager).** 26 824 of the 31 595 gross rows are the C2L family (27.7 percent of its
full-weight rows), with per-satellite medians up to 13.7 m and per-station medians that walk by
metres between hours. `logs/network_sat_bias_states.csv` and `logs/network_rx_states.csv` of the
6 h arm show the mechanism: the satellite C2L biases converge to a formal sigma of 0.00 m within the
first hour and never move again (G07 -5.51 to -5.46 m over 6 h; G12 -6.20 m, the C2W value, from
its first epoch), while the receiver `GPS_ISC_C2L` states walk by metres against a formal 0.1 m
(DARW00AUS +5.5, +4.6, +3.4, +2.9, +3.1, +3.8 m by hour; KIT300UZB +2.7 to +6.6 to +3.6). The S-basis
completion is not the cause: its first-epoch directions are band-2 PHASE columns (L2W, L2L, L2X),
released the next epoch. The structural difference from card SCC is that C2L has NO canonical
station (C2W is latched at all 58), so its satellite columns are determined only through the
receiver inter-signal states of 24 stations, the split is fixed on the first epochs' geometry, and
the satellite side's near-zero process noise freezes it while the receiver side, which has process
noise, absorbs the mismatch as the satellite mix changes. SCC's C1C family had 40 canonical
stations to determine its satellite columns and never had this problem. Every family the widening
adds on this posture (the X channels, the L2C channels, L1L/C1L) is canonical nowhere; the phase
families have the anchor election per column instead, and their cost is the S-basis directions
and the rows.

**What follows.** `all` as built is correct where a family has a canonical station and is KEEP
OFF on this posture. The next read is the IGNITION DATUM of a family with no canonical station
(the satellite level frozen at a one-epoch split: card B5D's class in a new form): a birth
variance and horizon for such a family's satellite columns, or a roster rule that models a family
only where a station latches it canonically until that datum exists. The author decides.
