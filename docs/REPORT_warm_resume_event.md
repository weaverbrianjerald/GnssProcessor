# REPORT — RTB-7(d) warm-resume event response + branch grading ("RS card")

Executes `docs/SPEC_warm_resume_event_card.md` (2026-07-23). All code is
**default-OFF and uncommitted**; nothing shipped. Supersedes the interpretation in
`docs/HANDOFF_warm_resume_branch_faithful.md`.

> **Follow-on (RTB-7(e), 2026-07-23):** `docs/REPORT_carry_blob_diagonal.md` — the
> dense Pᶻ is 98% of the snapshot and turns out to be worth ≤3% of the warm-resume
> benefit. `network.state_snapshot_carry_diagonal` writes a blob v3 that stores only
> the Pᶻ diagonal (50 sta: 89.7× smaller blob, CLK slope 29.79 vs 30.18 mm/ep).

## 0. Headline

1. **The handoff's "partial restoration is counterproductive" conclusion is RETIRED.**
   It was drawn inside a 15-epoch window whose decay constants *are* 15 epochs. Over
   60 epochs the branch-faithful arm crosses below the cold arm at **ep 18** and
   diverges from the inline reference at **~half** the rate (CLK 30 vs 56 mm/ep).
2. **The resume shock (RE-2) does not kill the ep-15 transient at any swept σ.**
   G4(a) fails everywhere (best 1.40× cold vs the ≤1.1× bar). Per the card's own
   decision tree this is a **STOP**: do not layer RE-3 on an unconfirmed mechanism.
3. **§0.2's primary mechanism is disproven.** The σ̂₀ warm-up channel is *inert* here
   (below), so the surviving explanation is the station-layer conduit alone.
4. **The branch instrument (BR) is not valid across runs** — the serialized `z` is
   correction-frame. G3 failed its anchor, so G4(e) is **not graded**.

## 1. P0-1 measurement audit (blocking — reported first)

Every prior resume arm, classified from its log (`WARM RESUME from` /
`BRANCH-FAITHFUL` / `COLD START`):

| arm | verdict |
|---|---|
| `warm_resume` (old binary, the 834 mm arm) | WARM, no carry (feature did not exist) |
| `warm2_resume` (carry ON, the 1480 mm arm) | **WARM + CARRY** |
| `warm2_resume_nocarry` (carry OFF) | **WARM, no carry** |
| `snaptest_resume` | COLD START — the protocol bug found and fixed *before* any grading; not part of any comparison |

**Verdict: the prior 834/1480 table stands.** No arm graded as warm had silently
cold-started; the §0.4 `snap_wwork` overwrite hazard did not materialise (the working
snapshot had been re-seeded before each launch). The hazard is nonetheless real, and is
now closed structurally by `scripts/run_warm_resume_arm.sh` (re-seed + verdict assert)
and by **HB-1** (`state_snapshot_require`).

## 2. P0-4 — extending the window inverts the conclusion

Same configs, 900 → 1830 s, pre-card build. CLK between-sat SD-of-SD vs inline [mm]:

| epoch | cold | warm |
|---:|---:|---:|
| 15 | 833.9 | **1479.9** ← transient |
| 18 | 1207.0 | **1121.2** ← crossover |
| 30 | 1708.0 | 1793.2 |
| 45 | 2743.4 | 2314.8 |
| 60 | **3524.7** | **2618.7** |

Fitted slopes ep20–60: **CLK cold 55.8 vs warm 29.5 mm/ep; CB cold 28.6 vs warm 16.1.**
Warm is below cold in 33/46 overlap epochs. The review's slope reading (21 vs 58) and
crossover prediction (~ep33) were correct in direction and conservative in timing.

## 3. Run matrix and gate outcomes

Matrix (50-station GPS smoke, 60 ep, v2 blob, HB-2 protocol, all arms asserted):
A-60, B1, B2-OFF-60, B2-ON-60, B2-RS-60 × {0.05, 0.10, 0.20, 0.50} m.

CLK SD-of-SD vs inline [mm]:

| epoch | cold | warm0 | rs005 | rs010 | rs020 | rs050 |
|---:|---:|---:|---:|---:|---:|---:|
| 15 | 833.9 | 1479.9 | 1479.4 | 1466.3 | 1412.1 | **1169.8** |
| 30 | 1708.0 | 1769.2 | 1765.1 | 1741.9 | 1733.4 | 1839.8 |
| 45 | 2743.4 | 2331.6 | 2315.5 | 2304.6 | 2307.9 | 2480.5 |
| 60 | 3524.7 | 2630.3 | **2610.5** | 2622.2 | 2630.4 | 2863.6 |
| **slope ep20–60** | **55.81** | 30.18 | **29.74** | 30.57 | 31.75 | 37.11 |
| stays below cold from | — | ep33 | — | — | **ep32** | ep35 |

The shock reduces the transient monotonically (1480 → 1170) and monotonically *erodes*
the long-run benefit (slope 30.2 → 37.1; ep60 2630 → 2864). σ = 0.05 m is a no-op, as
the spec predicted (it sits below the carry's own q_z·Δt ≈ 55 mm).

| gate | outcome |
|---|---|
| **G0** structural byte-identity (3 arms) | **PASS** — no-snapshot, snapshot+carry-off, and carry-on/shock-0 restoring the existing **v1** blob are all byte-identical to the pre-card build. Proves reset-first + binding validation + the v2 reader change nothing at defaults. |
| **G1** unit | **PASS** — 884 ran, 883 passed, 0 failed. Five additive gates: v2 round-trip incl. `S`; v1 accepted (legacy); `restoreCarry` clears side-state; unbound-vertex/group blob rejected loudly; `applyResumeShock` touches only the diagonal. |
| **G2** mechanism (informative) | **REVISED — see §4.** |
| **G3** tooling anchor | **FAIL — see §5.** |
| **G4** decisive A/B | (a) **FAIL** at every sweep point; (b) **PASS** at every point; (c) **marginal** (below cold from ep32–35, not ep30); (d) **not graded** (no per-epoch flag telemetry); (e) **not graded** (G3); (f) consistent with exposure. |
| **G5** telemetry | **PASS** — the RE-4 run-end line is present and correct in every arm, including the negative case (`carry restored NO`) on the cold and inline arms. |

## 4. §0.2 mechanism, revised

The spec named two warm-specific channels. The measurement separates them:

- **D5 σ̂₀ warm-up — DISPROVEN in this regime.** The converge blob carries
  `S, σ̂₀² = 0.804, eff_rows = 12266`. But `innovInflationSigma0` clamps the factor to
  **[1, cap]** ("only RAISE the threshold"), so a σ̂₀² *below* 1 clamps to 1.0 —
  precisely the value an unseeded accumulator returns pre-warm-up. **RE-1 is correctly
  wired (telemetry confirms `sigma0 SEEDED 0.8968`) yet numerically inert here**, and
  the transient survives regardless. The gate was never miscalibrated at resume in this
  operating point. RE-1 would only bite where the converged σ̂₀² > 1.
- **Station-layer conduit — remains the surviving explanation.** It predicts the shock
  should *partially* relieve the transient without eliminating it (the tension is real
  data-driven re-convergence, not a gate artifact), which is exactly the monotone,
  partial, benefit-eroding response measured.

Counters (60 ep, whole-run):

| arm | D5 flags | final σ̂₀ | chord births | slip refloats |
|---|---:|---:|---:|---:|
| A inline | 1377 | 1.76 | 3321 | 1695 |
| cold | 1489 | 1.82 | 3307 | 1711 |
| warm0 | 1708 | 1.88 | **2403** | 1972 |
| rs020 | 1678 | 1.86 | 2379 | 1948 |
| rs050 | **1615** | 1.80 | 2321 | 1890 |

Births drop ~27% on every warm arm — the carry is demonstrably doing its job. G4(f):
the warm arms' `slip_kills` excess (+15%) tracks their ~1214 extra chords of early
exposure, consistent with the spec's exposure caveat rather than an event storm.

## 5. G3 — the branch instrument is not valid across runs

BR-3's anchor was expected to be ≈100%. Measured **29.9%** same-integer, median
|Δz| = 4.9 cyc. Diagnosis (via a control the card did not specify — *consecutive epochs
within a single run*, where the branch cannot move):

- within-run, one epoch apart: median |Δz| = **0.29 cyc**, same-integer **68.1%**.
  0.29 cyc is exactly `√(q_z·Δt) = √(1e-4·30) = 55 mm` — the carry's own process noise.
  So the spec's 0.05-cyc "branch-agree" bar sits *below the per-epoch noise floor* and
  measures estimate precision, not branch identity. The metric was corrected to
  **same-integer (`round(Δz)==0`)**, the actual branch question.
- Even corrected, BR-3 (29.9%) is far below the within-run ceiling (68.1%). Cause: the
  serialized `z` is the **correction-frame** DD (`λ·N` minus `Σ c·x_lin`), so it is
  **run-specific**. Two runs linearize differently, and as their solutions diverge the
  frame diverges — which is what BR-4's apparent "decay" (29% → 3.9% over ep15→60)
  actually tracks.

**Consequence:** the carry blob is datum-invariant but *not frame-invariant across
runs*. Per G3, BR-4 is not trusted and **G4(e) is not graded**. Fixing this needs a
frame-invariant functional (serialize `x_lin`, or compare a between-chord difference
that cancels the frame) — a follow-up, not a tuning change.

## 6. Decision-tree ruling

The card's tree: *"Storm survives the whole sweep → §0.2 is wrong; STOP, report, do not
layer RE-3 on top of an unconfirmed mechanism without the user."*

The storm survives every sweep point (G4(a) fails 1.40×–1.77× against a 1.1× bar), and
§0.2's primary channel is disproven. **Ruling: STOP. RE-3 (funnel grace) is NOT built.**

## 7. Recommendation (user rules on shipping)

1. **Retire "counterproductive"** in the handoff (done — banner added). The
   branch-faithful carry roughly **halves** post-transient divergence from inline and
   cuts chord re-births ~27%.
2. **For the dev loop**: `state_snapshot_carry: true`, **no shock** (σ=0 keeps the best
   slope and ep60 level), `state_snapshot_require: true`, and **discard the first ~5
   epochs** after resume. The transient is ~3 epochs wide and fully skippable — which is
   cheaper and more honest than any shock value measured here.
3. **Do not ship RE-2 armed.** It trades long-run accuracy for a transient you can skip.
   Keep it default-0 as an instrument.
4. **Same-snapshot A/B remains the primary dev tool** and is now structurally safe
   (HB-1 + the harness). Validity envelope: knobs inside the resume fingerprint —
   roster/order, datum toggles, carry q's, clock model, decimation, coupling set, and
   `carry_invariant` itself — cannot be A/B'd against a shared snapshot; grade changes
   that interact with the transient from ≥2× the longest adaptive window past resume;
   a snapshot pins one realization, so use it for deltas, not magnitudes.

## 8. Deferrals (invariant #3)

| item | class | condition |
|---|---|---|
| RE-3 funnel grace | **struck** | decision tree forbids it — mechanism unconfirmed |
| frame-invariant branch metric | follow-up, **blocking for any future branch claim** | needed before BR-4/G4(e) can ever be graded |
| per-epoch D5 flag telemetry | follow-up | required to grade G4(d) |
| C1 dense side-car serialization | field-inert | one-epoch gap; revisit only if a future G4(a) attempt with G2 confirming flags are not the driver |
| per-station filter checkpoint | out of scope | full process checkpointing, declined by RTB-7 design |
| MW/IF-TD pool serialization | field-inert | cannot fire at ep 15; cold in both arms |

## 9. Artifacts

Code (default-OFF, uncommitted): `state_snapshot_carry` (blob v2 + `S`),
`state_snapshot_resume_shock_m`, `state_snapshot_require`, `state_snapshot_keep_history`;
`NetworkCarryVce::seedRobustFloor`; `NetworkInvariantCarryDriver::{serializeCarry,
restoreCarry,applyResumeShock,validateCarryBinding}` + resume telemetry; RE-4 run-end line.
Tooling: `scripts/{run_warm_resume_arm.sh, warm_resume_sd_metric.py, diff_carry_blobs.py}`
+ `scripts/README.md`. Configs: `config/test_suite/_rs_*.json`, `_warm_*.json`.
