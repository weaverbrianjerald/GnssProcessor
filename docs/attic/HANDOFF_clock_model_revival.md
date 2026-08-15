# HANDOFF — Two-state clock model revival: Hadamard recalibration + 30 s arming A/B

> ## ★ PHASE A + A.5 COMPLETE (2026-07-14, Opus) — clock_model is a NO-GO; the stability asset is the C1 RATE product instead
>
> Instrument: `scripts/clock_hadamard_calib.py` (self-test PASS: recovers a known
> q2 to ratio 0.996, drift-immune; `--clk` mode ingests a RINEX CLK product).
> Deliverable: `docs/CLOCK_HADAMARD_CALIB_2026-07-14.md`. Inputs = the three named
> 30 s runs + the CODE MGEX 30 s precise clock `data/2024-129/products/
> COD0MGXFIN_20241290000_01D_30S_CLK.CLK`.
>
> **Finding 1 — RWFM is absent everywhere (premise false).** Hadamard slope p≈1
> (white-FM) for BOTH the emitted network clock (GPS 1.02 / Gal 1.05) AND the
> intrinsic CODE precise clock (GPS 1.50 / Gal 0.88 / BDS 0.76). Intrinsic q2
> upper bounds (GPS ≤ 5.8e-12, Gal ≤ 1.7e-13) are 4–6 orders BELOW the documented
> 1.5e-7. Expected clock physics: WFM/flicker dominates 30 s–30 min; RWFM only
> emerges at hour+ averaging. The "q2·dt³/3 = 3.7 cm" tightness mechanism does not
> exist. (The 1.5e-7 measured the orbit/datum-contaminated network *residual*.)
>
> **Finding 2 — the real asset is the DETERMINISTIC RATE, and the residual carry
> cannot see it.** Measured directly as RMS(Δx)/RMS(Δ²x): a two-state drift state
> beats the scalar carry by **16–17× on the intrinsic clock (Galileo/BeiDou), 1.7×
> GPS** — the satellite's predictable frequency. But on the **network residual**
> (what `clock_model` actually carries) the advantage is **~1.0× (Gal 0.9× / GPS
> 1.1×) — nothing**: the deterministic rate already cancels against the precise
> apriori, and the residual is ~0.2 m/epoch estimation+datum noise. (Intrinsic
> small-τ has a phase-interp caveat ⇒ multiplier order-of-magnitude, ranking
> Gal≫GPS≫GLONASS real.)
>
> **Verdict: `clock_model` NO-GO** (no RWFM; ~1× residual-carry gain, moot on a
> dense net; and arming at q2=1.5e-7 would NIS-storm). **PIVOT: the stability the
> user wants IS real and large but lives in the clock RATE — exploit it via the
> C1 clock-rate product** (`project_network_clock_rate_c1`; producer hardcodes
> c1=0 today). Emit the drift so low-latency / RT consumers extrapolate instead of
> ZOH (saw-tooth fix); benefit grows as the consumer's apriori clock weakens
> (broadcast / RT). Largest for Galileo. §2–§3 below are SUPERSEDED — do not arm.

*Written 2026-07-14 (Fable). Implementer: Opus. Phase A is analysis-only (new
standalone Python script, no C++); Phase B is config-arming + a small optional
config addition. Sequence AFTER the parallel-sweeps card merges (Phase B logs
land in `ApplicationNetwork.cpp` territory).*

## 1. Context and the user mandate

The user wants satellite-clock stability exploited to strengthen the network.
The mechanism exists and is dormant: `network.clock_model` — a per-satellite
Zucca–Tavella two-state filter x=[offset (m), drift (m/s)] on the estimable
clock (anchor `clockOscPredict`, `ApplicationNetwork.cpp`; struct `ClockOsc`),
Q(dt) = [[q₁dt+q₂dt³/3, q₂dt²/2],[q₂dt²/2, q₂dt]], measurement = the epoch's
network-solved offset (R = `clock_model_meas_std_m`² = (0.05 m)²), per-sat
true-elapsed-time gaps (eclipse-safe), stale reseed at `clock_model_max_gap_s`
(T5 event), NIS fault flag at `clock_model_k_sigma`=6 with per-constellation
datum common-mode removed. When armed, the shared clock column's carry prior
becomes the two-state prediction (info = 1/p₀₀) instead of the scalar
`var + sat_clock_q·dt`.

Why it's worth reviving: at 30 s the prediction σ is q₂dt³/3 ≈ **3.7 cm/epoch
vs ~55 cm for the scalar carry — ~15× more clock information**, plus lock-ins
become detectable NIS faults instead of silent wander. Note the dt³: at 300 s
the prediction σ is ~1.16 m — the leverage is a HIGH-RATE asset. **Never
evaluate this card at the 300 s dev tier.**

History to respect:
- The defaults are already empirical, not datasheet: q₂ = 1.5e-7 m²/s³
  (GPS ≈ 1.1e-7 / Galileo ≈ 1.8e-7, from Allan/Hadamard inversion of the
  network Δδtˢ residual — see the `networkClockModelQ2()` Doxygen).
- The one negative field data point (Jul-3, closed-loop consumer campaign):
  `clock_model_q2 = 1e-8` tested WORSE — a hand override ~15× stiffer than the
  measured value (datasheet-thinking applied to an estimable that contains
  BRDC clock-polynomial error + radial orbit error). It falsified the wrong q,
  not the model.
- Everything datum-side has changed since calibration (sum-to-zero clock gauge,
  eq34 chain datum, carry_invariant, pin-aware PD, parallel seam) — hence
  recalibrate FIRST, then arm.

## 2. Phase A — recalibration instrument (analysis only)

New standalone script `scripts/clock_hadamard_calib.py` (python; numpy ok;
input = a run's `logs/network_solution.csv`, kind==C rows → per-sat offset
series + `logs/network_trace.csv` for epoch inventory):

1. Per satellite: remove the per-constellation per-epoch common mode (median
   across that constellation's sats — the clock-monitor convention) so datum
   realization drops out.
2. Segment each series at disc events / value steps > a coarse guard (IODE
   Δ_eph rebase steps and datum re-references must not enter the statistics;
   the producer re-bases carried clocks at cutovers but the emitted series can
   still step).
3. Per segment, compute **Hadamard** deviation (drift-insensitive — the right
   statistic for BRDC-contaminated series; Allan as a cross-check) over
   τ ∈ {30, 60, 120, 300, 600, 1800} s; fit (q₁, q₂) by regressing the
   two-state model's variance law over τ; report per satellite, per block
   (IIF-Rb / III-Rb / PHM / Galileo-PHM), per constellation.
4. Deliverable: a table + recommended q₂ per constellation, with spread. If
   intra-constellation block spread is >3×, propose per-constellation q₂
   override keys (`clock_model_q2_gps`, `clock_model_q2_gal`; tiny
   GnssConfigLoad addition, default = the global key — byte-identical).

Input data: the converged 305-sta artifacts from the Jul-14 attribution re-run
(3 h + 6 h all-on, 12 h GPS-only PASS — see memory
`project_net305_attribution_rerun` for run dirs). ≥3 h of 30 s epochs per
satellite is sufficient for τ ≤ 1800 s; if those CSVs were purged, any fresh
30 s lineage run works — do NOT calibrate from a 300 s run (τ floor too high,
and the tier's D5 churn contaminates).

**Gate A:** q₂ table reproduces the documented order (≈1e-7 class); flag any
satellite whose fitted q₂ deviates >10× from its block median (those are
fault-history candidates, exclude from the block fit); write the table into
`docs/` alongside this card.

## 3. Phase B — arming A/B at 30 s

Config: a 30 s GPS-only lineage config (`_net305_3card_gpsonly_30m.json` base,
1–3 h) + `clock_model: true` + Phase-A q₂ values. Control = same config,
clock_model off. Same build, Release, renamed exe, zombie pre-kill.

Watch (counters + graded, per invariant #4/#5):
- NIS flag rate (clock-monitor storms = residual miscalibration; a healthy arm
  flags rarely and on real events),
- T5 stale-reseed events ≈ 0 (max-gap sane at 30 s),
- dd-frac (datum-invariant grade) hold-or-improve vs control,
- clock prediction residuals vs the 3.7 cm/epoch expectation,
- run-end carry/anchor-info: the clock prior info fraction (B7 trace) should
  rise materially — that IS "using stability to our advantage", quantified.
- Stretch: closed-loop consumer vertical vs control (the M4 "FULL =
  clock-wander bug" hypothesis — a tight honest clock carry is the missing
  piece that card identified).

**Invariant #1 pre-flight (mandatory):** the two-state carry is a stiffer
time-linked prior, so every event response must be FORCE-FIRED once in a smoke
before any graded run (the test-cadence rule: smoke arms, force-fire paths):
(a) clock-monitor flag → soft large-q reinit (`clk_mon_reinit_var` path),
(b) T5 gap reseed, (c) IODE cutover Δ_eph re-base (no spurious disc). All three
exist; the gate is proving they fire under the CURRENT datum machinery
(sum-to-zero / eq34 / carry_invariant were all built after this model).

**Gate B:** force-fire smoke clean → 1 h A/B: NIS sane, dd-frac ≥ control,
prior-info fraction up. Only then a 3 h graded arm. Report honestly if the
sum-to-zero datum interacts badly with the common-mode removal (that
interaction is the untested seam — anchor `datum common-mode removed`).

## 4. Non-goals

- Datasheet q's. Never again; q comes from Phase A or not at all.
- Arming at 300 s (dt³ makes it structurally unrewarding — see §1).
- An emission-frame clock continuity filter (clock twin of the PB filter) —
  plausible follow-up if Phase B still shows wire clock wander, but it is NOT
  this card; do not scope-creep into it.
- Touching `sat_clock_q` (the scalar path stays the default and byte-identical).

## 5. Pointers

`ApplicationNetwork.cpp` anchors: `clockOscPredict`, `struct ClockOsc`,
`stale_prior_reseed` (T5), `datum common-mode removed`. Config getters:
`networkClockModel*` block in `GnssConfig.hpp` (the q₂ Doxygen holds the
calibration provenance). Memory: `project_network_clock_rate_c1` (the C1 card),
`project_closedloop_consumer_vertical_bug` (the q2=1e-8 negative + clock-wander
root), `project_net305_attribution_rerun` (input artifacts),
`project_dev300s_tier` §Q-theory (the estimable-vs-physical decomposition this
card operationalizes).
