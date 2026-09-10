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

## §6 PRE-REGISTERED predictions (written 2026-09-04 BEFORE any round-2 result was read)

**Arms** (Release, same successor exe, sequential; result `runs/<arm>/ambint.txt`):
`nonint_base3h` (control, 3 h prefix), `nonint_vf005_3h` (`network.phase_leveling_var_floor_m2`
0.05 m²), `nonint_vf001_3h` (0.01 m²), `nonint_ploff_1h` (`phase_leveling` OFF, 1 h),
`nonint_dual_1h` (GPS C1C/L1C/C2W/L2W + Gal C1C/L1C/C5Q/L5Q, `frequency_mode` dual, 1 h),
and the F1 arm to follow, `nonint_los3h` (`network.phase_leveling_los_scale` ON, 3 h).

### 6.1 The window expectation must come from the CRLB curve, not the 24 h mixture

`scratchpad/record/predict_windows.py` (uniform steady-state arc ages 1..A over the §2.5 DD
curve, log-log interpolated; 4e5 draws):

| window | ages | L1C med\|e\| | round0 | WL round0 | EWL round0 |
|---|---|---|---|---|---|
| 1 h | 1..120 | 0.373 | 61 % | 96 % | 99.7 % |
| **3 h** | 1..360 | **0.134** | **87 %** | 98.5 % | 99.9 % |
| 24 h (check vs agent1's 0.120 / 88 %) | 1..720 | 0.079 | 93 % | 99 % | 100 % |

(The 24 h row is optimistic vs agent1's 0.120 because it lacks his elevation-weighted
age mixture; the 3 h row is the reference for the 3 h arms, the 1 h row for the 1 h arms.
Full-window grades include the ignition hour — grade the LAST 25 %.)

### 6.2 The var-floor arithmetic (M6 with a cap)

Once the floor binds, every row injects at weight 1/v_f, so the prior on I is stationary,
Σμ²/v_f, and the recursion becomes an exponential smoother with forgetting
a = (Σμ²/v_f)/(Σμ²/v_f + p_code):

    K_eff = 1/Σw² = (1+a)/(1−a) = 1 + 2·Σμ²/(v_f·p_code)        τ = 1 + Σμ²/(v_f·p_code)

p_code(I) is bracketed: the DD-visible cold value from the trace's own k=1..6 DD σ
(7.2 → 5.7 cyc L1C ⇒ v_I = (σ/2·λ₁)² = 0.47 → 0.29 m² ⇒ **2.1–3.4 m⁻²**) and the CRLB at
30° (19 m⁻²); the column-marginal row weights the code lens dumped (0.13–0.99 m⁻², var
1.0–7.5 m²) include the DD-invisible station-common share and UNDER-state it.  Point
estimate p_code ≈ 3 m⁻².

| v_f | p_code m⁻² | K_eff | τ ep | 3 h mixture L1C med\|e\| / round0 | formal DD σ floor 2√(v_f/Σμ²)/λ₁ |
|---|---|---|---|---|---|
| 0.05 | 1 / **3** / 10 | 278 / **93** / 29 | 140 / **47** / 15 | 0.138 / **0.274** / 0.691 cyc; 87 / **76** / 38 % | **0.89 cyc** |
| 0.01 | 1 / **3** / 10 | 1387 / **463** / 140 | 694 / **232** / 70 | 0.134 / **0.134** / 0.201; 87 / **87** / 85 % | **0.40 cyc** |

The floor does NOT bind at k = 7 (harvested var at k = 6 is 0.3–7.5 m² ≫ 0.05), so the
first-inject ratio is unchanged; it binds from k ≈ 8–10.

### 6.3 The prediction table (L1C unless stated; "settled" = last 25 % of the window)

| arm | med\|e\| settled | round0 | WL / EWL round0 | k=6→7 σ ratio | dd_sigma_cyc at k=10 / settled | \|e\|/σ settled | \|e\| vs age slope |
|---|---|---|---|---|---|---|---|
| **base3h** | 2.6–2.9 (24 h trace h1–h2: 2.55 / 2.70); full window 2.9–3.2 (h0 = 3.74) | 8–11 % | 30–40 % / 93–97 % | **0.49–0.51** | 0.80 / 0.02–0.04 | > 50 (trace h1: 73) | −0.05…0 |
| **vf005_3h** | **0.15–0.7, point 0.27** | 40–87 %, point 76 % | > 95 % / > 99 % | **0.49–0.51 (unchanged)** | ≥ 1.0 / **flat ≈ 0.9** | **0.2–0.8 — the formal σ turns CONSERVATIVE** | −0.4…−0.5 to age ≈ 2τ ≈ 100, then flat |
| **vf001_3h** | **0.13–0.20, point 0.134** (age-limited: behaves as the correct estimator over 3 h) | 85–87 % | 98 % / 99.9 % | 0.49–0.51 | ≥ 1.0 / **flat ≈ 0.4** | 0.3–0.5 | ≈ −0.5 |
| **ploff_1h** | **1.2–2.2, point 1.49** (single-epoch at the CURRENT elevation — BETTER than base, whose frozen level carries the 10° birth code) | 15–25 % | 50–60 % / 95–98 % | **≈ 1.0 (no inject; 0.9–1.0 from border convergence)** | ≈ σ₁(el) ≈ 2–3 / same | **0.6–0.8 (calibrated)** | 0 |
| **dual_1h** | 2.6–3.6, point 3.2 (σ₁ larger by √(6.93/3.71) = 1.37 with two codes; K_eff 3 not 2) | 7–11 % | 25–40 % / n.a. | **0.577 (±0.03)** | per-epoch ratio k=7..10 ≈ 0.71–0.80 | ≫ 10 (collapse persists at 1/√2 per epoch) | −0.05…0 |
| **los3h (F1)** | **0.13–0.25, point 0.134** | 75–87 % | 95–99 % / ≥ 99.5 % | **0.707 (±0.03)** | ≈ 2–3 (CRLB at 4–5 accumulated epochs) / tracks 0.11–0.33 | **0.7–1.5** | −0.4…−0.5 |

Secondary (from the code lens's T4-pool HYPOTHESIS: `NetworkRaim.cpp:236` admits any row with
|A_r| > 0.5 as "a phase row of this ambiguity" and the coupling row carries exactly 1.0, so
T4 ARC flags are the over-confident prior's OWN standardized residual): base3h T4 ARC ≈ 290
(24 h trace h0–h2: 63 + 122 + 103); **vf005 / vf001 / los3h < 100**; ploff ≈ data-side only,
≪ 290.  `refsat_reset` unchanged in every arm (≈ 486 in h0 then 55–65/h — it is a detector
rule, not a level effect).

### 6.4 Falsifiers — what refutes the mechanism

| observation | verdict |
|---|---|
| base3h differs from the 24 h trace's first 3 h by > 0.3 cyc med\|e\| or the k=6→7 ratio leaves 0.49–0.51 | the successor build changed the mechanism; every other arm is uninterpretable until re-based |
| vf005 or vf001 leave settled med\|e\| > 2 cyc WITH dd_sigma_cyc floored at 0.9 / 0.4 | the level is not the injected prior weight — **M6 refuted** |
| vf arms' dd_sigma_cyc keeps collapsing to 0.02 | the floor knob is inert on this path (check the manifest's `phase_leveling_var_floor_m2` and `Assembly:2910`) — arm uninterpretable, not a refutation |
| vf005 better than vf001 (non-monotone) | the floor-smoother model is wrong (K_eff is monotone in v_f for any p_code) |
| ploff shows a FROZEN level (lag-1 > 0.9, D(1) < 0.1 cyc, \|e\|/σ > 10) | the freezer is NOT the leveling carry — look at the S-basis / anchor structure; M6 refuted |
| ploff med\|e\| WORSE than base (> 3 cyc settled) | the carry adds real information beyond 2 epochs — K_eff derivation wrong |
| dual first-inject ratio 0.50 or K_eff ≈ 2 (from its own σ₁) | the over-count is B-independent ⇒ station-common, not per-band ⇒ M6 as written refuted |
| dual ratio 0.577 AND K_eff ≈ 3 | the B-dependence is confirmed — the strongest single confirmation available |
| los3h med\|e\| > 1.5 cyc or first-inject ratio < 0.6 | the scaling is not reaching the rows (check the B_eff histogram counter — invariant #4) or the mechanism is elsewhere |
| los3h intermediate (0.5–1.5) with ratio ≈ 0.7 | the station-common residue matters more than DD-invisibility allows, or B_eff mis-counts skipped siblings — F2/F3 owed |
| within one LOS-pair corr(e_L1C, e_L2W) < 0.95 on any arm still at ~3 cyc | the error is not one iono-direction scalar — M6 refuted (24 h bed: off-plane residual 2.3 mm says > 0.99) |

### 6.5 What the arms cannot decide

A 1 h arm cannot settle the LEVEL channel (SPEC_synthetic §13.8.1 makes 1 h legal for the
INTEGER question only); `ploff_1h` and `dual_1h` are graded on the first-inject ratio,
K_eff and time structure, not on absolute med|e|.  None of the arms grades the
station-common residue (§3) — that needs `network_ionosphere.csv` station-mean δI vs
`truth_atmo`, consumer-side.

### 6.6 Provenance of the numbers above

CRLB table and mixtures: `scratchpad/theory/crlb_amb.py` (agent1) and
`scratchpad/record/predict_windows.py` (this card).  Floor arithmetic: §6.2, closed form.
Trace anchors: §2.4 (k table), §2.2 (hourly med|e|), §2.3 (T4 hourly counts).

### 6.7 Results — AMENDMENT (2026-09-04, round 2; §6.3 untouched)

Arms as run: Release, `config/test_suite/_nonint_*.json`, 60 sta, tow 259230 + 360 epochs
(3 h) or 121 (1 h).  `nonint_base3h` on the 2026-09-03 exe; `nonint_los3h` and
`nonint_base3h_f1exe` on the F1 exe (2026-09-04 20:08).  **The GF-datum dual arm
(`nonint_dual_1h`) could not run**: exit 1, `NetworkSbasis` null-space gate 43–48 near-zero
eigenvalues against a declared S-basis dimension of 2, every epoch withheld, ZERO solution
epochs — two bands + `code_bias_gf_datum` is structurally under-specified (the loader interlocks
it; with triple mode + trimmed signals the run withheld every epoch).  The B = 2 reading comes
from `nonint_dual_v1_1h` — the **V1 posture** (`estimate_sat_code_bias` false, .BIA applied): a
DIFFERENT datum, the same carry path; graded on the first-inject ratio only.

#### 6.7.1 The results table (same columns as §6.3; "settled" = last 25 %; L1C unless stated)

| arm | med\|e\| settled | round0 | WL / EWL round0 | k=6→7 σ ratio L1C (L2W / L5Q / L7Q) | dd_sigma_cyc at k=10 / settled | \|e\|/σ settled (p90) | \|e\| vs age slope (runs ≥ 60) | T4 attrib_arc / refsat_reset |
|---|---|---|---|---|---|---|---|---|
| **base3h** | **2.720** (full 2.928) | 10.2 % | 36.5 / 97.9 % | **0.494** (0.548 / 0.561 / 0.570) | 0.804 / 0.024 | 83 (275) | −0.24 | 288 / 1170 |
| **vf005_3h** | **3.457** (WORSE) | 9.0 % | 72.5 / 99.8 % | 0.494 (unchanged) | 1.001 / **0.859** (floor binds from k ≈ 9) | 4.0 (14) | −0.28 | 0 / 1170 |
| **vf001_3h** | **3.635** (WORSE) | 9.6 % | 52.5 / 96.2 % | 0.494 | 0.812 / **0.424** | 8.3 (30) | −0.32 | 0 / 1170 |
| **ploff_1h** | **4.445** (1 h; WORSE than base) | 6.8 % | 49.3 / 98.6 % | **0.970** (0.985 / 0.992 / 0.993) | 5.50 / 6.45 | **0.8** (2) | −0.29 | 0 / 1051 |
| **dual_v1_1h** | 5.205 (1 h, V1 datum; L2W 5.459, L5Q 5.708) | 6.5 % | 31.5 / n.a. | **0.565** (0.604 / 0.636 Gal-L5Q) | 1.43 / 0.022 | 134 (782) | −0.32 | 94 / 1051 |
| **los3h (F1)** | **1.276** (full 2.374; L2W 1.133, L5Q 1.293, L7Q 1.517) | **23.1 %** | **87.4 / 100.0 %** | **0.588** (0.650 / 0.660 / 0.665) | 1.767 / 0.717 | **1.9** (6) | −0.36 (L2W −0.39, L5Q −0.38) | **0** / 1170 |

DD identity on the settled window (`--iono-join --stride 4`; e_b vs μ_b·DD(δI)/λ_b, L1C):

| arm | med\|pred\| (iono direction) | resid med\|.\| (non-iono) | slope / r² | reading |
|---|---|---|---|---|
| base3h | 2.659 | 0.258 | 1.01 / 0.94 | pure iono-direction, the 24 h bed reproduced |
| vf005_3h | 1.477 | **2.073** | 2.22 / 0.89 | iono part halved, a GEOMETRIC error appeared |
| vf001_3h | 1.809 | **2.182** | 1.76 / 0.78 | same, non-monotone vs vf005 |
| ploff_1h | 2.530 | 2.299 | 1.99 / 0.88 | single-epoch in EVERY direction |
| dual_v1_1h | 3.330 | 2.258 | 1.51 / 0.83 | different datum; not graded on level |
| **los3h (F1)** | **0.636** | **0.768** | 1.99 / 0.82 | 76 % of the iono-direction error removed; remainder ≈ half iono, half geometric / station-common |

Also measured: MW refloats 143 and PB-gauge anchor handovers 102 identical across every 3 h arm
(0 / 33 and 93 / 22 on the two 1 h arms); the σ schedule at k ≥ 60 is a population median over
mixed ages (los3h 0.71–0.74 flat, base3h still collapsing 0.074 → 0.024), so read the k ≤ 20
ladder for the recursion and |e|/σ for calibration.

#### 6.7.2 Verdict per pre-registered row

- **base3h — CONFIRMED** (7 of 8 columns inside the band; the successor build did not change
  the mechanism: 24 h trace h1–h2 2.55/2.70 vs 2.72, ratio 0.494 = 0.494, so §6.4 row 1 did
  not fire).  Age slope −0.24 vs the predicted −0.05…0 is MIXED: on a 3 h window the age bins
  alias elevation gain — `ploff`, K_eff = 1 by construction, shows the same −0.29 — so the
  24 h minlen-200 slope −0.033 stays the frozen-level instrument.
- **vf005 — FALSIFIED as written** (predicted 0.15–0.7, point 0.27; measured 3.46, WORSE than
  base by 0.74 cyc).  Confirmed sub-rows: σ floor 0.86 (predicted 0.89), first-inject unchanged
  0.494, T4 < 100 (0).  Failed: |e|/σ 4.0 (predicted 0.2–0.8 — σ still 4× optimistic), WL
  72.5 % (predicted > 95 %).
- **vf001 — FALSIFIED** (predicted 0.13–0.20; measured 3.64); σ floor 0.42 (predicted 0.40);
  NON-MONOTONE vs vf005 (3.64 > 3.46 in total AND 1.81 > 1.48 in the iono part) — §6.4's
  "vf005 better than vf001" falsifier fired too.
- **Why the var-floor predictions were wrong — plainly.**  §6.2 projected every row onto the
  iono direction and treated the floor as a cap on the iono-direction prior weight only.  A
  floor applies to the WHOLE row: it also releases the row's geometric / station-common share,
  which the stiff diagonal carry had — wrongly weighted but effectively — PINNED through the
  densely-carried border (the direction §3(f) said accumulates).  The identity split shows it:
  the iono part fell 2.66 → 1.48 / 1.81 cyc (the floor DID act on the iono direction, at 5–13×
  the predicted magnitude) while a NON-iono residual of 2.07 / 2.18 cyc appeared (base 0.26),
  slope 1.7–2.2, band ratios ≈ equal in cycles — the geometric signature, not (1, 1.28, 1.34).
  The theory skeptic's own floor model (r2_4: 0.43 / 0.19 cyc, |e|/σ ~0.7) was wrong for the
  same reason — both derivations omitted the station-common / geo coupling the skeptic himself
  measured on the ZWD (§3.1), and the ZWD under vf005 shows it directly: prior loosened to
  0.054 m, error 0.26–0.30 m median (base 0.027).  §6.4's "M6 refuted" reading of the > 2 cyc
  floor falsifier does NOT follow: that falsifier was written for a one-direction model; the
  direction-resolved instrument shows the iono direction behaved as the mechanism says and the
  total got worse for a reason outside that model.  B_fit = 3.00 (§2.7) and the F1 arm stand
  on their own instruments.  **Verdict on the knob: FALSIFIED as a fix — worse total, releases
  the geo direction; stays the emergency lock-in guard only.**  Open: why vf005 < vf001 in the
  iono part (§7).
- **ploff — MIXED.**  Ratio ≈ 1.0 CONFIRMED (0.970–0.993); |e|/σ 0.8 CONFIRMED (calibrated);
  WL 49 % and EWL 98.6 % inside; T4 0.  med|e| 4.45 **FALSIFIED** (predicted 1.2–2.2, BETTER
  than base; the theory skeptic's 4.0–4.7 was right).  §6.4's "ploff WORSE than base ⇒ the carry
  adds real information beyond 2 epochs ⇒ K_eff derivation wrong" fired as written, and the
  resolution is again directional: base's K_eff ≈ 2 holds in the IONO direction (data skeptic
  2.1) while its GEO direction accumulates through the border (§3(f)); a no-carry LOS is
  single-epoch in EVERY direction — identity split resid 2.30 (geo) + pred 2.53 (iono).  The
  prediction's "single-epoch at the current elevation" reasoning forgot the second direction.
- **dual — CONFIRMED on the ratio, level NOT GRADABLE.**  L1C 0.565 (predicted 0.577 ± 0.03;
  skeptic's per-signal 0.55 ± 0.02), L2W 0.604 (0.61 ± 0.02), Galileo L5Q 0.636; every band
  shifted +0.056 … +0.075 from triple (predicted +0.077); second step 0.678/0.696/0.705 vs
  triple 0.616–0.628 (B=2 model 0.655, B=3 0.556 — both arms sit 0.03–0.06 above their model,
  the excess the theory skeptic left open).  The settled 5.2 cyc is 1 h on a different datum
  (predicted 2.6–3.6 for the GF posture) — not a level test.  §6.4's strongest row ("0.577 AND
  K_eff ≈ 3") is half-taken: the ratio, yes; K_eff not measured on this posture.
- **los3h (F1) — MIXED, in the pre-registered "intermediate" row.**  med|e| 1.276 (predicted
  0.13–0.25 — FALSIFIED as a point; inside §6.4's 0.5–1.5 band); round0 23.1 % (75–87 %); WL
  87.4 % (95–99 %, just below), EWL 100 % ✓; first-inject 0.588 L1C / 0.650–0.665 others
  (predicted 0.707 ± 0.03 — FALSIFIED for L1C, marginal for the rest; the per-band ORDER is the
  station-common share collapsing at the station-wide rate — the same reason base L1C read
  0.473 < 0.500 per chord; the skeptic pre-predicted L1C ~0.68 and got 0.588, i.e. the share is
  larger than his estimate); |e|/σ 1.9 (p90 6) vs 0.7–1.5 — nearly honest, the p90 tail is the
  frozen residue; age slope −0.36 … −0.40 ✓ (with the ploff caveat: the extra −0.12 over base is
  the accumulation signal; the k ≤ 20 σ ladder 3.35 → 0.95 is the cleaner one); T4 288 → 0 ✓;
  run-end counter §5.  §6.4's row "los3h intermediate (0.5–1.5) with ratio ≈ 0.6–0.7 ⇒ the
  station-common residue matters more than DD-invisibility allows — F2/F3 owed" is the one
  that fired.  Identity split: iono part 0.64 (from 2.66), non-iono 0.77 (from 0.26).  **Verdict
  on the knob: VALIDATED on the sim bed as the per-LOS fix; not the whole fix.**
- **Byte identity (invariant #6) — PASS, MEASURED.**  sha256(`runs/nonint_base3h/products.txt`)
  = sha256(`runs/nonint_base3h_f1exe/products.txt`) =
  `1c1b60c7792e5f91ef6e4e2f659883603a9fc0ae8112d3a26f86c3355029229d` (8,601,257 bytes) — the
  2026-09-03 exe vs the F1 exe with the knob OFF, i.e. cross-build identical; `ambint.txt`
  identical to the printed digit; manifests differ by the one added key.
- **Secondary predictions**: T4 base ≈ 290 → 288 ✓; vf / los < 100 → 0 ✓ (stronger than
  predicted); ploff ≪ 290 → 0 ✓; `refsat_reset` unchanged → 1170 in every 3 h arm and 1051 in
  both 1 h arms (a detector rule) ✓.
- **§6.1 window rule**: no arm reaches the 3 h CRLB reference (0.134 cyc / 87 %); los3h's 1.28
  is 10× above it — the station-common channel, not the window.

#### 6.7.3 Provenance

`scripts/simnet_amb_integer.py <fixture> --run runs/<arm> --sigma-ratio --by-age --by-age-minlen 60
--iono-join --stride 4 --gal-lanes --check-lambda` (default output byte-identical to the round-1
grader; λ PASS ≤ 3.7e-7 on every arm; Gaussian consistency 3–16 % per band on the carried arms,
8–10 % on los3h settled).  Outputs: `scratchpad/arms/flags_grade_r2.txt` (five arms),
`scratchpad/record/los3h_flags.txt`, `scratchpad/record/sigma_ratio_r3.txt` (head-only
ladders, all six arms), `scratchpad/record/zwd_freeze_arms.out`; event counts by
`grep -c <class> runs/<arm>/logs/network_events.csv`.

---

### 6.8 F3 results — AMENDMENT (2026-09-04, round 4; §6.3 and §6.7 untouched)

Arms: Release, `config/test_suite/_nonint_*.json`, 60 sta, tow 259230 + 360 epochs, quiet wu93 bed.
`nonint_losblock3h` (F3, handover ceiling 1e-4 m²), `nonint_losblock3h_hv` (F3 + ceiling **1e-2**),
`nonint_los3h_hv` (F1 + 1e-2), OFF twin `nonint_base3h_f3exe`; two independent graders; MEASURED
unless marked.

#### 6.8.1 Truth grade vs the pre-registered line (settled = last 25 %; grader flags as §6.7.3, `--stride 1`)

| quantity | pre-registered (§5.1 / r3_5) | base3h | los3h (F1) | **losblock3h (F3)** | **losblock3h_hv** | verdict |
|---|---|---|---|---|---|---|
| L1C med\|e\| / round0 | 0.55–0.62 cyc / 42–46 % | 2.720 / 10.2 % | 1.276 / 23.1 % | **0.217 / 79.3 %** (full window 0.357 / 59.7 %) | **0.187 / 83.1 %** | **BEATEN 2.7×** (the > 0.8 falsifier not tripped) |
| L2W / L5Q / L7Q med\|e\| (round0) | ~0.66 / 0.76 / 0.86 | 3.223 / 3.614 / 3.974 | 1.133 / 1.293 / 1.517 | **0.266 / 0.287 / 0.287** (72.9 / 70.9 / 70.9 %) | 0.203 / 0.243 / 0.274 (80.8 / 76.5 / 73.2 %) | BEATEN |
| WL(1,2) / EWL(2,5) round0 | ~93 % / 100 % | 36.5 / 97.9 | 87.4 / 100 | **98.8 / 100** (med 0.065 / 0.022 cyc) | 99.0 / 100 | BEATEN / CONFIRMED |
| first-inject σ(7)/σ(6) L1C (L2W / L5Q / L7Q) | ≈ 0.707 | 0.494 (0.548 / 0.561 / 0.570) | 0.588 (0.650 / 0.660 / 0.665) | **0.720** (0.722 / 0.728 / 0.722) | identical | CONFIRMED (+3 %) |
| \|e\|/σ settled (p90) | ~1.0–1.1 | 83 (275) | 1.9 (6) | **0.8** (2) | 0.7–0.8 | CONFIRMED, slightly conservative |
| formal σ_DD settled, legs ≥ 30 (≥ 100) | "unchanged ~0.55" | 0.026 | 0.616 | **0.239 (0.190)** | 0.199 (0.153) | **FALSIFIED** — 0.55 was not the floor (§6.8.2) |
| implied B, mid-run newborn chords (a5 fit) | 1.00 ± 0.01, same mature-leg floor | 3.00 | 1.000 ± 0.005, v_m 0.38 cyc² | **1.000 ± 0.002, v_m −0.07 (floor GONE)** | 1.000 ± 0.002, v_m −0.09 | B CONFIRMED; floor FALSIFIED |
| DD identity slope / r² (grader, L1C) | — | 1.01 / 0.94 | 1.99 / 0.82 | **0.983 / 0.95–0.99** (resid med 0.045) | 0.98–1.00 | exact |
| \|e\| vs age slope (runs ≥ 60) | — | −0.24 | −0.36 | **−0.60** (1/√K = −0.5; elevation rises along the arc) | −0.62 … −0.68 | — |
| L1C by min-leg elevation 10-15 / 15-20 / 20-30 / 30-45 / 45-90° | 1.0 → 0.23 (elevation-limited) | flat 2.5–2.7 | 1.08 / 0.66 / 0.50 / 0.33 / 0.23 | **0.20 / 0.22 / 0.19 / 0.13 / 0.09** | — | **FALSIFIED** (flat) |
| ZWD h2 med\|err\| / \|err\|/σ / D(10) | ≤ 0.02 m / ~1 / < 0.01 | 0.027 / 4.9 / 0.008 | 0.127 / 2.2 / 0.039 | **0.0056 m / 1.0 / 0.0064** | 0.0053 / 0.9 / — | CONFIRMED |
| ZWD-geometry DD part Z (a3), med [rms] | ≤ 0.10 cyc | 0.201 [1.02] | 0.694 [1.88] | **0.032 [0.079]** | 0.03 [0.067] | CONFIRMED |
| T4 attrib_arc / T3 cell flags | 0 / — | 288 / 4900 | 0 / 0 | **0 / 1** (k = 42, ignition, unacted) | 0 / 1 | CONFIRMED |
| data-skeptic (a) c_a at a = 1..5 / (b) med\|r(100)\|/med\|r(0)\| / (c) DD level vs spread | (b),(c) PASS; (a) proxy-incapable | 0.18/0.04/0.03/0.02/0.03 / 0.718 / rising | 0.31/0.11/0.10/0.09/0.07 / 0.153 / flat | **0.477/0.257/0.234/0.189/0.250 = the 1/(a+1) law / 0.066 / 0.070, 0.047, 0.052 m flat** | identical ± 0.01 | all three PASS |

`los3h_hv` (F1 + 1e-2) on the same instrument: 1.200 / 24.8 %, WL 87.5 %, EWL 100 % — the ceiling
lift buys F1 0.08 cyc; F1 stays in §6.4's "intermediate" row.

#### 6.8.2 Why the prediction UNDER-shot — plainly (residue verifier; DERIVED from the MEASURED parts)

The §5.1 / r3_5 line took F1's settled iono-direction part (0.569 cyc) as the "honest per-LOS
accumulation floor" no local-local fix could lower at 3 h, and predicted F3 would remove only
the 0.69-cyc ZWD-geometry term.  Both halves were wrong, each for one reason:

1. **The 0.57 was not honest — it carried the ZWD wander.**  Per station, the frame-removed per-LOS
   iono error regressed on mf(el) has an elevation-shaped station term of rms 0.0975 of 0.1258 m
   under F1 (**60 % of the variance**) whose slope tracks the station's ZWD error at corr **−0.83**
   (−0.44 m/m); under F3 0.012 of 0.037 m (10 %), corr +0.14.  At DD level F1's "iono part"
   correlates with Z at −0.73 — a Z-projected share of rms **0.878 of 1.197 cyc (54 % of its
   variance)**: the ZWD wander entered F1's DD error at ~1.45× (0.46 via the iono part + 0.99 via
   ν) and round 3 credited only the 0.99.
2. **The rest was the diagonal carry's cross-band information LOSS.**  The single-LOS recursion
   with the F1 prior (diag(posterior) × 3) reproduces the B = 1 ladder exactly (0.7073 / 0.8168)
   yet its actual σ saturates at DIAG3/FULL = 1.06 / 1.31 / 1.58 / 1.73 at k = 100 / 200 / 300 /
   360 — the mature-leg floor v_m = 0.38 cyc² a5 fitted on F1, gone on F3 (−0.07).
3. **"The CRLB gap is age" is FALSIFIED.**  The white population CRLB on the 24 h producer run's
   own DD population reads L1C **0.145 / 86.1 % at h2–3 and 0.137 / 86.4 % at h12–24** (min-leg
   age p50 117 vs 119) — the 3 h settled quarter already IS the steady-state age mix; the F3
   residue is mechanism, not K.

Residue table (settled L1C, cycles; a3 subset ep ≥ 270 / legs ≥ 30 unless noted):

| component | base3h | los3h (F1) | **losblock3h (F3)** | losblock3h_hv |
|---|---|---|---|---|
| TOTAL med\|e\| (a3) [grader all rows / round0] | 2.594 [2.720 / 10.2 %] | 1.159 [1.276 / 23.1 %] | **0.187 [0.217 / 79.3 %]** | 0.159 [0.187 / 83.1 %] |
| per-LOS iono part, med [rms] | 2.565 [3.96] | 0.569 [1.20] | **0.183 [0.342]** | 0.155 [0.284] |
| — of which Z-projected (ZWD leaked into per-LOS levels), rms | 0.038 | **0.878** | 0.007 | 0.025 |
| ZWD-geometry part DD(mf·δZWD), med [rms] | 0.201 [1.02] | 0.694 [1.88] | **0.032 [0.079]** | 0.03 [0.067] |
| remainder | 0.048 | 0.057 | 0.032 | 0.031 |
| identity slope (a3) | ~1.1 | 2.13 | 0.97 | 0.96 |
| population CRLB mixture on the same rows (white) | 0.124 / 93.9 % | same | same | same |
| med \|e\|/σ_crlb (bound 0.674) | 12.2 | 5.58 | **0.970 (1.44×)** | **0.839 (1.24×)** |
| formal σ_DD (legs ≥ 30) / formal ÷ crlb / \|e\| ÷ formal | 0.026 / 0.14 / 90 | 0.616 / 3.15 / 1.90 | 0.239 / 1.10 / 0.79 | 0.199 / 1.03 / 0.80 |
| ZWD h2 med\|err\| / rms / \|err\|/σ (m) | 0.027 / 0.401 / 4.9 | 0.127 / 0.202 / 2.2 | **0.0056 / 0.0091 / 1.0** | 0.0053 / 0.0083 / 0.9 |
| PB-gauge handovers continuous / cold; leveling carries erased | 90 / 3; 57 | 0 / 93; 1653 | 0 / 93; 1653 | **70 / 23; 408** |

Where the remaining 1.44× (hv 1.24×) lives — the population-CRLB instrument
(`r4resid/crlb_pop.py`: white 0.003 / 0.30 m per sin el, I free per epoch, ρ′ known, each leg's
information summed over its PRESENT epochs since birth at the producer's elevations, DD = 4
independent legs) is the standing grade from here on:

| row class (losblock3h / hv) | med \|e\|/σ_crlb | reading |
|---|---|---|
| all four legs born mid-run (n = 1627) | **0.694 / 0.640** (0.230 vs CRLB 0.222) | AT the bound (sim GM code multipath is worth +4–12 % in σ at K = 100–300) |
| 1–2 / 3–4 ignition-born legs | 0.925 / 1.075 (hv 0.832 / 0.882) | the whole excess |
| ignition-born, NO cold-handover hit, NO T1/T0 refloat on the leg (n = 2085 / 5936) | 0.843 / 0.904 | 1.25–1.34× with no logged event; T1/T0 history moves nothing (± 0.02) |
| leg hit by a cold PB-gauge handover since birth | 1.163–1.206 (unhit 0.842) | ~1.4× σ cost on hit legs; hv shrinks the hit population to 93 rows |
| 1+ re-birth leg | 0.78 / 0.66 | gap re-births NOT measurable above the bound (§7 item 9) |
| by min-leg age [1,30) / [30,60) / [60,120) / [120,240) / [240,361) | 0.81 / 0.79 / 0.86 / 0.99 / 1.20 (hv 0.80 / 0.70 / 0.77 / 0.91 / 0.90) | the ignition class ageing in |
| by min-leg elevation (5 bins) | 0.97 / 0.92 / 1.03 / 0.98 / 0.88 | elevation-flat |

#### 6.8.3 The handover ceiling — census, refusal attribution, wire continuity

`network.phase_datum_handover_max_var_m2` (`NetworkPhaseDatum.cpp:376-403`: usable = ledger hit
∧ finite ∧ var > 0 ∧ var ≤ max_var, else `++handover_cold` and `++handover_refused_var` when
var > max_var; the ledger var is the successor's border-exact `pureConditionalLocalVarianceDiag`,
`NetworkRunContext.cpp:2806-2846`).  The lineage value **1e-4 m²** (σ = 1 cm; config line 764,
inherited from the camp60wu sticky base; loader default 1.0, banner "keep this TIGHT") was sized
against the OVER-COUNTED σ; F1 and F3 both make the successor's σ honest (settled P σ 3–5 cm), so
the round-3 prediction "F3 will be refused the same way" is CONFIRMED.

| arm | ceiling | continuous / cold (refused) | per hour cont \| cold | settled cont / cold | carries erased | 1(d) marked |
|---|---|---|---|---|---|---|
| base3h | 1e-4 | 90 / 3 (3) | 33,30,27 \| 0,3,0 | 21 / 0 | 57 | 174 |
| los3h (F1) | 1e-4 | 0 / 93 (93) | 0 \| 33,33,27 | 0 / 21 | 1653 | 264 |
| los3h_hv (F1) | 1e-2 | 67 / 26 (26) | 27,23,17 \| 6,10,10 | 13 / 8 | 461 | 197 |
| losblock3h (F3) | 1e-4 | 0 / 93 (93) | 0 \| 33,33,27 | 0 / 21 | 1653 | 264 |
| **losblock3h_hv (F3)** | 1e-2 | **70 / 23 (23)** | 17,26,27 \| 16,7,0 | **21 / 0** | 408 | 194 |

Refusal attribution at 1e-2 (`r4_wire/handover_age.py`, successor arc age from `station_residuals`
presence): under **F3 every refusal is a YOUNG successor** — cold ages n = 23, med 1680, max
**3120 s**, all in h0/h1; continuous ≥ 960 s (med 4170) — an AGE gate under honest σ, so the settled
window is fully continuous.  Under **F1 the refusals are per-BAND** — cold L1C 3 / L2W 7 / L5Q 12 /
L7Q 4 on arcs up to 10560 s old, 9 MIXED families (L1C continuous, other bands cold), four settled
(268530 G29, 269040 E33, 269280 E25, 269970 G31): the B_eff-scaled non-L1C bands sit above σ = 10 cm
while their L1C passes (DERIVED — no per-handover var is traced).  Neither reaches the ≥ 90/93 bar.

Wire continuity (`simnet_seam_tools.py steps`, phase-side P − C per (sat, signal), settled last 25 %):

| arm | n > 0.05 m | n > 0.5 cyc | step rms (m) | max (m) | epochs with a PB step > 1 m | matched to cold-handover tows |
|---|---|---|---|---|---|---|
| base3h | 47 | 22 | 0.0104 | 0.285 | 0 | — |
| los3h | 233 | 91 | 0.3095 | 14.25 | 7 | 7/7 |
| los3h_hv | 87 | 34 | 0.2077 | 14.58 | 4 | 4/4 (268530 6.39, 269040 14.58, 269280 4.45, 269970 10.96 m) |
| losblock3h | 200 | 85 | 0.3117 | 15.15 | 7 | 7/7 (268050 G32 10.38, 268530 G29 2.65, 268980 E10 7.06, 269040 E33 14.47, 269280 E25 7.59, 269640 G22 15.15, 269970 G31 9.07 m) |
| **losblock3h_hv** | **78** | **21** | **0.0111** | 0.291 | **0** | — (whole run after 20 min: 8 epochs > 1 m, all matched, all h0/h1) |

Wire ACCURACY is neutral to the ceiling on both mechanisms (clock / CB / rx-DCB identical to 3
digits; ZWD 0.0088 → 0.0081 under F3, 0.191 → 0.204 under F1); the only wire-side gain is
continuity (Psig med 0.049 → 0.031 m; settled integer-class drifts E 30 → 6).  **F1 + 1e-2 is NOT
a valid combination** (band-mixed handovers; consumer +0.43 m up bias, §6.8.4).  **F3 + 1e-2 is
the arm to carry forward** — a working value, not a promotion (§7 item 15).

#### 6.8.4 Wire and consumer (settled last 25 %; `simnet_grade.py --settle-frac 0.75`, `simnet_sat_clock_census.py`, `network_epoch_status.csv`)

| channel | base3h | los3h (F1) | los3h_hv | **losblock3h (F3)** | losblock3h_hv |
|---|---|---|---|---|---|
| sat clock residual rms / p50 / contrast rms (m) | 1.704 / 1.112 / 2.194 | **1.437 / 0.832 / 1.751** | 1.437 / 0.833 / 1.756 | 1.583 / 1.011 / 2.039 | 1.582 / 1.000 / 2.040 |
| sat clock vf / URA med (m) | 135958 / 0.0045 | 7423 / 0.0172 | 7472 / 0.0172 | 106552 / **0.0048** (base-like) | 112029 / 0.0047 |
| per-sat settled tails G12 / G16 / G09 / G18 (m) | 7.08 / 4.41 / 3.00 / 1.86 | 6.28 / 3.49 / 2.10 / 1.35 | 6.24 / 3.53 / 2.12 / 1.33 | 6.31 / 4.23 / 2.78 / 1.88 | 6.28 / 4.25 / 2.79 / 1.89 |
| sat code bias rms / p50 (m) | 0.814 / 0.046 | 0.504 / 0.011 | 0.504 / 0.012 | **0.507 / 0.011** | 0.507 / 0.012 |
| ZWD rms / p50 / max / \|err\|/σ (m); stations > 2 cm | 0.416 / 0.026 / 3.39 / 4.72; 12 > 0.10 | 0.191 / 0.122 / 0.84 / 2.59; 40 > 0.10 | 0.204 / 0.121 / 0.86 / 2.62 | **0.0088 / 0.0055 / 0.047 / 0.94; 1** (S26 0.021) | 0.0081 / 0.0051 / 0.035 / 0.91; 0 |
| rx-DCB contrast rms / formal σ (m) | 1.587 / 0.066 | 1.228 / 0.089 | 1.236 / 0.089 | **1.226 / 0.067** | 1.226 / 0.067 |
| rx clock between-station contrast rms (m) | 1.941 | 1.675 | 1.761 | 1.684 | 1.738 |
| σ̂0 post-fit / code / phase (epoch_status medians) | 1.198 / 1.699 / 0.939 | 0.053 / 1.063 / 0.0078 | 0.053 / 1.069 / 0.0078 | 0.920 / **1.104** / 0.830 | 0.932 / 1.105 / 0.848 |
| station_residuals rms(s) / \|s\| > 6 | 1.782 / 1014 | 0.275 / 0 | 0.274 / 0 | 1.179 / 0 | 1.182 / 0 |
| DD-fractional trace (tail 2700 s): med\|f\| / P(< 0.1) / circR | 0.253 / 0.195 / 0.016 | 0.248 / 0.202 / 0.006 | 0.248 / 0.202 / 0.005 | **0.203 / 0.265 / 0.152** | **0.185 / 0.292 / 0.214** |
| consumer SZZ000SIM settled 3D rms / p50 / max (m) | 0.414 / 0.411 / 0.477 | 0.296 / 0.239 / 0.548 | 0.535 / 0.544 / 0.650 | 0.295 / 0.280 / 0.392 | **0.245 / 0.233 / 0.359** |
| consumer up mean / E rms / N rms (m) | +0.191 / 0.091 / 0.350 | +0.007 / 0.225 / 0.165 | **+0.428** / 0.301 / 0.100 | +0.023 / 0.283 / 0.070 | +0.039 / 0.237 / 0.028 |
| consumer 30-min bins, 3D rms (m) | 2.42 / 3.06 / 1.77 / 0.76 / 0.44 / 0.41 | 2.32 / 0.77 / 0.66 / 0.28 / 0.15 / 0.34 | 2.09 / 0.55 / 0.41 / 0.49 / 0.45 / 0.56 | 2.13 / 0.72 / 0.47 / 0.23 / 0.24 / 0.31 | 2.13 / 0.77 / 0.79 / 0.24 / 0.21 / 0.27 |
| consumer walk (last-10 median − median at [2.33, 2.42) h) | −0.006 | +0.333 | +0.063 | +0.128 | +0.172 |

Readings.  (i) F3 equals or beats F1 on every station-common channel (ZWD 47× base, 22× F1; CB and
rx-DCB equal) and is the first arm to move the DD-fractional trace off uniform (circR 0.016 → 0.15 /
0.21); the P-record between-sat fractional vs truth stays uniform (0.25–0.31 cyc rms) and AR 0/360
on every arm.  (ii) The ONE channel worse than F1 is the satellite clock (rms 1.583 vs 1.437; G16 /
G09 / G18 tails at base level) with a base-like URA 0.005 m — better than base, a regression on F1
(§7 item 18).  (iii) F3's whitened rows do NOT enter the post-fit pool (n_pf_rows identical on all
five arms): the phase σ̂0 0.83 is the data rows fitted under an exact stiff prior, F1's 0.008 the
free-ambiguity zero; in the RAIM-attrib pool |effect| p50 ARC 0.454 → 1.463 (max 4.07 < 6), pool σ̂0
0.233 → 0.991, 0 acted (§7 item 4).  (iv) The final-40-min consumer WALK is NOT the cold handovers:
an EAST drift (blk_hv −0.15 → −0.32 m from ~267930, G12's re-emission epoch) on every F arm, wire
flat (clk contrast 2.110 → 2.089, cb_rms 0.559 → 0.518, n_sats 16) — blk_hv has ZERO settled cold
handovers and still walks; round 3's cold-handover half is FALSIFIED, its G12 half untested (§7 item
19).  (v) The los3h_hv +0.43 m up bias (264510 → 265710, that arm alone) is specific to F1 + lifted
ceiling — HYPOTHESIS.  (vi) Every pre-existing event family is identical across the five arms
(refsat_reset 1170, mw_wl_slip 143, anchor_handover 102, cta_* 64, CLK_STEP 11, topology 28 / 28);
only the PB-gauge mode and its 1(d) / 1f / 1g counters move.  Runtime +7.4 % ms/epoch (F3 and F1 +
ceiling alike; uncontrolled load).  prires / block-Baarda / congruence not armed in any `_nonint`
config — UNAVAILABLE, not clean.

#### 6.8.5 Invariants #6 / #4 / #7 on the F3 build

- **#6 byte identity — PASS, MEASURED.**  sha256(`products.txt`) `nonint_base3h` = `nonint_base3h_f1exe`
  = `nonint_base3h_f3exe` = `1c1b60c7792e5f91ef6e4e2f659883603a9fc0ae8112d3a26f86c3355029229d`
  (8,601,257 B) — three exes, both knobs OFF.  ON arms differ (los3h `ccfa9eba…`, los3h_hv
  `bd16aca2…`, losblock3h `cdbeaee0…`, losblock3h_hv `b7cea2ad…`).
- **#4 run-end counters (armed only; `main_log.log:803` / `:796`).**  losblock3h: `Network (leveling
  local-block F3): 21180 block(s) whitened, 950551 row(s) emitted, 8534 column(s) marginalized at
  inject, 0 pair(s) zeroed (harvest-stamp mismatch), 0 column(s) refused, 0 factorization
  fallback(s); block dim histogram <10:0 10-24:221 25-49:15834 50-74:5125 75+:0 (max 66);
  within-epoch refloat re-whiten 0 ok / 0 failed`; hv 951726 / 8859 / dims 218 : 15773 : 5189.
  21180 = 60 sta × 353 epochs (every station every epoch from k = 7); the F1 fallback never fired;
  the refloat re-whiten path was never exercised (T2 354 opportunities, 0 flags) — uncertified.
- **#7 edges.**  No new event population; the PB-gauge flip is inherited (§6.8.3).  Gap: the 8,534
  marginalized columns are not traced by reason (§7 item 16).

#### 6.8.6 What remains, and the 24 h pre-registration (HYPOTHESIS, written before any 24 h F3 arm exists)

Remaining F3 residue on this window, in order: (a) the cold PB-gauge handover erase edge (~1.4× σ on
hit legs; 408 carries still erased at 1e-2 — §7 item 15); (b) an ignition residue on legs born at
k = 0 with no logged event (1.25–1.34×; §7 item 8); (c) the rx-CB direction's 2× over-count (§7 item
17); (d) L7Q's satellite-arc-mean frame term (§7 item 3); (e) the satellite-clock gap to F1 (§7 item 18).

**Pre-registered 24 h F3 + hv reading** (same bed; grade the last 25 %, where 99.5 % of rows are
mid-run-born): settled L1C med|e| **0.14–0.15 cyc / round0 84–86 %** (L2W ~0.19–0.20 / 79–80 %,
L5Q ~0.19–0.20 / 78–80 %, L7Q ~0.18–0.19 / 79–81 %), WL(1,2) ≥ 99 %, EWL 100 %, ZWD |err|/σ ~1;
with the 1e-4 ceiling (every handover cold, ~550 erased carries / h) **0.16–0.17 / 82–83 %**.
Basis: the h12–24 CRLB mixture 0.136 / 86.4 % × the measured mid-run-born excess 1.03 × multipath
1.04–1.10 → f = 1.03–1.10; the cold lineage adds 1.1–1.2× on hit legs → f ≈ 1.15–1.25 (factor
table f 1.00 / 1.03 / 1.10 / 1.24 / 1.44 → 0.136 / 0.141 / 0.150 / 0.169 / 0.197 cyc, 86.4 / 85.8 /
84.5 / 82.0 / 78.5 %).  **FALSIFIERS:** settled L1C > 0.20 cyc or round0 < 80 % on the hv posture
(an unmodelled floor); |e| rising with age beyond 360 (an erase / handover mechanism still
active); ZWD |err|/σ > 1.5.

#### 6.8.7 Provenance

Wire + consumer: `<scratchpad>/r4_wire/` (grade_*_s75.{txt,json}, series_*.csv, steps_*, quality_*,
diff_*, drift_*, consumer_{30min,5min}, consumer_at_handovers, handover_age, events_census,
sigma0_consumer, ddfrac_*, census.txt).  Residue verifier: `<scratchpad>/r4resid/` (grader_*.txt;
build_arm.py + arms/*/{dd_e,los,frame}.npz; a1..a5_nonint_losblock3h{,_hv}.txt; **crlb_pop.py — the
population-CRLB instrument**; crlb_24h, crlb_f1model, crlb_ref, leak_test, handover_hit, ev_raim).
Recipe (extends §8): F3 arms = the F1 arm + `network.phase_leveling_local_block: true` (+
`network.phase_datum_handover_max_var_m2: 0.01` for `_hv`); `python "<scratchpad>/r4resid/crlb_pop.py"`;
`grep "leveling local-block F3" runs/nonint_losblock3h/logs/main_log.log`;
`sha256sum runs/nonint_base3h{,_f1exe,_f3exe}/products.txt`.

---


## §6.9 The 24 h F3 arm (2026-09-05, `runs/nonint_losblock24h_hv`: F3 + handover ceiling 1e-2, 2879/2879 epochs) — the pre-registered settled number is NOT met, and the reason is an EDGE, not the mechanism

Pre-registered (§6.8): settled L1C med|e| 0.14–0.15 cyc / round0 84–86 %.  Measured (`scripts/simnet_amb_integer.py`, 7,851,033 rows, 0 dropped):

| window | L1C med\|e\| | round0 | WL(1,2) | EWL(2,5) | Gal WL(1,5) / EWL(5,7) |
|---|---|---|---|---|---|
| full 24 h | 0.283 | 64.2 % | 97.7 % | 100.0 % | 97.0 % / 100.0 % |
| **last 25 % (h18–24)** | **0.425** | **53.2 %** | 97.9 % | 100.0 % | 97.2 % / 100.0 % |
| quiet hours h2–h5, h12–h16 (hourly, stride 4) | 0.18–0.23 | 74–83 % | 98 % | 100 % | — |
| mature chords (all 4 legs ≥ 30 ep), h13–h16 | **0.160–0.168** | **86–89 %** | 100 % | — | — |

Mechanism signatures hold all day: first-inject σ ratio 0.720/0.722/0.728/0.722 (all bands; correct recursion 0.707), settled |e|/σ 1.2–1.3, |e| vs chord age slope −0.46 (runs ≥ 200), F3 run-end `172320 block(s) whitened, 7,793,292 row(s) emitted, 69,410 column(s) marginalized, 0 fallbacks`, T4 attribution 0 until the run-end day-edge burst (49 at h23; base 63/122/103/48/… + 68), refsat_reset 2482 (identical to base), handovers 948 continuous / 87 cold (base 1030 / 5).  The LANES are solved on every hour of the day (WL 96.6–99.1 %, EWL 99.5–100 %).

**What breaks the settled quarter — two ~4 h DEGRADATION EPISODES, both trailing an eclipse re-admission cluster by 1–2 h** (hourly L1C med|e|: h0 1.26, h1 0.26, h2–5 0.19–0.27, **h6–8 0.42/0.49/0.38**, h9–16 0.32→0.18, h17 0.27, **h18–21 0.46/0.58/0.56/0.42**, h22–23 0.38/0.31).  Conditioned grades (`scratchpad/arms/{mature,coldho,readmit,station_episode}_grade.py` on the run dir):

| split (L1C, stride 4) | quiet h15 | episode h19 | verdict |
|---|---|---|---|
| all four legs ≥ 30 epochs old (tracer re-keying to young low-PRN refs removed) | 0.160 / 89 % | **0.515 / 50 %** | young legs are NOT the driver: mature chords degrade too |
| no `handover_cold` on sat_i/ref within 120 ep (97–99 % of rows) | 0.182 / 83 % | 0.585 / 47 % | cold handovers (2–3 % of rows, 87/day) are NOT the driver |
| no satellite RE-ADMITTED from eclipse exclusion within 240 ep on sat_i/ref ("clean") | 0.175 / 86 % | **0.680 / 45 %** (h19 clean n=5122; touched 0.561 / 48 %, 82 % of rows) | the degradation is NETWORK-WIDE — chords that do not touch a returned satellite degrade as much as those that do |

28 re-admission events over 54 satellites (gap ≥ 10 epochs of network-wide absence); the two clusters are h3.3–6.9 (E02, G01, G26, G16, E30, G25 …) and h16.6–18.9 (E25, G01, G26, E02, G16).  The episodes start ~1–2 h into each cluster and decay over ~4 h.  The wire's clock spread is unchanged through them (epoch-status `clock_std_m` 2.0–2.1 m on both arms every hour; `worst_dev_m` 7.1–7.5 vs 6.9–7.0; code σ̂0 1.1 vs 1.7), so this is not a clock re-realization the DD sees.

**Consumer (seam_tools, hourly 3D rms, F3+hv vs the 24 h control):** h0–2 1.60/0.58/0.24 vs 2.76/1.36/0.42 (F3 3–5× faster ignition); **h3–h7 1.81/1.10/1.19/1.40/1.11 vs 1.23/0.98/0.48/0.12/0.12** — the base recovers from the h3–4 pivot-transit cluster in 1 h, F3 takes until h9 (0.31) / h10 (0.16); h12–23 0.06–0.19 vs 0.05–0.15 (comparable).  The second episode (h18–21) costs the consumer nothing (0.06–0.10) — the DD instrument sees a network-wide level error the single-station consumer partly absorbs.

**Reading (HYPOTHESIS, discriminating arms queued):** under the diagonal over-counting carry every level was frozen, so a satellite returning from exclusion with a cold re-realization (the E2 gap of SPEC_synthetic §18.4 — the re-coupling edge marks but never re-validates) moved nothing the DD could see; under an honest carry the returned satellite's shared states enter every station's coupling regressors (`M_j·ŝ_prev`) with a STALE `ŝ_prev`, and the F3 whitening (rows = L⁻¹·[I|M|y], dense over the block) mixes that one bad shared term into every ambiguity of the station — which is exactly the network-wide shape measured.  Under F1 (diagonal rows) the same stale term would hurt only the columns coupled to it.  Discriminators: (a) `runs/nonint_losblock24h` (F3 at the 1e-4 ceiling, every handover cold) — running; (b) `runs/nonint_los24h_hv` (F1 24 h, same ceiling) — queued behind it: if F1 shows no network-wide episodes, the fix is the invariant-#7 re-coupling edge on the BLOCK: at a satellite's re-admission (col_added after a gap ≥ N epochs / the CTA "first sighting"), marginalize its shared terms out of every block's regressors (or feed the re-admission into `carry_coupling_reinit_sources`, Card 1g) for K epochs and count it.  Until then: **F3 stays default-OFF; on a quiet window it is at the CRLB (mature chords 0.16 cyc / 89 %), and the 24 h settled-quarter prediction is FALSIFIED as written because the pre-registration assumed no edge response was missing.**

**Arm 2 — F3 at the lineage's 1e-4 ceiling (`runs/nonint_losblock24h`, every PB-gauge handover cold: 20 continuous / 1015 cold):** full 24 h L1C 0.322 / 62.2 %, last quarter 0.449 / 52.5 %; first-inject 0.720/0.722/0.728/0.722, |e|/σ 1.2, T4 37 (day-edge), 0 fallbacks; the re-admission-conditioned hourly grade reproduces arm 1 to the digit (clean chords h6 0.571 / h19 0.658 vs 0.538 / 0.680).  So the ceiling is NOT the lever for the DD episodes.  It IS the lever for the consumer: h5–h7 1.86/2.57/1.64 m (arm 1 1.19/1.40/1.11; base 0.48/0.12/0.12) and h18–21 0.18–0.24 m (arm 1 0.06–0.10) — every cold handover erases the family's carries and re-pins at 0, and the consumer pays for each.  The two findings separate cleanly: the handover ceiling governs the WIRE's continuity (fix: re-express it on the honest-σ scale, ledger row); the ~4 h network-wide DD episodes after each re-admission cluster are a different edge (F1 control pending).

**F1 24 h control (`runs/nonint_los24h_hv`, F1 + ceiling 1e-2, 2879/2879, 744 continuous / 291 cold handovers, T4 0):** full 24 h L1C 0.853 / 33.1 %, last quarter 0.739 / 36.3 %, WL 90–92 %, EWL 100 %; hourly L1C 0.56–1.0 cyc all day (F1's own station-common ZWD-geometry residue, §6.7), first-inject 0.588/0.650/0.660/0.665, |e|/σ 0.9–1.1.  Its re-admission-episode response is a +0.15–0.30 cyc bump on a 0.7 cyc floor (clean chords h13 0.67 → h19 0.90, h8 1.02) against F3's +0.35–0.45 cyc on a 0.19 cyc floor (h15 0.18 → h19 0.68).  READING: the post-re-admission edge response exists for ANY honest carry (it is the E2 re-coupling gap the STABILITY campaign named, invisible under the frozen over-counted carry), and the F3 block roughly doubles its absolute size, consistent with the dense L⁻¹ mixing of a stale shared-border term into every ambiguity of the station — but F1 cannot separate "smaller edge" from "hidden under its own floor".  Consequences: (1) F3 delivers the CRLB on quiet windows (mature chords 0.16 cyc / 89 %, lanes 100 %) and is the vehicle to keep; (2) the next card is the block's invariant-#7 RE-COUPLING EDGE at satellite re-admission (col_added after a gap / CTA first sighting): marginalize the returned satellite's shared terms out of every block's regressors for K epochs (or feed the re-admission into `carry_coupling_reinit_sources`, Card 1g), counted, pre-registered on this bed as "no clean-chord degradation after the h16.6–18.9 cluster"; (3) the handover ceiling is a separate wire-continuity card (arm 2).  Until (2) lands F3 stays default-OFF for settled work, and the 24 h number to quote is the full-window 0.283 cyc / 64 % (quiet hours 0.18–0.23 / 74–83 %), not the settled quarter.

Pre-registration audit: §6.8's "24 h F3+hv 0.14–0.15 / 84–86 %" was derived from the 3 h settled window, which contains no eclipse re-admission cluster; the 24 h bed has two.  The quiet-hour mature-chord reading (0.160 / 89 %) is the number the prediction actually described.

## §6.10 The RE-ADMISSION EDGE card — BUILT 2026-09-05 (E2 seed-first re-admission + the block re-coupling window + the honest-σ-scale handover ceiling); PRE-REGISTERED, arms running

**What §6.9 named as the next card is built, default-OFF, byte-identical OFF, counted at both edges.**
Three knobs, one mechanism family (the satellite re-admission edge — the invariant-#7 edge that
§18.1 (T3 clock tails), §6.9 (the F3 24 h post-eclipse DD episodes) and §18.12 (the G20/E31 seam
residual) each measured from a different instrument):

| knob | what | where |
|---|---|---|
| `network.readmit_seed_horizon_s` (s, 0 = OFF) | E2 seed-first re-admission: a departing satellite's D2 **clock + code-bias** carry is PARKED (`CarryPrior::t_park_s`, walked by every frame re-base that walks `prev_carry`, dropped at a clock-datum change of its constellation, never serialized) and on re-registration inside the horizon SEEDS the fresh column at `var + q·gap`; the Q3 anchor folds ANCHORED over its true per-satellite gap (bounded by the horizon instead of `clock_anchor_max_gap_s`) and its NIS guard is the mark-if-moved (a trip within 300 s of the seed is counted as the data rejecting it).  The phase-bias family is deliberately NOT seeded — its frame is re-realized at the incumbent-less cold re-pin (the leveling carries do not survive a 55 min absence at the 120 s leveling gap) and a stiff seed would fight the pin. | park: `networkStashCarry` tail; seed: `networkInjectD2CarryPrior` (both D2 branches); fold bound: `clockAnchorFoldVariance(…, seed_horizon_s)`; attribution: `NetworkClockAnchor.cpp decouple()`; seed-vs-solve check at the stash (`READMIT seed_held/seed_moved`) |
| `network.readmit_coupling_marg_ep` (epochs, 0 = OFF) | the block/row RE-COUPLING edge: for K epochs after a satellite's clock column returns from a ≥ 3-loop-epoch absence, every Card-1 coupling term on its shared columns takes the DEPARTED route (σ² += M²·P_prev, coefficient dropped) in every station's rows and F3 block.  Both edges evented (`READMIT readmit` / `marg_expired`); the diverted-term count is the run-end denominator. | detector + window: `networkStashCarry` tail; consumer: `CouplingRowBuilder::readmitDiverted` (scalar, F3 block and the Card-1b joint path) |
| `network.phase_datum_handover_max_var_ratio` (0 = OFF) | item 15: a ledger value is usable only if `var ≤ ratio × var_first` — the arc's FIRST-STASH conditional variance (`LocalCarry::var_first`: set at birth, kept across re-stashes, restarted by an erase-and-rebirth, kept by a re-level); unknown ⇒ refused (the 378-σ guard stays armed); the absolute ceiling stays a rail; refusals counted on `handover_refused_ratio`; every PBGAUGE handover row carries `var`/`var_first` when armed (the item-15 prerequisite). | `networkElectPhaseAnchors` (`ratio_ok`); `var_first` at the leveling stash + the handover-ledger harvest; optional trailing L-row field in the snapshot (armed only) |

Unit contract: `NetworkCouplingTest.ReadmitWindowDivertsThePresentColumnAndExpires`,
`ClockAnchorFold.SeededReturnFoldsAnchoredOverTheTrueGap`,
`PhaseDatumHandover.RatioCeilingRefusesUnconvergedAndUnknownArcs`,
`PhaseDatumHandoverLoader.RatioCeilingDefaultsOffParsesAndRejectsNegative`,
`ReadmitContinuityConfig.*`, `ResumeFrameRestoreTest.DenseCarryCovAndVarFirstRoundTripUnderV3`
(the §18.12 (2a) half, see SPEC_synthetic §18.13).  Ledger rows added as UNVALIDATED; posture rows
SCOPED; neither knob fingerprinted (documented reasons in the loader/ledger).

**Arms (Release, `gnss_processor_readmit.exe` sha256 808a0f91addee887…, sequential, own CWD):**

| arm | config | single change vs its base |
|---|---|---|
| `readmit_off3h` | `_readmit_off3h.json` | the F3 3 h base (`_nonint_losblock3h_hv`) on the NEW exe, every new knob OFF |
| `ratio3h` | `_ratio3h.json` | + `phase_datum_handover_max_var_ratio 0.01`, absolute ceiling widened to a 1 m² rail |
| `readmit24h_e2` | `_readmit24h_e2.json` | the 24 h F3 arm (`_nonint_losblock24h_hv`) + `readmit_seed_horizon_s 3900` + `readmit_coupling_marg_ep 20` |
| `cplreg_q12warm_d` | `_cplreg_q12warm_d.json` | §18.12 arm C (frame + coupling restore, same v3 `.ep1440`) + the two readmit knobs |

**PRE-REGISTERED (written 2026-09-05 19:15 PDT, before any arm was read):**

1. `readmit_off3h`: `products.txt` sha256 **identical** to `runs/nonint_losblock3h_hv/products.txt`
   (invariant #6).  Falsifier: any byte differs — then nothing below is interpretable until the
   OFF path is repaired.
2. `readmit24h_e2` (the §7 FIRST criterion, verbatim): **no clean-chord degradation after the
   h16.6–18.9 re-admission cluster** — `readmit_grade.py` clean-chord L1C med|e| in h18–h21 ≤ 0.25
   cyc (base F3: 0.45–0.58); **settled quarter L1C ≤ 0.2 cyc / ≥ 80 %** (base 0.425 / 53 %); the
   quiet hours unchanged (h13–16 mature 0.16 / 89 %); lanes unchanged (WL ≥ 98 %, EWL 100 %).
   Counters (invariant #4): `n_readmit_events` ≈ the eclipse-return count (order 50–60 over 24 h
   for ~30 exclusion edges / 12 h), clock seeds > 0 with `n_readmit_seeded_anch` > 0,
   `n_readmit_seed_moved` < 10 % of clock seeds, `n_cpl_readmit_diverted` > 0.  Consumer h3–h7 ≤ 0.5
   m (base F3 1.1–1.8 m; the pre-F3 base 0.12–0.48).  Falsifiers: settled quarter > 0.3 cyc;
   `seed_moved` > 50 % of seeds (the parked realization is wrong more often than right); a
   degradation episode NOT present in the base.  A settled quarter between 0.2 and 0.3 cyc with the
   episodes visibly shortened is a PARTIAL (the edge is the right target, the seed/window sizing is
   not) and gets one follow-up arm, not a verdict.
3. `ratio3h`: settled window (last 25 %) handovers fully continuous (the 1e-2 arm: 21/21), **0
   settled PB frame steps > 1 m**, every refusal on an arc with `var/var_first > 0.01` (by
   construction — read the PBGAUGE rows), DD grade within noise of the 1e-2 arm (0.187 cyc / 83.1
   %), consumer settled 3D rms within 10 % of 0.245 m.  Falsifier: the settled window loses
   continuity (refusals on settled arcs) or the consumer walks.
4. `cplreg_q12warm_d`: the G20 (306240) and E31 (306450) `gap` trips do NOT fire (both seeded;
   `cta_decouple reason=gap` count over the leg 15 → ≤ 2), G20/E31 per-sat clock error at 309630 ≤
   0.3 m (arm C −2.25 / −1.12), **consumer h12 rms ≤ 1.0 m** (arm C 2.05, B 0.67, A 0.77 — the
   §18.12 stop rule 1 satisfied), wire between-sat SD at the seam unchanged vs C (0.025 G / 0.030
   E).  Falsifier: a seeded satellite trips `nis` within 300 s of its seed on ≥ half the returns.

Results go in §6.11 as they land; nothing above is edited after the fact.

**Addendum, arm 5 (written 2026-09-05 21:05 PDT, after arm 2's DD grade and BEFORE arm 5 was run).**
Arm 2 (`readmit24h_e2`) returned the base's DD to the digit (full 0.281 / 64.3 %, settled 0.420 /
53.4 % vs 0.283 / 64.2 % and 0.425 / 53.2 %) with the returned satellite's clock seeded AND its
columns marginalized out of every block for 20 epochs (2.98 M terms diverted) — so the post-eclipse
episodes are NOT the shared border.  By elimination the path is LOCAL: the legs BORN at a satellite's
re-admission (every station sees the returning satellite at once: ~20 stations × B bands of fresh
legs per return) carry their code-level birth error into the block, and the joint whitening L⁻¹
spreads it into the station's mature columns at their tight variance — §3.1's birth-frame
inheritance, amplified by F3 (which is why F1's diagonal control showed a smaller bump, §6.9).
Built: `network.phase_leveling_local_block_min_age_ep` — a leg younger than K loop epochs since its
first stash (`LocalCarry::birth_ep`) is kept OUT of the block and emitted as a per-column row
(F1-scaled over the young set when los_scale is armed); counted (`pl_blk_young_cols`).

| arm | config | single change vs its base |
|---|---|---|
| `readmit8h_young` | `_readmit8h_young.json` | the 24 h F3 posture (`_nonint_losblock24h_hv`) cut to 8 h + `phase_leveling_local_block_min_age_ep 30`; control = the first 8 h of `runs/nonint_losblock24h_hv` |

PRE-REGISTERED: after the h3.3–6.9 re-admission cluster the clean-chord L1C med|e| in h6–h7 stays
≤ 0.25 cyc (base 0.42 / 0.49) and the h6–h7 all-chord med|e| ≤ 0.30 (base 0.42 / 0.49); the quiet
hours h2–h3 unchanged within ±0.03 cyc (base 0.19 / 0.20); lanes unchanged; `pl_blk_young_cols` > 0
and rising at each return.  Falsifiers: h6–h7 > 0.35 cyc (the young legs are not the path either),
or h2–h3 worse than 0.25 (the gate costs the quiet state).  A partial (h6–h7 0.25–0.35) gets one
K-sweep arm, not a verdict.

**Arm 5 RESULT + arm 5b pre-registration (written 2026-09-05 22:45 PDT, after arm 5 was read and BEFORE arm 5b
was run).**  Arm 5 (`readmit8h_young`, young legs routed as per-column DIAGONAL rows) drove EVERY hour of the 8 h
window back to the pre-F1 over-count — L1C med|e| 3.78 / 2.58 / 2.58 / 2.70 / 2.31 / 2.27 / 1.99 / 2.35 cyc, 10–16 %
at the integer, WL 40–51 % (base F3 0.19–0.49 / 74–82 % / 96–99 %), quiet hours included, clean and touched chords
alike (228,423 young columns routed per-column over 55,440 blocks).  The falsified premise is "a diagonal row for a
SUBSET of a station's legs is harmless": it is not — §3.1's station-common over-count (rx clock / ZWD / rx-DCB
asserted ~n·B-fold by diagonal rows) re-enters through the ~15 % young rows and freezes the station level for
mature and young legs alike.  Corollary, recorded: the F3 factorization FALLBACK (F1/diagonal rows for that
station-epoch) is harmful for the same reason (0 fallbacks on every arm so far — latent).
Built: `network.phase_leveling_local_block_young_mode` = "free" (default; a young leg is NOT injected until it
matures — no memory, re-solved from its own epoch's data, joins the block at age K with its then-current
conditional variance) | "diagonal" (arm 5's route, kept for the record, loader-warned as measured harmful).

| arm | config | single change vs its base |
|---|---|---|
| `readmit8h_youngfree` | `_readmit8h_youngfree.json` | arm 5 + `phase_leveling_local_block_young_mode "free"` (K = 30, 8 h); control = the first 8 h of `runs/nonint_losblock24h_hv` |

PRE-REGISTERED (arm 5b): the arm-5 criterion verbatim — clean-chord L1C med|e| in h6–h7 ≤ 0.25 cyc (base 0.54 / 0.48
clean; 0.42 / 0.49 all), h2–h3 unchanged within ±0.03 (base 0.19 / 0.20), lanes unchanged, `young column(s) kept
out of the block` > 0.  Additional falsifier specific to "free": the young-leg population's own med|e| (the
mature/young table) must not exceed 1.0 cyc at h2–h3 (base young 0.74 / 0.64) — a free young leg has one epoch of
data, and if that reads worse than the diagonal-carried young leg the trade is a loss even if the mature chords win.

## §6.11 Results of the §6.10 arms (filled as they land; nothing in §6.10 is edited)

> ⚠ **Read with §6.12 (2026-09-06):** every DD number below is AS-TRACED against the trace's lexically-lowest reference satellite; the "post-eclipse network-wide DD episode" that items 2, 6 and 7 chase is that reference (G01 / E02) returning from eclipse.  The ref-free re-grades are in §6.12.3.


1. **`readmit_off3h` — invariant #6 PASS (2026-09-05 19:22 PDT).**  New exe `gnss_processor_readmit.exe`
   (sha256 808a0f91…), every new knob at its default: `products.txt` sha256 **b7cea2adc8fc6fb6…**, 8,593,106
   bytes — identical to `runs/nonint_losblock3h_hv/products.txt`.  The OFF path of all four increments is
   byte-inert on the F3 3 h lineage; the arms below are single-family A/Bs against that base.

2. **`readmit24h_e2` (E2 seed 3900 s + coupling window 20) — CLOCK HALF PASS, DD HALF FALSIFIED (20:45 PDT).**
   Counters: 28 re-admissions; 110 entries parked; 26 clock + 84 code-bias seeds (26 clock seeds folded
   ANCHORED); 0 expired / 0 datum-dropped / 0 unconsumed; Q3 trips within 300 s of a clock seed 8 (5 in-epoch at
   the return on the T3 satellites G25/G26/G16/G12 with e 0.18–0.37 m, 3 one epoch later at 5–18 cm); seed checks:
   clock 21 held / 5 moved, C1C 28/28 held, C2W 16/16 held, **C5Q 12 held / 16 moved, C7Q 0 held / 12 moved**
   (the third-frequency code biases re-realize at every return — the §18.1 cold-start C5Q misrealization carried
   through a stiff σ 1.9 cm seed); `gap` trips 26 (base) → 0; 28 coupling windows, 2.98 M terms diverted.
   WIRE (per-sat clock census, settled half, truth): **G25 6.27 → 1.69 m, G12 5.38 → 2.71, G26 6.51 → 4.34** — the
   T3 tails; every other satellite within ±0.2 m.  CONSUMER (hourly rms, E2 / base F3 / pre-F3 control): h3 0.90 /
   1.81 / 1.23; h4 0.54 / 1.10 / 0.98; h5 0.54 / 1.19 / 0.48; h6 0.50 / 1.40 / 0.12; h7 0.32 / 1.11 / 0.12; h8 0.15 /
   0.47 / 0.07; h12–23 0.05–0.13 vs 0.06–0.19 vs 0.05–0.15 — the post-cluster excursion cut 2–3× (pre-registered
   ≤ 0.5 m at h3–h7: met at h6–h7, not h3–h5).  **DD: full 0.281 cyc / 64.3 % (base 0.283 / 64.2 %); settled
   quarter 0.420 / 53.4 % (0.425 / 53.2 %); every hourly episode identical** (h6 0.442 / h7 0.480 / h19 0.581 / h20
   0.557 vs 0.422 / 0.485 / 0.585 / 0.561; clean-chord h6 0.545, h19 0.654).  The pre-registered settled ≤ 0.2 /
   ≥ 80 % is NOT met, and the mechanism §6.9 named — the returned satellite's shared columns entering the blocks —
   is FALSIFIED: with those columns marginalized out of every block for 20 epochs AND the clock seeded, the DD did
   not move by a digit.  Verdict: `readmit_seed_horizon_s` = the T3 fix on the wire; the code-bias half of the seed
   is wrong for third-frequency signals (sub-knob `readmit_seed_code_bias`, arm `readmit24h_e2clk`);
   `readmit_coupling_marg_ep` = inert on the DD (no harm measured) — keep OFF pending a target.

3. **`ratio3h` (ratio 0.01, absolute rail 1.0 m²) vs the 1e-2 arm — MECHANISM VALIDATED, VALUE 0.01 FALSIFIED.**
   Handovers 53 continuous / 40 cold (base 70 / 23); the 40 refusals are exactly the r = var/var_first > 0.01
   population (0 by the rail).  Settled window 18 / 3 (base 21 / 0): the 3 refusals are ONE family at ONE epoch —
   E25 L1C/L5Q/L7Q at 269280 (S01000SIM → S09000SIM), var 2.8e-3 / 9.0e-3 / 8.2e-3 m² (all UNDER the 1e-2
   absolute), var_first 0.075 / 0.238 / 0.215 m² → r = 0.038; every other settled handover had r in
   0.0003–0.0084.  Cost: the cold re-pin put a **7.6 m phase-bias frame step** on the wire at 269280 (base:
   0.03 m, 0 keys) — the pre-registered "0 settled PB steps > 1 m" FAILED.  DD 0.196 / 82.1 % (base 0.187 / 83.1 %)
   and consumer 0.223 vs 0.243 m: no harm elsewhere.  Reading: the instrument is exact and both edges are counted,
   but the DENOMINATOR is biased for an arc that spent time PINNED as the anchor — a pinned arc is never carried, so
   its "first stash" lands after the pin moves away with an already converged variance (E25 @ S09000SIM: 0.075 m²,
   σ 27 cm, vs the ~0.8 m² code-level birth of a fresh arc) and the ratio reads a converged, harmless successor as
   unconverged.  KEEP the absolute 1e-2 as the working value; the ratio form needs a birth variance harvested at the
   arc's FIRST SIGHTING (pinned or not) before it can replace it.  Ledger: KEEP OFF, denominator card owed.

4. **`cplreg_q12warm_d` (seam leg: arm C + E2) — E2 INERT on the seam consumer; §18.12's attribution corrected.**
   See SPEC_synthetic §18.14.

5. **`frwriter_sticky_v3c` — WRITER GATE PASS.**  products.txt 34,840,533 bytes, a byte-identical prefix of the
   continuous run; `.ep1440` v3 with QK 376 / QP 376 / QL 2811 (16.8 MB); history pruned to the branch point.

6. **`readmit8h_young` (arm 5: young legs as per-column DIAGONAL rows, K = 30) — FAIL, and a finding.**  L1C
   med|e| 3.78 / 2.58 / 2.58 / 2.70 / 2.31 / 2.27 / 1.99 / 2.35 cyc (h0–h7), 10–16 % at the integer, WL 40–51 % —
   the ENTIRE solution back at the pre-F1 over-count, quiet hours included, clean and touched chords alike
   (228,423 young columns over 55,440 blocks).  The falsified premise is "a diagonal row for a SUBSET of a
   station's legs is harmless": §3.1's station-common over-count re-enters through the ~15 % young rows and freezes
   the station level for every leg.  Corollary: the F3 factorization FALLBACK (diagonal rows) is harmful for the
   same reason (0 fallbacks on every arm so far — latent).  Response: young legs FREE (arm 5b).

7. **`readmit8h_youngfree` (arm 5b: young legs FREE until age 30).**  FAIL — the newborn-leg hypothesis is falsified too (23:05 PDT).  Hourly L1C (5b / base): h2 0.221 / 0.192, h3 0.214 / 0.198, h4 0.308 / 0.232, h5 0.350 / 0.271, h6 0.554 / 0.424, h7 0.588 / 0.485; WL 84–92 % (base 96–99 %); clean chords h6 0.761 / h7 0.616 (base 0.538 / 0.478) — the episode LARGER, not smaller; mature legs h2–h3 0.175 / 0.169 (base 0.160 / 0.167); free young legs 1.7–2.0 cyc (base carried young 0.74 / 0.64, the additional falsifier hit); 232,089 young columns kept out of 55,440 blocks.  Removing the newborn legs from the block does not protect the mature chords through the cluster.  With arm 2, the post-eclipse DD episodes are NEITHER the returned satellite's shared columns NOR the newborn legs' block membership; both age-gate responses fail → `phase_leveling_local_block_min_age_ep` KEEP OFF (free = harmless but useless, diagonal = harmful).  The base's event census through the h16–h21 episode shows no datum / era / withhold events (anchor handovers 12–33 and ~20 T1 refsat_resets per 20 min, 7-column col_added/removed per return) — the next card is a fresh investigation of what re-realizes NETWORK-WIDE at a re-admission cluster that the exactly-conditioned F3 block resists (prefit census / topology ledger / rhs_attrib), not another gate.

8. **`readmit24h_e2clk` (clock-only seed).**  FALSIFIED as an improvement (23:55 PDT).  26 clock seeds, 0 code-bias seeds; seed checks 20 held / 6 moved; 7 seed-attributed Q3 trips (arm 2: 8).  WIRE (settled-half per-sat clock): G12 6.26 m (arm 2 2.71, base 5.38), G26 6.15 (4.34, 6.51), G25 5.63 (1.69, 6.27) — the T3 tails RETURN.  CONSUMER h3–h8 1.76 / 0.71 / 0.66 / 0.78 / 0.34 / 0.13 m (arm 2 0.90 / 0.54 / 0.54 / 0.50 / 0.32 / 0.15).  DD unchanged (0.287 / 64.0 %, settled 0.430 / 53.0 %).  The IF-pair code-bias seeds anchor the clock-vs-code-bias SPLIT at the return, so the clock lands near the pre-gap realization even when its own seed is rejected; without them the cold third-frequency misrealization relocates into the clock — §18.1's T3 mechanism.  The third-frequency seed rejections are the stash-side check working, not harm.  `readmit_seed_code_bias` stays TRUE (default); `readmit_seed_horizon_s` 3900 s (both kinds) is the validated T3 fix → PROMOTED: ledger RECOMMEND_SCOPED (Q3-anchored lineages without clock_model, armed_when > 0), posture row, sim golden pin 3900 (manifest, regenerated golden, verify PASS, 20/20 smoke).

## §6.12 The "post-eclipse network-wide DD episode" was the INSTRUMENT'S REFERENCE SATELLITE (2026-09-06)

Opened as the investigation card §6.11 items 2/6/7 pointed at ("what re-realizes NETWORK-WIDE at a
re-admission cluster that the exactly-conditioned F3 block resists").  Closed on the existing 24 h arm's
own trace without a new run: nothing re-realizes in the estimator — the DD trace's reference satellite does.

### 6.12.1 The instrument

`network_dd_fractional.csv` (filter/NetworkDdFrac.cpp) forms, per (constellation, signal), the
between-satellite SD of every graded satellite against ONE reference — "the lexically-lowest satellite
present at ≥ 2 stations" — and then the between-station DD of that SD over every station pair that sees
both.  On the wu93 24 h fixture that reference is **G01 and E02 for the entire day** (the coverage audit,
`scratchpad/ddep/dd_coverage.py`: G01 on 22 of 24 hours, G02 substituting only while G01 is excluded at
h4–5 and h16–17; E02 on 22 of 24 hours, E03 at h3–4 and h17–18).  Two consequences the graders never
controlled for:

1. **Coverage rotates with the reference's footprint.**  A chord exists only for station pairs that both see
   G01 (E02), so the trace covers 21–41 of the 60 stations at any hour (196–437 pairs) and the covered set
   is a different region hour by hour (S02–S36 at h15, S20–S60 at h19).  Every hourly, clean/touched,
   mature/young and per-station split of §6.11 compared DIFFERENT station populations.
2. **Every chord inherits the reference's own error.**  With s the between-station SD ambiguity error of a
   satellite at a pair, the graded quantity is e_i = s_i − s_ref: the reference's error is COMMON to all
   chords of a (ref, sta_a, sta_b, signal) group.  When G01 / E02 return from eclipse exclusion (G01 at
   h5.6 and h17.6, E02 at h4.3 and h18.4 — the GPS and Galileo orbital periods), the whole trace is
   re-referenced to a satellite whose legs were just reborn at every station, and the trace reads that
   satellite's per-station convergence as a network-wide episode.  The "episodes" at h6–h9 and h18–h22 are
   the hours after those four returns, and h14–h15, where G12 (14.3) and E25 (16.6) returned but the
   references did not, show none — the cluster hypothesis never fit that.

### 6.12.2 The decisive test — re-referencing in post-processing

Because the reference's share is common to the group it cancels in any between-satellite difference.
`scripts/simnet_dd_reref.py` (selftest `--selftest`) streams the trace and reports, per hour and
constellation, the as-traced grade med|e_i|, the group's COMMON component c = median_i e_i (≈ −s_ref when
most legs are good), the REF-FREE residual med|e_i − c| with its round-to-zero share (groups of ≥ 3 sats),
and an oldest-reference variant e_i − e_j with j the longest-present satellite.  Base F3 arm
`runs/nonint_losblock24h_hv`, L1C, stride 4:

| hour | ref | as-traced med\|e\| (round0 %) | common med\|c\| | ref-free med\|u\| (round0 %) |
|---|---|---|---|---|
| 2 (quiet) | G01 | 0.173 (83.8) | 0.128 | 0.094 (91.8) |
| 7 (episode 1) | G01 | **0.523 (48.9)** | **0.515** | 0.110 (83.3) |
| 13 (quiet) | G01 | 0.166 (86.7) | 0.119 | 0.078 (93.6) |
| 19 (episode 2) | G01 | **0.592 (46.9)** | **0.593** | 0.105 (85.4) |
| 20 | G01 | 0.485 (50.4) | 0.505 | 0.087 (86.5) |
| Galileo 2 / 6 / 20 | E02 | 0.217 / **0.486** / **0.659** | 0.176 / 0.526 / 0.602 | 0.087 / 0.084 / 0.108 |

The common component tracks the as-traced number to the digit through both episodes; the ref-free residual
is **flat all day at 0.07–0.11 cyc L1C (83–94 % at the integer)**, WL(1,2) 98.6–99.6 % and EWL(2,5) 99.7–100 %
ref-free in every hour.  Split by the graded satellite's OWN leg age (epochs of continuous presence at the
pair; `dd_reref_split.py`): young (< 120 ep) 0.08–0.17 with a mild rise in the episode hours (the
returning satellites' own legs), mid (120–360) 0.045–0.085 and mature (≥ 360) 0.015–0.079 with NO episode.
The ref-free convergence curve of a leg is 0.270 / 0.170 / 0.114 / 0.066 / 0.046 cyc at 1–10 / 10–30 /
30–100 / 100–300 / 300–1000 epochs.

### 6.12.3 Corrected readings of the §6.11 arms (ref-free, `simnet_dd_reref.py`, stride 8)

- **Arm 2 (`readmit24h_e2`)**: identical to the base in BOTH frames (ref-free h7 0.105 / h19 0.102 vs base
  0.110 / 0.105) — the coupling window and the seeds never had a DD target.  `readmit_coupling_marg_ep`
  KEEP OFF stands, for the corrected reason.
- **Arm 5 (`readmit8h_young`, diagonal young rows)**: ref-free 1.14–2.66 cyc every hour — the harm was
  REAL in every frame (§3.1's station-common over-count re-entering through the diagonal rows).  Verdict
  unchanged.
- **Arm 5b (`readmit8h_youngfree`)**: ref-free h2–h7 0.113 / 0.093 / 0.084 / 0.086 / 0.122 / 0.119 vs base
  0.088 / 0.089 / 0.076 / 0.074 / 0.088 / 0.094 — a small COST (the free young legs at 1.7–2.0 cyc pull the
  group medians), no benefit.  KEEP OFF stands.
- **The 24 h "settled quarter 0.420 cyc / 53 %" (§6.11 item 1 and every place it is quoted)** is the
  reference's convergence after its h17.6 / h18.4 returns.  Full-trace `--ref-free` summary (1,523,679 groups of
  ≥ 3 sats graded, 741,312 pairs with < 3 sats skipped): L1C med|u| **0.089 cyc / 86.8 % at the integer** over
  the day and **0.095 / 85.9 %** over the last quarter (as-traced 0.283 / 64.2 % and 0.425 / 53.2 %); L2W
  0.120 / 84.7 %, L5Q 0.117 / 83.5 %, L7Q 0.100 / 82.7 %; EWL(2,5) 0.012 cyc / 100 %, WL(1,2) 0.029 / 98.8 %
  (last quarter 99.1 %).  The by-age slopes (−0.46) are also contaminated: a chord's "age" restarts when the
  reference returns while its own leg is old.

### 6.12.4 What the field inherits

The DOY-129 fractional histogram (`med|frac_cyc|` uniform 0.25, SPEC_pivot / ledger §A2) rides the same
trace and the same reference rule; a young or slipping G01 smears every fraction of the constellation.
That verdict's CONCLUSION (no integer structure) does not rest on the reference alone, but every fractional
number from that trace must be re-read knowing that one satellite's fraction is in all of them.
Consumers of the trace that inherit the rule: `scripts/grade_ddfrac.py`, `grade_ddfrac_perhour.py`,
`grade_churn_arms.py`, `field_lane_frac.py` and the pyviz viewer `network_dd_fractional.py` — none of them
re-references; each reports the reference's fraction folded into every chord.

### 6.12.5 Fixes

1. **Grader — DONE.**  `scripts/simnet_amb_integer.py --ref-free` adds a REF-FREE block (per band + lanes,
   all / last-window; default output byte-identical; selftest extended with a bed whose reference carries a
   known error) and `scripts/simnet_dd_reref.py` is the hourly per-constellation instrument with the
   leg-age curve.  Quote DD grades ref-free from now on.
2. **Producer — OWED (design; blocked 2026-09-06 only because the concurrent session holds uncommitted edits
   in `GnssConfig.hpp`, `GnssConfigTypes.hpp` and `NetworkRunContext.*`).**  `network.trace_dd_fractional_ref`
   = `"lowest"` (default, byte-identical) | `"oldest"`: per (constellation, signal) keep in `NetworkCarryState`
   an age map (epochs of continuous presence at ≥ 2 stations, reset on absence); pick the satellite with the
   largest age, ties → lexically lowest; count reference switches and print them at run end; manifest key;
   posture row SCOPED under `trace_dd_fractional`.  Trace-class, fingerprint-SAFE.  Validation = an 8 h
   same-build A/B (`lowest` vs `oldest`): products byte-identical, and the `oldest` arm's AS-TRACED hourly
   grade must match the base's REF-FREE grade within the leg-age noise.  The ref-free grader remains the
   reference-independent instrument even after the knob lands (a single old reference still carries its own
   slip / re-level history).
3. **Method rule (also SPEC_synthetic §13.8):** a grade built on a SINGLE reference element must report that
   element's share separately or difference it away; "the whole network moved at once" is the signature of a
   moved reference, and the first check on any network-wide episode is which reference the instrument used
   and what happened to it at the onset.

Result: the card's target does not exist; the re-admission edge card's remaining DD item is closed as an
instrument artifact and the young-leg convergence rate (6.12.2's curve) is the only per-satellite residual.

### 6.13 The FIELD re-grade, and the producer knob (2026-09-06)

§6.12.4 left the DOY-129 fractional histogram owed a re-read: it rides the same trace and the same
reference rule, so "one satellite's fraction is in all of them" applies there too.  It is now measured, and
**the answer is that the field's uniform-0.25 histogram is NOT this artifact.**

#### 6.13.1 A truth-free instrument, and why it needs a null

The field has no `truth_ambiguities.csv`, so `simnet_dd_reref.py` does not apply.  The only observable is the
trace's own `frac_cyc`, which lives on the unit circle.  That is enough: within one
(epoch, cons, signal, sta_a, sta_b) group the reference's share is COMMON, so a circular median estimates it
and the residual is per-satellite.  `scripts/dd_frac_reref.py` (selftest `--selftest`, two beds - one
reference-dominated, one pure uniform noise) reports four readouts per hour and constellation.

⚠ **Every one of them needs a null, and two of the four nulls are counter-intuitive.**

1. **The ref-free residual shrinks by chance.**  Subtracting a group's OWN circular median from its members
   reduces |residual| even for independent uniform fractions - a group of 4 has four chances to sit near its
   own median.  Measured on the uniform bed: as-traced 0.251 -> ref-free **0.145**, a 42 % "improvement" that
   is pure estimator artifact.  The tool therefore prints a size-matched Monte-Carlo null beside every
   ref-free number and the verdict is the **GAP**, never the raw value.
2. **The triangle-closure null is NOT independent-uniform.**  A DD is per-station additive by construction
   - f_i(a,b) = [s_i(a) - s_i(b)] - [s_ref(a) - s_ref(b)] - so for a FIXED satellite the triangle closes
   exactly, reference or no reference; and because c-hat is a circular median it IS one of those data points,
   so a triangle closes whenever the median lands on the same satellite in all three pairs.  Against an
   independent-uniform null the field "closed" at 0.182 vs 0.250 and read as real structure.  Against the
   correct null - the DD structure with NO reference term - it is 0.182 vs **0.172**, i.e. exactly at the
   null.  The first null would have manufactured a finding.

The resultant length R = |mean exp(2*pi*i*f)| is the readout with no estimator in it at all, and it carries
the verdict.

#### 6.13.2 The instrument fires on the sim and is silent on the field

Same tool, same settings; the sim row is the §6.12 base arm, where the artifact is independently known from
the truth grade.

| | sim base `nonint_losblock24h_hv` (G) | field `fld63_stk_on` L1C | L2W | L1W |
|---|---|---|---|---|
| as-traced med\|f\| (uniform null) | **0.168** (0.248) | 0.251 (0.246) | 0.251 (0.247) | 0.248 (0.251) |
| near-integer share (\|f\| <= 0.1) | **32.3 %** | 19.5 % | 20.0 % | 20.0 % |
| resultant R (null) | **0.610** (0.430) | 0.468 (0.473) | 0.447 (0.452) | 0.441 (0.438) |
| ref-free med\|u\| (null), GAP | 0.079 (0.126) **-0.047** | 0.117 (0.109) +0.008 | 0.127 (0.117) +0.010 | 0.130 (0.123) +0.006 |
| triangle closure (DD-structure null) | **0.088** (0.184) | 0.182 (0.172) | 0.182 (0.174) | 0.175 (0.171) |

The truth-free ref-free number on the sim (**0.079**) reproduces the truth-graded one (**0.089**, §6.12.2), so
the instrument is validated on real data and not merely on its own bed.  On the field **every readout sits at
its null**: the wrapped fractions are uniform WITHIN each group, so there is no common component to remove,
and no reference policy - `lowest`, `oldest`, or an oracle - can improve the field histogram.

Two supporting facts, both from the same run.  The field reference **already rotates on its own**: 8-11
switches over 12 h across 3-9 distinct satellites (G01 dominant, G03 / G06 substituting), against the sim's
G01/E02 all day.  And the as-traced grade does **not** degrade after a switch - med|f| 0.246 in the 20 epochs
following a switch vs 0.251 settled - which is the field analogue of the sim's episode signature, absent.

**Verdict: the DOY-129 "no integer structure" reading stands on its own evidence.**  It was never resting on
the reference.  §6.12.4's caution was right to raise the question and is now answered: field DD fractions are
uniform per-satellite, not uniform-because-smeared-by-one-satellite.  What the field DOES inherit from §6.12
is the COVERAGE half - a chord exists only for pairs that both see the elected reference - so per-station and
per-region field splits off this trace still compare rotating populations.

⚠ **A run-selection lesson, paid for in this session.**  The first two "sim controls" chosen (`camp60wu_f3`,
`camp60wu_quiet24h_sticky`) showed field-like uniformity and appeared to contradict §6.12 outright.  They are
different arms whose DD errors are 2-20 CYCLES, so their fractions wrap into uniformity and the reference's
share is unrecoverable from the wrapped file.  **A truth-free instrument is only meaningful where the
underlying errors are sub-cycle**; check the truth-graded magnitude before reading a wrapped grade, and pick
the control arm by name from the document that made the claim, not by resemblance.

#### 6.13.3 The producer knob - BUILT, not yet validated

`network.trace_dd_fractional_ref` = `"lowest"` (default) | `"oldest"`, per §6.12.5's design.

- `include/config/GnssConfigTypes.hpp` - `enum class NetworkDdFracRef` + the `NetworkConfig` field.
- `src/core/config/GnssConfigLoadNetwork.cpp` - parse; an unrecognised name warns and falls back (trace-only,
  so a typo must not kill a long run, but it must never pass silently).
- `include/app/NetworkRunContext.hpp` - `ddfrac_ref_age` / `ddfrac_ref_prev` in `NetworkCarryState`, and the
  three counters.  Age = consecutive TRACED epochs at >= 2 stations; **an absence ERASES the entry**, so a
  returning satellite re-enters at age 1 and cannot inherit its pre-gap seniority - which is the point, since
  a returned reference's newborn legs are exactly what re-referenced the trace.  Not snapshotted: a warm
  resume restarts ages at zero and re-elects by the tie-break, and the run-end switch count makes that visible.
- `src/core/filter/NetworkDdFrac.cpp` - the election.  `satmap` is a `std::map`, so the scan is already
  lexical and a STRICT `>` keeps the lowest of a tied age: **`oldest` degenerates to `lowest` exactly when
  every candidate is the same age**, which is what makes the age_win counter meaningful.
- `src/core/filter/NetworkTeardown.cpp` - run-end readout on BOTH policies.  The switch rate was always there
  and always silent; a grade built on a single reference element has to state that element's behaviour
  (SPEC_synthetic §13.8).

**Status: A/B RUN 2026-09-06 — the mechanism WORKS, the pre-registered PASS criterion FAILS, and the
goldens still stay on `lowest`.**

6 h same-build arms `_ddref6h_low` / `_ddref6h_old` on the F3 base lineage, single knob, crossing G01's h4-5
and E02's h4.3 exclusions.

*Structural half — PASS.*  `products.txt` byte-identical (and the `lowest` arm was re-run from scratch and
reproduced its own hash, so the pipeline is deterministic); `network_solution`, `network_events`,
`network_epoch_status`, `network_stations` byte-identical; `network_dd_fractional.csv` differs, as it must.
NOT inert: 4320 elections each, **765 elections differed from `lowest`**, reference switches **12 -> 6**, and
the reference census shifts hard toward the settled satellite (E03 36,381 -> 112,394 L1C rows;
G02 56,050 -> 81,542).

*Grade half.*  h0-h3 and GPS h4 are IDENTICAL arms — early in a run every satellite has the same age, so the
tie-break returns the lexically lowest, and h0 is ignition (as-traced 1.25-1.32 cyc in both) and not gradeable
at all.  The divergent hours, truth-graded L1C, stride 4:

| | `lowest` as-traced | `oldest` as-traced | base REF-FREE |
|---|---|---|---|
| E h4 | 0.280 cyc / 69.8 % (E02+E03) | **0.227 / 73.6 %** (E03) | 0.068 / 89.0 % |
| E h5 | 0.334 / 58.9 % (E02) | **0.195 / 70.3 %** (E03) | 0.058 / 87.5 % |
| G h5 | 0.236 / 74.6 % (G02+G01) | **0.170 / 84.6 %** (G02) | 0.081 / 91.6 % |

So `oldest` does exactly what it was built to do: at the hours where the lexically-lowest reference has just
returned from eclipse, it elects the satellite that stayed up and the as-traced grade improves 25-42 % (E h5
0.334 -> 0.195, and 58.9 -> 70.3 % at the integer).  **But it does NOT reach the ref-free grade — it lands
2.1-3.4x above it** — so §6.12.5's pre-registered PASS ("the `oldest` arm's as-traced grade matches the base's
ref-free grade within the leg-age noise") is **FAILED**, and it was the wrong bar: it assumed a settled
reference contributes negligibly, and the measurement says a settled reference still contributes 2-3x the
ref-free residual.  That is the theory working, not a defect — a single reference of ANY age is still one
satellite's error in every chord.

Two second-order readings, both coverage effects rather than quality regressions.  The `oldest` arm's own
REF-FREE grade is slightly worse at the divergent hours (E h4/h5 0.095 / 0.111 vs the base's 0.068 / 0.058)
and its leg-age curve starts higher (0.355 vs 0.267 cyc at 1-10 epochs), because electing a different
reference changes WHICH station pairs have chords at all (E h4: 7,307 graded vs 8,388) and E03's footprint
skews younger.  **A policy A/B moves the covered population, so never compare the two arms' ref-free numbers
as if they were the same chords** — the as-traced column is the one the knob is about.

Ledger verdict: **RECOMMEND_SCOPED**, narrowly.  The goldens keep `lowest` under a documented manifest
exception, for a reason that outranks the diagnostic gain: the goldens are the comparison baseline, and
changing which satellite their dd-frac trace references breaks continuity with the entire existing record of
dd-frac grades.  There is no loader posture row — this is an option, not a recommendation the loader should
push.

And the caveat the knob's name invites getting wrong, now measured rather than argued: **an `oldest` trace is
quieter, never reference-FREE.**  For any GRADE the fix is `simnet_dd_reref.py` / `--ref-free`; the knob is for
the case where the as-traced CSV must stand alone — a viewer, an archived file, a consumer script that does
not re-reference.

#### 6.13.4 ⚠ A/B HYGIENE — the stale-trace trap that faked byte-identity here

The first pass of this A/B reported "byte-identical on every trace including `network_dd_fractional.csv`".
**That was wrong, and the way it was wrong is worth more than the result.**  The network traces are written
CWD-relative to `./logs/`, but the capture script (adapted from `scripts/_arm_run.sh`) copied from
`build/x64-Release/logs/`, which held a **stale 10-epoch file from 2026-08-13**.  Both arms copied the same
old file, so the comparison was stale-vs-stale and returned IDENTICAL for every trace — the single most
reassuring possible answer, produced by reading nothing.  It survived a smoke A/B and was only caught because
the run-end counter said 765 elections differed while the files said none did.

Three rules, now enforced in the capture script:

1. **Assert the captured file is NEWER than the run's start time.**  Presence is not freshness; a copy's own
   mtime is always new, so `stat` the SOURCE, not the destination.
2. **Capture from `./logs/` and verify the path on a file whose content you can date** (a row count, an epoch
   span).  A trace that is the wrong size by two orders of magnitude is the cheapest possible check.
3. **A trace-only A/B must be cross-checked against a run-end COUNTER.**  Byte-identity is the expected result
   of a correct OFF path AND of a broken capture; only a counter distinguishes them (standing invariant #4
   applied to the instrument itself).

The second arm also OVERWROTE the first arm's `./logs/` trace before it was captured — the concurrent-run log
collision, hit sequentially — so the `lowest` arm had to be re-run.  `scripts/_arm_run.sh` carried the same
wrong source path and is fixed: it reads `./logs/` and asserts the SOURCE mtime is newer than the run's start.

**The smoke A/B was then re-run through the fixed capture (2026-09-06, Release rebuilt from committed source,
sha256 `fe907a95…`), and it IS byte-identical — genuinely this time.**  `products.txt` and every trace
(`network_dd_fractional`, `_solution`, `_events`, `_epoch_status`, `_estimability`) match, with no `STALE`
warning on either arm.  The two cheap checks that would have caught the original failure both fire correctly
now: the captured trace is **111,139 rows over 40 epochs** where the stale file was 20,001 rows over 10 — a
5.5x size and 4x epoch-span discrepancy visible without opening it — and the counter cross-check AGREES with
the files (`0 election(s) differed from 'lowest'`, 240 elections per arm, 0 switches).  That agreement is the
whole point: byte-identity plus a zero counter is a consistent, explained result, whereas byte-identity plus
`765 elections differed` was the contradiction that exposed the broken capture.  A trace-only A/B is only
believable when both readings are quoted together.

## §6.14 The IF-TD reference gate: 100 % false, and the fix was already in the goldens (2026-09-08)

§7 item 5 ("1917 false refloats/day, IDENTICAL across arms") is CLOSED.  It is a real defect of the
posture that produced it, its accuracy on this bed is exactly **zero**, its root cause is the
simulator's own phase noise, and the knob that removes it (`network.cycle_slip.if_phase_td_ref_gate`,
Card R) is **already armed in both goldens** — the `nonint_*` ladder had it POSTURE OFF.

### 6.14.1 Ground truth: there is nothing to detect

`runs/simnet60_quiet_wu93_24h` has `cycle_slips.rate_base = 0.0`; all sixty `truth_slips.csv` are
empty (**0 injected slips**).  Every one of the 2482 `refsat_reset` events (1917 acting + 565
diagnostic) is therefore a false positive.  Precision **0/1917**.  The pool arm over the same run:
**0 flags / 2,754,791 opportunities** (`network_flag_rates.csv` T1) — correctly silent.

### 6.14.2 The statistic is pure common mode, and it is the reference satellite's own noise

The gate tests `|median(non-reference DDs)| > 0.5·λ_IF` = 53.48 mm.  Because
`dd(sat) = ΔOMC(sat) − ΔOMC(ref)`, the reference's own epoch-to-epoch noise is common to **every**
member of the family, so the median cannot average it away:

| n (family size) | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|
| σ(median) [mm] | 17.67 | 17.30 | 17.65 | 17.40 | 17.68 | 17.66 | 18.39 | 18.24 |

Flat — no 1/√n at all.  A variance decomposition over cells of (epoch, reference satellite) shared by
≥ 3 stations splits it **88 % station-local (14.83 mm) / 12 % satellite-side (5.43 mm)**, and the
station-local half is exactly the declared fixture noise: 3 mm zenith phase × 2.9782 (IF
amplification) × √2 (time difference) ÷ sin(el_ref ≈ 62°) = **14.3 mm predicted vs 14.83 measured**.
The same budget predicts p50 |dd| = 15.4 mm at 45° (measured 15.2) and 34.3 mm at 15° (measured 33.6).

So σ(statistic) = 17.7 mm and the fixed bar sits at **3.02 σ**.  Measured exceedance 0.72–0.84 % of
station-epoch-constellations; × 342,623 opportunities/day = the observed ~2500.  **The gate is a 3σ
threshold on noise, and nothing in it is about a cycle slip.**

Three populations, all false:

| population | rate | share |
|---|---|---|
| cold-start burst, ep 1–11 (profile 116, 114, 113, 112, 110, 106, 99, 88, 70, 43, 25) | ~100/epoch | 996 = 40 % |
| steady state, ep ≥ 12 | 0.504/epoch = 1452/day | 1486 = 60 % |
| of which reference-handover epochs | **12.26 %** vs 0.487 % baseline (25×) | 328 = 17 % |

The burst profile is **byte-identical** between the `nonint_*` ladder and the golden lineage, so it is
a property of the fixture and the detector, not of the estimator arm — and it decays in lockstep with
convergence.  That also corrects the header's claim that the detector is "independent of Kalman filter
state": the *observable* is, but `phi_if_omc` subtracts `obs.modeled`, whose ZWD term is a filter
state, and the burst is that term settling.

### 6.14.3 It is a FIXTURE-REALISM artifact — the field is 5× quieter

Elevation-matched against a real 300-station DOY-129 IF-TD trace
(`runs/_partial_ggq_ep11_traces_20260812`), p50 |dd| in mm:

| elev | 10–20 | 20–30 | 30–40 | 40–50 | 50–60 | 60–70 |
|---|---|---|---|---|---|---|
| sim | 33.61 | 21.98 | 17.38 | 15.22 | 14.07 | 13.31 |
| field | 6.85 | 4.84 | 3.54 | 2.98 | 2.60 | 2.14 |
| ratio | 4.9× | 4.5× | 4.9× | 5.1× | 5.4× | 6.2× |

The sim flattens at ~13 mm above 40° — that floor **is** the reference satellite's own noise (predicted
14.3 mm), visible a third way.  At field noise the same 53.5 mm bar is ≈ 15 σ, not 3 σ, which is why the
field measured **14** unsupported refsat flags (`SPEC_slip_qc_hardening` §8.4.4.1) where this bed
produces 2482/day.  ⚠ Do not port a fixed physical threshold between beds whose noise differs 5×.

### 6.14.4 The A/B — Card R takes the acting refloats to ZERO

Both arms from `config/golden/golden_sim_tri_w6.json`, ONE knob differing, same exe
(`gnss_processor_ifref.exe`, md5 `2468e875…`), 2 h / 240 epochs / 60 stations, sequential with
SOURCE-mtime capture assertions (no `STALE`).  This discharges the "one 2 h arm owed".

| | OFF (legacy) | ON (Card R) |
|---|---|---|
| refsat refloats, ACTING | 546 | **0** |
| refsat, diagnostic (carry cold) | 565 | 118 |
| distinct (station,sat) arcs hit | 137 | **0** |
| steady-state rate, ep ≥ 12 | 0.504/epoch | **0.000/epoch** |
| total events logged | 1502 | 509 |
| MW/WL slip events | 142 | 142 |
| pool arm T1 opp / flags | 227,644 / 0 | 228,634 / 0 |
| σ̂₀ / σ̂₀_pf / σ̂₀_pf_phase (settled median) | 1.0000 / 0.8802 / 0.7985 | 1.0000 / 0.8802 / 0.7996 |

Run-end counter, the within-run control (ON arm): **1090 legacy candidates → 118 fired**; 948
suppressed by the empirical scale / coherence test, 13 thin-family, 11 by the consecutive-fire cap.
1090 against the OFF arm's 1111 events is a 1.9 % agreement — the counter and the event file are
independent readings and they agree, per §6.13.4.

The 118 survivors are all at epochs 1 and 3 and all `diagnostic`: with zero empirical information the
cold prior `floor_frac·λ_IF/k` makes the gate degenerate to *exactly* the legacy floor by design.  From
the first warm epoch onward it is silent for 237 consecutive epochs.  Offline replay had predicted
this: settled `k·s_eff` = 0.1275 m = **2.38× the legacy floor**, 98.4 % of fire groups already exceed
the legacy floor on 6·disp alone, and the coherence test rejects 75 % of them outright (fires have
disp p50 0.0446 m against a null of 0.0213 — they are the NOISY-family epochs, the opposite of the
coherent common step a reference slip would produce).

⚠ The Card-R evidence base before this run was a **10-epoch COLD smoke** (§8.4.4.1) — a window in
which, by the cold-prior design above, the gate has not yet reached its empirical arm at all.  This is
its first warm validation.

### 6.14.5 What the pool arm can and cannot detect

`thr = max(k_mad·s_eff·elev_map, floor_frac·λ_IF)` with `k_mad` 8 and `floor_frac` **1.0**, i.e. the
floor is exactly the (1,1) jump and the test is a strict `>`.  Settled, over 2.53 M rows: thr p50
**1.9 λ_IF**, p90 4.3; by elevation 4.1 λ_IF at 10–20°, 2.5 at 20–30°, 1.8 at 30–40°, 1.1–1.2 above
60°.  The physical floor binds on only **5.21 %** of rows and **never below 30°**.  Max |dd|/thr over
the whole settled run = 0.846 — it was never close to firing.

For GPS L1/L2 the IF response is `0.4844·ΔN1 − 0.3775·ΔN2` and MW/WL sees `ΔN1 − ΔN2`:

| slip | IF [λ_IF] | WL [cyc] | MW | IF-TD |
|---|---|---|---|---|
| (1,0) | 4.53 | +1 | yes | yes |
| **(1,1)** | **1.00** | **0** | **blind** | **not > floor ⇒ blind** |
| (2,2) | 2.00 | 0 | blind | only above ~30° |
| (7,9) | −0.06 | −2 | yes | blind |

The complementarity is real — (n,n) is exactly what MW cannot see — but the shipped floor sits *on*
the (1,1) boundary, so IF-TD's unique reach is (n,n) with **n ≥ 2 and elevation ≳ 30°**.  The sim
cannot test any of this: it injects only ≥ 12-cycle single-band slips (median 50, i.e. 42–250 λ_IF)
and **never above 44.9° elevation**, while the reference satellite sits at p10 48.0° / p50 61.7° —
**zero overlap, so a reference-satellite slip is unreachable on this bench by construction**.

Two smaller code observations — **both FIXED 2026-09-08, §6.14.6.**

- The pool's own robust scale is computed over `getLastIfJumps()`, which includes the reference
  satellite's structurally-zero DD (12.5 % of samples).  That **deflates s_epoch by 16.0 %**
  (0.01190 vs 0.01416 m).  Harmless in direction (a tighter threshold on a detector with zero false
  alarms), but the scale is not the quantity it claims to be.
- `network_flag_rates.csv` T1 reports **0 flags** on a run in which the same tier drove 1917
  refloats: the reference gate has no flag-rate tier.  Standing invariant #4 — the ON-path telemetry
  does not see the ON path.

### 6.14.6 Both fixed, and the two-arm A/B that says so (2026-09-08)

Neither is a numerical-correctness bug, so neither is graded on truth; both are graded on **what
the instrument reads**.  Bed: `config/test_suite/_ifsc_{old,new}.json`, a 2 h arm of
`config/golden/golden_sim_tri_w6.json` (the quiet 60-station wu93 truth bed — a CLEAN bed by
construction) with only `max_duration_s` and the run paths changed.  The two arms differ **only in
the exe** (`gnss_processor_ifsc_{old,new}.exe`, one source hunk apart, same Release build config and
toolchain), run SEQUENTIALLY because concurrent runs share `./logs/`.  Runner
`scripts/_ifsc_ab.sh` (SOURCE-mtime freshness assertion — the stale-trace-dir trap), comparison
`scripts/_ifsc_ab_compare.py`.

**Defect 1 — tier T1r.** The reference arm now has its own flag-rate row, `T1r`, fed by two new
UNCONDITIONAL counters in `IfPhaseSlipDetector::RefGateCounters` (`gate_opportunities` /
`gate_flags`) and drained at teardown.  It is a SEPARATE tier from T1 on purpose, and the arm's own
numbers are why:

| tier | what one opportunity IS | opportunities | flags |
|---|---|---|---|
| T1 (pool) | one (station, sat, signal-pair) sample | 228 634 | **0** |
| T1r (reference) | one (station, epoch, constellation) family of n ≥ 2 non-ref DDs | 28 680 | **118** |

The denominators differ by ~8× (one reference per family), and the thresholds are different
quantities — `max(k·s_eff, floor_frac·λ_IF)` armed vs a bare physical floor on the legacy path.
Averaging the two would have re-hidden the asymmetry.  **This is the defect exhibited**: the shipped
table reads `T1 … flags=0` for a run whose IF-TD tier reset a reference 118 times.  Cross-checked
against a second instrument (invariant #4): `network_events.csv` carries exactly **118**
`refsat_reset` rows in both arms.  The counters are gate-independent by construction (a unit case
pins `gate_opportunities` equal with the Card-R gate armed and disarmed), so a
`if_phase_td_ref_gate` A/B now compares two rates over one denominator.

**Defect 2 — the reference's structural zero is out of the scale.** `IfJumpInfo::is_reference`
marks it (the pool cannot otherwise know which satellite is the reference), and **both** scale
estimators drop it: the cold pool prior `s_pool` and the warm EWMA feed `s_epoch` — the latter is
the quantity the 16.0 % was measured on, and `s_eff = w·ewma + (1−w)·s_pool`, so fixing one half
alone leaves the scale biased the moment a station-pair goes warm.  The zeros are **12.47 %** of the
228 634 trace rows here, reproducing the 12.5 % measured on the 24 h bed.  Measured, p50 over all
rows:

| quantity | old (ref included) | new (ref excluded) | change |
|---|---|---|---|
| `s_pool` | 0.012451 m | 0.014582 m | **+17.1 %** |
| `ewma_scale` (= s_epoch) | 0.014770 m | 0.017025 m | **+15.3 %** |
| `s_eff` | 0.014744 m | 0.016999 m | **+15.3 %** |
| `thr` | 0.235355 m | 0.273397 m | **+16.2 %** |

The +15.3 % on the EWMA half reproduces the independently-measured 16.0 % deflation.  Pre-registered
PASS criterion — **the pool flag population is unchanged on a clean bed** — holds, and holds
stronger than asked:

- flagged samples **0 → 0**, sample set identical (228 634 rows both arms, 0 only-in-either);
- `dd_m` and `med_m` **bit-identical** on every row (the fix touches the scale, never the
  measurement or its centre);
- `network_solution.csv` and `network_events.csv` **byte-identical** — the solve did not move.

⚠ **Read the 0 → 0 honestly: it is a vacuous pass, and it was always going to be.** §6.14.5 already
measured max |dd|/thr = 0.846 on the settled 24 h run — the pool arm was never close to firing, so
on a clean bed there is no flag population to perturb.  The load-bearing claims are the byte-identity
of the solve and the bit-identity of `dd`/`med`.  What the arm CAN say about sensitivity: max
|dd−med|/thr goes 0.730 → 0.704, i.e. still **30 % short of firing**, so nothing detectable was lost.

⚠ **The direction is permissive.** A correct (larger) scale is a LOOSER threshold, so this fix costs
a little sensitivity — it is not an improvement in detection power and must not be quoted as one.
What it buys is that the number is the quantity it names.  One measured consequence worth having:
the physical floor was the operative threshold on **4.63 %** of rows with the deflated scale and is
on **0.91 %** with the corrected one (§6.14.5 measured 5.21 % on the 24 h bed).  The deflated scale
was pushing the gate onto its λ_IF floor ~5× more often than the noise justified — so the detector
is now genuinely scale-driven rather than floor-driven, which is the substantive change here.

Still open (unchanged by either fix): the floor sits ON the (1,1) boundary, so IF-TD's unique reach
is still (n,n) with n ≥ 2 and elevation ≳ 30°, and the sim bench still cannot test a
reference-satellite slip (zero elevation overlap, §6.14.5).

## §6.15 The FIELD F3 arm — PRE-REGISTERED 2026-09-09 (§7 item 21; campaign queue item 3's lineage question deferred to a triple-frequency chain)

F3 (`network.phase_leveling_local_block`) is sim-validated only: 3 h (§6.8), 24 h (§6.9, ref-free re-grade §6.12
— L1C 0.089 cyc / 86.8 % at the true integer over the day), and it is the vehicle of `golden_sim_tri_w6`.  The
field has never run it: every field lineage (golden_field_gps_dual, FT63/EXT63/FLD63) carries ambiguities on the
Card-4 `carry_invariant` vehicle, under which the phase-leveling inject is skipped outright
(`ApplicationNetworkAssembly.cpp` `do_amb_carry = (…) && !carry_invariant`), so F3 is structurally inert there.
A field F3 arm is therefore a VEHICLE SWITCH first and F3 second, and the ladder is built that way.

### 6.15.1 Bed, arms, provenance

Bed: DOY-129 (2024-05-08) 00–12 UT, the E3 field posture `_fld63_stk_on` = `_ext63_12h_evt_pl_clkpin` (61 real
GPS L1/L2 stations, SH-VTEC deg-8 co-estimated iono, external COD orbit/code-bias, precise-clock soft hold,
pivot clock datum MBAR00UGA, `phase_leveling: true`, `carry_invariant: true`, `carry_conditional_coupling: true`,
`phase_leveling_max_gap_s` 900, Card R armed, `emit_clock_disc`, the standard QC/observability set) +
`sticky_pivot: true`.  WTZZ00DEU is the colocated hold-out consumer.  The chain sits at the loader's 8-level
`extends` limit, so the arms restate the posture on `_ext63_12h_evt_pl_clkpin` rather than extending
`_fld63_stk_on`; smokes and replays are siblings, not children.

| arm | config | knobs beyond the posture | single mechanism |
|---|---|---|---|
| 0 `fldf3_ci` | `_fldf3_ci.json` | `trace_rx_states`, `trace_zwd` (trace-class) | the shipped posture, same-build control |
| 1 `fldf3_pl` | `_fldf3_pl.json` | + `carry_invariant: false` (with its sub-family `carry_invariant_innov_gate / _innov_calibrate / _innov_corroborate / _honesty / _pd_safe` set false explicitly — inert without the vehicle, and the loader hard-errors on `innov_gate` without `carry_invariant`), and the EWL/UWL lane layer off (`mw_wl.ewl_enable / uwl_enable / uwl_cold_fix / wl_inject_enable: false` — the lanes ride the invariant carry's chord basis and the loader hard-errors without it), `cycle_slip.relevel_free_epoch: true` | the VEHICLE (leveling carry + its invariant-#1 slip response; the lanes are structurally unavailable on it) |
| 2 `fldf3_on` | `_fldf3_on.json` | + `phase_leveling_local_block: true` | F3 |

`relevel_free_epoch` is part of the leveling vehicle, not a fourth arm: standing invariant #1 requires the zero-q
leveling carry's wired slip response, the re-level-walk fix IS that response (SPEC_slip_relevel_walk, 720/720 on
wu93; pinned on the sim golden) and `mw_detect` is already armed on the chain.  Everything else stays at the
chain's value in every arm, two of which matter for reading the result: `phase_datum_handover_max_var_m2` is the
loader default **1.0 m²** on this chain (not the 1e-4 of the FT/camp60wu sticky lineages), so the honest-σ
successor interlock of §6.8.3 cannot bind here; and `phase_datum_handover_continuous` is **false** (cold PB-gauge
handovers, as the chain has always run).

⚠ **A named asymmetry, kept on purpose.**  On the `carry_invariant` vehicle the post-fit network RAIM pool is
inert unless `network.raim_carry_invariant` (default false) re-hosts it (`ApplicationNetwork.cpp` calls it under
`raim_on && (carry_warm || raim_ci_on)`, and `carry_warm` needs the leveling carry) — arm 0 reports RAIM armed and
never runs it, while arms 1 and 2 run it live once warm, with refloats.  Arm 0 is left as the SHIPPED posture
(comparability with `runs/fld63_stk_on` and the field golden) rather than re-hosted off-posture; the card's
graded delta is **arm 2 ↔ arm 1** (same vehicle, same live pool, same refloat response), and the vehicle-switch
reading (arm 1 ↔ arm 0) is graded on the instruments that exist on BOTH sides — the prires / block-Baarda /
station-congruence run-end counters and the truth-free DD instrument — with this asymmetry stated.

Provenance: one exe for every arm, built from HEAD `00d5fdf` in a detached worktree
(`.claude/worktrees/f3field`, VS 2026 Release), snapshot `gnss_processor_f3field.exe` beside its DLLs, sha256
`4c6aa137f67888de32afb851a2d72dbb29dcb85954f8d73739a71a6d4c1d69b4`; runs sequential, each from its own
`runs/<arm>` CWD (the traces are CWD-relative); 20-epoch smokes of all three arms first (`processing.max_duration_s`
600 — the `network.max_duration_s` key only clips the RINEX parse; a first smoke that set the wrong key ran
the full window and was killed).  `mw_wl.enable` and `lane_carry.enable` are OFF on the chain already; the chain's
EWL/UWL lanes (`mw_wl.ewl_enable`, `uwl_enable`, `uwl_cold_fix`) are the one further thing the vehicle switch
must drop, so the vehicle-switch reading (arm 1 ↔ arm 0) also carries "no EWL/UWL lanes" — the lanes touch the
invariant carry's chords and the consumer's lane products, not the phase-leveling carry, so the F3 delta
(arm 2 ↔ arm 1) is unaffected.

Baseline of the truth-free instrument on the field control (`runs/fld63_stk_on`, 2026-09-02 exe, stride 4, G):
L1C as-traced med|f| 0.249 (null 0.246), near 19.4 %, R 0.461 (null 0.472), ref-free gap +0.012; and the
synthesized **WL(1,2)** — 37,913 paired chords of 66,087 L1C / 202,411 L2W rows (the unpaired rows are
mixed-reference or single-band chords) — reads R 0.487 (null 0.482), near 21.4 %, ref-free gap +0.005: ALSO at
the null.  So the control's L1 DD errors are multi-cycle even at the WL scale (|e₁| ≳ 2 cyc), consistent with
§13.8.2's "~5 cycles off with a ±20-cycle tail".  That is the reading arm 2 has to move.

### 6.15.2 Instruments (all truth-free; the field has no `truth_ambiguities.csv`)

1. **DD integer structure** — `scripts/dd_frac_reref.py <run> --signal {L1C,L2W} --cons G` (§6.13.1).  ⚠ Its
   null is the UNIFORM case = no integer structure, so "at the null" is the WORST reading, not a floor: the field
   control reads R 0.468 vs null 0.473 / ref-free gap +0.008 (§6.13.2) and improvement is leaving the null UPWARD
   (R − null > 0, near-integer share > 20 %, ref-free GAP < 0 — the sim F3 base reads R 0.610 vs 0.430, gap
   −0.047).  Quote the GAP to the size-matched null, never the raw median; closure against its DD-structure null.
   ⚠ An at-null reading is AMBIGUOUS on a truth-free bed: multi-cycle DD errors wrap to uniform and read exactly
   like "no integer content" (§6.13.2's run-selection lesson).  So the falsified branch is paired with three
   SCALE-BEARING readouts before it is called a falsification: (i) the **WL(1,2) DD fraction** synthesized from
   the same trace — `dd_cyc(L1C) − dd_cyc(L2W)` per (epoch, sat, ref, sta_a, sta_b) chord, joined on the SAME
   reference satellite in both bands (the reference is elected per signal, so mixed-reference rows are dropped
   and counted), λ_WL = 86 cm.  An n-cycle L1 error is n/3.5 cycles at WL if iono-driven ((f₁−f₂)/f₂ = 0.283 —
   the applicable factor, since the sim root cause is 99 % iono) and n/4.5 if range-common (λ₁/λ_WL = 0.221), so
   a sub-half-cycle WL corresponds to |e₁| ≲ 1.8 cycles: multi-cycle-but-bounded L1 errors show WL structure
   while L1/L2 stay uniform (`scripts/dd_frac_wl.py` writes the synthesized trace in the same schema; the same
   tool grades it AS-TRACED and REF-FREE with the same nulls — quote the ref-free gap); (ii) the trace's own
   `dd_sigma_cyc` median per arm, filtered to rows with `dd_sigma_cyc ≥ 0` and `sigma_carry_aware = 1` (the σ is
   −1 when the covariance is unavailable and the carry-aware flag says whether it could see the carried state —
   the retained fraction is reported per arm so arms are compared on the same population; an honest σ ≥ 0.1 cyc
   on the leveling arms, a collapsed sub-0.02 cyc σ beside a uniform histogram is the over-count's signature);
   (iii) the prires / σ̂₀ families of instrument 2.  Per-station and per-hour splits off this trace compare ROTATING populations
   (the field reference switches 8–11 times/day and a chord needs both stations to see it) and are quoted with
   that caveat.
2. **The σ̂₀ families** present on both vehicles: prires census (worst |t|, rows > 4σ̂₀), block-Baarda census
   (blocks flagged / block-only), station congruence, the constraint-aware global σ̂₀ per family; T3/T4/T1r counts
   from `network_flag_rates.csv`; the post-fit σ̂₀ where the pool is live.
3. **F3's own counters** (invariant #4): the `Network (leveling local-block F3)` run-end line — blocks whitened,
   rows emitted, columns marginalized at inject, pairs zeroed, columns refused, factorization fallbacks, block-dim
   histogram, within-epoch refloat re-whiten ok/failed.
4. **Continuity censuses**: PB-gauge handovers (`network_events.csv` PHASE_DATUM / PBGAUGE tiers), the
   leveling-carry retirement census [A]/[C], the rx-CB carry line, the D0/INC-C0 carry census.
5. **ZWD plausibility** (`network_zwd.csv`, `trace_zwd`): settled per-station |ΔZWD| per epoch (p50/p95),
   hourly |rate| p95, formal σ, per-station span.  No ZTD truth on this bed; the reading is physical
   plausibility (a ZWD that moves decimetres per hour is the over-count wandering, §6.8.4's field analogue).
6. **Consumer**: the in-process WTZZ00DEU consumer (metre-class and non-attributing on this chain — §18.8 item 3;
   reported, not leaned on) and a float-only cold-consumer REPLAY of each wire
   (`_fldf3_{ci,pl,on}_replay.json`, `ar_mode 0`, `Application::runConsumerReplay`) — hourly 3D rms and p50,
   settled ≥ h6.
7. **Cost**: D0 total ms/epoch median after epoch 30 (sim: +7.4 % under F3).

### 6.15.3 Pre-registered predictions (written before any 12 h arm ran; smokes only)

- **P1 (F3 fires and is well-conditioned).** Arm 2's F3 line shows blocks whitened every warm epoch (≈ 61 per
  epoch → ~8×10⁴ over 12 h), 0 factorization fallbacks, 0 columns refused; the block-dim histogram sits in the
  10–49 range (GPS dual: ~2 bands × ~8–12 sats per station).  Falsifier: fallbacks > 0 (the F1 diagonal fallback
  re-asserts the station-common directions, §6.11 arm 5) or refused columns.
- **P2 (the headline).** Arm 2 lifts the field DD fractions OFF uniform on L1C and L2W: R − null ≥ +0.05,
  near-integer share ≥ 25 % (uniform 20 %), ref-free GAP ≤ −0.02, settled half (h6–h12).  Arms 0 and 1 stay at
  the null (|R − null| ≤ 0.02, |gap| ≤ 0.015, as `fld63_stk_on`).  Falsifier: arm 2 at the null on L1C, L2W AND WL(1,2) with an honest `dd_sigma_cyc` → the field's
  non-integer DDs are NOT the leveling over-count (the field residual is then multipath / real slant-iono
  mis-modelling under the SH posture / the §13.8.2 multi-cycle tail), and the AGU-26 "uniform 0.25" stands with
  F3 exonerated as the cure.  A partial lift (R − null in (0.02, 0.05)) is a real but insufficient effect and is
  reported as such, not rounded either way.  Intermediate branch: L1C/L2W at the null but WL(1,2) lifted → the
  errors are multi-cycle-but-bounded at L1 (1–4 cycles) and F3's effect is graded on the WL readout; PARTIAL.
- **P3 (station-common wander).** Arm 1's ZWD wanders (hourly |rate| p95 and per-station span markedly above
  arm 0's — the over-count's field signature, §6.8.4: sim F1 rms 0.19 m) and arm 2 brings it back to arm 0's
  scale or below (sim F3: 0.009 m rms, |err|/σ 0.94); arm 2's formal ZWD σ is HONEST-scale (≥ 3 mm), not the
  collapsed sub-mm of a diagonal carry.
- **P4 (σ̂₀ families).** Arm 2 ≤ arm 1 on prires rows > 4σ̂₀, block-Baarda blocks flagged and worst |t|; T4
  attribution flags on arm 2 ≈ 0 where arm 1 shows the over-confident coupling rows' own residual (sim: 288 → 0).
  Against arm 0 no direction is pre-registered (different live QC).
- **P5 (consumer replay, float).** Settled (h6–h12) 3D rms: arm 2 ≤ arm 1; arm 2 vs arm 0 not pre-registered
  (vehicle + RAIM asymmetry), reported.  The in-process AR consumer is reported only.
- **P6 (continuity).** PB-gauge handovers on arm 2 are not more often cold than on arm 1 (same ceiling, same
  policy); leveling carries erased on arm 2 within 2× of arm 1.
- **P7 (cost).** Arm 2 D0 median within +15 % of arm 1.
- **Stop rules.** Any arm exit ≠ 0, an F3 fallback count > 0, or a withhold era > 60 min ends the ladder before
  grading; a smoke that shows loader warnings naming any ladder knob as INERT stops it before the 12 h arms.

### 6.15.3a Pre-data amendments (2026-09-09 17:45–18:00, written while arm 0 was still running and before any 12 h arm was graded; on peer review)

1. **L5 is absent on this chain** (`processing.frequency_mode: dual`; the control trace carries only G/L1C, L1W,
   L2W, L2X), so the EWL(2,5) readout that would discriminate at the ~5-cycle scale (÷17.9 iono-driven) is
   unavailable here.  The wrapped scale check on this ladder tops out at WL(1,2)'s ÷3.5.
2. **The falsifier of P2 is sharpened.**  The WL(1,2) instrument is at the null on the CONTROL too, so an
   at-null reading on arm 2 bounds the RESIDUAL DD error at ≳ 3 L1 cycles (a uniform WL fraction needs
   |e_WL| ≳ ~1 cyc) and falsifies "F3 brings the field to the integer regime" — it is NOT by itself a proof
   that the leveling over-count is absent (F3 may remove it while another multi-cycle error dominates).
   Conversely the control's WL null is a FINDING, not a baseline: a truth-free lower bound of ≳ 3 cycles on
   the field's DD error scale, corroborating the multi-cycle figure that only the truth side had before.
3. **A third, UNWRAPPED scale readout — per-chord DD ARC STABILITY** (`dd_arc_stability.py`, v3; promoted to
   `scripts/` with the results).  For the SAME chords on the SAME arcs in both bands (paired epochs, arcs keyed on
   `ref_sat` so a reference switch starts a new arc, first 10 epochs of every arc dropped, ≥ 20 epochs, settled
   ≥ h6): MAD of `dd_cyc` about the arc median, drift = |median(last 10) − median(first 10)|, the cross-band
   slope of the deviations (1.283 iono-driven, 0.779 range-common, 1.000 cycle-common), and a REF-FREE pass
   (group median over satellites of the arc-relative deviation removed) quoted beside a size-matched
   iid-Gaussian null of the same subtraction (the estimator shrinks the statistic by itself — the wrapped
   grade's trap 1 transposed).  ⚠ STABILITY IS NOT QUALITY: it cannot see a constant bias (a frozen wrong level
   is perfectly stable — the diagonal-carry sim arm is the MOST stable thing measured here and carries 1.3–2.7 cyc
   of real ref-free error), so an arc-stability number is never quoted alone; every one is paired with an
   accuracy readout (truth-graded DD on the sim; the wrapped fraction and the σ̂₀ families on the field), and
   arms are never ranked by stability.  With the fraction test it splits a multi-cycle DD error into BIAS vs
   WANDER.  Semantics on the sim (v3, trimmed, settled ≥ h6, 6,893 arcs): the F3 24 h arm reads raw MAD p50
   0.148 cyc L1C / 0.189 L2W / 0.047 WL (ref-free 0.063 / 0.080 / 0.021; ref-free/raw REAL 0.43 vs NULL 0.68 —
   the fixed G01 reference's own wander is genuinely in every chord, §6.12) with cross-band slope 1.262 (p10 1.16,
   p90 1.35, corr 0.995) — its residual wander is IONO-SHAPED, the honest per-LOS accumulation floor; the diagonal-carry sticky arm 0.020 / 0.018 / 0.020 (frozen-but-biased: its DD
   errors are 2–20 cycles, §6.13.2), slope uninformative (noise, corr 0.39).  Validation criterion (amended
   17:55, before any field arm was graded — an absolute "≪ 0.1" gate first stated in review was mis-specified,
   since the F3 arm's honest iono-shaped wander is itself 0.1–0.2 cyc): the instrument must show SEPARATION,
   and it does — the sim F3 arm sits ~70× below the field control and the frozen diagonal arm ~7× below F3.
   **Field control (`runs/fld63_stk_on`, L1C+L1W / L2W, 361 chords, 372 arcs):** raw MAD p50 e1 **10.4** / e2
   **10.4** / wl **0.57** cyc (p90 36 / 36 / 1.6), drift p50 25 / 26 / 0.8 cyc per ~65-min arc, 100 % of
   per-band arcs with MAD > 1 cyc; cross-band slope p50 **1.004** (p10 0.962, p90 1.048; corr 0.999); ref-free
   MAD ratio REAL 0.65 vs NULL 0.77 (gap −0.12 on e1, −0.15 on wl).  (The equal MADs, the slope and the small
   WL are ONE measurement — the joint (σ₁, σ₂, ρ) = (10.4, 10.4, 0.999) structure — presented three ways: the
   OLS slope is ρσ₂/σ₁ and the difference's spread σ√(2(1−ρ)) ≈ 0.47, not three independent agreements.)
   So on the shipped field posture the
   per-band DD ambiguity ESTIMATES are not frozen biases: they WANDER by ~10 cycles (MAD) / ~25 cycles per hour,
   in lock-step across L1 and L2 — 1:1 in CYCLES, which is neither the iono direction nor a range-like error —
   while the WL DD is ~20× stiffer; about a third of it is the reference satellite's own wander, the rest
   per-satellite.  ⚠ The mechanism is NOT guessed into the record (candidates, unranked: a cycle-domain model
   term; an estimator constraint holding N₁−N₂ stiff on this posture, e.g. the armed EWL/UWL lanes; a trace
   artifact).  One structural DEDUCTION from the slope alone (not a mechanism claim): a wander that is 1:1 in
   CYCLES across two bands cannot originate in a metric quantity — range-like and iono errors each impose a
   wavelength ratio (0.779 / 1.283) and neither is measured — so it lives in a channel COUNTED IN CYCLES, i.e.
   the ambiguity/level side of the estimator or a cycle-domain model term.  The discriminators are already in
   the ladder: the leveling arms carry no lanes and no invariant carry, and the sim arms' slopes say what the
   estimator does without field effects (F3 1.271 = iono-shaped; diagonal carry: noise).
5. **Independent leg — the estimated slant-iono states of the same chords** (`scripts/iono_arc_wander.py`,
   `network_ionosphere.csv` `iono_total_m` = x_lin + x_r, the full state, so the x_r-only frame caveat does not
   apply).  Field control, settled ≥ h6, same arc rules: single-LOS iono_total_m arc MAD p50 **3.9 m** (p90 13.3),
   drift 8.7 m (p90 29.7); the DD of the iono states over the DD trace's chords (646 chords, 584 arcs) MAD p50
   **1.25 m = 6.6 L1 cycles** (p90 5.4 m), drift 3.5 m = 18 cyc.  So the iono states wander by metres per arc
   too.  Joint per-arc regression of the DD-ambiguity deviation e1 (cycles) on the DD-iono deviation (L1 cycles):
   slope p50 **+1.72** (p10 +0.32, p90 +2.50), corr **+0.93**.  The two wanders are COHERENT and co-directed —
   not the compensation an iono-state error would produce (slope −1 at L1) and not independence (slope 0) —
   yet the ambiguity part is cycle-common across bands (item 3), which the model's iono direction is not.  So
   the (I, N₁, N₂) triple of a field chord moves together along a direction that is NOT the estimator's iono
   direction; recorded as a measurement, mechanism still not guessed.  (Real ionospheric change on DOY-129 is
   metres per hour at low elevation, so the iono states' own wander is not by itself anomalous; the coherent
   cycle-common ambiguity motion riding on it is the anomaly.)
   Sharper form (peer review): if the ambiguities were absorbing an iono-state error δI, the cross-band ratio
   would be δN₂/δN₁ = (f₁²/f₂²)(λ₁/λ₂) = f₁/f₂ = 1.283 — convention-free, identical to the iono fingerprint
   — and 1.004 was measured; so the +0.93 coherence is NOT the ambiguity eating an iono-state error.  The +1.72
   is regressed on the raw per-band e₁, which the wind-up card warns DILUTES a cycle-domain quantity (§13.8.6:
   regress the extracted A).  **Pre-registered before running (18:00):** decompose each chord-epoch exactly
   into e_i = A + B·(λ_i/λ₁) (two bands ⇒ B = (e₂−e₁)/0.2833 = −WL/0.2833, A = e₁ − B) and regress the arc
   deviations of A and of B separately on the DD-iono deviation (L1 cycles).  Prediction: B carries whatever
   iono relation exists (|slope| of order 1, the sign fixing the convention) and A does not (slope ≈ 0); an A
   that still regresses near +1 on the iono deviation is the finding, since a cycle-common channel tracking
   the iono states 1:1 is nothing the measurement model can produce.  Same test on the sim F3 arm as the
   semantic control.  ⚠ Wind-up is the one physical term that is band-common in cycles (ΔWL = 0, MW-blind) and
   it is ALREADY FALSIFIED as the band-common DD error at 0.08 % of the variance, 144× too small
   (SPEC_synthetic §13.8.6, 2026-09-08) — do not re-derive it; the open question is what else is
   cycle-common at a TEN-cycle amplitude, two orders above wind-up.
   **Result (18:05, field control, 372 paired arcs):** A (cycle-common) on the DD-iono deviation: slope p50
   **+1.68** (p10 +0.26, p90 +2.68), corr **+0.90**; B (iono-shaped): slope **+0.09** (p10 −0.14, p90 +0.42),
   corr +0.33.  The pre-registered "finding" branch: the cycle-common channel co-moves with the iono states
   ~1.7:1 and the iono-shaped channel does not.  **Semantic control, sim F3 arm** (`nonint_losblock24h_hv`,
   6,893 arcs): its DD iono states wander by a comparable amount — MAD p50 0.88 m = 4.6 L1 cyc per arc (the
   simulated ionosphere's real change; single-LOS 1.5 m) — and the joint regression of e₁ on the DD-iono
   deviation reads slope **+0.001** (p10 −0.055, p90 +0.111), corr +0.075: on a healthy arm the iono state
   tracks the real ionosphere and the ambiguity does not move with it.  So the field's iono-state wander is
   physical-scale and not itself the anomaly; the anomaly is the ambiguities' cycle-common co-motion with it.
   **How this is argued (peer review, 18:10, before the sim A/B split was read):** (i) A ≈ e₁ by algebra —
   with e₁ = A + B and e₂ = A + 1.283·B, B = −3.53·WL, so with WL MAD 0.57 the B channel holds only ~2 cyc
   against e₁'s 10.4 and A's +1.68 restates the raw +1.72, not a second measurement.  (ii) The load-bearing
   statement is a MAGNITUDE one, convention-free: an ambiguity absorbing the iono-state wander does so
   iono-shaped by construction, i.e. entirely in B, so a 6.6-cycle DD-iono wander would put ~6.6 cyc of MAD into
   B; B carries ~2 — 3.3× too small — and its regression on the iono deviation is +0.09.  (iii) B is
   noise-amplified (a 0.999-correlated difference × 3.53), so "B ≈ 0" needs a power check.  ⚠ The state-total
   regression is NOT that check: on the sim F3 arm the ambiguity error is iono-shaped (cross-band 1.262) yet e₁
   does not follow the iono STATE's total (slope +0.001), because the state total is dominated by the real
   ionosphere the state tracks and the absorbed quantity is the state's ERROR — so the pending sim A/B split is
   pre-registered to read A ≈ 0 AND B ≈ 0 on the state total, and that is expected, not a failure of B.  The
   power check that exists is the cross-band STRUCTURE itself: on the sim F3 arm the error is B-dominant
   (§13.8.6: med|B| 0.206 vs med|A| 0.108, ratio ≈ 2) — the B channel demonstrably carries an iono-shaped error
   when one exists — while the field control is A-dominant (10.4 vs ~2, ratio ≈ 0.2).  A truth-iono join
   (regress B on the state ERROR, sim only) would make the positive control explicit; not run here.
   **Sim A/B split (read 18:03, as pre-registered):** A slope −0.000 (corr −0.005), B +0.002 (corr +0.074) on
   the state total — both ≈ 0, expected.  **Truth-join positive control (regress B on the sim state ERROR):
   OWED**, and it is the leg that decides the load-bearing reading — the field's +1.68 is a regression on the
   iono state TOTAL, which on a healthy arm is dominated by the real ionosphere the state tracks, so it says
   "the cycle-common ambiguity channel co-moves with the estimated DD ionosphere", NOT "with the state's
   error".  **Truth-free discriminator, baseline length** (real DD ionosphere grows with station separation;
   `iono_arc_wander.py` split, field control): DD-iono MAD p50 0.74 m (< 500 km, 63 arcs) → 0.92 (500–1500,
   16) → 1.56 (1500–4000, 88) → 1.57 m (> 4000 km, 205) — it grows like real ionosphere; A's MAD grows with it
   (8.8 → 7.1 → 11.2 → 11.3 cyc) and the A-on-iono slope stays flat (+1.87 / +1.93 / +1.66 / +1.51).  So the
   cycle-common ambiguity channel follows the ESTIMATED DD ionosphere proportionally at every baseline
   length, i.e. it follows what the state tracks — the real ionosphere — not a baseline-independent error.
   **Trace semantics verified in source:** `network_dd_fractional.csv` forms dd_m from the plain ambiguity
   states, `x_lin + x_r` per (station, sat, signal) in metres, differenced between satellites then stations
   (`NetworkDdFrac.cpp:143-158, 245-248`); `iono_total_m` is likewise the plain per-LOS iono state
   `x_lin + x_r` (`NetworkIonoDiag.cpp:98-110`).  Neither is a derived quantity, so the (I, N₁, N₂) co-motion
   is a motion of the ESTIMATED STATES.  ⚠ Consistency question this raises, to be read before any
   mechanism: in metres the co-motion is ΔN₁ ≈ +1.7 ΔI, ΔN₂ ≈ +2.2 ΔI against phase iono terms of −ΔI / −1.65 ΔI,
   leaving ~+0.6 ΔI of range-like model change on BOTH bands that no DD of clocks / geometry can absorb — so
   either the phase post-fit σ̂₀ of the control carries it (check `network_epoch_status.csv`), or a fourth
   per-LOS term of the field posture (the SH↔slant pin with its GM deviation is the candidate) closes the
   budget and the co-motion is a legal, weakly-observed direction of THAT model.  Not resolved here.
   **Read (18:05, the 09-02 control):** the model FITS — `network_epoch_status.csv` settled `sigma0_pf_phase`
   median 0.0105 m (p90 0.012; `sigma0_pf_code` 0 = the code pool is empty on this vehicle, `sigma0_global`
   3.1), and `network_pf_keyed.csv` per-station phase rms p50 1.6 cm (p90 5.4, p99 13 cm; max p50 4 cm) over
   703k settled phase rows.  A 0.75 m range-like misfit per cycle of iono change is nowhere in the residuals,
   **and that FORKS the reading rather than settling it** (peer review, 18:10): the unexplained band-common
   range is ~0.6·ΔI ≈ **0.75 m per arc** (ΔI MAD 1.25 m), and nothing in a double difference is that size —
   receiver and satellite clocks cancel EXACTLY, DD troposphere is centimetres (2 cm ZWD through a 3.8 mapping
   across four terms ≲ 15 cm), DD orbit error centimetres, and a 0.75 m SH↔slant pin deviation would be
   conspicuous elsewhere.  Attenuation makes it worse, not better: the regression is on the iono STATE, which
   carries its own error, so errors-in-variables bias the slope toward zero and +1.7 is a LOWER bound on the
   structural coefficient.  So with the fit normal, no physical term closes the budget, and the two readings
   left are: (a) the regression slope is not a structural coefficient of the phase model at all (corr 0.90 ⇒
   ~19 % of A's variance is not ΔI; the budget line assumes all of A is 1.7·ΔI), or (b) the traced states do
   not enter the phase equation with the assumed coefficients/frame — the TRACE-ARTIFACT branch is alive, not
   dead: the DD trace and the iono trace are both `x_lin + x_r`, but whether that sum is the solve's state at
   the SAME stage of the epoch (before/after re-linearization, harvest, relevel) is not verified.  The
   definitive check is direct: reconstruct the phase residual of a few chords from the TRACED I and N and the
   run's own model terms and compare with the run's post-fit residual (cm) — metres of reconstructed residual
   ⇒ (b).  OWED, with the truth-join.  Until then the field co-motion is a measurement about the TRACES, and
   "the DD ambiguity is not estimable-invariant on this vehicle" is a candidate reading, not a finding.
   **(b) as stated — a cross-sweep STAGE mismatch — is ELIMINATED by a source read (peer, 18:15):** the two
   sweeps are back-to-back in the driver (`networkIonoDiag` at `ApplicationNetwork.cpp:2127`, `networkDdFrac`
   at `:2131`, both after `networkSolveGlue` `:2041` / `networkInstallCarry` `:2075` and before
   `networkLeveling` `:2154` / `networkPostFitRaim` `:2165`, same `est`/`x_s`/`plan`/`asm_res`), and both form
   `x_lin + x_r` identically (`getStateVector()` + `asm_res[i].h2local`).  The traces are consistent with each
   other; what is unverified is only that this `x_lin + x_r` is what the phase rows were fit with (a
   re-linearization/relevel between the fit and the sweep would move both traces together).  Weight moves to
   (a).  Reconstruction scoping: per chord-epoch the four LOS iono totals and four ambiguity totals form the
   modelled band-1/band-2 phase to compare with the run's post-fit ROW residual — but on the ci arm the per-row
   phase residual stream rides the inert RAIM pool (`trace_station_residuals` writes 0 rows on carry_invariant;
   `network_pf_keyed.csv` is per-key aggregates), so the check runs fully only on the pl arm (pool live), i.e.
   it validates the traces on a different vehicle than the one the 0.75 m was measured on; on ci it needs a
   per-row residual dump first.
   **The last unverified step is closed too (peer source read, 18:12, two tools):** the only writer of a
   station's state vector on the network path is `NetworkIonoDwRelease.cpp:438`, invoked at
   `ApplicationNetwork.cpp:2150` — AFTER both sweeps — and the four seams between install-carry and the sweeps
   (producerAr, solPrep, emitStore, fastClockHarvest) contain no mutating `sm->` call; `NetworkInstallCarry.cpp`
   `:422-423` / `:460-461` form absolute WL values with the same `x_lin + x_r` idiom (the wu93 lane observatory was
   validated on it).  **And NO coefficient closes the budget, so (a) does not rescue it either:** zero phase
   residual needs A = 1.000·ΔI on band 1 but 1.2833·A = 1.6469·ΔI ⇒ A = 1.283·ΔI on band 2 — a cycle-common
   channel scales 1.283 between the bands, an iono change 1.647, so a cycle-common channel STRUCTURALLY cannot
   absorb an iono deviation; the best-balanced A still leaves ±0.18·ΔI ≈ 0.22 m band-DIFFERENTIAL residual
   against 1.6 cm measured (A = 1.7 leaves +0.70/+0.54·ΔI).  Attenuation cannot produce an over-estimate.
   **Reconstruction outcome PRE-REGISTERED (18:13, before it runs and before the leveling arms were read):**
   if the traced `x_lin + x_r` pairs are what the phase rows were fit with, the reconstruction MUST show
   decimetre-to-metre residuals the run does not have; either that gap appears (the estimability reading gets
   its evidence, and the run's residual bookkeeping is then the question) or it does not — and then the traced
   pairs are NOT the fitted quantities despite clean staging, which sends the search to the DD CONSTRUCTION
   itself (column mapping `asm_res[i].h2local`, state units cycles-vs-metres, per-station re-estimation of
   ambiguities under the invariant carry making `x_r` large on the field where the sim's frozen carries keep
   it near zero).  Both outcomes are informative; the assumption both the budget and the check share is that
   the DD of states maps linearly onto the DD of modelled phase with no other state moving.
   **Five-minute check of the mis-pairing candidate on data already on disk (18:16):** `network_ionosphere.csv`
   emits `x_lin_m` and `iono_m` (= x_r) separately.  On BOTH beds the iono `x_lin` is a constant seed within
   every arc (arc MAD 0.000) and x_r carries the entire time variation — field control |x_r| p50 11.0 m, arc
   MAD 3.9 m; sim F3 arm |x_r| p50 5.9 m, arc MAD 1.5 m.  So the premise "x_r ≈ 0 on the sim" is FALSE for
   the iono state: the sim's x_r wanders metres per arc, and the sim's DD-ambiguity trace nevertheless reads
   clean (MAD 0.15 cyc).  An ambiguity↔iono column mis-pairing in the DD trace is therefore FALSIFIED as a
   universal bug (it would print the sim's 1.5 m iono x_r into the sim's ambiguities); what survives of that
   candidate is only a vehicle-specific mapping difference (carry_invariant field vs leveling sim), which the
   leveling arms test directly.  ⚠ Correction (peer): this check measured the IONO column's x_r, not the
   AMBIGUITY column's — the original reason to expect a small sim x_r was frozen CARRIES, which govern
   ambiguities — so the mis-pairing is NARROWED, not killed; the DD-trace column split stays the decisive test.  The ambiguity states' own x_lin/x_r split is not traced (only the sum) — the
   one-line `x_lin_m`/`x_r_m` column addition to `NetworkDdFrac.cpp:318-327` (trace-only, byte-identical) is
   the follow-up instrument for the reconstruction.
4. **What the field's "uniform 0.25" now means.**  A DD estimate that wanders tens of cycles within an arc
   wraps to uniform trivially, and no leveling-carry fix addresses WANDER of that scale (the over-count freezes
   a WRONG but STABLE level).  P2's success branch therefore requires arm 2 to ALSO read stable on this
   instrument (per-band MAD ≲ 0.3 cyc, as the sim F3 arm does); an arm 2 that reads at the fraction null but
   with per-band MAD of cycles is a WANDER verdict (the field's DD problem is dynamic, upstream of the
   ambiguity/iono split), not an over-count verdict.

### 6.15.4 Results — filled as they land (nothing above is edited)

**Arm 0 `fldf3_ci` — landed 18:03 (exit 0, 1440 epochs, 27 min wall under a third session's concurrent run):**
the same-build control REPRODUCES the 09-02 control on every instrument: L1C as-traced med|f| 0.250 (null 0.246),
R 0.464 (0.472), ref-free gap +0.012, near 19.6 %; WL12 (37,913 paired chords) R 0.487 (0.482), gap +0.006;
arc stability e1/e2/wl MAD 10.24 / 10.26 / 0.56 cyc, cross-band slope 1.004 (p10 0.962, p90 1.048, corr 0.999),
ref-free/raw 0.65 vs null 0.77; iono leg identical (DD-iono MAD 1.25 m, A on iono +1.66 corr 0.90, B +0.09,
baseline split 0.74 → 1.58 m with a flat A slope).  Not byte-identical to the 09-02 run (carry census
marginalizations 18,456 vs 18,535, T1 flags 322 vs 343, consumer path differs) — the IF-TD pool-scale unbias
(§6.14.6) landed between the two builds, as expected; every graded reading is the same to the quoted digit.
Run-end: prires worst |t| 178 (rxcb), block-Baarda 34/1441 (2.4 %), rx-CB 46,426 folded / 41,297 withheld;
T3/T4 0 / 0 (post-fit RAIM inert on this vehicle, as pre-registered).  In-process AR consumer settled (≥ h6)
3D rms 9.6 m / p50 8.6 — the documented pathology, reported not leaned on.  D0 median 1,072 ms (overlapped).

**Arm 1 `fldf3_pl` — landed 18:13 (exit 0; only 1,122 D0 epochs of 1,440 — 209 `gate_withheld`, 165
`phase_heal`, sbc engage/refuse/release 15/10/13; D0 median 289 ms):** the phase-leveling vehicle WITHOUT
F3 is numerically sick on the field.  ZWD (settled, 61 stations): per-station span median **120 m**, max
1,305 m, |rate| p50 8.8 m/h, formal σ 0.019 m — the station-common over-count of §3 at a scale the sim never
showed (sim F1 rms 0.19 m).  Prior-residual worst |t| 1.2e8 (pl/CLEAN), block-Baarda 5,877 flags, T2
within-epoch trigger 259/974 (27 %), T3 43,714/481,017 (9.1 %), T3c 5,087, T4a 10,601 — the live pool
refloating continuously.  DD instruments: at the null on every signal (L1C R 0.465 vs 0.468, gap +0.008;
L2W 0.449 vs 0.450; WL12 0.478 vs 0.480, 28,304 paired chords).  Arc stability WORSE than the control: e1/e2
MAD **24.2 / 24.8 cyc** (p90 141), drift 63 / 67, WL 4.8 cyc (vs 0.56), cross-band slope 1.080 (p10 0.85,
p90 1.28, corr 0.994) — between cycle-common and iono; ref-free/raw 0.38 vs null 0.70 (the reference carries
a third of it).  Iono leg: DD-iono MAD **3.7 m** (3× the control), and the ambiguity↔iono coupling has MOVED
INTO THE IONO-SHAPED CHANNEL — B MAD 16.8 cyc, B on DD-iono slope +0.78 (corr 0.88), A +0.31 (corr 0.44):
on this vehicle the ambiguities absorb iono error the way the model does it (band-shaped), at ~17 cycles.
In-process AR consumer settled 9.9 m (pathology).  **Reading (peer review, 18:18): arm 1 is not degraded, it
is DIVERGED — and that is itself the result.**  The 199 `skip_epoch` withholds are the `NetworkSbasis`
null-space gate reporting "observed 0 (near-)zero eigenvalues" against its declaration (a datum/rank
mismatch: under the leveling vehicle the chain's `sbasis_complete` + eq34 datum pins MORE than it declares),
i.e. a gauge failure, not a noise one; ZWD spans of 10² m are the same tell.  Consequences: (i) the card's
graded delta (arm 2 ↔ arm 1) is F3 on a diverged baseline unless arm 2 itself converges, so arm 2 is read on
its OWN convergence first (withholds, ZWD span, prires |t|, T3/T4 rates) and only then against arm 1;
(ii) arm 1's B-channel reading (17 cyc, iono-shaped) is a property of a diverging solve and is kept OUT of
the mechanism discussion until a converged leveling arm exists; (iii) the vehicle question underneath §6.15's
opening — WHY every field lineage rides carry_invariant — now has a quantitative answer: on this lineage the
diagonal leveling carry does not converge on real data, so item 21's "does F3 help the field" becomes "F3's
vehicle is unreachable on the field until the leveling posture's datum declaration is fixed" — unless arm 2
shows that the divergence WAS the over-count (F3's whitened block is precisely a conditioning fix for the
diagonal rows' B×n_LOS assertion of the station-common directions, which on the sim took the ZWD from 0.42 m
to 0.009 m), in which case F3 is the fix for the vehicle itself.  Structural next checks, not tuning: the two
arms' declared dimension vs observed null-space census and datum-graph census; and whether the knobs the
vehicle switch had to force off (carry_invariant sub-family, EWL/UWL lanes) removed something load-bearing.
⚠ **ZWD trace caveat that also touches the CONTROL:** `network_zwd.csv` `zwd_m` = `x_lin + x_r(zcol)` in
metres with `x_lin ≡ 0` (`NetworkIonoDiag.cpp:137-141`, zcol via `h2local[getTropoIndex()]`), and on the
control arm it ALSO reads unphysically — per-station settled span median 21 m (BSHM00ISR −74 … +20 m) — on a
vehicle where the ZWD is re-estimated per epoch (not carried).  Either the shipped field posture's per-epoch
ZWD really is a 10-m-class sink (a large finding about the field products; the E3 pair never traced ZWD) or
the trace's local-column mapping differs by vehicle (the same `h2local` question as the DD trace; the sim F3
arm's ZWD grade — 0.009 m rms, |err|/σ 0.94 — came through this trace, so the mapping is right on the
leveling vehicle).  **Checked (18:25, on disk):** the ZWD values do NOT coincide with any of the same
station-epoch's iono x_r values (0 of 20,000 rows beyond chance on ci, pl and the sim), so the ZWD column is
not landing in the iono block; and the σ beside the value is scale-consistent on the control — `std_m` p50
0.57 m (p10 0.11, p90 1.0) beside |zwd| p50 1.3 m — i.e. the shipped posture's per-epoch ZWD correction is
genuinely a weakly-determined metre-class state (not carried, not emitted; it wanders ~40σ over 6 h, so the
σ is not honest either), while on the diverged pl arm the σ is 0.016 m beside |zwd| 1.7 m — the diagonal
leveling vehicle's over-confidence (§3's station-common over-count) in its rawest form; the sim F3 arm reads
0.12 m ± 0.007 (honest).  A shared-`h2local` mapping miss is therefore NOT supported for the ZWD column;
the ambiguity column's split remains untested (the DD-trace column addition).
**The fourth member, PRE-REGISTERED (18:30, before running; peer derivation):** the control's per-epoch ZWD is
a metre-class sink, so DD troposphere on this posture is METRE-scale and band-common in metres — the shape of
the leftover.  With a band-common term T beside ΔI and a cycle-common A, the two-band system λ₁A − ΔI + T = 0,
λ₂A − 1.6469·ΔI + T = 0 closes EXACTLY at A = 2.283·ΔI_cyc, T = −1.283·ΔI (at the control's ΔI MAD 1.25 m that
is 1.6 m of DD tropo per arc, reachable from a 1.3 m per-station |zwd| through four mapping terms).  The
"no physical DD term closes it" premise is RETRACTED (it assumed a centimetre tropo).  Predictions, testable on
disk (`tropo_arc_wander.py`: DD_T = Z_a(mf_a^i − mf_a^ref) − Z_b(mf_b^i − mf_b^ref), mf = 1/sin el, ZWD and
elevations from the run's own traces): (i) the DD-ZWD deviation regressed on the DD-iono deviation has slope
≈ −1.28 with high |corr|; (ii) the joint fit e₁ = a·ΔI_cyc + t·ΔT_cyc raises a from the attenuated 1.7 toward
2.28 and puts t near −1.  If both hold, §6.15.3a's cycle-common channel is the PROJECTION onto the ambiguity
block of (I, N, ZWD) moving together along a weakly-determined direction of this posture — consistent with a
1.6 cm fit, i.e. the estimability reading with its fourth member; and independently of F3, the shipped field
posture's tropo state is a metre-class per-epoch sink with a σ that does not know it.
**RESULT (18:33, field control, 584 arcs, `tropo_arc_wander.py`):** DD-tropo arc MAD **1.22 m** — the same
scale as the DD-iono's 1.25 m.  (i) DD-tropo deviation on DD-iono deviation: slope p50 **−0.87** (p10 −1.49,
p90 0.00), corr **−0.85** — the predicted anti-correlation, at ~70 % of the exact −1.283 with a crude 1/sin el
mapping and attenuation.  (ii) joint fit e₁ = a·ΔI_cyc + t·ΔT_cyc: **a = +0.73** (p10 +0.08, p90 +1.09),
**t = −1.17** (p10 −1.68, p90 −0.89) — i.e. ≈ (+1, −1), which is the band-1 phase equation itself
(λ₁ΔN₁ = ΔI − ΔT).  ⚠ Prediction (ii)'s "a → 2.28" was mis-stated: 2.28 is the SINGLE-regressor coefficient
when T is exactly −1.283·ΔI (the measured single-regressor 1.72 is its attenuated version); in the joint fit
the structural coefficients are (+1, −1), and that is what was measured.  So the band-1 budget CLOSES with
the ZWD as the fourth member: the "impossible" cycle-common channel is the projection onto the ambiguity
block of **(I, N₁, N₂, ZWD) moving together along a weakly-determined direction of the shipped field
posture** — per-LOS iono (SH-pinned), per-epoch un-carried ZWD (a metre-class sink), and per-station
ambiguities trade against each other while the phase fit stays at 1.6 cm.  The DD "ambiguity" of this
posture is therefore not an estimable-invariant quantity in practice; its 10-cycle-per-hour wander is the
tropo–iono trade riding through it.  On the sim golden the ZWD is CARRIED (honest σ, F3-whitened) and e₁ does
not follow I (slope 0.001) — the same estimator with the fourth member pinned.  Fix direction (not built
here): pin the fourth member — carry the ZWD on the field vehicle (B9 `zwd` carry with a physical q) — and
re-read the DD wander; a single-knob arm on the SHIPPED vehicle, independent of F3.

**Smokes (20 epochs each, 2026-09-09 17:4x, before the 12 h arms):** all three exit 0.  Arm 2's F3 line at 20
epochs: 779 blocks whitened, 9,719 rows emitted, 116 columns marginalized at inject, 28 pairs zeroed
(harvest-stamp mismatch), 0 refused, 0 factorization fallbacks, block-dim histogram <10: 136 / 10–24: 643
(max 18) — the GPS-dual block sizes predicted in P1.  No ladder knob reads INERT.  Loader lines that DO change
with the vehicle and are therefore part of the arm-1↔arm-0 reading: `state_snapshot_carry` disabled (needs
carry_invariant), `emit_pb_filter` INERT without carry_invariant (the v1 PB emit filter is off on the leveling
wires — the consumer sees unfiltered PB records), `mw_wl.bridge_sample_debias` INERT without `ewl_enable`; the
post-fit RAIM pool goes LIVE (T3 5,838 opportunities / 91 flags on arm 1 vs 0 / 0 on arm 0 over the 20 epochs),
and on arm 2 it flags 8× more cells than arm 1 in ignition (733 / 5,838; T3c consensus 125 vs 5) — the
§7 item 4 population (F3's dense L⁻¹ rows sit in the T3/T4 pool at unit variance, admitted by the |A_r| > 0.5
test); to be read settled, not in ignition.  Cost: the leveling arms run at ~300 ms/epoch against the control's
~800–1,100 (no invariant DD carry blob), so P7 is read as arm 2 vs arm 1 only.

---

## §6.14.6 The follow-up build (2026-09-09) — abstain while cold, normalize the bar, close the gating leak

Four items came out of §6.14.  Two landed and are validated; the third landed as a
complete mechanism with its FEED deferred, for a reason worth recording; the fourth
turned out not to need the bench it was blocked on.  Every knob is default-OFF and
interlocked to its host arm, so the OFF path is byte-identical.

### 6.14.6.1 Item 1 — ABSTAIN while the empirical arm is cold

`network.cycle_slip.if_phase_td_ref_warmup_abstain` (default false).

With zero empirical information the cold prior `floor_frac·λ_IF/k` makes `k·s_eff`
collapse onto `phys`, so the hardened gate reverts to *exactly* the legacy 3σ fixed
bar for its first few epochs.  That fallback is deliberate — shrinking toward the
current epoch's own dispersion is self-contaminating and once swallowed the
OWMG00NZL/G31 re-lock — but §6.14.4 measured where it leaves us: **all 118 surviving
fires were at epochs 1 and 3, and every one was `action=diagnostic`**, because
`carry_warm` (n_emitted ≥ 6) is false there and a cold detection cannot refloat
anything.  Firing on a bar we cannot justify, to raise an event that cannot act, is
pure event-stream pollution.

Armed, the gate returns `Suppress` until `ref_scale_count` reaches `shrink_kappa`
— the sample count at which the shrinkage weight reaches ½.  Abstention asserts
nothing about the scale; it declines to decide.  The cost is explicit: a genuine
reference slip inside the warm-up is not caught by THIS arm, where it was already
unactionable, and MW/LLI still screens the window.  `suppressed_warmup` counts what
was given up, and the abstained epochs are still counted as legacy candidates — the
refusal is a refusal to decide on a real candidate, not a failure to see one.

### 6.14.6.2 Item 2 — the bar has to track the REFERENCE's elevation

`network.cycle_slip.if_phase_td_ref_elev_norm` (default false),
`…_ref_elev_clamp_deg` (5.0, matching the pool arm's clamp).

The gate tests the MEDIAN of the family, and §6.14.2 measured that 88 % of that
statistic's variance is the reference satellite's own noise divided by sin(el_ref)
— a term in every member that no family size averages away.  The scale it was
judged against, `disp`, is a different quantity: the family's scatter about that
median, i.e. the per-SATELLITE noise at whatever elevations happened to be up.
Unlike the pool arm the gate applied no elevation map at all, and the state holding
that scale (`ConstellationState`) outlives the reference satellite it describes.

Armed, the EWMA stores the ZENITH-equivalent `disp·⟨sin el⟩_family` — a stationary
per-station receiver-noise scale that survives a handover — and the test rebuilds
the statistic's own sigma from it:

```
σ_ref = s_z / sin(el_ref)                  the common term
σ_fam = 1.2533 · (s_z / ⟨sin el⟩) / √n     the part that DOES average down
thr   = max(phys, k · hypot(σ_ref, σ_fam))
```

Validated against the 24 h bed before it was written: s_z ≈ 12.8 mm at el_ref ≈ 62°
gives σ_ref 14.5 mm against a measured station-local common of **14.83 mm**, σ_fam
10.1 mm, and hypot 17.7 mm against a measured σ(median) of **17.70 mm**.  The flat
`k·s_eff` form is blind to both el_ref and n; this tracks them.  The physical FLOOR
stays raw in both modes — a slip is a slip at any elevation (slip-QC lesson 7).

⚠ **The direction is NOT "fewer fires" — measure it before assuming.**  Replayed
offline over the settled 24 h trace (314,318 station-epoch-constellations, s_eff
approximated by that epoch's own dispersion), the normalized bar is p50 **0.78×**
the raw one (p10 0.67, p90 1.00); only 2.5 % of groups get a HIGHER bar.  On this
bed's geometry — an overhead reference over a family whose scatter largely averages
down — the correct statistic is SMALLER than `k·disp`, so `elev_norm` makes the gate
MORE sensitive, and could raise the fire count.  Its case is that the bar tracks the
quantity it is testing, not that it suppresses anything.  Pre-registered before the
arm ran: with `warmup_abstain` already near zero fires, `ifg_we` and `ifg_w` should
be CLOSE, and any difference must appear at the geometry extremes rather than
uniformly.

⚠ One wart, deliberately left: the cold prior is a RAW scale while the EWMA under
`elev_norm` is zenith-normalized, so the shrinkage blend mixes units while cold.
The direction is conservative — the rebuilt bar is ≈1.4× the raw one at typical
geometry, i.e. it errs toward NOT firing, the same direction as item 1, and the two
knobs are meant to be armed together.  Normalizing the prior instead would mean
making a *physical* floor geometry-dependent, which is the thing lesson 7 forbids.

### 6.14.6.3 Item 3 — the nuisance gate, and the leak it exposed

Two halves.  One is a defect fix and landed; the other is a mechanism whose FEED is
deferred, with data.

**The measurement.**  The only filter state that survives both the between-satellite
and the between-epoch differencing is ZWD.  Median |ΔZWD| per epoch against the
reference gate's own fire count over a cold start:

| ep | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| med \|ΔZWD\| mm | 539 | 471 | 431 | 466 | 438 | 619 | 279 | 151 | 98 | 126 | 82 | **62** |
| refsat fires | 116 | 114 | 113 | 112 | 110 | 106 | 99 | 88 | 70 | 43 | 25 | **0** |

They die together.  And the mechanism does not stop at ignition: in the settled era
(station-scoped, baseline P(fire) 0.862 %) the fire rate rises monotonically with
|ΔZWD|, from 0.55 % in the quietest decile to **1.77 % in the top decile — 2.05×
baseline, 3.2× across the deciles**.  This also corrects the header's claim that the
detector is "independent of Kalman filter state": the OBSERVABLE is, but
`phi_if_omc` subtracts `obs.modeled`, and the burst is that model settling.

By contrast, PRODUCER-side model events do NOT drive it — station-scoped lifts are
`anchor_handover` 0.31×, `handover_continuous` 0.35×, `handover_cold` 0.00×,
`attrib_arc` 0.00×, `mw_wl_slip` 0.00×, all at or BELOW baseline.  "Any disturbance"
is the wrong generalization; the troposphere is the specific one.

**THE LEAK (landed).**  A gated epoch suppressed this detector's own flagging but
still published its jumps as `valid`, and the pool arm admits any sample with
`jp.valid` — so the pool went on thresholding double differences the detector had
just declared untrustworthy.  Gating has to reach both arms or it protects neither.
`info.valid` now carries `&& !pos_gated`; jumps stay cached and the gradient chain
still advances, so detection resumes seamlessly.  A second, latent half of the same
defect: `processDoubleDifferences` re-derived `pos_gated` from `position_sigma_3d`
alone, so any new gating arm would have been silently excluded from the reference
gate.  Both now read one member, `epoch_gated_`, set once in `detect()`.

**The gate (landed) and the FEED (deferred).**  `detect()` takes a
`model_nuisance_m` and `setNuisanceGate(max_m)` arms the threshold; 0 disables it.
There is deliberately NO config knob yet, because there is nothing to feed it:

- `st.pstate.troposphere_zwd` is set to 0.0 at setup and never updated on the
  network path — a dead field there.
- The live ZWD is reconstructed in the serial POST-SOLVE diagnostic sweep
  (`NetworkIonoDiag`), behind the `trace_zwd` gate.  Feeding the detector from it
  would mean either gating a QC decision on a trace knob — which this codebase has
  an explicit rule against — or hoisting the reconstruction into the hot path.
- The detector runs in PASS-A, BEFORE the solve, so the feed is one-epoch-lagged by
  construction.  (Defensible — ΔZWD is smooth on both the ignition and the weather
  timescale — but it is a real property of the design, not an implementation detail.)
- Decisively: items 1 and 2 are predicted to leave it no target.  Shipping a config
  knob for a mechanism with no demonstrated work to do is exactly the armed-but-inert
  failure this codebase keeps paying for (standing invariant #3: triage the deferral
  with DATA).  The pool arm it would also protect has 0 flags in 2,754,791 samples.

So: mechanism complete and unit-tested, feed deferred until the §6.14.6.4 bench can
show it a target.  If the armed A/B below leaves a residue that tracks |ΔZWD|, the
feed is the next increment and the harvest point is named above.

### 6.14.6.4 Item 4 — the bench mostly EXISTS; the gap was elevation

Item 4 was filed as "needs a bench that doesn't exist yet".  That is not right, and
the correction matters because it moves the (1,1) floor decision from blocked to
one option away.  `scripts/simnet_inject_arms.py` already has:

- a magnitude ladder defaulting to ±1, 2, 3, 5, 10, 50 cycles — the sub-2-cycle
  rungs the floor decision needs are already there; and
- `--signal-mode mw-blind`, which puts the SAME delta on both bands of a pair —
  which is exactly the (n,n) class MW is structurally blind to and the only class
  where IF-TD has unique reach.

The one thing it could not express is a slip at HIGH elevation.  The random slip
model is capped at `max_elev_deg = 45` while the reference sits at p50 61.7° (§6.14.5:
zero overlap, so a reference-satellite slip is unreachable by construction) — and
the planner emitted only form-A (exact-epoch) events, which need an elevation the
visibility oracle does not carry.  `ScriptedSlip` has supported form B (an elevation
WINDOW, `elev_lo_deg` / `elev_hi_deg` / `not_before_epoch`) all along; the planner
just never emitted it.  Added: `--elev-window LO HI`.  The planner still verifies the
station actually TRACKS the (sat, signal) and still places the event inside the
requested arc window (the chosen epoch becomes `not_before_epoch`), but hands the
firing decision to the simulator's own elevation trigger.

⚠ Form B fires on the FIRST visit to the window, and an unfired scripted slip is a
run-end failure — so verify every event fired and that the fixture's arcs reach that
elevation.  The runner prints the warning.

**PRE-REGISTRATION for the floor_frac card (write this down before running it).**
The decision is whether `floor_frac = 1.0` — the physical floor sitting exactly ON
the (1,1) jump with a strict `>` — should come down.  Measured constraints going in:
the floor binds on only 5.21 % of rows and NEVER below 30°, so on 94.8 % of samples
the empirical arm is already the binding constraint and lowering the floor changes
nothing there; the historical field sweep gave floor_frac 0.5 → 45 flags, 1.0 → 26,
1.5 → 24.  PASS = on a `--signal-mode mw-blind --elev-window 60 90` fault arm, the
(1,1) and (2,2) rungs are detected at a materially higher rate than at
`floor_frac = 1.0`, with the false-alarm count on the matched NULL arm unchanged
(0 pool flags, and the reference gate at its §6.14.6 armed level).  FAIL = any
false-alarm increase on the null arm; the (1,1) rung is not worth buying with one.

### 6.14.6.5 The three-arm A/B — item 1 confirmed, item 2 COSTS on this bed

`config/test_suite/_ifg_{off,w,we}.json`, each differing from
`golden_sim_tri_w6` only by the run paths, a 2 h window, and its own knob; same
exe (`gnss_processor_ifg.exe`, md5 `2456cdfa…`); sequential, SOURCE-mtime capture
assertions, all three "capture: all fresh".

| | ifg_off | ifg_w (+abstain) | ifg_we (+abstain+elev_norm) |
|---|---|---|---|
| refsat ACTING refloats | 0 | 0 | 0 |
| refsat diagnostic | **118** | **0** | **2** |
| fires ep 0–11 / ep ≥ 12 | 118 / 0 | 0 / 0 | 2 / 0 |
| tier T1r opp / flags | 28680 / 118 | 28680 / 0 | 28680 / 2 |
| tier T1 (pool) opp / flags | 228634 / 0 | 228752 / 0 | 228750 / 0 |
| MW/WL events | 142 | 142 | 142 |
| total events logged | 509 | 391 | 393 |

Run-end counters (candidates → fired; suppressed by scale/coherence, thin, cap, COLD):

- `ifg_off` 1090 → **118**; 948, 13, 11, 0
- `ifg_w`  1111 → **0**;   818,  9,  0, **284**
- `ifg_we` 1111 → **2**;   816,  9,  0, **284**

**OFF control byte-identical** to `runs/ifsc_new` (pre-change exe, same config) on
`network_solution.csv`, `network_events.csv`, `network_flag_rates.csv` and the
31 MB `network_if_slip.csv` — the new code is inert with the knobs off.

The candidate count moving 1090 → 1111 is explained, not noise: in `ifg_off` the
118 fires each reset a reference arc and perturbed the stream behind them, so
abstaining leaves more families intact to be screened.  Same for T1's opportunity
count (228634 → 228752): fewer reference resets means more satellites retained in
the pool.

**Item 1 is confirmed, and its win is smaller than it looks.**  118 → 0, with 284
abstentions naming exactly what was given up, MW and the pool arm untouched, and
zero change to the acting path.  But the acting path was ALREADY zero under Card R
(§6.14.4) — on this posture every fire is cold-era `diagnostic`.  So what item 1
removes is 118 unactionable events, i.e. event-stream pollution and a principled
cold posture, NOT any refloat damage.  Worth having; not worth overselling.

**Item 2 is confirmed in DIRECTION and it is a net cost here.**  The pre-registered
prediction (§6.14.6.2: bar p50 0.78× raw, more sensitive, difference at the geometry
extremes not uniform) is exactly what happened — `elev_norm` ADDED 2 fires, both in
the burst window, on a bed with zero slips.  On a truth bed with nothing to detect,
extra sensitivity is pure false-alarm cost.  Its case remains that the bar tracks the
statistic it is testing rather than a proxy for it, and that case can only be settled
on the detection side, which this bed cannot measure.  **It is therefore NOT
recommended for arming until the §6.14.6.4 fault arm runs** — the honest verdict is
"more correct, measurably more expensive, benefit unmeasured".

### 6.14.6.6 What is NOT validated here

**Detection power is not validated by any of this, and cannot be on this bed.**  The
quiet fixture injects 0 slips, so every arm here measures FALSE-ALARM behaviour only.
Items 1 and 2 both make the gate *less* likely to fire; on a zero-slip bed that is
unfalsifiable as a quality claim — a gate that never fires scores perfectly.  What
would falsify it is the §6.14.6.4 fault arm, and until that runs neither knob has
evidence that it preserves a real detection.  Both are filed UNVALIDATED, and the
goldens are deliberately NOT changed.

**Item 2's stated motivation was partly already discharged.**  The 25× reference-
handover lift (12.26 % vs 0.487 %) was measured with the gate OFF, i.e. against the
legacy FIXED bar — so it cannot have been caused by a stale empirical scale, because
on that path there is no empirical scale.  It is the handover rebase's own residual,
and §6.14.4 shows Card R already absorbs it (0 steady-state fires, 11 reselects).
So elev_norm is not buying back a measured residue here; its case is that the bar
should track the geometry it actually depends on — which matters most where the
el_ref distribution differs from this bed's, i.e. the field.  Recorded so nobody
re-derives the 25× as evidence FOR this knob.

**Also out of scope:** sim-only, no field arm; a 2 h window, which is inside the
level channel's convergence budget and so supports counting fires but no wire-quality
claim; and the nuisance gate has no feed (§6.14.6.3).

## §7 Open questions (re-cut 2026-09-04, round 2; round-4 closures and items 15–21, §6.8)

**⚠ FIRST (2026-09-05, §6.9): the RE-COUPLING EDGE on the F3 block at satellite re-admission — BUILT the same day as `network.readmit_seed_horizon_s` + `network.readmit_coupling_marg_ep` (§6.10, pre-registered; arms running)** — the 24 h arm shows two ~4 h NETWORK-WIDE degradation episodes after the eclipse re-admission clusters (clean chords 0.19 → 0.58–0.68 cyc), absent from the quiet hours where F3 sits at the CRLB.  Build: at a satellite's re-admission (col_added after a gap ≥ N epochs / CTA first sighting), marginalize its shared terms out of every station's block regressors for K epochs (or register the re-admission as a Card-1g reinit source); counters at both edges; pre-registered PASS = no clean-chord degradation after the h16.6–18.9 cluster on `_nonint_losblock24h_hv`, settled quarter ≤ 0.2 cyc / ≥ 80 %.

1. ~~**The birth value.**~~ **CLOSED (§2.7)** — age 0 IS the free single-epoch absolute-code
   solve at the birth elevation (med|r0| tracks 0.6745·σ_P/(sin el·√Σμ²) within 15 % in every
   elevation bin; corr with the absolute-code proxy 0.62); the round-1 1.49 m / corr 0.58 was a
   tail-dominated 6-station rms.  Residual sub-item: ~30–40 % of the age-0 level not explained
   by the LOS's own 2-band proxy — station-common per-epoch rx-DCB code noise vs proxy
   attenuation (a 3-band proxy needs the L1C/L5Q pair rows, absent from `network_slip_raw.csv`).
2. ~~**Birth-spread dependence.**~~ **CLOSED (§3.1a; criterion measured §6.8.1)** — birth-frame
   inheritance (station-term slope 1.18); the DD-level-vs-birth-spread criterion reads flat under
   F1 (0.299 / 0.269 / 0.214 m) and F3 (0.070 / 0.047 / 0.052 m, slope −0.16 / −0.02 / −0.03) — PASS.
3. **L7Q** — the median OFFSET is CLOSED (§6.8): +1.01 (base) → −0.115 (F1) → **+0.017 / +0.009**
   (F3 / F3+hv), every band's settled med(e) +0.004 … +0.017 — it was the station-common frozen
   term.  What survives is a satellite-arc-mean-FRAME term in L7Q's iono part (M_t slope
   +2.7 … +6.4, R² 0.46–0.58 on 454 mid-run rows) invisible in its CRLB ratio (0.92); candidate
   the Galileo E5b satellite code bias (wire E:C7Q residual 0.53 m rms on every F arm).  Unquantified.
4. **RAIM pool admission of block rows** (`NetworkRaim.cpp:236` admits any row with |A_r| > 0.5,
   no `cpl_row_start` exclusion).  Round 3: T4 erases 288 → 0 under F1 (the coupling row's own
   standardized residual shrinks 1/√B_eff).  Round 4 (§6.8.4): under F3 the POST-FIT pool census
   is unchanged (n_pf_rows identical on all five arms) but the RAIM-attrib pool σ̂0 goes
   0.233 → 0.991 with |effect| p50 ARC 0.454 → 1.463 (max 4.07 < k = 6, 0 acted) and one ARC row
   at prominence 4.0e9 (a near-zero denominator) — the dense L⁻¹ rows sit in the T3/T4 pool at
   unit variance, admitted arbitrarily by the 0.5 test.  Still owed: tag block rows prior-class
   and exclude them (or grade the prior residual as its own tier), a per-column pool census so
   admission is READ not inferred, and the 4e9 denominator fix.
5. ~~**IF-TD reference gate: 1917 false refloats/day**~~ **CLOSED (§6.14, 2026-09-08)** —
   100 % false (the bed injects 0 slips), and the statistic is the reference satellite's own
   phase noise: σ 17.7 mm, FLAT in family size, 88 % station-local, against a fixed
   0.5·λ_IF = 53.48 mm bar = **3.02 σ**.  A fixture-realism artifact — the sim is 5×
   noisier than a real 300-station trace at matched elevation, where the same bar is ≈15 σ
   (hence 14 field flags vs 2482/day here).  The owed 2 h arm ran: Card R
   (`network.cycle_slip.if_phase_td_ref_gate`, armed in BOTH goldens) takes acting refloats
   **546 → 0** and the steady-state rate **0.504 → 0.000/epoch**, MW and the pool arm
   untouched, σ̂₀ unchanged.  The `nonint_*` ladder simply ran it OFF.  Of the two items filed
   in §6.14.5, **both are now FIXED (§6.14.6, 2026-09-08)** — the missing flag-rate tier (now
   `T1r`: 28 680 opp / 118 flags where T1 read 0, cross-checked against 118 `refsat_reset`
   events) and the reference satellite's structural zero deflating the pool scale (s_eff p50
   +15.3 %, solve byte-identical).  What stays OPEN is the physics: the pool floor still sits ON
   the (1,1) boundary, so the one MW-blind slip class is unreachable — and the sim bench cannot
   test a reference-satellite slip at all (zero elevation overlap).
6. **Run-level warmup** (`carry_warm = n_emitted ≥ 6`, `ApplicationNetwork.cpp:1450-1452`) —
   moot under F1 as predicted; the ignition cohort remains its own regime (item 8).
7. **What bounds the formal σ at 0.009–0.013 cyc?**  Round 2: the collapse has no fixed point
   for B ≥ 2; the 24 h value needs an additive ~1.5e-6 m²/epoch, i.e. the Card-1d
   `tu_var = M²·κ·q·Δt` with P_prev ~1e-6 m² (inferred — `cov_df(j,j)` is not traced).  A
   one-station dump of (M_clk, κ, tu_var) per row would pin it.
8. **Ignition cohort — now the LARGEST F3 residue** (§6.8.2).  Under F3 the per-chord clean
   first-inject reads 0.715 / 0.731 / 0.761 by elevation (the station-common share collapses at
   the honest rate) — but legs born at k = 0 with NO logged event (no cold-handover hit, no
   T1/T0 refloat) sit at 1.25–1.34× the population CRLB (0.843 F3 / 0.904 hv, n = 2085 / 5936)
   where mid-run-born legs sit AT it (0.72 / 0.64), and the T1/T0 history moves nothing (± 0.02).
   Candidates: the k ≤ 6 warmup solves before the first block exists (blocks start at k = 7),
   the k = 7 first-inject where rx-CB / clock / ZWD collapsed before any block, a frozen
   ignition-era level in the still-diagonal rx-CB carry.  Recipe: per-leg r(t) vs CRLB along the
   arc for k = 0 vs mid-run legs (`r4resid/arms/*.npz` carry everything needed) — fixed at k ≤ 7
   ⇒ a warmup / first-inject card; accruing ⇒ a carry-path card.  Never quote an ignition-epoch
   ratio as a B test.
9. **Gap re-births** (one effective prior row at the first re-inject, ratio 0.72–0.75, 429 arcs)
   — DEPRIORITIZED: under F3 rows with a re-birth leg sit BELOW the population bound (0.78 /
   0.66), so the edge is not measurable above the CRLB on this window.  The harvest / prune
   ordering read (`NetworkRunContext.cpp:3476-3486`, `NetworkSlipQc` post-gap) stays a code question.
10. ~~**F1's L1C first-inject 0.588 vs 0.707**~~ **CLOSED (§6.8.1)** — F3 reads 0.720 / 0.722 /
    0.728 / 0.722 on L1C / L2W / L5Q / L7Q (predicted ≈ 0.707): the share IS station-local.
11. **vf005 < vf001 in the iono part (1.48 vs 1.81 cyc)** — non-monotone in v_f, contrary to
    any smoother model; not pursued (the knob is falsified as a fix), recorded so nobody
    re-derives the floor.
12. ~~**Do `carry_zwd` and the rx-CB carry share the defect**~~ **RE-CUT into item 17.**  The
    ZWD is not carried on this posture (carried flag 0 / 21,600 rows), so F3's marginal ambiguity
    block is exact for it — measured honest (|err|/σ 0.94, §6.8.4); the rx-CB carry IS separate
    and diagonal — item 17.
13. **SPEC_synthetic §13.7's settled float 8–9.4 cyc (WORSE than single-epoch, K_eff < 1)** vs
    today's 2.7–3.8: did that arm run without `carry_coupling_time_update` (the SPEC_card1 §3
    poisoned-row signature)?  Unverified.
14. ~~**Wire grade of F1**~~ **CLOSED (§6.8.4)** — F1 and F3 wire + consumer grades landed on all
    five arms; the ledger rows carry the verdicts.
15. **The handover ceiling must be re-expressed on the honest-σ scale before F1 or F3 is armed
    on ANY lineage** (§6.8.3).  **BUILT 2026-09-05 as `network.phase_datum_handover_max_var_ratio` (§6.10, `runs/ratio3h` pre-registered).**  `network.phase_datum_handover_max_var_m2` = 1e-4 m² was sized to
    the over-counted σ: under any honest-σ carry 0/93 successors are continuous, 7 settled 4–15 m
    PB frame steps land on the wire, 1653 leveling carries are erased.  **Measured at 1e-2 m²:**
    F3 70/93 (all 23 refusals are arcs ≤ 52 min old in h0/h1; settled window 21/21 continuous,
    0 settled steps > 1 m, 408 erased), F1 67/93 (per-BAND refusals — L5Q 12 / L2W 7 / L7Q 4 /
    L1C 3 on arcs up to 3 h old, 9 mixed families, 4 settled > 1 m steps, consumer +0.43 m up
    bias — an INVALID pairing); neither reaches ≥ 90/93.  Candidates: a relative form
    (var ≤ k²·var_at_first_inject) or an age gate; either must keep the 378-σ unvalidated-target
    guard (SPEC_pivot_visibility_gauge_defect §5.0) demonstrably armed (inject an unvalidated
    target on the sim bed).  Prerequisite: trace the successor's conditional var and age per
    handover — `network_events.csv` carries the target only.  Interim: 1e-2 is the WORKING value
    for F3 arms, not a promotion.
16. **F3 telemetry gaps** (§6.8.5): no per-epoch breakdown of the marginalized-at-inject columns
    by reason (departed / skipped / freed / pinned — 8,534 lumped in the run-end line); the
    within-epoch RAIM refloat re-whiten path never exercised (T2 354 opportunities / 0 flags,
    uncertified on data); add both to `network_epoch_status.csv` before the field arm.
17. **rx-CB direction 2× over-count** (r3_5 design flag, now the named residue): the F3 block
    covers the ambiguity columns only while the rx-CB keeps its own diagonal carry, so the DCB
    direction is asserted twice (2×, not 25×).  DD-invisible above the bound at 3 h (mid-run rows
    AT the CRLB) and hinted only by the rx-DCB formal 0.089 (F1) → 0.067 (F3) = base's 0.066 m;
    the DCB truth join is definition-limited (0.85 m rms on every arm, corr 0.30–0.36 GPS, ~0
    Galileo) — a state-definition-aware grade is needed before calling it inert.  Fix = block ALL
    carried local columns of the station.
18. **F3's satellite clock lags F1** (settled rms 1.583 vs 1.437 m, p50 1.011 vs 0.832; G16 4.23 vs
    3.49, G09 2.78 vs 2.10, G18 1.88 vs 1.35) with a base-like URA (0.005 m, vf 1.1e5; F1 0.017 m)
    while every station-common channel is equal or better.  HYPOTHESIS: the exact stiff block
    re-asserts the carried ambiguities' retired-frame memory through the coupling rows that F1's
    blunt 3× loosening released.  Test: a3 / a5 + a per-satellite clock re-realization census keyed
    on the eclipse-readmission seam, on `nonint_losblock3h_hv`.
19. **Consumer east walk from the G12 re-emission epoch** (~267930): blk_hv E −0.15 → −0.32 m over
    the last 40 min with ZERO settled cold handovers, wire flat, n_sats constant — the round-3
    cold-handover explanation is FALSIFIED, the G12 half untested.  Test: replay from 267330 with and
    without G12, or a ≥ 6 h arm; grade the WIRE first.  Also open: los3h_hv's +0.43 m up bias (F1 +
    1e-2 only; the same families are cold on los3h with no bias); the consumer emits no reset /
    discontinuity counters on these arms.
20. **24 h F3 + hv arm owed** — pre-registered §6.8.6 (settled L1C 0.14–0.15 cyc / 84–86 %, WL
    ≥ 99 %, EWL 100 %, ZWD |err|/σ ~1; falsifiers > 0.20 cyc or < 80 %, |e| rising with age
    beyond 360, ZWD |err|/σ > 1.5); grade with `simnet_amb_integer.py` + `r4resid/crlb_pop.py` on
    the last 25 %, where 99.5 % of rows are mid-run-born.
21. **Field arm owed (FLD63 / FT63 lineage) with the same instruments** — F3 is sim-validated only.
    No truth there: grade on DD-fractional circR / med|f|, ZWD vs an independent ZTD, the PB-gauge
    census + seam-tools steps, the prires / block-Baarda / congruence σ̂0 families and
    `trace_prefit_census` (arm them on the next `_nonint` arms too — UNAVAILABLE on all five), and a
    consumer replay.  Fresh convergence (fingerprinted), wu93 side of the wind-up fix, item 15 first.

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
