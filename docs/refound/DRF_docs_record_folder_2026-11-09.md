# DRF: the docs working set and one record folder

**STATUS**

- **id:** DRF
- **topic:** build-infra
- **status:** open
- **summary:** Built and rehearsed on branch `docs/record-folder` (not merged): `scripts/docs_record.py` moves every closed or superseded card, the frozen monoliths, the attic, the decisions, the topic pages and every uncited document into `docs/record/` (old layout mirrored, links re-pointed, nothing rewritten), and leaves 145 of 602 docs files in the working set (this card included). Every docs check passes on the rehearsal. Landing waits for the in-flight branches and the author's decisions (section 7).
- **source:** the author, 2026-11-09: "I also want to completely clean up our 'docs', 'cards', etc. I mean, we should eliminate clutter in this project and only keep the active or known-issue parts."; answers that day: closed material goes to "One record folder", the reach is "Docs plus branches"
- **knobs / posture:** none; no code path, key or golden changes (C++ edits are path strings in comments and log text)
- **dates in the text:** 2026-11-09
- **hashes / run dirs:** base main `27d6ece`; no runs

**Outcome.** Question: can `docs/` hold only the active and known-issue parts, with nothing lost? Result: one rule (closed or superseded cards move; open and parked cards stay; a reference card or an uncited document stays only while an active file cites it by path) leaves 145 files in the working set (this card included) and moves 457 by `git mv` into `docs/record/`, which mirrors the old layout. 912 links and 1,322 path strings are re-pointed; no record prose and no verbatim text changes. Verdict: rehearsal PASS on all four docs checks. Standing: open until the author decides the rule text and lands it.

## 1. Question

`docs/` tracked 601 files on main `27d6ece` (602 with this card). 299 of the 329 cards were closed or superseded and 20 were open or parked; the index (45 KB), the two program card lists (46 KB and 38 KB) and 57 loose specs sat beside them. The author asked for the working set to keep only what is active or a known issue, with the closed material in one record folder outside it, and nothing lost.

## 2. The rule

A card whose STATUS is `closed` or `superseded` moves to the record. A card whose STATUS is `open` or `parked` stays where it is. A `reference` card, and a document with no STATUS block (a spec, design, plan, handoff or queue), stays only while an active file cites it by path: `CLAUDE.md`, `README.md`, `CONTRIBUTING.md`, `docs/onboarding/`, `docs/status/`, `docs/report/`, the program page and the open and parked cards. A companion without a STATUS block (`*_part2.md`, a notices file) follows its parent card. The frozen monoliths, their routing stubs, the attic, the decisions and the topic pages move whole; the config ledger's topic page stays with the ledger. Code comments that cite a document do not keep it: they are re-pointed.

`docs/topics/` moves (16 of 17 pages): each page is an index of closed cards with a "current posture" dated 2026-09-14, now carried by `CLAUDE.md`, the ledger and the onboarding pages. `docs/report/` stays whole: it is the maintained mathematical reference (LaTeX sources and PDFs, revised 2026-10-30), cited by the onboarding pages, and holds no cards.

## 3. Counts and the tree after

| folder | stay | move |
| --- | --- | --- |
| top level | 14 | 70 (18 routing stubs, 52 specs, designs, plans, handoffs, old indexes) |
| `archive/` | 0 | 18 |
| `attic/` | 0 | 44 |
| `cards/` | 4 | 145 |
| `css/` | 2 | 0 |
| `decisions/` | 0 | 2 |
| `onboarding/` | 40 | 0 |
| `refound/` | 21 (19 open or parked cards, a companion, the program page) | 162 |
| `report/` | 59 | 0 |
| `status/` | 4 | 0 |
| `topics/` | 1 | 16 |
| total | 145 | 457 |

```
docs/
  INDEX.md                          the active index (6 KB)
  CONFIG_promotion_ledger.json/.md  the ledger and its view;  CONFIG.md, OVERVIEW.md, test_suite.md
  SPEC_eq34_reattach_hygiene.md  SPEC_eq34_seed_round.md  SPEC_prefit_census_observability.md
  SPEC_slip_relevel_walk.md  rt_seam_decomposition.md  rt_seam_manifest.json
  rt_service_conventions.md  rt_service_operations.md
  refound/    NETWORK_REFOUNDING_2026-09-14.md, CARDS.md (19 entries), 19 open or parked cards (+1 companion)
  cards/      harness/ (2 reference cards), network-vehicle/ (1), sim-bed/ (1)
  status/     the four registers;  topics/  config-ledger.md
  onboarding/ (40)   report/ (59)   css/ (2)
  record/     INDEX.md, path_map.json, inventory.tsv, refound/, cards/, archive/, attic/, decisions/,
              topics/, and the 70 top-level files
```

Keep-list (references an active file cites; each stays while cited): `CONFIG.md`, `OVERVIEW.md`, `test_suite.md`, `SPEC_eq34_reattach_hygiene.md`, `SPEC_eq34_seed_round.md`, `SPEC_prefit_census_observability.md`, `SPEC_slip_relevel_walk.md`, `rt_seam_decomposition.md`, `rt_service_conventions.md`, `rt_service_operations.md`, `cards/harness/HARNESS-REF.md`, `cards/harness/HARNESS-SEEDS.md`, `cards/network-vehicle/SBASIS-THEORY-E.md`, `cards/sim-bed/SIMNET-REF.md`. Each one's citers are listed in the new index.

## 4. The tool

`python scripts/docs_record.py plan | apply | verify | close <card> | resolve <old path>`.

- `apply` computes every rewrite against the tree before any move, moves by `git mv`, re-points Markdown link targets and `docs/...` path strings in the files that stay (`docs/` outside the record, `CLAUDE.md`, `README.md`, `CONTRIBUTING.md`, `src/`, `include/`, `tests/`, `scripts/`), re-points in record files only link targets that pointed outside the record, never touches text below a line-start `<!-- BEGIN VERBATIM:` marker, rewrites the ledger's `card` paths as text and re-renders the view, re-points the `scripts/docs_split.py` maps (new optional `archive` and `stub` keys), splits the card lists, writes both indexes, the path map and the inventory, and ends with `verify`. It is re-runnable: on a split tree it moves only what has closed since.
- `close <card>` is `apply` restricted to one card and its companions; it refuses a card whose STATUS is not `closed` or `superseded`.
- `verify` runs `scripts/docs_index_check.py`, `scripts/docs_split.py --verify`, `scripts/ledger_render.py --check` and `scripts/make_golden_config.py --verify`, checks the path map, and warns on an open program card with no list entry. `docs_index_check.py` gains check 7: once `docs/record/` exists, a closed or superseded card outside it fails.
- File contents are read from the object store (`git cat-file --batch`), not from the working tree: a worktree on the shared USB disk took minutes to read; the plan takes about 30 s.

## 5. The rehearsal (branch `docs/record-folder`, base `27d6ece`)

`apply` moved 457 files and re-pointed 912 links and 1,322 path strings in 555 files. An audit of the staged change against `27d6ece` (every rename and modification compared blob to blob, CRLF read as LF) found: 205 record files differ only in link targets; 44 staying Markdown files and 300 other files differ only in link targets and `docs/` to `docs/record/` path strings; no file changed below a verbatim marker. `docs_index_check.py` PASS (every Markdown file reachable from the index, no dangling link), `docs_split.py --verify` 18 of 18 PASS, `ledger_render.py --check` PASS, `make_golden_config.py --verify` PASS. C++ edits are path strings only: 480 changed lines in `src/` and `include/`, of which 16 are message strings of the loader and the run end (log text changes, products do not); the tree was not rebuilt, since no edit leaves a comment or a string literal.

## 6. Landing

After the in-flight branches have merged (they bring new and newly closed cards and edits to the index files), on a branch from the then main:

1. `git cherry-pick` the tool commit (scripts and this card; no other docs change; every check passes on the old layout).
2. `python scripts/docs_record.py apply` (the one command), then commit.
3. `git cherry-pick` the rule-text commit if the author adopts it (`CLAUDE.md`, section "Documentation").

## 7. Decisions left to the author

1. **The rule text.** The proposed documentation section of `CLAUDE.md` is the last commit on the branch; the new rules of `docs/INDEX.md` are in `scripts/docs_record_index.md` (the template `apply` renders).
2. **Generated card lists (proposed, not adopted).** Every branch that pre-registers or closes a card appends to the same tail of the program list, which is where concurrent branches conflict. A generator (`docs_record.py lists`, checked like the ledger view) could render the active list from each card's STATUS block between GENERATED markers. Costs: a STATUS summary runs to several hundred characters, so each card needs a short one-line field (a header revision); the order needs a pre-registration date, which program cards carry in the file name and topic cards do not; and a generated file still conflicts on merge, resolved by re-running the generator. Recommended scope: generate the active list only (about 20 entries) and keep the record lists frozen, appended by `close`.
3. **Register pruning (nothing changed beyond links).** Resolved by their own text: `OPEN_ITEMS.md` 8 entries (two superseded duplicates of "Self-contained clock level" and "Inter-constellation level without CODE"; five "Sim bed and harness" tooling items; "Card names in code: done"), `OPEN_ITEMS_CODE_DEFECTS.md` 8 (the ProductCache deadlock, `service_mode`, run-manifest dump, loader messages, redundancy-census mask, minor suspicions, the bootstrap login page and its DataDownloader twin), `OPEN_ITEMS_CONSUMER_REALTIME.md` 2 (real-time gap tolerance, GLONASS 1020 replay date), `OPEN_ITEMS_BUILT_OFF.md` none; 17 more are partly resolved. `OPEN_ITEMS_BUILT_OFF.md` still describes as off five keys armed on 2026-11-07 (card RF0HV18B section 5.1): `raim_prior_channel`, `if_phase_td_reseed_on_flag`, `if_phase_td_sat_clock_event` with `clock_anchor_defer_coupled`, `estimation.seed_full_equation`, `signal_integrity.census`. The "Open cards" table of `OPEN_ITEMS.md` lists six cards whose STATUS is closed (NOM, NOM2, NOM3, ARL, NIS, NSD) and omits eleven open or parked ones (ALD, BRD3, IRDF, PES, PIV, SCR, SMX, ZTL, AB1, BCL, SCU); `refound/CARDS.md` now carries the open list. A resolved page in the record would take the resolved entries.
4. **`OPEN_ITEMS_CODE_DEFECTS.md` is at the bar:** 61,418 bytes on a CRLF checkout after the re-pointed links (+77), 22 bytes under 61,440. The next entry fails rule 4; moving its resolved entries (item 3) or splitting it is owed.
5. **`docs/topics/` and `docs/report/`:** as in section 2. Alternative: keep the topic pages' "Conventions and invariants" blocks in the onboarding pages. `docs/report/main.bbl-SAVE-ERROR` (2026-06-28) is a LaTeX build leftover, a deletion candidate.
6. **Not re-pointed:** `config/` (224 configurations name a `docs/` path, mostly in `_comment` fields; out of scope, resolvable through the path map); the goldens' provenance line names the ledger's previous hash until the next regeneration; the agent memory files outside the repository; `scripts/gen_docs_index.py` still writes the superseded generated index as `README.md` at the top of `docs/`.
