# A4 — THE GOLDEN CONFIGS — the ledger made executable, and the smoke results

**STATUS**

- **id:** A4
- **topic:** config-ledger
- **status:** closed
- **summary:** THE GOLDEN CONFIGS — the ledger made executable, and the smoke results
- **dates in the text:** 2026-08-08 → 2026-09-05
- **knobs / posture:** `network.sticky_pivot`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/CONFIG_promotion_ledger.md` — A4, A4.1

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: CONFIG_promotion_ledger.md -->
## A4 — THE GOLDEN CONFIGS (2026-09-05): the ledger made executable

**Why.** The user's own words: "we frequently chase problems we've already resolved."  The
ledger records what is validated; the loader's POSTURE block and `config_posture_check.py`
report what an arm left unarmed; but nothing produced the arm that arms everything.  Every
campaign re-derived its base from an older arm and inherited that arm's gaps (the F3 arm of
2026-09-04 was still missing 6 ON-tier + 7 SCOPED-tier recommendations that this ledger had
carried for weeks).

**What.** `config/golden/golden_manifest.json` names one golden per lineage (base config,
pins, unset keys, and the deliberate EXCEPTIONS with reasons); `scripts/make_golden_config.py`
flattens the base's `extends` chain (the loader's own merge), applies the pins, rewrites the
run paths, writes a FLAT golden with a provenance header, and VERIFIES it: every
`RECOMMEND_ON` / `RECOMMEND_SCOPED` row whose `regime` matches the golden must be armed
(`armed_when` honoured) or be a named exception — anything else, a stale exception, or a pin
that does not read back FAILS (`--selftest` covers all three).  `python
scripts/make_golden_config.py --verify` is the promotion step: a new validated row that the
goldens do not carry fails it until either the golden is regenerated with the knob pinned or
the exception is written down.

| golden | base (flattened) | armed in regime (ON / SCOPED) | exceptions (unarmed on purpose) | smoke |
|---|---|---|---|---|
| `golden_sim_tri_w6` | `_camp60wu_quiet24h_sticky` (r2q3 + sticky flagship) + F3 `phase_leveling_local_block` + handover ceiling 1e-2 + the 12 ledger gaps it carried (disc_threshold 0.25, parallel_seam, trace_section_timers, station_pd_pin_aware + phase_datum_min_stations 2, cb_kaula_prior σ 3, sbasis_complete, code_datum_time_linked, if_phase_td_ref_gate, exclude_unmodeled_antenna_stations, parallel_max_threads 20) | 15/16 · 32/33 | `estimate_iono_sh` (posture C: the sim grades the FREE slant iono against truth), `clock_datum_sum_to_zero` (does not graduate), `carry_invariant` (+ `_pd_safe`: mutually exclusive vehicle with the F3 leveling block — the ledger regime now excludes F1/F3 lineages) | see §A4.1 |
| `golden_field_gps_dual` | `_golden63_gps_1h_laneoff` (the 2026-08-08 signed-off golden, 5-base chain) + `emit_pb_filter_gate_v2` (retiring the v1 `jump_k` / `meas_sigma_m` hand tunings), `phase_leveling` (the carry_warm gate), `code_bias_disc_jump_test`, `emit_ura`, `sbasis_complete`, `sticky_pivot` (GPS L1/L2/L5 = the weak-third-signal regime), `parallel_max_threads` 20 | 21/21 · 32/33 | `clock_datum_sum_to_zero` (does not graduate) | see §A4.1 |

Both goldens are FRESH-LINEAGE seeds (fingerprinted knobs armed); a warm-resume leg inherits
its posture from its own seed's snapshot and must not re-pin them (the checker's `CHAIN:`
line).  Arms: copy the golden, change ONLY the knob under test and the run paths.

**Ledger JSON sync done alongside (2026-09-05):** the duplicate `network.sticky_pivot` row
dropped (single RECOMMEND_SCOPED row, regime `tri and not s2z and not brdc-anchored`);
`carry_invariant_pd_safe` given its regime; `carry_invariant`'s regime now excludes the
F1/F3 lineages; rows added for `phase_leveling_local_block` (F3), `phase_leveling_los_scale`
(F1, superseded by F3) and the value row `phase_datum_handover_max_var_m2 ≥ 1e-2` under
F1/F3.  The loader table (`GnssConfigNetworkPosture.cpp`) gained the same four rows plus
`sticky_pivot`, so the runtime POSTURE block and the offline checker agree again
(`config_posture_check.py --verify-loader`).

### A4.1 Smoke results (2026-09-05, Release exe of the F3 build, 20 epochs each)

| golden | exit | emitted | loader errors | run-end evidence |
|---|---|---|---|---|
| `golden_sim_tri_w6` | 0 | 20/20 | 0 | `Network (leveling local-block F3): 780 block(s) whitened, 35103 row(s) emitted, 168 column(s) marginalized at inject, 0 pair(s) zeroed, 0 refused, 0 factorization fallback(s)` — F3 live on the golden |
| `golden_field_gps_dual` | 0 | 20/20 | 0 | Card-4 carry + PD-safe fold + `emit_pb_filter_gate_v2` posture loads; `phase_leveling` armed as the carry_warm gate under carry_invariant |

The runtime POSTURE tallies printed by that exe (13/15 ON, 30/30 SCOPED on the sim golden;
19/20, 30/33 on the field golden) are the PRE-sync loader table; the offline checker on the
same files reads sim 15/16 · 33/33 (the one gap = the documented `estimate_iono_sh` posture-C
exception) and field 21/21 · 33/33.  The loader table gained the six missing rows in the same
commit (`config_posture_check.py --verify-loader` = 0 coverage gaps), so the next Release
build prints the same numbers.  Smoke recipe: `config/test_suite/_<golden>_smoke.json` =
the golden with `max_duration_s` 600 and its own run paths.

**Generator-side lesson paid on the first attempt:** the run-path rewriter must touch ONLY
output keys (`product_output_path`, `state_snapshot_path`).  Rewriting every `runs/…` string
pointed the sim golden's 60 station `rinex_path`s at `runs/<golden>/` and the smoke failed to
parse any RINEX — `OUTPUT_PATH_KEYS` in `make_golden_config.py` is the fix, and the selftest
pins it.

