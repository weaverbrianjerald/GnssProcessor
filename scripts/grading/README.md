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
