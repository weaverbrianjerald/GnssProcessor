# RKT: a warn-once tombstone of the retired configuration keys

**STATUS**

- **id:** RKT
- **topic:** config-ledger
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-30: follow-on F4 of card RKD. A declared table of the 150 network keys card RKD deleted with their code, each with its deletion commit, beside the loader; at load, one warning per retired key a configuration sets, under either spelling the old loader read. No key is added and nothing else changes. Gates frozen in section 3 before any run of the changed executable.
- **source:** card RKD section 5 (follow-on F4, plan question Q2, plan risk R1); the register item "Retired-key deletion: documentation notes and follow-ons" ([OPEN_ITEMS](../status/OPEN_ITEMS.md)); the loader and the ledger at main `4bdd693` (`git show 4bdd693:docs/CONFIG_promotion_ledger.json`)
- **knobs / posture:** none: no key is added, read or armed; both goldens unchanged
- **dates in the text:** 2026-10-30
- **hashes / run dirs:** base main `5127429`; reference exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_rktref.exe` (the Release build of `5127429` in the worktree `C:/GnssProcessor/.claude/worktrees/pensive-ptolemy-4f6046`, build tree `D:/GnssProcessorSupport/worktrees/pensive-ptolemy-4f6046_build`, sha256[:16] `45ddd4c9329e8ced`); candidate `gnss_processor_rkt.exe` beside it (the Release build of branch `claude/pensive-ptolemy-4f6046` in the same build tree); run root `D:/GnssProcessorSupport/runs_nom`, arms `_rap_rkt_ref_f2`, `_rap_rkt_cand_f2`, `_rap_rkt_ref_s2`, `_rap_rkt_cand_s2` and `_rap_rkt_ret_f2`; user-side tags `rktref` and `rkt`
- **gates (frozen at pre-registration):** section 3, written before any run of the changed executable

**Outcome.** Question: can the loader name each retired key a configuration sets, once and with the deletion that removed it, while every run stays as it was? Pending: the gates of section 3.

## 1. Question

Card RKD deleted 150 network keys with their code on 2026-10-14. The loader has no unknown-key check, so a
configuration that still sets a deleted key loads the old default without a word (plan risk R1; about 712 live
configurations did at the time). A refusal would make those configurations and the retired goldens fail to load
(card RKD section 5), so the follow-on is a warning. `scripts/rapid_arm.py make` already refuses an unread key on
the tool side (`scripts/loader_keys.py`); the loader itself says nothing. The question: can the loader warn once per
retired key, naming the key and the deletion, with no other change to any run?

## 2. Design

| piece | where | what |
| --- | --- | --- |
| the table | `include/config/RetiredConfigKeys.hpp`, `kRetiredConfigKeys` | one row per retired key: the dotted path, the flat top-level spelling the old loader also read, and the family; 150 rows |
| the families | the same header, `kRkd*` | one per deletion commit of card RKD (28): the card, the date, the commit on main and what the keys armed |
| the scan | `findRetiredConfigKeys`, `src/core/config/RetiredConfigKeys.cpp` | one hit per row the merged document sets: the dotted path through nested objects, or the flat key at the top level (the two lookups of `detail::jval`, in its order); `network.stations[].fixed` once for the whole roster |
| the warning | `warnRetiredConfigKeys`, called at the head of `loadNetworkSection_` | one `gnss_config` warning per hit, naming the key, the spellings found, the value, and the card, date, family and commit of the deletion |
| the test | `tests/test_retired_config_keys.cpp` | the pins listed below |

The facts the table rests on, checked against `4bdd693` and the 35 deletion commits before the build:

- The ledger at `4bdd693` holds 141 REMOVE rows. Card RKD's count of 144 is the plan's appendix A, which adds the
  three `lane_carry` rows (KEEP_OFF and PARAM) deleted with the WL-3 carry. With the five `mw_wl` keys the lane layer
  orphaned (PARAM and TRACE rows) and `iono_constraint_deweight_stations` (no row) the deletion is 150 keys; 149
  ledger rows left the ledger (784 to 635).
- 149 keys were read by `jval` with one flat spelling each, and no two share one. `network.stations[].fixed` was read
  from each roster object and had none. All 150 reads sat in network-section helpers (`loadNetwork*_`), so the scan
  runs in network mode only, where the reads stood, and a user-side run cannot change.
- The family of a key is the commit whose loader diff removed its read: 28 commits, with the counts of card RKD
  section 1 and the orphaned keys under `365edfe` (five) and `158b9b2` (one).
- No retired path is read by the current loader (`scripts/loader_keys.py`), and no flat spelling is a string literal
  of `src/core/config/*.cpp`.
- The table is in a header on purpose: `scripts/loader_keys.py` and `scripts/comment_gate.py` read every string
  literal of `src/core/config/*.cpp` as a key the loader reads, so a retired key spelled there would read as live and
  `rapid_arm.py make` would stop refusing it. The `.cpp` holds no key name.

The unit test pins: (1) the rows equal the 141 REMOVE rows of the ledger at `4bdd693` (embedded, blob `c8d31c0e`)
plus the nine keys deleted beside them; (2) the families in commit order with card RKD's counts, the rows of a family
sorted; (3) every `jval` key has its own flat spelling; (4) no row is a key of the current ledger; (5) the current
goldens set none and the retired `golden_field_gps_dual.json` sets 28; (6) the scan: either spelling found once, the
dotted value when both are set, the roster member once, and silence on live keys, near misses, a flat spelling below
the top level and a dotted name used as one literal key; (7) the message text; (8) a load with six retired keys logs
six warnings in table order and otherwise exactly the log of the same configuration without them.

## 3. Gates (frozen 2026-10-30, before any run of the changed executable)

The reference executable is the Release build of main `5127429` in this worktree (`gnss_processor_rktref.exe`,
sha256[:16] `45ddd4c9329e8ced`). The candidate is the Release build of this branch from the same build tree, copied
beside it as `gnss_processor_rkt.exe`. Arms are golden copies made with `scripts/rapid_arm.py make` under
`RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`; the sim arms start at 00:30:15 (`network.start_offset_hours`
0.504167). At most two arms run at once while card NOM5's gate runs are live.

| gate | arms | bar |
| --- | --- | --- |
| G0 unit and tools | Debug `gnss_tests`; scripts | the suite `RetiredConfigKeys` passes; the full suite passes apart from the fixture-gated skips; `loader_keys.py selftest`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader` and `docs_index_check.py` pass |
| G1 field | `_rap_rkt_ref_f2` and `_rap_rkt_cand_f2`, the 2 h field golden | `nom_td_gate.py --whole` IDENTICAL |
| G2 sim | `_rap_rkt_ref_s2` and `_rap_rkt_cand_s2`, the 2 h sim golden | `nom_td_gate.py --whole` IDENTICAL |
| G3 user side | `uom_gate.py` quick set, tags `rktref` and `rkt` | 16 of 16 IDENTICAL |
| G4 retired keys present | `_rap_rkt_ret_f2`: the 2 h field golden plus the 28 retired keys `golden_field_gps_dual.json` sets, at its values, plus `carry_iono` and `network_fast_clock_enable` flat and `parallel_reduce` flat beside its dotted spelling; candidate exe, against `_rap_rkt_cand_f2` | `nom_td_gate.py --whole --no-arm-check`: every product, CSV and run manifest record identical; the logs differ only by inserted warning lines, 30 (one per retired key the arm sets), each naming its key, card RKD and the deletion commit |
| G5 census | every `config/**/*.json` resolved through `extends` (`scripts/gnss_config_chain.py`) | reported with the results: the network-mode configurations that now warn and the keys they set; the census reads the header's table, and on the G4 arm it predicts exactly the warnings the candidate printed |

A gate that fails is reported with the difference; no tolerance is applied to a byte comparison. Results are
appended below under dated headings.
