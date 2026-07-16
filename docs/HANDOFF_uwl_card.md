# HANDOFF — Ultra-Wide-Lane tier (M5 card UWL-1)

**Implements** `docs/SPEC_uwl_card.md` increments 1–4 (Fable, Jul-16 2026 eve)
**+ increment 5 (dual-top preferred/fallback rung) + the v2 per-family pair
groups (Fable, Jul-16 night — see the "Increment 5 + family pairs" section
below).**  Default-OFF, byte-identical; GPS-only runs BIT-identical even when
armed.
**Status: increments 1–5 + family-pairs v2 LANDED + smoke-gated (10-ep battery
+ 15-ep A/B with per-cons lane trace).  §6.4 acceptance MET and improved
(E constrained circR 0.813 @ n=138 under dual+family+mid).  The §1b sibling
closure was MEASURED and the naive hard gate is a NO-GO (residual = clean
per-cycle integer constant ≠ 0 — see below); gate ships default-OFF.  Deferred
to the graded session: the Galileo force-fire (§6.5) + the cold-fix latency
A/B (increment 4's acceptance MEASUREMENT — the mechanism is landed and
smoke-armed).  Nothing committed.**

## Result summary

| gate | result |
|---|---|
| Suite | **772 run / 771 pass / 1 pre-existing skip** (+11 UWL gates: tier table, Galileo/AltBOC bridge hand values, GPS bit-id-under-table, cold MW/bridge serve, layer cold decision-only ×2, cons filter, cold ordering, config contract) |
| Byte-id (a) all-OFF 15-ep vs `gnss_processor_wl3_run.exe` | **BIT-identical** (solution 90DB…, dd-frac DB1A…) |
| Byte-id (b) GPS+GAL lanes armed, `uwl_enable=false`, cross-exe | **BIT-identical** (C436…, F908…) — arming a constellation without the card changes nothing |
| Byte-id (c) GPS-only `uwl_enable` true-vs-false, same exe | **BIT-identical** (F48A…, 1C64…) — the table + joint census reproduce the GPS digits exactly |
| 10-ep GPS+GAL smoke (`_wlver_50sta_10ep_uwl.json`) | Galileo UWL(7,5) **591/591 FFR-fixed**, mean R 0.0001 cyc² (GPS EWL: 0.0004); walk 67 decided → 8 moved + 19 re-pinned + 3 injects; **D5 598 < control 639** |
| AltBOC leg (`_uwl8`, λ 19.537 m) | **555/555 fixed**, D5 590; bridge coarser (99 no-bridge/73 wide vs 39/76 — λ_top amplifies the (φ_top−φ_lo) phase noise in meters) ⇒ **'7' stays the default**; '8' is the colder statistic if a future tier wants it |
| Cold-fix smoke (`_uwl_cold`) | Galileo **82 cold-fixed n<2 + 68 cold-unfixed** (FFR self-protection), 3 cold-bridge re-pins; **GPS 0 cold** (λ floor 8 m holds); D5 583 |
| 15-ep A/B | **D5 858 armed vs 991 GPS+GAL control** (stabilizing); Galileo 858/858 fixed, 104 decided → 9 moved + 34 re-pinned, closure backstop live (9 blocks) |
| §6.4 lane trace (per-cons split of `grade_lane_trace.py`) | **Galileo constrained circR 0.719 / med\|f\| 0.010 / P(<.15) 0.765** (pooled last-5, n=34) vs GPS constrained 0.471 / 0.069 / 0.660 — *the stronger seed shows*, exactly the spec's criterion.  Controls: E 0.097, G 0.027 |

Exe `build/x64-Release/gnss_processor_uwl_run.exe`; reference
`gnss_processor_wl3_run.exe`.  Configs `_wlver_50sta_10ep_{gpsgal_ctl,uwl,uwl8,
uwl_cold,uwl_lt,uwl_d5,lane_uwlarmed}.json`, `_wlver_50sta_15ep_{gpsgal_ctl,
uwl,uwl_lt}.json`.

## What landed where

- **`include/slip/MwLaneTiers.hpp` (NEW)** — the per-constellation lane-tier
  table `mwLaneTierOf(cons, gal_top_band)`: G {1,2,5}; E {1,5,'7'|'8'}
  (`network.mw_wl.uwl_gal_top_band`, default 7 = (E5b,E5a) λ 9.768 m; 8 =
  (E5-AltBOC,E5a) λ 19.537 m — the user's ~19.5 m option, now a config knob,
  not a study).  C absent (BLOCKED on `beidou-network-clock-divergence`), R
  excluded (FDMA).
- **Detector** (`MwSlipDetector`): `setLaneTiers` (empty = legacy {1,2,5} scan
  for every cons, byte-identical; pushed table ⇒ per-cons digits, untabled
  cons forms NO bridge); `updateBridgeArc` generalized — top pair (hi,lo) by
  ACTUAL frequency (GPS's shared band is the top pair's high side, Galileo's
  its low side; λ_top stays positive; iono products already order-free);
  **cold accessors** `arcZdMeanCold` (UNLAGGED n≥1; var NaN at n=1 for the
  caller's a-priori substitution) + `arcBridgeCold` (unlagged n≥2 — one epoch
  ahead of the lagged serve).  Both DECISION-ONLY by contract.
- **Layer** (`NetworkMwWlLayer`): `Config.cons_filter` (per-cons instances,
  filterless legacy default); `ArcData.{cons,mw_cold}`; cold cycles bypass the
  young gate but are **decision-only** — never emitted (not even under the
  `ewl_fold_rows` study shape, not as floats), integer exposed via
  `FixedEwlRow` APPENDED after the warm rows (walk-order discipline);
  `Counters.{rows_fixed_cold,rows_cold_unfixed}`.
- **ApplicationNetwork**: `band5_of` → `band_top_of` (per-cons top digit under
  the card; legacy '5' otherwise); EWL resolver pairs (primary sig2, top band)
  ordered (hi,lo) by frequency + cold substitution (λ-floor-gated, a-priori σ
  at n=1); per-cons EWL lane instances + per-cons `WlCounters` (reference
  indirection `wc` in the walk; `wlAccumulate` helper); cold-bridge fallback
  in the walk (only when the lagged read is immature — never replaces mature
  statistics); loud once-per-run warns (untabled cons under the card; non-GPS
  cons under legacy digits — fact 3's silent death is now loud); **joint pair
  census** (below); one-shot census + group-census INFO lines; per-cons
  run-end telemetry (a pooled line never hides which lane storms).
- **Config**: `network.mw_wl.{uwl_enable,uwl_gal_top_band,uwl_cold_fix,
  uwl_cold_sigma_m,uwl_cold_min_lambda_m}` — parse/clamp/mutex (uwl requires
  ewl; λ floor clamped ≥6 m so the GPS EWL can never be admitted; σ>0), ON
  summary lines.

## Spec deviations — each measured, none speculative

1. **§2/§8.1 The WL-3 per-band census is FALSIFIED for multi-family
   constellations — replaced by a JOINT pair census under the armed card.**
   Galileo channels come in correlated receiver families — (L1C,L5Q) vs
   (L1X,L5X), measured 132/115 vs 127/127 arcs on the 2024-129 fixture — and
   the per-band majorities composed the PHANTOM pair (L1C,L5X) that NO station
   tracks: **zero Galileo pair arcs formed** (group census: E had only
   per-band groups while GPS had its L1C+L2W [PAIR]).  The fix counts each
   (rx,sat)'s raw two-lowest-CARRIED-band channel pair and elects the majority
   pair per constellation (ties lexicographic), then steers `chan_pref` (and
   through it preferChan + the bridge keys) to the elected pair.  Result:
   `L1X+L5X [PAIR]` forms (127 arcs) and the Galileo walk went from
   0 moved/0 re-pinned/67 basis-skips to 8/19/40.  **Gated under `uwl_on`**
   to keep every WL-3 validated baseline + byte-id gate intact; at commit
   time the joint census should become the ONLY census (it degenerates to the
   per-band result whenever channels are uncorrelated — GPS bit-id verified,
   gate (c)) after re-running the WL-3 15-ep A/B on it.
2. **§3.3 cold-fix aggregation math corrected.**  `uwl_cold_sigma_m` is a
   per-ARC per-sample σ; a DD cycle aggregates √(Σc²)·σ/λ, so an ALL-cold
   4-arc cycle (~0.14 cyc at the 0.7 m default) correctly self-gates at the
   0.10 FFR σ gate, while the actual target — ONE re-warming arc inside a
   mature cycle (~0.072 cyc) — passes.  Measured: 82 cold-fixed / 68
   cold-unfixed in the cold smoke.  The spec's "0.7 m ⇒ 0.072 cyc" skipped
   the cycle aggregation.
3. **§3.3 serve semantics sharpened**: the cold reads are UNLAGGED (the F1 lag
   would postpone the n=1 decision by an epoch, defeating the card); safe
   because they are decision-only — nothing folds (rows suppressed even under
   the fold study shape; the WL-3 lane increments stay on the lagged
   once-only accessors).  The cold BRIDGE serves at unlagged n≥2 (variance
   needs two samples) ⇒ post-slip re-pin lands at t+2, statistic decision at
   t+1.  During a cold window the MW(1,5) closure is usually absent (its arc
   reset too) ⇒ a cold decision is frac-gated + D5-backstopped + idempotently
   re-decided as stats mature — the force-fire must show zero wrong integers.
4. **§7.5 AltBOC promoted from study to config** (`uwl_gal_top_band=8`),
   measured (see table): colder statistic, coarser bridge — default stays 7.

## Reconciliation — the spec grew increment 5 (§1b) mid-implementation

While increments 1–4 were being implemented, the SPEC gained (concurrent
session, "user-directed, Jul-16") §1b (integer-accessibility ladder) and a
re-scoped increment 5: band-8 as Galileo's **preferred** top rung *where
tracked*, (5,7) as the fallback rung, the **untaken sibling as a closure
gate**, and BOTH bands {7,8} uncarried.  What this build delivers vs that:

- Delivered: the 19.537 m tier as a RUN-LEVEL choice (`uwl_gal_top_band=8`,
  measured — see the AltBOC leg above).  One top rung per constellation per
  run; only the SELECTED band is uncarried.
- **RESOLVED (Jul-16 night): increment 5 proper is LANDED as `uwl_dual_top`
  (per-cycle preferred/fallback, both siblings uncarried) — see the
  "Increment 5 + family pairs" section.  The sibling closure was implemented
  measure-first and note 2's caveat proved decisive: the residual is a clean
  per-cycle integer constant ≠ 0 (independent acquisition), so the naive hard
  gate is a NO-GO and ships default-OFF.**  Original implementer notes kept
  below for the derivations:
  1. **λ_top must be uniform per CYCLE** (the identity
     z_WL = [Σc·GF_b + λ_top·N_top]/λ_WL sums one λ_top across the cycle's
     arcs), so "where band-8 tracked" resolves per cycle, not per arc.  The
     natural shape: TWO top-tier lane instances per constellation (an '8'
     instance whose cycles with any band-8-less arc die as skip_missing_band,
     and a '7' instance deciding only what the '8' one didn't) + the detector
     maintaining BOTH bridge triples (1,5,7) and (1,5,8) — `updateBridgeArc`
     currently forms ONE triple per constellation per obs; that is the real
     new surface.
  2. **The sibling closure identity is 2·N(5,8) = N(5,7)** per cycle DD:
     f_E5 = (f_E5a+f_E5b)/2 exactly, so a coherent AltBOC phase has
     N8 = (N5+N7)/2.  Receiver half-cycle conventions may offset this —
     MEASURE the residual distribution before hard-gating (a half-integer
     population means the convention differs; the gate can learn a constant).
  3. Both siblings uncarried: `isUncarriedThirdBand` must return a SET for E
     under increment 5 (today it excludes only the selected top band).

## Deferrals (triaged)

- **Galileo force-fire + cold latency A/B (§6.5, increment 4's acceptance
  measurement)** — needs the WL-2-style synthetic-slip RINEX edit on an
  L1X/L5X-family satellite; graded session, on command.  Field-blocking for
  ARMING (invariant #1 wants the event response proven on a Galileo sat);
  everything else on the card is smoke-clean.
- **Minority-family pair groups — RESOLVED (Jul-16 night):** landed as
  `uwl_family_pairs` (steered-primary-pair admission, run-latched set); the
  15-ep `skip_basis` population 139 → 0 and the constrained lane population
  tripled at BETTER integer clustering (circR 0.813).  Per-family bridge
  preferences turned out unnecessary — the detector's preferred-channel
  fallback already keys minority bridges consistently.
- **BDS-3 (increment 5)** — table entry deliberately absent; BLOCKED on
  `beidou-network-clock-divergence`.  A BDS run under the armed card forms no
  bridge and warns once (loud, not silent).
- **Poison-pair watch (§6.6)** — UFPR/POAL are not in the `_wlver` 50-sta
  list and the harness runs `estimate_iono_sh=false` (H1 deweight inert).
  The watch applies to the field-grade config, not this harness.

## All-pairs MW census (user-directed, Jul-16 late — the "don't restrict, survey everything" check)

`scripts/mw_census_allpairs.py` (NEW, permanent instrument): estimator-free MW
over EVERY channel combination across distinct frequency bands, G/E/C, ordered
by λ — per pair: per-sample σ (n=1 decidability) + star-basis DD fractional
stats (the network closed-cycle constraint; receiver/satellite biases cancel).
Run: 10 ep × 30 s over the FULL 2024-129 fixture (309 stations parsed).
Full table in the session scratchpad + reproducible by rerun.  Findings:

| rung (best channel combo) | λ (m) | σ₁ (cyc) | DD med\|f\| | circR | coverage |
|---|---|---|---|---|---|
| **C (B1C,B1I) L1P/L2I** | **20.932** | 0.009 | 0.013 | **0.995** | 139 sta / 29 sat |
| E (E5b,E5) L7Q/L8Q | 19.537 | 0.005 | 0.004 | **0.998** | 181 / 25 |
| E (E5,E5a) L8Q/L5Q | 19.537 | 0.005 | 0.005 | 0.986 | 180 / 25 |
| C (B2b,B2ab) L7Z/L8X | 19.537 | 0.003 | 0.009 | 0.999 | 28 / 29 |
| E (E5b,E5a) L7Q/L5Q | 9.768 | 0.011 | 0.010 | 0.986 | 181 / 25 |
| C (B2b,B2a) L7D/L5P | 9.768 | 0.010 | 0.011 | 0.929 | 78 / 29 |
| G EWL (L2W,L5Q) | 5.861 | 0.022 | 0.018 | 0.971 | 182 / 17 |
| E (E6,E5b) L6C/L7Q | 4.186 | 0.027 | 0.032 | 0.951 | 161 / 25 |
| C (B3I,B2b) L6I/L7D | 4.884 | 0.018 | 0.032 | 0.942 | 118 / 32 |
| E (E6,E5) L6C/L8Q | 3.448 | 0.026 | 0.035 | 0.946 | 161 / 25 |
| E (E6,E5a) L6C/L5Q | 2.931 | 0.037 | 0.070 | 0.910 | 161 / 25 |
| G WL (L1C,L2W) | 0.862 | 0.173 | 0.127 | 0.422 | 308 / 40 |

- **Galileo's 19.5 m siblings are near-perfect single-epoch integers in BOTH
  receiver families** (Q and X: circR 0.986–0.998) — increment 5's preferred
  rung is fully supported by data.
- **Galileo E6 is a NEW independent rung** (band 6 = a new frequency, not a
  {5,7,8} sibling): (6,7)/(6,8)/(6,5) at λ 2.9–4.2 m, circR 0.91–0.95, 161
  stations.  The §1b budget for Galileo is F−1 = 4 lane rungs; the cascade
  uses 2 today.  A mid-ladder E6 hop would cut each bridge hop's λ ratio from
  ~26 (19.5→0.75) to ~5 — a candidate increment after inc 5.
- **BeiDou's MW/DD side is HEALTHY**: the (B1C,B1I) 20.9 m ultra rung (wider
  than Galileo's), the 19.5/9.8 m pairs, and the B3I mid rungs all cluster
  cleanly at the network-DD level.  The `beidou-network-clock-divergence`
  blocker is a SOLVE defect only — the tier table's C row will work the day
  it closes.  BDS-2 legacy signals are the noisy ones (L6I/L7I med|f| 0.159
  vs BDS-3 L7D 0.032; L2I/L7I circR 0.041 at λ 0.85).
- **Channel-alignment caveat**: X-attribute combos run systematically worse /
  biased (E L6X/L7X med|f| 0.131 with circR 0.89 — a cluster OFF the integer;
  GPS L1C/L2X circR 0.236) — receiver-family phase-alignment conventions.
  Confirms the JOINT census + per-family grouping direction (v2).
- Design conclusion (the user's question): NO restriction on the measurement
  side — the detector already builds all pairs and the census surveys them;
  the restriction that must remain is the FOLD/re-pin budget (F−1 independent
  rungs per constellation, widest-first; dependent siblings are closure
  gates, never lane rows — the F-A refold class).

## E6 mid-ladder bridge hop (user-directed, Jul-16 late — LANDED + smoke-gated)

`network.mw_wl.uwl_mid_enable` (default OFF; requires uwl_enable): the Galileo
WL determination chains **top → (E6,E5a) λ 2.931 m → WL** instead of the direct
top→WL hop.  Hop-hi fixes a ROUNDED mid-rung integer m (own FFR gates + the
free MW(6,5) closure); hop-lo re-expresses the WL statistic against m —
stripping the top hop's λ_TOP-scale phase/iono amplification (~3.3× smaller λ
multiplier).  The direct hop stays the per-cycle fallback AND the
wrong-integer cross-check (both-credible-but-different ⇒ decide neither,
counted).  Decision-side only: no new folds, no estimator columns (the hops
are phase-only detector reads), the WL-3 lane increments stay on the direct
triple's lagged source; cold serves stay a direct-hop privilege (λ floor).

- **Mechanics**: `MwLaneTier.b_mid` ('6' for E when armed; GPS none);
  `updateBridgeArc` refactored to a per-triple feeder — base (b1,b2,b_top)
  always, plus (b_mid,b2,b_top) hop-hi and (b1,b2,b_mid) hop-lo when armed
  and tracked (reset funnel/LLI erase are key-scan generic — covered
  automatically).  Chain identity GF_direct = GF_hi + GF_lo and
  C_direct = C_hi + C_lo unit-pinned (exact).
- **Gates**: suite 775/774/1 (+3); byte-id (a) all-OFF 90DB/DB1A ✓, (b)
  GPS+GAL legacy armed-bridge path C436/F908 ✓ (the feedTriple extraction is
  float-neutral), (d) uwl-armed mid-OFF vs the pre-mid exe
  (`gnss_processor_uwl_ref.exe`) BIT-identical (3A39/B5A0).
- **10-ep smoke**: Galileo decided **67 → 154** (150 via the mid hop), lane
  fixes 8+19 → **18+35**, injects 3 → 12 (|Δ|≈6 cyc — the code-seed wall on
  cycles that never previously decided); bridge-wide skips 76 → 25; **0 mid
  FFR fallbacks / 0 MW(6,5) closure blocks / 0 direct-disagreements** (the
  backstops never fired); GPS bit-unchanged; D5 618 (mid-off 598, ctl 639).
- **15-ep + lane trace**: decided 104 → **215** (202 mid), fixes 9+34 →
  **22+54**; constrained population n 34 → **118** (pooled last-5) at
  med|f| **0.056** (mid-off 0.061 — median IMPROVED while the population
  tripled into harder cycles; circR 0.547 vs 0.719 reflects the expansion,
  not quality loss); GPS 0.473 ≈ baseline; D5 867 (mid-off 858, ctl 991).
- **Sharpened v2 case**: 101/154 (10-ep) decided integers now land on
  minority-family cycles whose carry is per-band (`skip_basis`) — the mid hop
  makes per-family pair groups the single biggest remaining coverage lever.

## Increment 5 + family pairs (Jul-16 night — LANDED + smoke-gated)

Continues the reconciliation section's implementer notes.  Exe
`gnss_processor_uwl5_run.exe`; references `gnss_processor_wl3_run.exe` (all-OFF)
and `gnss_processor_uwl_run.exe` (uwl/mid armed).  New configs
`_wlver_50sta_10ep_uwl_{dual,fam,dualfam_mid}.json`,
`_wlver_50sta_15ep_uwl_dualfam_lt.json`.

### What landed

- **`uwl_dual_top` (increment 5 proper):** `MwLaneTier.b_top2` — E prefers '8'
  (λ 19.537 m) per CYCLE where tracked across every arc, falls back to '7'
  (λ 9.768 m); BOTH {7,8} uncarried.  Detector feeds both base triples
  (1,5,8)+(1,5,7) — the fallback triple forms even when band 8 is untracked
  (the legacy tri-frequency early-out is restructured, single-top flow
  verbatim) — plus both hop-hi triples under mid.  TWO E lane instances
  (preferred FIRST); the walk selects the fx's top map per entry
  (`band_top2_of`), dedupes fallback entries once the preferred rung DECIDED
  the row (a preferred entry that fails a gate does NOT consume — free
  demotion), and folds at most ONE lane bridge increment per (block,row).
  Forces `ewl_fold_rows=false` at load (F-A refold class).
- **`uwl_sib_gate` + sibling telemetry:** the untaken rung's MW DD is measured
  on every dual decision (residual r = z_sib − N·λ_top/λ_sib); hard gate
  (0.5+3σ_sib, block ⇒ demote to fallback) default-OFF per the measure-first
  note — correctly, see the measurement below.
- **`uwl_family_pairs` (v2) + `uwl_family_min_arcs` (8):** the admissible pair
  SET — majority pair + run-LATCHED minority families counted over the
  **STEERED primary pairs** (see falsification below).  Routing admits any set
  member; the group key was already the pair string, so families are disjoint
  graphs.  Fixture: E (L1C,L5Q) 115 + (L1X,L5X) 127 + (L1C,L7Q) 13;
  G (L1C,L2W) 388 + (L1C,L2X) 8.

### Measured findings (each changed the design or a default)

1. **Raw-census family admission FALSIFIED (smoke E, first build):** the raw
   two-lowest census counts a dual-tracker's pair as (L1C,L2L) (L2L < L2W
   lexicographically) — 143 raw counts vs **1** steered arc — latching a
   stranded 1-arc micro pair-group that forfeits that arc's per-band cycles.
   Cured: admission counts the STEERED primary pairs, the exact quantity the
   routing matches.  Under the fix no stranded groups form.
2. **The naive sibling closure gate is a NO-GO on real data.**  With
   integer-valued targets (preferred-rung decisions), the residual is
   integer-CLEAN (mean |frac| 0.043–0.053) but **100% off-integer** (163/163,
   226/226): per-signal ambiguities acquire INDEPENDENTLY, so
   2·N(8,5) − N(7,5) is an arbitrary per-cycle integer constant — the AltBOC
   coherence exists only modulo acquisition.  (Smoke D's mean |frac| 0.213 is
   the same effect mixed with half-integer targets from odd-N₇ fallback-side
   checks.)  A v2 gate would learn the per-cycle constant and gate on it
   CHANGING — but D5 already covers change detection ⇒ **deprioritized**; the
   telemetry stays (the |frac| dial is a free channel-alignment monitor).
3. **Dual-top alone (10-ep, mid off):** E decided 83 vs 67 single-top (39 via
   '8' where fully tracked, 44 via fallback; 39 deduped); both lanes decide
   (preferred 555/555 FFR-fixed, fallback 591/591); D5 495 (single-top 598,
   ctl 639).  GPS bit-unchanged in its walk counters.
4. **Family alone (10-ep):** E `skip_basis` 56 → **0**; lane fixes 24 moved +
   47 re-pinned (single-top: 8+19); D5 836 — the one-time regroup transient
   (minority cycles refloat from per-band into pair groups; see the 15-ep
   settling below).
5. **Full stack (dual+family+mid+cold, 10-ep):** E decided **171** (156 mid,
   26 fallback, 145 deduped), 54+117 lane fixes, `skip_basis` 0, cold serving
   live (155 cold-fixed n<2 on the preferred lane, 6 cold-bridge decisions);
   D5 649 ≈ mid-leg 618 / ctl 639.
6. **15-ep A/B:** control (mid_lt on the new exe) reproduces the pre-change
   counters EXACTLY (215 decided / 202 mid / 22+54 / D5 867 — a third
   byte-id confirmation).  Dual+family+mid: decided **234** (211 mid, 34
   fallback, 200 deduped), lane fixes **60 + 174** (≈3× the control's folded
   coverage), `skip_basis` 139 → **0**, injects 33 (mean |Δ| 20.8 cyc — first
   decisions on newly-opened minority cycles, the code-seed wall class), D5
   976 vs 867 (+13% — regroup + first-decision churn; watch on the graded
   run).
7. **Lane trace per-cons (pooled last-5, same grader both legs):** E
   constrained n 49 → **138** at med|f| 0.006 → **0.001**, circR 0.691 →
   **0.813**, P(<.15) 0.796 → 0.877; controls ~uniform; G flat (0.475 →
   0.465).  The recovered population arrives CLEANER than the old one.

### Gates (final build)

| gate | result |
|---|---|
| Suite | **780 run / 779 pass / 1 pre-existing skip** (+5: dual tier table, λ(8,5)=2·λ(7,5) production-table identity, fallback-triple-without-preferred-band, dual+mid both-hop-hi chain identities, dual/sib/family config contract incl. the fold-rows force) |
| Byte-id (a) all-OFF 15-ep, wl3 exe vs new | **BIT-identical** (90DB1548 / DB1A081C — same digits as the inc-1–4 session) |
| Byte-id (b) uwl-armed 10-ep, uwl exe vs new (re-run on the FINAL binary) | **BIT-identical** (3A395593 / B5A04846) |
| Byte-id (c) mid-armed 10-ep, uwl exe vs new | **BIT-identical** (17308359 / 26D8FDCB) |

## Verification checklist (final build)

- OFF byte-identity ×3 + cross-exe lane-shape check: all BIT-identical (hash
  prefixes in the tables above; full hashes in the session transcripts).
- Suite 780/779/1 on the final tree (was 772/771/1 pre-mid, 775/774/1 with
  mid, +5 dual/family gates).
- Smoke battery: no errors/NaN; every skip counted; per-cons lines present;
  dual-top and family-census lines present where armed.
- Quick-only per the staged cadence; nothing committed.
