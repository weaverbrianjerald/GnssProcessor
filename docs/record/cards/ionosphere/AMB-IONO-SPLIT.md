# AMB-IONO: the ambiguity/iono split over-count on the quiet sim bed

**STATUS**

- **id:** AMB-IONO
- **topic:** ionosphere
- **status:** closed
- **summary:** Answered: the diagonal leveling carry re-asserts each LOS's iono direction once per band (B_fit 3.00), freezing the ambiguity level; F3's joint block removed it and is the float vehicle's structural block since RF-1.
- **dates in the text:** 2026-07-30 to 2026-09-04
- **knobs / posture:** `network.phase_leveling_los_scale` (F1, BUILT and VALIDATED sim 3 h, default OFF), `network.phase_leveling_local_block` (F3, BUILT and VALIDATED sim 3 h, default OFF, fingerprinted); F2 (per-LOS block) RETIRED; 3 h arms set `processing.max_duration_s` 10800; bed posture: `phase_leveling` on, free per-LOS slant iono, FLOAT only
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/camp60wu_quiet24h_sticky` (24 h bed), `runs/simnet60_quiet_wu93_24h` (fixture), `runs/nonint_base3h`, `runs/nonint_los3h` (F1), `runs/nonint_losblock3h` (F3), `runs/nonint_base3h_f1exe` and `runs/nonint_base3h_f3exe` (OFF twins), `runs/nonint_*` (round-2 arms)
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` (preamble), §0, §1, §2 (2.1 to 2.7), §3, §3.1, §4, §5, §5.1, §8

**Outcome.** The question: why a zero-slip 24 h sim bed never gave per-band integers (settled L1C med|e| 2.87 cyc). Answered: the diagonal leveling carry re-asserts each line of sight's ionosphere direction once per band (B_fit 3.00), freezing the level at a code value; responses, multipath, the grader and the seeds are FALSIFIED. F3's joint block removed the over-count (B 1.000, L1C 0.217 cyc at 3 h) and is the float vehicle's structural block since [RF-1](../../refound/RF-1_float_ambiguities_design.md); integers continue in [ARL](../../refound/ARL_integer_ambiguity_ladder_2026-10-09.md). Status reviewed 2026-10-13.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
# SPEC — the AMBIGUITY / IONO SPLIT OVER-COUNT
### (why the network producer never reaches per-band integers on the QUIET simulated bed: B independent per-band carry rows re-assert ONE iono-direction datum B times per epoch — p_k = B·p_{k−1} + p_code, K_eff = (B+1)/(B−1) = 2)

2026-09-04.  Companions: `docs/SPEC_synthetic_minimum_network.md` (§13.8 the 1 h rule for the
integer question; §18 the stability campaign whose flagship `runs/camp60wu_quiet24h_sticky` is
this bed), `docs/SPEC_card1_coupling_carry.md` (Card 1 — the local↔SHARED half of the same
defect, fixed), `docs/SPEC_slip_relevel_walk.md` / `docs/SPEC_lane_slip_response.md` (the
response edges §5 must honour), `docs/SPEC_redundancy_census.md` §D1 (the field's
`K_eff(30) ≈ 2`, which §3 closes in form), `docs/CONFIG_promotion_ledger.md` Tier 3 (the two
rows this card adds).

---

## §0 READ THIS FIRST — status, and the verdict

| section | status |
|---|---|
| §1–§4 | **live** — round-1 measurements on the 24 h bed, five independent lenses (theory, carry code-read, event-conditioning, time-structure, truth-iono), 2026-09-04 |
| §2.7 / §3.1 | **round-2 amendments** (2026-09-04, two adversarial lenses): B_fit = 3.00 ± 0.03; three round-1 datapoints RETRACTED; puzzles (a),(b) CLOSED; the station-common channel MEASURED (ZWD + birth-frame inheritance) |
| §5 fix ladder | live — F1 `network.phase_leveling_los_scale` BUILT + **VALIDATED sim 3 h** (`runs/nonint_los3h`, §6.7); F2 RETIRED (adds nothing over F1 along the iono direction); **F3 `network.phase_leveling_local_block` BUILT + VALIDATED sim 3 h** (spec §5.1; results §6.8, round 4) |
| **§6.3 predictions** | **PRE-REGISTERED** — written 2026-09-04 BEFORE any round-2 arm result was read (`runs/nonint_*/ambint.txt` deliberately not opened).  The prediction rows are never edited |
| **§6.7 results** | **round-2 amendment** — every §6.3 row graded CONFIRMED / FALSIFIED / MIXED against `runs/nonint_*`; three point predictions were WRONG for one reason (§6.7.2) |
| **§6.8 F3 results** | **round-4 amendment** (2026-09-04) — F3 graded against its §5.1 / r3_5 pre-registration: **BEAT it 2.7×** (0.217 cyc / 79.3 % vs 0.55–0.62 / 42–46 %) and the reason is named (§6.8.2); wire + consumer graded on five arms (§6.8.4); the PB-gauge handover ceiling measured at 1e-4 and 1e-2 m² (§6.8.3); 24 h reading pre-registered (§6.8.6) |
| §7 open | live — re-cut in round 2; round-4 closures and items 15–21 |

**Provenance.**  Round-1 scripts live under the session scratchpad
`C:/Users/weaverb/AppData/Local/Temp/claude/C--GnssProcessor/4d31ca6a-c7d9-4b89-895c-055dcfa292a7/scratchpad/{theory,lens_memory,evt,lensT,iono,record}/`
— **session-scoped**; the numbers below are the record, the scripts are the recipe.  Every
input is a repo artifact (§1 traces, §8 commands), so any figure can be re-derived.

**The question the user asked: why does simulated data — zero true slips, known truth,
24 h — never give integers?**

**The answer (DERIVED §3, MEASURED §2).**  The per-band DD ambiguity error IS the DD of the
per-LOS slant-iono error (identity slope 1.00–1.03, r² ≥ 0.97 settled): the ambiguity/iono
SPLIT of every LOS is frozen at a code-level value within ~3 epochs of the arc's first warm
inject and never averages down.  The phase-leveling `LocalCarry` stashes ONE scalar variance
per (station, sat, signal) column — the Card-1 pure conditional on the SHARED border,
MARGINAL over the LOS's own free iono and its sibling bands — and injects every column as its
OWN row.  Because the B band ambiguities of one LOS are affine in the epoch's single free
iono I (λ_b N_b = c_b + μ_b I), the B rows re-assert the same iono-direction datum B times per
epoch: p_k = B·p_{k−1} + p_code instead of p_{k−1} + p_code.  The formal σ collapses
geometrically to the phase floor (0.009 cyc DD at 24 h, 3–5× below the CRLB of a perfect
batch), the VALUE freezes at a (1−1/B)(1/B)^j-weighted average of the first ~3 warm epochs'
code solutions, K_eff = (B+1)/(B−1) = 2 (measured 2.9), and med|e| sits at ≈ 0.48·σ₁ ≈ 2.9 cyc
L1C for the arc's life.  Card 1 fixed local↔shared; this is the local↔LOCAL instance of the
standing "independence-assuming diagonal carry = the lock-in" defect.  Not the iono model
(no constraint exists on this posture), not the responses (virgin chords carry the full
error), not the grader, not multipath.  Exact along the iono direction in ~15 LOC (F1, §5).

**Round-2 addendum (2026-09-04, §6.7).**  Two adversarial lenses CONFIRMED the mechanism with
**B_fit = 3.00 ± 0.03** on 9,507 mid-run newborn chords (§2.7).  F1 took the settled L1C error
**2.72 → 1.28 cyc**, WL round0 36.5 → 87.4 %, EWL → 100 %, |e|/σ 83 → 1.9 and T4 attribution
erases 288 → 0, and landed in §6.4's pre-registered **"intermediate"** row: the remainder is the
STATION-COMMON over-count (§3.1) — half iono-direction, half geometric in the DD identity —
which F3 (§5.1) is built to remove.  The var-floor point predictions (0.27 / 0.134 cyc) were
WRONG: both floor arms got WORSE in total (3.46 / 3.64) because a floor releases the
geometric/station-common share the stiff carry had pinned (§6.7.2).  OFF byte-identity of the
F1 build is MEASURED (sha256-identical products across the two exes).

**Round-4 addendum (2026-09-04, §6.8).**  F3 `network.phase_leveling_local_block` took the
settled L1C error **1.28 → 0.217 cyc** (round0 23.1 → 79.3 %; WL 98.8 %, EWL 100 %; **0.187 /
83.1 %** with the handover ceiling lifted to 1e-2 m²), the ZWD to 5.5 mm with |err|/σ 0.94, and
B = 1.000 ± 0.002 with no mature-leg floor — 2.7× BETTER than its own pre-registration, because
the round-3 "honest per-LOS floor" of 0.57 cyc was 54 %-by-variance the wandering ZWD leaked into
the per-LOS levels plus the diagonal carry's cross-band loss; OFF byte-identity holds across all
three exes, and the one regression is inherited, not F3's: the 1e-4 m² PB-gauge handover ceiling
refuses every honest-σ successor (§6.8.3).

---

## §1 The bed and the fresh 24 h grade

| item | value |
|---|---|
| producer run | `runs/camp60wu_quiet24h_sticky` — 2879 epochs @ 30 s, tow 259230 → 345570, 60 sta, GPS L1C/L2W/L5Q + Galileo L1C/L5Q/L7Q, FLOAT only (`ambiguity_resolution.mode 0`), zero true slips |
| fixture | `runs/simnet60_quiet_wu93_24h` (per-station `truth_ambiguities.csv`, `truth_atmo.csv` with `I_L1_m` per LOS-epoch, `truth_bias.csv`) |
| config | `config/test_suite/_camp60wu_quiet24h_sticky.json`: `phase_leveling` ON (`q_amb` 0, `var_floor_m2` 0, `warmup_epochs` 6 RUN-level), `carry_invariant` OFF, `carry_conditional_coupling` ON (Card 1), `shared_carry_full_cov` ON, `carry_coupling_time_update` ON (1d), `carry_iono` OFF, `estimate_iono_sh` OFF, `ionosphere_weighting.enable` false ⇒ free per-LOS slant iono, NO iono constraint of any kind; `code_bias_gf_datum` ON; `relevel_free_epoch` ON; `raim_attribution` ON; `if_phase_td` ON; `mw_detect` ON; `sticky_pivot` ON |
| traces | `logs/network_dd_fractional.csv` (7,851,033 rows), `network_ionosphere.csv` (`iono_total_m = x_lin + x_r`, L1 m, delay-positive — gradable on this posture), `network_events.csv`, `network_if_slip.csv`, `network_raim_attrib.csv`, `main_log.log` (run-end counters :4645-4675) |
| grader | `scripts/simnet_amb_integer.py` — e = dd_cyc − DD_true (full cycles); lanes e_WL = e_L1C − e_L2W, e_EWL = e_L2W − e_L5Q |

**Fresh 24 h grade** (MEASURED, `scripts/simnet_amb_integer.py`; lens re-grades agree to 0.02 cyc):

| band / lane | med\|e\| cyc | round(e)=0 | last 25 % med\|e\| | ratio to L1C |
|---|---|---|---|---|
| L1C | 2.87 | 10.0 % | 2.67 | 1 |
| L2W | 3.58 | 7–10 % | 3.43 | 1.25–1.28 |
| L5Q | 3.79 | 7–10 % | 3.53 | 1.32 |
| L7Q | 3.77 | 7–10 % | 3.40 | 1.31 |
| WL(1,2) GPS | 0.763 | 35.3 % | 35.8 % | — |
| EWL(2,5) GPS | 0.152 | 95.1 % | — | — |
| EWL(5,7) Gal | 0.096 | 99.9 % | — | — |

The band ratio 1 : 1.25–1.28 : 1.32 is the IONO direction (1, 1.2833, 1.3391), not geometry
(1, 0.779, 0.747).  Flat over 24 h; hourly med|e| L1C oscillates 2.27–3.74 with no decay.

---

## §2 The five measurements

### 2.1 The DD error IS the per-LOS ambiguity/iono split (MEASURED)

`scratchpad/iono/dd_identity.py` (120 s stride, 653k L1C DD rows) and
`scratchpad/lensT/pass4_iono_split.py` (full rate, GPS triples):

| test | value |
|---|---|
| e_b regressed on μ_b·DD(δI)/λ_b, 24 h slope / r² | L1C 1.120 / 0.875 (hour 0 = 2.14, ignition); L2W 1.075 / 0.943; L5Q 1.067 / 0.951; L7Q 1.067 / 0.946 |
| same, settled hours ≥ 5 | slope 0.99–1.03, r² 0.97–0.999 (every band) |
| residual e − μ·DD(δI)/λ (unit slope) | med −0.006, med\|.\| 0.113 cyc, p90 1.08; round0 70 % |
| iono coefficient b·λ₁ vs DD(I_est − I_true) | corr +0.9989, LS slope +0.9992, med\|diff\| 0.0121 m (settled 0.0099) |
| LS split of (e_L1C, e_L2W, e_L5Q) on geo / iono | med\|geo\| 0.100, med\|iono\| 2.740, **off-plane residual 0.0123 cyc = 2.3 mm** (phase residuals are mm) |
| per-epoch two-way decomposition of δI = I_est − I_true (`decompose2.py`, 690k LOS-epochs) | network-common +1.490 m (increment 0.013 m/epoch, a frozen frame); station×system rms 1.545 m; satellite rms 1.763 m; **per-LOS residual r rms 0.398 m, med 0.240, p90 0.634** — DD(δI) med\|.\| 0.525 m = 2.76 cyc L1 |
| r rms by hour | 0.618 (h0), 0.479, 0.448, 0.417, then flat 0.354–0.390 for h4–h23 — no decline over 20 h |
| r vs TEC / elevation / seeds | corr(r, I_true) 0.0095; med\|r\| 0.260/0.239/0.236/0.230 m for 10–20/20–30/30–50/>50°; corr with Klobuchar 0.011, with x_lin seed 0.09–0.14, with previous arc of the same (sta,sat) 0.02 |

The frame terms (station×system, satellite) are large but DD-invariant; the DD ambiguity
error is entirely the per-LOS residual r.

### 2.2 Time structure: a level frozen at birth, not noise, not a walk (MEASURED)

`scratchpad/lensT/pass2_timestruct.py`, `pass3_birth_steps.py` (L1C unless noted):

| statistic | value |
|---|---|
| between-chord share of variance (runs ≥ 200 ep) | 0.883 / 0.909 / 0.931 / 0.953 (L1C/L2W/L5Q/L7Q) |
| corr(e_0, e_100) step-free runs ≥ 101 (n=2863) | +0.9926; med\|e_0 − e_100\| 0.198, \|e_3 − e_100\| 0.082, \|e_10 − e_100\| 0.061 cyc; med\|e_100\| 3.078 |
| within-run SD (step-free) | 0.045 cyc; lag-1 autocorr 0.946; D(1)/(√2·SD) = 0.006–0.009 (white would be 1) |
| structure function D(τ) = med\|e(t+τ) − e(t)\| | τ = 1/10/100/500: 0.0013 / 0.0078 / 0.0415 / 0.0962 cyc (slope +0.76: a 2 cm drift at 4 h, not a walk) |
| \|e\| vs chord age, bins 1–10/10–30/30–100/100–300/300–1000 | 3.127 / 3.006 / 2.853 / 2.757 / 2.652 → log-log slope **−0.033**; iono coefficient b slope −0.030 |
| GEO coefficient a, same bins | 0.289 / 0.177 / 0.115 / 0.061 / 0.052 → slope **−0.357** (geometry/clock/tropo information DOES accumulate through the border) |
| per-leg level r by age (`iono/level_seed_test2.py`, 332 arcs) — ⚠ **RETRACTED in round 2 (§2.7)**: tail-dominated rms on a 6-station subset; all-station mid-run profile is rms 0.586 → 0.434 m, corr(r0, settled) 0.902 | rms 1.49 / 1.12 / 0.90 / 0.68 / 0.59 / 0.54 / 0.44 m at ages 0–6, then 0.42–0.44 to age 50; corr(r(q), settled) 0.58 (age 0) → 0.94 (age 6) → 0.99 (age 20) |
| lanes | EWL within-run SD 0.026, WL 0.034 cyc; EWL projection −0.056·b (2.74·0.056 = 0.153 ≈ med\|e_EWL\| 0.152), WL −0.283·b (0.78 ≈ 0.763) — the lanes inherit the same scalar |

### 2.3 Responses are exonerated (MEASURED)

`scratchpad/evt/{build_events,age_grade,paired_delta,ifslip_count,mw_steps,t4_cluster}.py`:

| class | count / 24 h | paired same-quadruple \|Δe\| across the event | age imprint |
|---|---|---|---|
| **VIRGIN chords** (no in-arc IFTD/MW/T4/ANCHOR on any of 4 legs) | 97,942 L1C rows (15 %) | — | **L1C 2.72 / L2W 3.36 / L5Q 3.52 / L7Q 3.69 cyc; WL 38.9 %, EWL 94.8 %** — the population value |
| IF-TD `refsat_reset` (legacy per-station reference gate, `IfPhaseSlipDetector.cpp:617-620`, `ref_gate_.floor_frac` 0.5·λ_IF ≈ 5.35 cm, Card-R gate POSTURE OFF) | 2482 = 1917 acting + 565 diagnostic; pool arm **0 / 2,754,791**; 1.33 per station-hour at the highest-elevation reference (el p50 57°) | touched L1C 0.06 / 0.04 / 0.03 cyc at 0 / +30 / +150 s; L2W 0.02; untouched third band 0.00–0.01 | none (2.887 vs never 2.891) |
| MW warm refloat | 44 (of 186 detections) | 0.02–0.03 | none |
| **T4 attribution ARC** (all-band erase, `NetworkRaim.cpp:632` criterion, erase :785-825) | 447 events → 1341 carries; 377 in h0–7, 68 in the last 5 epochs | **3.23 / 3.74 / 3.90 / 3.76 cyc at +30 s** on arcs already at 6.30 / 7.22 / 7.42 / 6.72 before; step 68 % iono-direction | 2.0–2.6 % of rows, 3.3–3.8 % of Σ\|e\|; excess over virgin ≈ 1–2 % of mass |
| PB-gauge anchor handovers (1035; 1(d) erased 93) | ~43/h | — | none (2.77 / 2.72 / 2.88 at 0–5 / 5–20 / 60–200 ep) |

Acting responses run at 0.105 per LOS-hour; the only level-changing class (T4) at 0.019.
**≥ 96 % of rows sit on chords whose level no response ever changed.**  The IF-TD refloat is
band-scoped (`NetworkSlipQc.cpp:909-918` refloats only sig1/sig2), so the third band + the
Card-1 regressors pin the LOS and the SPEC_slip_relevel_walk "one epoch of code" cost does
not actuate here (it WOULD on a dual-frequency lineage — open, §7).

### 2.4 The formal σ collapses geometrically while the value is frozen (MEASURED)

`scratchpad/lens_memory/ddfrac_sigma_vs_age.py` (median `dd_sigma_cyc`, L1C, by run epoch k;
med|e| truth-joined at the same epochs):

| k | 6 (last cold) | **7 (first warm inject)** | 8 | 9 | 10 | 20 | 60 | 120 | 1080 | 2870 |
|---|---|---|---|---|---|---|---|---|---|---|
| dd_sigma_cyc | 5.694 | **2.817 (ratio 0.495)** | 1.734 | 1.139 | 0.803 | 0.214 | 0.074 | 0.036 | 0.012 | 0.009 |
| med\|e\| | 8.8 | 8.0 | 7.6 | 7.3 | 7.2 | 6.6 | 3.40 | 2.52 | 3.14 | 3.16 |
| ratio σ_k/σ_{k−1} | — | 0.495 | 0.616 | 0.657 | 0.705 | | | | | |

Per-arc (`lensT/pass3`, runs ≥ 31, n=15,816 per age): σ at ages 0..10 = 3.414, 1.703, 0.946,
0.543, 0.319, 0.195, 0.127, 0.092, 0.073, 0.061, 0.053 cyc (×0.55/epoch for 5 epochs) while
med|e| = 3.59, 3.30, 3.21, 3.17, 3.15, 3.15, …, 3.03 — flat.  Calibrated only at age 0
(ratio 1.27); settled **|e|/σ = 158 / 202 / 204 / 205** (p90 632–941); |e| < 3σ on 3 % of rows.
A code-only 1/√K recursion would give 1.08 cyc at K = 10; observed 0.053.  The first-inject
ratio 0.495 is the cleanest single number: a correct recursion gives 1/√2 = 0.707.
⚠ **Round 2 (§2.7):** the ALL-ROWS k=6→7 figure is a population-mix coincidence of the
ignition cohort (0.39–0.84 by elevation); the clean statistic is the mid-run newborn ladder
0.499/0.555/0.574 (B_fit 3.00), which says the same thing for the right reason.

### 2.5 CRLB vs measured: K_eff ≈ 2–3 epochs (MEASURED / DERIVED)

`scratchpad/theory/crlb_amb.py` — per-epoch Schur elimination of the free I, 10 → 60 → 10°
pass, σ_P = 0.3 / sin el, σ_φ = 0.003 / sin el (the producer's and the sim's IDENTICAL
elevation laws, `StochasticModel.cpp:124-139`, `src/sim/main.cpp:2428-2430`):

| K epochs | 1 | 3 | 10 | 30 | 100 | 300 | 720 | 2000 |
|---|---|---|---|---|---|---|---|---|
| ZD σ(N1) cyc, white code | 3.45 | 1.95 | 0.99 | 0.48 | 0.164 | 0.055 | 0.033 | 0.019 |
| DD (4 legs) = 2× | 6.90 | 3.90 | 1.98 | 0.96 | 0.33 | 0.11 | 0.066 | 0.038 |

Gauss-Markov multipath (τ 300 s, 2–3 cm) adds ≤ +9 % at K=1, ≤ +6 % settled (MC 200).  Lanes
ZD K=1 → 720: WL 0.984 → 0.009, EWL 0.216 → 0.002 cyc.  Settled correct-estimator
expectation (uniform ages 1..720, 4 legs): **L1C med|e| 0.120 cyc, p90 0.58, round0 88 %;
WL 0.034 / 98.3 %; EWL 0.007 / 100 %**.

Measured: the record is ONE zero-mean Gaussian per LOS-pair (σ from med|e| 4.26 cyc vs from
round0 3.98 — 7 % apart; it predicts the lanes: WL 0.814 / 32 % vs measured 0.763 / 35 %,
EWL 0.158 / 97 % vs 0.152 / 95 %).  Against the run's OWN epoch-1 formal DD σ (7.203 cyc,
n=1008, no carry yet): **K_eff = (7.2/4.26)² = 2.9** (3.3 by the round0 route).  The formal
σ at 12–24 h (0.013 cyc) is 3–5× BELOW the CRLB of a perfect 24 h batch (0.039–0.066).
⚠ **Round 2 (§2.7):** 2.9 used the cold epoch-1 DD σ as σ₁; the mid-run per-leg settled/birth
ratio 0.69 gives **K_eff = 2.1** = (B+1)/(B−1) at B = 3.

### 2.6 The truth-iono channel: the LOS's own code never reaches the level (MEASURED)

`scratchpad/iono/level_seed_test3.py` (proxy g = single-epoch GF-code iono error from
L1 − C1 + 2·I_true and MW; proxies validated: GF-phase vs truth-I increments slope 0.6466 vs
expected 0.6469, resid 1.45 cm; C1 noise white, lag-1 of differences −0.495):

| age q | 1 | 2 | 3–6 | 7 | ≥ 8 | settled |
|---|---|---|---|---|---|---|
| gain slope(Δr, Δg) | +0.014 | +0.005 | +0.003 | +0.0009 | ≤ +0.0003 | −0.00003 |

A free single-epoch LOS would give 1; a correct running mean 1/q.  corr(r(0), g(0)) = 0.10;
no lag structure (|corr| ≤ 0.12, lags −3..+3).
⚠ **RETRACTED as a datapoint in round 2 (§2.7):** g was the GEOMETRY-FREE combination
(e1−e2)/(1−μ₂), whose expected correlation with the absolute-code solution is 0.17 even under
the mechanism.  With the absolute-code proxy: corr(r(0), g(0)) = +0.62, increment gain
0.145/0.044/0.014/0.007 at ages 1..4 (geometric 1/3 = 1/B decay), corr(r(20), mean g(0..19))
= +0.06.  The CONCLUSION (no running-mean accumulation) stands on the replacement instrument.  30 s increments: δI 0.048 m rms vs truth
0.040; I_est increments track truth with slope 1.0014, corr 0.95 — the TEMPORAL motion is
phase-tracked exactly; only the LEVEL is wrong.  `filter.process_noise.ionosphere_q` is
INERT on the network path (parsed `GnssConfigLoad.cpp:1684`, consumed only by
`KalmanFilter::predict`; zero call sites in `ApplicationNetwork*.cpp` / `Network*.cpp`).

### 2.7 Round-2 amendments to §2 (adversarial verification, 2026-09-04)

Two independent lenses attacked claim (6) on the 24 h bed: theory/code
(`scratchpad/adv/{sigma_ratio_head,age_profile_all,recursion_exact,zwd_freeze}.py`) and data
(`scratchpad/verify/{build_los,bfit,puzzles,refute,rebirth_bands}.py`, 2,762,871 joined
LOS-epochs, all 60 stations at 30 s).  Session-scoped scripts; the numbers are the record.

| item | round-1 reading | round-2 measurement | verdict |
|---|---|---|---|
| **B, the count** | inferred from one ratio | **B_fit = 3.00 ± 0.03**: model-free implied B_a = (p_a−1)/p_{a−1} at ages 1/2/3 = 3.02/2.99/2.95 (L1C, n = 9,507 mid-run single-newborn chords), 3.02/3.01/2.98 (L2W), 3.02/3.00/2.97 (L5Q, L7Q); GPS = Galileo ± 0.01; **2.95–3.01 again on 656 T4-erase re-births at 10–90°**.  Exact numerical B=3 recursion with the run's own weights reproduces the 10-epoch ladder 0.499/0.555/0.574/0.588/0.610/0.655/0.721/0.788/0.837/0.872 (model 0.497/0.554/0.572/0.584/0.601/0.636/0.695/0.760/0.814/0.852, incl. the phase-noise saturation); B=2 (0.573/0.651/0.681) and B=1 (0.698/0.807/0.856) excluded by the first three steps.  Per-chord (event-free, n=1794): 0.527 pooled, p25–p75 0.409–0.716 | **CONFIRMED** |
| value weights (1−1/B)(1/B)^j | derived | measured c_a = 0.224/0.059/0.026/0.010 (model 0.250/0.077/0.025/0.008); settled/birth 0.69 (model 0.707); corr(r0, r100) 0.854 | CONFIRMED |
| §2.4 "0.495 at k=6→7" | the cleanest number | a population-mix coincidence: the IGNITION cohort (all four legs born cold) reads 0.394 / 0.559 / 0.841 by min-leg elevation 10–20 / 20–40 / 40–90° (implied B 5.5 / 2.2 / 0.41 — Card-1d tu_var-inflated cold rows at 1.0–7.5 m²), and its VALUE does not step at k = 6..9 (1−slope −0.10/−0.05/0.00); only mid-run births carry the clean 0.499 | RE-ATTRIBUTED — never quote an ignition-epoch ratio as a B test |
| §2.5 K_eff 2.9 | (7.2/4.26)² | σ₁ = 7.2 is the cold epoch-1 DD σ; per-leg settled/birth 0.69 on mid-run arcs ⇒ **K_eff 2.1** | AMENDED |
| §2.2 per-leg r by age 1.49 → 0.44 m, corr 0.58 | lens 5, 332 arcs, 6 stations | tail-dominated rms (r(0) rms 1.49 vs med 0.65 there); all 60 stations, 4302 mid-run arcs ≥ 91 ep: rms 0.586 → 0.490 → 0.464 → 0.456 → settled 0.434 m, corr(r0, settled) 0.902, ratio 0.74 (model 0.94 / 0.71); ignition arcs (829) 3.22 → 0.84 (k5) → **1.24 (k9)** → 0.37 (k60) — a cold-border leak, the other population | **RETRACTED — puzzle (a) CLOSED**: age 0 IS the LOS's single-epoch absolute-code solve at its birth elevation — med\|r0\| 0.382/0.184/0.137/0.122 m at 10–12/20–30/30–45/45–90° vs 0.440/0.181/0.129/0.091 expected; x_r non-zero at age 0 (med 2.87 m), birth = first truth epoch ≥ 10° for 4105/4115 births |
| §2.6 code gain ≤ 1.4 %, corr(r0, g0) 0.10 | GF-combination proxy | expected corr 0.17 under the mechanism (see the §2.6 marker); absolute-code proxy gives 0.62 / gain 0.145/0.044/0.014 | RETRACTED as a datapoint; conclusion stands |
| lens-4 "elevation-FLAT settled level 0.47 m at 10° births vs 0.37 at 45–90°" (`lensT/pass7`) | evidence against a birth-code origin | `pass7_leg_birth_elev.py:30-31` demeaned by STATION only, so the GPS-vs-Galileo rx-DCB difference (station×system term, rms 1.5 m) leaked into r and flattened it; under station×system + satellite demeaning the settled level is elevation-dependent 0.267 → 0.107–0.119 m = 0.7× the birth-epoch single-epoch code σ | **RETRACTED** |
| §3 (e) "formal σ saturates at the phase floor" | floor ~0.01 cyc | NO floor: geometric collapse until λ²v ~ σ_φ² (k ≈ 60–120), then σ ∝ σ_φ/(λ√k) with no fixed point for B ≥ 2 (stationarity pV+B = 1 has no positive root); the 24 h value 0.009 cyc is a ~1e-6 m²/epoch Card-1d inflation (P_prev inferred, not traced — §7) | AMENDED: regime change, not a floor |
| puzzle (b) birth-spread dependence | open | birth-frame INHERITANCE — §3.1 | **CLOSED** |
| gap re-births at el ≥ 20° (429 arcs) | — | first re-inject ratio 0.734/0.745/0.734/0.721 on all four bands (the B=1 signature 0.707), then 0.562 (B=3), all bands present from age 0, no event logged: ONE effective prior row acts at the first re-inject | NEW — §7 |
| band-scoped responses | level-inert (§2.3) | refsat refloats: touched L1C/L2W σ ×1.64/×1.71 at the free epoch (the phase class 0.03 cyc), untouched L5Q ×0.994, \|Δe\| 0.063/0.025 cyc — the third band's carried row pins the LOS iono at phase precision; Galileo touched L5Q only ×1.007 (asymmetry open) | CONFIRMED |

The code lens's T4 hypothesis (§6.3 secondary) now has an ON-path instrument: T4 attribution
erases 288 (`nonint_base3h`) → **0 under F1** and 0 under both var floors (§6.7) — the
coupling row's own standardized residual was the flag (§7 item 4).

---

## §3 The mechanism (DERIVED; READ citations from the code lens)

**Model, one LOS (station r, sat s), B bands, phase exact** (σ_φ 3 mm ≪ code):

    L_b = ρ' − μ_b·I + λ_b·N_b        P_b = ρ' + μ_b·I        b = 1..B,  μ_b = (f₁/f_b)²

Given the shared border (ρ' incl. clocks and biases), the B phase rows fix the B−1 LANE
directions exactly and leave ONE free direction per LOS — the iono:

    λ_b·N_b = (L_b − ρ') + μ_b·I  ≡  c_b + μ_b·I                                    (3.1)

The only information on I — hence on the ambiguity LEVEL — is the code:
p_code = Σ_b μ_b²/σ_P,b² per epoch (≈ 77·sin²el m⁻² in I-metres for σ_P = 0.3/sin el, GPS;
Σμ² = 1 + 1.6469² + 1.7933² = 6.93).

**What the carry stores and injects (READ):**

| step | code | what it does |
|---|---|---|
| harvest | `NetworkRunContext.cpp:2899-2939` `harvestCoupling` → `est.stationRegression(est_idx, col)` per SINGLE column; `:3005` `value_abs = pa.x_lin + xr(col)`; `:3010-3016` `var = hc.first`; `:3173-3174` `LocalCarry{value_abs, var, pl_now_s, coupling}` | one scalar var per (station, sat, signal) + a regression on the SHARED border only |
| cond_var | `NetworkNormalEquations.cpp:1552-1574`: `n_c = Nrr_ldlt.solve(e_c)`, `cond_var = n_c(local_col)` (:1571) = (N_rr⁻¹)_cc | the PURE conditional on the border, **MARGINAL over every other local column** (I, sibling bands, rx clock, ZWD, rx-DCB) |
| struct | `include/filter/NetworkRunTypes.hpp:342-347` | `double var` — no local-local cross term exists to store |
| inject | `ApplicationNetworkAssembly.cpp:2884-2924`: no carry → free (:2884); gap > 120 s skip (:2890); `var_eff = var + pl_q_amb·gap` (:2909), floored by `pl_var_floor` when > 0 (:2910), × vce factor; `cb.buildCplRow(col, x_lin, carry, var_eff, gap)` (:2922) | one row per column, weight 1/var_eff |
| row | `NetworkCoupling.cpp:381-388`: `R.A_r(m0+k, local_col) = 1.0`, `A_s` shared entries only; `row.w = 1/σ²` (:298-299) | **exactly ONE local entry; no sibling-band or iono entry** |
| warm gate | `ApplicationNetwork.cpp:1450-1452` `carry_warm = pl_on && n_emitted ≥ pl_warmup` (6) | RUN-level, not per arc (§7) |
| loader | `GnssConfigLoadNetwork.cpp:2963-2973` (`phase_leveling`, `q_amb`, `warmup_epochs` 6, `max_gap_s` 120, `var_floor_m2` 0) | no cap on the carried weight on this posture |

**The over-count.**  If the column's uncertainty is iono-dominated, var_b = μ_b²·v_I with v_I
the posterior variance of I.  Row b, weight 1/var_b, +1 on λ_b N_b, projected on I by (3.1)
has precision μ_b²/var_b = 1/v_I.  The B rows are INDEPENDENT in the normals, so they SUM:

    p_I,k = B·p_I,k−1 + p_code            (correct recursion: p_I,k = p_I,k−1 + p_code)   (3.2)

**Consequences, each measured:**

| consequence | derived | measured |
|---|---|---|
| (a) first inject k=6→7: p₇ = (B+1)·p₆ (cold epochs carry nothing, p₆ = p_code) ⇒ σ ratio 1/√(B+1) | 0.500 (B=3); 0.577 (B=2); correct 0.707 | **0.495** (§2.4) |
| (b) asymptotic per-epoch ratio 1/√B | 0.577 | 0.616 / 0.657 / 0.705 at k=8/9/10 (slower: p_code adds; Card-1d κ·q·Δt inflation on present border terms adds a floor) |
| (c) value: x_k = (B·p_{k−1}·x_{k−1} + p_code·x_code,k)/p_k with p_k ≈ B^k p₀ ⇒ weight of epoch j's code solution w_j = (1−1/B)(1/B)^j = 0.667, 0.222, 0.074, 0.025, … ⇒ **frozen after ~3 warm epochs** | K_eff = 1/Σw_j² = **(B+1)/(B−1) = 2** (B=3), 3 (B=2), ∞ (B=1) | K_eff 2.9 (§2.5); level fixed by age 3–6 (§2.2); code gain ≤ 1.4 % (§2.6) |
| (d) settled error | med\|e\| = 0.674·σ₁/√K_eff = 0.48·σ₁ = 2.7–3.1 cyc for σ₁ 5.7–6.5 (cold DD) | 2.87 L1C |
| (e) formal σ saturates only at the phase floor: var_b collapses until the lane (phase-limited) variance dominates, σ_φ ≈ 3 mm ≈ 0.016 cyc ZD — ⚠ AMENDED round 2 (§2.7): not a floor but a regime change to σ ∝ σ_φ/(λ√k); the 24 h value is Card-1d inflation | DD floor ~0.01 cyc | 0.009–0.013 cyc at 12–24 h; per-leg ~1.5e-6 m² |
| (f) direction: the lanes are re-fixed by phase every epoch, the geo direction accumulates through the densely-carried border; ONLY the iono direction is over-counted | error → (1, 1.283, 1.339) | ratios 1 : 1.25–1.28 : 1.32; geo slope −0.36 vs iono −0.03 |

**Closed forms behind two standing findings.**  (i) The field's D1 (`docs/SPEC_redundancy_census.md`
§D1 / memory `project_why_non_integer_rootcause` D1, 2026-07-30): fitted σ² = σ₁²/(1+β(K−1)),
β = 0.0348, **K_eff(30) = 2.01** on a triple-frequency per-band field lineage — that is
(B+1)/(B−1) at B = 3, not a 30-epoch coincidence.  (ii) The S-system/DIA assessment
(memory `project_network_ssystem_dia_assessment`, deviation 1): "diagonal-only,
independence-assuming carry — information accumulates ~n against a time-correlated error
floor, formal variance → 0 — the mechanism behind the ep42 and G15 lock-ins".  Card 1
addressed the local↔SHARED cross-covariance; this card names the surviving local↔LOCAL
instance inside one LOS.

**The station-common residue (DERIVED, not DD-graded).**  rx clock, ZWD and the
per-constellation rx-DCB are shared by ALL n_sat·B ≈ 75 ambiguity columns of a station; the
same argument over-counts those directions ≈ 75× per epoch.  DD-invisible (cancels in the
grade) but consumer-visible: it is the natural owner of §2.1's 1.545 m station×system
level term and of the rx-CB "rolling common-mode memory" the level-budget card measured.
F1 does not touch it (§5).  ⚠ **"DD-invisible" was WRONG on this bed as it stands — §3.1.**

### 3.1 The station-common channel (round 2, MEASURED) — the residue F1 leaves

Same argument, station scale (theory skeptic, DERIVED): along a common direction d with joint
posterior C = diag(v_ind) + var_common·ddᵀ the correct prior precision is
(Σ1/v_ind)/(1 + var_common·Σ1/v_ind) → 1/var_common; the diagonal carry asserts
75/(v_ind + var_common), F1 25/(v_ind + var_common) — below 1 at the first inject (v_ind ~0.3 m²
≫ var_common ~0.004) and rising to ~14× by k ≈ 100 as v_ind → 0.3/k.  So under F1 the rx
clock (1,…,1), ZWD (mf_i) and rx-DCB_GF directions still freeze about an hour in.  The
station-common IONO shift is NOT in this class (it is the sum of per-LOS iono directions and
C5Q pins it: residual μ₅·δI = 1.79·δI per row) — F1 fixes that one exactly.

**(a) Birth-frame inheritance — DD-VISIBLE** (data skeptic, `verify/puzzles.py` (b),
`refute.py` §C).  The station×system frame term s(t) drifts: rms 0.032/0.067/0.150/0.221/
0.293/0.367 m at τ = 1/10/100/300/600/1200 epochs (satellite term 0.020 … 0.786, tails =
re-anchoring; common 0.007–0.086).  Inside a settled arc the ABSOLUTE per-leg level δI is frozen
— med|Δ δI| 0.47/0.68/1.65/3.6/5.8 cm at τ = 1/10/100/300/600 — while the demeaned r moves WITH
the frame (corr(Δr, −Δframe) +0.52 … +0.67).  Each leg therefore freezes the station-common
per-epoch error AT ITS BIRTH, and a DD of legs born at different epochs sees the difference.
Direct test on 56,991 settled L1C rows: e·λ₁ regressed on the signed 4-leg sum of the frame at
each leg's birth — **station term slope +1.18** (Huber +1.24), satellite +0.62, common +0.80;
rms(P_s) grows with birth spread 0.147/0.209/0.308/0.368/0.505 m for spreads
[30,120)/[120,300)/[300,600)/[600,1200)/[1200,3000) epochs while med|y| grows
0.368/0.526/0.558/0.609/0.595 m (slope(y ~ P_s) 3.0 at the shortest spread → 0.93: the 25-leg
consensus s(t) UNDERSTATES what a newborn inherits, so the share is a lower bound).  Only ~60 %
of the frozen level's regression weight is the LOS's own code; the rest is this inherited
station-common error.  Puzzle (b) closed.

**(b) The ZWD freeze** (theory skeptic `adv/zwd_freeze.py` on the 24 h bed; this card's
`record/zwd_freeze_arms.py` on the 3 h arms — `network_zwd.csv` vs `truth_tropo.csv`, 60 sta):

| arm | formal std_m: k=6 → k=7 → settled | rms err at k=7..10 | hour-2 rms / med\|err\| / std_m | last 25 %: per-station-mean rms / within-sta std | D(τ=200) med |
|---|---|---|---|---|---|
| 24 h bed | 0.48 → 0.159 → 0.0053 | 2.87–3.05 m | 0.40 / — / 0.0055; 0.008–0.011 by h11–23 | 0.038 / 0.013 (h2–24) | 0.017 (τ 2000) |
| base3h | 0.484 → 0.159 → 0.0054 | 2.87–3.05 | 0.401 / 0.027 / 0.0055 | 0.411 / 0.0084 (between-share 0.98) | 0.037 |
| **los3h (F1)** | 0.484 → 0.210 → **0.050** | 2.88–3.05 | **0.202 / 0.127 / 0.051** | 0.183 / 0.052 (0.92) | 0.100 |
| vf005 | 0.484 → 0.159 → 0.054 | 2.87–3.05 | 0.455 / 0.289 / 0.054 | 0.436 / 0.069 (0.96) | 0.176 |
| ploff (1 h) | 0.484 → 0.482 → 0.475 | 2.90–3.11 | h1: 0.516 / 0.347 / 0.475 | — | — |

Reading.  Under the diagonal carry the ZWD formal σ collapses 90× in 24 epochs while the actual
error balloons to 3 m at the first injects (the coupling rows drag ~75 frozen locals against a
still-converging border) and then relaxes over hours BY GEOMETRY — the mapping function
re-observes ZWD from phase — not by the carry: a correct estimator with 75 code rows/epoch would
sit at 0.05 m by k = 100 (measured 0.38) and 0.02 by k = 480 (0.13).  At 3 h base3h shows a
MEDIAN ZWD error of 2.7 cm but an rms of 0.40–0.47 m (per-station-mean share 0.98 — a few
stations frozen far off; |err|/σ 4.9 at the median).  This slow relaxation is round-1 lens-4's
decaying DD "geo" component (0.29 → 0.05 cyc) = DD(mf)·δZWD — the station-common channel is
DD-visible by a SECOND route.  Under F1 the ZWD is 2.5–3× better in rms (0.163 vs 0.474 at
k = 359; per-station-mean rms 0.18 vs 0.41) and its σ 10× more honest (0.050 m), but the residue
is real: med|err| 0.10–0.14 m against σ 0.05 (|err|/σ 2.1–2.8 settled) and D(200) ≈ the level
— no longer frozen, still over-weighted 2–3× in σ.  The var floor LOOSENS the ZWD prior to the
same 0.054 m but leaves the error at 0.26–0.30 m median — it removes the pin without adding the
information (§6.7.2).  From the DD side the same residue is the los3h identity split (§6.7):
iono-direction part 0.64 cyc, non-iono 0.77 cyc.  Both halves are F3's target (§5.1).

---

## §4 Falsified — do not retry (each with the closing measurement)

| candidate | closed by |
|---|---|
| multipath (GM τ 300 s, 2–3 cm code / 2 mm phase) | ≤ 0.02 cyc settled, 0.3 at K=1 (MC) — 100× short (§2.5) |
| receiver PCO/PCV per-signal mismatch (incl. E5b→G02 fallback) | sim and producer call the same `Antenna::getAntennaCorrection` (`EphemerisProvider.cpp:344-372`) — cancels by construction |
| grader λ / DD sign / truth join | trace λ = truth λ to 1e-6 (0.190294/0.244210/0.254828/0.248349); sign matches `NetworkDdFrac.cpp:196-200`; lane/round0 Gaussian consistency 6–15 % (the grader's own frac check is a tautology — the consistency IS the validation) |
| stale x_lin | never written back, but graded/carried quantity is x_lin + x_r (exact); an x_lin shift is a solve no-op |
| iono seeds — 0, Klobuchar, x_lin GF seed | \|corr\| < 0.1 at birth and settled (§2.1) |
| rx/sat GF code-bias wander sampled at the legs' births | corr −0.02 (rx, SD 0.18 m), sat GF pinned at 0.005 m |
| `ionosphere_q` / any temporal iono constraint | INERT on the network path (no `getProcessNoiseMatrix` call site); increments phase-tracked (§2.6) |
| responses as the cause (IF-TD, MW, T4, anchors) | virgin chords 2.72 vs 2.87; paired \|Δe\| ≤ 0.06 except T4 (2 % of rows) (§2.3) |
| IF-TD POOL false positives | 0 / 2,754,791; every flag is the legacy reference gate |
| MW catching integer WL jumps | \|ΔMW\| p50 0.65 cyc, fractional part p50 0.35 vs local scatter 0.59 — noise |
| white / random-walk / 1/√K time structure | lag-1 0.946; D(1) 0.0013 cyc; age slope −0.033 (§2.2) |
| TEC- or elevation-proportional scale error | corr(r, I_true) 0.0095; rms flat across quintiles; el dependence +18 % |
| field-N regime (SPEC_synthetic §15.1 "field-N grades worse") | moot here: EWL 95.1 % over 24 h equals the legacy-regime 94.1 % |
| "code information does not reach the ambiguities" | it reaches them every epoch (the first inject halves σ); it is OVER-COUNTED, not blocked |
| a fixed / floored / q-inflated carry variance ("M2") | `pl_var_floor` 0, `pl_q_amb` 0, vce factor 1; the stash is the honest single-epoch conditional (1.0–7.5 m² at TOW 259440) and then collapses |
| departed-column marginalization / Card 1b joint whitening | departed terms 0.018 % of 1.43e9 |
| `NetworkRelevel.cpp` as an ambiguity re-level | it is the brdc_rts clock re-base, INERT on SP3 |

---

## §5 The fix ladder, with the invariant #1 / #7 edges

| rung | what | exact along | cost | edges |
|---|---|---|---|---|
| **F1** `network.phase_leveling_los_scale` (**BUILT + VALIDATED sim 3 h**, default OFF) | at inject, multiply each carried ambiguity row's `var_eff` by **B_eff = the number of ambiguity columns of the SAME (station, sat) injected with a carry THIS epoch** | the iono direction: Σ_b μ_b²/(B·var_b) = 1/v_I ⇒ p_k = p_{k−1} + p_code | as built (2026-09-04, pre-F3 line numbers): `ApplicationNetworkAssembly.cpp:2846-2998` (pl_finish_var / pl_emit_row lambdas, `PlPend` pre-pass, per-station B_eff map, scale ×B_eff after var + q·gap and BEFORE the floor and the VCE factor), loader `GnssConfigLoadNetwork.cpp:2976-2977` (+ ARMED info :8160, INERT warn :8181 when phase_leveling off / carry_invariant on), `NetworkKnobs.pl_los_scale` (`NetworkRunContext.hpp:94`), counters `n_pl_los_scale_rows` / `n_pl_los_scale_hist[4]` (`NetworkRunContext.hpp:2128-2134`), run-end line `NetworkTeardown.cpp:1885-1902`, armed-only fingerprint tag `pl:losscale` (`NetworkRunContext.cpp:637-642`), harvest mirror ×B_eff (:3091-3093), manifest key (`NetworkRunTraces.cpp:1484`).  Lane priors B× looser, but the phase rows re-fix the lanes every epoch at σ_φ — measured: WL round0 36.5 → 87.4 %, EWL → 100 % (§6.7) | **B_eff counts INJECTED rows only**: a band skipped at this epoch (`relevel_free_epoch` latch, pending-reseed, gap > `max_gap_s`, non-finite x_lin, no carry yet) must NOT count.  B_eff = 1 ⇒ factor 1 ⇒ byte-identical for single-band arcs and OFF (MEASURED: sha256-identical `products.txt` across the pre-F1 and F1 exes, §6.7).  T4 erase / 1(d) erase remove every band (arc solves free, B_eff = 0).  **Residue F1 leaves and every report must name:** the station-common ~n_sat× over-count (rx clock / ZWD / rx-DCB) — ⚠ NOT DD-invisible: DD-visible through birth-frame inheritance and DD(mf)·δZWD (§3.1); measured on los3h as 0.64 cyc iono-direction + 0.77 cyc non-iono residual |
| ~~**F2** per-LOS block~~ **RETIRED 2026-09-04** | stash the B×B conditional block; inject a whitened B-row block | the whole LOS: iono AND lanes | — | Adds nothing over F1 along the iono direction (F1 is already exact there — r2_4 simulation: F1 tracks the full-covariance recursion to 1 % in value, K_eff 48.8 vs 47.3 at k=59) and the lanes are re-fixed by phase each epoch (measured WL 87 % / EWL 100 % under F1).  The residue is STATION-common, which a per-LOS block cannot see.  Do not build for that purpose; the F1 `pl_pend` grouping is the seam if a per-LOS block is ever wanted for a lane reason |
| **F3** `network.phase_leveling_local_block` — station-wide jointly-whitened local block (**the next build; spec §5.1**) | joint local block over all carried ambiguity columns of a station (~75×75), Card 1b's joint whitening (`NetworkCoupling.cpp:144-231`, `carry_coupling_joint_whiten`) extended from departed-shared to local-local | all local↔local incl. the station-common residue — exact for every local-local direction given the border | O(75³) per station-epoch — trivial vs the Card-1 border solve; the cross terms are FREE at harvest (the `n_c = N_rr⁻¹e_c` solve already computed per column is its whole column) | same marginalize-never-condition rule per departed column; the block is the marginal over the epoch's free nuisances, i.e. the correct posterior; the remaining approximation is Card 1's border regression |

### 5.1 F3 specification — `network.phase_leveling_local_block` (bool, default false)

Carry each station's carried ambiguity set as ONE jointly-whitened local block instead of
independent rows.

- **Harvest** (`harvestCoupling`, `NetworkRunContext.cpp:2899-2934`): for the station's carried
  ambiguity columns c_1..c_n, stash the conditional covariance sub-block
  Σ = [(N_rr⁻¹)_{c_i c_j}] (conditional on the shared border) — the `n_c = N_rr⁻¹e_c` solve at
  `NetworkNormalEquations.cpp:1565-1571` already yields the whole column, so stash n_c(c_j) for
  the other carried columns j at zero extra solve cost — plus, per column, the existing
  shared-border regression row (`out.row = N_sr·n_c`).  Re-stash the FULL block every epoch
  from the posterior (no cross-epoch drift of Σ).
- **Inject** (next epoch, at the `do_amb_carry` site): take the sub-block over the columns that
  are injectable THIS epoch under the UNCHANGED admission predicates; departed / skipped /
  freed / pinned columns are **MARGINALIZED** (drop their rows and columns from Σ before
  whitening — never condition).  Factor Σ_inj = L·Lᵀ (LDLᵀ / Cholesky with a PD guard; on
  failure fall back to the F1 diagonal rows and COUNT it).  Emit n_inj rows
  L⁻¹·[(value_abs − x_lin) + Σ_j M_j ŝ_prev,j] with A_r = L⁻¹ (dense over the block's local
  columns) and A_s = L⁻¹ · (per-column regressor rows), unit weight.
- **Edges (invariants #1 / #7)**: a band-scoped free-epoch latch or pending reseed on one column
  marginalizes that column; T4 erase / gap prune remove it; a column's RETURN is a re-coupling
  edge (its fresh single-epoch value re-enters the block at its harvested variance) — count it.
- **Counters (invariant #4)**: blocks whitened, rows emitted, columns marginalized at inject,
  factorization fallbacks, block-dimension histogram; run-end line printed only when armed.
- **Precedence**: supersedes F1 when both are armed — apply the F1 ×B_eff scaling only in the
  fallback path.  Fingerprinted (touches the carried prior): fresh convergence only.
- **Cost**: n ≈ 75 per station → 75×75 block per station-epoch.
- **Validation arm** (staged): `config/test_suite/_nonint_losblock3h.json` →
  `runs/nonint_losblock3h` (3 h, same bed) against `nonint_base3h` / `nonint_los3h`; OFF twin
  `runs/nonint_base3h_f3exe` (sha256 of `products.txt` vs `nonint_base3h`).  Pre-registered
  expectation from §6.7: the los3h non-iono residual 0.77 cyc and iono part 0.64 cyc both fall
  toward the 3 h CRLB reference (0.134 cyc / 87 %); the ZWD |err|/σ 2.1–2.8 → ~1; the
  birth-spread slope of the DD level → flat; L1C first-inject ratio 0.588 → ≈ 0.707.

**Invariant #1 (stiff time-linked quantity needs a wired response).**  Unchanged by F1: the
existing responses (IF-TD refloat, MW, T4 erase, gap prune, 1(d) reref) all act BEFORE the
inject site, so a skipped band simply drops out of B_eff.  Under F2/F3 every response must
be re-pointed at the block (erase = drop the whole block; band refloat = marginalize the
band) before an ON arm.

**Invariant #4 counters (F1, run-end line "Network (leveling LOS-scale F1)"):** rows scaled;
B_eff histogram (1 / 2 / 3 / 4+); OFF ⇒ line not printed and byte-identical products.
**MEASURED on `runs/nonint_los3h` (`main_log.log:801`):** `950064 row(s) scaled over 317175
LOS group(s); B_eff histogram 1:487 2:0 3:316688 4+:0` — 99.85 % of LOS groups at B_eff = 3,
487 single-band groups (a band skipped or not yet carried), no 2s: the scaling reached the
rows.  Not built: the sibling-skipped counter and mean/max factor (the histogram carries the
same information on a triple bed).

**Invariant #7 (event at both edges):** a band's return after a skip is a RE-COUPLING edge —
under F1 its row re-enters with its own harvested var (fresh single-epoch value if it solved
free) and the sibling factors change from B−1 to B that epoch; log the B_eff transition per
LOS with `trace_events` so the edge is countable.

---

## §8 Reproduction

```powershell
# grade (quiet fixture: n_at reduces to N_int; last-frac = settled window)
python scripts/simnet_amb_integer.py runs/simnet60_quiet_wu93_24h --run runs/camp60wu_quiet24h_sticky --last-frac 0.25
python scripts/simnet_amb_integer.py runs/simnet60_quiet_wu93_24h --run runs/nonint_base3h   --last-frac 0.25   # each round-2 arm
# pre-registration arithmetic (no run data read)
python "<scratchpad>/record/predict_windows.py"
# round-1 recipes (session-scoped scratchpad; re-derive from the traces if gone)
python "<scratchpad>/theory/crlb_amb.py"                         # §2.5 CRLB + M6 recursion model
python "<scratchpad>/lens_memory/ddfrac_sigma_vs_age.py"         # §2.4 k table (streams the 729 MB trace)
python "<scratchpad>/lensT/pass2_timestruct.py"                  # §2.2 structure function / age slope
python "<scratchpad>/evt/age_grade.py"                           # §2.3 virgin / last-toucher attribution
python "<scratchpad>/iono/dd_identity.py"; python "<scratchpad>/iono/decompose2.py --tag _sys"   # §2.1
# run-end counters (invariant #4): main_log.log :4645-4675 — Card 1/1d stashes, free-epoch latches, T4 erases, gap prunes
# round 2 (2026-09-04): mechanism-signature flags on every arm (§6.7; ~2.5 min per 3 h arm)
python scripts/simnet_amb_integer.py runs/simnet60_quiet_wu93_24h --run runs/nonint_los3h --sigma-ratio --by-age --by-age-minlen 60 --iono-join --stride 4 --gal-lanes --check-lambda
python "<scratchpad>/arms/sigma_ratio.py" nonint_base3h nonint_los3h nonint_dual_v1_1h   # head-only k=1..14 ladders per band
python "<scratchpad>/record/zwd_freeze_arms.py" nonint_base3h nonint_los3h nonint_vf005_3h  # §3.1b station-common ZWD channel
python "<scratchpad>/verify/bfit.py"; python "<scratchpad>/adv/zwd_freeze.py"             # §2.7 B_fit / §3.1 (24 h bed)
sha256sum runs/nonint_base3h/products.txt runs/nonint_base3h_f1exe/products.txt          # invariant #6 (identical)
grep "leveling LOS-scale F1" runs/nonint_los3h/logs/main_log.log                          # invariant #4 (950064 rows, hist 3:316688)
```

The F1 arm: derive from `config/test_suite/_camp60wu_quiet24h_sticky.json` with
`processing.max_duration_s` 10800 and `network.phase_leveling_los_scale: true` — single
knob, fresh convergence (the knob touches the carried prior, so treat it as fingerprinted:
never flip it on a warm resume).  Release only; renamed exe when another session may build.
