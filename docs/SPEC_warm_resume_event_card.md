# SPEC — RTB-7(d) warm-resume event response + branch grading ("RS card")

**Status: EXECUTED IN FULL and ruled on (2026-07-23). Code COMMITTED in `dee2824`** (verified 2026-07-28) — `state_snapshot_{carry,resume_shock_m,require,keep_history}`, `applyResumeShock`, `validateCarryBinding`. **The blob format is no longer free to revise.** Originally a forward plan (Cards P0/BR/RE/HB, a run matrix, gates G0–G5, a decision tree); rewritten outcome-first on 2026-08-15 after its report was lost. Card ground rules (default-OFF + same-build byte-identity; Release-only data runs with a renamed exe and name-scoped zombie kills; staged cadence — every run here is smoke-scale by design; empirical thresholds; never hard-exclude measurements; run-end counters prove wiring; additive unit tests; Doxygen + explicit CMake lists; no co-author trailer) are `docs/SPEC_slip_qc_hardening.md` §1, unchanged.
**Regime for every number in §1–§5**: 50-station GPS smoke, 60 epochs, v2 blob, HB-2 (re-seeded-snapshot) protocol, Release. Run matrix in §7.

> **Two source documents were pruned in commit `a8f044f` and are recoverable at `a8f044f^`:** `docs/REPORT_warm_resume_event.md` (this card's own outcome record — folded into §1–§4) and `docs/HANDOFF_warm_resume_branch_faithful.md` (the predecessor measurement this card audited; its "partial restoration is counterproductive" interpretation is RETIRED, §1). The RTB-7(e) follow-on record `docs/REPORT_carry_blob_diagonal.md` was pruned in the same commit; its finding survives in memory `project_carry_blob_diagonal` and in §5.

---

## 0. Mechanism analysis (verified in tree, 2026-07-23) — the part that is still doctrine

### 0.1 The prior conclusion was drawn inside its own transient

The predecessor's table, read as a derivative instead of two levels: cold diverges 834→1708 mm over ep15→30 (**~58 mm/ep**), warm 1480→1793 (**~21 mm/ep**). The warm arm pays a one-time transient and then diverges from inline **3× slower**; naive extrapolation crosses at ~ep33. "Warm stays worse" was concluded inside a transient whose decay constants (D5 calibrate window = 15 ep, iono-deweight window = 15 ep) equal the entire graded window. What was actually proven is "partial restoration doubles the ep-15 transient." **§1 measures the rest and retires the claim.**

### 0.2 The mechanism, corrected — the station-layer conduit

The predecessor blamed "slip-detector pools cold-start and read the warm carry's innovations as slips." Mechanically wrong at ep 15:

- **IF-TD cannot fire at ep 15** — it needs the previous epoch's phase, per-station state deliberately excluded from the snapshot (`NetworkRunContext.cpp:617-627`). Cold in BOTH arms; sees phase data, never carry innovations.
- **MW pools** — likewise cold in both arms; not a warm-specific channel.
- The only detector that literally reads carried-DD innovations is the **D5 gate**, and it is genuinely miscalibrated at resume by documented choice: the σ̂₀² inflation accumulator is not serialized (`NetworkInvariantCarryDriver.hpp:242-244`), and below `kInflationWarmupRows = 200` the gate runs the **raw formal test** (`NetworkInvariantCarryDriver.hpp:559-561`) — the same formal Cov(v) that D5 v2 exists to correct ("10-30× optimistic", `hpp:105`). The inline arm at ep 15 gates with a 14-epoch-calibrated σ̂₀; the warm resume gates 1214 restored chords at seed σ̂₀ = 1. The cold arm has zero carried chords at ep 15 — nothing to gate, no storm possible. (σ̂₀ gates with a one-epoch lag, `hpp:288-291`, so ep 15 is fully uncalibrated; from ep 16 the accumulator is statistically full but was fed the storm epoch's innovations.) **⚠ This channel was later DISPROVEN in this operating point — §3.**
- The innovations themselves are genuinely biased at ep 15, because the dominant unrestored state is the **per-station filter layer** (ambiguity / iono / receiver-bias states + covariances), excluded on purpose with a written rationale that assumes the phase branch re-converges *with* it (`NetworkRunContext.cpp:617-627`), plus the adaptive iono-deweight window restarting at full constraint weight (`iono_constraint_deweight_*`, 15-ep window, cold at resume). In the cold arm, single-epoch phase has zero redundancy (every phase row is absorbed by its own free ambiguity), so these biases cannot reach the clocks through phase. The restored DD priors give phase redundancy — **they turn phase into a conduit that pumps station-layer re-convergence error into the shared clocks. Restoring the carry did not create the inconsistency; it created observability of it**, and the graded product (CLK between-sat SD) sits at the end of the conduit. This is standing invariant #1 verbatim: a stiff carry crossed an event (the resume) with no wired event response. **This is the surviving explanation (§3).**

Corollary on the counters: "slip refloats" in the predecessor's table is the driver's `slip_kills` (`NetworkTeardown.cpp:513`), which only counts chords that *would have been carried* — the warm arm has ~1214 more chords of exposure in the early window, so +201 partially reflects exposure, not events. The D5 flags/retaps are separate counters at `NetworkTeardown.cpp:669-676`; the carry census line (births / slip refloats / marginalizations) is at `NetworkTeardown.cpp:265-270`, `:511-520`.

### 0.3 State inventory — what the snapshot restores

Restored (`networkTryWarmResume`, `NetworkRunContext.cpp:307-561`): D2 border means/vars (diagonal), emitter blob (all-or-nothing with everything else), `local_carry` + `zwd_carry` (`:458-463` — the predecessor said these were absent; they are restored, merely empty in this config), `clock_osc`/`clock_rate_est`, `prev_clk`, pivots, URA/ramp, `relevel_iode`, generation id, `prev_week/tow` (so `beginEpoch` dt and D2 q·dt inflation are correct across the gap), and — under `state_snapshot_carry` — the CARRY blob + `carry_sat_vertex` + `carry_group_id` (`:518-540`).

Not restored, by class:

| state | where | materiality at resume |
|---|---|---|
| per-station filters (amb/iono/rx-bias + cov, slip arcs/history) | design exclusion, `NetworkRunContext.cpp:617-627` | **dominant**; restoring = full checkpoint, declined trade — OUT OF SCOPE |
| D5 σ̂₀² accumulator | `NetworkInvariantCarryDriver.hpp:242-244` | **the one cheap fix** — one number + row count (RE-1) |
| C1 dense border side-car | re-seeds at first emitted epoch, `NetworkRunContext.cpp:430-437` | **one-epoch gap only** — the predecessor overweights it; deferred with data condition (§6) |
| eq34 sticky election maps | `NetworkRunContext.hpp:212-277` | fresh tree at ep 15; the carry reconcile projects across basis changes by design; PB-frame churn only |
| Card-2 `code_bias_prev_` / IF-pin maps | documented re-seed + `code_bias_datum_resumed_` bump, `NetworkRunContext.hpp:315-359` | per-sat clock↔CB datum step, **shared by both arms** — pollutes absolute metric levels, not the warm-vs-cold delta |
| MW/IF-TD pools, `arc_spike_streak` | per-station / documented re-accrue | cold in both arms; cannot fire at ep 15; second-order — declined (§6) |

### 0.4 Protocol hazard — the snapshot-overwrite trap (real; now closed structurally)

The resume configs point `state_snapshot_path` at `snap_wwork.txt` and the resume run **overwrites it** every emitted epoch (`state_snapshot_interval_s = 10` < 30-s cadence). A subsequent resume launch against the un-re-seeded file sees a snapshot from its own future and `warmResumeAllowed` **rejects non-positive age** (`NetworkStateSnapshot.cpp:462-466`) → **silent full cold start** (warn-level log only, `NetworkRunContext.cpp:391-393`). Every same-snapshot A/B must therefore re-seed the working snapshot from the immutable converge artifact before *every* launch. The corroborating observation that motivated the audit: **the carry-OFF arm matching the old binary (833.9 vs 834) is consistent with *both* having cold-started identically.** The audit (§1) found the hazard did NOT materialise in the prior arms, but it is now closed by `scripts/run_warm_resume_arm.sh` (re-seed + verdict assert) and by `network.state_snapshot_require`.

---

## 1. Outcome — the conclusion inverts once the window is long enough

**P0-1 measurement audit (blocking, reported first).** Every prior resume arm classified from its log (`WARM RESUME from` + its restored-counts payload, `NetworkRunContext.cpp:543-556` / `BRANCH-FAITHFUL — restored invariant DD carry`, `:534-538` / any `COLD START` warn):

| arm | verdict |
|---|---|
| `warm_resume` (old binary, the 834 mm arm) | WARM, no carry (feature did not exist) |
| `warm2_resume` (carry ON, the 1480 mm arm) | **WARM + CARRY** |
| `warm2_resume_nocarry` (carry OFF) | **WARM, no carry** |
| `snaptest_resume` | COLD START — protocol bug found and fixed *before* any grading; not part of any comparison |

**Verdict: the prior 834/1480 table stands.** No arm graded as warm had silently cold-started; **no arm was silently cold.**

**P0-4 — extending the window inverts the conclusion.** `processing.max_duration_s` 900 → **1830** on `_warm_inline.json` and both resume configs (epochs 0..60 / 15..60; `start_offset_hours: 0.125` and B1/`_warm_conv.json` unchanged; resume age stays 30 s ≪ horizon 900 s). Same configs, pre-card build. CLK between-sat SD-of-SD vs inline [mm]:

| epoch | cold | warm |
|---:|---:|---:|
| 15 | 833.9 | **1479.9** ← transient |
| 18 | 1207.0 | **1121.2** ← crossover |
| 30 | 1708.0 | 1793.2 |
| 45 | 2743.4 | 2314.8 |
| 60 | **3524.7** | **2618.7** |

Fitted slopes ep20–60: **CLK cold 55.8 vs warm 29.5 mm/ep; CB cold 28.6 vs warm 16.1.** Warm is below cold in 33/46 overlap epochs. **The predecessor's "partial restoration is counterproductive" conclusion is RETIRED**: the branch-faithful carry crosses below cold at **ep 18** and roughly **halves** post-transient divergence from inline, while cutting chord re-births **~27 %**.

## 2. RE-2 resume shock — FAILS at every σ. DO NOT RE-PROPOSE.

`network.state_snapshot_resume_shock_m` (double, default **0.0** = OFF = byte-identical) adds `shock²` to the diagonal of every restored block's `Pz` on successful `restoreCarry` (`applyResumeShock`, called after the `BRANCH-FAITHFUL` branch, `NetworkRunContext.cpp:533-539`). Rationale as specced: the restored means ARE the branch, so opening the variance for the station-layer re-convergence window lets the fold absorb dm-scale transient tension without D5 discarding the branch — with **no gate special-case**, because Cov(v) inflates with the prior, standardized innovations shrink, and the gate stays armed against genuine slips (invariant #1 kept, not bypassed). Scale note: the restored prior already carries `q_z·Δt = 1e-4·30 = 3e-3 m²` (σ 55 mm ≈ 0.29 L1 cyc), so a shock below that is a no-op.

Swept {0.05, 0.10, 0.20, 0.50} m on the 60-epoch matrix. CLK SD-of-SD vs inline [mm]:

| epoch | cold | warm0 | rs005 | rs010 | rs020 | rs050 |
|---:|---:|---:|---:|---:|---:|---:|
| 15 | 833.9 | 1479.9 | 1479.4 | 1466.3 | 1412.1 | **1169.8** |
| 30 | 1708.0 | 1769.2 | 1765.1 | 1741.9 | 1733.4 | 1839.8 |
| 45 | 2743.4 | 2331.6 | 2315.5 | 2304.6 | 2307.9 | 2480.5 |
| 60 | 3524.7 | 2630.3 | **2610.5** | 2622.2 | 2630.4 | 2863.6 |
| **slope ep20–60** | **55.81** | 30.18 | **29.74** | 30.57 | 31.75 | 37.11 |
| stays below cold from | — | ep33 | — | — | **ep32** | ep35 |

**G4(a) FAILS at every sweep point — the storm survives the whole sweep at 1.40×–1.77× cold against the ≤1.1× bar** (best 1.40×). The shock monotonically shrinks the transient (1480 → 1170) and monotonically **ERODES the long run** (slope 30.2 → 37.1; ep60 2630 → 2864). σ = 0.05 m is a no-op exactly as predicted (below the carry's own q_z·Δt ≈ 55 mm). Per the card's own decision tree ("storm survives the whole sweep → §0.2 is wrong; STOP"), **RE-3 (funnel grace) was STRUCK and NOT built. Do not re-propose resume shock.**

## 3. §0.2 mechanism, revised by measurement

- **D5 σ̂₀ warm-up — DISPROVEN in this regime.** RE-1 (blob header `CARRY,v2,<n>` + one `S,<sigma0_sq>,<eff_rows>` line; `restoreCarry` accepts v1 and v2) is **correctly wired** — telemetry confirms `sigma0 SEEDED 0.8968` from a converge blob carrying `σ̂₀² = 0.804, eff_rows = 12266` — **but NUMERICALLY INERT**: `innovInflationSigma0` clamps the factor to **[1, cap]** ("only RAISE the threshold"), so a σ̂₀² *below* 1 clamps to 1.0, precisely what an unseeded accumulator returns pre-warm-up. **RE-1 only bites where the converged σ̂₀² > 1.** (Implementation trap, recorded because the symbol alone does not carry it: the seeding hook had to be added to `NetworkCarryVce` — read `NetworkCarryVce.hpp` first, because **the constructor's `seed_floor_m2` alone is NOT sufficient; pre-warmup the gate ignores the accumulator entirely.** The shipped symbol is `NetworkCarryVce::seedRobustFloor`.)
- **Station-layer conduit — the surviving explanation.** It predicts the shock should *partially* relieve the transient without eliminating it (the tension is real data-driven re-convergence, not a gate artifact) — exactly the monotone, partial, benefit-eroding response measured in §2.

Counters (60 ep, whole-run):

| arm | D5 flags | final σ̂₀ | chord births | slip refloats |
|---|---:|---:|---:|---:|
| A inline | 1377 | 1.76 | 3321 | 1695 |
| cold | 1489 | 1.82 | 3307 | 1711 |
| warm0 | 1708 | 1.88 | **2403** | 1972 |
| rs020 | 1678 | 1.86 | 2379 | 1948 |
| rs050 | **1615** | 1.80 | 2321 | 1890 |

Births drop ~27 % on every warm arm — the carry is demonstrably doing its job. G4(f): the warm arms' `slip_kills` excess (+15 %) tracks their ~1214 extra chords of early exposure, consistent with the exposure caveat rather than an event storm.

## 4. The branch metric (Card BR) is INVALID ACROSS RUNS — G3 failed

`network.state_snapshot_keep_history` (bool, default false) writes each snapshot to `<snap_path>.ep<NNNN>` (write-side only; estimation byte-identity structural — the writer takes `const` refs and runs after emit, `NetworkRunContext.cpp:606-612`), and `scripts/diff_carry_blobs.py` compares two blobs per-(group, chord) **after translating ids to names** — rx vertex = roster index (stable: roster order is fingerprinted, `NetworkRunContext.cpp:178`), sat vertex via each file's OWN `carry_sat_vertex`, group via each file's OWN `carry_group_id` (first-seen-order ids MAY diverge between runs; comparing raw ids is the silent mis-binding trap the translation exists to prevent). Wavelength per group comes from the (const,signal) key — a static GPS table is enough for this card, and pair blocks carry λ inline. **Output contract per epoch-pair**: n common chords; the `|Δz|` distribution in cycles (median/p95); fraction `|Δz| < 0.05 cyc` ("branch-agree"); **count with `|Δz − round(Δz)| < 0.05 cyc, round(Δz) ≠ 0` ("branch-flip")**; **set churn (only-in-A / only-in-B)**. CANDIDATE thresholds — report the full distributions, do not tune to them.

BR-3's anchor (B2-warm's first post-restore blob vs B1's final) was expected ≈100 %. **Measured 29.9 % same-integer, median |Δz| = 4.9 cyc.** Diagnosis via a control the card did not specify — *consecutive epochs within a single run*, where the branch cannot move:

- within-run, one epoch apart: median |Δz| = **0.29 cyc**, same-integer **68.1 %**. 0.29 cyc is exactly `√(q_z·Δt) = √(1e-4·30) = 55 mm` — the carry's own process noise. **The spec's 0.05-cyc "branch-agree" bar sits BELOW the within-run noise floor** and measures estimate precision, not branch identity. The correct instrument is **same-integer, `round(Δz) == 0`**, with a within-run ceiling of **68.1 %**.
- Even corrected, BR-3 (29.9 %) is far below that ceiling. Cause: **the serialized `z` is the CORRECTION-frame DD** (`λ·N` minus `Σ c·x_lin`), hence **run-specific**. Two runs linearize differently and the frame diverges with their solutions — which is what BR-4's apparent "decay" (29 % → 3.9 % over ep15→60) actually tracks.

**Consequence: the carry blob is datum-invariant but NOT frame-invariant across runs.** BR-4 is not trusted and **G4(e) is not graded** — its pre-registered CANDIDATE bar was *≥ 80 % of restored chords branch-agree vs A at ep 20*, and §4's noise-floor finding moots the 0.05-cyc branch-agree metric it was written against; the replacement instrument (same-integer) was never given a bar. A fix needs a frame-invariant functional (serialize `x_lin`, or compare a between-chord difference that cancels the frame) — blocking for any future branch claim.

## 5. Gate ledger, shipping posture, and the Aug-2 re-measurement

| gate | outcome |
|---|---|
| **G0** structural byte-identity, 3 arms | **PASS** — (i) no snapshot armed, (ii) snapshot armed + `state_snapshot_carry` off, (iii) carry on / shock 0.0 restoring the existing **v1** blob: all byte-identical to the pre-card build. Proves RE-4's reset-first + binding validation + the v2 reader change nothing at defaults. |
| **G1** unit (additive, `tests/test_network_invariant_carry_driver.cpp`) | **PASS** — 884 ran, **883 passed**, 0 failed. Five new gates: v2 round-trip incl. `S`; v1 accepted (legacy); `restoreCarry` clears side-state; unbound-vertex/group blob rejected loudly; `applyResumeShock` touches only the diagonal. |
| **G2** mechanism (informative) | **REVISED** — §3. |
| **G3** tooling anchor | **FAIL** — §4. |
| **G4** decisive A/B | (a) **FAIL** at every sweep point (1.40×–1.77× vs ≤1.1×); (b) **PASS** at every point; (c) **marginal** (below cold from ep32–35, not ep30); (d) **not graded** — no per-epoch flag telemetry; its bar was *D5 flags ep15–17 ≤ 2× that arm's own ep30+ steady rate*; (e) **not graded** (G3; bar in §4); (f) consistent with exposure. |
| **G5** telemetry | **PASS** — the RE-4 run-end line is present and correct in every arm, including the negative case (`carry restored NO`) on the cold and inline arms. |

**RE-4 hygiene, shipped with the first build:** `restoreCarry` begins with `reset()` (it previously replaced `carried_` but left `lane_fix_w_/lane_fix_epoch_/lane_fix_staged_/gate_exempt_/last_innov_*/install_epoch_index_/counters_` untouched — `NetworkInvariantCarryDriver.cpp:116-127` vs `:159-237`); vertex/group consistency is validated on restore (every blob vertex `< n_stations` or present in `carry_sat_vertex`; every blob group present in `carry_group_id`; on failure warn + cold-start the carry, the same degrade path as a parse failure, `NetworkRunContext.cpp:526-532`); and the run-end line reports carry restored y/n + count, σ̂₀ seeded y/n + value, shock applied y/n + σ — "0 restored / no shock" is visible, not inferred from absence (invariant #4).

**`network.state_snapshot_require`** (bool, default false): when armed with a snapshot path set, ANY failure to complete a warm resume (missing file, parse fail, fingerprint mismatch, age gate, emitter-section fail) is a **hard run error**, not warn+cold-start. This is the structural fix for §0.4 — a dev A/B harness arms it and can never silently compare warm-vs-cold. Byte-identical when off or when resume succeeds.

**SHIPPING POSTURE (user rules on shipping; knobs stay default-OFF):** `state_snapshot_carry: true`, **`resume_shock_m: 0`** (σ=0 keeps the best slope and ep60 level), `state_snapshot_require: true`, and **discard the first ~5 epochs after resume**. The transient is ~3 epochs wide and fully skippable — cheaper and more honest than any shock value measured here. **Do not ship RE-2 armed**; keep it default-0 as an instrument. Same-snapshot A/B remains the primary dev tool and is now structurally safe.

**RTB-7(e) follow-on (2026-07-23):** the dense Pᶻ is 98 % of the snapshot and worth ≤3 % of the warm-resume benefit. `network.state_snapshot_carry_diagonal` writes a blob v3 storing only the Pᶻ diagonal — at 50 sta, **89.7× smaller blob, CLK slope 29.79 vs 30.18 mm/ep**. (Record: memory `project_carry_blob_diagonal`; its doc was pruned in `a8f044f`.)

**⚠ Aug-2 FT63 re-measurement narrows the benefit window to ~3 epochs**: 1491 births saved, essentially all in ep1–2, and **both arms end at exactly 775 carried DDs**.

### HB-3 — same-snapshot A/B validity envelope (doctrine for future users)

1. Any knob inside the resume fingerprint (`NetworkRunContext.cpp:170-303` — roster/order, datum toggles, carry q's, clock model, decimation, coupling set, and `carry_invariant` itself) **cannot be A/B'd against a shared snapshot**: the variant arm cold-starts, or hard-fails under `state_snapshot_require`.
2. Changes that interact with the post-resume transient (slip/gate/deweight thresholds, warm-up windows) must be graded from **epochs ≥ 2× the longest adaptive window past resume**, and confirmed occasionally on a cold run per the staged cadence.
3. **A snapshot pins one realization** — fine for deltas (Psychas single-realization mandate), not for magnitudes.

## 6. Deferral triage (invariant #3)

| item | class | condition |
|---|---|---|
| RE-3 funnel grace — *for the first `ceil(innov_calibrate_window)` installs after a restore, D5 flags still re-tap their chord (conservative, safe) but are NOT fed into `carry_innov_pending`, i.e. no next-epoch station-ambiguity refloat* | **STRUCK** | decision tree forbids it — mechanism unconfirmed (§2/§3). Named in full so it cannot be re-proposed under another label. |
| frame-invariant branch metric | follow-up, **BLOCKING for any future branch claim** | needed before BR-4 / G4(e) can ever be graded (§4) |
| per-epoch D5 flag telemetry | follow-up | required to grade G4(d) against its ≤2×-own-steady-rate bar (§5) |
| C1 dense side-car serialization | field-inert | one-epoch gap (`NetworkRunContext.cpp:430-437`); revisit only if a future G4(a) attempt has G2 confirming flags are not the driver |
| per-station filter checkpoint ("all-or-nothing") | out of scope | full process checkpointing, declined by RTB-7 design (`NetworkRunContext.cpp:617-627`); revisit only if the branch itself is shown to decay |
| MW/IF-TD pool serialization | field-inert | structurally unable to fire at ep 15; cold in both arms; no warm-specific channel (§0.2) |
| PB-fractional grading (DD-frac style) in the SD-of-SD metric | follow-up | phase product currently ungraded by the metric; add using the existing DD-frac machinery |

## 7. Artifacts

Knobs (all default-OFF/0, committed in `dee2824`): `network.state_snapshot_carry` (blob v2 + `S` line), `state_snapshot_resume_shock_m`, `state_snapshot_require`, `state_snapshot_keep_history`, `state_snapshot_carry_diagonal` (v3, RTB-7e). Code: `NetworkCarryVce::seedRobustFloor`; `NetworkInvariantCarryDriver::{serializeCarry,restoreCarry,applyResumeShock,validateCarryBinding}` + resume telemetry; the RE-4 run-end line. Tooling: `scripts/{run_warm_resume_arm.sh, warm_resume_sd_metric.py, diff_carry_blobs.py}` + `scripts/README.md`. Configs: `config/test_suite/_rs_*.json`, `_warm_*.json` (all carry `parallel_carry: true / parallel_reduce: false`). Run matrix, 50-station GPS smokes at 60 ep: A-60 (`_warm_inline.json` @ 1830 s), B1 (`_warm_conv.json`, 450 s, converge + snapshot), B2-OFF-60 (`_warm_resume_nocarry.json`), B2-ON-60 (`_warm_resume.json`), B2-RS-60 × {0.05, 0.10, 0.20, 0.50}.
