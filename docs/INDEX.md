# GnssProcessor: documentation index

The entry point for `docs/`. The working set holds what is active: the current program, its
open and parked cards, the known issues, the config ledger, the onboarding pages and the few
references an active file cites. Closed material is in [the record](record/INDEX.md).

## The current program

- [refound/NETWORK_REFOUNDING_2026-09-14.md](refound/NETWORK_REFOUNDING_2026-09-14.md): the
  network re-founding (2026-09-14), the program every network card belongs to. The posture,
  the goldens and the baseline pair are in `CLAUDE.md` (section "Network runs: start from a
  golden").
- [refound/CARDS.md](refound/CARDS.md): the 16 open and parked cards, one entry each, in
  pre-registration order.
- `config/golden/` and `python scripts/make_golden_config.py --verify`: the armed set as
  configurations.

## Known issues

| Register | Content |
| --- | --- |
| [status/OPEN_ITEMS.md](status/OPEN_ITEMS.md) | What is owed, by area |
| [status/OPEN_ITEMS_CODE_DEFECTS.md](status/OPEN_ITEMS_CODE_DEFECTS.md) | Defects found in the code, fixed or owed |
| [status/OPEN_ITEMS_BUILT_OFF.md](status/OPEN_ITEMS_BUILT_OFF.md) | Keys built and left off, and what each waits on |
| [status/OPEN_ITEMS_CONSUMER_REALTIME.md](status/OPEN_ITEMS_CONSUMER_REALTIME.md) | The consumer and the real-time engine |

## Configuration

- [CONFIG_promotion_ledger.md](CONFIG_promotion_ledger.md): one verdict per config key, the
  view generated from `CONFIG_promotion_ledger.json` (the ledger itself);
  [topics/config-ledger.md](topics/config-ledger.md) describes how a verdict changes.
- [CONFIG.md](CONFIG.md): the configuration reference.

## Onboarding and reference

- [onboarding/README.md](onboarding/README.md): the path from an empty machine to a merged
  change, then `CONTRIBUTING.md` (repository root).
- [report/README.md](report/README.md): the mathematical reference (LaTeX, one PDF per
  component and one combined volume).
- [OVERVIEW.md](OVERVIEW.md): architecture, capabilities, build targets, config sections.
- [test_suite.md](test_suite.md): the real-data truth-graded harness.
- References an active file cites by path (the keep-list; each stays while it is cited):

  - [SPEC_eq34_reattach_hygiene.md](SPEC_eq34_reattach_hygiene.md): cited by `refound/IRD_integer_recovering_datum_2026-10-13.md`
  - [SPEC_eq34_seed_round.md](SPEC_eq34_seed_round.md): cited by `refound/IRDB_integer_recovering_datum_continued_2026-10-30.md`, `refound/IRD_integer_recovering_datum_2026-10-13.md`
  - [SPEC_prefit_census_observability.md](SPEC_prefit_census_observability.md): cited by `CLAUDE.md`, `status/OPEN_ITEMS.md`
  - [SPEC_slip_relevel_walk.md](SPEC_slip_relevel_walk.md): cited by `CLAUDE.md`
  - [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md): cited by `refound/SMX_scenario_matrix_2026-10-24.md`
  - [cards/harness/HARNESS-SEEDS.md](cards/harness/HARNESS-SEEDS.md): cited by `refound/SMX_scenario_matrix_2026-10-24.md`
  - [cards/network-vehicle/SBASIS-THEORY-E.md](cards/network-vehicle/SBASIS-THEORY-E.md): cited by `onboarding/workbook/answers_1_4.tex`, `refound/IRD_integer_recovering_datum_2026-10-13.md`
  - [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md): cited by `onboarding/workbook/answers_5_9.tex`
  - [rt_seam_decomposition.md](rt_seam_decomposition.md): cited by `CLAUDE.md`, `onboarding/where_new_code_goes.md`
  - [rt_service_conventions.md](rt_service_conventions.md): cited by `status/OPEN_ITEMS_CONSUMER_REALTIME.md`
  - [rt_service_operations.md](rt_service_operations.md): cited by `README.md`

## The record

[record/INDEX.md](record/INDEX.md) holds the closed and superseded cards, the frozen
monoliths, the attic, the decisions, the topic pages and every document no active file cites,
in the layout `docs/` had before the move of 2026-11-10. An old path is looked up with
`python scripts/docs_record.py resolve <old path>` or in
[record/path_map.json](record/path_map.json).

## The rules

1. **One card = one file.** A card of the network program is pre-registered as a file in
   `docs/refound/`, with its entry in [refound/CARDS.md](refound/CARDS.md); any other card goes
   in `docs/cards/<topic>/`.
2. **Results are appended to the card's own file**, below its text, under a dated heading. A
   closed card in the record still takes appended results.
3. **A card's entry is written twice**: at pre-registration (status `open`), and at close.
   A card closes in one commit: its STATUS becomes `closed` or `superseded`, and
   `python scripts/docs_record.py close <card>` moves the file into the record, moves its entry
   to the record's list and re-points every link to it.
4. **No file outside `docs/record/archive/` over 60 KB.** One that approaches the bar is split
   by topic. The one exemption is the generated ledger view `CONFIG_promotion_ledger.md`
   (`SIZE_EXEMPT` in `scripts/docs_index_check.py`).
5. **Every card file opens with the STATUS block** (`id`, `topic`, `status`, `summary`,
   `source`) and an **Outcome** paragraph of about 90 words. `status` is one of `open`,
   `closed`, `parked`, `superseded`, `reference`. The header is revised as results land; the
   text below it never is.
6. **The record is not rewritten.** It changes only by a move into it, by a result appended to
   a card, and by link targets the tool re-points. The frozen monoliths in
   `docs/record/archive/` are kept byte for byte.
7. **`python scripts/docs_record.py verify` passes before a docs commit.** It runs
   `scripts/docs_index_check.py` (reachability, links, STATUS blocks, the 60 KB bar, the frozen
   banners, the generated ledger view, no closed card outside the record),
   `scripts/docs_split.py --verify`, the ledger render check and
   `scripts/make_golden_config.py --verify`.

Existing section text is never rewritten, shortened or reflowed: the docs are the scientific
record, and a superseded result is annotated, not deleted. Text below a card's
`<!-- BEGIN VERBATIM: … -->` marker is the verbatim history of the 2026-09-14 split; its links
were written against the old location and are deliberately not checked.
