# T3 mechanism — the per-row census, the falsification ledger, and the MECHANISM FOUND — per-LOS ionosphere estimation error, with the DD-integer tie

**STATUS**

- **id:** T3 mechanism
- **topic:** raim-qc
- **status:** closed
- **summary:** the per-row census, the falsification ledger, and the MECHANISM FOUND — per-LOS ionosphere estimation error, with the DD-integer tie
- **dates in the text:** —
- **knobs / posture:** `network.trace_raim_redundancy`, `network.trace_ionosphere`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/simnet24_mp0`, `runs/t3mp0`, `runs/simnet24_inject_null`, `runs/pareref_on`, `runs/t3ri`, `runs/t3cmp0`, `runs/t3iono`
- **source:** `docs/archive/SPEC_t3_null_derivation.md` — 7, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 7.8, 7.8.1, 7.8.2, 7.9

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_t3_null_derivation.md -->
## 7. The open question this card does NOT close

**Why does the post-fit standardized residual vary 4.7× by station on a fixture with one global noise
model, uncorrelated with elevation or ionosphere?**  That is a stochastic-model / model-error
question, not a test-statistic question.  Until it is answered the per-station factor is doing double
duty as a nuisance absorber — which is exactly why R2 (declare it) is not optional.

**✅ It has since been ANSWERED — §7.8: per-LOS ionosphere estimation error, truth-graded at
corr = +0.9258.**  The three original leads all died on the way there and are kept only as the record
of what was tried: (1) the sim's time-correlated multipath vs a white stochastic model —
**FALSIFIED §7.1**; (2) the per-signal split (L1C 2.7× L2W at identical elevation, weight and nominal
σ) read as frequency-specific and *not* ionospheric, since iono "would make L2/L5 worse, not better"
— **FALSIFIED §7.2, and the sign intuition was wrong: §7.6 shows the iono direction is L1-DOMINANT
once the common mode is removed**; (3) the ignition ramp (0.84 → 1.82) read as ambiguity convergence
changing the residual scale — **FALSIFIED §7.2/§7.3; the ramp IS r_i converging.**  Ten candidates in
total are dead; the ledger is §7.7.

### 7.1 Lead 1 FALSIFIED — it is not the multipath; and the per-row r_i census is the prerequisite it named

The sim's Gauss-Markov phase multipath was the best-motivated suspect: 2 mm zenith on top of 3 mm
white noise, τ = 300 s = 10 epochs, elevation-amplified, drawn per station from its own `seed_salt`.
**Tested by zeroing it.**  `config/sim/simnet24_inject_mp0.json` is `config/sim/simnet24_inject.json`
with `multipath.phase_amp_zen_m: 0.0` and nothing else changed; fixture `runs/simnet24_mp0`; producer
config `_simnet24_inj_t3mp0.json`, byte-identical to `_simnet24_inj_pareref_on.json` except for the
fixture paths; arm `runs/t3mp0`, same exe snapshot.  ⚠ **The control that makes this a single knob:**
regenerating the *unmodified* fixture into a fresh directory reproduces `runs/simnet24_inject_null`
**byte-identically** (5/5 station RINEX checked by sha256), so the generator is deterministic and the
only thing that moved is the multipath realization.

| channel | multipath ON | multipath OFF |
|---|---|---|
| T3 cell rate (all epochs) | 3.663e-02 | **3.906e-02** *(up)* |
| T3 rate, settled window | 4.381 % | **5.294 %** *(up)* |
| **station scale spread** | **4.7×** | **4.8×** |
| **per-signal spread** | **2.70×** (L1C 2.70 / L2W 1.00 / L5Q 1.32) | **2.77×** (L1C 2.50 / L2W 0.90 / L5Q 1.20) |
| station-stratified flag rate | 0.672 % | 0.622 % |

**Every structural feature survives removing the error source entirely**, and the flag rate moves the
wrong way.  Removing it also lowered all three per-signal levels by a near-uniform ~8–10 % — a
*common* rescaling the statistic did not follow, i.e. a second confirmation of §2's invariance by a
completely different mechanism from §5's B2 arm.

**The prerequisite this probe named, and BUILT in §7.2: `network.trace_raim_redundancy`** (default
OFF) emits one row per post-fit RAIM **phase row** —
`week,tow,station,sat,signal,row,w,r_i,resid_m,z_meas,z_baarda,below_floor` — one level finer than
the cell, because the RAIM cell is a max *over* signals so `network_station_residuals.csv` has no
signal column and per-row r_i was never streamed.  The design point that makes it usable: it causes
`row_r` to be **computed** but never **used**, so unlike `raim_redundancy_normalize` (which drops rows
at the floor and is solve-affecting) **the census is solve-neutral**.  Proven, not asserted: with the
census armed, `network_solution.csv`, `network_dd_fractional.csv`, `network_station_residuals.csv`,
`network_epoch_status.csv`, `network_events.csv` and `network_flag_rates.csv` are all
**bit-identical** to `runs/pareref_on/logs_v2`; with it off, no file is produced and the same six are
bit-identical.  Cost 17.4 s → 20.3 s (+17 %), 65 364 rows.  Arm `runs/t3ri`, config
`_simnet24_inj_t3ri.json`.  `z_baarda = z_meas/√r_i` is emitted *without* arming the normalization, so
both standardizations are comparable **on one solve**.

### 7.2 Leads 2 and 3 FALSIFIED — r_i is uniform

Lead 2's reframing was "L2W/L5Q are *suppressed* because the per-LOS iono state and the code biases
soak them up" — a per-signal redundancy story, i.e. lead 3 wearing a different hat.  **Both fall on
the census.**  Settled rows above the floor (29 931):

| signal | median r_i | median z_meas | median z_baarda |
|---|---|---|---|
| L1C | 0.5469 | **1.6833** | 2.3654 |
| L2W | 0.5767 | 0.6509 | 0.8706 |
| L5Q | 0.4630 | 0.8445 | 1.2705 |
| **spread** | **1.25×** | **2.59×** | **2.72×** |

**r_i is essentially uniform across signals (1.25×) while the residual it is supposed to explain
spans 2.59×** — and dividing by √r_i makes the signal spread slightly *worse* (2.59× → 2.72×).  The
same holds on the station axis: per-station r_i spreads **1.23×** against a z_meas spread of
**3.01×**, with corr(log z_meas, log r_i) = **−0.284** — far too weak to be the mechanism.  Over the
whole settled population r_i is remarkably flat: median **0.5114**, q01 0.0015, q99 0.6687.
Absorption would show up as low r_i on exactly those signals, and it does not.  So the heterogeneity
is **genuine misfit that varies by station and signal, on an estimator whose information distribution
is uniform**.

### 7.3 A sharper defect surfaced by the census: E[w·v²] ≠ r_i

If `v` were a correct post-fit residual under the assumed stochastic model, then $E[w_i v_i^2] = r_i$
and $z_{baarda} = |v|\sqrt{w}/\sqrt{r_i}$ would be exactly standard normal — so
$1.4826\cdot\operatorname{med}|z_{baarda}|$ should read **1.0**.  Measured, settled, above floor:

| standardization | 1.4826 · median | distance from nominal |
|---|---|---|
| `z_meas` = \|v\|√w — **the shipped one, theoretically wrong** | **1.3683** | 0.37 |
| `z_baarda` = z_meas/√r_i — **the theoretically correct one** | **1.9291** | 0.93 |

**The correct standardization lands 2.5× FURTHER from nominal than the wrong one.**  That says
$E[w v^2] \neq r_i$ on this solve, i.e. the redundancy numbers do not describe the actual residual
covariance.  Two concrete candidates: `rowRedundancy` evaluates on the **gauge-nulled free
covariance** (`NetworkEstimator.cpp:451-453`), which is not the system that produced `v` if the datum
constraints bind; or the stochastic-model error is large enough to swamp r_i entirely.  This is the
same class the in-house constraint-aware global σ̂₀ rider was written for
(`docs/SPEC_prior_residual_census.md`; formerly its own card `SPEC_sigma0_constraint_aware.md`, now
merged there), and it explains §5 cleanly: B2 measured null on the flag rate *and* pushed σ̂₀
1.246 → 1.819 precisely because it divides by a quantity that is not the residual's variance.

Incidental, and worth keeping: **the ignition ramp IS r_i.**  Median r_i runs 0.0000 → 0.0004 over
the first three assessed epochs and reaches ~0.52 by mid-run, then sits flat — the 0.836 → 1.816 rise
in `s_raw` (§4) is redundancy converging, not the residual growing.  And the floor drops only 0.49 %
of settled rows, **identically across signals** (49/10026 each), so it is not a per-signal filter.

### 7.4 `rowRedundancy` IS the right covariance — the model is what is wrong

§7.3 named two candidates for `E[w·v²] ≠ r_i`.  **The covariance is exonerated on four independent
links.**

1. **The algebra is exact** — `NetworkNormalEquations::rowRedundancy` computes
   $\mathrm{lev} = (a_s - M^\top a_r)^\top \Sigma_{ss} (a_s - M^\top a_r) + a_r^\top N_{rr}^{-1} a_r$
   with $M = N_{rr}^{-1}N_{sr}^\top$; re-deriving the arrowhead block inverse
   ($\Sigma_{sr} = -\Sigma_{ss}M^\top$, $\Sigma_{rr} = N_{rr}^{-1} + M\Sigma_{ss}M^\top$) gives
   $a^\top\Sigma a$ **identically** that expression.
2. **The datum is handled correctly** — `NetworkEstimator::solve` realizes each pin as a true
   **Dirichlet elimination** (RHS move, zero row *and* column, set the diagonal;
   `NetworkEstimator.cpp:280-292`), so `shared_cov_` is conditional *on* the datum and the row/col
   zeroing at `:444-449` only strips the artificial `1/pin_scale`.  Redundancy numbers are
   datum-invariant for estimable rows, and every row here is a row of A.
3. **The priors are in the same system** — `addStationPrior` throws if `solved_`, forcing injection
   *before* the Schur reduction, and the DD priors fold into the reduced shared normals
   (`NetworkEstimator.cpp:243-251`) before `solveSharedConstrained` fills `shared_cov_`.
4. **The residual comes from that system** — `reconstructStation` solves
   $x_r = N_{rr}^{-1}(u_r - N_{sr}^\top x_s)$ with the **same** `Nrr_ldlt` and `N_sr` the leverage uses.

Nor is the identity naive-LS-only: for a Bayesian solve with prior $\theta\sim N(\theta_0,P)$, the
prior contributions to $v = y - A\hat\theta$ cancel exactly and
$\operatorname{Cov}(v) = W^{-1} - A\Sigma A^\top$ **still holds — provided P is the true dispersion of
θ about θ₀.**  That proviso is the whole story.

**What the census measures is therefore a VARIANCE FACTOR, enormous and structured.**  With
$E[w v^2] = r_i$, the classical variance-component estimator $\hat\sigma^2 = \sum w v^2 / \sum r_i$
must read 1.0.  Settled rows with r_i > 0.01 (29 333):

| stratum | σ̂² = Σz²/Σr | σ̂ | robust med(z²/r) | mean r_i |
|---|---|---|---|---|
| **global** | **11.80** | 3.44 | 1.78 | 0.51 |
| L1C | **27.37** | 5.23 | 5.85 | 0.5151 |
| L2W | **2.05** | 1.43 | 0.80 | 0.5548 |
| L5Q | 5.87 | 2.42 | 1.70 | 0.4464 |
| S06000SIM (worst) | **61.80** | 7.86 | — | 0.4496 |
| S16000SIM (best) | **2.14** | 1.46 | — | 0.5373 |

**The variance factor spreads 13.4× across signals and 28.9× across stations while mean r_i is flat
at 0.45–0.55 (1.24×).**  That is the cleanest statement of the whole card: **r_i correctly describes
how much freedom the estimator HAS; it cannot describe how wrong the estimator's model IS, and only
the latter varies.**  L2W sits at 2.05 — nearly consistent with the assumed model — while L1C is 27×,
so whatever is mis-specified is concentrated on L1C rather than spread over the pool.  (The gap
between the aggregate 11.80 and the robust 1.78 is itself the heavy tail: the *typical* row is ~1.8×
over-dispersed, the *aggregate second moment* 11.8×.  Report both.)

### 7.5 This upgrades R1 from a heuristic to the correct estimator

§6's R1 specified a slow per-station factor fitted as a **median of cell maxima** — a heuristic,
chosen because nothing better was measurable.  The census removes that constraint.  The right
per-stratum scale is the **variance component with the redundancy in the denominator**:

$$\hat\sigma^2_{stratum} \;=\; \frac{\sum_{i\in stratum} w_i v_i^2}{\sum_{i\in stratum} r_i}$$

which is exactly what the constraint-aware global σ̂₀ rider derives globally
(`docs/SPEC_prior_residual_census.md`; formerly `SPEC_sigma0_constraint_aware.md`), now computable
**per station and per signal** because r_i is streamed.  Strictly better than the heuristic on three
counts: unbiased under the model rather than calibrated to a reference quantile; no `n_c` assumption
(the median-of-maxima reference constant disappears); and it degrades gracefully as the stratum's
redundancy changes, where a median of maxima silently conflates "quiet" with "unchecked".  ⚠ It is
also outlier-sensitive, so a production form needs a robust variant (trimmed or Huberized numerator)
— and the 11.80-vs-1.78 gap says that choice is not cosmetic.

### 7.6 The residual lives in the IONO DIRECTION — and ten candidates are dead

Decomposing the post-fit phase residual 3-vector (v_L1, v_L2, v_L5) at each (station, sat, epoch)
onto three physically meaningful directions — `(1,1,1)` (anything common in metres: clock,
troposphere, orbit, position, tides), the iono direction `(1, γ₂, γ₅)` with γ₂ = 1.6469 /
γ₅ = 1.7933, and the remaining orthogonal direction (reachable only by the per-signal
ambiguity/bias states):

| direction | share of residual variance | rms |
|---|---|---|
| common (clock / tropo / geometry) | 5.02 % | 7.7 mm |
| **iono** | **89.76 %** | **32.4 mm** |
| perp (bias/ambiguity only) | 5.22 % | 7.8 mm |

**This is the L1C answer.**  Once the common mode is removed the iono direction is
`(−0.804, +0.280, +0.525)` — **L1-dominant**, because L1 has the *smallest* iono coefficient (1 vs
1.65 / 1.79) and therefore sits furthest from the mean.  Per signal, L1C's 30.6 mm rms is 26.0 mm of
iono; L2W's 8.1 mm is 9.0 mm of iono (partly cancelling).  **L1C is not noisy — it is where
unabsorbed iono-direction error lands.**  The per-station heterogeneity is the *same* effect: the
loudest four stations are **93.2 %** iono share at 36.3 mm rms; the quietest four are **65.6 %** iono
at 8.2 mm.

**Structural corollary — the decomposition rules out an entire class by itself.**  Every
signal-independent error (satellite/receiver clock, troposphere, orbit, station position,
solid-earth tide, relativity) is identical *in metres* on all three signals and can only appear in
the common direction, which carries **5 %**.  So the culprit is necessarily frequency-dependent.
Phase wind-up (∝ λ) and the ionosphere (∝ 1/f²) both have the **wrong sign** — they load L2/L5 more
than L1.

### 7.7 Falsification ledger — do NOT re-run these

Every entry measured on this fixture, not argued.  ⚠ Two of them are *arms* with their own fixtures;
the rest are joins against truth or config.

| # | candidate | test | result |
|---|---|---|---|
| 1 | phase multipath | arm `runs/t3mp0` (`phase_amp_zen_m: 0`) | station spread 4.7→4.8×, flag rate UP |
| 2 | code multipath | arm `runs/t3cmp0` (`code_amp_gps_*: 0`) | iono share 89.76→89.43 %, all vf −8 % uniform |
| 3 | redundancy r_i | per-row census | r_i uniform 1.24× vs residual 2.6–3.0× |
| 4 | elevation (PCV) | join vs `truth_atmo` | L1C/L2W 14.9→8.2 over 10–90°, vs a 10–15× base |
| 5 | arc age (convergence) | first-sighting join | ratio flat 6–17 across all ages |
| 6 | iono **magnitude** | join vs truth `I_L1_m` | corr(log,log) **+0.089** |
| 7 | iono **rate of change** | epoch-difference join | corr(log,log) **+0.102** |
| 8 | receiver code biases | join vs `rx_code_bias` | corr(log,log \|C1C−C2W\|) **+0.264**; S05 has the LARGEST GF bias and is nearly the QUIETEST |
| 9 | phase-bias anchor / datum | station × signal grid | L1C/L2W > 3 at **22 of 23** stations — universal, not anchor-local |
| 10 | constellation | field, GPS vs Galileo | scale ratio **1.05×** |

⚠ **Method note, recorded because it cost a wrong claim.**  Candidate 8 was proposed from two
cherry-picked extremes (the loudest station had a large geometry-free receiver bias, the quietest a
small one).  Under correlation over all 23 stations it collapsed to +0.26 and the extremes actively
contradict it.  Propose from the correlation, not from the ends of the table.

**What survives.**  Two robust facts and no mechanism: (a) ~90 % of the post-fit phase residual is in
the iono direction, reproduced on two independent fixtures; (b) the per-station variance factor
spreads **29×**, is stable in time, and correlates with nothing observable yet tested.  The next
probe should grade the **estimated states against truth** (`truth_atmo` slant iono, `truth_bias`)
rather than continue inferring from residuals — that requires arming a per-LOS iono state trace,
which this posture does not emit.  ⚠ **This makes §6's R2 (declare, do not absorb) load-bearing
rather than stylistic**: a per-station variance component would bury an unidentified iono-direction
defect of 29× inside the noise model.

### 7.8 ✅ MECHANISM FOUND — it is per-LOS ionosphere estimation error (+0.926)

§7.7 named the next probe: stop inferring from residuals, grade the **states** against truth.  Done,
and it closes the question.

**The instrument.**  `network.trace_ionosphere` existed but was frame-broken exactly as CLAUDE.md
warns — it emitted the reconstructed correction `x_r` alone, while the per-LOS iono state is **seeded
at allocation**, so the column was a correction, not the absolute slant ionosphere.  Fixed on the ZWD
block's own pattern (`NetworkIonoDiag.cpp`): the `iono_m` column is unchanged (pyviz consumes it) and
two columns are appended — `x_lin_m` (the seed) and **`iono_total_m` = x_lin + x_r**, the absolute
slant ionosphere.  Scale of the defect: median `iono_m` **0.95 m** against median `iono_total_m`
**8.88 m** — anyone who graded the old column was grading ~11 % of the quantity.  Trace-class, so the
gate is bit-identity: with it armed, `network_solution.csv`, `network_dd_fractional.csv`,
`network_station_residuals.csv` and `network_epoch_status.csv` are **bit-identical** to
`runs/pareref_on/logs_v2`.  Arm `runs/t3iono`, config `_simnet24_inj_t3iono.json`.

**The grade.**  Joined to `truth_atmo.csv`'s `I_L1_m` over 13 054 settled (station, sat, epoch)
triples.  The state is LUMPED (the config log: *"the receiver code delay rides inside the slant
ionosphere"*), so per standing invariant #5 the error is decomposed `e = mu + a(sta) + b(sat) + r`
and only the **interaction r** — what no receiver or satellite bias can absorb — is datum-invariant:

| term | value |
|---|---|
| raw error (lumped, NOT gradable) | mean 1.407 m, rms 3.564 m |
| overall mu | 0.328 m |
| station lump a(sta) — the receiver GF term | −3.137 … +4.156 m |
| satellite lump b(sat) | −3.116 … +4.736 m |
| **interaction r — the genuine per-LOS iono error** | **rms 0.764 m**, median\|r\| 0.142, max 9.27 |

**THE RESULT:**

$$\operatorname{corr}\bigl(\log \hat\sigma^2_{station},\; \log \mathrm{rms}\,r_{station}\bigr) \;=\; \mathbf{+0.9258}$$

The per-station iono interaction rms runs **0.106 m (S13) … 2.551 m (S06)** and orders almost exactly
with the §7.4 variance factor (S06 61.80 / S16 2.14).  The contrast is decisive: the **lumped** term
correlates **−0.103** — so it is *not* the receiver code-bias partition, and not any bias the datum
can move.  It is the per-line-of-sight ionosphere estimate being wrong, by up to 2.5 m rms at the
worst station.

### 7.8.1 The causal chain, and one correction to it

| step | measured |
|---|---|
| per-LOS slant iono state error (datum-free interaction) | rms **0.817 m** |
| absorbed by the collinear per-arc **ambiguity** | ~96 % |
| unabsorbed remainder → phase residual, iono direction | rms **0.033 m** |
| corr(iono error **level**, residual iono coefficient) | **+0.532** |
| corr(iono error **change**, residual iono coefficient) | **+0.046** |

⚠ **The residual tracks the iono error's LEVEL, not its rate of change.**  The natural guess — that
the ambiguity soaks up the constant part so only the time-variation shows — is **wrong**: the change
correlation is null.  The ambiguity absorbs ~96 % of the level and the unabsorbed ~4 % passes through
proportionally.

That closes every open thread in §7.7: the effect is universal (every station has iono error), stable
in time (the error level is persistent per arc), independent of elevation and arc age, invisible to
`r_i` (the *geometry* is uniform; the *iono fit* is not), unmoved by multipath, and L1-dominant
because the iono direction is L1-dominant once the common mode is removed (§7.6).

### 7.8.2 Two consequences beyond this card

1. **R2 is now non-negotiable.**  A per-station variance component would bury a measured
   **0.11–2.55 m per-station ionosphere estimation error** inside the noise model.  That is no longer
   a stylistic worry — it names the quantity that would be hidden.
2. **This is very likely the same root as the integer-resolution failure.**  The ambiguities are
   absorbing ~96 % of a 0.8 m iono error; an ambiguity biased by that cannot be fixed to an integer.
   The project's standing finding is `project_why_non_integer_rootcause` — *"NL 99.3 % iono"* — and
   the AGU-26 amendment records AR fix rate 0 % with med\|frac_cyc\| uniform at 0.25.  §7.8 supplies
   the truth-graded number behind both.  ⚠ Asserted as a connection, not measured here — see §7.9,
   which then measured it.

### 7.9 ✅ The DD-integer tie — MEASURED, and the sensitivity ladder holds

**The prediction.**  For phase on signal j, `φ_j = ρ + T − γ_j·I_L1 + λ_j·N_j`.  An iono error
`d = Î − I_true` must be absorbed by the ambiguity:

$$\lambda_j\,(\hat N_j - N_j^{true}) = \gamma_j\, d \quad\Longrightarrow\quad e_j = \frac{\gamma_j}{\lambda_j}\, d$$

In DD form the station and satellite main effects cancel exactly, so the driver is the double
difference of the §7.8 interaction term.  The resulting per-band sensitivities — **L1C 5.255,
L2W 6.744, L5Q 7.037 cyc/m** — reproduce the ladder `scripts/simnet_amb_integer.py` already documents
(*"L1 5.26 cyc, WL 1.49, EWL 0.29"*), and the lane values fall out as **differences**:
WL(1,2) = 5.255 − 6.744 = **−1.489**, EWL(2,5) = 6.744 − 7.037 = **−0.293**.  ⚠ The documented
figures are magnitudes; the **sign is negative** for both lanes.

**The measurement.**  DD integer error `e = dd_cyc − DD_true` from `network_dd_fractional.csv` joined
to `truth_ambiguities.csv`, regressed on the DD of the truth-graded iono interaction (7326 matched
quadruples):

| combo | predicted slope (cyc/m) | fitted slope | corr | rms e (cyc) | variance explained, **zero free parameters** |
|---|---|---|---|---|---|
| L1C | 5.255 | 9.731 | **+0.9266** | 11.847 | **67.9 %** |
| L2W | 6.744 | 10.149 | **+0.9543** | 12.003 | **81.0 %** |
| L5Q | 7.037 | 10.288 | **+0.9585** | 12.116 | **82.8 %** |
| WL(1,2) | −1.489 | −0.418 | −0.4632 | **1.005** | — |
| EWL(2,5) | −0.293 | −0.140 | −0.7239 | **0.217** | — |

**The per-band DD ambiguity error is 68–83 % accounted for by the truth-graded per-LOS ionosphere
error, at correlation 0.93–0.96, using the physics slope with nothing fitted.**  That is the tie
§7.8.2 owed.  And the ladder explains which rung survives:

| combo | sensitivity, relative to L1C | measured rms e, relative to L1C | `round(e)=0` (grader) |
|---|---|---|---|
| L1C | 1.000 | 1.000 | 5.1 % |
| WL(1,2) | 0.283 | 0.085 | 51.0 % |
| EWL(2,5) | 0.056 | 0.018 | **95.9 %** |

EWL is the only rung that grades near-integer, and it is the rung with **18× less iono sensitivity**.
The pre-registered handoff shape ("EWL near-integer, WL plausible, NL not") is confirmed, and the
reason is now measured rather than inferred.

⚠ **Two honest gaps.**  (1) The *fitted* per-band slope is ~1.85× the predicted one (9.7 vs 5.26 on
L1C).  The DD annihilates the station and satellite lumps exactly, so that is not a datum artifact;
the ambiguity is absorbing more than the pure-iono prediction and the excess is **unexplained** —
most likely a third error the iono and ambiguity jointly absorb.  (2) The lane rows show the mirror:
the measured error ladder (1 : 0.085 : 0.018) is ~3× *steeper* than the sensitivity ladder
(1 : 0.283 : 0.056), so the lanes cancel more than iono alone, and applying the full predicted slope
over-corrects (hence the negative "explained" for the lanes, omitted above rather than dressed up).

