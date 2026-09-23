# OWD: the defects card GAL uncovered, fixed before Galileo adoption

**STATUS**

- **id:** OWD
- **topic:** network-vehicle
- **status:** closed
- **summary:** CLOSED 2026-09-23 (section 5): BUILT (`d64c230` on main `e47b267`), every pre-registered gate passes and the products are byte-identical on both beds, 2 h and 6 h (the records with the header reported apart; epoch status, post-fit residuals and the clock wire identical). O1: the S line is on every wire and names the day's own orbit and clock files; a replay of a wire without one exits with an error unless `network.replay_allow_undeclared_wire` (`runs/_rap_owd_refuse_anmg`); the WTZZ00DEU replay of the new wire writes a closed-loop trace byte-identical to the in-process consumer's, where the legacy replay differs at 168 epochs by up to 0.0032 m; the 16-site census on the new wire (keys `owd`, `owdf`) equals `rf3gl2nb` in every site's count and in the across-site median of every hour, each site's hourly median within 0.002 m and the positions within 0.0041 m. O2: every re-inflation keeps a larger variance; the producer counts 0 raised and 0 kept on every run (byte-identical by construction on this posture), each consumer 4 kept (census sites and closed loops), the positions within 0.0001 m; the legacy replay (keys `owdleg`, `owdlegf`) reproduces the recorded key's discontinuity channel at 16 of 16 sites. O3: the window latch equals the first record at every processed satellite (6 h: 97 on the window rule, 13 with no healthy record, none differing); a same-value rule keeps the first record's bits, because the merged navigation file prints 29 of the 32 GPS values with different last digits and that alone moved the field products by up to 3.4e-5 m. O4: one door for the network sync and the consumer replay (1 537 822 field observations of inactive systems removed); the two archived GPS-only configs load with a warning. O5: all 3 019 unnamed post-fit rows named. Cost: 277.50 against 317.79 ms per epoch, not slower. Sequenced, the author's decisions (section 4): O6, O7, health on the network path. Original summary: Build card, pre-registered 2026-09-22 by the author's direction to fix the mistakes card GAL uncovered before continuing (the list of its section 6.2, card CLV taking the frozen clock level). Five code fixes: the product S contract line written on every wire, naming the product files in force during the run, and a replay of a wire without one refused unless asked for (O1); every variance re-inflation of the state manager raises and never shrinks, with a raised/kept counter each, and a GPS clock re-reference re-inflates every allocated inter-system bias (O2); the broadcast group delay latched as the processing window's median healthy record instead of the file's first record (O3); one admission door for constellations, `processing.active_systems` (O4); post-fit rows that name the observation's signal (O5). Sequenced with a written rule: the BeiDou group-delay pair rule (O6), the sim fixture regeneration and the Galileo yaw law (O7), health on the network path. Located without a fix: the GPS clock wire 0.0041 to 0.0047 m with Galileo (O8), a slow per-satellite clock drift of millimetres in no station class. Measured before the build: the contract knob changes the product header only (343 100 identical body lines on card GAL's pair); the decided latch reproduces the first record at all 110 satellites on both beds' 00:00 windows and would differ by up to 0.698 m on a run starting at 18:00; both clock pivots of the field day are eclipsed, and their eclipse is the once-per-run constellation re-reference.
- **source:** [GAL](GAL_gps_galileo_dual_2026-09-22.md) section 6.2 (the list) and 6.1; [CLV](CLV_clock_level_2026-09-22.md) section 5
- **knobs / posture:** new `network.replay_allow_undeclared_wire` (bool, default false: a replay of a wire without an S contract line is refused); `network.serialize_sbasis_contract` accepted and inert (the S line is unconditional), pinned true on both goldens; the re-inflation fix has no knob (a defect fix); the RF-0hv working bases at main `53cd382` at pre-registration (the build base is recorded at close); the build base main `e47b267`, the code commit `d64c230`; the goldens regenerated with the pin (`--verify` PASS, 62 armed, 6 documented exceptions)
- **dates in the text:** 2026-09-22, 2026-09-23
- **hashes / run dirs:** baseline pair `runs/_rf0hv10_{field,sim}6`; census key `rf3gl2nb`; card GAL's `runs/_rap_gal_g5_field6` and `_g5s_field6`; arms `runs/_rap_owd_*`; exe `gnss_processor_owd.exe` (reference `gnss_processor_owd_ref.exe`); result (section 5): final exe sha256 `2c0e9d8f087c912f59bc731f36bd488036e989c5ee1bed9dc04368551f053eef`, G1b exe `836a8ea486fad7602a937d02b5efe4e0cb1ff21eef3309d18e91878531ab962e` (`gnss_processor_owd_g1b.exe`), reference `e75a62590171803fab85253e1d7c3df8f2901e3d0240ebe6eb2d26effdf682d1`; 2 h arms `runs/_rap_owd_{ref,g0,g1,g1b}_{sim,field}2`; 6 h `runs/_rap_owd_{field,sim}6` (G1b exe) and `runs/_rap_owd_{field,sim}6f` (final exe); census keys `owd` / `owdf` (the new wire), `owdleg` / `owdlegf` (the recorded wire, this build), `owdref` (the recorded wire, the reference build); refusal arms `runs/_rap_owd_refuse_anmg` and `_rap_owd_refuse_anmg_f`

## 1. The defects, as measured (read-only survey of main `53cd382`, 2026-09-22)

**O1, the contract line gated on an output option.** `network.serialize_sbasis_contract` decides
at one site whether the product header carries the S line (`ApplicationNetworkSetup.cpp:1317`);
without it the replay consumer builds the default emitter (`NetworkProductIO.cpp:1122-1133`: flat
0.05 m phase-bias threshold, no clock-discontinuity channel, no staleness horizon, no code-jump or
set-change channel) and only warns (`ApplicationConsumerReplay.cpp:220-224`). The goldens do not
arm it, which is why card GAL's first census diverged at all 16 sites. The knob touches nothing
else: card GAL's field pair `runs/_rap_gal_g5_field6` (off) and `_g5s_field6` (on) carry 343 100
identical body lines once the `#` and `S` lines are removed, and their `closed_loop_trace.csv`,
`network_epoch_status.csv` and `network_pf_rows.csv` are identical. Where the S line exists, its
a-priori identity names the first merged product file (`GnssConfig.cpp:67-80`; the resolver merges
the neighbouring days), so `_g5s_field6` declares DOY-128 files for a DOY-129 run; and
`scripts/check_solve_identity.py` hashes the S line with the records.

**O2, re-inflations that set the variance.** All seven re-inflations of the state manager
(ionosphere, ionosphere rate, receiver code bias, inter-system bias, satellite code bias,
satellite phase bias, satellite clock) zero the row and column and SET the diagonal to the request
(`StateManager.cpp:1549, 1597, 1652, 1706, 1955, 2157, 2392`), so a larger current variance is
deflated. A GPS clock re-reference re-inflates only the inter-system biases observed in that
epoch, and the declaration is consumed once per satellite (`RealTimeEngineSsrBreaks.cpp:828-883`,
`StateManager.cpp:1094-1105`), so an absent constellation's bias is stale when it returns. On the
goldens the producer's only re-inflation (the ionosphere in the deweight-release seam) returns
before acting, because the posture has no spherical-harmonic ionosphere
(`NetworkIonoDwRelease.cpp:84-93`); the consumer takes the network clock, so it estimates no
satellite bias; its slip re-inflation of the ionosphere requests 25 m2 against slant variances of
0.009 to 0.55 m2. The receiver code bias is the live case: the W6 slice forces
`estimation.estimate_ifb`, the consumers inherit it, the state starts at 10 m, and each declared GPS
code-bias step requests 0.25 m2 (134 to 324 steps per 6 h at the 16 census sites,
`runs/_rap_msc_*_rf3gl2nb_d/logs/rt_engine.log`), so an early step is deflated today.

**O3, the group delay latched on the first record.** The setup keeps the first record's group delay
per satellite and consults neither health nor the Galileo data-source word
(`ApplicationNetworkSetup.cpp:464-521`). Both beds latch from
`data/2024-129/products/BRDC00IGS_R_20241290000_01D_MN.rnx` (11 498 records, 110 satellites). The two
satellites card GAL named (all records healthy):

| satellite | toc span (UT) | F/NAV records, BGD(E1,E5a) | I/NAV records, BGD(E1,E5a) / BGD(E1,E5b) |
| --- | --- | --- | --- |
| E21 | 05-07 23:30 to 05-08 05:10 | 28, 0.000 ns | 83, 0.000 / -0.233 ns |
| E21 | 05-08 05:10 to 17:10 | 45, -0.233 ns | 135, -0.233 / -0.698 ns |
| E21 | 05-08 17:10 to 23:50 | 35, -2.328 ns | 102, -2.328 / -2.794 ns |
| E24 | 05-07 23:40 to 05-08 05:50 | 12, -8.615 ns | 38, -8.615 / -10.012 ns |
| E24 | 05-08 05:50 to 17:00 | 42, -8.149 ns | 124, -8.149 / -9.313 ns |
| E24 | 05-08 17:10 to 23:20 | 28, -6.286 ns | 84, -6.286 / -7.683 ns |

The within-day steps are the ground segment's uploads (around 05:10 to 05:50 and 17:10 to 17:50
UT), in every message type at once; the first record is the value in force when the file starts.
F/NAV and I/NAV carry the same value sets for every Galileo satellite (they differ in 1 to 3 batches
per satellite at an upload). Zeros are quantized values (one step is 2^-32 s, 0.233 ns): E03 is 0 all
day and E25 goes 0.233, 0.000, 0.698 ns. No GPS satellite changes its T_GD within the file. Thirteen
satellites broadcast no healthy record all day: G01 (health 63, T_GD -19.558 ns, the GPS clock pivot
until its eclipse), E14, E18, six BeiDou and four QZSS. The first-record latch is right for a run
that starts with the file and wrong for any other: a run from 18:00 to 24:00 would take the
superseded value at 21 Galileo satellites, by 0.698 m on E21 and E24.

**O4, two doors for one constellation.** The network admits an observation by
`processing.signal_priorities` (the model's signal selection) and allocates the inter-system bias by
`processing.active_systems`; nothing upstream filters by system (the parser and the multi-station
sync keep every system, and five seams read the raw synced observations). The loader therefore
refuses a Galileo list without Galileo active (`GnssConfigLoadNetwork.cpp:4113-4160`), which rejects
`config/test_suite/ppp_rtk_network_global_5min_gpsonly.json` and
`config/archive/test_suite/_sdewl_probe_gps10.json` (its list inherited from
`_golden0729_allon_delta.json`). The PPP, realtime and baseline paths already filter by
`active_systems`.

**O5, post-fit rows without a signal.** The row writer takes the signal from the first labelled
column the row touches (`NetworkFrameDiag.cpp:322-332`). `runs/_rf0hv10_field6/logs/network_pf_rows.csv`
has 32 code rows and 2 987 phase rows with an empty signal, all GPS, at six stations (KOKV00USA
1 482, BOGT00COL 1 211, VOIM00MDG 140, PALM00ATA 115, CUSV00THA 63, STHL00GBR 8); the sim base has
none. The observation's own signal is known at assembly: the measurement's `signal_types` are
row-aligned with the design matrix, and every later row is appended.

**O8, the GPS clock wire with Galileo.** Card GAL's field arm `runs/_rap_gal_g5s_field6` grades
0.0047 m against the base's 0.0041 m (hours 2 to 6). With the runs that exist: (1) it is not
within-hour noise (arcs cut at the hour give 0.0023 0.0019 0.0022 0.0025 0.0027 m against 0.0025
0.0019 0.0025 0.0025 0.0027 m in hours 2 to 6); (2) it is a slow per-satellite drift, the difference
of the two runs' clocks having an rms about its mean of 1.9 mm (p50 over satellites), larger in
hours 2 and 6 than in hours 3 to 5; (3) it is broad (14 of 31 satellites up by more than 0.3 mm, 9
down); (4) it is in no station class (it correlates -0.39 with the pilot family's share of a
satellite's rows, and the GPS post-fit residual medians of the 28 pilot, 30 combined and 6
antenna-fallback stations are unchanged); (5) it follows neither the shared troposphere
(correlation -0.10) nor the receiver clocks (+0.03), and the emitted phase biases are identical
between the runs for the median satellite, so the drift rides on the clock record alone.
Unattributed further; re-graded on card CLV's build.

## 2. The design

**O1.** The S line is part of the wire: the product writer always stages the contract (the v1
branch of the setup is removed; the writer class keeps both constructors for legacy fixtures); the
knob stays accepted, is forced true in the loader and logged as inert when a config names it; the
periodic re-broadcast depends on `network.contract_rebroadcast_s` alone; the teardown round-trip
self-check always runs and reports its seconds. The a-priori identity names the SP3 and CLK files
whose span, read from the IGS long name, meets the processing window (one day's pair on these beds;
every merged file when a name does not parse). The replay consumer refuses a wire without an S
line with an error naming the cause and the two fixes, unless `network.replay_allow_undeclared_wire`
is set, in which case the present warning runs (a legacy leg can still be graded on purpose, never
silently); the live tail is unchanged. The records stay byte for byte what they were;
`scripts/check_solve_identity.py` hashes the records only and reports the header apart.

**O2.** Every re-inflation sets the diagonal to the maximum of the current and requested variances
(value kept, partition broken as before), logs raised or kept, and counts both; one run-end line
per consumer (realtime, closed loop, replay), per producer (summed over the stations, naming the
stations with a kept) and per PPP run. On a GPS clock re-reference every allocated non-reference
inter-system bias is re-inflated, observed in that epoch or not, at most once per epoch; inert on
GPS-only runs, which allocate none. No knob.

**O3.** The rule: *the latched group delay of a satellite is the low median of the group delays of
its healthy records whose clock reference epoch lies in the run's processing window, Galileo's F/NAV
(E1, E5a) records preferred when present; a satellite with no such record takes the healthy record
in force at the window start; a satellite with no healthy record at all is latched by the same rule
over all its records; zeros are real values.* One value per run. The parser stores the Galileo
data-source word; the log line keeps the spread report and adds the counts per reason and the
satellites whose value differs from the first record.

**O4.** The network strips the observations of every system outside `processing.active_systems`
once, right after the multi-station sync, so every seam sees the admitted systems only; the loader
check becomes a warning naming each ignored list; the information line stays.

**O5.** The assembly keeps each data row's own signal (only when the rows trace is armed); the
writer names it, falling back to the column label for appended rows; run-end counters
`pf_rows_no_signal` and `pf_rows_sig_relabel` (a named row renamed) join the teardown line.
Trace-only: the solve is untouched.

## 3. The gates (rapid mode: 2 h for structure, 6 h for the census and the baseline pair)

One consumer grader serves base and candidate: the hourly p50 of the 3D error in six 3600 s bins
from TOW 259200 and the count below 0.1 m over every epoch (the TOW 280800 epoch of the sim
included). On the baseline pair it prints 0.560 0.059 0.028 0.030 0.039 0.048 m and 611 of 720
(`runs/_rf0hv10_field6`), 0.601 0.104 0.096 0.056 0.055 0.049 m and 448 of 720 (`runs/_rf0hv10_sim6`).

| gate | arm | bar |
| --- | --- | --- |
| G0 | `owd_g0_sim2`, `owd_g0_field2` (O1, O2, O4, O5) against `owd_ref_sim2`, `owd_ref_field2` (the reference exe, the same golden bytes) | the product records identical (the header reported apart); the S line present with the run's own product files, `network_contract_check.csv` PASS; `network_epoch_status.csv` identical; the producer's re-inflation line with its kept count (0 expected); `closed_loop_trace.csv` identical where every consumer kept count is 0, otherwise graded per hour with the kept counts listed; `network_pf_rows.csv` identical except the filled signal column, `pf_rows_no_signal` and `pf_rows_sig_relabel` at 0; the full test suite |
| G1 | `owd_g1_sim2`, `owd_g1_field2` (O3) | the two latch log lines; expected on both beds: 110 satellites, none differing from the first record, 97 on the window rule and 13 with no healthy record, 7 342 I/NAV records set aside for F/NAV; the products identical to G0 |
| goldens | the manifest pins `network.serialize_sbasis_contract` | `scripts/make_golden_config.py --verify` PASS; each golden's diff is the knob line, the comma above it and the two provenance lines that echo the manifest |
| G2 | `owd_field6` (6 h), census key `owd`; legacy replay key `owdleg` on the base wire with `network.replay_allow_undeclared_wire` | the census paired against `rf3gl2nb` with the kept counts per site; `owdleg` reproduces the recorded key's discontinuity channel at all 16 sites (the ordered code-bias discontinuity lines of each replay's `rt_engine.log`), its consumer traces identical where the kept counts are 0 and graded per hour and paired otherwise; a replay without the knob exits with the error; against the baseline pair: the consumer not worse than the numbers above, the clock wire 0.0041 m, gross code rows 213 / 32; `owd_sim6` only if O2 or O3 moved the sim (gross rows 595 / 1) |
| G3 | loader unit test | both archived configs load with the warning naming the ignored list (the executable has no load-only path) |
| cost | `owd_field6` | the epoch-loop wall per epoch within 2 percent of `runs/_rf0hv10_field6` (228 806 ms over 720 epochs); the teardown self-check seconds reported |

## 4. Sequenced (not built by this card; each the author's decision)

**O6, the BeiDou group-delay pair rule.** The legacy D1/D2 message references the satellite clock
to B3I (a B1I user applies dt - TGD1, a B2I user dt - TGD2), so in the model's convention
d(B1I) = c TGD1, d(B2I) = c TGD2 and d(B3I) = 0, whereas GPS and Galileo reference the clock to the
pair's ionosphere-free combination and the pinned geometry-free value is the delay itself
(`NetworkSbasis.cpp:835-850`). With the builder's order (RINEX band digit) and mu relative to the
first signal (`NetworkSbasis.cpp:857`), a pinned BeiDou pair takes: (C2x, C6x), that is (B1I, B3I),
-c TGD1 / (mu - 1) with mu = 1.5145, i.e. -1.9437 c TGD1; (C6x, C7x), that is (B3I, B2I), BDS-2
only, c TGD2 / (mu - 1) with mu = 1.1043, i.e. 9.5895 c TGD2 (the parser's `tgd1`); (C2x, C7x),
BDS-2 only, c (TGD2 - TGD1) / (mu - 1) with mu = 1.6724, i.e. 1.4872 c (TGD2 - TGD1); a pair with a
BDS-3 open signal (B1C, B2a, B2b, B2a+b) takes zero (only D1/D2 records are decoded). The latch must
then keep both delays per BeiDou satellite. Today's rule gives every non-Galileo system the band
digits 1 and 2 (`NetworkSbasis.cpp:61-63`), which match no legacy BeiDou pair and would give
c TGD1, unscaled, to a (B1C, B1I) pair it does not describe. No BeiDou bed exists to validate it.

**O7, the sim fixture.** `runs/simnet60_quiet_wu93_24h` (GPS and Galileo) was generated while the
1 ms clock bound rejected E05, E11 and E12; under the 0.1 s bound (`GnssCorrections.cpp:399-413`,
byte-identical for GPS, whose largest sample is 0.708 ms) they are simulated. The simulator draws
measurement noise from one stream per station consumed across the satellite loop
(`src/sim/main.cpp:1596`, `:2445-2448`), and multipath and slips from a second per-station stream
the same way (`:1909`), so three more satellites change every later draw of every satellite, GPS
included; a regeneration moves every sim baseline, the GPS-only ones too: a one-time re-base, best
done before the next sim baseline is recorded.

**O7, the Galileo yaw law.** Attitude is nominal yaw steering (`SatAttitude.cpp:123-150`) and a
satellite in Earth's umbra is dropped at the geometry (`ModelGeometry.cpp:682-700`;
`processing.exclude_eclipsing_satellites` is true on both goldens). On DOY 129, with the same
cylindrical shadow test on the COD orbits, 7 of 25 Galileo satellites pass the shadow over the day
(E02, E21, E24, E25, E27, E30, E31, 55 to 120 min each) and 4 inside 00:00 to 06:00 (E02, E24, E25,
E30); GPS 8 of 32 and 5. Both clock pivots are among them: E02 is in shadow from 3.33 to 4.25 h and
G01 from 4.75 to 5.58 h, and the only clock-pivot handovers of the runs are E02 to E03 at TOW 271230
(3.34 h) and G01 to G02 at TOW 276150 (4.71 h) (`logs/network_events.csv`, tier PIVOT, in
`runs/_rap_gal_g5s_field6` and `runs/_rap_gal_g2b_sim6`; the GPS one also in
`runs/_rf0hv10_{field,sim}6`). The once-per-run constellation re-reference card GAL recorded is
therefore the pivot's eclipse, and O2's inter-system bias re-inflation answers it. A yaw law would
keep the eclipsing satellites; a pivot election that skips a satellite due to enter the shadow
inside the window would remove the handover without one.

**Health on the network path.** Health is consulted nowhere on the network path: G01 broadcasts
health 63 in every record of the day and is the GPS clock pivot until its eclipse; E14 and E18 are
unhealthy all day and on the Galileo wire (1 440 clock records in
`runs/_rap_gal_g5s_field6/products.txt`). O3 latches such satellites from all their records, so
their datum stays where it is until this decision.

## 5. Result (built 2026-09-22, graded 2026-09-23)

The code commit is `d64c230` on main `e47b267` (card CLV's code). The build went in the
pre-registered stages, each a Release executable: the reference, `e47b267` before the first edit
(`gnss_processor_owd_ref.exe`, sha256 `e75a62590171803fab85253e1d7c3df8f2901e3d0240ebe6eb2d26effdf682d1`); G0's O1, O2, O4 and O5 (sha256 prefix
`dddb69165ccd1555`); G1b, with O3 (`836a8ea486fad7602a937d02b5efe4e0cb1ff21eef3309d18e91878531ab962e`, kept as `gnss_processor_owd_g1b.exe`); and the final build
(`gnss_processor_owd.exe`, sha256 `2c0e9d8f087c912f59bc731f36bd488036e989c5ee1bed9dc04368551f053eef`), which adds O4's door to the consumer replay after G2
found that the replay no longer received the live feed (5.3, 5.6). The final build's 6 h runs
`runs/_rap_owd_{field,sim}6f` are byte-identical to G1b's `runs/_rap_owd_{field,sim}6` in every
output file compared (`products.txt`, `network_epoch_status.csv`, `closed_loop_trace.csv`,
`network_pf_rows.csv`, `network_contract_check.csv`): the network path is the same in both. The
full Debug suite on the final code runs 2 637 tests from 331 suites: 2 636 pass and 1 skips
(`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the fixture gate), against 2 609 from 327 on
card CLV's build. Four suites are new, 28 cases: `UndeclaredWireAdmission` 7,
`ReinflateNeverShrinks` 10, `ActiveSystemAdmission` 3, `GroupDelayLatch` 8. Two tests changed:
the state-manager re-inflation tests of `tests/test_filter_core.cpp` now exercise the raise
path, and card GAL's refusal test is now `GalileoListWithoutGalileoActiveIsIgnored`.
`scripts/config_posture_check.py --verify-loader` reports 0 coverage gaps. The 2 h arms copy the
goldens as committed at `e47b267` (reference and candidate on the same bytes, only the run paths
changed); the 6 h arms copy the regenerated goldens (5.4). The grader of section 3 serves every
consumer number below.

**5.1 G0 (O1, O2, O4, O5): PASS.** `runs/_rap_owd_ref_{sim,field}2` against
`runs/_rap_owd_g0_{sim,field}2`:

| | sim | field |
| --- | --- | --- |
| product records (`#` and `S` lines removed) | 45 821 lines, identical | 59 194 lines, identical |
| header | v1 to v2 with one S line: a-priori `COD0MGXFIN_20241290000_01D_05M_ORB.SP3+COD0MGXFIN_20241290000_01D_30S_CLK.CLK`; `network_contract_check.csv` PASS | the same |
| `network_epoch_status.csv` | identical | identical |
| `network_pf_rows.csv` | 473 620 rows; no other column differs; 0 empty signals | 539 635 rows; no other column differs; 1 041 empty signals filled, 0 left |
| `pf_rows_no_signal`, `pf_rows_sig_relabel` | 0, 0 | 0, 0 |
| removed at the door (O4) | 100 276 (Galileo) | 511 579 (BeiDou 220 172, GLONASS 125 958, Galileo 120 545, QZSS 16 496, SBAS 28 408) |
| producer re-inflations | every counter 0 raised, 0 kept (60 stations) | the same (58 stations) |
| consumer re-inflations (closed loop) | code bias 0 raised, 4 kept; every other kind 0 | code bias 2 raised, 4 kept; every other kind 0 |
| `closed_loop_trace.csv` | `n_sats` at all 240 epochs (the consumer's feed now holds the admitted systems only, O4: 16 to 10 at the first epoch); the 3D error at 5 epochs and east, north, up at 1 to 2, each by 0.0001 m (from TOW 259530); 0.601 0.104 m in hours 1 and 2 and 53 of 240 below 0.1 m on both | `n_sats` at all 240 epochs (46 to 12 at the first epoch); the 3D error at 10 epochs and east, north, up at 11, 10, 21, each by 0.0001 m (from TOW 259410); 0.560 0.059 m and 131 of 240 on both |
| teardown round-trip self-check | 0.18 s (240 epochs, 1 491 911 bytes) | 0.19 s (1 946 459 bytes) |

The producer's ionosphere counter, the one re-inflation section 1 found in the producer, is 0
raised and 0 kept on both beds, as is every other producer counter: under O2 the producer is
byte-identical by construction on this posture. The inter-system bias rule is inert on GPS-only
runs, which allocate no inter-system bias (0 raised, 0 kept on every consumer).

**5.2 G1 (O3): PASS, after one correction.** The first build of the rule
(`runs/_rap_owd_g1_{sim,field}2`) was not byte-identical: the merged navigation file
(`BRDC00IGS_R_20241290000_01D_MN.rnx`) prints the unchanged T_GD of 29 of the 32 GPS satellites
with different last digits (at most 4e-20 s apart, 1.2e-11 m), the median picked a representation
other than the first record's, and the products moved: field clock records by up to 3.4e-5 m
(7 596 of 7 597 records), code biases 2.6e-5 m, phase biases 1.5e-5 m; sim 1.1e-8, 3.2e-10 and
2.3e-7 m. The rule now keeps the first record's exact bits when the latched value equals it
within 1e-12 s (one broadcast step is 0.233 ns; test `RepresentationNoiseKeepsTheFirstRecordsBits`).
With it, `runs/_rap_owd_g1b_{sim,field}2` equal G0 byte for byte on both beds: product records,
header, `network_epoch_status.csv`, `closed_loop_trace.csv`, `network_pf_rows.csv`. The latch
lines (`logs/main_log.log`):

| run | window (TOW) | window rule / in force at the start / no healthy record | set aside: I/NAV, outside | differ from the first record |
| --- | --- | --- | --- | --- |
| `_rap_owd_g1b_field2` | [259200, 266400) | 95 / 2 / 13 | 7 342, 3 807 | 0 |
| `_rap_owd_g1b_sim2` | [259230, 266430) | 97 / 0 / 13 | 7 342, 3 812 | 1: J02, -0.1396 m |
| `_rap_owd_field6f` | [259200, 280800) | 97 / 0 / 13 | 7 342, 3 120 | 0 |
| `_rap_owd_sim6f` | [259230, 280830) | 97 / 0 / 13 | 7 342, 3 120 | 0 |

The 6 h windows read exactly as pre-registered. The 2 h windows differ in two details: on the
field, G03 and G25 have no record whose clock epoch lies in [00:00, 02:00) (their records sit at
23:59:44 and 02:00) and take the record in force at the start, the same value; on the sim, whose
window starts at 00:00:30, J02 (a QZSS satellite the GPS-only run does not process, unhealthy all
day) has two records inside, 01:00 at 1.863 ns and 02:00 at 1.397 ns, a one-record excursion (the
03:00 record is 1.863 again), and the low median of two is the lower. On a window that holds two
records, one odd record decides the latch. The 13 satellites without a healthy record are C35 C48
C50 C56 C58 C62 E14 E18 G01 J02 J03 J04 J07; C29, E03 and E21 latch exactly 0; unhealthy records
set aside: 0.

**5.3 G2: PASS.** The 6 h runs of the regenerated goldens against the baseline pair (the final
build's runs; G1b's are byte-identical to them):

| | field: `_rap_owd_field6f` / `_rf0hv10_field6` | sim: `_rap_owd_sim6f` / `_rf0hv10_sim6` |
| --- | --- | --- |
| product records | 176 055 lines, identical (`scripts/check_solve_identity.py`: records identical, header reported apart; CLK, CB and PB identical) | 136 246 lines, identical (the same checks) |
| `network_epoch_status.csv` | identical | identical |
| consumer p50 by hour, epochs below 0.1 m | 0.560 0.059 0.028 0.030 0.039 0.048 m, 611 of 720, on both | 0.601 0.104 0.096 0.056 0.055 0.049 m, 448 of 720, on both |
| `closed_loop_trace.csv` | `n_sats` at every epoch (O4); the 3D error at 10 of 720 epochs, each by 0.0001 m (TOW 259410 to 262230); consumer code bias 69 raised, 4 kept | `n_sats` at every epoch; the 3D error at 6 of 720, each by 0.0001 m (TOW 259530 to 267570); 7 raised, 4 kept |
| clock wire, hours 2 to 6 (all / tracked arcs) | 0.0041 / 0.0028 m on both | 0.0040 / 0.0030 m on both |
| gross code rows above 3 m / 10 m | 213 / 32 on both (of 900 016) | 595 / 1 on both (of 704 618) |
| `network_pf_rows.csv` | 1 593 647 rows; 3 019 empty signals filled (section 1's 32 code and 2 987 phase rows), 0 left; no other column differs | 1 409 236 rows; nothing to fill; no other column differs |
| removed at the door (O4) | 1 537 822 (BeiDou 662 539, GLONASS 370 202, Galileo 369 145, QZSS 50 270, SBAS 85 666) | 298 960 (Galileo) |
| producer re-inflations | every counter 0 | every counter 0 |

The sim 6 h run was owed by the card's condition (the G0 build moved the sim consumer by
0.0001 m at 5 epochs), not by O3, which changed no processed satellite's datum.

The census. Every key replays the same 16 sites (`runs/_msc/site_list.json`) plus WTZZ00DEU on the
recorded key's own base arm, `runs/_rf0hv9_field6/arm.json` (`runs/_msc/scripts/msc_replay_set.py`):
`owd` replays the new wire (`runs/_rap_owd_field6/products.txt`, the records identical to the
recorded wire, the S line added); `owdleg` replays the recorded key's wire
(`runs/_rap_rf3b_gl_nobound/products.txt`, no S line) with `network.replay_allow_undeclared_wire`;
`owdref` replays it with the reference executable, to separate this card's change from everything
committed since the recorded key was made. `owd` and `owdleg` ran on G1b's build and again on the
final build as `owdf` (on `runs/_rap_owd_field6f/products.txt`) and `owdlegf`: every replay's
position columns are identical between the two builds, the discontinuity lines too, and the
final build's door changes `n_sats` only (12 239 rows over the 17 replays; 48 505 to 152 755
observations of inactive systems removed per site).

| key | median p50 by hour (m) | median count | against `rf3gl2nb` |
| --- | --- | --- | --- |
| `rf3gl2nb` (recorded) | 0.670 0.169 0.104 0.077 0.092 0.070 | 348.0 | |
| `owdref` | the same | 348.0 | the discontinuity lines and the traces byte-identical at 16 of 16 sites |
| `owdleg`, `owdlegf` | the same | 348.0 | the ordered discontinuity lines identical at 16 of 16 sites (the legacy-path proof); the 3D error differs at 15 sites in 149 epochs, each by 0.0001 m |
| `owd`, `owdf` | the same | 348.0 | the 3D error differs at 15 sites in 2 102 epochs, at most 0.0041 m (BADG00RUS, TOW 259410) |

The median column is the across-site median of each hour's p50 (`msc_pair.py`); each site's own
hourly p50 moves by up to 0.00195 m on the new wire (22 of 96 site-hours, the largest at DAV100ATA
in hour 1) and by up to 0.0001 m on the recorded wire (2 of 96). Every site's count is the same
in all six keys: CHTI00NZL 499, SCRZ00BOL 398,
ANMG00MYS 204, QUIN00USA 583, HARB00ZAF 358, POHN00FSM 0, DAV100ATA 339, BADG00RUS 185, STPM00SPM
581, ASCG00SHN 180, CEDU00AUS 357, KOKB00USA 125, UTQI00USA 13, ARHT00ATA 562, WUH200CHN 69,
BAKE00CAN 504. The consumer re-inflations per site, code bias raised on the new wire / the
recorded wire (the same in both builds), with 4 kept at every site and every other kind 0 raised
and 0 kept (`logs/rt_engine.log`): ANMG00MYS 71 / 71, ARHT00ATA 29 / 14, ASCG00SHN 16 / 16,
BADG00RUS 74 / 71, BAKE00CAN 45 / 43, CEDU00AUS 13 / 13, CHTI00NZL 6 / 6, DAV100ATA 26 / 14,
HARB00ZAF 24 / 21, KOKB00USA 19 / 12, POHN00FSM 26 / 22, QUIN00USA 18 / 13, SCRZ00BOL 13 / 13,
STPM00SPM 8 / 8, UTQI00USA 73 / 70, WTZZ00DEU 69 / 68, WUH200CHN 77 / 74.

Reading the keys together: the build of `e47b267` reproduces the recorded key byte for byte, so
every difference of `owdleg` is this card's, and the only consumer-side change that moves a
position on a GPS-only replay is O2's kept rule (CHTI00NZL keeps 4 and its positions are still
identical). `owd` against `owdleg` is the S line alone: the replay now builds the producer's
emitter from it (`logs/main_log.log`: threshold 0.0500 m, the code-jump and clock channels on)
and receives the clock-discontinuity channel (6 to 75 clock lines per site); no site's count
moves. At WTZZ00DEU, the wire's own closed-loop station, the final build's replay of the new wire
writes a `closed_loop_trace.csv` byte-identical to the in-process consumer's of
`_rap_owd_field6f`, while the legacy replay differs from it at 168 epochs by up to 0.0032 m
(`owdlegf`; 0.0031 m on the reference build). That is O1's purpose, met: a replay is the
consumer the producer ran.

The refusal: `runs/_rap_owd_refuse_anmg` (G1b's build) and `runs/_rap_owd_refuse_anmg_f` (the
final build), ANMG00MYS on `runs/_rf0hv10_field6/products.txt` without the knob, exit 1
(`exitcode.txt`), write no trace, and log the error that names the missing S line, its
consequence and the two remedies (`logs/main_log.log`).

**5.4 Goldens and G3: PASS.** The manifest pins `network.serialize_sbasis_contract: true` on both
current goldens and drops its exception; `scripts/make_golden_config.py --verify` passes with 62
armed in regime and 6 documented exceptions on each; each golden's diff is the knob line, the
comma above it and the two provenance lines. G3: `ActiveSystemAdmission.ArchivedGpsOnlyConfigsLoadWithTheWarning`
loads `config/test_suite/ppp_rtk_network_global_5min_gpsonly.json` and
`config/archive/test_suite/_sdewl_probe_gps10.json`; both load, each with the warning that its
'Galileo' list (100 signals, the attribute string expanded over five bands and four observation
types) is ignored because Galileo is not in `processing.active_systems`.

**5.5 Cost: PASS (not slower).** `_rap_owd_field6f`, run alone on the final build, spends
199 797.8 ms in the epoch loop over 720 epochs, 277.50 ms per epoch (G1b's `_rap_owd_field6`:
278.30), against 228 805.8 ms and 317.79 ms on `runs/_rf0hv10_field6`; the sim, also alone,
216.53 against 234.57 ms. The difference is not claimed as a gain: the 2 h field arms measure
339.34 (reference), 345.16 (G0) and 317.52 ms per epoch (G1b), and G0 and G1b differ only in the
setup-time latch, so the spread on this machine is at least 8 percent. The teardown self-check
re-reads the whole wire in 0.39 s (field, 720 epochs, 5 788 668 bytes) and 0.41 s (sim,
4 435 835 bytes).

**5.6 Found on the way.**

1. The solve's sensitivity to input bits (not fixed): a change of at most 1.2e-11 m in the
   group-delay pins (5.2) moved the field wire by up to 3.4e-5 m. Byte identity is therefore a
   statement about the exact bits of every input value, and any rule that recomputes an input
   must reproduce them. Unattributed.
2. O4's first build stripped the in-process consumer's feed but not the replay's own one-station
   sync, so the replay's observation vector stopped being the live feed that
   `ApplicationConsumerReplay.cpp` states it reuses the synchronizer to reproduce; it showed as
   `n_sats`, 46 in the replay against 12 in process at WTZZ00DEU's first epoch. The final build
   applies the same door in the replay (fixed; no position moved). Every closed-loop trace written from this build
   on counts only the admitted systems in `n_sats`, so a byte comparison with a trace written
   before it sets that column aside.
3. The four kept consumer re-inflations fall in the first two hours in both closed loops (the 2 h
   arms count the same four as the 6 h runs), consistent with section 1's reading: the receiver
   code-bias state starts at 10 m and a declared step requests 0.25 m2.

**5.7 Verdict.** Every pre-registered bar passes, with the two details of 5.2 on the 2 h
windows. The fixes have no knob and are in force from `d64c230`; the goldens pin the S line.
The card closes. Owed, the author's decisions (section 4): O6, the BeiDou pair rule; O7, the sim
fixture regeneration and the Galileo yaw law; health on the network path. O8 stays as section 1
left it.
