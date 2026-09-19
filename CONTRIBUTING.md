# Contributing to GnssProcessor

New here? Do the [onboarding path](docs/onboarding/README.md) first — it takes you from
an empty machine to a merged change. This page is the one-page reference you come back
to afterwards.

## The five rules

1. **New behaviour is opt-in.** A new model, option or correction is selected by a
   config value and the default stays what it was. Someone who does not ask for your
   change must get bit-identical results. This is what lets us trust old results.
2. **Every change comes with a unit test** that would fail without it.
3. **If you change the math, you change the LaTeX reference** (`docs/report/`) in the
   same merge request. Code and reference never disagree.
4. **Public functions get Doxygen comments** (`@brief`, `@param`, `@return`). Match the
   style of the header you are editing.
5. **Data runs use the Release build. Tests use the Debug build.** Never the other way
   round.

## The checklist

Copy this into your merge request and tick it off.

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

| You are adding… | Code | Test | Reference |
| --- | --- | --- | --- |
| an observation weighting model | `include/model/StochasticModel.hpp`, `src/core/model/` | `tests/test_stochastic_model*.cpp` | `docs/report/stochastic_qc.tex` |
| a troposphere / ionosphere model | `include/atmosphere/`, `src/core/atmosphere/` | `tests/test_atmosphere_models.cpp` | `atmosphere.tex` |
| a site-displacement or antenna correction | `include/corrections/`, `include/model/Antenna.hpp` | `tests/test_site_displacement.cpp`, `tests/test_antenna_model.cpp` | `site_displacement.tex`, `observation_physics.tex` |
| a cycle-slip detector | `include/slip/`, `src/core/slip/` | `tests/test_turbo_edit_resolver.cpp`, `tests/test_if_phase_slip_detector.cpp` | `slip_detection.tex` |
| a Kalman filter / state change | `include/filter/` — state layout belongs to `StateManager` only | `tests/test_filter_core.cpp`, `tests/test_state_key_schema.cpp` | `kalman_smoother.tex`, `state_vector.tex` |
| a config option | `include/config/GnssConfig.hpp`, `src/core/config/GnssConfigLoad.cpp` | a loader test, e.g. `tests/test_config_extends.cpp` | `docs/CONFIG.md` |
| a real-time step | the matching `src/core/realtime/RealTimeEngine*.cpp` seam | `tests/test_realtime_seams.cpp`, `tests/test_rtcm.cpp`, `tests/test_sbf.cpp` | `realtime_engine.tex` |
| an input file type | `include/products/PathResolver.hpp` first | `tests/test_rinex_nav_parser.cpp` (parser pattern) | `product_evaluation.tex` |

The complete, step-by-step example of the first row is
[docs/onboarding/03_worked_example.md](docs/onboarding/03_worked_example.md).

## Code conventions that will bite you

- **Includes carry the sub-directory:** `#include "filter/KalmanFilter.hpp"`, never
  `"KalmanFilter.hpp"`.
- **No source globbing.** A new `.cpp` that is not in `CMakeLists.txt` produces
  "unresolved external symbol" at link time.
- **State-vector keys** are built and parsed only through
  `include/filter/StateKeySchema.hpp`. Do not format key strings by hand.
- **Post-processing-only features** (smoother, `gnss_rate_s`) must not run in real-time
  mode — guard on `AppMode::RealTime`.
- **One test suite name per file.** Prefer a new test file over growing an old one.

## What you can ignore for now

The repository also holds a multi-station **network producer** research program. Unless
you are asked to work on it, you can skip: `docs/refound/`, `docs/cards/`,
`docs/archive/`, `config/archive/`, `config/golden/`, `config/test_suite/`, every
`Network*` file under `include/filter/` and `src/core/filter/`, and most of `scripts/`.
`CLAUDE.md` is the operating manual for that program (and for AI coding assistants) —
useful later, not on day one.

## Getting help

Ask early. A question after 30 minutes of being stuck is cheaper than a day lost.
When you ask, include: the command you ran, the last ~20 lines of output, and your
branch name.
