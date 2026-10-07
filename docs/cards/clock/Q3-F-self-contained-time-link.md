# Q3-F: the self-contained clock time-link on the leveling vehicle

**STATUS**

- **id:** Q3-F
- **topic:** clock
- **status:** closed
- **summary:** The Q3 clock anchor at the physical floor (q 2.9e-8) PASSES the frozen code-side bars with the campaign's best consumer tail (0.954 m); the sim's q 3e-6 fails all three.
- **dates in the text:** 2026-09-14
- **knobs / posture:** the sim golden's 12-key clock-anchor family on base `_pv_b.json`: `clock_temporal_anchor: true`, `clock_anchor_q` 3e-6 (c8) or 2.9e-8 (c8b), `clock_anchor_max_gap_s` 120, `clock_anchor_k` 6, `clock_anchor_abs_m` 2, `clock_anchor_recouple_epochs` 4, `clock_anchor_mark`, `clock_anchor_resolve`, `clock_anchor_var_floor_m2` 1e-5, `clock_anchor_sigma_floor_m` 0.0018, `clock_anchor_gauge_free: false`, `clock_anchor_gauge_free_stat: proj`; c8c = c8 + the c3 external pin
- **hashes named in the text** (commit / exe / byte-cert): `63a2a4a` (R6 worker commit), `45ac4a3` (R7 worker commit), `6282c55` (Q3-F worker commit, merged as `95caa8e`); exe `4CD7D664…`
- **run dirs named in the text:** none as `runs/` paths; arms PV-B (control), c3, c8, c8b, c8c
- **source:** `docs/archive/SPEC_event_harness.md` §24.16 (with the §24.15 R6 and R7 results)

**Outcome.** The question: does the sim golden's clock-anchor family on the leveling vehicle reproduce the external pin's code side? At q 2.9e-8 the variance floor binds on 4575 of 7064 folds and the bars pass (code rms p50 1.09 m, station max 3.46 m, 8 stations). The free per-epoch satellite clock IS the second code-side contributor; the wire's within-arc walk is NOT the clock's. Closed by adoption into the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) frozen posture and the goldens ([RF-3-prep](../../refound/RF-3-prep_golden_rebase_2026-09-15.md)); G4's field half on this posture is the field-lineage blocker named in [Q3 G4](../clock/Q3-G4-SIM-HALF.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.16 Card Q3-F — the self-contained clock time-link on the leveling vehicle (pre-registered before any arm is read)

Base `_pv_b.json`, control PV-B, the CE exe: **c8** = the sim golden's clock-anchor family transplanted verbatim (`clock_temporal_anchor:
true`, `clock_anchor_q: 3e-6`, `clock_anchor_max_gap_s` 120, `clock_anchor_k` 6, `clock_anchor_abs_m` 2, `clock_anchor_recouple_epochs` 4,
`clock_anchor_mark`, `clock_anchor_resolve`, `clock_anchor_var_floor_m2` 1e-5, `clock_anchor_sigma_floor_m` 0.0018,
`clock_anchor_gauge_free: false`, `clock_anchor_gauge_free_stat: proj`); **c8b** = the same with `clock_anchor_q: 2.9e-8` (the physical
floor); **c8c** = c8 + c3 (the temporal link beside the external pin — additive, or the same information?).  Reads (i)–(v), the consumer,
and the anchor's OWN counters (marks / resolves / re-couplings / gap breaks — the guard family is invariant #1's wired response and must be
live, not silent).  Bars: §24.8's, the same rules against PV-B.  **Frozen reading:** c8 or c8b reproduces c3's code side ((i) ≤ 1.2 m, max ≤
3.5 m, (ii) ≤ 8) ⇒ the free per-epoch satellite clock IS the second code-side contributor and the time-link is its self-contained fix — the
posture card becomes the leveling vehicle + the Q3 family, and G4 (the DOY-129 event window) is graded on it next; neither does while c8c
does ⇒ the gain needs the external LEVEL, which the producer cannot have — the code side is then bounded by the free clock and the card
moves to the wire's phase side (R6); a divergence or a silent guard ⇒ recorded, the family not armed.

**§24.15 R6 RESULTS (2026-09-14 ~04:30 local; worker commit `63a2a4a`, merged; PV-B traces only).**  None of the four classifications
fires: the share of within-arc D_phase energy within ± 2 epochs of a declared boundary or election is 0.28–0.53 for the noisy four against
a band that already covers 36–49 % of the rows by construction (ENRICH 0.72–1.16 — the noisiest three are DEPLETED at the boundaries); a
per-arc linear drift takes 0.13–0.32; the coverage correlation peaks at |r| 0.043 with the wire QUIETER on set-change epochs for five of
six; and no satellite is near eclipse (closest approach 2.1 R⊕, |β| 22–68°, from COD MGX SP3 with the Earth-fixed velocity corrected).  For
G29 / G28 / G32 the largest term is the unexplained remainder (0.39–0.52).  Two structural facts: **G32 churns its anchor five times in
24 min with three successors below 20°** (and is the one satellite louder on set-change epochs, 1.64× on 13 pairs); **G18 split-references
— its signal families elect DIFFERENT anchor stations in the same epoch at 2 of its 3 election epochs** (k 170: GCGO → CHUR and GCGO → CPVG;
k 191: REYK → BOGT and REYK → CHUR).  With R5's DJIG election these point at the coverage-only election rule, not at a per-satellite record.

**R7 (pre-registered, same worker, PV-B traces): does the wire follow its anchor's code level?**  The satellite phase bias on this vehicle
is one station's phase level (the gauge "pinned at the highest-coverage receiver tracking each satellite"), and R1 showed the per-LOS code
level GROWS over an arc for 74–77 % of arcs — so the hypothesis: the within-arc D_phase wander is the anchor station's own code-level
evolution (multipath / group-delay drift with elevation) carried onto the wire.  Read: per satellite and declared arc (≥ 20 epochs), the
Pearson r and slope of D_phase(t) against (A) the anchor station's per-LOS code residual level series for that satellite (pf_rows,
signal-pooled and C1) and (B) the tracking-set mean of the same, at matched epochs; pooled r weighted by arc length; the anchor's
elevation per epoch and r(within-arc D rms, anchor elevation) across arcs; G32's five anchors read individually.  **Frozen reading:**
r(A) > 0.5 and r(A) > r(B) ⇒ the wire follows its anchor's code level — the phase-datum REALIZATION is the wire defect (a one-station,
coverage-elected, code-tracking gauge), and the card is a multi-station gauge (or a minimum anchor age + elevation with the level
decoupled from the code after convergence) — a design card; r(B) > r(A) > 0.5 ⇒ the level tracks the code network-wide by construction (a
leveling-memory posture knob); both < 0.3 ⇒ the wander is not code-driven, and the per-arc spectrum (white vs random walk) is recorded.

**§24.15 R7 RESULTS (2026-09-14 ~05:15 local; worker commit `45ac4a3`, merged; PV-B, all 32 satellites, 295 declared arcs ≥ 20 epochs).**
**The third branch applies — the wire's within-arc wander is NOT code-driven and NOT the anchor's.**  Pooled r(D_phase, the anchor
station's own per-LOS code level) = **0.079** (per-signal 0.095, C1 0.094); r against the tracking-set mean = −0.010; the noisy four run
r(A) −0.17 … +0.17 and r(B) −0.19 … +0.09; |r(A)| 0.307 against a wrong-satellite NULL of 0.310 (gap −0.003 — the raw correlation measures
arc length and autocorrelation, not the anchor); within-arc D rms against the anchor's elevation across arcs: Pearson −0.017, Spearman
−0.007; one satellite of 23 (G21, r(A) 0.534 with r(B) −0.375) matches the "follows its anchor" branch.  **G32 refutes the hypothesis in its
sharpest form:** through its five anchors in 24 min at 16–28° its D rms was 0.06–0.17 m; on one 49.9° anchor (INEG00MEX) held for 152
epochs it produced D rms 1.79 m with r(A) −0.07.  **What the arcs are instead is a random walk or steeper:** on the 67 arcs where both
correlations fail, σ²(lag 10) / σ²(lag 1) = p10 8.4 / p50 16.6 / p90 77 (white ≈ 1, random walk ≈ 10, drift ≈ 100), 100 % above 5.  With
R6 (not at declared boundaries, not a fitted drift, not coverage, not eclipse) the remaining candidate is a stochastic satellite-side
within-arc process — the free satellite clock's own random walk (`filter.process_noise.sat_clock_q` 0.001 m²/s ≈ 0.17 m per 30 s step on
the field golden, against a physical floor of 2.9e-8) absorbing whatever the phase-consistent combination lets it, which is exactly
what card Q3-F (§24.16, the temporal clock anchor at q 3e-6 and 2.9e-8) is running now; if D_phase collapses under c8 / c8b the wander was
the clock's; if it stays, it lives in the phase-bias / level channel and the next read targets that state's process noise and carry.
The anchor-election defects of R5 / R6 (coverage-only rule, split-referencing families, newborn low anchors) remain OWED cards on their own
evidence, off the bars.

**§24.16 RESULTS (2026-09-14 ~05:45 local; worker commit `6282c55`, merged 95caa8e; exe `4CD7D664…`; manager grade).**  The sim golden's
12-key clock-anchor family transplants without a refusal and is LIVE on the field (run-end: 7064–7092 ANCHORED folds of 7327, 28–35 trips
split across the `nis` detector and one `gap` staleness trip, 59–66 re-coupling edges, 28–35 wire marks; at q 2.9e-8 the variance floor
binds on 4575 of 7064 folds — c8b runs at the floor, not its nominal q).  All three arms cold, 240/242, 0 withheld, vehicle lines = PV-B.

| arm | (i) code rms p50 / p90 / max m | (ii) n (worst) | (iii) D_phase / D_code / clock LEVEL m | level p99 below / above 25° | consumer p50 tail (at 2 h) |
|---|---|---|---|---|---|
| PV-B | 1.79 / 3.28 / 7.08 | 13 | 0.95 / 0.13 / 0.74 | 6.34 / 3.29 | 1.187 (0.546) |
| c3 external pin σ 0.02 | 1.08 / 1.97 / 2.86 | 7 | 0.82 / 0.12 / 0.04 | 2.58 / 2.02 | 1.343 (0.744) |
| c8 Q3 q 3e-6 | 1.71 / 3.34 / 5.17 | 13 (GUAM +2.5, GOPE +1.9, GRAZ +1.1) | 0.90 / 0.11 / 0.58 | 3.40 / 3.21 | 1.847 (1.719) |
| **c8b Q3 q 2.9e-8** | **1.09 / 2.12 / 3.46** | **8** (GOPE +2.1, GRAZ +1.2, LCK4 +0.9) | 0.87 / 0.10 / 0.60 | 2.42 / 2.18 | **0.954 (0.188)** |
| c8c Q3 + pin | 1.07 / 1.96 / 2.82 | 7 (GOPE +1.9, GRAZ +1.1, BOGT −1.0) | **0.78** / 0.11 / 0.04 | 2.58 / 2.04 | 1.484 (0.891) |

**Grade against §24.16's frozen thresholds ((i) ≤ 1.2 m, station max ≤ 3.5 m, (ii) ≤ 8): c8b PASSES all three (1.09 / 3.46 / 8), c8c passes
(1.07 / 2.82 / 7), c8 fails all three (1.71 / 5.17 / 13).**  The first frozen reading applies: **the free per-epoch satellite clock IS the
second code-side contributor, and the guarded temporal clock anchor at the PHYSICAL floor is its self-contained fix** — it reproduces the
external pin's code side to 0.01 m on (i) and 0.16 m on the level p99, gives the best consumer tail of the whole campaign (0.954 m
settled, 0.19 m at the 2 h mark), and is neutral-to-better on every σ̂₀ instrument (block-Baarda 1.375 → 1.165, prires rxcb 1.086 →
0.869).  The sim's own q (3e-6) does NOT transfer: the field clock needs the floor, two orders tighter, and at 3e-6 the instruments get
louder (prires cb 3.12 → 3.70).  c8c shows the two anchors are complementary, not additive (+0.02 on (i), +1 station, D_phase 0.87 →
0.78 with the LEVEL contrast collapsed to 0.04) at the consumer's expense (0.954 → 1.484).  **The wire's within-arc random walk (R7) is NOT
the clock's** — D_phase moves 0.95 → 0.87 only — so it lives in the phase-bias / level channel, the one channel this campaign has not
opened; it is the remaining lever between 0.87 m and the 0.5 m bar.  Under §24.8's absolute bars nothing crosses yet ((i) 1.09 vs 1.0,
(ii) 8 vs 0, (iii) 0.87 vs 0.5).

