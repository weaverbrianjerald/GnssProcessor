# config/archive — retired campaign configs

234 configs moved here from `config/test_suite/` on 2026-07-29. **Nothing was deleted**; every
byte and all git history is intact, and any of these can be moved back or run in place.

## Why they moved

`config/test_suite/` had accumulated 350 configs, most of them one-off diagnostic A/B arms from
cards that are now closed (`_c4ab_arm{A,B,C}_*`, `_clAiono_*`, `_dcmp_50sta_*`, …). That made
the *active* config set illegible — the directory gave no signal about which configs anything
still runs.

A config stayed in `config/test_suite/` if any of these held:

- it is named in any `manifest*.json`, script, doc, source file, or memory record;
- it sets `network.state_snapshot_path` (a **warm-resume** config — see the warning below);
- it has no leading `_` (the shipped, hand-maintained configs).

## ⚠️ These are still the reproduction record

A closed card's verdict is only reproducible from the config that produced it. The project's
own history shows closed verdicts getting re-opened, so archiving is deliberately reversible
and pruning was declined. Before deleting anything here, check whether the corresponding
`runs/<name>/config.json` archive also exists — those per-run copies are the independent
provenance, and `runs/**` was left completely untouched by this move.

## ⚠️ Warm-resume configs were NOT moved, on purpose

40 configs set `network.state_snapshot_path`. Their paired snapshots under `runs/` can only be
resumed by a config whose **fingerprint** matches (`NetworkRunContext.cpp:180-315`), and 16 of
the field-recommended knobs are inside that fingerprint. Moving such a config is harmless, but
*editing* one is not: with `network.state_snapshot_require` on the leg hard-aborts, and with it
off the leg **silently cold-starts and reads as a clean resume**. The loader's POSTURE block
now prints a `CHAIN:` line whenever that situation applies — see
`docs/CONFIG_promotion_ledger.md`.

## Reducing the sprawl going forward

The root cause of 493 standalone configs was that the loader had no composition mechanism. It
now supports `"extends"`:

```json
{
  "extends": "../profiles/network_305.json",
  "network": { "raim": true }
}
```

Bases merge left-to-right, the child wins, objects merge key-by-key, and **arrays replace
rather than concatenate** (a concatenating `network.stations` would silently change the roster
order and count, both of which are fingerprinted). A config with no `extends` key parses
exactly as before — verified byte-identical on a 305-station config.

Check any config's posture against the field record with:

```bash
python scripts/config_posture_check.py <config> --tier all
```
