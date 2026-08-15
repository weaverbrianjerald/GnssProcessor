# REPORT — FC-1 split-cadence fast clock: BUILT (INC-0..4), gate FAILED, card is a NO-GO

*2026-07-30. Spec: `docs/SPEC_fast_clock_split_cadence.md`. Code: `filter/NetworkFastClock.{hpp,cpp}`,
`network.fast_clock.*`, default OFF. Grader: `scripts/fc1_grade.py`.*

---

## 0. Verdict

**The card is built end-to-end and its decisive gate FAILS by a factor of five.  The failure
is structural, not a tuning miss, and the same measurement explains why.**

| INC-4 (50 sta, GPS+Gal, 3 h, REF `n=1` vs SPLIT `n=2`) | measured |
|---|---|
| SD-RMS(fast vs REF) | **327.7 cm** |
| SD-RMS(ZOH vs REF) — what a consumer gets today | **326.4 cm** |
| improvement factor | **1.00×**  (gate requires ≥ 5×) |

> **Post-report audit (2026-07-30, same day): the row above is saturated and the emit
> path had a frame bug — both fixed, verdict unchanged.**  The two arms' full solves
> realize independent frame random walks ~318 cm SD-RMS apart, so both errors above
> floor at that divergence (√(318² + 68.5²) ≈ 325) and the gate could never have shown
> more than ~1.03× (§5.7 — grader now aligns the arm frames and prints the removed
> divergence).  De-saturated, the original build graded **0.97×** — measurably *worse*
> than ZOH — because the emitted K double-counted broadcast af1 drift + relativistic
> rate (§5.6 — fixed; post-fix re-run grades **1.00×**: fast 68.494 vs ZOH 68.494 cm).
> The NO-GO and the Q3 prerequisite stand exactly as written below; the fast tier's
> ~0.5 cm of true cross-satellite signal per 30 s tick is simply invisible against the
> 68.5 cm churn step, which is §2's finding restated on the wire.

The fast tier is not inaccurate.  It is measuring the right quantity to a formal σ of
3.8 mm with σ̂₀ ≈ 0.9.  What defeats it is that **the reference it must reproduce is
~98 % producer frame churn**, and churn is precisely what a physically-derived measurement
does not contain.

This is the C1 clock-rate verdict recurring on a different mechanism.  C1's conclusion —
*"C1 is decoupled by construction, so it cannot reduce producer wander — and the wander is
what defeats it.  C1's prerequisite is Q3's guarded temporal anchor, not the reverse"* —
transfers to FC-1 verbatim.  **FC-1's prerequisite is the same guarded temporal anchor.**

**Do not delete the card.** It is correct, cheap, inert when off, default-OFF byte-identical,
and it becomes valuable the moment the producer clock acquires temporal stiffness. In the
meantime its shadow mode is a permanent, calibrated instrument for measuring exactly that
stiffness — see §4.

---

## 1. What was built

All five increments, each gated before the next.

| INC | Content | Gate | Result |
|---|---|---|---|
| 0 | config block, `FastClockCarry`, seam, 14 counters, `network_fast_clock.csv`, `SB_FASTCLOCK`, driver hooks | default-OFF byte-identity | **PASS** |
| 1 | anchor harvest at emit-success, arming streak, lump + lump-rate cache | byte-identity + unit tests | **PASS** |
| 2 | tick estimation: ρ recompute, IF delta, median-polish seed, Schur WLS, screen, floors | σ̂₀, screen rate, shadow grade | **PASS** (mechanics) |
| 3 | `F`/`K` wire records, `pushFastClock`, replay parity | round-trip + consumer-contract tests | **PASS** |
| 4 | cadence-split A/B | fast ≤ ZOH/5 | **FAIL — 1.00×** |

Isolation contract held at every increment: with the tier armed in shadow (both hooks firing
on every epoch), `products.txt` and all 23 estimation CSVs are byte-identical to the tier-off
arm.  The only differences are the wall-clock `G` generation stamp and the two timing CSVs.

Runtime health of the tick itself (SPLIT arm, 3 h, 180 ticks):

- 179 / 180 ticks estimated and emitted, 0 datum-withheld, 0 gap-withheld
- 131 920 LOS used / 1 095 screened = **0.82 %** (spec bar: < 1 % on quiet data)
- σ̂₀ **0.85–0.98**, per-satellite formal σ **3.8 mm**
- tick wall **14.2 ms mean, 38.2 ms max** at 50 stations — inside the §7.3 50 ms budget.
  ⚠ Extrapolating by station count, 305 stations lands at ~85 ms mean / ~230 ms max, i.e.
  **over budget**; the per-LOS `getSatellitePV` call is the cost and the documented fix
  (evaluate PV once per satellite and extrapolate to each station's emission time by
  velocity, ~0.1 mm error) was not needed for the verdict and was not built.

---

## 2. The measurement that decides it

Shadow mode computes the fast tick on FULL epochs and discards it, so every epoch yields
both a phase-derived clock change and the producer's own.  50 stations, GPS+Galileo, 3 h,
common mode removed per epoch (invariant #5).

### 2.1 The producer's clock change is not the satellite's clock change

| hour | producer Δc0 | phase-measured Δc | correlation |
|---|---|---|---|
| 0 | 79.4 cm | 1.74 cm | −0.009 |
| 1 | 67.8 cm | 1.73 cm | −0.001 |
| 2 | 83.1 cm | 1.93 cm | +0.018 |

Flat across three hours ⇒ **not a convergence transient**.  Correlation is zero at every
hour ⇒ the two quantities are unrelated, not merely different in scale.

Cold-start amplification was ruled out (the ratio does not improve with time), discontinuity
bookkeeping was ruled out (5 disc advances in 530 consecutive pairs), and detrending was
ruled out (comparing against the producer's *second difference*, which matches the fast
observable's own de-trending, leaves the disagreement unchanged).

### 2.2 The ceiling on any fast tier, at every horizon

| ZOH horizon | ZOH error | √Δt law | physical change | **headroom** |
|---|---|---|---|---|
| 30 s | 76.9 cm | 76.9 cm | 1.80 cm | 2.3 % |
| 60 s | 112.1 cm | 108.8 cm | 2.06 cm | 1.8 % |
| 120 s | 161.6 cm | 153.8 cm | 1.95 cm | 1.2 % |
| 300 s | 243.4 cm | 243.2 cm | 2.05 cm | 0.8 % |
| 600 s | 317.0 cm | 343.9 cm | 2.28 cm | 0.7 % |
| 1200 s | 431.2 cm | 486.4 cm | 2.81 cm | 0.7 % |

Two things are visible at once:

1. **The producer's emitted clock follows the √Δt random-walk law almost exactly** — an
   independent confirmation of C1's measured `dt^0.581` signature, on different data.
2. **The true clock change is FLAT at ~2 cm and does not accumulate.** Over twenty minutes
   the physical satellite clock (beyond the a-priori) moves 2.8 cm while the emitted product
   random-walks 4.3 m.

Therefore the *maximum* error a perfect fast tier could remove is **2.3 % at 30 s, falling
to 0.7 % at 20 minutes**.  The gate asks for 80 %.  No cadence, no threshold and no
estimator improvement reaches it, because the missing 97.7 % is not clock.

---

## 3. Consequences beyond this card

- **The producer's clock random walk is its own error budget, now measured against a
  physical reference for the first time.** C1 established the walk existed and inferred it
  was frame churn from its magnitude relative to Allan deviation.  This measures the
  physical clock directly and finds the walk is ~98 % non-physical.
- **Any consumer-side clock coasting scheme inherits the same ceiling** — high-rate clock
  interpolation, SSR saw-tooth correction, C1 rate extrapolation.  They are all attempts to
  predict a walk that is producer-internal.
- **The lever that pays is Q3 / producer temporal stiffness**, not faster measurement.
  Reducing the 0.77 m per-30 s churn is worth up to 100× more than measuring the 1.8 cm the
  fast tier recovers.

---

## 4. What to keep

`network.fast_clock.shadow` should stay as a standing instrument, in the same
"instrument, never feedback" class as the M2 LS-VCE carry-variance monitor.  It is the only
way the project can currently answer *"how much of the emitted clock is real?"* — it costs
14 ms/epoch at 50 stations, is byte-identical to off, and its σ̂₀ ≈ 0.9 shows it is
correctly calibrated.  Re-run it after any Q3 / temporal-anchor work: the ratio in §2.1 is
the direct score for that work, and INC-4 becomes passable exactly when the ratio closes.

---

## 5. Spec defects found while building

1. **§2.1 vs §8 contradict each other on the emitted quantity.** `Ṁ` absorbs the clock's
   linear drift, so `δc` is the change *beyond* the trend (§2.1 says so).  §8 nevertheless
   emits `c0(t₀) + δc`, which drops the drift — the dominant part of true motion over
   30–300 s — and would publish something worse than ZOH.  Fixed by recovering `ċ^s` from
   the same lump rates via the same bipartite separation (`Ṁ = τ̇_r − ċ`, so the satellite
   column effect *is* `−ċ` once the datum row is pinned) and emitting
   `c0(t₀) + ċ·Δt + δc`.
2. **§8's "a pre-FC-1 consumer must fail loudly" is not achievable.** The banner is a `#`
   comment and no C++ reader has ever inspected it; a version bump is invisible to every
   consumer already built.  Both the marker and the payload therefore use private tags
   (`F`/`K`), so an old reader drops the whole record and degrades to exactly its pre-card
   behaviour instead of re-attaching a familiar `C` line to the previous full epoch.  A test
   asserts a fast-stripped file reads identically to a pre-card one.
3. **§2.1's fitted-phase harvest is unbuildable as written.** It needs the post-fit residual,
   but §5 forbids a new `reconstructStation` sweep and all three existing sweeps run *after*
   the emit site behind trace/RAIM knobs.  Defining the lump on OBSERVED phase makes the
   observable exact TDCP, where the ambiguity and both hardware biases cancel *identically*
   on a live arc; the cost is one epoch of phase noise as a per-LOS constant (~2 mm, ~0.6 mm
   on δc after averaging).
4. **§2.2's screen scale is the wrong quantity** — it sizes the per-LOS screen with
   `clockOscPredict`'s q2 spread, but the satellite clock motion that describes is already
   removed as the median-polish column effect, and the standing project directive is that
   thresholds come from the observable stream, never the stochastic model.  Implemented as
   the robust spread of the interactions themselves, floored by the formal σ.
5. **§5/§11 point at the wrong file for `medianPolish2Way`** — it is not in
   `NetworkClockQc` (which uses plain per-constellation `nth_element` medians).  It is
   `include/filter/NetworkRobustStats.hpp:66`; the precedents are `NetworkRaim.cpp:258` and
   `NetworkSolveGlue.cpp:930`.

6. **Implementation defect (post-report audit, 2026-07-30; FIXED same day): the emitted
   `ċ·Δt` term was in the wrong frame.**  The lump was raw `Φ·λ − ρ` with no a-priori
   satellite clock subtracted (`fastClockGeometricRange` evaluated the clock for light
   time and discarded it), so the median-polished `ċ` was the FULL physical clock rate —
   per-sat correlation **+0.93 with broadcast `c·af1`** (G22 agreed to 0.02 mm/s) and only
   +0.07 with the emitted c0's own slope, plus the periodic relativistic rate on eccentric
   satellites (E14/E18, ±10–15 mm/s).  The consumer's a-priori evaluation already advances
   both, so every K double-counted them: **15.3 cm RMS spurious drift per 30 s tick, up to
   ~62 cm on E19** (af1 = 20.7 mm/s), vs the ~1.8 cm physical signal — and indeed the
   de-saturated grade of the original runs shows fast = √(ZOH² + 15.3²): 70.290 measured
   vs 70.2 predicted, i.e. the tier graded WORSE than ZOH (0.97×) with E18/E19 in the
   worst-satellite list.  Fix: the lump (and the tick's phase) is now built on
   model-consistent corrected phase `Φ̃ = Φ + c·δt_ap` — the same polynomial + periodic
   relativistic a-priori the full model applies, gated by the same knob — so `ċ` is the
   RESIDUAL clock rate in the emitted frame (`NetworkFastClock.cpp` §`fc1_apriori`).
   Post-fix verification (same-build 3 h re-runs, `*_v2` configs): corr(ċ, c·af1)
   **+0.93 → +0.12**, embedded drift **15.3 → 1.48 cm** per 30 s tick, E19's ċ −0.57 mm/s
   vs af1's +20.65; K − anchor now carries 0.545 cm of cross-satellite structure (the
   physical signal) under a 9.5 cm per-epoch common mode (datum receiver, cancels in SD
   grading).  The shadow instrument was and is unaffected — §2.1/§2.2 reproduce to
   0.01 cm (76.981 → 76.971 SD-RMS, formal σ 0.383 cm unchanged) — and the OFF/shadow
   byte-identity gate re-passed.  One prediction from the audit did NOT materialize:
   `ċ` does not now correlate with the producer's own 3 h c0 slope (−0.04), and cannot —
   that slope's RMS (1.386 mm/s) is ~97 % the random walk's realized endpoint drift
   (0.77 m·√360 / 10800 s ≈ 1.35 mm/s), i.e. the §2.1 churn finding restated at slope
   scale.

7. **Grader defect (post-report audit, 2026-07-30; FIXED same day): the INC-4 metric was
   saturated by construction.**  The SPLIT and REF arms' full solves differ by ~318 cm
   SD-RMS at common epochs (independent producer frame walks), so fast-vs-REF and
   ZOH-vs-REF both floored at the inter-arm divergence — √(318² + 68.5²) ≈ 325 ≈ the
   326–328 cm §0 measured — and the maximum measurable improvement factor was ~1.03×
   against a 5× gate, at ANY churn level.  `grade_split` now estimates the per-satellite
   arm-frame offset at the bracketing common full epoch (SPLIT full − REF full, held
   across the era), subtracts it from both errors — algebraically turning the ZOH error
   into REF's own step across the era and the fast error into the tick's miss of that
   step — and PRINTS the removed divergence (`arm-frame divergence: SD-RMS 317.989 cm
   REMOVED`), so saturation can never hide again.  Any future INC-4 re-run (post-Q3)
   inherits the honest metric automatically.

## 6. Reproduce

```bash
python scripts/fc1_grade.py shadow --products runs/fc1_shadow_3h/products.txt \
    --trace runs/fc1_shadow_3h/logs/network_fast_clock.csv
python scripts/fc1_grade.py split --products runs/fc1_split_n2_3h/products.txt \
    --ref-products runs/fc1_shadow_3h/products.txt \
    --trace runs/fc1_split_n2_3h/logs/network_fast_clock.csv
```

Configs: `_fc1_byteid_off.json` / `_fc1_byteid_shadow.json` (isolation gate),
`_fc1_shadow_3h.json` (REF + shadow grade), `_fc1_split_n2_3h.json` (SPLIT arm).

The stored `runs/fc1_*` directories are the ORIGINAL (pre-§5.6-fix) build's output and
are kept as provenance; `grade_split` on them now reports the de-saturated 0.97×.  The
post-fix arms live in `runs/fc1_*_v2` with configs `_fc1_byteid_off_v2.json` /
`_fc1_byteid_shadow_v2.json` / `_fc1_shadow_3h_v2.json` / `_fc1_split_n2_3h_v2.json`
(same posture, fresh output dirs — append-mode service CSVs make reusing a run dir
unsafe); grading those reports the honest post-fix 1.00×.
