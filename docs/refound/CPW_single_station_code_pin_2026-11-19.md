# CPW: the single-station code pin (a Galileo code column walks while one signal of its pinned pair is parked)

**STATUS**

- **id:** CPW
- **topic:** code-bias
- **status:** open
- **summary:** When one code signal of a satellite's pinned pair is seen at one station or none, the pin falls to the single-frequency branch, an all-ones row at target 0 at the gauge weight, and pulls the surviving column toward zero (17 to 30 cm on observed Galileo E14, E18, E32; 1 to 2 m on the simulator); the IF code stays off after the return. Key `network.code_pin_single_leg_hold` holds the surviving column and keeps the parked signal's satellite code bias in its single-station row; second form passes its bar on DOY 146 and 147 and is ARMED (RECOMMEND_SCOPED) in the two primary reference files since 2026-11-20 (5207fdc, main 019ed34).
- **source:** packages `carried-pin-walk-evidence` and `carried-pin-walk-response` of the day program of 2026-11-19 (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/`: `MECHANISM.md`, `DESIGN.md`, `CHECKPOINT_1.md` to `CHECKPOINT_5.md`, `CLOCK_SLIDE_answer.md`, `NIGHT_RUNS.md`; grader `D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/carried-pin-walk-response/event_bar.py`); commits b22ce97 (second form), 7e4ea01 (its merge), 5207fdc (the arming); ledger row `network.code_pin_single_leg_hold`; the managing session's reads of 2026-11-19 and 2026-11-20.
- **filed:** 2026-11-20, after the fact, from the package records named under source (the work ran on 2026-11-19 and 2026-11-20 before this file existed).

**Outcome.** The walk is the code datum, not the data: a satellite whose pinned pair loses one signal to a single station is pinned by one all-ones row at target 0 (`Sbasis.cpp:1491-1497`, weight 1e3 m^-2), so the surviving column moves toward zero by the carried-prior law, and the return re-seeds both time links. A first form held the column but left the parked signal's single-station row without its bias, so the misfit slid into the satellite clock (found by eye on a viewer page). The second form keeps that bias as a known term; it passes the registered bar on both observed days and is armed in the reference files. Owed: the simulator's one-station trigger (2.5 cm against 1.5 cm at the return) and the degraded declaration.

## 1. Question

Does a satellite's code column move when one signal of its pinned pair is parked network-wide or kept at one station,
and does the satellite's ionosphere-free code stay off after the parked signal returns? If so, can a default-off
response keep the code level through the interval without moving any satellite that has no such interval?

## 2. Mechanism (read at `int/2026-11-19` cf983dc; `MECHANISM.md` section 1)

1. A (satellite, code signal) column is registered for the epoch only when two or more present stations carry it
   (`src/core/network/driver/ApplicationNetworkAssembly.cpp:624`). When C5Q of a Galileo satellite falls to one
   station, the registered set is {C1C}.
2. `NetworkSbasis::selectIfPinPair` finds no second frequency, so the builder writes the all-ones row on every
   registered column with target 0 (`src/core/network/solve/Sbasis.cpp:1491-1497`), folded at
   `kCodeBiasGaugeWeight` = 1e3 m^-2, kind `CbIfPin`.
3. With one frequency the column is not separable from the satellite clock: only its carried prior resists the row.
   Each epoch the column moves toward 0 by about w / (w + 1/var) of its value (0.4 % per 30 s epoch at a 2 mm sigma,
   2.5 % at 5 mm). Under the broadcast group-delay datum the value is of order c times BGD (0.3 to 2 m).
4. The single-frequency branch writes no pin record, and the harvest replaces the pair map wholesale, so at the
   return the pair is new, the parked signal has no previous value, and both time links re-anchor at the absolute
   target (`Sbasis.cpp:1456-1487`, `:1601-1614`).
5. GPS is spared on observed data because a GPS satellite nearly always keeps a second frequency (C2L or C5Q beside
   C2W); the simulator shows GPS takes the same branch when its band 2 is removed with no other band configured.

## 3. Response (key `network.code_pin_single_leg_hold`, default off in the loader)

- First form (branch `carried/pin-single-leg-2026-11-19`, merged into `int/2026-11-19` on 2026-11-20 01:20 as
  9f13089): at the decoupling edge the surviving column is held at its value of the last kept epoch (one row at
  1e3 in place of the all-ones row at 0); the pair record is kept; at the return both time links target the pair's
  pre-edge combinations, so nothing re-seeds. Events `CB_PIN` (open, recouple) and a run-end line.
- Second form (b22ce97): in addition, while an edge is open the parked signal's single-station code row takes that
  signal's satellite code bias at its parked value, as a known term of the prediction (no column, no Jacobian
  entry). One edge rule, `NetworkSbasis::pinLegEdgeThisEpoch`, is read by the datum rows and by the station rows
  (`networkCodePinLegKnownBiases`); new run-end counter `parked_station_epochs`.

## 4. Bars

- As first registered (2026-11-19): the surviving column moves less than 5 cm and the IF code ends within 5 cm of
  its pre-edge value (simulator, against an uninjected control); off identity; the held-out user by hour.
- Corrected on 2026-11-20 07:10, after the clock slide (section 5.3): the change of (IF phase bias minus IF code
  bias) of the event satellite, between satellites of one system, against the 95th percentile of the other
  satellites of that system over the same interval, at the return, +30 min and +60 min. The medians use the
  satellite's own system only (`event_bar.py` stops when a median spans two systems). On the simulator, where phase
  levels between satellites move by metres, the code combination run minus control is ranked the same way
  (`code_vs_control.py`).

## 5. Results

### 5.1 2026-11-19: the walk on observed data and on the simulator

- Observed (package `carried-pin-walk-evidence`): when a satellite's C5Q is seen at one station (C1C at two), the C5Q
  column is dropped and the pin becomes one row on C1C at target 0; C1C is pulled toward zero in 964 of 964 steps,
  at 1.0025 to 1.0062 times the law. After the return the IF code bias stays 0.09 to 0.66 m off all day; the
  wide-lane bias moves 0.07 to 0.17 cycle. Only E14, E18, E32 behind PALM00ATA (8 of 150 Galileo satellite-days,
  DOY 143 to 147). It does not explain the wrong user hold UTQI E18-E06, whose cause is on the user side.
- Simulator (`_rap_pl_e06g`, `_rap_pl_e06` against `_rap_pl_ctl`, one signal out network-wide for 30 min from
  01:11:00): E04 without L5Q: C1C walks 1.03 m toward zero, IF code 1.95 m off at the return and 1.91 m at the end
  of the run; G22 without L2W: C1C 1.99 m, IF code about 4.0 m off to the end. The walking column's formal sigma
  stays 3 to 5 mm. Identical to the control before the outage.

### 5.2 2026-11-19 (night): the first form on observed data

CB_PIN rows with the key on: 2 on DOY 146 (E18 open at TOW 184290, recouple at 186180, "solved 0.0000 m away"), 5 on
DOY 147 (E18 281550 to 283110, E32 297120 to 301620, E14 from 341220); 0 rows with the key off. Runs
`_rap_plN_n146_{off,on}`, `_rap_plN_n147_{off,on}`.

### 5.3 2026-11-19 22:30: the clock slide, found on a viewer page

On the page `codewalk_fix_on_vs_off_n146.html` the key-on clock of E18 moved opposite to the key-off C1C code bias.
Measured from `products.txt` (E18, change since TOW 184260, clock relative to the median of the other 29 Galileo
clocks): key off, C1C +0.299 m at the return, clock +0.066 m then +0.146 m half an hour later; key on, C1C +0.001 m,
clock -0.083 m at the return and -0.06 to -0.08 m afterwards. Clock and IF code bias together after the event: 0.5 to
0.8 m off with the key off, 6 to 8 cm with it on. The code residual jump is the same in both runs (the coverage event
itself). Consequence: the first form was not an adoption candidate, and the bar was rewritten (section 4).

### 5.4 2026-11-20 07:10: the slide explained (`CLOCK_SLIDE_answer.md`, no engine run)

While the pair is parked, the parked signal's code row stays in the solve at the one station that still tracks it,
but that signal's satellite code bias is pruned by the two-station rule (`ApplicationNetworkAssembly.cpp:611-641`),
so the row misses about 2 m; the slant ionosphere takes it up and pushes the C1C rows; with C1C held the clock absorbs
it and the carried phase biases keep the offset after the return. The simulator "failure" of bar N3 was a grader
slip (GPS pooled into the Galileo median); corrected, N3 reads -1.1 mm with the key on and +1.92 m off. On the
corrected bar the first form FAILS for E18 on both days (-0.058 to -0.092 m against 0.03 to 0.08 m) and passes for
E32. The second form was released the same morning.

### 5.5 2026-11-20: the second form (`CHECKPOINT_5.md`; engine `gnss_processor_pinleg_known.exe`)

- Debug cases: 235 passed, 7 skipped (fixture-gated), 0 failed; new cases `TheKnownBiasRuleOpensHoldsAndCloses`,
  `TheSingleStationRowKeepsItsBiasWhileParked`. Off identity (2 h, DOY 146, `_rap_pl2_n146_x` against
  `_rap_pl_n146_x`, `nom_td_gate.py --mask-time`): identical.
- Simulator one-station trigger (`_rap_pl2_e06g1_on`, E04 L5Q out at 21 of 22 stations), code combination run minus
  control: -0.025 m at the return (others' 95th percentile 0.015), +0.005 (0.006) at +30 min, +0.007 (0.006) at the
  end; first form -0.108, -0.032, -0.017; key off -1.470, -0.623, -0.475. Four times smaller than the first form at
  the return, still just outside the other satellites' spread.
- Observed, 24 h, key on against the key-off runs (`_rap_plK_n14x_on` against `_rap_plN_n14x_off`), at the return,
  +30 min, +60 min (limit, rank of 29): DOY 146 E18 +0.029 (0.058, 17), -0.014 (0.039, 15), -0.019 (0.089, 6);
  first form -0.058, -0.092, -0.092 (last); key off -0.564 to -0.584. DOY 147 E18 -0.033 (0.035, 25), -0.074 (0.077,
  24), inside narrowly; first form outside; key off -0.360, -0.496. DOY 147 E32 +0.032 (0.033, 26), +0.024 (0.032,
  21), +0.043 (0.053, 26). E14 (22:47, no return in the day): the bar cannot print.
- Code residual RMS of the event satellite over its receivers during the event (key off, first form, second form;
  other satellites 0.36 to 0.37 m): DOY 146 E18 1.65, 1.84, 0.29 m; DOY 147 E18 1.98, 2.10, 0.31; E32 1.45, 1.57,
  0.38; E14 1.70, 1.75, 0.35. Clock of E18 against the other Galileo clocks within 0.019 m (146) and 0.015 m (147).
- Products identical to the key-off run up to the first opening; satellites without an event within 1.9 cm in clock
  and 3.6 cm in phase bias (E34, day 146), millimetres on day 147; built-in user, on minus off, at most +0.245 cm
  by hour (bar +5 mm).
- Pages: `D:/GnssProcessorSupport/netview_out/codewalk_fix2_on_vs_off_n146.html` and `..._n147.html`.

### 5.6 2026-11-20: merged and armed

- 16:00: merged into `int/2026-11-19` as 7e4ea01 (key default off, ledger UNVALIDATED). Full Debug suite on 7e4ea01:
  5,080 cases in 683 suites, 4,986 passed, 94 skipped, 0 failed.
- 16:30: ARMED in the two primary reference files by 5207fdc (ledger RECOMMEND_SCOPED; regime: satellite code bias
  estimated, time-linked code datum, ring fix on; the frozen `rf0hv18` lineages and the BeiDou lineage keep it off).
  Six-hour pair with the key armed (`runs_irm/_rap_cp1_f129_6`, `_rap_cp1_s_6`, engine `gnss_processor_int1120k.exe`
  = 7e4ea01): zero openings in both, and products and every trace identical to `_rap_la4_f129_6` /
  `_rap_la4_s_6` (only the key's run-end line differs).
- 19:25: `main` = 019ed34 (pushed on the author's words "Yes, move and push").

### 5.7 Owed (2026-11-20)

- The simulator's one-station trigger still sits just outside the other satellites' spread (2.5 cm against 1.5 cm at
  the return).
- The degraded declaration (URA by the formal sigma against the other satellites) stays a design paragraph, built
  only if the bar still fails where the data is thin.
- The snapshot fingerprint token and a contribution kind of the key's own.
