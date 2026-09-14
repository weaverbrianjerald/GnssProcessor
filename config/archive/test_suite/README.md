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

## All of these load in place

Nine had a bare-basename `extends` whose base stayed in `config/test_suite/`; a bare
basename resolves relative to the config's own directory, so those nine were **re-pointed
to the absolute path of their base** (`C:/GnssProcessor/config/test_suite/<base>.json`,
the convention several other arms already use) when they were archived. That edit is the
only change made to any file in this directory.

Each was verified with the loader-faithful flatten helper — 9/9 resolve and merge:

```bash
python scripts/gnss_config_chain.py config/archive/test_suite/_lvlauth_g3_on.json --chain
```

The list and the 8-deep chain behind the seven `_fld63_` / `_lvlauth_` arms are in
[`../../test_suite/README.md`](../../test_suite/README.md). The other 273 carry no
`extends` (247) or already used an absolute path (26).
