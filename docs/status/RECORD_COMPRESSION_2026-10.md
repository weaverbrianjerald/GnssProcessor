# Record compression, 2026-10

Register of the compression of the project record on 2026-10-24: `CLAUDE.md`, the open-items register and the agent memory directory. The rule of every pass is that nothing is lost. Text leaves a file only by moving verbatim to a named destination, by being reduced to a pointer whose destination holds the full text, or (memories only) by being deleted because it is wrong against the tree or fully present elsewhere. Each such step is listed with byte counts before and after. Sizes are LF bytes (the git blob); the Windows working copy adds one byte per line.

| pass | target | before | after |
| --- | --- | --- | --- |
| 1 | `CLAUDE.md` | 69,800 B (70,538 B as checked out) | 34,610 B (34,992 B as checked out) |

## Pass 1: CLAUDE.md

Line numbers below are those of `CLAUDE.md` at commit 7bdcebc. The new pages are under `docs/onboarding/`: [build_toolchain.md](../onboarding/build_toolchain.md), [data_files.md](../onboarding/data_files.md), [where_new_code_goes.md](../onboarding/where_new_code_goes.md) and [claude_md_history.md](../onboarding/claude_md_history.md).

### Moved verbatim

| original lines | text | destination | bytes |
| --- | --- | --- | --- |
| 20 to 64 | VS 2026 migration facts: vcvars64 path, toolchain versions, the two CMakes, the `%PATH%` trap, the cached-static logger defect, the vswhere noise | build_toolchain.md, Migration facts | 3,222 |
| 72 to 76 | hardware background of the disk placement (C: and D: drives, USB link) | build_toolchain.md, Disk placement | 353 |
| 92 to 95 | scratchpad bullet of the disk placement, original wording | build_toolchain.md, Disk placement | 255 |
| 197 to 224 | `runs/` Layout entry: the four dated relocations, the MANIFEST story, the 2026-08-12 archive | build_toolchain.md, Run-directory relocations | 2,110 |
| 516 to 637 | File management in full: unified layout, both modes, migration from the flat layout, CDDIS authentication, crx2rnx, `ResolvedPaths`, bootstrap, baseline fixture presets, `.gz` cleanup | data_files.md | 6,209 |
| 656 to 738 | the two anti-regrowth decision trees at full length (network producer, real-time engine); the two headings are raised from level 3 to level 2 | where_new_code_goes.md | 23,732 |
| 493 to 512 | Retired network rules (the heading text is quoted at the top of the block) | claude_md_history.md | 1,653 |
| 455 to 470 | Baseline: card GAD paragraph with its run numbers | claude_md_history.md | 1,414 |
| 245 to 262, 273 to 278, 293 to 300 | Documentation section: introduction and tree block, rule 5, ledger paragraph | claude_md_history.md | 2,402 |
| 187 to 193, 226 to 236 | Layout entries `tests/`, `scripts/`, `data/` | claude_md_history.md | 1,225 |
| 11 | Build sentence that introduced the wrapper | claude_md_history.md | 49 |
| 641 to 645 | Known state at full length (2083-case count and its history, suite hygiene, C4828 warnings, vswhere, real-time mode) | claude_md_history.md | 2,397 |
| 649 to 654 | Entry points at full length | claude_md_history.md | 1,581 |

A line-by-line check (every non-blank line of the old file against the new file and the four pages, whitespace normalised) finds each line verbatim in one of them, except the heading of the Retired block, whose text is quoted in claude_md_history.md. The Layout block is checked as word sequences per entry (30 entries unchanged in words, four replaced).

### Condensed in CLAUDE.md, full text in the destination

| section | what the compact text keeps | what it drops |
| --- | --- | --- |
| Build | wrapper calls, targets, build tree, the three disk-placement rules, the Debug rule | migration narrative (a two-line pointer remains), hardware detail, the scratchpad rationale |
| Layout | every entry as written (the block is re-wrapped: description column 22 to 16, width 120; same words) | the `runs/` story (one sentence, the MANIFEST pointer and the trust rule remain), stale counts in `tests/` |
| Documentation | rules 1 to 4, 6, 7, the never-rewrite paragraph, the ledger procedure | the introduction and tree block (the INDEX maps the tree), the field list of rule 5 (INDEX rule 5 holds it) |
| Network runs | posture, arms, grading, sim start, shared conventions, interim hold, armed QC, the grading traps | the GAD run numbers (card GAD holds them), the Retired list (names kept, records in the history page) |
| File management | `data_source` modes, date selection, path precedence | layout tree, CDDIS, crx2rnx, bootstrap, presets, `.gz` cleanup |
| Known state | latest recorded test count with date, what the suite does not validate, suite hygiene rule | counts of earlier dates, LOC ratio, the C4828 and vswhere bullets |
| Entry points | one paragraph naming `Application.cpp`, `GnssConfigLoad.cpp` and `StateKeySchema.hpp` with their rules | per-file prose, the `KalmanFilter.hpp` and `PathResolver.hpp` lines (File management holds the latter) |
| Decision trees | one rule per row with its destination and its "never" clause; the real-time rows are merged where short | entity access functions, check scripts per row, line counts, card lineage, the real-time cut statistics |

### Corrected while moving

- Known state: the 2083-case figure (2026-09-14) is replaced by the latest recorded count, 3182 cases (3158 passed, 24 fixture-gated skips, 0 failed), Debug, 2026-10-17, from `docs/status/OPEN_ITEMS.md`. Not re-measured in this pass: the wrapper rebuilds the main checkout.
- Layout `data/` entry described the pre-date-first layout (`products/{orbit,clock,...}/{zip,unzip}/`). It now states the current layout, checked against `C:\GnssProcessor\data` (`2024-129\{obs,products}`, `static`, `cache`).
- Layout `tests/` entry: the 1540/215/104 counts (superseded) and the "80 cases" of `gnss_simulator_selftest` (not re-verified) are dropped.
- Layout `scripts/` entry: the "see File management below" clause moved into the `data/` entry.

### Other files edited

- `docs/INDEX.md`: links to the four pages and to this register.
- `docs/OVERVIEW.md`: the migration-notes pointer now names build_toolchain.md.
- `docs/onboarding/README.md`: a reference-pages table. `docs/onboarding/07_network_tour.md`: a pointer to where_new_code_goes.md.

### Left as written

Header, the Visual Studio directive, the wrapper block, targets, build tree, disk-placement rules 1 to 3, the Debug rule, Conventions, Standing invariants, the golden rules and the grading traps.
