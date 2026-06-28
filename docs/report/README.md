# GnssProcessor: Mathematical Reference

LaTeX technical documentation that complements the Doxygen-rendered API
reference under `include/`.

The reference is organised as a set of **component documents**, each of which
compiles **two ways**:

- as its own **standalone PDF** (e.g. `slip_detection.pdf`) — a focused,
  shareable document covering one core component; and
- as a **part of the combined volume** (`main.pdf`) — the complete reference,
  with a single table of contents, bibliography, and cross-references resolved
  to section numbers.

Both come from one source tree via the [`subfiles`](https://ctan.org/pkg/subfiles)
package. Notation, packages, and TikZ styles live once in `gnssreport.sty` (the
single source of truth), so a symbol renders identically everywhere.

## Build

Requires a TeX distribution with `latexmk` and `biber`:

- **Windows:** MiKTeX or TeX Live; both ship `latexmk` and `biber`.
- **macOS:** MacTeX (`brew install --cask mactex`).
- **Linux:** `apt install texlive-full` or distro equivalent.

From this directory:

```sh
latexmk                       # build the combined volume + every standalone PDF
latexmk main.tex              # build only the combined volume
latexmk slip_detection.tex    # build only one component, standalone
```

A bare `latexmk` builds every target because `latexmkrc` lists them in
`@default_files` (with `main.tex` first, so its `.aux` exists before the
standalone builds). Convenience wrappers are also provided:

```sh
./build.ps1          # Windows (PowerShell);  -Regen regenerates tables, -Clean cleans first
./build.sh           # Unix;  --regen / --clean
```

`latexmk` runs `pdflatex` / `biber` / `pdflatex` as needed to resolve
cross-references and citations. Clean intermediates with `latexmk -c`;
clean intermediates **and** PDFs with `latexmk -C`.

## Layout

```
docs/report/
  gnssreport.sty            SHARED PREAMBLE: packages, TikZ styles, ALL notation
                            macros, and the \dcref cross-document macro. Single
                            source of truth — do not redefine notation elsewhere.
  main.tex                  Combined volume. \subfile's every component, grouped
                            with \part, plus the introduction and bibliography.
  latexmkrc                 Build config: a bare `latexmk` builds all targets.
  build.ps1 / build.sh      Convenience build wrappers.

  COMPONENT DOCUMENTS (each a standalone-capable subfile):
  foundations.tex           Shared substrate. Wrapper that supplies the \section
                            headings and \input's the four content fragments
                            below — the notation, state vector, observation
                            equations, and differencing that everything cites.
    notation.tex              Symbol table + sign conventions.
    state_vector.tex          Variable-dimension Kalman state vector layout.
    observation_equations.tex Uncombined PPP pseudorange/phase observation model.
    bssd_dd.tex               BSSD (PPP-RTK) and DD (baseline) transformations
                              + S-transform structure.
  frames_time.tex           WGS84 ECEF<->geodetic<->ENU, GNSS time scales + leap
                            seconds (GLONASS TOD, BeiDou BDT), legacy IAU-1976/1980
                            and CIO-based ECI<->ECEF transforms.
  product_evaluation.tex    Baseline orbit/clock product evaluation: derotated-
                            Neville / sidereal-trig SP3 interpolation, rate-anchored
                            clock LSQ, broadcast Kepler / GLONASS PZ-90, ephemeris
                            selection — the baseline the SSR chapter overlays.
  atmosphere.tex            Tropospheric (Saastamoinen ZHD, Niell/GMF/VMF3/GPT3
                            mapping, Chen-Herring gradients) and ionospheric
                            (uncombined slant scaling, GIM/IONEX STEC + iono-
                            weighted pseudo-obs, Klobuchar seed, higher-order)
                            delay models.
  site_displacement.tex     Geophysical station displacements: solid Earth tide
                            (IERS-2010 Steps 1+2), pole + ocean-pole tide, ocean
                            tidal loading, and the LOS-projection integration.
  observation_physics.tex   Deterministic per-observation corrections: antenna
                            PCO/PCV + ATX fallback, satellite attitude/yaw, phase
                            wind-up, relativity/Shapiro/Sagnac, group-delay/biases.
  stochastic_qc.tex         Measurement stochastic models (elevation/SNR/sat-block
                            weighting) + a-posteriori QC: variance factor, Baarda
                            data-snooping, redundancy numbers, MDB, full-R kernel.
  estimator_infrastructure.tex  Filter-bootstrap machinery: WLS position/clock/tropo
                            seed, first-epoch residual-difference state initialisation
                            (ISB/phase-clock/code-bias/iono/ambiguity), BSSD/DD pivot
                            selection + disturbance detection.
  kalman_smoother.tex       Variable-dim EKF (Joseph update, prior-anchored IEKF,
                            robust gating) + backward RTS smoother.
  slip_detection.tex        MW/GF/GRAPHIC/EWL combinations + TurboEdit 2x2 repair,
                            code-event veto, gap bridge.
  ambiguity_resolution.tex  MLAMBDA ILS, LDL^T decorrelation, WL/NL, validation,
                            constrained (pseudo-observation) feedback.
  imu_aiding.tex            Strapdown mechanisation + 15-state INS error model.
  ssr_corrections.tex       SSR orbit/clock/bias to undifferenced-observation
                            correction mapping.
  realtime_engine.tex       Streaming architecture: NTRIP transport, RTCM/MSM
                            decode, observation/SSR synchronisation, per-epoch
                            real-time update path.
  network.tex               Multi-station PPP-RTK network estimator: joint model,
                            satellite phase-bias state, S-basis datum, Schur
                            reduction — the producer of the SSR-style corrections
                            the single-receiver modes consume.
  pipeline_diagrams.tex     Curated TikZ process diagrams.
  generated_tables.tex      Appendix subfile; \input's the generated/ tables.

  generated/                Auto-generated reference tables (see below).
    processing_modes.tex      ProcessingMode/AppMode/DataSource/DifferencingMode.
    config_toggles.tex        Feature flags + ModelCorrections + LoggingConfig.
    slip_detectors.tex        Cycle-slip detector registry (active vs. staged).
    state_keys.tex            Dynamic state-vector key formats.
  references.bib            Bibliography (BibLaTeX).
  README.md                 This file.
```

### Editing a component

Each component file is a subfile:

```latex
\documentclass[main.tex]{subfiles}
\begin{document}
\ifSubfilesClassLoaded{...standalone title...}{}
\section{...}\label{sec:...}
   ... body: edit here ...
\ifSubfilesClassLoaded{\printbibliography}{}
\end{document}
```

Everything outside the `\section ... \printbibliography` body is boilerplate
that makes the dual (standalone + combined) build work — leave it alone. Notation
comes from `gnssreport.sty`; reuse those macros rather than redefining them.

### Cross-document references

Within one document, use `\Cref{...}` as usual. To reference a label defined in
a **different** component document, use `\dcref{label}{human-readable name}`
(defined in `gnssreport.sty`). In the combined volume it renders the section
number; in a standalone PDF — where a combined-volume number would be
meaningless — it renders the readable name. Example:

```latex
the SSR correction model (\dcref{sec:ssr}{the Real-time SSR corrections document})
```

## Generated reference tables (do not edit by hand)

Everything under `generated/` is produced from the C++ source tree by
`scripts/gen_report_tables.py`, so the tables track the code instead of
drifting from it. The generator parses the *single source of truth* for
each table — the cycle-slip registry, the `jval<bool>` config loader, the
`make*Key` builders in `StateKeySchema.hpp`, and the mode enums — and emits
LaTeX fragments. A renamed flag or a new detector shows up on the next run.

```sh
python scripts/gen_report_tables.py            # regenerate the fragments
python scripts/gen_report_tables.py --check    # exit non-zero if stale (CI guard)
```

The curated diagrams in `pipeline_diagrams.tex` are the opposite: they show
data flow and algorithm phases that a parser cannot recover, so they are
maintained by hand. Each box cites the function it maps to; keep them in
sync when the pipeline changes.

> **Note on the API docs.** The Doxygen build (`cmake --build … --target docs`)
> renders class/include/collaboration *structure* graphs when Graphviz `dot`
> is installed — enabled automatically via `CMakeLists.txt`. Install Graphviz
> and re-run CMake to turn them on; add `-DGNSS_DOXYGEN_CALL_GRAPHS=ON` for the
> verbose per-function call graphs.

## Scope

This document covers the estimation mathematics and design rationale —
the material that ages slowly because it is settled theory or algebra,
not churning code:

- State vector layout (rarely changes).
- Observation equations (settled GNSS theory).
- Coordinate frames and time systems (WGS84/ENU, leap seconds, CIO/SOFA).
- Orbit/clock product evaluation (SP3 interpolation, Kepler/PZ-90).
- Atmospheric delay models (troposphere mapping, ionosphere GIM/HOI).
- Geophysical site displacements (solid Earth / ocean / pole tides).
- Observation-model physics (antenna, attitude, relativity, range corrections).
- Differencing transformations (algebraic, code-independent).
- Filter equations (KF predict/update, IEKF, RTS smoother).
- Stochastic model and quality control (weighting, variance factor, Baarda).
- Estimator infrastructure (WLS seed, state initialisation, pivot management).
- Cycle-slip combinations and the TurboEdit integer repair.
- Integer ambiguity resolution (MLAMBDA, WL/NL, validation).
- Inertial aiding (strapdown mechanisation, INS error state).
- Real-time SSR-to-observation correction mapping.
- Real-time streaming engine (transport, decode, synchronisation).
- Network-mode PPP-RTK estimation (the correction producer).

Empirical thresholds and tuning (MAD/Welford/EWMA gates, per-arc
calibration) are deliberately left to the code and cited, not
reproduced. Implementation notes that mirror evolving code — call
signatures, configuration field reference, per-class responsibilities —
belong in the headers under `include/` and are rendered via Doxygen.
Cross-references between the two are explicit:

- LaTeX → C++: cite the source path inline, e.g.
  *"see `src/core/filter/KalmanFilter.cpp`"*.
- C++ → LaTeX: cite the section anchor in the header docstring, e.g.
  `@see GnssProcessor mathematical reference, §3.2`.

The point of the split is that the LaTeX side ages slowly (equations
don't churn) while the Doxygen side tracks the codebase one-to-one.
