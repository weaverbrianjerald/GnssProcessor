# SID: signal identity and arc lifecycle, the re-founding of the layer that keeps failing

**STATUS**

- **id:** SID
- **topic:** network-vehicle
- **status:** open
- **summary:** AUDIT and DESIGN 2026-09-22 (Opus audit worker, read-only; every number re-read by the manager from `runs/`), opened by the author's question whether the whole signal and channel organisation needs rethinking on the way to every satellite, frequency and channel. Verdict: the estimator is per signal already and stays; the layer that decides WHICH signal stands for a band, and the lifecycle of an arc, are decided in 128 sites across 38 files with at least six independent "two lowest bands" rules and 29 arc-keyed containers of which 31 lifecycle holders have both edges silent. Measured on the field baseline: the raw tracker's signal differs from the carried one at 46 station-band cells of 44 of 58 stations (not the 10 of card GRK: those were the ten with a re-entry in the window); 0 of 31 band-1 re-entries at L1W stations were answered against 55 of 67 elsewhere; the wire's L1W PHASE-BIAS family is missing for 23.6 percent of satellite-epochs (5 323 of 22 586, 17 of 32 satellites; median 4 carried stations) because a station contributes one phase per band, the class card SCC fixed on the code side only, and the mechanism behind card EV7's edges. Plan: a per-station SignalRoster and an ArcLedger owned by the run context, built as an OBSERVATORY first (phase 1, byte-identical armed), then migrated one reader at a time (phases 2 to 5), then `channel_mode: all` on the network path (phase 6). Phase 1 pre-registered in section 5; the author passed the recommended fixes to the build.
- **source:** the author's question of 2026-09-22; cards [CSL](CSL_canonical_signal_latch_2026-09-20.md), [GRS](GRS_gap_reentry_per_signal_2026-09-20.md), [EV7](EV7_all_sat_clock_decouple_2026-09-21.md), [SCC](SCC_second_code_channel_2026-09-21.md), [GRK](GRK_gap_reentry_key_mismatch_2026-09-22.md) (the six defects of one shape); the audit report (`runs/_msc/sid_audit_report_2026-09-22.md`, 912 lines, outside the repository with the run directories; its tables are summarised here and its measurements are reproducible from the run directories named)
- **knobs / posture:** none built at the time of the audit. Proposed: `network.signal_roster` (phase 1, observatory), later phases named in section 4. Posture: the RF-0hv working bases (baseline field `runs/_rf0hv8_field6`, sim `runs/_rf0hv7_sim6`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** `runs/_rf0hv8_field6` (all Part D numbers), `runs/_rap_k1_strip_l1c`, `runs/_rap_grk_g1_field_on`; traces `network_slip_raw.csv`, `network_amb_state.csv`, `network_events.csv`, `network_pf_rows.csv`, `closed_loop_sat_trace.csv`, `products.txt`

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
