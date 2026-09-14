# LEDGER — how to read the ledger — the verdict tiers, the evidence rules, and the relationship to CONFIG_promotion_ledger.json

**STATUS**

- **id:** LEDGER
- **topic:** config-ledger
- **status:** reference
- **summary:** how to read the ledger — the verdict tiers, the evidence rules, and the relationship to CONFIG_promotion_ledger.json
- **dates in the text:** 2026-07-15 → 2026-08-15
- **knobs / posture:** `network.mw_wl.bridge_sample_debias`, `processing.elevation_mask_deg`, `network.service_mode`, `network.x`, `network.clock_datum_sum_to_zero`, `network.trace_phase_anchor`, `network.trace_raim_redundancy`, `network.mark_refsat_reset_disc`, `network.serialize_sbasis_contract`, `network.emit_epoch_status_record` … (+5 more in the text)
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/guards_cont`
- **source:** `docs/archive/CONFIG_promotion_ledger.md` — (preamble)

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: CONFIG_promotion_ledger.md -->
# Config promotion ledger — which default-OFF knobs are validated, and in what regime

**Why this exists.** The project ships new work default-OFF and byte-identity-gated (standing
invariant #6). That is right for *new* work, but there is no **promotion step**: nothing moves from
"validated good" to "default on", so validated knobs pile up in the OFF column and get re-discovered
by accident, one wasted run at a time.

**This is a ledger, not a switch — scope is load-bearing.** `network.mw_wl.bridge_sample_debias` was
validated at 50 stations as the bridge age-bias fix and is *net-harmful* at 305 (short-baseline
wrong-integer rate 0.0% -> 12.5%). A blanket "turn on the good ones" would have caused harm. Read the
regime before arming anything in Tier 2.

**At runtime the loader says this out loud.** `GnssConfig::logNetworkPosture_` emits a POSTURE block
once per run: the detected regime, a `POSTURE OFF [ON|SCOPED] <knob> — <why>` line for every
recommended knob that is unarmed *and whose regime matches this run*, per-bundle integrity, per-tier
armed counts. Log-only. It covers **all 88 reportable recommended knobs** (the other 4 of the 92 are
structurally unreportable with a recorded reason — see `--verify-loader`). ON-tier gaps log at WARN;
SCOPED gaps at INFO, because 40+ equal-weight warnings would drown the tier that has no known
counter-scope.

**Coupled bundles are first-class** — several options are only correct together, so a partial bundle
WARNs:

| bundle | members | why they travel together |
|---|---|---|
| MW/WL lane stack | `cold_clock_seed`, `carry_invariant`, `phase_datum_eq34`, `mw_wl.ewl_enable` | lane rows are built in the CORRECTION frame, so an unseeded cold receiver clock lands in the ambiguity seeds and `lane_max_abs_xlin_m` quarantines the cycles (x_lin **269 km** measured at 305 sta) |
| zero-q phase memory + slip response | `cycle_slip.mw_detect`, `cycle_slip.if_phase_td` | standing invariant #1 — a stiff carry with no jump response warps the whole solution |
| armed QC set | `raim_attribution`, `raim_within_epoch`, `clock_runaway_check` | RAIM directive; withhold is the backstop, adapt-and-emit is the goal |
| SH-VTEC / W6 slice | `ionosphere_weighting.enabled`, `elevation_cutoff_deg ≥ 25`, `estimate_sat_code_bias` | the SH field eats a per-epoch CODE slant iono, so the 25° cutoff is load-bearing and is a SEPARATE gate from `processing.elevation_mask_deg` |

**Regime predicates are machine-readable.** Every recommended knob carries a `regime` expression in
the JSON — the same condition the loader evaluates as its `applies` field — over a fixed vocabulary:
`n_sta` / `n_sys` (station, constellation count) · `tri` (`frequency_mode` triple or all) · `warm` (a
`state_snapshot_path` is set) · `lane` (`mw_wl.enable` / `mw_wl.ewl_enable` / `lane_carry.enable`) ·
`rt` (real-time `app_mode` **or** `network.service_mode` — service mode *is* the RT producer) ·
`par` / `pstations` (the per-station parallel family engages / is merely enabled) · `gps_only`,
`gps_gal_only` (constellation set by NAME — a *count* admits GPS+BeiDou) · `spacing_s`
(`gnss_rate_s × epoch_decimation_n`, the config-determinable epoch spacing) · `on("network.x")`
(knob-on-knob dependency) · `val("network.x", default)` (the knob's **value**, with its C++ code
default named explicitly). A knob whose regime does not match is **suppressed, not reported** — that
is what stops the checker recommending the real-time-only `apriori_source = brdc_rts` against a
post-processed 305-station run. Knobs whose recommended state is a *value* rather than a boolean
(thresholds, counts, `apriori_source`) additionally carry `armed_when`, so a threshold sitting at its
non-recommended default is not miscounted as armed.

**A recommendation is not always actionable: the CHAIN axis.** **17 of the reportable recommended
knobs** are members of the warm-resume **fingerprint** (the `add()` set in `NetworkRunContext.cpp`
`networkSnapshotFingerprint` — the window spanned `NetworkRunContext.cpp:180-315` when last pinned;
grep the symbol, it drifts), so on a *resumed leg* arming one does not take effect mid-chain — with
`state_snapshot_require` the leg hard-aborts, and with it **off the leg silently cold-starts and
reads as a clean resume** (standing invariant #4). Those knobs carry `fingerprinted: true`, and on a
warm config both the loader and the checker append a `CHAIN:` line telling you to arm it in a **fresh
lineage (new seed)** instead. The 17: `apriori_source`, `carry_conditional_coupling`,
`carry_coupling_time_update`, `carry_invariant`, `clock_datum_sum_to_zero`,
`code_bias_disc_jump_test`, `code_datum_time_linked`, `emit_clock_disc`, `emit_generation_id`,
`emit_ura`, `epoch_decimation_n`, `estimate_iono_sh`, `estimate_sat_code_bias`, `phase_datum_eq34`,
`phase_datum_prioritized`, `phase_leveling`, `phase_leveling_max_gap_s`. Two further knobs carry
`fingerprinted: true` without being recommended, so they are outside that count but obey the same
chain rule: `carry_vce_apply` (KEEP_OFF) and `mdb` (UNVALIDATED). Verified **not** members despite
looking like they should be: `state_snapshot_carry` (appears in that file only in a comment),
`global_vtec_sh.max_degree` (only `layer2_max_degree` is fingerprinted), and the whole `raim*` /
`emit_pb_filter_gate_v2` / `disc_threshold_frac_cycles` / `carry_invariant_qz` /
`carry_invariant_innov_*` group — all safe to arm mid-chain.

> ⚠️ **`on()` is a truthiness test and returns false for an ABSENT key, so it INVERTS on a knob whose
> loader default is `true`.** Not hypothetical: it silently suppressed the `emit_pb_filter_gate_v2`
> recommendation (via default-true `carry_invariant_emit_rebase`) — the one knob this ledger calls
> the live promotion gap — and it inverted all five `parallel_*` predicates via default-true
> `parallel_stations`. Use `val("network.x", True)` for those. `--verify-loader` now scans the loader
> for default-`true` `jval<bool>` sites and fails on any `on()` reference to one.

**Check any config offline:** `python scripts/config_posture_check.py runs/guards_cont/leg.json --tier
all`. Add `--show-suppressed` to see what the regime filter held back and why, `--no-regime` for the
old unfiltered behaviour, and `--verify-loader` to cross-check that every recommended knob here is
named in `GnssConfig::logNetworkPosture_` (the drift guard: a knob added here but not there is
invisible at run time). Machine-readable source: `docs/CONFIG_promotion_ledger.json`. Built
2026-07-29 by a 6-family audit of `GnssConfigLoadNetwork.cpp` + `GnssConfigTypes.hpp` + the field
record, each family independently adversarially verified.

**Census — re-derived from the JSON (the machine authority) 2026-08-15; re-derive it, never hand-edit
it.** JSON: **146 entries / 145 unique knobs** (`network.clock_datum_sum_to_zero` appears twice — see
its entry): **20 RECOMMEND_ON, 55 RECOMMEND_SCOPED, 36 UNVALIDATED, 35 KEEP_OFF**; 75 recommended, 3
structurally exempt from the POSTURE block ⇒ **72 reportable, 71 named in `logNetworkPosture_`**.
`--verify-loader` reports exactly 3 live coverage gaps: `network.clock_datum_sum_to_zero` reads
MISSING-from-loader (artefact of the duplicate row — one of the two carries no recommended verdict),
`network.trace_phase_anchor` is a loader POSTURE ROW with **no ledger entry** (so the script's tally
reads one short of the run's), and one `armed_when` predicate (`> 0`) was a syntax error
(✅ fixed 2026-08-15, see below). **The prose
figures this paragraph used to carry ("137 knobs: 20 / 48 / 33 / 36", "68 of 71 reportable") were
stale in three of four tier totals — that drift is the argument for deriving the census.**

⚠ **The .md and the .json have also drifted in MEMBERSHIP, three ways.** This file carries **143
`###` entries (19 ON / 55 SCOPED / 34 UNVALIDATED / 35 KEEP_OFF)** — re-counted 2026-08-15 after the
duplicate `clock_datum_sum_to_zero` row was resolved into one Tier-2 entry (143 / 36 KEEP_OFF before)
and the new `network.trace_raim_redundancy` Tier-2 entry was added. The JSON carries **147** knob
objects (145 after the duplicate was dropped, +2 for the newly added `serialize_sbasis_contract` /
`emit_epoch_status_record`).
(1) Five JSON knobs had **no `###` entry here** — `trace_prefit_census`, `trace_topology`,
`rhs_attrib_on_trip`, `prefit_fuse_sigma` (the Aug-1 delta below, named in prose but never given
entries), `trace_raim_redundancy`. ✅ **`trace_raim_redundancy` was written up 2026-08-15 (serial
cleanup pass) as a Tier-2 entry, transcribed from its JSON object** — the `SPEC_t3_null_derivation`
§14.2 assertion that this correction landed is now true in both files. **Four still open.**
(2) Two `###` entries have **no JSON object** —
`emit_clock_frame_hold`, `clock_disc_jump_test` (the Aug-9 Card CLK-R pair). (3) **NEW and most
consequential: seven knobs are fully specified in §A2 (defaults, fingerprint status, evidence,
promotion disposition) with NO `###` tier entry and NO JSON object** — `rx_cb_harvest_demean`,
`rx_cb_harvest_screen_z`, `h_sigma0_prefit_census`, `h_sigma0_classes`, `serialize_sbasis_contract`,
`emit_epoch_status_record`, `network.mark_refsat_reset_disc`; two of them
(`serialize_sbasis_contract`, `emit_epoch_status_record`) are recorded in §A2 F16 as **PROMOTED and
APPLIED 2026-08-08 into `_golden63_gps_1h_laneoff.json` / `_golden305_gps.json` and every
descendant**, so knobs that have SHIPPED into the golden lineage are invisible to
`config_posture_check.py` and to `logNetworkPosture_` — precisely the promotion gap this ledger exists
to close. The JSON wins at run time. ✅ **PARTIALLY CLOSED 2026-08-15 (serial cleanup pass):
`network.serialize_sbasis_contract` and `network.emit_epoch_status_record` now have JSON objects**
(verdict `RECOMMEND_ON`, family EMISSION and PRODUCT, evidence quoted from §A2 F16), so the two knobs
that actually shipped are no longer invisible to the posture checker. **Still OPEN:** the other five
(`rx_cb_harvest_demean`, `rx_cb_harvest_screen_z`, `h_sigma0_prefit_census`, `h_sigma0_classes`,
`network.mark_refsat_reset_disc`) still have neither a `###` entry nor a JSON object, and none of the
seven has a `###` tier entry yet. ✅ **Also closed 2026-08-15: the one malformed `armed_when`
predicate** — `network.prefit_fuse_sigma` carried the bare, unparseable `"> 0"`; it now reads
`val("network.prefit_fuse_sigma", 0.0) > 0.0`, matching the form every other predicate uses
(loader default verified `0.0`). **Still OPEN:** `network.trace_phase_anchor` is a loader POSTURE
ROW with no ledger entry.

⚠ **One census number is knowingly wrong, deliberately.** `network.carry_invariant_pd_safe` sits
physically in Tier 3 under a ⚠ TIER-LABEL-IS-STALE banner (to keep `###` order in step with the JSON)
while its own entry documents P0 md5-identity PASS, PD gate PASS on every arm, and the 720-epoch P6
leg. The `19/54/34/35` split therefore counts a knob the document itself says is field-validated as
UNVALIDATED. Not a contradiction.

**Aug-1 delta** — the prefit-census card (`docs/SPEC_prefit_census_observability.md`):
`trace_prefit_census` + `trace_topology` promoted to ON, `rhs_attrib_on_trip` to SCOPED (memory, not
accuracy), `prefit_fuse_sigma` recorded KEEP_OFF (a validated guard held back for want of total_IF
grading and a second event, per user directive). **Aug-3 delta** — the PD-safe fold card
(`docs/SPEC_pd_safe_dd_fold.md`): `carry_invariant_pd_safe`, DEFECT measured (99.2 % of the carried
rows sit in blocks that fail the `scripts/parse_carry_psd.py` PD gate at 3 h); **its "no field arm has
been run" status is STALE as of 2026-08-03** — the field ladder ran on the pd_safe substrate that same
day and the gate passed (its Tier-3 entry; `docs/RUNQUEUE_pdfix_reask.md` P0-P6). Its INC-0
observability half is always-on and is not a knob.

**Campaign evidence-of-record.** The two overnight campaign reports that supplied much of the
per-knob evidence below are appendices here: **§A1** (NIGHTRUN 2026-07-15), **§A2** (overnight
2026-08-07/08), **§A3** the campaign-anchor → knob-entry index. Cite as
`CONFIG_promotion_ledger.md §A1 R<n>` / `§A2 F<n>`.

> **Not to be confused with `docs/CONFIG.md`**, the user-facing "which knob does what" reference for
> scenario configs (`data_source`, `processing.mode`, `baseline_fixture`, sat-bias strategies,
> `filter.*`, logging). This file is the **network-producer default-OFF promotion audit** covering
> `network.*` plus `estimation.global_vtec_sh.*` / `estimation.ionosphere_weighting.*`. Zero content
> overlap; adjacent filenames only.

