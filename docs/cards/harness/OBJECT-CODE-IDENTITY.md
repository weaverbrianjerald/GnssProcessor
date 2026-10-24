# OBJ-IDENTITY: object-code identity proof for pure moves, and the shared logs directory hazard

**STATUS**

- **id:** OBJ-IDENTITY
- **topic:** harness
- **status:** reference
- **summary:** Method to prove a pure code move cannot change behaviour in any mode (disassembly and headers of the Release object file before and after), and the hazard that concurrent runs from one working directory overwrite each other's traces.
- **dates in the text:** 2026-09-23 to 2026-10-07
- **source:** agent memory notes `project_object_code_identity_proof` and `project_concurrent_run_log_collision`, copied 2026-10-24

**Outcome.** Release compiles with /O2 /Ob2 /Zi and no /GL, so the object files are real machine code: identical disassembly and headers (apart from the timestamp) prove an edit cannot change behaviour in any mode, which one same-build output A/B cannot. Card PRM cites the method by name as a gate for pure moves. The second note records the logs directory hazard (`Application::log_dir_` is relative to the working directory) and the launch rule: one working directory per concurrent run.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: object-code identity proof for pure moves (method and capture traps)

These results were recorded only in the agent memory note `project_object_code_identity_proof` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Method, first used 2026-09-23 on the `KalmanFilter::update` const_cast fix; `docs/refound/PRM_parameter_registry_2026-09-29.md` cites this memory by name as a gate for pure moves. Release compiles `/O2 /Ob2 /Zi` with no `/GL` (re-verified in `build/x64-Release/build.ninja` 2026-10-07), so `build/x64-Release/CMakeFiles/gnss_core.dir/<path>.cpp.obj` is real machine code.

1. Save the baseline .obj, after confirming it was compiled from the pre-edit source.
2. Rebuild, then `dumpbin /nologo /disasm` and `/headers` on both (`C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe`; about 20 MB of listing for KalmanFilter).
3. Identical disassembly and identical headers except the timestamp prove the edit cannot change behaviour in ANY mode; one same-build output A/B covers only one config's paths. The only expected raw-byte diffs: header TimeDateStamp, `.debug$S` records and their reloc offsets, the PDB age in `.debug$T`, `.chks64`. Map `cmp -l` offsets to sections via the /headers file pointers to prove it.

**How to apply:** keep the edit's line count unchanged when possible (stable line tables, a clean diff for concurrent sessions). Still run the output A/B when the author asks for one.

Capture traps (2026-09-23):
- The `kalman_filter` spdlog file ROTATES (`kalman_filter.1.log`) on 1 h PPP-RTK runs with debug logging: count events from stdout, which carries every line.
- PowerShell `*>` rewrites bare-LF stdout lines to CRLF: compare line content, not bytes, across capture methods (`Start-Process -RedirectStandardOutput` keeps raw bytes).
- Run-to-run noise in PPP logs is wall-clock only: `KF timing (us)`, `Processing completed in N ms`, the timing summary, the wls/kalman_debug wall-clock prefixes and headers. Mask exactly those.
- The persistent-bias R-inflation (`estimation.persistent_bias_min_cycles`, default 0.10) fires 0 times in 1 h of `config/ppp_rtk_internal.json` (GCGO); a floor of 0.01 gives 2367 events on 41 rows, enough to exercise it.

## 2026-10-24: concurrent runs share the logs directory of the working directory

These results were recorded only in the agent memory note `project_concurrent_run_log_collision` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

⚠ `Application::log_dir_{"logs"}` (Application.hpp) is relative to the process working directory and no config key sets it (still true 2026-10-07; CLAUDE.md Layout records the path, not the hazard). The LogManager and every trace writer (`TraceLogger`, `NetworkRunTraces`, the MW / TurboEdit / wind-up traces) hang off it, so two runs from one CWD write the same `logs/*.csv` and `main_log.log`, and the later one truncates the earlier's mid-run. Redirecting `network.product_output_path` / `state_snapshot_path` does not isolate the logs.

- Both runs exit 0 and the solve is unaffected; what is destroyed is every CSV trace, the evidence the arm was launched to produce. Each process's stdout (run-end counter lines) and the redirected products survive.
- Rule: every concurrent arm runs with its own working directory. `scripts/ev_launch.ps1` does it (CWD = run dir, `logs/` created there) and `scripts/rapid_arm.py run` launches through it. By hand: `Push-Location <run dir>`, then `& C:\GnssProcessor\build\x64-Release\gnss_processor_<arm>.exe <absolute config>`. Config paths are absolute, so a foreign CWD is safe; the exe stays beside its DLLs (memory note `feedback_concurrent_build_locks`).
- Read traces from `<CWD>/logs/`, never `build/x64-Release/logs/` (memory note `feedback_stale_trace_dir_fake_ab`).
- Paid for 2026-08-22: two 63-station 4 h field arms (wind-up census, withhold attribution) launched from the repo root; the second truncated the first's traces about 5 min in; both re-run from separate directories.
- Open, small: a `logging.log_directory` knob would remove the footgun (not built as of 2026-10-07).
