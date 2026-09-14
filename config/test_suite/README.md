# config/test_suite — what is here, and why

**697 `*.json` arm configs**, plus three shipped `*.jsonc` profiles
(`ppp_static`, `ppp_rtk_static`, `rt_replay_covl`) and one `.HOLD`.

This directory is the working set. On **2026-09-14** it held 979 `*.json`; **282 were
moved to [`../archive/test_suite/`](../archive/test_suite/)**, nothing was deleted, and
every byte plus its git history is intact. This is the second such pass — the first moved
234 configs to `../archive/` on 2026-07-29; see [`../archive/README.md`](../archive/README.md),
whose policy this pass follows.

## The rule a config stayed by

Applied in this order; the first that matches keeps the file.

| # | rule | count |
| --- | --- | ---: |
| 1 | **Cited by name** anywhere in `CLAUDE.md`, `docs/`, `scripts/`, `tests/`, `src/`, `include/`, the rest of `config/`, or the author's memory directory | 387 |
| 2 | **In the `extends` chain of a config kept by rule 1** — a kept config's whole lineage is kept, transitively, so no arm loses its base | 8 |
| 3 | **Warm-resume config** — it sets a non-empty `network.state_snapshot_path` (the 2026-07-29 policy, and the reason is below) | 294 |
| 4 | **Shipped / hand-maintained** — no leading underscore | 8 |
| | **total kept** | **697** |

Rule 4's eight: `global_network_stations_305.json`, `manifest_vmf3.json`,
`ppp_rtk_network_global_{5min,5min_gpsonly,par,par3h,ser,ser3h}.json`.

## Why warm-resume configs are never archived

A snapshot under `runs/` can only be resumed by a config whose **fingerprint** matches
(`NetworkRunContext.cpp`), and sixteen of the field-recommended knobs sit inside that
fingerprint. Moving such a config is harmless; *editing* one is not — with
`network.state_snapshot_require` on, the leg hard-aborts, and with it off the leg
**silently cold-starts and reads as a clean resume**. Keeping them visible in the working
set is a guard against exactly that.

## Reproducing the reference scan

```bash
# a config is "cited" if its stem appears outside config/test_suite and config/archive
grep -rl '_myarm_name' CLAUDE.md docs scripts tests src include config \
    ~/.claude/projects/C--GnssProcessor/memory | grep -v '^config/test_suite/'
```

Then close the result under `extends` — a config's `extends` may be a string or a list,
and its entries may be bare basenames, repo-relative paths, or absolute paths.

## Nine archived configs keep a base that lives here — re-pointed, and verified

`extends` entries that are bare basenames resolve relative to the config's own directory,
so an archived arm whose base stayed here would not have loaded from
`../archive/test_suite/`. Those nine were **re-pointed to the absolute path of their base**
(`C:/GnssProcessor/config/test_suite/<base>.json` — the convention several other arms
already use), so they load from the archive unchanged:

| archived arm | its base (still here) |
| --- | --- |
| `_congruence_smoke_off.json`, `_congruence_smoke_on.json` | `_ft63_redund_on.json` |
| `_fld63_relevel4h_off.json`, `_fld63_relevel_off.json`, `_fld63_relevel_on.json`, `_fld63_wh_bytecert_off.json`, `_fld63_wh_bytecert_on.json`, `_lvlauth_g3_ctl.json`, `_lvlauth_g3_on.json` | `_ext63_1h_evt_pl.json` |

Each was verified with the loader-faithful flatten helper — 9/9 resolve and merge:

```bash
python scripts/gnss_config_chain.py config/archive/test_suite/_lvlauth_g3_on.json --chain
```

The seven `_fld63_` / `_lvlauth_` arms sit on an **8-deep** chain
(`_ft63_24h_conv` → `_ft63_churn_base` → `_ft63_pdfix_on` → `_ft63_pdfix_v3` → `_agu63` →
`_golden63_gps_1h_laneoff` → `_ext63_gps_dual_1h` → `_ext63_1h_evt_pl`), which is exactly
`kMaxExtendsDepth`; every member of it stayed in this directory. The other 273 archived
configs carry no `extends` (247) or already used an absolute path (26).

## Before you add another one

Start every new network arm from a **golden**, not from a config in here:

```bash
config/golden/golden_sim_tri_w6.json        # sim truth bed
config/golden/golden_field_gps_dual.json    # field DOY-129
python scripts/make_golden_config.py --verify
```

An arm is a copy of a golden with **only** the knob under test and the run paths changed.
Check any config's posture against the field record with
`python scripts/config_posture_check.py <config> --tier all`.

Note that `extends` **arrays replace, they do not concatenate** — a concatenating
`network.stations` would silently change the roster order and count, both of which are
fingerprinted.
