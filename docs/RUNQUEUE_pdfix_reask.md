# RUN QUEUE — re-asks on the working fold (2026-08-03)

**STATUS (2026-08-03 eve).** P0 ✅ (invariant #6 passes at 3 h). P1 ✅ winner = `v3`
(`carry_invariant_innov_corroborate`; lifetime 12.0 → 55.3 ep, D5 precision ~2 % →
68 %). P2 ✅ — q_z IS the DD walk but zeroing it is a CONSERVATION, not a win (fails
clock QC). P3 ✅ — complete null, reproduces. **P3b ✅ (added) — deg15 an expensive null, rep15 a FAIL with the documented aliasing signature; the iono representation axis is CLOSED in both directions.** **P4 ✅ P5 ✅ P6 ✅ — THE QUEUE IS COMPLETE.** P6: the +12.4 % total_IF was an ignition
transient (+2.7 % by hr3) but med|frac| stays uniform at 6 h. P5: both lane arms
net-harmful, "do NOT arm" survives. P4: the giant-K fear retired (fold FRAGMENTS and
gets healthier), but a 4-epoch S-basis null-space failure surfaced.
**Instrument note for every arm from here**: the exe must carry the σ̂₀ CLAMP
VISIBILITY fix (`docs/SPEC_pd_safe_dd_fold.md` §11). Any run-end `σ̂₀ = 1.00×` that
does NOT also print a state histogram is from a pre-fix exe and means "unknown,
probably floored" — on the corrupt substrate the clamp bound on 94.7 % of installs.

Every verdict below was previously graded on the corrupt-fold substrate
(docs/SPEC_pd_safe_dd_fold.md §1) and is therefore OPEN again. Common base for
ALL arms: `_ft63_churn_base.json` + `carry_invariant_pd_safe: true`, cold, 3 h,
one variable per arm, smoke-first, Release renamed exe, fresh convergence
(pd_safe is FINGERPRINTED — no warm resume across the flip). Grade with:
`parse_carry_psd.py` (PSD + diffuse census), `grade_dd_spillway.py` (steps-by-age
+ structure function + cross-band), `chain_stability.py` (total_IF hr2 + clk),
epoch-status counters BY NAME (births / refloats / d5_flags / ddfold_*).
`dd_sigma_cyc` stays carry-blind — do not grade on it.

Reference: `runs/pdfix_on` (D5 v2, dual-layer SH + mech-B v3k, qz 1e-4):
SF k=1 0.92 / k=32 31.5 cyc, mature steps 0.85-1.0, D5 ramp 15.6→89.9/ep,
σ̂₀ 4.05, total_IF hr2 8.812/22.14/5.00.

## P0 — the gate (BLOCKS everything; run first)
- **pdfix_off 3h** (`_ft63_churn_base` on the pdfix exe, knob absent).
  Invariant-#6: solve artifacts md5-identical to `runs/churn_base2`. Also THE
  same-build control — no cross-build claim (incl. pdfix_on's +12.4 % total_IF)
  is legal until this passes.

## P1 — D5 policy matrix (decides the SUBSTRATE for everything below)
D5 v2 is now the maturation cap (re-tap equilibrium ~9 ep late-run). One arm
each, vs pdfix_on as control:
- **pdfix_v5b**: + `carry_invariant_innov_carried_only` — kills the born half
  of the loop. Expect flags↓, births↓; does the ramp flatten?
- **pdfix_v3**: + `carry_invariant_innov_corroborate` — suppresses
  measured-and-flat tension flags (96.5 % of the population): chords mature
  through iono tension. THE high-stakes arm: watch where the undischarged
  tension lands (total_IF, a00, postfit σ̂0_phase) — invariant #1 in real time.
- (optional) **pdfix_v4**: + `retap_only` — middle ground, re-tap without
  refloat.
Metric: chord informational lifetime (flags/ep vs pop), mature steps, SF knee,
total_IF. Winner becomes the base posture for P2-P6.

## P2 — the q_z axis (the original "time-constant ambiguities" question)
Configs exist (`_ft63_qz0*.json`) — add the pd_safe knob + P1 winner:
- **pdfix_qz0**: `carry_invariant_qz: 0`. Prediction: mature steps → sub-0.3,
  SF flattens; D5/tension behavior is the open question. Re-measures the
  POSTURE ledger's "1e-4 beats zero-q" (corrupt-substrate evidence).
- **pdfix_qz0_iofloat**: + ties released (σ_rep 50, mech-B triple off,
  layer2 0, sh_health/deweight-adaptive off) — the uncombined-PPP-RTK cell.

## P3 — iono representation ladder (slant vs SH vs dual-layer, one axis each)
Base lineage IS dual-layer + mech-B v3k, so the ladder is subtractive:
- **pdfix_l2off**: `layer2_max_degree: 0` (single-shell SH). The old l2off
  null (±0.1 %) was corrupt-substrate — does layer 2 matter on a working carry?
- **pdfix_mechBoff**: `rep_error_tau_s: 0` + demean/d5_keep false (SH only,
  no per-LOS deviation carry). Does mech-B finally reduce the SF long-lag walk?
- **pdfix_iofloat**: σ_rep 50 + both off, qz UNCHANGED 1e-4 (pure slant-float
  cell, separates representation from the q_z axis).
Metric: SF k=16/32 (the spillway walk), equatorial-pair step ranking, a00
band, total_IF.

## P3b — iono representation, the ADDITIVE direction (added 2026-08-03)
P3 above is SUBTRACTIVE and came back a complete null. P2 explains why it had to:
at q_z = 1e-4 the carried-DD walk IS q_z (mature steps 0.953/0.789/0.485, SF still
growing to k=32), so it swamps any representation term. **These arms therefore run
on the `pdfix_qz0` substrate**, where the walk collapses to 0.168/0.096/0.030 and
the SF flattens (k=32: 21.15 → 1.51) — the only floor against which an additive
iono change is visible.
- **pdfix_qz0_deg15**: `estimation.global_vtec_sh.max_degree` 8 → 15. Outside the
  POSTURE ledger's deg-15 envelope on purpose: that entry needs n_sta ≥ 150 (this
  is 63) though it does meet the ≥ 25° iono cutoff. 256 layer-1 coefficients
  against ~500-600 LOS/epoch ≈ 2 LOS per coefficient.
- **pdfix_qz0_rep15**: `representation_sigma_tecu` 5.0 → 1.5 — the untested half of
  the axis (P3 ran 5 → 50). ONE key, TWO mechanisms: it sets both the SH↔slant pin
  σ and the mech-B GM channel's steady-state σ. Runs at a documented hazard ("too
  tight aliases unrepresentable small-scale TEC into huge low-degree coefficients").

Control = `runs/pdfix_qz0`: mature steps **0.168 / 0.096 / 0.030** cyc/ep,
med|frac| 0.253/0.252/0.228, SF k=1 0.09 / k=32 1.51, cross-band **1.265**,
total_IF hr2 spread_med **9.521** / worst_med 25.35 / clk_std **5.29**, a00 32.57
TECU (L1 30.35 + L2 2.22), iono-deweight 11 sta ever / peak 8 / 12 engage.

**qz0 IS A DIAGNOSTIC SUBSTRATE, NOT A POSTURE** — P2 graded it a FAIL on the
card's own clock-QC rule. A win here is a lead that must be re-run on the q_z=1e-4
posture (control `runs/pdfix_v3`) before it can be promoted.

**THE DISCRIMINATOR IS THE CROSS-BAND RATIO, NOT THE STEP SIZE.** Every arm of
P2/P3 including qz0 sits at 1.263-1.265 against the common-cycle signature 1.2833:
the residual's DIRECTION has never changed, only its magnitude. An iono
representation win must MOVE that ratio. Steps that tighten without moving it mean
the residual is unchanged in kind and something else got quieter.

### P3b RESULT (2026-08-03) — **deg15 = expensive null, rep15 = FAIL. Axis CLOSED.**

| metric (3 h, 63 sta) | control `pdfix_qz0` | `deg15` | `rep15` |
|---|---|---|---|
| SH coefficients | 81 + 25 | **256 + 25** | 81 + 25 |
| mature steps 25-50 / 50-100 / 100+ | 0.168 / 0.096 / 0.030 | 0.167 / 0.096 / 0.030 | **0.185** / 0.091 / 0.030 |
| SF k=16 / 32 / 64 / 128 | 1.06 / 1.51 / 1.03 / 0.91 | 1.05 / 1.51 / 1.00 / 0.84 | **1.13 / 1.62 / 1.43 / 1.18** |
| **cross-band ratio** | **1.265** | **1.265** | **1.265** |
| med\|frac\| 100+ | 0.228 | 0.233 | **0.300** |
| med σ 25-50 / 50-100 (cyc) | 5.434 / 5.007 | 5.462 / 5.020 | **5.108 / 4.610** |
| total_IF hr2 spread / worst_med / worst_max | 9.521 / 25.35 / 29.14 | 9.527 / 25.54 / 29.98 | 9.789 / 26.45 / **38.52** |
| total_IF excursions | none | none | **1 ep, 40.15 m on G07** |
| clk_std hr2 | 5.29 | 5.17 | 5.28 |
| convergence ratio | 0.688 | 0.682 | 0.751 |
| a00 (layer1 + layer2) | 32.57 (30.35 + 2.22) | 31.16 (28.75 + 2.41) | **37.88 (37.09 + 0.79)** |
| post-fit σ̂₀ code | 4.0125 | 4.0257 | 3.9968 |
| PD gate (`parse_carry_psd.py`) | PASS | PASS | PASS |
| perf | 1743 ms/ep | **2342 (+34.4 %)** | 1828 (+4.9 %) |

**deg15**: 3.2× the layer-1 coefficients, every DD and product statistic inside
noise, +34.4 % wall. No aliasing (SH health never left band; deweight 11 sta /
peak 9 vs 11 / peak 8). Degree 15 simply buys nothing at 63 stations, which is
consistent with — not a refutation of — the POSTURE ledger's n_sta ≥ 150 condition.

**rep15**: the documented hazard, observed. The doxygen warns that too tight a
representation prior "aliases unrepresentable small-scale TEC into huge low-degree
coefficients", and that is exactly the signature: **a00 layer-1 30.35 → 37.09 TECU
(+22 %) while layer 2 collapses 2.22 → 0.79** — power migrated INTO the lowest
coefficient. The long-lag structure function grew at every lag ≥ 8 (k=64 +39 %,
k=128 +30 %), med|frac| on the oldest bin moved AWAY from zero (0.228 → 0.300), and
a total_IF excursion appeared where the control had none. Note the trap: the FORMAL
σ FELL 6-8 % (5.434 → 5.108) — the tightened prior makes the solution look more
confident while the actual DD walk gets worse. Grade the walk, never the σ.
**Confound resolved without a new arm**: `representation_sigma_tecu` sets both the
pin σ AND the mech-B GM steady-state σ, so rep15 tightened the pin and shrank the
deviation channel together — but P3's `mechBoff` arm (channel removed entirely) was
a null, so the harm is attributable to the PIN, not the mech-B side.

**THE AXIS IS NOW CLOSED IN BOTH DIRECTIONS.** P3 subtractive (layer2 off, mech-B
off, σ_rep 5 → 50) = null; P3b additive (degree 8 → 15, σ_rep 5 → 1.5) = null and
harm. And the cross-band ratio has read **1.263-1.265 in all seven arms** — q_z 1e-4,
qz0, layer2-off, mechB-off, σ_rep 50, σ_rep 1.5, degree 15 — against common-cycle
1.2833 and iono 1.6469. Degree, layer count and prior strength are all
SPATIAL-SPECTRAL levers on a thin-shell VTEC surface; the residual does not respond
to any of them in either direction, so **it is not a deficiency of the surface's
flexibility** and the next lever is not on this axis. Do not re-run the SH
representation ladder on this regime without a new mechanism to test.

## P4 — constellation: GPS-only vs GPS+Galileo
- **pdfix_gg 3h**: the ft63 GPS+Gal config family (gg12h base, trimmed to 3 h)
  + pd_safe. Tests the Aug-2 S-basis completion under a working carry; doubles
  the DD pool and iono sampling. Control = pdfix_on (GPS-only).
  Watch: giant-block K growth (pair block will exceed 686 — pivot trajectory
  + fold cost), datum/SBC health, per-constellation total_IF.

## P5 — EWL / WL integer re-asks (gated on P1 winner)
- **pdfix_sdewl**: `network.sd_ewl.fix_enable` — the SD-EWL integer hold
  (built, gate-3 field verdict still owed; channel-consensus yield may be low
  on GPS-only — pair with P4 if starved). Grade per its own SPEC: fixed-group
  CB/PB redistribution, no datum-only fixes.
- **pdfix_mwwl**: the MW/WL conditioning tiers ("do NOT arm" verdict =
  corrupt-substrate evidence). One arm, WL lane tier only, vs P1 winner.
Metric: WL DD step (already 0.32 cyc/ep — should tighten), NL frac circR.

## P6 — the payoff leg
- **pdfix_best 6h** (P1 winner + best of P2/P3): (a) does total_IF converge
  past the control after the slow ignition (hr2 was still falling); (b) the
  first honest med|frac| / circR trend check on matured chords — the "does
  integer information EXIST at this cadence/geometry" question, finally on a
  substrate that can answer it.

### P6 RESULT (2026-08-03) — `runs/pdfix_best6h`, 720 epochs, 0 skipped, 0 withheld

**The fold holds for 720 epochs**: 0 degenerate, 0 repairs, 0 rebirths, worst
relative pivot +7.03e-09. INC-1c and INC-2a stay field-UNEXERCISED at 6 h too.
σ̂₀ `active` 718/720 (post-run 3.38×). D5 713 flags / 713 re-taps over 6 h vs 307
over 3 h — the ramp stays flat, so v3's suppression holds at double the duration.
Determinism check: hr0-hr2 total_IF is IDENTICAL to `runs/pdfix_v3` to 3 decimals.

**(a) Does total_IF converge past the control?  Not past — TO parity, and the
+12.4 % is an IGNITION TRANSIENT.**  Per-hour total_IF spread_med against the
pre-fix 24 h lineage (`runs/ft63_24h_conv`; note it differs in TWO knobs, pd_safe
AND corroborate, so it is a lineage control, not a single-variable one):

| hr | P6 (working fold) | 24 h pre-fix | Δ |
|---|---|---|---|
| 0 | 14.042 | 10.602 | +32.4 % |
| 1 | 11.064 | 7.975 | +38.7 % |
| 2 | 8.779 | 7.871 | +11.5 % |
| 3 | 6.968 | 6.782 | **+2.7 %** |
| 4 | **5.192** | 4.891 | +6.2 % |
| 5 | 11.778 | 12.225 | −3.7 % |

The cost decays monotonically to inside hour-to-hour noise by hr3 and the absolute
level keeps falling to **5.192 m at hr4** — better than any 3 h number on either
substrate. So the SPEC's open +12.4 % item resolves as **a transient of the slower,
more honest ignition, not a standing cost**. It is NOT a crossover claim: hr5's
−3.7 % is contaminated (below).

**hr5 IS A DOY-129 DATA EVENT, NOT A FOLD ARTIFACT — and the 24 h run proves it.**
Both substrates show the SAME hr4→hr5 signature: hr4 bimodal (spread_med 5.192 vs
p90 13.174 here; 4.891 vs 12.472 there), hr5 spread ~12 m, **nsat 32→30**, and the
worst satellite switching to **G02** in both. On the 24 h run it recovers (hr6
10.834, hr7 8.622), so it is a transient of the constellation/geometry at
05:00-06:00 UTC, present with a frozen carry and with a working one alike. Station
drops cluster there (25 in hr5 vs 2-13 elsewhere, KIT300UZB 34 of 63 run-total) but
at 0.3 % of station-epochs they read as a co-symptom, not the cause. **Do not spend
a card on hr5 from this lineage — it needs its own, and it is not pd_safe's.**

**(b) Does integer information appear on matured chords?  NO — the clearest
negative yet.**  med|frac| by hour: **0.246 / 0.254 / 0.249 / 0.253 / 0.251 /
0.250** — dead flat at the uniform-distribution value for all six hours, and
0.253 / 0.248 / 0.256 across the mature age bins (25-50 / 50-100 / 100+). Six
hours, a working fold, 55-epoch chord lifetimes, and the fractional part is still
indistinguishable from uniform. The cross-band ratio in the same run is **1.262**.
This is now the strongest available statement that **the DD integer information is
not present at this cadence and geometry** — it is no longer attributable to the
corrupt substrate, to churn, to chord lifetime, or to run length.

### P5 RESULT (2026-08-03) — **both lane arms NET-HARMFUL; "do NOT arm" SURVIVES the re-ask**

Control `runs/pdfix_v3`. Both arms were re-pointed at the P1 winner first (they had been
written against `pdfix_on`, which would have made each two variables at once).

| metric | v3 | `sdewl` | `mwwl` |
|---|---|---|---|
| mature steps 25-50 / 50-100 / 100+ | 0.953 / 0.789 / 0.485 | **2.244 / 1.698 / 0.954** | **2.037 / 1.634 / 0.950** |
| SF k=1 / k=32 | 0.77 / 21.15 | 1.66 / 35.06 | 1.63 / 27.55 |
| cross-band ratio | 1.263 | **1.328** | **1.308** |
| total_IF hr2 spread / worst_med | 8.779 / 21.78 | 9.365 / 22.07 | 9.048 / 22.03 |
| clk_std hr2 | 4.86 | **5.58 (+14.8 %)** | 5.06 (+4.1 %) |
| total_IF excursions | none | **51 ep / 25.5 min, peak 44.65 m G12** | none |
| post-fit σ̂₀ code / phase | 3.5643 / 0.1707 | 4.3269 / 0.6145 | 3.8084 / 0.2132 |
| med\|frac\| mature | 0.248 / 0.249 / 0.261 | 0.255 / 0.235 / 0.248 | 0.255 / 0.252 / 0.248 |
| D5 flags | 307 | 350 | 314 |
| perf | 1819 ms/ep | 1824 | 1830 |

Both roughly DOUBLE the mature DD walk. `sdewl` additionally trips the card's stated
fail condition (clock-QC degradation + a 25-minute excursion the control does not have),
and it does so despite a healthy-looking lane yield: 10 828 EWL rows INTEGER-FIXED via
FFR at mean R = 0.0001 cyc². `mwwl` is milder — no excursion, +4.1 % clk_std — but still
net-harmful, folding 58 063 WL rows at σ ≈ 0.097 cyc to buy a 2× worse walk. Neither
moves med|frac| off 0.25. **The `project_mwwl_conditioning_day129` verdict was flagged as
corrupt-substrate evidence; it is now re-established on a working fold.**

**The methodological result is bigger than the verdict: THE CROSS-BAND RATIO MOVED.**
1.263 → 1.328 (`sdewl`, toward the L5Q common-cycle 1.3391) and → 1.308 (`mwwl`). First
movement in nine arms — and it moved because the lanes inject L5-referenced structure
into the DDs, not because iono was absorbed. So the statistic is NOT insensitive, and its
invariance at 1.262-1.265 across the whole iono ladder (P3 + P3b) is a real finding rather
than a dead instrument. Keep using it as the discriminator.

### P4 RESULT (2026-08-03) — **the giant-K risk did NOT materialize; a DATUM defect did**

`runs/pdfix_gg`, 63 sta GPS+Galileo, 3 h, 8864 ms/epoch (4.9x GPS-only).
CROSS-CONFIG vs `runs/pdfix_v3` — read for DIRECTION only, never as an A/B number.

**1. The structural risk this arm was written to test is RETIRED.** The card feared the
joint pair block would grow past K=686 and re-create the near-singularity the whole
defect was born of. It did the opposite: the component structure FRAGMENTED. 4990 blocks
over 355 epochs = **14 blocks/epoch vs GPS-only's 3**, carrying 835 766 rows =
2354/epoch vs ~780 — **3x the carried dimension in 4.7x the blocks**, so per-block K
FELL. Result: **0 degenerate, 0 repairs, 0 rebirths, worst relative pivot +6.18e-10**,
0 untestable D5 rows. Adding a constellation makes the fold's conditioning problem
EASIER, not harder — consistent with the `_gg305_pdfix_deg15_smoke` observation (worst
pivot 4.2e-3 at 305 sta, five orders better than 63). INC-1c and INC-2a remain
field-unexercised across every arm ever run.

**2. WHAT BROKE INSTEAD: the S-basis null-space gate, 4 consecutive epochs.**
TOW 263280-263370 (a 2-minute window, 01:08-01:09) — `drop-and-heal failed
(NetworkSbasis null-space gate: observed 4 (near-)zero eigenvalue(s) below 3.39e-04 but
the declared S-basis dimension is 2 ... the datum is under-specified)`. 5 epochs withheld
by the observability gate over the run, all in that window. This is the Aug-2 GPS+Gal
S-basis completion meeting a case it does not declare — see
`project_network_ssystem_dia_assessment`. It is contained, reproducible and short, which
makes it a good target: **4 epochs of a 360-epoch leg is a cheap, well-bracketed
investigation.** It is NOT a pd_safe issue (the fold was healthy through it).

**3. Products (per constellation — separate gauges, never a shared median):**

| hr2 | GPS | Galileo | GPS-only v3 |
|---|---|---|---|
| spread_med | 7.025 | 6.766 | 8.779 |
| worst_med | 17.28 | 11.78 | 21.78 |
| clk_std | 10.09 | 10.09 | 4.86 |
| excursions | none | none | none |

Both constellations' total_IF spread is TIGHTER than GPS-only, but clk_std is 2.1x
looser. Cross-config, so this ranks a direction to chase, not a verdict.

**4. AND THE ANSWER TO P6(b) IS UNCHANGED: med|frac| = 0.259 / 0.255 / 0.250 / 0.247 /
0.249 / 0.250 / 0.247 across every age bin.** Doubling the constellation — the main
lever on geometry, and the one P6's null pointed at — does NOT produce integer
information either. The DD pool went from 387 keys to 1922 and the fractional part is
still indistinguishable from uniform. (Cross-band is not comparable here: the gg signal
set makes the grader pick L1C-vs-L1W (1.000, n=283) and L1C-vs-L2L (1.351, n=212) rather
than the L1C-vs-L2W pair the GPS-only arms report.)

σ̂₀: 352 active / 1 floored / 2 warm-up over 355 installs. Post-fit σ̂₀ code 3.7782
(9523 rows) vs GPS-only 3.5643 (3345 rows). D5 994 flags / 994 re-taps.

## Standing cautions
- One variable per arm; every arm smoke-first; wipe snapshots (own paths).
- D5 flag storms are SIGNAL on this substrate, not regression — grade where
  the tension lands, not whether it exists.
- total_IF: eyes-open (78 % of the carry is newly live); degradation blocks
  promotion, movement alone does not.
- Do NOT re-baseline goldens until the matrix picks a posture.
