# Workbook: the network's estimation theory and reference choices

Coursework of 35 questions in seven modules, built from this project's own record, on the
estimation theory of the network processor and its reference (datum, S-basis) choices. It was
written for the status update of 29 October 2026
(`D:\GnssProcessorSupport\presentation\status_update_2026-10-29\status_update_2026-10-29.tex`):
each module bounds one part of that report, and each ends with a claim question, what the
presentation may say on that part and what it may not. Every answer in
[answer_key.md](answer_key.md) names its source (card and lines, file and lines, or run and
command) and, where one exists, the command that reproduces it.

## How to work it

1. Work the modules in order; each takes 45 to 60 minutes, each question about ten.
2. Read the module's opening section and its figures. Open a source only when a question sends
   there.
3. Write each answer before opening the answer key. An answer is complete when it names its
   source.
4. Compare with [answer_key.md](answer_key.md). Where an answer differs, rerun the key's
   **Check** command.
5. Keep the seven claim answers together: they are the speaker's limits for the meeting.

Question kinds, mixed in every module: **derive** (a short calculation), **read** (what a given
line of a card, header, configuration block, trace row or log line says), **measure** (run an
existing tool on an existing run and interpret the number) and **claim**.

**Before the measure questions.** Commands run from `C:\GnssProcessor` in Git Bash, with
`C:/Python313/python.exe` (the Python on PATH lacks numpy). Several runs and the clock data set
are on D: (`D:/GnssProcessorSupport/runs_nom/`, `D:/GnssProcessorSupport/fixtures/`), which must
be connected. No question builds the code or starts a new run; every command reads an existing
run, and all of them together take about two minutes.

## The modules

| Module | Topic | Part of the report it bounds | Questions |
| --- | --- | --- | --- |
| [1](01_observation_model.md) | The uncombined equations, the unknowns of a station and of the network, the rank deficiency 1 + 2f + (1 + f)(n − 1 + m) and the slant-ionosphere term | "One observation model"; the step 6 check of undetermined directions | 4 |
| [2](02_reference_choices.md) | What a reference choice fixes, the datum-invariance theorem, the four kinds of choice, the four choices of the reference configurations, the rank gate, each station's level direction | Step 5 row "Reference choices, integers"; the step 6 model | 6 |
| [3](03_what_a_user_cannot_see.md) | Why a user cannot see a reference choice; grading only datum-invariant quantities; the header line of reference choices; the reference-satellite trap | Step 3 (case D matches case C); the step 5 clock method | 5 |
| [4](04_satellite_clock.md) | The temporal anchor as a time constant, the phase-clock hold and what it passes to a user, the clock-error simulation, the self-contained and broadcast-clock postures | Step 5 clock precision; the next step "clocks free of final products" | 5 |
| [5](05_float_and_integer_ambiguities.md) | Why an integer is mostly ionosphere, the float choice, complete and partial pins, the fixed-cycle fold, the integer-recovering datum | Step 3 integers; the step 6 integer result | 5 |
| [6](06_simulated_network.md) | What the simulated network certifies, the start artifact, grading in the network's basis, perfect corrections and the user floor | Step 6 | 5 |
| [7](07_real_data_and_claims.md) | The test station by hour, 16 further stations, five days, the clock against CODE and GFZ, the cost of a clock free of final products, the three messages | Step 7; the three messages for the meeting | 5 |

## Terms

The workbook uses plain words; the cards, code and configurations use the record's labels.

| In this workbook | In the record | Meaning |
| --- | --- | --- |
| our network, the network processor | producer, network producer | `runNetwork`: estimates satellite clocks and biases from many stations |
| corrections, the correction file | the wire, `products.txt` | what the network writes per epoch: C (clock), B (code bias), P (phase bias) and other records |
| user | consumer, closed-loop consumer | a receiver positioned with the corrections |
| test station | held-out station | a reference station left out of the network and positioned as a user: WTZZ00DEU (real data), SZZ000SIM (simulated) |
| 16 further stations | census | users positioned afterwards from the stored corrections (`scripts/clock_level/gcensus.py`) |
| real data | field, field bed | 58 IGS stations, 8 May 2024 (day 129) |
| simulated data set | fixture, sim bed | `runs/simnet60_quiet_col_24h` (quiet), `D:/GnssProcessorSupport/fixtures/simnet60_clk_col_24h` (clock error) |
| reference configuration | golden | `config/golden/golden_{field,sim}_refound.json`, generated, never hand-edited |
| case | arm | a copy of a reference configuration with one setting changed |
| setting | knob | a configuration key |
| step | rung | a section of the status report |
| the first hour | ignition | hour 1 after a cold start |
| reference choice | datum, S-basis, gauge | the constraints that fix the directions no data determine |
| header line of reference choices | S line | line 2 of `products.txt` |
| perfect corrections | truth wire | corrections written from the simulator's truth (`scripts/simnet_truth_wire.py`) |

## Figures

The 19 figures in [figures/](figures/) are downscaled copies (1.4 MB in all) of figures made for
this project: the status report and its results (`results/t1` to `t3`), the deck of 23 October
2026 (`figures/deck/slideNN_k.png` beside the report, extracted from the deck; the deck itself is
hand-edited and is never regenerated) and the September 2026 concept figures
(`D:\GnssProcessorSupport\presentation\figures\`). Each caption names its original; caption
paths starting `results/`, `figures/` or `slide` are relative to the status report folder
(`figures/deck/` for the slides). Three companions are not copied:

- the concept animation, `D:\GnssProcessorSupport\presentation\figures\network_concept.gif`
  (two receivers, two satellites: the arrowhead, the three pins, the station elimination and the
  shared solve, 33 s); figures 1.1 and 2.1 are two of its frames;
- the claude.ai page "GnssProcessor: from network to user"
  (<https://claude.ai/artifact/WpS4ViBMp1kK2KTUdNrS5b>), the deck as a page with speaker notes,
  in the record's labels;
- the claude.ai page "Linear Algebra in GNSS"
  (<https://claude.ai/artifact/DSJYw38fdNmuJ6zFgGSUP9>), worked examples that reproduce their
  numbers: the Schur complement (module 1), a network with a datum defect and two choices of
  datum (module 2), double differences sharing a reference satellite (module 3) and pull-in
  regions (module 5). Its table of code paths predates the network reorganization.

Both pages are private to the author's claude.ai account. Pages from before the re-founding of
2026-09-14 (the "Network Brief" and the clock-level review pair) describe retired postures and are
not used.

## Report statements the answers qualify

| Report statement (line of the .tex) | What the evidence supports | Answers |
| --- | --- | --- |
| "One model and one filter carry every step" (125) | one model; one user filter; the network processor is a separate estimator | Q1.4, Q7.5 |
| "a few reference choices are fixed" (548) | 226 rows and 120 prior entries at the first simulated epoch | Q2.3, Q2.6 |
| "stated in the correction file" (443) | the real-data header line omits the Galileo code-bias pin pairs | Q2.6, Q3.2 |
| "keeps each satellite's published phase bias constant" (491 to 493) | the armed hold keeps clock minus phase bias constant; the effect stands | Q4.5 |
| "hours 1 to 6: GPS 4.3 cm, Galileo 7.6 cm, against 1.4 and 2.6 cm" (488 to 489) | 01:00 to 06:00 set against CODE over 00:00 to 06:00; like for like CODE reads 1.20 and 2.23 cm | Q7.3 |
| "a user of our corrections is 1.7 cm worse today" (495) | the native posture against the GPS-only reference of 2026-10-07; the clock's share about 1.0 cm | Q7.4 |
| "faulty in their first 30 minutes" (611 to 613) | 9 extrapolated epochs; 00:30:15 is a grading origin | Q6.2 |
| "42 % of the eligible ambiguities ... were fixed, none wrongly" (563 to 565) | a 2 h case of card ARL without phase colour, log only; "eligible" undefined | Q5.5, Q6.5 |
| "median of the 16 stations 3.9 cm" (631 to 632) | the stored grades give 3.8 cm (median) or 4.1 cm (mean) | Q7.2 |
| "WTZZ00DEU is the best station" (650) | true on 8 May 2024, not on 10 May; it shares its site with two network stations | Q7.1, Q7.2 |

## Verification log

Checks run on 2026-10-24 against the current tree (branch `card/scb-clock-bed`, 77a2154) and the
runs named. Each reproduced the answer key; three statements gathered for the key failed their
check and were removed (last rows).

| # | Answer | Check | Result |
| --- | --- | --- | --- |
| 1 | Q1.1, Q1.3 | `sed -n '380p;639p;658,686p'` of `runs/_rf0hv15_sim6/stdout.txt`; `scripts/rf_null_space_read.py runs/_rf0hv15_sim6` (9 s) | table as given; `nshared=280`; eigenvalues 4.036e-11 and −1.803e-10 with weights 0.2040 and 0.1767; cutoff 1.0459e-6; weakest real 1.124e-4; run end min 2, max 2 |
| 2 | Q2.2, Q4.3 | `scripts/make_golden_config.py --verify`; `src/core/config/GnssConfigLoadNetwork.cpp` lines 311 to 319, 2612 to 2617 | "golden verify: PASS"; the refusal and the warning read as quoted |
| 3 | Q2.4 | B records of G05 against the navigation file | C1C −3.2108400641 m = c·T_GD; C2W/C1C = 1.646944 = μ2 |
| 4 | Q3.3 | the datum-step script on `runs/_rf0hv15_sim6/products.txt` | 31 satellites, mean step 1.6278 m, 0.0027 m after the mean; an ordinary epoch pair −0.0001 and 0.0008 m |
| 5 | Q3.4 | `scripts/simnet_amb_integer.py ... --ref-free` (11 s) | as traced 0.304 cycle, 65.3 %; reference-free 0.122 cycle, 81.6 %; last quarter 0.260 and 0.092 cycle |
| 6 | Q4.4, Q6.3 | `scripts/simnet_clock_truth_grade.py` on `_rap_scb_{g,c,n}_k6`, `_rap_scb_{g,n}_q6` and `runs/_rf0hv15_sim6` (with and without `--no-ds`) | every number of the answers, including the native user 1.1 to 2.1 cm higher on the clock data set in hours 1 to 3 |
| 7 | Q5.4 | `scripts/simnet_wire_integer_consistency.py` (4 s); `scripts/simnet_producer_ar_grade.py` (4 s) | wide lane 0.252 to 0.260 against a null of 0.250; "5572/5572 = 100.00%", "WRONG FIXES: none" |
| 8 | Q7.1, Q7.2, Q7.3 | `scripts/rapid_arm.py score`; `scripts/clock_level/gcensus.py grade galpc galp galpg`; the five-day script; `results/t3/clock_stats.csv` | the hourly medians, −0.0198 m with 10 better and 2 worse; 0.038, 0.0406 and 0.018 m; CODE 01:00 to 06:00 1.20 and 2.23 cm |
| 9 | all derive answers | one script with the arithmetic of Q1.2, Q2.5, Q4.1, Q4.2, Q5.1, Q5.2 | as given (6.661°, cos 0.99325; 0.933 mm; 2,099 s; 0.1998 m; 116.2 m; 369, 361, 1,349, 465) |
| 10 | removed | the claim that the 16 further stations have no co-located network station | false: KOKB00USA and network station KOKV00USA carry the same approximate position in their RINEX headers (`data/2024-129/obs/`) |
| 11 | removed | "integer fixing equals the float within 0.1 mm at ALGO00CAN" | the cited line (`docs/status/OPEN_ITEMS_CODE_DEFECTS.md` line 55) gives a different comparison |
| 12 | removed | per-satellite agreement of the code-bias records "to 3e-11 m" | came from a scratch script, not a repeatable command; replaced by the run's own count (stdout line 631) |
