# GnssProcessor

A C++17 engine for high-precision GNSS positioning: **PPP** and **PPP-RTK** for a single
receiver — in post-processing (RINEX + IGS products) or in real time (NTRIP / RTCM3 /
Septentrio SBF) — plus double-differenced baselines and a multi-station network
producer. Kalman filter with a dynamic state vector, integer ambiguity resolution
(MLAMBDA), cycle-slip detection and repair, full physical modelling, RTS smoother.

**New here? → [docs/onboarding](docs/onboarding/README.md)** takes you from an empty
machine to your first merged change.

## Quick start

Windows, Visual Studio 2026, vcpkg at `C:\vcpkg`, Python 3.13 — details and
troubleshooting in [Setup](docs/onboarding/01_setup.md).

```powershell
scripts\vs2026_build.ps1 -Config x64-Release                    # build (always through this wrapper)
python scripts\bootstrap_internal_data.py                       # download the test data, once
build\x64-Release\gnss_processor.exe config\ppp_internal.json   # run PPP; results land in logs\
python pyviz\run_report.py logs                                 # plot them
```

```powershell
scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_tests   # unit tests
build\x64-Debug\gnss_tests.exe
```

> **Release runs data. Debug runs tests.** A Debug data run is 50–100× slower.

## Where things are

| Path | What |
| --- | --- |
| `include/` · `src/core/` | The engine. Headers (with the documentation) and sources mirror each other: `app`, `model`, `filter`, `measurement`, `slip`, `atmosphere`, `corrections`, `products`, `rinex`, `rtcm`, `sbf`, `realtime`, `config`, `math`, `logging`, `common`. |
| `tests/` | GoogleTest unit suite → `gnss_tests.exe`. |
| `config/` | One JSON file = one run. Start with `ppp_internal.json`. |
| `data/` | GNSS data and products. Git-ignored; filled by the bootstrap script. |
| `docs/onboarding/` | Setup, code tour, worked example, git workflow. |
| `docs/report/` | The LaTeX mathematical reference — `main.pdf`. |
| `docs/` (rest) | Research record of the network program; entry point `docs/INDEX.md`. |
| `pyviz/` | Python plotting and HTML reports for run output. |
| `scripts/` | Build wrapper, data bootstrap, analysis utilities. |
| `tools/` · `python/` · `matlab/` · `third_party/` | Side tools, pybind11 module, legacy MATLAB viewers, vendored IAU SOFA. |
| `build/` · `logs/` · `venv/` | Generated. Git-ignored. Safe to delete. |

Run outputs, archives and reference software live **outside** the repository, in
`D:\GnssProcessorSupport\` (see its `MANIFEST.md`).

## Documentation

| I want to… | Read |
| --- | --- |
| get set up and make a first change | [docs/onboarding](docs/onboarding/README.md) |
| know the rules and the merge-request checklist | [CONTRIBUTING.md](CONTRIBUTING.md) |
| understand the math | [docs/report/main.pdf](docs/report/main.pdf) |
| see architecture, capabilities, build targets, config sections | [docs/OVERVIEW.md](docs/OVERVIEW.md) |
| look up a config option | [docs/CONFIG.md](docs/CONFIG.md), then the Doxygen in `include/config/GnssConfig.hpp` |
| run real time | [docs/rt_service_operations.md](docs/rt_service_operations.md) |
| work on the network producer | [docs/INDEX.md](docs/INDEX.md) and [CLAUDE.md](CLAUDE.md) |

## License

No license file yet: all rights reserved by the author. Ask before redistributing.
