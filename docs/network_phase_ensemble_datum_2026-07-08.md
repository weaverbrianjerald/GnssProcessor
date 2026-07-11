# Fix A — anchor-free ensemble phase-bias datum (2026-07-08)

**Problem (Odijk 2016 S-system diagnosis).** The emitted satellite phase bias
`δ̃ˢ_σ = [phys] − z^s_{anchor,σ}` (Odijk Table 3) is time-constant only while its
datum reference — the anchor receiver's integer ambiguity — is fixed. The
producer realizes that gauge with a **per-(sat,signal) prioritized anchor
receiver, re-elected per epoch**. Two consequences: (b) anchor handovers step the
emitted bias by a between-receiver integer → consumer resets → phase never
accumulates; and (b-static) **different receivers anchor different satellites**,
so the user's between-satellite fractional-bias term
`δ_{anchor(s')} − δ_{anchor(s)}` does NOT cancel → the user DD ambiguity is
non-integer → 0 fixes even after the float converges.

**Cure.** Replace the single-anchor pin with an **anchor-free sum-to-zero over the
receiver ambiguities** for each (sat,signal): `Σ_r z^s_{r,σ} = 0`. No pivot, no
handover, no SPOF; the phase bias references the same receiver ENSEMBLE for every
satellite, so the user's between-satellite fractional term cancels consistently.
The direct analogue of the clock `clock_datum_sum_to_zero`, but for the
phase-bias↔ambiguity gauge.

## The architectural catch and its resolution

The clock sum-to-zero sums **shared** columns (satellite clocks) → a one-line
`addSharedConstraintRows`. The phase-bias gauge couples the **shared** satellite
phase bias to the **per-station LOCAL** ambiguities. An exact `Σ_r z_r = 0` is a
**cross-station** constraint the arrowhead kernel (independent per-station Schur
elimination) cannot fold as a shared row.

**Resolution — Schur-project the soft constraint (weight w→∞).** The constraint
`w·(Σ_r z_r)²` = `w·(cᵀx)²` with `c` supported on the (s,σ) local columns across
stations. Woodbury through the block-diagonal local normals gives a **rank-1
correction on the reduced shared system**, computed entirely from the cached
per-station LDLᵀ factors:

```
  n_r  = N_rr⁻¹ e_{z_r}                 (one local solve per (station, gauge))
  β    = Σ_r (n_r)_{z_r}   = Σ_r (N_rr⁻¹)_{z_r z_r}          (scalar > 0)
  g    = Σ_r N_sr n_r                                        (n_shared vector)
  γ    = Σ_r (N_rr⁻¹ u_r)_{z_r}                              (scalar)

  reduced_N_ss += g gᵀ / β          reduced_u_s += g γ / β        (w→∞ limit)
```

**Constrained reconstruction** (so the carried/QC ambiguities live in the SAME
sum-to-zero gauge as the emitted bias — no gauge fight):

```
  S_unc = γ − gᵀ x_s                (total unconstrained ambiguity sum, scalar)
  x_r  -= n_r · (S_unc / β)         per station r in the gauge
```

`x_r`/`x_s` here are the standard (unconstrained-reconstruction) values; the
correction touches only the gauge (common-shift) direction, leaving every
estimable (between-receiver, between-satellite) quantity identical to the anchor
datum → it is a pure **S-transform** (unit-test-proven).

## Kernel API (NetworkNormalEquations)

- `struct SumZeroGauge { vector<pair<station, local_col>> terms; }`
- `void foldReducedSumZeroGauges(const vector<SumZeroGauge>&)` — call AFTER
  `reduce()`, BEFORE the null-space gate reads `reducedSharedNormals()` (so the
  gate sees a phase-full-rank system: declared_dim stays = #clock deficiencies,
  exactly as the anchor mode where the local pins were pre-eliminated). Folds the
  corrections and stores `(terms, g, β, γ)` per gauge + a per-station index.
- `reconstructStation` applies the `−n_r·S_unc/β` correction for every stored
  gauge the station participates in (byte-identical no-op when none are folded).
- `reduce()` recompute and `reset()` clear the stored gauges (a rebuilt
  `reduced_N_ss_` no longer carries the fold; the driver re-folds after any
  re-reduce, mirroring the code-bias/clock-constraint re-apply in the within-epoch
  RAIM re-solve).

## Driver wiring (NetworkSbasis + ApplicationNetwork), behind a toggle

`network.phase_datum_ensemble` (default OFF = the prioritized-anchor pin,
byte-identical). ON: skip the anchor election + the 1e10 local pins; after the
kernel reduce, build one `SumZeroGauge` per (sat,signal) over the tracking
stations' ambiguity columns and call `foldReducedSumZeroGauges`; declare the
phase gauge in the S-contract (`AmbDatum::SumToZeroPerReceiver`). Composes with any
clock datum (orthogonal). Requires the phase-leveling carry to be in the same
gauge (the constrained reconstruction guarantees it).

## Validation plan

1. Unit test: sum-to-zero recovers `b_s + mean_r(M_{r,s})` for the phase bias and
   `M_{r,s} − mean_r(M_{r,s})` for the ambiguities (Σ_r = 0), and the between-station
   DD ambiguities MATCH the anchor datum (pure S-transform).
2. Byte-identical OFF (hash-proven) on an existing network run.
3. 305-sta A/B vs the prioritized-anchor baseline: PB disc churn ↓, phase
   accumulation (float convergence) ↑, and — the decisive AR test — whether the
   consumer starts fixing (the AR-poison is removed).

## Deferred (follow-ups)

- Covariance corrections (`reconstructStationCovariance*`, `rowRedundancy`) under
  the gauge — used by network-AR + carry-variance seeding + RAIM redundancy. The
  point solve (product + carry values) lands first; the gauge-direction variance
  reduction is a conservative-if-omitted second pass.

## 2026-07-08 — BLOCKER DIAGNOSED + RESOLVED (`phase_datum_hold_sigma_m`)

The un-held ensemble runs away on real data (producer clocks ~7×/epoch → 5e8 m,
wedge). Root cause (5-agent audit, point-solve re-derived exact): the anchor-free
`Σ_r z^s_r = 0` datum references the emitted bias to `mean_r(ẑ^s_r)` — a
**per-epoch-re-realized functional of the FLOAT ambiguities**, whose absolute LEVEL
is code/iono-precision-limited, not phase. So the emitted absolute bias **churns
~1 m/epoch** (the between-satellite structure is stable; the level is not — a
*dynamic* datum applied to a *static* product). It is present from epoch 1 (the
per-sat level clusters at tens of km — the exact ensemble value at that sat's
conditioning, NOT a bug: the point solve is a proven per-epoch S-transform and the
clocks are byte-exact to the anchor). The co-estimated SH-VTEC (carried) absorbs
the churn and amplifies it ~3×/epoch into the exponential runaway. The
phase-leveling carry cannot cure it — an individual float ambiguity is intrinsically
under-determined, so its harvested conditional variance is code-scale and the carry
is inert (PL-on ≡ PL-off, observed). No covariance fix removes the churn.

Two supporting covariance-consistency fixes landed first (both treat each gauge as
an EXACT datum column, the true analogue of `conditionalLocalCovariance`'s datum-pin
zeroing; **not** the handoff's "project out `g`" — `g = λ·N_ss·e_pb` has clock
components, so projecting it would corrupt the correct clock covariance): (i)
`NetworkEstimator::zeroPhaseGaugeColumns` zeros the gauge columns of the D2-carried
`shared_cov_` (removed the fast epoch-2 runaway), and (ii) the same in
`conditionalLocalCovariance`/`Diag`/`rowRedundancy` (via `SumZeroGauge::shared_col`).
These are necessary but not sufficient — they leave the ~1 m/epoch churn.

**The cure — seed once, then time-lock (`network.phase_datum_hold_sigma_m`, default
0 = churny/off):** stop re-realizing the datum every epoch. SEED `Σz=0` once when a
`(sat,signal)` first appears, then HOLD the shared bias with a stiff temporal prior
of this sigma (a clean fixed variance — deliberately NOT the fold-contaminated
`shared_cov_` column, which is the epoch-2 fast runaway). `buildEnsembleGauges` skips
the fold for a `(sat,signal)` that already carries a held bias (a competing hard
`w→∞` fold would override a finite prior); the D2 stash carries the bias with the
fixed hold-variance; a dedicated injection re-applies the held prior EVERY epoch,
independent of the clock-pivot `datum_changed` gate (the phase datum is orthogonal
to the clock datum). A `(sat,signal)` that vanishes and returns re-seeds
(auto — `prev_carry` is per-epoch). Files: `GnssConfig.hpp`/`GnssConfigLoad.cpp`
(toggle), `ApplicationNetwork.cpp` (hold locals, `buildEnsembleGauges` skip, D2 stash
hold carry, dedicated always-on injection).

**Validated (3-station reproducer, sigma=0.01 m):** clock rms stable ~1.2 the whole
window (was 1.2→373→…→5e8), **0 runaways / 0 σ̂0-withholds** (was 65), emitted phase
bias **held to <2 mm/epoch, disc=0** (was churning 1 m→tens of km→explosion), PB disc
churn 216 vs the un-held 5423, consumer comparable to the anchor (U median +0.26 vs
+0.87 m). Byte-identity OFF hash-proven; suite 590/1.

### Remaining follow-ups (post-hold)
- **Seed quality — NOW THE DOMINANT BLOCKER (305 A/B confirmed).** The epoch-1 float
  seed gives a poor absolute level (tens of km) held for the whole arc. On the full
  305 window the consumer settles to a persistent **+1.7 m U bias** from that seed and
  the epoch-2 consumer error spikes to ~12 m as it settles — so ensemble+hold ends up
  only COMPARABLE to the anchor on the consumer (e3d 2.81 vs 2.74 m) despite slashing
  PB churn (1716 vs 7138, −76%) and curing the runaway. (The first 305 run looked
  better — e3d 2.05 — only because 4 observability-gate withholds accidentally hid the
  poor-seed epochs; the pre-gate review-fix emits the full window and exposes them.)
  **Seed after the arc's ambiguities converge (a warmup gate) for a clean,
  between-satellite-consistent reference — this is the likely key to the AR-poison
  cure (0 fixes today), not optional.**
- **Covariance `diag(β)`→full `B⁻¹`:** the multi-gauge fold folds each gauge
  independently (`Σ_k g_k g_kᵀ/β_k`); the exact w→∞ Woodbury needs the full
  inter-gauge `B = C_lᵀ N_ll⁻¹ C_l` (non-diagonal via the white-noise rx clock). The
  POINT solve is unaffected (proven); only `shared_cov_` is distorted. This was the
  *crash* amplifier — **now moot** with the hold (no churn to amplify) but still a
  real covariance-only defect worth a second pass for network-AR seeding.
- **Slip re-seed:** on a confirmed in-arc integer slip, drop the held bias and
  re-seed (currently only vanish-return re-seeds).
