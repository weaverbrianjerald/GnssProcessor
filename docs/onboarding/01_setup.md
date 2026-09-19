# 1 · Setup

**Goal:** build the code, run one PPP solution, run the unit tests.
**Time:** about half a day, mostly installs and the first build.

The steps are done in order. Each step ends with a **Check** that must pass before the
next step. The [troubleshooting table](#troubleshooting) covers the failures seen so far.

---

## Step 1: Install the tools

The build scripts assume the locations below. Using them avoids editing scripts.

| Tool | Required location | Notes |
| --- | --- | --- |
| Visual Studio **2026** Enterprise, "Desktop development with C++" workload | default install dir (`C:\Program Files\Microsoft Visual Studio\18\Enterprise`) | Provides MSVC, CMake 4.x and Ninja. A separate CMake is not needed. For the Community or Professional edition, `$vsRoot` at the top of `scripts\vs2026_build.ps1` is changed accordingly. |
| [vcpkg](https://github.com/microsoft/vcpkg) | `C:\vcpkg` | C++ dependencies (installed below). |
| Python **3.13** | `C:\Python313` | Used by the build (pybind11) and by every script. |
| Git for Windows | any | Provides `git` and Git Bash. |
| MiKTeX (or TeX Live) | any | Needed only for edits to the LaTeX reference. Can be installed later. |

Install the C++ libraries (once, about 30 min):

```powershell
C:\vcpkg\vcpkg.exe install eigen3 spdlog pybind11 gtest netcdf-c curl zlib --triplet x64-windows
```

**Check:** `C:\vcpkg\installed\x64-windows\include\Eigen` exists.

## Step 2: Clone the repository

The clone location is `C:\GnssProcessor`. The build wrapper and the IDE settings use
that path.

```powershell
git clone https://gitlab.com/weaverb/gnssprocessor.git C:\GnssProcessor
```

The project's git hooks are enabled once per clone:

```powershell
git -C C:\GnssProcessor config core.hooksPath .githooks
```

**Check:** `git -C C:\GnssProcessor config core.hooksPath` prints `.githooks`.

## Step 3: Create the Python environment

```powershell
cd C:\GnssProcessor
C:\Python313\python.exe -m venv venv
venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

`venv\` is git-ignored. The environment is activated (`venv\Scripts\Activate.ps1`) in
every new terminal before a project script is run.

**Check:** `python -c "import hatanaka, pandas, plotly"` prints nothing.

## Step 4: Build

All builds go through the wrapper. A bare `cmake --build` fails on this machine setup,
because the wrapper is what loads the compiler environment and selects the correct CMake.

```powershell
scripts\vs2026_build.ps1 -Config x64-Release      # the program that runs data  (~10 min first time)
scripts\vs2026_build.ps1 -Config x64-Debug        # the unit tests              (~10 min first time)
```

Later builds recompile only what changed and take seconds to a minute.

> **Debug versus Release.** Data is run only with the **Release** executable. Unit
> tests are run from **Debug**. A Debug data run is 50 to 100 times slower, and its
> timing numbers are meaningless.

**Check:** `build\x64-Release\gnss_processor.exe` and `build\x64-Debug\gnss_tests.exe` exist.

## Step 5: Download the test data

The repository does not store GNSS data. One script downloads a fixed set of IGS
products and station files into `data\`.

The source is NASA CDDIS, which requires a free account:

1. Register at <https://urs.earthdata.nasa.gov>.
2. Create the file `%USERPROFILE%\_netrc` with one line:
   `machine urs.earthdata.nasa.gov login YOUR_USER password YOUR_PASSWORD`

Then:

```powershell
python scripts\bootstrap_internal_data.py
```

The script can be re-run safely. Existing files are skipped.

**Check:** `data\2024-129\products\` contains `.SP3`, `.CLK` and `.BIA` files, and
`data\static\atx\igs20.atx` exists.

## Step 6: Run a first PPP solution

```powershell
build\x64-Release\gnss_processor.exe config\ppp_internal.json
```

This processes one station for one day (2024, day 129) with IGS final products. The
run is fully described by that one JSON file. Its comments are part of the
documentation and are worth reading.

**Check:** the run ends without an error and writes trace files under `logs\`.
[Page 2](02_code_tour.md) describes those files and how to plot them.

## Step 7: Run the unit tests

```powershell
build\x64-Debug\gnss_tests.exe                                   # full suite, ~90 s
build\x64-Debug\gnss_tests.exe --gtest_filter=StochasticModel*   # one suite, < 1 s
```

**Check:** the last line reads `[  PASSED  ]`, and the only skipped test (if any) is
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`.

The development setup is complete. The next page is the [code tour](02_code_tour.md).

---

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `Cannot open include file: 'memory'` | `cmake` was run directly. | Use `scripts\vs2026_build.ps1`. |
| `No CMAKE_CXX_COMPILER could be found` | Same cause, or the C++ workload is missing. | Use the wrapper; check the VS installer. |
| `vswhere.exe is not recognized` during a build | Harmless noise. | Ignore. |
| Warnings `C4828` (illegal character) | Old mixed-encoding comments. | Ignore. |
| `LNK1104: cannot open file ...gnss_processor.exe` | The exe is running, or another build is. | Close it, rebuild. |
| Linker error `unresolved external symbol` after a new `.cpp` was added | The file is missing from `CMakeLists.txt`. Sources are listed explicitly. | Add it to the list ([worked example, stage 6](03_worked_example.md#stage-6-write-the-unit-test)). |
| Bootstrap prints HTTP 401 | `_netrc` is missing or wrong. | Re-check Step 5. |
| Test run reports a failure in `SetUpTestSuite` | A fixture threw, usually because `data\` is missing. | Run Step 5. |
| Build breaks after a Visual Studio update | Stale CMake cache. | `scripts\vs2026_build.ps1 -Config x64-Debug -Reconfigure` |
