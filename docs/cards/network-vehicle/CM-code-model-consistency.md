# CM: code-model consistency, eleven cold arms and the row-level read

**STATUS**

- **id:** CM
- **topic:** network-vehicle
- **status:** closed
- **summary:** No estimation-side knob names the field golden's code inconsistency (eleven cold arms clear them all); the a-priori code model is right and the station block is near-rank-deficient at ignition.
- **dates in the text:** 2026-09-14
- **knobs / posture:** field golden (`carry_invariant` vehicle) as control A0, one knob per cold 2 h arm: `network.carry_rx_code_bias`, `network.estimate_sat_code_bias` (with `model_corrections.enable_code_phase_bias`), `network.estimate_iono_sh`, `filter.process_noise.sat_code_bias_q`, `network.cb_kaula_prior`, `network.carry_zwd` + `network.zwd_carry_q`, `estimation.zwd_constraint`, a three-station roster (A8), `estimation.global_vtec_sh.representation_sigma_tecu`, `estimation.global_vtec_sh.max_degree`, `network.external_sat_code_bias` (A9); trace `network.trace_postfit_rows`
- **hashes named in the text** (commit / exe / byte-cert): worker commits `c416761`, `dd1d2c4`, `98c16e1`, `9247d92`; exe `13c1d7ef…`; smoke `87a33bf0…`
- **run dirs named in the text:** `runs/_cm_a0` to `_cm_a5`, `runs/_cm_a10` / `a10b` / `a11`, `runs/_ci_decomp_net` / `_net2`, `_ci_decomp_phasedw` / `_ctl20`
- **source:** `docs/archive/SPEC_event_harness.md`, sections 24.8, 24.9

**Outcome.** The card asked whether one estimation-side knob explains the metre-level code inconsistency of the field golden. No arm crosses the three bars: closing the per-epoch ZWD sink diverges the solve at epoch 102, and a three-station network halves the code rms (1.66 m) while deepening the sink. The row-level read finds the a-priori model right, a WROC ZWD of −17.8 m at epoch 1 and 47 of 130 rows uncheckable. The vehicle test ([PV](../network-vehicle/PV-vehicle-test.md)) later halved every bar on the leveling vehicle without crossing one, and the [network re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) froze that posture.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.8 Card CM — CODE-MODEL CONSISTENCY: single-knob COLD arms that name the mechanism (pre-registered before any arm is read)

Six arms from the field golden, 2 h cold from 00:00 (240 epochs; the fingerprinted knobs must cold-start), one knob each
against the control, every arm with `network.trace_postfit_rows: true` (trace-only):

| arm | knob | what it removes |
|---|---|---|
| A0 | control (the golden + the trace) | — |
| A1 | `network.carry_rx_code_bias: false` | the receiver code-bias carry (the frozen first-epoch memory; §21.18's gate) |
| A2 | `network.estimate_sat_code_bias: false` + `model_corrections.enable_code_phase_bias: true` | W6 — CODE's OSB replaces the estimated satellite code biases (the consumer L4's satellite side) |
| A3 | `network.estimate_iono_sh: false` | the SH-VTEC tie — the per-LOS ionosphere floats at `ionosphere_q` |
| A4 | `filter.process_noise.sat_code_bias_q: 1e-4` | the frozen satellite code-bias states (q 1e-10 → 1e-4) |
| A5 | `network.cb_kaula_prior: false` | the 3 m Kaula prior on the code biases |

A knob the loader refuses on this lineage is reported with the loader's line and the arm dropped, never worked around; a knob
that forces others (W6's forcing of `estimate_sat_biases`, `promote_sat_code_bias`, `estimate_ifb`) is recorded with the
effective posture from `gnss_config.log`.  **Reads per arm, last hour (k 121..240):** (i) per-station code residual rms p50
across stations (`network_pf_keyed.csv` STA; control ≈ 4.5 m); (ii) per-station ZWD absolute p50 and the count of stations with
|ZWD| > 0.5 m (`network_zwd.csv`; control 7 stations beyond 5 m); (iii) the wire-vs-CODE census on the arm's `products.txt`
(`scripts/wire_vs_code_census.py`, hour 1: within-arc rms of D_phase and D_code, the raw clock contrast LEVEL p50; control
2.8–5 m / 1.3–1.9 m / 3–6 m); (iv) the per-LOS level SS share and its dispersive fraction (`scripts/pf_rows_attrib.py`); (v)
availability (emitted / withheld) and the run-end counters.  **Bars (frozen):** an arm NAMES the mechanism when it takes (i)
below 1.0 m AND (ii) to zero stations beyond 0.5 m AND (iii) D_phase within-arc rms below 0.5 m in hour 1 — all three; a
partial mover (any bar halved) is reported as a contributor; no bar moved ⇒ that mechanism is exonerated.  Two or more arms
crossing the bars ⇒ the fix card combines them and is graded on the same three plus the consumer's convergence (§24.3's
instrument, now with a warm consumer).  Availability must not fall below the control's by more than 5 %; an arm that holds the
bars by withholding is not a pass.

**§24.8 first six arms READ (2026-09-14 ~06:45; worker c416761 / dd1d2c4, `runs/_cm_a0`..`_cm_a5`, exe 13c1d7ef… = main's Release rebuilt after the
rows merge; both smokes 87a33bf0…).**  No single knob names the mechanism and no bar is halved: last-hour per-station code rms p50 4.54–4.79 m,
49–56 of 61 stations with |ZWD| > 0.5 m (NIUM −38 m, CAS1 −20, COCO +20 in the control), D_phase within-arc rms 1.6–3.1 m, per-LOS level share 82–83 % with the
dispersive fraction 15–16 %, availability 240 / 242 in every graded arm.  A1 (rx-CB carry off), A4 (sat code-bias q 1e-4) and A5 (Kaula prior off)
are the control to the digit; A3 (SH tie off) is the largest sub-half mover (D_phase 2.74 → 1.62, seven fewer ZWD stations) and its effective posture
is W6 with the external GIM in place of the SH field, not a free ionosphere; A2 as written is INERT (the loader forces `estimate_sat_code_bias` back
on under `estimate_iono_sh`, `GnssConfigLoadNetwork.cpp:2830`); the EXT-CB realization (A2x: CODE's .BIA, no satellite code-bias state) collapses the
SH field to −83 TECU in one epoch and withholds 239 / 240 emissions — the W6 columns absorb something real rather than causing it.  So the
inconsistency lives upstream of every code-bias and ionosphere knob.  **Pre-registered extension (before any arm is read): the two things every arm
shares and the converging standalone PPP does not** — the per-epoch white ZWD sink and the multi-station coupling itself:

| arm | knob | what it tests |
|---|---|---|
| A6 | `network.carry_zwd: true`, `network.zwd_carry_q: 1e-8` (a physical random walk; the block form `carry_zwd_in_block` only if the loader requires it) | the ZWD as a per-epoch sink: time-linked, it cannot absorb tens of metres |
| A6b | the same with `zwd_carry_q: 1e-7` | the stiffness axis |
| A7 | `estimation.zwd_constraint` (the pseudo-observation toward the a-priori, σ 0.2 m) if it exists on this path | the same, as a prior instead of a time link |
| A8 | the golden's station list cut to WROC00POL + GOP600CZE + FFMJ00DEU (+ the consumer), everything else unchanged | the multi-station coupling: three well-behaved stations 130–300 km apart |

Reads and bars as §24.8; for A8 the bars are read on its three stations and the wire census on its products (a three-station SH field is
Kaula-held; report the a00 series).  Frozen reading: A6 / A6b / A7 crossing the bars names the ZWD sink as the mechanism (the code
model is right, its parameterization let the ZWD eat the inconsistency); A6 moving the metres INTO the code residuals with a
1/sin(el) station pattern names the inconsistency as tropospheric in shape (then the a-priori tropo model of the producer path is the
suspect); A8 crossing the bars with the full-network posture names the coupling (datum, pins, the global field) and A8 failing them
names the per-station model itself.

**§24.8 extension READ (2026-09-14 ~07:40; worker 98c16e1 / 9247d92).**  A6 / A6b (ZWD carry q 1e-8 / 1e-7): the carry pins the cold sink value
(WROC −9.7 m at epoch 8, σ 0.45 → 0.09 m) and the solve DIVERGES at epoch 102 (a00 → 698 TECU by epoch 125, 136 of 242 emissions withheld, ZWD
+65 km at POTS by hour 2) — the per-epoch sink is load-bearing: it absorbs a real, metre-level, station-common inconsistency EVERY epoch, and
closing it without removing the source breaks the solve.  A7 (`estimation.zwd_constraint`) is byte-identical to A0 — the knob is read only by the
RT engine, not by any network seam.  A8 (three stations WROC / GOP6 / FFMJ, datum WROC): code rms p50 **1.66 m** (halved), declared-arc D_phase
**0.32 m** (under the bar; tracking arcs 2.20 — 120 declared re-realizations per hour carry the motion), and the same three stations' ZWD sink
DEEPENS to −7.0 / −3.3 / −6.2 m (A0: −1.8 / +0.7 / +3.7).  **The inconsistency is present at epoch 1 (WROC ZWD −9.2 m before any carry, datum or gate
acts), invariant to every estimation-side knob and to network size, and absent in a standalone PPP of the same files.**  No arm pushes a
tropo-shaped (a + b/sin el) pattern into the residuals (0.1–10 % in every arm).

**What is left standing, and the pre-registered second extension.**  The standalone PPP differs from the network's station model in exactly two
things the arms have not touched: it applies CODE's OSB to its code rows and it estimates the per-LOS ionosphere FREELY (`ionosphere_q` 1e-2, no
field), whereas the network ties every per-LOS ionosphere to a degree-8 global SH field within `representation_sigma_tecu` 5 (0.8 m on L1) and
estimates its own satellite code biases against that field's datum (A2x showed the two are one object: remove the code-bias states and the field
collapses).  A representation error of tens of TECU at an isolated site (the field cannot follow the real VTEC over an ocean or a pole; the
"level" cards measured −4…−15 TECU even in the mean) leaves, after the tie, a residual whose dispersive part the tied deviation state cannot fully
absorb and whose band-MEAN part is mapping-shaped and station-common — which is what the ZWD sink takes (10 TECU ≈ 2.4 m band-mean at zenith,
the right sign and order), the leftover being a per-LOS level that no longer looks dispersive.  A3 (the tie replaced by the GIM) was the one
mover.  Arms (cold 2 h, one change each vs A0, the same reads and bars, plus the cold consumer's 3D at 1 h and 2 h):

| arm | change | what it tests |
|---|---|---|
| A10 | `estimation.global_vtec_sh.representation_sigma_tecu: 50` | let the per-LOS deviation absorb the representation error (the tie 10× looser) |
| A10b | the same at 20 | the stiffness axis |
| A11 | `estimation.global_vtec_sh.max_degree: 15` (+ `layer2_max_degree` 8) | a field that can follow the VTEC |
| A9 | `network.estimate_iono_sh: false` + `network.external_sat_code_bias: true` (+ `estimate_sat_code_bias: false`, `model_corrections.enable_code_phase_bias: true`) | the PPP posture at the station level: CODE's OSB and a free per-LOS ionosphere — if the loader keeps the field on, record it and drop |

Frozen reading: A10 / A10b crossing the bars names the tie's stiffness as the mechanism and the fix is a posture change with the level
instruments re-graded; A11 crossing them names the field's resolution; A9 crossing them while A10 / A11 do not names the coupled code-bias +
field datum (W6) itself; none crossing them exonerates the ionosphere representation and the remaining suspect is the modeled observable
of the network path (the row-by-row modeled-value diff the worker proposed).

**§24.8 second extension READ (2026-09-14 ~13:10; arms run by the worker before its session limit, graded by the manager with the merged
`scripts/cm_grade.py`, `runs/_cm_a10 / a10b / a11`).**  A10 (`representation_sigma_tecu` 50), A10b (20) and A11 (degree 15 / layer-2 degree 8)
are the control on every bar: code rms p50 4.75 / 4.73 / 4.67 m, 58 / 58 / 57 stations beyond 0.5 m of ZWD (NIUM −39, CAS1 −20, COCO +19 in all),
D_phase declared within-arc rms 2.47 / 2.59 / 2.85 m, per-LOS level share 81.7–82.0 %, availability 240 / 242, tropo-shaped fit share 1.3 %; a00
sits at 23 / 26 / 30 TECU (the looser tie lowers the level, nothing else moves); the cold consumer at 90–120 min reads 7.8 (A0) / 9.3 / 9.0 / 8.6 m.
A9 (free per-LOS ionosphere + CODE's OSB, the PPP posture at the station level) is NOT GRADABLE on this producer: 0 of 240 epochs emitted, every
epoch withheld by the observability gate on an undeclared machine-zero null direction (smallest eigenvalue −8e-11 against a declared datum
dimension of 1).  **The ionosphere representation is exonerated.**  Eleven arms have now cleared every estimation-side knob — the code-bias
states and their priors, the receiver code-bias carry, the SH tie, its stiffness and its degree, the ZWD sink (load-bearing, not causal), the
network's size — and the inconsistency is in the code rows at epoch 1 of a three-station solve.  What remains is the row itself: `y = observed
− modeled` as the network path builds it against the same `PppModel::computeModels` the converging PPP uses (`ApplicationNetworkAssembly.cpp:1308`).
The next read is the producer's own per-row `CODE-DECOMP` trace (`MeasurementModel.cpp:1740`, trace level: observed, predicted and every model
term per row) on a four-epoch three-station network run and on the WROC standalone PPP, diffed row by row at the first epoch.

### 24.9 The row-level read (2026-09-14 ~14:30, manager): the a-priori model is right, the code rows do not ask for the ZWD the solve reports, and the station block is near-rank-deficient at ignition

`runs/_ci_decomp_net` / `_net2` (the A8 three-station posture, 4 epochs, `measurement_model` at trace level; the rx-state / iono / rows
traces) and `_ci_decomp_phasedw` / `_ctl20` (20 epochs, phase σ 0.5 m vs 2 mm).

**The a-priori pseudorange model is right.**  At WROC's first epoch the engine's `predicted` for every code row agrees with an INDEPENDENT
model (the ITRF position, the SP3 with light time and Earth rotation, CODE's 30 s clock, the relativistic periodic term, Saastamoinen ZHD at
the geodetic height) to the ~2 m the independent model omits (the satellite antenna offset).  The pre-fit innovations are +2.3…+5.0 m on C1C
and +4.1…+6.9 m on C2W (the station clock's and the bias seeds' linearization) and their ionosphere-free combination spans −0.7…+2.1 m with
NO elevation trend (G15 at 23°: −0.25; G30 at 73°: +0.79) — a −18 m ZWD would need a 28 m spread between them.  The post-fit code residuals
are 0.3–0.55 m rms per station at epochs 1–4; the row weights are normal (σ 0.31–0.77 m code, 2–3 mm phase).

**And yet the ZWD reads −17.8 ± 4.5 m at epoch 1 and converges to −19.6 ± 1.1 m by epoch 20** at WROC (GOP6 −9.9 → −11.5, FFMJ −7.1 → −9.5; σ
falling as σ₀/√n).  The phase-deweight diagnostic (§24.10's first arm, phase σ 0.5 m — a code-only solve) leaves it at −14.5 → −18.0 m (FFMJ
−4.5 → −5.2, GOP6 −6.6 → −7.2) with the code residuals SMALLER (0.26–0.39 m): **the sink is driven by the code rows' STATE STRUCTURE, not by
the phase rows.**  What `network_zwd.csv` writes is `x_lin + x_r(zcol)` with `x_lin ≡ 0` (`NetworkIonoDiag.cpp:139`; the station's tropo
state is a correction about zero) and `x_r` the local correction RECONSTRUCTED from the shared solution (`est.reconstructStation`), its σ the
conditional local variance.  A formal σ of 4.5 m for a column whose partial is the wet mapping function against ten code rows of σ 0.4 m is
not possible (it would be ~0.1 m): the ZWD column's effective information in the reduced station block is ~1/50 of that, and the
redundancy census of the same epoch says why — **47 of 130 rows (36 %) are UNCHECKABLE (redundancy ≤ 1e-6)**, i.e. fully absorbed by free
local parameters.  With the ambiguities free per epoch (carry_invariant), the phase rows carry no absolute information, and the code rows
must determine, per station and per epoch, {clock, ZWD, the γ-scaled receiver "DCB", N per-LOS ionospheres tied at 5 TECU}: the DCB and
the ionospheres share the γ-vector (only their sum per LOS is observed; the SH tie is the only separator), and clock vs ZWD separate only
through the ionosphere-free combination's elevation spread.  The local block is near-rank-deficient; the reconstruction picks a solution
along the weak direction, the residuals stay small, the ZWD reads absurd, and the station's contribution to the SHARED states (the satellite
clocks and biases — the wire) is a projection of that weak split, which is how a wire metres off CODE and a consumer that never converges
come out of stations whose own data fit to 0.4 m.  Three supporting facts from the same traces: the receiver "code-bias" seed is γ-scaled
between C1 and C2 at all three stations (ratio 1.647 = (f₁/f₂)²) — an ionosphere-like offset, not a hardware DCB; the per-LOS ionosphere at
epoch 1 is inconsistent between neighbours (G15 at 23–28°: WROC +14.7, GOP6 +9.6, FFMJ +3.5 m); the residual-pool trace omits every C1C row at
WROC (rows "attributed to neither family" — the pinned / canonical signal), a trace gap.  The sim bed carries the same sink (the campaign
run's `zwd_m` spans −11.9…+22.2 m around a 0.12 m median), which the medians of the storm grades never showed.

