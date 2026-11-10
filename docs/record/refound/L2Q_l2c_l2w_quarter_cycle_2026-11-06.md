# L2Q: L2C against L2W quarter-cycle alignment in the field files

**STATUS**

- **id:** L2Q
- **topic:** network-vehicle
- **status:** closed
- **summary:** CLOSED 2026-11-06: the GPS L2C minus L2W phase fraction, read from the raw RINEX of 294 stations over DOY 128 to 132 of 2024 (579 station-days, 21 receiver types, 34 482 arcs), sits at 0 at every station-day and never at a quarter cycle; the largest station offset is 0.018 cycle, the per-satellite part at most 0.004 cycle. The SYS / PHASE SHIFT records disagree with each other across receiver families (0, -0.25, +0.25 for the same physical relation) while every file is aligned. No correction is needed for field integer work, provided `model_corrections.apply_phase_shifts` stays false (on, it un-aligns the Trimble, Leica, JAVAD and Topcon files) and the files are used as recorded. Every golden station latches L2W, and a station-consistent offset cancels in every double difference by construction.
- **source:** the register item "L2C against L2W quarter-cycle alignment" of [OPEN_ITEMS](../../status/OPEN_ITEMS.md) (re-filed from [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) at its close, a prerequisite of [IRD](../../refound/IRD_integer_recovering_datum_2026-10-13.md)); RINEX 3.04 (IGS/RTCM, 2018-11-23) section 9.1, Table 25, Table A2 and Table A23; the parser `src/core/rinex/RinexParser.cpp`; the canonical latch `src/core/model/PppModel.cpp`; the run log of `runs_nom/_rap_bdsc_c6`
- **knobs / posture:** nothing armed; `model_corrections.apply_phase_shifts` (default false) read and left off, its KEEP_OFF ledger row landed 2026-11-06 (section 8); the field golden's GPS priority list (C2W, L2W before C2S, C2L, L2S, L2L; no 2X)
- **dates in the text:** 2026-11-06; data of 2024 DOY 128 to 132
- **hashes / run dirs:** inputs `C:\GnssProcessor\data\2024-129\obs` (311 stations) and `D:\GnssProcessorSupport\data_mday\2024-{128,130,131,132}\obs` (75 stations); outputs `D:\GnssProcessorSupport\runs\l2align\` (`headers.csv`, `arcs.csv`, `arcs_files.csv`, `grade\`); script `scripts/l2_quarter_cycle.py` (selftest 17 of 17); worktree branch `l2/quarter-cycle`

**Outcome.** Question: do the field receivers put the L2C phase a quarter of a cycle from the L2W phase, by receiver type and firmware, and does the code need a correction before integer work moves to the field? Answer: no. The L2C minus L2W fraction is 0 at all 579 station-days of 21 receiver types (offsets -0.007 to +0.018 cycle, repeatable to 0.005 cycle over five days), with none within 0.05 cycle of a quarter target, against a uniform null whose mean distance to the nearest quarter target is 0.0625 cycle (measured 0.0055). The header records document three different conventions for the same aligned data, so the knob that subtracts them must stay off. The latch gives every golden station L2W, and a station-consistent offset cannot reach a double difference.

## 1. The question, and what is formed

GPS L2C (RINEX attributes S, L, X) and the semi-codeless P(Y) phase (L2W) are tracked on one
carrier; at the satellite the two signals are in phase quadrature. A receiver that reports both
raw may therefore place its L2C phase a quarter of a cycle from its L2W phase, and RINEX 3
requires the writer to align them and to document the applied shift in the SYS / PHASE SHIFT
header record. The register item asked whether the files of the field network are aligned,
per receiver type and firmware, because a station-consistent quarter cycle on one signal would
be invisible to the float vehicle and visible to any integer that mixes the two signals.

The instrument is the single difference between the two phases of one satellite at one station
and one epoch, d = L2C - L2W in cycles, both phases as recorded. Geometry, satellite and
receiver clocks, troposphere, ionosphere (one frequency), phase wind-up and the antenna
patterns of satellite and receiver cancel exactly. What remains is an integer (the two
ambiguities), the alignment shift, the inter-signal phase bias between the two tracking modes
at the receiver and at the satellite, and noise with multipath. The fractional part of d is the
observable: it is invariant to an integer slip on either signal, so an arc is ended only by a
gap over 300 s, a loss-of-lock flag (LLI bit 0) on either signal, or a wrapped step of d above
0.15 cycle between consecutive epochs (a half-cycle event or a realignment). Epochs flagged
half-cycle ambiguous (LLI bit 1) are counted and left out. An arc of at least 20 epochs (10 min
at 30 s) yields one circular mean of the fraction; a station-day's offset is the circular mean
over its arcs, its uncertainty the arc-to-arc scatter over the square root of the number of
arcs. Epochs within an arc are not independent (multipath); arcs nearly are.

What the difference cannot remove is reported, not assumed: the receiver inter-signal bias is
the station offset itself; the satellite inter-signal bias is the per-satellite structure left
after each station-day's offset is removed (section 3.4); noise and multipath are the
within-arc scatter.

## 2. Data, method and the null

**Files.** 608 RINEX observation files: the 311 stations of DOY 129 held on C: and the 75
stations of DOY 128, 130, 131 and 132 held on D: (74, 75, 75 and 73 files present). Versions:
3.04 (394), 3.05 (128), 3.03 (47), 3.02 (30), 4.00 (6), 4.01 (1), 3.01 (1), 2.11 (1, CIT1,
listed and not analysed: RINEX 2 does not separate L2C from L2W). 580 station-days hold L2W and
an L2C code in the header; 27 files of 15 stations report L2W alone (AJAC00FRA, AUCK00NZL,
CHTI00NZL, DUND00NZL, GRAC00FRA, HERT00GBR, MARS00FRA, MDVJ00RUS, MQZG00NZL, NVSK00RUS,
PENC00HUN, SCTB00ATA, WARK00NZL, WGTN00NZL, WUHN00CHN); no station reports L2C without L2W.
One L2C attribute per file: L2L at every Septentrio and Topcon receiver, L2X at every Trimble
and JAVAD receiver, L2S at every Leica and Stonex receiver (two Trimble NetR9 files carry all
three).

**Sweep.** 1 667 108 epochs, 15 660 507 GPS satellite-epochs read, 34 482 L2C-L2W arcs of at
least 20 epochs at 579 station-days of 294 stations (one station-day with the pair in its
header yields no arc of 20 epochs). Arc breaks: 19 956 gaps, 134 309 loss-of-lock flags, 18 820
jumps; 149 791 fragments under 20 epochs were dropped. The side pair L1C/L1L/L1X minus L1C
(C/A) gives 5 240 arcs at 374 station-days (section 3.6). 38 548 satellite-epochs have the two
phases equal to the printed millicycle; they occur in 468 of 608 files, at most 890 in a file
(3 % of its satellite-epochs), never a whole file, so no converter duplicates a column, and
they are kept as data.

**The null.** A fraction is defined modulo one cycle, and the wrapped-fraction traps apply
(CLAUDE.md, "Grading traps"): no group is centred on its own circular median before a
dispersion is quoted. The targets are fixed in advance: 0 (aligned), +0.25 and -0.25 (a
quarter cycle either way), 0.5 (a half cycle). Concentration at the quarter targets is tested
on the angle 4 x 2 pi x frac (a Rayleigh test; a set at any mix of the four targets gives a
resultant near 1), and the mean distance to the nearest quarter target is quoted against the
same statistic on size-matched uniform fractions, whose expectation is 1/16 cycle; the mean
distance to 0 is quoted against its uniform value of 1/4 cycle. A station-day is assigned to a
target only when its offset lies within 0.05 cycle of it.

**Reproduction.**

```
python scripts/l2_quarter_cycle.py selftest
python scripts/l2_quarter_cycle.py headers <obs dirs> --out D:/GnssProcessorSupport/runs/l2align/headers.csv
python scripts/l2_quarter_cycle.py arcs    <obs dirs> --out D:/GnssProcessorSupport/runs/l2align/arcs.csv --workers 16
python scripts/l2_quarter_cycle.py grade --headers ...headers.csv --arcs ...arcs.csv --out ...grade --golden config/golden/golden_field_refound.json
```

The obs dirs are `C:/GnssProcessor/data/2024-129/obs` and
`D:/GnssProcessorSupport/data_mday/2024-{128,130,131,132}/obs`. The sweep takes about three
minutes on 16 workers; satellite identifiers go through `scripts/rinex_ids.py`, and the script
is registered in that module's reader list.

## 3. Results

### 3.1 Per receiver type

L2C minus L2W, every day pooled. "offset" is the circular mean of the station-day offsets of
the type, "max dev" the largest wrapped distance of a station-day from that mean, "at 0" the
station-days within 0.05 cycle of 0; the columns for +0.25, -0.25 and 0.5 are 0 in every row
and are not printed. "header" is the value the files record for the L2C code (section 4).

| receiver | station-days | stations | arcs | offset (cyc) | max dev (cyc) | at 0 | pair | header |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| SEPT POLARX5 | 177 | 99 | 10 855 | +0.0036 | 0.0116 | 177 | L2L-L2W | 0.00000 (172), none (5) |
| TRIMBLE ALLOY | 85 | 38 | 5 471 | +0.0070 | 0.0062 | 85 | L2X-L2W | -0.25000 (74), blank (7), none (5) |
| JAVAD TRE_3 DELTA | 75 | 23 | 4 624 | +0.0002 | 0.0074 | 75 | L2X-L2W | +0.25000 (75; 70 with a satellite list) |
| SEPT POLARX5TR | 58 | 34 | 2 994 | +0.0036 | 0.0037 | 58 | L2L-L2W | 0.00000 |
| TRIMBLE NETR9 | 43 | 27 | 2 574 | +0.0049 | 0.0183 | 43 | L2X-L2W | -0.25000 (31), none (12) |
| LEICA GR50 | 37 | 21 | 2 232 | +0.0026 | 0.0029 | 37 | L2S-L2W | -0.25000 (37; 1 with a satellite list) |
| JAVAD TRE_3 | 32 | 8 | 1 571 | -0.0017 | 0.0026 | 32 | L2X-L2W | +0.25000 with a satellite list |
| JAVAD TRE_G3TH DELTA | 11 | 7 | 533 | +0.0028 | 0.0050 | 11 | L2X-L2W | +0.25000 (11; 9 with a satellite list) |
| JAVAD TR_G3TH | 10 | 2 | 528 | +0.0014 | 0.0022 | 10 | L2X-L2W | +0.25000 with a satellite list |
| SEPT ASTERX4 | 9 | 5 | 446 | +0.0052 | 0.0077 | 9 | L2L-L2W | 0.00000 |
| LEICA GR10 | 8 | 4 | 438 | +0.0032 | 0.0020 | 8 | L2S-L2W | -0.25000 |
| LEICA GR30 | 8 | 8 | 485 | +0.0031 | 0.0015 | 8 | L2S-L2W | -0.25000 |
| SEPT POLARX5S | 8 | 4 | 594 | -0.0005 | 0.0079 | 8 | L2L-L2W | 0.00000 (7), none (1) |
| SEPT POLARX4TR | 6 | 2 | 291 | -0.0006 | 0.0023 | 6 | L2L-L2W | 0.00000 |
| JAVAD TRE_3N DELTA | 3 | 3 | 168 | +0.0031 | 0.0018 | 3 | L2X-L2W | +0.25000 |
| LEICA GR25 | 3 | 3 | 277 | +0.0011 | 0.0007 | 3 | L2S-L2W | -0.25000 |
| TPS NET-G5 | 2 | 2 | 94 | +0.0038 | 0.0016 | 2 | L2L-L2W | +0.25000 |
| JAVAD TRE_G3T DELTA | 1 | 1 | 111 | +0.0005 | | 1 | L2X-L2W | +0.25000 with a satellite list |
| SEPT POLARX5E | 1 | 1 | 61 | +0.0052 | | 1 | L2L-L2W | 0.00000 |
| STONEX SC2200 | 1 | 1 | 64 | +0.0005 | | 1 | L2S-L2W | -0.25000 |
| TPS NET-G3A | 1 | 1 | 71 | +0.0019 | | 1 | L2L-L2W | +0.25000 |
| **all** | **579** | **294** | **34 482** | | | **579** | | |

Pooled over all 34 482 arcs: mean distance to the nearest quarter target 0.0055 cycle (uniform
null 0.0625; the size-matched null of 5 000 uniform fractions has mean 0.0625 and 1st
percentile 0.0614); mean distance to 0: 0.0055 cycle (uniform null 0.2500); Rayleigh resultant
on 4 x the angle 0.9841, on the angle itself 0.9989. The mass is at 0, not spread over the
four targets.

Per firmware (61 groups of receiver type and firmware string) and per converter (72 groups of
receiver type and the program of PGM / RUN BY / DATE): every group's offset is within 0.011
cycle of 0 and no station-day in any group lies off 0. No firmware and no converter among the
files differs in alignment.

### 3.2 Station-days

All 579 station-days are at 0. The offsets run from -0.0068 (YKRO00CIV) to +0.0176 cycle
(SAVO00BRA); the median |offset| is 0.0037 cycle. Twelve station-days exceed 0.012 cycle, all
Trimble: SAVO00BRA (NetR9 5.60, 0.014 to 0.018 on all five days), CPVG00CPV (Alloy, 0.013 on
three days), BELE00BRA, DAE200KOR (-0.013), RIO200ARG and YAR200AUS. These are the receiver
inter-signal phase bias between the two tracking modes, at most 4.3 mm on L2, constant over
the five days. The standard error of a station-day offset is 0.0006 cycle at the median
(largest 0.0113, VOIM00MDG on DOY 132, whose arcs are noisy); arcs per station-day 13 to 139
(median 55); the largest Rayleigh p-value on 4 x the angle at any station-day is 1.3e-6. The
within-arc scatter (the noise and multipath of the difference) is 0.0045 cycle rms at the
median station-day (1.1 mm), 0.010 at the 90th percentile.

### 3.3 Where the arc fractions lie

| band of the arc fraction | arcs | share |
| --- | ---: | ---: |
| within 0.05 of 0 | 34 432 | 99.85 % |
| 0.05 to 0.15 from 0 | 49 | 0.14 % |
| within 0.05 of +0.25 or -0.25 | 0 | 0 |
| within 0.05 of 0.5 (a half cycle) | 1 | 0.003 % |
| elsewhere | 0 | 0 |

The 18 820 arcs opened by a jump of the difference return to 0 (2 922 of the 2 927 that reach
20 epochs lie within 0.05 of 0): the jumps are single noisy epochs, not realignments. The one
half-cycle arc (JAVAD TR_G3TH at VOIM00MDG, DOY 132) is the only half-cycle ambiguity left in
34 482 arcs; the LLI half-cycle flag removed 0 epochs network-wide, so the L2W tracking of
these receivers resolves its half cycle before it reports.

### 3.4 The satellite part

After each station-day's own offset is removed, the residual per satellite over all stations
and days (25 satellites with L2C; G02, G13, G16, G19, G20, G21 and G22 transmit none) lies
between -0.0037 (G29) and +0.0036 cycle (G24), standard error at most 0.0003 cycle (1 334 to
1 566 arcs per satellite). Per receiver type with at least 10 arcs on the satellite (357
rows), the largest is 0.0073 cycle (SEPT POLARX5S on G10, 22 arcs; SEPT POLARX4TR on G25,
-0.0072, 12 arcs). The satellite-side inter-signal phase bias between L2C and L2P(Y), as the
receivers see it, is therefore below 0.004 cycle pooled and below 0.008 cycle (2 mm) for any
receiver type. This is the one term of the difference that a double difference between two
stations on different L2 signals does not cancel (section 6).

### 3.5 Repeatability and the golden roster

The 72 stations present on two or more days repeat their offset to 0.0054 cycle at worst
(median spread 0.0014 cycle). Of the 59 stations of the field golden (58 producer stations and
the held-out consumer WTZZ00DEU), 57 hold both signals on DOY 129 and all 57 are at 0
(-0.0068 to +0.0172 cycle; WTZZ00DEU, JAVAD TRE_3 DELTA with L2X, +0.0017); SCTB00ATA and
WARK00NZL (Trimble Alloy) report L2W alone. The receiver mix of the roster is Septentrio 24,
JAVAD 15, Trimble 14, Leica 4 (section 7 lists the per-station values in
`grade/golden_coverage.csv`).

### 3.6 The side pair on L1

The same difference between the L1C-family phase (L1L at Septentrio and Leica, L1X at JAVAD
and Trimble) and the C/A phase L1C, 5 240 arcs at 374 station-days of 10 receiver types: every
station-day at 0, type offsets -0.0024 to +0.0037 cycle, the largest station-day deviation
0.017 cycle (a SEPT POLARX5). The files record 0.00000 (Septentrio), +0.25000 (Trimble,
Leica) and -0.25000 (JAVAD, with a satellite list) for the L1C-family code.

## 4. The header records against the data

**What the specification says** (RINEX 3.04, IGS/RTCM, 2018-11-23; the 3.05 text of
2020-12-01 is the same on these points). Section 9.1: all phase observations must be aligned in
3.01 and later files and the record is mandatory; the correction is reported "to allow the
reconstruction of the original values"; the uncorrected reference signal's record is left
blank; a value of zero means the receiver or the data stream had already aligned the data and
the converter applied nothing; a blank observation-code field with the rest blank means the
alignment is unknown. Table 25 gives the sign: RINEX phase = original phase + the recorded
correction, the original being the phase "as issued by the GNSS receiver or in a standardized
data stream such as RTCM-MSM". Table A23 names the reference per band: for GPS L2 the
reference is L2P, with L2W at "None", and L2S, L2L and L2X carry a correction of -1/4 cycle;
for GPS L1 the reference is L1C (C/A) and L1S, L1L and L1X carry +1/4 cycle. Table A2 defines
the record: system, observation code, "Correction applied (cycles)" in F8.5, the number of
satellites (0 or blank for all) and their list, continued on further records.

**What the files record for the L2C code, by family.**

| family (converter) | record | reading under the specification | data |
| --- | --- | --- | ---: |
| Septentrio PolaRx5 / 5TR / 5S / 5E / 4TR, AsteRx4 (sbf2rin, ssrcrin, RTCM2RINEX) | L2L 0.00000, L2W blank | aligned by the receiver, nothing applied | 0 |
| Trimble Alloy (Alloy, cnvtToRINEX, Converto, CONVBIN) and NetR9 (NetR9, cnvtToRINEX) | L2X -0.25000, no L2W record | -1/4 applied, the Table A23 sign | 0 |
| Trimble Alloy (BINEX2RINEX, 7 files) | L2X blank | alignment unknown | 0 |
| Trimble Alloy (5 files), NetR9 (dconv, TPP; 12 files), Septentrio (6 files) | no record for the L2C code | the mandatory record missing | 0 |
| JAVAD TRE_3, TRE_3 DELTA, TRE_3N DELTA, TRE_G3TH DELTA, TRE_G3T DELTA, TR_G3TH (JPS2RIN, MAKERINEX, RTCM2RINEX) | L2X +0.25000, mostly with the list of the 25 satellites that transmit L2C; L2W 0.00000 with all 32 | +1/4 applied, the sign opposite to Table A23 | 0 |
| Topcon NET-G5, NET-G3A (RTCM2RINEX) | L2L +0.25000 | +1/4 applied, the sign opposite to Table A23 | 0 |
| Leica GR10 / GR25 / GR30 / GR50 (GR firmware, Spider, Converto) and Stonex SC2200 | L2S -0.25000 | -1/4 applied, the Table A23 sign | 0 |

Every family's data are aligned (column "data": the measured fraction, section 3), whatever
the record says. Three records describe the same aligned relation: 0.00000 where the receiver
aligned internally, -0.25000 and +0.25000 where a converter did it and documented opposite
signs. The same holds on L1 (section 3.6), with the signs swapped as Table A23 swaps them.
Under the specification's own formula the "original" phase reconstructed by subtracting the
record would differ by half a cycle between a JAVAD file and a Trimble file for the same
physical relation; the data do not say which converter has the sign right, only that both
aligned the phases. Where the record is blank or missing (24 station-days), the data are
aligned too.

## 5. How the code reads the record

`src/core/rinex/RinexParser.cpp:487` parses every SYS / PHASE SHIFT record: a system-wide value
into `phase_shifts`, a value with a satellite list (continuation records included, blank-padded
identifiers zero-filled) into `phase_shifts_by_sat`. The values are applied in one place,
`RinexParser.cpp:778`: when `model_corrections.apply_phase_shifts` is true, the recorded value
is subtracted from every phase observation of that code (a per-satellite entry first, else the
system-wide one); code observations are never touched. The knob defaults to false
(`src/core/config/GnssConfigLoad.cpp:2405`), is documented at
`include/config/GnssConfigTypes.hpp:5358` as recovering "the unshifted phase convention" for
bias products calibrated against it, has no row in `docs/CONFIG_promotion_ledger.json`, and is
set false explicitly in `config/ppp_rtk_internal_AR.json` and `config/realtime_ppp_rtk_AR.json`.
No golden names it, so every network run keeps the phases as recorded.

With the knob off the parser's reading matches the data: the files are aligned and are used
aligned. With the knob on, the Trimble, Leica and Stonex files would have +0.25 cycle added to
their L2C phase (subtracting -0.25), the JAVAD and Topcon files -0.25 cycle, and the Septentrio
files nothing: the L2C phase would then sit a quarter of a cycle from L2W at 162 of the 294
stations, in two opposite directions. The knob's premise, one receiver-native convention
recovered network-wide, is not available either, because the recorded signs differ between
families for the same relation (section 4). The knob is therefore wrong for this network in
both directions and is to stay off; its ledger row (verdict KEEP_OFF) is owed, the author's
decision as every ledger change.

The user-side `scripts/rinex_edit.py` copies the records through unchanged when it merges or
decimates files (`build_header`), so an edited fixture keeps its writer's declaration.

## 6. What a misalignment would do on this codebase

**One signal per band per station, decided once.** The canonical latch
(`PppModel::decideCanonicalPicks`, `src/core/model/PppModel.cpp:446`) picks, per station,
constellation, band and observation type, the first signal of the priority list whose
satellite count at the first epoch reaches `processing.canonical_signal_min_coverage` (default
0.5, `GnssConfigLoad.cpp:355`) of the band's largest count, under the golden's
`canonical_signal_rule: priority`. The latch is decided once per station and constellation
(`constellation_canonical_decided_`); the declared-roster follow of
`network.closed_loop.roster_latch` (default false, absent from the golden) acts only at that
decision. At every later epoch a satellite that lacks the latched signal has the band skipped
for that satellite (`PppModel.cpp:1292`, "selectUncombinedSignals skips the band for this
sat"); no substitute attribute is ever taken. The field golden's GPS list holds C2W and L2W
before C2S, C2L, L2S and L2L and no 2X, and every station of this data set tracks L2W, so the
latch is L2W everywhere: the run log of `runs_nom/_rap_bdsc_c6` (the BeiDou golden arm, the
same GPS posture) shows "Canonical latch for GPS" at 59 stations with L2L=L2W at all of them
(31 as the sole L2 phase, 24 over L2L, 4 over L2S), and its "signals as planned" table reads
band 2: phase L2W 58, excluded not_latched L2X 28, L2L 24, L2S 4. Within one station, L2C and
L2W are never mixed across satellites, by construction.

**Across stations, a station-consistent offset cancels.** Suppose station B's latched signal
carried a station-consistent quarter cycle delta_B (a file misaligned, or a receiver bias) and
station A's did not. The term enters every arc of B on that band equally, so it cancels in B's
between-satellite single difference and hence in every double difference (A, B; s, p),
whatever signal A uses. In the float vehicle it is absorbed by the arcs' ambiguities together
with B's receiver phase-bias column of that signal (`StateKeys::makeReceiverPhaseBiasKey`,
`include/filter/StateKeySchema.hpp:576`; `findReceiverPhaseBias` in
`include/network/state/StationParams.hpp`), and it never reaches a satellite state or the
wire. The producer's SD-EWL route (`include/network/ar/SdEwlAr.hpp`, "Exactness precondition:
channel identity") additionally elects one network-common channel pair per satellite and
admits only the stations holding it, so there the two signals do not meet at all. A latch that
changed mid-run would be a station-wide event on every arc of the band at once; the golden has
no such path (the latch is decided once).

**What survives.** The only term a double difference between a station on L2W and a station
on L2C does not cancel is the satellite-dependent part of the L2C minus L2P(Y) inter-signal
phase bias, measured here at or below 0.004 cycle pooled and 0.008 cycle (2 mm) for any
receiver type (section 3.4). On the golden posture it does not arise (every station on L2W);
on a posture that admits an L2C-only station it is below the resolution of the integer ladder
([ARL](ARL_integer_ambiguity_ladder_2026-10-09.md): a narrow lane needs about an hour of arc
at the field code noise). A file misaligned by a quarter cycle on L2W itself (the reference,
blank in every record) would be a station-wide term of the same kind and would cancel the same
way; this analysis cannot see it, because both phases of the difference would carry it.

**Not examined.** The real-time path reads MSM phase ranges from the receiver with no RINEX
writer between them; alignment there is the receiver's (the Septentrio records of 0.00000 say
those receivers align internally), and the same difference can be formed on a recorded stream
when a real-time integer posture is prepared. Other constellations: the Galileo E1 and E5a
postures track one attribute per band in the goldens (C1C/L1C, C5Q/L5Q, with the X variants as
alternatives), and the question of two attributes on one carrier was not asked of them here.

## 7. Verdict

1. **No correction is needed** for field integer work on the files of DOY 128 to 132 as
   recorded: the L2C and L2W phases are aligned at every station-day of every receiver type,
   firmware and converter, to within the receiver inter-signal bias (at most 0.018 cycle,
   constant over the five days) and the satellite inter-signal bias (at most 0.004 cycle).
2. **Two conditions keep it so.** `model_corrections.apply_phase_shifts` stays false (on, it
   un-aligns 162 of 294 stations in two directions; a KEEP_OFF ledger row is owed), and files
   are used as recorded. A new station, firmware or converter is read with
   `scripts/l2_quarter_cycle.py` before it joins a roster that admits an L2C attribute; the
   station table (`grade/stations.csv`) is the per-station record of this data set.
3. **The codebase is already safe against the failure the item feared.** The latch gives one
   L2 signal per station for all satellites (L2W at every golden station), a station-consistent
   offset cancels in every double difference and is absorbed by the float vehicle, and the
   SD-EWL route refuses to mix channels across stations. What a mixed posture would see is the
   satellite part, 2 mm at most.
4. **The header records are not a usable instrument** on their own: three conventions
   describe the same aligned data, and 24 station-days carry a blank or missing record. The
   data decide, and the script is the check.

The register item leaves [OPEN_ITEMS](../../status/OPEN_ITEMS.md) with this card.

## 8. The ledger row, 2026-11-06

The KEEP_OFF row for `model_corrections.apply_phase_shifts` that sections 5 and 7 record as owed
landed the same day at the author's decision, in a second commit on branch `l2/quarter-cycle`
(the first, f22e2eb, holds the card and the script): verdict KEEP_OFF, area `model_corrections`,
default false, evidence this card. The goldens were not regenerated: no golden names the key and
a KEEP_OFF row pins nothing, so their content is unchanged, and the ledger hash in their
provenance still records the generation of 2026-11-02, as after every earlier row-only ledger
change. With the row, `python scripts/ledger_render.py` renders 686 keys (98 KEEP_OFF),
`python scripts/make_golden_config.py --verify` passes on the three goldens,
`python scripts/config_posture_check.py --verify-loader` reports 0 coverage gaps, and
`python scripts/docs_index_check.py` passes.
