# CBG: the user fixes on a code without its bias (the code-bias gap)

**STATUS**

- **id:** CBG
- **topic:** user-ar
- **status:** open
- **summary:** Pre-registered 2026-11-21 by the author's order of that evening. While the network sends no code bias for a satellite's second signal (a code-bias column needs two stations; card CPW's hold keeps the value inside the network and sends none), the user applies that signal's code with a bias of zero; a fix made while the satellite's ambiguities are loose takes the neighbouring Galileo wide lane with the matching narrow lane, the step (3, 4), and nothing tests it again. Observed at ARHT00ATA: E18 on 26 May 2026 held (91, 99) where (94, 103) is right, E32 on 27 May held wide lane -3 where -4 is right. Response under test: key `network.closed_loop.code_bias_gap_response` (off, revalidate, full), default off. Result 2026-11-22 (section 6): with the key off the user takes the wrong hold on the simulator (with the network's phase-bias reset inside the gap) and in both observed cases; "revalidate" releases it after 20 epochs and refixes right; "full" never takes it; no right hold released, off byte-identical, position within 5 mm. Question 5 (section 7): 2 wrong Galileo holds in the day-long users (607 of 1.57 million held satellite-epochs), both from this defect, where the twin check counts none. Not armed.
- **dates:** 2026-11-21 (pre-registered, before any run of the response); 2026-11-22 (results, sections 6 and 7).
- **knobs / posture:** `network.closed_loop.code_bias_gap_response` with `code_bias_gap_revalidate_ep` (20) and `code_bias_gap_window_ep` (120), derived into the consumer as `processing.ssr_code_bias_gap_response` and its two lengths; default "off", byte-identical. User replays only: the 24 h simulator network `runs_irm/_rap_irm_day24_s` with the SZZ000SIM user of `runs_nom/_rap_ufq_s129_f0`, and the poster's day-long ARHT users (`runs_nom/_rap_agu3_d10x_{146,147}_arht`) on their network products.
- **code:** `include/realtime/CodeBiasGap.hpp`, `src/core/realtime/CodeBiasGap.cpp` (pure, unit-tested), `src/core/realtime/RealTimeEngineCodeBiasGap.cpp` (the engine seams), `AmbiguityResolver::forgetHeldPairs`; branch `user/code-bias-gap-2026-11-21`.
- **run dirs:** `D:\GnssProcessorSupport\gates\e18_wide_lane\runs\cbg_*`; tools and reports in `D:\GnssProcessorSupport\gates\e18_wide_lane\` (`REPORT_1.md`, `REPORT_2.md`, `tools\`).
- **source:** the author's order of 2026-11-21 (about 21:15), the managing session's brief `gates\e18_wide_lane\BRIEF.md`; `REPORT_1.md` (questions 1 to 3); card [CPW](../../refound/CPW_single_station_code_pin_2026-11-19.md).

**Outcome.** The response passes every bar of section 4 on user replays. With the key off, the user holds the neighbouring Galileo wide lane in all three tests (simulator E14 with the network's phase-bias reset inside the gap, ARHT E18 on 26 May, ARHT E32 on 27 May); "revalidate" releases that hold 20 epochs after it is taken and the satellite is refixed right; "full" leaves the bias-less code out, bars the fix and takes the right integers. No right hold was released in 36 runs, off is byte-identical, the position moves at most 5 mm. A correct count for the day-long users: 2 wrong Galileo holds, both from this defect. Next: the author's decision on arming "full".

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

Can a default-off user response keep a user from fixing on a code whose satellite code bias the stream does not carry,
release a hold already taken on one, and leave every other hold and the position unchanged?

## 2. Mechanism (Report 1, section 1)

1. The stream carries a satellite's code biases as one record per epoch that replaces the previous one
   (`SsrCorrectionStore::pushCodeBias`); a signal left out of an epoch has no bias at the user from that epoch on.
2. The code row is then built with a bias of zero (`addSsrBias`, `ObservationEquation.cpp`) and stays in the update.
   The only reaction is a re-inflation of the constellation's receiver code-bias states at both edges
   (`reinflateOnCodeBiasDisc`); the satellite stays eligible for a fix (the AR gate reads phase biases only).
3. The network leaves the signal out whenever fewer than two stations carry it (`ApplicationNetworkAssembly.cpp:624`;
   the emitted code biases are the estimator's columns). Card CPW's hold keeps the parked value as a known term inside
   the network and does not emit it.
4. A missing E5a bias b moves the code's wide lane by f5 / (f1 + f5) b / lambda_WL (1.21 cycles for E18's -2.128 m,
   0.78 for E32's -1.376 m). It matters when the satellite's ambiguities are loose during the gap: a network phase-bias
   reset (E18, day 146) or a new arc (E32 rose inside its gap, day 147). The fix then takes the neighbouring wide lane
   with the matching narrow lane, (3, 4) cycles, which moves the range by 5.6 mm and the slant ionosphere by 0.56 m.

## 3. Response (default off)

Key `network.closed_loop.code_bias_gap_response`:

- **Detection, both edges.** A (satellite, code signal) is in a gap when the stream corrects the satellite this epoch (a
  fresh code or phase bias of another of its signals) and holds no code bias for this signal (the apply site's
  freshness test). Open and close are events with counters and identities (log lines `Code-bias gap OPEN` /
  `CLOSE`; satellite-trace actions `cbgap_open`, `cbgap_close`). A satellite the stream does not correct at all is the
  unrecorded-satellite rule's domain and is left alone.
- **"full", open edge.** The bias-less code row is left out of every pass of the update (the BSSD pivot's row is kept
  and counted). The satellite's ambiguities that are not held are barred from the integer search while the gap is
  open and for the first `code_bias_gap_revalidate_ep` epochs after it closes; a hold taken before the gap on clean
  code is kept.
- **"revalidate" and "full", close edge.** The satellite's re-validation window opens for `code_bias_gap_window_ep`
  epochs. Every hold of the satellite in the window is tested over `code_bias_gap_revalidate_ep` consecutive epochs
  of clean code: the held wide lane minus the receiver's Melbourne-Wubbena corrected with the stream's biases, less the
  per-epoch median of the same quantity over the held satellites of the system. A mean beyond half a cycle releases
  the satellite (its ambiguities are re-floated, the resolver forgets its held pairs, a new window opens); otherwise
  the hold is validated. Half a cycle is the rounding boundary of the tested quantity.
- **Counters** (run-end line `Code-bias gap response`): gaps opened and closed, signal-epochs in a gap, rows left out,
  pivot rows kept, satellite-epochs barred, windows, validated, released, expired, held epochs tested, closure.
- **Not in the RT snapshot:** a restored consumer starts with no open gap and no window (field-blocking only for a
  warm-resumed consumer, which no reference posture runs).

Options weighed (Report 2 gives the costs): (a) this signal-level response; (b) the satellite-level rule, an extension
of `network.closed_loop.drop_unrecorded_phase_bias` to code biases, which leaves the whole satellite out and releases
every hold carried through a gap; (c) the network side, emitting the parked value of card CPW with a quality mark,
which changes the products and needs a network rerun.

## 4. Tests and bars (fixed before the runs)

**Unit tests** (`tests/test_code_bias_gap.cpp`): the edges and their lengths, the rows left out, the bar and its
length, validation, release and a new window, an expired window, a gap superseding a window, the median removal, the
corrected Melbourne-Wubbena and the size of a missing bias. Every one passes; the full Debug suite has no new failure.

**Simulator, one injected event** (`runs_irm/_rap_irm_day24_s/products.txt`, DOY 129 of 2024; E14's C5Q and C5X code
biases removed from 14:00:00 to 14:29:30, 60 epochs; `tools\inject_cb_gap.py`). User SZZ000SIM, four starts, 3 h each:
A 12:00 (fixes E14 before the gap), B 14:00 (every float born in the gap), C 14:15 (born 15 minutes before the return),
D 14:35 (after the gap); arms off, revalidate, full, and a control on the untouched products. Truth: every hold of
10 epochs or more against the receiver's corrected Melbourne-Wubbena (`tools\wl_truth.py`, frame integer from the
control). E14's missing bias is -2.253 m: 1.28 cycles.

| Bar | Statement |
|---|---|
| S1 | The off arm shows E14's C5Q code bias empty in the user trace at exactly the 60 gap epochs. |
| S2 | The defect on the simulator: at least one of B and C off holds E14's wide lane 0.7 cycle or more from the truth. If neither does, the simulator shows the response is harmless but not that it helps; the observed cases carry that. |
| S3 | full: no hold of E14 more than 0.5 cycle from the truth in A to D; A's hold of E14 survives the gap (no release); one gap opened and closed in each user that tracks E14 across the edges, rows left out equal to the gap epochs it spends in the solution. |
| S4 | revalidate: every wrong E14 hold of B and C off is released within the window, and no hold that the truth calls right is released, in any user. |
| S5 | off identity: the off arm equals the same-exe run without the key in every user log file and trace (masked lines only as in card URS's comparison). |
| S6 | Position: after the gap, every hour's 3D median of full and revalidate within 1 cm of off, per user. |

**Observed** (user replays on the poster's network products; ARHT00ATA day 146 from 01:00 for 4 h, day 147 from
10:00 for 3.5 h; arms off, revalidate, full):

| Bar | Statement |
|---|---|
| O1 | off reproduces the defect: E18 held (91, 99) after 03:46 on day 146 (a 01:00 user held it in card IOF section 18), E32 held at wide lane -3 on day 147, if the 10:00 start fixes E32 inside its gap. |
| O2 | full: E18 and E32 hold the wide lane of the clean code (offset below 0.5 cycle in `wl_truth.py`), and no other hold changes its verdict. |
| O3 | revalidate: every wrong E18 or E32 hold of off is released within the window, and no hold the truth calls right is released. |
| O4 | Position: from the hour after each event, every hour's 3D median of full and revalidate within 1 cm of off. |

**Readings fixed now.** If S3, S4, O2 and O3 pass and S5 holds, "full" is the candidate posture for the user (both
edges), with "revalidate" as the release that also covers a hold taken by a consumer without the open-edge response.
A failure of S4 or O3 on a right hold (a release of a hold the truth calls right) stops the revalidate path until its
statistic is understood. If O1 fails on day 147 (the 10:00 user fixes E32 after the return), O3 for day 147 is read
on whatever hold the off arm takes. Nothing here is armed in a reference file or merged without the author's own words.

## 5. Question 5 (Report 2)

The count of wrong holds of the poster's day-long users, with and without these two events, from the truth tool of
Report 1.

## Results, 2026-11-22

### 6. The response on the simulator and on the two observed cases

Engine `gnss_processor_cbg1.exe` (sha256[:16] 01cede27227cca58, card commit 1788d73 plus the response); 36 user replays
in `gates\e18_wide_lane\runs\cbg_*`, every one exit 0; grade `gates\e18_wide_lane\cbg_grade_all.txt`
(`tools\cbg_grade.py`); full account in `gates\e18_wide_lane\REPORT_2.md`.

- **Unit tests.** The 73 tests of the touched suites pass (12 new, `tests/test_code_bias_gap.cpp`); the full Debug suite
  of the branch: 5,111 tests in 688 suites, 5,017 passed, 94 skipped, 0 failed (`gates\e18_wide_lane\tests_full.log`).
- **Simulator, gap only.** The simulator replay processes its first epoch about 31 minutes after its start offset, so
  the first B, C and D started after the gap; B2 and C2 were added 31.25 minutes earlier (first epochs 14:00 and
  14:15). User A holds E14 from 13:21 through the gap in every arm (validated at 14:39, -0.12 cycle). No user fixes E14
  in or soon after the gap, the control included: S1, S3, S5, S6 met; S2 not met by the gap alone.
- **Simulator, gap plus the network's declared phase-bias reset of E14 at 14:15** (counter +1, level unchanged, the
  pattern of E18 on 26 May; added because section 2 item 4 needs loose ambiguities during the gap). Control (reset, no
  gap): refixed at (-15, 159), right. Off: refixed at 14:36 at (-18, 155), the step (3, 4), offset +0.77, held to the
  end (S2 met). Revalidate: released at 14:45 (+0.78 over 20 epochs), refixed right, validated (S4 met). Full: refixed
  right, validated (S3 met). Position within 5 mm in every hour (S6 met).
- **Observed, day 146 (user from 01:00).** Off: E18 refixed at 03:46 at (17, 31), offset +1.00, held to the end (O1
  met). Revalidate: released at 03:56:30 (+1.06), refixed at 04:05 at (20, 35), validated (O3 met). Full: 63 code rows
  left out, 48 satellite-epochs barred, refixed at 03:52 at (20, 35), validated (O2 met).
- **Observed, day 147 (user from 10:00).** Off: E32 fixed at 12:07, 20 minutes after the return, at (477, 480), wide
  lane -3, offset +0.89 (O1 met in the form section 4's readings foresaw). Revalidate: released at 12:17:30 (+0.84),
  refixed at 12:28 at (480, 484), wide lane -4, validated (O3 met). Full: 127 rows left out, 146 satellite-epochs
  barred, fixed at 12:07 at (480, 484), validated (O2 met).
- **No right hold released:** 3 releases, each on a hold the truth calls wrong; 9 validations within 0.19 cycle;
  3 windows ended with no hold to test. No other hold changed its verdict in any run. Position: at most 1 mm per hour
  on both observed days (O4 met).
- **Off identity (S5).** Key absent, key written "off", and key absent on the engine of cade9f5: identical in all 29
  log files and stdout after card URS's three masks.

Reading (section 4, fixed before the runs): S2 to S6 and O1 to O4 pass, so "full" is the candidate posture of the
fixing user, with "revalidate" as the release for a hold taken without the open-edge response. Not armed: the author's
decision. Options (b) and (c) of section 3 are costed in REPORT_2 section 3.

### 7. Question 5: a correct count for the day-long users

Judge: the receiver's own Melbourne-Wubbena corrected with the network's biases over each hold's clean epochs (the
twin check cannot see a step (3, 4), and in a gap the twin's float is the side that moves). Day-long users, 100 of 105
graded, Galileo: 10,313 hold segments, 1,569,672 held satellite-epochs. Wrong beyond doubt: 5 segments, 607 epochs,
all from a missing code bias (ARHT E18 day 146, ARHT E32 day 147). Between 0.5 and 0.7 cycle: 106 segments, 7,741
epochs, mostly E11 and E19 at -0.5 to -0.6 at many receivers (a satellite-wide half-cycle code-phase disagreement of
the network's biases, which the code cannot decide), not counted. GPS not graded (Report 1: a +2 wide-lane frame on
ten satellites and tails that need their own look). The poster's 85 starts: no wrong Galileo hold from a gap.
