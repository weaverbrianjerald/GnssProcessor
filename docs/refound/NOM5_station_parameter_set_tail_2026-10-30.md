# NOM5: the network's station parameter set, finished (card NOM step N6, steps N6f to N6j)

**STATUS**

- **id:** NOM5
- **topic:** network-vehicle
- **status:** open
- **summary:** REOPENED 2026-10-30 for section 6, the gradient's bound: `network.tropo_gradient_prior_sigma_m`, a zero-mean prior row per gradient column and epoch (default 1 mm), pre-registered before any run (section 6.1). Sections 1 to 5, closed 2026-10-30 on branch `nom/n6-tail` (not merged; the merge is the author's decision): BUILT and GATED (section 4). N6f: the producer's data rows are sparse (`StationDataRows`, written straight into the shared border and the local block); every output identical (24 of 24 network arms with the snapshot, 16 of 16 user arms, real time apart from wall-clock text); the loop cost flat within 0.15 % (the bar passes; the expected gain not shown). N6g: with `estimation.seed_full_equation` the producer is byte-identical and the consumer moves by under 1 mm at its births and not at all by hour; every clause of the recommendation bar passes, a neutral change; arming is the author's decision. N6h: a single-receiver layout flag in a network run with no consumer is refused at load (no configuration under `config/` sets one); `network.estimate_tropo_gradients` was added through the set alone and its conditioning clause passes, but its uncarried columns fit noise at their formal sigma on the sim bed (no gradient in the truth) and at HKSL00HKG absorbed part of a station-level step (the gradient reached 390 mm) while other arcs' carried levels moved by metres: field-blocking (standing invariant 3), a bound owed. N6j: the tests, onboarding pages 7 and 8, the `CLAUDE.md` routing and lint check V13. Both keys stay UNVALIDATED and default off. At pre-registration: card NOM's step N6 landed its body (steps N6.0 to N6e, merge `f64f916`, 2026-10-20: the network owns its station parameter set, and one observation equation serves the producer, the consumer and the seeds); the merge named N6f to N6j as remaining. This card builds them on branch `nom/n6-tail`: N6f, direct assembly (no dense Jacobian on the network; byte-identical; cost expected lower); N6g, the seeds on the whole equation behind `estimation.seed_full_equation` (default off, fingerprinted; the consumer moves at births); N6h, a user-side layout flag that reaches no parameter of a network run refused at load, and `network.estimate_tropo_gradients` (default off, fingerprinted) as the first station parameter added through the set alone; N6j, the close (tests, onboarding pages 7 and 8, `CLAUDE.md` routing). N6i (a canonical column order) is not taken (decision D2's default).
- **source:** `scripts/nom3_designs/N6_design.md` (sections 3 to 7; decisions D1 to D10, defaults taken); the merge `f64f916` ("N6f to N6j remain"); `docs/onboarding/08_adding_a_network_parameter.md` (the recipe and its open rough edge on gradients); `docs/refound/UOM_n6_user_side_edits_2026-10-17.md` (the user-side notices of N6a and N6b)
- **knobs / posture:** N6f and N6j: no key. N6g: `estimation.seed_full_equation` (new, default false, fingerprinted, ledger UNVALIDATED). N6h: `network.estimate_tropo_gradients` (new, default false, fingerprinted, ledger UNVALIDATED); the load-time refusal of section 2.3 (no key). Section 6: `network.tropo_gradient_prior_sigma_m` (new, PARAM, default 0.001 m, read only with the gradients). Base: the goldens of main `5127429` (card BOF's posture; baseline pair `runs/_rf0hv17_{field,sim}6`).
- **dates in the text:** 2026-10-30
- **hashes / run dirs:** branch `nom/n6-tail` from main `5127429`: pre-registration `6821ea0`, N6f `4381a61` (test pin `f9b4dac`), N6g `3748df0`, N6h `bf32eee`, N6j `1a36c66`, section 4.1 `e28b324`, comment edit `30d5d4b`; worktree `D:/GnssProcessorSupport/worktrees/n6tail`; executables under `D:/GnssProcessorSupport/gates/exe/x64-Release/`: control `gnss_processor_n6tctl.exe` (the worktree's Release build of `5127429`, before any change, sha256[:16] `7696875ef0d8c41f`; its copy `gnss_processor_n6tctl2.exe`), N6f `gnss_processor_n6tf.exe` (`1bcd0e640ee44aea`, build of `4381a61`), final `gnss_processor_n6th.exe` (`bf865f5b79546234`, build of `bf32eee`, the processor of the tip); gate records `D:/GnssProcessorSupport/gates/n6t/` (N6f), `final/` (G0, G4), `g3/` (G3), `g1final/` (the Debug suite at the tip); run root `D:/GnssProcessorSupport/runs_nom`: `_rap_g5_n6tctl2_*` and `_rap_g5_n6th_*` (G0), `_rap_n6t_g3{off,on}_{f2,s2,f6,s6}` and their census replays (G3), `_rap_n6t_g4{off,on}_{f,s}2` (G4)
- **gates (frozen at pre-registration):** section 3, written before any run of a changed executable, with amendment A1 (section 3.1). G0, G1, G2, G3 and G5 pass; G4's conditioning clause passes and its estimates are field-blocking (section 4.4). Section 6.1 (frozen 2026-10-30, before any run of a bounded executable): G6a to G6c for the bound

**Outcome.** Question: can card NOM step N6 be finished as designed? Result: yes, one key excepted. The producer's
rows are assembled sparsely, every output identical, the cost flat (no gain shown). The full-equation seeds leave the
producer identical and move the consumer under 1 mm at its births only: every bar clause passes, a neutral change. A
layout flag that reaches no parameter is refused at load. The troposphere gradient conditions well, but its unbounded
columns fit noise and at HKSL00HKG absorbed part of a station-level step: field-blocking. Standing: branch `nom/n6-tail`, not
merged; both keys UNVALIDATED; the bound, the arming decision and the merge owed (section 5).

## 1. Question

Can the station parameter set of card NOM step N6 be finished as designed? Four parts: the producer's data rows
assembled straight into the shared border and the local block with no output bit moved (N6f); the seeds read the whole
observation equation behind a key, with the consumer's births graded (N6g); a user-side layout flag that reaches no
parameter refused, and a new station parameter added through the set alone (N6h); the tests and the reader's pages
brought to the finished structure (N6j).

## 2. The steps

### 2.1 N6f: direct assembly (no key)

At `5127429`, `networkStationRows` (`network/assembly/StationRows.cpp`) fills a dense matrix H of m rows by one column per
slot of the station's set (zeroed, and one zeroed vector of the set's length per row), and `networkSplitStation`
(`StationSplit.cpp`) copies its columns into `A_s` (the border) and `A_r` (the local block). The congruence step
(`network/qc/Congruence.cpp`) reads the three position columns of the same H, and `rxIfbExcludedRows` reads its
receiver IFB columns.

After N6f the row loop keeps each row's partials as (slot, value) pairs in the order `scatterDense` writes them; a slot
written twice in one row keeps the last value, as the dense assignment does. The split decides each slot's
disposition by the same tests in the same order (position pinned; a shared entry with a border column; the receiver
IFB rule; a slot whose every partial is plus or minus zero is unobserved, the `maxCoeff() == 0.0` meaning for an
explicit zero partial; a slot whose nonzero partials are all on excluded rows is unobserved) and writes every pair,
zeros included, into `A_s` or `A_r`. When two shared entries name one border column, the later entry of the split's
loop order owns it, as the column copy does today. The congruence step builds its m by 3 position block from the
pairs; `rxIfbExcludedRows` reads the pairs.

Expected: no output bit moves; the field golden's loop is faster (no dense H, no column copies).

### 2.2 N6g: the seeds on the whole equation (`estimation.seed_full_equation`)

`kLegacySeedMask` (`measurement/Seeds.hpp`) reproduces the two pre-N6 copies of the equation, which leave out the range
correction, the satellite phase bias, the satellite clock, the SSR bias, the phase colour and (for the residual seeds)
the phase clock. With the key on, a seed subtracts every known term of its row (the mask `kFullSeedMask`, its exact
term list stated in this card when built, with the order of subtraction extended at the end of `kSeedOrder` so the
legacy terms keep their order), and every seed takes the carrier frequency of the satellite's own GLONASS channel (the
open frequency item of `Seeds.hpp`). The key is fingerprinted: a resumed run never crosses it.

Expected: the producer is unchanged with the key on (at a producer birth the omitted terms are zero: shared states at
0, no SSR term, range correction 0; the goldens carry no GLONASS); the consumer moves at births, because its SSR code
and phase biases (metres) leave its inter-system bias, clock, code-bias and ambiguity seeds.

### 2.3 N6h: the layout flags and the gradient (`network.estimate_tropo_gradients`)

**The flags.** Decision D7's default is a load error, in network mode, for `estimation.estimate_velocity`,
`estimation.estimate_phase_clock`, `estimation.estimate_tropo_gradients` and `imu.enabled`, naming the network key. Its
premise (design risk 7: such a flag gives a network station extra columns) was removed by N6e, and a finding of this
pre-registration narrows it: the closed-loop and replay consumers are configured by a copy of the network
configuration (`GnssConfig::deriveClosedLoopConsumerConfig`, `GnssConfigLoadNetwork.cpp`), which keeps these flags, so
in a network run with a consumer they set the consumer's layout. The setup's warning ("a network station has no such
parameter; it is ignored", `ApplicationNetworkSetup.cpp`) is therefore wrong for a run with a consumer. N6h refuses a
flag only where it reaches no parameter of the run (network mode with no consumer), naming the network key where one
exists, and states the flag's scope (the consumer, not the stations) otherwise. The six flags of today's warning
(the four above, `estimation.state_ambiguity_basis nl_wl` and `estimation.between_satellite_sd.enabled`) take one rule.
The configurations under `config/` are scanned for the flags in network mode before the rule lands.

**The gradient.** `network.estimate_tropo_gradients` gives each station two local columns, north and east, born at 0 on
its first epoch with the key on, through the recipe of onboarding page 8 (the `ParamRef` kind, the set's field, the
registry row, the inventory class, the contribution sets, the labels, the loader line, `NetworkKnobs`, the ledger row
UNVALIDATED, a run-end line). The observation equation already holds the term
(`mf_grad * (cos(az) * G_N + sin(az) * G_E)`). A first version is uncarried: estimated afresh every epoch, as the
golden posture's wet delay is. Page 8 leaves one decision open: whether uncarried gradient columns keep the station
blocks well conditioned or need a prior. That decision is graded here (gate G4); a prior
(`network.tropo_gradient_prior_sigma_m`, a zero-mean pseudo-observation per column per epoch) is built only if the
uncarried columns fail it.

### 2.4 N6j: the close (no key)

The 11 files of `tests/network` that still name `StateManager` or `StateKeys::` at `5127429` are re-pointed where the
name stands for the network's own set (a test of a user-side helper keeps it); onboarding page 8 takes the gradient as
its worked example and updates its rough edges, page 7 (the network tour) and the code tour follow; the `CLAUDE.md`
network routing names `StationParams` and the equation for a new station parameter; `docs/status/OPEN_ITEMS.md` and
`nom_epoch_lint.py` are brought up to date.

## 3. Gates (frozen 2026-10-30, before any run of a changed executable)

Executables: Release builds of the worktree, renamed copies under `D:/GnssProcessorSupport/gates/exe/x64-Release/`;
the control is the build of `5127429` before any change. Every difference a gate shows is a finding to trace, not to
explain away; nothing lands with an untraced difference.

- **G0, identity (N6f; N6g and N6h with their keys absent).** The 23 G5 arms and the snapshot bytes
  (`gates/g7/gate_pair.sh`): 24 of 24 IDENTICAL. The 16 user arms (`scripts/uom_gate.py`, quick set): 16 of 16
  IDENTICAL. Real time (`sbf`, `sbf_bssd`, `sbf_te`; `cl`): identical apart from wall-clock text. For N6h the declared
  text moves are the setup's flag lines and the run-start tables' rows that name the new kind; nothing else.
- **G1, checks.** The Debug suite of the worktree passes (fixture skips only); `nom_epoch_lint.py`,
  `nom_n4_verify.py --whole`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader`,
  `ledger_render.py --check` and `docs_index_check.py` pass.
- **G2, cost (N6f).** Two timing pairs per golden bed on 2 h arms (control and candidate at the same time, the launch
  order swapped in the second pair, `scripts/cost_attrib/abba.sh`): the loop per epoch is not slower than the control by
  more than 1 % on any pair (the design allowed +3 % per body step and expects N6f below zero); the station-add section
  is reported.
- **G3, full-equation seeds (N6g, key on, against the same executable with the key off).** The producer's products and
  traces on both 2 h goldens are byte-identical (the prediction of section 2.2; a move is a finding traced before any
  grading). The consumer: `WTZZ00DEU` by hour on the 6 h field golden, `SZZ000SIM` from 00:30:15 on the 6 h sim golden,
  and the 16-site census (`gcensus.py`; `simcensus6h.py` with `SIMCENSUS_BASE`). Recommendation bar (decision D6):
  hours 1 to 6 not worse at the held-out consumers by more than 1 mm in any hour, the census median site not worse,
  and no census site worse by more than 1 cm (card NSP: a neutral change moves no site by more than 0.19 mm, so a site
  past 1 cm is a response). The arming decision is the author's.
- **G4, the gradient (N6h, key on, against the same executable with the key off).** The three run-start tables list
  the two columns per station (estimated-local counts, integrity lines at 0); the run-end line counts the columns
  estimated. Conditioning, on 2 h field and sim arms: no station block refused or dropped beyond the control's count
  (the kernel's positive-definiteness refusals and `sta_pd_dropped` events); the per-station robust spread of the
  epoch estimates reported (the sim truth has no gradient, so its estimates are noise); the held-out consumers and the
  wire against the control reported, with no adoption claim. A failed conditioning clause builds the prior of
  section 2.3 and repeats G4 with it.
- **G5, the refusal (N6h).** Unit tests: each flag in a network configuration with no consumer fails to load and the
  message names the network key where one exists; with a consumer the configuration loads and the setup states the
  flag's scope; outside network mode nothing changes. The configuration scan of section 2.3 lists every file the rule
  refuses.

### 3.1 Amendment A1 (2026-10-30, before any run of an N6g executable)

The configuration toggle convention (`CLAUDE.md`, "Config toggles") puts a user-visible key on the run-start
`Toggles:` line. `estimation.seed_full_equation` takes the field `seed_full=` at the end of that line, so G0 for N6g
reads identical apart from that one line of `stdout.txt` and `main_log` (`seed_full=off`) in every arm. No other text
moves are declared for N6g.

## 4. Results

### 4.1 2026-10-30: N6f, direct assembly

**Build.** Control `gnss_processor_n6tctl.exe` (sha256[:16] `7696875ef0d8c41f`), the worktree's Release build of
`5127429` before any change; candidate `gnss_processor_n6tf.exe` (`1bcd0e640ee44aea`), the Release build of
`4381a61`. Both hashes were unchanged after every run. An 11-epoch field golden smoke on the two executables read
IDENTICAL in every part (`nom_td_gate.py --whole`: products, every trace, the manifest, the whole logs, the exit
code). Gate records: `D:/GnssProcessorSupport/gates/n6t/` (`summary_n6tf.txt`, `uom_cmp_n6tf.log`,
`rt_cmp_dr.log`, `rt_cmp_cl.log`, `dbg_suite.log`, `check_*.log`, `g2_report.txt`, `nc_table_pair{1..4}.txt`).

**G0, identity: pass.** The 23 G5 arms and the snapshot bytes: 24 of 24 IDENTICAL (arms `a4`, `gate`, `joint` and
`zero` exit 1 on both sides, as on every earlier side). User side, quick set: 16 of 16 IDENTICAL. Real time: the
`dr` arms (`sbf`, `sbf_bssd`, `sbf_te`) differ in one line each, the wall-clock "Real-time session ended ... ms
wall"; the `cl` arm differs in four lines of `console.log` and of `main_log.log`, all wall-clock (RINEX ingestion
seconds, the epoch-loop wall, the round-trip self-check seconds, the `COST:` tail of the arc-ledger line), the
manifest in `compiled`, `written_utc` and `loop_ms`, and `network_service_health.csv` in its `ms_per_epoch_window`
column. Every other file is byte-identical.

**G1, checks: pass.** The full Release build found one more reader of the removed member: the compile-time pin of
`StationBuild`'s hand-off in `tests/network/test_network_station_build.cpp`, fixed test-only in `f9b4dac` (the
executable unchanged). The Debug suite of `f9b4dac`: 3652 tests in 497 suites, 3619 passed, 33 skipped (the
fixture-gated list), 0 failed. `nom_epoch_lint.py`, `nom_n4_verify.py --whole`, `docs_index_check.py`,
`make_golden_config.py --verify`, `config_posture_check.py --verify-loader` and `ledger_render.py --check` pass.

**G2, cost: pass on the bar; no gain shown.** Concurrent pairs on 2 h goldens (`scripts/cost_attrib/abba.sh`),
launched only after a quiet check (no build, no foreign processor run):

| pair | bed (launched first) | loop ms/epoch, control / candidate | assembly wall | `est.addStation` | products |
| --- | --- | --- | --- | --- | --- |
| 1 | field (control) | 1428.4 / 1427.3 (-0.08 %) | 288.1 / 292.6 | 245.5 / 248.5 | identical, 113889 records |
| 2 | field (candidate) | 1182.3 / 1182.5 (+0.01 %) | 260.7 / 259.8 | 220.1 / 217.4 | identical |
| 3 | sim (control) | 838.2 / 838.9 (+0.08 %) | 145.8 / 147.2 | 117.6 / 118.9 | identical, 80691 records |
| 4 | sim (candidate) | 645.5 / 644.6 (-0.14 %) | 145.6 / 147.2 | 115.8 / 116.9 | identical |

No pair is slower by more than 1 % (largest +0.08 %), so the clause passes. The design's expectation of a lower
cost is not borne out: the loop is flat within 0.15 %, and the two section rows move within 1.6 % with mixed signs.
`est.addStation` receives byte-identical blocks on both sides, so its own spread (-1.3 % to +1.2 %) is the noise of
a section row in these pairs; the removal of the dense matrix's zero fill and column copies lies below it.

**Verdict, N6f.** Identity, checks and the cost bar pass. The network forms no dense matrix of rows by slots, at
no measurable cost and no measurable gain.

### 4.2 2026-10-30: the final build with both keys absent (G0); checks at the tip (G1)

**Build.** Candidate `gnss_processor_n6th.exe` (sha256[:16] `bf865f5b79546234`), the Release build of `bf32eee`;
step N6j (`1a36c66`) and the comment edit `30d5d4b` change no C++ of the processor, so it is the executable of the
branch tip. Control `gnss_processor_n6tctl2.exe`, a copy of the section 4.1 control (`7696875ef0d8c41f`). Both hashes
were unchanged after every run. The machine was shared with gate G3's runs and other sessions' runs (the lists are in
`stages.log`); no gate of this section reads time. Gate records: `D:/GnssProcessorSupport/gates/n6t/final/`
(`REPORT_FINAL.txt`, `g0a_aggregate.txt`, `g0a_detail/`, `g0b_uom_detail.txt`, `g0b_rt_dr_detail.txt`,
`g0b_rt_cl_detail.txt`, `stages.log`).

**G0, identity: pass; the text moves are the declared ones.** The 23 G5 arms: every product file, every CSV trace
(26 to 39 per arm), `products.txt`, `rt_engine.log`, every module log other than `gnss_config.log`, and the exit codes
are identical (arms `a4`, `gate`, `joint` and `zero` exit 1 on both sides). The snapshot bytes are identical apart from
the write time in its first record, which the gate masks; its fingerprint field is equal, and the reading arm `snapr`
exits 0. The text of every arm differs in two classes, and no line falls outside them:

1. The `Toggles:` line (amendment A1): the control's line plus `, seed_full=off`, in `stdout.txt` and
   `logs/gnss_config.log`. Amendment A1 named `main_log`, which does not carry the line.
2. The rows of the new kind, estimated "no": a `tropo_gradient` row in "Network (param model)" and "Network (param
   responses)" (`stdout.txt` and `main_log`), and in the run manifest one `param_model` entry and one entry in each of
   the four data kinds of every `contributions` record (`data_code`, `data_code_companion`, `data_phase`,
   `data_phase_second`, whose parameter lists name every kind a row can touch, armed or not). The param model table is
   printed again at run end ("Network (param model, run end)") with the same row: the one move that the frozen G0
   text, which named the run-start tables, did not name.

User side, 16 arms: each differs in the `Toggles:` line alone (`logs/gnss_config.log`, `stdout.txt`); every CSV,
every other log and every exit code (0) is identical. Real time: `sbf`, `sbf_bssd` and `sbf_te` differ in the
`Toggles:` line alone (`console.log`, `gnss_config.log`; 30 to 32 files identical); `cl` adds the class 2 rows and
records and wall-clock text (the cost tail of the arc-ledger line; the manifest's wall-clock keys and the
service-health timing columns, which `rt_seam_compare.py` masks), with 35 files identical.

**G1, checks at the tip `30d5d4b`: pass.** `nom_epoch_lint.py` (its new check V13: 304 network files, none names
`StateManager`, `MeasurementModel`, `StateInitializer` or `EpochMeasurement` outside comments),
`nom_n4_verify.py --whole`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader`,
`ledger_render.py --check` and `docs_index_check.py` pass. The Release unit suite of `bf32eee`: 3665 tests in 498
suites, 3632 passed, 33 skipped (the fixture-gated list), 0 failed. The Debug suite of `30d5d4b` (records
`D:/GnssProcessorSupport/gates/n6t/g1final/`): 3665 tests in 498 suites, 3632 passed, 33 skipped (the same 33 names as
at `f9b4dac`), 0 failed; the 13 tests added since `f9b4dac` (`NetworkLayoutFlagsTest` 7, `SeedsTest` 4,
`StationBirthTest` 2) pass, and none was removed. `gnss_simulator_selftest`: 99 of 99 passed.

### 4.3 2026-10-30: N6g, the full-equation seeds (G3)

**Arms.** The final executable, with the key the only difference; 2 h and 6 h arms on both goldens, all exit 0.
Records: `D:/GnssProcessorSupport/gates/n6t/g3/` (`td_gate_{f,s}{2,6}.txt`, `table_field6.txt`, `table_sim6.txt`,
`table_sim6_from0030.txt`, `rowdiff_{field,sim}6.txt`, `census_field_grade.txt`, `census_field_exact.txt`,
`census_sim_grade.txt`, `census_sim_persite.txt`, `replay_active.txt`); runs
`D:/GnssProcessorSupport/runs_nom/_rap_n6t_g3{off,on}_{f2,s2,f6,s6}` and the census replays beside them.

**The producer is unchanged, as predicted.** On both beds, at 2 h and at 6 h, `products.txt` (113,888 lines field and
80,766 sim at 2 h), all 31 `logs/network_*.csv`, the run manifest, `main_log` and the exit code are identical with the
key on. The consumer's files differ: `closed_loop_trace.csv`, `closed_loop_isb.csv` and `closed_loop_sat_trace.csv`
from their first row, the consumer's per-epoch lines (`rt_engine.log` and their copies in `stdout.txt`), and the
`Toggles:` line (`seed_full=on`).

**The consumer moves at its births only, by under a millimetre.** At the consumer's first epoch the full seeds take
the SSR terms out of the seeds (field, `closed_loop_isb.csv` and `closed_loop_sat_trace.csv`: the Galileo
inter-system bias 2.8025 to 2.8058 m, the receiver clock -30768.3625 to -30768.3653 m, and the phase innovation of
G22 L1W at its birth -28.106 m, its SSR phase bias, to 0), and the first update absorbs the rest: the
held-out consumer's 3D error changes by 0.7 mm on the field's first row and by at most 0.2 mm in the sim's first hour,
and no row after hour 2 changes by more than 0.05 mm. Held-out consumers, p50 of `pos_err_3d_m` by hour (m), off and
on equal to 0.1 mm in every hour:

| consumer | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| `WTZZ00DEU`, from the first row | 0.0588 | 0.0123 | 0.0231 | 0.0123 | 0.0165 | 0.0120 |
| `SZZ000SIM`, from the first row | 0.1368 | 0.0723 | 0.0307 | 0.0239 | 0.0257 | 0.0182 |
| `SZZ000SIM`, from 00:30:15 | 0.1004 | 0.0274 | 0.0374 | 0.0215 | 0.0247 | 0.0161 |

The off rows reproduce the baseline pair `_rf0hv17` by hour. The GLONASS channel part of the key is not exercised:
the goldens carry no GLONASS.

**Census.** Every one of the 33 replay pairs ran with the key live (`seed_full=off` and `on`, differing consumer
files). Field (`gcensus.py`): median site p50 by hour 0.142 0.050 0.036 0.029 0.032 0.030 m off and on; paired change
over hours 2 to 6 +0.0000 m (better 0, worse 0, n 16), hour 1 +0.0000 m; largest hourly change of any site +0.15 mm
(site SCRZ, hour 1). Sim (`simcensus6h.py`): 0.397 0.065 0.029 0.029 0.025 0.024 m off and on; change +0.000 m in
every hour, better and worse at 1 cm 0 and 0; largest site change 0.1 mm (hour 1).

**Bar (decision D6).** Held-out consumers not worse by more than 1 mm in any hour 1 to 6: largest change +0.0 mm on
both beds, pass. Census median site not worse: equal on both beds, pass. No census site worse by more than 1 cm: none
(largest 0.15 mm), pass. The arming decision is the author's.

**Verdict, N6g.** With `estimation.seed_full_equation` the seeds read the whole observation equation. The producer
is byte-identical with the key on, and the consumer moves at its births by under a millimetre and not at all by
hour: every clause of the recommendation bar passes, as a neutral change.

### 4.4 2026-10-30: N6h, the refusal (G5) and the troposphere gradient (G4)

**G5, the refusal: pass.** Before the rule landed, none of the 1,334 configurations under `config/` set one of the
six flags in network mode. In the unit suite (`tests/network/test_network_layout_flags.cpp`, 7 cases, among the 3632
passed), each flag in a two-station network configuration with no consumer fails to load, the message ending "so it
reaches no parameter; the network stations' key is network.estimate_tropo_gradients" for the gradient flag and "so it
reaches no parameter; a network station has no such parameter" for the other five; with a consumer each loads; with
no flag both forms load; the network key loads alone, and a top-level `estimate_tropo_gradients` stays the
single-receiver flag's legacy spelling. The rule runs only in the network section's loader and returns at once when a
consumer is configured, so outside network mode nothing changes (the 16 user arms, section 4.2). The setup's scope
line for a run with a consumer and a flag set is not covered by a test; no golden sets a flag.

**G4, the gradient.** 2 h arms of both goldens, with the key the only difference, all four exit 0. Records:
`D:/GnssProcessorSupport/gates/n6t/final/g4_field.txt`, `g4_sim.txt`, `g4_halfhour.txt`; runs
`D:/GnssProcessorSupport/runs_nom/_rap_n6t_g4{off,on}_{f,s}2`.

*Arming.* The on arms' "parameters as armed" tables list `tropo_gradient.north` and `tropo_gradient.east`
estimated-local at every station (58 field, 60 sim) at the first epoch and over the run (13,919 and 14,400
station-epochs, none unobserved); the integrity lines are 0 (the field's one unreached station-epoch, a station with
no observations, is on both arms). The run-end line, field: "Network (troposphere gradients): north estimated in 13919
station-epoch(s) and left out as unobserved in 0; east 13919 and 0; uncarried (estimated afresh every epoch)."
`logs/network_tropo_gradient.csv` holds 13,919 and 14,400 rows; the off arms write none.

*Conditioning, the clause of G4: pass on both beds.* No `sta_pd_dropped` event and no `n_sta_pd_dropped` on either
arm of either bed; no station-epoch rejected after the split; unreached 1 and 1 (field), 0 and 0 (sim); 240 of 240
epochs emitted on every arm, none withheld. By the frozen rule, no prior is built.

*The estimates.* Hours 2 onward; per station the median, the robust spread (1.4826 MAD) and the median formal sigma,
then the median over stations (mm):

| bed | median north | median east | spread north | spread east | sigma north | sigma east |
| --- | --- | --- | --- | --- | --- | --- |
| field, 58 stations | -1.5 | +0.3 | 2.7 | 3.9 | 4.7 | 6.6 |
| sim, 60 stations (truth 0) | +0.7 | -1.1 | 3.0 | 4.2 | 4.9 | 7.1 |

The sim truth carries no gradient, and its estimates have the field's size and spread: the single-epoch estimates
are noise at the level of their formal sigma. The tails are larger. In hours 2 onward, 48 of 58 field stations and 54
of 60 sim stations exceed 10 mm in magnitude at some epoch, and 8 field and 13 sim stations exceed 30 mm (the sim's
largest 70 mm, S31000SIM).

*HKSL00HKG.* Its gradient read a few millimetres until 01:00 (formal sigma 6 to 10 mm), then -224 mm north and
+315 mm east at 262800, decaying over the next hour (-20 and +37 mm at 266100); its largest magnitude is 390 mm, at
262890. The two arms' event logs at the station agree until 262770, after the IF-TD test had refloated Galileo E04
six times in nine minutes on both. At 262770, with the key off, the arc attribution refloated E04, E13, E19, E26 and
E33 (as it had refloated E04, E13 and E19 at 262650), and an hour later the station's prior-residual reports read 4.3
to 7.1 against the threshold 4 (residuals -0.02 to -0.06 m). With the key on, no attribution refloat fired at the
station: the gross-LOS screen declared E04 (statistic 16.3 against the 12 m cold bound) and erased it; the same epoch's prior
residuals moved GPS arcs' carried levels by metres (G14 -7.7 m on L1C and -9.3 m on L2W; G17, G30 and G09 by 0.7 to
3.2 m); the redundancy census flagged the local column `amb:E04_L1C` unobservable at 262800; and the prior-residual
reports read 346 to 385 on E13, E19 and E26 at 263220 to 263280 (residuals -0.19 to -0.35 m). The two free columns
absorbed part of a station-level step; the attribution test that answers such a step did not trip; the carried
levels of other arcs took the rest, and the uncarried gradient fit the mismatch for an hour. Over the run the
attribution refloats fell from 30 to 5 (field) and from 14 to 5 (sim).

*Effect.* Held-out consumer, p50 of `pos_err_3d_m` by hour (mm):

| consumer | hour | off | on | change |
| --- | --- | --- | --- | --- |
| `WTZZ00DEU`, from the first row | 1 | 58.8 | 55.1 | -3.7 |
| | 2 | 12.3 | 11.3 | -1.0 |
| `SZZ000SIM`, from the first row | 1 | 136.8 | 204.2 | +67.3 |
| | 2 | 72.3 | 52.3 | -20.0 |
| `SZZ000SIM`, from 00:30:15 | 1 | 100.4 | 137.5 | +37.1 |
| | 2 (30 min) | 34.8 | 38.7 | +3.9 |

`products.txt` differs in almost every line (as multisets, 113,379 of the field's 113,888 lines appear in the off arm
only; every B, P and U record). Wall under shared load: field 1039 and 1054 s, sim 386 and 394 s. No adoption claim
(2 h arms).

**Verdict, N6h.** The refusal passes G5. The gradient was added through the set alone (the kind, the set's field, the
registry block, the inventory class, the contribution sets, the labels, the loader line, `NetworkKnobs`, the ledger
row, the run-end line, the trace), and its conditioning clause passes. The estimates fail a test the clause does not
make: uncarried and unbounded, the two columns fit noise at their formal sigma on a bed with no gradient, and at
HKSL00HKG they took up a station-level step that the attribution test answers with the key off, moving other arcs'
carried levels by metres. Triage (standing invariant 3): field-blocking. `network.estimate_tropo_gradients` stays
UNVALIDATED and is not field-runnable until its columns are bounded (the zero-mean prior of section 2.3, or a carried
random walk); the bound and a repeat of G4 with it are owed.

## 5. Owed at close

1. **The gradient's bound** (section 4.4): the prior `network.tropo_gradient_prior_sigma_m` of section 2.3 or a
   carried random walk, then G4 repeated with HKSL00HKG's episode and the attribution-refloat count read on both
   beds.
2. **The arming decision on `estimation.seed_full_equation`** (section 4.3, the author's).
3. **The merge of `nom/n6-tail`** (the author's), and the `docs/status/OPEN_ITEMS.md` lines of items 1 and 2, kept
   off the branch because the main checkout holds another session's uncommitted edits of that file.

## 6. 2026-10-30: the gradient's bound (section 5, item 1)

### 6.1 Design and gates (frozen 2026-10-30, before any run of a bounded executable)

**A prior, not a carry.** Section 5 named two bounds. A carried random walk on the diagonal route would hold each
gradient as a prior taken from the previous epoch's marginal posterior: the route of `network.carry_zwd`, the falsified
station-common double count of the wet delay (ledger KEEP_OFF: the quiet-sim consumer 0.204 to 0.723 m, divergent on
the field). The gradient columns share the station's arcs with the carried ambiguities as the wet delay does, so that
route would count the same information twice; its corrected form is the in-block vehicle, out of regime on both
goldens. The zero-mean prior of section 2.3 adds what no epoch's data supplies, the physical size of a gradient, and
derives nothing from an earlier posterior: it counts nothing twice and links no epoch to the next. Standing invariants 1
and 7 add no response: the prior carries no stiff time-linked quantity, and it is the same in every epoch, so nothing
decouples or re-couples at an edge.

**The prior.** `network.tropo_gradient_prior_sigma_m` (new, PARAM, default 0.001 m). With
`network.estimate_tropo_gradients` on, every station block gains one row per gradient column and epoch: +1 on the
column, target minus the column's linearization value (the row holds the gradient toward absolute zero), weight
1/sigma^2. A new contribution kind, `tropo_gradient_prior` (a station constraint row, after the test-only
zero-ionosphere rows; row class local pin row; the RAIM pool does not admit it), built by the step
`tropo_gradient_prior` of `kStationBuildSteps` in `network/assembly/StationAnchors.cpp`. The default is the physical
size of a gradient, not a fit: the single-receiver filter of this code seeds its gradients with the same 1 mm
(`filter.initial_std.tropo_gradient`), and the field's per-station medians of section 4.4 (-1.5 mm north and +0.3 mm
east at the median station) are of that order. One epoch of data determines a gradient to 5 to 7 mm (section 4.4), so
the prior dominates each epoch: the expected estimate is about 1/26 of the data-only one, and the bounded columns act
as a stochastic-model term for azimuthal delay more than as an estimate. A converging estimate needs a carried
gradient, which waits on the in-block vehicle. 0 removes the prior (the columns of section 4.4). With the gradients
armed, the fingerprint gains `tgrad_prior:<sigma>`. The run-end gradient line states the prior and its rows, and warns
INERT when the prior is armed, a gradient was estimated and no row was built.

**Declared moves.** With the gradient key absent: the new kind's row and entries (count 0) in the contribution tables
(run start and run end; `stdout.txt`, `main_log`) and in the manifest's `contributions` records, and the gradient
kind's `source` text, which names the prior, in the param model tables and the manifest's `param_model` record. With
the key on and sigma 0: the same, the run-end gradient line's clause on the prior, and the fingerprint token.

**Gates.** Candidate: the Release build of the branch with the bound. Identity control: `gnss_processor_n6th.exe`
(`bf865f5b79546234`, section 4.2).

- **G6a, identity with the gradient key absent.** The 23 G5 arms and the snapshot bytes against the control's arms
  `_rap_g5_n6th_*`: identical apart from the declared moves. The 16 user arms: identical. Real time: `sbf`,
  `sbf_bssd` and `sbf_te` identical apart from wall-clock text; `cl` apart from the declared moves and wall-clock text.
- **G6b, the prior is the only change.** The key on with `tropo_gradient_prior_sigma_m` 0, 2 h field and sim arms of
  the candidate against section 4.4's on arms (`_rap_n6t_g4on_{f,s}2`): products and every CSV trace identical; the
  text apart from the declared moves.
- **G6c, the bounded gradient.** The key on at the default, 2 h field and sim arms, against the key-off arms of the
  candidate:
  1. *Arming.* The new kind counts two rows per station-epoch with both gradient columns estimated, the tables'
     integrity lines at 0; the run-end line states 1.00 mm and the same count.
  2. *Conditioning.* No station block refused or dropped beyond the key-off arm's count.
  3. *Bounded on the sim bed.* No station's gradient exceeds 3 mm (three prior sigma) in magnitude at any epoch from
     hour 2 (the truth has no gradient). The field: section 4.4's table of the estimates and the count of stations
     past 3 mm, reported.
  4. *HKSL00HKG.* The station's gradient stays within 3 mm from 262650 to 266100, and its attribution refloats from
     262650 to 262800 name the arcs that the key-off arm's name there (E04, E13, E19, E26 and E33).
  5. *Attribution refloats.* On the sim bed, at least 75 % of the key-off arm's count over the run. The field is
     reported: a bounded gradient can take up a real gradient that the key-off arm answers with refloats, so the
     field's count may fall for a sound reason.
  6. *Effect.* The held-out consumers (`WTZZ00DEU`; `SZZ000SIM` from the first row and from 00:30:15) by hour against
     the key-off arm: none worse by more than 5 mm in hour 2.

  Verdict: with G6a, G6b and every clause of G6c passed, the field-blocking finding of section 4.4 is answered (triage
  field-inert), the key stays UNVALIDATED, and adoption remains a 6 h grade with the census, not made here. A failed
  clause is traced and reported, and the key stays not field-runnable.
