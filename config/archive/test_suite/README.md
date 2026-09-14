# config/archive/test_suite — retired arm configs, 2026-09-14

282 configs moved here from `config/test_suite/` on 2026-09-14. **Nothing was deleted**;
every byte and all git history is intact, and any of these can be moved back.

They moved because nothing in `CLAUDE.md`, `docs/`, `scripts/`, `tests/`, `src/`,
`include/`, the rest of `config/`, or the author's memory directory names them, and they
are not in the `extends` chain of a config that is named — mostly one-off diagnostic A/B
arms from cards that have since closed (`_agu305_*`, `_agu63_rp_*`, `_agu63pv2_rp_*`, …).

The full rule set, the counts, and the reproduction command are in
[`../../test_suite/README.md`](../../test_suite/README.md). The sibling directory
`../` holds the first such pass (234 configs, 2026-07-29) and its policy notes.

## ⚠ These are still the reproduction record

A closed card's verdict is only reproducible from the config that produced it, and this
project's own history shows closed verdicts getting re-opened. Archiving is deliberately
reversible; pruning was declined. Before deleting anything here, check whether the
matching `runs/<name>/config.json` copy also exists — those per-run copies are the
independent provenance.

## ⚠ Nine of these cannot run in place

Their `extends` is a bare basename, which resolves relative to the config's own directory,
and their base stayed in `config/test_suite/`. Move the arm back, or re-point its
`extends` to `../../test_suite/<base>.json`. The list is in
[`../../test_suite/README.md`](../../test_suite/README.md). The other 273 either carry no
`extends` (247) or use an absolute path (26).
