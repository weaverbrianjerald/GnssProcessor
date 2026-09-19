# 1 · Setup — from a bare machine to a first position solution

**Goal:** build the code, run one PPP solution, run the unit tests.
**Time:** about half a day, most of it waiting for installs and the first build.

Work through the steps in order. Each step ends with a **Check** — do not go on until
it passes. If a check fails, the [troubleshooting table](#troubleshooting) at the bottom
covers the failures we have actually seen.

---

## Step 1 — Install the tools

The build scripts assume these exact locations. Using them saves you from editing
scripts on day one.

| Tool | Where it must be | Notes |
| --- | --- | --- |
| Visual Studio **2026** Enterprise, "Desktop development with C++" workload | default install dir (`C:\Program Files\Microsoft Visual Studio\18\Enterprise`) | Brings MSVC, CMake 4.x and Ninja. You do **not** need a separate CMake. If you have Community/Professional, change `$vsRoot` at the top of `scripts\vs2026_build.ps1`. |
| [vcpkg](https://github.com/microsoft/vcpkg) | `C:\vcpkg` | C++ dependencies (next step). |
| Python **3.13** | `C:\Python313` | Used by the build (pybind11) and by every script. |
| Git for Windows | anywhere | Gives you `git` and Git Bash. |
| MiKTeX (or TeX Live) | anywhere | Only needed when you edit the LaTeX reference. Can wait. |

Install the C++ libraries (one time, ~30 min):

```powershell
C:\vcpkg\vcpkg.exe install eigen3 spdlog pybind11 gtest netcdf-c curl zlib --triplet x64-windows
```

**Check:** `C:\vcpkg\installed\x64-windows\include\Eigen` exists.

## Step 2 — Clone the repository

Clone to `C:\GnssProcessor`. The build wrapper and the IDE settings use that path.

```powershell
git clone https://gitlab.com/weaverb/gnssprocessor.git C:\GnssProcessor
```

Then, once per clone, turn on the project's git hooks:

```powershell
git -C C:\GnssProcessor config core.hooksPath .githooks
```

**Check:** `git -C C:\GnssProcessor config core.hooksPath` prints `.githooks`.

## Step 3 — Create the Python environment

```powershell
cd C:\GnssProcessor
C:\Python313\python.exe -m venv venv
venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

`venv\` is git-ignored. Activate it (`venv\Scripts\Activate.ps1`) in every new terminal
before running a project script.

**Check:** `python -c "import hatanaka, pandas, plotly"` prints nothing.

## Step 4 — Build

Always build through the wrapper. A bare `cmake --build` fails on this machine setup
(the wrapper loads the compiler environment and pins the right CMake for you).

```powershell
scripts\vs2026_build.ps1 -Config x64-Release      # the program you run on data  (~10 min first time)
scripts\vs2026_build.ps1 -Config x64-Debug        # the unit tests               (~10 min first time)
```

Later builds only recompile what changed and take seconds to a minute.

> **The one rule about Debug vs Release.** Run **data** only with the **Release**
> executable. Run **unit tests** from **Debug**. A Debug data run is 50–100× slower —
> minutes become hours — and its timing numbers mean nothing.

**Check:** `build\x64-Release\gnss_processor.exe` and `build\x64-Debug\gnss_tests.exe` exist.

## Step 5 — Download the test data

The repository does not store GNSS data. One script downloads a fixed set of IGS
products and station files into `data\`.

It downloads from NASA CDDIS, which needs a free account:

1. Register at <https://urs.earthdata.nasa.gov>.
2. Create the file `%USERPROFILE%\_netrc` containing one line:
   `machine urs.earthdata.nasa.gov login YOUR_USER password YOUR_PASSWORD`

Then:

```powershell
python scripts\bootstrap_internal_data.py
```

The script is safe to re-run; it skips what is already there.

**Check:** `data\2024-129\products\` contains `.SP3`, `.CLK` and `.BIA` files, and
`data\static\atx\igs20.atx` exists.

## Step 6 — Run your first PPP solution

```powershell
build\x64-Release\gnss_processor.exe config\ppp_internal.json
```

This processes one station for one day (2024, day 129) with IGS final products. The
whole run is described by that one JSON file — open it and read it; the comments in it
are part of the documentation.

**Check:** the run ends without an error and writes trace files under `logs\`.
[Page 2](02_code_tour.md) explains what those files are and how to plot them.

## Step 7 — Run the unit tests

```powershell
build\x64-Debug\gnss_tests.exe                                   # everything, ~90 s
build\x64-Debug\gnss_tests.exe --gtest_filter=StochasticModel*   # one suite, < 1 s
```

**Check:** the last line reads `[  PASSED  ]` and the only skipped test (if any) is
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`.

You now have a working development setup. Continue with
[2 · Code tour](02_code_tour.md).

---

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `Cannot open include file: 'memory'` | You ran `cmake` directly. | Use `scripts\vs2026_build.ps1`. |
| `No CMAKE_CXX_COMPILER could be found` | Same, or the C++ workload is missing. | Use the wrapper; check the VS installer. |
| `vswhere.exe is not recognized` during a build | Harmless noise. | Ignore. |
| Warnings `C4828` (illegal character) | Old mixed-encoding comments. | Ignore. |
| `LNK1104: cannot open file ...gnss_processor.exe` | The exe is running (or another build is). | Close it, rebuild. |
| Linker error `unresolved external symbol` right after you added a `.cpp` | The file is not in `CMakeLists.txt` — this project lists sources explicitly. | Add it to the list ([worked example, step 5](03_worked_example.md)). |
| Bootstrap prints HTTP 401 | `_netrc` is missing or wrong. | Re-check Step 5. |
| Test run reports a failure in `SetUpTestSuite` | A fixture threw — usually missing `data\`. | Run Step 5. |
| After a Visual Studio update the build breaks oddly | Stale CMake cache. | `scripts\vs2026_build.ps1 -Config x64-Debug -Reconfigure` |
