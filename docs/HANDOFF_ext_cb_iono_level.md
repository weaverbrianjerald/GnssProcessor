# HANDOFF — EXT-CB external-product ladder: the ionosphere-level defect

Written 2026-08-04; **FINAL STATE 2026-08-05 — a fresh session starts at
`docs/SPEC_level_information_budget.md` §17** (the live roadmap; it opens with the
six-beat narrative of the whole arc).  This document is the CAMPAIGN RECORD (arms
1–36): binding sections are "Hard rules", "The goal", "Already built", "Eliminated",
and "Known-latent"; everything else is measured history whose verdicts were revised
by later arms — trust the §17 status over any section below.  Read `CLAUDE.md` first.

**Verdict in three lines.**  The "19 TECU level defect" decomposed into a
CONVENTION offset (the receiver fleet's −5 ns GF-bias mean vs our zero-mean datum —
receivers were the carrier, not a broken parameter), an IGNITION transient (heals by
hour 2 at 61 stations; essentially ABSENT at 303 — station support dissolves it),
and a slow TRACKING question (the rolling carry memory is the accidental tracker;
every frozen pin lost to it).  Permanent keepers: **LV-4** (frame-invariant carry
gate; 74 % of withholds were frame artifacts) and **LV-5** (within-epoch μ; kills
the lag-1 limit cycle); retired for settled work: the LV-1+LV-2 level stack.
Datum-invariant grading (DD fractions IDENTICAL across postures; consumer replay
poor in BOTH arms with different excursion signatures) shows the real frontier is
**product internal consistency / churn** — where this campaign started.

## Hard rules (from the user, non-negotiable)

- **NEVER run `gnss_processor` from the Debug tree.** Every data run uses
  `build\x64-Release`. Run a **renamed copy** (e.g. `gnss_processor_fable.exe`) so a
  concurrent rebuild in another session cannot hit a locked exe.
- Build:
  ```powershell
  $vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  cmd /c "`"$vcvars`" >nul && cmake --build C:\GnssProcessor\build\x64-Release --target gnss_processor gnss_tests 2>&1"
  ```
- Git commits carry **no** Claude co-author trailer and no "Generated with Claude Code" line.
  Do not commit unless asked.
- **Staged cadence**: 20-epoch smoke first; go to >= 1 h only when the smoke is clean.
- New work is **default-OFF** and byte-identical when off.
- **Standing invariant #4**: an ARMED-but-INERT mechanism reads as working. Check run-end
  counters, never absence of crashes.
- Grade only **datum-invariant** quantities.

## The goal

Validate the network estimator by replacing its internally-estimated **satellite**
parameters with external COD products, one family at a time, to isolate where the network
actually fails. Fixture: DOY-129 (2024-05-08), 61 in-network GPS stations, WTZZ00DEU held
out as a closed-loop consumer.

Note the border is deliberately **not** emptied: the satellite phase bias is still estimated
(as a residual to COD's phase OSB) and the clock is *soft*-held, so the datum machinery is
still exercised. Fixing every satellite parameter with a free slant iono would
block-diagonalize the arrowhead into 61 independent single-station PPP solves and bypass
exactly the mechanisms under test.

## Already built — do not rebuild

All default-OFF; **1251/1252 tests pass** (the 1 skip is the documented fixture-gated
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`).

1. **`network.external_sat_code_bias` (EXT-CB)** — take satellite code AND phase biases from
   the external `.BIA` instead of estimating them. Suppresses the `estimate_iono_sh` -> W6
   force chain that previously made "SH-VTEC iono + external `.BIA`" unreachable. Forces
   `enable_code_phase_bias` + `estimate_ifb` on. Loader ~`GnssConfigLoadNetwork.cpp:2026`.
2. **External-clock hold** — `network.clock_datum_broadcast_anchored` under
   `apriori_source: sp3` with an explicit `apriori_clock_anchor_sigma_m`. The satellite-clock
   state is a RESIDUAL to the a-priori (`MeasurementModel.cpp:1517-1531`), so anchoring it to
   0 at tight sigma holds the clock at COD's `.CLK`. Sigma is a continuous dial from
   fully-external to fully-self-contained through one code path. Loader ~`:5299`.
3. **`network.rx_code_bias_datum`** (+ `_sigma_m`, default 10 m) — receiver-DCB datum, a loose
   zero-mean prior folded **pre-reduce** through the diagonal prior channel
   (`ApplicationNetworkAssembly.cpp`, in the `pl_cb` walk).
   **A post-reduce cross-station `SumZeroGauge` was tried and is BLOCKED** by a correct
   pre-existing exclusion in `NetworkEstimator::solve` (Card-4 DD priors and post-reduce
   Woodbury gauges cannot compose — both fold against the same cached local factors).
   **Do not retry that route.**

### Configs

In `config\test_suite\`: parent `_ext63_gps_dual_1h.json`, children
`_ext63_gps_dual_20ep{,_sbc,_a00,_rxcb,_rxcb_loose}.json`,
`_ext63_gps_triple_20ep_rxcb.json`, `_ext63_gps_dual_20ep_el10.json`.

> **The loader refuses an `extends` chain deeper than 8.** `_ext63_gps_dual_1h.json` is
> already at 7, so **new arms must extend IT, not its children.**

Runs land in `C:\GnssProcessor\runs\<name>\`. Each arm's `_comment` block states its
prediction *before* the run — follow that style.

## Measured — do not re-derive

- **Coverage gates are benign**: `require_atx_coverage` + `require_bia_for_signals` cost
  nothing on GPS L1/L2 (1220 station-epoch blocks, 22560 code channels, 0 skipped).
- **DEFECT 1 — FIXED.** Removing the satellite code-bias sum-to-zero also removed the only
  anchor on the receiver-DCB common mode, which is confounded with the global ionosphere
  level (only the mapping-function spread separates them). a00 drift over 20 epochs:
  **-80** (ungauged) -> **-48** (tighter a00 prior) -> **-12** (with `rx_code_bias_datum`).
- **DEFECT 2 — ROOT-CAUSED (2026-08-04 evening).** a00 ignites at ~12.7 TECU vs GIM 32.24;
  see ROOT CAUSE section below. Key new measurements (all reproducible offline, no runs):
  - **Arm 8, `_ext63_gps_dual_20ep_d15.json`** (`runs\ext63_dual_20ep_d15`): SH degree
    8 -> 15, single knob. a00 ignites at **6.18** — LOWER than deg-8's 12.78, walk to
    -13.9 unchanged, deg-1 rms 30-40 TECU in BOTH arms (control: 8-12). Representation
    hypothesis **falsified**. Bonus: the deg-15 field at 61 sta rings pointwise
    (field-vs-GIM at BSHM's own IPPs: -446 TECU at ep1) — the pdfix aliasing watch
    confirmed; keep deg-8 on this lineage.
  - **Arm 5, `_ext63_gps_dual_20ep_rxcb_loose.json`** (`runs\ext63_dual_20ep_rxcb_loose`):
    anchor 0.02 -> 3.0 m. clock-Δ rms at ep1 = **2.994 m**; between-satellite std
    2.5-3.0 m; static per-sat offsets (G01 +4.5, G26 +4.5, G19 -4.4 m, stable across all
    20 epochs). **Arm 4's "8 mm clock agreement" was the anchor talking.** But see next
    bullet — this is NOT model error.
  - **Estimator-free raw-chain check** (`scripts\extcb_raw_if_check.py`): per-satellite
    product-referenced IF code residuals from raw RINEX + COD SP3/CLK/OSB, station-median
    removed, 16 stations: **std 0.33 m, all sats within ±0.9 m**. The modeled chain
    (orbit, clock interp, relativity, Sagnac, PCO, OSB application incl. the C1C basis)
    is consistent with COD at sub-metre level. The loose arm's ±4.5 m is therefore
    **estimator redistribution, not model-vs-product disagreement**. Per-sat clock states
    under EXT-CB are code-determined (free per-(sat,band) phase biases make phase rows
    clock-blind), so they inherit the iono-channel defect through the LS coupling.
  - **GIM-referenced GF analysis** (`scripts\extcb_gim_verdict.py`, 30 stations, 1070 LOS):
    per-satellite GF error vs GIM std **2.3 TECU** (GIM-error level — satellites CLEAN);
    receiver GF bias distribution **mean -14.8 slant-TECU (≈ -1.55 m ≈ -5 ns of mean
    C1C-C2W receiver DCB), std 21.5 TECU, range -34..+47, geographically clustered**
    (Europe uniformly ≈ -20..-25, KRGG/OAFA/GAMB +40..+47); network field vs GIM **at its
    own IPPs: median -13.5 TECU** (ep1) — the deficit is real at the observed IPPs, not an
    a00-globality artifact, and matches mean(rx bias)/⟨m⟩ ≈ -11 in sign and size.
- **Honest clock-agreement number**: model-vs-COD consistency is **±0.3 m or better**
  (raw IF check; dominated by the check's own tropo/PCV simplifications). The tight
  0.02 m anchor is safe and correct to keep.

## Eliminated — do NOT re-test

| hypothesis | how it died |
|---|---|
| satellite GF bias applied with wrong sign | COD's mean (C1W-C2W) OSB over all 32 GPS satellites is **exactly 0.0000 m** (sd 1.396). A sign error contributes precisely 0 to the network-mean level. |
| frequency mode (dual vs triple) | triple reproduced dual to within **0.1 TECU**. `require_bia_for_signals` drops C5 (COD has zero GPS C5/L5 entries), so the geometry-free CODE content is L1/L2 either way. |
| mapping-function separability | iono elevation cutoff 25 -> 10 deg did not move the start (12.60) and made it noisier. |
| cold-start artifact | the W6 control's own ignition converges up into band in 2 epochs and holds. |
| unimported receiver biases | COD **does** publish receiver OSBs (140 stations, 28 of our 61, **4-character** station names) and our `ProductParser` rejects them (it requires a 3-char PRN). But for GPS **56 of 57 entries are exactly 0.0000** — COD's convention puts the receiver GPS group delay in their receiver CLOCK (header: `RECEIVER_CLOCK_REFERENCE_GNSS G`, `SATELLITE_CLOCK_REFERENCE_OBSERVABLES G C1W C2W`). Not the cause. |
| SH representation error (deg-8 too coarse; free sat DCBs absorbing it) | arm 8 (deg 15, single knob): ignition moved 12.78 -> **6.18** (wrong direction), walk unchanged, dipole unchanged. Falsified 2026-08-04. |
| code-OSB sign/basis error on the corrections path | per-sat loose-arm clock residuals vs every .BIA signature (flip signature 2·[IF(C1C,C2W)−IF(C1W,C2W)], all DCB variants): \|r\| ≤ 0.05. Also verified COD's IF(C1W,C2W) OSB content is exactly 0 per satellite. |
| a-priori clock time-tag error (leap-second / epoch off-by-one) | corr(residual, af1·c) = 0.03; COD clock drifts ~5e-12 are three orders too small for ±4.5 m. |
| relativistic eccentricity term mis-applied in the network chain | corr(residual, −2r·v/c) = −0.30, slope −0.20 (a chain mismatch predicts slope ±1, r ≈ ±1; G26: rel ≈ 0.03 m but residual +4.5 m). |
| any metre-level modeled-chain defect | estimator-free per-sat IF residuals: std 0.33 m (`scripts\extcb_raw_if_check.py`). |

## ROOT CAUSE (2026-08-04, measured)

**The receiver C1C-C2W GF hardware biases of the 61 stations have a nonzero network mean
(≈ -5 ns ≈ -1.55 m of GF code ≈ -15 slant-TECU) and continent-scale clustered structure
(std ≈ 21 TECU: Europe uniformly ≈ -22, southern outliers +40..+47). The
`rx_code_bias_datum` zero-MEAN convention forces Σ rx-DCB = 0, so:**

- the true **mean** cannot live in the rx-DCB states → it lands in the confounded
  direction, the **field level**: a00 low by mean(b)/⟨m⟩ ≈ 11-15 TECU (measured field-GIM
  at IPPs: -13.5) — DEFECT 2's level;
- the **clustered spread** is per-station-absorbable in principle, but the field's spatial
  coherence couples same-biased neighbouring stations, and the leak into the low degrees is
  the **30-40 TECU deg-1 dipole** (control: 8-12);
- the code rows then drag the code-determined **clock states** (loose arm's ±4.5 m) —
  satellite-tagged in appearance, receiver-caused in origin.

Why the W6 self-contained control gets the level right with the SAME confound: it arms no
rx datum, so the weak-but-correct mapping-spread channel sets the level (23-28, GIM-band).
EXT-CB arm 2 (no datum) showed that channel cannot hold against the leveling/carry
dynamics — a00 walked to -80 with collapsing sigma. The zero-mean datum (arm 4) stops the
walk but pins the WRONG VALUE: convention ≠ physics by exactly the rx-bias mean. Both
symptoms and the residual -1 TECU/ep walk are consistent; note the walk mechanism
(leveling-era ratchet pumping the confounded direction) is the one piece still inferred
rather than measured — the 1 h validation run will show whether it saturates.

Open mechanism note (unmeasured): the per-LOS iono x_lin is seeded at allocation from raw
GF code (memory `project_network_xlin_invariance`), i.e. WITH the receiver bias in it —
the ignition split between rx-DCB states and iono may be charged wrong from birth, which
would explain why per-station absorption loses to the field leak. Worth a look if the
dipole survives the level fix.

### The fix and its validation (arms 9-10, run 2026-08-04 evening)

The level defect is an S-basis realization mismatch, so the fix is a FRAME CHOICE: adopt
COD's iono level via the existing **per-epoch** mean-reverting a00 anchor
(`NetworkRunContext` ~:1309 — the clim anchor folds EVERY epoch in information space, so
it stands against the walk; an initial-only prior would not).

- **Arm 9, `_ext63_gps_dual_20ep_a00gim.json`** (`mean_vtec_prior_tecu: 32.24`, sigma 2):
  ROOT CAUSE CONFIRMED BY RESPONSE — L1 a00 ignited **28.3** (predicted 28-33; arm 4:
  12.78), `a00_in_band` 20/20 (arm 4: left at ep 8), walk saturated ~21 instead of
  running to -12. **But the confounded direction escaped into the UNANCHORED layer-2
  level** (default anchor 3±5 = info 0.04/ep): L2 a00 dived -15.9 -> -32.8, the TOTAL
  level stayed defective, and the SH health gate rightly withheld 12 epochs on the summed
  band. The tension re-routes to the loosest remaining level-like direction: arms 2-4 the
  L1 field, arm 9 the L2 field. Also note: the rx-CB carry innovation gate withheld
  710/1219 priors — the rx-DCB states visibly re-realizing.
- **Arm 10, `_ext63_gps_dual_20ep_a00gim2.json`** (arm 9 + `layer2_mean_vtec_prior_sigma`
  5 -> 0.5): closed the L2 escape (L2 flat 2.0-2.6) and the sh_health gate went 0/0/0 for
  the first time on this ladder. With both escapes shut, the pump discharged into L1
  alone (ignition 23.9, sag to ~7 by ep 16) and then BOTH defect curves inflected: a00
  turned upward (7.06 -> 8.70 over the last 5 ep) and the deg-1 dipole peaked at ep 6
  (39.5) and decayed (-> 30.3) — the transient signature.
- **Arm 11, `_ext63_gps_dual_1h_a00gim2.json`** (1 h settled window): the equilibrium
  flatline reading was FALSIFIED — a00 recovered from the ignition dip (8.3 at 10 min) to
  **20.2 by 30 min**, then BREATHED (second dip to 9.2 at ~45 min, back to **21.3 rising
  at run end**); deg-1 halved from peak (39.5 -> 18.6, Kaula ratio 4.0 -> 1.86) and
  visibly exchanges energy with the level (deg-1 swells during level dips: field sloshing
  between level and low degrees on the ~20-30 min constellation-sweep timescale while the
  rx-DCB harvest completes — fold fraction 43 % and rising). sh_health 0/0/0, in-band
  121/121. Total level at end 23.9 vs GIM 32.24 — residual gap + breathing is a bounded,
  NAMED behavior tied to the clustered-spread mechanism: the candidate next card, not a
  blocker.
- **Arm 12, `_ext63_gps_dual_20ep_a00gim_sl.json`** (user direction: kill layer 2,
  single-layer deg-8): reproduces arm 10's dynamics (ignition 24.7, dip to ~6.1 at ep 15,
  inflection up, gate 0/0/0) with a CLEANER dipole (peak 34.6 vs 39.5, end 26.1 vs 30.3 —
  even pinned, the L2 shell contributed slosh room). **CANONICAL EXT-CB POSTURE going
  forward: single-layer deg-8 + a00 GIM anchor (32.24±2) + rx_code_bias_datum** — also
  frame-commensurate with the single-shell GIM reference. Dual-layer stays on the
  self-contained lineage where Iono-2 mech-A earned it.
- The **deg-1 dipole** survived every level fix as predicted (it is the clustered
  rx-bias SPREAD, not the mean).
- Rejected alternative: a non-zero-sum target for `rx_code_bias_datum` (same effect,
  uglier plumbing than the a00 anchor).
- **Arm 13, `_ext63_gps_dual_1h_a00gim_sl.json`** (1 h single-layer canonical): same
  trajectory family as arm 11 (ignition 24.7, trough 7.1, recovery to 22.4 by 30 min,
  second breath to 10.1 at ~45 min, ends 20.95 rising), gate 0/0/0, in-band 121/121,
  deg-1 peak shallower (31 vs 35). **Canonical posture validated at 1 h.** Prediction
  miss, noted honestly: rx-CB carry fold fraction stayed 43 % (predicted rising).

## The level's information budget is ACCIDENTAL — arm 14, the day's deepest finding

**Arm 14, `_ext63_gps_dual_20ep_a00gim_sl_nc.json`** (arm 12 + `carry_rx_code_bias:
false`, the pump-attribution discriminator): the stated flywheel prediction was WRONG in
an informative way — nature took a third door. a00 **collapsed** 24.5 -> -3.5 in ONE
epoch, free-fell at ~-9 TECU/ep and saturated at **-37..-39** (out-of-band from ep 2, all
19 emissions withheld — the gates work). So the rx-CB carry was never the pump: **its
stiffness (q=1e-9 on per-station DCBs) was the accidental crutch holding the
level<->rxDCB<->field-null degeneracy closed.** Carry ON = frame memory + breathing
(arms 11/13); carry OFF = raw collapse. Neither is a designed equilibrium.

Where the collapse gradient comes from (and why it is always DOWNWARD): the per-LOS iono
x_lin is seeded from raw GF code WITH the receiver bias in it (mean -15 slant-TECU,
clustered). Any per-LOS stiffness toward the seed frame is ~0.04 info x 376 LOS ~ 15
units pulling the supported-region level toward the seed (bias-in) frame — dwarfing the
a00 anchor (0.25) and the rx datum's common mode (0.01, N-independent). The deg-8 basis
then localizes the contest: the SUPPORTED region fits the seed-frame iono, the
UNSUPPORTED hemisphere extrapolates under Kaula, and a00 (a GLOBAL mean, not a
supported-region mean) lands wherever that extrapolation sends it (-37 with the dipole at
23-30 fitting the regional structure). One mechanism now explains the level defect, the
dipole, the breathing, AND the arm-14 collapse.

### Revised internal-fix ranking (no external data)

**Implementation SPEC: `docs/SPEC_level_information_budget.md`** (cards LV-1/LV-2/LV-3,
written 2026-08-04 for hand-off; carries the user's waiver of byte-identity ceremony —
direct implementation, results-first).

1. **Bias-aware per-LOS iono seeding** (promoted to primary): seed/prior the per-LOS
   states in a bias-free frame — at cold start use the station-median-removed GF (the
   admission screen's own statistic) or seed from the field prior and let the DCB states
   take the residual. Kills the gradient at the source (level pull AND dipole pressure).
2. **rx-CB carry common-mode de-mean** (`network.rx_cb_carry_demean`, default-OFF): keep
   the carry's per-station-difference rigidity (arm 14 proves it is load-bearing — do
   NOT just turn the carry off), remove its frame memory: subtract the weighted mean
   innovation (computed at the serial harvest, applied lag-1 — the Iono-2 v3
   `rep_error_demean` idiom) so the carry exerts zero net common-mode force and the
   designed authority (datum/anchor/sat-gauge) owns the frame.
3. **Explicit level-information budget** per posture: EXT-CB = the a00 GIM anchor is the
   single level authority (validated, arms 12/13); SELF-CONTAINED = the W6 satellite
   sum-zero gauge already realizes the IONEX-community convention (which is WHY the
   control's level is GIM-compatible with no external data — convention agreement, not
   luck) + optionally a LOOSE internal climatology from the broadcast Klobuchar/NeQuick-G
   coefficients already in the ingested BRDC stream (operationally self-contained).

The W6 control arms the SAME carry (verified in `runs\agu63\run_console.log`) — the
crutch-plus-degeneracy structure is live there too, buffered by the 32 free satellite
DCB states (more absorbing directions), presenting only as sub-band a00 wander. The
EXT-CB ladder did exactly what it was built for: removed absorbers until the structural
defect surfaced.

## Arms 15–19 — the LV cards, BUILT and RUN (2026-08-05)

Implementation + full record: `docs/SPEC_level_information_budget.md` §9–§10;
the per-posture frame contract (LV-3) is `docs/CONTRACT_level_authority.md`.
Knobs: `network.iono_seed_field_frame` (LV-1), `network.rx_cb_carry_demean`
(LV-2), both default-OFF, both fingerprinted when armed.

- **Arm 15, `_ext63_gps_dual_20ep_lv1.json`** (`runs\ext63_dual_20ep_lv1`): arm 12
  + LV-1. a₀₀ ignition 24.69, trough **13.81** (arm 12: 6.05), end 14.78, mean
  18.35, in-band 20/20, gate 0/0/0.  580 seeds reframed, 79 station-epoch offsets,
  0 fallbacks.  rx-CB folds 448 → 382, skips 710 → 776.
- **Arm 16, `_ext63_gps_dual_20ep_lv1_nc.json`** (`runs\ext63_dual_20ep_lv1_nc`):
  **THE DECISIVE ARM — the spec's STOP clause fired.**  Arm 14's posture (carry
  OFF) + LV-1 reproduces arm 14 exactly: 24.5 → −3.5 → −37.1, endpoints agreeing
  to **0.006 TECU**.  With the carry off LV-1 has no actuator, so §1's per-LOS
  seed-frame gradient model is FALSIFIED — as the 2026-08-01 x_lin invariance
  theorem says it must be.  Arm 15's gain is therefore NOT a seed-stiffness
  effect: it is the rx-CB inject gate (a PREFIT screen on |value_abs − x_lin(cb)|,
  and x_lin(cb) is derived from the seeded iono) changing WHICH carry priors fold.
- **Arm 17, `_ext63_gps_dual_20ep_lv2.json`** (`runs\ext63_dual_20ep_lv2`): arm 12
  + LV-2. trough **13.81**, end 19.56, mean 19.99, in-band 20/20, gate 0/0/0.  μ
  computed on 19/19 epochs, 0 freezes, |μ| ≤ 1.64 m, 18–36 contributing stations.
  Skip rate barely moved (61 % → 64 %) — μ is small against the gate's ≈ 5.1 m
  threshold, so the ~58 % skip rate is a FRAME artifact of a frozen x_lin(cb), not
  a coherent common-mode trigger.  **The predicted skip-rate collapse did not
  happen; the level improvement did.**
- **Arm 18, `_ext63_gps_dual_20ep_lv12.json`** (`runs\ext63_dual_20ep_lv12`): both
  cards.  trough **19.09**, mean 25.80, in-band 20/20, gate 0/0/0 — the two
  channels compose.
- **Arm 19, `_ext63_gps_dual_1h_lv12.json`** (`runs\ext63_dual_1h_lv12`): the 1 h
  settled window, against arm 13.  a₀₀ mean **16.36 → 35.09** (GIM 32.24), min
  5.42 → 18.38, end 20.95 → **33.08**, in-band 121/121.  **DEFECT 2's −15.9 TECU
  deficit is substantially CLOSED (+2.85 mean, on the reference at run end).**
  rx-CB fold fraction 43.5 % → **51.9 %** (the 20-epoch skip reading was an
  ignition artifact).  μ swings ±1.8 m early and damps to ±0.8 m, mean −0.04 m
  over 120 epochs, 0 freezes.  Costs, stated: the trajectory still BREATHES
  (18–47 TECU) and the deg-1 dipole grew (peak 35.0 → 37.9, end 20.1 → **29.0**,
  Kaula 2.01 → 2.90) — the clustered-SPREAD mechanism is untouched, as predicted.

### Arms 20–21 — LV-4, the carry screen made frame-invariant

`network.rx_cb_carry_step_gate` (SPEC §11).  The fold-time test on
|value_abs − x_lin(cb)| is retired; the event response moves to the serial harvest,
where the step between two ABSOLUTE realizations is judged against a robust
peer-relative scale (1.4826·MAD of this epoch's steps, median removed, floored at
the formal σ).

- **Arm 20, `_ext63_gps_dual_20ep_lv12_ig.json`** (`runs\ext63_dual_20ep_lv12_ig`):
  arm 18 + LV-4.  Folds **381 → 1148** of 1219 harvests, **11 erases** (0.96 %),
  60/61 stations still carrying at run end, max robust σ̂ 1.70 m, max z 111.  The
  run's own A/B: the retired screen **would have withheld 852 of those 1148 folds
  (74.2 %)**.  a₀₀ trough 19.55, mean 39.23 — but noisy (19.5–61.6) inside ignition.
  ⚠ First cut used the FORMAL σ (0.52 m) and erased **432/777** entries, leaving 10
  stations carrying and destabilizing the level — the estimator's own re-realization
  is metre-class at ignition, so a formal-σ step test is a guaranteed mass-trigger.
  That is why the shipped screen is robust and peer-relative.
- **Arm 21, `_ext63_gps_dual_1h_lv12_ig.json`** (`runs\ext63_dual_1h_lv12_ig`): the
  1 h grading, against arm 19.  Folds **3793 → 7182** of 7365 (97.5 %), 123 erases
  (1.7 %), 0 stand-downs, in-band 121/121.  a₀₀ mean 37.19 (arm 19: 35.09) — but
  the settled window (20–60 min) **sd 4.98 → 15.53 TECU, range 14.3 → 54.1**: the
  level LIMIT-CYCLES between ~15 and ~55 on a ~3-epoch period, with max |μ| 1.96 →
  10.15 m.
- **Verdict.**  The screen defect is closed and proven closed (74 % of withheld
  folds were frame, not physics).  But admitting the full carry exposes a SECOND
  accidental budget: LV-2's μ is applied **lag-1**, and the loop gain scales with
  how much carry information folds — stable at 33 % admission, oscillating at
  97.5 %.  **The broken screen had been acting as the loop-gain limiter.**  Keep
  LV-4 OFF on this lineage until μ is computed WITHIN-EPOCH (SPEC §11.4, card
  LV-5: the carry innovation needs only the carry map and x_lin, both available in
  PASS A, so μ can be reduced between PASS A and PASS B and applied to the same
  epoch's folds).  Canonical posture stays LV-1 + LV-2.

### Arms 22–23 — LV-5, the within-epoch μ (the lag removed)

`network.rx_cb_carry_demean_within` (SPEC §12).  μ is reduced SERIALLY between
PASS A and PASS B — both terms of the carry innovation (the stashed absolute
posterior, the frozen `x_lin`) are known there — and PASS B folds against the SAME
epoch's value.  The post-solve harvest still reduces the folded set's mean, now as a
diagnostic: the difference is the RESIDUAL common-mode force the correction left.

- **Arm 22, `_ext63_gps_dual_20ep_lv125.json`** (`runs\ext63_dual_20ep_lv125`): the
  20-epoch wiring check.  a₀₀ sd 13.06 → **5.99**, folds unchanged (1147 vs 1148),
  12 erases, and **mean |resid| 0.0057 m against max |μ| 8.50 m**.
- **Arm 23, `_ext63_gps_dual_1h_lv125.json`** (`runs\ext63_dual_1h_lv125`): the 1 h
  grading.  Settled (20–60 min) **sd 15.53 → 4.69** — below even arm 19's 4.98 with
  1.9× the carry information admitted — range 54.1 → 13.9, settled mean 35.24, **end
  32.68 against GIM 32.24**.  97.5 % folded, 124 erases, 0 stand-downs, in-band
  121/121, mean |resid| 0.0208 m.
- **Prediction that missed:** max |μ| did NOT fall back to ~2 m (8.50 m, ≈ arm 21's
  10.15).  μ is genuinely that large once the full carry folds; the lag removal
  fixed the correction's PHASE, not its size.

**Recommended EXT-CB posture is now LV-1 + LV-2 + LV-4 + LV-5.**  LV-4 without LV-5
oscillates; LV-5 without LV-4 is nearly inert.  All four stay loader-default OFF.

### Arms 26–28 — what the cards actually do (the story changed)

- **Arm 26, `_agu63_nolane_30ep_lv125.json`** (`runs\agu63_nolane_30ep_lv125`): the
  SELF-CONTAINED (W6) spot-check, 30 ep, against `runs\agu63_nolane_30ep`.  a₀₀
  24.77 → **29.21** mean (control ends 24.37, LV stack 30.37) — i.e. the stack moves
  the production posture's level TOWARD GIM 32.24, not away.  deg-1 dipole SHRINKS
  (6.98 → 5.49 mean, 5.26 → 2.82 end).  LV-4: **0 erases**, max z 7.14, max robust
  σ̂ **0.189 m** (vs 1.70 m on EXT-CB — the self-contained posture's receiver DCBs
  are far more stable epoch-to-epoch because the satellite gauge determines them),
  and the retired screen would have withheld **12.7 %** of folds here vs 74 % on
  EXT-CB.  **So the frozen-frame pathology is posture-dependent**, and its severity
  tracks how far the first-epoch DCB seed lands from the realized value.
- **Arm 27, `_ext63_gps_dual_1h_lv245.json`** (`runs\ext63_dual_1h_lv245`): LV-2+4+5
  with LV-1 OFF, 1 h.  Predicted to reproduce arm 23 (LV-1 should be inert once LV-4
  retires its only known channel).  **It did not**: settled 26.45 vs 35.24, end
  25.11 vs 32.68 — a flat −9…−12 TECU offset — with **identical row sets** (7180 vs
  7182 step checks, 125 vs 124 erases).
- **Arms 28a/28b, `_ext63_gps_dual_1h_lv45{_lv1,}.json`**: the attribution pair (LV-4+5,
  de-mean OFF, LV-1 on/off).  With LV-2 off, the LV-1 effect collapses from ~10 TECU
  to **mean −1.97, max 3.35** on a level of ~4–6 TECU — the scale of the pure-FP
  amplification already measured for a near-null level direction (arms 14 vs 16:
  1.8 TECU).  **Attribution: the de-mean is the channel.**

**The correction this forced (SPEC §13).**  LV-2 does NOT free the DCB common mode.
The de-mean moves the fold's RHS, never its information, so the carry still puts
Σλ ≈ 100 units on that direction; in absolute terms the targets' weighted mean
becomes **mean(x_lin)** — the carry re-pins the common mode to the FROZEN
first-epoch code-bias seed frame.  It removes the FEEDBACK (why LV-5 mattered), not
the memory.  And `initializeCodeBiases` derives that seed by subtracting the seeded
iono, which is what LV-1 changes — so **LV-1's real function is to put the
physically correct value (the true ≈ −1.55 m network-mean receiver GF bias) into
the frame LV-2 pins**, instead of the ≈ 0 that DEFECT 2's zero-mean convention
produces.  That is why the pair lands the level on the GIM reference, and it is not
what either card was written to do.

### Arms 24–25 — the 12 h grading pair (reference vs full stack)

`_ext63_gps_dual_12h_ref.json` / `_ext63_gps_dual_12h_lv125.json`, same window, the
four LV knobs the only difference.  **This pair reversed the verdict — SPEC §14.**

First, the grading had to change.  Every level number on this ladder was quoted
against ONE figure, "GIM 32.24", the 00:00 map's global mean; but the GIM's own
global mean moves through DOY-129 (32.0 at 00 UT → 36.9 at 08 → 33.9 at 24), so a
rising a₀₀ may just be tracking it.  New tool:
`python scripts/gim_global_mean_series.py <run_dir> …` reports
`a₀₀(t) − GIM_mean(t)` hourly (a sun-fixed a₀₀ **is** the global mean, Ȳ₀₀ ≡ 1).

`a₀₀ − GIM_mean`, hourly:

| hour | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | mean |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| canonical | **−15.8** | −8.2 | −3.9 | −3.8 | −9.2 | −8.1 | −13.2 | −8.2 | −7.2 | −14.1 | −9.8 | **+1.0** | **−8.4** |
| LV-1+2+4+5 | **+4.2** | +4.8 | +4.9 | +13.2 | +13.9 | +20.8 | +17.9 | +15.0 | +16.5 | +16.6 | +18.4 | **+20.2** | **+13.9** |

- **The canonical posture's DEFECT-2 deficit is about HALF an IGNITION TRANSIENT**:
  it starts −15.8 (what every 20-epoch and 1 h arm measured), then wanders −3.8…−14.1
  about a −8.4 mean and ENDS AT +1.0 — on the reference.
- **The LV stack inverts that**: best ignition on the ladder (+4), then a smooth
  ramp (37 → 50 TECU across hour 3, no step, station/LOS counts flat) to +15…+21
  where it stays.
- Mechanism is the one §13 named: the de-meaned carry PINS the DCB common mode to
  the frozen epoch-1 seed frame at Σλ ≈ 100.  Frozen is right at ignition (LV-1
  makes it the physically correct value) and wrong later — the canonical has no such
  pin (43 % folds, un-demeaned) so its level re-realizes freely.
- LV-5's residual did NOT grow with the window (mean |resid| 0.0208 m at 1 h →
  0.0235 m at 12 h); LV-4: 195 erases / 87423 checks (0.22 %), 0 stand-downs; folds
  87423/87679 (99.7 %).  The cards do exactly what they claim — the claim was just
  not what the level needed on a settled window.

**Recommendation: do NOT arm the LV stack for settled work here.**  LV-4 and LV-5
remain correct fixes to real defects; what they expose is a stiff carry with a
FROZEN common-mode pin.

### Arm 29 — the one-knob "make the pin track" fix, FALSIFIED

`_ext63_gps_dual_12h_lv125_q6.json` (`runs\ext63_dual_12h_lv125_q6`): arm 25 with
`rx_cb_carry_q` 1e-9 → 1e-6 (τ = var/q ≈ 2.8 h), so the pin can follow the day
while within-epoch rigidity is untouched.  **Predicted the ramp would flatten;
measured the opposite** — the +20 plateau arrives at hour 3 instead of 5 and the
run ends **+25.5** (12 h mean +17.8 vs +13.9).  Carry mechanics stayed healthy
(87435/87679 folds, 183 erases, μ ≈ 0), so this is the level responding.

**The frozen pin was RESISTING the drift, not causing it.**  Across four postures
the ranking is not monotone in admitted carry information — what separates them is
the kind of MEMORY the common mode has:

| posture | common-mode memory | 12 h level |
|---|---|---|
| canonical (43 % folds, no de-mean) | **rolling** (last epoch's realization) | wanders about −8.4, ends **+1.0** |
| arm 14 (carry OFF) | none | −9 TECU/ep collapse to −37 |
| LV stack q=1e-9 | **frozen** at epoch-1's seed frame | ramps to +20.2 |
| LV stack q=1e-6 | frozen, decaying over ~2.8 h | ramps to +25.5 |

So the flywheel LV-2 removed is also the only thing that TRACKS the field, and the
±6 TECU breathing is the price it charges.  The next card is unchanged in direction
but now has four arms behind it: a **per-epoch, data-derived** authority for the
receiver-DCB common mode (SPEC §13.4 option 1, §15.2) — target |a₀₀ − GIM| ≲ 4 TECU
at hour 0 AND hour 11, which no posture reaches today.

Open after these arms: the tracking-pin card above; the deg-1 dipole needs its own
card; the self-contained spot-check (arm 26) covers 15 min of ignition only and must
be re-graded on a settled window before anything is concluded there; the
305-station gate is the next scale step.

### ~~Gated behind a correct level~~ — SUPERSEDED 2026-08-05

The gate condition dissolved: the level is convention + ignition transient, and at
303 stations the transient is essentially absent (hour-0 mean −1.98 ± 3.66 vs
time-matched GIM).  The 305 scale ran (arms 30+, incl. the 30/30 mask extreme,
which bought nothing — a00 goes prior-dominated exactly as predicted; read a00
posterior σ before any level number at high masks).  Deg-15 stays OFF at 61–63
stations (aliasing, arm 8).

## CURRENT STATE + NEXT (2026-08-05) — the section a new session acts on

Posture of record for settled EXT-CB work: **canonical** (no LV level stack).
Grading law (CLAUDE.md Amendments §6): time-matched GIM via
`scripts/gim_global_mean_series.py`, ≥ 2 h windows, datum-invariant quantities
first (invariant #5 — a00 is a diagnostic, never a grade).

Ranked next actions (detail + gates live in `SPEC_level_information_budget.md` §17):

1. **Churn / product internal consistency** — the re-exposed true blocker.
   Excursion-hour attribution of the replay pair
   (`runs\replay_ext63_12h_{ref,lv125}`) against products.txt disc/datum records;
   then the `emit_clock_disc` validation arm (the 13 product-wide clock jumps are
   currently silent to every consumer).
2. **§17 item 1** — the LV-4-alone rolling-memory ladder at canonical masks
   (1 h stability → 3 h first level read → 12 h tracking verdict); if it grades,
   the level card closes with no new code.  Consider running the decision at 305,
   where the level is near-perfect from hour 0.
3. **§17 item 3 — "S-consistent harvest"** (LASER-source-shaped: solve → transform
   → harvest, O(n), oracle `C:\LASER\test\GPS3710_test_PMINOLESS`) — only if the
   ladder misses a gate.
4. **The deg-1 dipole card** (clustered fleet-bias spread; untouched by every level
   fix) and **the CAS receiver-DCB rung** (the ladder's true endpoint — no soft
   absorber left; needs the ProductParser 4-char card below, which multi-GNSS
   needs anyway).

## Known-latent, out of scope here

`ProductParser` cannot read COD's receiver OSB records (3-char PRN assumption vs 4-char
station names). Inert for GPS-only because those entries are zero, but **load-bearing the
moment EXT-CB goes multi-GNSS** — Galileo E1X and BeiDou C2I receiver OSBs run -3.4 to
-3.9 ns. Worth its own card.
