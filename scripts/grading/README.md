# Grading scripts of card NRF

The bars that card NRF's decisions rest on, one owner per bar, moved here from the grading folder
`D:\GnssProcessorSupport\runs_nom\_irdu_grades\scripts_s20` under card GRA
([docs/cards/tooling/GRA_grader_audit_2026-11-18.md](../../docs/cards/tooling/GRA_grader_audit_2026-11-18.md),
section 5). The older scripts stay where they are; a bar listed here has a new owner that reproduces the recorded
outputs.

**Runs.** A run is a bare tag (`<RAPID_RUNS_ROOT>/_rap_<tag>`, the root defaulting to
`D:\GnssProcessorSupport\runs_nom`), a `_rap_` folder name, or a path to the folder. Epoch times are matched on whole
seconds (`runs.tow_key`), since the stream's times are not whole seconds under `processing.exact_epoch_time`.

**Views.** Where a script replaces older ones it has two views. `legacy` prints byte for byte what the replaced script
printed on the same run, quirks included. The corrected view (`exact`, or the name the script gives) applies the
corrections card GRA names, and is printed beside the legacy one, never in its place.

**Self-tests.** Each script runs `py -3.14 scripts/grading/<script>.py --selftest`, the repository's convention (a
`selftest()` that prints `selftest: PASS` or `FAIL` and sets the exit code, as in `scripts/nrf_two_run_audit.py`). It
builds a synthetic run folder from `fixtures/<step>.json` in a temporary folder (JSON, because the repository ignores
every `logs/` folder and `*.log` file), and, when the frozen real-run fixtures are present
(`D:\GnssProcessorSupport\gates\grader_fixtures_2026-11-18`, or `GRADING_FROZEN_ROOT`), reruns the commands of
`frozen.json` and compares the sha256 of their output.

| bar | script | run | views |
| --- | --- | --- | --- |
| shared readers of network and user runs | `runs.py` | imported by the others | none |
| synthetic and frozen fixtures | `fixture.py` | imported by the self-tests | none |
| held share by hour, family and receiver mode; GPS held share; RAIM rows; epochs; lost satellite-samples (B1, B2, B3, B8) | `network_hourly.py` | `table <run> --xfile <list>`; `compare <arm> <control> --xfile <list>`; `families <run> ...` | legacy: `single_hourly.py`, `c4_compare.py` as of section 118.1, `held_by_family.py`, the held, RAIM, epoch and withheld rows of `d24_hourly.py` (`--view d24`); exact: unrounded shares (the worst hour and the mean of a comparison taken from them), a row per family, RAIM rows by constellation, lost samples by both rules (band 1 flagged; both signals flagged, section 96.1) |
| links held through a flagged slip, bar 7 (B7); through an injected slip, F3 and C3 (B7b); the injected slips' truth | `slip_hold.py` | `flagged <reference run> <tested run> [--out f.jsonl]`; `injected <truth.jsonl> <run> ...`; `truth <arm> <base> <out.jsonl>` | `flagged` legacy: `slip_hold_check.py` (printout and `--out` file); `flagged --view lli`: the `lli_slip` refloat events counted as flags of the link and reported apart, windows from the run's epoch interval; `injected` = `grade_slip_bar.py`; `truth` = `injected_slips.py` |
