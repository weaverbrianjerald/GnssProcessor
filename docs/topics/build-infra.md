# `build-infra` — Build, test harness, configuration and ingest

The toolchain and everything around the estimator rather than inside it: the
configuration reference, the real-data truth-graded harness, ingest guards, data layout,
the aiding-source abstraction, the datum-transformation reference and the docs tooling.

**Conventions and invariants**

- VS 2026 + the `scripts/vs2026_build.ps1` wrapper; VS 2022 is retired. Field runs are
  Release-only.
- Config inheritance: arrays REPLACE, they do not merge; and never grep knob counts to
  compare two configs — flatten them.
- `phase_elevation_power` changed 0.5 → 1.0 on 2026-07-31; every `test_suite` bench before
  that date is confounded.
- Input-file location goes through `PathResolver` and one field, `data_source`; new file
  types plug in there, not into the call sites.
- Concurrent runs share `./logs/` — there is no config knob for it; use a renamed exe and
  a distinct run directory.

**Current posture (2026-09-14)** — unchanged by [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md), which is a
producer program; what it needs from here is a clean Release build and the goldens.
The docs tooling (`scripts/docs_split.py`, `scripts/docs_index_check.py`) is new and
lives here.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

_No split cards in this topic._

**Standing documents** (still at their original paths, indexed not moved):

- [CONFIG.md](../CONFIG.md)
- [README.md](../README.md)
- [SPEC_latex_doc_refresh_2026-07-17.md](../SPEC_latex_doc_refresh_2026-07-17.md)
- [SPEC_rinex_zero_sentinel.md](../SPEC_rinex_zero_sentinel.md)
- [aiding_source_abstraction_design.md](../aiding_source_abstraction_design.md)
- [datum_transformation_reference_2026-08-07.md](../datum_transformation_reference_2026-08-07.md)
- [test_suite.md](../test_suite.md)

---

Back to [INDEX.md](../INDEX.md).
