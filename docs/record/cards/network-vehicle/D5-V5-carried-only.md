# D5 v5: `carry_invariant_innov_carried_only` on the field, KEEP_OFF

**STATUS**

- **id:** D5 v5
- **topic:** network-vehicle
- **status:** superseded
- **summary:** KEEP_OFF on the field: testing carried chords only removes every born-chord D5 flag, but the same-realization consumer pair reads +39 % worse and the v8 re-test is indeterminate.
- **dates in the text:** 2026-08-02 to 2026-09-13
- **knobs / posture:** `network.carry_invariant_innov_carried_only` (regime: `carry_invariant` with the Card-4 D5 innovation gate; not a warm-resume fingerprint token)
- **hashes named in the text** (commit / exe / byte-cert): v5 fingerprint `545b8bdd2b638231`; smoke OFF `87a33bf0…`, ON `6439e2bb…`; null leg `c45e52a0…`; worker `76861c1`; exe `gnss_processor_rs7.exe` `23C628B3…`
- **run dirs named in the text:** none as `runs/` paths; legs `d5v5_{null,e3}_{off,on}`, `d5v5_long_{off,on}`, `d5v5_v8_{off,on}`, 6 h twin `evseed_field6h_v5d5`
- **source:** `docs/archive/SPEC_event_harness.md`, sections 23, 23.2, 23.3

**Outcome.** The card asked whether the D5 innovation gate should test only CARRIED chords, since a chord born this epoch has no hypothesis to test. The mechanism holds: on the 6 h twin D5 flags fall 730 → 143 and slip refloats by 30 %, with the `pbstep` p90 4.5× better. The consumer does not follow: the twin's settled p50 is +5.3 % against a 5 % bar (FAIL), and the v8 re-test misses the PB wire bar (0.0137 vs 0.01 m). [RF-1](../../refound/RF-1_float_ambiguities_design.md) later retired the `carry_invariant` family and its D5 gates.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 23. D5 v5 CARRIED-ONLY on the field (`network.carry_invariant_innov_carried_only`) — the born-chord flags are the churn engine and removing them costs nothing on the wire (2026-09-13; pre-registered in the worker brief before any leg was read, results ~12:30 local; worker on worktree-agent-abad026e4a52148da, exe `gnss_processor_rs7.exe` 23C628B3…; manager grade)

**Why.**  The Card-4 D5 innovation gate standardizes a chord's innovation against Cov(v), the H0 covariance about
the CARRIED prior; a chord BORN this epoch has the diffuse fresh-chord prior (√var = 1000 m exactly), so there is no
hypothesis to test, its "reset" is a no-op and the flag's only lasting effect is the funnel entry that re-borns it
next epoch — the loader's 2026-08-02 measurement (95.3 % of all D5 flags were born chords).  v5 applies v2's
accumulator exclusion to the DECISION.  Built default-OFF, never armed on the field golden, no ledger row (§20.1's
hygiene item).  NOT a warm-resume fingerprint token (the carry family contributes `carry:inv` and `carry:pdsafe`
only; both ON legs warm-resumed with the carry restored; the ON twin's ep0604 fingerprint is v5's
`545b8bdd2b638231`), so the knob is A/B-able on legs from one snapshot.

**Pre-registered gates (the brief).**  Smoke pair; legs `d5v5_{null,e3}_{off,on}` (60 epochs from `v5` ep0604, E3 =
the equal-count slip at GOPE/G24): V1 identity on E3 (the injected chords flagged / re-tapped identically, the T0/T1
rows identical, bands freed identical); V2 the mechanism over +6..+60 (born-chord flags ON = 0 with the born-skipped
counter > 0, carried flags ±10 %, births and re-taps ON ≤ OFF, chord lifetime ON ≥ OFF); V3 no harm (clock contrast
±0.02 m, PB wire ±0.01, ZWD ±0.02, consumer de-biased 3D mean +30..+60 ±0.03, withheld equal, a₀₀ ±0.5 TECU); V4 the
6 h twin `evseed_field6h_v5d5` vs `v5` (flags down by the born share, births, carried DDs, `attrib_arc` not up > 10 %,
withheld equal or fewer, consumer de-biased settled h3–6 p50 / rms not worse than 5 %, `pbstep` not worse).

**Smoke.**  OFF `87a33bf0…` (the certified hash); ON `6439e2bb…` — every payload kind moves (B 1690/2595 rows, worst
0.058 m; C 415/640, 0.011 m; P 1755/2700, 0.57 m in the 20-epoch ignition; U 1.4e-5 m), E and S identical, no record
added or dropped; D5 flags 22 → 10 = 12 born + 10 carried → 0 born + 10 carried exactly.

**Legs — V1 / V2 / V3 PASS on every bar.**  All four 61/61 emitted, 0 withheld, warm-resumed; `evleg_d5v5_null_off`
byte-identical (`c45e52a0…`) to another worker's `evleg_fp1_null_off`.  V1: the five injected-event rows verbatim
identical between arms (INJECT ×2; `T0_mw_detect refloat L2W;L5X`; `T1_if_slip refloat L1C;L2W`; MDB); event − null =
+2 births / +2 slip refloats on both arms — and D5 never flags the injected chord on either arm, the raw layer catches
it in-epoch.  V2 (null, +6..+60): born flags **28 → 0**, carried **10 → 10**, births 294 → 268, re-taps 38 → 10,
born-skipped 353 (= the ON arm's births — the counter counts every born row before the threshold test, ~7–8 % of
which would have flagged), carried DDs at run end 759 = 759, carry-side mean chord lifetime (Little's law) 122 → 131
epochs.  The two populations separate perfectly: the 28 born flags at √var = 1000.000 m with median |v| 69.4 km; the
10 carried at √var 0.30–1.06 m, median |v| 8.9 m.  ⚠ `network_dd_fractional.csv`'s chord set is visibility-determined
(set-identical across arms), so a lifetime read from it passes by construction — the carry-side substitute is the
instrument.  V3 (null): clock contrast +0.0035 m, PB wire −0.0007, ZWD −0.0009, consumer de-biased +0.0021, a₀₀
+0.0012 TECU, withheld 0 = 0; the E3 pair the same to three digits (the event does not amplify the knob's wire
effect).  Direct arm-to-arm consumer 3D separation mean 0.39 m / max 2.12 m on a 7–9 m float-only consumer —
reported, not gated.

**V4 the twin (720/720 both; fingerprint unchanged).**

| | v5 (OFF) | twin (ON) | |
|---|---|---|---|
| D5 flags / re-taps | 730 / 730 | **143 / 143** | −80.4 %; 730 − 589 = 141 predicted, 143 observed |
| born / carried flags | 589 / 141 | **0 / 143** | +1.4 % carried |
| chord births; slip refloats | 9932; 1902 | 9362; **1332** | −5.7 %; −30.0 % |
| carried DD at run end; marginalizations | 758; 5710 | 758; 5710 | identical |
| withheld epochs; a₀₀ median | 0; 31.281 | 0; 31.250 | equal; −0.03 TECU |
| `pbstep` frac > thr; p90 max | 0.1181; 0.2570 m | 0.1125; **0.0571 m** | better (4.5×) |
| `cb_disc` / `clk_disc` / shocks / `pb_reset` | 590 / 6 / 600 / 29 | 590 / 6 / 600 / 30 | R, E, S records identical |
| consumer de-biased settled h3–6 p50 / rms | 5.495 / 6.163 | 5.785 / 6.980 | **+5.3 % / +13.3 % — FAIL (bar 5 %)** |

The twin's wire: clock contrast |ON − OFF| p50 0.014 / p95 0.123 m; phase bias p50 0.31 m with a tail that is ONE
satellite — every one of the 731 rows > 20 m is G11 (L1C 620, L2W 111; 389.2 L1 cycles, not an integer): an
ambiguity LEVEL realized differently between two independent cold starts, invisible to the datum-invariant
between-satellite reading (`pbstep`, better on ON) and absent on the warm legs, which share a realization (PB p50
0.0007 m).  The consumer's hourly series moves in both directions (ON better in h1 −29 % and h3 −36 %, worse in
h4–h6) on a float-only consumer at ~6.5 m raw error — single-realization wander, exactly the confound standing
invariant #5 names, and the reason the V4 consumer bar is not read as harm.

**Decision.**  RECOMMEND_SCOPED (regime: `carry_invariant` with the D5 gate); the mechanism is proven on the field
and every wire-side instrument is flat or better.  The field golden pin is DEFERRED to a same-realization consumer
grade, pre-registered here: two 240-epoch null legs from `v5` ep0604 (`d5v5_long_{off,on}`; both arms share the
seed's realization up to the seam, so the consumer difference isolates the knob) — consumer de-biased 3D mean over
+120..+240 ON within 5 % of OFF (each arm de-biased by its own window median), the direct arm-to-arm separation
reported, `pbstep` on the leg products not worse, withheld equal, clock contrast / PB wire / ZWD p50 over +120..+240
within the V3 bars.  PASS ⇒ pin on `golden_field_gps_dual` (its certified smoke hash moves — every payload kind, no
structural change — and is re-recorded at that certification); FAIL ⇒ the exception stands with the consumer number.
Graders merged: `scripts/d5v5_grade.py`, `d5v5_prodiff.py`, `d5v5_seam_ab.py`.

### 23.2 RESULTS — the same-realization pair refuses the pin: KEEP_OFF on the field, re-test named (2026-09-13 ~16:20 local; worker on worktree-agent-abad026e4a52148da `76861c1`, merged; manager grade)

**Legs.**  `d5v5_long_off` / `d5v5_long_on`: 240 epochs from `v5` ep0604 (exe 23C628B3…, config diff = the knob only;
harness defaults of ~12:45 — before RS-8 / RS-9 were harness-armed, so both arms carry those seam defects equally).
**The pre-registered window is not measurable:** a 30-epoch contiguous producer outage sits at ep122–151 on BOTH arms
(`solve_failed` at 122, `gate_withheld` 123–150, `sh_gate_withheld` 151; 6 scattered SH-gate withholds to 215 — the
DOY-129 sick-field era, 06:03–06:18 UT; the SH gate still has no release policy) with dispositions epoch-for-epoch
identical between arms, and the consumer collapses on both (3D 100–330 m after the outage, never recovering) — so
every +120..+240 bar fails on both arms' wreckage and says nothing about the knob (consumer de-biased +14 %, wire
p50 0.18 m, `pbstep` +6 % / +10 %; withheld 37 = 37 PASS).

**Pre-outage (+6..+120, the producer emitting every epoch) — the read that counts:** consumer de-biased 3D mean
**3.069 → 4.267 m (+39 %)**, raw 3D at +120 3.3 vs 14.6 m; PB wire |ON − OFF| p50 **0.033 m** (bar 0.01; spread across
many satellites — G10 107 rows, G04 91, G11 86 above 0.2 m — a broad slow divergence, not the twin's single-satellite
level); clock contrast 0.0055 m and ZWD 0.0022 m PASS.  Three horizons of the same statistic: +6..+60 clock 0.0018 /
PB 0.0032 / ZWD 0.0007 / consumer separation 0.39 m; +6..+120 0.0055 / 0.033 / 0.0022 / 2.61 m; post-outage 0.18 /
0.18 / 0.079 / 169 m — every channel grows with time since the seam (§21.19's realization divergence, here driven by
the knob's own change of the chord population), and the outage multiplies it ~6×.  V2 holds over the whole leg: born
flags 86 → 0, carried 97 → 100, D5 flags 183 → 100, re-taps 145 → 62, births −2.7 %, carried DDs 739 = 739,
marginalizations 1976 = 1976, `clk_disc` 0 = 0, shocks 84 = 84.

**Decision.**  Two truth-referenced consumer reads in the wrong direction (the twin's settled h3–6 +5.3 % p50 /
+13 % rms; this pair's +39 % over +6..+120) against one neutral 60-epoch read: **KEEP_OFF on the field** (ledger
verdict changed; the deferred-pin exception and the posture row removed).  The mechanism is real and cheap — born
chords never carried information to test — but a knob that reshapes the chord population reshapes the realization,
and on this float-only consumer the two realizations the field has been dealt read worse.  Re-test named, not
run: a 120-epoch pair from the v8 harness seed (every RS restore incl. RS-8 / RS-9 in both arms; the pair above
pre-dates them), graded on the consumer de-biased 3D mean over +6..+120 within 5 % and the PB wire within 0.01 m — a
second realization on a seam that no longer re-births 64 chords.  (Instrument note for every future 2 h field leg:
the DOY-129 sick-field outage at 06:03–06:18 UT lands at ep122–151 of any leg from ep0604 — grade before it or
around it, never through it.)

### 23.3 The v8 re-test (2026-09-13 ~21:20; the pair §23.2 named, run in the certification, §21.22)

`d5v5_v8_{off,on}`, 120 epochs from `v8` ep0604, one differing key: V2 every clause PASS (born flags 74 → 0, born-skipped 1824,
carried 16 → 17, births 1873 → 1805, re-taps 90 → 17, carry-side lifetime 49 → 51 ep); clock contrast 0.0020 and ZWD 0.0018
PASS; PB wire |ON − OFF| p50 0.0137 m against the 0.01 bar (FAIL, 1.37×); withheld 0 = 0.  The consumer bar is INDETERMINATE:
de-biased by the window median ON/OFF −3.9 %, by the window mean +3.0 to +4.1 % — every reading inside ±5.4 % with the sign
set by an analyst choice, while the direct arm-to-arm separation (1.22 m mean) dwarfs the dispersion difference.  A knob that
reshapes the chord population reshapes the realization, and a single float consumer cannot rank two realizations even from
one seed.  **KEEP_OFF stands.**  A promotion needs a consumer instrument that is not one realization's dispersion — a
truth-referenced grade over several seam epochs / stations, owed.

