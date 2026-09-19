# 3 · Worked example — adding a stochastic model, start to finish

This page follows **one small, real change** through every stage: idea → code → test →
data run → LaTeX reference → commits → merge request. Do it once by hand, on your own
branch, and you will have touched every part of the workflow.

The finished change lives on the branch **`example/stochastic-model`**. It is kept
unmerged on purpose, as the answer key:

```powershell
git fetch
git diff main...origin/example/stochastic-model --stat     # which files
git diff main...origin/example/stochastic-model            # every line
```

Try each step yourself first; look at the branch when you are stuck.

| Stage | You will | Time |
| --- | --- | --- |
| [0](#stage-0--understand-the-change) | understand the math and find the code | 30 min |
| [1](#stage-1--branch) | make a branch | 1 min |
| [2](#stage-2--add-the-enum-value) – [5](#stage-5--implement-the-model) | write the code: enum → config → loader → model | 1–2 h |
| [6](#stage-6--write-the-unit-test) | write the unit test | 1 h |
| [7](#stage-7--build-and-test) | build, test | 15 min |
| [8](#stage-8--try-it-on-data) | run real data, before and after | 30 min |
| [9](#stage-9--update-the-latex-reference) | document the math in LaTeX | 45 min |
| [10](#stage-10--commit-and-open-the-merge-request) | commit, push, open the merge request | 15 min |

---

## Stage 0 — Understand the change

**What a stochastic model is.** The Kalman filter weights each observation by the
inverse of its variance. The *stochastic model* is the function that assigns that
variance. The project default is the classic elevation law — low satellites are
noisier, so they get less weight:

```
var = var_zen / sin^(2p)(el)            p = 1 by default
```

**The idea.** That law treats *all* the noise as elevation-dependent. Receiver thermal
noise is not — it is the same for every satellite. So keep a fraction `f` of the
zenith variance constant and scale only the rest:

```
var = var_zen * ( f + (1 - f) / sin^(2p)(el) )          0 <= f <= 1
```

Three properties make this a good first change, and they become our tests:

- at zenith the bracket is 1 for every `f` → `var_zen` keeps its meaning;
- `f = 0` **is** the existing Elevation model; `f = 1` **is** the Constant model;
- it is gentler at low elevation — at 10° the factor is 17.1 instead of 33.2.

**Find the code.** Read these before writing anything:

| Read | To learn |
| --- | --- |
| `docs/report/stochastic_qc.pdf`, §"The stochastic-model dispatch" | the math of the four existing models |
| `include/model/StochasticModel.hpp` | the class; one private `compute…` function per model |
| `src/core/model/StochasticModel.cpp`, `computeVariance` | the `switch` that picks the model |
| `tests/test_stochastic_model.cpp` | what a test here looks like |

Search for the config value to see how it arrives from JSON:

```powershell
git grep -n "stochastic_model" -- include src config/ppp_internal.json
```

That search shows you the full path a setting takes, and therefore the files to edit:

```
config/*.json  ──►  GnssConfigLoad.cpp  ──►  GnssConfig (getter)  ──►  StochasticModelWeight
 "filter": {         parses the string        holds the value          uses it
   "stochastic_model"
```

## Stage 1 — Branch

```powershell
git switch main
git pull
git switch -c feature/elevation-floor-model
```

## Stage 2 — Add the enum value

**File:** `include/config/GnssConfigTypes.hpp` — find `enum class StochasticModelType`.

```cpp
        SatelliteBlock, ///< Block/frequency-dependent zenith noise with 1/sin²(el) scaling.
        ElevationFloor  ///< Elevation weighting with an elevation-independent floor: var = var_zen * (f + (1-f)/sin^(2p)(el)).  See GnssConfig::getElevationFloorFraction().
```

Add new values **at the end** of an enum. Inserting in the middle renumbers the others.

## Stage 3 — Add the config option

A config option is always the same three pieces. **File:** `include/config/GnssConfig.hpp`.

**(a) the private member**, next to its relatives (search `code_elevation_power_;`):

```cpp
        double elevation_floor_fraction_; ///< Floor fraction f in [0,1] of the ElevationFloor model …
```

**(b) the public getter, with Doxygen**, next to `getPhaseElevationPower()`:

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

Write the comment for the person who finds this getter in two years and has never seen
your merge request: the formula, the units, the default, the JSON key.

**(c) the default value**, in the constructor's initializer list in
`src/core/config/GnssConfig.cpp`:

```cpp
        , code_elevation_power_{ 1.0 }
        , elevation_floor_fraction_{ 0.5 }
        , phase_elevation_power_{ 1.0 }
```

> **Trap:** the initializer list must be in the **same order** as the member
> declarations in the header, or the compiler warns (and the warning is right).

## Stage 4 — Read it from JSON

**File:** `src/core/config/GnssConfigLoad.cpp`. Search for `"satellite_block"`. Two edits.

Teach the loader the new model name:

```cpp
            else if (low == "satellite_block") stochastic_model_ = StochasticModelType::SatelliteBlock;
            else if (low == "elevation_floor") stochastic_model_ = StochasticModelType::ElevationFloor;
```

Read the new number, a few lines below, after the elevation powers:

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

`jval<T>(json, "section.key", "key", default)` is the project's one way to read a JSON
value. **Always validate what a user can type.** Here `f < 0` would produce a negative
variance at high elevation, so the loader refuses it loudly instead of letting the
filter fail mysteriously later.

Finally, show users the option exists — add a commented line to the sample config
`config/ppp_internal.json`, in the `filter` block:

```jsonc
    //"elevation_floor_fraction": 0.5,   // only read when stochastic_model = "elevation_floor"
    "stochastic_model": "elevation"
```

## Stage 5 — Implement the model

**Header** `include/model/StochasticModel.hpp`: declare a private function next to
`computeElevation` (with Doxygen), add the member `elevation_floor_fraction_`, and add the
model to the list in the file's top comment.

**Source** `src/core/model/StochasticModel.cpp` — four small edits:

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

Notice what step 4 does **not** do: it does not re-implement `1/sin²`. It reuses
`computeElevation`, so the horizon guard and the code/phase power logic exist in one
place only — and `f = 0` gives `0·v + 1·x = x`, *bit-identical* to the old model. Look
for this kind of reuse before writing new math.

> **Trap:** after adding an enum value, search for every `switch` over it
> (`git grep -n "StochasticModelType::"`). A `switch` you miss silently falls through
> to a default.

Quick compile check — builds only the library, about a minute:

```powershell
scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_core
```

## Stage 6 — Write the unit test

**New file:** `tests/test_stochastic_model_elevation_floor.cpp`. One new file, one new
suite name (`StochasticModelElevationFloor`) — do not grow the existing test file.

Decide what to test **from the math, before looking at your code**:

| Test | Asserts | Why this one |
| --- | --- | --- |
| `ClosedFormAtThirtyDegrees` | `f=0.5`, el=30° → `2.5 × var_zen` | a number you can check by hand: sin 30° = ½ |
| `ZeroFractionIsBitIdenticalToElevation` | `f=0` equals the Elevation model **exactly** (`EXPECT_DOUBLE_EQ`) | proves we did not disturb existing behaviour |
| `UnitFractionIsConstant` | `f=1` → `var_zen` | the other limit |
| `ZenithNormalisedAndMonotonic` | el=90° → `var_zen`; variance grows toward the horizon; smaller than pure Elevation at 10° | the physical shape |
| `PhaseUsesPhaseElevationPower` | phase rows use `p_phase`, code rows `p_code` | the reuse in Stage 5 really works |
| `…Config.DefaultsAreOptIn` | default model is still `Elevation`; default `f = 0.5` | **rule 1: new behaviour is opt-in** |
| `…Config.ModelNameParsesCaseInsensitively` | `"Elevation_Floor"` selects the model | loader contract |
| `…Config.OutOfRangeFractionIsClamped` | `-0.2 → 0`, `1.7 → 1` | the validation from Stage 4 |

The tests write a tiny JSON config to a temp file, load it with the real loader, build
the real `StochasticModelWeight`, and call the **public** `computeVariance()` — the same
path a user's run takes:

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

Register the file — sources are listed by hand. **File:** `CMakeLists.txt`, search
`tests/test_stochastic_model.cpp`, add below it:

```cmake
    tests/test_stochastic_model_elevation_floor.cpp
```

## Stage 7 — Build and test

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

Then the whole suite — your change must not break anything else:

```powershell
build\x64-Debug\gnss_tests.exe
```

```
[==========] 2273 tests from 300 test suites ran. (210289 ms total)
[  PASSED  ] 2256 tests.
[  SKIPPED ] 17 tests.
```

The run prints thousands of log lines; only the last three matter. Any `[  FAILED  ]`
line is yours to fix before going on. `SKIPPED` tests are ones that need files from
`data\` — with the bootstrap data in place you will see only one or two.

**See a test fail once.** Change `2.5` to `2.6` in your test, rebuild, run, read the
failure message, change it back. You should know what red looks like before you trust
green.

## Stage 8 — Try it on data

Unit tests prove the function computes the formula. They do not tell you whether the
formula helps positioning. For that, run data — **Release build only**.

```powershell
scripts\vs2026_build.ps1 -Config x64-Release -Target gnss_processor
```

Make two copies of the sample config so that **only one thing differs**:

```powershell
copy config\ppp_internal.json config\_my_elev.json
copy config\ppp_internal.json config\_my_floor.json
```

In `_my_floor.json` set, in the `filter` block:

```jsonc
    "elevation_floor_fraction": 0.5,
    "stochastic_model": "elevation_floor"
```

Run both. Each run overwrites `logs\`, so rename the folder after each (a 24-hour,
four-constellation PPP run takes about 3 minutes):

```powershell
build\x64-Release\gnss_processor.exe config\_my_elev.json
move logs logs_elev

build\x64-Release\gnss_processor.exe config\_my_floor.json
move logs logs_floor
```

**First check that your option was really used** — a typo in a JSON key is silently
ignored, and then you compare a run with itself:

```powershell
findstr /c:"Elevation floor" logs_floor\gnss_config.log
```

```
[gnss_config] [info] Elevation floor fraction: 0.500
```

Then compare:

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

**Read the result honestly.** For this station and day the new model changes almost
nothing: the 3-D RMS is identical (5.79 cm) and the solutions differ by millimetres on
average. The one number that moved is the filter's *formal* sigma (1.05 → 0.89 cm) —
the filter has become more confident, without the position becoming better. That is a
warning sign, not a win: low-elevation observations now carry more weight than their
real errors justify.

So the conclusion for the merge request is: *"implemented and verified; no measurable
benefit on GCGO 2024-129 with f = 0.5; formal sigma becomes more optimistic; stays
default OFF."* That is a perfectly good outcome. An honest "no difference" is a result.
One station and one day never justify changing a default.

Files starting with `_` in `config/` are your scratch files — do not commit them.

## Stage 9 — Update the LaTeX reference

**Rule 3: the math changed, so the reference changes in the same merge request.**

**File:** `docs/report/stochastic_qc.tex`, subsection *The stochastic-model dispatch*.
The existing models are paragraphs (a)–(d); add **(e)** after (d), in the same shape:
one sentence of motivation, the boxed equation with a `\label`, where each symbol comes
from (config key, default), the limiting cases, how it is implemented.

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

House rules for the reference:

- **Use the existing macros** (`\zenvar`, `\sinel`, `\rx`, `\sat`, `\sigl`). They are
  defined at the top of the file or in `gnssreport.sty`. Do not invent new notation for
  something that already has a symbol.
- **Name the code** in `\texttt{}` — function, config key — so a reader can `grep` from
  the PDF to the source.
- **Fix the surrounding text.** This section said "the four-way … set" and "All four
  strategies"; both are now five. A search for `four` finds them.

Build the one component you touched, then the full volume:

```powershell
cd docs\report
python ..\..\scripts\gen_report_tables.py   # once per clone: writes the git-ignored generated\ tables
latexmk stochastic_qc.tex                   # ~1 min; open stochastic_qc.pdf and READ your paragraph
latexmk main.tex                            # the combined reference, a few minutes
cd ..\..
```

> **Trap:** on a fresh clone `latexmk main.tex` stops with
> ``File `generated/processing_modes.tex' not found`` until you have run
> `gen_report_tables.py` once. If a failed build leaves `main.pdf` deleted, restore it
> with `git restore docs/report/main.pdf` and build again.

**Check:** no `undefined` references — `findstr /c:"undefined" docs\report\stochastic_qc.log`
prints nothing (`Font shape … undefined` warnings are harmless).

The `.pdf` files are tracked in git (so people without LaTeX can read them); the
`.aux/.log/…` intermediates are ignored. Commit the `.tex` and the `.pdf`.

## Stage 10 — Commit and open the merge request

Three commits, each of which builds and passes on its own
([why](04_git_workflow.md#good-commits-for-one-change)):

```powershell
git status                          # read it. Only files you meant to touch?

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

Open the link that `git push` prints, create the merge request, paste the
[checklist](../../CONTRIBUTING.md#the-checklist), the test summary line and your
before/after numbers from Stage 8. Assign it to Brian.

---

## What you just learned

| Pattern | Where it appeared | You will reuse it for |
| --- | --- | --- |
| JSON → loader → `GnssConfig` getter → consumer | Stages 3–5 | every new option |
| New behaviour behind a config value, default unchanged | Stages 2–4, test `DefaultsAreOptIn` | every new feature |
| Reuse an existing kernel instead of re-deriving it | Stage 5 | every new model |
| Tests derived from the math: closed form, limits, contract | Stage 6 | every test |
| A bit-identity test against the old behaviour | `ZeroFractionIsBitIdenticalToElevation` | every change to a shared path |
| Explicit `CMakeLists.txt` entry | Stage 6 | every new `.cpp` |
| One-variable A/B on data, Release build | Stage 8 | every claim that something "helps" |
| Reference paragraph: motivation, boxed equation, symbols, limits, code names | Stage 9 | every change to the math |

Next: pick a first real task with Brian. Good first tasks look like this example — one
model, one option, one test file, one paragraph.
