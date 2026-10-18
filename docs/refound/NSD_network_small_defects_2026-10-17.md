# NSD: three small network defects

**STATUS**

- **id:** NSD
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-10-17 on branch `fix/netsmall`. (1) The hard clock refloat (`clock_monitor_reinit_sigma_m` <= 0) is counted in the response registry by three new sites (`clk_hard_refloat`, `pb_hard_refloat`, `cb_hard_refloat`). (2) The stash-status mirrors (the carry LS-VCE monitor snapshot and the prior-residual census snapshot) skip every parked entry, a readmit seed the D2 fold applied included: confirmed in code, counted at run end by a new READMIT line, and taken as rows behind the default-off key `network.readmit_seed_mirrors`. (3) The de-commoned clock step of the coupled-mark census takes one median per constellation. Items 1 and 3 are measure-only and carry no key; item 2's key is unarmed. The same-build check (CHECK) is owed.
- **source:** `docs/status/OPEN_ITEMS.md` rows "Hard clock refloat uncounted", "Stash-status mirrors (suspected)" and "Mixed-system clock step"; [PRM3](PRM3_responses_2026-10-02.md) section 5; `docs/cards/queues/CAF-COMMENT-AUDIT-FINDINGS.md` sections 4 and 5
- **knobs / posture:** `network.readmit_seed_mirrors` (ledger row UNVALIDATED, default false, not fingerprinted, unarmed in both goldens); no key for items 1 and 3
- **dates in the text:** 2026-10-17
- **hashes / run dirs:** branch `fix/netsmall` (worktree `D:/GnssProcessorSupport/worktrees/netsmall`); tests `tests/network/test_net_small_defects.cpp` (suites `PbHoldDecommonedStepTest`, `StashHardRefloatTest`, `StashStatusSeedMirrorTest`); smoke output `D:/GnssProcessorSupport/gates/netsmall/`

**Outcome.** Question: are three small open defects of the network producer real, and can each be fixed without
moving a number? All three are real in the code. The hard refloat left out three kinds of border entry with no count;
now each is a site of the response table. The two stash-status mirrors never saw a readmit seed although the fold
applies it; a counter states how many, and one default-off key takes them as rows. The coupled-mark census read one
median across GPS and Galileo where the hold's own transfer is per constellation; it is per constellation now. Every
output difference with the key off is declared line by line in section 4. The same-build check is owed
([section 6](#6-what-remains)).

## 1. Hard clock refloat uncounted

**Confirmed.** `networkStashCarry` (`src/core/network/state/BorderStash.cpp`) holds `full_refloat = !(clk_mon_reinit_sig > 0)`.
For a satellite in `clock_refloat_sats` it then skips the stash of the satellite's clock (off the clock model), of every
phase bias it holds (off `carry_invariant`) and of every code bias, at three `continue` sites. The next epoch's fold finds
no prior for those columns, so each starts diffuse; the READMIT park does not take the entry (its column is present).
The response table (`network/assembly/Responses.cpp`) had a row for the soft form (`clk_reinit`, `pb_reinit`,
`cb_reinit`) and none for this one ([PRM3](PRM3_responses_2026-10-02.md) section 5: still uncounted).

**Fix.** Three sites of `ResponseSite`: `ClkHardRefloat`, `PbHardRefloat`, `CbHardRefloat`, each event slip, policy
`erase_and_rebirth`, count source an increment at the stash. A site counts an entry only when the stash would otherwise
have kept it: a datum-pinned column, a column without a positive variance and a failed column lookup count nothing. The
arming gates are those of the soft reinit with `!reinitSoft` in place of `reinitSoft` (the clock monitor or the clock
model's fault test as the source of flags, the carried kinds; the code-bias excursion guard has no hard form). A site that
responds on a run where it is not declared armed is the run end's declaration error, as for every other site.
`kResponseSites` goes from 65 to 68.

## 2. Stash-status mirrors

**Confirmed.** `networkStashStatus` (`src/core/network/diag/StashStatus.cpp`) snapshots the folded border priors before the
re-stash, twice: for the carry LS-VCE monitor (`vce_shared_prior`, phase-bias and code-bias families) and for the
prior-residual census (`prires_shared_prior`, plus the clock family). Both skipped every entry with `t_park_s > 0`. A parked
entry whose column returned inside `readmit_seed_horizon_s` is folded by the D2 fold as `Seeded`, at its variance plus q
over the gap since the park (`carryFoldDecide`, `networkInjectD2CarryPrior`), so a seed is a folded prior that neither
mirror ever paired with its re-stashed solved value. Both goldens run READMIT (3900 s), the monitor (`carry_vce`,
components pl, pb, cb) and the census (families pl, pb, cb, zwd, rxcb): the code-bias seeds are the gap there; the clock
family is in neither, so clock seeds are not.

**Count.** Three counters in `BorderCounters`: `n_readmit_mirror_seed_vce_cb`, `n_readmit_mirror_seed_census_cb` and
`n_readmit_mirror_seed_census_clk`, each a seed (a parked entry whose fold record reads `Seeded`) met by that snapshot
where the mirror is armed for the kind. A new run-end line in the READMIT report prints them beside the seeds the fold
applied, with "skipped them" or "took them as rows" by the key. The count is the same with the key off and on.

**Fix, behind a key.** `network.readmit_seed_mirrors` (default false, inert without `readmit_seed_horizon_s`; loader
warnings when armed without it or without `carry_vce` and the census). Armed, an applied seed is a row of the monitor
(code-bias family) and of the census (code-bias and clock families) like any folded prior, with the gap since the park as
its interval and the fold variance formed over that gap (`carryFoldedSeed`, `carryMirrorGapS` in
`network/state/CarryDisposition.hpp`). A seed the fold retired, screened or left parked is still skipped: only an entry
whose fold record reads `Seeded` counts, so the mirror follows the fold's own decision instead of re-deriving it. The key
can change the solution only where the monitor feeds the fold (`network.carry_vce_apply` with the code-bias family); the
census has no feedback. It is not fingerprinted: neither accumulator is part of a state snapshot. Ledger row UNVALIDATED.

## 3. Mixed-system clock step

**Confirmed.** `decommonedClockStep` (`src/core/network/emit/PbHold.cpp`) subtracted one median taken over the steps of
every satellite with a clock at two emissions, GPS and Galileo together. The hold's own transfer takes `M_k` per
constellation (`pbHoldCommonClockStep`, because each constellation's clock common mode moves with its own pivot
handover and frame-hold re-anchor), and the census exists to measure what a coupled frame move would have to transfer.
With three GPS satellites at a common step of 0 and three Galileo satellites at 0.5 m, the single median reads 0.25 m and
leaves -0.25 m on every GPS satellite and +0.25 m on every Galileo satellite, so the coupled-mark census (`pb_hold_coupled_step_m`, the
run-end line "pb hold, coupled (PBH)", the `PB_HOLD coupled_mark` events) mixed the two common modes into every step.

**Fix.** The function is `pbHoldDecommonedClockStep` (declared in `network/emit/PbHold.hpp`) and takes one median per
constellation. For one constellation the population, the median and the result are those of the old function (the same
steps, the same even-count mean), so a GPS-only run is unchanged. The step is measure-only: it feeds the census and the
event, not the emission.

## 4. Declared output differences

With `network.readmit_seed_mirrors` off (the only state of the goldens), the Release output differs from main
`30ca49e` in these lines and no others:

| output | difference | when |
| --- | --- | --- |
| run end, `Network (param responses, run end): ... N of 65 declared site(s) armed on this run, M not armed and quiet; E declaration error(s).` | 65 becomes 68; M gains 3 | every network run (the new sites are unarmed and quiet unless the hard refloat is armed) |
| run manifest, record `param_responses`, array `sites` | three objects added (`clk_hard_refloat` after `clk_reinit`, `cb_hard_refloat` after `cb_reinit`, `pb_hard_refloat` after `pb_reinit`), each `armed:false,count:0` | every network run |
| run end, per-site rows `Network (param responses, run end): ... clk_hard_refloat` (and the pb and cb rows) | new rows | only with `clock_monitor` on and `clock_monitor_reinit_sigma_m` <= 0 |
| run start, `Network (param responses)` table and the manifest `param_model` policy masks | `erase_and_rebirth` added to the slip event of `sat_clock`, `sat_phase_bias`, `sat_code_bias` | only with the same posture |
| run end, `Network (READMIT seed mirrors): of the N clock + M code-bias seed(s) the D2 fold applied, ...` | one new line after `Network (READMIT continuity)` | every run with `readmit_seed_horizon_s` > 0 (both goldens) |
| run end, `pb hold, coupled (PBH)`: p50, p90, max of the de-commoned step | values change | only with `emit_pb_hold` on, two or more constellations and a coupled mark reaching a held key outside the phase-clock transfer |
| events trace, `PB_HOLD coupled_mark` rows | the value column changes at those epochs | the same |

No wire, product, solution or other trace file changes. With the key on, the monitor's and the census's rows, and so
their run-end lines and traces, change by the seed rows.

## 5. Gate (2026-10-18)

- **Structure.** `python scripts/nom_epoch_lint.py` and `python scripts/nom_n4_verify.py --whole` pass;
  `python scripts/make_golden_config.py --verify` passes (76 recommended knobs armed in regime on each golden, 0 failures);
  `python scripts/config_posture_check.py --verify-loader` reports 0 coverage gaps; `python scripts/docs_index_check.py`
  passes. `scripts/counters_split_check.py` reports its four pre-existing "not in the old struct" members and, with them,
  the three counters of section 2 (a tool pinned to the counters split; it fails on main for the same reason).
- **Unit suite.** Debug build of `gnss_core` and `gnss_tests` in the worktree is green. The full suite on the build before
  the last two additions to the new test file (a key-default case and the seed-gap variance assertions) ran 3349 cases in
  452 suites: 3319 passed, 30 skipped, 0 failed; the skips are the fixture-gated cases (the worktree's `data/` is
  empty). The new file's 12 cases pass in an executable linked from the final object (`tests/network/test_net_small_defects.cpp`:
  3 in `PbHoldDecommonedStepTest`, 5 in `StashHardRefloatTest`, 4 in `StashStatusSeedMirrorTest`).
  The cases are sensitive to the defects: the per-constellation test reads -0.1, 0.1, 0.0 m on the three GPS satellites and 0.0 m
  on the Galileo ones where one median across the six would read -0.3, -0.1, -0.2 and +0.1 m; the hard-refloat cases count 1, 1, 1 where the table had no site; the seed case
  reads a fold variance of 0.07 m^2 for the seed (gap 600 s) against 0.013 m^2 for the live prior (step 30 s).
- **Smoke.** Release `gnss_processor_netsmall.exe` (built from the worktree, 11 epochs on each golden, arms
  `_rap_nsd_smoke_sim` and `_rap_nsd_smoke_field` under `D:/GnssProcessorSupport/runs_nom`): both exit 0; the run end reads
  `32 of 68 declared site(s) armed on this run, 36 not armed and quiet; 0 declaration error(s)` on both beds; the new READMIT
  line prints. The sim bed applied no seed in 11 epochs. The field bed applied 1 code-bias seed, which the monitor's snapshot
  and the census snapshot each met and skipped (`network.readmit_seed_mirrors` off): the gap is live on the field golden, not
  only in the code. The hold's coupled-mark line reads 0 samples on both (no coupled mark reached a held key in 11 epochs), so
  item 3 is exercised by the unit cases and not by the smoke.

## 6. What remains

Owed to the check stage: the same-build golden comparison (field and sim, 2 h, against a control built from main
`30ca49e`) confirming that only the lines of section 4 differ, and the seed counts of section 2 on a 6 h field golden arm
(how many code-bias seeds the monitor and the census skip over the run). Arming `network.readmit_seed_mirrors` is not
proposed: it needs a graded arm with `carry_vce_apply` on the code-bias family, and the census effect is a trace.
