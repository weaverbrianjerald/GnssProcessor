# GnssProcessor

A C++17 engine for high-precision GNSS positioning. It implements PPP and PPP-RTK for
a single receiver, in post-processing (RINEX + IGS products) and in real time (NTRIP /
RTCM3 / Septentrio SBF), plus double-differenced baselines and a multi-station network
producer. Core components: a Kalman filter with a dynamic state vector, integer
ambiguity resolution (MLAMBDA), cycle-slip detection and repair, full physical
modelling, and an RTS smoother.

New contributors start at [docs/onboarding](docs/onboarding/README.md).

## Quick start

Requirements: Windows, Visual Studio 2026, vcpkg at `C:\vcpkg`, Python 3.13. Details
and troubleshooting are in [Setup](docs/onboarding/01_setup.md).

```powershell
scripts\vs2026_build.ps1 -Config x64-Release                    # build (always through this wrapper)
python scripts\bootstrap_internal_data.py                       # download the test data, once
build\x64-Release\gnss_processor.exe config\ppp_internal.json   # run PPP; results are written to logs\
python pyviz\run_report.py logs                                 # plot the results
```

```powershell
scripts\vs2026_build.ps1 -Config x64-Debug -Target gnss_tests   # unit tests
build\x64-Debug\gnss_tests.exe
```

> **Release runs data. Debug runs tests.** A Debug data run is 50 to 100 times slower.

## Repository layout

| Path | Contents |
| --- | --- |
| `include/` · `src/core/` | The engine. Headers (which carry the documentation) and sources mirror each other: `app`, `model`, `filter`, `measurement`, `slip`, `atmosphere`, `corrections`, `products`, `rinex`, `rtcm`, `sbf`, `realtime`, `config`, `math`, `logging`, `common`. |
| `tests/` | GoogleTest unit suite, built as `gnss_tests.exe`. |
| `config/` | One JSON file describes one run. `ppp_internal.json` is the PPP starting point. |
| `data/` | GNSS observations and products. Git-ignored; populated by the bootstrap script. |
| `docs/onboarding/` | Setup, code tour, worked example, git workflow. |
| `docs/report/` | The LaTeX mathematical reference (`main.pdf`). |
| `docs/` (remainder) | Research record of the network program. Entry point: `docs/INDEX.md`. |
| `pyviz/` | Python plotting and HTML reports for run output. |
| `scripts/` | Build wrapper, data bootstrap, analysis utilities. |
| `tools/` · `python/` · `matlab/` · `third_party/` | Side tools, pybind11 module, legacy MATLAB viewers, vendored IAU SOFA. |
| `build/` · `logs/` · `venv/` | Generated. Git-ignored. Safe to delete. |

Run outputs, archives and reference software are kept outside the repository, in
`D:\GnssProcessorSupport\` (see its `MANIFEST.md`).

## Documentation

| Topic | Document |
| --- | --- |
| Setup and a first change | [docs/onboarding](docs/onboarding/README.md) |
| Contribution rules and the merge-request checklist | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Mathematical reference | [docs/report/main.pdf](docs/report/main.pdf) |
| Architecture, capabilities, build targets, config sections | [docs/OVERVIEW.md](docs/OVERVIEW.md) |
| Configuration options | [docs/CONFIG.md](docs/CONFIG.md), then the Doxygen in `include/config/GnssConfig.hpp` |
| Real-time operation | [docs/rt_service_operations.md](docs/rt_service_operations.md) |
| Network producer | [docs/INDEX.md](docs/INDEX.md) and [CLAUDE.md](CLAUDE.md) |

## License

No license file is present. All rights are reserved by the author; redistribution
requires the author's permission.
