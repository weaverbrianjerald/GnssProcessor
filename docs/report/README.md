# GnssProcessor: Mathematical Reference

LaTeX skeleton for the technical documentation that complements the
Doxygen-rendered API reference under `include/`.

## Build

Requires a TeX distribution with `latexmk` and `biber`:

- **Windows:** MiKTeX or TeX Live; both ship `latexmk` and `biber`.
- **macOS:** MacTeX (`brew install --cask mactex`).
- **Linux:** `apt install texlive-full` or distro equivalent.

From this directory:

```sh
latexmk -pdf main.tex
```

`latexmk` runs `pdflatex` / `biber` / `pdflatex` / `pdflatex` as needed
to resolve cross-references and citations. The PDF lands at
`main.pdf`.

To clean intermediate files:

```sh
latexmk -c
```

To clean intermediates **and** the PDF:

```sh
latexmk -C
```

## Layout

```
docs/report/
  main.tex                  Top-level document; \input's the section files.
  notation.tex              Symbol table + sign conventions.
  state_vector.tex          Kalman filter state vector layout.
  observation_equations.tex Uncombined PPP observation model.
  bssd_dd.tex               BSSD and DD differencing transformations.
  references.bib            Starter bibliography (BibLaTeX).
  README.md                 This file.
```

Each section file is meant to stand alone: edit one without touching
the others, and `main.tex` glues them together.

## Scope

This document is restricted to **invariant** material:

- State vector layout (rarely changes).
- Observation equations (settled GNSS theory).
- Differencing transformations (algebraic, code-independent).
- Filter equations (KF predict/update, RTS smoother).

Implementation notes that mirror evolving code — call signatures,
configuration field reference, per-class responsibilities — belong
in the headers under `include/` and are rendered via Doxygen.
Cross-references between the two are explicit:

- LaTeX → C++: cite the source path inline, e.g.
  *"see `src/core/filter/KalmanFilter.cpp`"*.
- C++ → LaTeX: cite the section anchor in the header docstring, e.g.
  `@see GnssProcessor mathematical reference, §3.2`.

The point of the split is that the LaTeX side ages slowly (equations
don't churn) while the Doxygen side tracks the codebase one-to-one.
