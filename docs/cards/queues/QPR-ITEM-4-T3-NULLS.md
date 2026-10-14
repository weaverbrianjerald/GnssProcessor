# QPR 4: T3-class test statistics, correct nulls not recalibrated constants

**STATUS**

- **id:** QPR 4
- **topic:** queues
- **status:** superseded
- **summary:** T3's false-alarm excess is per-LOS ionosphere error, not a miscalibrated constant; R2 and R4 built measure-only; R1 and the rest of the T3 null work SUPERSEDED by the re-founding's code-row QC (RF-3).
- **dates in the text:** 2026-08-14 → 2026-08-21
- **knobs / posture:** `network.raim_station_factor`, `network.raim_rate_hygiene`, `network.raim_redundancy_normalize`, `network.estimate_iono_sh`, `network.trace_ionosphere`, `network.trace_raim_redundancy`
- **hashes named in the text** (commit / exe / byte-cert): `65ba362`
- **run dirs named in the text:** `runs/staf_`, `runs/pareref_on`, `runs/t3b2_`, `runs/t3fault`, `runs/t3iono`, `runs/simnet24_mp0`, `runs/t3mp0`, `runs/simnet24_inject_null` … (+3 more in the text)
- **source:** `docs/archive/QUEUE_principled_replacements.md`, item 4

**Outcome.** The item asked for the derived null of the T3-class statistic in place of a recalibrated 6σ constant. The statistic is scale-blind; its 1.86e7× excess over nominal is pool heterogeneity from per-LOS ionosphere error (interaction rms 0.764 m), so R1 was held and R2 and R4 landed measure-only. The fault-fixture gate passed (20 of 24 slips). The phase-pool calibration it served (R1, R3, the different-seed verify, the field B2 arm) is superseded by [RF-3](../../refound/RF-3_code_row_qc_2026-09-22.md)'s code-row QC; the prefit phase column is filed in [OPEN_ITEMS](../../status/OPEN_ITEMS.md). Status reviewed 2026-10-13.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: QUEUE_principled_replacements.md -->
## 4 — T3-class test statistics: correct nulls, not recalibrated constants

⚠ **DOWNGRADED 2026-08-14 (third amendment): R1 is HELD behind item 8.** The investigation below
found the root — per-LOS ionosphere error — and a stratified per-station variance component would
ABSORB it (0.11–2.55 m per station) into the noise model. Build **R2 (declare) and R4 (population
hygiene)**, correct regardless; hold **R1** until 8(a) reports.

✅ **R2 + R4 BUILT AND VALIDATED 2026-08-21** (`SPEC_t3_null_derivation.md` §15):
`network.raim_station_factor` (the declared §7.5 trimmed variance component, EWMA slow channel,
both-edge `T3_stafactor` depart/return events, feeds NO denominator — R1 stays HELD) +
`network.raim_rate_hygiene` (untestable cells out of the T3 opportunity count; T3c opportunities
unconditional).  Both measure-only, default-OFF, byte-identical OFF (fld63 recert vs the `65ba362`
references); solve-identity proven by intervention (armed ≡ control on `runs/staf_{ctl,on}`);
**corr(log fitted station scale, log run-end EWMA factor) = +0.9589** over all 24 stations, the §4
structure reproduced to the digit on the fresh exe; suite 1491/208 green.  Collateral finding:
§4's "1.67 % untestable" was a FORMATTING artifact of the 3-decimal L13 trace — the structurally
row-less population is 9 cells (0.04 %); T3c's like-for-like rate reads 1.10e-3.  One recorded
deviation: a single depart/return transient (run-max ratio 6.45) during EWMA convergence, closed
by the hysteresis — recorded, not tuned.  Still owed: the §8-step-2 different-seed null verify,
the field B2 unclamp arm before any FIELD grading, a station-fault bench for depart power.

**Bandaid:** a Gaussian 6σ quantile under a statistic that is a max-over-rows normalized by a
median-over-rows. Measured implied α: 3.78e7× nominal on the quiet 24-sta fixture, 2.7e7× on the
quiet 300-sta fixture; the implied-scale-vs-quantile curve rises monotonically (1.83 → 313) ⇒ the
residue is SHAPE, not scale; a 4.5 % floor survives datum election (pivot spec §6). Sibling in class:
the rx-CB admission gate's flat 26.7–28 % trip rate — but ⚠ that sibling does **NOT generalize**:
its numerator is a frame offset (`value_abs − x_lin`), not a residual, so it has no null to derive;
its replacement is already built (LV-4's frame-invariant `rxCbStepGate`) and blocked by
`SPEC_level_information_budget.md` §19.3 (+64 % postfit phase rms, ~15 TECU level move) and by the
absence of any truth-clean fixture arming `carry_rx_code_bias`. That chain is a POINTER, not a
verified finding.

~~**Principled replacement:** derive the null of the statistic actually computed — extreme-value /
order-statistics of correlated standardized residuals, or per-row w-tests under FDR.~~

✅ **DERIVED AND MEASURED 2026-08-14 — `docs/SPEC_t3_null_derivation.md` carries the full
derivation. The extreme-value framing above is the WRONG replacement.** All numbers below are on the
24-sta field-N sim null arm `runs/pareref_on` (1 h, GPS-only, slant-only posture) unless a row says
otherwise.

**§3 — the statistic is SCALE-BLIND.** `S_c = max_{i∈c}|z_i| / max(1, 1.4826·med_{j∈P}|z_j|)` is
invariant under `z → λz` (numerator and denominator both homogeneous of degree 1 in the same pool),
so no global variance-model fix and no recalibrated constant can move its false-alarm rate — only
the pool's INTERNAL heterogeneity can.

| quantity | value | reading |
|---|---|---|
| exact per-cell null `1−(2Φ(k)−1)^n_c`, k=6, n_c=3 | **5.92e-9** = **3.00×** the quoted per-row α | the EV correction is a rounding error … |
| observed excess over nominal | **1.86e7×** | … against this |
| denominator noise on the α=1e-6 threshold (N=1400) | 5.10 → **5.06** | negligible; the `max(1,·)` floor makes the shipped test *conservative* under a nominal null ⇒ **k=6 is already stricter than a 1e-6 per-cell test**, and a too-strict threshold cannot cause a 1e7× excess |

**§4 — the pool is a MIXTURE on a fixture with ONE global error model.**
`config/sim/simnet24_inject.json` has no per-station noise term and no `multipath` block, and the
emitted `_child_*.json` differ only in `station.seed_salt` — every station draws an INDEPENDENT
REALIZATION of the SAME model, worth a few percent of spread in a robust scale. Measured instead:

| axis | multiplicative spread | note |
|---|---|---|
| station | **4.7×** | flags concentrate in 11 of 24 stations, **S20 alone 33.7 %**, **4.13× overdispersed vs Binomial** |
| signal | **2.75×** (L1C **2.74** vs L2W **1.00**) | *not* elevation weighting (shared across signals at a cell) and *not* ionosphere in the naive sense (that would make L2/L5 worse) |
| epoch — the only axis σ̂₀ estimates | 1.3× | |

No observational covariate explains the station axis (|corr| ≤ 0.24 against elevation and truth iono).

**§5 — stratifying works OUT-OF-SAMPLE.** Station scale fitted on the first half of the settled
window and applied to the held-out half: flag rate **4.4625 % → 0.9736 %** at k=6, implied-c curve
flattening **2.25→3.93 to 1.97→2.39**, and the factor is transferable (corr(log scale, half 1 vs 2)
= **+0.847** over 23 stations).

❌ **FALSIFIED — the redundancy standardization** (`network.raim_redundancy_normalize`, default false
at `GnssConfigLoadNetwork.cpp:3461`). The statistic standardizes a POST-FIT residual by the
MEASUREMENT σ (`z ~ N(0,r_i)`, not `N(0,1)`) — true, and **not the cause**. Single-knob A/B
`runs/t3b2_{off,on}` (OFF arm bit-identical to `runs/pareref_on/logs_v2` on all four graded CSVs —
also the exe-provenance control): numerator **×1.4527**, denominator **×1.4179**, statistic
**×1.0248** — a 1.025 multiplier against a 1.86e7× excess; settled flag rate 4.381 % → **4.460 %** (slightly worse); kurtosis and station spread
unchanged; flags **798 → 772 at Jaccard 0.622** — it reshuffles WHICH cells flag without changing
HOW MANY, at **+38 % runtime**. ⚠ **Sim-scoped**: on the field its ×1.45 lift would *unpin* a clamped
σ̂₀ rather than cancel, so "do not arm it" does not transfer — a field B2 arm is owed.

✅ **GATE PASSED — the fault-fixture MDB check (§12).** Fault arm `runs/t3fault` (the 24-event
±{1,2,3,5,10,50}-cycle ladder in the validated null posture), station scale fitted on the NULL arm:
**20/24 detected at k=6 with a byte-identical per-rung profile** across shipped / station /
station×epoch, while flagged cells fall **912 → 235 (3.9×)**. Matched-budget at 200 flags: shipped
**17/24**, stratified **20/24**. Run-level, the 24 injected slips add only **0.52 points**
(**3.663e-02 → 4.186e-02**) on top of the false-alarm floor — the case for the card in one number.
T3 carries the whole SMALL end (±1/±3/±5 all 4/4) while ±10/±50 are grabbed upstream; 3 of the 4 T3
misses were caught upstream (**T0** MW / T1 IF / T2), and **one event is invisible to EVERY tier** —
S05000SIM G20 L2W **+50 cy at 50.1° elevation** — a genuine detection gap unrelated to this card.

⚠⚠ **FIELD (§13): the STRUCTURE reproduces, the LEVERAGE does not.**

| arm | station spread | half-to-half transferability | OOS station stratification | implied-c curve |
|---|---|---|---|---|
| sim 24 sta, 1 h (`pareref_on`) | 4.7× | +0.847 | 4.46 % → 0.97 % (**4.6×**) | 2.25→3.93 ⇒ 1.97→2.39 |
| field 49 sta, 1 h GPS+Gal (`net_matrix_2024-129/net50_1h`, DOY-129) | **10.5×** | **+0.873** | 27.58 % → 18.48 % (**1.5×**) | stays steep, 3.07 → 6.02 |
| field 303 sta, 354 ep, 1.02 M cells (`pdp305_pinaware/h3_guard`, 3 h) | — | **+0.782** | 9.57 % → **10.31 %** (WORSE than shipped); station × epoch 8.42 % (**1.14×**) | — |

Two mechanisms, both measured. (1) The per-station factor is **not stationary over 3 h** (+0.782), so
a once-fitted factor is stale by the second half — and not for sample-size reasons (~1300
cells/station/half at 305 vs ~200 at 24). (2) **σ̂₀ is clamped at the 1.0 floor on 95.7 % of
`net50_1h` epochs and 88.1 % of `h3_guard`'s, against 33 % on the sim**; in that regime the
denominator is constant and the statistic is an unnormalized max against a fixed 6σ bar, so a
stratified pool has nothing to fix. Why the field pool sits under the floor closes the loop: for a
post-fit residual E[w·v²] = r_i < 1, so the missing √r_i is exactly what pushes the median under 1.
⇒ **UNCLAMP FIRST on the field**, and R1's slow factor must TRACK (EWMA, time constant unmeasured),
not be fitted once; the slow/fast split survives, "slow = constant over the run" does not.
⚠ **Field runs have no truth** — **27.3 %** is not a false-alarm rate, and the tail (q999 **879**,
max **38 935**, 40 cells > 1000) contains real events; these are archived pre-VS2026 runs read for
residual STRUCTURE only, never for an accuracy verdict. Falsified in passing: the constellation axis
is a null (GPS vs Galileo scale **1.05×**, flag rates **27.23 %** vs **27.48 %**).
⚠ `scripts/t3_null_fit.py` **timed out (50 min) on 1 M cells** — a tool defect, since field scale is
exactly where it matters.

✅ **THE L1C EXCESS IS EXPLAINED — the residual lives in the IONO DIRECTION (§7.6).** Decomposing the
phase residual 3-vector onto (1,1,1) / (1,γ₂,γ₅) / perp: **common 5.02 % · IONO 89.76 % · perp
5.22 %**. With the common mode removed the iono direction is **(−0.804, +0.280, +0.525) —
L1-dominant**, because L1 has the SMALLEST iono coefficient (1 vs **1.65**/**1.79**) and sits
furthest from the mean. L1C is not noisy; **it is where unabsorbed iono-direction error lands.** The
per-station heterogeneity is the same effect: loudest 4 stations **93.2 %** iono share at **36.3 mm**
rms, quietest 4 **65.6 %** at **8.2 mm**. *Structural corollary:* every signal-independent error
(clock, tropo, orbit, position, tides) is identical in METRES across signals and can only appear in
the common direction — 5 %. So the culprit is necessarily frequency-dependent, and wind-up (∝λ) and
the ionosphere itself (∝1/f²) both have the **wrong sign**. ⚠ `network.estimate_iono_sh` is **OFF**
on this arm (free per-LOS slant iono), so an SH-truncation explanation is excluded.

✅✅ **MECHANISM — per-LOS IONOSPHERE ESTIMATION ERROR, truth-graded (§7.8).**
`network.trace_ionosphere` was frame-broken exactly as CLAUDE.md warns (it emitted the correction
`x_r` alone against a state SEEDED at allocation — median **0.95 m** vs the true **8.88 m**, ~11 % of
the quantity). Fixed on the ZWD block's pattern: `iono_m` unchanged, `x_lin_m` + **`iono_total_m`**
appended; trace-class, so with it armed the four graded CSVs are bit-identical to
`runs/pareref_on/logs_v2`. Arm `runs/t3iono`, config `_simnet24_inj_t3iono.json`, graded against
`truth_atmo.csv` `I_L1_m` over **13 054** settled triples. The state is LUMPED, so per invariant #5
only the datum-free **interaction** r (after removing receiver a(sta) and satellite b(sat)) is
gradable: **rms 0.764 m**, per-station **0.106 … 2.551 m**.
**corr(log station variance factor, log iono interaction rms) = +0.9258**; the **lumped** term
correlates **-0.103** — so it is NOT the receiver code-bias partition and not anything the datum can
move. **Causal chain:** iono state error **0.817 m** rms → ~96 % absorbed by the collinear per-arc
ambiguity → unabsorbed ~4 % lands in the phase residual in the iono direction (**0.033 m** rms) →
L1-dominant per §7.6 → the 29× per-station variance-factor spread. ⚠ It tracks the error's **LEVEL**
(+0.532), **not** its rate of change (+0.046) — the natural "the ambiguity soaks up the constant,
only the variation shows" guess is falsified. ⚠ **R2 is therefore non-negotiable**: a per-station
variance component would bury a measured 0.11-2.55 m per-station ionosphere error inside the noise
model.

✅✅ **AND IT IS THE INTEGER-RESOLUTION ROOT — MEASURED (§7.9).** From the phase observation equation
an iono error d forces `e_j = (gamma_j/lambda_j)·d` cycles into the ambiguity; in DD form the station
and satellite lumps cancel exactly, so the driver is the DD of the §7.8 interaction.

| band / lane | sensitivity (cyc/m) | rel. to L1C | measured rms DD error (rel.) | corr with DD iono error | var. explained, 0 free params | `round(e)=0` |
|---|---|---|---|---|---|---|
| L1C | **5.255** | 1.000 | 1.000 | **+0.9266** | **67.9 %** | 5.1 % |
| L2W | **6.744** | 0.283 | 0.085 | **+0.9543** | **81.0 %** | 51.0 % |
| L5Q | **7.037** | 0.056 | 0.018 | **+0.9585** | **82.8 %** | **95.9 %** |
| WL | **−1.489** | — | — | — | — | — |
| EWL | **−0.293** | — | — | — | — | — |

(7326 quadruples of `dd_cyc` − DD_true from `truth_ambiguities`. ⚠ the documented 1.49/0.29 lane
figures are MAGNITUDES; the sign is negative.) **EWL is the only near-integer rung and it is the one
with 18× less iono sensitivity** — the pre-registered handoff shape, now measured not inferred.
⚠ Two honest gaps: the FITTED per-band slope is ~**1.85×** the predicted one (**9.7 vs 5.26** on L1C)
and is unexplained (not a datum artifact — the DD annihilates both lumps); and the measured lane
ladder is ~3× steeper than the sensitivity ladder, so the lanes cancel more than iono alone.

❌ **TEN further candidates falsified (§7.7) — do NOT re-run**: phase multipath (arm) · code
multipath (arm) · redundancy r_i (census) · elevation **+0.24** · arc age (flat) · iono MAGNITUDE
**+0.089** · iono RATE OF CHANGE **+0.102** · receiver code biases **+0.264** (S05 has the LARGEST GF
bias and is nearly the QUIETEST) · phase-bias anchor/datum (universal at 22/23 stations) ·
constellation (1.05×). ⚠ Method note: candidate 8 was proposed from two cherry-picked extremes and
collapsed under correlation over all 23 stations — **propose from the correlation, not the ends.**

❌ **The top lead was the multipath, and it is FALSIFIED (§7.1).** The sim's Gauss-Markov phase
multipath (2 mm zenith, τ = 300 s, per-station seed) was zeroed outright
(`config/sim/simnet24_inject_mp0.json` → `runs/simnet24_mp0` → `runs/t3mp0`), with the control that
regenerating the *unmodified* fixture reproduces `runs/simnet24_inject_null` **byte-identically**
(5/5 RINEX by sha256). Station spread **4.7× → 4.8×**, per-signal spread **2.70× → 2.77×**, flag rate
went **UP (3.663e-02 → 3.906e-02)** — every structural feature survives removing the error source
entirely. Bonus: the removal lowered all three per-signal levels ~8–10 % uniformly and the statistic
did not follow — a second, independent confirmation of the invariance theorem.

❌ **The redundancy family is falsified too (§7.2), and the prerequisite got built.** New
**`network.trace_raim_redundancy`** (default OFF) emits one row per post-fit RAIM **phase row** with
`w, r_i, resid, z_meas, z_baarda, below_floor` — one level finer than the cell. It makes `row_r`
COMPUTED but never USED, so unlike `raim_redundancy_normalize` **the census is solve-neutral**: with
it armed all six graded CSVs are bit-identical to `runs/pareref_on/logs_v2` (+17 % runtime, **65 364
rows**). Arm `runs/t3ri`, config `_simnet24_inj_t3ri.json`. **r_i is uniform**: per signal L1C
**0.5469** / L2W **0.5767** / L5Q **0.4630** = **1.25× spread** against a `z_meas` spread of
**2.59×** — and dividing by √r_i makes the signal spread *worse* (2.59× → **2.72×**). Per station r_i
spreads **1.23×** against `z_meas` **3.01×**, corr(log z_meas, log r_i) = **−0.284**. Whole settled
population: median r_i **0.5114**, q01 **0.0015**, q99 **0.6687**. So "L2W/L5Q are suppressed by
absorption" is wrong — absorption would mean LOW r_i on exactly those signals. **The heterogeneity is
genuine misfit varying by station and signal, on an estimator whose information distribution is
uniform.**

✅ **`rowRedundancy` AUDITED and EXONERATED on four links (§7.4)** — it IS the right covariance: the
arrowhead block-inverse identity is EXACT (re-derived by hand); the datum is a true Dirichlet
elimination (contribution to RHS, row+col zeroed, diagonal set — `NetworkEstimator.cpp:280-292`), so
`shared_cov_` is genuinely the datum-CONDITIONAL covariance and the zeroing only strips the
`1/pin_scale` artifact; the priors are in the same system (`addStationPrior` throws if `solved_`; DD
priors folded pre-solve); and `reconstructStation` uses the identical `N_rr`/`N_sr`. The Bayesian
expansion also shows `Cov(v) = W⁻¹ − AΣAᵀ` survives a prior — **provided P is the true dispersion of
θ about θ₀**. That proviso is the whole story.

⚠⚠ **So the defect is the MODEL, measured as a VARIANCE FACTOR.** With `E[w v²] = r_i`, the estimator
`σ̂² = Σw·v²/Σr_i` must read 1.0. On **29 333** settled rows: **global 11.80**; by signal **L1C 27.37
/ L2W 2.05 / L5Q 5.87** (13.4× spread); by station **2.14 … 61.80** (**28.9× spread**) — all while
mean r_i stays flat at **0.45–0.55 (1.24×)**. **r_i correctly describes how much freedom the
estimator HAS; it cannot describe how wrong its model IS, and only the latter varies.** L2W at 2.05
is nearly model-consistent while L1C is 27×, so the mis-specification is CONCENTRATED on L1C rather
than spread over the pool. ⚠ Outlier-sensitive — aggregate **11.80** vs robust median **1.78** says a
robust numerator is not cosmetic.

~~⚠⚠ **A sharper defect fell out (§7.3): E[w·v²] ≠ r_i.**~~ *(superseded by §7.4 — the measurement
stands, the attribution moves from the covariance to the model.)* If `v` were a correct post-fit
residual, `1.4826·med|z_baarda|` would be 1.0. Measured: **z_meas 1.3683** (the shipped,
theoretically WRONG standardization) vs **z_baarda 1.9291** (the theoretically CORRECT one) — the
correct one lands **2.5× further from nominal**. Candidate then considered: `rowRedundancy` evaluates
on the gauge-nulled free covariance (`NetworkEstimator.cpp:451-453`), which is not the system that
produced `v` if the datum constraints bind. It explains §5 cleanly — B2 measured null *and* pushed
σ̂₀ **1.246 → 1.819** because it divides by something that is not the residual's variance. Incidental:
**the ignition ramp IS r_i** (0.0000 → ~0.52, then flat), so `s_raw`'s **0.836 → 1.816** rise is
redundancy converging, not the residual growing; the floor drops only **0.49 %** of settled rows,
identically across signals (**49/10026** each).

⚠ **"Excess kurtosis 4391" does NOT reproduce** — settled `s_raw` on the null arm has excess kurtosis
**10.38**. Do not carry 4391 forward without re-deriving it.

**Replacement, specified (NOT built):** a slow per-station factor × the existing fast network-wide
scale — the split is forced by the noise budget (a per-station-per-epoch scale has **24.3 % SD**;
pooled over the window, **5.7 %**) — with the station factor promoted to a **declared statistic**
rather than silently absorbed (invariant #7: absorbing model error into a denominator masks it), α
set by **FDR** over the (cell × epoch) family instead of a fixed k, and the **1.67 %**
structurally-untestable cells (`s_raw` exactly 0) dropped from the opportunity count. The right
per-stratum form is the variance component **σ̂²_stratum = Σ w·v² / Σ r_i** (§7.5) — what
`docs/SPEC_prior_residual_census.md §14` derives globally, now computable per station and per signal
because r_i is streamed; unbiased under the model rather than calibrated to a reference quantile, no
`n_c` assumption, and it degrades gracefully where a median of maxima conflates "quiet" with
"unchecked". Full build plan and the four owed validations: `docs/SPEC_t3_null_derivation.md` §8.

**What survives:** ~90 % iono-direction residual (two independent fixtures) and a 29× per-station
variance factor stable in time. Next probe: grade the **estimated states against truth**
(`truth_atmo`, `truth_bias`) instead of inferring from residuals.

**The class, ranked** (§11): **T3c (2.85e6×), T4a (4.89e5×, and it acts at ANY σ̂₀) and T4 are free
riders** on this fix — same σ̂₀, same `raim_k`, same file.

⚠⚠ **Two collateral defects found while inventorying, both verified on the null arm, both needing
their own card** (§14):

1. **The prefit census's PHASE statistic is degenerate by 4–5 decades on truth-clean data** —
   `pc_phase_max_s` median **1.024e5 σ** and `pc_phase_n1k` median **112 rows/epoch**, against a sane
   code class (**75 σ**, `n1k` = 0). `trace_prefit_census` is in the STANDARD ARMED SET, so this
   column is published network-wide; it is inert only because `prefit_event_classes` defaults to
   `code,sh,other` (`GnssConfigLoadNetwork.cpp:1410-1412`) — **adding `phase` to that mask, or arming
   `prefit_fuse_classes` with phase, yields ~112 fuse events per epoch on clean data.**
2. **T1's flag rate excludes an acting sub-path** — reported `26482 opp / 0 flags / rate 0` while
   `network_events.csv` carries **13 `T1_if_slip/refsat_reset` events**. `fr_if_slip` counts only the
   per-pair loop (`NetworkSlipQc.cpp:530`); the reference-satellite common-mode path refloats with no
   counter and no tier — standing invariant #4, one tier over from the T4a line added to fix exactly
   this.

**Sim validation — two standing rules for anyone fitting this null:**

- ⚠ **The fixture cannot calibrate a 6σ-class quantile empirically at all.** 9975 settled cells
  resolve no rate below ≈ 1e-4; a 5.9e-9 threshold would need ~1e8 cells, i.e. ~1e4 of these runs.
  The instrument is for measuring the pool's STRUCTURE and for the fit → out-of-sample verify
  protocol — never for reading a tail quantile off the data. This is why the replacement had to be
  DERIVED rather than fitted. Harness: `scripts/t3_null_fit.py` (`--selftest` / `--run` / `--fit …
  --verify …`).
- ⚠ **The verification arm must be SAME-REGIME, different-seed — NOT the 60-sta fixture.**
  `runs/simnet60_tri` is LEGACY-N (N ∈ [1e5,1e6)) and a different posture while the 24-sta null is
  field-N; the regime alone moves the DD channel by 24 points (`project_sim_field_n_regime`). Fitting
  on one and verifying on the other conflates REGIME with STATISTIC. Generate the verification
  fixture from the same `config/sim/simnet24_inject.json` with a different roster salt — the roster
  is deterministic in N, so only the noise/integer realization moves.

*(Both blockers that once held this item are CLEARED: R0 landed — σ̂₀_first is a per-epoch
`network_epoch_status.csv` column on every assessed epoch, where before it was emitted only inside a
trip event's `detail` span at `NetworkSolveGlue.cpp:1640`, so there was no null sample on disk at
all; and 1(d) landed, making `runs/pareref_on` the regenerated null arm — measurement-clean AND
carry-marginalized at all 30 handovers, terminal σ̂₀_first **1.39** vs the OFF arm's **2.16**. The
earlier premise "the truth-clean fixtures ARE the null-calibration instrument" was falsified for
`runs/inj_null_pdp`, which is measurement-clean in the strict sense — all 25 `truth_slips.csv` are
header-only — but carries **10 genuine state discontinuities**, the anchor handovers, so fitting a
quantile there calibrates it to the discontinuities.)*

**⚠ NOT in this class:** MW slip thresholds — empirical by standing decision (data-rate-dependent);
do not "theorize" them.

