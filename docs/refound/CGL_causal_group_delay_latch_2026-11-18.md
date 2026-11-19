# CGL: a broadcast group-delay latch without look-ahead (2026-11-18)

**STATUS**

- **id:** CGL
- **topic:** code-bias
- **status:** open
- **summary:** A design for rule change 11 of the chain audit (part 3): a key `network.sat_gf_latch`, default off, under which each satellite's broadcast group delay is taken from the navigation records available by transmission time at its first use and held, so that a short run is the exact beginning of a longer one and a stream has a defined value. It inventories every read past the served epoch in the network engine (four inside the latch, plus the chain restore of step S7 (b), which at `7fb0737` already saves the latch with its window in the chain memory lines); the held value therefore extends that saved line instead of adding a second owner. Nothing was built or run.
- **source:** plan package 5 of the program of 2026-11-18 (a design), "Causal broadcast group-delay latch, with an inventory of every look-ahead read"; drafted at `03dfcbc` of branch `chain/audit` and re-read at `7fb0737` for network code and docs (step S7 (b) landed there), main `5ddc422` for user-engine code; `docs/refound/CHA_chain_audit_2026-11-18_part3.md` (rule change 11); `docs/refound/NRF_network_ring_fixing_2026-11-14_part18.md` (sections 123 to 125); two navigation files of DOY 146 and 147 read by scratch scripts (file reads, no engine run). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The latch reads ahead in four ways: the window median, a whole-file health choice, a whole-file Galileo F/NAV choice and an "earliest after" fallback; a resumed run also inherits the seed's window through the chain restore. The chosen rule holds each satellite's value from the latest healthy record available at its first use, availability being the transmission time. A later broadcast step is reported, never followed. The held value has to extend the `GD` line of the chain memory section, not add a second owner. On DOY 146 the day run would take the short-window Galileo values, which section 124 saw in a GPS failure only with two other conditions absent here; a 5 h check needing no code tests that precaution first.

Read at commit `03dfcbc`. Every `path:line` below is at that commit. Files with uncommitted edits in the worktree were read with `git show 03dfcbc:<path>`.

## 1. The latch today

- The window: `t0` = first synced epoch + `start_offset_hours`; `t1` = `t0` + `max_duration_s`, or the last synced epoch + 1 s when no duration is set (`src/core/network/driver/ApplicationNetworkSetup.cpp:357-361`). `t1` is always finite.
- The latch: `NetworkSbasis::latchBroadcastGroupDelays` (`src/core/network/solve/Sbasis.cpp:121-199`), called once at `ApplicationNetworkSetup.cpp:710-711`; the value goes to `setup.sat_gf_target_m` (`ApplicationNetworkSetup.cpp:725`) and is constant for the run.
- [corrected 2026-11-19: see Corrections, item 4] Its only reader is the code-bias datum builder (`src/core/network/solve/CodeBiasDatum.cpp:396`, passed on to `Sbasis.cpp:1565`); the tests are the only other users.
- [corrected 2026-11-19: see Corrections, item 10] Under the time link (`network.sat_gf_datum_time_linked`, armed in both reference configurations, `docs/CONFIG_promotion_ledger.json:223`) the absolute target enters the solve only when a satellite is anchored: it is saved as `abs_target` (`Sbasis.cpp:1590`), replaced by the carried `(p2 - p1)/(mu2 - mu1)` when both previous code biases exist (`Sbasis.cpp:1599-1603`), and asserted otherwise, at a first sighting or a return (`Sbasis.cpp:1604-1614`, counted by `n_sat_gf_reanchored`); the extra-channel anchor reads the same `abs_target` (`Sbasis.cpp:1806-1810`). A change of the latched value during continuous tracking therefore never reaches the solve.
- The four reads ahead inside the latch:
  - (a) the low median over records whose toc lies in `[t0, t1)` (`Sbasis.cpp:156-167`);
  - (b) the health pool over the whole file: a satellite with any healthy record anywhere drops its unhealthy ones (`Sbasis.cpp:136-141`);
  - (c) Galileo F/NAV preferred whenever any F/NAV record exists anywhere in the file (`Sbasis.cpp:143-153`);
  - (d) the fallback "the latest at or before `t0`, else the earliest after it" (`Sbasis.cpp:170-179`); its second branch reads ahead.
- The record's effect (NRF section 124, item 1): over 24 h against 5 h on DOY 146, 22 Galileo satellites latch differently by +0.0698 to +0.4886 m, and the first epoch's code-bias records differ by minus that on the E1 codes and minus 1.793 times it on the E5a codes (E04: -0.4188 and -0.7510 m). The configured length, not the number of epochs processed, sets the first epoch.

## 2. Inventory of reads past the served epoch

[corrected 2026-11-19: see Corrections, item 5] [corrected 2026-11-19: see Corrections, item 6] The inventory of the first stage, with the review's amendments merged (rows 2, 3 and 13 and the per-epoch finding). The review's open point on loader defaults is settled here: `network.sat_health` defaults to `off` (`src/core/config/GnssConfigLoadNetwork.cpp:9752-9753`), `network.pivot_election` to `off` (`:9778-9779`), both coverage fractions to 0.0 (`:6831`, `:6833`), and `network.apriori_source` to `sp3` (`:4354-4355`). Of the rows below only the latch is armed in the reference configurations.

| # | Read (path:line) | Future data used | Legitimate in post-processing | Blocks an endless stream | Causal form needed |
|---|---|---|---|---|---|
| 1 | Window `[t0, t1)`, `ApplicationNetworkSetup.cpp:352-361` | Without a duration, the last synced epoch | Yes | Yes: every window reader inherits a finite end taken from files | Drop `t1`; readers use records available at `t` |
| 2 | Group-delay latch, `Sbasis.cpp:121-199`, called at `ApplicationNetworkSetup.cpp:710-711` | Up to `max_duration_s` of navigation records (24 h on day runs): window median, whole-file health and F/NAV choices, earliest record after `t0` | As a fixed datum yes, but a short run then differs from a prefix of a long one (up to 0.49 m on E1, 0.88 m on E5a, DOY 146) | Yes | This document: value from the record available at first use, availability by transmission time (`has_transmission_time`, `src/core/products/RinexNavParser.cpp:272-281`), toc only where no transmission time is parsed; health and F/NAV choices over available records only; held |
| 3 | Broadcast health series, `Sbasis.cpp:202-210`, `:228-280`; exclusion at `ApplicationNetworkSetup.cpp:432-449`; window at `:394-417` | Records take force at their toc: about 664 s early for Galileo (review, amended). The first record's status also holds before its time (`Sbasis.cpp:209`, `:267`). The window enters only the log and an in-window count | Yes | Yes for Galileo timing; in a stream the series is the startup file's and freezes at its last record (inferred from `:209`) | Status "unknown" before the first available record; availability by transmission time; fed from live ephemerides |
| 4 | Broadcast-clock a priori (`sp3_brdc_clock`), `ApplicationNetworkSetup.cpp:602-607`; `src/core/products/BrdcClockApriori.cpp:110-112`, `:129-168`, `:177-199` | No future content (records by transmission time, `:110`, `:202`); the table is built ahead to `end_s` near `t1`; past it the node is clamped and the last record extrapolated with no fit-window check (`:184-196`, inferred) | Yes | Yes past `t1` | Append a segment as each record's transmission time passes; remove `end_s` |
| 5 | S contract a-priori identity, `ApplicationNetworkSetup.cpp:1700-1730`; `aprioriIdentityForWindow`, `src/core/products/NetworkProductIO.cpp:1406` | Names the product files in force over `[t0, t1)` | Yes (a label) | Mislabels a stream | Name the source per generation (brdc_rts already names the navigation source, `ApplicationNetworkSetup.cpp:1719-1723`) |
| 6 | Precise products: previous, current and next day merged, `src/core/products/ProductResolver.cpp:733-740`; SP3 11-node interpolation, `src/core/model/ModelGeometry.cpp:1007`; CLK bracket or 9-point Lagrange, `src/core/corrections/GnssCorrections.cpp:428`, `:568`, `:762` | Whole-day final estimates; interpolation reads nodes after `t` | Yes | Yes in `sp3` mode | brdc_rts (exists) or predicted products; the phase-bias hold still needs a precise clock, so it is not stream-ready |
| 7 | ERP interpolation, `GnssCorrections.cpp:1020-1051` | The next ERP sample | Yes | Minor | Predicted ERP |
| 8 | Signal coverage gates (`signal_min_station_fraction`, `phase_bias_min_station_fraction`), `ApplicationNetworkSetup.cpp:792-860` | Every synced epoch of the run for a run-constant allowlist | Yes | Yes: under streaming it tallies the metadata files and nothing refuses it | A running tally with admission events at both edges, or a declared static list |
| 9 | Code-bias admission tally (`cb_admission`), `src/core/network/assembly/BiasAdmission.cpp:42-55`, `:289-335` | Epochs `[i0, i0 + K)` after the first processed epoch, latched for the run | Yes | Refused under streaming, which falls back to the per-epoch gate (`:314-319`) | A verdict at `i0 + K` with the pair held out until then, or the per-epoch gate |
| 10 | Pivot coverage predictor (`pivot_election`), `src/core/network/solve/PivotElection.cpp:243-300`; horizon default 7200 s, `GnssConfigLoadNetwork.cpp:9788-9790` | SP3 orbit nodes up to 2 h ahead | Yes (geometry ahead is predictable) | With broadcast orbits it falls back to the lexical first satellite (`PivotElection.cpp:297-300`) | Propagate the broadcast ephemeris in force at `t` (a prediction, not a read ahead) |
| 11 | RINEX clip, `ApplicationNetworkSetup.cpp:284-307`; span, `src/core/rinex/MultiRinexSync.cpp:191` | Parses each file to start offset + duration + 60 s; its span is what rows 1, 8, 9 and 12 read | Yes | Its span feeds stream setup as if it were live | None once rows 1, 8 and 12 stop reading the synced span |
| 12 | Warm-resume age reference, `src/core/network/state/WarmResume.cpp:154-166` | Not a read ahead | Yes | Under streaming the age is measured against the metadata file's first epoch (inferred) | Measure against the first epoch the solve processes |
| 13 | Start-offset fast-forward, `src/core/network/driver/EpochAdmission.cpp:35-50`; replicated at `BiasAdmission.cpp:322-330`; and the brdc_rts feed, `EpochAdmission.cpp:99-106` | Fast-forward: none. The brdc_rts feed releases records by `navAvailabilityGpsSeconds` = toc (`include/network/driver/SetupResult.hpp:53`): about 664 s early for Galileo, about 2 h late for GPS (review, amended) | Yes | The feed: yes for Galileo timing | Fast-forward already causal. The feed needs availability by transmission time under its own key: changing the shared O3 rule moves brdc_rts output |
| 14 | Legacy GIM, `ApplicationNetworkSetup.cpp:776-789` | A daily IONEX file interpolated in time | Yes | Refused under brdc_rts (`:770-775`) | Not needed (`w6_iono_constraint: none` in the reference configurations) |

**The per-epoch spine.** The first stage found that it reads nothing ahead; the review refuted that for Galileo (row 13). The max-duration stop (`EpochAdmission.cpp:117-123`) and the broadcast-clock store feed by transmission time (`BrdcClockApriori.cpp:201-205`) are causal. The network has no backward pass (the RTS smoother is user-side only). A pattern grep of `src/core/network` found no other read ahead in per-epoch code; that rests on the grep only (inferred).

**The Galileo timing (measured).** In `BRDC00IGS_R_20261470000_01D_MN.rnx`, Galileo transmission time minus toc has a median of +664 s over 11 535 records, 99.92 % positive; GPS has a median of -7 182 s. Measured by the first stage and reproduced independently by the review. A rule keyed on toc therefore uses a Galileo record about 11 minutes before it is broadcast and a GPS record about 2 h after.

**What an endless stream gets today.** With `network.streaming.enable` (`src/core/network/driver/ApplicationNetwork.cpp:328-345`) only the epoch iteration changes. The setup still parses the configured RINEX files for station metadata (`include/network/driver/StreamFrontEnd.hpp:34-37`), synchronizes them and builds `[t0, t1)` from them, so the latch applies its window rule over the metadata files' span, from the navigation file on disk, and never reads the ephemerides pushed into `LiveEphemerisStore`. No loader check found ties streaming to a duration or to the live day.

## 3. The chosen rule

Two variants were designed: A, hold each satellite's value at first use; B, follow every broadcast change as a declared event. The first stage recommended A; the review confirmed A with four corrections and rejected B as the active rule. The rule below is A with the corrections.

**Rule (key value `first_use`).**
1. *Availability.* A navigation record is available at its transmission time when the parser set `has_transmission_time` (`RinexNavParser.cpp:272-281`), else at its toc (BeiDou shifted to the GPS scale as `brdcTocGpsSeconds` does). This replaces the toc rule only inside the new rule; the shared `navAvailabilityGpsSeconds` is not touched (correction 1 of the review: variant A as first designed was keyed on toc, which is an 11-minute lead for Galileo, and its claim that toc is the conservative choice is the wrong way round for Galileo).
2. [corrected 2026-11-19: see Corrections, item 9] *First use* `t_u` of a satellite: the first kept epoch at which the satellite has a registered code-bias column in `networkCodeBiasDatum` (`registered_code_keys`, `CodeBiasDatum.cpp:325-326`). Kept epochs only, so a withheld or skipped epoch does not move `t_u` (correction 3). Whether the seam runs on skipped epochs is open (section 11).
3. *Pool.* The satellite's records available at or before `t_u`.
4. *Health.* The latest healthy record (health 0) in the pool; if the pool has none, the latest record of any health (reason `InForceUnhealthy`). This replaces the whole-file "all unhealthy" test.
5. *Galileo source.* The latest available F/NAV record (the data-source test of `Sbasis.cpp:143-151`); I/NAV only when no F/NAV record is available yet (reason `InavFallback`, counted). An F/NAV record arriving later does not change a held value.
6. *No median.* The median existed to pick the value in force for most of the window, which needs future records. A single record's value is taken as broadcast.
7. *Zero.* A broadcast 0 is kept as a real value (`satGfBroadcastTarget` still classes it `BroadcastZero`, `Sbasis.cpp:112-115`).
8. *Representation.* Kept: a held value within `kSameBroadcastGroupDelayS` (1 ps) of the satellite's first record in the file takes that record's bits (`Sbasis.cpp:188`). The 1 ps comparison reads the first record, which is a file property, not a read past `t_u`; it only selects between two representations of one value.
9. *Held.* Written once, never updated: a return after a gap, a cut of the time link and a new extra channel all read the held value.
10. *No record available at `t_u`.* Post-processing and stream are treated differently (correction 2):
    - Post-processing: by default the earliest record after `t_u` (reason `AfterFirstUse`, its lag in seconds counted and listed). It depends on the file, not on `t1`, so the prefix property holds.
    - Stream: no fallback reads ahead. The satellite's code columns are held out until a record is available, as a declared admission event (section 4). Today's `Missing` would anchor at zero (`Sbasis.cpp:108-110`) and the time link would carry that zero for the rest of the run; variant B's "the rows wait" leaves rank-deficiency direction 3b open (the comment at `Sbasis.cpp:1622-1627`) unless the columns are held out too.
    - [corrected 2026-11-19: see Corrections, item 7] At a day start the transmission-time rule leaves Galileo with no available record in that day's file for about 11 minutes (inferred from the +664 s median and a file whose first Galileo toc is 00:00, not checked). The previous day's navigation file, the toc fallback or the hold-out covers it; the choice is a decision for the author.

**Why not variant A0 (latch every satellite at `t0`).** Prefix-safe in post-processing, but a satellite rising hours later takes a stale value, and in a stream the live store is empty at `t0` (review, item 5). Dropped.

**Why not variant B (follow each broadcast change).** Measured by the first stage on the navigation files (unconfirmed by the review, though consistent with the 22 and 16 changed medians of section 124): on DOY 146 all 30 Galileo satellites step twice, near 05:30 to 06:10 and 17:30 to 18:20 (60 steps of 0.070 to 0.558 m on E1, up to 1.0 m on E5a); on DOY 147, 48 steps on 30 satellites; GPS and BeiDou none; many partly revert (E13 +0.419 m, then -0.419 m). These are most likely twice-daily re-estimates by the ground segment, not hardware changes (inferred). Following them makes about 50 to 60 datum events a day; each would move the satellite's code biases, its slant ionosphere and, through it, the phase side by mu times the step (about 3 E1 cycles at 0.558 m, not an integer), breaking held links. Its ionosphere re-expression is also at risk from the known trap that an `x_lin` shift on a carried column is an exact solve no-op, and its sign and factor were not shown. Its lazy form (adopt at a return) only changes a returning satellite's frame and gains nothing over A. The review recommends keeping `running` as a reserved name; this document does not add an unbuilt value to the loader (section 6), which is a minor disagreement for the author.

**Invariant 1.** The geometry-free level per satellite is a datum: any constant serves, and users apply code biases and ionosphere together. A broadcast step re-states the control segment's estimate; it is reported, not followed. Neither variant answers invariant 1 for the real near-constant here, a true jump of a satellite's geometry-free code bias (a payload switch, for example): the time link carries that level with zero process noise (`Sbasis.cpp:1599-1603`) and re-seeds only at a return, so a jump during continuous tracking would be absorbed by the ionosphere unseen. That gap exists today, is not introduced by this rule, and needs a data detector (one common geometry-free code step on every line of sight of one satellite), owed separately.

## 4. Events at both edges

1. **The hold edge** (one per satellite). Not a coupling change in the solve: it fixes the value that the existing first-sighting anchor asserts. Declared by one log line and counted by reason.
2. **The wait for a record** (stream; post-processing only if the author chooses hold-out at a day start). This changes which information reaches which states, so it is an event at both edges (invariant 7):
   - Decoupling edge: the first kept epoch at which the satellite has a code column and no available record. Its code columns are held out of the solve (the mechanism to reuse is not chosen: inferred owed detail). Bound: the held-out satellite contributes nothing, so nothing accumulates on its columns. Counted with satellite and epoch.
   - Re-coupling edge: the first epoch a record is available. The value is latched, and the satellite enters as a first sighting, anchored at the held value (`Sbasis.cpp:1604-1606`); no carried state of it exists to re-validate. Counted with the wait in epochs.
3. **A broadcast step after the hold.** Measure-only: an observer compares the value available each epoch with the held value and records the departure (satellite, epoch, c times the step). No response, no edge in the solve.
4. **A return.** Already a re-coupling with its own counter (`n_sat_gf_reanchored`, `Sbasis.cpp:1610-1614`); under the new rule it asserts the held value. The run-end report adds whether that value differed from the one then available.

## 5. Counters

- Run-start line, only when armed: "group-delay latch rule first_use: availability by transmission time (toc where none); N satellites with records; each satellite held at its first use from the records available then; no read-ahead except AfterFirstUse (listed)".
- Per hold: one line with satellite, epoch, value (m), reason, and the toc and availability time of the record used.
- Run end, one row of `kNetworkRunEndReports` (`src/core/network/runend/RunEnd.cpp:115`), per constellation:
  - satellites held by reason: `InForce`, `InForceUnhealthy`, `InavFallback`, `AfterFirstUse` (with the largest lag in s), held at 0, restored from a snapshot;
  - waits for a record: satellites, decoupling and re-coupling edges, total and largest wait in epochs;
  - broadcast steps seen after the hold: satellites, count, largest absolute c times the step, the list;
  - returns that asserted a held value differing from the value then available, beside the existing `n_sat_gf_reanchored`.
- The step monitor is one row of `kNetworkEpochObservers` (`include/network/driver/EpochObservers.hpp`). The hold writes satellite state, so it needs a `net::kEntityWriters` row (`src/core/network/state/Entities.cpp:1352`, check V12 of `scripts/nom_epoch_lint.py`) and a row of `tests/network/test_network_epoch_contract.cpp` (correction 3).

## 6. The key and its ledger row

Key: `network.sat_gf_latch`, an enumeration: `window` (default, today's rule) and `first_use`. Loaded beside `network.sat_gf_datum` and `network.sat_gf_datum_time_linked` (`GnssConfigLoadNetwork.cpp:3700-3703`, `:3724-3727`); the loader refuses `first_use` unless `sat_gf_datum` is `broadcast_tgd`, and refuses any other value. An enumeration leaves room for a future value without a second key. Getter with Doxygen, a `NetworkKnobs` member, the run-start line, and the ledger row in the same commit.

Ledger row text (beside `docs/CONFIG_promotion_ledger.json:222-223`):

```json
{"knob": "network.sat_gf_latch", "verdict": "UNVALIDATED", "area": "SharedStateForcings", "default": "window", "why": "Rule of the broadcast group-delay latch (RF-0c). window: the low median of each satellite's healthy records over the processing window, which reads navigation records up to the run's end. first_use: each satellite's value from the latest healthy record available (by transmission time) at its first use, held for the run; no read-ahead except a counted fallback, so a short run is the exact beginning of a longer one and a stream has a defined value. Fingerprinted; the held values are saved in the snapshot.", "card": "docs/refound/CHA_chain_audit_2026-11-18_part3.md"}
```

## 7. Saved state and fingerprint

- [corrected 2026-11-19: see Corrections, item 1] **One owner.** A new optional member of `net::SatelliteState`, holding the value (s), the hold epoch, the reason and the toc and availability time of the record used; beside `code_bias_seen` (`include/network/state/Entities.hpp:417-420`), with a declaration row of the `Persist::Record` kind under a new record code `GDL`, on the pattern of the `CBS` row (`src/core/network/state/Entities.cpp:574-575`). The setup keeps only the per-satellite record series, constant after setup. Rows are written only when armed.
- **Section 125.** It registers saving "each satellite's latched value and the window it was latched over" (part 18, section 125, line 91). Under `first_use` the window becomes a hold epoch per satellite, so the record needs no window.
- **On resume.** With the chain restore of section 125: satellites in the record keep the uninterrupted run's values, and satellites not yet used at the save epoch hold at their first use in the resumed run by the same rule, so they also equal the uninterrupted run's (inferred, on condition that first use is defined on kept epochs). Without restore: every satellite holds at the resumed run's first use; a run-start line counts the satellites whose value then differs from the record's. This matters even when `code_bias_prev` is restored, because a return re-anchors at the held value (`Sbasis.cpp:1604-1614`).
- **Fingerprint.** No token covers this datum today (the only "gf" hits are the cta tokens; review). An armed-only token `gflatch:first_use` in `SnapshotFingerprint.cpp`, on the pattern of the armed-only tokens at `src/core/network/state/SnapshotFingerprint.cpp:295-305`, so every existing fingerprint is unchanged and a snapshot written under `window` is never restored into a `first_use` run.
- [corrected 2026-11-19: see Corrections, item 2] **The uncommitted snapshot edits.** `StateSnapshotWrite.cpp`, `NetworkStateSnapshot.*`, `Entities.*`, `RunFields.cpp`, `WarmResume.cpp` and `SnapshotFingerprint.cpp` hold half-built edits of the paused step (section 125) in the worktree. They were not read; `GDL` must be reconciled with them.

## 8. Unit tests

A pure function `NetworkSbasis::latchBroadcastGroupDelayAt(records of one satellite, t_use, availability)`, tested in `tests/test_group_delay_latch.cpp` with its `rec()` helper. The eight existing cases (lines 51 to 160) stay unchanged and guard `window`.

1. Prefix: records appended after `t_use`, stepped, leave the result unchanged; the window rule changes on the same input (mirrors `WindowMedianIgnoresRecordsAfterTheRun`, line 51).
2. A Galileo record with toc at or before `t_use` but transmission after it is not used; a GPS record with transmission at or before `t_use` is used.
3. A record available exactly at `t_use` is used; one available 1 s later is not.
4. A record without a parsed transmission time falls back to its toc.
5. An earlier healthy record wins over a later unhealthy one; with only unhealthy records available the reason is `InForceUnhealthy`; a later healthy record is ignored.
6. F/NAV preferred when available; I/NAV fallback when not; an F/NAV record arriving after `t_use` leaves the value unchanged.
7. A held 0 stays exactly 0 and classes as `BroadcastZero`.
8. No record available at `t_use`: post-processing mode gives the earliest after with reason `AfterFirstUse` and its lag; stream mode gives no value (the wait).
9. Representation noise keeps the first record's bits (as at line 138).
10. The BeiDou time shift at the boundary.
11. Holding: after a step, the held value is unchanged and the step monitor records one departure of the right size.
12. A round trip of the `GDL` record; the fingerprint token present only when armed.
13. A satellite that waits: its code columns are held out until the first available record and enter as a first sighting (a network unit case on the support library, if a small fixture allows; otherwise named as owed).

## 9. Implementation steps (each identical in output with the key off)

1. [corrected 2026-11-19: see Corrections, item 11] The pure function, the availability helper and unit cases 1 to 11; no caller.
2. The key, loader, getter, `NetworkKnobs`, the ledger row, the run-start line when armed; the ledger re-render and the configuration generator's verify.
3. The `SatelliteState` member, its declaration and writer rows, the snapshot struct, `GDL` written and read only when armed; `scripts/nom_n4_verify.py`.
4. Setup builds the per-satellite series when armed; the hold at the opening of `networkCodeBiasDatum`; the target map built each epoch from held values instead of `setup.sat_gf_target_m`. The key-off path is not touched.
5. The fingerprint token, the restore and the restore run-start line.
6. The wait (stream hold-out) with its counters at both edges.
7. The step-monitor observer and the run-end report.

The gate after each step: the three 2 h controls identical with the key off (`scripts/nom_td_gate.py --whole --mask-time`), with the entry-check control as in the audit's gate; the chain unit cases; `scripts/nom_epoch_lint.py` and `scripts/nom_n4_verify.py`.

## 10. Validation runs needed later (listed; none requested)

1. **The prefix test, first.** With the key on: a 2 h run equals the first 2 h of a 5 h run, products and every trace with time masked, on DOY 146 in the section 124 configuration (exact epoch time on, then off) and on the sim bed from 00:30:15; then the first 2 h of a 24 h run. If it fails, locate the first difference among the other window readers (rows 3, 4, 5 of the inventory).
2. [corrected 2026-11-19: see Corrections, item 8] **Does the GPS failure of section 124 come into the adopted configuration?** (review). On DOY 146, `first_use` gives every run from 00:00 the first-record Galileo values, which today only the short runs have; with exact epoch time on and the single pair tree, only two of section 124's three conditions would protect the day run, and the record holds no run with all three on DOY 146 (inferred from part 18, sections 123 and 124). First check, needing no new code: a 5 h run of the adopted configuration on DOY 146 under `window` against hours 1 to 5 of the adopted 24 h DOY 146 run of the five-day set (with no step before about 05:30, the 5 h window takes the values `first_use` would hold). Grade: GPS fraction refusals by hour, receivers over 10 %, satellites over 30 %, BOR100POL, and the common ionosphere-free offset of unheld links per satellite.
3. Key off against on, steps 1 to 7: the same-build comparison on the observed-data, sim and BeiDou configurations, 2 h each.
4. The section 124 control configuration (exact epoch time off, per-signal trees) over 24 h, `first_use` against `window`: isolates the latch with the length fixed, which configuration cannot do today.
5. The adopted configuration over 24 h, `first_use` against `window`, DOY 143 to 147, hours 6 to 24 included: Galileo code-bias moves at each satellite's anchor equal c times (held minus window value) on E1 and 1.793 times that on E5a; the held-out user by hour, the 16-site set and GPS fraction refusals by hour within noise; the step monitor counting the 05:30 and 17:30 steps.
6. The resume of section 125 across 05:30 with the `GDL` record: restored values equal the uninterrupted run's, and a satellite first used after the save holds identically in both.
7. A short streamed run (`network.streaming.enable`) with the live navigation feed: waits and their edges only where expected, no `AfterFirstUse`.
8. A sim run with one injected group-delay step in the navigation file (the sim file probably has none): the step monitor sees it and nothing in the solve moves.


## Decisions for the author

1. [corrected 2026-11-19: see Corrections, item 3] Whether to build rule change 11 at all, and before S7 (b) as part 3 proposes.
2. Variant A (`first_use`, hold at first use) as the rule; variant B (follow each broadcast change) not built.
3. Availability by transmission time inside the new rule, with toc only where no transmission time is parsed; the shared O3 rule (the brdc_rts feed and the health series) left as it is, its Galileo lead to be fixed under a key of its own if wanted.
4. [corrected 2026-11-19: see Corrections, item 7] At a day start, Galileo records of that day's file are not yet available for about 11 minutes: merge the previous day's navigation file, accept the counted `AfterFirstUse` fallback, or hold the satellites out.
5. In a stream, a satellite with no available record has its code columns held out as a declared admission event; the hold-out mechanism to reuse is to be chosen.
6. Whether to reserve `running` in the enumeration now (the review) or add it only if ever built (this document).
7. Whether to run the GPS check of section 10, item 2, before adoption, given that `first_use` puts the short-window Galileo values into the adopted DOY 146 day run.
8. Whether a data detector for a true geometry-free code jump of one satellite (invariant 1, a gap of today's time link) becomes its own design.

## Not checked

- [corrected 2026-11-19: see Corrections, item 9] Whether `networkCodeBiasDatum` runs on withheld or skipped epochs, and whether a satellite's code column is registered at its first sighting (both bear on the definition of first use).
- The first Galileo toc and transmission time in the daily navigation files, hence the length of the day-start gap.
- Whether the earliest transmission time in a merged file equals first reception at a real-time station.
- The step counts of variant B (60 and 48): from the first stage's script only.
- The behaviour of rows 4 and 12 past `t1` in a stream (inferred from code, not exercised).
- Whether any loader check refuses streaming without a duration or ties the metadata files to the live day (none found).
- Whether the chain and ring-fix seams under `src/core/network/ar` hold a delayed decision that buffers later epochs (pattern grep only).
- How a Galileo code datum reaches GPS narrow-lane levels under the per-signal trees (section 124, not traced).
- The uncommitted snapshot edits in the worktree and how `GDL` fits them.
- The mechanism and sign of variant B's ionosphere and phase re-expression (moot if B is not built).

## 2026-11-19: corrections from the check against the code

Trees: `7fb0737` is the frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737` (network code, docs); `main` is `C:/GnssProcessor` at `5ddc422` (user engine). The draft was read at `03dfcbc`; between `03dfcbc` and `7fb0737`, `ChainMemory.cpp` (new), `Entities.cpp`, `SnapshotFingerprint.cpp`, `WarmResume.cpp`, `RunFields.cpp`, `StateSnapshotWrite.cpp`, `GnssConfigLoadNetwork.cpp` and the ledger changed (step S7 (b) of the chain audit). Navigation-file numbers were re-measured by the check with scratch scripts.

1. **Section 7, one owner.** The document proposes a new `net::SatelliteState` member with a `GDL` `Persist::Record` row. At `7fb0737` the latch is already saved in one place, the `GW`/`GD` lines of the chain memory section (`src/core/network/state/ChainMemory.cpp:56-58`, restore `:79-127`, `7fb0737`). A separate `GDL` row would be a second owner; the held value, hold epoch and reason belong in an extension of the `GD` line, or `GD` moves to the satellite table as its one owner.
2. **Section 7, the uncommitted snapshot edits.** The document says the snapshot edits of section 125 are uncommitted and were not read. They are committed at `7fb0737` (step S7 (b)); the bullet is overtaken.
3. **Decision 1.** The document asks whether to build rule change 11 "before S7 (b) as part 3 proposes". `7fb0737` is S7 (b) (git log); the rule now has to fit its saved latch.
4. **Section 1, the latch's readers.** The document says the code-bias datum builder is the only reader and the tests the only other users. At `7fb0737` the chain memory section writes the latch with its window to the snapshot (`ChainMemory.cpp:57-58`, `7fb0737`) and, under `network.resume_chain_restore`, replaces it with the saved values (`ChainMemory.cpp:127`, `7fb0737`). The reader in the solve is still the code-bias datum builder (`CodeBiasDatum.cpp:396`, `Sbasis.cpp:1563-1565`, `7fb0737`).
5. **Section 2, inventory completeness.** One row is missing: row 15, the chain restore (`ChainMemory.cpp:57-58`, `:127`, `7fb0737`). A resumed run takes the seed's latch, formed over the seed's window, and so inherits row 2's look-ahead; under `first_use` it must carry the held values instead.
6. **Section 2, which rows are armed.** "Of the rows below only the latch is armed in the reference configurations" is overstated. Of the keyed rows (3, 4, 8, 9, 10, 14) none is armed in the reference configurations; rows 1, 2, 5, 6, 7 and 11 are active in them (`apriori_source` stays `sp3` by default, `max_duration_s` is set to 21600; `config/golden/*.json`, `7fb0737`).
7. **Section 3 item 10 and decision 4, the day-start gap.** "About 11 minutes" for Galileo is wrong. Measured on the two navigation files: on DOY 146, 24 of 30 Galileo satellites have an F/NAV record transmitted before 00:00 in the day's file (same value as the next) and 6 wait 470 to 2 670 s; on DOY 147, 19 of 30, and 11 wait 650 to 1 600 s. Decision 4 reads "up to about 45 minutes for a minority" instead of "about 11 minutes".
8. **Section 10 item 2 and the Outcome, the GPS risk.** The wording is stronger than the record. The day run would meet one of section 124's three conditions (the short-window latch); exact epoch time off and the per-signal Galileo trees, the other two, are absent in the adopted configuration. The record holds no DOY 146 run that combines the short-window latch with exact epoch time on and the single Galileo network. Each run lacking one condition converged, so the 5 h check is a precaution, not a demonstrated risk.
9. **Section 3 item 2 and "Not checked", skipped epochs.** Partly settled: `networkCodeBiasDatum` is the second stage of `networkSolveGlue` (`src/core/network/solve/SolveGlue.cpp:253`, `7fb0737`), before the solve-skip exits (`:255-256`). A hold at its opening would fire on epochs that then skip; the hold is to be written only once the epoch is known kept (for example at the code-bias capture).
10. **Section 1, armed configurations.** The time link is armed in the three reference configurations (the BeiDou one included), not two; ledger line `docs/CONFIG_promotion_ledger.json:224` (`7fb0737`).
11. **Section 9 step 1, the availability helper.** No new helper is needed: the existing `brdcTransmissionGpsSeconds` (`include/products/BroadcastEphemerisTypes.hpp:164`, `7fb0737`) serves, with the toc fallback.
12. **Section 4, the wait (addition, no marker).** The hold-out mechanism is unchosen, so the invariant-7 bound is a design statement, not yet a wired event.
13. **Section 3, invariant 1 (addition, no marker).** The triage that invariant 3 requires is missing: blocking or inert for observed-data runs is not triaged, and no data on true single-satellite geometry-free code jumps is shown.

### Line numbers that moved

All at `7fb0737` unless marked.

- Ledger: time link `:223` to `:224`; ledger row neighbours `:222-223` to `:223-224`.
- `GnssConfigLoadNetwork.cpp`: loader defaults `:9752-9753`, `:9778-9779`, `:6831`, `:6833`, `:4354-4355` to `:9783-9784`, `:9809-9810`, `:6859`, `:6861`, `:4374`; the `sat_gf_datum` keys `:3700-3703`, `:3724-3727` to `:3720-3729`, `:3744-3746`.
- `Sbasis.cpp`: fallback `:170-179` to `:168-179`; extra-channel anchor `:1806-1810` to `:1803-1818`; time link `:1599-1614` read as `:1599-1615`.
- `CodeBiasDatum.cpp`: `registered_code_keys` `:325-326` to `:324-325`.
- `Entities.hpp:417-420` to `:421`; `Entities.cpp:574-575` (`CBS`) to `:577`; `Entities.cpp:1352` (`kEntityWriters`) to `:1451`.
- `SnapshotFingerprint.cpp:295-305` to about `:318-328`.
- `ApplicationNetworkSetup.cpp`: health window `:394-449` read as `:393-449`; S contract identity `:1700-1730` read as `:1706-1728`; coverage gates `:792-860` read as `:837-862`. `NetworkProductIO.cpp:1406` to `:1405`.
- `BrdcClockApriori.cpp` read as `:110-116`, `:129-134`, `:184-196`, `:201-205`; `BiasAdmission.cpp` read as `:52-55`, `:314-330`.
- `GnssCorrections.cpp` ERP `:1020-1051` read as `:1018-1030` (main).

### What stands

The four look-ahead reads, availability by transmission time, the hold-at-first-use rule and the rejection of variant B stand; the step counts and sizes of variant B were reproduced exactly (F/NAV records in transmission order; first step 05:42 on DOY 146, 05:32 on DOY 147). The rule is prefix-safe in post-processing and causal in a stream as designed. Invariant 7: the wait has both edges and counters on paper, its mechanism unchosen. Invariant 1: reporting a broadcast step without a response is a reasoned datum choice; the true-jump gap exists today and is untriaged. Invariant 6: each step is planned identical with the key off and gated; plausible, unverified. Section 7 must be redesigned around the committed chain memory (item 1).

The decisions after correction:

1. Decision 1 is re-posed: whether to build rule change 11, now that S7 (b) is built and the rule has to fit its saved latch.
2. Decision 2 (variant A, variant B not built) stands.
3. Decision 3 (transmission-time availability inside the new rule only; the shared O3 rule left as it is) stands.
4. Decision 4 stands with a smaller and narrower gap: a minority of Galileo satellites wait up to about 45 minutes at a day start (item 7).
5. Decision 5 (stream hold-out as a declared admission event, mechanism to be chosen) stands.
6. Decision 6 (reserve `running` or not) stands.
7. Decision 7 stands as a precaution: the 5 h DOY 146 check needs no code, and the value equality it relies on holds (no F/NAV change transmitted before 05:42 on DOY 146), but the record makes a failure unlikely (item 8).
8. Decision 8 (a data detector for a true single-satellite geometry-free code jump as its own design) stands, with the triage of item 13 owed.
9. New, from item 1: whether the held value extends the `GD` line of the chain memory section or `GD` moves to the satellite table as the one owner.

### Left unconfirmed

- Whether the adopted 24 h DOY 146 run of the five-day set exists (no run folder opened).
- Health filtering in the step counts (all F/NAV records counted).
- The loader line at `GnssConfigLoadNetwork.cpp:2248` on streaming.
- Whether `n_sat_gf_reanchored` covers link cuts.
- `ProductResolver` behaviour at a week boundary.
- The BeiDou availability shift.
- The SSR and real-time user path (main) for group delays.
