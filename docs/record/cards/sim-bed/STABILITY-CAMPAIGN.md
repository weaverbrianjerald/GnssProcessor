# stability: the settled state must stop moving (targets 1 to 4)

**STATUS**

- **id:** stability
- **topic:** sim-bed
- **status:** closed
- **summary:** Late drift is the eclipse-exclusion valve's edge response (carry aging FALSIFIED); `sticky_pivot` cuts the sim settled consumer rms 74 % (RECOMMEND_SCOPED); the seam transient is the wire's phase-bias re-convergence.
- **dates in the text:** 2026-08-13 → 2026-09-02
- **knobs / posture:** `network.sticky_pivot`, `network.closed_loop.replay_products_path`, `network.consume_ura`, `network.consume_phase_bias_sigma`, `exclude_eclipsing_satellites`, `amb_seam_reinflate_var_m2`
- **hashes named in the text** (commit / exe / byte-cert): `fd1d123`, `c799a8b`, `ed170b2`
- **run dirs named in the text:** `runs/simnet60_quiet_wu93_24h`, `runs/camp60wu_quiet24h_r2q3`, `runs/camp60wu_quiet24h_sticky`, `runs/simnet24_inject_`, `runs/inj24g_`, `runs/camp60wu_q12warm_szv`, `runs/camp60wu_q12warm_ari`, `runs/fld63_stk_` … (+2 more in the text)
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §18, 18.1, 18.2, 18.3, 18.4, 18.5, 18.6, 18.7, 18.8, 18.9

**Outcome.** The campaign asked why the 60-station sim bed's settled state keeps moving. Metre-class per-satellite clock tails are the clock and C5Q code-bias split re-realizing at eclipse re-admission, and the largest consumer steps ride the eclipsing clock pivots. On the field (GPS dual) the `sticky_pivot` mechanism transfers exactly but the return steps are mm-class, so its recommendation stays scoped (both re-founding goldens arm it). The injection bench re-verifies a 2-cycle MDB (22/24 attributable); `amb_seam_reinflate_var_m2` is a proven NULL. The seam build continued in [frame restore](../resume-seam/RESUME-FRAME-RESTORE.md) and closed in [E2 + dense border](../resume-seam/RESUME-DENSE-BORDER-E2.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §18 THE STABILITY CAMPAIGN — the settled state must stop moving (2026-09-01 →)

Successor phase to the §17 deep on/off campaign (7 verdicts, fd1d123→c799a8b,
plus the seam-ZWD/RT-snapshot session ed170b2/8da1de6).  The standardized base
is `_camp60wu_quiet12h_r2q3` (R2 sticky continuous handover + Q3 anchor q 3e-6
+ relevel_free_epoch + pivot clock datum + zwd_seam_reseed on resume postures);
targets, in priority order: (1) settled late drift, (2) producer post-resume
transient, (3) per-sat settled clock tails, (4) T3/T4 MDB on a faulted bed,
(5) LV-6 design.  One mechanism per arm; verdicts land here + ledger rows.

### 18.1 TARGET 3 — per-sat metre-class settled clock tails: ROOT-CAUSED
### (2026-09-01, census instrument on the 12 h bed; fix cards derived, not built)

Instrument: `scripts/simnet_sat_clock_census.py` — per-sat settled clock
residual, median-demeaned per constellation per epoch (datum-free, robust to
level wander), joined against fixture `truth_visibility` coverage, wire
presence (emitted vs truth-visible), and the producer's own URA.  Run over
`_r2q3`, `_r2q3s0`, `_r2q3bca`.

**The tails are not a fixture property and not "clock quality" — they are a
producer RE-COUPLING defect acting on the fixture's eclipse schedule:**

1. **Every metre-class tail sat has a ~55-min visible(≥10 sta)-but-NOT-emitted
   window**, and the 15 windows over 12 h form the classic eclipse staircase
   (sequential per orbital plane, ~1.8 h spacing).  The excluder is
   `exclude_eclipsing_satellites` (armed in the standard set; it logs no
   per-event line — the census infers the windows from wire-vs-visibility).
2. **The tail is a STEP exactly at the re-admission epoch, then constant for
   hours** (p50 ≈ rms ≈ max): G25 d −0.43 → −7.13 at h3.22 (±3 cm for the
   following 8.7 h), G12 +6.33 at h2.35, G26 −9.4 (own frame) at h5.9, G22
   +1.81 at h11.30 (mid-settled, the directly-measured case).
3. **Mechanism: the clock ↔ C5Q-code-bias split RE-REALIZES at re-admission.**
   Pre-eclipse G25 carried clock −0.90 m / b_C5Q −7.53 m — the ~7 m is a
   COLD-START third-frequency misrealization parked in the weakly-estimable
   C5Q channel (7 m from the injected ±0.6 m OSB truth) where it is
   consumer-inert.  Post-readmission: clock −7.62 / b_C5Q −0.42 — the
   misrealization RELOCATED into the consumer-visible clock, and the stiff
   states hold the new split for the rest of the run.  G12 shows the exchange
   with BOTH channels landing badly (clock +0.06→+6.42, C5Q +1.91→−4.89):
   the re-seed lands wherever the readmission epoch's estimability puts it,
   then freezes.  The applied C1C combination (clk + b_C1C) steps ~6.6 m.
4. **Marking status — CORRECTED 2026-09-01 (same session, deeper read; the
   first published form of this point claimed "silent on every armed
   instrument" and was WRONG in two parts).**  On THIS posture the
   re-admission IS marked, by Card Q3's pending-disc path: the anchor
   decouples during the gap (`cta_decouple` reason=gap), the disc mark goes
   PENDING and LANDS at the return epoch (`cta_disc_landed` — R record +
   per-sat phase/code/clock counter bumps; verified at G25's return tow
   314010), the URA spikes to ~0.30 m at the return epoch and decays to
   ~0.02 m within 4–5 epochs (honest at the edge — the earlier "URA silent
   4–7 mm" reading sampled settled medians and post-window epochs, missing
   the single-epoch spike), and the closed-loop consumer logs phase + code +
   clock discontinuity resets for the sat.  The ~0.06 m ordinary-sat edge
   response of §18.3 is this MARKED chain working.  What remains true and
   defective: (a) the CLK-R magnitude tier (`clock_disc_jump_test`, armed)
   emitted ZERO CLK_STEP events for these 6.3–6.7 m steps — **gap-blind**
   (the rebase engine treats a vanish-return as a first sighting), so on any
   posture WITHOUT `clock_temporal_anchor` + `clock_anchor_mark` (every
   field lineage while G4 blocks Q3) the re-admission re-realization goes
   unmarked; (b) the re-coupling edge is marked but NOT re-validated — the
   split is free to re-realize anywhere and the stiff states freeze it, so
   the churn and the frozen far-from-truth realization are signalled, not
   prevented; (c) the settled URA returns to mm on a lump sitting ~7 m from
   the truth satellite clock — contract-legal (the wire carries estimable
   lumps and the consumer applies C+B+P coherently), but any absolute-clock
   consumer pays it unwarned.
5. **Datum posture changes WHO PAYS, not whether.**  r2q3 and bca reproduce
   the same top-3 (G25 7.2/7.6, G26 6.9/7.0, G12 6.7/6.4 — the "fixture-
   common, both datums" reading from the bca session, explained).  Under bca
   the r2q3 pivot G01 itself shows a 4.1 m tail (it eclipses h4.7–5.6; under
   the pivot datum its own re-realization redistributes network-wide through
   the gauge).  Under s0 the per-sat tails collapse ~10× (G25 0.71, G12
   0.28): the per-epoch Σclk=0 constraint SMEARS the misrealization across
   the ~18-sat constellation (7 m/18 ≈ 0.4 m each) — the S0 lesson again;
   the bill moves to cb.
6. **Bridge to target 1 (hypothesis, 24 h bed will test):** GPS sats eclipse
   twice daily; the 12 h bed's rising h10–h11 contains the G14 (h10.69), G22
   (h11.29), E21 (h11.70) readmissions.  The settled consumer level-wander
   may be partly the superposition of these split re-realizations passing
   through the applied products.

Derived fix cards (NOT built; re-prioritized by the marking correction above
and by §18.3's consumer numbers — row updated in
SPEC_prefit_census_observability §13):
- **E3 (THE lever, from §18.3): satellite-pivot / datum hygiene at the
  exclusion edge** — the pivot sats' own transits step the consumer
  0.3–0.8 m network-wide; election avoiding eclipse-season sats, or a
  level-bridged re-pin at pivot return.
- **E2 — realization-continuity at re-admission**: re-seed the returning
  sat's clock↔bias split AT its pre-gap realization (then let the data
  re-earn any true change) instead of letting it re-realize freely and
  freezing the result.  Prevents the churn the current chain can only mark.
- **E1 — gap-aware CLK-R jump test**: give `clock_disc_jump_test` a pre-gap
  baseline so it catches re-admission steps on postures without Q3 +
  `clock_anchor_mark` (all current field lineages).  Redundant on this bed;
  the robustness card elsewhere.
- **The C5Q cold-start misrealization itself** (why 7 m parks in the third
  frequency at ignition) — LV-6 / level-authority family; it is the reason
  every re-realization is LARGE.  Keeping the lump parked consumer-inert may
  remain the right product call; shrinking it shrinks every edge.

### 18.2 TARGET 1 — settled late drift: instrument + 12 h re-analysis
### (verdict in §18.3)

Instrument: `scripts/simnet_drift_age.py` (commit 2306134) — hourly
consumer/channel/event decomposition + DD-truth-error-vs-chord-age join
(the simnet_amb_integer join; stations from the FIXTURE dir).  Join gate:
reproduces the flagship's published hourly consumer values exactly.

12 h re-analysis, before the 24 h bed:
- **h11 is partly product-day-edge artifact**: all 79 h11 `attrib_arc` events
  sit in the final 5 epochs (tow 345420–345540, many-sat = product-quality);
  h11 decomposes pre-zone 0.157 / edge-zone 0.200 / final-6-ep 0.269.  The
  real drift is h7 0.107 → h10 0.132 → h11-clean ~0.157.
- **The drift is a wandering UP-dominated BIAS** (hourly up-mean −0.073 →
  −0.111 → +0.008 → +0.108 → +0.145 m h7→h11, zero-crossing at h9 where 3D
  dropped back to 0.107; rms ≈ |mean|) — level-wander signature, not noise
  growth and not monotone aging.
- **No graded channel moves h7→h10** (clk residual/contrast flat, cb falls,
  network-median ZWD flat); settled per-epoch consumer 3D correlates +0.53
  rxdcb_contrast, +0.45 clk_contrast, −0.67 rxclk_contrast.
- **DD ambiguity truth error does NOT age**: med|e| by chord-age bucket
  3.57 → 3.45 → 3.32 → 3.13 → 2.96 → 3.16 cyc (0-10 → 481-960 ep) —
  flat-to-improving; traced population constant (~320–400k rows/h).

The 24 h bed (`runs/simnet60_quiet_wu93_24h`, `config/sim/simnet60_quiet_24h.
json`: 00:00:30 → 23:59:30 of DOY 129, single product day, final epoch = the
12 h fixture's exact trailing epoch) decides monotone-aging vs bounded-wander
with 15 clean settled hours, decorrelates run-hour from time-of-day (the
00:00 anchor), and carries the RTB-7 snapshot writer + per-epoch history so
the scheduled mid-run seam-reseed arm can branch from any hour.

### 18.3 TARGET 1 VERDICT (2026-09-01, the 24 h arm `runs/camp60wu_quiet24h_r2q3`):
### carry aging FALSIFIED; the "late drift" is the transient response to the
### ECLIPSE-EXCLUSION valve's edge events, dominated by the clock PIVOTS' own transits

The arm: `_camp60wu_quiet24h_r2q3` = the flagship posture on the 24 h bed
(single mechanism: fixture + duration; datum pinned S30000SIM, which is also
the elected #1 on this arc), RTB-7 writer + per-epoch `.ep` history armed
(write-outputs-only, proven).  2879/2879 epochs emitted, 0 withheld, consumer
processed all; 1395 ms/epoch.  Pinned snap 5.26 h (vs 4.37 on the 12 h bed —
NOT like-for-like: the 00:00 anchor puts the first-transit eclipse cluster
h2.3–6.9 inside the convergence window).  Settled (h12–h24) consumer 3D rms
0.303 / p50 0.130 — the p50 matches the 12 h flagship (0.128) exactly; the
rms gap is entirely the h16–h18 excursion tail.

1. **Aging falsified on every axis it could act.**  Hourly consumer 3D rms
   h7→h15: 0.108, 0.069, 0.065, 0.072, 0.077, 0.115, 0.083, **0.053, 0.056**
   — the BEST hours of the whole run at 14–15 h of carry age.  DD ambiguity
   truth error IMPROVES monotonically with chord age (med|e| 3.67 → 3.57 →
   3.53 → 3.43 → 3.35 → 2.98 cyc, 7.85M rows joined, 0 dropped).  Old chords
   beat young in every settled hour except h17 (old 5.11 vs young 3.39 — the
   pivot-cluster hour: the EVENT damages carried state more than fresh state,
   a micro-confirmation of the mechanism).  Network-median per-station ZWD
   drift h7→h23: −0.004 m (flat over 16 h).
2. **The only settled excursions coincide with eclipse windows.**  h16–h18
   (rms 0.434/0.434/0.711, excursion-dominated: h16 rms 0.434 vs p50 0.079)
   contains, in sequence: E25 readmit h16.61, **G01 (the GPS clock pivot)
   leave h16.68 / return h17.58**, G26 h16.94/17.83, **E02 (the Galileo
   clock pivot) leave h17.41 / return h18.35**, G16 h18.01/18.87.  The h12
   bump (0.115) aligns with the E31/G20 windows.  Edge-aligned consumer bias
   steps ([-30,-5] vs [+5,+30] min): ordinary sats p50 **0.060 m**; the
   pivot cluster: E02 RETURN **0.826 m**, E02 LEAVE 0.515, G16 LEAVE 0.510,
   G26 RETURN 0.441, E25 RETURN 0.422, G01 0.308/0.343.  (No control
   stretch exists: in the settled half no 50-min window is ever > 45 min
   from an eclipse edge — the "quiet bed" is not quiet, it carries a
   30-edge/12 h event schedule.)  Corroborated by the event census:
   cta_decouple/recouple spike ~50–100/h exactly at the two transit
   clusters (h3–h5, h16–h18) against a ~1/h background — same UTC as the
   12 h bed's h4–h5 spike.
3. **Not TOW-deterministic — a transient response, not a diurnal signal.**
   Identical UTC evenings across beds (12 h h7–h11 ≡ 24 h h19–h23):
   0.107/0.115/0.107/0.132/0.179 vs 0.225/0.267/0.207/0.117/0.113.  The
   schedule is deterministic; the network's response depends on state
   history.
4. **The 12 h bed's "monotone h7→h11 rise" decomposes fully**: the evening
   eclipse staircase (E30/E27/G14/G22/E21 readmissions, UTC 20–24) + the
   final-5-epoch product-day-edge artifact (§18.2).  The wandering
   UP-dominated bias of §18.2 is the low-frequency tail of these edge
   responses.  "Young-carry resumed legs beat the flagship h8–11" re-reads
   the same way: fresh filters respond differently to the same evening
   edges — not evidence of aging.
5. **Consequences for the campaign:**
   - The mid-run scheduled seam-reseed arm is DE-MOTIVATED as an aging probe
     (nothing ages); the `.ep` history (2879 files) stays as target-2's
     branch library.
   - The dominant stability lever is now TARGET 3's valve: the biggest
     consumer steps ride the SATELLITE clock pivots G01/E02 — both
     lowest-PRN elected, both eclipsing daily on this fixture.  Satellite
     pivot election that avoids eclipse-season sats (or pivot-handover
     hygiene at the exclusion edge) attacks the largest term; the §9.8
     "pivot election is a product-quality decision" lesson, now for
     SATELLITE pivots.  Plus §18.1's cards: gap-aware disc marking +
     re-coupling split re-validation.
   - Any future "settled stability" grading on this fixture must treat the
     eclipse schedule as part of the bed: quote settled p50 alongside rms,
     and never grade a settled window without stating which transit windows
     it contains.

### 18.4 E2 DESIGN SKETCH — realization continuity at re-admission (not built)

The current chain (per the §18.1 correction) MARKS the re-realization and the
consumer resets; E2 would PREVENT it.  Mechanism: retain per satellite, at
last emission, the realized split — common-removed clock (the E1 stage-2b
baseline already retains exactly this) plus the per-signal code-bias values.
On re-registration within a horizon, seed the returning sat's fresh columns
AT the pre-gap realization as a PRIOR with gap-grown σ (never a pin —
invariant #1: a real in-gap jump must be able to win), instead of the
generic wide initial_std.  The estimator then re-converges FROM the old
realization: continuity is free unless the data genuinely moves it, and a
genuine move still trips CLK-R/Q3 marking — the response ladder becomes
seed-first, mark-if-moved.

The hard part, named: the stored realization lives in the EMITTED estimable
frame; the seed must enter the estimator's internal frame through the frame
map current at the seed epoch (the carried-prior η = Λ·(value_abs − x_lin)
family — the exact algebra the seed-site snap card got wrong before it was
deleted, SPEC_lane_slip_response §7.2).  The seam is the returning-sat column
initialization path (StateInitializer / the shared-column registration in the
estimator), NOT the emitter.  Own card, with the frame-map derivation done
first and a sim A/B on this bed's eclipse schedule as the validation venue
(the schedule provides 30 natural events/12 h; the census instrument already
grades per-sat steps).  Prediction to pre-register when built: the per-sat
census tails (§18.1) collapse to the pre-gap realization's continuity, and
the ordinary-sat consumer edge steps (~0.06 m) shrink toward zero; the PIVOT
edges are NOT E2's to fix (that is E3's sticky/hygiene territory).

### 18.5 E3 VERDICT — `network.sticky_pivot` (RTA-1) removes the pivot-eclipse
### consumer cost: settled 24 h consumer 3D rms 0.303 → 0.078 m (−74 %)
### (2026-09-01, arm `runs/camp60wu_quiet24h_sticky`, single knob vs §18.3's baseline)

Prediction verified EXACTLY: **2 PIVOT handovers** (G01→G02 at G01's eclipse
leave h4.70, E02→E03 at E02's leave — both `adjacent`, no flip-backs) vs the
baseline's 8; on this roster the advanced-to pivots G02/E03 never eclipse, so
the datum never rides an excluded satellite again.

- **The h16–h18 pivot-cluster excursion is GONE**: 0.054/0.052/0.052 rms vs
  0.434/0.434/0.711.  The evening staircase response collapses with it
  (h19–h23: 0.058–0.080 vs 0.113–0.267).  Settled (h12–24) consumer 3D
  **rms 0.078 / p50 0.064 / max 0.192** vs 0.303/0.130/2.24 — the largest
  single-knob consumer win this bed has produced (R2 gave 0.49→0.27, Q3
  0.27→0.20).  Ignition also improves (first-transit cluster no longer
  disturbs convergence: snap 5.26 → 4.73 h; h5 0.471 vs 1.235).
- **Built-in structural control**: h0–h3 are numerically IDENTICAL to the
  baseline — sticky differs only when the incumbent vanishes, and the first
  divergence is exactly G01's leave epoch.
- **Mechanism separation proven**: the per-sat census tails are UNCHANGED
  (G26 6.35 / G12 5.96 / G25 5.79 vs baseline 6.36/5.98/5.75) — the split
  re-realizations still happen per satellite (E2's territory); sticky removes
  their NETWORK-WIDE datum ride (E3's).  URA/coverage unchanged.
- **Who pays (S0-lesson diligence)**: cb residual p50 2.4 → 3.9 cm, rms
  0.54 → 0.79 (max unchanged ≈ 7.1 = the C5Q lumps) — a small bill does move
  to cb, but unlike s0 the CONSUMER (which applies cb) wins outright.  ZWD
  improves (0.0109 → 0.0091); clk contrast flat (2.13 → 2.15).  Minor: h11–
  h14 slightly worse (e.g. h12 0.145 vs 0.115) — small against the wins.
- Fingerprint: `sticky_pivot` is IN the warm-resume fingerprint (fresh
  convergence only — never flip it mid-chain); this arm was fresh.

Scoped recommendation (ledger row): arm on this bed's posture family
(pivot-datum quiet-bed lineages) — it is the E3 lever the §18.3 verdict
called for, at zero mechanism cost.  Field lineages inherit the same
argument wherever the clock datum is pivot-realized and pivots can be
eclipse-excluded; their arm needs the standard fresh-convergence discipline.

### 18.6 TARGET 4 VERDICT — the injection bench re-verified on the r2q3 posture:
### MDB unchanged at 2 cycles, 22/24 attributable (2026-09-01)

The §15 bench REGENERATED post-wu93 (the 2026-08-13 fixtures predate the
wind-up fix — A3 rule: re-capture, never re-grade) and re-run under the
STANDARDIZED posture (the full r2q3 armed set, GPS-scoped): fixture
`runs/simnet24_inject_{null,fault}_wu93[g]`, plan `--constellations G`
(ladder 1/2/3/5/10/50 cyc ×2, seed 7, 24 per-band events), arms
`runs/inj24g_{null,fault}` under `_inj24g_{null,fault}_r2q3.json`
(elected datum S15000SIM), preflight CLEARED on slip_detect /
raim_postfit / raim_within_epoch.

- **22/24 = 91.7 % attributable within 3 epochs, median latency 0 —
  reproducing the published bench rate; empirical MDB: every ≥2-cycle rung
  100 % except one +50 miss.**  The bca-session worry ("T3/T4 flag volume
  collapses 650× under anchor-free datums — re-verify MDB before trusting
  post-fit QC") does NOT transfer to the standardized posture: Q3+R2
  preserve the detection stack's power intact.
- Misses, both gradeable: a +1 cyc L5Q at elev 22° (below MDB — expected
  class) and **a +50 cyc L2W at elev 50° — a detector-RANGE hole** (a huge
  step evading the slip funnel, plausibly rejected as an outlier instead of
  flagged; S05000SIM G20 tow 305400).  Own follow-up note; the 50-cyc rung
  read 75 % here and the mechanism deserves a look before any field claim
  about large-slip immunity.
- Null arm NOT quiet (35 WL detections — the §15 known caveat, reproduced),
  but ZERO coincidence with fault flags, so the attributable rate is honest.
- Scope honesty: detection is T0/T1-led (median latency 0), so this
  re-verifies the DETECTION STACK's MDB under r2q3; T3/T4's isolated
  post-fit power (catching what the pre-processors miss) was not separately
  exercised (3 T4-differential flags rode along) — the isolation extension
  is a T0/T1-disarmed arm pair, not run.
- ⚠ GPS+Galileo postscript: the full r2q3 posture FAILS the NetworkSbasis
  null-space gate on this tri-family fixture (21–25 near-zero eigenvalues
  vs declared dim 2 — the Galileo side lacks a closing datum in this
  posture on this fixture; the original bench was GPS-only and the E-event
  plan wasted 10 events on a G-blind producer).  Recorded; a GPS+GAL bench
  needs its own estimability card first.

### 18.7 TARGET 2 VERDICT — the post-resume transient is a ~4 h WIRE-REALIZATION
### re-convergence; carry-side seam responses are already native (2026-09-01)

Leg A (`runs/camp60wu_q12warm_szv`): the flagship posture warm-resumed at h12
of the 24 h bed from the continuous run's `.ep1440` snapshot, zwd_seam_reseed
armed (window ran exactly: 28800 = 60×480 folds).  Leg C
(`runs/camp60wu_q12warm_ari`): leg A + `amb_seam_reinflate_var_m2 0.0025`.
`state_snapshot_resume_shock_m` recorded INAPPLICABLE on this posture (it
shocks the carry_invariant blob only; this bed runs carry_invariant false).

1. **The transient is real and lasts ~4 h, not minutes**: consumer 3D rms
   h12 0.733 → h13 0.254 → h14 0.336 → h15 0.300 vs continuous 0.115/0.083/
   0.053/0.056; parity ~h16.  ZWD no-restart at the seam (h12 0.014 vs
   cold-class ~0.39) — the zwd_seam card's value reproduces at h12.
2. **It lives ENTIRELY in the applied-products realization.**  Every
   datum-invariant channel is equal-or-BETTER on the warm leg through
   h12–h16 (clk contrast h12: 1.29 vs 2.30; rxdcb/rxclk/phase flat) — the
   §18.3 signature again: consumer-visible, datum-invisible.  The binding
   candidate for the first minutes is EMITTED-FRAME continuity across the
   resume seam (the products' realization re-settling through the consumer),
   not any carry-side memory.
3. **After parity the young leg beats the aged continuous 2–3× through the
   eclipse evening** (h19–h23: 0.119/0.095/0.078/0.073/0.048 vs 0.225/0.267/
   0.207/0.117/0.113) — "young legs beat the flagship" reproduced at matched
   TOW; consistent with §18.3's h17 old-chord vulnerability.  The
   still-converging leg takes the pivot cluster harder (h17 0.832 vs 0.434)
   — with `sticky_pivot` landed (§18.5) that cluster cost is gone anyway.
4. **Leg C = a proven NULL, and the mechanism is named.**  Products
   BYTE-IDENTICAL to leg A (sha256 equal) despite the mechanism firing
   ("+2.5e-3 m² added to 2811 restored leveling carries").  Cause:
   `relevel_free_epoch` (standard-armed) latches EVERY re-latching arc at
   the seam — the inject-site skip (ApplicationNetworkAssembly.cpp:2860)
   precedes the only live read of `LocalCarry.var` (:2889), and the harvest
   then overwrites the entries with fresh posteriors (run counters: 2294
   latches, 1824 inject skips).  So (a) the knob is INERT AS BUILT on any
   relevel_free_epoch posture — ledger row says DO NOT ARM; (b) the seam
   already gets a native per-arc free re-level — a stronger response than
   the soft refloat the knob intended; (c) the (c')-bed "restored amb-carry
   drag" reading deserves a re-look on relevel_free_epoch postures (the
   h-vintage amb VALUES survive only one epoch there).  No follow-up build
   warranted: leg A shows no post-retire ZWD drag on this bed (h16–h23
   0.009–0.018 ≈ continuous).
   ⚠ **CORRECTION (2026-09-02, §18.10 readers):** the NULL verdict stands but
   the mechanism named above is wrong.  The 2811 restored leveling carries
   are never re-leveled by `relevel_free_epoch` — they are ERASED before PASS
   B at the first warm epoch by the QUEUE-1(d) phase-anchor carry
   re-reference (`networkApplyPhaseAnchorCarryReref`, NetworkPhaseDatum.cpp:
   453-471), because `anchor_incumbent` / `anchor_pin_target` are not
   snapshotted and the election at 302430 runs incumbent-less for all 162
   (sat,signal) families (NetworkPhaseDatum.cpp:431-435, no event emitted).
   Run-end counters: "phase-leveling local carry(ies) erased" 2862 on the
   12 h warm leg vs 129 on the 24 h continuous run; the "2294 latches / 1824
   inject skips" are the ordinary RUN RATE (continuous 24 h: 4010/3762), not
   the seam; `free_ambs` is empty at 302430 (the only events there are the
   two CLK_FRAME resume_seam marks), and the free-epoch latch's seam role is
   the SECOND epoch's 146 cold-MW false refloats (292 latches).  So
   `amb_seam_reinflate_var_m2` inflates entries that are deleted one pass
   later: the h-vintage values survive ZERO epochs, not one.  The (c')-bed
   "restored amb-carry drag" re-read (queue item 6) is moot for the same
   reason — no restored value is ever folded on this posture.

Open (cards, not built): emitted-frame continuity across the resume seam —
the ~4 h consumer re-convergence against flat datum-invariant channels is
the §18.1/§18.3 family's resume-seam instance; the RTB-7 `resume_seam`
datum-mark exists, the CONTINUITY (bridging the realization instead of
re-marking it) does not.

### 18.8 E3 FIELD TRANSFER (2026-09-02, `runs/fld63_stk_{ctl,on}`):
### mechanism transfers EXACTLY; the sim bed's harm is ABSENT on the GPS-dual
### lineage, so the recommendation stays SCOPED

The pair: `_ext63_12h_evt_pl_clkpin` (61 real stations, GPS dual, DOY-129
UTC 00–12, pivot datum MBAR00UGA, eclipse exclusion armed, NO Q3 / NO CLK-R
jump test) re-run FRESH under the current build (the archived
`evt_pl_clkpin12` predates wu93 — A3), single knob `network.sticky_pivot`
between arms, both cold starts.  Instrument:
`scripts/field_eclipse_census.py` (wire-only; join-gated on the sim bed).

1. **Mechanism transfer: EXACT.**  All 8 GPS exclusion windows appear at the
   sim bed's UTC schedule (G01 h4.70–5.62 vs sim h4.70–5.60; G12/G25/G26/
   G16/G14/G22/G20 all match) — same day, same constellation, same shadow.
   Control: 2 PIVOT handovers (G01→G02 `adjacent` at the leave, **flip-back
   G02→G01 at h5.62**); sticky: 1 (no flip-back; the non-eclipsing G02 holds
   the pivot for the rest of the run).  Arms numerically identical to h4.71
   (the structural control transfers too).
2. **The harm sticky prevents is ABSENT here.**  Per-sat return steps are
   MILLIMETRE-class (2–8 mm vs the sim bed's 6.3–6.7 m): GPS-dual has no
   weakly-estimable third frequency to park a cold-start misrealization in,
   and the SH iono field tightens the bias estimability.  The producers are
   arm-equal on every instrument: post-fit σ̂₀ 0.0683 both (phase 0.0093 vs
   0.0092), prefit H 8.40 vs 8.55, event census matching tier-by-tier
   through the divergent hours.
3. **Consumer: NO attributable difference.**  This chain's closed-loop
   consumer floats at metre level with ±3 m hourly swings in BOTH arms (the
   known consumer pathologies — vertical bug / FAIL-UNSAFE AR family); the
   post-divergence hour deltas (including one h8 excursion, sticky 9.09 vs
   control 2.39 rms) have NO producer-side driver and are consumer
   realization, not product quality.  No field consumer win is demonstrated
   — and none was expected, given (2).
4. **Scoping (ledger row updated):** the §18.5 RECOMMEND stays scoped to
   postures with MATERIAL per-sat re-realizations at re-admission —
   triple-frequency / weak-third-signal lineages, the sim bed family.  On
   GPS-dual + SH field lineages sticky is mechanism-sound (strictly fewer
   datum re-references, one instead of two per pivot transit) and
   cost-free on every producer instrument, but benefit-undemonstrated:
   legal to arm on a fresh convergence, not evidence-recommended.
5. **E1 corollary**: the unmarked-re-admission gap E1 catches is likewise
   immaterial on GPS-dual + SH — E1's field validation arm belongs on a
   TRIPLE-frequency field lineage, where the C5Q-class misrealization
   exists to relocate.

### 18.9 QUEUE-1 DATA VERDICT — the post-resume transient is the WIRE's phase-bias
### RE-CONVERGENCE, not the consumer's cold start and not a one-time realization
### step (2026-09-02, consumer-only replays on the h12 warm leg)

Instrument: `network.closed_loop.replay_products_path` (Application::
runConsumerReplay, ~33 s per 12 h arm) driving the SAME held-out consumer
(SZZ000SIM, ar_mode 0, undifferenced, stiff ambiguities q 1e-12) cold from
h12 against an already-written wire.  Exactness check: the replay of leg A's
own wire (`runs/seamctl_replay_warm`) reproduces leg A's in-process consumer
TO THE LAST DIGIT in every hour h12–h23 (0.736/0.254/0.336/0.300/0.437/0.833/
0.874/0.119/0.096/0.078/0.073/0.048) — the driver is an exact consumer
instrument on this bed.  ⚠ Driver semantics: `start_offset_hours` anchors on
the first epoch AFTER the fast-forward to the first product record
(ApplicationConsumerReplay.cpp:355-365); a wire that begins at h12 takes offset
0 — a 12 h offset on top skips the whole file ("0 epochs processed").
Configs: `config/test_suite/_seamctl_replay_{cont,warm,warm_pbsig,warm_csig,
warm_p30m,warm_p2h}.json`; run dirs `runs/seamctl_replay_*`.

**1. The confound is dead: the consumer's own cold start is 20 minutes.**
Cold consumer at h12 against the CONTINUOUS wire (`_cont`): 20-min bins
0.90 → 0.12 → 0.09 → 0.07 (rms), h13 0.060, h14 0.067, h15 0.062 — the
continuous run's in-process consumer at the same hours reads 0.083/0.054/0.056.
The same consumer against the WARM wire: 0.81/0.74/0.65/0.30/0.23/0.23/0.32/
0.36/0.32/0.30/0.30/0.30 — the §18.7 "~4 h transient" is a property of the
wire.  From h13 the replay against the continuous wire tracks that run's
in-process consumer within noise (h16–h23: 0.405/0.457/0.636/0.223/0.262/
0.205/0.113/0.107 vs 0.433/0.434/0.711/0.227/0.266/0.207/0.117/0.113).

**2. What the warm wire does (products A = leg A, B = continuous, matched
epochs; instruments seam_product_diff / seam_decompose / seam_walk /
seam_quality / wire_pb_steps / wire_drift — session scratchpad, to be ported
under scripts/ with the build card):**
- *Epoch-1 realization step* (302430): emitted CLOCK between-sat SD (common
  mode removed) G 1.30 m (max 4.17; G12 −5.04, G17 −3.17, G10 −3.07), E 0.53;
  CODE BIAS identical to 0.1 mm; PHASE BIAS SD 8–12 m.  Decomposed per
  (sat,signal): the code-side change a consumer sees is −dC (p50 1.09 m G /
  0.29 E, CB unmoved); the phase-side change dP − dC has a UNIFORM fractional
  part (|frac| p50 0.28 cyc, p90 0.45) — so the seam offset is NOT a consumer-
  null transform (δ on clock + every bias of the sat, plus integers) and the
  consumer legitimately must re-absorb it.  It is CONSTANT for 12 h (E clock SD
  0.53→0.55; G 1.30→0.91 only because the eclipse re-admissions relocate G20
  +0.82, G12 +3.45, G25 −3.53, E24 +1.41 — the §18.1 tail sats, realized
  differently per run).  A step costs the consumer one reset (20 min) — it is
  not the 4 h.
- *Advertised uncertainty at epoch 1*: PB σ 1.11 m median (vs 0.012), URA
  0.30 m (vs 0.005), sigcc 0.30 (vs 0.005): the shared PB/clock posterior is
  COLD at the first warm epoch despite "376 D2 shared-carry + 2811 ambiguity-
  carry restored".  URA is back to 0.005 by +20 min; PB σ decays 0.139 (+20
  min) → 0.055 (+40) → 0.034 (+1 h) → 0.022 (+2 h) → 0.016 (+3 h) → 0.014 (+4 h)
  against the continuous wire's 0.009–0.011.
- *The reset storm (0–20 min)*: epoch-to-epoch phase-side steps (P−C) on the
  warm wire rms 0.77 m at epoch 2 (150/162 keys over the contract's 0.05 m
  disc_thresh, 143 over half a cycle), 0.27 (epoch 4), 0.18, 0.15, … 0.048
  (+9 min), 0.035 (+14), 0.029 (+20); keys over threshold per epoch
  150→142→131→124→98→82→75→92→70→49→…→8.  The emitter bumps the (sat,signal)
  counter on the first jump beyond threshold (NetworkProductEmitter.cpp:
  367-394) and the consumer resets that ambiguity on every counter change
  (RealTimeEngineSsrBreaks.cpp:112-118): the consumer log shows 20–50 of its 54
  ambiguities reset EVERY epoch for ~25 epochs (51/49/43/48/43/37/29/17/22/34/
  19/43/27/30/26/37/26/23/…).  Producer-side refloats are NOT the driver: MW
  refloated 146 arcs at epoch 2 only (cold slip-detector history), then at most
  2 per epoch.  The clock contributes nothing after epoch 6 (dC rms 0.005):
  the storm is the PHASE BIASES re-converging.
- *The drift era (20 min → ~4 h)*: after the steps fall under threshold the
  warm wire's PB per-epoch motion stays 2–3× the continuous wire's (dP rms
  3.4/3.1/2.7/2.7/3.8/2.3/1.6/1.4 mm per epoch from +1.5 h to +4.5 h vs
  1.4/0.9/1.2/1.1/3.6/1.9/1.4/0.7) and it is SYSTEMATIC (converging: G01 L1C
  −3.43 → −3.26 m over the first 30 epochs, ~6 mm/epoch); +20 min → +1 h sub-
  cycle drift p50 0.052/0.041 m (E/G) vs 0.026/0.031, with several L5Q keys
  exceeding λ/2 (the sub-cycle metric saturates at 0.126 m — it cannot size the
  tail; the late-start replays below do).  Continuous wire h12–h16 for
  reference: a quiet settled wire drifts sub-threshold too (3–6 cm per 40 min
  on PB keys) and its consumer does not care — the harm is the coherent,
  converging drift, not sub-threshold motion per se.

**3. The consumer integrates the drift (consumer-side invariant #1, third
instance after the Q5 mixed-pair ratchet and the RT-snapshot "fights").**
The disc test is step-only, so a converging phase-bias drift below 0.05 m
per epoch is never marked; a stiff consumer (amb q 1e-12, static position)
freezes whatever bias it converged onto.  Direct demonstration with late
starts on the warm wire: a consumer STARTED at +30 min (skipping the storm)
reads 0.44 → 0.27 → 0.12 (h13.3) and then WALKS OUT 0.21 → 0.38 → 0.49 →
0.52 → 0.54 → 0.54 → 0.58 → 0.65 → 0.69 m (h16.3) — worse than the h12 start;
a consumer started at +2 h needs ~2 h to reach 0.11 (0.85/0.43/0.35/0.22/0.14/
0.11/0.13/0.14).  Leg A's in-process consumer, having lived through the
storm, freezes at 0.30 ± 0.03 (h13.7–h16.3, p50 ≡ rms) until the h16.7
eclipse cluster resets it, by which time the wire has settled — hence "young
legs beat the flagship" afterwards (§18.7 item 3).

**4. Honouring the advertised σ is a TRADE, not the fix.**  This posture's
consumer ignores every σ channel (`network.consume_ura`,
`network.consume_phase_bias_sigma`, `closed_loop.consume_combined_sigma` all
default OFF — the emitted sigma=/sigc=/U records are decoration for it).
Replays with `consume_phase_bias_sigma` (or `consume_combined_sigma`, near-
identical) ON: h14 0.18 vs 0.34, h15 0.11 vs 0.30, parity with the continuous-
wire consumer by h15.7 (0.064 vs 0.303) — soft-while-uncertain ambiguities do
not freeze.  But through the h17–h19 eclipse cluster the σ-honouring consumer
reads 1.37/1.48/0.48 vs 0.83/0.87/0.12: the re-admission σ (URA-class spikes)
de-weights returning satellites into a worse geometry.  Recorded as a scoped
consumer-side mitigation with a named cost; the re-admission σ is E2's.

**5. Verdict and the card's target.**  Emitted-frame continuity across the
resume seam is a SOURCE-side problem: the shared phase-bias / clock memory does
not survive the seam as a cm-class prior (PB σ 1.11 m at epoch 1), so the
network re-converges its phase-bias split from the cold station layer for
~4 h and every consumer — warm or cold, in-process or replayed — pays for the
re-convergence, not for the step.  No wire-side hold can bridge a
re-convergence (a hold on moving values is invariant #7's silent walk), and a
drift-aware mark would only convert the drift into more resets.  The build
card (§18.10) therefore targets the restored-memory path at the seam: why the
D2 shared carry and the 2811 restored leveling carries do not act as the
pre-seam posterior once the station layer cold-starts and relevel_free_epoch
re-levels every restored arc (§18.7 item 4), and what a seam-scoped, forward-
only response (the `zwd_seam_reseed` shape) must restore so the first warm
epoch's PB σ is ~0.01 m and the per-epoch steps never cross the disc
threshold.  Pre-registered predictions for the ON arm on the .ep1440 branch:
first-warm-epoch PB σ median < 0.02 m; keys over 0.05 m at epoch 2 < 10 (from
150); consumer h13–h15 within 1.5× of the continuous-wire replay (0.060/0.067/
0.062); the +30 min late-start consumer converges monotonically (no walk-out).

