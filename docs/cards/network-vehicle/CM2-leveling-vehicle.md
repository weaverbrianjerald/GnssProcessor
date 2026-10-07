# CM2: the second defect, the CM knob arms on the leveling vehicle

**STATUS**

- **id:** CM2
- **topic:** network-vehicle
- **status:** closed
- **summary:** No knob arm crosses or halves a bar on the leveling vehicle, so the second defect is not a knob; two arms are INERT on the producer and `sat_code_bias_q` 1e-4 regresses.
- **dates in the text:** 2026-09-14
- **knobs / posture:** base `config/test_suite/_pv_b.json` (PV-B, the control), one knob per cold 2 h arm: `network.carry_rx_code_bias`, `network.carry_zwd` + `network.zwd_carry_q`, `network.cb_kaula_prior`, `filter.process_noise.sat_code_bias_q`, `network.estimate_iono_sh`, `network.estimate_sat_code_bias` (with `model_corrections.enable_code_phase_bias`), `network.representation_sigma_tecu`, `estimation.zwd_constraint.enabled` (`estimation.zwd_constraint`)
- **hashes named in the text** (commit / exe / byte-cert): worker `310d3ee`; exe `13c1d7ef…`
- **run dirs named in the text:** `runs/_cm_pvb` (the control, not re-run)
- **source:** `docs/archive/SPEC_event_harness.md`, section 24.12

**Outcome.** The card re-ran the [CM](../network-vehicle/CM-code-model-consistency.md) knob arms on the leveling vehicle (PV-B: code rms p50 1.79 m) to name the code-side defect that survives it. The closest approach to a bar is 0.82× (b7, on the code rms). The fourth frozen reading applies and both of its candidates fall: without the rx-CB carry the ZWD outliers remain, and a ZWD random-walk carry no longer diverges yet leaves the code and wire bars unmet. [Q3-F](../clock/Q3-F-self-contained-time-link.md) then named the free per-epoch satellite clock as the second contributor; the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) froze the leveling vehicle with its clock anchor (`clock_anchor_q` 2.9e-8).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.12 Card CM2 — the second defect: the §24.8 knob arms on the LEVELING vehicle (pre-registered before any arm is read)

Base `config/test_suite/_pv_b.json` (PV-B; control = `runs/_cm_pvb`, not re-run), 2 h cold, one knob per arm, the §24.8 traces on:
**b1** `network.carry_rx_code_bias: false`; **b2** `network.carry_zwd: true` + `network.zwd_carry_q: 1e-8` and **b2b** q 1e-7 (the B9
random-walk carry — it DIVERGED on the carry_invariant vehicle (§24.8; SPEC_amb_iono §6.15.4 `_fldf3_ci_zwd`) where the sink was
load-bearing; on a determined block it is the physical model); **b3** `network.cb_kaula_prior: false`; **b4**
`filter.process_noise.sat_code_bias_q: 1e-4`; **b5** `network.estimate_iono_sh: false` (the sim's posture C — free slant iono; the
loader's dependent lines recorded); **b6** `network.estimate_sat_code_bias: false` + `model_corrections.enable_code_phase_bias: true`
(CODE OSB; (i) / (iv) readable only once the code-row fallback lands — (ii), (iii) D_phase and the consumer are graded regardless);
**b7** `network.representation_sigma_tecu: 20`; **b8** `estimation.zwd_constraint.enabled: true` + `sigma_m: 0.2` (dropped as inert if
the loader / run-end says the network path does not consume it).  Reads (i)–(v) + the consumer, as §24.11.  **Bars (frozen, absolute =
§24.8's):** (i) p50 < 1.0 m AND (ii) zero stations |ZWD| > 0.5 m AND (iii) D_phase < 0.5 m — all three NAMES the second defect; any
bar halved vs PV-B = contributor; nothing halved = exonerated on this vehicle; availability within 5 % of PV-B; a divergence (withholds,
ZWD blow-up) is a result, not tuned.  **Frozen reading:** one arm crosses ⇒ the posture card = vehicle + that knob; two or more cross ⇒
combined and re-graded on the same bars + the consumer; contributors only ⇒ one union arm of the contributors (b9, run after the grade);
nothing moves ⇒ the second defect is not a knob — the candidates are the rx-CB inject gate's frame-blindness (§21.18, a C++ card) and
the ZWD parameterization (§24.10's per-epoch prior), decided on card ZP's station-block trace read on PV-B.

**§24.12 RESULTS (2026-09-14 ~01:00 local; worker on worktree-agent-aa414b8d02ff172f0 `310d3ee` — `scripts/cm2_make_arms.py` + `cm_grade
--control`, fast-forwarded to main; exe `13c1d7ef…` re-hashed before and after; manager grade).**  Nine arms from `_pv_b.json`, each
JSON diff = the knob + 2 paths (re-verified from the `arm.json` consumed); all cold, 240/242 emitted, 0 withheld — availability identical
to PV-B in every arm; the vehicle lines identical in all ten (394 latches / 250 skips / 61 handovers).  Two arms measured INERT on the
producer (products byte-identical to PV-B): **b6** — the loader forces `estimate_sat_code_bias` back on under `estimate_iono_sh` and W6
then forces the .BIA OSB OFF (`GnssConfigLoadNetwork.cpp:2818–2831`; §24.8's A2 reproduces — the CODE-OSB posture is not reachable with
the SH tie armed); **b8** — `estimation.zwd_constraint` is read only by the closed-loop consumer's `RealTimeEngine::applyZwdConstraint`
(`RealTimeEngineEstimate.cpp:128–143`; the consumer trace differs, the products do not — a correction to §24.8's A7 read).

| arm | knob | (i) p50 / p90 / max m | (ii) n (worst) | (iii) D_phase / D_code / clock LEVEL m | (iv) level SS / disp. | consumer p50 tail (at 2 h) | verdict |
|---|---|---|---|---|---|---|---|
| PV-B | control | 1.79 / 3.28 / 7.08 | 13 (BOGT −3.5, GOPE +2.5, GAMB −2.1) | 0.95 / 0.13 / 0.74 | 97.8 / 5.6 | 1.19 (0.55) | control |
| b1 | `carry_rx_code_bias` off | 1.84 / 3.20 / 7.07 | 13 (GOPE +2.4, GAMB −2.1, NIUM +1.8) | 0.83 / 0.11 / 0.65 | 97.9 / 5.8 | 2.61 (2.63) | exonerated |
| b2 | `carry_zwd` q 1e-8 | 1.66 / 3.43 / 7.36 | 13 (GOPE +3.1, GRAZ +1.4, CPVG +1.0) | 0.87 / 0.10 / 0.63 | 98.5 / 5.6 | 1.56 (0.96) | exonerated |
| b2b | `carry_zwd` q 1e-7 | 1.64 / 3.38 / 7.29 | 12 (GOPE +3.2, GRAZ +1.4, CPVG +0.9) | 0.86 / 0.11 / 0.65 | 98.5 / 5.5 | **0.97** (1.07) | exonerated |
| b3 | `cb_kaula_prior` off | 1.84 / 3.18 / 6.97 | 13 (BOGT −3.5, GOPE +2.5, GAMB −2.2) | 1.04 / 0.14 / 0.76 | 97.9 / 5.6 | 2.01 (2.00) | exonerated |
| b4 | `sat_code_bias_q` 1e-4 | 1.70 / 3.62 / 9.42 | **33** (BOGT −4.2, GAMB −4.1, GOPE +4.0) | 1.07 / 0.12 / 0.78 | 92.6 / 6.6 | 3.64 (1.65) | regression |
| b5 | `estimate_iono_sh` off | 1.53 / 3.38 / **4.63** | 13 (BOGT −4.2, GAMB −3.4, LCK4 +1.7) | 1.09 / 0.15 / **0.99** | 97.7 / 5.7 | 1.66 (0.67) | exonerated |
| b6 | CODE OSB | inert (= PV-B) | | | | | inert |
| b7 | `representation_sigma_tecu` 20 | **1.46** / 3.10 / 4.85 | 14 (GAMB −2.2, BOGT −2.1, NIUM +1.2) | 0.88 / 0.13 / 0.66 | 98.3 / 5.5 | 3.36 (2.95) | exonerated (18 % on (i)) |
| b8 | `zwd_constraint` σ 0.2 | inert on the producer (= PV-B) | | | | 1.25 | inert |

**Grade: no arm crosses or halves any bar** (closest 0.82× on (i) by b7, 0.87× on (iii) by b1, 0.92× on (ii) by b2b); no union arm.
**The fourth frozen reading applies — the second defect is not a knob.**  Three facts narrow it: (1) b1 removes the rx-CB carry entirely
and GAMB / GOPE keep their 2 m ZWDs — the frame-blind inject gate (§21.18) cannot be the second defect; (2) b2 / b2b carry the ZWD as a
random walk WITHOUT diverging (100 % of rows carried, |ZWD| p50 0.22–0.24 m, the best consumer tail 0.97 m) and still leave (i) / (iii)
— the sink is safe to close on a determined block but was not the defect, and §24.10's per-epoch ZWD prior is retired as a candidate;
(3) b4's unfrozen satellite code biases absorb the misfit and hide it from both σ̂₀ instruments (prires cb 3.12 → 0.39, P1-4 phase
0.175 → 0.039) while the ZWD count goes 13 → 33 — the misfit is real and per-satellite-shaped enough that a free satellite code-bias
state eats it.  And the per-LOS level test FLIPS on this vehicle: A0's levels were shape-REFUTED as ionospheric, PV-B's are
shape-ADMITTED (implied dispersive 88 %); b7's looser tie buys 18 % on (i) at the consumer's expense and b5's free iono buys the code
max at the wire's — the remaining code residual is dispersive-shaped and the SH tie is trading it against the wire.

