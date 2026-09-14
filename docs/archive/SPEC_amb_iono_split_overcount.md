<!-- FROZEN / READ-ONLY: the pre-split monolith, kept byte-for-byte. Never append here — new results go in the card file (see docs/INDEX.md). -->
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
the ZWD as the fourth member.  ⚠ Read the (+1, −1) correctly (peer): it is the band-1 phase equation, which
the traced states MUST satisfy if they are what the solve fit to 1.6 cm — so it is not evidence for the
near-null direction (that would be circular); what it IS is the RECONSTRUCTION CHECK marked OWED above, done
in aggregate and PASSED: the traced `x_lin + x_r` pairs satisfy the observation model, branch (b) is closed
EMPIRICALLY (not only by source read), and the 0.75 m gap was an artifact of omitting T.  The evidence for
the near-null direction is the COVARIANCE STRUCTURE: three states that should be small and near-independent
are metre-class (ΔI 1.25 m, ΔT 1.22 m, A ~10 cyc), strongly mutually correlated (tropo↔iono −0.85), and
carry a σ that does not know it (40σ wander).  On that evidence the cycle-common channel is the projection
onto the ambiguity block of **(I, N₁, N₂, ZWD) moving together along a weakly-determined direction of the
shipped field posture** — per-LOS iono (SH-pinned), per-epoch un-carried ZWD (a metre-class sink), and per-station
ambiguities trade against each other while the phase fit stays at 1.6 cm.  The DD "ambiguity" of this
posture is therefore not an estimable-invariant quantity in practice; its 10-cycle-per-hour wander is the
tropo–iono trade riding through it.  On the sim golden the ZWD is CARRIED (honest σ, F3-whitened) and e₁ does
not follow I (slope 0.001) — the same estimator with the fourth member pinned.  Fix direction (not built
here): pin the fourth member — carry the ZWD on the field vehicle (B9 `zwd` carry with a physical q) — and
re-read the DD wander; a single-knob arm on the SHIPPED vehicle, independent of F3.
**Arm 0z `fldf3_ci_zwd` — PRE-REGISTERED 18:40 (queued behind the probes; smoke first):** the control +
`network.carry_zwd: true` (B9 per-station ZWD random-walk carry at its default q; the chain never set it —
`carried` = 0 on every row of every arm so far, and the sim golden does not carry it either but has a FREE
per-LOS iono and F3, which is why its per-epoch ZWD is honest at 9 mm).  Smoke gate: `network_zwd.csv`
`carried` = 1 on warm epochs, else the carry is inert on this vehicle (the inject sits under `carry_warm`)
and the arm is not run.  Predictions if it injects: ZWD per-station settled span < 0.5 m with an honest σ
(≥ 1 cm); DD-ambiguity arc wander e₁ MAD from 10.2 cyc to ≤ 2; the A-on-iono co-motion slope from +1.7 toward
0 (the tropo–iono trade loses its free member); DD fractions NOT predicted to leave the null; post-fit phase
σ̂₀ unchanged (~1.6 cm).  Falsifier: ZWD pinned but the DD wander and the co-motion unchanged ⇒ the ZWD was
a passenger, not the free member, and the weakly-determined direction has another member.
**RESULT (19:00, `runs/fldf3_ci_zwd`, 12 h, exit 0):** the smoke gate PASSED — the B9 carry injects on the
shipped vehicle (`carried` = 1 on 2,012 of 2,439 smoke rows; the peer's "inert" reading was wrong, as the
golden's `phase_leveling: true` predicted) — and the 12 h arm DIVERGED, a third outcome neither branch
named: 1,322 solved of 1,440 with **118 S-basis withholds on the SHIPPED vehicle** (the control had 0),
ZWD per-station span **54 m** (control 21 — pinning made the sink worse, σ 0.057), prires worst |t| 2.7e7 on
the ZWD prior itself, DD wander e1 MAD **39 cyc** (control 10), DD-iono MAD 3.8 m (control 1.25), and an
in-process consumer that left the building (h4 220 m).  The joint fit still reads the band-1 equation
(a +1.00, t −0.99) — the traces stay faithful while the solution goes off the rails.  Reading: the ZWD IS a
member of the weakly-determined direction — constraining it with a diagonal random-walk prior does not pin
the direction, it moves the tension onto the other members and closes the declared null direction (the same
gate the leveling arms trip); and a diagonal ZWD carry is §3's over-count in its purest form (a station-common
direction asserted n_LOS-fold per epoch, σ collapsing onto a metre-class first estimate — the sim's "ZWD
formal 0.005 m vs 0.35 m actual" of §6.8.4, on real data).  Standing invariant #1 applies as the peer warned:
a stiff time-linked ZWD with no wired response.  And it closes the loop with probe 4: the ledger's
`carry_conditional_coupling` row records that the Card-1 coupling rows fold NOTHING under carry_invariant unless
`carry_zwd` is on — arming the ZWD carry made those rows LIVE on the shipped vehicle, and the S-basis gate
tripped 118 times on a vehicle that never trips it: the conditional coupling rows are what close the declared
null direction on this chain, on both vehicles.  **`network.carry_zwd` on the shipped field vehicle: KEEP OFF
(12 h: divergence).**  The fourth member is real; the fix for it is not a carry, it is whatever makes the
(I, N, ZWD) direction observable per epoch (F3-class whitening of the station block INCLUDING the ZWD column,
or an honest external ZTD) — a design card, not a knob.

**Arm 2 `fldf3_on` — landed 18:25 (exit 0; 956 solved epochs of 1,440: 223 `gate_withheld` by the same S-basis
null-space gate from the SAME onset epoch 101, plus 261 gate-ok-but-unsolved; withholds persist all day, h4–h11
49–75/h; D0 median 388 ms vs arm 1's 289 = +34 %, overlapped).**  P1 PASSES — F3 fires and stays conditioned
all day: 86,344 blocks whitened, 991,230 rows, 60,458 columns marginalized at inject, 2,813 pairs zeroed
(harvest-stamp mismatch), **0 refused, 0 factorization fallbacks**, block dims 10–24 (max 21), within-epoch
refloat re-whiten 9,924 ok / 0 failed.  But the vehicle does not converge under it either: ZWD per-station
settled span median **55 m** (arm 1 120, control 21), σ 0.043 m (over-confident); prires worst |t| 1.5e8; the live
pool refloats continuously — T2 646/950 (68 %), T3 **80,222/403,728 (19.9 %)**, T3c 12,935/24,832 (52 %) — worse
than arm 1's 9 % (the §7 item 4 population: F3's dense L⁻¹ rows admitted to the pool at unit variance);
T4a 10,601 → 3,618 (better).  **P2 FALSIFIED — at the null on every signal:** L1C as-traced med|f| 0.246
(null 0.247), R 0.458 (0.466), gap +0.009; L2W R 0.442 (0.449); WL12 (24,269 paired chords) R 0.485 (0.476),
near 19.8 %, gap +0.003 — the WL null bounds the residual at ≳ 3 L1 cycles, on a NON-converged arm (the
pre-registered ambiguity).  Arc stability: e1/e2 MAD **6.95 / 6.97 cyc** (control 10.2, arm 1 24.2), WL 0.63,
cross-band slope **1.003** (p10 0.83, p90 1.19, corr 0.998) — cycle-common again; ref-free 2.98 cyc (gap −0.19).
Iono leg: DD-iono MAD **0.77 m** (control 1.25, arm 1 3.7), single-LOS 0.82; A on iono +1.40 (corr 0.83), B +0.20
(corr 0.49), baseline split 0.35 → 0.87 m with A's slope flat +1.3…+1.7 — the same (I, N, ZWD) trade as the
control, at ~60 % of its amplitude.  In-process AR consumer settled 7.9 m (pathology).

**Float-only cold-consumer replays (WTZZ00DEU hold-out, truth position, settled ≥ h6, 3D rms / p50):**
control wire **6.56 / 5.83 m**, arm 1 wire 4.37 / 2.13, arm 2 wire **2.90 / 1.62** — P5 PASSES (arm 2 ≤ arm 1)
and the shipped control's wire is the WORST of the three for a float consumer, i.e. every field wire here is
metre-class (the sim's cold-on-continuous floor is 0.06 m) and the (I, N, ZWD) trade is priced into the products.

**VERDICT of the ladder (18:35).**  (1) The phase-leveling vehicle does NOT converge on the FLD63/EXT63 field
chain: both leveling arms trip the NetworkSbasis null-space gate ("observed 0 (near-)zero eigenvalues" vs
declared 1) from epoch 101 onward and carry a 10²-m ZWD sink — a datum/rank failure of a posture (eq34 chain
datum + SBC + SH-pinned iono + leveling) that was never validated anywhere; the sim golden's leveling vehicle
runs the prioritized pivot phase datum with continuous handover.  (2) F3 itself is sound on the field — fires,
conditioned, 0 fallbacks — and MITIGATES the vehicle's divergence on every scale-bearing instrument (ZWD sink
120 → 55 m, DD-iono 3.7 → 0.77 m, DD-ambiguity wander 24 → 7 cyc, T4a 10.6k → 3.6k, float consumer 4.4 → 2.9 m)
but does not cure it, and its pre-registered headline — lifting the field DD fractions off uniform — is
FALSIFIED on this chain (at the null on L1C, L2W and WL12), with the confound that the arm it was read on
never converged.  (3) The field control's DD "ambiguities" are not estimable-invariant in practice: (I, N₁, N₂,
ZWD) co-move along a weakly-determined direction of the shipped posture (§6.15.3a item 5), and that structure is
present on all three arms.  Ledger: `phase_leveling_local_block` stays RECOMMEND_SCOPED on its sim regime, with
a FIELD entry "KEEP OFF on the field until a leveling posture converges there" — item 21 is re-cut from "does F3
help the field" to "make the leveling vehicle converge on the field (datum declaration), then re-run F3"; the
pre-registered probes `_fldf3_pl_pdp3h` (sim golden's phase-datum family) and `_fldf3_pl_nosbc3h` (SBC off) and
the shipped-vehicle arm `_fldf3_ci_zwd` (pin the fourth member) are queued behind this ladder.

**Two record notes (peer review, 18:45).**  (1) *The tension in the KEEP OFF.*  The F3 arm gave the BEST
consumer wire of the three (float cold consumer 2.9 m vs the shipped control's 6.6 m) and the row still says
KEEP OFF: the verdict is decided on PRODUCER-INTERNAL convergence — 223 withheld epochs, a 55 m ZWD sink,
prires |t| 1.5e8, a 20 % T3 rate — because a producer that withholds a sixth of its epochs is not shippable
whatever its emitted epochs do for a consumer; the consumer replay moves the other way and both facts are
real.  (2) *"Every field wire metre-class" retires the DD-uniformity question as an independent mystery.*
6.6 m of float-consumer rms is ~35 L1 cycles, and the producer's own DD arc wander is ~10 cycles (1.9 m) per
hour; DD errors at that scale wrap to uniform NECESSARILY, so the at-null L1C/L2W/WL12 fractions on every arm
are what a wire of that quality must produce, not a separate fact about integer content.  Three numbers, one
story: wire 6.6 m, DD wander 10 cyc, fractions at the null.  It also validates the on-the-fly WL instrument:
its truth-free lower bound (|e₁| ≳ 3.5 cyc from the WL null) sits below the ~10–35 the other instruments
read — consistent and conservative, as a lower bound should be.  **Scoping consequence for whatever follows
item 21:** the integer question cannot be answered on this bed while the wire is metre-class — no refinement
of a wrapped-fraction instrument reaches it, the errors are two orders above the regime where the statistic
carries information — so getting the DOY-129 field wire to sub-decimetre (the (I, N, ZWD) trade and the
leveling-vehicle datum are the two named levers) is UPSTREAM of every integer claim, and outranks the F3
question that started this section.

**Divergence probes (3 h, same exe; pre-registered; grading: S-basis withhold count, h1–h3 ZWD span, prires
|t|, T2/T3/T4 rates, arc stability at settle 1 h).**  Probe 1 `_fldf3_pl_pdp3h` — the sim golden's phase-datum
FAMILY (prioritized pivot datum + continuous handover + anchor carry re-ref + min_stations 2) instead of the
chain's eq34: **still withholds — 84 of 360 epochs, same onset TOW 262230 ("observed 0 (near-)zero" ×68,
"observed 2" ×16), prires |t| 1.5e8**, h1–h3 ZWD span p50 1.7 m (σ 0.010).  The phase-datum family is
EXONERATED as the cause of the WITHHOLDS — but not of the wander: on the same 3 h window the phase-datum
family swap takes the DD arc wander from arm 1's e1 MAD 24.2 cyc / WL 4.7 (491 arcs) to **0.068 / 0.138 cyc on
the 45 arcs that survive** (a selection effect is possible — far fewer chords reach 30 epochs) and the h1–h3
ZWD span from 14.4 m to 1.7 m, so the eq34 chain datum + leveling pairing owns most of arm 1's wander even
though the withholds are unchanged.  Probe 2 `_fldf3_pl_nosbc3h` (`sbasis_complete` off): **identical to arm
1's first 3 h to the printed digit** (82 withheld, ZWD span 14.425 m, σ 0.0153) — SBC never engaged in that
window (arm 1 logged 15 engagements over 12 h) and is not the cause.  Queued (18:50, pre-registered the same way, one family each): probe 3 `_fldf3_pl_nosh3h` —
`estimate_iono_sh: false`, a FREE per-LOS iono as on the sim (the leveling vehicle was only ever validated
with free iono; the SH pin plus absolute leveling priors may close the declared null direction); probe 4
`_fldf3_pl_nocpl3h` — the Card-1 conditional coupling rows off (leveling rows as plain diagonal priors);
probe 5 `_fldf3_pl_vf3h` — `phase_leveling_var_floor_m2: 0.01` (a GAUGE probe: if over-stiff absolute
ambiguity priors are what pins the declared null direction, loosening them should let the gate see it).
Working hypothesis being tested, not asserted: the leveling carry injects ABSOLUTE per-station ambiguity
priors, which are not datum-invariant, so with the chain's declared null-space dimension of 1 they can pin
the last gauge direction (observed 0 ⇒ withhold) — a declaration defect of the leveling posture on this
chain rather than a numerical one.  The ZWD-carry arm (`_fldf3_ci_zwd`) is re-queued behind these probes.
**Probes 3–5 (read 18:50):** probe 3 (SH iono OFF) — 82 withheld, same gate, ZWD span 12 m: SH exonerated.
Probe 5 (leveling var floor 0.01 m²) — 82 withheld, ZWD 10.5 m, prires |t| 6e5 (from 1e8): stiffness is not
the cause.  **Probe 4 (Card-1 conditional coupling OFF): the "observed 0" withholds VANISH (0 of 360)** — and
96 epochs fail to solve instead (gate ok, unsolved), ZWD 15.9 m, 6 surviving arcs: the CONDITIONAL COUPLING
ROWS are what closes the declared null direction on this chain, and without them the leveling vehicle cannot
solve at all.  The sim golden runs the same rows without tripping the gate — and it arms
`network.carry_coupling_gauge_project: true` (project the coupling regressors out of the gauge directions),
which the field chain does not.  **Probe 6 `_fldf3_pl_gp3h` PRE-REGISTERED (18:55): arm 1 +
`carry_coupling_gauge_project: true` — withholds 82 → ~0 with the solve intact (no unsolved epochs beyond arm
1's one), h1–h3 ZWD span below 14.4 m; persisting withholds ⇒ the rows pin the gauge through a channel the
projection does not cover.**
**Probe 6 read (19:03): the withholds are GONE — 0 of 360 (arm 1: 82), 350 solved + 10 unsolved — so the
Card-1 coupling rows' datum-gauge projection (Card 1f, "each (constellation, kind) coefficient sum of a
truncated row held at its harvest value") is exactly what keeps those rows off the declared null direction,
as on the sim golden; but the vehicle still DIVERGES on this chain: h1–h3 ZWD span p50 21.6 m (arm 1 14.4),
prires |t| 6e7, DD wander e1 MAD 50 cyc (14 arcs).  Probe 1 had the mirror image (wander 24 → 0.07 cyc and ZWD
1.7 m with the withholds unchanged).  **Probe 7 `_fldf3_pl_gppdp3h` PRE-REGISTERED (19:05): both together —
gauge projection + the sim golden's phase-datum family (prioritized pivot datum, continuous handover, anchor
carry re-ref, min_stations 2) = the sim golden's leveling posture transplanted onto the field chain.  PASS =
withholds ≤ 5/360 with ≤ 10 unsolved, h1–h3 ZWD span p50 < 2 m with σ ≥ 5 mm, DD arc wander e1 MAD < 1 cyc
on ≥ 40 arcs ⇒ the converging field leveling posture exists and the F3 12 h arm is re-run on it (with its
own no-F3 control); FAIL on any leg ⇒ the two defects are not additive and the leveling vehicle needs a
design card before F3 can be read on the field.**
**Probe 7 read (19:10): PASS on every gate — 360/360 solved, 0 withholds, 0 unsolved; h1–h3 ZWD span p50 0.49 m
(p90 3.5, max 9.1) with σ 8 mm; DD arc wander e1 MAD 0.255 cyc on 59 arcs (54 % < 0.3, a 30 % tail > 1 cyc;
cross-band slope −0.87, corr −0.91 — band-ANTI-correlated sub-cycle wander, i.e. a GF-shaped residual, noted not
read); prires worst |t| still 2.5e8; DD fractions at the null as expected without F3.  So the converging field
leveling posture is the sim golden's: `carry_coupling_gauge_project` + the prioritized phase-datum family
(`phase_datum_eq34: false`, `phase_datum_prioritized`, `phase_datum_handover_continuous`,
`phase_anchor_carry_reref`, `phase_datum_min_stations: 2`).  **Second round PRE-REGISTERED (configs written
19:08, before probe 7 was read): `_fldf3_pl2` (that posture, F3 off) and `_fldf3_on2` (+ F3), 12 h fresh,
same exe, float replays of both wires; §6.15.3 P1–P7 apply unchanged with on2 ↔ pl2 as the graded delta.**

**SECOND ROUND — landed 19:20 (both arms 1,440/1,440 solved, 0 withholds, 0 unsolved; ~8 min wall each).**

| | pl2 (converging vehicle) | on2 (+ F3) | control ci (shipped) |
|---|---|---|---|
| ZWD per-station span p50 (σ) | 0.80 m (8 mm) | 1.20 m (9 mm) | 21 m (0.57 m) |
| DD-tropo arc MAD | **0.07 m** | 0.08 m | 1.22 m |
| DD-iono arc MAD | 0.56 m | 0.45 m | 1.25 m |
| e₁ on DD-iono slope (corr) | **+0.03 (0.49)**, A −0.000 | +0.09 (0.46), A +0.001 | +1.72 (0.93) |
| DD arc wander e₁ MAD / WL | **0.12 / 0.12 cyc** (ref-free 0.065) | 0.71 / 0.19 (ref-free 0.35) | 10.2 / 0.56 |
| cross-band slope | 1.07 (p10 −1.27, p90 1.37) | 1.17 (0.86–1.34), iono-shaped | 1.004 |
| DD fractions L1C: R (null), gap | 0.454 (0.449), −0.000 | 0.468 (0.449), −0.000 | 0.464 (0.472), +0.012 |
| L2W: R (null) | 0.430 (0.433) | 0.444 (0.433) | — |
| **WL12: R (null), near, ref-free gap** | 0.468 (0.450), 22.8 %, +0.004 | **0.562 (0.450), 24.3 %, −0.028** | 0.487 (0.482), 21.4 %, +0.006 |
| prires worst \|t\| / T3 / T4a | 2.5e8 / 3.4 % / 0.31 % | 2.9e8 / 2.3 % / 0.18 % | 178 / inert / inert |
| block-Baarda block-only | 0.8 % | 5.4 % | 2.4 % |
| float cold consumer, settled 3D rms / p50 | 3.77 / 3.18 m | 3.99 / 3.22 m | 6.56 / 5.83 m |
| D0 median | 265 ms | 268 ms | 1,072 ms |

**Readings.**  (1) *The converging posture closes the weakly-determined direction on the field.*  On pl2 the
DD-tropo wander is 0.07 m (control 1.22), the ambiguity↔iono co-motion is gone (slope +0.03, A −0.000 — the sim
F3 arm's signature), and the per-band DD wander is 0.12 cyc (control 10.2) — the (I, N, ZWD) trade of
§6.15.3a is a property of the SHIPPED posture (eq34 datum, no coupling gauge projection, per-epoch ZWD as a
sink, invariant carry), not of the field data.  Its float consumer is 3.8 m against the shipped wire's 6.6 m
— better, and still metre-class.  (2) *F3 on it — PARTIAL, the pre-registered intermediate branch.*  P1 passes
(87,140 blocks, 0 fallbacks, 0 refused; +1.4 % D0, P7 passes).  **WL12 leaves the uniform null for the first
time on the field** — R 0.562 vs 0.450 (+0.112), near-integer 24.3 %, ref-free gap −0.028, closure 0.143 vs
0.174 — while L1C and L2W stay at it (R − null +0.019 / +0.011, below the pre-registered +0.05): the L1 errors
are multi-cycle-but-bounded and F3's effect is read on the WL, as §6.15.2 said it would be.  The residual
per-band wander under F3 is LARGER than pl2's (0.71 vs 0.12 cyc) but iono-shaped (cross-band 1.17, the sim
F3 arm's 1.26) — the honest per-LOS floor family, not the frozen-level family.  Consumer flat (3.99 vs 3.77,
P5 not met), T3/T4a lower, block-Baarda block-only higher (5.4 %).  (3) *Verdict, revised.*  The F3 knob is
SOUND on real data and gives the first non-uniform WL fractions the field has produced, but it does not
change the wire (metre-class on every arm) and does not lift L1/L2; ledger stays KEEP OFF on field lineages
as a promotion decision (no consumer win, prires |t| ~1e8 on both arms of the converging posture), with the
field regime now DEFINED: the converging leveling posture = `carry_coupling_gauge_project` + the prioritized
phase-datum family.  **The field lever this ladder actually found is that posture, not F3:** it retires the
shipped posture's (I, N, ZWD) trade and halves the float-consumer error (6.6 → 3.8 m) with the invariant carry
replaced by the leveling carry — a vehicle question for the field goldens, owed its own card (the invariant
carry's RAIM pool re-host, the lanes and the E2/E3 machinery all assume the shipped vehicle).  What is still
open on the converging posture: the prires |t| 1e8 rows (over-confident priors on both arms), the 23 % of arcs
with > 1 cyc wander on pl2 (a tail population), the block-Baarda rise under F3, and the metre-class wire itself
(§6.15.4's scoping note stands: sub-decimetre first, integers after).

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

## §6.16 The ZWD sink census, the slipped-bed golden, and where the integer information is (2026-09-09)

Four measurements from one day, in the order they constrain each other: a truth-free instrument that
reads the same defect on every bed; the one station that looked like the cause and is not; the golden
posture failing on a slipped truth bed (so the sim now reproduces the field's metre-class wire); and
the first honest look at whether the EMITTED wire carries integer information at all.

### 6.16.1 The instrument — settled per-station ZWD span

`logs/network_zwd.csv` (`week,tow,station,zwd_m,std_m,carried`); per-station **span = max − min over a
SETTLED window** (field ≥ h6; sim last 50 %). Physical ZWD varies < 0.5 m / 12 h, so any span above
~1 m is a sink, not weather — the estimator is parking something in the wet delay.

⚠ **Never quote a whole-run span.** Ignition dominates it on every bed: `runs/golden_sim_tri_w6`
reads whole-run p50 **2.011 m / max 17.23 m** and settled (h12+) p50 **0.0632 m / max 0.0797 m, 0 of
60 above 0.10 m** (`runs/golden_sim_tri_w6/BASELINE.md` §2).

**It is truth-validated on the sim**: quiet golden span 0.076 m ↔ ZWD truth rms 0.009 m; slipped
`camp60wu_f5c` span 2.29 m ↔ truth rms 2.85 m. That licenses it as a FIELD proxy, where no truth
exists.

| bed / vehicle | settled span p50 | n > 1 m / > 10 m | top stations |
|---|---|---|---|
| FIELD shipped control `runs/fldf3_ci` (carry_invariant + eq34 + SH, DOY-129, 63 sta) | **21.4 m** | 61 / 50 | BSHM 94, HKSL 67, KRGG 55, GAMB 51 |
| FIELD converging `runs/fldf3_pl2` (leveling vehicle, gauge projection + prioritized family) | **0.797 m** | 24 / 3 | **BSHM 368.9**, CUSV 12.8, COCO 12.7 |
| FIELD `runs/fldf3_on2` (the same + F3) | 1.20 m | 39 / 4 | INEG 492, BSHM 35 |
| SIM golden lineage, quiet 24 h (`readmit24h_e2`, `camp60wu_quiet24h_sticky`, `nonint_losblock24h_hv`) | **0.076 m** | 0 / 0 | S06 0.20 |
| SIM slipped campaign, pre-F3 (`runs/camp60wu_f5c`, h3–h6) | 2.29 m | 52 / 4 | S45 28 |
| SIM `carry_invariant` + eq34 arms, Aug code (`simnet60_ab_eci_eq34_ci`, `ab_d5_*`, `simnet60_tri_slant_trio_480`) | 389 – 34 083 m | 60 / 57–60 | km-class divergence |

(Census of 2026-09-09; each figure recomputed from that run's own `network_zwd.csv`. The three
`fldf3_pl2*` rows were re-derived for this section and reproduce to the printed digits — p50 0.797 /
0.660 / 0.904 m on control / `gap_reentry_relevel` / bridge arms respectively.)

**Reading.** The shipped field vehicle's per-epoch ZWD is a metre-to-decametre sink at EVERY station,
and the same vehicle reproduces km-class on the sim. The converging leveling posture (§6.15.4) brings
the fleet centre to sub-metre; the quiet sim golden at 7.6 cm is what "honest" looks like. **The
sink, not the ambiguity level, is now the field's leading residual.**

### 6.16.2 BSHM00ISR is a phase-side receiver fault — and roster edits are not the lever

BSHM00ISR is broken from the FIRST epoch on every field arm: its ZWD is **−5.46 m at tow 259200**
(`runs/fldf3_pl2/logs/network_zwd.csv`; a physical ZWD is 0–0.4 m), run min/max **−2445.97 / +359.43 m**,
and the station-congruence rider flags it CLEAN-labelled with d = **(−417.82, −144.14, −770.88) m** ENU
at t = **2.188e6** on `n_code 0 / n_phase 14` (`runs/fldf3_pl2/logs/network_congruence.csv`, tow
271200, `reported=1`, measure-only). Code side admits it, phase side does not: the fault is on PHASE.
Its roster ECEF is 17 cm from the RINEX APPROX POSITION, so this is not a coordinate blunder. (The
−2446 m excursion at 271200 specifically is the gap-re-entry poison case —
`docs/SPEC_slip_qc_hardening.md` §12.1 — not the baseline pathology.)

**Footprint A/B (3 h, DOY-129, settled ≥ h1, 57 stations common to all three arms):**

| arm | 57-sta Σ settled span | INEG00MEX | OAFA00ARG | consumer h2 rms |
|---|---|---|---|---|
| base `runs/fldf3_pl_gppdp3h` | **59.8 m** | **1.85 m** | 9.12 m | **2.02 m** |
| `_nobshm` (BSHM dropped) | **61.5 m** | **26.32 m** | 8.21 m | **3.92 m** |
| `_notop4` (BSHM/OAFA/INEG/CUSV dropped) | **53.8 m** | — | — | **1.53 m** |

**Dropping the worst station does not shrink the sink — it relocates it.** INEG00MEX takes it
(1.85 → 26.3 m) and the fleet sum goes UP. Dropping the top four improves most instruments but the
consumer is non-monotone in roster (2.02 / 3.92 / 1.53 m). ⇒ **The residual null direction parks on
whichever station is weakest; roster editing is not a fix, and a per-station ZWD excursion is not
evidence about that station.** The shipped `carry_invariant` vehicle hides BSHM entirely because its
free-ambiguity rows absorb the step (ZWD σ 0.85 m there vs 0.028 m on the leveling vehicle) — the
sink is a property of the vehicle, not of the roster.

### 6.16.3 The golden posture on the SLIPPED sim bed — FAIL on all three gates

`runs/golden_sim_campaign` (= `config/golden/golden_sim_tri_w6.json` verbatim, 61 `rinex_path`
re-pointed at `runs/simnet60_campaign_wu93`, duration and output paths only; exe
`gnss_processor_golden0.exe` sha256 `04267126…` = HEAD `36ace5e`), 720/720 epochs, exit 0.
Full card `runs/golden_sim_campaign/VERDICT.md`.

| gate | pre-registered | golden-on-campaign | pre-F3 `camp60wu_f5c` | verdict |
|---|---|---|---|---|
| ZWD settled truth rms | ≤ 0.10 m | **2.098 m** | 2.742 | **FAIL** (21×) |
| per-sta settled ZWD span p50 | ≤ 0.20 m | **1.590 m** | 2.292 | **FAIL** (8×) |
| consumer settled h3–h6 p50 3D | ≤ 0.30 m | **1.752 m** | 1.614 | **FAIL** (6×, worse than pre-F3) |

**The integer half, however, collapses in the good direction** (`simnet_amb_integer.py --ref-free`,
"at own true integer", settled): EWL(2,5) **99.0 %** (f5c 79.1), WL(1,2) **81.5 %** (33.7), L1C
**39.1 %** (18.6); as-traced EWL 98.7 / WL 70.5. ⇒ **F3 fixed the ambiguity over-count and left the
station-level ZWD⇄iono exchange intact** (interaction 3.54 → 2.46; median exchange correlation
−0.433 → −0.350; corr(log ZWD rms, event rate) 0.692 → **0.765**, i.e. the plateau is MORE tightly
tied to the per-station event rate). **The sim truth bed now reproduces the field's metre-class wire
under realistic slips** — which is the useful outcome of a FAIL.

### 6.16.4 E1 / E2 — two separable halves, and neither knob is the fix

Single-knob arms off the same file, same exe, same fixture, 720/720 each (VERDICT.md
§"E1/E2 discriminators").

⚠ **E1's knob is `network.raim_arc_persist_epochs: 1000000`, NOT `raim_attribution: false`** — the
loader force-re-enables `raim` + `raim_attribution` whenever `raim_within_epoch` is armed, so that
knob is a proven byte-identical no-op. The persistence gate is what starves the T4 isolated-arc
ERASE while leaving both detection tiers armed (`NetworkRaim.cpp:608-649`).

| row (settled) | **E0** golden | **E1** no arc-refloat | **E2** F3 off | `f5c` pre-F3 | quiet `readmit24h_e2` |
|---|---|---|---|---|---|
| ZWD truth rms (m) | 2.098 | **0.933** | **3.799** | 2.742 | 0.007 |
| per-sta span p50 (m) | 1.590 | 1.312 | 3.227 | 2.292 | 0.053 |
| consumer h3–h6 p50 (m) | 1.751 | **2.559** | 1.996 | 1.615 | 0.075 |
| consumer last-30 median (m) | 1.423 | **0.686** | 2.808 | 1.720 | 0.068 |
| iono⇄ZWD interaction rms (m) | 2.456 | 2.493 | 4.090 | 3.544 | — |
| sat-clock contrast rms (m) | 8.050 | **2.761** | 13.307 | 13.001 | — |
| sat code-bias residual rms (m) | 0.608 | **0.289** | 0.978 | 1.252 | — |
| **T4 one-solve arc refloats** | 22 968 | **0** | **35 010** | — | 147 (24 h) |
| post-fit σ̂₀(phase), last ep | 1.424 | **2.687** | 1.217 | 0.906 | 1.314 |
| T3 cell rate | 1.561e-1 | **3.191e-1** | 8.350e-2 | 5.148e-2 | 3.919e-4 |
| T3c consensus rate | 4.414e-1 | **8.159e-1** | 2.638e-1 | 2.024e-1 | 6.463e-1 |
| WL(1,2) ref-free at own integer | **81.5 %** | 65.4 % | 37.6 % | 33.7 % | 99.1 % |
| EWL(2,5) ref-free | **99.0 %** | 91.0 % | 84.6 % | 79.1 % | 100 % |
| D3 arc-age interaction rms, 0–60 s → 1200+ s (m) | 4.264 → 1.974 | **3.114 → 2.932 (FLAT)** | 6.011 → 2.995 | 5.252 → 2.785 | — |
| corr(log ZWD rms, log event rate) | 0.765 | **0.123** | 0.853 | 0.692 | — |

**Reading — the halves separate, and each has a different agent.**

**(a) The T4 erase channel is the dominant single contributor to the metre-class ZWD plateau.**
Starving it: ZWD rms −56 %, sat-clock contrast −66 %, code-bias −52 %, the arc-age slope GONE, the
plateau-to-event-rate correlation 0.765 → 0.123, while the iono⇄ZWD interaction is FLAT (2.456 →
2.493). So the ZWD gain is not a change in the exchange — it is the removal of a repeated
re-convergence forcing: **every erase is a code-level re-level, i.e. a young arc, i.e. a ZWD/clock
sink** (invariant #1's re-coupling edge again).

**(b) The carries T4 was catching are real.** Without erases the consumer settled p50 is 46 % worse,
ref-free WL gives back a third of what F3 bought, σ̂₀(phase) 1.42 → 2.69, and T3c runs at 0.816 —
nothing is being erased, so the post-fit pool is loud and un-screened. Slip response shifts tier, not
size (`T4_attrib` 53 → 0 while `raim_refloat` 19 → 84; DETECTED flat at 94.0 %).
⇒ **The fix is BOUNDED IDENTIFICATION (Tier-C3 solution-separation confirmation of the median-polish
proposals), NOT switching T4 off.** `raim_arc_persist_epochs` stays a probe.

**(c) F3 is load-bearing and stays armed.** E2 is worse on every channel (ZWD 3.80 m, WL 37.6 %,
consumer rms 3.61) — and **T4 goes UP without F3 (35 010 vs 22 968)**, which falsifies the standing
suspicion that F3's block rows are what drives the T4 storm (§7 item 4).

Ordering on the wire: E1 (0.93) < E0 (2.10) < f5c (2.74) < E2 (3.80). On the integers:
E0 > E1 > E2 > f5c. **Nothing on this bed is simultaneously best at both.**

### 6.16.5 The HEAD baseline on the quiet bed (PARTIAL, 18.6 h) — the ~5 cm KPI holds

`runs/golden_sim_tri_w6` = the golden verbatim on `runs/simnet60_quiet_wu93_24h`, HEAD exe. ⚠ The run
was killed by a reaped shell at 2 230 epochs (18.6 h of 24 h): **no run-end telemetry, no
`network_flag_rates.csv` / `network_run_summary.csv`, `network_raim.csv` 0 bytes ⇒ the QC tier table
is NOT gradeable on it**; consumer "settled" is h12–18.6 (`BASELINE.md` header).

| channel | `golden_sim_tri_w6` (18.6 h) | `readmit24h_e2`, matched window |
|---|---|---|
| consumer settled 3D rms / p50 (m) | **0.0823 / 0.0506** | 0.0882 / 0.0779 |
| consumer last-30 median (m) | 0.0305 | 0.0294 |
| ZWD residual rms / p50 / max (m) | **0.0070 / 0.0045 / 0.0374** | 0.0076 / 0.0047 / 0.1596 |
| per-sta settled ZWD span p50 (m) | **0.0632** (0 of 60 > 0.10) | 0.0533 |
| sat clock residual rms (m) | 1.5354 | 1.6285 |
| sat code bias rms (m) | 0.1469 | 0.4435 |
| DD ref-free settled at own integer | **EWL 100 %, WL 99.2 %, L1C 90.5 %**, L2W 88.1, L5Q 87.4, L7Q 87.3 | EWL 100, WL 99.1, L1C 90.0, L2W 86.9, L5Q 86.4, L7Q 87.1 |
| DD ref-free settled med\|u\| (cyc) | L1C 0.068, WL 0.025, EWL 0.010 | L1C 0.079, WL 0.028, EWL 0.011 |

At or slightly better than the 24 h reference on every like-for-like row despite 2.7 h less settling.
**The ~5 cm consumer KPI and the sub-cm ZWD hold at HEAD** — so nothing in §6.16.3's failure is a
regression of the code; it is what the SLIPPED bed does to this posture.

### 6.16.6 Where the integer information is — the P records carry none, and that is gauge-ambiguous

Measured on the quiet golden `runs/readmit24h_e2` (truth bed), 2026-09-09; session-scratchpad
analysis recorded in the ZWD-sink census memory record, not re-derived here:

* The ESTIMATOR's DD ambiguities are ~**87 %** at the true integer ref-free (per-band ref-free
  87.1–90.5 %, §6.16.5).
* The EMITTED P records carry **no fractional structure**: within one epoch the between-satellite SD
  fractions of the phase biases are uniformly scattered — within-group concentration p50 **0.18**
  against the uniform null ≈ **0.28** for n ≈ 10; per-satellite circR p50 **0.43** as-traced /
  **0.52** ref-free.
* **1 267** ref-free steps > 0.15 cyc in h12–24 across 156 (cst, sig, sat) series, and **NONE within
  90 s of a PBGAUGE handover** (942 continuous + 93 cold on the run). So SPEC_amb_iono §6.8's
  "P-record fractional uniform on every arm (open)" is a **high-frequency per-epoch wander, not
  handover steps** — the handover card is not the explanation.

**Reading (pre-registered, NOT proven).** On the free-slant-iono posture each satellite's per-band
phase-bias pair carries an iono-direction gauge (δ, γδ) pinned only by code (σ 0.3 m zenith on this
fixture). That direction is invisible to between-station DDs — hence the 87 % — and is EXACTLY
absorbed by a consumer that estimates its own slant ionosphere. **⇒ the per-band SD-fraction grade
(`simnet_grade.grade_phase`) is not the integer test that matters on this posture; it is
gauge-ambiguous.** The test that matters is the closed-loop consumer fixing integers against
`truth_ambiguities.csv`, and it has NEVER been run on the sim golden (`closed_loop.ar_mode: 0`).

**Arms queued** (written 2026-09-09, not yet run): `config/test_suite/_golden_sim_consumer_ar_a.json`
and `_b.json` — `golden_sim_tri_w6.json` verbatim on the quiet 24 h bed with the consumer's
`closed_loop.ar_mode` 0 → 1, `ratio_threshold` 3.0, `enable_partial_ar`, `logging.enable_ambiguity_trace`,
producer UNCHANGED (float); a/b differ ONLY in `estimation.ambiguity_resolution.hold_variance`
(1e-3 vs 1e-5) and their own output paths.

**AR sequencing decision (binding until the consumer test reports).**
1. Run the consumer-AR truth test first. If integers recover, the wire already carries them and the
   next question is robustness under slips (campaign bed) and then the field.
2. Only if the consumer test says the WIRE loses integers does the network datum work come first
   (emission chain / a phase GF datum / design-doc Phase 3 integer-leveled pins —
   `docs/network_ambiguity_resolution_design.md` §2–§4: pivot pins are float-valued `w = 1e10`
   targets, `ApplicationNetworkAssembly.cpp:2559`; anchor handovers pin at the successor's float
   value, `NetworkPhaseDatum.cpp:335-428`).
3. **Producer DD-fixing (M5) stays DO-NOT-BUILD** until the consumer test says the wire needs it; its
   prerequisite (DD convergence) is met on the quiet bed only.
⚠ The FIELD golden runs `ar_mode 1` on a metre-class wire — wrong fixes by construction; consider
`ar_mode 0` there until the wire is sub-decimetre.

### §6.16.7 Late results (2026-09-10 00:00–02:30)

Five results that landed after §6.16 was written: the full-length HEAD baseline that §6.16.5 could
only sample; the consumer-AR test §6.16.6 pre-registered; a per-satellite accounting of the FIELD
consumer's error; the ZWD-in-block card's three arms; and the `code_datum_time_linked` A/B that
closes `docs/SPEC_family_gf_datum.md` §2.1's owed work. **Nothing above this line is edited.** Every
figure below was recomputed for this section from the named run's own files.

#### 6.16.7a The full 24 h HEAD baseline — §6.16.5's PARTIAL, replaced

`runs/golden_sim_tri_w6_full` = `config/test_suite/_golden_sim_full.json`, which is
`config/golden/golden_sim_tri_w6.json` **verbatim with `network.product_output_path` as the only
differing key** (flattened config diff run for this section; its `_comment_arm` is the only other
addition), exe `gnss_processor_golden0.exe` = HEAD
`36ace5e`. 2 879/2 879 epochs emitted, 0 skipped, exit 0. Grade files under `grade/`
(`simnet_grade.py --settle-frac 0.5`, `simnet_amb_integer.py --ref-free`).

| channel (settled = last 50 %, h12–24) | **full 24 h** | §6.16.5 PARTIAL 18.6 h | quiet ref `readmit24h_e2` |
|---|---|---|---|
| consumer 3D rms / p50 / max (m) | **0.0762 / 0.0604 / 0.2073** | 0.0823 / 0.0506 / — | 0.0882 / 0.0779 |
| consumer UP mean / rms / max (m) | **+0.0066 / 0.0644 / 0.1918** | — | — |
| consumer last-30 median / final 3D (m) | 0.018 / 0.117 | 0.0305 | 0.0294 |
| ZWD residual rms / p50 / max (m) | **0.0072 / 0.0046 / 0.1436** | 0.0070 / 0.0045 / 0.0374 | 0.0076 / 0.0047 / 0.1596 |
| sat clock residual rms / contrast rms (m) | 1.5005 / 1.4563 | 1.5354 / — | 1.6285 |
| sat code bias rms / p50 / max (m) | 0.1482 / 0.0961 / 0.5231 | 0.1469 | 0.4435 |
| rx clock contrast rms (m) | 1.8730 | — | — |
| DD ref-free, settled, at own true integer | **EWL 100.0 %, WL 99.1 %, L1C 86.4 %**, L2W 85.8, L5Q 83.5, L7Q 80.5 | EWL 100, WL 99.2, L1C 90.5 | EWL 100, WL 99.1, L1C 90.0 |
| DD ref-free settled med\|u\| (cyc) | EWL 0.010, WL 0.025, L1C 0.078 | EWL 0.010, WL 0.025, L1C 0.068 | — |
| DD **as-traced** settled at own integer | EWL 100.0 %, WL 97.9 %, **L1C 54.1 %** | — | — |
| P-record between-sat SD fraction, rms / p50\|·\| (cyc) | 0.277–0.301 / 0.213–0.261 | — | — |

Run-end, same console: **1 035 anchor handovers, 953 CONTINUOUS + 82 cold** (largest avoided
`|target|` 27.94 m), 328 marked family-epochs marginalized, 1 463 leveling local carries erased;
**28 re-admissions detected**, 110 entries parked, 26 clock + 84 code-bias columns seeded on return
(all 26 clock seeds folded ANCHORED).

**Readings.** (1) **The ~5 cm consumer KPI holds at HEAD over a full day** — 7.6 cm rms / 6.0 cm p50,
and the UP channel is unbiased to 7 mm with 6.4 cm scatter. (2) The 18.6 h PARTIAL was not
optimistic: every like-for-like row agrees within a few per cent, so §6.16.5's conclusion survives
its own truncation. (3) The as-traced / ref-free split of §6.12 is reproduced verbatim on a
full-length run — L1C reads **54.1 % as-traced and 86.4 % ref-free**, a 32-point instrument
artifact, while the lanes (EWL/WL) barely move because the reference satellite's error is
lane-suppressed. (4) The P-record fractions sit AT the uniform null on all six (cst, sig) channels
(rms 0.277–0.301 cyc against 1/√12 = 0.289), i.e. §6.16.6's "the emitted wire carries no fractional
structure" is confirmed on the full run — and §6.16.7b says what that does and does not mean.

#### 6.16.7b Consumer AR on the quiet golden — 0 fixes in 2 879 attempts, and the blocker is the CONSUMER's own float

Arm A ran: `runs/golden_sim_consumer_ar_a` (`_golden_sim_consumer_ar_a.json`, the golden with the
consumer's `closed_loop.ar_mode` 0 → 1, `ratio_threshold` 3.0, partial AR, `hold_variance` 1e-3,
`logging.enable_ambiguity_trace`; **producer untouched**), quiet 24 h bed, 2 879/2 879 epochs.

| quantity | value | source |
|---|---|---|
| AR invoked / accepted | **2 879 / 0** (0.0 % fix rate), first fix NONE | console run-end block |
| gate that refused | `ratio_battery` on **2 596** of 2 879, `min_epochs` 10, `sigma_gate` the rest | `logs/closed_loop_trace.csv` |
| ILS ratio (rows that reached the battery) | p50 **1.01**, min 1.00, max **1.26**; hourly p50 **1.00–1.07 in all 24 hours** | same |
| candidates per epoch | p50 **20** (range 0–30) | same |
| position, settled h12–24 | 3D rms 0.0762 / p50 0.0604 m — **identical to the float baseline** | `grade/` of both runs |

**Why it cannot fix, measured with truth.** Joining the consumer's own float ambiguities
(`logs/ambiguity_trace.csv`: `week,tow_s,key,value_m,std_m`) to
`runs/simnet60_quiet_wu93_24h/SZZ000SIM/truth_ambiguities.csv` (one `N_int` + wavelength per
(sat, signal)) over the settled window h12–24:

| band | n | float σ, p50 (cyc) | med\|frac(e)\| vs truth (cyc) |
|---|---|---|---|
| L1C | 22 292 | **0.321** | 0.244 |
| L2W | 12 837 | **0.368** | 0.230 |
| L5Q | 22 292 | **0.422** | 0.251 |
| L7Q | 9 455 | **0.439** | 0.239 |

(The session record's "0.29–0.45 cyc" is the same quantity over its own window; the p50s above are
the recomputation.) **A per-band float σ of 0.32–0.44 cyc cannot produce a ratio > 3 no matter what
the wire carries** — the ILS search is choosing between near-equiprobable candidates, which is
exactly the observed ratio ≈ 1.0. The med|frac| column is at the uniform null (0.25) for the same
reason: with a free per-LOS slant ionosphere at the consumer and **no WL/NL cascade** (the
undifferenced consumer runs direct per-band LAMBDA; `ar_wl_only` is inert under
`consumer_undifferenced`), the per-band ambiguities are **undetermined by construction**.

**⇒ Two consequences, both binding.** (1) The 23:00 reading "the wire carries no integers" is **NOT
established either way** — this test cannot see the wire through a float that weak, and neither can
the P-record per-band SD-fraction grade (§6.16.6), which is confounded twice: the iono-direction
gauge AND the consumer's float weakness. (2) **The AR sequencing of §6.16.6 is corrected**: step 1 is
now the **consumer-side MW-WL → IF-NL cascade on the undifferenced model** (MW-WL is iono-free, σ ≈
0.05 cyc over hours), step 2 an anchor-aware TRUTH grader on the WL/IF combinations of the P records,
and only then network-side integer leveling (design-doc Phase 3). Producer DD-fixing (M5) stays
DO-NOT-BUILD. Arm B (`hold_variance` 1e-5) was killed as moot — the gate never reaches the hold.
Field note: `ar_mode 1` on the FLD63 chain also never fixes; the `_fldf3_pl2_arfloat` probe
(`ar_mode 0`) is **byte-identical in position** to its control (settled 3.178 rms / 2.418 p50 on
both, AR invoked 1 440 → 0), so the field consumer's AR is inert-in-effect, neither harm nor help.

#### 6.16.7c The FIELD consumer's residual is a slowly-varying VERTICAL wander that rides the producer's ZWD

All arms below share ONE wire (`runs/fldf3_pl2_ctl_new`, the converging leveling posture of §6.15.4,
DOY-129 00–12 UT, 61 stations); the probes differ only in consumer-side or log-only keys and their
`products.txt` is the control's.

**(i) The error is vertical, and it is not the phase-bias churn.** Settled (≥ h6, n = 720):
3D rms **3.178 m**, p50 2.418; E −0.28 ± 0.40, N +0.56 ± 1.18, **UP −2.018 ± 2.018 m** — the vertical
carries **80.7 %** of the settled sum of squares. Per-satellite churn census (scratchpad
`satchurn/`, runs `fldf3_pl2_ctl_new` + `_rtlog`):

| finding | number |
|---|---|
| consumer resets / 12 h, distinct (sat,signal) keys | **665 on 52 keys**, all on L1W/L2W (the signals WTZZ00DEU tracks) |
| wire PB bumps (adjacent-epoch \|ΔPB\| > 0.05 m in the P records) | 5 025 on 114 keys; **48.8 % (2 450) sit on L1C/L2X keys the consumer never sees** |
| producer PB-discontinuity counter bumps | 4 839 on 114 keys, max 280/key (state-snapshot `ep,` lines) |
| **eclipse re-admission link** | **3 of 665 resets (0.5 %)** within ±10 min of THAT satellite's own return; consumer error after returns 3.195 m vs 3.185 m elsewhere ⇒ **DEAD on the field** |
| clock-column departures with a return inside 3 900 s | 8 of 8 — the only population `readmit_seed_horizon_s` could act on, and `estimate_sat_code_bias` is OFF on this chain (the FALSIFIED clock-only variant) |
| coverage / elevation at bump epochs | median ratio (bump N)/(satellite median N) = **1.000**; 39 below / 23 equal / 52 above ⇒ no coverage signal |
| reset concentration | **52.2 % of all resets in one hour** (04:30–05:30, 7 satellites) = the run's peak prior tension, 1.3–2.1 h AFTER the returns |
| the counter-example | **h3: 1 reset (0.2 %) yet rms 4.325 m, mean UP −4.037 m** — the largest vertical hour has essentially no reset activity |

Window-exclusion of the top-3 satellites' (G12/G25/G24) reset windows takes the settled rms
3.178 → **1.496 m** on the 301 surviving epochs — but at **p(null ≤ obs) = 0.110** under a
circular-shift null that preserves the burst structure and destroys only its phase (200 draws; null
median Δ +0.154, 5–95 % [−1.739, +1.590]; scratchpad `satchurn/q5b.txt`). The same file's
variance-share cut says where the error actually is: **h6/h7/h8 carry 42.8 / 30.2 / 19.2 % of the
settled sum of squares** and h9–h11 carry 7.8 %; at h9–h11's level the settled rms would be 1.25 m.
⇒ **PB churn is at most secondary; the residual is the long-open consumer
vertical defect** (`project_closedloop_consumer_vertical_bug`).

**(ii) It is not the consumer's ionosphere and not its AR.** Three consumer-only probes on the
identical wire (settled ≥ h6): SH-VTEC slant feed OFF (`consume_network_iono: false`,
`_fldf3_pl2_noiono`) → **3.771 m rms** (WORSE — the feed helps, as the 305-sta record said);
iono-supply σ×3 (`estimation.ionosphere_weighting.std_scale` 1 → 3, `_fldf3_pl2_ionoscale3`) →
**3.721 m**, UP mean −2.447 (also worse, ≈ feed OFF ⇒ the honest σ is near-optimal); AR off
(`_fldf3_pl2_arfloat`) → **byte-identical** 3.178 / 2.418. The −2 m DC and the ±2 m wander survive
all three.

**(iii) It rides the producer's per-epoch ZWD.** From the consumer's own state series
(`_fldf3_pl2_statetrace`, log-only, `logs/state_trace.csv`; products identical to the control):

* the consumer's own ZWD state is **physical** — settled 0.414–0.572 m, sd **5.5 cm** —
* yet its height error tracks that state at **dUP/dZWD = −28.9 m/m, corr −0.785**, an order of
  magnitude above the classic ZWD⇄height trade ⇒ **a common EXTERNAL driver**, not the
  consumer's own tropo estimation;
* consumer UP vs the producer's **fleet-mean per-epoch ZWD deviation** (per-station deviation from
  its own settled median, meaned over the 61 stations; sd 2.241 m): **corr +0.842, slope +0.758 m/m**.

**(iv) Two leak regimes.** Per-station: **BSHM00ISR alone gives corr +0.850 at slope +0.0129 m/m**,
against 1/61 = 0.0164 — i.e. a sick station's per-epoch ZWD excursion enters the shared satellite
clocks/PBs at very nearly its weight in the solve and arrives at the consumer looking like tropo.
On the arms where BSHM's excursion is gone (`gapfree2`, `nosh`, `on2`) the top correlations move to
HEALTHY stations at slopes 10–60 m/m — a **network-wide common tropo-like wander** that every
station's ZWD and the consumer's height ride together, i.e. a weakly-determined per-epoch common
(ZWD ⇄ clock) direction with all ZWDs free every epoch. Same missing piece in both regimes: **a
temporal prior on the producer's ZWD** (§6.16.7d), plus a station-level response to the congruence
screen (§6.16.2's t = 2.2e6, measure-only today).

> ⚠ **(unsourced) — one figure in the session record does not reproduce.** "healthy-57 median ZWD
> wander sd 9 mm" could not be reproduced from `runs/fldf3_pl2_ctl_new/logs/network_zwd.csv` under
> any of four definitions of "wander" over the settled window, excluding BSHM/CUSV/COCO/BOR1:
> raw per-station sd of the deviation **71.4 mm** (p25 45 / p75 161), common-mode-removed sd
> 78.0 mm, MAD 32.8 mm, adjacent-epoch-difference sd 29.7 mm. The nearest mm-scale number on that
> file is the median per-station **formal σ, 7.7 mm**, which is a different quantity. Use 71 mm as
> the healthy-fleet wander until someone re-derives the 9 mm.

**(v) The logging trap that produced a false "0 consumer resets".** The FLD63 chain leaves
`logging.enable_rt_engine` at its loader default FALSE while the sim golden sets it true, so a field
console carries **no `[RT]` lines and `rt_engine.log` is 0 bytes** — measured: 0 bytes on
`fldf3_pl2_ctl_new` and `fldf3_pl2_zwdblk`, 480 717 bytes / 665 reset lines on `_rtlog` where the
logger is armed. **Any consumer-churn claim must come from `rt_engine.log` with the logger armed,
never from the console.** The log-independent cross-check is the emitter's own PB-discontinuity
counter in the state snapshot (4 839 bumps / 114 keys above).

#### 6.16.7d The ZWD-in-block card (`network.carry_zwd_in_block`, merged 966d4fd + 7b32a74) — four arms

Built to remove §7 item 17's double count: a **diagonal** B9 row asserts the station-common ZWD
direction a second time on top of the assertion every carried ambiguity row of that station already
makes implicitly; inside F3's jointly-whitened local block the harvested (N_rr⁻¹) cross terms
subtract the overlap exactly. Default OFF, OFF twin byte-identical (`products.txt` sha256
`47102044…` on both smoke arms), 10 unit tests. Full card:
`runs/golden_sim_campaign_zwdblk/VERDICT.md`.

**Arm Q — quiet 24 h (`runs/golden_sim_zwdblk_quiet`), the load-bearing negative control.**

| settled h12–24 | gate | **in-block** | uncarried, full 24 h (§6.16.7a) | uncarried, PARTIAL 18.6 h | **DIAGONAL `carry_zwd`** |
|---|---|---|---|---|---|
| consumer 3D rms | ≤ 0.10 m | **0.0877** | 0.0762 | 0.082 | **0.723** |
| consumer 3D p50 | ≤ 0.06 m | **0.0525** | 0.0604 | 0.051 | — |
| ZWD truth rms | ≤ 0.02 m | **0.0057** | 0.0072 | 0.0069 | — |
| per-sta settled ZWD span p50 / max | — | 0.032 / 0.054 m | — | 0.063 / 0.080 | — |

(The card's own VERDICT graded arm Q against the PARTIAL control, which did not cover h12–24; the
full 24 h baseline of §6.16.7a is the matched-window control and is added here. Against it the
in-block carry is 15 % worse on consumer rms, 13 % better on p50, and **21 % better on the ZWD
channel it targets** — the gate verdict is unchanged.)

Wiring: 172 320 blocks whitened, **172 320 ZWD columns, 0 diagonal fallbacks, 0 missing cross
terms**, 0 factorization fallbacks; `network_zwd.csv carried` = 172 320/172 740 = 99.8 % (the 420
uncarried rows are exactly 60 stations × the 7-epoch leveling warm-up). **⇒ Q1 PASS, and it is the
result that matters: what falsified B9 on the quiet bed was the DOUBLE COUNT, not the idea of
carrying ZWD** (0.723 m diagonal vs 0.088 m in-block, same bed, same carry).

**Arm Z — slipped campaign bed, 6 h (`runs/golden_sim_campaign_zwdblk`), single knob vs E0.**

| channel (settled) | E0 golden | **in-block** | Δ |
|---|---|---|---|
| per-sta settled ZWD span p50 | 1.590 m | **0.207 m** | **−87 %** (Z2 PASS) |
| ZWD settled truth rms | 2.098 m | **1.188 m** | −43 % (Z1 FAIL vs ≤ 0.5; falsifier 1.5 not reached) |
| stations with span > 1 m | 39/60 | 15/60 | −62 % |
| ZWD hourly rms h0..h5 | 2.10 1.37 1.84 1.95 2.12 2.21 | 1.98 1.31 0.96 0.58 1.05 1.67 | better in every hour |
| WL(1,2) / EWL(2,5) ref-free at own integer | 81.5 / 99.0 % | **86.2 / 99.2 %** | Z4 PASS |
| L1C ref-free at own integer | 39.1 % | 44.0 % | better |
| **sat-clock contrast rms** | 8.050 m | **11.700 m** | **+45 % (worse)** |
| iono⇄ZWD interaction rms | 2.456 m | 3.030 m | +23 % (worse) |
| consumer settled h3–h6 p50 / rms | 1.752 / 1.799 m | **1.085** / **2.389** m | Z3 FAIL (p50 −38 %, rms +33 %) |
| consumer hourly rms h0..h5 | 2.06 3.81 2.92 1.71 1.85 1.83 | 1.59 2.19 1.56 1.03 **2.55 3.09** | better h0–h3, worse h4–h5 |
| T4 one-solve arc refloats | 22 968 | **29 613** | +29 % |
| slip census DETECTED / RESPONDED / SILENT | 2337 / 72 / 77 | 2337 / 75 / 74 | flat (not a detection artifact) |

`raim_pool_zwd_rows` = **42 682 of 42 780 (99.8 %)** — §7 item 4's interaction, now MEASURED rather
than assumed: the post-fit RAIM pool admits a row by `|A_r(row, amb_col)| > 0.5`, a test written for
data phase rows, and a whitened ZWD block row clears it on essentially every station-epoch. This
card changes nothing about the pool; the number is published so the next card decides on data.

**Arm F — FIELD, 12 h (`runs/fldf3_pl2_zwdblk` vs `_ctl_new`).**

> ⚠ **THE FIELD ARM DID NOT MEASURE THE IN-BLOCK VEHICLE — the loader disabled the sub-knob and the
> run carried the DIAGONAL B9 row instead.** `config/test_suite/_fldf3_pl2_zwdblk.json` arms
> `carry_zwd` + `carry_zwd_in_block` on the **pl2** posture, which is the converging leveling vehicle
> **with F3 OFF**; the interlock at `GnssConfigLoadNetwork.cpp:8778-8784` therefore fired —
> *"carry_zwd_in_block is on but network.phase_leveling_local_block (F3) is OFF … DISABLING
> carry_zwd_in_block"* (verbatim in `runs/fldf3_pl2_zwdblk/console.txt`) — and the run shows **no
> "ZWD in block" run-end clause at all** while `network_zwd.csv carried` reads **87 392 / 87 819**.
> The interlock behaved exactly as designed (armed-and-inert is forbidden, invariant #4). **So the
> field numbers below grade `network.carry_zwd` (diagonal B9) on the converging leveling vehicle,
> not the in-block variant.** They are still a first: the diagonal carry DIVERGED on the shipped
> `carry_invariant` vehicle (§6.15.4 arm 0z) and does not diverge here. **The in-block field arm is
> still OWED and must run on a posture that arms F3 (`_fldf3_on2`-class), not pl2.**

| settled ≥ h6 | control `ctl_new` | **`zwdblk` (= diagonal `carry_zwd`)** | pre-registered |
|---|---|---|---|
| BSHM00ISR settled ZWD span | 368.87 m | **0.87 m** | < 5 m **PASS** |
| fleet settled span p50 / n > 2 m / n > 10 m | 0.797 m / 14 / 3 | **0.275 m / 2 / 0** | p50 < 0.3 **PASS** |
| consumer UP mean / sd / rms | −2.018 / 2.018 / **2.854** m | **−0.713** / 2.660 / 2.754 m | UP rms < 1.5 **FAIL** |
| consumer 3D rms / p50 | 3.178 / 2.418 m | 3.322 / 2.316 m | < 2.0 **FAIL** |
| corr(UP, BSHM ZWD deviation) | +0.850 | **−0.722** | < 0.4 **FAIL** (sign flipped, still coupled) |
| vertical share of settled 3D SS | 80.7 % | 68.7 % | — |
| consumer hourly rms h0..h11 | 3.78 2.31 2.02 4.33 2.35 3.64 5.09 4.28 3.42 0.79 1.24 1.59 | 2.96 1.48 1.17 2.43 2.61 **7.36** 4.75 **5.14** 2.48 0.62 1.61 2.84 | — |
| **T3c consensus flags / opportunities** | 1 105 / 15 763 | **7 619 / 29 795** (6.9×) | reported |
| T3 post-fit cell flags | 22 713 / 665 332 | **60 222 / 665 338** (2.7×) | reported |
| T4 ARC attribution flags | 2 032 | 2 723 | reported |
| T4 one-solve arc refloats (run-end census) | 3 899 | 5 156 | reported |

**Arm F2 — FIELD IN-BLOCK on the F3 posture, 12 h (`runs/fldf3_on2_zwdblk` vs `runs/fldf3_on2`).**
The arm Arm F said was still owed. Base = `_fldf3_on2`, the F3-armed converging field leveling posture (pl2 +
`network.phase_leveling_local_block`, §6.15.4 second round); the arm adds `network.carry_zwd: true`
+ `network.carry_zwd_in_block: true` and **nothing else** — a flattened sorted-JSON diff of the two
configs differs on exactly those two keys plus the two run paths and the header comment. DOY-129,
63 stations, 12 h (tow 259 200–302 370), WTZZ closed-loop consumer, exit 0 both.

| settled ≥ h6 | control `fldf3_on2` | **`fldf3_on2_zwdblk` (in-block)** | pre-registered |
|---|---|---|---|
| fleet settled ZWD span p50 | 1.195 m | **0.143 m** | p50 < 0.3 **PASS** |
| fleet n > 2 m / n > 10 m | 19 / 4 | **3 / 0** | no station > 10 m **PASS** |
| worst station span | 491.51 m (INEG00MEX) | **5.12 m** (BSHM00ISR) | — |
| BSHM00ISR settled ZWD span | 34.85 m | **5.12 m** | — |
| consumer UP mean / sd / rms | **+2.834** / 2.388 / 3.706 m | **+0.072** / 1.861 / **1.862** m | \|mean\| < 1.0 **PASS**; rms < 1.5 **FAIL** (1.86, close) |
| consumer 3D rms / p50 | 4.034 / 3.058 m | **2.926 / 2.375 m** (−27 % / −22 %) | < 2.0 **FAIL** |
| consumer E rms / N rms | 1.009 / 1.233 m | 1.364 / 1.798 m | — (**worse**, +35 % / +46 %) |
| corr(UP, BSHM ZWD deviation) | −0.161 | **+0.058** | < 0.4 **PASS** |
| vertical share of settled 3D SS | 84.4 % | **40.5 %** | — |
| T3 post-fit cell flags | 15 435 / 663 738 (2.33e-2) | **10 717 / 665 335 (1.61e-2)** | reported |
| **T3c consensus flags / opportunities** | 1 038 / 9 000 (1.15e-1) | **849 / 5 379** (1.58e-1) | reported |
| T4 ARC attribution flags | 1 187 (1.79e-3) | **952** (1.43e-3) | reported |
| T4 one-solve arc refloats (run-end census) | 2 159 | **1 750** | reported |
| within-epoch trigger (T2) flags | 41 | **2** | reported |
| consumer hourly rms h0..h11 | 3.83 1.63 1.90 3.17 4.06 2.33 2.98 **6.78** 3.67 3.59 2.39 3.28 | 5.34 2.49 1.27 1.55 2.52 3.23 4.78 2.25 1.95 2.83 2.12 2.67 | — |

Wiring, verbatim from `runs/fldf3_on2_zwdblk/console.txt` — *"Network (leveling local-block F3):
87313 block(s) whitened … **ZWD in block: 87313 col(s), 98 diagonal fallback(s), 297 missing cross
term(s), 87274 RAIM-pool admission(s) of the ZWD row**"* — i.e. the vehicle actually ran this time:
87 313/87 313 columns in the block, 0.11 % diagonal fallback, and the F3 interlock at
`GnssConfigLoadNetwork.cpp:8778-8784` is **silent**. `network_zwd.csv carried` = 87 392/87 819 =
99.5 % against the control's 0/87 608 (`carry_zwd` OFF there). Re-derived with
`scripts/field_consumer_zwd_grade.py <run> --settle-h 6`; every headline number above is that
script's output, not the console's.

⚠ **Not a same-build A/B.** The control's exe is from the `00d5fdf` worktree, the arm's is the HEAD
build after merge `7b32a74` (`gnss_processor_zwdblk_main.exe`, sha256 `319a6aed…`); the OFF-path
differences between them are byte-certified by the merge smokes, not by this pair. A same-build
control `runs/fldf3_on2_ctl_sb` is in flight and will be appended here.

⚠ **The T3c count fell but its RATE rose** (1.15e-1 → 1.58e-1) because opportunities nearly halved
(9 000 → 5 379). A T3c opportunity is a `(tow, satellite)` with at least one spiked cell, so the
halving *is* the quieting — 31 % fewer spiked cells over the same cell population — and the higher
rate says only that, conditional on a satellite spiking anywhere, more stations now agree. Both
readings point the same way; quote the counts with the opportunities attached.

**Reading, after four arms — one mechanism, and one inference that Arm F2 retracts.**
* **On its own channel the prior works, on every bed and every vehicle.** Quiet sim: ZWD truth
  5.7 mm and the diagonal regression NOT reproduced. Slipped sim: span −87 %, truth −43 %, integers
  up. Field, diagonal (F): the sick station can no longer park corrupt phase in its own ZWD
  (368.87 → 0.87 m). Field, in-block (F2): fleet span p50 1.195 → 0.143 m, the > 10 m tail empties
  (4 → 0), and the worst station in the fleet goes 491.5 m → 5.1 m. Four arms, two beds, three
  vehicles, same direction.
* **The "it RELOCATES the tension into a per-SATELLITE misattribution storm" inference was drawn
  from the DIAGONAL field arm, and Arm F2 does not reproduce it.** Arm F ran `carry_zwd` with the
  in-block sub-knob loader-disabled, so its T3c 6.9× / T3 2.7× storm is the **diagonal** row's — the
  double count of §7 item 17, asserting the station-common ZWD direction a second time on top of
  every carried ambiguity row. On the in-block F3 vehicle every QC tier goes the other way: T3 cells
  −31 %, T3c 1 038 → 849, T4a 1 187 → 952, T4 one-solve refloats 2 159 → 1 750, T2 41 → 2 — **quieter
  on the same field data**, while the consumer improves 27 % on 3D rms and the vertical DC bias
  (UP mean +2.83 m) goes to **+0.07 m**, with corr(UP, BSHM ZWD deviation) −0.16 → +0.06 and the
  vertical share of the settled 3D sum-of-squares 84.4 % → 40.5 %. So on the field there is **no
  storm to bound**: the in-block vehicle removes the vertical DC bias and quiets the QC at the same
  time. What still stands, unretracted, is the SIM-BED cost — the slipped campaign bed relocates
  into the satellite clock (contrast 8.05 → 11.70 m, iono⇄ZWD 2.46 → 3.03 m) — and that relocation
  is bed-specific evidence, not a field prediction.
* **What remains on the field is a 1.86 m UP wander plus ~1–1.8 m N/E, and it is unattributed.**
  The vertical is now a zero-mean wander (sd 1.861 m ⇒ rms 1.862 m) rather than a bias, which is why
  the pre-registered UP rms < 1.5 gate misses by 0.36 m and consumer 3D < 2.0 misses at 2.93 m.
  ⚠ The horizontal got **worse**, not flat: E rms 1.009 → 1.364 m, N rms 1.233 → 1.798 m, with the
  component means moving too (E −0.31 → −0.57, N +0.29 → +0.84). Pinning the wet delay took the
  vertical's dominance away (84.4 % → 40.5 % of the settled SS) and left a residual that is no
  longer mostly vertical — so the next lever must be graded on all three components, not on UP.
  Candidate levers, in the order the evidence supports them: **bounded identification** with a
  station-level quarantine response (§7 items 22/26 — still motivated, but by E1 and by the sim
  clock relocation, not by a field misattribution storm); a **young-arc age gate inside the block**
  (`network.phase_leveling_local_block_min_age_ep` > 0, the named-but-unrun sim arm); and a
  **level-preserving re-level** so the erases that drive the leak stop creating fresh code-level
  birth error in the first place.
* ⇒ **`network.carry_zwd_in_block` is promoted to RECOMMEND_SCOPED in regime `carry_zwd` AND
  `phase_leveling_local_block` (ledger, 2026-09-10):** whenever `carry_zwd` is armed on an F3
  lineage the in-block vehicle is **mandatory**, because the diagonal row under F3 is the falsified
  double count (quiet sim 0.723 m vs 0.0877 m; field diagonal T3c 7 619 vs in-block 849). The
  **pair** (`carry_zwd` + `carry_zwd_in_block`) is a RECOMMEND_SCOPED **candidate** for
  `phase_leveling` + F3 lineages — sim quiet consumer rms +7 % / p50 −13 % / ZWD −21 %, field F3
  consumer −27 % with the vertical DC bias removed — whose **golden pin awaits the user's decision**;
  the loader now emits a RECOMMEND line for the `carry_zwd` + F3 + in-block-OFF combination
  (`GnssConfigLoadNetwork.cpp`, log-only). `carry_zwd` itself stays **KEEP_OFF for the DIAGONAL
  vehicle** on every lineage.

#### 6.16.7e `code_datum_time_linked` OFF — the co-armed "inconsistent gauge" pair costs nothing measurable, and time-linking is load-bearing

`runs/golden_sim_tlink_off` = the sim golden with **`network.code_datum_time_linked: false`** plus
the trace-only `network.cycle_slip.gap_instrument: true` (flattened config diff run for this section:
those two keys, the output path and an `_comment_arm` are the only differences), same exe `gnss_processor_golden0.exe`, quiet 24 h
bed, 2 879/2 879 epochs, exit 0. Control = `runs/golden_sim_tri_w6_full` (§6.16.7a).

| settled h12–24 | ON (golden) | **OFF** | Δ |
|---|---|---|---|
| consumer 3D rms / p50 / max (m) | 0.0762 / 0.0604 / 0.2073 | **0.1013 / 0.0749 / 0.2708** | **+33 % rms (WORSE)** |
| consumer UP rms (m) | 0.0644 | 0.0862 | +34 % |
| sat code-bias residual rms / p50 / max (m) | 0.1482 / 0.0961 / 0.5231 | **0.4182** / 0.0146 / **3.7038** | rms 2.8× worse, p50 6.6× better |
| … per signal, worst two | E:C7Q 0.1376, G:C5Q 0.1578 | **E:C7Q 0.3092, G:C5Q 0.9022** | the tail is two signals |
| sum-to-zero datum check (emitted per-channel mean / rms) | +1.92e-2 / 6.23e-2 m | −2.59e-4 / 2.08e-2 m | OFF holds the datum tighter |
| sat clock residual rms (m) | 1.5005 | 1.5100 | flat |
| ZWD residual rms (m) | 0.0072 | 0.0073 | flat |

**Reading.** The two arms split cleanly: **ON carries a bias-like offset** (per-channel mean +1.9 cm,
p50 9.6 cm, max 0.52 m — a level the sum-to-zero datum does not hold at exactly), **OFF carries a
heavy tail** (p50 1.5 cm but max 3.70 m, concentrated on E:C7Q and G:C5Q). The consumer prefers ON by
a third on rms. **⇒ the §2.1 "inconsistent gauge" pair (time-linked s2z RHS + the primary GF code-bias
pin) injects NO measurable tension on this bed, and time-linking is load-bearing for the consumer:
keep it armed, and the loader WARN stays informational.** `SPEC_family_gf_datum` §2.1's "quantifying
it on a real arm is owed work" is CLOSED by this arm (see that document's §2.1 addendum).

**Rider, from the same arm's Card-G instrumentation.** The quiet bed reads **2 376 resumptions =
2 308 SHIFTED + 68 unshifted + 0 unmeasurable**, and of the shifted arcs **0 re-entered bridged of
4 616** — every one was fresh. ⇒ **the gap-poison class of `SPEC_slip_qc_hardening` §12 does not
exist on the simulator** (its only gaps are eclipse returns, far beyond the 120 s loader-default
leveling horizon), which is why that card is field-only until the simulator gets a dropout scenario.

---

## §6.17 The 2026-09-10 morning — the same-build control, three truth censuses, two falsified cards, and the field's phase-bias steps

Everything below is one morning's work on the two beds §6.16 opened: the slipped 60-station sim
campaign (`runs/simnet60_campaign_wu93`, 2 452 truth slips / 6 h) and the DOY-129 63-station field
chain. Nothing in §6.16 is edited. Seven results, in the order they landed; (a) closes an owed
control, (b)–(d) are truth censuses that redirect two open items, (e)–(f) are two falsified cards,
(g) is where the field actually is, (h) is what is in flight.

---

### 6.17a The F2 same-build control — the A/B is same-build by proof

§6.16.7d's Arm F2 compared `runs/fldf3_on2_zwdblk` (HEAD exe `gnss_processor_zwdblk_main.exe`,
319a6aed…) against `runs/fldf3_on2`, whose exe came from the `00d5fdf` worktree. The OFF path was
byte-certified by smokes, but the 12 h control was not same-build, and §7 item 24 recorded that as
owed.

`config/test_suite/_fldf3_on2_ctl_sb.json` re-runs `_fldf3_on2` unchanged on the HEAD exe
(63 sta, 12 h, ~7 min wall). The result is **byte-identical to `runs/fldf3_on2`**:

| artifact | `runs/fldf3_on2` | `runs/fldf3_on2_ctl_sb` |
|---|---|---|
| `products.txt` sha256 | `18aa2854b2575658…` | `18aa2854b2575658…` |
| `logs/closed_loop_trace.csv` | — | identical |
| `logs/network_zwd.csv` | — | identical |
| `logs/network_flag_rates.csv` | — | identical |

⇒ **the F2 A/B is same-build by proof**, not by smoke inference, and every number in §6.16.7d stands
as a single-knob-pair contrast. §7 item 24's last open clause is closed.

---

### 6.17b The T4 erase census, truth-graded — T4 re-erases the damage its own previous re-level created

`scripts/simnet_t4_census.py` (`--selftest` 6/6) joins `logs/network_events.csv`'s `T4_attrib /
attrib_arc` rows to the DD-fraction trace and to `truth_slips.csv`, on `runs/golden_sim_campaign`
(E0) and `runs/golden_sim_campaign_zwdblk`. Both the as-traced and the ref-free DD statistic were
run (§6.12); the two agree on every share, so the as-traced numbers are quoted.

**⚠ METHOD — the slipped bed has no clean floor, so calibrate against SAME-EPOCH quiet arcs.** The
bed's ambient per-band DD error is **1.699 cyc** (median over a 3 000-arc control of arcs with no
re-level within ±600 s; 1.729 on the strict variant that admits no T0/T1/T4 row at all; 1.653 as the
epoch-median reference used in the age profile). An absolute 0.5-cyc "OFF" cut therefore classifies
**83 %** of untouched QUIET arcs as OFF. Every share below is a ratio to that same-epoch control;
a raw cut on this bed measures the bed, not the event.

**Population.** 7 656 T4 events = **22 968 arc erases** (the events CSV is per (station, satellite),
signal-blind; each event erases 3 signals). The DD trace covers 42 of 60 stations, and 2 043 events
(26.7 %) have ≥ 3 chords on ≥ 1 signal at t₀−30 s, i.e. are *measurable*; the event rate is unbiased
across the covered/uncovered split. Truth-slip proximity: **1.4 %** of events sit within 90 s of a
truth slip on that (station, satellite), **4.9 %** within 10 min.

**The discriminator — is the erased arc actually damaged?**

| population | n | med \|e_pre\| | ratio to same-epoch control | > 2× | control quantile | **below the control MEDIAN** |
|---|---|---|---|---|---|---|
| T4-erased arcs | 2 043 | **4.546** | **2.59** | 55.7 % | 0.781 | **29.0 %** |
| detector refloats | 993 | 2.585 | 1.58 | 41.3 % | 0.640 | 38.1 % |
| quiet arcs (control) | 3 000 | 1.699 | 1.00 | — | 0.50 | — |

So T4 is **not** firing at random — the median erased arc is 2.6× the ambient — but **29 % of its
erases land on arcs the same epoch's untouched population cannot distinguish from healthy** (H-B
false alarms).

**It is not fresh damage.** Censoring at the arc's own previous re-level, the erased arc was already
at that level 10 min earlier (med \|e\| then 3.048 vs 3.176 at t₀; grew in 44.5 % of cases — a coin
flip) and 20 min earlier (2.656 vs 2.574, grew 44.9 %). T4 fires on a **chronically** bad arc.

**H-C IS THE MECHANISM — the re-level VALUE is worse than what it replaced.**

| chain | n | med gap | med \|e\| BEFORE 1st erase | AFTER 1st erase | BEFORE 2nd erase | 1st erase made it WORSE |
|---|---|---|---|---|---|---|
| T4 → T4 on one (S,s) | 990 | **210 s** | 8.345 | **13.197** | 14.082 | **64.4 %** |
| any re-level → any | 1 811 | 120 s | 4.594 | 6.082 | 5.288 | 56.8 % |

**48.4 %** of all T4 events fire within 15 min of a previous re-level on the same (station, signal)
(cross-tab: 1 161 < 2 min + 846 at 2–5 min + 1 701 at 5–15 min of 7 656). Improvement 10 epochs
later is a coin flip for the OFF class (**52.3 %**) and essentially never happens when the arc was
FINE (**1.7 %**, n = 59) or AMBIGUOUS (22.1 %).

**The re-level profile, on a FIXED cohort** (only events with no next re-level anywhere in the
window — the censored profile drops re-hit arcs as k grows, which is survivorship bias):

| k (epochs since the erase) | 0 | 1 | 5 | 10 | 20 | 40 | ambient |
|---|---|---|---|---|---|---|---|
| med \|e\| (cyc) | 3.031 | **4.192** | 3.195 | 2.877 | 1.885 | **2.059** | **1.653** |

An erase injects ≈ 2.5× ambient at k = 1 and walks back over ~20 min to a level that **never reaches
the floor** (k=1 → k=40 ratio 0.50, against 1/√40 = 0.16 for full averaging and 1.00 for frozen).

**The zwdblk twin says the same thing with the ZWD pinned.** `runs/golden_sim_campaign_zwdblk`:
9 871 T4 events (**+29 %**), T4 → T4 med gap 120 s, and the chain's injected error is **half** —
3.529 → 5.429 → 4.529 against E0's 8.345 → 13.197 → 14.082. Pinning the wet delay removes half of
what a fresh re-level dumps, and buys more erases.

**⇒ VERDICT: BOUNDED IDENTIFICATION IS THE WRONG LEVER.** §7 item 22 opened on "T4 erases good arcs;
bound the identification with a separability test". The census says T4 mostly confirms genuinely-off
arcs — whose error **its own previous re-level created**. The disease is the re-level VALUE
inheriting the station's corrupted per-epoch states (ZWD / iono / clock), i.e. a station-wide
positive-feedback loop, not the identification. Item 22 is superseded (§7).

---

### 6.17c The consumer's float, joined to truth — the lanes are PRECISE and the offset is a BIAS

`scripts/consumer_float_truth_join.py` joins `logs/ambiguity_trace.csv` from
`runs/golden_sim_consumer_ar_a` (§6.16.7b, quiet 24 h bed, SZZ000SIM) to `truth_ambiguities.csv`,
forms within-constellation between-satellite SDs (only those are comparable — the satellite phase
OSB does not cancel between satellites, the receiver bias and the ISB do), and splits the wrapped
residue into **within-arc scatter** (what averaging removes) and **per-arc offset**
(`|wrap(arc mean)|` — the irreducible part).

Settled h12–24, one fixed reference per constellation:

| channel | n_arcs | arc len (ep) | within-arc std p50 | **\|offset\| p50** | off < 0.15 |
|---|---|---|---|---|---|
| GPS EWL (L2W−L5Q) | 25 | 264 | **0.0260** | **0.1734** | 44 % |
| GPS WL (L1C−L2W) | 25 | 264 | 0.0845 | 0.2895 | 24 % |
| GPS NL | 35 | 112 | 0.0528 | 0.2188 | 34 % |
| GPS L1C / L2W / L5Q | 24–25 | 264 | 0.203 / 0.223 / 0.227 | 0.244 / 0.281 / 0.273 | 24–29 % |
| Gal EWL (L5Q−L7Q) | 14 | 521 | **0.0148** | 0.2951 | 29 % |
| Gal WL (L1C−L5Q) | 14 | 518 | 0.0545 | 0.2507 | 29 % |
| Gal NL | 20 | 171 | 0.0553 | 0.2423 | 35 % |
| Gal L1C / L5Q / L7Q | 13–14 | 510–598 | 0.193 / 0.182 / 0.193 | 0.244 / 0.287 / 0.295 | 21–38 % |
| *uniform null* | | | | *0.2500* | *30 %* |

**Read it in two halves.** The lanes are already precise **within** an arc — GPS EWL 0.026 cyc,
Galileo EWL 0.015, WL 0.05–0.08, NL 0.053 — so nothing about noise blocks a fix. But **every**
channel's per-arc offset sits at the uniform null, and only GPS EWL is partly concentrated (0.173,
44 % inside ±0.15). That is a **BIAS, not noise**: more integration cannot remove it. The consumer's
own per-band float σ confirms the other half — SD σ **0.41 / 0.51 / 0.53 cyc** (GPS L1C/L2W/L5Q) and
0.48 / 0.63 / 0.62 (Galileo) after 12 h at a cm-level static position, which is why AR dies at the
ratio battery (2 879 attempts, 0 fixes, ratio p50 1.01, `ratio_battery` 2 596; §6.16.7b).

**⇒ the consumer-side WL→NL cascade of §7 item 27 is DEAD ON ARRIVAL as specified.** The cascade's
WL would come from `computeSatMwBias`'s MW SD wide-lane, which subtracts *the wire's own* satellite
MW bias — the same biases that produce the uniform per-arc offsets above. It would lock onto wrong
integers with a high ratio, which is exactly the Jul-30 user-path failure
(`project_pppar_user_control`: FAIL-UNSAFE, never trust its fix count). The order flips: **the wire's
SD integer consistency has to be established first.** Rider, same run: 2 274 SSR phase-bias resets
(0.79/epoch) + 1 513 code-bias discontinuities + 47 clock discontinuities restart consumer arcs
continuously (median settled arc 2.2 h).

---

### 6.17d The wire, joined to truth directly — every channel is at the null, and the gauge is the ANCHOR's FLOAT

`scripts/simnet_wire_integer_consistency.py` removes the consumer from the question. It forms
`E = [clk_emitted + P_emitted] − [clk_true + osb_true]` per (satellite, signal, epoch) on the same
quiet bed, differences between satellites within a constellation, and grades the wrapped fraction.

**⚠ Fixture semantics, both of which bite.** Satellite-clock truth is written only when
`sat_clk_err.active()`, so `truth_satclock.csv` is all-zero on this fixture; OSB truth is
`truth_bias.csv`'s `osb_m` **on the phase code** (the code rows are a different quantity).

**Result 1 — the wire is at the uniform null on every channel** (settled h12–24, p50 |wrapped SD
fraction|, uniform null 0.2500; n = 43 824 GPS / 29 424 Gal):

| GPS | L1C | L2W | L5Q | WL | EWL | NL |
|---|---|---|---|---|---|---|
| p50 \|f\| | 0.2372 | 0.2854 | 0.2726 | 0.2507 | 0.2382 | 0.2712 |

| Galileo | L1C | L5Q | L7Q | WL | EWL | NL |
|---|---|---|---|---|---|---|
| p50 \|f\| | 0.2652 | 0.2149 | 0.2337 | 0.3143 | 0.2200 | 0.2070 |

Undifferenced it is the same story: `dP/λ` p50 |frac| 0.227–0.271 across all six (satellite, signal)
channels, and `W/λ = (dC − dP)/λ` — what a consumer's ZD ambiguity absorbs — 0.24–0.31.

**Result 2 — the consumer inherits the wire EXACTLY, and the sign is MINUS.** Both sides
re-differenced against the same per-epoch reference, UNWRAPPED (the wrapped test cannot see this: a
> 1-cycle float scatter fills the circle regardless):

| | slope⁻ | Pearson r⁻ | slope⁺ | Pearson r⁺ | resid sd |
|---|---|---|---|---|---|
| mean over 6 channels | **+0.888** | **+0.936** | −0.79 | **−0.859** | **13.7 cyc** |

The residual scatter *is* the consumer's own float error (§6.17c). So `W = dC − dP` is the right
convention — the consumer adds −C and +P, and its float absorbs λN_true + (C − C_true) − (P −
osb_true).

**Result 3 — the offset is a STABLE per-(sat, band) gauge, and it is nobody's integer.** Within one
PBGAUGE/PHASE_ANCHOR tenure the wrapped fraction is a constant to **0.024–0.036 cyc** circular std
(settled; tenures ≈ 86 ep GPS / 148 ep Galileo, 295 / 133 steps), stepping by 0.030–0.057 cyc p50 at
tenure boundaries (p90 up to 0.38). Inverting the gauge — `n = round(−dP/λ)`, then asking which of
the 61 stations' `truth_ambiguities.csv` holds that integer:

| channel | \|dP/λ\| p50 (cyc) | share \|wrap\| < 0.25 | **== SOME station's truth N** | **== the CURRENT anchor's** |
|---|---|---|---|---|
| GPS L1C | 28.1 | 0.516 | 0.703 | **0.004** |
| GPS L2W | 15.9 | 0.461 | 0.753 | **0.031** |
| GPS L5Q | 22.8 | 0.480 | 0.728 | **0.011** |
| Gal L1C / L5Q / L7Q | 24.5–27.7 | 0.497–0.529 | 0.50–0.74 | **0.000–0.003** |

⇒ **the emitted phase bias is 16–28 cycles from truth with a uniform fraction**, and the integer it
carries is *a* receiver's — almost never the current anchor's (`network.phase_datum_prioritized`
pins each (sat, signal) at its own highest-coverage receiver, and the sticky handover keeps an
earlier frame). Splitting the identical population on same-anchor vs different-anchor (STEP 4c)
gives large R at large |μ| on a population of 6–7 satellites — **a common non-integer offset, not
integer content**.

**Result 4 — the shape, made identifiable.** The three-band model `dE_b = r + γ_b i + λ_b n_b + ε_b`
fitted on WRAPPED fractions is **vacuous**: 3 obs − 2 params − 3 free integers = 1 dof, and a
size-matched Monte-Carlo null of independent U(−0.5, 0.5) fractions fits equally well. Fit it on
consecutive-epoch DIFFERENCES instead (integer jumps excluded, 0.05 % of pairs) and it becomes exact:

| settled | full (r, i) | range-only | iono-only | null |
|---|---|---|---|---|
| GPS R² / resid | **0.9999** / 0.31 mm-cyc | 0.9887 | 0.9589 | 0 |
| Galileo R² / resid | **0.9996** / 0.42 mm-cyc | 0.9838 | 0.9594 | 0 |

with \|dr\| p50 **4.1 mm** / 30 s and \|di\| p50 **0.15 mm** (GPS; 3.1 mm / 0.27 mm Galileo).

**⇒ THE READING THE DATA SUPPORTS.** The wire's between-satellite motion is a pure per-satellite
two-factor range+iono offset — no band-specific content above 0.1 mm — sitting on a per-(sat, band)
level that is **an arbitrary receiver's float ambiguity**, held to 0.03 cyc within a tenure and
re-elected at every handover. The non-integer budget of the emitted product is therefore **the
anchor's own float error**, and no amount of consumer-side work reaches it. This is the object
`docs/network_ambiguity_resolution_design.md` Phase 3 exists to replace (float-valued `w=1e10`
pivot pins, `ApplicationNetworkAssembly.cpp:2559`; handover pins at the successor's float,
`NetworkPhaseDatum.cpp:335-428`).

---

### 6.17e The ORACLE slip response — the mechanism is exact, the plateau does not move, and that is the finding

Given 6.17b, the alternative to bounding T4 is to fix the **re-level value**: preserve the level
across a detected slip instead of freeing the arc. Building that honestly (EWL/WL-MW integer
estimation of the jump, `SPEC_lane_slip_response`) is expensive, so its **ceiling** was measured
first. `network.cycle_slip.oracle_relevel` (sim-only; `include/slip/NetworkSlipOracle.hpp`,
`src/core/slip/NetworkSlipOracle.cpp`, 19 tests, OFF byte-identical `D8D94923…`) looks each
detector actuation's TRUE integer jump up in the fixture's `truth_slips.csv` and shifts
`LocalCarry::value_abs` by exactly Σδ·λ instead of freeing the arc. Everything else — the detector
population, the gap re-entry drain, the T4 erase path — is untouched. Full knob contract:
`docs/SPEC_slip_qc_hardening.md` §13.

**The mechanism is exact.** 1 161 repairs; \|δ\| applied p50 50 cyc (p10 26, p90 87); detection
latency 0 for **1 124 of 1 161** (so a pre/post slope test is uninformative — the pre-sample is
already post-slip on the other 37 — and the ratio statistics below are the instrument):

| statistic | value |
|---|---|
| **unrepaired fraction** \|e(t₀) − e(t₀−30)\| / \|δ\|, latency-0 | **p50 0.005** (0.239 cyc), p90 0.020 |
| \|e(t₀)\| / \|δ\| (0 = repaired, 1 = inert, 2 = wrong sign) | **0.070** |
| \|e(t₀+150 s)\| / \|δ\| | **0.078** |

**The slip-side population split** (2 452 truth slips, and what answered each):

| answered by | n | share | \|e\|/\|δ\| at t₀ | at t₀+150 s |
|---|---|---|---|---|
| **REPAIRED** by the oracle | 1 161 | **47.3 %** | 0.073 | 0.076 |
| detected, **KEPT** on every row (window/keying missed it) | 627 | **25.6 %** | 0.892 | 0.112 |
| **FREED** — a no-carry `refloat` row (the entry was gone) | 520 | **21.2 %** | 0.943 | 0.096 |
| **SILENT** — no detector row on (S,s) in the window | 144 | 5.9 % | 0.483 | 0.077 |

Of the 2 697 `oracle_keep` rows, 603 are the partner band of a repair at the same (t₀, S, s), 430
are the same slip already repaired at an earlier epoch (the `last_applied_s` bound, correct), **0**
are a carry re-born by a T4 erase after the slip (the `birth_s` floor, by design), 69 are standalone
with a truth slip within ±600 s, and only **8 are genuine data-domain detector false alarms**. The
remaining 1 587 are unexplained misses whose arcs sit at \|e\|/\|δ\| ≈ 0.889 at t₀ — never repaired —
which is the 25.6 % KEPT class seen from the event side. **That is an open defect of the ceiling
instrument, and it biases the ceiling DOWNWARD.**

**And the plateau did not move.**

| settled | E0 (`golden_sim_campaign`) | E0 + ORACLE | zwdblk | zwdblk + ORACLE |
|---|---|---|---|---|
| ZWD truth rms (m) | 2.098 | **1.779** | 1.188 | 1.104 |
| consumer 3D p50 / rms (m) | 1.752 / 1.799 | 1.736 / 1.896 | 1.085 / 2.389 | **2.234** / 3.493 |
| ambient per-band DD \|e\| (cyc) | 1.653 | **2.429 (+47 % WORSE)** | 1.654 | 1.892 |
| T4 `attrib_arc` events | 7 656 | **13 562 (+77 %)** | 9 871 | 16 095 |
| detector re-levels | 4 545 | **687 (−85 %)** | 4 545 | 717 |
| ref-free WL(1,2) at own integer | 81.5 % | 81.5 % | 86.2 % | 85.9 % |
| ref-free per-band L1C at own integer | 18.1 % | **11.2 %** | 21.0 % | 16.1 % |
| sat-clock contrast rms (m) | 8.050 | 7.777 | 11.791 | 11.806 |

Every pre-registered gate FAILS (ZWD ≤ 0.5 m; ambient ≤ 0.30 cyc; T4 ≤ 1 500; consumer p50 ≤ 0.50 m;
sat-clock ≤ 3 m; WL ≥ 95 %). The mechanism actuated (P1: 1 161 applied) and the wire got *worse* on
the two channels that matter most.

**⇒ THE (ρ, I) SPLIT FREEZE — the reading the data supports.** Detector re-levels fell 85 % while T4
grew 77 %: **arcs restored to their true integer are then rejected by the corrupted MAJORITY.** The
per-band DD errors are equal in CYCLES while the lanes are clean — E0 ref-free medians L1C 0.896,
L2W 1.086, L5Q 0.980, L7Q 0.816 cyc against EWL(2,5) **0.032** and WL(1,2) **0.141** — and an error
that is a constant ε cycles on every band is exactly a (range, iono) split, not an ambiguity error:
solving `δρ + γ_b δI = ε λ_b` on the L1/L2 pair gives (δρ, δI) = (0.1069, 0.0834)·ε metres, which
predicts L5's own per-band error to **0.6 %**. So the station's zero-q levels are frozen around a
*self-consistent* mis-split of range against ionosphere — the (I, N, ZWD) trade of §6.16, latched at
every re-level — and a single-arc level preservation, however perfect, cannot undo a station-wide
one. **Level preservation is the CEILING of half a fix.**

**THE DISCRIMINATOR — oracle + E1 (2026-09-10 11:15, `runs/golden_sim_campaign{,_zwdblk}_oracle_e1`,
both exit 0, 719/720 epochs emitted, 1 A4 σ̂₀ ceiling skip).** The pre-registration (written into the
arm configs' `_comment`, before the runs): **ZWD settled truth rms ≤ 0.5 m PASS** = the plateau is
the T4 chain re-freezing the split on top of the re-levels; 0.5–0.9 PARTIAL; **≥ 0.9 (no better than
E1 alone) FAIL** = the freeze is independent of both. `network.raim_arc_persist_epochs: 1000000`
took: `T4_attrib/attrib_arc` rows are **0** in both arms.

| settled | E0 | **E1** (T4 starve) | oracle | **oracle + E1** | zwdblk | zwdblk + oracle | **zwdblk + oracle + E1** |
|---|---|---|---|---|---|---|---|
| **ZWD truth rms (m)** | 2.098 | **0.933** | 1.779 | **1.819** | 1.188 | 1.104 | **1.239** |
| consumer 3D p50 (m) | 1.751 | 2.559 | 1.730 | **3.781** | 1.084 | 2.234 | **3.334** |
| consumer 3D rms (m) | 1.799 | — | 1.896 | **4.909** | 2.389 | 3.493 | **4.294** |
| ambient per-band DD \|e\| (cyc) | 1.65 | — | 2.43 | **6.09** | 1.65 | 1.89 | **5.66** |
| T4 `attrib_arc` events | 7 656 | 0 | 13 562 | **0** | 9 871 | 16 095 | **0** |
| detector re-levels | 4 545 | — | 687 | **275** | 4 545 | 717 | **275** |
| ref-free WL(1,2) at own integer | 81.5 % | 65.4 % | 81.5 % | **35.4 %** | 86.2 % | 85.9 % | **39.7 %** |
| ref-free EWL(2,5) | 99.0 % | — | 98.6 % | **75.5 %** | 99.2 % | 99.0 % | **74.7 %** |
| ref-free per-band L1C | 39.1 % | — | 29.8 % | **19.9 %** | 44.0 % | 38.0 % | **20.7 %** |
| sat-clock contrast rms (m) | 8.050 | 2.76 | 7.777 | **3.159** | 11.791 | 11.806 | **3.243** |
| sat code-bias residual rms (m) | 0.608 | 0.29 | 0.537 | **0.362** | 0.678 | 0.638 | **0.356** |

**VERDICT: FAIL, and worse than that.** Both arms are ≥ 0.9 m, so the pre-registered FAIL branch
fires — *the split freeze is independent of both mechanisms*. But the arms are also **worse than E1
alone on every channel E1 improved**: ZWD 0.933 → 1.819, consumer p50 2.559 → 3.781, WL 65.4 →
35.4 %. Only the two channels E1 owned outright — sat-clock contrast and code bias — kept E1's gain
(8.05 → 3.16 m, 0.61 → 0.36 m). **The two "fixes" are anti-synergistic.**

**And the ceiling instrument got BETTER while the result got worse**, which removes the obvious
objection. With T4 starved the oracle repairs **1 549 of 2 452 truth slips (63 %, up from 1 161 =
47 %)** on an *identical* detector population (13 485 actuations in both arms), and the `no_carry`
fall-through — 6.17e's 21.2 % FREED class — collapses **3 149 → 1 396 (−56 %)**, with the
`birth_s` lower bound binding on 273 instead of 1 041. **That confirms the FREED class was T4
deleting the carry out from under the repair**; removing T4 recovers it, and the plateau still does
not move. Slip DETECTION is flat at **94.0 %** on every arm (`simnet_slip_census.py`) — nothing here
is a detection story.

**⇒ THE LEVER IS THE LEVEL'S UNCERTAINTY ALONG THE SPLIT DIRECTION, NOT THE RESPONSE.** With every
erase starved AND every detected slip repaired to truth, the ambient per-band DD error **rises to
6.09 cyc** (E0 1.65) while the lanes hold better than the bands. Nothing is left to disturb the
frozen (ρ, I) split, so it compounds: T4's erases, destructive as 6.17b shows them to be, were also
the only mechanism periodically re-randomising it. A zero-q level that is *individually* correct is
still wrong *jointly* — the station's per-LOS ionosphere and its levels share a direction the data
does not pin, and neither a perfect detector response nor removing the erase tier addresses that.
The candidate build is a **per-LOS ionosphere carried IN-BLOCK** (the same vehicle distinction the
ZWD card just paid for, §6.16.7d) — ⚠ **re-read `project_iono_carry_8b_campaign`'s four
falsifications for the double-count reading before building it**; a diagonal per-LOS iono row is
exactly the shape that was falsified there, and §6.16.7d's whole finding is that the diagonal
vehicle is the double count. §7 item 33.

---

### 6.17f PB-gauge LEVELLING — FALSIFIED as built (branch `worktree-agent-a7e2586003a605548`, commit `9b59a2c`, NOT merged)

6.17d says the emitted phase bias carries an arbitrary receiver's float. The obvious emission-side
answer is to estimate the common fractional part per (sat, signal) — δ̂ = circular mean over the
carried stations of the SD fraction against a sticky reference satellite, unwrapped EWMA — and emit
`P' = P + λδ̂`. Built as `network.emit_pb_gauge_level` (+`_alpha` 0.2, `_min_stations` 5,
`_min_concentration` 0.6, `_trace`) and validated `runs/golden_sim_pbgauge` vs `_ctl`, quiet 24 h,
single knob, consumer `hold_variance` 1e-6, graded with the two truth joins above.

**The mechanism actuated and changed nothing it exists to change.** 347 683 of 457 392 P records
(76.0 %) adjusted across 143 families, mean \|δ\| 0.65 cyc — and:

| pre-registered gate | result |
|---|---|
| G1 wire SD fraction p50 \|f\| 0.25 → ≤ 0.08 on **every** band | **FAIL — 0 of 12 channels.** ON wins 6, loses 6; best is Gal NL 0.138, the six primary bands 0.228–0.280 |
| G2 consumer SD arc-offset p50 → ≤ 0.10 | **FAIL — 1 of 12** (GPS EWL 0.173 → 0.036); 5 of 12 got WORSE |
| G3 consumer fixes > 0 at p_succ ≥ 0.999 | **FAIL — 0 fixes in BOTH arms**, ratio max 1.21 vs a 3.0 threshold |
| G4 float consumer 3D rms unchanged ±10 % | **FAIL — 0.0761 → 0.1037 m rms (+36 %), p50 0.0603 → 0.0907 (+50 %)** |
| G5 PB-disc resets ≤ +10 % | PASS (2 274 → 2 313, +1.7 %) |
| P1 mechanism-alive | PASS |

Worse, the wire's **within-tenure** circular std rose **1.1–1.9×** and its tenure-to-tenure step
rose up to 2.1×: the gauge added wander instead of removing an offset.

**Two independent root causes, both visible in its own trace.**

1. **The premise is only weakly true.** The carried per-station SD fractions have R p50 **0.572** at
   n ≥ 5 — against ≈ 0.41 for pure uniform noise at n = 6 — and **the carry covers a median of 6 of
   61 stations** per family per epoch. Three quarters of all innovations never reach the EWMA:
   39.0 % rejected `n < 5`, 36.2 % rejected `R < 0.6`, 24.9 % accepted. The "network-common
   fractional part" being measured is largely the estimator's own scatter.
2. **The unwrapped EWMA is a RANDOM WALK.** Per-epoch \|δ step\| is tiny (p99 0.039 cyc, well inside
   the α/2 = 0.1 bound; only 0.70 % of epochs above 0.05), but the never-wrap design integrates a
   persistently-signed innovation: per-family \|δ\| span p50 **1.146 cyc**, p90 2.52, **max 7.67**;
   **half of all families walk more than a full cycle over 24 h**; 19 families exceed 2 cyc.
   G03/L1C runs −0.06…−0.08 cyc/epoch for six straight hours under ONE unbroken reference (G09, no
   switch) and arrives at −7.63 cyc = **−1.45 m injected into the emitted L1C phase bias**. It is
   uncorrelated with PHASE_ANCHOR churn (20.18 events/sat on the big-δ families vs 20.78 on the
   others) — an integrator property, not an event-response failure.

**⇒ KEEP_OFF, FALSIFIED as built.** The lesson generalizes: an emission-side gauge estimate needs a
**bounded/wrapped** δ̂ with per-epoch authority and a much higher concentration bar — and even a
perfect one would not have been fixable here, because the consumer's own float is 13.8 cyc sd
(6.17c/6.17d) and AR never reaches p_succ. The branch is **not merged**; it survives as the vehicle
for the field CONTINUITY mode (6.17h), which is a different mechanism with no integer content.

---

### 6.17g The FIELD after the ZWD fix — one wandering direction, and phase biases that step by METRES

**The consumer residual is ONE slowly-wandering axis** (`scripts/field_consumer_residual_census.py`,
`runs/fldf3_on2_zwdblk` vs the control `runs/fldf3_on2`, settled ≥ h6):

| | in-block (`fldf3_on2_zwdblk`) | control (`fldf3_on2`) |
|---|---|---|
| PC1 share of settled ENU variance | **0.733** | 0.767 |
| PC1 direction | **el 49.8° / az 9.3°** (NNE) | el 73.2° / az 137.3° |
| corr(N, UP) | **+0.822** | −0.390 |
| UP ACF < 0.5 | **11.5 min** | 13.0 min |
| UP sd(30-min block means) / sd(within block) | 1.466 / 1.232 (**59 %** of UP variance is between blocks) | 2.038 / 1.389 |
| site ZWD deviation sd | **0.0101 m** | 0.1247 m |
| fleet ZWD span p50 / max | **0.143 / 5.12 m** | 1.195 / 491.5 m |
| AR fixes (settled) | 0 | 0 |

The axis points into the site's own constellation void (zero LOS-epochs at az 315–45°, el 30–60°),
i.e. it is the float solution's weakest direction. **The tropospheric channel is CLOSED**: fleet
ZWD deviation sd 21.5 → 4.9 mm, regressing the site's own ZWD out of UP removes 1.8 % of the
variance, and the slope (+33.8 m/m) is nowhere near mapping-collinear. ⚠ **Block-level "drivers" do
not survive**: the top Spearman correlations at 30-min block resolution (`n_dw_engaged` +0.82,
`PBGAUGE` −0.74, `zwd_fleet_dev` −0.72) are slow levels whose autocorrelation-corrected p is
0.24–0.37; the fast per-epoch counters (arc refloats, T4, `n_eff` ≈ 304) find nothing.

**So the instrument moved to the wire** (`scripts/field_wire_phase_clock_census.py` — truth-free
30-min local-linear smoothness of clk and clk+P per satellite, clipped at every PBGAUGE /
PHASE_ANCHOR / retro-bump / pivot / datum-change / absence boundary, per-epoch trimmed mean removed
as the datum). On `runs/fldf3_on2_zwdblk`, settled 6 h, 32 GPS satellites, 22 725 sat-epochs:

| channel | within-tenure deviation rms p50 | p90 | max | × COD-final's own floor |
|---|---|---|---|---|
| **clk** | **0.0256 m** | 0.082 | 0.127 | 3.9× (floor 0.0066 m) |
| pb_L1C | 0.2817 | 0.645 | 1.56 | 42.6× |
| pb_L2W | 0.6345 | 1.555 | 6.79 | — |
| **pc_IF** (= clk + P, IF) | **0.4642** | **2.297** | **10.535** (G06; G12 4.62) | **70.2×** |

**The emitted CLOCK is smooth — essentially at the final-clock floor — and the emitted PHASE BIASES
STEP BY METRES.** Decomposing: cutting every within-tenure one-epoch step > 0.50 m removes **86.3 %**
of pb_L1C's variance, **91.9 %** of pc_L2W's and **69.2 %** of pc_IF's, against only 27.5 % of the
clock's. These are **unmarked** steps by construction (the one-epoch differences are re-centred on
their own cross-satellite median, so anything constellation-common is invisible), i.e. invariant #7
edges with no event: 1 012 pc_IF steps > 0.25 m in 6 h = **5.3 per satellite per hour**. The largest
are network-wide — 26 satellites at tow 282150 (events: T4_attrib ×4), 18 at 283770 (T4_attrib ×7),
12 at 299490 (**T4_attrib ×22, and nothing else**).

**The handovers are not continuous either.** In the 5 min after a PBGAUGE (349) or PHASE_ANCHOR
(382) event the deviation is **6.9 / 6.5 m rms against 1.4 m elsewhere — ×5**. The successor's
variance exceeds `handover_max_var` 0.01, so the "continuous" handover cold re-realizes at a
metre-class float. Every such step trips the 0.25 λ discontinuity test ⇒ a consumer ambiguity
RESET ⇒ the field consumer never holds an arc (665 resets / 12 h, §6.16.7c).

**⚠ The geometry closure FAILS, and that is informative.** Projecting each satellite's wire wander
through the consumer's own geometry (`G = [−e_ENU, 1]`, `W = diag(sin²el)`, four preparations ×
two trims) gives corr(predicted, actual) ≈ 0 in E/N/U on every combination (|z| < 2.2 everywhere;
`n_eff` 15–369 after Bartlett correction), and the predicted axis is el 76–79° against the measured
el 49.8° / az 9.3°. Only the geometry-free magnitude link survives, weakly: corr(rms visible
\|δ\|, \|consumer error on the axis\|) = **+0.396**, |z| 2.05. ⇒ **the consumer's 12-min wander is
NOT a linear projection of the wire's fast (ACF 1–2.5 min) bias wander — it is the consumer's
perpetual RE-CONVERGENCE after each reset, expressed in its weakest direction.** Two independent
sanity controls hold: a white-in-time surrogate with the same per-satellite rms predicts 25.8 m on
the axis (real wander 2.98 m), and adding a ZWD column to the design inflates the prediction rather
than absorbing it.

---

### 6.17h What is in flight (pre-registered; results append here, nothing above is edited)

1. **Field emission CONTINUITY** — `network.emit_pb_gauge_mode: continuity` on the same branch as
   6.17f, but a different mechanism with **no integer content**: a per-family emission offset that
   absorbs every frame step (0.10–40 cyc) so the emitted bias is continuous across handovers and
   mass refloats. The satellite's true bias is constant and the gauge is arbitrary and common to
   every station's product, so absorbing the step is free. Arms `_fldf3_on2_zwdblk_pbcont` vs
   `_logctl`; pre-registered: consumer resets −70 %, pc_IF within-tenure deviation 0.46 → ≤ 0.10 m,
   consumer 3D 2.93 → ≤ 2.0 m. Note `emit_pb_filter` (the v1 continuity KF) is structurally OFF on
   the leveling vehicle — it is nested under `emit_rebase`, which is `carry_invariant`-only.
2. **Phase-2 producer AR, re-run with a gradeable trace.** `NetworkProducerAr` /
   `network.ambiguity_resolution.*` exists and was falsified only on the pre-F3 bed (near_int 34 %);
   the quiet golden now carries 87 % ref-free DDs at the integer. It is **log-only** today, so a
   trace (`network_producer_ar.csv`: per-DD float, fixed, ratio, and the pair identity) is a
   prerequisite for grading its fixes against `truth_ambiguities.csv`. See §7 item 30.

---

## §6.18 The 2026-09-10 afternoon — the level-memory and T4-persistence ladders, the wire wander, and the continuity card

§6.17e closed the morning with one open lever: the (ρ, I) split frozen into the zero-q levels, and
the two candidate ways at it — the levels' MEMORY (soften the prior and let the split move) and the
T4 erase tier (stop chopping arcs before they converge). The afternoon ran both as config-only
ladders on the slipped bed, graded the second one ON THE WIRE, and ran the field continuity card of
§7 item 31 three times. Five results: (a) the level-memory ladder FAILS four ways; (b) the
persistence ladder inverts — fewer erases help the producer and hurt the consumer; (c) the wire
grade says why, and re-points the next build; (d) the field continuity card is KEEP_OFF twice, for
a reason that is in the TRIGGER and not in the plumbing; (e) an addendum to §6.17g's field census.
Nothing in §6.16 or §6.17 is edited.

---

### 6.18a The level-memory ladder — softening the levels does not release the split (FAIL ×4)

Two knobs, both value knobs on the leveling carry, both default 0 (= off):

| knob | default | loader | what it does |
|---|---|---|---|
| `network.phase_leveling_q_amb` | 0.0 m²/s, `include/config/GnssConfig.hpp:6766` | `src/core/config/GnssConfigLoadNetwork.cpp:3098-3099` | process noise inflating the carried prior variance over the inter-epoch gap; 0 = pure zero-process-noise accumulation (random-constant) |
| `network.phase_leveling_var_floor_m2` | 0.0 m², `include/config/GnssConfig.hpp:6769` | `src/core/config/GnssConfigLoadNetwork.cpp:3105-3106` | floor on the carried prior variance (caps Λ — the lock-in guard); 0 = no cap |

Four single-knob arms off E0 (`runs/golden_sim_campaign`, the F3 golden posture on the slipped
60-station campaign bed `runs/simnet60_campaign_wu93`, 720 ep / 6 h, 2 452 truth slips), graded
whole-run with `simnet_grade.py`:

| arm | knob | ZWD truth rms (m) | ZWD truth \|residual\| p50 (m) | consumer settled p50 (m) | T4 arc erases |
|---|---|---|---|---|---|
| **E0** | — | **2.098** | **0.123** | **1.751** | **22 968** |
| `_plq7` | `phase_leveling_q_amb` 1e-7 | 1.93 | 0.256 | 2.76 | 12 348 |
| `_plq6` | `phase_leveling_q_amb` 1e-6 | 1.96 | 0.450 | 2.75 | 12 477 |
| `_plvf4` | `phase_leveling_var_floor_m2` 1e-4 (σ 1 cm) | 2.089 | 0.111 | 1.85 | 22 524 |
| `_plvf3` | `phase_leveling_var_floor_m2` 1e-3 (σ 3.2 cm) | 2.060 | 0.120 | 1.89 | 21 429 |

Arms `config/test_suite/_golden_sim_campaign_{plq7,plq6,plvf4,plvf3}.json`, each a copy of
`_golden_sim_campaign.json` with ONE key and the run paths changed.

**All four FAIL.** The q half is not inert — it halves the T4 churn (22 968 → ~12 400, −46 %), which
is the first direct evidence that the erase tier is sensitive to level STIFFNESS — but the channel it
was aimed at barely moves (ZWD truth rms 2.098 → 1.93/1.96) while the per-station-epoch truth
|residual| MEDIAN gets **worse** (0.123 → 0.256/0.450 m) and the consumer pays **+57 %**. The var-floor half is NEUTRAL on every
channel at both values: ZWD rms within 2 % of E0, |residual| p50 within 10 %, consumer +6 %,
T4 within 7 %.
Non-monotonicity in the value (1e-7 ≈ 1e-6, 1e-4 ≈ 1e-3) says both knobs are past the point where
the prior's own scale sets anything.

**Reading the data supports.** The freeze is not a stiffness-of-the-levels problem. A re-levelled arc
is code-quality in VALUE but phase-stiff in the block, so it is inconsistent with the geometry until
~20 min of geometry change refines it; T4 at E0's rate (21 erases per station-hour) erases it before
it converges — an erase → re-level limit cycle (§6.17b's H-C, measured). Softening the prior lets the
level MOVE but gives it nothing to move TOWARD: on a bed where arcs keep restarting, the station's
per-epoch (ρ, I) split has no information to converge to, so a looser level just tracks the same
mis-split with a larger variance and a noisier value. That is why the q arms cut the erase count (a
looser prior is a smaller standardized residual) and still do not release the plateau.

---

### 6.18b The T4 persistence ladder — fewer erases buy the producer and cost the consumer

`network.raim_arc_persist_epochs` = K (default 1, `include/config/GnssConfig.hpp:6857`; loader
`src/core/config/GnssConfigLoadNetwork.cpp:4401-4402`; gate `src/core/filter/NetworkRaim.cpp:629`)
requires an isolated-arc spike to trip K epochs in a row before the T4 attribution ERASE acts. The
design intent is exactly the limit cycle above: a converging young arc clears a K-epoch window, a
truly bad arc is still erased after K. Pre-registered gate: ZWD ≤ 0.5 m **with** consumer rms ≤ 2.0 m
(i.e. E1's consumer cost must not reappear).

Four arms on the in-block posture (`carry_zwd` + `carry_zwd_in_block`, §6.16.7d's vehicle), same bed,
whole-run `simnet_grade`; K = 1 is `runs/golden_sim_campaign_zwdblk` and K = 1e6 is E1-equivalent:

| K | arm | T4_attrib events | arc erases (×3 signals) | ZWD truth rms (m) | consumer p50 / rms (m) | sat-clock contrast (m) | clock vf |
|---|---|---|---|---|---|---|---|
| **1** | `_zwdblk` | 9 871 | 29 613 | 1.188 | **1.085** / 2.389 | 11.79 | 2.60e6 |
| 10 | `_zwdblk_p10` | 1 553 | 4 659 | 0.977 | 2.296 / 2.015 | 3.44 | 3.73e5 |
| 40 | `_zwdblk_p40` | 334 | 1 002 | 0.931 | 2.727 / 2.800 | 2.81 | 2.30e5 |
| 1e6 | `_zwdblk_e1` | **0** | **0** | **0.818** | 2.320 / 2.429 | **2.66** | 2.20e5 |

**FAIL on the consumer gate at every K.** And the shape of the failure is the finding: fewer erases
buy the PRODUCER monotonically — ZWD truth rms −22 % at K = 40 and −31 % at K = 1e6, satellite-clock
datum-free contrast **11.79 → 2.66 m (−77 %)**, clock variance factor 2.60e6 → 2.20e5 (−92 %) — while
the CONSUMER degrades: settled p50 1.085 → 2.296 → 2.727 (×2.5 by K = 40) and still 2.1× worse at
K = 1e6. The two halves of the ladder point in opposite directions across the whole range, so no K
tunes out of it.

⇒ **the consumer does not feel the satellite clock alone; it feels the per-satellite PHASE-CLOCK
(C + P).** "Sat-clock contrast" improving 4.4× while the consumer gets 2.5× worse is a direct
demonstration that it is not, by itself, a consumer-relevant metric — grade a consumer-facing change
on the phase-clock, not on the clock channel. §6.18c measures the phase-clock and confirms it.

---

### 6.18c The wire grade of the ladder — the wander rises with K, the jitter does not

Instrument (`scratchpad/persist/summary.txt`; script forms `wire_wander.py` / `wire_wander2.py` on
the four arms): the SD phase-clock error `W = dC − dP` against one FIXED reference satellite (G03 /
E03, the same in every arm — matched, but its own error is in every chord, so §2 below re-does the
contrast against the per-epoch median as the reference-free cross-check), decomposed inside a
PB-gauge/anchor tenure into **wander** (std of 10-min block means, the hypothesis quantity) and
**jitter** (|30 s difference|). Settled h3–h6, all in cycles.

| channel | wander K=1 | K=10 | K=40 | K=1e6 | jitter \|d\| p50 (K=1 → 1e6) | tenures | handovers |
|---|---|---|---|---|---|---|---|
| Galileo L1C | 0.249 | 0.611 | 0.633 | **0.793** | 0.071 → 0.068 | 143 → 114 | identical |
| Galileo L5Q | 0.303 | 0.699 | 0.762 | 0.877 | 0.068 → 0.070 | 138 → 108 | identical |
| Galileo L7Q | 0.478 | 0.681 | 0.748 | 0.850 | 0.070 → 0.066 | 140 → 114 | identical |
| GPS L1C | 0.507 | 0.836 | 0.885 | **0.944** | 0.072 → 0.076 | 210 → 172 | identical |
| GPS L2W | 0.523 | 0.833 | 0.881 | 0.953 | 0.067 → 0.069 | 203 → 156 | identical |
| GPS L5Q | 1.092 | 1.017 | 1.093 | 1.376 | 0.114 → 0.094 | 201 → 160 | identical |

Wander/within-tenure-std goes 0.44 → 1.12 (Gal L1C) and 0.89 → 1.43 (GPS L1C): the low-frequency
share of the within-tenure error grows from a third to all of it. **Handover counts are identical in
every arm** (PBGAUGE 279, PHASE_ANCHOR 297; only CLK_STEP moves, 68 → 4) and the per-epoch step
size is flat (step p50 0.204–0.283 cyc across the whole ladder), so **K changes the CORRELATION of
the steps, not their number or their size.** The motion itself is the frozen split seen through the
anchors: a 2-parameter (range, iono) fit of the per-satellite time-differenced motion reads
**R² 0.993–0.996** with |dr| ≈ 1.2 cm/30 s, and the SHAPE is invariant in K — only its persistence
changes.

The datum-free contrast against truth separates level from motion (per-epoch median removed; the
emitted phase bias carries the anchor's integer, so it is only meaningful arc-level-removed, and the
clock channel gets the identical treatment):

| channel, rms (m) | K=1 | K=10 | K=40 | K=1e6 |
|---|---|---|---|---|
| Galileo clock-only (lump removed) | 11.914 | 2.439 | 1.956 | 1.932 |
| GPS clock-only (lump removed) | 6.070 | 2.824 | 2.212 | 2.043 |
| Galileo clock-only **ARC-REMOVED** | 0.053 | 0.123 | 0.079 | 0.065 |
| GPS clock-only **ARC-REMOVED** | 0.079 | 0.086 | 0.096 | 0.075 |
| Galileo L1C phase-clock ARC-REMOVED | **0.143** | 0.226 | 0.286 | **0.277** |
| Galileo L5Q phase-clock ARC-REMOVED | 0.235 | 0.744 | 0.498 | 0.464 |
| Galileo L7Q phase-clock ARC-REMOVED | 0.174 | 0.366 | 0.346 | 0.358 |
| GPS L1C phase-clock ARC-REMOVED | 0.202 | 0.171 | 0.177 | 0.148 |

⇒ **the clock's 4.4× contrast gain is ALL arc LEVEL** — arc-removed, the clock channel is flat at
5–12 cm across the entire ladder — while the arc-removed PHASE-CLOCK **degrades** on every Galileo
band (L1C 0.143 → 0.277 m, ×1.9). The producer's clock statistic improves because erases stop
re-randomising the level; the quantity the consumer integrates gets worse.

The consumer side closes the loop, and it is a BIAS, not noise:

| K | PB-disc resets (settled / run) | 3D rms / mean (m) | UP mean (m) | jitter sd / wander sd (3D) | ρ(err, arc age) 3D / UP |
|---|---|---|---|---|---|
| 1 | 4 570 / 9 424 | 2.389 / 1.967 | **−0.218** | 0.898 / 1.015 | **−0.335** / −0.314 |
| 10 | 5 724 / 11 391 | 2.015 / 1.861 | −0.156 | 0.290 / 0.718 | **+0.420** / +0.222 |
| 40 | 5 985 / 11 509 | 2.800 / 2.571 | −1.548 | 0.569 / 0.952 | +0.200 / +0.085 |
| 1e6 | 5 859 / 11 400 | 2.429 / 2.232 | **−1.953** | 0.507 / 0.812 | +0.250 / +0.281 |

Both consumer wander and jitter FALL with K, and the settled MEAN rises — 3D mean 1.97 → 2.23–2.57 m
(worst at K = 40), UP mean −0.22 → −1.95 m (worst at K = 1e6). The within-run causal test is the sharp one: the correlation of |error|
with ARC AGE (epochs since the last phase-bias reset burst) **flips sign** at K ≥ 10, from −0.335
(error decays after a reset, K = 1) to +0.20…+0.42, with the mean |3D| by age bin growing
monotonically inside each run (K = 10: 1.72 m at age 0 → 2.45 at 5 → 2.55 at 10 → 2.69 at 20). A
consumer that is flushed every few minutes never accumulates the wire's within-arc walk; a consumer
that holds an arc integrates it. Consumer PB-disc resets RISE with K (4 570 → 5 859 settled) even
though the erases stop — the P-record jump proxy |dP| > 0.25 λ goes 13 260 → 17 387 — so the steps
the consumer sees are not the erases, they are the walk crossing the threshold.

**Reading the data supports: the T4 erases are the CRUDE CURE for the anchors' walk.** They chop a
continuous (ρ, I) walk into steps that the consumer flushes by resetting, at the cost of the ZWD and
clock channels (§6.17b: 99 % of them are not truth slips, and the re-level value is usually worse
than what it replaced). Removing them exposes the walk. So neither switching T4 off nor tuning K is
the fix, and neither is bounded identification (§7 item 22, already superseded): **the cure is
stopping the (ρ, I) walk itself**, which is §7 item 33's per-LOS ionosphere carried IN-BLOCK. That
card is now in build on branch `worktree-ionoblk` (from `dd6c81c`) as
`network.carry_iono_in_block`, pre-registered slipped ZWD ≤ 0.5 m **and** consumer p50 ≤ 0.6 m, with
the quiet-bed consumer within +10 % (the B9/Q1 protocol §6.16.7d demanded before any in-block claim).

⚠ **Caveats on this ladder.** (1) K = 1 (`_zwdblk`) was produced by a Sep-9 build; K = 10 → 40 → 1e6
are same-build. The knob's default is 1 and its code comment says "1 = act immediately
(byte-identical)", and the two intervening commits claim default-OFF byte-identity, but the K = 1 leg
is NOT a same-build A/B — the monotone K = 10 → 1e6 leg is. (2) These runs carry no
`logs/ambiguity_trace.csv`, so the grader's consumer join is empty and the wire-vs-consumer
side-by-side is inferential. (3) `scripts/simnet_wire_integer_consistency.py` treated
`truth_ambiguities.csv` as a STATIC per-(sat, signal) integer (2 486 slip rows touch 1 951 of 6 039
keys = 32.3 % of this bed), so its integer-dependent steps 4/4a/4b were contaminated here; **every
number quoted in this subsection is integer-free** (steps 1/2, 1c, 3b, 6). The grader now
accumulates truth slips via `simnet_amb_integer.load_truth` / `n_at` — fixed in this commit,
quiet-bed output byte-identical.

---

### 6.18d The field continuity card — validated as a mechanism, KEEP_OFF twice, and the defect is the TRIGGER

§7 item 31's card, `network.emit_pb_gauge_mode: "continuity"`: a per-family emission offset absorbs
every frame step so the emitted phase bias is continuous across handovers and mass refloats. No
integer content (the satellite's true bias is constant; the gauge is arbitrary and common to every
station's product), so it is a different mechanism from §6.17f's falsified level mode. It ran three
times on the field and once on the quiet sim.

**Pair 1 — ⚠ the branch-exe trap** (`runs/fldf3_on2_zwdblk_{pbcont,logctl}`, branch exe `pbgauge2`
= `b5af681`). The pair was supposed to run the in-block posture. **It did not.** The worktree
branched from `4bb799a`, before `966d4fd` (`network.carry_zwd_in_block`) landed, so the loader read
the key it did not know, ignored it silently, and both arms ran the DIAGONAL B9 carry: no "ZWD in
block" run-end clause, posture SCOPED tier 27/36 vs 27/35, fleet ZWD span p50 0.205 m vs the
reference's 0.143. The tell was a hash: the log-only twin's `products.txt` sha256 `95afa6d9…` ≠ the
main-tree run's `c48b9a5c…`. **A JSON loader reads the keys it knows and ignores the rest — a branch
exe knows only its own keys, and byte-identity on one posture proves nothing about knobs that
posture does not arm** (`feedback_branch_exe_unknown_knob_silent`; §7 item 38). Every pre-registered
ABSOLUTE baseline was therefore off a different posture; the paired A/B is still clean, and it is
reported as a posture contrast, never as a baseline.

On that (diagonal) posture the mechanism validated: 18 145 steps absorbed (58 at handovers /
18 087 unmarked; p50 0.233 cyc, p99 16.16), C records and P-stripped products BYTE-IDENTICAL
(emission-only), consumer PB-disc resets 1 158 → **104 (−91 %)**, |ΔP| > 0.25 λ 6 941 → 164 (−98 %),
pc_IF within-tenure deviation 0.230 → 0.164 m, step share of variance 57.8 → 11.7 %, pb_L1C unmarked
step rate 0.671 → **0.000** /sat/h — and the residual pc_IF step rate 0.401 equals the CLOCK's own
0.380, i.e. what is left in the phase channel after absorption is the clock's step content. The
CONSUMER got worse (3D 2.701 → 2.936 m, UP sd 1.99 → 2.32) and the residual converted from fast reset
jitter to slow wander (UP ACF < 0.5 at 11 → 55.5 min, %slow 61 → 98, PCA axis1 0.684 → 0.901),
corr(UP, BSHM ZWD) +0.334 → +0.707, regressing the co-located site ZWD out taking UP 2.488 → 1.355 m.
That produced a reading — "the resets were MASKING the ZWD sink" — which **pair 2 does not
reproduce**: it was the diagonal carry's, not the mechanism's.

**Pair 2 — the correct posture, v1** (`runs/fldf3_on2_zwdblk_{pbcont2,logctl2}`, rebased exe
`pbgauge3` = branch `865904b` = main `721dd39` + PB-gauge). Posture check cleared: both arms print
the identical run-end clause `ZWD in block: 87313 col(s), 98 diagonal fallback(s), 297 missing cross
term(s), 87274 RAIM-pool admission(s)`, and `logctl2`'s `products.txt` sha256 `c48b9a5c…` **exactly
reproduces** `runs/fldf3_on2_zwdblk`. Emission-only re-proved: all "P," lines stripped, sha256
`590470b2…`, 0 differing lines, max |ΔC| = 0.0.

| gate (settled ≥ h6, n = 720) | logctl2 | pbcont2 (v1) | verdict |
|---|---|---|---|
| P1 steps absorbed | — | 34 870 | PASS |
| G1 consumer PB-disc resets | 2 322 | **122** (−94.7 %) | PASS |
| G2 \|ΔP\| > 0.25 λ | 19 354 | **242** (−98.8 %) | PASS |
| G3 pc_IF within-tenure dev p50 (m) | 0.464 | **0.591** | **FAIL** (worse than its own control) |
| G3 step share of variance, pc_IF | 69.2 % | 70.2 % | **FAIL** (unmoved) |
| G4 UP rms / sd (m) | 1.862 / 1.862 | **1.131 / 1.127** | UP sd ≤ 1.2 **PASS** (−39 %) |
| G4 E rms (m) | 1.364 | 1.229 | PASS |
| G4 N rms (m) | 1.798 | **2.247** (+25 %) | **FAIL** |
| G4 3D rms (m) | 2.926 | 2.799 | FAIL (absolute gate 2.0) |

**Why G3's level half fails — measured, not inferred.** `dev()` is linear, so
`dev(pbcont2) = dev(logctl2) + dev(STAIRCASE)` where the staircase is the accumulated continuity
offset actually emitted. On the settled pc_IF channel: arm 1.1425 = control 0.7913 + **staircase
0.5314** m. The staircase fires **14 887 times in the settled window** with one-epoch |jump| p50
0.026 m — and the 0.10-cycle trigger is ≈ 1.9 cm. It is absorbing routine phase-bias NOISE (raw
pb_L1C |d| p50 4 cm/epoch, heavy-tailed p90 0.41 m), ~23 firings per family per hour, each a
PERMANENT offset ⇒ the emitted level RANDOM-WALKS, p50 18.8 m of accumulated excursion per satellite
over 6 h (max 284 m). A datum artefact was tested and REJECTED: the per-epoch trim datum is quieter
in the arm (|d datum| p50 0.0029 vs 0.0074 m, n > 0.25 m 26 vs 73), so the extra deviation is the
staircase, not the frame. The new residual is what an integrated per-satellite level error looks
like at a float consumer: a near-horizontal due-south very slow channel, PC1 72 % at el 14.4° /
az 175.6°, **84° off the geometric null**, N ACF < 0.5 at 56 min.

The 2×2's CONTROL column carries its own finding: **the in-block ZWD posture doubles-to-quadruples
the wire's phase-bias churn.** logctl → logctl2: consumer resets 1 158 → 2 322, the |ΔP| proxy
8 733 → 19 354, pb_L1C unmarked step rate 0.671 → 2.925 /sat/h, pc_IF deviation 0.230 → 0.464 m.
That is the wire-side price of the vertical §6.16.7d bought, it is what makes the continuity gauge
look good on this posture, and it deserves its own card.

**Pair 3 — v2 pend-and-confirm, a REGRESSION** (`runs/fldf3_on2_zwdblk_pbcont3` vs the same
`logctl2`; confirm k = 3 epochs within tol 0.10 cyc). Run-end: 4 636 pends, 4 212 confirmed, 190
reverted, 30 014 chained, 17 dropped at gaps, 168 dropped at the rail, **2 877 force-resolved (68 %)**.
Step suppression survives — G1 158 vs 2 322 (−93.2 %), G2 497 vs 19 354 (−97.4 %) — and everything
else is worse than v1: G3 pc_IF dev **0.844 m** (v1 0.591, ctl 0.464), step share **88.6 %**; G4 3D
**4.471 m**, UP rms **3.464** (mean −2.337, sd 2.559, **trend −0.921 m/h**), N 2.643, E 0.999 (the
only win). Hourly 3D: the best first six hours of the three arms (h0–h5 mean 1.72 m vs 3.45 v1 /
2.73 ctl) then a MONOTONE ramp h6 → h11 of 2.72 → **6.54 m** — the signature of an integrated level
error, not of an event population.

**The decisive measurement is that v2 gated the LABEL, not the WIRE.** The emitted P offset equals
`delta_cont_cyc` EXACTLY on every row of both arms — |(P_arm − P_logctl2)/λ − δ_cont| p50 = p99 =
max = 0.000000 — and that holds separately inside every `pend_state`, **including the 42 386 rows
marked PENDING**. Decomposing the 35 025 δ_cont increments: 2 163 (1 136 cyc, **3.2 %**) on
confirmation rows, 32 862 (34 064 cyc, **96.8 %**) on pending rows. The revert path is correct — all
190 reverts restore the pre-pend offset to the bit (|net move| = 0.000000 in 190 of 190) — and
irrelevant: only 4.1 % of pends revert, and the pend window sits pinned against its own
forced-resolution rail (duration p50 = p90 = max = 13 epochs). So the design premise "a reverting
excursion is dropped with the emitted value never having moved" does not hold: the staircase fires at
the same rate (14 495 vs 14 887 one-epoch jumps in 6 h), its per-satellite excursion is slightly
LARGER (p50 20.5 vs 18.8 m), and its 30-min structure is **1.60× worse** (0.848 vs 0.531 m).

**The quiet-sim v1 twin** (`runs/golden_sim_pbcont` vs `runs/golden_sim_pbgauge_ctl`, quiet 24 h
golden + the consumer-AR overlay, 61 sta, 0 truth slips, 457 392 P records). It does exactly what it
claims and almost nothing else: consumer PB-disc resets **2 274 → 151 (−93.4 %)** with code-bias
(1 513) and clock (47) disc lines IDENTICAL; products byte-identical outside the P lines
(`429c37dc…`); **the wire's SD fractions do not move** — mean |p50|f| − 0.25| is 0.0257 in BOTH arms,
ON closer to the null on 5 of 12 channels and further on 7, i.e. continuity moved no integer content
in either direction; AR unchanged (0 fixes both, ratio p50 1.01, settled `ratio_battery` 2 159
identical). The cold-handover population reconciles to the row: 53 absorbed steps at a
`PBGAUGE|handover_cold` (p50 **16.171 cyc** = 3.1 m on L1) + 70 at a `handover_continuous` (p50
0.146) = 123 = the run-end clause's "123 at handovers" exactly. The price: float consumer 3D rms
0.0761 → 0.0932 m (**+22.5 %**) with **p50 UNCHANGED** (0.0603 → 0.0601) and p95 +43 % — entirely
tail, the same trade the field shows one order of magnitude smaller.

★ **And the walk is a property of the TRIGGER, not of the field's event content.** On a bed with
ZERO truth slips the accumulated |δ_cont| still reaches p50 **28.6 cyc (5.4 m on L1)** and max 181 cyc
(34 m) over 24 h — ~9× slower per hour than the field's (1.19 vs 10.4 cyc/h) purely because **96.2 %
of the sim's candidates are sub-min (< 0.10 cyc) against 72.3 % on the field**, and with no consumer
ramp to show for it (hourly 3D flat in both arms). Two beds reach the same conclusion from opposite
directions: raise the trigger above the noise floor, or gate it on a declared re-reference. Tuning
the confirmation logic is not the lever.

⚠ The one place the quiet sim is genuinely CLEANER than the field is the rail. Steps > 40 cyc pass
through unabsorbed: on the sim, 52 records in 26 epochs of which **24 of 26 carry a declared
handover** (the other two are the first two emit epochs = ignition). On the field, **196 records in
15 epochs with only 6 of 15 carrying a marker**, and the three largest clusters (tow 280050 n = 76,
282150 n = 38, 262590 n = 17) are PRIORRES / T4_attrib / TOPOLOGY epochs, two with NO handover marker
at all. The population is identical in v1 and v2 (196 / 15 both) — untouched by pend-and-confirm.
**Undeclared network-wide frame motion is a FIELD defect (invariant #7's output edge), not a property
of the gauge.**

**Verdicts.** v1 and v2 both **KEEP_OFF as promotions**. The step-suppression half is validated
three times over (−91 / −95 / −93 % consumer resets, −98 / −99 / −97 % on the wire's own jump test,
emission-only by byte-identity on every arm) and the vertical chain on the field now reads
`on2` 3.71 → in-block 1.86 → + continuity **1.13 m**. What blocks promotion is the trigger's own
staircase and the N-component it feeds. v3 (a running median of the recent per-family offsets, then
absorb only the confirmed departure from it) is in build; the ledger row keeps the `_mode` /
`cont_*` knobs marked branch-only.

---

### 6.18e Addendum to §6.17g's field wire census — what drives the steps, and what does not

The 40-arm closure matrix at Bartlett effective N, run over `runs/fldf3_on2` and
`runs/fldf3_on2_zwdblk` (`scripts/field_wire_phase_clock_census.py`; outputs
`scratchpad/fieldres/wire_phase_clock_*.txt`), settles four things:

1. **The phase-bias steps are DRIVEN BY T4 RAIM-attribution refloats.** Of every event tier, only
   `T4_attrib` co-occurs with the pc_IF step count: **r +0.546, |z| 8.7** on the control and
   **+0.268, |z| 4.4** on the in-block arm (the emission sites are `NetworkRaim.cpp:888-893`, one
   `T4_attrib/attrib_arc` event per acted entity). Every other tier is at noise — T0_gap −0.057,
   T1_if_slip −0.101, PHASE_ANCHOR +0.018, PBGAUGE +0.019, |z| ≤ 1.5 throughout. The rate is
   **~5.3 pc_IF steps per satellite per hour above 0.25 m**, 205 above 1 m in 6 h, and up to **26
   satellites stepping in one epoch** (tow 282150, `T4_attrib ×4`; tow 299490 with `T4_attrib ×22`
   and no other tier).
2. **And they are UNSIGNALLED on the wire.** `network.serialize_disc_counters` is FALSE on the field
   chain, so the P records are the 6-field form (`P,sat,signal,bias_m,sig_int,wl_int`) with no
   `phase_disc` column: a FILE consumer gets no discontinuity signal at all. The R retro-bump fired
   **68 times against ~1 000 steps**. The closed-loop consumer still resets, but only because it
   reads the in-process store counter — invariant #7 with a bare OUTPUT edge (§7 item 39).
3. **The geometry closure is NULL in every arm.** Predicted-vs-actual consumer error correlates at
   |z| 2–3.7 at most and sign-inconsistently across the 40 (driver × prep × trim) cells; the
   predicted axis sits at el 75.6–79° against a measured 49.8° / az 9.3°, and two half-visible
   satellites (G12 40 % visible, G25 59 %) carry two thirds of the predicted amplitude. Decisively:
   **the same wire statistics feed two consumers with ORTHOGONAL error axes** (el 50 / az 9 in-block
   vs el 73 / az 137 on the control) ⇒ the consumer's wandering DIRECTION is not set by the wire's
   phase-clock. The magnitude link survives weakly (corr(rms visible |δ|, |consumer error on the
   axis|) +0.396, n_eff 27, |z| 2.05), which is consistent with the §6.18c reading — the consumer
   integrates the wire's walk into whatever its own weakest direction happens to be — but the
   projection itself is not the mechanism.
4. **Minor contract defect:** the S record advertises the DOY-128 a-priori
   (`COD0MGXFIN_20241280000_01D_05M_ORB.SP3+…_30S_CLK.CLK`) while three days are loaded — the
   advertised identity is the first day's, not the epoch's.

For scale, the same statistic on COD's own final clock is the floor: rms p50 **0.0066 m**. The
field's emitted clock sits at 0.0256 m (**3.9×** the floor) and its pc_IF at 0.464 m (**70×**).
COD's phase biases are daily constants, so every time-variation in the P channels is wire-side by
construction.

---

## §6.19 The 2026-09-10 second half — the slipped-bed probe ledger, the continuity closure, the false-alarm isolation, and the inert receiver clock

§6.18 ended with one lever named (`network.carry_iono_in_block`, §7 item 37) and one reading
untested: that the plateau is a frozen (ρ, I) split sustained by arc churn.  The afternoon and
evening ran that lever plus every remaining config-only probe on the same bed, closed the field
emission-continuity class, and found that the producer's receiver clock has no temporal prior at all.
The result is a ledger of eleven failures with ONE shape, and a change of development method.

Nothing above this section is edited.

---

### 6.19a The slipped-bed probe ledger — eleven probes, one shape

Bed: `runs/simnet60_campaign_wu93` (60 stations, GPS + Galileo triple, 2 486 truth slips in 6 h,
wu93 frame).  **Control for this section is the IN-BLOCK posture** `runs/golden_sim_campaign_zwdblk`
(the golden + `network.carry_zwd` + `network.carry_zwd_in_block`, §6.16.7c) — not E0.  Arms that ran
on the E0 posture (`runs/golden_sim_campaign`, ZWD truth rms 2.098 m, consumer settled p50 1.751 m /
rms 1.80, T4 22 968 arc erases) are marked ⁽ᴱ⁰⁾ and must not be read across the two control columns.
Every number is `scripts/simnet_grade.py <fixture> --run <run> --products <run>/products.txt`
whole-run, except the T4 column, which is the run-end counter
(`ONE-SOLVE REFLOATS: <n> T4 attribution`, invariant #4).

| probe | arm (`runs/…`) | ZWD rms | ZWD p50 | consumer p50 | consumer rms | sat-clk contrast | T4 arc erases | WL / integers |
|---|---|---|---|---|---|---|---|---|
| **IN-BLOCK CONTROL** | `golden_sim_campaign_zwdblk` | **1.188** | **0.048** | **1.085** | **2.389** | **11.79** | **29 613** | WL ref-free **86.2 %**, L1C med\|e\| 1.77 |
| oracle exact repair ⁽ᴱ⁰⁾ | `_campaign_oracle` | 1.779 | — | 1.74 ⁽ᴱ⁰⁾ | — | — | 40 686 | WL 81.5 % flat; ambient DD \|e\| 1.65 → **2.43** |
| oracle exact repair | `_campaign_zwdblk_oracle` | — | — | **2.23** | — | — | — | — |
| oracle + E1 (T4 starved) ⁽ᴱ⁰⁾ | `_campaign_oracle_e1` | 1.819 | — | 3.78 | — | — | **0** | WL 35 %; ambient \|e\| **6.09** |
| oracle + E1 (T4 starved) | `_campaign_zwdblk_oracle_e1` | 1.239 | — | 3.33 | — | — | — | ambient \|e\| 5.66 |
| level q ×2 ⁽ᴱ⁰⁾ (`phase_leveling_q_amb` 1e-7 / 1e-6) | `_campaign_plq7` / `_plq6` | 1.93 / 1.96 | 0.256 / 0.450 | 2.76 / 2.75 | — | — | 12 348 / 12 477 | — |
| var floor ×2 ⁽ᴱ⁰⁾ (`phase_leveling_var_floor_m2` 1e-4 / 1e-3) | `_campaign_plvf4` / `_plvf3` | 2.089 / 2.060 | 0.111 / 0.120 | 1.85 / 1.89 | — | — | 22 524 / 21 429 | — |
| persistence K = 10 / 40 / 1e6 (`raim_arc_persist_epochs`) | `_zwdblk_p10` / `_p40` / `_e1` | 0.977 / 0.931 / 0.818 | — | **2.296 / 2.729 / 2.320** | 2.015 / 2.800 / — | 3.44 / — / 2.66 | 4 659 / 1 002 / **0** | wire wander ↑ monotone in K |
| k_sigma 4 (`raim_k_sigma` 6 → 4) | `_zwdblk_k4` | 0.972 | 0.055 | **1.317** | **3.26** | 14.4 | **46 575** | L1C med\|e\| 1.67 |
| iono-in-block (`carry_iono_in_block`) | `_zwdblk_ionoblk` | 1.133 | 0.042 | **1.592** | 2.24 | 11.08 | 28 509 | L1C med\|e\| **1.32** (−26 %), round = 0 21 → 25 % |
| young-arc inflate V0 0.09 m² | `_zwdblk_yng` | **0.823** | 0.290 | 1.73 | **7.91** | 18.6 | 26 076 | — |
| young-arc inflate V0 0.01 m² | `_zwdblk_yng03` | 1.175 | — | 1.60 | 2.17 | 19.6 | 31 956 | — |
| false-alarm-only KEEP (`oracle_true_slip: free`) | `_zwdblk_orafa` | 1.066 | 0.104 | **2.92** | **4.46** | — | **48 078** | — |
| rx-clock q 1000 → 0.09 (`filter.process_noise.clock_bias_q`) | `_zwdblk_rxq` | — | — | — | — | — | 29 613 | **INERT — `products.txt` byte-identical** (`bba9a7f2865070bc…`) |

**Every probe FAILS its pre-registration.**  The two gates the ladder was run against were slipped-bed
ZWD truth rms ≤ 0.5 m *and* consumer settled p50 ≤ 0.6 m (§7 item 37); the best ZWD in the table
(0.818 at K = 1e6, 0.823 at V0 0.09) buys a consumer at 2.3–7.9, and the best consumer in the table is
the CONTROL.

Two riders on reading it.  (a) The T4 opportunity denominator is **identical** in every slipped arm
(`attribution ARC (T4): 645 406 opp`), so the erase column is directly comparable across rows — quote
counts with opportunities (the §6.16.7d rate-vs-count trap).  (b) `simnet_grade` p50 and rms answer
different questions on this bed: the young-arc arm has the second-best ZWD **rms** in the table and the
worst ZWD **p50** and consumer **rms** — its failure is a tail, and a p50-only ladder would have read
it as a win.

**The unified reading.**  The station frame — its stiff zero-q phase levels together with its
per-epoch ZWD, receiver clock and slant iono — **WALKS under arc churn**.  Arcs are levelled at
different epochs against different values of the frozen (ρ, I) split, so the station's compromise
drifts as its arc population turns over; the wire grade measures the walk directly at 1.2 cm/30 s in
the range-like direction with the iono-like direction riding it at R² 0.993–0.996 (§6.18c).  An arc is
consistent with that frame only if it is periodically re-levelled INTO the current frame.  That single
sentence predicts the whole table:

- **every response that REDUCES re-levelling makes the consumer worse** — exact oracle repair
  (+105 % consumer p50 in-block), keeping the false alarms (+169 %), starving the erases at any K
  (+112…+151 %), softening the levels (+57 % ⁽ᴱ⁰⁾).  A restored-to-truth or preserved arc is
  *inconsistent with its own station's corrupted states*, and the corrupted majority then rejects it
  (§6.17e: detector re-levels −85 % while T4 grew +77 %);
- **more erasing is not the other end of a lever, it is just noise** — `k_sigma` 4 adds 57 % more
  erases, improves ZWD 18 % and costs the consumer 21 % on p50 and 36 % on rms.  With the persistence
  ladder this **brackets the erase-rate family: the shipped k = 6 / K = 1 corner sits at the consumer
  optimum of that family**, and the erase rate is exhausted as a knob;
- **making young arcs honestly weak relocates the tension rather than dissolving it** — the in-block
  young-arc inflation moves it into the satellite clocks (contrast 11.8 → 18.6 / 19.6 m) and into the
  consumer's tail (rms 7.91 at V0 0.09);
- **carrying the I half of the split does not release it either.**  `carry_iono_in_block` is the one
  probe whose *integers* improve (L1C settled ref-free med\|e\| 1.77 → 1.32 cyc, −26 %; the share
  rounding to the right integer 21 → 25 %) and its ZWD moves −5 %, but the consumer regresses +47 % on
  p50.  Mechanism live and clean: 671 535 iono columns carried, **0 diagonal fallbacks**;
- **the only lever that ever helped is a TEMPORAL PRIOR ON THE ρ DIRECTION** — ZWD-in-block, −43 % on
  the ZWD channel (2.098 → 1.188) and the only knob that removed the field consumer's vertical DC bias
  (§6.16.7d).  Its cost is a clock relocation, and it is still 2.4× off its own gate.

The list of things that do NOT work is now long enough to be informative: level stiffness (q, floor),
erase rate (both directions), erase persistence, the slip response's VALUE (exact repair), the
false-alarm population, young-arc variance, and the split's I half in the block.  What has never been
tried is a prior that makes the frame itself observable epoch-to-epoch in more than one direction —
which is why §6.19d's receiver-clock finding matters and why the next probes belong on an EVENT
harness (§6.19f), not on a storm bed.

#### 6.19a.1 The quiet-bed neutrality of `carry_iono_in_block` — the B9/Q1 protocol, passed

§7 item 37 made the quiet-bed test mandatory before any slipped-bed claim, because
`project_iono_carry_8b_campaign` falsified a DIAGONAL per-LOS iono row four times and §6.16.7d's
finding was that the diagonal vehicle IS the double count.  `runs/golden_sim_zwdblk_ionoblk_quiet`
(24 h quiet golden + in-block ZWD + in-block iono) vs `runs/golden_sim_zwdblk_quiet`, settled h12–24:

| | in-block ZWD only | + in-block iono |
|---|---|---|
| consumer 3D rms | 0.0875 m | 0.0890 m (+1.7 %) |
| consumer 3D p50 | 0.0525 | 0.0555 |
| consumer UP rms | 0.0759 | 0.0768 |
| ZWD truth rms | 5.6 mm | 5.6 mm |
| iono columns carried | — | 2 750 000, **0 fallbacks** |
| T4 arc erases | 159 | 408 |

**NEUTRAL: no double-count regression** (contrast the diagonal ZWD carry's 0.204 → 0.723 m on this same
bed) **and no gain.**  The in-block vehicle is sound — the four B9 falsifications were the diagonal row's
over-count, exactly as the ZWD card found — and the mechanism is simply not where the information is.
The 2.6× rise in T4 erases on a bed with ZERO truth slips (159 → 408) is the one thing worth carrying
forward: an extra carried direction per LOS gives the post-fit consensus more to flag.

Ledger verdict: **`network.carry_iono_in_block` KEEP_OFF** — mechanism validated on both beds, benefit
absent on both.  Branch `worktree-ionoblk` (`7311ad0`, plus `2bf6821` for the young-arc inflation)
stays UNMERGED; §7 item 37 closes.

---

### 6.19b The false-alarm isolation — the re-levels the detectors get WRONG are load-bearing

Two independent measurements, and they point the same way.

**1. Attribution (`scripts/simnet_slip_census.py` on the in-block arm, post-Card-R goldens).**
Of the bed's 2 486 truth slips, **2 337 (94.0 %) are DETECTED** — by tier MW 2 006 / IF-TD 331 / T4 59 /
raim 16 — and **74 (3 %) are SILENT**, mostly ≥ 90-cycle jumps at el ≈ 10°.  Detection is not the
problem.  The other side of the ledger is:

> **slip-tier events with NO truth slip anywhere in the window: 2 371 of 4 708 — MW 1 132, IF-TD 1 239.**

That is ≈ **6.4 false re-levels per station-hour against 6.8 true slips per station-hour**: after Card R
fixed the IF-TD reference gate on the QUIET bed (§6.14.6), BOTH detectors still fire as often on nothing
as on something on the SLIPPED bed.  Consistent with §6.14.3 — the fixture's phase noise is ~5× the
field's and the MW thresholds are field-empirical, so a fixed physical threshold does not port between
beds (`feedback_mw_thresholds_empirical`).
⚠ **Counting basis.**  The census counts EVENTS per (station, satellite); the oracle below counts
per-signal ACTUATIONS.  The earlier "13 485 actuations" figure was the per-signal offer count.  The
honest per-event false-alarm count on this bed is **~2 300 per 6 h**; the honest per-signal actuation
count is 6 804.  Never mix the two bases in a rate.

**2. The false-alarm-only oracle (`runs/golden_sim_campaign_zwdblk_orafa`, commit `3dd1284`).**  The
2×2 of the oracle's two policies has a corner that isolates exactly this population:
`oracle_true_slip: free` + `oracle_false_alarm: keep` — every REAL slip pays today's shipped response,
and only actuations with no truth slip in the window keep their level.  **6 804 false alarms kept**
(MW 4 156 / IF-TD 2 131 / LLI 517), 3 409 true slips freed as today.

| | in-block control | false alarms KEPT |
|---|---|---|
| ZWD truth rms | 1.188 m | 1.066 (−10 %) |
| ZWD truth p50 | 0.048 | **0.104** (2.2× worse) |
| consumer settled p50 | 1.085 m | **2.92** |
| consumer settled rms | 2.389 | **4.46 (2.7×)** |
| T4 arc erases | 29 613 | **48 078 (+62 %)** |

**FAIL, and decisive in the opposite direction from the one it was built to test.**  NOT re-levelling
an arc that had no slip makes the consumer 2.7× worse and forces 62 % more T4 erases.  ⇒ **the
false-alarm re-levels are a RESYNC mechanism**: their job on this bed is not slip repair at all, it is
dragging a drifting arc back into its station's current frame, and the detectors' 50 % false-alarm rate
is doing useful work by accident.  This kills "tighten the detector thresholds on this bed" as a card —
a better detector would remove the resyncs and leave the walk.

The corollary for §7 item 22 is now complete: **bounded identification is the wrong lever** (§6.17b:
T4 mostly re-erases arcs its own previous re-level damaged) **and so is better detection** (here).
Both are attempts to fire the response more accurately; the finding is that the response's ACCURACY is
not what the consumer is buying from it.

---

### 6.19c EMISSION-SIDE CONTINUITY — CLOSED, falsified as a CLASS

§7 items 31 / 36 kept the field emission-continuity card alive through two KEEP_OFFs on the grounds
that the mechanism was right and the TRIGGER was wrong.  A third variant closes the class.

All three run on the in-block field posture, `runs/fldf3_on2_zwdblk_*` vs `logctl2`
(DOY-129, 63 sta, 12 h, WTZZ closed-loop consumer; `logctl2`'s `products.txt` = `c48b9a5c…`, identical
to `runs/fldf3_on2_zwdblk` — the §7 item 38 hash rule, so the branch exe is certified on this posture).
Settled ≥ h6:

| | control `logctl2` | v1 absorb-every-step `pbcont2` | v2 pend-and-confirm `pbcont3` (`525e85c`) | v3 running-median k = 5 then absorb `pbcont4` (`ad0b1d0`) |
|---|---|---|---|---|
| consumer 3D rms | 2.93 m | **2.80** | 4.47 | **8.26** |
| consumer UP rms | 1.86 | **1.13** | 3.46 | 7.34 |
| consumer N rms | 1.80 | 2.25 | 2.64 | 3.57 |
| consumer E rms | 1.36 | 1.23 | — | — |
| accumulated \|offset\| p50 | — | **125 cyc** | 132 cyc | 124 cyc |
| absorptions (settled) | — | 34 870 | 42 386 pending rows moved | **33 403** |

**v3 is the decisive arm.**  It was built on the reading that v1's staircase absorbs routine noise, so a
running median over k = 5 epochs should pass a genuine step and reject a spike.  It absorbed **33 403
steps against v1's 34 870 — 96 % of them.**  The median filter changed essentially nothing, because
there is no flat level for a spike to stand out from:

> the field's per-epoch phase-bias motion is a **RANDOM WALK** (raw \|Δ\| p50 4 cm/epoch, heavy-tailed
> p90 0.41 m), not spikes over a flat level, and **a median cannot separate a walk from steps**.

Its consumer is the worst of the four (3D 8.26 m, UP mean −6.3 m, corr(UP, BSHM ZWD) −0.70) and its
accumulated offset lands at the same p50 124 cyc as v1's 125 and v2's 132 — three different triggers,
one common outcome, which is the signature of the SOURCE rather than of any trigger.

The quiet-sim twins say the same on a bed with ZERO truth slips: v1 cuts consumer PB-disc resets
**−93 %** and costs the float consumer **+22 %** rms (a tail), with accumulated offsets already at
28.6 cyc median over 24 h; v2 confirmed 2 622 of 4 048 pends and still walked.

**READING.**  The wire's biases random-walk because **the PRODUCER's phase-bias state walks** — the same
frozen (ρ, I) split of §6.19a, seen through the anchors (§6.18c measured it: within-tenure wander rises
with K, jitter flat, step size flat, handover counts identical).  Any emission-side absorber converts
walk-steps into offset-steps and hands the consumer an *integrated* error; the consumer, given the
choice, prefers the resets, because a reset flushes the accumulated error and an offset does not.
**The fix belongs in the estimation, not at emission.**

**VERDICT: the class is CLOSED.**  `network.emit_pb_gauge_level` stays KEEP_OFF — level mode falsified
(§6.17f), all three continuity modes falsified here.  Branch
`worktree-agent-a7e2586003a605548` (HEAD `ad0b1d0`, rebased on main `721dd39`) stays **UNMERGED as the
record**; nothing from it is proposed for main.

Two durable field lessons survive the closure, and both are cheap:

1. **The in-block ZWD posture itself doubles-to-quadruples the wire's phase-bias churn** (pb_L1C
   unmarked steps 0.67 → 2.9 per sat-hour; consumer resets 1 158 → 2 322 on the 2×2's control column).
   That is its price for the vertical it buys, and it must be quoted with the −27 % consumer 3D.
2. **`network.serialize_disc_counters` must be armed on the field template** (§7 item 39) — a FILE
   consumer of this product gets no discontinuity signal at all while the wire steps ~5.3 times per
   satellite per hour above 0.25 m.

---

### 6.19d The receiver clock has NO temporal prior on the network producer — and the sim's truth is a walk

The unified reading of §6.19a says the only thing that ever helped was a temporal prior on the ρ
direction.  The station frame has three other members with no prior at all.  The cheapest to test was
the receiver clock, because a knob for it appears to exist.

**It does not.**  `runs/golden_sim_campaign_zwdblk_rxq` = the in-block control with
`filter.process_noise.clock_bias_q` 1000 → 0.09 m²/epoch.  Its `products.txt` is
**byte-identical** to the control's (`bba9a7f2865070bc…`, verified by `sha256sum`), and its run-end T4
counter is identical to the digit (29 613 erases / 9 871 events / 645 406 opportunities).

> ⚠ **`filter.process_noise.clock_bias_q` is INERT on the network producer.**  The per-station receiver
> clock is a **per-epoch LOCAL parameter** — estimated fresh, marginalized, never carried — so a
> process-noise value for it has nothing to act on.  The knob is meaningful on the PPP / baseline /
> real-time paths only.  An arm that sets it on a network config is armed-and-inert (invariant #4): no
> counter moves, no clause appears, and the products are identical.

And the thing it would have modelled is real.  The simulator's own receiver-clock truth
(`truth_clock.csv`, e.g. `runs/simnet60_campaign_wu93/S01000SIM`, 720 epochs) is a **deterministic
drift plus a white increment**: mean step **+2.997e-08 s / 30 s = +8.99 m per epoch** (≈ +0.2995 m/s)
with the step's standard deviation **1.0055e-09 s = 0.301 m**, and a second-difference sd of
1.436e-09 s (= √2 × the step sd, the signature of white increments on a linear ramp).  So the truth is
exactly the object a random-walk prior describes — after the drift is removed.

**CARD (pre-registered, building on `worktree-ionoblk`): `network.carry_rx_clock_in_block`** — the
station's receiver clock carried INSIDE the F3 whitened local block with q ≈ 0.09 m²/epoch, the ZWD
card's sibling and the third member of the (amb, iono, ZWD, clock) block.  It inherits the ZWD card's
whole contract: in-block or nothing (a diagonal row would be the same station-common double count that
falsified `carry_zwd` and the four B9 iono arms), a counted diagonal fallback, the loader interlocks on
`phase_leveling` / `phase_leveling_local_block` / `carry_invariant`, fingerprinted, OFF byte-identical,
and the B9/Q1 quiet-bed neutrality test BEFORE any slipped-bed claim.

⚠ **Its one design risk is the drift.**  A plain RW on the absolute clock will be fighting a 9 m/epoch
ramp — three decades above the 0.30 m increment it is trying to model — so the carry must be on the
RESIDUAL about a per-epoch re-linearized `x_lin`, or the state must become a (bias, drift) pair.  Get
that wrong and the card is a stiff prior carrying a stale value, which is standing invariant #1.

⚠ **And validate it on the EVENT HARNESS first** (§6.19f), not on a storm bed: one injected slip on one
arc of a converged station, and the question is whether the station frame holds.  That question is
unanswerable in the presence of 2 486 interacting slips, which is precisely why eleven storm-bed probes
produced one shape and no mechanism.

---

### 6.19e The Phase-2 producer-AR diagnostic, graded against truth

*(§7 item 44; the instrument is `dd6c81c`, the design-doc note of 2026-09-10 and
`scripts/simnet_producer_ar_grade.py`.)*

**The run.** `runs/golden_sim_netar` = `config/golden/golden_sim_tri_w6.json` VERBATIM plus
`network.ambiguity_resolution.{enable, trace, ils_max_dim 200, min_epochs 10, enable_partial_ar,
min_success_rate 0.9999, ratio_threshold 3.0, nl_max_per_pair_frac 0.10}` and the run paths — quiet
60-station truth bed `runs/simnet60_quiet_wu93_24h`, 24 h, 2 879 epochs, exit 0.  Phase 2 is READ-ONLY
(no re-pin, no re-solve), so the emitted product is the golden's.  Run-end clause, verbatim:

```
Network (producer AR, Phase 2 log-only): 2869 epochs searched, 298 DD p50 / 200 searched p50,
fixed epochs 2238 (78.0%), DD fixed total 29828, ratio p50 12.60, p_succ p50 1.0000,
med_DD_sigma p50 0.3170 cyc; carry-blind; trace on
```

**Cost:** `netar` = **2 152 ms of a 3 984 ms epoch (54 %)** at 60 stations, median over the settled
half — the joint conditional covariance and the 200-dim MLAMBDA, not the CSV.  A 24 h arm costs
~1.7 h of extra wall on this box.

**Grade** (`scripts/simnet_producer_ar_grade.py runs/simnet60_quiet_wu93_24h --run runs/golden_sim_netar
--settle-h 12`), 852 260 trace rows joined to truth, **0 unjoinable**:

| pre-registered gate (`_golden_sim_netar.json` `_comment_arm`) | measured, settled h12–24 | |
|---|---|---|
| searched-subset \|e\| p50 ≤ 0.10 cyc | **0.198 cyc** (wrapped \|frac\| p50 0.147) | **FAIL** (2.0×) |
| fixed epochs > 50 % | **86.8 %** (1 240 / 1 429; h0–12 69.3 %) | **PASS** |
| CORRECT-fix rate among kept ≥ 99 % | **85.52 %** (13 328 / 15 585) | **FAIL** |
| wrong fixes, with \|e\| | **2 257** — see below | reported |
| P1: trace rows > 0 | **852 260 rows, 0 unjoinable** | **PASS** |

**VERDICT: NO-GO for Phase 3.**  The pre-registration named this exact outcome — *"a wrong-fix rate
> 1 % means the covariance must become carry-aware first"* — and the measured rate is **14.48 %,
fourteen times the threshold.**

**What the wrong fixes look like, and why it is the covariance.**  Every one of the 200 largest-\|e\|
wrong fixes in the settled window is off by **exactly ±1 cycle** (offset histogram −1: 91, +1: 109),
with \|e\| between 0.90 and 1.10 cyc at a formal σ whose median is **0.091 cyc** — i.e. a **median
\|e\|/σ of 10.4**, accepted at ratio p50 14.0 and `p_succ` p50 **1.0000**.  The float sat a whole
cycle away and the ILS snapped, confidently, to the neighbouring integer.  The battery never had a
chance to refuse: its inputs are the covariance's own ratio and success probability, and the
covariance does not know what it does not carry.  The run-end clause says so in one word —
`carry-blind`.

**The tell that makes it diagnostic rather than merely bad: correctness gets WORSE as the run
settles.**

| window | ratio p50 (fix epochs) | correct-fix rate | searched \|e\| p50 | ceiling (\|e\| < 0.25) |
|---|---|---|---|---|
| h0–12 | 11.31 | **93.28 %** | 0.217 cyc | 67.1 % |
| settled h12–24 | **14.01** | **85.52 %** | 0.198 cyc | 72.4 % |

The gate's confidence rises 24 % while its accuracy falls 8 points.  A covariance that shrinks faster
than the error it describes is exactly what a carry-blind conditional block does on this vehicle: the
filter accumulates and the formal σ contracts, but the DD float carries an error inherited through the
phase-leveling carry that the conditional covariance never sees.  Both windows agree the search is
**not being refused by its gates** — it is fixing enthusiastically onto a float that has not arrived.

**The float, and the ceiling.**  Settled, the searched subset's wrapped \|frac\| p50 is 0.147 with
50.9 % under 0.15 and **72.4 % under 0.25** — so plain rounding would already be wrong on **27.6 %** of
the very population the `ils_max_dim` cap picked as its tightest.  Per band, settled subset: E:L1C
0.128 / ceiling 78.1 %, G:L1C 0.151 / 70.2 %, E:L5Q 0.151 / 73.0 %, G:L2W 0.161 / 68.9 %.  Correct-fix
rate splits the same way — **GPS 90.4 / 90.3 %, Galileo 79.6 / 81.7 %.**

⚠ **The graded population rotates** (§6.12's rule, and the grader reports it because of it): over the
2 869 epochs the star topology's reference SATELLITE switches 213 times on Galileo and 261 on GPS, and
the reference STATION 242 / 313 times across 52 / 46 distinct stations.  Every DD in this file inherits
its reference pair's own error, so a per-band or per-station split off this trace compares rotating
populations.  The whole-population rates above are the safe readings.

**What is NOT claimed.**  This does not say producer AR cannot work; it says the Phase-2 search
**cannot be promoted to Phase 3 pins on a carry-blind covariance**, because the gate that would decide
which pins to trust is provably not a discriminator here (85.5 % correct at p_succ 1.0000).  Nor does
it re-open the DO-NOT-BUILD verdict on producer DD-*fixing* (M5) — Phase 2 is a diagnostic search plus
its grader, and it stays log-only.  ⇒ **The next step on the AR track is `network.carry_aware_covariance`
applied to the Phase-2 DD block, re-graded with this same instrument, before any Phase-3 build.**  The
83 % ceiling that §6.16.6 measured on the estimator's own DDs is real, but it is not reachable through a
covariance that cannot see the carry.

---

### 6.19f THE EVENT HARNESS — the development loop changes

**USER DIRECTIVE, 2026-09-10** (`feedback_event_harness_over_daylong_runs`), after a day of 6 h and
24 h arms:

> "Why do the runs take so long?  I thought our simulated network would create rapid development and
> fast iteration.  Let's say a pivot handover is evaluated.  We simply run the network for 1 h, or
> however long to reach convergence, then inject the simulated handover, continue running for a few
> more epochs to study the effects.  This approach seems much faster and easier to isolate problems
> than these full-day runs that are frankly taking way too long!"

**The day's own record is the argument for it.**  Almost every discriminator in §6.19a is a
SINGLE-EVENT question — what a re-level's value error does over the next 40 epochs, what T4 does to a
young arc, what a handover steps on the wire and how the consumer answers, whether an age-inflated arc
converges.  The storm beds answered all of them only STATISTICALLY, at 23–90 min per arm, with events
interacting so thoroughly that eleven probes returned eleven aggregate numbers and no mechanism.  The
one time this section isolates a mechanism cleanly (§6.19c's v3), it is because the *trigger* was
varied against a fixed bed — an event-level experiment done the expensive way.

**Design (branch `worktree-evharness`, building; nothing merged).**  Four pieces, three of which
already half-exist:

1. **A converged snapshot library.**  Quiet sim at 6 h and 12 h, field at 6 h.  The vehicle is the
   RTB-7 warm-resume snapshot, and `project_seam_continuity_card` closed the correctness question in
   2026-09: with `resume_frame_restore` + `resume_coupling_restore` on a QK/QP-bearing snapshot, a warm
   restart's consumer converges like a cold consumer on the continuous wire.  ⚠ Grade every seam card
   with a COLD-CONSUMER REPLAY, never the seam-epoch diff.
2. **A producer-side fault injector, `network.inject.*`** — default OFF, byte-identical when unarmed,
   each event scheduled at an absolute TOW and counted at both edges (invariant #7):
   `slip` (station, satellite, signal(s), Δcycles — the simulator's scripted-slip content, but injected
   at RESUME rather than baked into the fixture), `outage` (a tracking gap of N epochs — the class the
   simulator has NEVER had, which is why the gap-poison card of §12 is field-only),
   `anchor_handover` (force a PHASE_ANCHOR / PBGAUGE handover at a chosen epoch),
   `arc_erase` (force a T4-style one-solve refloat on a named arc, so the erase can be studied without
   waiting for RAIM to choose one), and `sat_clock_step`.
3. **A leg generator** — resume from a snapshot, arm one injection, run 40–60 epochs.  At 60 stations
   that is 1–2 minutes, against 23–90 for a storm arm.
4. **A per-event grader** — the arc's level trajectory against truth, the station's ZWD / iono / clock
   response, the emitted C and P step at the event epoch and after, and the consumer's reset and
   re-convergence.  Every one of these instruments exists; what is missing is the per-event framing.

**The rule going forward.**  Storm beds are for FINAL validation of a fix whose event-level behaviour is
already understood (they are the only place emergent plateaus live), never for the development loop; and
never more than 2–3 concurrent 60-station arms — the box was oversubscribed at 5–6 today and everything
ran 2–3× slow.  §6.19d's `carry_rx_clock_in_block` is the first card to be built this way.

---

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
   **⚠ AMENDED 2026-09-09 (§6.16.4): the block rows are NOT the T4 driver.** E2 (F3 off, single
   knob, same bed) has T4 one-solve arc refloats **35 010 vs E0's 22 968** — turning the block rows
   OFF makes the T4 storm 1.5× WORSE, so the "F3 creates T4 work" reading is falsified and the
   admission item is now about pool HYGIENE (σ̂₀ / denominator / readability), not about the erase
   rate.  The erase rate's own agent is E1's persistence gate (item 22).
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
22. **BOUNDED T4 IDENTIFICATION — the dominant agent of the metre-class ZWD plateau (opened
    2026-09-09, §6.16.4).**  Starving the T4 isolated-arc erase (`network.raim_arc_persist_epochs:
    1000000`, E1) halves the ZWD truth rms (2.098 → 0.933 m), takes sat-clock contrast −66 % and
    code-bias −52 %, and flattens the D3 arc-age slope — but costs the consumer 46 % (settled p50
    1.751 → 2.559 m), a third of F3's integer gain (ref-free WL 81.5 → 65.4 %) and leaves the
    post-fit pool un-screened (σ̂₀(phase) 1.42 → 2.69, T3c 0.816).  **The knob is a PROBE, not a
    fix — ledger UNVALIDATED, do not arm.**  The build is Tier-C3 solution-separation confirmation
    of the median-polish identification, i.e. a BOUNDED mass-refloat: erase only what a separability
    test confirms.  Prerequisite named by `project_network_ssystem_dia_assessment`: median polish has
    no separability analysis today.
    **⚠ SUPERSEDED 2026-09-10 by the truth-graded T4 census (§6.17b) — bounded identification is the
    WRONG LEVER.**  This item assumed T4 erases healthy arcs and that a separability test would stop
    it.  Calibrated against same-epoch quiet arcs (the bed's ambient per-band DD error is **1.699 cyc**,
    so an absolute 0.5-cyc cut calls 83 % of untouched arcs "OFF"), the median erased arc is **2.59×**
    the ambient — T4 mostly confirms genuinely-off arcs.  Only **29 %** are indistinguishable from
    healthy, and the arc was already at that level 10 min earlier (grew in 44.5 % of cases — a coin
    flip).  What T4 is actually chasing is **its own previous re-level**: **48.4 %** of events fire
    within 15 min of one on the same (station, signal), the T4 → T4 chain runs 8.345 → **13.197** →
    14.082 cyc with the first erase making it WORSE in **64.4 %** of pairs, and a fixed-cohort profile
    goes k=0 3.031 → k=1 **4.192** → k=40 2.059 against a 1.653 floor it never reaches.  The disease
    is the re-level VALUE inheriting the station's corrupted per-epoch states — a station-wide
    positive-feedback loop, not an identification error.  Superseded by items 29 (the level-preserving
    ceiling, measured) and 33 (the split freeze).  ⚠ The knob itself stays a PROBE — ledger
    UNVALIDATED, do not arm; item 29's arms make that sharper, not softer.
23. **A LEVEL-PRESERVING gap re-entry response is unbuilt, and the two obvious candidates are spent
    (2026-09-09, `docs/SPEC_slip_qc_hardening.md` §12).**  The bare refloat
    (`network.cycle_slip.gap_reentry_relevel`) closes the defect — 88 → 0 bridged arcs, 129 freed,
    DD arc-wander tail 47 → 38 %, T4a −46 % — and still loses: float consumer +56 % settled rms,
    WROC00POL settled ZWD span 3.6 → 15.6 m, because each freed arc re-levels from ONE epoch of code
    (invariant #1's re-coupling edge, 129 re-seeds / 12 h).  Bridge v2 + v3 as built for detector
    slips (`relevel_delta_bridge` + `relevel_bridge_gf`, `runs/fldf3_pl2_bridge`) are FLAT on the
    same posture (52/282 pends replaced, 49 GF applies; consumer rms −1.5 % / p50 +17 %, ZWD span
    p50 0.797 → 0.904), so extending them to re-entries is not justified.  Design constraint: carry
    the level across the break at PHASE precision or do not act.  ⚠ The MW/LLI drain still has the
    identical live-state hole (`SPEC_slip_qc_hardening` §11 item 4) — a detector assertion on a
    not-yet-recreated ambiguity is silently inert there too.
24. **The ZWD-in-block card — LANDED and SCOPED-PROMOTED 2026-09-10 (§6.16.7d, four arms).**  The per-epoch ZWD has NO temporal prior
    and `carry_zwd` is blocked by item 17's block-coverage gap, so every erase / re-level that makes
    a young arc leaks into the wet delay: D3 interaction rms 4.264 m at arc age 0–60 s → 1.974 m at
    1200 s+ (E0), and the leak vanishes when the erases do (E1 flat 3.114 → 2.932).  The card puts the
    station's ZWD column INSIDE F3's marginal block, so the wet-delay direction is not left to absorb
    what a freshly re-levelled arc cannot explain (item 17's "fix = block ALL carried local columns
    of the station" is the same clause, one column wider).  Independent of items 22 and 23; grade it on the
    settled ZWD span (§6.16.1), not on the consumer.  **Outcome:** four arms (quiet sim Q, slipped sim Z,
    field-diagonal F, field-in-block F2).  The vehicle distinction is the whole card — the DIAGONAL
    row is the falsified double count (quiet sim consumer 0.723 m vs in-block 0.0877 m; field
    diagonal T3c 7 619 flags vs in-block 849 on the same data), so the in-block variant is not an
    optimisation of `carry_zwd`, it is the only correct way to carry it under F3.  Graded on its own
    channel as item 24 asked: fleet settled span p50 1.195 → 0.143 m on the field (F2), 1.590 →
    0.207 m on the slipped sim (Z), max 491.5 → 5.1 m.  The consumer came along anyway on the field
    (3D rms 4.034 → 2.926 m, −27 %; UP mean +2.834 → +0.072 m) and the QC tiers went QUIETER
    (T3 cells −31 %, T3c −18 %, T4a −20 %).  ⇒ ledger `network.carry_zwd_in_block` =
    **RECOMMEND_SCOPED** in regime `on("network.carry_zwd") and on("network.phase_leveling_local_block")`;
    the PAIR is a RECOMMEND_SCOPED candidate for `phase_leveling` + F3 lineages whose golden pin
    awaits the user's decision, and the loader now emits a log-only RECOMMEND for `carry_zwd` + F3 +
    in-block-OFF.  Still open on the field: a 1.86 m zero-mean UP wander plus ~1–1.8 m N/E (the
    horizontal got worse, +35 % / +46 %), unattributed.  **Same-build control DONE 2026-09-10
    (§6.17a): `runs/fldf3_on2_ctl_sb` — the control re-run on the HEAD exe — is BYTE-IDENTICAL to
    `runs/fldf3_on2` (`products.txt` sha256 `18aa2854b2575658…`, plus `closed_loop_trace.csv`,
    `network_zwd.csv` and `network_flag_rates.csv`), so the F2 A/B is same-build by proof and every
    number above stands as a single-knob-pair contrast.**  This item is now CLOSED.
25. **Tool debt / telemetry (2026-09-09).**  `scripts/simnet_plateau_decompose.py` raised inside
    `np.percentile` on an arm with ZERO T4 events (exactly E1) — guarded 2026-09-10 at
    `scripts/simnet_plateau_decompose.py:172-175` (empty jump population now prints `n=0` and skips
    the summary); the E1 numbers in §6.16.4 were produced with a scratchpad copy carrying the same
    two-line guard.  Also: E1's `network_flag_rates.csv` / `network_run_summary.csv` were lost to a
    box reboot, so its T3 / T3c rates are RECONSTRUCTED from `logs/network_raim.csv`
    (`count(spiked==1)` = T3 flags, distinct `(tow,sat)` = T3c opportunities, distinct with
    `n_sta_spiked ≥ min_sta` = T3c flags — validated exactly on three runs where both sides exist),
    and its T4a tier count is NOT recoverable.
26. **BOUNDED IDENTIFICATION, SECOND HALF — a station-level response the network's own screens can
    already drive (opened 2026-09-10, §6.16.7d; PREMISE AMENDED same day by Arm F2).**  Item 22 named the
    first half (Tier-C3 solution-separation confirmation of the median-polish proposals, so a
    mass-refloat is bounded by a separability test rather than by a polish residual).  ⚠ **The premise, stated honestly.**  The
    misattribution storm this item was opened on — **T3c 1 105 → 7 619 flags (6.9×), T3 cells
    22 713 → 60 222 (2.7×), T4 one-solve refloats 3 899 → 5 156**, with the h5 = 7.36 m / h7 =
    5.14 m consumer excursions — was measured on Arm F, which the loader had silently reduced to the
    **DIAGONAL** `carry_zwd` vehicle.  Arm F2 ran the real in-block vehicle on the F3 posture and
    every one of those tiers went the OTHER way (T3c 1 038 → 849, T3 cells −31 %, T4a 1 187 → 952,
    T4 refloats 2 159 → 1 750).  **So there is no field misattribution storm; that storm was the
    diagonal row's double count.**  What still motivates this item is (a) **E1** — the T4-erase half
    of the sim plateau, where removing the erases flattens the D3 interaction (3.114 → 2.932) and
    accounts for half the ZWD sink — and (b) the **sim clock relocation** under in-block ZWD
    (contrast 8.05 → 11.70 m, iono⇄ZWD 2.46 → 3.03 m on the slipped bed): the tension the pinned wet
    delay no longer absorbs has to be identified and bounded somewhere, and on that bed it is not.  The station-congruence
    rider has been screaming about that station since §6.16.2 (**t = 2.2e6**, d = (−418, −144,
    −771) m ENU, `reported=1`) and is **measure-only**: there is no quarantine response, so the
    identification the network already has never acts.  Build, both edges (invariant #7): a
    station-level quarantine/deweight driven by the congruence screen, with a release policy, plus
    the Tier-C3 confirmation of item 22 — validated on BOTH beds with ZWD-in-block armed, since
    that posture is what exposes the residuals in the first place — and grade it on the SIM clock
    contrast and on all three consumer components, not on the field's T3c count, which is already
    quiet.  ⚠ **No longer a prerequisite for `network.carry_zwd_in_block`** — Arm F2 promoted that
    knob to RECOMMEND_SCOPED in regime `carry_zwd` + F3 on 2026-09-10 without it.  It remains the
    prerequisite for arming the **PAIR** as a golden default, and for the sim bed at all.
    ⚠ **RE-STATED 2026-09-10 (§6.17b/§6.17g): DEAD as a STATION-QUARANTINE motive.**  Half (b) of
    this item asked for a station-level quarantine driven by the congruence screen, on the theory
    that a station fault read as satellite faults was driving the field's consumer excursions.  The
    field residual census says the tropospheric channel is CLOSED after the in-block fix (fleet ZWD
    deviation sd 21.5 → 4.9 mm; the site's own ZWD explains 1.8 % of the UP variance) and the
    remaining residual is ONE slow axis at el 49.8° / az 9.3° pointing into the site's constellation
    void, with every block-level "driver" collapsing under an autocorrelation-corrected n (p
    0.24–0.37).  There is no station to quarantine on the current field posture.  What survives of
    this item is the SIM clock relocation under in-block ZWD, and item 22's half is now superseded
    outright (see there).  Any future quarantine card must first show a station whose exclusion moves
    a truth-graded channel — §6.16.2's roster A/B already showed exclusion only RELOCATES the sink.
27. **CONSUMER-SIDE WL/NL CASCADE ON THE UNDIFFERENCED MODEL — the AR track's step 1 (opened
    2026-09-10, §6.16.7b).**  The closed-loop consumer cannot fix per band and the blocker is its
    OWN float, not the wire: after 12+ h at a cm-level static position on the quiet truth bed the
    per-band float σ is **0.321 / 0.368 / 0.422 / 0.439 cyc** (L1C / L2W / L5Q / L7Q, p50 over
    h12–24 from `logs/ambiguity_trace.csv` joined to `truth_ambiguities.csv`), so ILS chooses
    between near-equiprobable candidates — **2 879 attempts, 0 fixes, ratio p50 1.01 (max 1.26),
    `ratio_battery` refusing 2 596 of them**.  With a free per-LOS slant ionosphere and no cascade,
    per-band ambiguities are undetermined by construction; the GF direction is code-determined.
    Build: MW-WL float (iono-free, σ ≈ 0.05 cyc over hours) → fix WL → NL from the IF float, on the
    undifferenced consumer model.  The machinery exists for the BSSD/user paths but `ar_wl_only` is
    INERT under `consumer_undifferenced`.  ⚠ Until this lands, **no per-band fractional grade is a
    statement about the wire** — neither the consumer's fix rate nor `simnet_grade.grade_phase`'s
    P-record SD fractions (confounded twice: the iono-direction gauge of §6.16.6 AND the float
    weakness measured here).  Step 2 of the track is an anchor-aware TRUTH grader on the WL/IF
    combinations of the P records; step 3 (network integer-leveled pins) stays behind both.
    ⚠ **REPLACED 2026-09-10 — the ORDER FLIPS: the WIRE comes first, and the cascade as specified is
    DEAD ON ARRIVAL (§6.17c/§6.17d).**  Step 2 was built (`scripts/consumer_float_truth_join.py`,
    `scripts/simnet_wire_integer_consistency.py`) and it answers step 1's premise.  (i) The
    consumer's lanes are already PRECISE **within an arc** — GPS EWL 0.026 cyc, Galileo EWL 0.015,
    WL 0.05–0.08, NL 0.053 — so noise is not the blocker; but **every** channel's per-arc offset sits
    at the uniform null (p50 |wrap(arc mean)| 0.22–0.30, only GPS EWL concentrated at 0.173).  That
    is a BIAS, and more integration cannot remove it.  (ii) The cascade's WL would come from
    `computeSatMwBias`'s MW SD wide-lane, which subtracts *the wire's own* satellite MW biases — the
    same biases producing those offsets — so it would lock onto WRONG integers with a high ratio:
    exactly the Jul-30 user-path failure (`project_pppar_user_control`, FAIL-UNSAFE).  (iii) Graded
    against truth directly, the WIRE is at the uniform null on all 12 channels, the consumer inherits
    it exactly (unwrapped slope +0.888, Pearson +0.936 on `W = dC − dP`), and the offset is a stable
    per-(sat, band) gauge (within-tenure circular std 0.024–0.036 cyc) that is **an arbitrary
    receiver's float ambiguity** — `round(−dP/λ)` matches SOME station's truth integer 50–75 % of the
    time and the CURRENT anchor's 0–3 %.  **⇒ build order: (1) make the wire integer-consistent
    (design-doc Phase 2 diagnostic re-run with a truth-gradeable trace → Phase 3 integer pins,
    item 30); (2) only then the consumer cascade.**  The cascade stays a real gap — it is what makes
    a good wire fixable — but it cannot be validated, or safely armed, before the wire carries
    integers.
28. ~~**Quantify the `code_datum_time_linked` + `code_bias_gf_datum` "inconsistent gauge" pair on a
    real arm**~~ **CLOSED 2026-09-10 (§6.16.7e; `docs/SPEC_family_gf_datum.md` §2.1 addendum).**
    Single-knob 24 h A/B on the quiet golden (`runs/golden_sim_tlink_off` vs
    `runs/golden_sim_tri_w6_full`, same exe): OFF is **worse** — consumer settled rms 0.0762 →
    0.1013 m (+33 %), code-bias truth rms 0.1482 → 0.4182 m (max 0.52 → 3.70 m, concentrated on
    E:C7Q and G:C5Q), clocks and ZWD flat.  The pair injects no measurable tension on this bed and
    time-linking is load-bearing for the consumer: **keep it armed; the loader WARN stays
    informational.**  Not closed for the FIELD (no field A/B), and not a statement about a bed where
    the veteran set and the pinned set diverge — §2.1's rank-1 dependency is exact only when they
    match.
29. **A LEVEL-PRESERVING SLIP RESPONSE IS ONLY HALF A FIX — the ceiling is measured, and it does not
    move the plateau (opened and answered 2026-09-10, §6.17e; `docs/SPEC_slip_qc_hardening.md` §13).**
    `network.cycle_slip.oracle_relevel` shifts `LocalCarry::value_abs` by the TRUE integer jump at
    every detector actuation instead of freeing the arc — the sim-only CEILING of the engineering
    card (`SPEC_lane_slip_response`'s EWL/WL-MW jump estimator), so the card could be priced before
    it is built.  The mechanism is exact: unrepaired fraction |e(t₀) − e(t₀−30)|/|δ| **p50 0.005**,
    |e(t₀)|/|δ| **0.070**, holding at 0.078 after 150 s, latency-0 on 1 124 of 1 161 repairs.  And
    every pre-registered gate FAILS: ZWD 2.098 → **1.779 m** (target ≤ 0.5), ambient per-band DD |e|
    1.65 → **2.43 cyc (WORSE)**, T4 events 7 656 → **13 562 (+77 %)**, consumer p50 flat, ref-free
    WL flat at 81.5 %.  Adding the E1 T4 starve makes it **worse still** (ZWD 1.819, consumer p50
    3.781, WL 35.4 %) even though the ceiling instrument itself IMPROVES there (repairs 1 161 →
    1 549, `no_carry` fall-through 3 149 → 1 396).  ⇒ **do not build the engineering card as a
    plateau fix.**  It may still be worth building for the field's own reasons (slip-response
    quality), but it must be graded on a channel that is not the plateau.  Two residual defects of
    the instrument itself, both biasing the ceiling DOWNWARD and worth fixing if it is re-used: the
    **25.6 % detected-but-KEPT** class (1 587 unexplained `oracle_keep` rows whose arcs sit at
    |e|/|δ| ≈ 0.89 — the window or the (station, sat, signal) keying missed them), and the 5.9 %
    SILENT class the detectors never saw.
30. **THE WIRE'S INTEGERS ARE THE AR TRACK'S CRITICAL PATH, and the emission-side shortcut is
    FALSIFIED (opened 2026-09-10, §6.17d/§6.17f).**  `network.emit_pb_gauge_level` estimated the
    per-(sat, signal) common fractional part from the carried stations and added it back at
    emission.  It actuated (76.0 % of P records levelled, mean |δ| 0.65 cyc) and FAILED 4 of 5
    science gates: the wire's SD fraction did not move on any of 12 channels, the float consumer got
    **+36 % worse** (0.0761 → 0.1037 m rms), and the wire's within-tenure circular std ROSE 1.1–1.9×.
    Two root causes, both general lessons: (i) the premise is only weakly true — the carried
    per-station SD fractions have R p50 **0.572** at n ≥ 5 against ≈ 0.41 for pure noise at n = 6,
    and the carry covers a median of **6 of 61** stations, so 75 % of innovations are rejected before
    the estimator sees them; (ii) an **unwrapped EWMA is an integrator** — per-epoch steps are tiny
    (p99 0.039 cyc) but half of all families walk more than a full cycle in 24 h and G03/L1C reaches
    **−7.63 cyc = −1.45 m** of injected bias under one unbroken reference.  Any future emission-side
    gauge needs a bounded/wrapped δ̂ with per-epoch authority and a much higher concentration bar.
    ⇒ **the corrected build order is the design doc's own**: Phase 2 (the producer's DD search,
    `NetworkProducerAr` / `network.ambiguity_resolution.*`) re-run on the quiet golden — it was
    falsified only on the pre-F3 bed at near_int 34 %, and the quiet bed now carries 87 % ref-free
    DDs at the integer — then Phase 3 integer pins replacing the float-valued `w=1e10` pivot pins,
    then the consumer cascade of item 27.  **Prerequisite: Phase 2 is LOG-ONLY today.**  A trace
    (`network_producer_ar.csv`: per-DD float value, fixed value, ratio, and the pair identity) plus a
    truth grader against `truth_ambiguities.csv` must exist before its fixes can be believed — a fix
    count is not a grade (`project_pppar_user_control`).  The branch
    `worktree-agent-a7e2586003a605548` (commit `9b59a2c`) is NOT merged and stays as the vehicle for
    item 31.
31. **FIELD EMISSION CONTINUITY — pre-registered, in flight (opened 2026-09-10, §6.17g/§6.17h).**
    The field's emitted CLOCK is smooth (within-tenure deviation from a 30-min local-linear fit p50
    **2.6 cm**, only 3.9× the COD final clock's own floor) but the emitted PHASE BIASES **step by
    metres**: pc_IF p50 0.46 m, p90 2.3, max 10.5; **69–92 %** of that variance is within-tenure
    steps > 0.5 m; 1 012 pc_IF steps > 0.25 m in 6 h = 5.3 per satellite per hour, unmarked by
    construction (the per-epoch cross-satellite median is removed first), i.e. invariant #7 edges
    with no event.  The 5 min after a PBGAUGE (349) or PHASE_ANCHOR (382) handover carry **6.9 m rms
    against 1.4 m elsewhere (×5)** — the "continuous" handovers cold re-realize because the
    successor's variance exceeds `handover_max_var`.  Every such step trips the 0.25 λ discontinuity
    test ⇒ a consumer ambiguity RESET, which is why the field consumer never holds an arc.  Card:
    `network.emit_pb_gauge_mode: continuity` — a per-family emission offset that absorbs the frame
    step (0.10–40 cyc) with **no integer content** (the satellite's true bias is constant; the gauge
    is arbitrary and common to every station's product), so it is a different mechanism from the
    falsified item-30 levelling and its failure modes do not carry over.  Arms
    `_fldf3_on2_zwdblk_pbcont` vs `_logctl`; pre-registered: consumer resets −70 %, pc_IF
    within-tenure deviation 0.46 → ≤ 0.10 m, consumer 3D 2.93 → ≤ 2.0 m.  Note `emit_pb_filter` (the
    v1 continuity KF) is structurally OFF on the leveling vehicle — it is nested under `emit_rebase`,
    which is `carry_invariant`-only — so this is not a re-run of that.
32. **FIELD INSTRUMENT GAPS that blocked this morning's work (opened 2026-09-10).**  Three, all
    cheap, all measured as missing on the DOY-129 chain: (a) **`logging.enable_ambiguity_trace`** is
    off, so the field has no consumer ambiguity series and §6.17c's arc-offset decomposition has no
    field analogue (the field has no truth, but the per-arc offset STRUCTURE is truth-free); (b)
    there is **no consumer ZWD trace**, so §6.16.7c's height ⇄ consumer-ZWD correlation had to come
    from a separate log-only arm; (c) **`network.emit_clock_disc`** is unarmed, so the per-satellite
    CLK_STEP / CLK_ANCHOR rows are structurally empty and §6.17g's clock smoothness had to be
    measured truth-free off the wire.  Arm all three on the next field lineage — (a) and (b) are
    trace-class and fingerprint-SAFE; (c) is the companion the AGU-26 amendment 5 named for
    `network.clock_datum_broadcast_anchored` and still has no first validation, so without it every
    product-wide clock jump is silent to the consumer.
33. **THE (ρ, I) SPLIT FREEZE — the level's uncertainty along the split direction (opened
    2026-09-10, §6.17e; supersedes item 22's mechanism).**  With every T4 erase starved AND every
    detected slip repaired to its true integer, the slipped bed gets WORSE, not better: ambient
    per-band DD |e| **1.65 → 6.09 cyc**, ZWD 0.933 (E1 alone) → 1.819 m, ref-free WL 65.4 → 35.4 %.
    The per-band DD errors are equal in CYCLES while the lanes stay clean (E0 ref-free L1C 0.896 /
    L2W 1.086 / L5Q 0.980 / L7Q 0.816 against EWL **0.032**, WL **0.141**), and an error that is a
    constant ε cycles on every band is a (range, iono) split, not an ambiguity error: solving
    `δρ + γ_b δI = ε λ_b` on the L1/L2 pair gives (δρ, δI) = (0.1069, 0.0834)·ε metres and predicts
    L5's own per-band error to **0.6 %**.  ⇒ the station's zero-q levels are frozen around a
    SELF-CONSISTENT mis-split of range against ionosphere, which no per-arc response and no
    identification tier can undo — and T4's erases, destructive as §6.17b shows them to be, were also
    the only mechanism periodically re-randomising it.  **The lever is the LEVEL's uncertainty along
    the split direction.**  Candidate build: a per-LOS ionosphere carried IN-BLOCK, i.e. the same
    vehicle distinction the ZWD card paid for in §6.16.7d, one column per LOS instead of one per
    station.  ⚠ **Re-read `docs/SPEC_iono_carry*.md` and `project_iono_carry_8b_campaign`'s FOUR
    falsifications before building it** — a DIAGONAL per-LOS iono row is precisely the shape those
    falsified, and §6.16.7d's finding is that the diagonal vehicle IS the double count; if the
    in-block vehicle is what makes it work, that must be shown on the quiet bed first (the B9/Q1
    protocol), not assumed.  Second candidate, cheaper to falsify: a truth-consensus level oracle
    that shifts a whole station's levels to the truth-consistent split rather than one arc at a time
    — if THAT does not collapse the plateau, the freeze is not in the levels at all.
34. **THE LEVELS' MEMORY IS NOT THE LEVER — FALSIFIED ×4 (opened and closed 2026-09-10, §6.18a).**
    The first candidate against item 33's (ρ, I) split freeze was the levels' own stiffness: soften
    the zero-q prior and let the split move. Two knobs, four single-knob arms off E0 on the slipped
    bed. `network.phase_leveling_q_amb` 1e-7 / 1e-6 m²/s (`runs/golden_sim_campaign_plq7` / `_plq6`)
    HALVES the T4 churn (22 968 → 12 348 / 12 477 arc erases, −46 % — the first direct evidence that
    the erase tier is sensitive to level stiffness) and still leaves ZWD truth rms at 1.93 / 1.96 m
    (E0 2.098) while the truth |residual| MEDIAN gets WORSE (p50 0.123 → 0.256 / 0.450 m)
    and the consumer
    pays **+57 %** (settled p50 1.751 → 2.76 / 2.75). `network.phase_leveling_var_floor_m2` 1e-4 /
    1e-3 m² (`_plvf4` / `_plvf3`) is NEUTRAL on every channel (ZWD 2.089 / 2.060, consumer 1.85 /
    1.89, T4 22 524 / 21 429). Both knobs are non-monotone in their own value across a decade, i.e.
    past the point where the prior's scale sets anything. **Reading the data supports:** a re-levelled
    arc is code-quality in VALUE but phase-stiff in the block, so it is inconsistent with geometry
    until ~20 min of geometry change refines it, and T4 at E0's rate (21 erases per station-hour)
    erases it first — a limit cycle. Softening the prior lets the level MOVE but gives it nothing to
    move TOWARD while arcs keep restarting. ⇒ **do not re-run this ladder**; both knobs now carry
    KEEP_OFF ledger rows with these numbers, regime `on("network.phase_leveling")`.
35. **T4 ERASES ARE THE CRUDE CURE FOR THE ANCHORS' WALK — and the consumer feels the PHASE-clock,
    not the clock (opened and answered 2026-09-10, §6.18b/§6.18c).** The `network.raim_arc_persist_
    epochs` ladder K = 1 / 10 / 40 / 1e6 on the in-block posture splits the bed cleanly in two.
    **Producer, monotone with K:** ZWD truth rms 1.188 → 0.977 → 0.931 → **0.818 m**, satellite-clock
    datum-free contrast **11.79 → 2.66 m (−77 %)**, clock variance factor −92 %, T4 arc erases
    29 613 → 0. **Consumer, monotone the other way:** settled p50 1.085 → 2.296 → 2.727 (×2.5) and
    2.320 at K = 1e6 — the pre-registered gate (ZWD ≤ 0.5 WITH consumer rms ≤ 2.0) fails at every K,
    and no K tunes out of an inversion that holds across the whole range. The wire grade says why:
    **within-tenure WANDER rises monotonically with K** (Galileo L1C 0.249 → 0.793 cyc, GPS L1C
    0.507 → 0.944) while per-epoch JITTER is FLAT (~0.07 cyc), step size is flat (p50 0.21–0.28 cyc)
    and **handover counts are identical in every arm** (PBGAUGE 279 / PHASE_ANCHOR 297) — K changes
    the CORRELATION of the steps, not their number or size — and the motion is a genuine 2-parameter
    (range, iono) walk, R² 0.993–0.996, |dr| ≈ 1.2 cm/30 s, SHAPE invariant in K. Arc-level-removed,
    the clock channel is flat at 5–12 cm across the whole ladder (**the 4.4× clock gain is ALL arc
    LEVEL**) while the arc-removed PHASE-clock DEGRADES on every Galileo band (L1C 0.143 → 0.277 m).
    The consumer's degradation is a settled BIAS, not noise (3D mean 1.97 → 2.23–2.57 m, worst at K = 40;
    UP mean −0.22 → −1.95 m; wander and jitter both FALL), and the within-run causal test is decisive: the
    correlation of |error| with ARC AGE **flips sign**, −0.335 at K = 1 (error decays after a reset)
    to +0.20…+0.42 at K ≥ 10 with the per-age-bin mean growing inside each run. ⇒ **the erases chop a
    continuous walk into steps the consumer flushes by resetting.** Two carry-forward rules: (a)
    `sat-clock contrast` is NOT a consumer-relevant metric by itself — grade a consumer-facing change
    on the per-satellite PHASE-clock (C + P); (b) the fix is neither T4-off nor a K, and not bounded
    identification (item 22, superseded) — it is stopping the walk (item 37).
36. **FIELD EMISSION CONTINUITY — MECHANISM VALIDATED, PROMOTION KEEP_OFF ×2; the defect is the
    TRIGGER, not the plumbing (item 31's first round, answered 2026-09-10, §6.18d).** Three field
    pairs and one quiet-sim twin. The step-suppression half passes everywhere: consumer PB-disc
    resets −91 / −95 / −93 % (field v1 diagonal / field v1 in-block / field v2) and −93.4 % on the
    quiet sim, the emitter's own |ΔP| > 0.25 λ jump test −98 / −99 / −97 %, pb_L1C unmarked step rate
    0.671 → 0.000 /sat/h, and **emission-only by proof on every arm** (all "P," lines stripped ⇒
    byte-identical products, max |ΔC| = 0.0). On the correct in-block posture v1 also takes the field
    consumer's **UP rms 1.862 → 1.131 m (−39 %)**, passing the pre-registered UP sd gate, so the
    field vertical chain now reads `on2` 3.71 → in-block 1.86 → + continuity 1.13 m. **What blocks it
    is the trigger.** `dev()` is linear, so dev(arm) = dev(ctl) + dev(STAIRCASE), and the staircase is
    measured: 14 887 firings in the settled window at one-epoch |jump| p50 **0.026 m** against a
    0.10-cycle trigger of **≈ 1.9 cm** — it is absorbing routine phase-bias NOISE (~23 firings per
    family per hour), each absorption a permanent offset, so the emitted level RANDOM-WALKS (p50
    18.8 m of accumulated per-satellite excursion in 6 h) and the consumer's N rises +25 % on a
    near-horizontal due-south channel 84° off the geometric null. A datum artefact was tested and
    REJECTED (the per-epoch trim datum is QUIETER in the arm). **v2 pend-and-confirm is a REGRESSION**
    (3D 2.93 → 4.47 m, UP rms 3.46 with a −0.921 m/h ramp, G3 0.844 m) for a reason that is not a
    tuning question: the emitted P offset equals `delta_cont_cyc` EXACTLY on every row **including the
    42 386 PENDING rows** (p50 = p99 = max = 0.000000), **96.8 % of the offset motion happens on
    non-confirmation rows**, and 68 % of confirmations are force-resolved at the 13-epoch rail — it
    gated the LABEL, not the WIRE. The revert path is correct (190/190 restore to the bit) and fires
    on 4.1 % of pends. ★ **And the walk is a property of the TRIGGER, not of the field's event
    content:** on the quiet sim with ZERO truth slips the accumulated |δ_cont| still reaches p50
    28.6 cyc = 5.4 m on L1 over 24 h, ~9× slower per hour only because 96.2 % of its candidates are
    sub-min against 72.3 % on the field. ⇒ **v3 = hold the EMISSION (not the label) for the
    confirmation window, or raise the trigger well above the noise floor / gate it on a declared
    re-reference** — in build. Owed and untouched by v2: the 196 rail pass-throughs (> 40 cyc) in 15
    epochs, 9 with no handover marker, whose sim analogue is 24 of 26 DECLARED — undeclared
    network-wide frame motion is a FIELD defect (invariant #7's output edge), not a property of the
    gauge. Rider, from the 2×2's control column: **the in-block ZWD posture itself doubles-to-
    quadruples the wire's phase-bias churn** (resets 1 158 → 2 322, unmarked pb_L1C steps 0.671 →
    2.925 /sat/h, pc_IF deviation 0.230 → 0.464 m) — the wire-side price of §6.16.7d's vertical, and
    its own card.
37. ~~**THE IONO-IN-BLOCK CARD**~~ — **BUILT, GRADED ON BOTH BEDS, KEEP_OFF (opened and closed
    2026-09-10; build §6.18c, results §6.19a / §6.19a.1).** The card ran exactly as pre-registered and
    the pre-registration FAILED on the slipped bed: ZWD truth rms 1.188 → 1.133 m (−5 %, gate ≤ 0.5)
    and the consumer REGRESSED, settled p50 1.085 → 1.592 m (+47 %, gate ≤ 0.6), with the mechanism
    demonstrably live (671 535 iono columns carried, **0 diagonal fallbacks**). The B9/Q1 protocol
    PASSED — on the quiet 24 h bed the arm is NEUTRAL (consumer 3D rms 0.0875 → 0.0890 m, +1.7 %; ZWD
    truth 5.6 mm both; 2.75 M columns, 0 fallbacks) — which settles a second question worth keeping:
    **the four `project_iono_carry_8b_campaign` falsifications were the DIAGONAL row's over-count, not
    "carrying iono"**, exactly as §6.16.7d found for ZWD. The one channel that moved the right way is
    the INTEGERS (L1C settled ref-free med|e| 1.77 → 1.32 cyc, −26 %; share rounding to the true
    integer 21 → 25 %) — carried forward as evidence for the AR track, not as a reason to arm this.
    Rider: on the quiet bed the extra carried direction per LOS gives the post-fit consensus more to
    flag (T4 arc erases 159 → 408 with ZERO truth slips). Ledger row `network.carry_iono_in_block`
    (+ `_q_m2`, `_max_gap_s`) = **KEEP_OFF**; branch `worktree-ionoblk` (`7311ad0`) UNMERGED. The
    original pre-registration, for the record: `network.carry_iono_in_block` on branch
    `worktree-ionoblk` (from `dd6c81c`): the I half
    of the frozen (ρ, I) split carried INSIDE the F3 local block with process noise — one column per
    LOS where the ZWD card is one per station. The motive is now measured rather than argued: with the
    erases starved the anchors' levels WALK along a 2-parameter (range, iono) direction at R² 0.99,
    and the consumer integrates it (item 35). Pre-registered: slipped-bed ZWD ≤ 0.5 m **and** consumer
    settled p50 ≤ 0.6 m, quiet-bed consumer within +10 % of the golden. ⚠ **The B9/Q1 protocol is
    mandatory here** — `project_iono_carry_8b_campaign` falsified a DIAGONAL per-LOS iono row FOUR
    times, and §6.16.7d's finding is that the diagonal vehicle IS the double count; the quiet bed must
    show the in-block vehicle neutral BEFORE any slipped-bed claim, exactly as the ZWD card did. ⚠ And
    grade it on the PHASE-clock and all three position components, not on the satellite-clock contrast
    (item 35(a)) and not on the vertical alone (§6.16.7d's horizontal regression).
38. **A BRANCH/WORKTREE EXE SILENTLY IGNORES EVERY CONFIG KEY IT DOES NOT PARSE (opened 2026-09-10,
    §6.18d; `feedback_branch_exe_unknown_knob_silent`).** The PB-gauge worktree branched from
    `4bb799a`, so it lacked `966d4fd` (`network.carry_zwd_in_block`), `9963959` and `5f1f422`. Its exe
    passed an OFF-path byte-identity gate against HEAD on the QUIET GOLDEN smoke — which arms none of
    those — and the field pair `_fldf3_on2_zwdblk_{pbcont,logctl}` then ran with the sub-knob **dropped
    without a word**: no "ZWD in block" run-end clause, the diagonal carry instead, and every
    pre-registered ABSOLUTE baseline off the wrong posture. The JSON loader has no strict mode: it
    reads the keys it knows and ignores the rest. Three rules, all cheap: (a) before launching an arm
    on a branch exe, `git merge-base --is-ancestor <commit-of-every-armed-knob> <branch>` for every
    knob the arm arms beyond its golden; (b) grep the smoke console for the run-end clause or manifest
    echo of every armed knob — invariant #4, counters are the ON-path telemetry, and an absent clause
    means the knob was dropped; (c) **the `logctl` hash rule** — a log-only twin of a main-tree run
    must reproduce that run's `products.txt` sha256 exactly, and a mismatch is a posture defect, not
    noise. It was rule (c) that caught this one (`95afa6d9…` ≠ `c48b9a5c…`), and rule (c) that
    certified pair 2 (`logctl2` = `c48b9a5c…`, exact). Byte-identity on ONE posture proves nothing
    about knobs that posture does not arm.
39. **`network.serialize_disc_counters` IS OFF ON THE FIELD CHAIN — file consumers get no
    discontinuity signal at all (opened 2026-09-10, §6.18e).** The DOY-129 lineage emits the 6-field
    P record (`P,sat,signal,bias_m,sig_int,wl_int`) with no `phase_disc` column, while the wire is
    stepping at **~5.3 pc_IF steps per satellite per hour above 0.25 m** (205 above 1 m in 6 h, up to
    26 satellites in one epoch), driven by T4 RAIM-attribution refloats (`NetworkRaim.cpp:888-893`;
    r +0.546 |z| 8.7 on the control arm, +0.268 |z| 4.4 in-block, every other tier at noise). The R
    retro-bump fired **68 times against ~1 000 steps**. The closed-loop consumer only resets because it
    reads the in-process store counter; a FILE consumer of the same product is blind. That is
    invariant #7 with a bare OUTPUT edge, and it also made G2 of the continuity card UNMEASURABLE on
    its own terms (the grade had to fall back to the emitter's |ΔP| > 0.25 λ proxy). **Arm it on the
    next field template**, alongside item 32's three trace gaps (`logging.enable_ambiguity_trace`, a
    consumer ZWD trace, `network.emit_clock_disc`) — all of them trace/serialization-class, none of
    them solve-affecting.
40. **THE SLIPPED-BED PROBE LEDGER IS CLOSED — the station frame WALKS under arc churn, and every
    response that reduces re-levelling makes the consumer worse (opened and closed 2026-09-10,
    §6.19a).** Eleven config-only and card-level probes against the in-block control
    (`runs/golden_sim_campaign_zwdblk`: ZWD truth rms 1.188 m, consumer settled p50 1.085 m, T4 29 613
    arc erases): oracle exact repair, oracle + E1, level q ×2, var floor ×2, persistence K = 10/40/1e6,
    `raim_k_sigma` 4, iono-in-block, young-arc inflation ×2, false-alarm-only keep, receiver-clock q.
    **All FAIL both gates** (ZWD ≤ 0.5 m *and* consumer p50 ≤ 0.6 m). The single reading that predicts
    the whole table: the station frame — its stiff zero-q levels together with its per-epoch ZWD,
    receiver clock and slant iono — walks (measured at 1.2 cm/30 s range-like, §6.18c), so an arc is
    consistent with it only if periodically re-levelled INTO it. ⇒ the erase-rate family is BRACKETED
    (k = 6 / K = 1 is its consumer optimum; more erasing is noise, less is drift), the level's
    stiffness is not the lever (item 34), the response's ACCURACY is not what the consumer buys from it
    (item 35 + §6.19b), and the only thing that ever helped is a temporal prior on the ρ direction
    (ZWD-in-block, −43 %). ⚠ Method riders for anyone re-using this table: the T4 opportunity
    denominator is identical (645 406) in every slipped arm, so the erase counts are directly
    comparable; and p50 vs rms disagree on this bed by design — the young-arc arm has the table's
    second-best ZWD rms and its worst ZWD p50 and consumer rms, so a p50-only ladder reads its tail as
    a win. WHAT IS STILL OPEN: a prior that makes the frame observable epoch-to-epoch in more than one
    direction (item 42 is the next member), and integer constraints (item 44). Both go to the EVENT
    harness (item 43) first.
41. **FALSE-ALARM RE-LEVELS ARE A RESYNC MECHANISM — "tighten the detectors" is dead as a card
    (opened and closed 2026-09-10, §6.19b).** The slipped bed's detectors fire on nothing as often as
    on something — **2 371 of 4 708 slip-tier events have no truth slip in the window (MW 1 132,
    IF-TD 1 239)**, ≈ 6.4 false re-levels per station-hour against 6.8 true slips, while detection
    itself is fine (94.0 % of the 2 486 truth slips detected, 3 % silent). The isolating arm
    (`oracle_true_slip: free` + `oracle_false_alarm: keep`, `runs/golden_sim_campaign_zwdblk_orafa`,
    commit `3dd1284`) KEEPS 6 804 false alarms and frees every true slip as today — and the consumer
    gets **2.7× worse** (settled rms 2.389 → 4.46 m, p50 1.085 → 2.92) with **62 % more T4 erases**.
    ⇒ on this bed the false re-levels are doing the resync work that keeps arcs inside a walking frame,
    so a better detector would remove them and leave the walk. Together with §6.17b (T4 mostly
    re-erases the damage its own previous re-level caused) this closes BOTH accuracy-of-response
    levers — item 22's bounded identification and this one. ⚠ Counting basis: the census counts EVENTS
    per (station, satellite) (~2 300 per 6 h); the oracle counts per-signal ACTUATIONS (6 804). Never
    mix them in a rate. ⚠ Still OPEN and NOT addressed by this: the fixture's phase noise is ~5× the
    field's and the MW thresholds are field-empirical, so this false-alarm rate is a property of THIS
    BED (§6.14.3), and the same probe on the field is unowned.
42. **THE RECEIVER CLOCK HAS NO TEMPORAL PRIOR ON THE PRODUCER — card
    `network.carry_rx_clock_in_block`, pre-registered, in build (opened 2026-09-10, §6.19d).**
    `filter.process_noise.clock_bias_q` is **INERT** on the network producer: the per-station receiver
    clock is a per-epoch LOCAL parameter — estimated fresh, marginalized, never carried — so the knob
    has nothing to act on, and an arm that sets it is armed-and-inert (`_zwdblk_rxq` `products.txt`
    byte-identical, `bba9a7f2865070bc…`; T4 counter identical to the digit). Meanwhile the object it
    would model is real and simple: the simulator's own receiver-clock truth is a deterministic drift
    plus a WHITE increment — mean step +2.997e-08 s / 30 s (≈ +0.2995 m/s) with the step's sd
    1.0055e-09 s = **0.301 m**, second-difference sd 1.436e-09 s = √2 × that. Build: carry the receiver
    clock INSIDE the F3 whitened local block (the ZWD card's sibling; the fourth member of the
    (amb, iono, ZWD, clock) block), q ≈ 0.09 m²/epoch, in-block or nothing, counted diagonal fallback,
    the same loader interlocks, fingerprinted, OFF byte-identical, B9/Q1 quiet-bed neutrality BEFORE
    any slipped-bed claim. ⚠ **Design risk: the drift.** A plain RW on the ABSOLUTE clock fights a
    9 m/epoch ramp, three decades above the 0.30 m increment — carry the RESIDUAL about a per-epoch
    re-linearized `x_lin`, or make the state a (bias, drift) pair. A stiff prior carrying a stale value
    across the ramp is standing invariant #1. ⚠ Validate on the EVENT harness (item 43) first.
43. **THE EVENT HARNESS IS THE DEVELOPMENT LOOP NOW — USER DIRECTIVE (opened 2026-09-10, §6.19f;
    `feedback_event_harness_over_daylong_runs`).** Develop on a CONVERGED SNAPSHOT + ONE INJECTED
    EVENT + 40–60 epochs (1–2 min at 60 stations), not on day-long storm beds (23–90 min per arm).
    §6.19a is the argument: eleven storm arms produced eleven aggregate numbers and no mechanism,
    because every discriminator in it is a single-event question answered only statistically with the
    events interacting. Four pieces, three half-existing: (a) a converged snapshot library (quiet sim
    6 h / 12 h, field 6 h) on the RTB-7 warm-resume vehicle — correctness closed by
    `project_seam_continuity_card`, and ⚠ every seam card graded with a COLD-CONSUMER REPLAY, never the
    seam-epoch diff; (b) a producer-side fault injector `network.inject.*`, default OFF, byte-identical
    unarmed, each event scheduled at an absolute TOW and counted at BOTH edges (invariant #7):
    `slip`, `outage` (**the class the simulator has never had — the reason §12's gap-poison card is
    field-only**), `anchor_handover`, `arc_erase` (a forced T4-style one-solve refloat on a named arc),
    `sat_clock_step`; (c) a leg generator (resume → arm one injection → 40–60 epochs); (d) a per-event
    grader (arc level trajectory vs truth, the station's ZWD/iono/clock response, the emitted C and P
    step, the consumer's reset and re-convergence) — every instrument exists, the per-event framing does
    not. Branch `worktree-evharness`, building; nothing merged. **The rule: storm beds are for FINAL
    validation of a fix whose event-level behaviour is already understood, and never more than 2–3
    concurrent 60-station arms** (the box was oversubscribed at 5–6 on 2026-09-10 and everything ran
    2–3× slow). Item 42's card is the first to be built this way.
44. **THE PHASE-2 PRODUCER-AR DIAGNOSTIC IS GRADED — NO-GO FOR PHASE 3, AND THE BLOCKER IS THE
    CARRY-BLIND COVARIANCE (opened and answered 2026-09-10, §6.19e).** `runs/golden_sim_netar` (the sim
    golden + `network.ambiguity_resolution.{enable,trace,…}`, quiet 24 h truth bed, exit 0, product
    byte-identical by construction — Phase 2 is read-only). Against the arm's own pre-registration:
    fixed epochs **86.8 % PASS**, P1 trace rows **852 260 joined / 0 unjoinable PASS**, searched-subset
    |e| p50 **0.198 cyc FAIL** (gate ≤ 0.10), and CORRECT-fix rate among kept **85.52 % FAIL** (gate
    ≥ 99 %) — a **14.48 % wrong-fix rate, 14× the 1 % threshold the pre-registration named as the
    carry-aware trigger.** Every one of the 200 largest-|e| wrong fixes is off by **exactly ±1 cycle**
    (|e| 0.90–1.10 cyc) at a formal σ median of **0.091 cyc** — **median |e|/σ = 10.4** — accepted at
    ratio 14.0 and `p_succ` **1.0000**; the run-end clause names the cause in one word, `carry-blind`.
    ⭐ **The diagnostic tell: correctness gets WORSE as the run settles** (93.28 % h0–12 → 85.52 %
    settled) while the gate's ratio p50 RISES (11.31 → 14.01) — a covariance shrinking faster than the
    error it describes, which is what a carry-blind conditional block does on the phase-leveling
    vehicle. The search is not being refused by its gates; it is fixing onto a float that has not
    arrived (fixability ceiling 72.4 % settled, so plain rounding would be wrong on 27.6 % of the
    tightest DDs the cap chose). Galileo is worse than GPS on both (correct-fix 79.6 / 81.7 % vs
    90.4 / 90.3 %). ⚠ The graded population ROTATES — 213 / 261 reference-satellite and 242 / 313
    reference-station switches over 2 869 epochs (§6.12) — so quote whole-population rates, not
    per-band splits off this trace. **NEXT, and it is a prerequisite, not an option:
    `network.carry_aware_covariance` applied to the Phase-2 DD block, re-graded with this same
    instrument.** Cost to budget for the re-grade: `netar` is **2 152 ms of a 3 984 ms epoch (54 %)** at
    60 stations. The DO-NOT-BUILD verdict on producer DD-*fixing* (M5) is untouched — this is a
    diagnostic search plus its grader, and it stays log-only.

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
