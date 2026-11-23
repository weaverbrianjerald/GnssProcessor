# SCE: one receiver's free-running code loop, and the station that stands down for 77 minutes

**STATUS**

- **id:** SCE
- **topic:** raim-qc
- **status:** open
- **summary:** Pre-registered 2026-11-23 by the managing session's order (in the author's absence). On 2024 day 129 the
  receiver of BSHM00ISR (SEPT POLARX5 5.5.0) lets G22's C1C code loop run free for three and a half minutes
  (02:47:30 to 02:51:00, single band, short of the range by 27 m growing to 1,340 m, about 7 m/s). The network takes
  three of those rows before the gross line-of-sight screen withholds the line of sight; the line of sight comes back
  with a second jump; with the CODE bias file read, the station's clock then stays 10 to 41 m off its trend and the
  screen's own declarations, with the erase response, keep the station from recovering until 04:08 (the marks of card
  GLM). Stage A0 measures from existing runs only: no build, no gate run.
- **dates:** 2026-11-23 (pre-registered, before any reading of stage A0).
- **knobs / posture:** none changed. The reference files of 2026-11-22 as run in card BRP: `runs_nom/_rap_brp_f129_c0_6`
  (with the .BIA) and `runs_nom/_rap_brp_f129_c0nb_6` (without); for item 6 the reference runs of 2024 days 129, 130,
  132 and 2026 days 143 to 147.
- **code:** none (branch `qc/station-code-event-2026-11-23` from main `33c5fef` holds the card).
- **run dirs:** none new. Tools and reads: `D:\GnssProcessorSupport\gates\station_code_event\` (order
  `manager_ORDER.md`, report `REPORT_0.md`); scripts under `D:\GnssProcessorSupport\runs_nom\_irdu_grades\scripts_s20\`.
- **source:** the managing session's order of 2026-11-23 04:13 (`gates\station_code_event\manager_ORDER.md`); card
  BRP's trace of 2026-11-23 (section "day 129: why BSHM00ISR's GPS lines of sight are declared longer with the .BIA
  read"); the managing session's check of G22's code against its range rate
  (`gates\broadcast_reference\manager_check\manager_bshm_g22_0245_0255.txt`); standing invariants 1 and 7 of
  `CLAUDE.md`; card GLM (branch `net/gross-los-emit-marks-2026-11-22`).

**Outcome.** Pre-registered; no reading yet. Three steps failed in a row on day 129 at BSHM00ISR: a code observation
hundreds of metres off entered the solution for three epochs; its line of sight came back with a value that made a
second jump; and the screen's declarations, which erase the code that would level the station's clock, held the
station off for 77 minutes, while the product marked its satellites interrupted at every epoch. Stage A0 measures the
timeline, the rules that waited, the return, the hold, the observation file's flags, the frequency on the reference
days and the bias file's part. Candidates are described, not built.

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

Why does one drifting code observation on one satellite at one station reach the solution, and why does the station
then stay off for 77 minutes? How often does that happen on the reference days, and what part does the CODE bias
file play?

## 2. The finding that opens the card

- At BSHM00ISR, G22's C1W and C2W are present to 02:47:00; at 02:47:30 only C1C is left; at 02:48:00 the satellite
  has no record; from 02:48:30 to 02:51:00 C1C alone; from 02:51:30 all three are back.
- Against the range rate before and after (11,497 m per 30 s at 02:47:00 rising 18.5 m per epoch to 11,682 m at
  02:52:00) the C1C of those epochs is short by about 27 m at 02:47:30, 500 m at 02:48:30, 720 m at 02:49:00, 930 m at
  02:49:30, 1,140 m at 02:50:00 and 1,340 m at 02:50:30, and back within about 25 m at 02:51:00.
- The network took three of these rows (residuals -145, -296, -375 m, prefit -722 m) before the screen withheld the
  line of sight at 02:50:00. With the bias file read, the station's clock was 9.9 m off its trend at 02:50:00 and 41 m
  off after 02:53:30; its other GPS lines of sight read 10 m and more off, were declared and erased, and the
  declarations held to 04:08. Without the file the clock stayed within 4 m and two satellites stayed declared.

## 3. Stage A0: readings from existing runs (fixed before they are made)

1. **Timeline**, 02:47:00 to 02:54:30 for G22 and on to 04:08 for the station, per epoch, both runs side by side:
   which of G22's rows entered the solve, with what prefit and weight; what each check saw and did (prefit census,
   RAIM within the epoch and its attribution, the screen's statistic and its count of epochs in a row, the cold bound,
   the station tier); the station's clock and receiver code-bias columns against their trend, with sigmas; the number
   of the station's code rows left in the solve.
2. **The rule that waited**: why rows of -500 to -900 m entered three times; the rule by file and line, and what it
   needed to act at the first epoch.
3. **The return**: when G22 was released or seeded again after 02:50:00, from which epoch's code, with what value
   against the code of 02:51:30 onward, and what makes the +118 / +184 m at 02:53:00.
4. **The hold**: why it lasts 77 minutes and why it ends at 04:08 (code rows left per epoch, the clock's sigma, the
   release that ends it).
5. **The observation file**: signal strength and loss-of-lock flags of G22's C1C and L1C from 02:47:30 to 02:51:00, and
   whether the engine reads any of them there.
6. **How often**, in the reference runs of 2024 days 129 (6 h and 24 h), 130, 132 and 2026 days 143 to 147: every code
   row with a prefit beyond 50 m that entered a solve, with what followed; every episode in which three or more lines
   of sight of one station are declared within two epochs or the station tier fires (station, time, duration, the
   first row that was off, the clock's largest departure from its trend).
7. **The bias file's part**: the station's receiver code-bias columns and sigmas with and without the file at
   BSHM00ISR and at one ordinary station, and which difference lets the clock go; settled by the code and the traces
   if they settle it, otherwise one pair of 6 h arms is named and not run.

## 4. Candidates (described in the report, not built)

(a) A bound on a code row's prefit that acts at once, its value from item 6. (b) A test of a code row against its own
carrier or the satellite's range rate. (c) A response at the level of the station: many lines of sight of one station
off by a common amount is the station's clock or code-bias block, levelled again as one declared event. (d) A
returning line of sight seeded only from epochs that passed the checks. Each with its place, the event at both edges
with counters, and the simulator test (a code ramp of about 7 m/s on one satellite at one station for three and a
half minutes, single band, as a new injector kind beside `code_step` of the GLM branch).
