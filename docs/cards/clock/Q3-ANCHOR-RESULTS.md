# Q3 results: gates G0 to G3, the binding knee and the FC-1 score

**STATUS**

- **id:** Q3 results
- **topic:** clock
- **status:** closed
- **summary:** G0 to G2 PASS and the binding knee is q 1.0 to 1.5e-5 m²/s, but the FC-1 gate is NOT met: the anchor makes the clock quieter, not more real.
- **dates in the text:** 2026-08-10
- **knobs / posture:** `network.clock_temporal_anchor` at `clock_anchor_q` 3e-5, `network.clock_anchor_gauge_free` (G3 third arm); `network.clock_model` stays NO-GO (§6)
- **hashes named in the text** (commit / exe / byte-cert): `gnss_processor_cta.exe` md5 `F5068BA6707F04708CDB9299893D4F6A`; `gnss_processor_ctagf.exe` md5 `974714FB7174A8C9CE6FB239C71AB9EF`; G0 products md5 `C015A68F73393B3A1C0C02DEEC85E9C1`
- **run dirs named in the text:** `runs/cta_g0_ctl`, `runs/clkr_20ep_ctl`, `runs/cta_20ep_`, `runs/cta_1h_`, `runs/cta3_`, `runs/cta_3h_`, `runs/clkr_12h_hold`
- **source:** `docs/archive/SPEC_clock_temporal_anchor.md` §5 (G0, G1, G2, §3 pass 1, G3), §6

**Outcome.** The card ran the Q3 anchor's first gates. G0 (OFF-path byte-identity), G1 (453 anchored folds, one guard trip resolved in-epoch) and G2 at 1 h (robust σ_w 8.89 → 5.34 cm, 0 withholds) PASS. In the 3 h three-arm ladder no arm lifted the FC-1 correlation off zero (−0.004), so that gate was NOT met; G4 stayed owed. Later, [Q3 floor](../clock/Q3-PHYSICAL-FLOOR.md) §7 retired the FC-1 gate and put the physical q at 2.9e-8 m²/s; [Q3 ladder](../clock/Q3-FLOOR-GOVERNED-LADDER.md) §8.3 made σ̂₀_clk gauge-aware; [Q3 G4](../clock/Q3-G4-SIM-HALF.md) executed G4's sim half.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_clock_temporal_anchor.md -->
## §5 Results

Session binary `gnss_processor_cta.exe` md5 `F5068BA6707F04708CDB9299893D4F6A` (Release,
2026-08-10).  Unit suite **1331 passed / 1 skipped** (the fixture gate) — +14 cases
(`tests/test_clock_anchor.cpp` × 11: OFF-path bit equality, born-LOOSE, per-satellite elapsed
time, staleness fallback, the variance floor, epoch bookkeeping, the in-epoch step recovery,
the common-mode non-trip, the re-coupling run, and the two loader contracts;
`tests/test_network_estimator.cpp` × 2: the withdraw is the exact inverse of the fold, and the
dense re-fold equals a from-scratch marginalized fold, with its restore branch and no-op
cases).

### G0 — OFF-path byte-identity: **PASS, maximally**

`_golden63_20ep_clkr_ctl.json` re-run unchanged (`runs/cta_g0_ctl`) is **whole-file
byte-identical** to the stored pre-card `runs/clkr_20ep_ctl/products.txt` (md5
`C015A68F73393B3A1C0C02DEEC85E9C1`).  Re-verified after the R-record fix below.

### G1 — 20-epoch ON smoke (`runs/cta_20ep_{ctl,on}`): **PASS**

Run-end line, ON arm: *20 epoch(s); 589 clock carry fold(s) — **453 ANCHORED** at
q = 3.000e-05, **136 LOOSE**, 0 at the variance floor.  Trips 1 = 1 nis; **1 in-epoch
re-solve (0 failed)**, **32 re-coupling edges**, 1 wire mark.  Largest |w| 12.55 (k = 6.00) on
G07 at TOW 259440.*  Control console: zero Card-Q3 lines.

- **Not inert** (invariant #4): 453 anchored folds.
- **The anchor is EARNED**: 32 re-coupling edges = one per satellite, all at TOW 259320 — the
  5th epoch, i.e. born LOOSE + 4 in-band epochs, exactly the configured re-validation.
- **Both edges exercised in 20 epochs**: `cta_decouple` G07 (|w| 12.55, ê = −1.527 m,
  med = −0.107, √S = 0.113) → in-epoch withdraw + re-solve → `cta_disc_landed` G07 at the SAME
  epoch → **`R,G07` on the wire**; census reports `wire marks: R records=1` against the
  control's `NONE`.  G07 at TOW 259440 is the same satellite and epoch CLK-R's independent
  stage-2 statistic marked in its own 1 h arm.
- **Ordering check** (why this seam precedes within-epoch RAIM): post-fit σ̂₀ code
  4.2707 → 4.2698, prefit H-rider σ̂₀ 4.3269 → 4.3179, RAIM T3 0 opp / 0 flag both arms, 20/20
  emitted, 0 withheld.  No σ̂₀ inflation.

**Defect found and fixed during G1** — `markSatDiscontinuity` alone bumps only the emitter's
in-process counters; on a lineage without `serialize_disc_counters` the per-record disc fields
are not written, so the **R record is the only carrier a `NetworkProductIO` replay can read**,
and the first ON arm produced a live-visible, replay-INVISIBLE mark.  Fixed by folding the
landed set into `retro_emit_sats` (which also gives those satellites the "forced" treatment
CLK-R's frame-hold median needs — a just-refloated satellite must not vote).  Solve untouched;
dev-channel numbers bit-comparable across the fix.  **⚠ OPEN: the three sibling bump queues in
`networkSolPrep` (`eph_cutover`, `cb_excursion`, `basis_relatch`) have the SAME gap and were
left alone — they are marking a wire their own replay cannot see.**

### G2 — 1 h A/B (`runs/cta_1h_{ctl,on}`, one knob + the anchor block): **PASS**

Wire census (`scripts/product_step_census.py`, dev channel — the graded quantity), 121
epochs, 3820 rows, 32 satellites:

| dev channel | ctl (q = 1e-3) | ON (q = 3e-5) | Δ |
|---|---|---|---|
| \|step\| **median** | 6.00 cm | **3.60 cm** | **−40 %** |
| p90 | 18.34 | **9.74** | −47 % |
| p99 | 52.56 | **31.82** | −39 % |
| **robust σ_w** | 8.89 cm | **5.34 cm** | **−40 %** |
| per-sat σ_w median | 9.08 | **5.70** | −37 % |
| epochs with a per-sat outlier > 0.5 m | 27 | **15** | −44 % |
| PB mid-arc steps > 0.1 m | 62 / 29 ep | 66 / 31 ep | +6 % (noise) |

QC: post-fit σ̂₀ code 5.0213 → 5.0886 (+1.3 %), 121/121 emitted both arms, **0 withholds, 0
skipped epochs, 0 within-epoch RAIM re-solves**.  Counters, ON: 3701 folds — **3537 anchored /
164 loose**, 8 trips (all `nis`), 6 in-epoch re-solves, 0 failed, 39 re-coupling edges, 8
marks → 4 R records.

### §3 pass 1 — the binding knee, MEASURED

The `clk` prior-residual family (Card Q3 INC-0), same 121 epochs and 3700 rows:

| arm | armed q | **σ̂₀_clk** | implied self-consistent q = q·σ̂₀² |
|---|---|---|---|
| ctl | 1e-3 | **0.2617** | 6.8e-5 |
| ON | 3e-5 | **0.6977** | 1.5e-5 |

Both < 1 ⇒ **both carries are still looser than the data supports**, and the iteration
converges downward — the fixed point (σ̂₀ = 1) sits at **q ≈ 1.0–1.5e-5 m²/s**.
**q ≈ 1.3e-5 is independently where FC-1's carrier-phase measurement of the PHYSICAL clock
change puts it** (≈ 2 cm per 30 s ⇒ q = 0.02²/30 = 1.3e-5): two instruments sharing no
machinery on the same process noise, and 20× tighter than the §3 desk estimate of 2.8e-4 —
which is why the card requires the knob to be measured, not modelled.

Ladder (|t| = |s|/σ̂₀, the population k is read from): ctl >4σ̂₀ 8.19 %, >12 0.62 %, >20
0.24 %; ON >4σ̂₀ 14.90 %, >12 **0.27 %**, >20 **0.11 %** — the ON tail is ~2× thinner relative
to its own (tighter) scale while its body is fatter, the signature of a scale still slightly
under-estimated.  ⚠ The ladder and the guard's realized trip count do NOT yet map onto each
other (the >8σ̂₀ rung would predict tens of rows/h against 8 observed); the plausible reason is
that the guard *suppresses its own population* — a tripped satellite goes LOOSE and its
next-epoch residual is tested against the much wider loose null — but that is an inference,
and k needs the 12 h window to settle before being set from the ladder.  The census FRAME
label ties out exactly: **8 `cta_refloat` rows = the 8 guard trips**, held out of the σ̂₀
calibration (their null is undefined — the treatment a held phase bias gets).

### Pre-registered prediction that did NOT verify

§1 argued the per-satellite σ_w SPREAD (1.37× across 32 satellites) is the signature of
estimation noise, and G2 pre-registered it should SHRINK as the anchor binds.  Measured:
**3.70× → 8.35×, it WIDENED** — quiet satellites gained most (min σ_w 4.70 → 1.44 cm) while
the noisiest barely moved (17.38 → 12.05).  Consistent with an anchor that floors each
satellite at its own data-limited rate rather than at a common process floor; not predicted,
not yet explained, and it does not touch the headline (level and median both fell 40 %).
§4's withdrawn-prediction note supersedes the expectation; this is the measurement behind it.

### G3 — the 3-arm 3 h ladder (2026-08-10)

Binary `gnss_processor_ctagf.exe` md5 `974714FB7174A8C9CE6FB239C71AB9EF`; arms
`runs/cta3_{ctl,on,gf}`, configs `_cta3_{ctl,on,gf}.json`, one build, one chain, fresh
convergence each, never overlapping.  Suite 1335 pass / 1 skip.  **Provenance, free**:
`cta3_ctl` and `cta3_on` came out **byte-identical** to the stored pre-gauge-free
`runs/cta_3h_{ctl,on}` — one comparison proving the estimator refactor (`denseFoldTerms_` +
the gauge-free branch) is arithmetically neutral on the OFF and raw-state paths, that FC-1
`fast_clock.shadow` is genuinely measure-only, and that the earlier 3 h numbers transfer.

| 3 h, 63 sta, DOY-129 00:00–03:00 | ctl | anchor | anchor + gauge-free |
|---|---|---|---|
| **dev channel** median / robust σ_w | 5.98 / 8.87 cm | **3.55 / 5.26** | 3.81 / 5.65 |
| dev p99 / outlier epochs > 0.5 m | 73.81 / 78 | 50.92 / 48 | **33.57 / 43** |
| **ex-pivot** floor / median / ceiling | 5.66 / 8.54 / 13.81 | **2.36** / 5.28 / **8.55** | 3.25 / **5.24** / 8.63 |
| **PIVOT row** (= drained common mode, a GAUGE) | 18.03 | 11.27 | **21.53** |
| CLK-R Σ\|μ\| drained (independent counter) | 49.43 m | 27.77 m | **79.53 m** |
| post-fit σ̂₀ code | 4.0460 | 4.0323 | **4.0057** |
| withholds / skipped epochs | 0 | 0 | 0 |
| trips (nis / abs / gap) | — | 65 (63/0/2) | 3 (0/1/2) |
| in-epoch re-solves / marks | — | 26 / 65 | 1 / 3 |
| σ̂₀_clk (census) | 0.2803 | 0.7539 | 4.4806 |
| consumer med / p95 / worst | 6.40 / 13.08 / 13.82 | **5.49** / 11.77 / 12.90 | 6.64 / **9.13** / **9.46** |
| AR fixed | 0 | 0 | 0 |

**The §2.1 mechanism is PROVEN, with the corrected sign.**  Σ|μ| 27.77 → **79.53 m**, above the
control's 49.43 (the control's legacy carry still loads the gauge slightly; the gauge-free fold
loads it exactly zero), census pivot row in lock-step (11.27 → 21.53).  10 922 keys downdated,
**0 inert epochs**.

**The estimable side is a TRADE, not a win** — gauge-free buys the tail and pays in the body
across three instruments: dev p99 50.92 → **33.57** (−34 %) and outlier epochs 48 → 43, but dev
median 3.55 → 3.81 and ex-pivot floor 2.36 → 3.25; consumer worst 12.90 → **9.46** and p95
11.77 → **9.13** (both best of the three arms) but median 5.49 → 6.64 and hour 0 3.00 → 5.42 (a
cold start converges more slowly with strictly less prior information); σ̂₀ code monotone best
(4.0460 → 4.0323 → **4.0057**), the predicted §2.1 signature.

**Trips collapse 65 → 3 without a fatter tail** (the gauge-free p99 is the smallest of the
three, which a merely desensitized guard could not do).  The raw-state arm's 63 `nis` trips have
|ê| median **0.28 m** (max 1.42 m) against a mean √S of **3.3 cm** — sub-metre residuals made
significant by a denominator smaller than the arm's own realized dispersion (5.26 cm) — and
**0 of 63 would have been caught by the 2.0 m absolute backstop**, so the raw-state guard was
substantially firing on the walk's tail, not on metre-class clock events.

**⚠ CONFOUND: the gauge-free arm changed TWO things** — the fold AND the guard's denominator
(§2.3's matched common-removed posterior), which is correct for that fold but independently
desensitizing; this ladder cannot apportion the 65 → 3 collapse.  **Owed: a
`clock_anchor_gauge_free` arm with the statistic forced to the raw-state denominator.**
*(RESOLVED in §8.6, and it INVERTS this reading: the fold alone drives trips 65 → 109; the whole
collapse is the denominator.)*

**⚠ Instrument defect found: `σ̂₀_clk` is not gauge-aware** — 0.2803 → 0.7539 → **4.4806**.  The
census still standardizes by `last_var_eff`, the per-satellite RAW anchor variance, while the
gauge-free fold carries strictly less information than that (by exactly the gauge direction), so
`disp` under-states and σ̂₀ blows up.  The knee readings (§3) stand for the ctl/anchor arms and
**must not be read across the gauge-free arm** until the census's fold_var is gauge-aware.

### The FC-1 shadow hour table — the score, and it says NO

`scripts/q3_fc1_hours.py`, cross-validated against `scripts/fc1_grade.py shadow` (SD-RMS
agrees to 3 decimals: 21.723 / 27.114 cm):

| 3 h, common-removed | ctl | anchor | gauge-free |
|---|---|---|---|
| producer, robust σ (**the body**) | 8.83 cm | **5.25** | 5.60 |
| producer RMS, declared marks removed | 21.72 | 15.83 | **13.93** |
| producer RMS, raw | 21.72 | 27.11 | **19.85** |
| phase-measured (TDCP) | 0.30 | 0.30 | 0.30 |
| **corr(producer, phase)** | −0.004 | **−0.004** | **−0.005** |
| ratio phase / producer | 0.014 | 0.011 | 0.015 |

**Not one arm moves the correlation off zero**, so the pre-registered FC-1 gate (*"a
successful anchor moves producer Δc0 toward the phase column AND lifts the correlation off
zero"*) is **NOT met**.  Q3 at q = 3e-5 makes the emitted clock **quieter, not more real**:
the body falls 41 %, the marks-removed RMS 27–36 %, and the information fraction stays at
~1–1.5 %.  Two reusable notes:

1. **An RMS is the wrong statistic for a guarded arm.**  Raw SD-RMS gets WORSE with the
   anchor (21.72 → 27.11) purely because the guard's 65 marked refloat steps are large
   one-epoch changes BY DESIGN and RMS is tail-dominated — while every satellite in the
   grader's own worst-5 table improved (G22 16.1 → 13.9, G28 14.7 → 11.5, G29 13.6 → 9.6, G18
   12.8 → 9.4).  A declared discontinuity is not an error the consumer integrates; it is one
   it is told to reset at.  `q3_fc1_hours.py` reports the raw RMS, the robust body AND the
   marks-removed RMS — none of the three is quotable alone.
2. **~~The physical floor on this lineage is an OPEN NUMBER~~ — RESOLVED in §7.**  The TDCP
   column measures 0.30 cm per 30 s here, 6× smaller than the 1.7–2.0 cm
   `project_fast_clock_card` records; §7 settles it — **both are MEASUREMENT FLOORS**, and the
   accumulating physics is q_phys ≈ 2.9e-8 m²/s (0.94 mm/30 s).

### Not yet run

- **G4 — the DOY-129 20:47 event-window replay**, still OWED.  The guard has fired on real
  data (65 trips incl. 2 genuine staleness `gap` trips at TOW 262950 / 267750, on satellites
  whose carry had aged past 120 s) but never on a KNOWN event with a known answer.
  **Quiet-day cleanliness is explicitly not a pass.**
- The 12 h leg, and the comparison against the `runs/clkr_12h_hold` denominator: a 3 h window
  cannot settle the LEVEL channel (~2 h).  `total_IF` is likewise not graded here.
- The gauge-free bisection and the gauge-aware `σ̂₀_clk`, both named above.

---

## §6 Relationship to the surrounding cards

- **CLK-R** (`emit_clock_frame_hold`, promoted) holds the clock's *gauge* at emission; Q3 is
  the *content* half.  CLK-R makes the per-sat channel readable, Q3 makes it quiet.  CLK-R
  stage 2's per-sat marks stay OFF; Q3's marks are a different population (guard trips, not
  census steps).
- **C1 / FC-1**: both measured NET-HARMFUL because they predict a producer-internal walk.  Q3
  is the named prerequisite of both; FC-1's shadow instrument is Q3's scoreboard (G3).
- **`network.clock_model`**: stays NO-GO.  Q3 deliberately does NOT build on the two-state
  carry — its data-incest defect (posterior fed back as a measurement with fixed R) is
  unfixed, its RWFM premise falsified (p ≈ 1), and `service_mode` force-arms it while silently
  making `sat_clock_q` a no-op.  Q3 uses the SCALAR carry and only borrows the two-state
  path's B3-corrected innovation VARIANCE, a general identity.
- **Odijk et al. 2016**: Q3 implements the paper's assumed random-walk clock constraint at a
  defensible q — the one row of the realization contract no S-transformation can fix, because
  it is a property of the estimable content, not of the basis.

