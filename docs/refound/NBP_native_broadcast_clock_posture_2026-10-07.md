# NBP: a native broadcast-clock producer posture (SP3 orbits and the broadcast clock, no CLK product)

**STATUS**

- **id:** NBP
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-07. Cards SCL, SCL2 and SCL2B measured the self-contained posture on a bed that writes the broadcast clock into a CLK product with each ephemeris cut-over's step removed. This card builds the posture natively: the producer reads SP3 orbits and the broadcast clock from the navigation file, selects the ephemeris by the bed's rule (the latest record transmitted at or before the epoch, healthy only), and keeps its a priori continuous with a per-ephemeris offset table, while the wire's clock refers to the stamped ephemeris (IODE) so a consumer holding that record reconstructs the same continuous clock. Nothing moves in the solve. Finding before the build: the shipped `apriori_source brdc_rts` path selects by the closest toe and differs from the bed's rule on 99.6 % of satellite-epochs (a priori difference p50 87 mm, max 1.12 m), so a real-time version of the measured posture needs this card's rule. Gates in section 5. BUILT, GRADED AND LANDED 2026-10-07 (sections 7 and 8): every gate passed; the native posture equals the bed to the print resolution (estimate, level, counters, consumers within 0.1 mm, both censuses median 0) at no cost; findings: the store's freshest-toe rule rejected the records the ttom rule stamps (a transmission-time insert added), the wire's C steps at the cut-overs so graders read C - O (an O trace), and the consumer's fallback for a satellite without a C record differs (did not act). Landed default off; nothing armed. MULTI-DAY READ (section 10): the self-contained native posture does not hold against the COD golden on DOY 130 to 132 (pooled median +0.017 m over hours 2 to 6, 40 % of site-days worse by more than 0.02 m; DOY 129 was its most favourable day), while it prevents ARHT00ATA's divergence; the attribution is owed.
- **source:** [SCL](SCL_self_contained_clock_level_2026-10-03.md) sections 2 and 5 ("Not in this card"); [SCL2B](SCL2B_shared_history_with_the_hold_2026-10-07.md) (the posture as graded); the read-only map `runs/_scl2_handoff/threads/rep_nbp_map.md` (2026-10-07, main `6655ba4`)
- **knobs / posture:** proposed, default off, fingerprinted (fresh convergence only): `network.apriori_source` value `sp3_brdc_clock` (with the sp3 posture's anchor sigma, not `brdc_rts`'s 3 m), `network.brdc_select` (`ttom`, the only rule at first). The loader requires card SMH's smoothed hold under the new value and refuses card LAH's phase-clock hold (valid only with a precise a priori, card SCL section 3.1). Nothing armed
- **dates in the text:** 2026-10-07
- **hashes / run dirs:** to be recorded at the build; the bed reference `runs/_scl_data_bc` (`scripts/clock_level/scl_brdc_apriori.py`), the new data root `runs/_scl_data_nbp` (the COD root without CLK files, G01 kept so health is exercised)

## 1. The question

Can the producer run the self-contained posture of card SCL2B without a CLK product, reading the broadcast clock from the
navigation file and declaring each ephemeris cut-over, so that its estimate, its wire and its consumers equal what the bed
measured?

## 2. Facts from the map (DOY 129, `BRDC00IGS_R_20241290000_01D_MN.rnx`)

- 32 GPS satellites, 465 records; only G01 unhealthy (63, all day); transmission time lead to toc p50 7182 s; a
  transmitted record exists at every 30 s node.
- 414 cut-over steps (G01 out): p50 84 mm, p90 174 mm, max 0.793 m, clustered at the even hours (31 to 35 per cluster);
  the pivot G02 cuts over 13 times (8 to 172 mm).
- Producer today (`sp3`): the a priori clock comes from the CLK cache (`GnssCorrections.cpp:161`), the broadcast store
  only as a fallback (`:218-225`). `brdc_rts` refuses SP3 and CLK (`ApplicationNetworkSetup.cpp:494`), feeds the store
  by toc, picks the closest toe (`LiveEphemerisStore.cpp:280-287`) and re-levels the carry at a cut-over after PASS B
  (`NetworkSolveGlue.cpp:1264`, `NetworkRelevel.cpp:114-169`).
- The consumer replay driver (`ApplicationConsumerReplay.cpp`) has no broadcast feed; a CLK left in a native root would
  shadow the broadcast clock silently.

## 3. The design (realization B of the map)

- **Selector** (`products/BrdcClockApriori.{hpp,cpp}`, new): the record of satellite s at epoch t is the latest one with
  transmission time at or before t and health 0 (ORBIT-7 parsed in `RinexNavParser`, beside the existing fields).
- **Continuous internal a priori:** a_s(t) = brdc_k(t) + O_s,k with O built at setup from the staged navigation: from
  00:00 of the observation day, at each switch node O_k = O_k-1 + brdc_k-1(t_sw) - brdc_k(t_sw) (the bed's rule,
  `scl_brdc_apriori.py:56-61`), for every satellite whether tracked or not; a fixed origin, recomputed on a warm resume.
- **Wire:** C_wire = C_est + O_s,k with `iode=k`, applied after the hold and before the store and the product write; the
  consumer forms brdc(k) + C_wire, which is continuous (card 7's IODE path). An O trace is emitted so graders can form C
  - O.
- **Store feed by transmission time** so the stamped record is current or previous for a consumer; the IODE stamp from
  the selector.
- **S line:** `sp3+brdc:<SP3>|<nav>|ttom`; a consumer refuses a CLK-bearing root under it.
- **Replay driver:** a navigation stage and feed, and no CLK load when the S line says native.
- Not in this card: the carry re-level design (realization A, `brdc_rts`'s); a mid-day health flip (none on DOY 129); a
  real-time switch delay (post-processing uses 0); multi-day navigation.

## 4. A finding recorded, not fixed

The shipped `brdc_rts` re-level runs after PASS B, so under the armed exact coupling update the cut-over epoch's rows are
in the old frame (`NetworkRelevel.cpp` notes it; the loader refuses the before-rows placement with `brdc_rts`). It is
armed nowhere; recorded for whoever arms `brdc_rts`.

## 5. Gates (frozen before the build)

| gate | bar |
| --- | --- |
| U0 | Full Debug suite with no new failure; `make_golden_config.py --verify` PASS; `config_posture_check.py --verify-loader` 0 gaps; `docs_index_check.py` passes. |
| U1, parser | ORBIT-7 parsed on the DOY-129 file: 465 GPS records, transmission lead p50 7182 s. |
| U2, selector | The selected record equals `scl_brdc_clk.py`'s ttom pick on 100 % of the 30 s grid (G01 excluded by health); a_s(t) equals the bed's CLK value (`scl_brdc_apriori.py --continuous --rule ttom`) at every node to the CLK print resolution. |
| U3, wire | brdc(iode) + C_wire equals a_s + C_est to 1e-9 m; the stamped record held at consume time on 100 % of records. |
| G0 | Knob off: the 2 h goldens byte-identical to the same exe's controls (products without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`); the `brdc_rts` unit tests unchanged. |
| G1, equivalence with the bed | The native posture against the bed on the same arm (q 5e-6, card SMH's hold, B1 + B2 + keep, X2 with X2b, as `runs/_rap_scl2h_*`), 2 h then 6 h, sim and field: emitted epochs, anchor trips, datum decouples and clone counts equal; C_est (C_wire - O) p99 absolute difference at most 1 mm; the de-commoned level within 1 mm per hour (sim); SZZ000SIM and WTZZ00DEU within 2 mm per hour; card PVN's NIS within 0.02. |
| G2 | The consumer's phase-visible clock change at the 12 cut-over clusters within the run's p95 of other epochs (sim truth). |
| G3 | A consumer replay of the native wire byte-identical to the in-process consumer (replay offset 0). |
| G4 | Field census (`census6h.py`) of the native wire paired against the bed key `s2xb`: absolute median at most 0.002 m over hours 2 to 6, no site differing by more than 0.01 m. |
| G5 | Sim census (`SIMCENSUS_FROM_START=1`) of the native wire against the bed wire made the same way: the same bar. |
| G6 | G01 contributes 0 observations; a record removed from the navigation file drops the satellite and re-admits it with an event (unit). |
| G7 | Loop time within 2 % of the bed arm. |

Arming stays with the decision on card SCL2B's posture.

## 6. Order

1. U1, U2 in the unit suite (parser and selector against the bed tooling) before any producer code.
2. The build; U0, U3, G0.
3. G1 on 2 h, then 6 h; G2, G3.
4. G4, G5; G7.

## 7. Built and graded (2026-10-07)

**The build** (session worktree at `9da0484`; exe `gnss_processor_nbp.exe`, sha256[:16] `38c4e88bd2c4a274`; a first build
`dd5b68392d47da0a` wrote a malformed S line and is superseded). `RinexNavParser` reads ORBIT-7 (transmission time, fit
interval, IODC); `products/BrdcClockApriori` holds the selector, the offset table and the wire stamp; the loader adds
`network.apriori_source` `sp3_brdc_clock` and `network.brdc_select` `ttom` (requires card SMH's smoothed hold, refuses card
LAH's phase-clock hold, requires SP3, refuses a loaded CLK, GPS only); producer stations take their a priori clock from the
selector; a new seam `filter/NetworkBrdcWire` writes C_wire = C_est + O with `iode=k` after the hold and an O trace
(`logs/network_brdc_offset.csv`); the replay driver stages and feeds the navigation file and refuses a CLK-bearing root under
the native S line; run-end telemetry; tests `tests/test_brdc_clock_apriori.cpp`. Data root `runs/_scl_data_nbp`.

Three findings of the build:

1. The store's freshest-toe rule rejected 43 new-upload records whose toe is 16 s earlier than the held record's, which are
   the records the ttom rule stamps: with the plain insert only 83 917 of 89 280 stamps were held, so a pinned consumer
   could not reconstruct the rest. The native feed inserts by transmission time (`LiveEphemerisStore::insertTransmitted`).
   A live RTCM stream under the ttom rule has the same hazard.
2. The wire's C is not the bed's C: it steps by up to 0.91 m at the cut-overs, and every grader that reads C must read C - O
   (the O trace provides it; with C - O every level grader reproduces the bed).
3. A consumer's fallback for a satellite with no C record differs: on the bed it took the bare CLK a priori as product-grade
   and kept its phase; natively it is broadcast-grade and code-only. It did not act on any graded window (the field
   census's satellite sets are identical).

**Gates** (arms `runs/_rap_nbp_*`; the bed arms of card SCL2B as the reference):

| gate | reading | verdict |
| --- | --- | --- |
| U0 | Debug suite 2839 tests: 2822 passed, 17 data-fixture skips, 0 failed; the golden verify PASS; the posture check 0 gaps | PASS |
| U1 | 465 GPS records with ORBIT-7; transmission lead p50 7182 s | PASS |
| U2 | the selected record equals the ttom pick on 89 280 of 89 280 healthy satellite-nodes (G01 never selected); a_s(t) equals the bed's CLK at every node to one unit of the last printed digit (1.9e-7 m); 414 cut-overs | PASS |
| U3 | 89 280 records stamped and held; max absolute brdc(iode) + C_wire - (a_s + C_est) 2.9e-11 m | PASS |
| G0 | knob off byte-identical to the base exe on the 2 h goldens (sim and field) | PASS |
| G1 sim | 2 h, 6 h and 24 h: emitted epochs, trips, decouples and clone counts equal, every run-end line byte-identical; C_est p99 0.0035 mm (max 0.0047); the de-commoned level equal to 0.0001 mm per hour; SZZ000SIM equal to 0.1 mm; PVN NIS equal | PASS |
| G1 field | 6 h: every count equal; C_est p99 0.0203 mm (max 0.67 mm at the first epoch); WTZZ00DEU +0.1 mm in hour 1, 0 after; PVN NIS 1.45916 against 1.45915 | PASS |
| G2 | over 24 h, 94 switch pairs at SZZ000SIM: the consumer's phase-visible change at a switch p50 0.94 mm against 1.27 mm elsewhere, 3 of 94 above the run's p95 (3.2 %); the step's leakage slope +0.0028 plus or minus 0.0027 (bound about 0.8 %); an IODE-blind consumer would show p50 89 mm. One epoch (18:01:00) exceeds the epoch-rms p95, driven by the pivot G02's own motion; the bed gives the identical value there | PASS on the cut-over question |
| G3 | replays at offset 0 byte-identical to the in-process consumer (2 h, 6 h, 24 h; field census replay of WTZZ00DEU likewise); a native wire replayed on a CLK-bearing root refused | PASS |
| G4 | field census against `s2xb`: every hour equal, median +0.0000 m, largest per-site change 0.020 mm | PASS |
| G5 | sim census from the start against the bed made the same way: median +0.00000 m every hour, largest station-hour change 0 | PASS |
| G6 | 0 rows name G01 in 17 traces and the products (the roster's RINEX hold 4 574 G01 records; 14 unhealthy records not selectable); the unit half (a removed record) not run | run half PASS |
| G7 | loop time -0.10 % against the bed (two interleaved pairs, one job at a time) | PASS |

**Reading.** The native posture is the bed moved into the producer: estimate, wire after C - O, consumers and censuses equal
to the bed's to the print resolution, at no cost. Its product is the posture without a CLK file, on the selection rule the
measurements used, with the stamp a consumer needs. The cut-over steps live only on the wire, where a consumer that honours
`iode=` sees nothing. Owed: the unit half of G6 (a removed record), a mid-day health flip, a real-time switch delay, the
consumer fallback of finding 3 on a window where it acts, and multi-day navigation.

## 8. Landed (2026-10-07)

The patch (28 files, `runs/_scl2_handoff/nbp/nbp_full.diff`) is committed on main with this section, default off. Verified
before the commit on a clean worktree of main `9da0484` with the patch applied: every file byte-identical to the tested
worktree; Release exe `gnss_processor_nbpL.exe` (sha256[:16] `f1af836195b4af1e`); the Debug suite 2839 tests, 2822
passed, 17 data-fixture skips, 0 failed; the golden verify PASS, the posture check 0 gaps; the 2 h goldens byte-identical
to the base exe with the knob off (`runs/_rap_nbpL_g0_{sim,fld}`); the native smoke arm byte-identical to the tested exe's,
O trace included (`runs/_rap_nbpL_smoke` against `runs/_rap_nbp_smoke_sim`). Another workstream's uncommitted edits in
`CMakeLists.txt` and `GnssConfigLoadNetwork.cpp` were left out of the commit.

## 9. The self-contained native posture on three more days (pre-registered 2026-10-07, before any run)

Every graded number of cards SCL to NBP is DOY 129. The read: on DOY 130, 131 and 132 (card MDR's data, 6 h from 00:00,
the field), the full self-contained native posture (card SCL2B's posture with this card's `sp3_brdc_clock`: q 5e-6, card
SMH's smoothed hold, B1, B2 and the keep, X2 with X2b, a per-day native root of SP3, ERP, OSB and the day's navigation file
with no CLK) against the field golden (COD a priori, card LAH's phase-clock hold) on the same day and the same exe
(`gnss_processor_nbpL.exe`), each day's roster moved by `scripts/mday_golden_config.py`. Graded on WTZZ00DEU by hour and
the 16-site census (`scripts/clock_level/mday_census.py` or `census6h.py`, PBF on, accumulating consumers), per day and
pooled over the three days and DOY 129 (key `nbp_n` against `s16c`).

Reading (card SCL's G4 statistic, no bar changes elsewhere): the posture holds across days if, pooled over the 48
site-days, the median paired change of the site p50 over hours 2 to 6 against the golden is at most +0.010 m and at most 20 %
of site-days are worse by more than 0.02 m; per day the same statistic is reported. Also reported: the run-end counters
(emitted epochs, anchor trips, datum decouples, NBP records stamped and held, cut-overs), any day whose navigation file
lacks a transmitted record at an epoch, and the day's unhealthy satellites.

## 10. The multi-day read: the self-contained posture does not hold against the golden (2026-10-07)

Arms `runs/_rap_nbpm_{130,131,132}_{g,n}` (6 h from 00:00; exe `gnss_processor_nbpL.exe`; day configs from
`scripts/mday_golden_config.py`, the observations on `D:/GnssProcessorSupport/data_mday`; native root
`runs/_nbp_mday_root`: SP3 of DOY 129 to 132, the day's OSB, ERP and navigation file, no CLK); census replays
`runs/_rap_nbpmc{g,n}_*` (`scripts/clock_level/mday_census.py`, PBF on, accumulating consumers; 102 of 102 exit 0). Each
golden arm is byte-identical to card MDR's wire of that day and its census reproduces card MDR's tables; every WTZZ00DEU
replay equals the in-process consumer.

The native machinery holds on every day: the navigation files have a transmitted healthy record at every node (G01 unhealthy
all day on each), 21 921 to 21 953 records stamped and all held, 104 to 107 cut-overs in the 6 h, 0 datum decouples, anchor
trips 3 to 18 against the golden's 38 to 66.

| set | site-days | median, native minus golden, hours 2 to 6 | worse by more than 0.02 m | better |
| --- | --- | --- | --- | --- |
| DOY 129 (`nbp_n` against `s16c`) | 16 | +0.0030 m | 4 (25 %) | 4 |
| DOY 130 | 16 | +0.0191 | 7 (44 %) | 2 |
| DOY 131 | 16 | +0.0090 | 5 (31 %) | 2 |
| DOY 132 | 16 | +0.0171 | 7 (44 %) | 2 |
| pooled, DOY 130 to 132 | 48 | **+0.0171** | **19 (39.6 %)** | 6 |
| pooled with DOY 129 | 64 | +0.0109 | 23 (35.9 %) | 10 |

Census medians of the site p50 by hour (golden / native): DOY 130 0.290 0.094 0.053 0.049 0.047 0.045 / 0.236 0.098 0.097
0.076 0.057 0.061; DOY 131 0.216 0.065 0.057 0.056 0.047 0.047 / 0.228 0.076 0.075 0.056 0.063 0.050; DOY 132 0.211 0.067
0.066 0.051 0.058 0.030 / 0.530 0.179 0.083 0.083 0.071 0.046. WTZZ00DEU, mean change over hours 2 to 6: +0.0027,
+0.0101, +0.0265 m; its hour 1 is worse every day (0.13 to 0.27 m against 0.06 to 0.09).

**Reading: section 9's criterion does not hold, on every day.** The self-contained posture converges more slowly in
hours 1 to 3 and keeps a lasting offset at a few sites (ASCG, HARB and SCRZ worse on all four days; HARB on DOY 132 0.06 to
0.22 m through hours 3 to 6 against 0.014 to 0.043). Without hour 2 (hours 3 to 6) it still fails: median +0.0112 m, 17 of
48 worse. Where the golden fails, the native posture holds: ARHT00ATA does not diverge on DOY 131 (the golden runs to 166
m) and is better on 3 of 4 days. DOY 129, where every card of the self-contained program was graded, is its most favourable
day. The native machinery (card NBP's build) is not implicated: it equals the bed on DOY 129 to the print resolution; what
fails is the posture, against a golden that uses COD's clocks.

Owed (pre-register before any build): the attribution of the cost between the a priori's information (the broadcast clock
against COD) and the regime (q 5e-6, card SMH's hold, B1, B2, X2), by the COD a priori under the self-contained regime on
the same days; and the hour-1 convergence of the self-contained posture.
