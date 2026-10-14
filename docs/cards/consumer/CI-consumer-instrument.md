# CI: the closed-loop consumer as an instrument, and the attribution legs

**STATUS**

- **id:** CI
- **topic:** consumer
- **status:** closed
- **summary:** The cold consumer reached 0.25 m on CODE's products and 6 to 8 m on the old wire, so the wire was the defect; the suspended bars died with their postures. Closed: the re-founding grades the cold 6 h consumer.
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.closed_loop` (legs: `consume_network_iono`, `consume_network_clock`, `consume_network_products`)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none as `runs/` paths; seed `evseed_field6h_v8`, legs L0 to L4 from v8 ep0604
- **source:** `docs/archive/SPEC_event_harness.md` §24, §24.1 to §24.3; the §24.3 results are in the body of [CC](../consumer/CC-declared-step-carry.md)

**Outcome.** Which consumer posture converges, and what had the consumer bars read? The §24.3 legs put the cold consumer at 6 to 8 m on any mix of the network's products and 0.25 m on CODE's: the consumer was sound, the wire was the defect ([CI census](CI-census-wire-vs-code.md)). Closed: the suspended bars (four slip knobs, D5-v5, RS-10b) belonged to retired postures and are moot; [the re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) grades the cold 6 h consumer, at WTZZ00DEU 0.058 m in hour 1 on the [GAD baseline](../../refound/GAD_galileo_adoption_2026-10-13.md). Status reviewed 2026-10-13.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 24. THE CLOSED-LOOP CONSUMER AS AN INSTRUMENT (card CI) — the field consumer wanders by metres, resets on every declared step it observes, and starts cold at every seam; the consumer-side bars of §20.3, §21.21 and §23.2 are SUSPENDED until it converges (pre-registered 2026-09-14 ~00:40 local, manager; the attribution legs launched before this was written, none read)

### 24.1 What the consumer bars have actually been reading

§20.3 refused the field pin of the four slip knobs on a consumer bar: the settled (h3–6) de-biased 3D error of the held-out
WTZZ00DEU float consumer, +18–33 % on the twin.  The same consumer decided §23.2/§23.3 (D5-v5 "indeterminate — the sign
flips with the de-bias centre") and the consumer end of §21.21 (RS-10b's cold in-run consumer).  Read on its own, the
continuous consumer of the harness seed `evseed_field6h_v8` is this (per-hour p50 over 120 epochs, ENU against the
surveyed WTZZ coordinates):

| hour | 3D | up | east | north | n_sats |
|---|---|---|---|---|---|
| 0 | 3.42 | −2.55 | +1.79 | +1.11 | 47 |
| 1 | 6.13 | −0.58 | −0.18 | +6.12 | 48 |
| 2 | 8.54 | +2.83 | −0.22 | +7.92 | 48 |
| 3 | 4.76 | −0.73 | −1.20 | +4.62 | 51 |
| 4 | 5.04 | −3.99 | −2.21 | −0.06 | 50 |
| 5 | 7.16 | −4.75 | +0.42 | −4.99 | 49 |

A float PPP-RTK user with precise orbits, a 61-station clock and phase-bias wire and a co-estimated ionosphere should sit
at decimetres after the first hour; this one walks the NORTH axis from +1 to +8 to −5 m and the vertical from −4 to +3 to
−5 m over six hours, and its first five epochs go 4.2 → 4.7 → 6.2 → 8.0 → 12.4 m — AWAY from its code start.  A consumer
that cold-starts at the seam (`cert_null_v8`, §21.22) reads 3.96 m at k = 1 and 16.1 m at k = 11.  Its hour-to-hour p50
moves by 2–4 m; a 5 % bar on the de-biased dispersion of that series (§20.3's G-G: 5.49 → 6.50 m) is inside the
instrument's own swing, and so is every consumer reading in §21.21 and §23.2.  The `n_sats` column says why the number
is not even a GPS-only reading: the consumer tracks four constellations, the wire covers GPS, and the other three ride the
precise CLK product in a different clock frame through the consumer's inter-system offsets.

**Ruling (retroactive, stated once).**  The WIRE-side bars of those sections stand (§20.3's `pbstep` +28 % epochs / p90
step 3×; §23.3's PB wire 1.37× its bar; §21.21's producer end); their CONSUMER-side bars are SUSPENDED — neither PASS nor
FAIL — until the instrument in §24.4 converges.  No consumer bar decides a pin while the consumer's own hour-to-hour p50
spread exceeds the bar.  (The `feedback_event_harness_over_daylong_runs` rule already said it: a gate that needs a
settled 6 h window on the single closed-loop station is a sign the instrument is wrong.)

### 24.2 The reset channel, measured — the four slip knobs did not reach the consumer through it

`RealTimeEngineSsrBreaks.cpp:94` `resetOnPhaseBiasDisc`: for every phase signal the consumer models on a satellite it
observes, a change of the wire's phase-bias discontinuity counter (`StateManager::noteSsrDiscontinuity`) calls
`resetAmbiguity` — the covariance row and column are zeroed (every cross-covariance to the clock, ZWD, iono and position
thrown away), the diagonal set to `ambiguity_std²`, and the ambiguity marked pending re-seed from ONE epoch of code.
Twins, resets per hour from `logs/rt_engine.log`: v8 **[8, 1, 0, 1, 17, 2] = 29**, v8fp2 **[7, 1, 0, 1, 17, 3] = 29** (code
590 / 586, clock 6 / 6).  The +107 refloats of §20.3 produced ZERO additional consumer resets: the extra declared steps
were on satellites WTZZ does not observe (the JPLM/G10 catch is over California) or below the declaration threshold, and
the wire's σ exposure barely moved (P rows with σ > 0.1 m: 2.32 % → 2.42 %).  On the 60-epoch E3/G24 legs both arms show
the same two resets (G24 L5X at the event epoch — WTZZ observes G24 — and G06 L2W at +31).  So the twin's consumer
difference is not attributable to the four knobs through any channel the consumer is known to react to; it is the wander
of an unconverged filter under a different realization of the same wire.

### 24.3 Pre-registered attribution legs (launched ~00:20, before this section; none read)

From v8 ep0604, 60 epochs, no event, consumer-only knobs under `network.closed_loop` (the producer is byte-identical to
`cert_null_v8` — assert `products.txt` sha `…` equal); the consumer is COLD at the seam in every arm, which is the
regime these legs grade — the DIRECTION of a cold float PPP over 30 minutes:

| leg | consumer knob | question |
|---|---|---|
| L0 `cert_null_v8` | as the golden (FULL: network clock + PB + SH-VTEC iono) | the reference: 3.96 m → 16.1 m by k = 11 |
| L1 `ci_null_noiono` | `consume_network_iono: false` | is the supplied slant ionosphere the vertical driver? |
| L2 `ci_null_noclock` | `consume_network_clock: false` (bias-only: precise CLK + network PB) | is the network clock frame the driver (the Aug FULL-mode divergence, `project_closedloop_consumer_vertical_bug`)? |
| L3 `ci_null_neither` | both false | the two together |
| L4 `ci_null_noproducts` | `consume_network_products: false` (plain PPP on SP3/CLK/.BIA) | does the consumer's OWN modeling converge at all on this station? |

Read per leg: 3D / up / north / east at k = 1, 11, 30, 60, the p50 over k 30–60 and the sign of the k 30–60 slope.
Frozen reading: a healthy float PPP with precise products crosses 1 m 3D within 20–30 min and keeps falling.  If L4
converges and L0 diverges, the network products break the consumer and L1/L2/L3 partition the damage between the iono
supply and the clock frame; if L4 also diverges, the consumer's own modeling on WTZZ (four constellations through the
inter-system offsets, the 25° `ionosphere_weighting.elevation_cutoff_deg`, the 15° mask, antenna handling) is the defect
and the network is not implicated — a consumer-side card before anything else.  Whichever arm converges becomes the
consumer posture of the field golden's `closed_loop` block for grading (a consumer knob is not a producer knob: the goldens
pin it in the manifest, no ledger row, no fingerprint).

