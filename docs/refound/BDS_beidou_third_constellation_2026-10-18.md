# BDS: BeiDou as the third constellation of the field golden

**STATUS**

- **id:** BDS
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-10-18, no run read yet. First GPS + Galileo + BeiDou network run on the field golden (DOY 129 2024, 58 stations, held-out consumer WTZZ00DEU), graded against GPS only and GPS + Galileo on one exe. BeiDou signals chosen from the roster's RINEX: B1I + B3I (C2I / C6I / L2I / L6I), the pair the loader's dual mode names for BeiDou. Arms G, GE, GEC; a BeiDou-required piece is armed only if the 2 h GEC run shows the failure it answers. Nothing is armed by this card.
- **source:** the author's request of 2026-10-18 ("perhaps we should do a GPS+Galileo+BeiDou run to compare against GPS-only and GPS+Galileo"); [GAD](GAD_galileo_adoption_2026-10-13.md) (how Galileo was added and graded); [SPEC_beidou_w6_datum.md](../SPEC_beidou_w6_datum.md) and [SPEC_cb_admission_card.md](../SPEC_cb_admission_card.md) (the four-piece emit path, failure mode B)
- **knobs / posture:** `processing.active_systems` `["GPS","Galileo","BeiDou"]`, `processing.signal_priorities.BeiDou` `["C2I","L2I","C6I","L6I"]`; candidate pieces, each only on the evidence of section 3: `network.cb_admission`, `network.rank_gate_tol` (placement), `network.cb_kaula_prior`; `network.phase_datum_prioritized` with `network.phase_datum_min_stations` 2 and `network.rank_gate_tol` 1e-11 are already in the goldens
- **dates in the text:** 2026-10-18
- **hashes / run dirs:** exe `gnss_processor_w3base.exe` (main `30ca49e`, sha256 `50ca2ea5fbbc540cbb2b41a0b208313e0b88cf0f60ae134e0230f778d209d285`) for every run; run root `D:/GnssProcessorSupport/runs_nom` (`RAPID_RUNS_ROOT`), arms `_rap_bds_*`; scratch `D:/GnssProcessorSupport/gates/bds/`
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registered; no result yet. Question: does a third constellation, BeiDou on B1I and B3I, run healthy on the self-contained field golden, and what does it change for the held-out consumer and the 16-site census against GPS + Galileo and against GPS only? The answer is written below the pre-registration, dated.

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

Not yet run.
