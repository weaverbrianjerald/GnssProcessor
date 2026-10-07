# CI census: the wire-vs-CODE census and the per-LOS code-residual attribution

**STATUS**

- **id:** CI census
- **topic:** consumer
- **status:** closed
- **summary:** W1 and W3 FAIL: the wire wanders 2.8 to 5.0 m rms within arcs through the satellite clock, which inherits a network code model inconsistent with the data by metres.
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.trace_postfit_rows`; instruments `scripts/wire_vs_code_census.py`, `scripts/pf_rows_attrib.py`
- **hashes named in the text** (commit / exe / byte-cert): `20377f2`, `e0d234ee`, `7ece11f`, `0dac1cc`, `87a33bf0`
- **run dirs named in the text:** `runs/evseed_field6h_v8`, `runs/evseed_field6h_v8fp2`, `runs/_ci_ppp_*`
- **source:** `docs/archive/SPEC_event_harness.md` §24.5, §24.6

**Outcome.** The census asks whether the wire is consistent with CODE's products as a float user sees it, and §24.6 where the producer's code residual comes from. Phase and code biases are flat within arcs while the satellite clock wanders; the four slip knobs change nothing (W2); code offsets exceed 1 m on 90 of 106 keys. The code residual is a persistent, non-dispersive per-LOS level (92.3 % of the sum of squares; A2 met only by the letter) absent from standalone PPP on CODE. [CM](../network-vehicle/CM-code-model-consistency.md) followed, and the census became a bar of [the re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.5 The wire-vs-CODE consistency census — truth-referenced, datum-invariant, consumer-free, readable on any leg (pre-registered before it is written; a read-only Opus census, no producer run)

What the consumer applies per satellite s and phase signal L at epoch t is `A_net(s,L,t) = c·δt_net(s,t) + PB_net(s,L,t)`
(FULL) against what the CODE user applies, `A_code(s,L,t) = c·δt_CODE(s,t) + OSB_CODE(s,L)`; for code signals the same with
the network W6 code bias against CODE's code OSB.  A float user is indifferent to any per-satellite CONSTANT difference and
to any per-epoch common-mode difference (its ambiguities and receiver clock absorb them), so the quantity that reaches the
position is `D(s,L,t) = [A_net − A_code](s,L,t) − median_s[A_net − A_code](L,t)`, and its WITHIN-ARC variation is the
consumer's damage.  Census on `runs/evseed_field6h_v8` (6 h) and `runs/evseed_field6h_v8fp2` (the four-knob twin), the
products.txt `C` / `P` / `B` rows against the CODE 30 s CLK and OSB BIA the seed's config names (the S line's a-priori
identity; the consumer used the same files in L4): per satellite and signal, D's mean over each arc (the datum offset — a
sanity read: it should be the same constant for every signal of a satellite up to the OSB), its rms ABOUT the arc mean, and
the per-hour p50 / p95 of |D − arc mean| pooled over satellites; the same for the code side; the raw clock contrast
`c·δt_net − c·δt_CODE` de-medianed per epoch, p50 / p95 per hour.  Also the 60-epoch windows the legs use (k = 1..60 from
tow 277320) on both seeds and on `evleg_cert_null_v8`.

**Bars.**  W1 (consistency): pooled within-arc rms of D_phase ≤ 0.05 m per hour after hour 1 on v8 ⇒ the products are
consumer-consistent and the metre-level consumer failure is the consumer's APPLICATION (a consumer build card follows,
instrumented by the applied-correction dump); rms ≥ 0.5 m ⇒ the wire itself moves within arcs at the metre level and the
consumer is reading it right (then the between-satellite clock / phase-bias wander is the producer defect to attack, and
the pin question is moot until it is).  W2 (the pin, re-graded on this instrument): v8fp2's pooled within-arc rms of
D_phase and D_code not worse than v8's by more than 10 % in hours 3–6; the count of arcs with a within-arc step > 0.25 m
reported per twin.  W3 (the code side): pooled within-arc rms of D_code ≤ 0.3 m; a per-satellite code-side CONSTANT
offset larger than 1 m is reported by satellite (a datum inconsistency the consumer cannot absorb because it carries no
per-satellite code-bias state — `estimate_sat_biases false`).

**§24.5 RESULTS — the wire is NOT consumer-consistent, and the satellite CLOCK carries it (2026-09-14 ~02:50 local; read-only Opus
census, `scripts/wire_vs_code_census.py` merged `20377f2`; manager re-read).**  Conventions taken from the code, not from memory:
the `C` record is a DELTA on the a-priori CODE clock (`clock_corr = interpolateClock(CLK) + applyClockToBias(c0)/c`), the
consumer's applied phase term is `−C_net + PB_net` against the CODE user's `−0 + OSB` (the common `−c·δt_CODE` cancels), the
S-line's a-priori identity names the FIRST product path of its extension (the 128 file) while every epoch was served by the
129 CLK; GPS L5 / C5 are absent from CODE's MGEX OSB, so 24 k of 95 k P rows are outside the census.  Per hour, pooled
within-arc |D − arc mean| on v8 (declared arcs = split at gaps, `R` marks and counter bumps):

| h | D_phase rms, declared / tracking arcs | D_code rms, declared / tracking | raw clock contrast LEVEL p50 / p95; within-arc rms |
|---|---|---|---|
| 0 | 5.00 / 9.85 | 1.43 / 6.53 | 5.71 / 12.79; 5.64 |
| 1 | 4.48 / 6.37 | 1.34 / 5.19 | 4.09 / 12.90; 3.97 |
| 2 | 3.71 / 4.94 | 1.52 / 3.59 | 2.69 / 9.21; 2.90 |
| 3 | 4.88 / 6.17 | 1.92 / 3.95 | 2.87 / 11.56; 4.54 |
| 4 | 4.96 / 6.41 | 1.45 / 5.12 | 5.62 / 14.48; 5.00 |
| 5 | 2.76 / 9.23 | 1.49 / 6.44 | 5.84 / 16.79; 2.68 |

**W1 FAIL by ×10 on the "wire moves" branch**: 2.8–5.0 m rms within declared arcs (bar 0.05; the ≥ 0.5 m branch).  The
decomposition is the finding: `PB_net − OSB` is flat within arcs (0.12–0.37 m rms, ZERO steps > 0.25 m), `B_net − OSB` is
flat (0.13–0.22 m, zero steps), and `C_net` alone wanders 2.7–5.6 m rms within declared arcs with a 12–31 m between-satellite
spread (the producer's own `clock_std_m` 6.7 m at epoch 1, `CLK_STEP` 0 — in the estimator, not at emission); worst arc G11
L1C mean +60.7 m / rms 30.4 m, worst clock G05 drifting +9.7 → −11.7 m over hours 0–4.7.  **W2 — the four knobs did not change
it** (declared-arc phase rms within 3 %, code identical; 24–27 % worse on tracking arcs in hours 4–5 only because 312 vs 301
declared re-realizations put more steps inside tracking arcs) — the pin question is moot until W1 is fixed.  **W3 FAIL both
halves**: code within-arc rms 1.3–1.9 m (declared), and per-satellite CONSTANT code offsets > 1 m on 90 of 106 keys (up to
10.7 m; ±3–7 m in the pinned C1C/C2W IF direction, 13 m on the clock pivot G01) — which a consumer without per-satellite
code-bias states cannot absorb.  The 60-epoch window on `cert_null_v8` reproduces the continuous seed to a few % on every
kind: the inconsistency is the wire's, not a seam artefact.  So §24.3's consumer was right, and the response-cost story of
§20.3 was a reading of a wire that leaves a float user at metres regardless of any slip knob.

**Two things ruled in and out immediately (manager, 03:00).**  (i) The 61 held station coordinates are ITRF2020 propagated to
DOY 129 to < 1 mm (62 / 62 in `data/static/sinex/ITRF2020-IGS-TRF.SSC`) and the per-station phase residual rms is 0.01–0.17 m
(`network_pf_keyed.csv`, kind STA, hours 2–5) — not a coordinate or geometry blunder.  (ii) **The producer's own settled CODE
residuals are 1.8–9.8 m rms per station, median 4.5 m** (`s0_code` 4.5 against the 0.3 m assumed; the epoch σ̂₀ code ≈ 5 the
seam grader has printed all along), rising with isolation — GAMB 9.8, JPLM 9.6, MAJU 8.7, CAS1 8.1, SAVO 7.7, KOKV 7.6, PALM
7.6 … WROC 1.8, GOP6 2.2, FFMJ 2.5; corr(rms, log mean distance to the 3 nearest stations) = +0.51; per signal C1C 6.7, C2W
5.5, C5Q 5.4, C1W 5.3, C5X 5.3, C2X 4.3 — the L2 / L5 residuals are SMALLER than L1's, the opposite of a dispersive (ionospheric)
error (which would read ×1.65 / ×1.79); per satellite G16 8.8 … G19 3.7.  The satellite clock is what the code determines: a
code model inconsistent by metres, station-dependently, IS the clock's metre-level wander, and the phase side inherits it
through the clock.  The seven stations the frame-blind rx-CB gate refuses all day (§21.18) sit high (GAMB, KOKV, CPVG, LCK4,
LMMF, MRO1, GOPE) but are not the whole list (JPLM, MAJU, CAS1, SAVO, PALM are not refused).

### 24.6 The CODE-RESIDUAL attribution — per-LOS, from the producer's own post-fit residuals (pre-registered before the trace is read)

A null leg from v8 ep0604 (`ci_null_resid`: `trace_station_residuals: true` + `raim_carry_invariant: true`, trace-only + the
pinned screen; products must hash `e0d234ee…`) supplies per-LOS post-fit code AND phase residuals per epoch if the writer
rides the re-hosted pool (if it still writes zero rows on this lineage — the known gap — the writer is re-pointed at
`qc_amb` as a trace-only build item and the leg re-run).  Reads over the settled window k = 10..60, per (station, satellite,
signal): the residual MEAN (a constant no state absorbed), its rms about the mean, and its elevation dependence (bins
15–25°, 25–45°, > 45°).  Pre-registered discriminators, each with the mechanism it names:

| signature | mechanism it names |
|---|---|
| rms(C2W)/rms(C1C) ≈ 1.65, rms(C5)/rms(C1C) ≈ 1.79, same sign per LOS | ionosphere representation (the SH field / per-LOS deviation cannot follow the slant) |
| ratios ≈ 1, per-(station, signal) CONSTANT mean of metres, flat in elevation | a receiver code bias not absorbed — the rx-CB state's gate / withhold / canonical-signal keying (§21.18's frame-blind gate is the named suspect) |
| ratios ≈ 1, mean ≈ 0, rms ∝ 1/sin(el), phase clean | code multipath / observation noise model (then the 0.3 m code σ is wrong, not the model) |
| per-satellite constant common to all stations | a satellite code-bias / clock datum term (the W3 constants) |
| the phase residual of the same LOS shares the signature | geometry / troposphere / mapping (excluded so far by the cm phase rms) |

Bars: A1 the per-station code rms of the leg reproduces `network_pf_keyed.csv`'s ranking (the trace is the same statistic);
A2 the dominant signature is named by the table with > 70 % of the pooled code residual variance in ONE row; A3 the named
mechanism's fix is pre-registered as the next producer card WITH a wire-vs-CODE bar (W1's within-arc rms halved on a 60-epoch
leg, then on a seed) — the first time this project has had a truth-referenced wire bar to build against.

**§24.6 RESULTS — the code residual is a persistent per-LINE-OF-SIGHT level of metres, non-dispersive, with no home in the model
(2026-09-14 ~05:30 local; worker on worktree-agent-ad8d3f6516c27591c `7ece11f`, merged `0dac1cc`: `network.trace_postfit_rows` → `logs/
network_pf_rows.csv` (signed `resid_m = y − A_s·x_s − A_r·x_r`, `w`, `elev_deg` per row, written inside the P1-4 pass; smoke ON/OFF both
`87a33bf0…`) and `scripts/pf_rows_attrib.py`; leg `ci_null_rows` from v8 ep0604 with the pinned screen, products `e0d234ee…`, 145 587 rows
over 60 epochs; manager grade + the LOS forensic below).**

**A1 PASS**: the dump reproduces the leg's own `pf_keyed` STA statistic at all 3 111 (tow, station) cells (max |Δ| 5e-5) and its
per-station ranking at the same epochs (Spearman 0.996); the ranking is NOT stationary over the day (Spearman 0.53 against
hours 2–5: GOPE 3.6 → 9.1, HOLB 8.0 → 13.0, JPLM 10.8 → 16.3 m) — a per-LOS population, not fixed station constants.

**The shape (window k = 10..60, 1 303 LOS-signal series, pooled code rms 4.33 m; phase rms 0.3 mm — numerically zero under
free ambiguities).**  The per-LOS MEAN carries **92.3 %** of the sum of squares (|mean| p50 1.76 / p90 6.62 / max 29.8 m; rms
about the mean p50 0.56 m), persists across window halves (r 0.91, slope 0.87), and is ×3.2 larger below 25° than above 45°.
It is NOT dispersive: rms(C2W)/rms(C1C) = 0.87 and rms(C5)/rms(C1C) = 0.70 against the 1.65 / 1.79 an ionospheric error
carries; the between-signal energy is 12× what an ionospheric pattern allows; the C1C and C2W per-LOS means are uncorrelated
(r 0.03).  Nested variance decomposition: per-(station, signal) constants **20.2 %**, per-(satellite, signal) constants 8.3 %,
station-epoch and satellite-epoch common modes 0.5 % each, **per-LOS signal-common 51.9 %**, per-LOS dispersive 16.4 %,
remainder 2.2 %.  The seven stations the rx-CB gate refuses all day carry the DCB signature the (station, signal) constants
predict — C1C mean −2.86 m vs +0.40 for the rest, C2W +1.95 vs −0.49, opposite signs at 6 of 7 (GAMB −6.1 / +4.1, CPVG −4.8 /
+2.1, LCK4 −3.4 / +2.7 …) — §21.18's frame-blind gate priced at a fifth of the code residual variance.  A2 by the letter: the
largest row is "the per-LOS residual after every state row" at 70.6 %; the ionosphere row is shape-refuted.

**The LOS forensic (manager, 05:20).**  JPLM/G10 (C1W / C2W / C5Q ≈ −16 / −15 / −19 m at k = 10, el 23.4°, sliding to −27 / −27 /
−30 m by k = 45 at 17.8° — signal-common, growing as the satellite sets) is the worst of 493 LOS with ≥ 30 rows; the levels
correlate with NEITHER the Sagnac term (r −0.015; Sagnac rms 15.7 m across the LOS — an omitted or mis-signed Earth-rotation
correction is excluded) NOR the receiver-clock light-time term ρ̇·δt_r (r −0.13; JPLM's receiver clock is +260 km ≈ 0.87 ms, the
term 0.43 m); |level| vs 1/sin(el) r +0.33, and a per-station `a + b/sin(el)` fit explains only **8.2 %** of the level SS — the
levels are not a zenith-referenced station pattern either.  What the network's own zenith channel shows is the finding of the
hour: **the per-station ZWD states are unphysical** — SCTB −16.2 m (min −34.7), JPLM +21.7, MAJU −12.2, GOP6 −10.7, OAFA −8.0,
KRGG −4.9, WROC −4.4 m against a physical 0–0.4 m — the white-sink ZWD absorbs tens of metres of station-common, mapping-shaped
error every epoch, and the per-LOS levels are what the mapping function cannot fit.  The iono states for the worst LOS are
ordinary (JPLM/G10 slant 10–12 m L1 at 17–23°).  A non-dispersive, station-common, mapping-shaped error of tens of metres has
no physical source at an IGS site with ITRF coordinates; the discriminating test is a plain standalone PPP of the same
stations on CODE products (JPLM, SCTB, MAJU, and WROC as the control), which decides between the observations at those sites
and the network's per-station modeling — running as this is written.

**The standalone-PPP forensic (manager, 05:45) — the observations and the shared single-station model are FINE; the defect is the
network's own estimation.**  Four network stations run as plain PPP on CODE SP3 / CLK / .BIA (`config/ppp_internal.json`, GPS-only,
15° mask, 2 h from 00:00, `runs/_ci_ppp_*`), graded on the last 30 min against the ITRF2020 roster:

| station (network code rms, network ZWD) | 3D p50 | E / N / U | 3D at 30 / 60 min | tropo state | IF code resid rms | phase rms |
|---|---|---|---|---|---|---|
| WROC00POL (1.8 m, −4.4 m) | **0.032** | −0.026 / +0.002 / +0.013 | 0.17 / 0.07 | 0.090 | 0.42 | 0.006 |
| JPLM00USA (9.6 m, +21.7 m) | **0.118** | +0.018 / −0.021 / −0.116 | 11.2 / 3.97 | 0.107 | 0.47 | 0.007 |
| SCTB00ATA (7.2 m, −16.2 m) | **0.186** | +0.069 / +0.013 / +0.172 | 1.60 / 0.24 | −0.122 | 1.41 | 0.009 |
| MAJU00MHL (8.7 m, −12.2 m) | **0.160** | +0.089 / +0.055 / +0.122 | 4.41 / 1.88 | 0.386 | 0.94 | 0.018 |

JPLM/G10 — the network's worst LOS at −25 m — reads **+0.04 m mean / 0.24 m rms** standalone.  Every geometric explanation
of the network's per-LOS levels is excluded by direct test on the 493 LOS of `ci_null_rows`: the Sagnac term (r −0.015),
the receiver-clock light-time term (r −0.13), a light-time or transmit-time error (corr with the range rate +0.125, slope
+1 ms), and a per-station position-error vector (a 4-parameter fit per station explains 42.8 % of the level SS with fitted
|d| of 3–20 m — the roster is ITRF2020 to 1 mm, so these are not coordinates).  The levels are metres, per LOS, non-dispersive,
persistent, present only in the network's uncombined multi-station solve, whose per-station ZWD sits at ±5–35 m to
accommodate them.  **Conclusion: the network producer's code model — the uncombined per-signal code rows with estimated
satellite code biases (W6), carried receiver code biases (frozen at their first-epoch seed, q 1e-9), the per-LOS ionosphere
tied to the SH field, and the per-epoch ZWD sink — is inconsistent with the data by metres at every station, worst at the
isolated ones; the satellite clock, which the code determines, inherits it (§24.5), and the wire's metre-level inconsistency
with CODE, the consumer's failure to converge (§24.3), and every "level channel" finding since July are downstream of it.**

