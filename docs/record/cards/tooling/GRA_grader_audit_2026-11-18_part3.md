# GRA part 3: plan steps G6 to G10 and the frame classes of the simulator's wrong holds (results of card GRA, continued)

**STATUS**

- **id:** GRA-part3
- **topic:** tooling
- **status:** closed
- **summary:** Results of card GRA continued from [part 2](GRA_grader_audit_2026-11-18_part2.md), which reached the size bar. Plan steps G6 to G10, carried out on 2026-11-19 at the author's go, under `scripts/grading/`: `truth.py` (G7: the run-end truth lines by kind, the frame-free cover in the recorded seed-0 order and in a sorted order, the frame class of every wrong held chord and hold), `compare_runs.py` (G6), `code_signature.py` (G8), `grade.py` (G9) and the retire listing (G10). Each is gated on numbers already recorded. The class view answers the managing session's question on the simulator beds' wrong holds. The clean-checkout re-check of every gate closes the entry.
- **source:** the managing session's order of 2026-11-19 (the author's "Yes, go" for G6 to G10; G7 first, for the class split); [part 1](GRA_grader_audit_2026-11-18.md) and part 2, section 5 (the plan); the package held-signature-evidence's rules of 09:14 (`D:\GnssProcessorSupport\gates\day_program_2026-11-19\packages\held-signature-evidence\parts\sim-power-by-class.md`); the outputs in `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\` (`g6`, `g7`, `g7c`, `g8`, `g9`).

**Outcome.** Plan steps G6 to G10 are built, and each reproduces its recorded numbers. G7 gives the run-end truth lines, the eleven seed-0 frame-free covers and the guard lines byte for byte. G6 reproduces the off-identity comparisons and member checks. G8 gives the arc spread and mode trace byte for byte and every number of the code signature. G9's orchestrated outputs equal the direct ones, and G10's retire listing finds no call of a retired script. On the simulator, 85 of h40_off's 86 wrong holds are one wrong tree link. Isolated wrong holds exist (14 in all) and show no residual signature.

## Results

### 2026-11-19: plan steps G6 to G10

**The go.** The author, 2026-11-19, to "Go ahead with the remaining grading steps G6 to G10 (about 1.45 M tokens, no
runs)?": "Yes, go" (relayed by the managing session, with the order: G7 first, then the frame classes of the simulator
beds' wrong holds, then G6, G8, G9, G10 and a clean-checkout re-check of every gate). Nothing in the old folders was
changed; no engine run, no build.

**What was built** (branch `audit/graders`, commits `cb20aeb` and `524d4dc`, under `scripts/grading/`).
- `truth.py` (G7; bars B5, B6). `runend` reads the engine's three kinds of truth line by name (the run's truth audit,
  the audit file's line, the cascade clauses; `RingFixSeams.cpp:1791`, `:1832`, `:1842`); `--view shell` is
  `grade_c3.sh:16` (the last line holding "held link-epochs right", which would be the audit file's line if a run named
  one). `cover` is `frame_free_count.py` written again (`truth.py:compute_cover`): `--order seed0` takes the greedy's
  candidates in a Python set's order under `PYTHONHASHSEED=0` and runs itself again under that seed when it is not set;
  `--order sorted` breaks ties in sorted order whatever the seed; each band's tree search is kept per satellite (the
  original searches once per chord; the predecessors up to the station are the same); `--cache` keys a cover by run,
  bed, stride, order and the ring-fix trace's sha256 (defect D6). `guard` gives `grade_guard_sim.py`'s truth and cover
  lines. `classes` gives the frame class of every held chord-epoch the audit calls wrong, by the rules the package
  held-signature-evidence wrote at 09:14 before looking at any case: frame-type (the cover reduces the chord's error
  through a tree link, or a link two wrong chords pass; also (0, 0) under a wrong audit), isolated (only through the
  chord's own link), another shape (not a multiple of (3, 4)), unresolved; each wrong hold (`held_integrity.py`'s holds)
  takes the majority class of its rows, a tie going to isolated, then another shape, then frame-type. Frame-type rows
  reduced only through links no other held chord passes are counted apart: there the cover cannot tell a tree link's
  error from the chord's own.
- `compare_runs.py` (G6; bar B12). `compare --view legacy` is the grading folder's `compare_runs.py`; `compare`
  (identity) compares whole files first by sha256, names the wall-time columns once (`WALL_TIME`), sets aside only the
  member records each run's legend declares ("# phase families (network.phase_families): ..."), checks each against
  its identity's record, counts the rest by type and signal and names the undeclared copies, and reports a trace in one
  run only. `members`: `--view awk` (`grade_n46.sh:24-30`), `--view check` (`check_member_records.py`), the legend view.
- `code_signature.py` (G8; bar B13). One reader of the postfit code rows, the ring-fix trace and the band-1 tree pins,
  by header name, with no awk program and no cache. `links` is `gf_code_signature.py` (legacy keeps its cache's
  rounding: each mean to 0.0001 m, the difference again, the held share to 0.001), `arcs` is `arc_signature_spread.py`,
  `modes` is `mode_code_trace.py`; the exact views are unrounded with lists in a fixed order.
- `grade.py` (G9). One orchestrator per bar group (`pair`, `network`, `sim`, `users`, `identity`, `signature`); a child
  that exits with a code it does not accept fails the grade (the two-run audit's exit 1 on a failing check is a
  result); `MANIFEST.json` records the grading commit (and uncommitted changes), the Python version, each child's
  arguments, exit code, duration and output sha256, and the size and time of every input file.
- `RETIRED.md` and `retire_listing.py` (G10). The 78 old scripts each owned, partly owned, a copy or named nowhere,
  with their owners here; the check that every listed file and owner exists and that no script of `scripts/grading`
  calls a retired script or an old folder (an import, a command line, a path; a docstring naming the script a view
  reproduces is not a call).
- Frozen cases: 12 more on `D:\GnssProcessorSupport\gates\grader_fixtures_2026-11-19b\` (the first two hours of
  `h40_off` with its bed's truth, hour 1 of `n46c_n146` and `s2_n146`, hour 7 of `s2_n146` and `d24_n146`, two hours of
  `h1_off`; 147 files, 202 MB, `MANIFEST.sha256` sha256 `a0e77b81028bbcccea2cd909fbefd17060ae159428ab1b2a6214f367949b19c0`),
  made by `gates/grader_audit_2026-11-18/freeze_fixtures_1119b.py` and `make_frozen_1119b.py`. All twelve self-tests
  pass with all 38 frozen cases (`selftests_g6_g10.txt`).

**The gates** (outputs in `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\`, scripts `gate_g6.sh`, `gate_g7.sh`,
`gate_g8.sh`, `gate_g9.sh`).

| step | recorded number and its source | here | agreement |
| --- | --- | --- | --- |
| G7 run-end lines | the four truth lines of `G/c3_n46.txt` (`m1_c` 404 958 right, 0 wrong) | `truth.py runend --view shell` | byte-identical |
| G7 frame-free cover | `G/cover_seed0_<tag>.jsonl` of section 108.1's ten runs and `d1_ev` (590 on `d1_off` and `d1_ev`, section 108.4) | `cover --order seed0 --cover-out` | all 11 byte-identical; the original's printout on `d1_off` byte-identical |
| G7 guard lines | the 15 truth and cover lines of `G/guard_sim_grades_seed0.txt` (d1 26 and 758 against 0 and 590; d31 346 and 3 against 275 and 0; h40 52 and 32 against 52 and 23) | `guard` | byte-identical |
| G6 legacy | `G/c2_n46c_n146_vs_s2_n146.txt`, `c2_n46c_n147_vs_s2_n147.txt`, `c2_n46cr_n146_vs_s2r_n146.txt`; the m1_c / u1_m1 block of `c3_n46.txt` | `compare --view legacy` | all byte-identical |
| G6 members | `G/members_n46c_n146.txt`, `_n147`, `members_n46cr_n146.txt` (344 249 and 343 432 member records, 0 unequal); `G/member_n42.txt` | `members --view awk`, `--view check` | byte-identical |
| G6 identity | H1 (`u2_z45` against `u2_z`), the off pairs `m1_off45` / `m1_off37`, `h40_off45` / `h40_off`, F1 (`f40_h40off` / `h40_off`): products byte-identical, the traces equal but for the wall-time files (sections 112.1, 116.1) | `compare` | products identical by sha256; 39 of 42 traces identical, 3 identical but for their wall-time columns, on every pair |
| G8 arcs, modes | `G/arc_signature_spread.txt` (section 108.3), `G/mode_code_n41_n146.txt` (section 112.6) | `arcs`, `modes` | byte-identical |
| G8 links | `G/gfrep_s2_n146.txt`, `gfrep_s2_n147.txt`, `gfrep_n41_n146.txt`, `gfrep_n41_n147.txt` (sections 107.2, 112.2) | `links` (legacy; the n41 pair with `--list 30`, as recorded) | every number equal (`compare_gfrep.py`): the counts, histograms, hours and pair lines as text, the tie-ordered lists as sets |
| G9 | the direct gate outputs of `two_run.py` on `ad_n146` / `adr_n146` and of `truth.py classes` on `h40_off` | `grade.py pair`, `grade.py sim` | byte-identical; both manifests list no failure |
| G10 | none (a listing) | `retire_listing.py` | 78 rows present, 8 owners present, 0 calls of a retired script or an old folder from `scripts/grading` |

**The frame classes of the simulator's wrong holds** (`truth.py classes`, every epoch, holds wrong when more than half
their rows are wrong, giving the scout's 86, 40 and 14; the cover's ties in sorted order; `g7c/classes_<run>.txt`).

| bed | wrong holds | frame-type | isolated | another shape | in the seed-0 order |
| --- | --- | --- | --- | --- | --- |
| `h40_off` | 86 | 85 | 1 | 0 | the same |
| `d1_off` | 40 | 30 | 10 | 0 | 26 and 14: four holds change class with the tie order |
| `d31_off` | 14 | 11 | 3 | 0 | the same |

- Every wrong row is a near-null step: (3, 4) and (-3, -4) on all three beds, and (6, 8) on `d1_off` (9 939 and 12 161
  rows; 5 007, 5 198 and 153; 1 299 and 2 272), the package held-signature-evidence's own count (its part
  `sim-signature-attack.md`, T3). No row has another shape; the rebuild finds no chord off where the audit says right.
- On `h40_off` the wrong holds are one wrong link of the tree, not a satellite. 21 869 of its 22 100 wrong chord-epochs
  are reduced through the single tree link S24000SIM with E02, a near-null step off; the chords that inherit it run
  through that link from 30 stations and many satellites (E36 2 044 chord-epochs, E33 1 390, E31 1 344, E24 1 237, E02
  1 121, and more). The 86th hold is the isolated chord S45000SIM with E02. These are the two errors the package names.
- On `d1_off` 18 links stand behind the 40 holds (seven tree links: S31000SIM-E07 behind 10 holds, S13000SIM-E19 5,
  S49000SIM-E26 5, S23000SIM-E21 3, S23000SIM-E19 2, S39000SIM-E08 2, S41000SIM-E10 2; and eleven holds' own links); on
  `d31_off` 8 links behind 14 (S02000SIM-E19 behind 6, S02000SIM-E07 2, ...).
- Isolated wrong holds exist: 14 in sorted order (1, 10, 3), 18 in the seed-0 order. None looks off by its own
  residual: means from -2.19 to +2.27 mm, one-sided shares 0.50 to 0.63, median absolute residuals 1.87 to 6.81 mm, no
  hold with the one-hour signature. S45000SIM E02 on `h40_off`: 1.90 h, 229 rows all wrong, mean -0.34 mm, one-sided
  0.55, median absolute residual 1.91 mm.
- With the other rule (any wrong row): `h40_off` 226 holds, 225 frame-type and 1 isolated; `d1_off` 59, 47 and 12;
  `d31_off` 27, 21 and 6.

**What the gates add.**
- The greedy cover's count depends on its tie order: in sorted order `d1_off` takes 524 links against 590 under seed 0,
  `d1_nn` 709 against 784, `d31_off` 272 against 275; `h40_off` and `h40_nn` are unchanged (section 108.4 found 545 and
  510 under random seeds on byte-identical runs). The recorded seed-0 numbers stand as recorded; a new grade states its
  order.
- The class view needs the arcs from the run's start. On a window cut at TOW 275400 of `h40_off`, every arc starts at
  the cut and takes the wind-up branch there: 11 433 chord-epochs come out off where the audit says right, and shapes
  such as (-4, -5) and (2, 3) appear. From the run's start (the frozen case), the rebuild agrees with the audit
  everywhere.
- The C2 pair (`n46c_n146` against `s2_n146`) by its legend: 172 156 declared P member records set aside, all equal to
  their identities; 86 078 B records on C1X and 86 015 on C5X that the legend does not declare, every one a copy of the
  same epoch's C1C or C5Q record; besides the held-out user's three traces, `network_solution.csv` (its member PB rows)
  and `network_stations.csv` (the phase_tracking column the legacy view drops) differ.
- The old code-signature tool's lists of equal counts and its pair examples follow the order of awk's arrays and of a
  Python set under a random seed, so no rewrite reproduces them byte for byte; every number does.
- The retire check reports 31 kept orchestrators of the old grading folder that still call a retired script (the
  `grade_*.sh` family among them). They keep working, since nothing is deleted.

**Not covered.**
- `truth.py classes` rebuilds Galileo chords only (the beds' wrong holds are all Galileo); a GPS chord is unresolved.
- The class of a `d1_off` hold can depend on the cover's tie order (four holds); frame-type rows reduced only through
  links no other chord passes (34, 26 and 1 rows on `d1_off`, `d31_off`, `h40_off`) cannot be told from isolated ones.
- The package held-signature-evidence counts 25 isolated segments and 42 events over its own segmentation (412
  segments of consecutive same-error rows); those numbers are not compared here.
- `grade_guard_sim.py`'s moves and dispositions, `grade_entry_sim.py`'s refusals and `grade_l12_parts.py`'s values mode
  stay with the old scripts (partly owned in `RETIRED.md`).
- `two_run.py` has none of the audit's wind-up options; `scripts/nrf_two_run_audit.py` keeps them.

**The clean-checkout re-check** (the independent check the package named; one reviewer that had not written the
scripts, from `git archive 524d4dc scripts/grading` into a folder of its own; its report
`D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\recheck_1119\REPORT.md`). All twelve self-tests pass from
the clean copy with their frozen cases; the retire check gives the same four lines; seven gates rerun against recorded
files are byte-identical (`slip_hold_self_ad_n143_{E,G}` printouts and JSON lines with the chord view's one case,
`audit_ad_n146` printout and JSON, the 179 resumptions, `cover_seed0_d1_off.jsonl`, the four truth lines of `c3_n46.txt`,
`members_n46c_n146.txt`, `arc_signature_spread.txt`, `five_days_free_combi_EL1XL5X_146.txt`). Its own counts, made
without the scripts, agree with every corrected view tried:
- F5: BOGT00COL G09's resumption at 572730 on L1C and L2W, its four held rows at -0.653 to -0.571 m and the release at
  572850; every GPS ring-fix row has the chord L1C/L2W, and three flags the chord view lists apart are on L2S, L1W and
  L5Q.
- F4 at TOW 181020, band E|L1C: of 4 696 four-link loops over 395 common links, the 22 that fail all contain
  GUAM00GUM-E29 (22 of its 22 loops); without it, none fails.
- The classes at TOW 276000 of `h40_off`: the six wrong L1C/L5Q chords' tree paths run through S24000SIM-E02 in both
  bands; S45000SIM E02 (L1X/L5X) does not, that edge being in no L1X or L5X tree.
- The C2 pair's undeclared records: 86 078 B records on C1X, all equal to the same epoch's C1C record.
- The one-signed hold GUAM00GUM G09 of `ad_n146`: 558 rows, mean -14.38 mm, one-sided 0.986.
Not checked by it: unknown chain rows (every row at that epoch was known), the fixtures' derivation, the source.

## 2026-11-20: closed

Closed with its parent card GRA by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); it follows the parent into the record.
