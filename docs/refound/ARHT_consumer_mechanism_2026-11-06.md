# ARHT: the mechanism of the ARHT00ATA consumer's response to the wire

**STATUS**

- **id:** ARHT
- **topic:** consumer
- **status:** closed
- **summary:** Closed 2026-11-06. ARHT00ATA's responses to the wire are one mechanism at two magnitudes, named from reads of the runs on disk and consumer-only replays of existing wires (no producer run): the wire declares a satellite's level steps and clock motion only through its held phase biases, so a consumer phase row whose signal has no phase bias on the wire gets no declaration, keeps its ambiguity across a step and sees the emitted clock's datum motion that the hold removes from the covered rows. The site latches the L1W family (a Javad receiver under the golden's `C1W`-first priority) under the one sky no L1W roster station covers (none of the 18 south of 45 S), so 60 % of its L1W rows run uncovered against 0 to 6 % at the five other L1W census sites. Metre scale: G01's leaked return record (about 100 m, one wire epoch) meets such a row on DOY 128 and 131; deleting those seven records, or G01 at the consumer for ten minutes, removes both divergences, reproduced to the printed digit on the current exe, while deleting G09 in the same window does not. Decimetre scale: the L1C + C1C family takes the site from 0.231 to 0.017 m (GPS only 0.593 to 0.058 m), the election's +40.6 mm on DOY 130 becomes +0.9 mm, single-satellite exclusions move it by at most 25 mm, and the common datum term the uncovered rows see drifts by 1.2 m (standard deviation) on the DOY 129 wire. The ARHT00ATA wire gain of posture C is the per-signal wire (D6 -208.5 mm, C6 -214.6 mm), not BeiDou. Nothing armed; the consequences for course A, PIV and the COD-regime candidate and three remedy candidates are in section 8.
- **source:** the request of 2026-11-06 (name the mechanism behind the ARHT00ATA responses before BeiDou course A, PIV and the COD-regime candidate are decided); cards [MDR](MDR_multiday_replay_2026-10-05.md) sections 2 to 4, [PIV](PIV_pivot_election_coverage_2026-10-17.md) sections 8 to 11, [NSP](NSP_census_neutral_spread_2026-10-29.md), [BDSB](BDSB_beidou_continued_2026-10-19.md) sections 4 and 5, [BDG](BDG_beidou_third_golden_2026-11-02.md) section 4, [NOM4](NOM4_network_object_model_finish_2026-10-15.md) with `scripts/nom3_designs/N3b_decisions.md`, [LAH](LAH_level_aware_hold_2026-09-29.md) section 9, [NBP](NBP_native_broadcast_clock_posture_2026-10-07.md) section 12, [MDAY](MDAY_multiday_candidate_2026-10-19.md) sections 6 and 9, [OTL](OTL_ocean_tide_loading_source_2026-10-16.md), [SNXB](SNXB_sinex_roster_arms_2026-10-19.md), [MSC](MSC_multi_site_consumer_2026-09-21.md), [PBF](PBF_pb_hold_presence_edges_2026-09-28.md), [NIS](NIS_consumer_innovation_screen_2026-10-17.md), [GAD](GAD_galileo_adoption_2026-10-13.md), [BOF](BOF_built_off_follow_through_2026-10-28.md)
- **knobs / posture:** none changed, nothing armed, no producer run. Consumer-only replays of existing wires: the baseline wire `runs/_rf0hv17_field6` (DOY 129), card MDR's DOY 131 and DOY 128 wires, card BDSB's DOY 131 C and GE wires, card BOF's DOY 130 control and election wires; the replays change only the consumer's RINEX (a satellite or a signal family blanked), the wire copy (one epoch's records removed) or `processing.active_systems`
- **code:** read, not changed: `src/core/realtime/RealTimeEngineSsrBreaks.cpp` (the presence and freshness flips, the hold generation, `reinflateOnClockDisc`), `RealTimeEnginePrepare.cpp` (`seedClockFromWls`), `src/core/filter/KalmanPostFitQc.cpp` (the post-fit peel), `src/core/network/emit/PbHold.cpp` (the phase-clock hold)
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/arht/x64-Release/gnss_processor_arht.exe`, a byte copy of the baseline pair's `gnss_processor_bofarm.exe` (sha256[:16] `91143c81de9a9547`); runs root `D:/GnssProcessorSupport/runs_nom`, tags `_rap_arhtm_*`; driver, bases, edited wires, scripts and tables under `D:/GnssProcessorSupport/gates/arht/`; wires (sha256[:16]): `C:/GnssProcessor/runs/_rf0hv17_field6/products.txt` `bf16e9474305e2de`, `D:/GnssProcessorSupport/runs/_rap_mday131_f6` `483049ef17142259`, `_rap_mday128p_f6` `fe28310651468a27`, `runs_nom/_rap_mday_c_131` `6e4020a49d30a0e7`, `_rap_mday_ge_131` `6dbe02e17f822e4d`, `_rap_bof_ctl130_f6` `eb02f00f8c87c59c`, `_rap_bof_piv130_f6` `c2ec5b195172f3f2`; edited wires `gates/arht/wires/wire131_noG01ret.txt` `cd58a8ecb763df92`, `wire128_noG01ret.txt` `01386db0733cf2a7`, `wireC131_noG01ret.txt` `ca81387c731ae0d3`; branch `arht/mechanism` from main `284ec29`
- **gates (frozen at pre-registration):** the predictions of section 4, written before any arm of section 6 ran

**Outcome.** Question: what makes the consumer at ARHT00ATA respond to the wire by centimetres where no other census site moves, and by 100 to 190 m on two days, and is it one mechanism? One mechanism at two magnitudes: a phase row whose signal has no phase bias on the wire gets none of the wire's declarations and sees the clock motion the held biases hide, and the site is the only census site that latches the L1W family under a sky the 18 L1W roster stations never see (60 % of its L1W rows uncovered). Tests that could have failed it: deleting G01's seven wire records at one epoch removes both divergences while a control satellite does not; the covered family takes the site from 0.231 to 0.017 m and the election's +40.6 mm to +0.9 mm. The BeiDou gain at the site is the per-signal wire. Nothing armed; course A, PIV and the COD candidate re-read in section 8.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

ARHT00ATA (Arrival Heights, Ross Island) is the deciding site of several held decisions: card BDG reopens BeiDou on
the primary golden (course A) when the mechanism of the ARHT00ATA wire gain is named and the G01 epoch is off the
wire; card PIV's coverage election fails one site clause on two of five days at this site alone; the COD-regime
candidate of card NBP section 12 is on file because it prevents the site's divergence. No card names the mechanism.
The question of this card: what makes the consumer at ARHT00ATA respond to changes of the wire by centimetres where
the other 15 census sites move by at most a few millimetres (card NSP's neutral floor is 0.19 mm), and by 100 to
190 m on two of five days? Is it one mechanism or several, and what about the site makes it the outlier?

The method, in the order the brief fixed: one consolidated evidence table (section 2), the site (section 3), the
competing hypotheses with what each predicts (section 4), reads of the runs already on disk (section 5), and then
consumer replays of existing wires with single-satellite exclusions (section 6). No producer run was made. Runs on
D: were used where they still exist; the two wires of card MDR (`_rap_mday131_f6`, `_rap_mday128p_f6`) are on
`D:/GnssProcessorSupport/runs/` and their census arms still point at the deleted C: copy, so every replay base here
carries the D: path.

## 2. The evidence, consolidated

Every number is the hours-2-to-6 mean of the hourly 3D p50 unless the row says otherwise; "change" is the site's
change between two wires, paired.

| # | card, date | day, posture | what ARHT00ATA did | size |
|---|---|---|---|---|
| 1 | LAH sections 4, 8, 9 (2026-09-29) | DOY 129, GPS only, 6 h | the phase-clock hold regresses the site in every settled hour (0.07 to 0.55, 0.03 to 0.69, 0.02 to 0.21, 0.03 to 0.24, 0.03 to 0.18 m); on the L1C + C1C family the regression disappears (0.17 0.04 0.02 0.03 0.02 m); C1C code on an L1W phase keeps it | +0.4 m |
| 2 | EXL section 4 (2026-10-01) | DOY 129, GPS only, 6 h, the armed hold | census at its limit: ARHT00ATA +0.404 m, KOKB00USA +0.222, QUIN00USA +0.041 | +0.404 m |
| 3 | PBF section 1 (2026-09-28) | DOY 129, the CLV wire, 2 h | a held phase bias appearing on a row in use (G32 L1W, 92.4 m) resets innocent satellites; hour 1 / 2 p50 478.953 / 803.907 m; 14 of 90 census keys above 400 m; the presence flip (armed) leaves 2.278 / 0.762 m | 400 to 800 m |
| 4 | MDR sections 2 to 4 (2026-10-05) | DOY 128 to 132, GPS-only golden `3959f24`, 6 h | worst census site on four of five days (0.84, 4.50, 0.44, 0.61 m); DOY 131 diverges to 166.22 m; DOY 128 with DOY 127's products improves to 1.98 0.59 0.27 0.28 m in hours 1 to 4 and jumps to 99.7 m at 03:36 UT (54.1 and 28.4 m in hours 5 and 6) | 100 to 166 m |
| 5 | NBP sections 10, 12 (2026-10-07) | DOY 131 | the self-contained regime on the COD a priori (q 5e-6, card SMH's hold, B1, B2, X2b) removes the divergence (c minus g -33.3 m); the native broadcast posture does not diverge either | -33 m |
| 6 | GAD, the GPS-only user census (2026-10-13) | DOY 129, GPS-only users of the Galileo package wire | ARHT00ATA +0.196 m (hours 3 and 4 1.227 / 0.565 m against 0.804 / 0.216), named; every other site within 0.016 m | +0.196 m |
| 7 | NOM4 and `scripts/nom3_designs/N3b_decisions.md` (2026-10-15) | DOY 129, 6 h census | D6 (one wire code bias per modelled signal) -208.5 mm, 14 of 16 sites unchanged to 0.1 mm: "the wire now carries a bias for the signal that consumer tracks"; D1 -64.8 mm; D7 -32.2 mm | -208.5 mm |
| 8 | OTL sections 4, 8 (2026-10-16) | DOY 129, 5 h and 24 h | the site has no SIF block and runs at zero loading; the producer's loading reaches it through the wire alone (+8.7 mm in 5 h, +2.59 mm over 24 h, consumer-only replay 0.0) | +8.7 mm |
| 9 | BDS section 5, BDSB sections 4.4 to 4.7 (2026-10-18 and 10-31) | DOY 129, 6 h; DOY 128 to 132, 24 h | as a BeiDou user of the BeiDou wire -0.107 to -0.112 m; as a GPS + Galileo user of arm C6 -0.2146 m (0.044 0.020 0.021 0.017 0.013 0.009 against 0.301 0.374 0.210 0.194 0.129 0.246 m), as a GPS-only user -0.5406 m; as a GPS-only user of the earlier BeiDou wire +0.12 m (GEC-s) and +0.046 m (GEC); as a GPS-only user of the C wire +17.27 m on DOY 128 (189 m at 03:36:30) and +14.91 m on DOY 131 (176 m at 03:23:00), each starting the epoch after both wires carry G01 about 100 m from the product; the same consumer survives on the GE wire | -0.21 m, -0.54 m, +17 m |
| 10 | MDAY sections 5.4, 6.2, 9 (2026-10-19) | DOY 128 to 132, GE and C, 24 h | 0.08 to 0.13 m better under C on four days; a 53-epoch excursion to 2.2 m on DOY 130 under GE (0.020 m under C); G01 is the worst in-solve satellite for 2,232 and 2,253 epochs from 03:36:00 (DOY 128) and 03:22:30 (DOY 131), withheld from the wire by `gross_los_emit_withhold` in all but three epochs a day, at -82 to -111 m | 3 leaked epochs a day |
| 11 | PIV sections 8.6, 8.7, 9, 11 (2026-10-18 to 10-30) | DOY 128 to 132, 6 h; DOY 129, 24 h | the election: -5.8 (129), -6.9 (129, hours 2 to 24), +40.6 (130), +7.6 (128), +3.0 (131), +25.9 mm (132), the worsening in the last hours of each window; the sum-to-zero datum +59.2 mm; under the election G01 returns within 0.6 m where the legacy pivot path emits it about 100 m off, and the site's G01 code innovation in the ten minutes after the return is -1.46 to +0.33 m against -5.33 to +0.73 m with three ambiguity resets | +40.6 mm |
| 12 | BOF section 2 (2026-10-28) | DOY 129, 6 h | `network.cycle_slip.gf_arc_jump` -16.8 mm; "ARHT00ATA decides every no-site-worse clause" | -16.8 mm |
| 13 | NSP section 9 (2026-10-30) | DOY 129 and 130, six re-rounded rosters each | neutral spread s 0.11 mm (130) and 0.014 mm (129); the +40.6 mm is 90 times the 99 % bound of a neutral change: a response of the site, not noise | 0.11 mm |
| 14 | SNXB sections 10, 12 (2026-10-19 and 10-31) | DOY 129, 6 h | the roster at its SINEX coordinates: +2.4 mm (the 10-19 wire), +7.4 mm (S2c), +11.2 mm (S3); MIZU00JPN restored at the held coordinate -22.3 mm; "the two Antarctic sites swing with the wire and not with the correctness of the coordinate" | +7.4 to +11.2 mm |
| 15 | NIS sections 9, 11 (2026-10-17 and 10-31) | DOY 129, the CLV and baseline wires | the knob-off replay of the 2026-10-17 exe no longer diverges on the CLV wire (the 92 m case not reproducible); injected phase-bias steps on this site do not bite at K 1 to 3 because its stepped GPS rows carry no L1W phase bias, injected clock steps move it 40.6 to 407.8 m | |
| 16 | MSC section 3 (2026-09-21) | the census design, seven wires | ARHT00ATA selected as a Javad L1W site, 3.1 km from the roster station SCTB00ATA; its count of epochs below a decimetre swings between 0 and 460 of 720 across the seven wires, the widest spread of the census | |

Two populations stand out before any new run: the metre-class events (rows 3, 4, 9, 10, 11: a satellite's level
step on the wire reaching the consumer), and the standing decimetre error with its centimetre responses (rows 1, 2,
6, 7, 8, 11 to 14), which every wire change moves and which two per-signal wires (D6, C6) take to the census level.

## 3. The site

- **Receiver and antenna.** JAVAD TRE_3 DELTA 4.3.00 on a JAVRINGANT_DM SCIS, marker 66001M005, ARP height
  0.0083 m, at 77.75 S, 166.66 E. The DOY 129 file carries GPS C1C L1C C1W L1W C2W L2W C2X L2X C5X L5X C1X L1X,
  Galileo C1X L1X C5X L5X (and E6, E7, E8), GLONASS, BeiDou B1I B3I, QZSS. With the golden's GPS priority list
  (`C1W` and `L1W` first) and the Galileo list (`C1X`, `L1X`, `C5X`, `L5X`), the consumer latches L1W / C1W and
  L2W / C2W on every GPS satellite and L1X / L5X on Galileo (the sat trace of every replay below). Four other
  census sites are Javad L1W sites (QUIN00USA, UTQI00USA, WUH200CHN, BADG00RUS), as is WTZZ00DEU; BAKE00CAN
  (Septentrio) latches L1W too. The receiver type is not what singles the site out.
- **Geometry.** From the SP3 at the half-hours of hours 2 to 6 of DOY 129, GPS + Galileo above the golden's
  15 degree mask: median 18 satellites (10 GPS), the most of the 17 sites, but the worst vertical geometry
  (VDOP 1.76 against 1.15 to 1.51 elsewhere, GPS-only VDOP 2.29 against 1.62 to 2.08) and a sky with no satellite
  above 56 degrees (every other site reaches 65 to 80). The horizontal geometry is the best of the set (HDOP 0.63).
  A factor of 1.2 to 1.5 in the vertical, not the factor of 5 to 17 the error shows (0.23 m against 0.013 to
  0.068 m, table of section 5.3).
- **Ocean tide loading.** Neither SIF registry names the station (`data/static/sif/igs20_SIF.json`,
  `ncn20_SIF.json`), so the goldens apply zero loading at the site; EOT20's M2 radial amplitude there is 1.9 mm
  (card OTL section 4; the Ross Sea tide is mostly diurnal, so the full loading is a few millimetres at most).
  Card OTL's consumer-only replay changed the site by 0.0 mm. Zero loading cannot carry a decimetre.
- **The roster neighbour.** SCTB00ATA (Scott Base) is 3.1 km away and on the roster, so the wire's slant
  ionosphere for the site's lines of sight comes from a station under the same sky; the network ionosphere is not
  the site's weakness. SCTB00ATA does not track L1W.
- **The L1W family's geography.** 18 of the 58 roster stations track L1W (card SID's count holds on this roster):
  WIND00NAM 22 S, VOIM00MDG 22 S, STHL00GBR 16 S, MBAR00UGA 1 S and fourteen stations between 5 N and 65 N. None
  lies south of 45 S. The six roster stations south of 45 S (KRGG00ATF, PALM00ATA, MAC100AUS, CAS100ATA,
  SYOG00ATA, SCTB00ATA) track L1C only. The wire emits an L1W phase bias for a satellite only while an L1W station
  tracks it, so a satellite over the southern polar sky has none: on the baseline wire `_rf0hv17_field6` over
  hours 2 to 6 the L1W phase bias exists in 0 % of G16's epochs, 3 % of G26's, 9 % of G01's, 17 % of G04's, 29 % of
  G09's, 36 % of G27's, 47 % of G31's, 51 % of G28's and 54 % of G08's, and in 97 to 100 % of the epochs of the
  satellites WTZZ00DEU tracks in the same window (section 5.4). That is the one property of the site no other
  census site shares: it latches the sparse family under the only sky that family does not cover.

## 4. The hypotheses and what each predicts

- **H1, the leaked return of G01.** The legacy pivot path hands the GPS pivot from G01 to G02 while G01 is out
  (card PIV section 11.4); on its return the producer's G01 clock state is about 100 m from the product, the
  gross-LOS withhold keeps it off the wire in every epoch but the first (card MDAY section 9), and that one epoch
  reaches a consumer that has G01 in view. Predicts: the divergence needs G01 in view at the return epoch
  (exposure), and vanishes when G01 is removed from the consumer's file for the minutes around the return, or when
  the leaked records are removed from the wire; removing another satellite in the same window changes nothing.
  Also predicts the election (G01 never the pivot) and the COD regime (q 5e-6 holds the clock to the a priori)
  remove it, as observed.
- **H2, the uncovered band-1 row.** A consumer phase row whose signal has no phase bias on the wire gets none of
  the declarations the wire makes through the P record (the held bias's generation, its presence and freshness
  flips), so its ambiguity is kept across a step the covered rows re-float, and it sees the satellite clock's own
  motion where the held bias cancels it on the covered rows (the phase-clock hold of card LAH writes each held
  bias so that the phase-visible clock c minus p keeps its generation-start value). Predicts: the standing error
  sits on the uncovered L1W rows and scales with the satellite's clock excursion; the site's error collapses when
  it latches the covered family (L1C + C1C) or when the wire carries an L1W bias for every satellite (D6, C6);
  excluding the uncovered satellites with large excursions (G01) lowers the error far more than excluding covered
  ones or uncovered ones with small excursions (G16); the election's +40.6 mm shrinks under the family swap.
- **H3, the southern weakness of the wire.** The satellites the site sees are tracked by the fewest roster stations,
  so their clocks and biases are the least determined and move most under any change of the solve. Predicts: the
  per-satellite clock scatter of the wire is largest for the satellites in the site's sky, the error is spread
  over them, and a family swap at the consumer changes little.
- **H4, the geometry gain.** The high-latitude up-clock degeneracy amplifies every wire error at this site. Predicts
  a gain of the order of the VDOP ratio (1.2 to 1.5) on every site of similar latitude (DAV100ATA at 68 S), not a
  factor of 10 at one site.
- **H5, ocean loading.** Zero loading at a site with a real tide. Predicts a tidal signature of a few millimetres
  and no response to wire changes; bounded by the loading itself (section 3).
- **H6, receiver or data quality.** A site-specific tracking defect. Predicts a wire-independent error (the same
  on every wire) and no family or satellite dependence.

H1 and H2 are the same statement at two magnitudes: a step on the wire that reaches a phase row without the
declaration that would re-float it. H3 to H6 are the alternatives that would make the site an outlier for a reason
of its own.

## 5. Reads of the runs on disk (no new run)

### 5.1 Exposure: who sees G01 at the return epoch

Card MDR's 17 census replays of DOY 131 (`D:/GnssProcessorSupport/runs/_rap_mdc_131_*`) and of DOY 128 with DOY 127's
products (`_rap_mdcp_128_*`), read at the first epoch the wire carries G01 after its gap (03:22:30 UT, TOW 444150,
and 03:36:00 UT, TOW 185760). Two of the 17 sites have G01 above the mask at that moment, and both see the same wire
record (G01 clock -104.698 m on DOY 131, -100.332 m on DOY 128):

| site | G01 elevation | satellites | 3D error one epoch before, one after | maximum in the next hour |
|---|---|---|---|---|
| ARHT00ATA, DOY 131 | 37.7 | 9 | 0.024 m, 181.444 m | 187.4 m (79 epochs above 1 m) |
| DAV100ATA, DOY 131 | 16.5 | 10 | 0.034 m, 0.037 m | 0.061 m |
| ARHT00ATA, DOY 128 | 37.3 | 9 | 0.290 m, 99.699 m | 106.8 m (119 epochs above 1 m) |
| DAV100ATA, DOY 128 | 16.2 | 10 | 0.027 m, 0.041 m | 0.092 m |
| the other 15 sites | not in view | 6 to 9 | unchanged | 0.04 to 0.96 m |

The divergence is therefore not a property of a wire change at large: it needs G01 in view, and only the two
Antarctic sites have it in view at that hour. DAV100ATA survives the same record: its band-1 row is L1C with no
band-1 code (the Septentrio latch of card MSC), and the trace shows the 104 m misfit going into the G01 slant
ionosphere state (iono -2.28 to +102.36 m at TOW 444150) while the position stays. At ARHT00ATA the band-1 row has
its code (C1W or C1C), which forbids that escape, and the misfit goes into the position.

### 5.2 One consumer, three wires: the knife edge

Card BDSB's R2 arms hold the same GPS-only consumer of ARHT00ATA on two wires of DOY 131
(`_rap_bdsr2cg131_arht` on the C wire, diverging to 176 m; `_rap_bdsr2geg131_arht` on the GE wire, surviving), and
card MDR's arm holds it on the GPS-only wire (diverging to 181 m). At TOW 444150 all three wires carry the same
kind of record for G01 (clock -101.6, -102.8 and -104.7 m; an `R,G01` retro bump; code biases for C1C, C1W and C2W;
a phase bias for L2W only, generation 2; URA 0.50 to 0.54 m) and the three consumer logs of that epoch are
identical line for line apart from the position: the L2W hold generation 1 to 2 and its `reset_flip`, the C1C and
C2W code biases flipping absent to applied, "SSR clock discontinuity: G01 (datum re-reference)" answered by an ISB
re-inflation, and the code-bias datum re-reference of eight other satellites. The satellite trace shows the one
difference: the G01 band-1 ambiguity. On the GE wire it is re-floated at the return epoch (`amb_var` 1e6, the
re-seeded value -96.25 m absorbing the step) by a path that writes no event token; on the C wire and the GPS-only
wire it keeps its value (6.62 m, variance 0.19 m²), and at the next epoch the L1C phase row enters with an
innovation of -102 m, a residual of 0.000 m after the update and the position 176 and 181 m away. The L2W row, which
has a held phase bias, is re-floated on all three wires. Card MDR's consumer of 2026-10-05 also re-seeded its
receiver clock from the code WLS at that epoch (-253.6 m; the seed's 15 km threshold does not fire on the current
consumer, which prints two seed lines in 6 h and none at the return); on the current consumer the C wire diverges
with the clock correct at the return epoch, so the seed is not the path.

### 5.3 Coverage against error, 17 sites, the baseline wire

Card NIS's screen-off census replays (`_rap_isc6_off_*`, the `_rf0hv15_field6` wire, 6 h) read for the share of
phase rows in the update whose signal has no phase bias on the wire ("uncovered"), hours 2 to 6:

| site | hours 2 to 6 mean p50 (m) | uncovered rows | of them L1W | L1W share of all phase rows | phase families |
|---|---|---|---|---|---|
| ARHT00ATA | 0.2253 | 16.6 % | 59.5 % of its L1W rows | 27.9 % | L1W L1X L2W L5X |
| DAV100ATA | 0.1293 | 8.8 % (Galileo L1C and L5Q) | none | 0 | L1C L2W L5Q |
| POHN00FSM | 0.0678 | 0 | | 0 | L1C L2W L5Q |
| ANMG00MYS | 0.0545 | 0 | | 0 | L1C L1X L2W L5X |
| KOKB00USA | 0.0495 | 0 | | 0 | L1C L2W L5Q |
| UTQI00USA | 0.0491 | 0 | 0 | 27.8 % | L1W L1X L2W L5X |
| WUH200CHN | 0.0455 | 0.5 % | 1.7 % | 30.3 % | L1W L1X L2W L5X |
| SCRZ00BOL | 0.0450 | 17.2 % (Galileo L1C and L5Q) | none | 0 | L1C L2W L5Q |
| CEDU00AUS | 0.0427 | 0 | | 0 | L1C L1X L2W L5X |
| CHTI00NZL | 0.0427 | 0 | | 0 | L1C L1X L2W L5X |
| HARB00ZAF | 0.0391 | 7.6 % (Galileo L1C and L5Q) | none | 0 | L1C L2W L5Q |
| ASCG00SHN | 0.0351 | 0 | | 0 | L1C L1X L2W L5X |
| QUIN00USA | 0.0264 | 1.6 % | 5.8 % | 27.3 % | L1W L1X L2W L5X |
| BADG00RUS | 0.0249 | 0 | 0 | 29.9 % | L1W L1X L2W L5X |
| STPM00SPM | 0.0239 | 0 | | 0 | L1C L1X L2W L5X |
| BAKE00CAN | 0.0165 | 0 | 0 | 26.7 % | L1C L1W L2W L5Q |
| WTZZ00DEU | 0.0132 | 0 | 0 | 27.8 % | L1W L1X L2W L5X |

Six sites latch L1W. At five of them the wire covers 94 to 100 % of the L1W rows; at ARHT00ATA 40 %. The three sites
with uncovered Galileo rows (SCRZ, HARB, DAV1) are the three that track Galileo on L1C and L5Q where the wire's
L1C and L5Q biases are partial for E02, E11, E25, E34 and E36 (section 5.4); their uncovered satellites' clocks
move by 1.5 to 4.6 cm in the window and the sites sit at 0.039 to 0.129 m. The sim bed's consumer SZZ000SIM has
0 uncovered rows of 19,841 in the baseline pair's 6 h, so the mechanism predicts that the sim bed cannot show it.

### 5.4 What the wire covers, satellite by satellite

The baseline wire `_rf0hv17_field6`, hours 2 to 6 (599 epochs), GPS: every satellite with a clock record has an
L1C and an L2W phase bias in every epoch; the L1W phase bias is present in every epoch of G02, G05, G06, G10 to
G15, G17, G19 to G25, G29 (72 %), G30 (87 %), G32 and in 0 % of G16's, 3 % of G26's, 9 % of G01's, 17 % of G04's,
29 % of G09's, 36 % of G27's, 47 % of G31's, 51 % of G28's, 54 % of G08's, 63 % of G07's, 74 % of G03's and 79 % of
G18's. The first group is the sky of WTZZ00DEU, QUIN00USA and the northern sites in that window; the second is the
sky of ARHT00ATA (G01 421 epochs in the update, G16 488, G26 445, G04 384, G09 497, G27 265, G31 289, G28 160, G08
157). Galileo: L1X and L5X biases in every epoch; L1C and L5Q partial for E02 (296 of 487), E11 (418), E18 (514),
E25 (416), E30 (373), E34 (304) and E36 (201 of 599).

### 5.5 The motion the hold hides

Under the phase-clock hold the held phase bias p follows the emitted clock c so that c minus p keeps its
generation-start value; the series of p on the wire is therefore the clock motion a covered row never sees and an
uncovered row sees in full. On `_rf0hv17_field6` over hours 2 to 6 the standard deviation of the held L2W bias is
1 to 7 cm for 27 of the 31 GPS satellites, and 1.54 m for G01 (the unhealthy satellite, clock range 1.3 m against
the GPS median), 0.73 m for G25, 5.12 m for G26 and 9.35 m for G12 (the three with 105 to 111 epoch gaps in the
window and a 1.4 to 29.8 m step of the held bias at their return, generation 2). G01 and G26 are uncovered at
ARHT00ATA for 421 and 445 epochs; G12 is covered everywhere it is seen (WTZZ00DEU 407 epochs). On the DOY 130 pair of
card BOF the same four satellites carry the motion (G01 7.6, G12 9.2, G25 12.4, G26 7.6 m on the control; 6.9, 9.3,
12.4, 7.7 m under the election) and the election's one visible change at the wire is the common clock datum
(c minus p constant to 3 mm across the window under the election, 0.79 m standard deviation on the control) and G01's
return level. (Read with section 6.2: these per-satellite excursions are not what carries the decimetre; the common term of section 6.4 is.)

### 5.6 The election pair and the per-signal wires

ARHT00ATA's uncovered share is the same on card BOF's DOY 130 control and election wires (14.6 18.0 17.5 14.9 15.6
15.2 % by hour against 14.6 18.0 17.5 14.9 15.7 15.1 %), with the same satellites (G16 494, G26 470, G01 458 epochs
and the rest), while the error differs by 40.6 mm; the election does not change which rows are uncovered, it changes
what they see. On the two per-signal wires the uncovered share falls to 0.1 to 1.5 % (G01 L1W for 89 epochs, five
rows once) and the error to the census level: card NOM4's D6 arm 0.0168 m against its base 0.2253 m, card BDSB's
arm C6 0.0159 m against 0.2305 m for a GPS + Galileo user and 0.0524 m against 0.5929 m for a GPS-only user. The
D6 and C6 gains at the site (-208.5 and -214.6 mm) are the same number: the ARHT00ATA wire gain of card BDSB
section 5 is the per-signal wire that posture C carries, not BeiDou.

## 6. Consumer replays of existing wires

Every arm is a consumer-only replay (`network.closed_loop.replay_products_path`) on one Release exe,
`D:/GnssProcessorSupport/gates/arht/x64-Release/gnss_processor_arht.exe`, a byte copy of the baseline pair's
`gnss_processor_bofarm.exe` (sha256[:16] `91143c81de9a9547`), built with `rapid_arm.py make` from the existing
census arms (the wire path moved to D: where the C: copy is gone) and run under `D:/GnssProcessorSupport/runs_nom`
as `_rap_arhtm_*`. A RINEX edit reaches the consumer's own file only (`stations=ARHT00ATA;consumer=1`). A 12-epoch
smoke (`arhtm_smk`) ran first. The driver, the bases, the edited wires and the grades are under
`D:/GnssProcessorSupport/gates/arht/` (`arht_replays.py`, `arht_grade.py`, `bases/`, `wires/`, `out/`). The
predictions of section 4 were written before any arm ran.

### 6.1 R1: the divergences (H1)

Wires: card MDR's DOY 131 and DOY 128 (with DOY 127's products) wires, GPS-only golden `3959f24`
(`D:/GnssProcessorSupport/runs/_rap_mday131_f6`, sha256[:16] `483049ef17142259`; `_rap_mday128p_f6`,
`fe28310651468a27`), and card BDSB's DOY 131 C and GE wires (`runs_nom/_rap_mday_c_131`, `6e4020a49d30a0e7`;
`_rap_mday_ge_131`, `6dbe02e17f822e4d`) with their GPS-only consumer. "Wire edit" is a copy of the wire without
G01's seven records (C, three B, P, U and the `R` entry) in the return epoch's block
(`gates/arht/wires/wire131_noG01ret.txt`, `wire128_noG01ret.txt`, `wireC131_noG01ret.txt`). "Window" is the
consumer's RINEX with every G01 observation blanked between TOW 444000 and 444600 (DOY 131, 03:20:00 to 03:30:00 UT)
or 185400 and 186300 (DOY 128, 03:30:00 to 03:45:00 UT). The G09 arm blanks G09, the highest satellite in the
site's sky at that hour, in the same window as a control.

| arm | hourly 3D p50 (m) | error one epoch after the return | maximum in the next hour | epochs above 1 m to the end |
|---|---|---|---|---|
| DOY 131, as it is | 0.051 0.025 0.031 166.219 0.143 0.047 | 181.444 m | 187.4 m | 79 |
| DOY 131, G01 blanked 03:20 to 03:30 | 0.051 0.025 0.031 0.033 0.069 0.058 | 0.021 m | 0.087 m | 0 |
| DOY 131, G01 blanked all day | 0.055 0.021 0.019 0.023 0.067 0.052 | 0.029 m | 0.080 m | 0 |
| DOY 131, G09 blanked 03:20 to 03:30 (control) | 0.051 0.025 0.031 0.574 0.090 0.048 | 361.760 m | 361.8 m | 44 |
| DOY 131, wire without G01's return records | 0.051 0.025 0.031 0.033 0.068 0.057 | 0.022 m | 0.088 m | 0 |
| DOY 128, as it is | 1.980 0.588 0.273 0.276 54.089 28.360 | 99.700 m | 106.8 m | 287 |
| DOY 128, G01 blanked 03:30 to 03:45 | 1.980 0.588 0.273 0.195 0.121 0.093 | 0.205 m | 0.289 m | 0 |
| DOY 128, wire without G01's return records | 1.980 0.588 0.273 0.194 0.122 0.092 | 0.205 m | 0.289 m | 0 |
| DOY 131, C wire, GPS-only user, as it is | 0.053 0.025 0.031 120.035 84.542 59.454 | 175.877 m | 175.9 m | 314 |
| DOY 131, C wire, G01 blanked 03:20 to 03:30 | 0.053 0.025 0.031 0.033 0.069 0.058 | 0.021 m | 0.087 m | 0 |
| DOY 131, C wire without G01's return records | 0.053 0.025 0.031 0.033 0.067 0.057 | 0.022 m | 0.089 m | 0 |
| DOY 131, GE wire, GPS-only user, as it is | 0.052 0.025 0.031 0.032 0.062 0.051 | 0.024 m | 0.084 m | 0 |

The current executable reproduces card MDR's divergences to the printed digit (166.22 m in hour 4 of DOY 131;
54.1 and 28.4 m in hours 5 and 6 of DOY 128) and card BDSB's C-wire divergence byte for byte over the 6 h
(`closed_loop_trace.csv` of `arhtm_cg131_plain` equals the first 720 rows of `_rap_bdsr2cg131_arht`). Removing
G01's seven records from the one wire epoch removes the divergence on both days and both postures; blanking G01 at
the consumer for ten to fifteen minutes around the return does the same; blanking G09 in the same window leaves the
divergence and doubles it (fewer rows to resist the step). The same GPS-only consumer survives the GE wire's
return record (-102.8 m against -101.6 m on the C wire) on this executable as it did on card BDSB's: the knife edge
of section 5.2 is reproduced, not exe-specific. H1 stands on a test that could have failed it.

**The knife edge, read with the Kalman-filter log on** (`arhtm_cg131_kf`, `arhtm_geg131_kf`: the same two arms with
`logging.enable_kalman_filter` and the `kalman_filter` and `rt_engine` levels at debug; the divergence and the
survival reproduce). The three divergences share one signature at the return epoch. The consumer applies no
measurement update there (0 of 18 satellite rows carry an innovation, against 17 of 18 in the surviving replay and
20 of 20 one epoch earlier in all of them), re-floats the ambiguities of G07, G16 and G26 on both bands (and G29 on
DOY 131 of card MDR's wire) with no event token, re-floats G01's L2W row through the hold generation, and leaves
G01's band-1 row with its ambiguity (variance 0.205 m²). The next epoch's update then has 14 to 16 rows, among them
that one row with a 100 m innovation, and the position takes the step. In the surviving replay the update runs at
the return epoch: the first pass absorbs the step into the state, leaving the two G01 code rows with -93 and -85 m
residuals and the band-1 phase row with -0.024 m; the peel excludes the code rows, treats the phase row as a
sub-cycle bias (|res| below the λ/4 floor of `estimation.pivot_slip_min_cycles`: a Huber R-inflation, "ambiguity
state preserved") through three rebuild passes until its residual grows to -0.081 m, above the floor, and only then
resets the ambiguity ("Phase outlier action (primary): ambiguity reset for key=G01_L1C", re-seeded at -96.255 m,
17 passes in all). The surviving row is therefore saved by a peel that first argues it away as sub-cycle; the
diverging consumer never reaches the peel at that epoch. A third replay of the diverging case with the engine, the MW detector and the filter logs at debug (`arhtm_cg131_rtdbg`, 4 h; the filter log rotates into `kalman_filter.1.log`, which is where the return epoch is) names the rule. The update does run at the return epoch: the first pass spreads the 102 m step over the states, so the two G01 code rows come out with -93 and -85 m residuals and the band-1 phase row with -0.024 m; the MW detector flags nothing (every pair within 0.3 of its threshold's wide lane). The peel then takes one row per pass, the largest standardized residual first: the code rows of G01 and, as the receiver code-bias states re-inflated at the datum re-reference soak up the step, the code rows of other satellites (36 rows at the first pass, 28 at the last), with every phase row it meets argued away as sub-cycle and Huber-inflated. The band-1 row of G01 sits at a standardized residual of 7 to 13 throughout and is never the pass's largest. The budget of passes is `computeRebuildPassBudget` (`src/core/app/EpochUpdateSupport.cpp`: the larger of 10, the ambiguity count plus 3, and half the measurement count plus 3), 23 passes here; the diverging replay spends all 23 without a clean pass, and the loop ends without a commit (`update_applied` false in `RealTimeEngineEstimate.cpp`): no innovation is written for that epoch, the position stays, and the ambiguity resets the passes applied to G07, G16 and G26 (both bands) stay with it, while G01's band-1 ambiguity is kept. The surviving replay converges at pass 17 of the same budget because its band-1 residual (-0.081 m) crossed the λ/4 floor while it was the largest, and the reset that followed made the next pass clean. The knife edge is therefore the order in which the peel reaches the rows and the pass budget against it, decided by the second decimal of the wire's record (-101.6 against -102.8 m): a 100 m step on a kept-ambiguity phase row is not a case the peel recognises, because the first pass has already moved the step out of that row's residual.

### 6.2 R2: the standing error (H2 against H3, H4, H6)

The baseline wire `_rf0hv17_field6` (DOY 129, the current golden, sha256[:16] `bf16e9474305e2de`), ARHT00ATA
installed as the consumer of card BOF's own arm, 6 h. "L1C + C1C" blanks every `?1W` type in the consumer's
file (`strip;types=?1W`), so the priority latch falls to C1C / L1C with C2W / L2W unchanged; "GPS only" sets
`processing.active_systems` to GPS; an exclusion blanks every observation of one satellite.

| arm | hourly 3D p50 (m) | hours 2 to 6 | change | uncovered phase rows, hours 2 to 6 |
|---|---|---|---|---|
| as it is (L1W + C1W, GPS + Galileo) | 0.301 0.374 0.210 0.194 0.129 0.250 | 0.2313 m | | 16.7 % |
| L1C + C1C | 0.035 0.022 0.022 0.020 0.013 0.008 | 0.0170 m | -0.2143 m | 0.4 % |
| GPS only | 2.291 0.744 1.240 0.633 0.257 0.091 | 0.5929 m | | 29.8 % |
| GPS only, L1C + C1C | 1.081 0.174 0.041 0.022 0.028 0.025 | 0.0579 m | -0.5350 m | 0.8 % |
| without G26 (uncovered 97 %, 445 epochs) | 0.299 0.367 0.185 0.164 0.101 0.214 | 0.2064 m | -0.0249 m | 15.2 % |
| without G16 (uncovered 100 %, 488 epochs) | 0.301 0.374 0.199 0.216 0.120 0.160 | 0.2138 m | -0.0175 m | 14.9 % |
| without G01 (uncovered 91 %, 421 epochs) | 0.309 0.375 0.219 0.207 0.152 0.265 | 0.2437 m | +0.0124 m | 15.3 % |
| without G20 (covered, 370 epochs) | 0.301 0.374 0.206 0.200 0.130 0.241 | 0.2303 m | -0.0010 m | 17.2 % |

The as-is arm reproduces card BDSB's GE-wire user (0.2305 m, hour 6 0.246 against 0.250 m) and the GPS-only arm
card BDSB's GPS-only user to four decimals (0.5929 m). Both family swaps take the site to the census level: the
GPS + Galileo consumer from 0.231 to 0.017 m (WTZZ00DEU on the same wire 0.013 m), the GPS-only consumer from 0.593
to 0.058 m; the GPS-only L1C numbers are card LAH's family swap of 2026-09-29 (1.08 0.17 0.04 0.02 0.03 0.02 m) on a
wire five weeks younger. The swap removes 93 % of the error at a site whose geometry, receiver, antenna and sky it
leaves untouched: H4 and H6 are out, and H3 would need the wire's clocks to be good on L1C and bad on L1W for the
same satellites, which the per-signal result of section 5.6 excludes. The exclusions say the error is collective,
not one satellite's: removing an uncovered satellite gains 17 to 25 mm, removing a covered one 1 mm, and removing
G01, the unhealthy satellite with the largest clock wander, costs 12 mm, because the site loses a tenth of its GPS
rows and keeps eight other uncovered satellites. The excursion-weighted reading of section 5.5 (G01 and G26 would
carry most of it) is therefore wrong at the decimetre scale; what the uncovered rows see in common is the term
(section 6.4).

### 6.3 R3: the election's response under the family swap

Card BOF's DOY 130 control and election wires (`_rap_bof_ctl130_f6`, `_rap_bof_piv130_f6`), the census arms of
ARHT00ATA as they are and with `?1W` blanked, 6 h, one exe:

| consumer | control wire | election wire | election minus control |
|---|---|---|---|
| as it is (L1W + C1W) | 0.372 0.278 0.169 0.184 0.141 0.216 m, hours 2 to 6 0.1976 m | 0.370 0.278 0.214 0.199 0.214 0.285 m, 0.2382 m | +40.6 mm |
| L1C + C1C | 0.081 0.016 0.031 0.034 0.038 0.024 m, 0.0284 m | 0.079 0.016 0.031 0.033 0.040 0.027 m, 0.0293 m | +0.9 mm |

The as-is pair reproduces card BOF's grade and card PIV's failed clause to the tenth of a millimetre. On the
covered family the same consumer on the same two wires sits at the census level on both and the election's
response is +0.9 mm, below the 1 cm clause by an order of magnitude and of the size the other 15 sites showed
(-3.6 to +1.6 mm). The +40.6 mm was the uncovered rows' view of the election.

### 6.4 What the uncovered rows see in common

A covered row sees c minus p, an uncovered row c. Both populations share the receiver, so what matters is not each
satellite's motion but the difference of the two populations' common terms: D(t) = median over GPS of c minus
median over GPS of (c minus p_L2W), read from the wire (`out/commondrift.py`). On `_rf0hv17_field6` over hours 2
to 6, D has a standard deviation of 1.21 m and a range of 3.52 m, with within-hour standard deviations of 0.30 to
1.13 m; it steps by 1.6 m at the G01-to-G02 pivot handover of 04:42:30 UT, which the held biases absorb (c minus p
does not step) and the emitted clocks carry. On the DOY 130 pair D's standard deviation is 0.17 m on both wires
(range 0.63 m), and the two wires' hourly means of D differ by 10 to 36 mm in hours 3 to 6, the hours in which the
election moved the site by 46 to 73 mm. The consumer has one receiver clock: it follows the 13 covered rows, and
the 5 uncovered rows carry D(t) as a common, time-varying misfit that their float ambiguities cannot hold and the
position, the zenith delay and the ambiguities share out. That is the decimetre, and it is why no single satellite
carries it and why the covered family removes all of it.

## 7. The mechanism, named

**One mechanism at two magnitudes: the wire declares a satellite's level steps and level motion only through its
held phase biases, so a consumer phase row whose signal has no phase bias on the wire gets no declaration, keeps its
ambiguity across the step, and sees the satellite clock's motion in full.** At ARHT00ATA the undeclared rows are the
L1W rows: the site latches the L1W family (a Javad receiver under the golden's `C1W`-first priority) under the one
sky that family does not cover (no L1W roster station south of 45 S), so for G01, G16, G26, G04, G09, G27, G31, G28
and G08 its band-1 phase row runs without a wire bias for most of their passes.

- **At the metre scale** (rows 3, 4, 9, 10 and 11 of section 2): G01 returns from a gap with its clock about 100 m
  from the product; the legacy pivot path emitted the step and the gross-LOS withhold lets the first epoch through
  (card MDAY section 9). The L2W row's held bias changes generation and the consumer re-floats that row; the band-1
  row has no bias and no declaration, and whether it is re-floated at that epoch is left to the post-fit peel's pass budget (section 6.1). The
  wire does declare the step on the satellite's clock record (`clock_disc`, the `R` line), but the consumer's
  response to a per-satellite clock discontinuity is an ISB re-inflation (`reinflateOnClockDisc`,
  `src/core/realtime/RealTimeEngineSsrBreaks.cpp`), which touches no ambiguity. When the band-1 row keeps its
  ambiguity it enters the next update with a 100 m innovation against a 0.4 m ambiguity sigma, and the static
  position, not the ionosphere (a band-1 code row forbids that escape, which is how DAV100ATA survives), takes the
  step; the position then recovers through code over hours. The test that could have falsified this (section 6.1):
  seven wire records removed from one epoch, or G01 blanked at the consumer for ten minutes, remove the divergence on
  both days and both postures, while blanking the highest satellite in the same window does not.
- **At the decimetre scale** (rows 1, 2, 6 to 8 and 11 to 14): the same rows see G01's 1.3 m clock wander, the
  re-levels of the satellites that leave and return (G26, G25), and whatever a change of the producer moves in those
  satellites' clocks, where every covered row sees c minus p and nothing of it. The family swap removes 93 % of it (0.231 to 0.017 m; the GPS-only consumer 0.593 to 0.058 m), the election's +40.6 mm becomes +0.9 mm on the covered family, no single satellite carries more than 25 mm of it, and what the uncovered rows see in common, the difference D(t) between the emitted clocks' datum and the held biases' datum, drifts by 1.2 m (standard deviation) over hours 2 to 6 of the DOY 129 wire and steps by 1.6 m at its pivot handover (sections 6.2 to 6.4).

The two magnitudes share the row, the declaration gap and the site property; they differ in what reaches the row
(a 100 m return record once or twice a day on two of five days, against metre-and-below motion every day). A
third process, the receiver-clock seed of the 2026-10-05 consumer (section 5.2), amplified the DOY 128 and 131
events on card MDR's exe and is absent from the current consumer, which diverges all the same.

**What was ruled out.** The southern weakness of the wire as a scatter (H3): the clock scatter of the satellites in
the site's sky (G16 0.048, G26 0.050, G04 0.043, G31 0.046 m against the GPS median) is no larger than that of the
northern sky (G12 0.087, G23 0.088, G14 0.068 m), and the per-signal wires remove the error without touching the
clocks. The single-satellite exclusions (section 6.2) remove at most 25 mm for an uncovered satellite and 1 mm for a covered one, and the family swap on the same satellites and the same sky removes 214 mm: the error is in the rows' coverage, not in the satellites' clocks. The geometry gain (H4): the vertical geometry is 1.2 to 1.5 times worse than the mid-latitude
sites, not 5 to 17 times, and the family swap leaves the geometry as it is. Ocean loading (H5): no block, 1.9 mm M2,
a consumer-only replay changes 0.0 mm. The receiver (H6): the same receiver type with full L1W coverage sits at
0.013 to 0.049 m at five census sites, and the same receiver, antenna and sky at ARHT00ATA sit at 0.017 m on
the L1C + C1C family. Not established:  the magnitude-2 contribution of each uncovered satellite beyond
the ones excluded here; the behaviour on a 24 h window and on days other than 128 to 131; whether the peel's pass budget and sub-cycle floor,
which let a 100 m step through at this site, would do so at a site with more rows and better geometry (the
same record did not divert DAV100ATA, by another route).

## 8. What this means

- **BeiDou, course A (card BDG section 4).** The "ARHT00ATA wire gain" of posture C is the per-signal wire
  (`network.n3c_wire_per_signal`), not BeiDou: card NOM4's D6 arm (that key alone, GPS + Galileo) gains -208.5 mm
  at the site and card BDSB's C6 -214.6 mm for the same user, both by covering the L1W rows (section 5.6). Course A
  is not the way to that gain; a ledger decision on `network.n3c_wire_per_signal` for the primary lineage is (its
  row is UNVALIDATED there and RECOMMEND_SCOPED under `has_bds`; D6's 16-site census, 14 sites unchanged to 0.1 mm,
  is its evidence, with one site clause to re-grade on the current golden). The second prerequisite of course A,
  "the G01 epoch off the wire", is named: the first epoch of a gap return passes the gross-LOS withhold, and the
  consumer does not act on the clock discontinuity the wire declares for it. Either side closes it; neither is
  built here.
- **PIV.** The election's failed clause is this mechanism read on DOY 130 and 132: the election changes G01's
  return level and the clock-datum sequence, and the uncovered rows at ARHT00ATA report the change where every
  covered site reports nothing (sections 5.5 and 5.6). On the covered family the same consumer on the same two wires reads the election at +0.9 mm (section 6.3). The election also removes the metre-scale event
  (G01 returns within 0.6 m, card PIV section 11.4). The clause as written measured what the uncovered rows see, not
  what the election does to a consumer the wire covers; whether a census clause should be graded on the covered
  family at this site is the author's.
- **The COD-regime candidate (card NBP section 12).** The regime prevented the DOY 131 divergence by holding the
  clock to the COD a priori, which removes the 100 m return record at the source; it does nothing for the uncovered
  rows, and its census neutrality elsewhere stands. It is one of three ways to remove the metre-scale event (the
  election, the regime, a producer rule that keeps a returning satellite off the wire until the screen has passed
  it), each with its own cost (card NBP's hour 1, card PIV's clock product); none is needed for the decimetre scale.
- **Remedy candidates, not built (the author's).** (a) Wire coverage: a phase bias for every signal a consumer may
  latch, the per-signal wire or an L1W bias derived from L1C where no L1W tracker sees the satellite. (b) Consumer:
  on a declared per-satellite clock discontinuity, re-float that satellite's ambiguities on every signal
  (`reinflateOnClockDisc` holds that a per-satellite reaction is impossible, which is true of the receiver states
  and not of the satellite's ambiguities); a consumer rule that latches the family the wire covers would do the
  same for the standing error. (c) Producer: the first epoch of a return should pass the gross-LOS screen before it
  is emitted. Each is a card of its own, default off, with the replays of section 6 as its bed.

## 9. Reproduction

Under `D:/GnssProcessorSupport/gates/arht/`: `python arht_replays.py bases`, `make <group>`, `run <group> [jobs]`
(groups `smoke`, `r1`, `r2`, `r2b`, `r3`, `r1b`); `python arht_grade.py` prints the tables of section 6 from the
`_rap_arhtm_*` runs under `D:/GnssProcessorSupport/runs_nom`. The wire edits are `out/wire_edit.py <wire> <out>
G01 <tow_from> <tow_to>`. The reads of section 5 are the scripts in `out/` with their outputs beside them:
`g01_exposure.py` (5.1), `coverage.py` (5.3, 5.6), `wire_cov.py` (5.4), `levelmotion.py` (5.5), `dop.py` (section
3), `satstats.py` and `respdiff.py` (the per-row tables). Exe `x64-Release/gnss_processor_arht.exe` (sha256[:16]
`91143c81de9a9547`, the DLLs of the build tree beside it). Load: the replays of section 6 ran with three other
sessions' producers on the machine, so their wall times (60 to 100 s for a GPS-only 6 h replay, about 30 minutes
for a GPS + Galileo one) say nothing about cost.
