# Build toolchain: Visual Studio 2026 migration record

Background for the Build section of [`CLAUDE.md`](../../CLAUDE.md). `CLAUDE.md` keeps the wrapper calls, the rule against running data on the Debug tree and the disk-placement rules. This page holds the migration record and the traps behind the wrapper `scripts/vs2026_build.ps1`, the hardware background of the disk placement and the run-directory relocations. The blocks below moved verbatim out of `CLAUDE.md` on 2026-11-02 (from main `86f9125`; a first pass of 2026-10-24 is kept under the git tag `archive/inspiring-torvalds-2026-10-24`); every date inside them is the original date.

## Migration facts (verified 2026-08-12)

Migration facts, all verified 2026-08-12 (fresh configure + 538/538 build +
full unit suite under the new toolchain):

- **vcvars64.bat moved.** `...\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\`
  — `18` is the install *major-version* dir, not a year. The user's default
  shell still does NOT source it, so a naive `cmake --build` fails with
  "Cannot open include file: 'memory'".
- **Toolchain: MSVC 19.51.36256 / toolset 14.51.36231.** The existing
  `C:\vcpkg` tree is v143-built and links against it cleanly (Eigen, spdlog,
  gtest 1.17, netCDF, CURL, ZLIB, pybind11 2.13.6) — no vcpkg rebuild needed.
- **⚠ Two CMakes on this box.** `cmake` on PATH is the standalone **3.18.3**
  (`C:\Program Files\CMake`) and vcvars does NOT override it; VS 2026 bundles
  **4.3.1** at `...\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`.
  The wrapper pins the bundled 4.3.1 by absolute path so a CLI configure and
  an IDE configure agree. CMake 4 hard-errors on `cmake_minimum_required` < 3.5;
  the lowest in our dependency closure is exactly 3.5 (and only in a doc file
  plus a vcpkg-internal port script, neither consumed by `find_package`), so
  the project configures clean.
- **⚠ Never `set "PATH=...;%PATH%"` inside the `cmd /c "vcvars && ..."` chain.**
  `cmd` expands `%PATH%` when it parses the whole `a && b` line — *before*
  vcvars runs — so the assignment writes back the pre-vcvars PATH and silently
  destroys the compiler entries, giving "No CMAKE_CXX_COMPILER could be found"
  on a machine where the compiler is fine. Ninja needs no pinning: vcvars
  already puts VS 2026's own ninja (1.13.2) on PATH.
- **⚠ The migration surfaced one real code defect — the cached-static logger
  accessor.** `RinexNavParser.cpp`'s `navLog()` held
  `static shared_ptr lg = spdlog::get(...) ? ... : spdlog::default_logger();`
  and returned `lg.get()`. Under VS 2026 that faulted (0xC0000005) on the only
  two parser paths that log, so the suite showed exactly two failures
  (`RinexNavParser.RejectsPreRinex3`, `MissingEndOfHeaderReturnsEmpty`).
  Causality was confirmed both ways (revert reproduces, fix clears), and a
  standalone probe in the identical Debug/`SPDLOG_SHARED_LIB` flavour showed
  `spdlog::default_logger()` itself returns a **valid** logger — so this is NOT
  a vcpkg ABI problem; explicit loggers and fmt formatting cross the v143 DLL
  boundary fine. **The pattern to avoid is caching a raw pointer out of a
  by-value `shared_ptr` (worse, out of a ternary of two DLL-returned
  temporaries) in a function-local `static`.** Resolve per call and return the
  `shared_ptr`; `spdlog::default_logger()` is nullable in principle because
  `spdlog::drop_all()` clears it, and `LogManager` calls that on both
  re-initialize and shutdown. `tailLog()` in `NetworkProductIO.cpp` had the
  same nullable shape with three unguarded `->` uses and was hardened too.
  Whether VS 2022 also faulted on the *current* source is unverified — the
  archived pre-migration exe is a different source revision, and current
  source was not rebuilt with 2022 per the retirement directive.
- `vswhere.exe not recognized` from the cmd subshell remains benign noise.
- **Update 2026-11-12 (commit `70f9d6f`).** The wrappers moved from the Enterprise
  edition to Visual Studio 2026 Community (`18\Community`, same compiler). They run
  `vcvars64.bat -vcvars_ver=14.51` and stop with a message when the component "MSVC
  Build Tools v14.51 for x64/x86" is missing, so the compiler stays that of every
  baseline. The IDE configures its own trees, `build\ide-x64-Debug` and
  `build\ide-x64-Release`, apart from the wrapper trees. The paths above name
  `18\Enterprise` as recorded on 2026-08-12. Current setup: [`01_setup.md`](01_setup.md).

## Disk placement: hardware background (owner request, 2026-10-13)

**Disk placement (owner request, 2026-10-13: keep C: free, target D: for builds
and runs).** C: is a 1 TB internal NVMe that runs short; D: is a 4 TB NVMe SSD
in a USB enclosure on a 5 Gbps port (about 0.4 GB/s; Release builds on D:
timed within the range of C: worktree builds, being CPU-bound; anything on D:
is unavailable while it is disconnected).

The scratchpad rule of the same section, in its original wording:

- Claude session scratchpads (`%TEMP%\claude\...`, where session worktrees
  used to accumulate 11 GB builds) move to D: only if the owner sets the
  documented `CLAUDE_CODE_TMPDIR` variable (user environment or user
  settings `env`; new sessions only).

## Run-directory relocations

The `runs/` entry of the Layout block of `CLAUDE.md`, as it stood before the compression:

```
runs/                 Campaign run outputs.  ⚠️ RELOCATED OFF C: AGAIN on
                      2026-09-19 (the repo-root cleanup for onboarding): the
                      741 run dirs then in the tree (149 GB, incl. every RF-* /
                      ALC / PBT run cited by `docs/refound/`) are at
                      D:\GnssProcessorSupport\runs\ (same names), so a cited
                      `runs/<name>` resolves there.  `reference_software/`
                      (M-PAGES, POINT, cssrlib), `val_out/`,
                      `test_suite_results/`, `out/`, `batch_results*/` and the
                      stray root files moved with them;
                      D:\GnssProcessorSupport\MANIFEST.md maps every old path.
                      Again on 2026-10-06: 4,719 finished run dirs (266 GB)
                      moved to the same place (MANIFEST addendum); the live
                      fixtures, baselines and the 2026-10-03+ working set
                      stayed on C:.  Again on 2026-10-13: 441 entries older
                      than a week (35 GB) moved there too, same rule, plus
                      every FCF / AGT arm (280 dirs, 60 GB; cards closed),
                      plus the runs of every other closed card (1,095 dirs,
                      65 GB) (MANIFEST addendum).  `runs/` is still the git-ignored default output dir; a
                      new campaign recreates it.  Earlier: ARCHIVED on 2026-08-12
                      to D:\GnssProcessor_archive\2026-08-12\runs\ (619 dirs,
                      40.6 GB) along with the pre-VS2026 build/ trees, to
                      reclaim disk.  Both are git-ignored.  Reports that cite
                      a `runs/<name>` or `build/x64-Release/run_<name>` path
                      (e.g. CONFIG_promotion_ledger.md §A1,
                      CLOCK_HADAMARD_CALIB_2026-07-14.md) resolve under the
                      archive, NOT in the working tree.  Per user: those
                      results predate the recent network work and are not
                      trusted — regenerate rather than re-grade them.
                      DELETED on 2026-11-02 (owner request: anything over
                      two months old): the archive's runs/ and every
                      D:\GnssProcessorSupport\runs\ folder whose newest file
                      predates 2026-09-02, incl. the pre-SFR sim fixtures
                      (simnet60_*_wu93*, simnet60_tri, simnet24_inject_*);
                      77 GB, MANIFEST addendum.  A run cited from before
                      September is regenerated, not looked up.  The
                      archive's build/ trees (26 GB, incl. the archived
                      pre-migration exe) went the same day, so
                      D:\GnssProcessor_archive\ no longer exists.
```
