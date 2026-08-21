# SPEC — the slip-RE-LEVEL level walk (the 60-sta campaign's central defect)

**Session 2026-08-20 ("settle the network processing").  Status: MEASURED,
ISOLATED, and the fix is BUILT + VALIDATED same-day (§8) — the √t walk is
DEAD under `network.cycle_slip.relevel_free_epoch` (+ delta_bridge); the
residual plateau is QUEUE 8(b) territory.  Diagnosis exe `a02717a5`, fix exe
`919bb885`; fixture regenerated post-`1b9e157`.**

The first long (6 h), multi-GNSS (GPS+GAL triple+triple), field-N, truth-graded
network runs with a **realistic natural slip population** (§9.7.9 rate 2e-4,
2 486 truth slips) exposed a defect class every prior validation had missed —
because every prior validation was either **quiet-fixture** (no slips) or
**1 h** (the walk had not accumulated).

## §1 The phenomenon

On `runs/simnet60_campaign` (60 sta + consumer, 720 ep @ 30 s, quiet GIM iono,
W6 biases, elected equatorial datum S30000SIM):

- **720/720 epochs emitted, 0 withholds, invariant-#7 R max 9.8e-05, σ̂₀
  1.0–1.4, wire census complete — every internal instrument silent.**
- Truth grade (settled half): ZWD rms **5.95 m** (hourly 2.6→6.5, √t), clock
  datum-free contrast **14.0 m**, consumer 3D **7.9 m**, per-LOS iono
  interaction **5.26 m** (24-sta quiet reference: 0.67), DD integers per-band
  med|e| 18→33 cyc worsening with settling, EWL round=0 66 %→50 %.
- The walk **onsets in the first carry-warm window** (t+10 min bin: 0.57→2.36 m)
  and is **equatorial-iono-loaded** (S38, lat 13.7°: 36.9 m ZWD rms h5 AND the
  worst iono-interaction station; polar S01: 0.33 m — healthy).
- Parallel symptom: a **T4 isolated-arc refloat storm** — 21 114 arc refloats
  vs ~4 500 slip-driven responses (80–120 carried ambiguities refloated per
  epoch, steady from hour 0).
- Slip DETECTION is good: 94.1 % detected + 3.5 % RAIM-responded of 2 486
  truth slips (`scripts/simnet_slip_census.py`); the 2.4 % silent are all at
  the 10–12° mask edge (arc-end exclusions).  **The defect is not detection —
  it is what the response does next.**

## §2 The elimination ladder (all single-knob/cluster, same fixture, same exe)

| arm | change | ZWD h5 rms | storm | verdict |
|---|---|---|---|---|
| `camp60_sh` (champion) | — (SH posture) | 6.46 m | 7.5 k/h declining | baseline |
| `camp60_gf` | GF-datum slant posture | ~6 (5.08 overall) | **identical (±3 %)** | iono POSTURE exonerated |
| `camp60_shp3` | `raim_arc_persist_epochs: 3` | 6.03 m | **halved — walk UNCHANGED** | storm is CO-SYMPTOM, not driver |
| `camp60_nocpl` | coupling stack OFF | 6.76 m | ramps to 16.5 k/h | coupling INNOCENT (it attenuates churn) |
| `camp60_gps` | GPS-only | 6.78 m (h0 already 4.2) | — | Galileo exonerated (it HELPS) |
| **`camp60_quiet`** | **same posture, QUIET twin fixture** | **0.153 m, CONVERGING** | **GONE (21/h by h5)** | **SLIPS own the whole complex** |

24-sta references on the no-slip injection fixture (q8a arms): ZWD flat at
0.6–0.77 m — consistent with the quiet-twin verdict at 60 sta.

⚠ The `raim_arc_persist_epochs` result is itself a finding: the gate's design
premise ("a model-error spike does not repeat on the same arc") is FALSE for
this error class — the per-LOS iono error is a LEVEL
(`SPEC_t3_null_derivation.md` §7.8), so hot arcs re-trip every K epochs and the
gate degenerates to a ÷K attenuator.

## §3 The mechanism (measured, not inferred)

Every refloat — true slip (~4 500) or false alarm (~19 000) — **re-levels the
arc from a single epoch of code**: the ambiguity is re-seeded from (L−P)-class
combinations and the per-LOS iono from the GF code pair
(`StateInitializer` iono_seed = ((P1−mod1)−(P2−mod2))/denom), then the
ambiguity is FROZEN (`ambiguity_q = 1e-12`) at that value.  The seed error is
metre-class and **station-common across the arcs re-leveled at one station**
(the same local iono-field error enters every seed), so it does not average
out; the stiff ZWD (q 1e-9) and clock absorb the mapping-collinear part
through the documented exchange, and each re-level event adds another step:
a random walk with per-event steps ∝ the station's iono error level.

Measured signatures, all consistent:
- per-station h5 walk amplitude: corr(log interaction rms, log ZWD) = **+0.54**;
  corr with slip count = +0.11 (count is roster-uniform);
- √t growth (2.6→6.5 m over 6 h);
- halving the FA refloats (Arm P) leaves the walk unchanged — the ~4 500 TRUE
  slips are un-halvable and dominate the injected error;
- the feedback loop: refloats keep the network's per-LOS iono at code-noise
  level → the T4 arc statistic (interaction/σ̂₀ > 6) stays hot → more refloats.
  Arm Q breaks the loop at the source (no slips → storm collapses 300×).

**This is standing invariants #1 and #7 in one place: the slip RESPONSE
(reset-over-repair, correct doctrine) has a RE-COUPLING edge that creates a
fresh zero-q quantity from one noisy epoch, with no re-validation and no
bounding of what the reconnected states can inject.**

## §4 Why nothing internal saw it

σ̂₀ stays ~1: the re-leveled arcs are self-consistent with their own wrong
levels (free iono absorbs per-arc, common part hides in ZWD/clock which have
no truth-referenced instrument).  The rank gate counts gauge dimensions, not
information quality.  A4 never fires (σ̂₀ quiet).  Invariant-#7's R counter
watches datum directions, not the leveling datum's VALUE.  The consumer is
AR-confounded and drifts within its own convergence noise.  **Only the truth
fixture sees it — which is the reason the fixture exists (§9.5 finding 2's
"confidently wrong" class, now with a mechanism).**

## §5 What IS healthy (the quiet twin doubles as the first long clean run)

`runs/camp60_quiet` (GF posture + coupling + 1f/1g + full QC, 60 sta, 6 h):
720/720, ZWD rms 0.153 m at h5 and converging, refloats 21/h by h5, orphan
channel 0-class, wire census complete with 720 H lines.  **The estimator is
genuinely good when arcs live.**  Posture facts at scale (from the twins):
the GF pin locks the code channel (B rms 1.39 m / p50 4.9 cm vs SH's 8.3 m);
Galileo makes the network QUIETER (GPS-only arm is worse); the coupling stack
attenuates refloat churn.

## §6 Fix directions (as designed pre-build — §8 records what was BUILT)

The doctrine "reset over repair" stands; the fix is at the RESET's re-coupling
edge:

- **(a) Predicted-phase re-level**: seed the new level against the network's
  model-predicted phase (geometry + current ZWD/clock/iono states), not raw
  code.  The injection becomes filter-consistent; per-arc iono still free.
- **(b) Carry+delta repair where the detector measured the integer**: MW/IF-TD
  produce delta estimates; re-level = carried level + rounded delta, with a
  validation gate (fall back to (a) on disagreement).  Note the reset-over-
  repair memory targeted BIASED repairs; a validated integer bridge is a
  different object — derive, don't assume.
- **(c) Post-re-level vet window**: hold a re-leveled arc's rows out of the
  ZWD/clock information flow (deweight the mapping-collinear content) for N
  epochs until its own level re-converges — the `resume_warmup_vet` shape.
- **(d) At minimum, an instrument**: a run counter for "information injected
  into ZWD/clock by re-leveled arcs this epoch" so the walk is visible without
  truth (the same move as the invariant-#7 R counter).

Grade any fix on: ZWD hourly rms flat ≤ 0.5 m on the SLIPPED fixture at 60 sta
/ 6 h; storm collapse toward the quiet twin's 21/h; slip-census coverage held
at ≥ 97 %; quiet-twin byte-neutrality (the fix must be event-gated).

## §7 Session artifacts

Fixtures: `runs/simnet60_campaign` (slipped) + `runs/simnet60_quiet` (quiet
twin), both `config/sim/simnet60_campaign*.json`, post-`1b9e157`, field-N,
byte-deterministic generator.  Arms: `runs/camp60_{sh,gf,shp3,nocpl,gps,quiet,sh_rerun}`,
configs `config/test_suite/_camp60_*.json`, all on exe `a02717a5`
(md5), elected datum S30000SIM (`datum_gauge_audit.py --fixture`, 10 losses /
min 15 sats / lat −0.9°).  Graders: `scripts/simnet_slip_census.py` (NEW —
organic-slip census; self-test reproduces the §15 bench 22/24 + the S05 G20
+50 cyc all-tier miss), `scripts/simnet_iono_interaction.py` (NEW — §7.8
decomposition promoted from scratch join, + the Table-8 split check).
Related session results recorded elsewhere: QUEUE item 8(a) verdict
(PERSISTS — `QUEUE_principled_replacements.md` §8), the RTA-10 pre-open
H-buffer fix, Card-1f orphan per-family attribution (88 orphans, all PB,
E:39/Σ2.5 m G:49/Σ35.5 m at the champion), CLK-R first sim validation at 60
sta (Σ|μ| drained 229.7 m / max |A_c| 9.67 m / 174 marks, leash margin OK).

---

## §8 THE FIX — BUILT AND VALIDATED (same session, 2026-08-20)

Three knobs under `network.cycle_slip.*`, all default-OFF, byte-identical OFF
(certified: post-fix exe `919bb885` reproduces the pre-fix q8a control
products.txt byte-exactly), fingerprint-EXEMPT (the seventh instance, recorded
in `networkSnapshotFingerprint`'s block).  Suite 1432/1434 green (+8 kernel
tests, `tests/test_relevel_delta_bridge.cpp`).

**The design panel's decisive code fact (found before implementation):** the
pending-reseed mark is CONSUMED by PASS-B's `ensureRequiredStates`
(`MeasurementModel.cpp:566-610` — re-seeds x_lin, clears the flag) BEFORE the
inject check reads it — so §3's "free re-level" was wrong in an important way:
for detector-caught slips the leveling refloat NEVER HAPPENED AT ALL; the
stale pre-slip level injected at converged weight and the T4 arc tier's
delayed erase was the de-facto (and miscalibrated) response path.  The walk
was the stale-injection → residual-tension → T4-cascade → code-re-level loop.

1. **`relevel_free_epoch`** — latch the detector-marked set
   (`RelevelEpochState.free_ambs`, filled beside every mark in the MW/LLI/IF-TD
   drains) where the inject site can still see it.  THE LOAD-BEARING PIECE.
2. **`relevel_delta_bridge`** — float 2×2 (MW WL integer + IF-TD step) solved
   by `networkSlipDeltaBridgeSolve`; shifts the carried level in place (the T0
   repair mutation shape), removes the bands from the free set.  Requires
   free_epoch + mw_detect + if_phase_td; mutex with turbo_edit.
3. **`relevel_retap_statistical`** — T4 isolated-arc refloats keep their level
   at a loosened SET variance (0.25 m²), 3-in-120s streak escalates to erase.
   Whole-sat consensus keeps the strong erase.

### Validation (slipped 60-sta fixture, same exe, single-knob ladder)

| arm | ZWD hourly rms (h0..h5) | T4 arc events | interaction | consumer 3D |
|---|---|---|---|---|
| baseline (camp60_gf) | 2.6 → **6.5 monotone** | 21 114 | 4.61 m | 4.06 m |
| F2 free(+inert bridge) | **2.34 2.34 3.03 3.04 3.09 2.35 — FLAT** | 7 876 | 3.57 m | 3.26 m |
| F3 + fixed bridge + retap | 2.23 2.34 3.07 3.21 3.09 2.40 — FLAT | (24 660 KEPT / 519 escalated) | 3.70 m | ≈F2 |
| quiet twin (reference) | 0.48 → 0.15 converging | — | — | — |

**The √t walk is DEAD — the free-epoch piece alone kills it** (7 141
stale-injections prevented; ZWD settled rms 5.95 → 2.85 m, p50 0.58 m).
Detection coverage unchanged (94.1 % + responded; the ~20 extra "silent" are
slips previously RESPONDED by the FA storm itself).  **The retap is
truth-NEUTRAL here** (F3 ≈ F2 on every channel): after the free-epoch fix an
FA refloat's free re-level already lands near the correct level.  Keep it
default-OFF; its value is churn hygiene, not accuracy (n=1).

### Honest residuals and the follow-on cards

- The ~2.3–3.2 m ZWD plateau (and interaction 3.6 m) is a STATIONARY process,
  not a walk — owned by the per-LOS iono representation under a constant
  arc-reset rate: **QUEUE item 8(b) territory, not re-level mechanics.**
- The bridge reached only 73/2486 slips: my first defaults were mutually
  contradictory (WL σ floor 0.10 cyc × β1/λ_IF = 0.45 > the 0.35 cap — fixed
  to 0.02/0.50), and **MW confirms its integer 1–2 epochs after T1 fires**, so
  the same-epoch joint gate leaves 3 959 candidates unpaired.  Bridge v2
  (pend the pre-slip carry + the T1-measured δIF, REPLACE the level when the
  MW integer lands, void on re-slip/datum doors) is the specified follow-on.
- One bridge applied |ΔN| 199.7 cyc at the 200 cap — audit the delta_bridge
  event rows against truth before trusting large-|ΔN| bridges; the loose
  variance bounds the damage by design.
- Known edge (audit NEEDS-GUARD, accepted by population argument): a KEPT
  retap arc is invisible to AnchorHygiene's slip feed that epoch (it reads the
  pending mark, which the retap deliberately does not set) — an anchor
  reattach could land on a kept arc.  FA-dominated population + loose var
  bound it; revisit if retap is ever promoted.

### Arming guidance

Sim network postures with slips: arm `relevel_free_epoch` + `relevel_delta_bridge`
(the campaign `_camp60_f2/f3.json` shape).  `relevel_retap_statistical` stays
optional.  All inert-and-disarmed under `carry_invariant` (loader interlock at
the end-of-section site — the Card-1g order-hazard pattern).  Field arm owed.

### §8.1 The bridge truth audit (autonomous follow-up, same day) — and the surgery

Joining F3's 73 `delta_bridge` event rows against `truth_slips` (±3 ep window,
correct = both bands within 0.5 cyc of the cumulative truth delta):

| source | correct | wrong | no-truth-in-window |
|---|---|---|---|
| joint (MW integer + IF-TD FLAG) | 13 | 11 | 0 |
| mw_only (MW integer + UNFLAGGED IF sample) | **0** | 33 | 16 |

**The mw_only branch was a wrong-bridge factory and was REMOVED**: an MW
candidate whose IF sample did not flag is dominated by the MW false-alarm
population on a healthy arc — it bridged deltas to ±199 cyc at cm-tight
propagated σ (the propagation carries measurement noise, not hypothesis risk).
The clean-MW complement (`if_clean`) was removed with it: its own σ
(β1·(0.7/3)/λ_IF ≈ 1.06 cyc) can never pass the σ cap, so it was dead as gated
(F3: 0 applications) — and the suite's `MwBlindEqualSlipCase` now PINS that
reject (this closed a latent one-test tally gap: the suite is now a full
1433 + 1 of 1434).  The quiet-twin neutrality arm PASSED: 808 event-gated
actions on the no-slip fixture, hourly ZWD grade equal-to-slightly-better
(h5 0.133 vs 0.153 m) — the response never acts off-event.

**Standing posture after the audit:** `relevel_free_epoch` is the validated,
recommended piece for slipped sim postures.  `relevel_delta_bridge` (now
joint-with-flag only, 13/24 correct) stays a default-OFF research knob — do
NOT arm for graded work until bridge v2 adds corroboration (pend the pre-slip
carry + δIF, REPLACE on the MW integer's arrival, post-fit validation gate).
`relevel_retap_statistical` stays default-OFF (truth-neutral).

### §8.2 Bridge v2 — pend-and-replace (built + validated 2026-08-20, same session)

Architecture: at the T1 flag epoch a **pend** captures the PRE-slip carry (both
bands' value/var — the PASS-A map still holds the previous harvest) and the
measured δIF with its empirical σ; the arc takes the v1 free epoch as before.
When the confident MW wide-lane integer arrives — same epoch via the candidate
index (stored in the pend, replaced at the next epoch's sweep) or 1–2 epochs
later in the MW drain (immediate replace) — the 2×2 solves and the carry is
REPLACED with `pre_slip + shift`, var = `pre_var + bridge var`, gated by the
kernel caps AND the **§8.1 corroboration**: the bridged level must agree with
the interim free re-level within `relevel_bridge_corrob_cyc` (3.0) per band.
Pends expire (`relevel_bridge_pend_window_s`, 120 s), are voided by any fresh
mark on the same (station, sat), and every non-Applied outcome keeps the free
re-level.  Pure replace function `networkSlipBridgeReplace` (unit-tested:
outcome ladder incl. the wrong-integer CorrobReject with carry untouched).
Suite 1434+1 of 1435; OFF-path byte-certified again.

**F4 validation (slipped fixture, free+bridge-v2):** 1 426 pends → 313 solve
attempts → **32 REPLACED**; the two guards did exactly their jobs — the σ-cap
rejected 238 (low-elevation solves whose bridge σ is honest-worthless) and the
corroboration rejected **43 of 75** kernel-passing solves (57 % ≈ v1's
measured ungated wrong-rate).  Truth audit of the 32: **25 correct, 7 wrong,
0 phantom** — and the wrong class COLLAPSED from v1's 6–113 cyc to 0.5–0.8 cyc
float dispersion (worst 3.5), covered by the propagated variance.  ZWD ≈ F2
(pre-registered: 32 events cannot move fleet statistics).

**Verdict:** v2 is correct-by-construction — the corroboration gate converts
the bridge from a wrong-level factory into a safe precision refinement — but
its YIELD is structurally capped on this fixture at ~1–2 % of slips (727 pends
expired awaiting an MW integer that never comes; the σ-cap owns the
low-elevation mass).  **The free epoch remains the load-bearing response;
the bridge stays a default-OFF precision knob, now safe to arm.**  Raising
yield would need a better second equation than MW (e.g. a third-band TD or a
multi-epoch δIF chain) — a new derivation card, not a tuning pass.

### §8.3 Bridge v3 — the better second equation (derived + built 2026-08-20/21)

The derivation card §8.2 asked for.  Design-panel-reviewed (adversarial math
check on the lane algebra); riders `network.cycle_slip.relevel_bridge_gf` /
`relevel_bridge_ewl` on `relevel_delta_bridge`, default-OFF, OFF-path
byte-certified (products + five solve CSVs vs the pre-card exe).

**The rescue accounting that set the scope** (from the F4 teardown + the
fixture's slip geometry — every slip lives below 45° el, rate 2e-4/sin²):

- **E5 (multi-epoch MW accumulation): REFUSED.**  The λ_IF-amplified σ-cap
  needs σ_WL ≤ 0.11 cyc ⇒ single-sample σ 0.246/sin(el) demands el ≥ ~48° —
  above where ANY fixture slip exists; √K over the 120 s window rescues ~0 of
  the 727 expirations, and a ROUNDED belated float at σ 0.25–0.5 cyc is
  5–30 % wrong — mw_only re-imported.  Do not re-litigate.
- **E4 (the same-pair GF-phase TD float row): THE YIELD PIECE.**
  d_GF = λ₁ΔN₁ − λ₂ΔN₂ + (α₂−α₁)δI + ε — geometry/clock-free, undifferenced,
  available AT the flag epoch from the qualified raw verdicts
  (`NetworkRawObservables`), σ EMPIRICAL (the station's own clean-step EWMA
  `gf_step_scale`, which already contains the iono-TD content; floored).
  With E1 (δIF) it closes the (ΔN₁,ΔN₂) plane same-epoch:
  det[β₁ −β₂; λ₁ −λ₂] ≈ −0.0465 m² (GPS) / −0.0485 (GAL); at cm-class inputs
  σ(ΔN) ≈ 0.15–0.3 cyc — float-only, ZERO integer-hypothesis risk.  Whenever
  σ_GF ≲ 0.75·σ_IF the GF row is worth as much as a CORRECT WL integer with
  no wait.  The two measurements share band-TD noise (ρ → +0.97 in the phase
  limit) and the solve coefficients have OPPOSITE signs, so the shared noise
  partially CANCELS: independent propagation OVERSTATES σ(ΔN) up to ~3.7×
  (unit-pinned) — the shipped ρ = 0.6 tightens honestly; over-setting ρ past
  the physical limit is the dangerous direction (understated σ).
  When the MW integer IS present the E4 row over-determines the plane and the
  standardized plane residual screens a wrong detector-rounded integer
  (±1 WL sits 2.5–5σ off the float plane; reject at 4σ) — the hypothesis-risk
  channel the §8.1 audit showed the propagated σ cannot carry.
- **E3 (the EWL-MW integer slot): BAND-3 COVERAGE.**  The MW detector already
  forms per-arc MW on the extra-widelane pairs (GPS L2W−L5Q λ 5.861 m, GAL
  L7Q−L5Q λ 9.768 m; single-epoch σ 0.036/0.022 cyc zenith — instantly
  roundable; wrong∧pass-frac ≈ 2e-5 even at 10°), and slips are independent
  PER BAND in the sim, so a (1,2)-plane bridge alone leaves band 3
  free-re-leveling (a band-1/2 slip refloats band 3 as collateral through the
  (1,3)/(2,3) candidates).  v3 routes one-shared-band candidates into the
  pend's EWL slot and chains ΔN₃ = ΔN_shared + ΔN_EWL, var₃ = var_shared +
  σ_EWL², λ₃ derived from (λ_shared, λ_EWL) alone.  **The sign trap this
  section exists to record**: candidates arrive frequency-sorted, so GAL's
  EWL is (L7Q,L5Q) = ΔN₃−ΔN₂ while GPS's is (L2W,L5Q) = ΔN₂−ΔN₃ — the fill
  helper (`networkSlipFillEwlSlot`) normalizes to ΔN₃−ΔN_shared at fill time
  and both directions are unit-pinned.  Band-3 pre-slip carry is captured at
  the FLAG epoch (a belated capture would read the interim free re-level).
  The v2 frac proxy is NOT reused for σ_EWL (it understates the single-epoch
  EWL noise ~100× at low el — an invariant-#1-class too-stiff inject);
  the floor is elevation-mapped.
- **EWL-only band-3 pends** (a band-3-only slip fires no IF flag → no pend at
  all today, ~830/6 h fully unbridged): DEFERRED to its own increment behind
  positive-evidence admission (a PRESENT below-threshold IF sample — absence
  ≠ silence, the exact mw_only failure), not built in this pass.

Kernel `networkSlipBridgeV3Solve` (staged closed form, no ILS: E2-or-E1+E4
plane, then the EWL chain; FLOAT throughout — rounding kernel output is
forbidden), replace `networkSlipBridgeV3Replace` (the v2 outcome ladder over
up to three bands, per-band corroboration, NO partial apply).  v2 kernel and
its tests untouched; an E2-only pend solves bit-identically through v3
(unit-pinned).  Suite: `tests/test_relevel_bridge_v3.cpp`, 13 cases.
Committed truth grader: `scripts/simnet_bridge_truth.py` (reproduces the
§8.2 scratch audit 25/7/0 on `runs/camp60_f4`; one wrong-row magnitude reads
8.1 vs the scratch 3.5 — a windowing detail on a double-slip row).

**F5 validation (arm `runs/camp60_b3`, single knob-set over the certified OFF
path):** 1 425 pends → **1 053 REPLACED (74 %**, vs v2's 32 = 2.2 %), **0
expired** (vs 727); GF rows captured 1 425/1 425, E1+E4 float-plane applies
1 048, wrong-WL plane rejects 24 (the screen fired), EWL fills 367 same-epoch
→ 109 band-3 replaces (belated fills 0 — the GF route makes pends terminal at
flag+1, as designed); σ-cap 128, corroboration rejects 238.  Truth join
(`scripts/simnet_bridge_truth.py`, exact-epoch window): **892 correct / 161
"wrong" / 0 phantom, worst |err| 2.03 cyc** — the "wrong" class is entirely
float dispersion at the 0.5-cyc tolerance (p50 0.71, p75 0.98), i.e. the
honest σ(ΔN) ≈ 0.15–0.3 cyc tail; **zero integer-class errors at 33× the v2
yield**.  ⚠ Grader lesson baked into the script: at a ±90 s window,
consecutive same-LOS slips graded each other's events "36–112 cyc wrong" in
complementary pairs — 19 such rows, every one a correct bridge; the exact-
epoch window is now the default.  The 15.3 %-beyond-tolerance rate exceeds
the pre-registered 10 %, with magnitude far inside the 2-cyc bound — recorded
as a calibration note (tolerance vs honest σ), not tuned away.  Truth-channel
effect: ZWD 2.862 ≈ OFF, interaction 3.72 — NEUTRAL, exactly as the 8(b)
falsification predicts (level precision is not the plateau's binding
constraint; the bridge re-asserts pre-slip levels, which carry the same
displaced state the free re-level would re-absorb anyway).  **Verdict: the
better second equation is validated as a mechanism — safe, high-yield,
integer-clean — and stays a default-OFF precision knob whose truth-channel
value materializes only after the displacement drain
(`SPEC_iono_carry_8b.md` §6.2) lands.**
