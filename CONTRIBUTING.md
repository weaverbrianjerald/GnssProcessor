# Contributing to GnssProcessor

New contributors complete the [onboarding path](docs/onboarding/README.md) first. It
leads from an empty machine to a merged change. This page is the one-page reference
used afterwards.

## The five rules

1. **New behaviour is opt-in.** A new model, option or correction is selected by a
   config value, and the default stays as it was. A run that does not select the new
   behaviour must produce bit-identical results. This keeps earlier results valid.
2. **Every change includes a unit test** that fails without the change.
3. **A change to the math includes a change to the LaTeX reference** (`docs/report/`)
   in the same merge request. Code and reference must not disagree.
4. **Public functions carry Doxygen comments** (`@brief`, `@param`, `@return`), in the
   style of the header being edited.
5. **Data runs use the Release build. Tests use the Debug build.** Never the reverse.

## The checklist

Paste this list into the merge request and tick each item.

```
[ ] Branch is off current main; one change only
[ ] Builds clean:            scripts\vs2026_build.ps1 -Config x64-Debug
[ ] New test passes:         build\x64-Debug\gnss_tests.exe --gtest_filter=<MySuite>*
[ ] Full suite passes:       build\x64-Debug\gnss_tests.exe
[ ] Default behaviour unchanged (new option is OFF unless selected)
[ ] New .cpp files added to CMakeLists.txt (sources are listed explicitly)
[ ] Doxygen on every new public function / config getter
[ ] Config option documented where users look (sample config comment, docs/CONFIG.md)
[ ] LaTeX reference updated and PDF rebuilt, if the math changed
[ ] If results could change: one Release run before/after, numbers in the MR
[ ] Commits are small, named "area: what changed", no generated files staged
```

## Where a change goes

| Change | Code | Test | Reference |
| --- | --- | --- | --- |
| Observation weighting model | `include/model/StochasticModel.hpp`, `src/core/model/` | `tests/test_stochastic_model*.cpp` | `docs/report/stochastic_qc.tex` |
| Troposphere / ionosphere model | `include/atmosphere/`, `src/core/atmosphere/` | `tests/test_atmosphere_models.cpp` | `atmosphere.tex` |
| Site-displacement or antenna correction | `include/corrections/`, `include/model/Antenna.hpp` | `tests/test_site_displacement.cpp`, `tests/test_antenna_model.cpp` | `site_displacement.tex`, `observation_physics.tex` |
| Cycle-slip detector | `include/slip/`, `src/core/slip/` | `tests/test_turbo_edit_resolver.cpp`, `tests/test_if_phase_slip_detector.cpp` | `slip_detection.tex` |
| Kalman filter or state change | `include/filter/` (state layout belongs to `StateManager` only) | `tests/test_filter_core.cpp`, `tests/test_state_key_schema.cpp` | `kalman_smoother.tex`, `state_vector.tex` |
| Config option | `include/config/GnssConfig.hpp`, `src/core/config/GnssConfigLoad.cpp` | a loader test, e.g. `tests/test_config_extends.cpp` | `docs/CONFIG.md` |
| Real-time step | the matching `src/core/realtime/RealTimeEngine*.cpp` seam | `tests/test_realtime_seams.cpp`, `tests/test_rtcm.cpp`, `tests/test_sbf.cpp` | `realtime_engine.tex` |
| Input file type | `include/products/PathResolver.hpp` first | `tests/test_rinex_nav_parser.cpp` (parser pattern) | `product_evaluation.tex` |

The first row is carried out step by step in
[docs/onboarding/03_worked_example.md](docs/onboarding/03_worked_example.md).

## Code conventions

- **Includes carry the sub-directory:** `#include "filter/KalmanFilter.hpp"`, never
  `"KalmanFilter.hpp"`.
- **No source globbing.** A new `.cpp` that is missing from `CMakeLists.txt` produces
  "unresolved external symbol" at link time.
- **State-vector keys** are built and parsed only through
  `include/filter/StateKeySchema.hpp`. Key strings are never formatted by hand.
- **Post-processing-only features** (smoother, `gnss_rate_s`) must not run in real-time
  mode. They are guarded on `AppMode::RealTime`.
- **One test suite name per file.** A new test file is preferred over extending an
  existing one.

## Documentation style

Markdown documents (README, onboarding pages, new cards) are written in an impersonal,
concise, technical register:

- No second-person address. "Changes to weighting are made in `model/`", not "you change
  the weighting in `model/`". Imperatives are used only in numbered procedures.
- No em dashes. A period, colon, comma or parentheses is used instead.
- No personal names as points of contact. "A maintainer" is used.
- Statements are checked against the code before they are written down: commands are
  run, file names are confirmed to exist, numbers come from an actual run.

Existing research records under `docs/cards/`, `docs/archive/` and `docs/refound/` are
never reworded to fit this style; they are the scientific record.

## AI coding assistants

AI coding assistants may be used. The output is held to the same standard as
hand-written work:

- The contributor understands and can explain every line that is submitted, and is
  responsible for it. "The assistant wrote it" is not an answer in review.
- All five rules and the checklist apply unchanged: opt-in behaviour, a unit test that
  fails without the change, the LaTeX reference, Doxygen, Release for data.
- Generated code matches the surrounding code: naming, comment density, Doxygen style,
  the include convention, explicit `CMakeLists.txt` entries. Generated prose follows the
  documentation style above.
- Numbers, test results and run outputs quoted in a merge request come from an actual
  run by the contributor, never from the assistant's description of what would happen.
- Commit messages carry no AI attribution lines. The `commit-msg` hook strips them.
- `CLAUDE.md` is the instruction file that assistants read. A change to project
  conventions is made there as well as here.
- Credentials (`_netrc`, NTRIP passwords) and unpublished data are never pasted into an
  external service.

## Out of scope for new contributors

The repository also holds a multi-station network producer research program. Unless a
task concerns it, the following can be skipped: `docs/refound/`, `docs/cards/`,
`docs/archive/`, `config/archive/`, `config/golden/`, `config/test_suite/`, every
`Network*` file under `include/filter/` and `src/core/filter/`, and most of `scripts/`.
`CLAUDE.md` is the operating manual for that program and for AI coding assistants. It
becomes useful later.

## Getting help

Questions are best asked early: 30 minutes without progress is the signal. A useful
question includes the command that was run, the last 20 lines of output, and the
branch name.
