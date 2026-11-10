# BRD3B: the clock-share requirement on the clock anchor's phase trip, built and graded (continuation of card BRD3)

**STATUS**

- **id:** BRD3B
- **topic:** network-vehicle
- **status:** closed
- **summary:** Card BRD3's continuation (that file nears the 60 KB bar): the build card BRD3 pre-registered in its section 4, graded against the gates frozen there on 2026-11-06; card BRD3 keeps the mechanism, the gates, the build's description and how the gates are read on the current main (its sections 4, 8.1 and 8.2). `network.clock_anchor_clock_k` withholds a phase-statistic trip whose clock-space statistic is below the value. At 2.0 on card BRD's tracked posture, rebuilt on the 2026-11-07 golden, it takes 0, 0, 1 and 0 trips on DOY 129 to 132 and withholds 1, 7, 1 and 28. The three days without a trip are byte-identical to the test-off arm; DOY 131 meets the day bars; 8 of 64 site-days are worse against the GPS golden (bar 20 %). The injected clock step still trips (|w_clk| 7.92), the sim legs are byte-identical with and without the key, and storm `c` withholds exactly its 8 key-carried trips. X-U0 and X0 to X4 pass; X5 fails on all three clauses in its letter (two of 28 withheld trips under a 0.9 keys' share, two over 20 mm of clock innovation, 28 withheld against 26). Nothing armed.
- **source:** card [BRD3](BRD3_anchor_decoupling_clock_share_2026-11-06.md) sections 2.3 (the clock-space share of every trip), 4 (the mechanism, the gates, what is not built), 5.1 (the step legs and their seeds), 5.2 (the 26 trips of DOY 132 read apart) and 8 (the build and how the gates are read); card [BRD2](BRD2_broadcast_remainder_hold_days_2026-11-03.md) sections 4 and 5.5 (gate D, the test-off posture); card [BRD](BRD_broadcast_referenced_level_2026-11-02.md) (the tracked posture); card [SMX](SMX_scenario_matrix_2026-10-24.md) (the matrix); standing invariants 1, 4 and 7 of `CLAUDE.md`
- **knobs / posture:** `network.clock_anchor_clock_k` (default 0 = off; ledger row UNVALIDATED, parent `network.clock_anchor_guard_space`). Graded posture: card BRD's T2 recipe on the 2026-11-07 golden (card BRD3 section 8.2), the key at 2.0
- **dates in the text:** 2026-11-09, 2026-11-10
- **hashes / run dirs:** branch `brd3/clock-share` from main `27d6ece`, worktree `D:/GnssProcessorSupport/worktrees/brd3k`; exes `gnss_processor_brd3kb.exe` (the base, sha256[:16] `5c2d8cc06c27acc7`) and `gnss_processor_brd3k.exe` (the key, `2134f60773a4de43`); arms `D:/GnssProcessorSupport/runs_nom/_rap_brd3k_{c129_2,s0_2,t132_2}{b,n}`, `_rap_brd3k_{x,tk,g}{129,130,131,132}_6`, `_rap_brd3k_rx{131,132}_6`, `_rap_brd3k_{x,g}132_24`; census replays `_rap_b3{x,tk,g}<doy>_<site>` and `_rap_b3{x,g}24_<site>`; sim labels `brd3kx` and `brd3k0` (`D:/GnssProcessorSupport/gates/smx/`, legs `evleg_smx_brd3k{x,0}_*`); day configs `D:/GnssProcessorSupport/runs_nom/_brd3k_cfg/`; tools in `D:/GnssProcessorSupport/worktrees/brd3k_tools/` (outside the repository): `make_arms.sh`, `ident.py`, `x_read.py`, `brd_census.py`, `brd_census24.py`, `brd_pool.py`, `brd_run.py`
- **gates (frozen at pre-registration):** card BRD3 section 4.2 (2026-11-06), read as card BRD3 section 8.2 states

**Outcome.** Question: does the clock-share requirement withhold the clock anchor's key-carried trips on the broadcast-referenced posture while keeping a real satellite clock step? Yes. On the tracked posture the key takes 1 trip in four days (DOY 131's G22, at a clock share of 2.06) and withholds 37, every one with |w_clk| at most 1.10; the days pass their bars and the pooled share (12 %). The injected step still trips, and the storm withholds only its key-carried trips. Gate X5 fails in its letter. Nothing armed.

## 1. The question and the method

Card BRD3 found every statistic trip of the clock anchor on the broadcast regime's DOY 132 carried by the phase-bias keys
(clock-space |w| at most 0.57, where a true 0.5 m step at the same q reads 7.94) and pre-registered a requirement on the
trip: in phase mode a statistic trip of an anchored satellite needs |w_clk| >= `network.clock_anchor_clock_k` as well as
|w_phi| over k. This card grades that build against card BRD3's gates X-U0 and X0 to X5, moving no bar. The build, its
events and counters, and how each gate is read on the current main (same-exe twins in place of card BRD2's 2026-11-03
references, the tracked posture rebuilt on the 2026-11-07 golden, the sim legs on card BRD3's 2026-11-06 seeds) are card
BRD3's section 8, written before any arm was read.

## 2. Results, gate by gate (2026-11-09)

Every number below was read from the run directories named. Hours from 00:00; the census is the mean of each site's
hourly p50 over hours 2 to 6, paired per site; "worse" means worse by more than 0.02 m.

**X-U0.** Debug `gnss_tests`: 4 196 tests in 554 suites, 4 159 passed, 37 fixture-gated skips, 0 failed (the 19 new tests in
one new suite, `ClockAnchorClockShare`, `tests/network/test_clock_anchor_clock_share.cpp`); `gnss_simulator_selftest` 99 of
99; `make_golden_config.py --verify` PASS; `docs_index_check.py` passes; `nom_epoch_lint.py` PASS; `config_posture_check.py
--verify-loader` 0 gaps; `nom_n4_verify.py --whole` PASS. The guard tests run the guard itself on a synthetic network of four
satellites with a clock and two phase-bias bands under a dense prior: a 20 m move of one metre-sd key with the clock at the
prior trips the phase statistic without the key (`reason=phi`, the satellite decoupled and in the hold's trip set) and is
withheld with it (the satellite anchored, no mark, no re-solve, one `phi_withheld` row, the decision column `withheld`); a
0.5 m clock step in the data still trips with the key (standing invariant 1); the absolute backstop and the satellite clock
event's forced trip decide a withheld row as before; a LOOSE row's re-validation is unchanged; an episode opens, extends and
closes clear, or closes by a decoupling, each edge an event. **X-U0 PASS.**

**X0, off.** The final exe with the key off against the base exe (card BRD3 section 8.2), `products.txt` without comment lines,
`closed_loop_trace.csv` and `network_epoch_status.csv`:

| arm | products lines | the three files |
| --- | --- | --- |
| 2 h field golden, DOY 129 (`brd3k_c129_2n` against `brd3k_c129_2b`) | 115 712 | identical |
| 2 h sim golden from 00:30:15 (`brd3k_s0_2n` against `brd3k_s0_2b`) | 80 690 | identical |
| tracked posture, 2 h, DOY 132 (`brd3k_t132_2n` against `brd3k_t132_2b`) | 58 605 | identical |

The other files of `logs/` that differ do so in wall-clock columns (`ms`, `loop_ms`, `ms_per_epoch_window`), the build stamp,
and the run's own paths and the configuration hash that contains them. **X0 PASS** (on the same-exe twins of card BRD3 section 8.2).

**X1, the refusals.** The loader refuses the key at `clock_anchor_q` 2.9e-8 and accepts it at 5e-6 and at the bar 1e-6;
refuses it in clock mode and in shadow mode; refuses it with the anchor off; refuses a negative value; each refusal beside a
control that differs only in the condition and loads. **X1 PASS.**

**X2, the four days.** The tracked posture with the key at 2.0 (`brd3k_x{129..132}_6`), the test-off arm
(`brd3k_tk{129..132}_6`) and the GPS golden of the day (`brd3k_g{129..132}_6`), all on the final exe:

| day | trips taken | withheld (episodes), max \|w_clk\| among them | key arm against the test-off arm | census against the GPS golden (worse) | WTZZ00DEU hours 2 to 6 against the golden |
| --- | --- | --- | --- | --- | --- |
| 129 | 0 | 1 (1), 0.01: G11 at 2.90 h | byte-identical | -0.0003 m (1) | 0.0151 against 0.0229 m (-0.0078) |
| 130 | 0 | 7 (7), 1.10: six satellites at 2.09 h, G05 at 3.25 h | byte-identical | +0.0023 m (4) | 0.0295 against 0.0493 m (-0.0197) |
| 131 | 1: G22 at 3.84 h, \|w_clk\| 2.06, dclk +67 mm, \|w_phi\| 12.7 | 1 (1), 0.12: G12 at 3.99 h | differs (the trip) | +0.0013 m (2) | 0.0310 against 0.0274 m (+0.0036) |
| 132 | 0 | 28 (26), 0.58: the 26 trips of card BRD3 section 5.2 | byte-identical | -0.0019 m (1) | 0.0164 against 0.0206 m (-0.0042) |
| pooled | 1 | 37 (35) | | +0.0006 m, **8 of 64 site-days worse (12 %)** | |

Card BRD3 section 2.3's prediction holds exactly: 0 trips taken on DOY 129, 130 and 132 and 1 on DOY 131, the same trip population on
today's golden as on the golden of 2026-11-03 (1, 7, 2 and 26 statistic events, at the same epochs and satellites on DOY 132).
On the three days without a trip `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv` are byte-identical to
the test-off arm (176 541, 176 348 and 176 115 product lines without comment lines), so the test-off arm's census is the key arm's. On DOY 131 the
key arm against the GPS golden reads +0.0036 m at WTZZ00DEU (bar +0.005) and +0.0013 m at the census median (bar +0.005), and
against the test-off arm +0.0000 m at the census (0 sites moved by 0.01 m) and +0.0010 m at WTZZ00DEU (bar 0.003); the one
taken trip is G22's, with a clock share of 2.06. Pooled over the four days 8 of 64 site-days are worse (bar 20 %): BAKE00CAN
on DOY 129; QUIN00USA, POHN00FSM, UTQI00USA and BAKE00CAN (+0.021 to +0.044 m) on DOY 130; SCRZ00BOL and POHN00FSM on DOY 131;
UTQI00USA (+0.118 m) on DOY 132. **X2 PASS.**

Per day, WTZZ00DEU by hour (m), GPS golden then key arm: DOY 129 0.115 0.024 0.017 0.018 0.030 0.027 / 0.099 0.014 0.016
0.010 0.019 0.016; DOY 130 0.061 0.024 0.026 0.050 0.076 0.070 / 0.125 0.021 0.018 0.021 0.047 0.041; DOY 131 0.087 0.027
0.017 0.027 0.037 0.029 / 0.151 0.050 0.013 0.016 0.037 0.038; DOY 132 0.087 0.026 0.021 0.017 0.025 0.014 / 0.078 0.017
0.012 0.014 0.024 0.016. Census median by hour, same order: DOY 129 0.391 0.097 0.039 0.045 0.036 0.035 / 0.261 0.067 0.051
0.051 0.045 0.051; DOY 130 0.263 0.081 0.039 0.047 0.042 0.043 / 0.197 0.068 0.060 0.052 0.038 0.044; DOY 131 0.204 0.060
0.052 0.046 0.048 0.038 / 0.188 0.073 0.057 0.048 0.038 0.044; DOY 132 0.191 0.055 0.046 0.035 0.037 0.030 / 0.215 0.062
0.041 0.034 0.034 0.033. Hour 1, census paired median against the golden: -0.0048, -0.0380, -0.0022, +0.0040 m (pooled
-0.0045). Decouplings per arm 0, 0, 1, 0 (the test-off arms 0); 30, 30, 31, 30 re-coupling events (the first engagements and,
on DOY 131, G22's at 445950); `PB_HOLD gen_start` rows 105 on every key arm and every test-off arm.

**X3, detection kept.** Labels `brd3kx` (the key at 2.0) and `brd3k0` (without it), one exe (the final exe), the seeds of
card BRD3 section 5.1:

- posture `c`: G25's trip at the step epoch (275910) is taken, |w_phi| 15.10, **|w_clk| 7.92** against 2.0, dclk -132.5 mm,
  and `evleg_smx_brd3kx_sat_clk_c` is byte-identical to `evleg_smx_brd3k0_sat_clk_c` (the three files);
- posture `n`: no trip of G25 in either label; the leg's one trip, G05 at 276900 (|w_clk| 6.74, dclk +125 mm), is in its null
  too and is taken in both; byte-identical;
- posture `t`: no trip of G25 (the column and the track take the step, as in card BRD3 section 5.1); the four trips at the resume
  epoch 275610 (G24, G10, G03, G32) carry |w_clk| 7.1 to 9.2 and are taken; byte-identical.

Every null leg (60 and 240 epochs) of both labels is byte-identical too. The matrix's verdicts are the same in both labels:
`sat_clk` on `c` FAIL (clk 35.1 against 5.0 mm), on `n` FAIL (cons_peak 1 139.0 against 28.3 mm), on `t` PASS; the two fails
are the seeds' and the key changes no byte of them. **X3 PASS.**

**X4, the storm.** On `n` both labels trip 75 times, none with |w_clk| below 2 (smallest 3.10): 0 of 75 withheld, as
predicted, and the legs are byte-identical. On `c` the knob-off leg trips 74 times (75 on card BRD3 section 2.3's exe), 8 with |w_clk|
below 2, all on G21 (|w_clk| 0.06 to 1.91). With the key those 8 trips are withheld, in 8 episodes of 15 withheld epochs (G21
stays anchored, and in one episode its phase statistic stays over k for 7 epochs, 276210 to 276390); the other 66 trips are
taken in both legs, and G21 trips twice more with a clock share: at 275970 (|w_clk| 4.71), where the knob-off leg already
held it LOOSE, and at 279480 (|w_clk| 3.35), where the knob-off leg, on another path since the first withhold, had it anchored
and in band. Of the 15 withheld rows 13 have a keys' share of at least 0.9; the clock's own innovation is at most 27.2 mm.
No cell changes verdict (`storm` FAIL on `c` and `n` in both labels, as in card SMX). The storm cells' clk and consumer
channels, key against knob-off: `c` clk 299.4 / 299.2 mm (bar 9.0), cons_peak 508.3 / 500.8 mm and cons_plat 382.8 / 380.6
mm (bar 32.9), pb 1 546.7 / 1 553.5 mm; `n` clk 490.7, cons_peak 369.6, cons_plat 123.3 mm in both. **X4 PASS.**

**X5, the withheld set.** On DOY 132 the key arm withholds 28 trips in 26 episodes: the 26 episodes open at the 26 trips of
card BRD3 section 5.2 (the same satellites at the same seven epochs), and G30 (527760) and G12 (532530) are withheld once more in the
epoch after, where the posture without the key had decoupled them one epoch earlier. 26 of the 28 have a keys' share of at least 0.9; G13 and G15 at
2.592 h read 0.849 and 0.807 (card BRD3 section 5.2's 0.85 and 0.81). The clock's own innovation exceeds 20 mm on two: G15 +21.68 mm
(2.592 h) and G14 +20.03 mm (1.750 h). Every one of the 28 has |w_clk| at most 0.58. The run-end clause counts 28 withheld,
not 26. **X5 FAIL** on all three clauses as written: the keys' share (2 of 28 below 0.9), the 20 mm bound (2 above it, by 1.7
and 0.03 mm) and the count (28 against 26, the 2 extra being second epochs of withheld events). The two satellites under 0.9
were already outside card BRD3 section 5.2's prediction; none of the three failed clauses is a trip that carries a clock share.

**Reported without a bar.** The regime on CODE with the key on DOY 131 and 132 and the 24 h run are in section 3. The GPS +
Galileo broadcast posture with the re-seed key and the clock share was not run (card ICL2's posture is a separate lineage of
keys, out of this card's time).

## 3. Reported without a bar (2026-11-10)

### 3.1 The regime on CODE with the key, DOY 131 and 132

Card BRD's regime on CODE (the T2 recipe without the broadcast a priori, the track and the two-way centring) with the key at
2.0 (`brd3k_rx{131,132}_6`). DOY 131: the two clock-carried trips of card BRD3 section 2.3's regime are taken, G14 (|w_clk| 4.15,
dclk +107 mm) and G22 (|w_clk| 2.01, dclk +49 mm), both at 3.84 h, the epoch of the tracked posture's one taken trip; 8
epochs withheld in 2 episodes (G05 for 7 epochs from 1.20 h, G12 at 3.99 h), |w_clk| at most 0.15. DOY 132: 0 trips taken;
39 epochs withheld in 33 episodes (card BRD3 section 2.3's 33 regime trips), |w_clk| at most 0.49, every one at a keys' share of at
least 0.92, the clock's own innovation at most 31.2 mm. On both a prioris the one epoch of the four days whose trips carry a
clock share is DOY 131 at 3.84 h; its clock moves are 5 to 11 cm, decimetre-scale events the requirement keeps.

### 3.2 24 h on DOY 132

The tracked posture with the key and the GPS golden, 24 h from 00:00 (`brd3k_x132_24`, `brd3k_g132_24`). The key arm takes
0 trips and withholds 36 epochs in 29 episodes: the 6 h arm's 28 in its first six hours, then G25 for 3 epochs at 15.27 h
and for 4 at 15.53 h and G26 at 17.88 h; |w_clk| at most 0.58 over the day, 34 of the 36 at a keys' share of at least 0.9.
Against the 24 h GPS golden, read over hours 2 to 24 with gate D's day bars for scale (no bar was frozen for 24 h):
WTZZ00DEU 0.0202 against 0.0187 m (+0.0015), census paired median +0.0032 m, 2 of 16 sites worse by 2 cm (UTQI00USA +0.032,
BAKE00CAN +0.028 m). Hours 2 to 6 repeat the 6 h arm (-0.0019 m, WTZZ00DEU -0.0042 m); hours 7 to 24 alone read +0.0060 m at
the census and +0.0030 m at WTZZ00DEU, where the key arm took no trip and withheld 8 epochs. ARHT00ATA, card BRD2's divergent
site, holds over the day: hourly p50 at most 0.058 m after hour 1, mean over hours 2 to 24 0.0287 m against the golden's
0.0318 m (`b3x24_arht`, `b3g24_arht`). WTZZ00DEU by hour (m), key arm then golden: 0.078 0.017 0.012 0.014 0.024 0.016 0.021
0.019 0.026 0.027 0.023 0.028 0.025 0.023 0.011 0.022 0.023 0.011 0.008 0.027 0.024 0.022 0.022 0.020 / 0.087 0.026 0.021 0.017
0.025 0.014 0.018 0.020 0.029 0.019 0.015 0.022 0.021 0.028 0.013 0.015 0.018 0.009 0.010 0.023 0.018 0.016 0.016 0.016.

## 4. Verdict

**The clock-share requirement does what card BRD3 pre-registered: it withholds the clock anchor's key-carried trips on the
broadcast-referenced posture and keeps the trips that move the clock. X-U0 and X0 to X4 pass; X5 fails on all three of its
clauses as written. Nothing is armed.**

1. **Off and refused.** With the key off the final exe reproduces the base exe byte for byte on the field golden, the sim
   golden and the tracked posture (X0); the loader refuses the key at the goldens' q, outside phase mode and with the anchor
   off (X1), so it can act only on the broadcast-referenced posture.
2. **The four days (X2).** On card BRD's tracked posture, rebuilt on the 2026-11-07 golden, the key takes 0, 0, 1 and 0
   trips on DOY 129 to 132 and withholds 1, 7, 1 and 28, the trip population card BRD3 section 2.3 read on the 2026-11-03
   golden. Three days are byte-identical to the test-off arm; DOY 131's one trip (G22, |w_clk| 2.06) leaves the census and
   WTZZ00DEU within 0.001 m of the test-off arm and inside gate D's bars against the GPS golden. Pooled over the four days
   the census median is +0.0006 m and 8 of 64 site-days are worse by 2 cm (12 %, bar 20 %; card BRD2's test-off arms read
   +0.0021 m and 17 % on the golden of 2026-11-03).
3. **Detection kept (X3, X4).** The injected 0.5 m clock step on posture `c` still trips at |w_clk| 7.92, and every step leg
   and null of `c`, `n` and `t` is byte-identical with and without the key. In the storm on `c` the key withholds exactly the
   8 trips with |w_clk| below 2 (all G21) and takes every other; no cell of the matrix changes verdict.
4. **The withheld set (X5, FAIL).** On DOY 132 the key withholds 28 epochs in 26 episodes, the 26 trips of card BRD3 section
   5.2 plus a second epoch of two of them. Two have a keys' share under 0.9 (G13 0.85, G15 0.81, the two card BRD3 section
   5.2 already flagged) and two a clock innovation over 20 mm (G15 21.7 mm, G14 20.03 mm), and the count is 28, not 26. All 28 read
   |w_clk| at most 0.58; the failed clauses are the composition and count of withheld key events, not a withheld clock event.
5. **The regime on CODE** (section 3.1) takes its two clock-carried DOY-131 trips and withholds the 33 DOY-132 trips. The
   24 h run on DOY 132 (section 3.2) takes no trip over the day and reads +0.0032 m at the census and +0.0015 m at WTZZ00DEU
   against a 24 h GPS golden over hours 2 to 24 (+0.0060 and +0.0030 m over hours 7 to 24); ARHT00ATA does not diverge.

Recommendation: the key is the field-runnable form of card BRD2's candidate (the anchor's statistic test kept, its
key-carried trips withheld, the step still detected); it is ready for the owed steps below, not for arming. Arming, any
golden change and the merge are the author's decisions.

## 5. What remains

- **Before any arming** (card BRD3 section 4.2): 24 h on DOY 129 against a 24 h GPS golden, DOY 128, and a real-time run
  (24 h on DOY 132 is section 3.2; its later 18 hours read +0.0060 m at the census, above gate D's day bar if one applied it).
- **The unhealthy broadcast satellite (G01)**, absent from every broadcast-referenced wire (card BRD2 section 4b): owed, not
  attempted. UTQI00USA, the site card BRD2 tied to G01, is this card's largest DOY-132 loss (+0.118 m).
- **ARHT00ATA.** On the key arms its mean over hours 2 to 6 is 0.024 to 0.038 m on all four days (-0.014, -0.011 and
  -0.019 m against the golden on DOY 129, 130 and 132); card BRD2's broadcast-referenced divergence (+0.79 to +0.90 m on DOY 132)
  does not appear on today's golden. On DOY 131 the GPS golden's own ARHT00ATA replay diverges from hour 4 (163 m in hour 4),
  a reference-side event that makes that site-day read better; without that site DOY 131's census median reads +0.0018 m,
  inside the bar.
- **Second epochs of a withheld event.** A satellite the knob-off posture decoupled at a key event stays anchored under the
  key, and its phase statistic can stay over k for one to seven epochs (DOY 132's G30 and G12; the storm's G21 for 7); each
  is a withheld epoch with its event and counter. Whether the episode should be one withhold (X5's count of 26 assumed it) is
  a counting choice for the author, with no effect on the solve.
- **The GPS + Galileo broadcast posture** with the re-seed key and the clock share (card BRD3 section 4.2's unbarred item),
  not run here.
