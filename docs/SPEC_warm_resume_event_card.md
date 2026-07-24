# SPEC — RTB-7(d) warm-resume event response + branch grading ("RS card")

**Handoff**: written 2026-07-23 by the Fable session that reviewed
`docs/HANDOFF_warm_resume_branch_faithful.md` (methods review; conclusions below
supersede that doc's §"Our interpretation"). Self-contained — do not assume
access to any prior conversation. Read `CLAUDE.md` (build pattern, **Standing
invariants**) and `docs/HANDOFF_warm_resume_branch_faithful.md` (the predecessor
measurement this card audits and extends) before starting. The ground rules of
`docs/SPEC_slip_qc_hardening.md` §1 apply unchanged (default-OFF + same-build
byte-identity; Release-only data runs with a renamed exe and name-scoped zombie
kills; staged cadence — every run in this card is smoke-scale by design;
empirical thresholds; never hard-exclude measurements; run-end counters prove
wiring; additive unit tests; Doxygen + explicit CMake lists; no co-author
trailer). All of RTB-7(c) (serialize/restore, `state_snapshot_carry`) is
UNCOMMITTED — this card revises it freely (blob format included); nothing is
committed without the user's say-so.

Cards state **MUST** criteria (binding) and **CANDIDATE** mechanisms
(non-binding — deviate with a stated rationale). If a MUST seems wrong, stop and
ask the user.

---

## 0. Evidence base — the review's corrections (verified in tree, 2026-07-23)

### 0.1 The prior conclusion is not supported at the measured horizon

The handoff's table, read as a derivative instead of two levels: cold diverges
834→1708 mm over ep15→30 (**~58 mm/ep**), warm 1480→1793 (**~21 mm/ep**). The
warm arm pays a one-time transient and then diverges from inline **3× slower**;
naive extrapolation crosses at ~ep33. "Warm stays worse" was concluded inside a
transient whose decay constants (D5 calibrate window = 15 ep, iono-deweight
window = 15 ep) equal the entire graded window. The conclusion "partial
restoration is counterproductive" is therefore **unproven**; what is proven is
"partial restoration doubles the ep-15 transient." This card measures the rest.

### 0.2 The mechanism, corrected (what actually differs warm-vs-cold at ep 15)

The handoff blames "slip-detector pools cold-start and read the warm carry's
innovations as slips." Mechanically wrong at ep 15:

- **IF-TD cannot fire at ep 15** — it needs the previous epoch's phase, which is
  per-station state deliberately excluded from the snapshot
  (`NetworkRunContext.cpp:617-627`). Cold in BOTH arms; sees phase data, never
  carry innovations.
- **MW pools** — likewise cold in both arms; not a warm-specific channel.
- The only detector that literally reads carried-DD innovations is the **D5
  gate**, and it is the one piece that is genuinely miscalibrated at resume,
  by documented choice: the σ̂₀² inflation accumulator is not serialized
  (`NetworkInvariantCarryDriver.hpp:242-244`), and below
  `kInflationWarmupRows = 200` the gate runs the **raw formal test**
  (`NetworkInvariantCarryDriver.hpp:559-561`) — the same formal Cov(v) that D5
  v2 exists to correct ("10-30× optimistic", `hpp:105`). The inline A at ep 15
  gates with a 14-epoch-calibrated σ̂₀; the warm resume gates 1214 restored
  chords at seed σ̂₀ = 1. The cold arm has zero carried chords at ep 15 —
  nothing to gate, no storm possible. (`σ̂₀` gates with a one-epoch lag —
  `hpp:288-291` — so ep 15 is fully uncalibrated; from ep 16 the accumulator is
  statistically full but was fed the storm epoch's innovations.)
- The innovations themselves are genuinely biased at ep 15, because the
  dominant unrestored state is the **per-station filter layer** (ambiguity /
  iono / receiver-bias states + covariances), excluded on purpose with a
  written rationale that assumes the phase branch re-converges *with* it
  (`NetworkRunContext.cpp:617-627`), plus the adaptive iono-deweight window
  restarting at full constraint weight (`iono_constraint_deweight_*`, 15-ep
  window, cold at resume). In the cold arm, single-epoch phase has zero
  redundancy (every phase row is absorbed by its own free ambiguity), so these
  biases cannot reach the clocks through phase. The restored DD priors give
  phase redundancy — they turn phase into a conduit that pumps station-layer
  re-convergence error into the shared clocks. **Restoring the carry did not
  create the inconsistency; it created observability of it**, and the graded
  product (CLK between-sat SD) sits at the end of the conduit. This is standing
  invariant #1 verbatim: a stiff carry crossed an event (the resume) with no
  wired event response.

Corollary on the counters: "slip refloats" in the handoff's table is the
driver's `slip_kills` (`NetworkTeardown.cpp:513`), which only counts chords
that *would have been carried* — the warm arm has ~1214 more chords of
exposure in the early window, so +201 partially reflects exposure, not events.
The D5 flags/retaps are separate counters printed at
`NetworkTeardown.cpp:669-676` and were not reported.

### 0.3 State inventory (what the snapshot restores today — corrects the handoff)

Restored (`networkTryWarmResume`, `NetworkRunContext.cpp:307-561`): D2 border
means/vars (diagonal), emitter blob (all-or-nothing with everything else),
`local_carry` + `zwd_carry` (`:458-463` — the handoff said these were absent;
they are restored, merely empty in this config), `clock_osc`/`clock_rate_est`,
`prev_clk`, pivots, URA/ramp, `relevel_iode`, generation id, `prev_week/tow`
(so `beginEpoch` dt and D2 q·dt inflation are correct across the gap), and —
under `state_snapshot_carry` — the CARRY blob + `carry_sat_vertex` +
`carry_group_id` (`:518-540`).

Not restored, by class:

| state | where | materiality at resume |
|---|---|---|
| per-station filters (amb/iono/rx-bias + cov, slip arcs/history) | design exclusion, `NetworkRunContext.cpp:617-627` | **dominant**; restoring = full checkpoint, declined trade — OUT OF SCOPE |
| D5 σ̂₀² accumulator | `NetworkInvariantCarryDriver.hpp:242-244` | **the one cheap fix** — one number + row count (Card RE-1) |
| C1 dense border side-car | re-seeds at first emitted epoch, `NetworkRunContext.cpp:430-437` | **one-epoch gap only** — the handoff overweights it; deferred with data condition (§6) |
| eq34 sticky election maps | `NetworkRunContext.hpp:212-277` | fresh tree at ep 15; the carry reconcile projects across basis changes by design; PB-frame churn only |
| Card-2 `code_bias_prev_` / IF-pin maps | documented re-seed + `code_bias_datum_resumed_` bump, `NetworkRunContext.hpp:315-359` | per-sat clock↔CB datum step, **shared by both arms** — pollutes absolute metric levels, not the warm-vs-cold delta |
| MW/IF-TD pools, `arc_spike_streak` | per-station / documented re-accrue | cold in both arms; cannot fire at ep 15; second-order — declined (§6) |

### 0.4 Protocol hazard in the prior measurement (audit before trusting any number)

The resume configs point `state_snapshot_path` at `snap_wwork.txt` and the
resume run **overwrites it** every emitted epoch
(`state_snapshot_interval_s = 10` < 30-s cadence). A subsequent resume launch
against the un-re-seeded file sees a snapshot from its own future and
`warmResumeAllowed` **rejects non-positive age**
(`NetworkStateSnapshot.cpp:462-466`) → **silent full cold start** (warn-level
log only, `NetworkRunContext.cpp:391-393`). If the prior B2 arms were run
without re-copying `snap_wconv.txt → snap_wwork.txt` before *every* launch,
some arms compared {fully cold} vs {warm}, not {border-warm} vs
{border-warm + DD}. The carry-OFF arm matching the old binary (833.9 vs 834) is
consistent with **both** having cold-started identically. P0-1 settles this
from logs.

---

## 1. Card P0 — measurement audit + free re-reads (no new code, no new runs)

Everything here comes from artifacts that already exist.

- **P0-1 (MUST, blocking)**: audit every prior run log (A inline, B1 conv,
  B2 carry-ON, B2 carry-OFF, old-binary B2). For each resume arm, record
  which of these lines is present: `"WARM RESUME from"` (+ its restored-counts
  payload, `NetworkRunContext.cpp:543-556`), `"BRANCH-FAITHFUL — restored
  invariant DD carry"` (`:534-538`), or any `"COLD START"` warn (`:360`,
  `:388`, `:391`, `:401`, `:527`). Verdict per arm: warm-with-carry /
  warm-no-carry / cold. If any arm that the handoff treated as warm was cold,
  the 834/1480 table is invalidated and G4's run matrix re-measures it (the
  runs are ~minutes each; this is cheap). Report the verdict table first in
  the card report.
- **P0-2 (MUST)**: pull from the same logs, per arm: `carry_innov_flags_total`,
  `carry_innov_retaps_total`, final σ̂₀ (`NetworkTeardown.cpp:669-676`), the
  carry census line (births / slip refloats / marginalizations,
  `NetworkTeardown.cpp:265-270`, `:511-520`). This tests §0.2: CANDIDATE
  prediction — warm arm shows a D5 flag total far above cold's, concentrated
  early (per-epoch timing lands with BR-1's traces if totals alone are
  ambiguous).
- **P0-3 (MUST)**: commit the SD-of-SD metric script used for the handoff
  (wherever it currently lives) to `scripts/` with a README line; regenerate
  the metric **per-epoch curves** (not two samples) for the three existing
  arms on one plot (matplotlib fine; house colormap rules apply to heatmaps
  only). This is the artifact that decides "transient vs divergence" by
  inspection.
- **P0-4 (MUST)**: extend the measurement window. `_warm_inline.json` and both
  resume configs: `processing.max_duration_s` 900 → **1830** (epochs 0..60 /
  15..60; `start_offset_hours: 0.125` and B1/`_warm_conv.json` unchanged;
  resume age stays 30 s ≪ horizon 900 s). Re-run A-60, B2-ON-60, B2-OFF-60
  under the §5 protocol (with the P0-1 re-seed rule enforced). Report the
  per-epoch curves and the ep20–60 fitted slopes. **Decision input**: if warm
  crosses below cold and tracks A with the shallower slope, the handoff's
  "counterproductive" conclusion is formally retired regardless of the rest of
  this card.

## 2. Card BR — branch-faithfulness metric (grade the feature on the branch, not on CLK)

Between-sat SD of CLK is an omnibus readout at the end of the §0.2 conduit; it
cannot attribute. The branch has a direct, datum- and integer-invariant
instrument: the carry blob itself.

- **BR-1 (MUST)**: per-epoch carry-blob history. New knob
  `network.state_snapshot_keep_history` (bool, default false, loader in
  `GnssConfigLoadNetwork.cpp` per house toggle conventions). When armed AND a
  snapshot path is set, `networkWriteStateSnapshot` additionally writes each
  snapshot to `<snap_path>.ep<NNNN>` (epoch index zero-padded; same atomic
  temp+rename). Write-side only — read path untouched; byte-identity of the
  estimation is structural (the writer already takes `const` refs and runs
  after emit, `NetworkRunContext.cpp:606-612`).
- **BR-2 (MUST)**: `scripts/diff_carry_blobs.py`: parse two snapshot files,
  extract the CARRY sections, and compare per-(group, chord) **after
  translating ids to names** — rx vertex = roster index (stable: roster order
  is fingerprinted, `NetworkRunContext.cpp:178`), sat vertex via each file's
  OWN `carry_sat_vertex` map, group via each file's OWN `carry_group_id` map
  (first-seen-order ids MAY diverge between runs for sats first seen after the
  fork — comparing raw ids is the silent mis-binding trap; the translation is
  the point of the script). Wavelength per group from the (const,signal) key
  (static GPS table is enough for this card; pair blocks carry λ inline).
  Output per epoch-pair: n common chords; |Δz| distribution in cycles
  (median/p95); **fraction |Δz| < 0.05 cyc** ("branch-agree"); **count with
  |Δz − round(Δz)| < 0.05 cyc, round(Δz) ≠ 0** ("branch-flip"); set churn
  (only-in-A / only-in-B). CANDIDATE thresholds — report the full
  distributions, don't tune to the thresholds.
- **BR-3 (MUST)**: sanity anchor: B2-warm's blob at its FIRST post-restore
  write vs B1's final blob — the restore is exact by unit test, so
  branch-agree at the restore boundary must be ≈100% on the surviving set;
  discrepancies here are tooling bugs, fix before grading anything.
- **BR-4 (MUST)**: grade all arms vs A at ep 15/20/30/45/60 with BR-2. This
  yields the card's central curve: **branch retention over time**, warm vs
  cold vs (later) warm+shock.

## 3. Card RE — resume event response (the build; the invariant-#1 fix)

The alternative to "all-or-nothing serialization" (which, per §0.3, means full
process checkpointing — out of scope): treat the resume as a wired event for
the stiff restored carry. Master knob `network.state_snapshot_resume_shock_m`
(double, default **0.0** = OFF = byte-identical current behavior; a positive
value arms the event response). All sub-pieces ride it; everything below is
inert at 0.0.

- **RE-1 (MUST)**: serialize σ̂₀. Bump the blob header to `CARRY,v2,<n>` and
  append one line `S,<sigma0_sq>,<eff_rows>` (from the driver's inflation
  accumulator at write; omit the line when calibration is off).
  `restoreCarry` MUST accept v1 (no `S` — legacy: accumulator starts at seed,
  current behavior) and v2. On v2-with-S restore AND `innov_calibrate` armed,
  seed the accumulator so the gate trusts the restored inflation immediately —
  the warmup gate (`kInflationWarmupRows`, `hpp:559-561`) must see the
  restored `eff_rows`, and the floor the restored σ̂₀². CANDIDATE mechanism:
  a small `NetworkCarryVce` seeding hook (read `NetworkCarryVce.hpp` first;
  the constructor's `seed_floor_m2` alone is NOT sufficient — pre-warmup the
  gate ignores the accumulator entirely). Unit-gate in G1.
- **RE-2 (MUST)**: one-shot Pz resume shock. On successful `restoreCarry`,
  add `shock² = (state_snapshot_resume_shock_m)²` to the **diagonal** of every
  restored block's `Pz` (driver API, e.g. `applyResumeShock(double var_m2)`,
  called from the resume site after the `BRANCH-FAITHFUL` branch,
  `NetworkRunContext.cpp:533-539`). Rationale: the restored means ARE the
  branch; opening the variance for the station-layer re-convergence window
  lets the fold absorb dm-scale transient tension without the D5 gate
  discarding the branch — and it needs **no gate special-case**, because
  Cov(v) inflates with the prior, standardized innovations shrink, and the
  gate stays armed against genuine slips (invariant #1 kept, not bypassed).
  Scale note for choosing the sweep: the restored prior already carries
  q_z·Δt = 1e-4·30 = 3e-3 m² (σ 55 mm ≈ 0.29 L1 cyc) — a shock below that is
  a no-op; the sweep (§5, G4) covers {0.05, 0.10, 0.20, 0.50} m and ships the
  smallest value that kills the storm.
- **RE-3 (CONTINGENT — build only if G4(d) fails after RE-1+RE-2)**: funnel
  grace: for the first `ceil(innov_calibrate_window)` installs after a
  restore, D5 flags still re-tap their chord (conservative, safe) but are NOT
  fed into `carry_innov_pending` (no next-epoch station-ambiguity refloat).
  Triage per invariant #3: deferred **with data condition** — armed only if
  the ep15–17 flag spike survives RE-1+RE-2 at every sweep point.
- **RE-4 (MUST, hygiene — rides this card's first build)**:
  - `restoreCarry` begins with `reset()` — today it replaces `carried_` but
    leaves `lane_fix_w_/lane_fix_epoch_/lane_fix_staged_/gate_exempt_/
    last_innov_*/install_epoch_index_/counters_` untouched
    (`NetworkInvariantCarryDriver.cpp:116-127` vs `:159-237`). Inert at run
    start today; the header advertises a general API — make it one.
  - Vertex/group consistency validation on restore: every blob vertex must be
    `< n_stations` or present in `carry_sat_vertex`; every blob group present
    in `carry_group_id`. On failure: warn + cold-start the carry — the same
    degrade path as a parse failure (`NetworkRunContext.cpp:526-532`) —
    instead of silently mis-binding chords. (Needs the roster size / maps
    passed or checked at the call site; caller-side check is fine.)
  - Run-end telemetry (invariant #4): the Card-4 run-end line (or a sibling
    line) reports, on a resumed run: carry restored y/n + count, σ̂₀ seeded
    y/n + value, shock applied y/n + σ, and (if RE-3 exists) grace epochs.
    "0 restored / no shock" must be visible, not inferred from absence.

## 4. Card HB — harness hardening (same-snapshot A/B prerequisites)

- **HB-1 (MUST)**: knob `network.state_snapshot_require` (bool, default
  false): when armed and a snapshot path is set, ANY failure to complete a
  warm resume (missing file, parse fail, fingerprint mismatch, age gate,
  emitter-section fail) is a **hard run error**, not a warn+cold-start. This
  is the structural fix for §0.4 — a dev A/B harness arms it and can never
  silently compare warm-vs-cold. Loader + Doxygen + summary-log line per
  house conventions; byte-identical when off or when resume succeeds.
- **HB-2 (MUST, protocol)**: every resume launch in this card (and in any
  future same-snapshot A/B) is preceded by re-seeding the working snapshot
  from the immutable converge artifact (`copy snap_wconv.txt snap_wwork.txt`)
  — scripted, not manual — and asserts the `WARM RESUME` line (plus
  `BRANCH-FAITHFUL` when the carry arm is on) in the run log afterward.
- **HB-3 (report item, no code)**: document in the card report the
  same-snapshot A/B validity envelope for future users: (i) any knob in the
  resume fingerprint (`NetworkRunContext.cpp:170-303` — roster/order, datum
  toggles, carry q's, clock model, decimation, coupling set, and
  `carry_invariant` itself) cannot be A/B'd against a shared snapshot — the
  variant arm cold-starts (or hard-fails under HB-1); (ii) changes that
  interact with the post-resume transient (slip/gate/deweight thresholds,
  warmup windows) must be graded from epochs ≥ 2× the longest adaptive window
  past resume, and confirmed occasionally on a cold run per the staged
  cadence; (iii) a snapshot pins one realization — fine for deltas (Psychas
  single-realization mandate), not for magnitudes.

## 5. Validation harness

**Build/run protocol** (every run): Release only; rebuild then run a renamed
copy (e.g. `gnss_processor_rs_run.exe`); name-scoped zombie pre-kill; the
`_warm_*` configs already carry `parallel_carry: true / parallel_reduce:
false` — keep. All runs here are 15–60-epoch 50-station GPS smokes (minutes);
nothing in this card needs a long/graded run without the user's say-so.

**Run matrix** (after P0-1's verdicts; re-measure any arm P0-1 invalidated):

| run | config | purpose |
|---|---|---|
| A-60 | `_warm_inline.json` @ 1830 s | inline reference, blob history armed |
| B1 | `_warm_conv.json` (unchanged, 450 s) | converge + snapshot (re-use existing artifact if P0-1 clears it) |
| B2-OFF-60 | `_warm_resume_nocarry.json` @ 1830 s | cold-carry resume baseline |
| B2-ON-60 | `_warm_resume.json` @ 1830 s | warm carry, no shock (current behavior, v2 blob) |
| B2-RS-60 ×4 | `_warm_resume.json` + `resume_shock_m` ∈ {0.05, 0.10, 0.20, 0.50} | the sweep |

All arms: `state_snapshot_keep_history: true`, HB-2 re-seed + log assert,
metric = P0-3 per-epoch SD-of-SD curves + BR-4 branch curves + per-arm D5/census
counters.

**Gates**:

- **G0 structural (MUST, before any run)**: full suite green. Same-build
  byte-identity, three arms: (i) no snapshot path armed → products
  byte-identical to the pre-card build on a 10-ep smoke; (ii) snapshot armed,
  `state_snapshot_carry` off → byte-identical to pre-card same config; (iii)
  carry on, shock 0.0, restoring the existing **v1** blob → byte-identical to
  the pre-card B2-ON products (proves RE-4's reset-first + validation +
  v2 reader change nothing at defaults).
- **G1 unit (MUST, additive tests in
  `tests/test_network_invariant_carry_driver.cpp`)**: v2 round-trip exact
  incl. `S` line; v1 blob accepted with legacy behavior; `restoreCarry`
  clears pre-populated side-state (seed `lane_fix_w_`/`gate_exempt_` via the
  test seams, restore, assert empty); malformed-vertex blob → false + loud
  reason (RE-4 validation); `applyResumeShock` adds exactly shock² to every
  Pz diagonal and nothing else.
- **G2 mechanism (informative)**: P0-2's counter pull confirms/denies the D5
  storm in the old warm arm. Denial does not block the build — it revises
  §0.2 in the report and shifts weight to the RAIM channel; the shock is
  graded empirically either way.
- **G3 tooling anchor (MUST)**: BR-3 passes before BR-4 is trusted.
- **G4 the decisive A/B (MUST — this is the card's verdict)**, best sweep
  point vs B2-OFF-60 vs A-60:
  - (a) ep-15 CLK SD-of-SD: warm+shock ≤ 1.1× cold (transient eliminated);
  - (b) fitted ep20–60 slope: warm+shock ≤ cold's (expect ~⅓ per §0.1);
  - (c) absolute curve: warm+shock ≤ cold from ep 30 on;
  - (d) D5 flags ep15–17 ≤ 2× that arm's own ep30+ steady rate;
  - (e) branch retention (BR-4): report; CANDIDATE bar — ≥ 80% of restored
    chords branch-agree vs A at ep 20;
  - (f) `slip_kills` excess vs cold consistent with exposure (report).
- **G5 telemetry (MUST)**: RE-4's run-end fields present and correct in every
  resumed arm's log (invariant #4: inert wiring is only visible through
  counters).

**Decision tree** (write into the card report; the user rules on shipping):
G4 all-pass → keep default-OFF, mark warm-resume field-runnable *for dev
iteration*, retire the handoff's "counterproductive" claim, recommend
`state_snapshot_carry` + shock as the dev-loop default with HB-1 armed.
(a)+(d) pass but (b)/(c) fail → the branch restore has no post-transient value;
keep serialization as groundwork, same-snapshot A/B (HB) becomes the primary
dev tool, no further mechanism. Storm survives the whole sweep → §0.2 is wrong;
STOP, report, do not layer RE-3 on top of an unconfirmed mechanism without the
user.

## 6. Deferral triage (invariant #3)

| deferral | class | data / condition |
|---|---|---|
| per-station filter checkpoint ("all-or-nothing") | field-inert for this card's goal (dev iteration) | declined by RTB-7 design (`NetworkRunContext.cpp:617-627`); revisit only if G4 fails AND BR-4 shows the branch itself decays (i.e. the conduit story is wrong) |
| C1 dense side-car serialization | field-inert | one-epoch gap (`NetworkRunContext.cpp:430-437`); revisit only if G4(a) fails at every sweep point with G2 confirming flags are NOT the driver |
| MW/IF-TD pool serialization | field-inert | structurally unable to fire at ep 15; cold in both arms; no warm-specific channel (§0.2) |
| PB-fractional grading (DD-frac style) in the SD-of-SD metric | follow-up | phase product currently ungraded by the metric; add after this card using the existing DD-frac machinery |
| RE-3 funnel grace | contingent | build only on G4(d) failure after RE-1+RE-2 (§3) |

## 7. Sequencing

P0-1 audit **first** (blocking; decides whether the old table stands) → G0/G1
(build + gates: RE-1/RE-2/RE-4/HB-1/BR-1 land together — they are one small
surface: driver serialize/restore + resume site + one knob each) → BR-2/BR-3
tooling → run matrix → G4 verdict → report
(`docs/REPORT_warm_resume_event.md`): P0-1 verdict table first, then per-epoch
curves, branch-retention curves, counter tables, gate outcomes, decision-tree
recommendation. Update `docs/HANDOFF_warm_resume_branch_faithful.md` with a
banner pointing here (do not rewrite its history). Commit nothing without the
user.
