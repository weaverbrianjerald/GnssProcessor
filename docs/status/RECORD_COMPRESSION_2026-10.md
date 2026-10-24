# Record compression, 2026-10

Register of the compression of the project record on 2026-10-24: `CLAUDE.md`, the open-items register and the agent memory directory. The rule of every pass is that nothing is lost. Text leaves a file only by moving verbatim to a named destination, by being reduced to a pointer whose destination holds the full text, or (memories only) by being deleted because it is wrong against the tree or fully present elsewhere. Each such step is listed with byte counts before and after. Sizes are LF bytes (the git blob); the Windows working copy adds one byte per line.

| pass | target | before | after |
| --- | --- | --- | --- |
| 1 | `CLAUDE.md` | 69,800 B (70,538 B as checked out) | 34,667 B (35,049 B as checked out) |
| 2 | `docs/status/OPEN_ITEMS.md` | 56,276 B, 79 items, 38 lines over 300 characters | 25,303 B, 79 items, 22 lines over 300 characters (none over 376 except one note) |

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

## Pass 2: docs/status/OPEN_ITEMS.md

The head rule of the register is one line per item with its evidence. Line numbers below are those of the file at commit bfb1f8e. Every item over about 300 characters (27 items, 38 lines, 39,846 bytes; the eight lines over 1,500 characters among them) moved verbatim to the card that owns it, under the dated heading "2026-10-24: register text moved from OPEN_ITEMS.md" at the end of the card, one sub-heading per item; the register row keeps one line and the link. The 79 top-level items are all still present.

### Moved verbatim

| old lines | items | destination | destination size, before to after |
| --- | --- | --- | --- |
| 22, 62, 63, 95 | open-cards row NSD, stash-status mirrors, hard clock refloat, mixed-system clock step | refound/NSD_network_small_defects_2026-10-17.md | 17,580 to 19,939 B |
| 23, 102 | open-cards row NIS, consumer innovation screen | refound/NIS_consumer_innovation_screen_2026-10-17.md | 33,109 to 34,622 B |
| 59 | block_baarda on the float vehicle | refound/BBE_block_baarda_estimable_2026-10-18.md | 25,775 to 26,676 B |
| 96 | GPS + Galileo cost | refound/NETCOSTB_gps_galileo_cost_continued_2026-10-18.md | 20,766 to 22,639 B |
| 137, 141, 143 | N3c signal decisions, object model remainder, onboarding parameter page | refound/NOM4_network_object_model_finish_2026-10-15.md | 39,241 to 41,074 B |
| 158 | Build 5, SIF outside its window | refound/SEA_seasonal_term_outside_window_2026-10-16.md | 25,937 to 26,968 B |
| 159, 160 | Build 2, ocean tide loading; SIF loading horizontal vector | refound/OTL_ocean_tide_loading_source_2026-10-16.md | 48,240 to 52,086 B |
| 161 | Build 1, clock pivot election | refound/PIV_pivot_election_coverage_2026-10-17.md | 26,330 to 27,618 B |
| 162 | Build 3, noon-turn attitude | refound/NTA_noon_turn_attitude_2026-10-16.md | 32,326 to 33,673 B |
| 163 | Build 4, wind-up through gaps | refound/WGB_windup_gap_bridge_2026-10-17.md | 52,846 to 54,435 B |
| 164 | retired-key deletion notes and follow-ons | refound/RKD_retired_key_deletion_2026-10-14.md | 14,639 to 16,591 B |
| 45, 106 to 115, 120, 121, 127, 129 to 131, 133 to 135 | gate records of the fix branches: product contract gaps (fix/contract); real-time gap tolerance with its seven sub-records, real-time suspicions, GLONASS 1020 date (fix/realtime); script-hygiene items and both check stages (hyg/scripts) | status/OPEN_ITEMS_CODE_DEFECTS_GATES.md (new) | 0 to 25,158 B |

The gate records are code-defect records, whose home is OPEN_ITEMS_CODE_DEFECTS.md. That file is 58,281 B against the 60 KB bar (61,440 B) and the records total about 25 KB, so they form a sibling page, listed in the INDEX and linked from the register's Code defects section. No destination reaches the bar (largest: WGB 54,435 B, OTL 52,086 B). NETCOST itself (61,366 B) takes no appended text for the same reason; its continuation card NETCOSTB holds the cost record.

A check of every non-blank line of the old register against the new register and the thirteen destinations finds each of them verbatim.

### Changed in the register rows

Each replacement row is written from the old row and shortened; two changes are corrections of state, made because the old rows would otherwise repeat a claim that no longer holds:

- GPS + Galileo cost: the old row listed `network.sparse_station_normals` as default off and the arming decision as owed. Card NETCOSTB (section "Armed (2026-10-19)") and the ledger (verdict ARMED, loader default true, pinned by both goldens) record that it was armed on 2026-10-19; the new row says so.
- Builds 1 to 5 and the fix branches: the old rows of Build 5 and Build 4 owed the merge. `git branch --merged` on 2026-10-24 lists fix/netsmall, fix/contract, fix/realtime, hyg/scripts, mpg/seasonal, mpg/otl, mpg/pivot, mpg/attitude and mpg/windupgap as merged into main; the new rows say "merged into main" and drop "owed: the merge".

Rows of 303 and 316 characters (the census site list, the archive candidates) were already one line and stay as written. After the pass 22 lines exceed 300 characters, all between 303 and 376 including their links, plus the 396-character note of the Code defects section.

### Other files edited

`docs/INDEX.md` (a line for the new page); the Code defects section of the register (one sentence naming the new page).
