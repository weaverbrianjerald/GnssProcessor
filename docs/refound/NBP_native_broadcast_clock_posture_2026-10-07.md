# NBP: a native broadcast-clock producer posture (SP3 orbits and the broadcast clock, no CLK product)

**STATUS**

- **id:** NBP
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-07. Cards SCL, SCL2 and SCL2B measured the self-contained posture on a bed that writes the broadcast clock into a CLK product with each ephemeris cut-over's step removed. This card builds the posture natively: the producer reads SP3 orbits and the broadcast clock from the navigation file, selects the ephemeris by the bed's rule (the latest record transmitted at or before the epoch, healthy only), and keeps its a priori continuous with a per-ephemeris offset table, while the wire's clock refers to the stamped ephemeris (IODE) so a consumer holding that record reconstructs the same continuous clock. Nothing moves in the solve. Finding before the build: the shipped `apriori_source brdc_rts` path selects by the closest toe and differs from the bed's rule on 99.6 % of satellite-epochs (a priori difference p50 87 mm, max 1.12 m), so a real-time version of the measured posture needs this card's rule. Gates in section 5.
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
