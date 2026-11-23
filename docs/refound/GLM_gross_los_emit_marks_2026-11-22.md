# GLM: the gross line-of-sight screen's emit marks at the edges of a declaration, not at every epoch

**STATUS**

- **id:** GLM
- **topic:** network-qc
- **status:** open
- **summary:** Pre-registered 2026-11-22 on the managing session's order 5 (part A), in the author's absence on the author's words of the same evening ("Proceed with your decisions in my absence"). Under `network.gross_los_emit_exclude` (armed in the reference files) the screen marks a satellite discontinuous in the product at every epoch in which any station holds a declared line of sight to it (`GrossLos.cpp` 872 to 905, `SolPrep.cpp` 465 to 470), and the replayed user bumps the satellite's discontinuity counters at each mark. On 2024 day 129 one station (BSHM00ISR) keeps G13, G15, G17, G19, G22 and G24 marked from 02:50 to 04:07 (965 standing marks), so every user loses those satellites' phase continuity for 77 minutes. A0 (no build) measures every standing declaration, the emitted values at its edges and the cost at the user; A1 (default off) marks the edges only.
- **dates:** 2026-11-22 (pre-registered before any run of A0 item 3 or A1; A0 items 1 and 2 read existing runs only and are recorded below as measured).
- **knobs / posture:** planned `network.gross_los_emit_edges` (default off, identical when off; read only under `network.gross_los_emit_exclude`). Base: the reference files `config/golden/golden_{field,sim}_refound.json`.
- **code:** planned, branch `net/gross-los-emit-marks-2026-11-22` from main 33c5fef: `src/core/network/qc/GrossLos.cpp` (the mark decision), the injector kind `code_step` (`network/carry/Inject`), tests in a file of their own.
- **source:** the managing session's order 5 (`D:/GnssProcessorSupport/gates/variable_rate/manager_ORDER_5.md`) and its check `manager_check/manager_retro_marks.py`; card VRE step 1 (reading R3, `REPORT_4.md`); the screen's record [RF-3](../record/refound/RF-3_code_row_qc_2026-09-22.md) (B3, section 5.3). Working folder `D:/GnssProcessorSupport/gates/gross_los_marks/` (scripts `glm_common.py`, `a0_marks.py`, `a0_pool.py`, `a0_edit.py`; outputs in `out/`).

**Outcome.** Open; pre-registered. Measured so far (A0 items 1 and 2): standing declarations occur on every observed reference day, from 6 marks (day 130, 2026 day 147) to 965 (day 129); the simulator has none. The emitted phase clock of a marked satellite steps at the declaration's first epoch (median 0.7 mm, 90th percentile 23 mm, up to metres in hour 1, against 0.46 mm on unmarked epochs), stays at the unmarked level while the declaration stands, and does not step at an ordinary release (33 releases, median 0.36 mm, largest 2.3 mm); a cold-bound release, which re-seeds the carried ambiguity, does (median 4.7 mm, refloats 179 and 330 mm). Day-129 users on the product without the standing marks (A0 item 3, 17 sites): the 8,386 resets of the marked satellites at those epochs are gone, their held samples over hours 3 to 5 rise from 9,297 to 15,780, the built-in user's hours 4 and 5 fall from 32.8 / 38.0 mm to 11.7 / 6.8 mm, and the median user moves 0.0 mm in every hour.

> Results are appended TO THIS FILE, below the text, under dated headings.

---

## 1. Question

Where must the screen's emission exclusion mark a satellite so that a user is told of every step the declaration
puts into the emitted values, and of nothing else; and what the marks at every epoch of a standing declaration cost a
user today.

## 2. A0, measure, no build

1. **Inventory.** Every run of consecutive gross-LOS emit marks per satellite (`gross_los_emit_disc` rows of
   `network_events.csv`) in the reference runs `runs_irm/_rap_n20_f{129,130,132}_6`, `_rap_irm_day24_f`, the 2026 days
   143 to 147 (`runs_nom/_rap_agu_final24_n143` to `_n147`, the poster's 24 h networks) and the simulator
   (`_rap_n20_s_6`, `_rap_irm_day24_s`): station, satellites, epochs, marks. A run of two or more epochs is a standing
   declaration.
2. **Emitted values at the edges.** For each run, the between-satellite epoch-to-epoch change (the change less the
   median change of the constellation's satellites unmarked at both epochs) of the emitted clock, each code bias, each
   phase bias and the phase clock (clock minus phase bias, the convention with the smaller spread on unmarked epochs), at
   the first epoch, inside the run and at the release, against the same quantity on unmarked epochs (robust sigma).
   Changes across a phase integer-counter change are left out.
3. **Cost at the user.** The day-129 product with the standing marks removed (`a0_edit.py`: the first mark of each run
   kept; a mark another source also sets at that epoch kept; the clock, code and phase discontinuity counters' steps at
   a removed mark taken out of the record and every later value of its key), replayed by the 16 outside receivers and
   WTZZ00DEU over 6 h with exe `gnss_processor_int1120.exe` (sha256[:16] de17cb83802a20c2), against the existing
   replays `runs_irm/_rap_n20_129_<site>`: 3D error by hour, held share, and the resets and held share of the marked
   satellites over hours 3 to 5. The editor reproduces a product without standing marks byte for byte (checked on
   `_rap_n20_s_6` before any replay). Reported, no reading.

## 3. A1, the fix (default off, identical when off)

Key `network.gross_los_emit_edges` (bool, default false), read only under `network.gross_los_emit_exclude`. With it
on, the screen keeps per satellite the set of stations whose declared line of sight is out of its contributor set, and
marks the satellite:
- at an **onset**: the epoch at which that set gains a station (the epoch the declaration reaches the emission; also a
  second station joining a standing declaration);
- at a **re-seeding release**: the epoch at which a station leaves the set through a release that re-seeds the
  carried ambiguity (the cold bound's release, refloat or expiry). An ordinary clear is not marked (A0 item 2: no step).
Every other epoch of a standing declaration is not marked and is counted as withheld. The withhold path (fewer than
`network.gross_los_sat_min_stations` contributors left) is unchanged. Counters: marks landed at onset, at a re-seeding
release, withheld as standing; events at both edges (`gross_los_emit_disc` at an onset, `gross_los_emit_release` at
each release with whether it was marked; standing invariant 7). Unit tests in a file of their own. For the simulator
event, the injector gains a kind `code_step` (metres added to the code observables of one satellite at one station
inside a window, phase untouched), default absent.

**Readings, fixed before the first run** (Release, one exe, the reference files with only the key and the run paths
changed; gate priority 4):
- **G1, identity off.** Key off on the two reference files, 2 h: products and every trace byte-identical to main
  33c5fef built in the same tree.
- **G2, day 129 key on, 6 h.** Gross-LOS marks at standing epochs: 0; R lines per hour reported against key off. Every
  other difference of the products and traces against key off is listed with its cause (the expected one: the R lines
  and the discontinuity counters of the marked satellites). Built-in user and the 16 receivers: 3D median over hours 2
  to 6 per receiver-day, key on minus key off, median over the 17 at most +1 mm and none above +20 mm untraced; held
  share of the marked satellites over hours 3 to 5 at least key off's.
- **G3, the simulator key on, 6 h.** No standing declaration in the reference simulator run (A0 item 1), so: products
  and every trace identical to key off; the ring fix's truth audit with its held, right and wrong counts beside (0
  wrong).
- **G4, one injected event, simulator.** `code_step` of 20 m on six GPS satellites at one station for 30 minutes from
  02:30:15, against the null leg (the same arm with no event): gross-LOS marks of those six satellites only at the
  onset epoch (and at a re-seeding release if the screen releases that way); from the epoch after the declaration to
  the end of the window, the network's emitted clock, code biases and phase biases of the six satellites within 1 mm of
  the null leg, between satellites.

Outcome rule: A1 passes if G1 to G4 all meet. Arming is decided afterwards by the managing session under the author's
arming rule.

## 4. What this card does not claim

The steps inside a standing declaration that another mechanism makes at the same station (on day 129 a RAIM
attribution at BSHM00ISR moves G17 and G19's phase clock by 35 to 38 mm at 269580, under the emitter's own 0.25-cycle
phase jump threshold) are left to that mechanism. The R record carries no channel, so a replay re-marks every channel
of a marked satellite (RF-3 section 5.3); that is unchanged.

---

## 2026-11-22: A0 items 1 and 2 (existing runs, measured before registration of item 3)

| run | epochs | declarations | marks | runs | standing (marks) |
|---|---|---|---|---|---|
| day 129, 6 h | 720 | 17 | 987 | 22 | 18 (983) |
| day 130, 6 h | 720 | 1 | 2 | 1 | 1 (2) |
| day 132, 6 h | 720 | 3 | 6 | 3 | 3 (6) |
| day 129, 24 h | 2880 | 31 | 1199 | 37 | 31 (1193) |
| 2026 days 143 / 144 / 145 / 146 / 147, 24 h | 2880 each | 7 / 3 / 8 / 10 / 3 | 36 / 8 / 21 / 27 / 6 | 12 / 4 / 8 / 14 / 3 | 9 / 2 / 8 / 9 / 3 |
| simulator, 6 h and 24 h | 720, 2818 | 0 | 0 | 0 | 0 |

Day 129: BSHM00ISR's GPS code goes gross from 02:50 (repeated whole-station stand-downs); declarations of G13, G15,
G17, G19 and G30 at 269400, G14, G22 at 269640, G24 at 269760; G15, G17 and G19 marked 155 to 156 consecutive epochs,
cleared at 274080. Pooled over the eight observed 6 h and 24 h runs (standing runs only; mm, between satellites):

| quantity | unmarked sigma | first epoch, median / p90 | inside, RMS median / p90 | release, median / p90 |
|---|---|---|---|---|
| GPS clock | 0.46 | 1.40 / 6.16 | 0.52 / 1.78 | 0.18 / 0.89 |
| GPS phase clock (clock minus phase bias) | 0.46 | 0.69 / 23.1 | 0.59 / 2.93 | 0.34 / 1.98 |
| Galileo clock | 0.42 | 2.27 / 28.9 | 0.47 / 9.87 | 0.55 / 7.21 |
| Galileo phase clock | 0.42 | 2.41 / 86.4 | 0.66 / 17.9 | 0.64 / 15.2 |
| code biases | 0.002 to 0.003 | 0.00 / 0.20 | 0.00 / 0.22 | 0.00 / 0.07 |

The largest first-epoch steps (0.3 to 6.3 m) are cold-bound declarations in hour 1 (G06 and G07 on day 129, E10, G23
on day 130, E33 at PARK00AUS on 2026 day 146), whose response refloats the carried ambiguity. Release by kind
(largest phase-clock step of the run): ordinary clear after hour 1, 33 runs, median 0.36 mm, largest 2.3 mm; cold
release, 8, median 4.7 mm, largest 21.0 mm; cold refloat, 2, 179 and 330 mm; no release row in the window, 10, median
0.45 mm, largest 7.0 mm.

The editor (`a0_edit.py`) on the simulator product: byte-identical. On the day-129 product: 965 marks removed (the
standing marks less the first of each run; none shared with another source), 155 R lines dropped, 5,814 counter steps
taken out, no counter without a step and none stepping by more than one at a removed mark; 88 marks remain (25 epochs).

## 2026-11-23: A0 item 3, the users on the day-129 product without the standing marks

Replays `D:/GnssProcessorSupport/runs_glm/_rap_glm0_129_<site>` (17 sites, 6 h, exe `gnss_processor_int1120.exe`,
product `gates/gross_los_marks/products/f129_6_nostanding.txt`), each exit 0 to 05:59:30, against the same exe's
replays of the unedited product `runs_irm/_rap_n20_129_<site>`. Grader `scripts/a0_user.py`, output
`out/a0_user_f129_6.{json,txt}`. Marked satellites (any standing run): E10, E30, E33, G06, G07, G12, G13, G14, G15,
G17, G19, G22, G24, G30. Hours 3 to 5 are 02:00 to 05:00 (the BSHM00ISR declarations stand 02:50 to 04:07).

- **Events.** The user events of the marked satellites (clock, code-bias or phase-bias reset) at the epochs whose mark
  was removed fall from 8,386 to 0 over the 17 sites (WTZZ00DEU 1,780, BADG 1,192); every other event of those
  satellites over hours 3 to 5 is the same in count at every site.
- **Held samples of the marked satellites, hours 3 to 5** (43,303 rows in both): 15,780 against 9,297 (tight rule:
  shared minimum or 0 < amb_var <= 1e-6 m2); 5,843 against 5,180 (registered rule). Fewer at STPM (304 against 368)
  and CEDU (21 against 22), and under the registered rule at BADG (1,272 against 1,326).
- **Held-epoch share from hour 2** (tight rule): WTZZ00DEU 0.865 against 0.598, WUH2 0.840 against 0.667, UTQI 0.425
  against 0.210, ANMG 0.082 against 0.058; STPM 0.528 against 0.670; the other 12 within 0.003.
- **3D median by hour, edited minus reference.** Median over the 17 sites 0.0 mm in every hour; mean +0.3, 0.0, 0.0,
  -4.7, -0.9, -0.2 mm; the median over hours 2 to 6 per site lies from -3.0 mm (BADG) to +4.2 mm (UTQI). Hours 4 and
  5: WTZZ00DEU 11.7 / 6.8 mm against 32.8 / 38.0 mm, ASCG -38.4 / -10.0, BADG -26.6 / -6.8, WUH2 +10.0 / -15.4 (+13.8
  in hour 6), STPM -7.9 / +5.8, ANMG +4.8 / +37.8 (-22.0 in hour 6). ANMG fixes nothing in either run (float error 32
  to 131 mm in both); the difference changes sign at 4.75 h, a float path move. Hour 1: DAV1 +12.4, WUH2 -6.1, BADG -1.7
  mm (hour-1 cold declarations of two epochs, whose second mark is removed). Hours 2 and 3 within 0.4 mm everywhere.

Reading, reported without a bound as registered: the standing marks cost the users the marked satellites' holds for
the declaration's duration and, at the built-in user, about 2 to 3 cm in hours 4 and 5; removing them moves the
median user by nothing and the mean by -4.7 mm in hour 4. STPM's lower held share is listed, not traced.
