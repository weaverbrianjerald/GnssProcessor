# 6 · First tasks

A list of small, real, open tasks on the single-receiver PPP and real-time path. Each is
the size of the [worked example](03_worked_example.md) or smaller, and each was checked
against the code on 2026-09-19. File and line references drift; the function or symbol
name is the reliable anchor.

A task is chosen together with a maintainer, who also confirms that nobody else has
started it. The same list can be entered as GitLab issues with the label
`good-first-task`; the text below is written to be pasted as the issue body.

| # | Task | Kind | Size |
| --- | --- | --- | --- |
| [T1](#t1-register-the-unit-suite-with-ctest) | Register the unit suite with `ctest` | build | 1 h |
| [T2](#t2-rewrite-configreadmemd) | Rewrite `config\README.md` | docs | 2 h |
| [T3](#t3-warn-when-a-constellation-is-silently-dropped) | Warn when a constellation is silently dropped | code + test | ½ day |
| [T4](#t4-remove-machine-specific-paths-from-the-sample-configs) | Remove machine-specific paths from the sample configs | config | ½ day |
| [T5](#t5-decide-the-fate-of-productmanager) | Decide the fate of `ProductManager` | code | ½ day |
| [T6](#t6-make-the-output-directory-configurable) | Make the output directory configurable | code + test + docs | 1 to 2 days |
| [T7](#t7-absolute-reference-for-compare_ppp_runspy) | Absolute reference for `compare_ppp_runs.py` | Python | 1 day |
| [T8](#t8-end-of-run-position-error-summary) | End-of-run position error summary | code + test + docs | 2 days |

---

## T1: Register the unit suite with ctest

**Problem.** `CMakeLists.txt` calls `enable_testing()` but the only `add_test` entry is
`gnss_simulator_selftest`. `ctest --test-dir build\x64-Debug` therefore never runs
`gnss_tests`, although older documentation suggests it does.

**Change.** Add `add_test(NAME gnss_tests COMMAND gnss_tests)` next to the existing
entry. The working directory must be the repository root if fixture-gated tests are to
find `data\` (`WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`).

**Done when.** `ctest --test-dir build\x64-Debug` lists and passes two tests, and
`docs\OVERVIEW.md` (section Testing) states the correct behaviour.

## T2: Rewrite config\README.md

**Problem.** `config\README.md` describes `ppp.json` and `ppp_rtk.json`, which do not
exist, and the pre-2026 flat data layout.

**Change.** Replace it with a table of the top-level configs that exist today (the
[code tour](02_code_tour.md#the-config-file) has the PPP and real-time subset), one line
on each sub-directory (`golden\`, `sim\`, `refound\`, `test_suite\`, `archive\`), and the
rule that files named `_my_*.json` are personal scratch files.

**Done when.** Every file name mentioned in the README exists, checked with a short
script or by hand, and `src\main.cpp`'s comment that points at the README is still true.

## T3: Warn when a constellation is silently dropped

**Problem.** In `PppModel.cpp`, the canonical-signal selection loop skips a constellation
that has no entry in `processing.signal_priorities`:
`auto pit = signal_priorities_.find(cons); if (pit == signal_priorities_.end()) continue;`
No message is logged. A config that lists GLONASS or BeiDou in `active_systems` but not
in `signal_priorities` runs to completion with those constellations contributing
nothing, and the result is bit-identical to a GPS-only run.

**Change.** Log one warning per constellation, once per run, that names the constellation
and the missing key. The decision "warn versus supply default priorities" is recorded in
the merge request; defaults are a possible follow-up task, not part of this one.

**Test.** A unit test that builds the model with `active_systems` containing a
constellation without priorities and asserts the warn-once behaviour (a counter or a
test-visible flag; the logger output itself is not parsed).

**Done when.** The warning appears in `logs\ppp_model.log` for such a config, appears
once, and results for correctly configured runs are unchanged.

## T4: Remove machine-specific paths from the sample configs

**Problem.** The sample configs pin absolute paths, for example
`"observation_path": "C:/GnssProcessor/data/2024-129/obs/gcgo00usa/..."` in
`ppp_internal.json` and `"input_path": "C:/GnssProcessor/sbf_capture.sbf"` in
`sbf_replay.json`. A clone in any other location fails.

**Change.** Use the auto-discovery that `PathResolver` already provides: an empty
`files.observation_path` plus `data_download.station`, `year` and `doy` as the date and
station hint (see "Path precedence" and "Date selection" in `CLAUDE.md`, section File
management). For `sbf_replay.json`, determine whether a relative `input_path` resolves
against the working directory, and document the answer in the config comment.

**Done when.** `ppp_internal.json`, `ppp_rtk_internal.json` and `sbf_replay.json` run
unchanged from a clone in a different directory, and produce the same `state_trace.csv`
as before (`scripts\compare_ppp_runs.py` shows a zero difference).

## T5: Decide the fate of ProductManager

**Problem.** `include\products\ProductManager.hpp` and its `.cpp` are compiled into
`gnss_core`, but nothing instantiates the class. Product loading is done by
`ProductParser` and `ProductCache`.

**Change.** Confirm the absence of callers with two independent searches (`git grep`
and the IDE's Find All References). Then either remove the two files and their
`CMakeLists.txt` entry, or, if a maintainer prefers to keep them, add a header banner
that states the class is unused and why.

**Done when.** The full build and the full unit suite pass, and the layout description
in `CLAUDE.md` and `docs\OVERVIEW.md` no longer lists a class that does not exist.

## T6: Make the output directory configurable

**Problem.** `Application::log_dir_` is a hard-coded `"logs"` relative to the working
directory (`include\app\Application.hpp`). Two runs cannot execute in parallel from the
same directory, and an A/B comparison requires renaming the folder between runs.

**Change.** A new config value, `logging.output_dir`, default `"logs"` (so the default
behaviour is unchanged). It follows the exact pattern of the worked example: member,
Doxygen getter, loader line, consumer. Both `Application::run()` and the real-time path
must honour it, and `LogManager` must write its `.log` files to the same place.

**Test.** A loader test for the default and a custom value.

**Done when.** Two runs with different `output_dir` values execute at the same time
without touching each other's files; `docs\CONFIG.md` and the code tour mention the key.

## T7: Absolute reference for compare_ppp_runs.py

**Problem.** Without `--ref`, `scripts\compare_ppp_runs.py` measures precision against
run A's own late median, not accuracy.

**Change.** A `--sinex <file> --station <code>` option that reads the station's `STAX`,
`STAY`, `STAZ` and `VELX`, `VELY`, `VELZ` from an IGS SINEX/SSC file (for example
`data\static\sinex\ITRF2020-IGS-TRF.SSC`), selects the solution whose validity window
contains the run epoch (or the last one, with a printed note), and propagates the
coordinate to the run epoch. `scripts\fetch_sinex_subset.py` shows how the blocks are
parsed.

**Test.** A `pytest` case with a three-line synthetic SINEX excerpt and a hand-computed
propagated coordinate.

**Done when.** `compare_ppp_runs.py A B --sinex ... --station GCGO` prints absolute
east, north and up errors, and the header states which solution and epoch were used.

## T8: End-of-run position error summary

**Problem.** A PPP run ends with a timing line only. `receiver.reference_position_ecef`
exists in the config but is used solely to seed the a-priori position. The quality of a
run is visible only after external analysis.

**Change.** When a reference position is configured, log a summary at the end of
`Application::run()`: mean and RMS of east, north and up over the final N hours, and
the convergence time to a configurable 3-D threshold. The arithmetic is the same as in
`compare_ppp_runs.py`. The feature is inert when no reference is configured, so
default behaviour is unchanged.

**Test.** A unit test of the summary computation on a synthetic position series with a
known offset and a known convergence epoch.

**Reference.** A short paragraph in `docs\report\product_evaluation.tex` that defines
the reported quantities.

**Done when.** The summary appears in `logs\main_log.log` for a config with a reference
position, and is absent otherwise.
