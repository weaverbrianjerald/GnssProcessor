# EXT-CB arms 1–36 — the EXT-CB campaign record, arms 1–36 — root cause, the fix, and the L2-ESCAPE

**STATUS**

- **id:** EXT-CB arms 1–36
- **topic:** ionosphere
- **status:** closed
- **summary:** the EXT-CB campaign record, arms 1–36 — root cause, the fix, and the L2-ESCAPE
- **dates in the text:** 2024-05-08 → 2026-08-15
- **knobs / posture:** `network.external_sat_code_bias`, `network.clock_datum_broadcast_anchored`, `network.rx_code_bias_datum`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 29, 29.1, 29.2, 29.3, 29.4, 29.5, 29.6, 29.7, 29.8

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 29. The EXT-CB campaign record, arms 1–36 (absorbed from `HANDOFF_ext_cb_iono_level.md`, 2026-08-04/05)

*Absorbed 2026-08-15; source now `docs/attic/HANDOFF_ext_cb_iono_level.md`. Only what is UNIQUE to the handoff
is reproduced (its binding sections — goal, already built, eliminated, root cause, known-latent — are
§29.1–§29.5 and §29.8); its per-arm history whose verdicts were later revised lives in §10–§28. Its build
block carried the retired VS-2022 vcvars pattern; §6 replaces it.*

**Verdict in three lines.** The "19 TECU level defect" decomposed into a CONVENTION offset (the receiver
fleet's -5 ns GF-bias mean vs our zero-mean datum — receivers were the carrier, not a broken parameter), an
IGNITION transient (heals by hour 2 at 61 stations; essentially ABSENT at 303), and a slow TRACKING question
(the rolling carry memory is the accidental tracker; every frozen pin lost to it). Permanent keepers:
**LV-4**, **LV-5**; retired for settled work: the LV-1+LV-2 stack. Datum-invariant grading puts the real
frontier at **product internal consistency / churn** — where this campaign started.

### 29.1 The goal, and the fixture design rationale

Validate the network estimator by replacing its internally-estimated **satellite** parameters with external
COD products, one family at a time, to isolate where the network actually fails. Fixture: DOY-129
(2024-05-08), 61 in-network GPS stations, WTZZ00DEU held out as a closed-loop consumer. **The border is
deliberately NOT emptied**: the satellite phase bias is still estimated (as a residual to COD's phase OSB) and
the clock only *soft*-held, so the datum machinery is still exercised. **Fixing every satellite parameter with
a free slant iono would block-diagonalize the arrowhead into 61 independent single-station PPP solves and
bypass exactly the mechanisms under test.**

### 29.2 Already built — do not rebuild

All default-OFF. (Test counts in the source — 1251/1252 — are a 2026-08-04 snapshot; §9.)

1. **`network.external_sat_code_bias` (EXT-CB)** — satellite code AND phase biases from the external `.BIA`
   instead of estimated. Suppresses the `estimate_iono_sh` → W6 force chain that previously made "SH-VTEC iono
   + external `.BIA`" unreachable. **Forces `enable_code_phase_bias`
   + `estimate_ifb` on.** Loader: the `jval<bool>(json, "network.external_sat_code_bias", …)` parse,
     `src/core/config/GnssConfigLoadNetwork.cpp:2062-2063` (the handoff's `GnssConfigLoadNetwork.cpp:2026` has
     drifted).
2. **External-clock hold** — `network.clock_datum_broadcast_anchored` under `apriori_source: sp3` with an
   explicit `apriori_clock_anchor_sigma_m`. **The satellite-clock state is a RESIDUAL to the a-priori**:
   `obs.modeled` already removed the a-priori product clock, so the residual-clock state enters with a -1
   Jacobian on BOTH code and phase rows — the `config_.estimateSatClocks()` block in
   `src/core/measurement/MeasurementModel.cpp` (~:1545-1563; the handoff's `MeasurementModel.cpp:1517-1531`
   now points at the Card-3 receiver-phase-bias block). Anchoring it to 0 at tight sigma holds the clock at
   COD's `.CLK`; **sigma is a continuous dial from fully-external to fully-self-contained through ONE code
   path.** Loader: `GnssConfigLoadNetwork.cpp:2499-2500` (the handoff's `~:5299` has drifted).
3. **`network.rx_code_bias_datum`** (+ `_sigma_m`, default 10 m) — receiver-DCB datum, a loose zero-mean
   prior folded **PRE-REDUCE** through the diagonal prior channel (`ApplicationNetworkAssembly.cpp`, the
   `pl_cb` walk). **A post-reduce cross-station `SumZeroGauge` was TRIED and is BLOCKED** by a correct
   pre-existing exclusion in `NetworkEstimator::solve` (Card-4 DD priors and post-reduce Woodbury gauges
   cannot compose — both fold against the same cached local factors). **DO NOT RETRY.**

Configs: parent `_ext63_gps_dual_1h.json`, children `_ext63_gps_dual_20ep{,_sbc,_a00,_rxcb,_rxcb_loose}.json`,
`_ext63_gps_triple_20ep_rxcb.json`, `_ext63_gps_dual_20ep_el10.json`. The `extends`-depth rule is §6.

### 29.3 Measured — do not re-derive

- **Coverage gates are benign**: `require_atx_coverage` + `require_bia_for_signals` cost nothing on GPS L1/L2
  — **1220 station-epoch blocks, 22560 code channels, 0 skipped.**
- **DEFECT 1 — FIXED.** Removing the satellite code-bias sum-to-zero also removed the only anchor on the
  receiver-DCB common mode. **a00 drift over 20 epochs: -80 (ungauged) → -48 (tighter a00 prior) → -12 (with
  `rx_code_bias_datum`).**
- **Arm 5, `_ext63_gps_dual_20ep_rxcb_loose.json`** (anchor 0.02 → 3.0 m): clock-Δ rms at ep1 **2.994 m**;
  between-satellite std 2.5–3.0 m; **STATIC per-sat offsets G01 +4.5, G26 +4.5, G19 -4.4 m, stable across all
  20 epochs.** **Arm 4's "8 mm clock agreement" was the anchor talking** — but this is estimator
  redistribution, not model error (next bullet).
- **Estimator-free raw-chain check** (`scripts\extcb_raw_if_check.py`): per-satellite product-referenced IF
  code residuals from raw RINEX + COD SP3/CLK/OSB, station-median removed — **16 stations: std 0.33 m, all
  sats within ±0.9 m.** The modeled chain (orbit, clock interp, relativity, Sagnac, PCO, OSB application incl.
  the C1C basis) agrees with COD at sub-metre level. Per-sat clock states under EXT-CB are code-determined
  (free per-(sat,band) phase biases make phase rows clock-blind), so they inherit the iono-channel defect
  through the LS coupling.
- **GIM-referenced GF analysis** (`scripts\extcb_gim_verdict.py`, **30 stations, 1070 LOS**): per-satellite
  GF error vs GIM **std 2.3 TECU** (GIM-error level — satellites CLEAN); receiver GF bias distribution **mean
  -14.8 slant-TECU (≈ -1.55 m ≈ -5 ns of mean C1C−C2W receiver DCB), std 21.5 TECU, range -34..+47,
  geographically clustered** (Europe uniformly ≈ -20..-25, KRGG/OAFA/GAMB +40..+47); **network field vs GIM AT
  ITS OWN IPPs: median -13.5 TECU at ep1** — the deficit is real at the observed IPPs, not an a00-globality
  artifact, and matches mean(rx bias)/⟨m⟩ ≈ -11 in sign and size.
- **Honest clock-agreement number**: model-vs-COD consistency is **±0.3 m or better** (raw IF check,
  dominated by its own tropo/PCV simplifications). **The tight 0.02 m anchor is safe to keep.**

### 29.4 ELIMINATED — do NOT re-test

| hypothesis | how it died |
|---|---|
| satellite GF bias applied with wrong sign | COD's mean (C1W−C2W) OSB over all 32 GPS satellites is **exactly 0.0000 m** (sd 1.396).  A sign error contributes precisely 0 to the network-mean level. |
| frequency mode (dual vs triple) | triple reproduced dual to within **0.1 TECU**.  `require_bia_for_signals` drops C5 (COD has zero GPS C5/L5 entries), so the geometry-free CODE content is L1/L2 either way. |
| mapping-function separability | iono elevation cutoff 25 → 10° did not move the start (12.60) and made it noisier. |
| cold-start artifact | the W6 control's own ignition converges up into band in **2 epochs** and holds. |
| unimported receiver biases | COD **does** publish receiver OSBs (140 stations, 28 of our 61, **4-character** names) and our `ProductParser` rejects them (3-char PRN assumption).  But for GPS **56 of 57 entries are exactly 0.0000** — COD's convention puts the receiver GPS group delay in their receiver CLOCK (header `RECEIVER_CLOCK_REFERENCE_GNSS G`, `SATELLITE_CLOCK_REFERENCE_OBSERVABLES G C1W C2W`).  Not the cause. |
| SH representation error (deg-8 too coarse) | arm 8 (`_ext63_gps_dual_20ep_d15.json`, deg 15, single knob): ignition 12.78 → **6.18** (wrong direction), **walk to -13.9 unchanged**, deg-1 rms 30–40 TECU in BOTH arms (control 8–12), dipole unchanged.  The deg-15 field at 61 sta RINGS pointwise (field-vs-GIM at BSHM's own IPPs **-446 TECU at ep1**) — keep deg-8; deg-15 stays OFF at 61–63 stations. |
| code-OSB sign/basis error on the corrections path | per-sat loose-arm clock residuals vs every `.BIA` signature (flip signature **2·[IF(C1C,C2W) − IF(C1W,C2W)]**, all DCB variants): **\|r\| ≤ 0.05**.  COD's IF(C1W,C2W) OSB content is exactly 0 per satellite. |
| a-priori clock time-tag error (leap-second / off-by-one) | **corr(residual, af1·c) = 0.03**; COD clock drifts ~5e-12 are three orders too small for ±4.5 m. |
| relativistic eccentricity term mis-applied | **corr(residual, -2r·v/c) = −0.30, slope -0.20** (a chain mismatch predicts slope ±1, r ≈ ±1; G26: rel ≈ 0.03 m but residual +4.5 m). |
| any metre-level modeled-chain defect | estimator-free per-sat IF residuals: **std 0.33 m**. |

### 29.5 ROOT CAUSE (2026-08-04, measured)

*(Cited by name from `config/test_suite/_ext63_gps_dual_20ep_a00gim.json`.)*

**The receiver C1C−C2W GF hardware biases of the 61 stations have a nonzero network mean (≈ -5 ns ≈ -1.55 m of
GF code ≈ -15 slant-TECU) and continent-scale clustered structure (std ≈ 21 TECU: Europe uniformly ≈ -22,
southern outliers +40..+47). The `rx_code_bias_datum` zero-MEAN convention forces Σ rx-DCB = 0, so:**

- the true **MEAN** cannot live in the rx-DCB states → it lands in the confounded direction, the **field
  level**: a00 low by mean(b)/⟨m⟩ ≈ **11–15 TECU** (measured field−GIM at IPPs: -13.5);
- the **clustered SPREAD** is per-station-absorbable in principle, but the field's spatial coherence couples
  same-biased neighbours and the leak into the low degrees is the **30–40 TECU deg-1 dipole** (control: 8–12);
- the code rows then drag the code-determined **clock states** (the loose arm's ±4.5 m) — **satellite-tagged
  in appearance, receiver-caused in origin.**

**Why the W6 self-contained control gets the level right with the SAME confound**: it arms no rx datum, so the
weak-but-correct mapping-spread channel sets the level (**23–28 TECU**, GIM-band). EXT-CB arm 2 (no datum)
showed that channel cannot hold against the leveling/carry dynamics — a00 walked to -80 with collapsing sigma.
The zero-mean datum (arm 4) stops the walk but pins the WRONG VALUE: **convention ≠ physics by exactly the
rx-bias mean.** Both symptoms and the residual -1 TECU/ep walk are consistent; the walk mechanism
(leveling-era ratchet pumping the confounded direction) is the one piece still inferred rather than measured.

### 29.6 The fix and its validation, arms 9–13 — including the L2-ESCAPE

The level defect is an S-basis realization mismatch, so the fix is a FRAME CHOICE: adopt COD's iono level via
the existing **per-epoch** mean-reverting a00 anchor — `networkInjectShVtecPrior`
(`src/core/app/NetworkRunContext.cpp:1419`), which folds the a₀₀ climatology target (`lay.mean_prior_tecu`,
`NetworkRunContext.cpp:1480`) EVERY epoch in information space, so it stands against the walk; an initial-only
prior would not. *(This SPEC previously anchored the fold at `NetworkRunContext` ~:1309 — that line is
snapshot serialization; the anchor was wrong.)*

- **Arm 9** (`_ext63_gps_dual_20ep_a00gim.json`, `mean_vtec_prior_tecu: 32.24`, sigma 2, 20 ep): ROOT CAUSE
  CONFIRMED BY RESPONSE — L1 a00 ignited **28.3** (predicted 28–33; arm 4: 12.78), `a00_in_band` 20/20 (arm 4
  left at ep 8), walk saturated ~21 instead of running to -12. **But the confounded direction ESCAPED into the
  UNANCHORED layer-2 level** (default anchor 3±5 = 0.04 info/ep): **L2 a00 dived -15.9 → -32.8**, TOTAL level
  still defective, SH health gate rightly withheld **12 epochs** on the SUMMED band. Rx-CB carry innovation
  gate withheld 710/1219 priors. **Generalizable trap: the tension re-routes to the loosest remaining
  level-like direction** — arms 2–4 the L1 field, arm 9 the L2 field.
- **Arm 10** (`_ext63_gps_dual_20ep_a00gim2.json`, arm 9 + `layer2_mean_vtec_prior_sigma` 5 → **0.5**, 20
  ep): closed the L2 escape (L2 flat 2.0–2.6), **first 0/0/0 sh_health on the ladder**. With both escapes shut
  the pump discharged into L1 alone (ignition 23.9, sag to ~7 by ep 16), then both defect curves inflected:
  a00 up 7.06 → 8.70 over the last 5 ep, deg-1 dipole peaked ep 6 (39.5) and decayed (→ 30.3) — the transient
  signature.
- **Arm 11** (1 h): the equilibrium-flatline reading was FALSIFIED — a00 recovered from the ignition dip
  (**8.3 at 10 min**) to **20.2 by 30 min**, then BREATHED (**second dip 9.2 at ~45 min, 21.3 rising at run
  end**); **deg-1 halved 39.5 → 18.6, Kaula ratio 4.0 → 1.86**, and **deg-1 SWELLS during level dips** — field
  sloshing between level and low degrees on the ~20–30 min constellation-sweep timescale while the rx-DCB
  harvest completes (fold fraction 43 %). sh_health 0/0/0, in-band 121/121, total level at end 23.9 vs GIM
  32.24.
- **Arm 12** (`…_a00gim_sl`, 20 ep; user direction: kill layer 2, single-layer deg-8): reproduces arm 10's
  dynamics (ignition 24.7, dip ~6.1 at ep 15, inflection up, gate 0/0/0) with a **CLEANER dipole (peak 34.6 vs
  39.5, end 26.1 vs 30.3) — even pinned, the L2 shell contributed slosh room.** **CANONICAL EXT-CB POSTURE:
  single-layer deg-8 + a00 GIM anchor (32.24±2) + `rx_code_bias_datum`** — frame-commensurate with the
  single-shell GIM reference. Dual-layer stays on the self-contained lineage where Iono-2 mech-A earned it.
- **Arm 13** (1 h single-layer canonical): same trajectory family as arm 11 (ignition 24.7, trough 7.1,
  recovery to 22.4 by 30 min, second breath to 10.1 at ~45 min, ends 20.95 rising), gate 0/0/0, in-band
  121/121, deg-1 peak shallower (31 vs 35). **Canonical posture validated at 1 h.** Prediction miss, noted
  honestly: rx-CB carry fold fraction stayed 43 % (predicted rising).
- The **deg-1 dipole survived every level fix as predicted** (it is the clustered rx-bias SPREAD, not the
  mean). **Rejected alternative, recorded once**: a non-zero-sum TARGET for `rx_code_bias_datum` — same effect
  as the a00 anchor, uglier plumbing.

### 29.7 Arm 14 — the level's information budget is ACCIDENTAL

**Arm 14** (`…_a00gim_sl_nc`, 20 ep = arm 12 + `carry_rx_code_bias: false`, the pump-attribution
discriminator): the flywheel prediction was WRONG in an informative way. a00 **collapsed 24.5 → -3.5 in ONE
epoch**, free-fell at ~-9 TECU/ep and saturated at **−37..−39** (out-of-band from ep 2, all 19 emissions
withheld — the gates work). **So the rx-CB carry was never the pump: its stiffness (q=1e-9 on per-station
DCBs) was the accidental crutch holding the level↔rxDCB↔field-null degeneracy closed.** Carry ON = frame
memory + breathing (arms 11/13); carry OFF = raw collapse. Neither is a designed equilibrium.

The handoff's collapse-gradient explanation (per-LOS stiffness toward the bias-in seed frame at ~0.04 info ×
376 LOS ≈ 15 units) is **the model §10.1 FALSIFIED** — arm 16 reproduced arm 14 to 0.006 TECU with LV-1 armed.
What survives is the localization observation: the deg-8 basis lets the SUPPORTED region fit whatever frame
the data implies while the UNSUPPORTED hemisphere extrapolates under Kaula, and a00 (a GLOBAL mean) lands
wherever that extrapolation sends it (-37 with the dipole at 23–30). The W6 control arms the SAME carry
(verified in `runs\agu63\run_console.log`) — the crutch-plus-degeneracy structure is live there too, buffered
by 32 free satellite DCB states into sub-band a00 wander. **The EXT-CB ladder did exactly what it was built
for: removed absorbers until the structural defect surfaced.**

### 29.8 Known-latent, out of scope: the 4-char receiver-OSB card

**`ProductParser` cannot read COD's receiver OSB records** (3-char PRN assumption vs 4-char station names).
**Inert for GPS-only because those entries are zero, but load-bearing the moment EXT-CB goes multi-GNSS —
Galileo E1X and BeiDou C2I receiver OSBs run -3.4 to -3.9 ns.** Worth its own card; it also blocks §17 item 0b
(the CAS MGEX receiver-DCB rung).

---

