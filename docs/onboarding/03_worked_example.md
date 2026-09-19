# 3 · Worked example: a new stochastic model, start to finish

This page follows **one small, real change** through every stage: idea, code, test,
data run, LaTeX reference, commits, merge request. Re-creating it once by hand, on a
personal branch, exercises every part of the workflow.

The finished change is on the branch **`example/stochastic-model`**. The branch stays
unmerged on purpose and serves as the answer key:

```powershell
git fetch
git diff main...origin/example/stochastic-model --stat     # which files
git diff main...origin/example/stochastic-model            # every line
```

Each stage is attempted first and then compared with the branch.

| Stage | Task | Time |
| --- | --- | --- |
| [0](#stage-0-understand-the-change) | Understand the math and locate the code | 30 min |
| [1](#stage-1-branch) | Create a branch | 1 min |
| [2](#stage-2-add-the-enum-value) to [5](#stage-5-implement-the-model) | Write the code: enum, config, loader, model | 1 to 2 h |
| [6](#stage-6-write-the-unit-test) | Write the unit test | 1 h |
| [7](#stage-7-build-and-test) | Build and test | 15 min |
| [8](#stage-8-run-data) | Run data, before and after | 30 min |
| [9](#stage-9-update-the-latex-reference) | Document the math in LaTeX | 45 min |
| [10](#stage-10-commit-and-open-the-merge-request) | Commit, push, open the merge request | 15 min |

---

## Stage 0: Understand the change

**Background.** The Kalman filter weights each observation by the inverse of its
variance. The *stochastic model* is the function that assigns that variance. The
project default is the classic elevation law: low satellites are noisier and receive
less weight.

```
var = var_zen / sin^(2p)(el)            p = 1 by default
```

**The idea.** That law treats all noise as elevation-dependent. Receiver thermal noise
is not; it is the same for every satellite. The new model keeps a fraction `f` of the
zenith variance constant and scales only the remainder:

```
var = var_zen * ( f + (1 - f) / sin^(2p)(el) )          0 <= f <= 1
```

Three properties make this a suitable first change, and they become the tests:

- At zenith the bracket equals 1 for every `f`, so `var_zen` keeps its meaning.
- `f = 0` is the existing Elevation model; `f = 1` is the Constant model.
- The law is gentler at low elevation: at 10° the factor is 17.1 instead of 33.2.

**Locating the code.** Reading before writing:

| Source | Content |
| --- | --- |
| `docs/report/stochastic_qc.pdf`, section "The stochastic-model dispatch" | The math of the four existing models |
| `include/model/StochasticModel.hpp` | The class; one private `compute…` function per model |
| `src/core/model/StochasticModel.cpp`, `computeVariance` | The `switch` that selects the model |
| `tests/test_stochastic_model.cpp` | The shape of a test in this area |

A search for the config value shows how it travels from JSON to the model:

```powershell
git grep -n "stochastic_model" -- include src config/ppp_internal.json
```

The result is the path every setting takes, and therefore the list of files to edit:

```
config/*.json  ──►  GnssConfigLoad.cpp  ──►  GnssConfig (getter)  ──►  StochasticModelWeight
 "filter": {         parses the string        holds the value          uses it
   "stochastic_model"
```

## Stage 1: Branch

```powershell
git switch main
git pull
git switch -c feature/elevation-floor-model
```

## Stage 2: Add the enum value

**File:** `include/config/GnssConfigTypes.hpp`, `enum class StochasticModelType`.

```cpp
        SatelliteBlock, ///< Block/frequency-dependent zenith noise with 1/sin²(el) scaling.
        ElevationFloor  ///< Elevation weighting with an elevation-independent floor: var = var_zen * (f + (1-f)/sin^(2p)(el)).  See GnssConfig::getElevationFloorFraction().
```

New values are appended **at the end** of an enum. An insertion in the middle renumbers
the values after it.

## Stage 3: Add the config option

A config option always consists of the same three pieces.
**File:** `include/config/GnssConfig.hpp`.

**(a) The private member**, next to its relatives (search `code_elevation_power_;`):

```cpp
        double elevation_floor_fraction_; ///< Floor fraction f in [0,1] of the ElevationFloor model …
```

**(b) The public getter, with Doxygen**, next to `getPhaseElevationPower()`:

```cpp
        /**
         * @brief Floor fraction @c f of the @c ElevationFloor stochastic model.
         *
         *   var_actual = var_zen * ( f + (1 - f) / sin^(2p)(elev) )
         *
         * … what f = 0 and f = 1 mean, when the value is read, the JSON key …
         *
         * @return Floor fraction in [0, 1].  Default 0.5.
         */
        double getElevationFloorFraction() const { return elevation_floor_fraction_; }
```

The comment is written for a reader who finds the getter years later without the merge
request at hand. It states the formula, the units, the default and the JSON key.

**(c) The default value**, in the constructor's initializer list in
`src/core/config/GnssConfig.cpp`:

```cpp
        , code_elevation_power_{ 1.0 }
        , elevation_floor_fraction_{ 0.5 }
        , phase_elevation_power_{ 1.0 }
```

> **Trap:** the initializer list must follow the **same order** as the member
> declarations in the header. Otherwise the compiler warns, correctly.

## Stage 4: Read the option from JSON

**File:** `src/core/config/GnssConfigLoad.cpp`, near `"satellite_block"`. Two edits.

The loader learns the new model name:

```cpp
            else if (low == "satellite_block") stochastic_model_ = StochasticModelType::SatelliteBlock;
            else if (low == "elevation_floor") stochastic_model_ = StochasticModelType::ElevationFloor;
```

The new number is read a few lines below, after the elevation powers:

```cpp
        elevation_floor_fraction_ = jval<double>(json,
            "filter.elevation_floor_fraction", "elevation_floor_fraction", 0.5);
        if (elevation_floor_fraction_ < 0.0 || elevation_floor_fraction_ > 1.0) {
            const double clamped = std::min(1.0, std::max(0.0, elevation_floor_fraction_));
            log->warn("filter.elevation_floor_fraction = {} is outside [0, 1]; clamped to {}",
                elevation_floor_fraction_, clamped);
            elevation_floor_fraction_ = clamped;
        }
```

`jval<T>(json, "section.key", "key", default)` is the project's single way to read a
JSON value. **User input is always validated.** Here `f < 0` would produce a negative
variance at high elevation, so the loader rejects it with a warning instead of letting
the filter fail later without explanation.

The option is also made visible to users: a commented line is added to the sample
config `config/ppp_internal.json`, in the `filter` block.

```jsonc
    //"elevation_floor_fraction": 0.5,   // only read when stochastic_model = "elevation_floor"
    "stochastic_model": "elevation"
```

## Stage 5: Implement the model

**Header** `include/model/StochasticModel.hpp`: a private function is declared next to
`computeElevation` (with Doxygen), the member `elevation_floor_fraction_` is added, and
the model is added to the list in the file's top comment.

**Source** `src/core/model/StochasticModel.cpp`, four small edits:

```cpp
// 1. constructor: copy the value out of the config
          elevation_floor_fraction_(config.getElevationFloorFraction()),

// 2. constructor: the name printed in the log
        case StochasticModelType::ElevationFloor: name = "ElevationFloor"; break;

// 3. computeVariance(): route the new enum value
        case StochasticModelType::ElevationFloor:
            return computeElevationFloor(obs_type, default_var, elevation_deg);

// 4. the model itself
    double StochasticModelWeight::computeElevationFloor(const std::string& obs_type,
                                                         double default_var,
                                                         double elevation_deg) const {
        const double f = elevation_floor_fraction_;
        return f * computeConstant(default_var)
            + (1.0 - f) * computeElevation(obs_type, default_var, elevation_deg);
    }
```

Edit 4 does **not** re-implement `1/sin²`. It reuses `computeElevation`, so the horizon
guard and the code/phase power logic remain in one place, and `f = 0` gives
`0·v + 1·x = x`, *bit-identical* to the old model. Reuse of an existing kernel is
preferred over new math wherever it is possible.

> **Trap:** after a new enum value is added, every `switch` over that enum is checked
> (`git grep -n "StochasticModelType::"`). A missed `switch` silently falls through to
> a default.

A quick compile check builds only the library and takes about a minute:

```powershell
scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_core
```

## Stage 6: Write the unit test

**New file:** `tests/test_stochastic_model_elevation_floor.cpp`. One new file and one
new suite name (`StochasticModelElevationFloor`); the existing test file is not extended.

The test list is derived **from the math, before the code is consulted**:

| Test | Assertion | Reason |
| --- | --- | --- |
| `ClosedFormAtThirtyDegrees` | `f=0.5`, el=30° gives `2.5 × var_zen` | A value that can be checked by hand: sin 30° = ½ |
| `ZeroFractionIsBitIdenticalToElevation` | `f=0` equals the Elevation model **exactly** (`EXPECT_DOUBLE_EQ`) | Proves existing behaviour is undisturbed |
| `UnitFractionIsConstant` | `f=1` gives `var_zen` | The other limit |
| `ZenithNormalisedAndMonotonic` | el=90° gives `var_zen`; variance grows toward the horizon; smaller than pure Elevation at 10° | The physical shape |
| `PhaseUsesPhaseElevationPower` | Phase rows use `p_phase`, code rows `p_code` | Confirms the reuse in Stage 5 |
| `…Config.DefaultsAreOptIn` | Default model is still `Elevation`; default `f = 0.5` | **Rule 1: new behaviour is opt-in** |
| `…Config.ModelNameParsesCaseInsensitively` | `"Elevation_Floor"` selects the model | Loader contract |
| `…Config.OutOfRangeFractionIsClamped` | `-0.2 → 0`, `1.7 → 1` | The validation from Stage 4 |

The tests write a small JSON config to a temp file, load it with the real loader,
construct the real `StochasticModelWeight`, and call the **public**
`computeVariance()`. This is the same path a user's run takes.

```cpp
TEST(StochasticModelElevationFloor, ClosedFormAtThirtyDegrees) {
    // el = 30 deg -> sin = 0.5 -> 1/sin^2 = 4.  f = 0.5: 0.5 + 0.5*4 = 2.5.
    const double v = varianceUnder("closed",
        "    \"stochastic_model\": \"elevation_floor\",\n"
        "    \"elevation_floor_fraction\": 0.5\n",
        "C1W", kZenVar, 30.0);
    EXPECT_NEAR(v, 2.5 * kZenVar, 1e-12);
}
```

The file is registered by hand, because sources are listed explicitly.
**File:** `CMakeLists.txt`, below `tests/test_stochastic_model.cpp`:

```cmake
    tests/test_stochastic_model_elevation_floor.cpp
```

## Stage 7: Build and test

```powershell
scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_tests
build\x64-Debug\gnss_tests.exe --gtest_filter=StochasticModel*
```

Expected:

```
[==========] Running 12 tests from 3 test suites.
[----------] 4 tests from StochasticModelTest
...
[----------] 5 tests from StochasticModelElevationFloor
[ RUN      ] StochasticModelElevationFloor.ClosedFormAtThirtyDegrees
[       OK ] StochasticModelElevationFloor.ClosedFormAtThirtyDegrees (17 ms)
[ RUN      ] StochasticModelElevationFloor.ZeroFractionIsBitIdenticalToElevation
[       OK ] StochasticModelElevationFloor.ZeroFractionIsBitIdenticalToElevation (133 ms)
...
[----------] 3 tests from StochasticModelElevationFloorConfig
...
[==========] 12 tests from 3 test suites ran. (279 ms total)
[  PASSED  ] 12 tests.
```

Then the whole suite, since the change must not break anything else:

```powershell
build\x64-Debug\gnss_tests.exe
```

```
[==========] 2273 tests from 300 test suites ran. (210289 ms total)
[  PASSED  ] 2256 tests.
[  SKIPPED ] 17 tests.
```

The run prints thousands of log lines; only the last three matter. Any `[  FAILED  ]`
line is resolved before the next stage. `SKIPPED` tests need files from `data\`; with
the bootstrap data in place only one or two remain.

**Observe one failure.** Change `2.5` to `2.6` in the test, rebuild, run, read the
failure message, and change it back. A passing test is only trusted once its failure
has been seen.

## Stage 8: Run data

Unit tests prove that the function computes the formula. They do not show whether the
formula improves positioning. That requires a data run, with the **Release build only**.

```powershell
scripts\vs2026_build.ps1 -Config x64-Release -Target gnss_processor
```

Two copies of the sample config are made so that **only one setting differs**:

```powershell
copy config\ppp_internal.json config\_my_elev.json
copy config\ppp_internal.json config\_my_floor.json
```

In `_my_floor.json`, in the `filter` block:

```jsonc
    "elevation_floor_fraction": 0.5,
    "stochastic_model": "elevation_floor"
```

Both configs are run. Each run overwrites `logs\`, so the folder is renamed after each
run. A 24-hour, four-constellation PPP run takes about 3 minutes.

```powershell
build\x64-Release\gnss_processor.exe config\_my_elev.json
move logs logs_elev

build\x64-Release\gnss_processor.exe config\_my_floor.json
move logs logs_floor
```

**The first check is that the option was used.** A typo in a JSON key is silently
ignored, and the comparison would then be a run against itself.

```powershell
findstr /c:"Elevation floor" logs_floor\gnss_config.log
```

```
[gnss_config] [info] Elevation floor fraction: 0.500
```

The comparison:

```powershell
python scripts\compare_ppp_runs.py logs_elev logs_floor --label-a elevation --label-b elevation_floor
```

```
reference : median of run A's last 6 h (precision / convergence only, NOT absolute accuracy)
RMS/sigma : epochs after the first 2 h

run            epochs   conv<10cm [h]   E [cm]   N [cm]   U [cm]  3D [cm]  sigma3D [cm]
---------------------------------------------------------------------------------------
elevation        2866           17.43     2.10     1.72     5.11     5.79          1.05
elevation_floor  2866           17.45     1.98     1.69     5.17     5.79          0.89

B - A, 2626 common epochs after 2 h:  mean E/N/U = +0.26 / -0.02 / -0.32 cm,  max |3D| = 6.50 cm
```

**Interpretation.** For this station and day the new model changes almost nothing: the
3-D RMS is identical (5.79 cm) and the solutions differ by millimetres on average. The
one value that moved is the filter's *formal* sigma (1.05 to 0.89 cm). The filter has
become more confident without the position becoming better. That is a warning sign, not
an improvement: low-elevation observations now carry more weight than their real errors
justify.

The conclusion for the merge request is therefore: *"implemented and verified; no
measurable benefit on GCGO 2024-129 with f = 0.5; formal sigma becomes more optimistic;
stays default OFF."* A reported "no difference" is a valid result. One station and one
day never justify a change of default.

Files in `config/` that start with `_my_` are scratch files and are git-ignored.

## Stage 9: Update the LaTeX reference

**Rule 3: the math changed, so the reference changes in the same merge request.**

**File:** `docs/report/stochastic_qc.tex`, subsection *The stochastic-model dispatch*.
The existing models are paragraphs (a) to (d). Paragraph **(e)** is added after (d), in
the same shape: one sentence of motivation, the boxed equation with a `\label`, the
origin of each symbol (config key, default), the limiting cases, and the implementation.

```latex
\paragraph{(e) ElevationFloor.}  The pure elevation law attributes \emph{all}
of the zenith variance to effects that grow toward the horizon. …
\begin{empheq}[box=\fbox]{equation}
\sigma^2_{\rx,\sat,\sigl}
  = \zenvar \,\Bigl[\, f + (1-f)\,\bigl(\sinel^{\sat}_{\rx}\bigr)^{-2p_{\mathrm{type}}} \Bigr],
\qquad 0 \le f \le 1 ,
\label{eq:qc-elevation-floor}
\end{empheq}
```

Conventions of the reference:

- **Existing macros are used** (`\zenvar`, `\sinel`, `\rx`, `\sat`, `\sigl`). They are
  defined at the top of the file or in `gnssreport.sty`. No new notation is introduced
  for a quantity that already has a symbol.
- **Code is named** in `\texttt{}` (function, config key), so that a reader can `grep`
  from the PDF to the source.
- **The surrounding text is corrected.** This section said "the four-way … set" and
  "All four strategies"; both are now five. A search for `four` finds them.

The edited component is built first, then the full volume:

```powershell
cd docs\report
python ..\..\scripts\gen_report_tables.py   # once per clone: writes the git-ignored generated\ tables
latexmk stochastic_qc.tex                   # ~1 min; the new paragraph is then proof-read in stochastic_qc.pdf
latexmk main.tex                            # the combined reference, a few minutes
cd ..\..
```

> **Trap:** on a fresh clone `latexmk main.tex` stops with
> ``File `generated/processing_modes.tex' not found`` until `gen_report_tables.py` has
> run once. If a failed build leaves `main.pdf` deleted, it is restored with
> `git restore docs/report/main.pdf` and the build is repeated.

**Check:** no `undefined` references. `findstr /c:"undefined" docs\report\stochastic_qc.log`
prints nothing (`Font shape … undefined` warnings are harmless).

The `.pdf` files are tracked in git, so that readers without LaTeX can open them. The
`.aux`, `.log` and similar intermediates are ignored. The `.tex` and the `.pdf` are
committed.

## Stage 10: Commit and open the merge request

The change is split into three commits, each of which builds and passes on its own
([rationale](04_git_workflow.md#commits-for-one-change)):

```powershell
git status                          # only intended files should appear

git add include/config/GnssConfigTypes.hpp include/config/GnssConfig.hpp `
        src/core/config/GnssConfig.cpp src/core/config/GnssConfigLoad.cpp `
        include/model/StochasticModel.hpp src/core/model/StochasticModel.cpp `
        config/ppp_internal.json
git commit      # model: add ElevationFloor stochastic model (default OFF)

git add tests/test_stochastic_model_elevation_floor.cpp CMakeLists.txt
git commit      # tests: ElevationFloor closed form, limits and loader contract

git add docs/report/stochastic_qc.tex docs/report/stochastic_qc.pdf docs/report/main.pdf
git commit      # docs: ElevationFloor in the stochastic-model reference

git push -u origin feature/elevation-floor-model
```

`git push` prints a link that opens the merge request. The description contains the
[checklist](../../CONTRIBUTING.md#the-checklist), the test summary line, and the
before/after numbers from Stage 8. The merge request is assigned to a maintainer.

---

## Patterns introduced

| Pattern | Where it appeared | Reused for |
| --- | --- | --- |
| JSON → loader → `GnssConfig` getter → consumer | Stages 3 to 5 | Every new option |
| New behaviour behind a config value, default unchanged | Stages 2 to 4, test `DefaultsAreOptIn` | Every new feature |
| Reuse of an existing kernel instead of a re-derivation | Stage 5 | Every new model |
| Tests derived from the math: closed form, limits, contract | Stage 6 | Every test |
| A bit-identity test against the old behaviour | `ZeroFractionIsBitIdenticalToElevation` | Every change to a shared path |
| Explicit `CMakeLists.txt` entry | Stage 6 | Every new `.cpp` |
| One-variable A/B on data, Release build | Stage 8 | Every claim that a change improves results |
| Reference paragraph: motivation, boxed equation, symbols, limits, code names | Stage 9 | Every change to the math |

A good first task has the same size as this example: one model, one option, one test
file, one paragraph. It is agreed with a maintainer.
