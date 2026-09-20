# 5 · Debugging in Visual Studio

**Goal:** stop the program at a chosen line, inspect the filter state, and step through
one epoch.

The command-line wrapper and the IDE use the same build trees (`build\x64-Debug`,
`build\x64-Release`) and the same CMake settings (`CMakeSettings.json`), so a tree built
by one can be used by the other.

---

## Open the project

1. Visual Studio → **Open a local folder** → `C:\GnssProcessor`. The project is a CMake
   folder; there is no `.sln` file.
2. Configuration drop-down (toolbar): **x64-Debug**. The first CMake configure takes
   about a minute. Its output is in **Output → CMake**.
3. **Build → Build All**, or build a single target from the **Solution Explorer →
   CMake Targets View**.

## Set the program arguments

`gnss_processor.exe` takes the config file as its one argument. Arguments are stored in
`.vs\launch.vs.json`, a per-user file that is git-ignored and therefore absent after a
clone.

1. **Select Startup Item** drop-down (next to the green arrow): `gnss_processor.exe`.
2. **Debug → Debug and Launch Settings for gnss_processor**. Visual Studio creates
   `.vs\launch.vs.json`. Its content is replaced by:

```json
{
  "version": "0.2.1",
  "defaults": {},
  "configurations": [
    {
      "type": "default",
      "project": "CMakeLists.txt",
      "projectTarget": "gnss_processor.exe",
      "name": "gnss_processor.exe",
      "currentDir": "C:/GnssProcessor",
      "args": [ "C:/GnssProcessor/config/_my_debug.json" ]
    },
    {
      "type": "default",
      "project": "CMakeLists.txt",
      "projectTarget": "gnss_tests.exe",
      "name": "gnss_tests.exe",
      "currentDir": "C:/GnssProcessor",
      "args": [ "--gtest_filter=StochasticModel*", "--gtest_break_on_failure" ]
    }
  ]
}
```

`currentDir` makes `logs\` land in the repository root, as it does for command-line
runs. Without it the traces are written next to the executable in the build tree.

## Keep Debug runs short

A Debug build is 50 to 100 times slower than Release. A full-day run is not debugged;
a short one is. The debug config is a copy of a sample config with a time limit:

```powershell
copy config\ppp_internal.json config\_my_debug.json
```

In `_my_debug.json`, in the `processing` block:

```jsonc
    "max_duration_s": 1800,        // stop after 30 min of data (60 epochs at 30 s)
```

Files named `config\_my_*.json` are git-ignored. A defect that appears only late in the
day is first located from the Release traces (`logs\*.csv`, the epoch time), and the
Debug run is then limited to a window that reaches it.

## Breakpoints worth knowing

| Location | Stops at |
| --- | --- |
| `Application::run()`, first line of the epoch loop (`collectEpochObservations`) | The start of every epoch |
| `PppModel::computeModels()` | Observation modelling for one epoch |
| `StochasticModelWeight::computeVariance()` | Every variance computation (frequent; use a condition) |
| `CycleSlipManager::detectAndRepair()` | Slip detection for one epoch |
| `KalmanFilter::update()` | The measurement update, with H, z and R assembled |
| `AmbiguityResolver::resolve()` | Integer ambiguity resolution |
| `RealTimeEngine::processEpoch()` | The start of every real-time epoch |

Useful debugger features:

- **Conditional breakpoint** (right-click the breakpoint → Conditions): for example on
  the satellite identifier or on an epoch counter. Without a condition a per-observation
  breakpoint stops thousands of times.
- **Eigen matrices** appear as nested `m_storage` members. A small matrix is easier to
  read in the **Immediate Window** with an expression such as `P(0,0)` or `x(3)`.
- **Tracepoint** (breakpoint → Actions → log a message, continue execution): prints a
  value every epoch without editing the code.

## Debugging a unit test

The second launch configuration runs one test suite. `--gtest_break_on_failure` stops
the debugger at the failing assertion. The filter is changed in `launch.vs.json` to the
suite under work.

## When the debugger is the wrong tool

Many estimation problems are found faster from the traces than from a breakpoint:

| Symptom | First file to inspect |
| --- | --- |
| Position jumps or diverges | `logs\state_trace.csv`, then `logs\measurement_residuals.csv` at the jump epoch |
| A satellite is missing or unused | `logs\satellite_status_trace.csv` |
| Ambiguities reset too often | `logs\cycle_slip_manager.log`, `logs\ambiguity_trace.csv` |
| A setting seems to have no effect | `logs\gnss_config.log` (parsed values are logged there; a misspelled key is silently ignored) |

A log file is empty unless its logger is enabled **and** its level in `logging.levels`
is below `"warn"`.

Next: [6 · First tasks](06_first_tasks.md).
