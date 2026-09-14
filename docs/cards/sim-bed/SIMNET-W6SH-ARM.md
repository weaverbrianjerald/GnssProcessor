# W6+SH arm — the W6+SH arm and its minimum-n ladder — the two-band wall is n-invariant, posture is the 16× lever

**STATUS**

- **id:** W6+SH arm
- **topic:** sim-bed
- **status:** closed
- **summary:** the W6+SH arm and its minimum-n ladder — the two-band wall is n-invariant, posture is the 16× lever
- **dates in the text:** 2026-08-12
- **knobs / posture:** `network.rank_gate_tol`, `network.cb_kaula_prior`, `network.trace_sat_bias_states`, `network.trace_rx_states`
- **hashes named in the text** (commit / exe / byte-cert): `6bf857bb`, `521512b6`, `7aaa21eb`
- **run dirs named in the text:** `runs/simnet300_wsh`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` — §12, 12.1, 12.2, 12.3, 12.4, 12.5, 12.7, 12.6

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §12 The W6+SH ARM — built, and its minimum-n ladder measured (2026-08-12)

Everything here was run; each claim names the arm that produced it.  Release only,
`gnss_processor_wsh0.exe` (sha256 `6bf857bb…`) for the ladder rungs and
`gnss_processor_wsh.exe` (sha256 `521512b6…`, the same tree plus the §12.4 traces) for the
trace A/B, one CWD per arm.

### 12.1 What was built

| piece | file | what it is |
|---|---|---|
| producer template | `config/test_suite/_simnet_wsh_template.json` | the W6+SH posture: `estimate_iono_sh` (which FORCES the whole coupled slice), 25° iono cutoff, SH degree 15, `sh_health_gate`, `if_phase_td` + `datum_station_detect`, `trace_estimability`, the two new σ traces, equatorial clock datum |
| 300-station fixture | `config/sim/simnet300_wsh.json` → `runs/simnet300_wsh/` | 300 solve + 1 consumer, 480 epochs = 4 h @ 30 s, **`corrections.signal_bias: true`** (the injected OSB IS the B-record truth), quiet, pinned storm anchor |
| roster generator fix | `scripts/simnet_make_roster.py` | 9-char markers past n = 99 (`S00101SIM`, scheme `wide3`); the old scheme emits a TEN-character name at n = 100, not a RINEX long name at all.  Legacy rosters regenerate byte-identically |
| per-arm products / prefix | `scripts/simnet_make_producer_config.py` | `--products`, `--epochs`; plus a LOUD warning when the clock datum falls back to `stations[0]` (on a Fibonacci roster that is the most POLAR station, and §9.8 measured ~8× more common-level wander from a polar datum) |
| B + receiver channels | `scripts/simnet_grade.py` | §4.4's two-stage S-transform applied to TRUTH (IF lump over the S line's own `cbpin` pair, then zero-mean per channel over the epoch's registered set), vf from the new σ trace; receiver DCB graded as a CONTRAST with the level reported as the absorbed Table-8 gauge |
| exchange decomposition | `scripts/simnet_exchange.py` | §11 item 4: the clock / receiver-DCB / a₀₀ common-mode series, their correlation matrix (series AND first differences), and the measured-vs-predicted exchange coefficient |

### 12.2 The ladder — four rungs at n = 50, 20 epochs each (~90 s per rung)

The 50-station G-A fixture is a valid EMISSION probe for the W6+SH slice even though it
was synthesized in the INT posture: rank structure does not depend on the injected bias
VALUES.  The whole ladder cost about six minutes.

| rung | one knob | epochs emitted | what it establishes |
|---|---|---|---|
| 1 `_simnet50_wsh_probe` | — (25° cutoff, deg 4) | **0 / 19** | posture bundle **3/3 armed**, so the §9.7.2 finding-5 withhold is **not** the elevation-cutoff artefact |
| 2 `_simnet50_wsh_eq34` | `phase_datum_eq34` | **0 / 19** | ~~the field's Eq-34 phase datum does **not** lift it — the datum is not the lever~~ **SUPERSEDED §12.8.2**: the datum DOES lift it (rank gate passes); all 19 withholds were the SH gate — a₀₀ −15 965 TECU at its first epoch |
| 3 `_simnet50_wsh_sbc` | `sbasis_complete` | 1 / 19 | gate PASSES and the **ionosphere is destroyed** — see below |
| 4 `_simnet50_wsh_dgtol` | `network.rank_gate_tol: 1e-12` | **19 / 19** | emits, and the solve is healthy |
| 5 `_simnet300_wsh_smoke` | **300 stations** (deg 15) | **0 / 20** | **station support is NOT the wall** — the n-invariance below |
| 6 `_simnet300_wsh_n50_carry` | `phase_datum_eq34` + `carry_invariant` | **0 / 19** | the carry cannot lift it COLD, and cannot warm through a withhold — the withholding gate is the SH gate, not the rank gate (§12.8.2) |

**The direction, named.**  Rung 1's dump (`network_estimability.csv`, `reduced_N_ss`
143×143, `declared_dim` 1, λmax 1.0283e+05 ≈ 1.03e+05, cutoff 1.0283e-04) shows exactly
two directions below the cutoff:

| ev | composition | reading |
|---|---|---|
| 6.94e-12 | `CLK\|G08…` uniform 0.1768 = 1/√32 | the declared clock gauge ✓ |
| **9.78e-06** | `PB\|*\|L2W` uniform ≈ 0.258 | **the L2W satellite phase-bias common mode** — stable to four digits across every epoch |
| 2.48e-02 | `PB\|G13\|L2W` + `PB\|G13\|L1C` | the weakest REAL layer |
| 2.30e-01 | `SH\|1\|0\|C`, `SH\|0\|0\|C`, … | the SH block, healthy |

With float ambiguities a satellite-common L2W phase bias is absorbable by the L2W
ambiguities, so this is a **gauge the declared dimension (= #constellations = 1) does not
count**, 5.9 decades above the numerical gauge and 3.4 below the weakest real layer.  The
standing record predicted it in words — *"'dual'/'single' leaves the secondary-band
(L2W/L5Q) phase-bias common mode under-determined at low station count"* — and this is it,
isolated and quantified.

**★ It is n-INVARIANT, and that reframes the whole brief.**  The 300-station arm (rung 5)
reproduces the identical spectrum: λmax 1.0283e+05 (≈1.03e+05) → 9.1255e+04 (≈9.13e+04),
ratio 0.89; clock gauge 6.94e-12 → −3.44e-10 (numerical zero); **the extra direction
9.78e-06 → 8.97e-06, ratio 0.92**; weakest real layer 2.48e-02 → 1.69e-02, ratio 0.68 —
every real layer moves with the data and the extra direction does not.  **Six times the
stations added no information to that direction**, which is what distinguishes a structural
gauge from a weakly-determined parameter and confirms the mechanism: more stations bring
more *ambiguities* as well as more data, so the absorbability is untouched.  The record's
closing clause *"at low station count"* is therefore wrong — it is not about station count
at all.

Rung 6 closes the remaining cheap hypothesis.  `carry_invariant` (the DD-chord time link,
the one mechanism that could stop the ambiguities absorbing the bias) is **refused by the
loader without `phase_datum_eq34`** (the cycle basis is defined only on the eq34 chain
datum), so the pair is the correct test — and the pair still emits nothing cold.  It also
cannot bootstrap: the carry needs emitted epochs to warm and the gate withholds them, the
degrade-during-withhold deadlock this project has already met once.  ⚠️ **The withholding
gate is the SH gate, not the rank gate — §12.8.2.**

**Rung 3 is the most important rung, and it is a WARNING.**  `sbasis_complete` constrains
the extra direction along its own eigenvector *at the carried level*; at epoch 2 that level
is cold and the tension went straight into the field — **a₀₀ = −6292 TECU, rms_all 3541
TECU**, whereupon the SH health gate withheld 18 of 19 epochs.  The rank gate was satisfied
and the answer was garbage: the fixture's whole reason for existing, reproduced on a knob.
The datum-station slip at TOW 302430 is not the cause — rung 4 sees the same slip (G09 at
S26000SIM, 28 ambiguities reseeded across 49 stations) and stays clean.  **Do not arm
`sbasis_complete` on a cold start in this posture.**

**Rung 4 is healthy, not merely emitting**: a₀₀ 31.55 → 31.59 TECU across the arc (σ 1.9 →
0.50), in band, per-degree rms stable; post-fit σ̂₀ **0.962 code** / 0.024 phase (the phase
pool is structurally ~0 with free ambiguities — expected and documented); consumer
converging.  Its tolerance was placed from this config's OWN dump as the BeiDou card's rule
requires — cutoff 1.03e-07, 4.8 decades above the gauge depth and 2.0 below the weakest
real layer — **never globally**, since `assertNullSpaceRank` throws in both directions.

### 12.3 What the ladder does and does not settle

- **50 stations are enough to run the W6+SH slice** on this fixture, making the graded arm
  ~6× cheaper than §11's recommendation assumed — and scaling to 300 buys nothing here, so
  §11 item 1's recommendation should not be followed.
- It did **not** settle *why the field emits at 303 with the default tolerance* — three
  candidates remained, each a ~90 s one-knob rung: **(a)** warm resume
  (`state_snapshot_carry`, so the rung-6 deadlock may never be entered); **(b)**
  `network.cb_kaula_prior` (σ 3 m), which the BeiDou card measured LIFTING near-null
  directions, armed in the field and absent here; **(c)** channel diversity the two-signal
  sim does not have.  **All three resolved in §12.8.3** (and the premise itself corrected:
  rungs 2/6 were SH-gate, not rank-gate, withholds — §12.8.2).
- **A rung that emits is not a rung that is right.**  Rung 4 estimates a direction that may
  be a genuine gauge; if so its emitted L2W phase-bias common mode will wander with an
  honest large σ — a graded question for the settled arm (the P channel +
  `emit_phase_bias_sigma`), not something the emission count says.

### 12.4 The §4.8 export gap is CLOSED (both halves)

`network.trace_sat_bias_states` → `network_sat_bias_states.csv`
(`week,tow,sat,signal,code_bias_m,sigma_m,s0,pinned`) and `network.trace_rx_states` →
`network_rx_states.csv` (`week,tow,station,kind,key,value_m,std_m`).  Both trace-only,
default-OFF, **no product grammar change** — the B record still carries no σ, so no
consumer fingerprint keyed on the header legend moves.

The satellite trace reuses the URA/combined-σ covariance treatment verbatim (gauge nulling,
datum-pin skip, positive-variance guard, σ̂₀ scaling) so a vf from it is comparable with a vf
from the U record; a datum-pinned column is written with an EMPTY σ and `pinned=1` rather
than its gauge-artefact variance.  The receiver trace follows the `network_zwd.csv` pattern
(`reconstructStation` + `conditionalLocalVarianceDiag`).  **Invariant #6, same-build A/B**:
`_simnet50_wsh_tron` vs `_simnet50_wsh_troff`, one knob pair, 19 epochs — `products.txt`
**byte-identical** (sha256 `7aaa21eb…` both), no files created when OFF.

First measurements (rung-4 posture, 19 epochs — **ignition, not verdicts**): 64 satellite
rows/epoch with σ 0.52 m → 4.5 mm across the arc; the receiver DCB is one `GPS_DCB` state per
station (not per code, so the comparable truth is the injected GF difference) with a
conditional σ of **~32 m** against a ±0.5 m injected spread — almost pure gauge, precisely
what Table-8 says.

### 12.5 Pre-registered predictions for the settled arm

Written before the graded run, per §6.  **All three resolved in §12.7** — this stub is
kept for the pre-registration record and because P1's wrong sign is a load-bearing lesson.

1. **P1 — the exchange.**  Pre-registered **−7.93 TECU per metre** of fleet-mean
   receiver-DCB error (reasoning: absorbing a positive receiver GF bias requires a negative
   ionosphere; γ = 1.6469, 0.1624 m/TECU, obliquity 1.2), predicting strong NEGATIVE
   correlation of `rxdcb_common` with `a00_err`.  ⚠️ **THE SIGN IS WRONG — do not re-adopt
   this number as written.**  Derived from the model's own coefficients the value is
   **+7.93** (§12.7, §14.6).  The correlation half was confirmed.
2. **P2 — the CB variance factor.**  Pre-registered **vf > 2** (optimistic σ), because
   every other channel on this fixture is optimistic (C vf 455, ZWD 1.63 at n = 50
   settled).  Answered §12.7: **≈ 4.9e+05**.
3. **P3 — the level.**  a₀₀ ran ~3.7 TECU BELOW the network-sampled truth mean in rung 4's
   ignition window, the same direction as the field's standing DEFECT 2.  Pre-registered:
   the settled offset stays negative and shrinks, graded time-matched against the truth
   field's own moving mean, never a single number.  Confirmed §12.7 (−1.42 TECU settled).

### 12.7 THE GRADED ARM — 466/480 epochs, and all three predictions resolved

`_simnet300_wsh_n50_pin` — 50 stations (stride-6 of the 300-station W6 fixture), 480
epochs = 4 h, `rank_gate_tol` 1e-12 placed from this arm's own dump, graded on the settled
half (233 epochs).  (The T2/T3 rungs of §12.8.3/§12.9 all extend the same base config
`_simnet300_wsh_n50.json`.)  **466 emitted, 14 skipped — all 14 by the A4 σ̂₀ ceiling, ZERO
by the observability gate.**  The SH field never left its band, so the health gate is
untested here and the run-end line says so.

| channel | settled statistic |
|---|---|
| **B (satellite code bias)** | residual rms **1.144 m** (C1C 0.841, C2W 1.382), mean −0.004 m |
| B datum realization | emitted per-channel mean **−3.6e-03 m** — the sum-to-zero datum is real |
| **B variance factor** | **489 286** over 14 614 rows |
| C (satellite clock) | residual rms 1.025 m, datum-free contrast 0.889 m, vf 5 684 (s0 = 2.075) |
| P (phase bias) | SD frac L1C 0.266 cyc, L2W 0.318 cyc |
| rx DCB contrast | rms 1.540 m (level −0.240 m = the absorbed Table-8 gauge) |
| rx clock contrast | rms 1.928 m |
| ZWD | residual rms 0.541 m, vf 2 301 |

**P2 — ANSWERED, and not subtly.**  The emitted satellite code bias carries **1.14 m rms of
true error** while its formal σ (median ~4.5 mm settled) claims millimetres: vf **~4.9e+05**,
σ optimistic by ~700× *in σ*.  So the field's "cb vf ≈ 10" is nowhere near the estimator's
actual calibration on truth — whatever the field census measures, it is not the code-bias
channel's true error budget.  §9.5 finding 2's formal-σ-collapses-while-true-error-grows
pattern holds on the code-bias channel too, and harder.

**P1 — the exchange is CONFIRMED, and my pre-registered SIGN was wrong.**  Settled
correlations (226 joint epochs): clk_common ↔ rxdcb_common **−0.939** (first difference
−0.009); clk_common ↔ a00_err **−0.848** (+0.149); rxdcb_common ↔ a00_err **+0.853**
(−0.467); cb_rms ↔ a00_err **−0.832** (+0.010).  **The exchange lives entirely in the slow
drift** — every level correlation 0.83 to 0.94, every first-difference correlation ≈ 0.  The
four channels do not share epoch-to-epoch noise; they share a wandering common direction,
exactly the shape the field campaign inferred and could not watch.

Coefficient: measured **+5.67 TECU per metre** of fleet-mean rx-DCB error (corr +0.853)
against a **derived +7.93** — 0.72× the ideal-absorption value.  ⚠️ **SUPERSEDED ON THE SAME
PRODUCTS by the current `simnet_exchange.py`: +7.69 (0.97×)** — quote 0.97×, not 0.72×
(§12.9 script-version note, §14.6).  Either way most but not all of the receiver-DCB common
error is traded against the field (the ionosphere is also held by its own data and its
Kaula prior).

⚠️ The **sign** in §12.5's P1 was wrong, and the correction matters more than the number: it
must be *derived from the model's own coefficients*, not argued.  The receiver DCB enters
the two reference-band code rows with `mu1/dmu` and `mu2/dmu`
(`StateInitializer::resolveReceiverBias`, mu1 = 1, mu2 = γ = 1.6469, dmu = −0.6469), so on
P1 − P2 its signature is exactly **+1 · x_DCB** against the ionosphere's
**(1 − γ) = −0.6469**, giving Δι = **+1.546** m of L1 per metre of DCB error — POSITIVE.
The **second** bias sign convention to bite in one session (the first: the absorbed IF lump
in the C channel, slope −1.08 / corr −0.78, opposite to §4.2's written `+`).  **Rule for
this fixture: never assert a bias sign from a verbal argument — read the coefficient off the
model, or measure the slope and say which you did.**

**P3 — directionally confirmed.**  a₀₀ runs **−1.42 TECU** below the network-sampled truth
mean on the settled window (σ 0.77) against −3.68 TECU in the 19-epoch ignition window:
negative and shrinking with settling, exactly as predicted.  Graded against the truth
field's own per-epoch coverage-weighted mean, never a single number — the trap §6 of the
level card names.

### 12.6 What is still owed

DISCHARGED (pointers, not re-argued): the 300-station arm at DEFAULT tolerance → rung 5,
**0/20**; a ≥ 2 h settled graded run of rung 4's posture → **§12.7**; the 15↔25
elevation-cutoff A/B WITH truth (§11 item 4) → **§12.9** — the truth grade INVERTS the
field verdict on this coherent fixture (15° wins every clock-adjacent channel 1.3–4×; the
cut FEEDS the exchange, 0.50× → 0.97× of ideal absorption; the field's 25° adoption stands
on field evidence, its separability cost is now measured); "why the FIELD emits at 303
with the default tolerance" → **§12.8.3** (the eq34 chain lifts the near-null, audited
complete; the sim's non-reproduction is its phase−code alignment convention, not rank;
`cb_kaula_prior` **FALSIFIED** alone AND paired with eq34).

STILL OWED:

- The `sbasis_complete` failure deserves its own note in the SBC card: a cold-start engage
  can inject a carried level that no rank instrument sees.
- The exchange decomposition should be re-run on a **datum-station A/B** (§9.8's strongest
  lever): if the clock↔rx-DCB −0.94 is really the datum station's own error propagating,
  moving the datum should move the whole signature.

