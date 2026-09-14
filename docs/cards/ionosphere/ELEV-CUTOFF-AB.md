# elev-cutoff A/B — the elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a coherent field

**STATUS**

- **id:** elev-cutoff A/B
- **topic:** ionosphere
- **status:** closed
- **summary:** the elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a coherent field
- **dates in the text:** 2026-08-12
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): `cbaefe0a`
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` — §12.9

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §12.9 The elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a
## coherent field (2026-08-12, HANDOFF_wsh_chain_slant T3)

*(T3's label map is §14.0; that handoff is now `docs/attic/`.)*

`_simnet300_wsh_n50_pin_el15` (15°) vs `_simnet300_wsh_n50_pin_el25b` (25°), 480 epochs
each, settled half graded, **same build** (`gnss_processor_wsh3.exe`, sha256 `cbaefe0a…`).
The 25° control re-run reproduces the stored §12.7 arm to four digits on every channel (C
1.0251/0.8891 vf 5683.6; B 1.1442 vf 489 286; ZWD 0.5413 vf 2301; P 0.2662/0.3178) — which
simultaneously certifies the A/B as single-knob AND that the §12.8 audit instrumentation
plus the day's concurrent tree edits are producer-inert on this posture (invariant #6
discharged by measurement, not inspection).

| settled channel | **15°** | 25° | 25/15 |
|---|---|---|---|
| emitted | 468 (12 A4) | 466 (14 A4) | parity (P-A/B-1 ✓) |
| C residual rms | **0.4908 m** | 1.0251 m | **2.09×** |
| C datum-free contrast rms | **0.5198 m** | 0.8891 m | 1.71× |
| C vf | 1 267 | 5 684 | 4.5× |
| B residual rms / vf | 1.1311 m / 503 478 | 1.1442 m / 489 286 | ≈ flat (P-A/B-3 ✓) |
| P frac L1C / L2W | 0.268 / 0.272 cyc | 0.266 / 0.318 cyc | L2W +17 % at 25° |
| rx clock contrast rms | **1.5023 m** | 1.9277 m | 1.28× |
| rx DCB contrast rms | 1.5052 m | 1.5403 m | ≈ flat |
| ZWD rms / vf | 0.4750 m / 1 653 | 0.5413 m / 2 301 | 1.14× |
| consumer 3D rms | 0.7463 m | 0.8372 m | 1.12× |
| **clk_common wander std** | **0.0598 m** (≈ 0.060) | 0.2478 m (≈ 0.248) | **4.1×** |
| a₀₀ error (mean ± std, TECU) | −0.12 ± 0.52 | −1.57 ± 1.03 | 3× |
| exchange coefficient (of +7.93 ideal) | +3.96 (**0.50×**) | +7.69 (**0.97×**) | — |
| clk↔rxdcb / rxdcb↔a₀₀ corr | −0.848 / +0.667 | −0.939 / +0.859 | tighter at 25° |

**P-A/B-2 is FALSIFIED in the pre-registered direction — the truth grade INVERTS the field
verdict.**  The 15° arm is better on every clock-adjacent channel, and the exchange numbers
say why mechanically: the low-elevation rays carry the OBLIQUITY DIVERSITY that separates
clock ↔ receiver-DCB ↔ ionosphere.  Cut them and the family collapses onto the
ideal-absorption manifold — the exchange coefficient rises 0.50× → 0.97× of the derived
ideal, the level correlations tighten toward ±0.9, the common-mode wander quadruples, and
the absorbed error surfaces as 2× clock truth error.  **The cutoff does not suppress the
exchange; it FEEDS it.**

**Reconciliation with the field A/B (§9.10.1: 25° trims wander 12 %).**  No contradiction —
the two arms measure two sides of one trade.  The synthetic field is COHERENT BY
CONSTRUCTION (the injected iono maps through the same single-layer geometry the estimator
assumes), so its low-elevation rows carry information and zero pathology: the cut is pure
separability loss, priced here.  The field's low-elevation rows additionally carry the REAL
§9.10 low–high incoherence, and removing that contamination nets −12 % wander *despite*
paying the same separability tax — which also explains why the field trim was so small
("~84 % of the variance survives"): the cut is partially self-defeating, weakening the very
containment that bounds the exchange.  The FT63 adoption of 25° stands on the field
evidence, but its cost now has a number, and any future "raise the cutoff further" proposal
must count BOTH sides (30° would likely lose in the field too).

**Script-version note.**  §12.7's exchange coefficient "+5.67 (0.72×)" is superseded ON
THE SAME PRODUCTS by the current `simnet_exchange.py`: +7.69 (0.97×), correlations
essentially unchanged (−0.939/−0.841/+0.859/−0.847).  The A/B above is internally
consistent (one script version for both arms); quote 0.97×, not 0.72×, as the 25° settled
exchange ratio.

