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

## 2026-10-24: closed-loop consumer vertical bias at 305 stations: the elimination table and the gotchas

These results were recorded only in the agent memory note `project_closedloop_consumer_vertical_bug` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: `network.closed_loop.consume_network_iono` is dead on the goldens, and the `NetworkEmitStore.cpp` anchor named below is `src/core/network/emit/EmitStore.cpp`.

Status (2026-10-07): historical. The failure: 305-sta em25 wire, held-out ZIM200CHE consumer at U -5.2 m
after 30 min, median 3D 3.45 m, 0/60 fixes, while an external PPP-RTK user on the same data settled in
5 min and standalone float PPP on the products sat at +9 mm. On 2026-09-14 the CI census put the defect
in the WIRE: the satellite clock wanders 2.8 to 5.0 m within arcs because it inherits a network code
model inconsistent with the data by metres; plain PPP on CODE products converges to 0.25 m, so the
consumer's own modeling was fine. That finding opened the network re-founding; on the RF goldens the cold
consumer converges (field WTZZ00DEU 0.108 m in hour 1, 0.026 m in hour 6, `runs/_rf0hv14_field6`). The s24
attribution, the census numbers and card CI's still-unlifted bar suspension are in
memory note `project_consumer_instrument_card`. The brdc_rts Card-8 read of the same failure (criterion 4: ~2.8 m
vertical-dominant float, 0 % fix, mode-independent, 3-station reproducer) is `docs/rt_service_conventions.md` §8.4.

Elimination table (each cost a graded run; none is a consumer-vertical lever):
1. `kSsrPhaseBiasSign` flip: dead. The CLFRAC dump (`AmbiguityResolver.cpp`, before the sigma gate) shows
   wandering fractions (mean |frac| 0.269, R <= 0.63), no static offset.
2. BSSD vs undifferenced consumer frame: both fail identically.
3. Slip detection off, mask 25 vs 10 deg, truth ECEF, product quality, horizon: all null.
4. `model_corrections.bias_coverage_requires_apply` (6fa0cd1, default false): a real phantom-.BIA
   coverage defect (3-sta U +5.11 to +0.009 m), byte-identical at 305 sta.
5. Signalling re-realizations (`code_bias_disc_jump_test`, `clock_disc_on_set_change`, ISB reinflate):
   U slightly worse; telling the consumer buys more resets.
6. Clock datum/gauge class: s2z, `clock_datum_broadcast_anchored`, `clock_signal_basis_hold` all null.
7. `filter.sat_clock_q` is a silent no-op under `network.clock_model` (the live pair is
   `clock_model_q1/q2`); tightening q2 over-tightens.
8. Code-to-clock leak inverted: loosening `code_std_m` worsens the clock (pseudoranges hold it together);
   tightening to 0.1 m buys -27 % e3d but +1.4 m vertical bias (diagnostic, not a production value).
9. `network.raim_arc_persist_epochs` backfires (K 2/3 halved refloats, PB churn and clock wander rose):
   refloats are damage control, not the disease.
10. Producer null-space leak: none in the shared normals (`trace_estimability`); the consumer's own
    ambiguity / rx-clock / height matrix was never tested.

Levers that moved it all traded AC (oscillation) for DC (bias): `phase_std_m` 2 to 3 mm (U -5.22 to
-2.20 m, plateau ~2.5 m 3D, 0 fixes); `phase_bias_min_station_fraction` (float50: clock tstd -70 %,
U-osc 2.43 to 0.67 m, but a -2.3 m DC height bias; FLOAT beats DROP, `signal_min_station_fraction` e3d
9.93 m); consumer `ionosphere_q` 1e-3 to 1e-4 (U-osc -33 %, +2.7 m DC); `closed_loop.consume_network_iono`
(a9301c6, a common-mode-free per-satellite slant pseudo-obs, network solve byte-identical), the only lever
that zeroed the vertical median (up -1.09 to +0.04 m at the honest sigma; std_scale 0.1 over-corrects to
+2.14 m, still 0 fixes: AR was not iono-limited). ⚠ Corrected 2026-10-07: `consume_network_iono` is armed on the RF goldens but
dead, since the producer emits slant iono only with an SH model (`NetworkEmitStore.cpp` ~:699); card AB1
(2026-09-20) parked the consumer iono constraint KEEP_OFF.

Gotchas:
- `network.raim_sigma0_floor` 1.0 is load-bearing: unclamped sigma0 collapses to ~0 because most pooled
  carried-phase residuals are ~0 (pinned), so RAIM gates at the fixed a-priori scale (6 sigma_phase).
  The structural fix, if ever wanted: pool sigma0 over active (non-pinned, non-zero) residuals only.
- The closed-loop consumer inherits the producer's filter block (`position_q` 100, `troposphere_q` 1e-9;
  current state in memory note `project_consumer_instrument_card`).
- The W6 slice (`estimate_sat_code_bias`, also force-armed by `estimate_iono_sh`) forces
  `model_corrections.enable_code_phase_bias = false` at load whatever the JSON says (how phantom .BIA
  coverage slipped in).
- At 305 sta the per-component loggers are off (`rt_engine.log` empty): grade consumer reactions from
  traces.
- A run's effective config is its `gnss_config.log`, not a same-named .json edited later.
- Reruns of one binary are byte-identical: every A/B delta is signal.
- Broadcast-anchored clock eigenvalue is 1/sigma^2, independent of N: the loader warns above 30 m and
  errors at >= 100 m.
- The Odijk diagnosis (moving PB datum, reset-not-repair) and its Fix A/B were overtaken: an ensemble PB
  reference cannot be integer (eq34 theorem), the carry-side step was fixed by `phase_anchor_carry_reref`
  and the held basis (memory note `project_pivot_visibility_gauge_defect`), and the RAIM premise was inert on
  carry_invariant (memory note `project_raim_pool_rehost`).
- NOT refuted: the split-anchor AR poison (Odijk 2016 Table 11). A user DD is integer only if one pivot
  receiver (or a chain) anchors every satellite; per-(sat,signal) anchors on different receivers leave a
  fractional term in the user's between-satellite difference. The eq34 chain datum answered it
  (`docs/cards/network-vehicle/CHAIN-SLANT-ODIJK.md` section 14.2) and is off on the RF goldens
  (prioritized per-(sat,signal) datum, float vehicle); re-read before any consumer AR work.

Record: `docs/cards/consumer/` CI-consumer-instrument.md, CI-census-wire-vs-code.md,
CC-declared-step-carry.md; `docs/refound/NETWORK_REFOUNDING_2026-09-14.md`,
`docs/refound/AB1_sh_consumer_product_2026-09-20.md`. Evidence runs `card8_brdc_rts/`,
`net_matrix_2024-129/` are under `D:/GnssProcessor_archive/2026-08-12/runs/` (pre-refound, untrusted);
grader `scripts/grade_codeloose.py`.

memory note `project_ssystem_theory_reference`.
