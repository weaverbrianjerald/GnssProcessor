# BDS: BeiDou as the third constellation of the field golden

**STATUS**

- **id:** BDS
- **topic:** network-vehicle
- **status:** open
- **summary:** Read 2026-10-18. First GPS + Galileo + BeiDou network run on the field golden (DOY 129 2024, 58 stations, held-out consumer WTZZ00DEU), graded against GPS only and GPS + Galileo on one exe. BeiDou on B1I + B3I (C2I / C6I / L2I / L6I), the pair the loader's dual mode names. HEALTHY: 720 of 720 epochs emitted, no withhold, BeiDou clocks metre class (maximum 14.6 m, median 3.9 m), no failure mode A, no failure mode B, so none of the four pieces was needed. Three stations that track BeiDou on B1I alone are dropped whole by the addStation guard (GEC); with their band-2 BeiDou removed (GEC-s) the run is clean. Census, hours 2 to 6: GEC-s against GE -0.0038 m (5 better, 0 worse; the ten sites with the pair -0.0143 m), against GPS only -0.0267 m (12 better, 1 worse). WTZZ00DEU hours 2 to 6 within +0.007 m of GE. Cost 2 to 3 times GE. Nothing is armed by this card. The single-band rule is built behind `network.second_band_admission` (section 8, 2026-10-18): on 2 h arms it reproduces the subtraction (no station dropped, near-null count 3, consumer within 1 mm of GEC-s). Section 9 (2026-10-19): key off, 24 IDENTICAL on the identity gate against the parent-commit exe; key on, 6 h, it reproduces GEC-s (no station dropped, no heal, census hours 2 to 6 -0.0035 m against GE, 5 better, 0 worse, and 0.0000 m against the GEC-s census, no site over 6 mm; consumer within 1 mm of GEC-s); UNVALIDATED, not armed.
- **source:** the author's request of 2026-10-18 ("perhaps we should do a GPS+Galileo+BeiDou run to compare against GPS-only and GPS+Galileo"); [GAD](GAD_galileo_adoption_2026-10-13.md) (how Galileo was added and graded); [SPEC_beidou_w6_datum.md](../SPEC_beidou_w6_datum.md) and [SPEC_cb_admission_card.md](../SPEC_cb_admission_card.md) (the four-piece emit path, failure mode B)
- **knobs / posture:** `processing.active_systems` `["GPS","Galileo","BeiDou"]`, `processing.signal_priorities.BeiDou` `["C2I","L2I","C6I","L6I"]`; candidate pieces, each only on the evidence of section 3: `network.cb_admission`, `network.rank_gate_tol` (placement), `network.cb_kaula_prior`; `network.phase_datum_prioritized` with `network.phase_datum_min_stations` 2 and `network.rank_gate_tol` 1e-11 are already in the goldens; section 8: `network.second_band_admission` (default false, UNVALIDATED), `network.second_band_scope` (`station`), `network.second_band_min_sats` (1)
- **dates in the text:** 2026-10-18
- **hashes / run dirs:** exe `gnss_processor_w3base.exe` (main `30ca49e`, sha256 `50ca2ea5fbbc540cbb2b41a0b208313e0b88cf0f60ae134e0230f778d209d285`) for every run; run root `D:/GnssProcessorSupport/runs_nom` (`RAPID_RUNS_ROOT`): 2 h `_rap_bds_{g2,gp2,ge2,gec2,gecs2}`, 6 h `_rap_bds_{g6,ge6,gec6,gecs6}`, census keys `bdsg`, `bdsge`, `bdsgec`, `bdsgecs`, `bdsgecge`, `bdsgecg`, `bdsgecsge`, `bdsgecsg` (replays `_rap_<key>_<site4>`), BeiDou-only consumer `_rap_bdsxc_bdsonly_wtzz`; scratch and scripts `D:/GnssProcessorSupport/gates/bds/`; section 8: branch `fix/single-band` commit `1546d1c`, exes `gnss_processor_sband2.exe` (sha256 `1183ce584a938bdc6827db126d0b3f2def18839607a2906e80f236dca2d86655`) and `gnss_processor_sband_ref.exe` (parent commit `a54155a`, sha256 `5fc9c713269cb26adb0946dd64844bc52e40112d4bd11edcc5aea9179926c5dd`), runs `_rap_sband_*` under the same run root, scratch `D:/GnssProcessorSupport/gates/sband/`; section 9: runs `_rap_sband_{ge6,off6,on6}` and census keys `sbandge`, `sbandoff`, `sbandon` on `gnss_processor_sband2.exe`, gate sides `w3sband` and `w3sbandref` under `gates/g5` and `gates/g7`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Question: does a third constellation, BeiDou on B1I and B3I, run healthy on the self-contained field golden, and what does it change for the held-out consumer and the 16-site census against GPS + Galileo and against GPS only? It runs healthy: 720 of 720 epochs on every arm, BeiDou clocks in the metre class (failure mode A absent at 58 stations), the rank gate never failed (failure mode B absent), no piece armed. As pre-registered (GEC) the three single-band stations are dropped at every epoch and the phase-anchor heal fires on 551 of 720; with their band-2 BeiDou removed (GEC-s) nothing fires. Census hours 2 to 6: GEC-s -0.0038 m against GE (5 better, 0 worse), -0.0267 m against GPS only (12 better, 1 worse); the consumer is within +0.007 m of GE in every settled hour. Users of the BeiDou wire without BeiDou are unchanged at the median (ARHT00ATA named). Cost 2 to 3 times GE on a shared box. The single-band rule (section 9) reproduces GEC-s on 6 h arms with the key on and is byte-identical with it off. Arming is open.

## 1. Question and history

The goldens are GPS + Galileo since card GAD (2026-10-13). BeiDou has never run on them. Two failures are on record and are kept apart.

- **Failure mode A, a satellite-clock divergence.** A 305-station W6-off run (2026-06-29, COD MGX final products) that added BeiDou to GPS + Galileo showed a satellite-clock change against the product of 75 km class at the first epoch (emitted BeiDou clock magnitude: mean 84 km, maximum 530 km), leaking into the GPS and Galileo clocks. It was present at epoch 1 and stable, so an a priori, gauge or conditioning cause, not a drift. The root cause was never found and is recorded outside the repository docs. Refuted then: missing products, the BeiDou frequency constants, the W6 IF pin, and the `MAX_CLOCK_BIAS` node reject.
- **Failure mode B, bias-datum under-specification.** With W6 on, BDS-3 signal sparsity on the phase-bias and code-bias columns left the bias datum under-specified and the null-space gate withheld every epoch. Closed by four pieces, none sufficient alone ([SPEC_beidou_w6_datum.md](../SPEC_beidou_w6_datum.md) section 0.1): the code-bias admission census with the state-allocation phase co-gate (`network.cb_admission`), the rank-gate tolerance placed per configuration (`network.rank_gate_tol`), the phase-anchor heal (`network.phase_datum_prioritized` with `phase_datum_min_stations`), and the Kaula prior on the admitted code biases (`network.cb_kaula_prior`). The graded emit leg never ran, so BeiDou product quality is unclaimed in the record.

What differs now: the goldens are self-contained (satellite geometry-free code bias pinned at the broadcast T_GD, no GIM, no Kaula prior, free per-LOS slant ionosphere), run the float vehicle and the exact coupling update, and already carry two of the four pieces (`rank_gate_tol` 1e-11 and the phase-datum prioritization with `phase_datum_min_stations` 2). `network.cb_admission` and `network.cb_kaula_prior` are KEEP_OFF in the ledger and off in both goldens. Both failures were seen at 305 stations; this run has 58. This card does not assume that either failure recurs or is absent. The figures of failure mode A above are as recorded in the author's working notes.

## 2. The BeiDou signals, chosen from the data

Counted over the 59 RINEX files of the field golden (58 network stations and the consumer WTZZ00DEU), observations of constellation C with a non-zero value in the first 6 h of the day (00:00 to 06:00, 720 epochs), pairs counted when a satellite has both the code and the phase of both bands at one epoch. Script `D:/GnssProcessorSupport/gates/bds/bds_count.py`, tables `bds_tracking_6h.json` and `headers.json` beside it.

| dual pair (code / phase) | network stations of 58 | with the consumer | pair satellite-epochs, 6 h |
| --- | --- | --- | --- |
| B1I + B3I (C2I L2I / C6I L6I) | 48 | 49 | 586,226 |
| B1I + B2I (C2I L2I / C7I L7I) | 50 | 51 | 207,084 |
| B1I + B2b (C2I L2I / C7D L7D) | 25 | 25 | 141,595 |
| B1I + B2a (C2I L2I / C5X L5X) | 21 | 22 | 163,279 |
| B1C + B2a (C1X L1X / C5X L5X) | 20 | 21 | 154,901 |
| B1C + B2a (C1P L1P / C5P L5P) | 21 | 21 | 141,494 |

Single types, network stations: C2I 51, C6I 48, C7I 51, C7D 25, C5X 21, C5P 21, C1P 21, C1X 20, C7Z 13, C8X 13, C1I 1. The header declarations agree (C2I 52 and C6I 49 files of 59). The X variants of B1I and B3I (C2X, C6X) occur in no file.

Ten network stations carry no usable BeiDou pair: six track no BeiDou at all (STJO00CAN, KOKV00USA, CHUR00CAN, SYOG00ATA, VOIM00MDG, SVTL00RUS) and four track B1I without B3I (WARK00NZL, MAJU00MHL, SCTB00ATA; HKSL00HKG spells B1I as C1I / L1I, RINEX 3.02, and tracks B2I). Satellites with the B1I + B3I pair at 3 or more stations: 50 of 50 tracked PRNs; 12 of them have no orbit in the COD MGX final SP3 (C01 to C05, C50, C56 to C60, C62; the SP3 holds 38 BeiDou satellites) and cannot enter, which removes every tracked GEO (the seven of C01 to C05, C59, C60). The 38 with an orbit are 10 IGSO, 3 BDS-2 MEO and 25 BDS-3 MEO; each is tracked with the pair at 16 or more stations (minimum C46 at 16). Of the 16 census sites, ten carry the B1I + B3I pair (QUIN00USA, HARB00ZAF, POHN00FSM, DAV100ATA, STPM00SPM, ASCG00SHN, CEDU00AUS, KOKB00USA, ARHT00ATA, WUH200CHN), CHTI00NZL and SCRZ00BOL track B1I without B3I, ANMG00MYS tracks C1I and B2I, and BADG00RUS, UTQI00USA and BAKE00CAN track no BeiDou. WTZZ00DEU carries the pair (27 satellites, 9,579 pair satellite-epochs in 6 h).

**Choice: B1I + B3I.** It has the largest pair volume (2.8 times the next pair) and 48 stations; B1I + B2I reaches two more stations but carries a third of the rows. The loader decides the same way: `processing.frequency_mode` `dual` maps BeiDou to the bands B1I and B3 (`getBandMap` in `src/core/model/PppModel.cpp`), that is the RINEX band digits '2' and '6' (`getAllowedBandDigits`), so B1I + B2I and B1C + B2a are not selectable in dual mode. B1C + B2a (21 stations) would need triple or all mode, which changes GPS and Galileo as well. The BeiDou entry of `processing.signal_priorities` is the explicit list `["C2I","L2I","C6I","L6I"]`, the form of the Galileo entry. The loader names the system `"BeiDou"` in `processing.active_systems` (`GnssConfigLoad.cpp`; the RINEX letter is `C`, `System::BeiDou` in `common/GnssIdentity.hpp`).

## 3. Arms, the piece rule and the stop rule

Every arm is a copy of `config/golden/golden_field_refound.json` made with `scripts/rapid_arm.py make`, the output path and the duration (`--hours`) aside. The golden, the ledger and the manifest are not edited.

| arm | differs from the golden | tags |
| --- | --- | --- |
| GE | nothing (GPS + Galileo, the baseline of card GAD) | `bds_ge2`, `bds_ge6` |
| GEC | `processing.active_systems` + `"BeiDou"`; `processing.signal_priorities.BeiDou` = `["C2I","L2I","C6I","L6I"]` | `bds_gec2`, `bds_gec6` |
| G | `processing.active_systems` `["GPS"]`; the `processing.signal_priorities.Galileo` entry removed (a derived base file, because `--set` cannot delete a key) | `bds_g2`, `bds_g6` |
| G-probe | G with the four Galileo-package keys at their loader defaults (`estimation.clock_disc_isb_reinflate_std` 0, `model_corrections.enable_receiver_cross_system_atx_fallback` false, `network.code_datum_family_difference` false, `network.code_family_birth_prior_sigma_m` 0) | `bds_gp2` |

G keeps the four Galileo-package keys of the golden, which are Galileo-specific and need two or more constellations; card GAD's GPS-only base carried none of them. The G-probe proves they are inert: if `bds_g2` and `bds_gp2` are identical over 240 epochs, G stands; if not, G-probe is the GPS-only arm of the 6 h stage and the difference is reported.

**Piece rule.** A BeiDou-required piece is armed only if the 2 h GEC run shows the failure that piece answers, one piece at a time, each named in the card and followed by a repeat of the 2 h GEC run (tag `bds_gec2_<piece>`):

- `network.cb_admission` (with `cb_admission_min_stations` 4 and `cb_admission_census_epochs` 15): only if the rank gate withholds epochs as UNDER-specified and the estimability dump (`network_estimability.csv`, `trace_estimability` is armed in the golden) puts the below-cutoff directions on sparse code columns of `C##` (the sparse-code confound class of SPEC_beidou_w6_datum section 0.1).
- `network.rank_gate_tol`: re-placed only from the GEC run's own spectrum, at least two decades above the gauge depth and two below the weakest real layer, and only if the gate withholds in either direction with the weak-real layer on the wrong side of the 1e-11 cutoff.
- `network.cb_kaula_prior`: only after the two above were tried, because gate calibration precedes any prior (the author's ruling, SPEC_beidou_w6_datum section 0.3), and only if the remaining withheld directions are code-bias dominated.
- The phase-anchor heal is already armed in the golden; no action.

**Stop rule.** If GEC diverges (failure mode A: any emitted BeiDou clock of kilometre class) or still withholds after the documented pieces, the card stops and records the evidence for the root-cause search: the first emitted epoch, the state columns involved (`CLK|C##`, `CB|C##`, `PB|C##`), the BeiDou clock a priori at that epoch (the SP3 / CLK entry and the `apriori_id` of the S line), the per-satellite clock values, and the spectrum. No fourth mechanism is improvised.

## 4. Gates (frozen before any run was read)

One exe (`gnss_processor_w3base.exe`) runs every arm; arms of one stage run concurrently. The 2 h stage develops and gates the 6 h stage. Hours are counted from 00:00; hour 1 is ignition, settled claims come from hours 2 to 6.

**Stage 1, 2 h, 240 epochs per arm.**

| gate | bar |
| --- | --- |
| B0 | The arm records differ from the golden by exactly the keys of section 3 (`rapid.json` and a flattened diff of `arm.json` printed); the exe hash is the one in the STATUS block. |
| H0 | `bds_g2` and `bds_gp2` identical over 240 epochs (`scripts/clock_level/ident2h.py`: products without comment lines, closed-loop trace, epoch status). |
| H1 | Each arm exits 0; 240 of 240 epochs emitted; 0 withheld by the observability gate, the A4 ceiling or the rank gate (UNDER or OVER-specified), 0 skipped (the run-end lines `Network producer complete` and `Network (Card DG)`). |
| H2 | GEC, emitted BeiDou satellite clock `c0` (the `C,C##` lines of `products.txt`): at most 50 m in magnitude at every epoch (metre class). Any value of 1 km or more is the failure-mode-A signature. A maximum between 50 m and 1 km is neither healthy nor mode A: the 6 h stage waits for the author. |
| H3 | GEC against GE, same epochs, GPS and Galileo clocks: the per-epoch rms of the change after removing the per-epoch median over the constellation (datum-free) at most 1.0 m at every epoch (mode A raised GPS by 3,200 and Galileo by 6,100 times), and the maximum |`c0`| of both at most 50 m. |
| H4 | GEC, ON-path telemetry (invariant 4): the median epoch carries at least 10 `C##` satellites on the wire, each with a clock line, code-bias lines and phase-bias lines; the run log's BeiDou counters (admission withholds on `C##` channels, gross-LOS and RAIM events by `C##` satellite) are reported with the zero cases named. Fewer than 10 means BeiDou is inert whatever the log says. |

Stage 2 runs only if G, GE and GEC pass H0 to H4 (with the pieces of section 3, named, if any were needed).

**Stage 2, 6 h, 720 epochs per arm, three arms concurrently.**

| gate | bar |
| --- | --- |
| S1 | H1 to H4 over 720 epochs (720 of 720 emitted on each arm). |
| S2 (consumer) | `closed_loop_trace.csv` of WTZZ00DEU by hour (3D p50, epochs below a decimetre): GEC against GE no worse than +0.010 m in any of hours 2 to 6. The consumer's systems are stated per row: the arm's own systems (WTZZ00DEU tracks the B1I + B3I pair, so the GEC consumer uses BeiDou), and the GPS + Galileo consumer of the GEC wire (a replay with the GE arm's configuration, `gcensus.py`). WTZZ00DEU is one instrument and is read, not adopted on (card MSC). |
| S3 (census, GE against G) | `gcensus.py` keys `bdsge` (base and wire GE) against `bdsg` (base and wire G): the reproduction of card GAD's clause on this exe. Bar: paired median over hours 2 to 6 at most 0 m and more sites better than worse (on file for card GAD on `gnss_processor_galp2.exe`: -0.0198 m, 10 better, 2 worse). |
| S4 (census, GEC against GE) | `bdsgec` (base and wire GEC) against `bdsge`. Bar: paired median over hours 2 to 6 at most 0 m and more sites better than worse; no site worse by more than 0.05 m without being named. Hour 1 is reported. |
| S5 (census, users of the GEC wire) | `bdsgecge` (GE arm as the consumer base, GEC wire) against `bdsge`: paired median over hours 2 to 6 at most +0.005 m (the form of card GAL2's G5 clause). `bdsgecg` (G arm, GEC wire) against `bdsg`: the same bar. Sites worse by more than 0.05 m are named. |
| S6 (cost, read) | Wall time (`exitcode.txt`) and the epoch loop's mean time per arm, with the concurrent load stated. |
| S7 (BeiDou wire, read) | Datum-free rms of `c0` by hour for `C`, `G` and `E` (deviation from the a priori product, not a truth), the median count of `C##` satellites per epoch, the BeiDou rows of the run-end tables. Extra, not gated: a BeiDou-only consumer of the GEC wire at WTZZ00DEU, if the consumer configuration allows. |

The census uses the 16 sites of `scripts/clock_level/site_list.json`; better and worse are counted at 1 cm. Of the 16, ten carry the B1I + B3I pair, so a GEC consumer uses BeiDou at ten sites, a single band at two, and none at four (section 2): the census change is read separately for those groups. A failed bar is recorded with its reading and reported; it does not reverse anything silently. Arming BeiDou in the goldens is the author's decision through a ledger row and the generator, never this card's.

## 4a. Amendment 2026-10-18 (before any 2 h result was read)

The startup log of the 2 h GEC run (epochs 1 to 4, no gate read) shows the addStation positive-definiteness guard rejecting WARK00NZL, MAJU00MHL and SCTB00ATA at every epoch ("N_rr_r is not positive-definite ... dropping it this epoch"). The three are the stations of section 2 that track BeiDou on B1I alone (17, 8 and 16 satellites at the first epoch, canonical signals C2I / L2I only, no B3I). Each is dropped as a whole station, GPS and Galileo rows included, so GEC as pre-registered runs on 55 stations and its comparison with GE mixes BeiDou with the loss of three stations.

Added arm **GEC-s** (tags `bds_gecs2`, `bds_gecs6`): GEC plus a RINEX subtraction at those three stations, the band-2 BeiDou types removed (`rapid_arm.py --rinex "strip;sys=C;stations=WARK00NZL,MAJU00MHL,SCTB00ATA;types=?2?"`), so they stay GPS + Galileo stations. It is graded under the same gates H1 to H4 and S1 to S7 beside GEC; GEC keeps every gate as frozen above, and both rows are reported. Gate H3 and the clock-class bars apply to both. This arm is a data subtraction, not a loader key, and does not enter the piece rule.

## 5. Results

### 5.1 Stage 1, 2 h (2026-10-18)

All arms on `gnss_processor_w3base.exe`, run root `D:/GnssProcessorSupport/runs_nom`, tags `_rap_bds_{g2,gp2,ge2,gec2,gecs2}`; the five arms ran concurrently, and four runs of another session shared the box, so wall times are inflated and ordered only roughly.

| gate | reading | verdict |
| --- | --- | --- |
| B0 | `rapid.json` and a flattened diff of `arm.json` against the golden (`gates/bds/B0_armdiff.txt`): GE differs in the output path and the duration only; GEC adds `processing.active_systems` `["GPS","Galileo","BeiDou"]` and `processing.signal_priorities.BeiDou`; G has `["GPS"]` and no Galileo entry; G-probe adds the four package keys at their defaults; GEC-s has the GEC keys and three rewritten RINEX paths. Exe sha256 as in the STATUS block. | PASS |
| H0 | G against G-probe, 240 epochs: products 2afe85364ed90d24 (59,198 lines), closed-loop trace 345f24129e48cd17, epoch status 13a21dec8df94f68, all IDENTICAL. The four Galileo-package keys are inert in a GPS-only run; G stands as the GPS-only arm. | PASS |
| H1 | Every arm exits 0 and emits 240 of 240 epochs; 0 skipped, 0 withheld by the observability gate, the A4 ceiling or the rank gate (UNDER or OVER-specified, 0 and 0). Wall: G 1,016 s, G-probe 1,027 s, GE 1,549 s, GEC 3,160 s, GEC-s 2,281 s. | PASS (all five) |
| H2 | Emitted BeiDou clock `c0` (the `C,C##` lines): GEC median 3.83 m, p99 5.73 m, maximum 14.65 m (C20, epoch 159); GEC-s median 3.82 m, maximum 13.88 m (the same satellite and epoch). None reaches 50 m, none 1 km: the metre class, not failure mode A. The maximum is a declared step: at TOW 263910 a per-signal gap with a slip at DARW00AUS (L2I, 7 epochs, LLI) tripped the clock-anchor test, C20 was withdrawn and the epoch re-solved, and the C20 clock steps by 4.7 m with the discontinuity marked (events `cta_decouple`, `gross_los_emit_disc`, `retro_bump`). | PASS |
| H3 | GPS and Galileo clocks, GEC against GE, the same epochs, the per-epoch rms of the change after removing the per-epoch median: GEC GPS at most 0.128 m, Galileo at most 0.079 m; GEC-s GPS at most 0.043 m, Galileo at most 0.059 m (bar 1.0 m; mode A was 5,283 m and the leak 3,200 and 6,100 times the baseline). Maximum \|`c0`\| GPS 2.14 m, Galileo 4.37 m (GEC) and 1.79 m, 4.44 m (GEC-s); the GE wire reads 1.77 m and 4.42 m. The constellation-common level (the median of the change) moves with GEC by -0.357 m (GPS) and +0.080 m (Galileo) and with GEC-s by -0.018 m and +0.010 m. | PASS (both) |
| H4 | The wire of GEC and GEC-s carries 38 BeiDou satellites per epoch (median 38, minimum 37 for GEC and 38 for GEC-s; the COD MGX SP3 holds 38) each with a clock line, two code-bias lines (C2I, C6I) and two phase-bias lines (L2I, L6I); the S line names the BeiDou pivot (`C:C06`) and the code-bias pin (`cbpin=C:C2I-C6I`). Counters by `C##` satellite over the run (GEC-s): 5,499 prior-residual rows, 609 per-signal resumptions (453 at BSHM00ISR, mostly on L6I), 142 MDB exceedances, 101 T4 arc attributions, 94 retro bumps, 90 gross-LOS emit discontinuities, 76 phase-bias hold generations. The measurement admission rejected one channel at birth, the same GPS channel as GE (BSHM00ISR G17), none of BeiDou; no `GATE` tier event in GEC-s. RAIM (armed, adapt and emit) acts on BeiDou: 312 rows of `network_raim.csv` name a `C##` satellite (188 spiked, 202 refloated; YKRO00CIV 46, KIT300UZB 24, GUAM00GUM 23, MAC100AUS 22, PARK00AUS 21) against 64 GPS and 74 Galileo rows in the same arm and 39 and 41 in GE. | PASS |

**Piece rule.** No arm withheld an epoch and the rank gate never failed, so none of the pieces of section 3 is armed: `network.cb_admission`, `network.rank_gate_tol` (stays 1e-11) and `network.cb_kaula_prior` (stays off). The phase-anchor heal, already armed in the golden, is the one that acted in GEC (below). Failure mode A did not occur at 58 stations: no BeiDou clock above 15 m.

**The three single-band stations (the amendment of section 4a).** In GEC the addStation guard dropped WARK00NZL, MAJU00MHL and SCTB00ATA at all 240 epochs (`n_sta_pd_dropped` 720 = 3.0 per epoch; stations in the solve: median 55, minimum 54). The null eigenvector of a rejected station is 76.0 % ambiguity, 19.5 % slant ionosphere and 3.7 % BeiDou inter-system bias: a satellite seen on B1I alone leaves those three local columns collinear. The loss of three stations made some GPS and Galileo satellites single-station for the phase-bias columns: the drop-and-heal fired on 240 of 240 epochs and soft-anchored 2,598 near-null phase-bias columns (all recovered; 1,837 GPS and 761 Galileo `phase_heal` events), the observed near-null count at the gate was 7 to 21 against a declared 3, and the S-basis completion constrained 293 extra directions on 171 epochs (2,509 skipped to the heal). With the three stations' band-2 BeiDou stripped (GEC-s) none of it occurs: the observed near-null count is 3 at every epoch, the S-basis completion and the heal do nothing, and no station is dropped, as in GE (observed 2, declared 2).

**Consumer, WTZZ00DEU, 3D p50 by hour (hours 1 and 2 only; epochs below a decimetre of 240).**

| arm | systems used by the consumer | hour 1 | hour 2 | below 0.1 m |
| --- | --- | --- | --- | --- |
| G | GPS | 0.113 | 0.023 | 175 |
| GE | GPS, Galileo | 0.058 | 0.011 | 208 |
| GEC | GPS, Galileo, BeiDou | 0.033 | 0.017 | 209 |
| GEC-s | GPS, Galileo, BeiDou | 0.030 | 0.015 | 209 |

G and GE reproduce the first two hours of card GAD's pair (0.113 0.023 and 0.058 0.011 on `gnss_processor_galp2.exe`). The stage 1 verdict is health only; the quality gates are stage 2.

### 5.2 Stage 2, 6 h (2026-10-18)

Four arms on one exe, `_rap_bds_{g6,ge6,gec6,gecs6}`; the consumer replays and the census ran afterwards on the same exe. Four runs of another session shared the box throughout.

| gate | reading | verdict |
| --- | --- | --- |
| S1 | Every arm exits 0 and emits 720 of 720 epochs (G 474 s, GE 1,734 s, GEC 6,646 s, GEC-s 5,226 s wall); 0 skipped, 0 withheld by the observability gate, the A4 ceiling or the rank gate (0 UNDER, 0 OVER-specified). Observed near-null count at the gate (declared dimension 1, 2, 3, 3): G 1 to 1, GE 2 to 2, GEC-s 3 to 3, GEC 3 to 21. H2: BeiDou `c0` maximum 14.65 m (GEC) and 13.88 m (GEC-s), both C20 at epoch 159 (the declared 4.7 m step of section 5.1), median 3.89 and 3.85 m, p99 10.15 and 9.85 m, none above 50 m. H3: GPS and Galileo clocks against GE, the datum-free rms of the change at most 0.128 and 0.121 m (GEC) and 0.054 and 0.083 m (GEC-s); the constellation-common GPS level moves by -0.36 m (GEC, -0.64 m in hour 6) and -0.01 to -0.08 m (GEC-s). H4: 38 BeiDou satellites with clock, two code-bias and two phase-bias lines at every epoch (GEC minimum 37). | PASS (GEC and GEC-s) |
| S2 | WTZZ00DEU, table below: GEC against GE no worse than +0.007 m in any of hours 2 to 6 (hour 4); GEC-s +0.007 m (hour 4). The GPS + Galileo consumer of the GEC wire reads 0.056 0.011 0.021 0.010 0.016 0.011 m (GE 0.058 0.011 0.018 0.010 0.015 0.011), of the GEC-s wire 0.057 0.012 0.024 0.011 0.014 0.009 m. | PASS (GEC and GEC-s) |
| S3 | Census `bdsge` against `bdsg`: hours 2 to 6 paired median -0.0198 m, 10 better, 2 worse (DAV100ATA +0.0605, HARB00ZAF +0.0158); hour 1 -0.2200 m (14 / 2). Median site p50 by hour 0.452 0.112 0.045 0.053 0.039 0.044 -> 0.136 0.054 0.041 0.036 0.040 0.037 m. Card GAD's numbers on `gnss_processor_galp2.exe` (-0.0198 m, 10 / 2, -0.2203 m in hour 1) are reproduced; the 6 h G and GE runs reproduce its pair (WTZZ00DEU 0.113 0.023 0.016 0.020 0.030 0.026 m, 655 epochs below a decimetre; 0.058 0.011 0.018 0.010 0.015 0.011 m, 688). | PASS |
| S4 | GEC-s (`bdsgecs`) against GE: hours 2 to 6 paired median -0.0038 m, 5 better, 0 worse; hour 1 -0.0331 m (10 / 2); no site worse than +0.0042 m. GEC (`bdsgec`) against GE: -0.0025 m, 5 better, 0 worse; hour 1 -0.0162 m (10 / 2); largest worsening +0.0087 m (HARB00ZAF). Median site p50 by hour: GE 0.136 0.054 0.041 0.036 0.040 0.037, GEC 0.096 0.039 0.033 0.036 0.031 0.031, GEC-s 0.092 0.035 0.032 0.032 0.030 0.031 m. Against GPS only: GEC-s -0.0267 m (12 better, 1 worse: HARB00ZAF +0.0120), GEC -0.0243 m (11 / 1: HARB00ZAF +0.0245). | PASS (both) |
| S5 | GPS + Galileo users of the GEC wire (`bdsgecge`) against `bdsge`: hours 2 to 6 median +0.0003 m (DAV100ATA +0.0259, HARB00ZAF +0.0223; none above 0.05 m); of the GEC-s wire (`bdsgecsge`): -0.0002 m (ARHT00ATA +0.0342). GPS-only users of the GEC wire (`bdsgecg`) against `bdsg`: +0.0000 m (ARHT00ATA +0.0463); of the GEC-s wire (`bdsgecsg`): +0.0001 m (ARHT00ATA +0.1216, QUIN00USA +0.0502, both named). | PASS on the median bars, sites named |
| S6 | Epoch-loop wall per epoch (the run log's `Network[timing]` line): 6 h G 457 ms, GE 2,188 ms, GEC 9,008 ms, GEC-s 7,000 ms; 2 h G 3,054, GE 5,546, GEC 11,872, GEC-s 8,853 ms. The load differs between the rows (the five 2 h arms ran together, the 6 h arms overlapped only at the start and the census replays followed), so the ratios are not controlled: GEC-s is 1.6 times GE at 2 h and 3.2 times at 6 h; card GAD read GE at 2.9 times G (mean) under its own load. Read as about 2 to 3 times GE. GEC costs 1.3 times GEC-s (the heal and the S-basis completion run on most epochs). | read |
| S7 | BeiDou wire, datum-free rms of `c0` by hour (GEC-s, 6 h): BeiDou 0.85 1.47 1.52 1.32 1.18 1.07 m, Galileo 0.64 0.41 0.18 0.14 0.12 0.12 m, GPS 0.81 0.80 0.81 0.80 0.81 0.85 m (GEC: BeiDou 0.85 1.54 1.57 1.35 1.21 1.10 m): the deviation from the a priori product, not a truth. 38 satellites per epoch. BeiDou-only consumer of the GEC-s wire at WTZZ00DEU (`_rap_bdsxc_bdsonly_wtzz`, `processing.active_systems` `["BeiDou"]`, 13.4 satellites): 0.154 0.075 0.041 0.047 0.042 0.026 m by hour, 621 of 720 epochs below a decimetre. | read |

**WTZZ00DEU, 3D p50 by hour (m), the consumer on the arm's own systems.**

| arm | systems | h1 | h2 | h3 | h4 | h5 | h6 | below 0.1 m of 720 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| G | GPS | 0.113 | 0.023 | 0.016 | 0.020 | 0.030 | 0.026 | 655 |
| GE | GPS, Galileo | 0.058 | 0.011 | 0.018 | 0.010 | 0.015 | 0.011 | 688 |
| GEC | GPS, Galileo, BeiDou | 0.033 | 0.017 | 0.020 | 0.017 | 0.018 | 0.014 | 689 |
| GEC-s | GPS, Galileo, BeiDou | 0.030 | 0.015 | 0.019 | 0.017 | 0.018 | 0.015 | 689 |

The consumer is better in hour 1 (-0.025 and -0.028 m) and up to 7 mm worse than GE in hours 2 to 6; it is one instrument, and the census is the bar.

**Census, 16 sites, mean of the hourly p50 over hours 2 to 6 (m), by what the site tracks.**

| site | BeiDou at the site | G | GE | GEC | GEC-s |
| --- | --- | --- | --- | --- | --- |
| CHTI | B1I only | 0.054 | 0.043 | 0.043 | 0.042 |
| SCRZ | B1I only | 0.045 | 0.045 | 0.042 | 0.041 |
| ANMG | B1I only | 0.058 | 0.054 | 0.056 | 0.054 |
| QUIN | B1I + B3I | 0.102 | 0.026 | 0.024 | 0.024 |
| HARB | B1I + B3I | 0.023 | 0.039 | 0.048 | 0.035 |
| POHN | B1I + B3I | 0.087 | 0.068 | 0.045 | 0.043 |
| DAV1 | B1I + B3I | 0.069 | 0.129 | 0.069 | 0.058 |
| STPM | B1I + B3I | 0.031 | 0.024 | 0.020 | 0.020 |
| ASCG | B1I + B3I | 0.071 | 0.035 | 0.039 | 0.039 |
| CEDU | B1I + B3I | 0.127 | 0.043 | 0.019 | 0.020 |
| KOKB | B1I + B3I | 0.345 | 0.050 | 0.047 | 0.040 |
| ARHT | B1I + B3I | 0.402 | 0.225 | 0.128 | 0.119 |
| WUH2 | B1I + B3I | 0.091 | 0.046 | 0.025 | 0.026 |
| BADG | none | 0.032 | 0.025 | 0.025 | 0.024 |
| UTQI | none | 0.091 | 0.049 | 0.049 | 0.050 |
| BAKE | none | 0.037 | 0.016 | 0.021 | 0.016 |

By group (GEC-s against GE, hours 2 to 6 paired): the ten sites with the pair -0.0143 m (5 better, 0 worse; hour 1 -0.0726 m), the three with B1I only -0.0004 m, the three without BeiDou -0.0006 m (the wire effect on a site that cannot use BeiDou). The gain sits at the sites that use BeiDou: ARHT00ATA -0.107 m and DAV100ATA -0.071 m (GE had DAV100ATA 0.060 m worse than GPS only; GEC-s brings it to 0.058 m against GPS only's 0.069 m), POHN00FSM -0.025, CEDU00AUS -0.023, WUH200CHN -0.020 m. GEC against GE by group: -0.0120 m (pair), +0.0003 m (B1I only), +0.0004 m (none). Scripts `census_groups.py`, `wire_clock.py`, `wire_sats.py`, `health.py`, `consumer_hours.py` and the grade outputs are in `D:/GnssProcessorSupport/gates/bds/`.

### 5.3 What the read says

- **Failure mode A did not recur.** At 58 stations on the self-contained golden the BeiDou satellite clocks are in the metre class from the first epoch (maximum 14.65 m, none above 50 m, none at kilometre class) and GPS and Galileo move by at most 0.13 m (datum-free). The 305-station W6-off configuration of the 2026-06-29 run is a different regime (W6 off, 305 stations, the older posture): this read does not close mode A for it.
- **Failure mode B did not recur and needed no piece.** The rank gate never withheld; the CB-ADM census and the Kaula prior stay off. Two of the four pieces were already in the golden, and one of them acted: the phase-anchor heal fired on 240 of 240 (2 h) and 551 of 720 (6 h) epochs of GEC and recovered every gate, soft-anchoring 3,323 phase-bias columns, while GEC-s needed neither the heal nor the S-basis completion. With one dual pair, B1I + B3I, tracked by 38 satellites at 16 or more stations each, the bias datum is not under-specified at this scale; the sparsity of failure mode B (many BDS-3 signals, many seen by one or two stations) does not arise from a single pair. Whether it returns at 305 stations or with the other bands is not read.
- **A station that tracks BeiDou on one band is dropped whole.** The addStation positive-definiteness guard rejects such a station (WARK00NZL, MAJU00MHL, SCTB00ATA) at every epoch, its GPS and Galileo rows with it. The null direction is ambiguity, slant ionosphere and the BeiDou inter-system bias (75.5 %, 19.3 % and 3.9 % of the null-vector mass over 6 h): one band leaves those three columns collinear. A roster that adds BeiDou has to remove the lone band at such stations (GEC-s did, by data subtraction) or the producer needs a rule that admits a constellation's rows at a station only with the second band. Owed.
- **BeiDou helps where it is used.** The settled census gain of GEC-s over GE is -0.0038 m overall and -0.0143 m at the ten sites with the pair, with ARHT00ATA and DAV100ATA (the two sites GE left worst) improving by 0.107 and 0.071 m; no site is worse by more than 0.0042 m. Sites without the pair and users of the wire without BeiDou are unchanged at the median.
- **BeiDou product quality now has a first measure.** A BeiDou-only consumer of the wire reaches 4 to 5 cm in hours 3 to 5 and 2.6 cm in hour 6 (621 of 720 epochs below a decimetre), without GPS or Galileo. It is one station and one day; the wire's BeiDou clocks deviate from the a priori product by 1.1 to 1.5 m rms (datum-free), against 0.1 to 0.2 m for Galileo and 0.8 m for GPS, so the BeiDou level is not yet a graded quantity.
- **RAIM acts on BeiDou more than on the others.** 367 rows name a `C##` satellite in GEC-s (243 spiked, 202 refloated) against 103 Galileo and 106 GPS rows, and 82 GPS and 70 Galileo rows in GE; the stations are mainly YKRO00CIV, KIT300UZB, GUAM00GUM, MAC100AUS and PARK00AUS (2 h read). The per-satellite and per-station attribution of the 6 h run is not read here.

## 6. Open

- The single-band rule (section 5.3): data subtraction used here, a producer rule owed; GEC-s is the arm to carry forward.
- Arming BeiDou in the goldens is the author's decision (ledger rows for `processing.active_systems` and `processing.signal_priorities.BeiDou`, the generator, a fresh-convergence baseline pair). The evidence of this card: all gates pass on GEC-s, the gain is at the sites that use BeiDou, the cost is 2 to 3 times GE.
- ARHT00ATA and QUIN00USA worse as GPS-only users of the BeiDou wire (+0.12 and +0.05 m on GEC-s, +0.046 m ARHT00ATA on GEC); the same fragility as card GAD's ARHT00ATA (+0.196 m).
- HARB00ZAF stays worse than GPS only (+0.012 m on GEC-s, +0.025 m on GEC); DAV100ATA is now level with GPS only.
- BeiDou attitude: the golden runs nominal yaw steering for every system (the BeiDou orbit-normal mode at low beta, `processing.enable_orbit_normal_attitude`, is off) and the wind-up is accumulated; the beta angles of the 38 satellites over the window were not read.
- The BeiDou RAIM load and the BSHM00ISR L6I gaps (453 of 609 per-signal resumptions in 2 h) are not attributed.
- The B1C / B2a pair (21 stations) and the BDS-3 B2b signals are not exercised: they need triple or all mode and are a different configuration.
- A controlled cost measurement (one arm per box load) and the 24 h and multi-day behaviour are not read.
- The 305-station and W6-off regimes of the earlier failures are not rerun.

## 7. Re-grade 2026-10-18 (independent recomputation; append only)

Sections 2 and 5 were recomputed from the run directories and the 59 RINEX files with scripts written afresh (`D:/GnssProcessorSupport/gates/bds/regrade/`), `gcensus.py grade` serving as the census cross-check. Agreement, to the printed precision: the consumer hourly p50 and the epochs below a decimetre of every arm and replay (G, GE, GEC, GEC-s, the four wire-user consumers, the BeiDou-only consumer); the paired census medians, better / worse counts, named sites and median-site rows of the nine comparisons of S3 to S5, the per-site table and the group medians of section 5.2; the arm records (each differs from the golden, sha256 `17f59663…`, in the declared keys and the output paths only; the replays differ from their bases in the wire and output paths only; the three stripped RINEX files lose exactly the `?2?` BeiDou types); the exe hash, the build stamp of every run and the base commit of the marker file; the H0 identity hashes; 720 of 720 emitted with 0 withheld and 0 UNDER or OVER-specified on every 6 h arm, and the 240-epoch counterparts; the near-null ranges (1 to 1, 2 to 2, 3 to 21, 3 to 3); the PD census (2,160 = 3 x 720, one record per WARK00NZL, MAJU00MHL and SCTB00ATA per epoch; mass 75.5, 19.3, 3.9 %); the heal and S-basis counters (551 epochs and 3,323 columns, 427 directions on 299 epochs; 2 h 240 epochs, 2,598 columns, 1,837 GPS and 761 Galileo); the BeiDou clock maximum, median and counts above 50 m and 1 km; the GPS and Galileo clock changes against GE; the wire census (38 satellites per epoch, minimum 37 for GEC, two code-bias and two phase-bias lines each); the RAIM rows by constellation and the 2 h event counters of H4; the wall times and the epoch-loop times; the tracking tables of section 2 (station counts, pair satellite-epochs, 50 tracked PRNs, 38 with an orbit, minimum 16 stations) and the pair membership of the 16 census sites.

Differences found:

- **S7, GEC, BeiDou datum-free rms of `c0`, hour 5:** printed 1.21 m, recomputed 1.2048 m, that is 1.20 m (the 3-decimal value 1.205 was rounded twice). The row reads 0.85 1.54 1.57 1.35 1.20 1.10 m.
- **The C20 step (sections 5.1 and 5.2):** the clock step at TOW 263910 (epoch 156 to 157, zero-based) is 4.87 m in GEC (9.012 to 13.882 m) and 4.75 m in GEC-s (8.460 to 13.209 m), not 4.7 m for both arms. The maxima at epoch 159 carry a further 0.77 m (GEC) and 0.67 m (GEC-s) of growth after the step.
- **Census grouping (section 5.2):** ANMG00MYS tracks C1I and C7I only (section 2), so neither band of the configured pair reaches it, yet the section 5.2 table and group medians place it with the B1I-only sites (10 / 3 / 3), while section 4 counts 10 / 2 / 4. Under the 10 / 2 / 4 split (CHTI00NZL and SCRZ00BOL; ANMG00MYS, BADG00RUS, UTQI00USA and BAKE00CAN) GEC-s against GE over hours 2 to 6 reads -0.0020 m for the two single-band sites (GEC -0.0016 m) and -0.0005 m for the four without a usable BeiDou pair (GEC +0.0011 m), no site better or worse by 1 cm; hour 1 reads -0.1440 m for the two (SCRZ00BOL 0.682 to 0.408 m) and -0.0015 m for the four. The 10 / 3 / 3 figures (-0.0004 and -0.0006 m) stand as printed for that grouping; the reading is unchanged.
- **DAV100ATA against GPS only (sections 5.2 and 6):** GEC-s 0.058 m against 0.069 m, -0.0106 m, which the 1 cm count scores as better (it is one of the 12 better sites of S4); "level with GPS only" is read as "marginally better".
- **Wording:** WARK00NZL, MAJU00MHL and SCTB00ATA lack B3I, not every other BeiDou band: all three also track B2I (C7I, L7I), WARK00NZL and SCTB00ATA also B1C and B2a. "B1I alone" and "B1I only" (the STATUS block, section 4a, the section 5.2 table and groups, the `CARDS.md` line) mean B1I without B3I, the only band of the configured pair they reach.
- **Reproduction of card GAD (S3):** the hours 2 to 6 clause (-0.0198 m, 10 / 2, DAV100ATA +0.0605, HARB00ZAF +0.0158), the median-site rows and the consumer pair (0.113 0.023 0.016 0.020 0.030 0.026 m, 655; 0.058 0.011 0.018 0.010 0.015 0.011 m, 688) are identical; hour 1 is -0.2200 m here against -0.2203 m in card GAD (the exe differs), so "reproduced" holds to 0.0003 m there.
- **Definition-level (at most 0.001 m):** the H3 constellation-common shifts quoted for the 2 h runs (GEC GPS -0.357 m, GEC-s GPS -0.018 m) are the hour-1 values; the whole-window mean of the per-epoch median change is -0.358 m and -0.017 m (Galileo +0.080 and +0.010 m both ways). The 6 h p99 values (10.15, 9.85 m) are nearest-rank; linear interpolation gives 10.14 and 9.84 m.

No gate verdict, Outcome statement or standing of this card changes.

## 8. The single-band rule built: second-band admission (2026-10-18; append only)

The rule that sections 5.3 and 6 left owed is built on branch `fix/single-band` (commit `1546d1c`, on a merge of this card into main `8d15451`) behind `network.second_band_admission`, default off, ledger row UNVALIDATED. The keys read by the loader are `network.second_band_admission`, `network.second_band_scope` (`station`, the default, or `satellite`) and `network.second_band_min_sats` (default 1, clamped at 1); the last two are PARAM rows of the first. The rule is not armed in either golden and is not fingerprinted.

### 8.1 The rule

- **A modelled band.** A band is modelled for a satellite when the epoch carries an admitted phase and an admitted code on it. Admitted means a signal of the constellation's priority list that the network coverage gate has not excluded (the candidates the canonical latch draws from). Bands are physical bands (`classifySignal`), so two signals of one carrier count once.
- **Station scope.** A constellation other than the time reference is admitted at a station while at least `network.second_band_min_sats` of its satellites are on two modelled bands there in the epoch. Every satellite of the constellation is then admitted, on one band or two. Failing, the constellation's records at that station are withheld whole and the station keeps its other constellations. **Satellite scope.** Each satellite is tested on its own.
- **Outside the rule.** The time reference constellation (no inter-system bias; withholding it would remove the station's clock reference), a constellation without a priority list, and a record on no modelled band at all (HKSL00HKG spells B1I as C1I / L1I, which the list does not name; such a record forms no row whatever the rule does). The last class is left alone and counted apart (`records_unmodelled`).
- **Position.** Immediately after the canonical-pair admission, on a per-epoch copy of the synced epoch, upstream of the roster, the models and the slip detectors. A withheld record is absent for everything downstream exactly as if the RINEX record were missing, so no state is pruned by hand and the row-level gap re-entry response (wired for every data gap) reads the return of a constellation as a re-entry. A station's latch for the constellation is decided only when the constellation is first admitted, so a station that gains its second band later latches both bands then, instead of keeping a one-band latch for the run. The held-out consumer is not on the producer roster and is never screened. A post-model placement (beside the product-referenced admission) was rejected for this reason and because it would need the state prune that the upstream placement gets for free.
- **Standing invariant 7.** Both edges are events with counters and identities: one `ADMISSION` / `second_band` row in `network_events.csv` and one log line per transition naming the station and the constellation (the satellite under satellite scope), never one per withheld epoch; the memory is `StationState::sba_since` (ColdStart). The run-end report prints the denominators (records screened, units, records left alone), the withheld records, both edge counts, the longest withheld run that came back, the stations affected per constellation, and an "ARMED AND INERT" clause when nothing was withheld.
- **Unarmed.** The seam returns the caller's epoch on its first statement: no copy, no walk, no counter, no entity row.

### 8.2 Checks

- **Unit suite.** `tests/network/test_second_band_admission.cpp`, 28 cases in three suites (`SecondBandPure`, `SecondBandSeam`, `SecondBandConfig`): the candidate table and the band count (code and phase both required, one carrier counted once, a gated signal removed); the unit state machine (one edge per transition, none while the state holds); the seam under both scopes (the single-band station loses BeiDou and keeps GPS, a single-band satellite among dual-band peers is kept, the threshold, the return when the second band appears, the memory across an absent constellation, a record on no modelled band); the exemptions (time reference, no priority list, the held-out consumer, unarmed identity); the run-end report; the loader. Debug suite 3385 cases: 3355 passed, 30 fixture skips, 0 failed. `nom_epoch_lint` and `nom_n4_verify --whole` pass; `ledger_render --check`, `make_golden_config --verify` and `config_posture_check --verify-loader` pass.
- **Same-build check, key off.** Field golden with the section 3 BeiDou keys, 12 epochs, exe `gnss_processor_sband2.exe` (sha256 `1183ce58…`) against `gnss_processor_sband_ref.exe` (sha256 `5fc9c713…`, built from the parent commit `a54155a`): the product file and every trace that carries no wall-time column are byte-identical (58 of the 68 files; `network_datum_graph.csv`, `rt_engine.log` and `arm.json` are equal once the wall-time column, the thread order and the run path are removed; the other seven are wall times, tags and the lines named next). `main_log.log` and `stdout.txt` differ in wall times and in one declared place: the run-end entity census reads 110 declared fields (109 before) and gains the line `station.sba_since (was ) ColdStart rows max 0 final 0`. The manifest differs in the build stamp, config path and hash, product path, time and loop wall only. Runs `_rap_sband_off_new2` and `_rap_sband_off_ref` under `D:/GnssProcessorSupport/runs_nom`; the comparison tool is `D:/GnssProcessorSupport/gates/sband/cmp_runs.py`.
- **Smoke, key on (12 epochs).** `n_sta_pd_dropped` 36 (3.0 per epoch) to 0, stations in the solve 55 to 58. Station scope withholds BeiDou at WARK00NZL (17 satellites, none on two modelled bands), MAJU00MHL (8) and SCTB00ATA (16) for the run, and at MAC100AUS (12, none on two bands) for its first epoch only, re-admitted at the second with 5 of 12 on two bands. HKSL00HKG's 15 BeiDou satellites carry no modelled band and are left alone (204 records in 13 epochs). Runs `_rap_sband_on_st2`, `_rap_sband_on_sat2`.

### 8.3 Development arms, 2 h, one exe

Three arms on `gnss_processor_sband2.exe`, the section 4a design: GEC (the three stations dropped by the guard), GEC-s (their band-2 BeiDou subtracted from the data), and GEC with the rule on (station scope, threshold 1). A fourth arm runs the satellite scope. Runs `_rap_sband_{gec2,gecs2,gecr2,gecsat2}`. Wall times 1343, 1298, 1300 and 1368 s.

| quantity | GEC | GEC-s | GEC + rule (station) | GEC + rule (satellite) |
| --- | --- | --- | --- | --- |
| `n_sta_pd_dropped` (240 epochs) | 720 | 0 | 0 | 0 |
| stations in the solve (min / median) | 54 / 55 | 57 / 58 | 57 / 58 | 57 / 58 |
| observed near-null count at the gate | 7 to 21 | 3 | 3 | 3 |
| phase-anchor heal fired | 240 epochs, 2,598 columns | none | none | none |
| S-basis completion, extra directions | 293 on 171 epochs | 0 | 0 | 0 |
| consumer WTZZ00DEU 3D p50, hours 1 and 2 (m) | 0.0333 0.0169 | 0.0305 0.0146 | 0.0299 0.0146 | 0.0282 0.0155 |
| epochs below a decimetre of 240 | 209 | 209 | 207 | 207 |
| maximum BeiDou `c0` (m) | 14.65 | 13.88 | 13.88 | not read |
| rule edges (withhold / readmit) | none | none | 4 / 1 | 1,698 / 1,413 |

- The rule reproduces the subtraction. Station scope names the same three stations plus MAC100AUS at its first epoch (4 withhold edges, 1 readmit), and every gate quantity above matches GEC-s: no station is dropped, the near-null count is the declared 3 at every epoch, and the heal and the S-basis completion do nothing. The per-epoch datum-free clock difference between the rule arm and GEC-s is at most 5 mm (median 0.1 mm) for GPS, 35 mm (median 2.3 mm) for Galileo and 77 mm (median 1.2 mm) for BeiDou; the same difference between GEC and either of them reaches 134 mm for GPS (median 49 mm). The consumer p50 differs from GEC-s by 0.6 mm in hour 1 and 0 in hour 2 (two epochs fewer below a decimetre); both are 3 mm better than GEC in hour 1. This is two hours: it separates the structural gates, not the settled consumer.
- The two arms are not byte-identical, as expected: in GEC-s the three stations still hold BeiDou records that reach the models (with no modelled signal, so no row), and in GEC + rule they are absent upstream.
- **Satellite scope is much heavier and is not the candidate.** It withholds 20,536 record-epochs (6.3 % of those screened) at 50 stations in each of BeiDou and Galileo (single-band satellites among dual-band peers, which station scope keeps), with 1,698 withhold and 1,413 readmit edges in 240 epochs, the longest run withheld 200 epochs, and 2,911 edge log lines past the 200-line limit. The settled effect on the consumer is unread; a band that flickers at a dual-band station costs the satellite's other band its arc each time, and the rule has no hysteresis (section 8.4).
- Cost: none measurable (wall 1300 s against 1298 s and 1343 s on a shared box); the seam copies the epoch on every epoch that withholds something.

### 8.4 Deferrals, triaged (standing invariant 3)

- **Satellite scope has no hysteresis.** Field-blocking for any arm that uses it (section 8.3); inert for the default scope. A tolerance in epochs, as the canonical-pair admission has, is the form it would take.
- **The withheld memory is not snapshotted.** Field-inert: the verdict is recomputed from the data every epoch, so a warm resume differs only by repeating one decoupling edge per withheld unit. A resume of a snapshot written without the rule into a run with it has not been exercised.
- **Threshold 1.** One dual-band satellite is taken to identify the inter-system bias. The 2 h arms hold (no PD drop, near-null 3), but the smallest number of dual-band satellites a station held over the run is not read. Field-inert here, to be read on the 6 h arm before any arming.
- **Constellations other than BeiDou.** The rule screens Galileo as well; at station scope no Galileo station was withheld in 2 h (every station holds at least one E1 + E5a satellite). A Galileo single-band station (none in this window) would be withheld the same way.
- **Not run:** 6 h, the 16-site census, the 305-station regime, a second day, and the warm resume.

### 8.5 Open

- The 6 h arm pair on one exe (GEC-s and GEC + rule) with the census, then the arming decision and the generator row (the author's decision, section 6).
- A tolerance for satellite scope, if it is wanted at all.

## 9. The second-band admission on 6 h arms and the key-off identity gate (2026-10-19; append only)

Section 8.5 left the 6 h pair with the census, and section 8.4 the threshold-1 read. Both are read here, with the identity gate of the key-off path. Exe `gnss_processor_sband2.exe` (sha256 `1183ce58…`), byte-identical to the Release build of branch `fix/single-band` at `1546d1c` (`a73df8b` adds this card only); the gate copy `gnss_processor_w3sband.exe` has the same hash. Arms `_rap_sband_{ge6,off6,on6}` (6 h) were made with `rapid_arm.py make` from the `arm.json` of `_rap_bds_ge6` and `_rap_bds_gec6`; a flattened diff shows the output path only for GE and rule-off, and the output path plus `network.second_band_admission` true for rule-on. The three arms ran together with the gate on a shared box, so wall times are not controlled. Census keys `sbandge`, `sbandoff`, `sbandon` (replays on the same exe, each with the wire's own arm as consumer base). Readouts and scripts: `D:/GnssProcessorSupport/gates/sband/`; gate sides `w3sband` and `w3sbandref` under `gates/g5` and `gates/g7`.

### 9.1 Key off: identity gate (G5, 23 arms and the snapshot bytes)

- **Against the pre-merge side `w3base` (main `30ca49e`).** Products and every trace are IDENTICAL in all 23 arms; the manifest (one `param_responses` record) and the whole logs differ in all 23. The parent-commit side (`w3sbandref`, exe `gnss_processor_sband_ref.exe`, sha256 `5fc9c713…`, built from `a54155a`, which carries the three small defects of [NSD](NSD_network_small_defects_2026-10-17.md) merged in main `8d15451`) differs from `w3base` in the same three files in the same 23 arms: those lines are NSD's. The reference for the rule is the parent side.
- **Against the parent side.** Products, every trace, the manifest and `rt_engine.log` are IDENTICAL in all 23 arms. `stdout.txt` and the `main_log` set differ in 48 to 56 lines per arm, all of them the run-end entity census (the count 109 to 110 and the added `station.sba_since` row, which shifts the rows below it). With those two lines reversed on the candidate's text outputs (`subs_w3sband.sed`, the mechanism of the earlier gates) every arm and the snapshot bytes of `snapw` read IDENTICAL: 24 IDENTICAL in `gates/g7/summary_w3sband.txt` (the raw and the `w3base` summaries are kept beside it).
- **6 h.** `_rap_sband_off6` against `_rap_bds_gec6` (the `w3base` exe, section 5.2): products, closed-loop trace, solution, events and epoch status byte-identical. `_rap_sband_ge6` against `_rap_bds_ge6`: products identical, consumer by hour and census (`sbandge` against `bdsge`) identical. The 6 h `w3base` arms are therefore same-build for the rule-off exe, and GEC-s and the census keys `bdsge`, `bdsgecs` stand as comparators.
- **User side and real time** are not run: the seam has one call site, in the network epoch driver, so neither `Application::run` nor the real-time engine reaches it.

### 9.2 Key on, 6 h

| quantity | GE | GEC, rule off | GEC, rule on | GEC-s (section 5.2) |
| --- | --- | --- | --- | --- |
| exit, wall (s) | 0, 3,223 | 0, 7,314 | 0, 6,449 | 0, 5,226 |
| epochs emitted of 720 (withheld by a gate) | 720 (0) | 720 (0) | 720 (0) | 720 (0) |
| stations in the solve, min / median | 57 / 58 | 54 / 55 | 57 / 58 | 57 / 58 |
| addStation rejections (`n_sta_pd_dropped`) | 0 | 2,160 | 0 | 0 |
| observed near-null count at the gate, min to max (declared) | 2 to 2 (2) | 3 to 21 (3) | 3 to 3 (3) | 3 to 3 (3) |
| S-basis completion, directions on epochs | 0 | 427 on 299 | 0 | 0 |
| phase-anchor heal, columns on epochs | 0 | 3,323 on 551 | 0 | 0 |
| epoch-loop ms per epoch (shared box) | 4,256 | 9,944 | 8,711 | 7,000 |
| rule edges, withhold / readmit | none | none | 5 / 2 | none |

- Rule-off reproduces plain GEC counter for counter (section 5.2); rule-on reproduces GEC-s: no station dropped, the declared near-null count at every epoch, the heal and the completion silent.
- **What the rule withheld.** BeiDou at five stations, 30,113 records in 2,165 of 78,587 station-constellation-epochs (721 epochs screened, 720 emitted): WARK00NZL, MAJU00MHL and SCTB00ATA for the whole run (no satellite on the pair), MAC100AUS at TOW 259230 (12 satellites, back next epoch with 5 on the pair) and BSHM00ISR at TOW 279540 (15 satellites, none on the pair; back at 279570 with 2). The 12,488 records of HKSL00HKG (B1I spelled C1I / L1I) are left alone. Both edges are event rows and log lines; the BSHM00ISR return is answered by the gap re-entry response (`reentry_relevel` and `arc_resume` rows at TOW 279570).
- **Wire.** Datum-free per-epoch rms of the change in satellite clock `c0`, rule-on against the GEC-s wire: GPS at most 5 mm, Galileo 35 mm, BeiDou 77 mm (medians 0.1, 1.6, 0.5 mm), the maxima in the first four epochs (the MAC100AUS withhold, decaying); the BSHM00ISR edge moves BeiDou by 0.5 to 1.2 mm. BeiDou `c0` maximum 13.88 m (C20, epoch 159), median 3.84 m, none above 50 m, as GEC-s. Against the rule-off wire the same measure reaches 0.13 m (GPS and BeiDou) and the GPS common level is 0.33 to 0.55 m apart.

**WTZZ00DEU, 3D p50 by hour (m), the consumer on the arm's own systems.**

| arm | h1 | h2 | h3 | h4 | h5 | h6 | below 0.1 m of 720 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GE | 0.058 | 0.011 | 0.018 | 0.010 | 0.015 | 0.011 | 688 |
| GEC, rule off | 0.033 | 0.017 | 0.020 | 0.017 | 0.018 | 0.014 | 689 |
| GEC, rule on | 0.030 | 0.015 | 0.019 | 0.017 | 0.018 | 0.015 | 687 |
| GEC-s | 0.030 | 0.015 | 0.019 | 0.017 | 0.018 | 0.015 | 689 |

Rule-on minus GEC-s: -0.6, 0.0, -0.1, -0.1, 0.0, 0.0 mm by hour; against GE at most +6.8 mm in hours 2 to 6 (hour 4), the shape of section 5.2.

### 9.3 Census, 16 sites

Median site p50 by hour (m): GPS only 0.452 0.112 0.045 0.053 0.039 0.044; GE 0.136 0.054 0.041 0.036 0.040 0.037; GEC rule off 0.096 0.039 0.033 0.036 0.031 0.031; GEC rule on 0.089 0.036 0.032 0.032 0.030 0.031; GEC-s 0.092 0.035 0.032 0.032 0.030 0.031.

| paired change, arm against base | hours 2 to 6, median (m) | better / worse at 1 cm | hour 1, median (m) | better / worse |
| --- | --- | --- | --- | --- |
| rule on against rule off | -0.0010 | 2 / 0 | -0.0103 | 8 / 0 |
| rule on against GE (`sbandge`, equal to `bdsge`) | -0.0035 | 5 / 0 | -0.0346 | 11 / 1 |
| rule off against GE | -0.0025 | 5 / 0 | -0.0162 | 10 / 2 |
| GEC-s against GE (section 5.2) | -0.0038 | 5 / 0 | -0.0331 | 10 / 2 |
| rule on against GPS only (`bdsg`) | -0.0267 | 11 / 1 | -0.3068 | 15 / 1 |
| rule off against GPS only | -0.0243 | 11 / 1 | -0.2948 | 15 / 1 |
| rule on against the GEC-s census (`bdsgecs`) | 0.0000 | 0 / 0 | -0.0008 | 2 / 1 |

- By group (the 10 / 3 / 3 grouping of section 5.2, rule on against GE): the ten sites with the pair -0.0143 m (5 / 0; hour 1 -0.0710 m), the three with B1I only -0.0007 m, the three without BeiDou -0.0005 m. GEC-s reads -0.0143, -0.0004, -0.0006 m, rule off -0.0120, +0.0003, +0.0004 m.
- No site is worse than GE by 1 cm. The changes above 5 cm are ARHT00ATA -0.112 m and DAV100ATA -0.067 m (GEC-s -0.107 and -0.071; rule off -0.097 and -0.060). Against GPS only the one worse site is HARB00ZAF +0.0124 m (GEC-s +0.0120, rule off +0.0245).
- Rule on against GEC-s, per site: largest change in hours 2 to 6 ARHT00ATA -0.0056 m and DAV100ATA +0.0045 m, none above 6 mm; hour 1 ASCG00SHN -0.028 m and CHTI00NZL -0.025 m (ignition hour). Not run: the GPS + Galileo and GPS-only users of the rule-on wire (the S5 replays of section 5.2).

### 9.4 Threshold 1, read (the deferral of section 8.4)

Per station, the count of BeiDou satellites per epoch with phase and code on both C2I / L2I and C6I / L6I, from the 58 RINEX files over the 6 h window (`dualcount.py`; the raw file, not the admission's verdict). 52 stations track BeiDou; four never reach the pair (WARK00NZL, MAJU00MHL, SCTB00ATA withheld whole, HKSL00HKG left alone). Two stations have an epoch with BeiDou tracked and none on the pair: BSHM00ISR and MAC100AUS, one epoch each, exactly the two one-epoch withholds. Lowest minimum of the count at the other stations: 1 at BSHM00ISR (2 epochs; 2 satellites in 6 epochs), then 3 at SAVO00BRA, 4 at BOR100POL, MRC100USA and POVE00BRA, 5 at GAMB00PYF and MAC100AUS. With threshold 1 the addStation guard rejected no station the rule admitted (0 in 720 epochs), BSHM00ISR with one or two pair satellites included; by the raw count, threshold 3 would differ from threshold 1 at BSHM00ISR alone (9 epochs). The threshold stays 1; the read covers this network and this day.

### 9.5 Verdict and open

- Identity with the key off: PASS (24 IDENTICAL against the parent side after the two declared census lines; products and traces identical in every arm against both references). Grade: ran. With the key on at station scope the rule reproduces the subtraction GEC-s in every quantity read (counters, wire, consumer, census); GEC no longer drops stations and the heal does not fire. The ledger row stays UNVALIDATED and the goldens are not changed; arming is the author's decision.
- Not run: the 305-station regime, a second day, the warm resume, the satellite-scope hysteresis, the users-of-the-wire census, and a same-exe 6 h GEC-s (the comparators are `w3base` runs; the rule-off exe reproduces its sibling arms byte for byte, section 9.1). Cost: epoch loop 8.7 s per epoch rule on, 9.9 s rule off and 4.3 s GE (2.0 times GE) on the same shared box, uncontrolled.

## 10. Independent check, recorded in card BDSB (2026-10-19; append only)

This file is at the 60 KB bar, so the independent repeat of the identity gate and of the 6 h grade of section 9, from fresh arms, is recorded in [BDSB](BDSB_beidou_continued_2026-10-19.md). Key off: 24 IDENTICAL against the parent side (products and traces identical against the parent side and against `w3base` in all 23 arms). Key on at station scope: no station rejected, the heal and the S-basis completion silent, the consumer within 0.6 mm of GEC-s, the census median change against GEC-s 0.0000 m with no site above 6 mm. The ledger row stays UNVALIDATED.
