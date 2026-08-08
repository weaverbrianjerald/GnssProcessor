# SPEC — the ionosphere-level information budget (LV-1/LV-2/LV-3)

Status: **BUILT (LV-1…LV-5) AND GRADED TO 12 h, 2026-08-05.  DO NOT ARM THE STACK
FOR SETTLED WORK — read §14 first.**

- §9 implementation record · §10 first outcomes · §11 LV-4 · §12 LV-5 · §13 the
  mechanism correction · **§14 the 12 h verdict, which reverses the 1 h one** ·
  §15 the one-knob fix for §14, tested and FALSIFIED · §16 review addendum (its
  §12.3 superseded in place by §13) · §18 the residual is NOT an extrapolation
  artifact (footprint-restricted: ~2× LARGER where we observe) and
  `rx_cb_carry_q` is second-order · §17 the roadmap ·
  §19 the roadmap's item 1 falsified at stage 1 and its item-2 audit
  re-attributing the whole ladder to LV-4 · §20 arm 30 + the first 303-station
  arm · §21 the 61-station mask 2×2 · §22 that mask trade FLIPS SIGN at 303
  stations ·
  **§23 ITEM 1 RE-RUN ON THE CURRENT DENOMINATOR — §19's stage-1 FAIL does NOT
  reproduce, and LV-4's data-side cost is a FUNCTION OF THE MASK (+64 % at obs
  10 / +35 % at obs 15 / +16 % at obs 30).  A continuing session starts THERE.**
  ⚠ Sections were appended by concurrent sessions and are NOT in numeric order
  on the page (13, 16, 14, 17, 15, 18, 19, 20, 21, 22, 23).  Read §23, then §22
  for the mask regime, then §17/§19 for what of them still stands.
- Everything is loader-default OFF and armed per-config.  LV-3's contract is
  `docs/CONTRACT_level_authority.md`; its optional BRDC knob is deferred with its
  prerequisite verified.
- Three of this document's own claims were falsified by its own arms and are
  corrected in place: §1's seed-frame gradient (§10.1), §4's "the de-mean frees the
  common mode" (§13), and the 1 h grading that made the stack look like a fix
  (§14).  **Do not quote §1, §3 or §4 without the correction that follows them.**

Original status line: **ready to implement** (handed to an implementing session, 2026-08-05).
Provenance: the EXT-CB external-product ladder, arms 2–14 (2026-08-04) —
`docs/HANDOFF_ext_cb_iono_level.md` carries the full measured record. Memory:
`project_extcb_defect2_rootcause`. Self-contained below; read `CLAUDE.md` first
(build pattern, Release-only runs, seam decision-trees, commit rules).

## 0. Rigor level for THIS initiative (user directive, 2026-08-04)

**The user has explicitly waived the byte-identity / golden-certification ceremony for
this work: new results sooner.** Concretely:

- Implement DIRECTLY on the shared path where that is the natural shape. No structural
  byte-identity proofs, no same-build golden A/B gates, no promotion ledger rows.
- Config knobs still exist, but as **A/B experiment switches**, armed immediately in the
  EXT-CB configs. Loader defaults stay `false` ONLY because flipping loader defaults
  invalidates OTHER lineages' stored-snapshot fingerprints (concurrent sessions resume
  warm chains). That is fingerprint hygiene, not certification.
- What is NOT waived (these are how we read the results, and correctness):
  - **Invariant #1**: every seed/carry change must behave correctly at slip,
    re-allocation and gap events — state what happens at each edge.
  - **Invariant #4**: run-end counters for every new mechanism. "0 reframed / 0
    demeaned" must be visible, never silent.
  - Stated predictions in each validation arm's `_comment` BEFORE the run.
- Unit tests: only where the arithmetic can silently be wrong (LV-2's weighted mean,
  LV-1's fallback). No off-path structural tests required.

## 1. The problem, in one paragraph

The network's ionosphere LEVEL (SH a00, and with it the whole GF frame: level ⊕
receiver-DCB common mode ⊕ per-LOS common content) is held by an **accidental**
information budget, not a designed one. The physical separator (mapping-function
spread) is weak; the designed authorities are feeble (a00 climatology anchor 0.25
info/ep at σ=2 TECU; rx-DCB zero-mean datum 0.01 on the common mode, N-independent);
and the thing that actually holds the level up in practice is the **rigidity of
unrelated stiff carries** (the rx-CB carry at q=1e-9 m²/s). Meanwhile the per-LOS iono
seeds are born in a **bias-in frame** (receiver DCBs ride into every seed), so the
per-LOS ensemble pulls the field toward that frame with ~15 info units (376 LOS ×
0.04). Result, all measured on DOY-129 / 61-station GPS EXT-CB arms:

| arm (runs\…) | posture | a00 behaviour |
|---|---|---|
| arm 2 (no rx datum) | ungauged | walk to **−80** in 20 ep, σ collapsing |
| arm 4 `ext63_dual_20ep_rxcb` | zero-mean rx datum | ignites **12.78** (GIM: 32.24), walks to −12 |
| arm 8 `ext63_dual_20ep_d15` | + SH deg 15 | ignites **6.18** — representation FALSIFIED |
| arm 9 `ext63_dual_20ep_a00gim` | + a00=32.24±2 | L1 holds 28→21, tension **escapes into unanchored L2** (−33) |
| arm 12/13 `…a00gim_sl` (canonical) | single-layer + anchor | ignition 24.7, trough ~7, recovery to ~21-22, **breathing ±6 TECU @ 20-30 min**, gates clean |
| arm 14 `…a00gim_sl_nc` | + carry_rx_code_bias OFF | **collapse −9 TECU/ep to −37..−39** — the carry is a load-bearing crutch |

Estimator-free ground truth (`scripts/extcb_raw_if_check.py`,
`scripts/extcb_gim_verdict.py`): the raw chain is clean (per-sat IF ±0.3 m, per-sat GF
vs GIM ±2.3 TECU); the receiver C1C−C2W biases have **mean ≈ −5 ns (−15 slant-TECU)
and continent-clustered spread (std ≈ 21 TECU)**. The self-contained W6 control
(`runs\agu63`) arms the SAME rx-CB carry and the same seed path — buffered there by 32
free satellite code-bias states into sub-band a00 wander, i.e. the structure is live in
the production posture too.

What is PROVEN vs INFERRED: the phenomenology above and the information arithmetic are
measured. The precise gradient decomposition (per-LOS-ensemble seed-frame stiffness as
the dominant collapse driver) is the one consistent model standing, but it has not been
instrumented directly — LV-1's counters (below) make it directly observable, and its
validation arm falsifies it cheaply if wrong.

## 2. Design principle (applies to all three cards)

**Separate estimable content from frame content.** Between-station differences,
per-LOS structure, field shape = estimable → carries and seeds may transport them.
Common modes (level, rx-DCB mean) = frame → exactly ONE designed authority per posture
owns each, refreshed per epoch, with NO memory of a previous realization riding in
through a side channel. Per posture the level authority is:

- **EXT-CB ladder**: the a00 GIM anchor (`estimation.global_vtec_sh.mean_vtec_prior_tecu:
  32.24`, `…_sigma_tecu: 2.0`) — already validated (arms 12/13).
- **Self-contained (W6)**: the satellite code-bias sum-to-zero gauge — it realizes the
  same convention the IONEX community uses (zero-mean satellite DCBs), which is WHY the
  control's level lands GIM-compatible with no external data. No change needed; LV-3
  documents it.

## 3. Card LV-1 — bias-free per-LOS iono seeding  (PRIMARY)

### Current behaviour

`StateInitializer::computeIonosphereSeed` (`src/core/measurement/StateInitializer.cpp:263`)
seeds each per-LOS iono state at allocation from the GF of product-referenced code
residuals: `iono_seed = ((P1−mod1) − (P2−mod2)) / (alpha1−alpha2)`. `mod` contains the
satellite side and geometry but NOT the receiver DCB, so the receiver's
(d_r,band1 − d_r,band2)/(α1−α2) rides into **every seed of that station** — the bias-in
frame. Its own comment (`:275`) notes the seed propagates into per-row predicted values
and downstream sat-bias/ambiguity seeds, so the frame contamination spreads at birth.
This runs at cold start AND at every re-allocation (slip refloat, gap re-birth) — which
is exactly why the fix must live inside this function, not at a call site.

### Change

Inside `computeIonosphereSeed`'s network use (gate on
`ProcessingMode::PPP_RTK_Network` — user PPP has no field to reframe against), replace
the station-common content of the seed with the field's own value at the IPP:

```
seed'_los = seed_los − station_offset_r + field_slant(IPP_los)
```

- `station_offset_r` = the **median over this station's per-sat seeds this epoch** (the
  robust station-common content: rx GF bias + median slant iono). Median, not mean —
  single-sat outliers must not move it. Computed once per station per allocation epoch;
  needs ≥ `min_sats` (default 5) seeds, else **fallback to the raw seed** and count it.
- `field_slant(IPP)` = m(el) × the SH field's current value at the LOS's IPP (the
  producer already computes IPPs and the field basis; at true cold start — epoch 1, no
  field yet — the field evaluates to the a00 prior mean, which is precisely the designed
  authority doing its job).
- Practical shape: `computeIonosphereSeed` cannot see other satellites (it is
  per-observation). Do the reframe one level up where the epoch's seeds are gathered
  per station (the allocation walk that calls it), or give StateInitializer a
  per-epoch, per-station cache: collect raw seeds first, compute the median once,
  reframe all. Keep the `|seed| < 200 m` sanity check on the RAW seed (a garbage
  channel must still be rejected before it enters the median).

### Events (invariant #1)

- Re-allocation mid-run (slip refloat, gap re-birth): same path runs; the median is
  taken over THAT epoch's live seeds of the station; the field is warm — the re-born
  LOS inherits the current frame, not the raw bias-in frame. This is the required
  event response and it falls out of putting the fix inside the seed path.
- Station appearing with < min_sats: raw-seed fallback + counter (`lv1_fallback`).
  Acceptable: a handful of bias-in LOS cannot move the field.

### Knob, counters, trace

- `network.iono_seed_field_frame` (bool, loader default false, **armed in every EXT-CB
  config from day one**; parse next to the other network toggles in
  `src/core/config/GnssConfigLoadNetwork.cpp`, mirror into `NetworkKnobs`).
- Run-end counters: `lv1_seeds_reframed`, `lv1_fallback_raw`, and per-station at cold
  start ONE log line. **Log the FIELD-REFERENCED offset**, not the raw median:
  `station_offset_r − median_los(field_slant(IPP_los))` — both terms are already
  computed by the reframe itself. The raw median alone is rx-bias PLUS the station's
  median slant iono (~20-40 slant-TECU) and would read as a mismatch; the
  field-referenced version is a direct estimate of the station's rx GF bias and must
  agree station-by-station (± a few TECU) with `scripts/extcb_gim_verdict.py` column
  `bias_TECU` (the fleet table: MAC1 −33.6 … KRGG +45.9). That agreement is a free
  cross-validation of the whole card; a systematic disagreement means the reframe is
  subtracting the wrong quantity — stop and check units/mapping before running arms.

### Validation arms + predictions (write these into the configs before running)

1. **`_ext63_gps_dual_20ep_a00gim_sl` + LV-1** (carry ON): ignition trough shrinks
   materially (arm 12/13 dipped to ~6-7; predict trough ≥ 15), deg-1 peak < 30
   (arm 12: 34.6), breathing amplitude reduced in a 1 h follow-up.
2. **THE DECISIVE ONE — `…a00gim_sl_nc` + LV-1** (carry OFF, the arm-14 posture): if
   the mechanism model is right, the −9 TECU/ep collapse largely DISAPPEARS without
   the carry crutch (predict: a00 stays > 10 through 20 ep, in-band). If it still
   collapses, the model is wrong — STOP, instrument the level-direction information
   decomposition before touching anything else.
3. Self-contained spot-check (optional, cheap): `_agu63_nolane_30ep` + LV-1 — a00
   ignition and the per-station offsets logged vs the known-good control trajectory.
4. **Bonus readout, campaign-relevant (measure, do not gate on it)**: DD fractional
   parts on a settled ≥1 h run with LV-1(+LV-2) armed. The bias-in seed frame puts a
   PERSISTENT, fleet-structured error into every ambiguity born under it (the seed's
   own comment documents the propagation; receiver biases cluster by manufacturer
   fleet — measured range −12..+16 ns, up to ~70 slant-TECU between mixed-hardware
   stations), and the standing record attributes 99.3 % of NL fractional variance to
   iono. Prediction: `med|frac_cyc|` moves off the uniform-0.25 signature. Even
   partial structure emerging confirms the channel; no change = the AR blocker is
   elsewhere and this SPEC's scope does not grow.

## 4. Card LV-2 — rx-CB carry common-mode de-mean

### Current behaviour (anchors)

- Inject (parallel produce, per station):
  `src/core/app/ApplicationNetworkAssembly.cpp:2458-2540` — each carried rx-DCB folds a
  diagonal prior toward its harvested ABSOLUTE value; `rxCbInjectGate` (10σ + 0.5 m
  guard, `var_eff = var + q·Δt`, q = 1e-9 m²/s) may skip (counted `pl_cb_innov_skips`).
- Harvest (serial, post-solve): `src/core/app/NetworkRunContext.cpp:2171-2254` —
  `rx_cb_carry[{station, key}] = LocalCarry{value_abs, var, now}`.
- Measured: at var ≈ 1e-2 m², the carried set puts ~**100 info units on the common
  mode** vs the datum's 0.01 — the carry, not the datum, owns the frame, and it owns it
  with LAST epoch's realization (memory). Skip rate ~58 % in the EXT-CB arms.
- Arm 14 proves the DIFFERENCE rigidity is load-bearing (collapse without it). So:
  keep the carry, remove only its net common-mode force.

### Change

Lag-1 weighted de-mean, race-free within the existing produce/harvest split:

1. Produce phase (per station, where the fold already happens): accumulate into the
   station's `StationAssembly R` two new sums over its FOLDED rx-CB rows:
   `pl_cb_sum_lam_innov += λ·innov`, `pl_cb_sum_lam += λ` (per constellation if
   multi-GNSS; GPS-only today — keep the map keyed by constellation from day one).
2. Serial harvest: sum across stations → `μ_c = Σλ·innov / Σλ` per constellation; store
   in `ctx.carry` (add the field to `NetworkCarryState`).
3. Next epoch's inject: `η_i = λ_i·(innov_i − μ_c)` instead of `λ_i·innov_i`, and the
   gate judges `|innov_i − μ_c|` (a coherent frame move must not mass-trigger the
   receiver-reset response — the current 58 % skip rate is suspected to be exactly
   this pathology; the skip-rate drop is a headline result either way).
4. Λ (the information weight) is `1/var_eff` — the same quantity the fold uses.

By construction Σλ(innov−μ) = 0: the carry exerts zero net common-mode force while
transporting differences at full stiffness. Do NOT implement the "demeaned target"
variant (pull every state toward mean-free absolute values): that converts the carry
into a STIFF zero-mean datum (~100 info) which would import the −5 ns convention
offset into the level at 400× the designed datum stiffness — the original DEFECT 2,
hardened.

### Events (invariant #7)

- μ is lag-1: on the FIRST epoch (no μ yet) inject un-demeaned (μ=0) — one epoch of
  legacy behaviour, fine.
- After a mass re-seed / RAIM re-reduce epoch where few rows folded (`Σλ` small): if
  `Σλ` is below a floor (say, fewer than 8 stations contributed), freeze μ at its
  previous value rather than trusting a thin mean. Counter: `lv2_mu_frozen`.
- Trace: μ_c per epoch (one value; append to an existing per-epoch trace line or the
  events CSV — do not create a new stream for one scalar). μ should sit near zero in a
  healthy run and swing during frame moves; it is the direct observable of the
  flywheel this card removes.

### Knob

`network.rx_cb_carry_demean` (loader default false, armed in EXT-CB configs). It
changes carried-state meaning mid-chain, so append it to the snapshot fingerprint
**conditionally when armed** — copy the exact pattern at
`src/core/app/NetworkRunContext.cpp:494-498` (`rxcb:1` conditional append).

### Validation arms + predictions

1. `_ext63_gps_dual_20ep_a00gim_sl` ± demean (carry ON, single knob): skip rate
   (`pl_cb_innov_skips` / harvests) collapses from ~58 %; ignition trajectory similar
   or mildly improved (LV-2 targets the BREATHING, not ignition).
2. 1 h follow-up of the winner: breathing amplitude (the ±6 TECU @ 20-30 min cycle of
   arms 11/13) damps materially; carry fold fraction rises from 43 %.
3. LV-1 + LV-2 together on the 1 h: the combined posture is the candidate to re-open
   the 305-station gate.

## 5. Card LV-3 — the level authority, explicit  (mostly documentation; one OPTIONAL knob)

1. **No code change for the frames themselves**: EXT-CB keeps the a00 GIM anchor;
   self-contained keeps the W6 satellite sum-to-zero. Write the frame contract into
   the product/consumer docs (which S-system the emitted biases/level are realized in —
   the handoff §ROOT CAUSE has the text to adapt).
2. **OPTIONAL — `network.a00_prior_from_brdc`**: compute the a00 target from the
   broadcast Klobuchar coefficients already present in the ingested navigation data
   (area-weighted global mean of the Klobuchar vertical delay, converted to TECU at
   L1; a small deterministic function — no download, operationally self-contained),
   applied through the existing `mean_vtec_prior_*` channel at LOOSE sigma (10 TECU).
   Rank LAST: only worth doing if ignition speed still matters after LV-1/LV-2, and
   only for postures without the GIM anchor. Verify first that the RINEX nav parser
   retains the ION ALPHA/BETA header records; if it does not, that parse is part of
   this card.

## 6. Build / run mechanics for the implementing session

- Build (the user's shell does NOT source vcvars):
  ```powershell
  $vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  cmd /c "`"$vcvars`" >nul && cmake --build C:\GnssProcessor\build\x64-Release --target gnss_processor gnss_tests 2>&1"
  ```
- **Release only for data runs; run a RENAMED exe copy** (e.g. `gnss_processor_opus.exe`)
  so concurrent rebuilds never hit a locked exe. Run with **cwd = the run directory**
  (`runs\<arm>\`) so `logs\` lands inside it; redirect console to `run_console.log`.
- New `.cpp` files (none expected — all three cards edit existing files) would need
  manual `CMakeLists.txt` entries.
- Configs: new arms extend `config\test_suite\_ext63_gps_dual_1h.json` DIRECTLY (the
  chain is at depth 7 of the loader's 8 limit — never extend its children). Copy the
  `_comment`-with-prediction house style from `_ext63_gps_dual_20ep_a00gim_sl.json`.
- Read results from `runs\<arm>\logs\network_vtec_sh_health.csv` (columns `a00_tecu`,
  `a00_in_band`, `rms_deg1_tecu`, `kaula_ratio_*`) and the run-end counter lines in
  `run_console.log`. Cross-grade any level claim with
  `scripts\extcb_gim_verdict.py` (edit its two hardcoded run paths as needed).
- Smokes are ~4 min wall; the 1 h arms ~10 min. Smoke first; 1 h only when the smoke
  matches its stated prediction.

## 7. Do NOT (falsified or blocked — do not rediscover)

- Do NOT turn `carry_rx_code_bias` off as "the fix" — arm 14: −9 TECU/ep collapse.
- Do NOT retry a post-reduce cross-station `SumZeroGauge` for the rx datum —
  structurally blocked against the Card-4 DD priors (`NetworkEstimator::solve`
  refuses; the pre-reduce diagonal channel is the sanctioned route).
- Do NOT chase SH degree/representation for the level — falsified (arm 8), and deg-15
  RINGS at 61 stations (pointwise −446 TECU vs GIM at ignition).
- Do NOT tighten the rx-DCB datum sigma to "hold" the frame — stiff zero-mean imports
  the measured −5 ns convention offset (DEFECT 2 again, harder).
- Do NOT add external-data dependence to the self-contained posture (GIM stays
  diagnostics-only there; the GIM anchor is EXT-CB-ladder-only).
- The satellite side and the modeled chain are CLEAN (verified estimator-free) — any
  new level anomaly is not in orbits/clocks/OSB application; do not re-audit them.

## 8. File-touch map

| file | card | change |
|---|---|---|
| `src/core/measurement/StateInitializer.cpp` (`:263`) | LV-1 | seed reframe (median + field-at-IPP), fallback, counters |
| `src/core/app/ApplicationNetworkAssembly.cpp` (`:2458-2540`) | LV-2 | Σλ·innov / Σλ accumulation; demeaned η and gate |
| `src/core/app/NetworkRunContext.cpp` (`:2171-2254`, `:494`) | LV-2 | μ_c at harvest; `NetworkCarryState` field; fingerprint append |
| `include/app/NetworkRunContext.hpp` | LV-1/2 | knobs mirror + carry-state field + counters |
| `src/core/config/GnssConfigLoadNetwork.cpp` | LV-1/2/3 | knob parsing + config-echo log lines |
| `include/config/GnssConfigTypes.hpp` | LV-1/2/3 | knob fields |
| `config/test_suite/_ext63_gps_dual_20ep_*` (new arms) | all | validation arms with predictions |
| `tests/test_codebias_network_config.cpp` (or a new file) | LV-2 | weighted-mean arithmetic + μ-freeze floor unit tests |
| `docs/HANDOFF_ext_cb_iono_level.md` | all | outcome lines per arm after each run |

Suite hygiene if adding tests: one suite name per file, prefer a new file over
widening an existing suite.

---

## 9. Implementation record (2026-08-05)

Built, default-OFF, armed in the new arms.  Deviations from §8 are noted.

| what | where |
|---|---|
| LV-1 median offset + reframe + counters + the one-shot per-station offset log | `StateInitializer::{stationSeedOffset, computeStationSeedOffset, applyIonoSeedFrame}` (+ `IonoSeedFrame` / `IonoSeedFrameStats`), consumed in `MeasurementModel::ensureRequiredStates`'s iono block (**consume-once**: the frame is disarmed after the block, so a stale field can never seed a later epoch) |
| LV-1 field sampling at the IPP | `networkShSlantAtLos_` (file-local, `ApplicationNetworkAssembly.cpp`), called per station in the PASS-B produce loop before `computeMeasurementUpdate` |
| LV-1 lag-1 field snapshot | `ctx.carry.sh_field_lag1`, written serially post-solve in `networkHarvestLocalCarries`; cold start falls back to the a₀₀ climatology prior mean (Ȳ₀₀ ≡ 1 under the orthonormal basis) |
| LV-2 Σλ·innov / Σλ accumulation + demeaned η and gate | `ApplicationNetworkAssembly.cpp` rx-CB inject block |
| LV-2 μ reduction (pure helper `rxCbDemeanMu`) + freeze counter | `NetworkRunTypes.hpp` (next to `rxCbInjectGate`), called in `networkHarvestLocalCarries` |
| LV-2 μ trace | one `lv2 / rx_cb_demean_mu` row per family per epoch on the EXISTING `network_events.csv` (`networkLeveling`), with `action` = fresh/frozen and `detail` = family;n_sta;sum_lambda |
| knobs | `network.iono_seed_field_frame` (+`_min_sats`, default 5), `network.rx_cb_carry_demean` (+`_min_stations`, default 8) — parsed in `GnssConfigLoadNetwork.cpp`, mirrored into `NetworkKnobs`; **both live in `GnssConfig.hpp` members/getters, not `GnssConfigTypes.hpp`** (that is where every sibling network toggle lives) |
| fingerprint | `rxcbdemean:<min_sta>` nested inside the existing conditional `rxcb:1`; `lv1seed:<min_sats>` conditional — pre-card snapshots unchanged |
| run-end counters | `NetworkTeardown.cpp`: an LV-1 line (reframed / offsets / both fallbacks / field refreshes) and an LV-2 line (μ epochs / freezes / last+max μ / skip rate on the demeaned innovation), each WARNing when its mechanism is armed-but-inert |
| tests | `tests/test_level_information_budget.cpp`, suite `LevelInformationBudget` (13 cases): median vs mean robustness, the min_sats fallback, μ information-weighting, **Σλ(innov−μ) ≡ 0 and difference-preservation as properties**, the freeze floor, and the loader's inert-safe forcing of both knobs.  Full suite 1264 pass / 1 documented skip. |

Two design choices worth stating because they differ from the letter of §4:

- **μ is keyed by receiver code-bias STATE KEY** (`GPS_DCB`, `GPS_IFB5`), not by bare
  constellation.  The key already carries the constellation, and averaging a DCB
  innovation with an IFB innovation would mean averaging different physical
  quantities.  In the GPS dual-frequency posture the two keyings coincide.
- **Only FOLDED rows vote** on μ (a gate-withheld row exerts no force, so it must
  not vote on the force being removed), and the mean is taken over the RAW
  innovation (using the already-demeaned one would decay μ geometrically to zero).

LV-3's optional `network.a00_prior_from_brdc` is **deliberately NOT built** (the
card ranks it last, conditional on LV-1/LV-2).  Its stated prerequisite is
VERIFIED: the RINEX-nav ION ALPHA/BETA records are retained
(`ProductParser::parseKlobuchar` → `ProductCache::getKlobucharCoeffs()`, with
`Ionosphere::loadKlobucharCoeffs` as a header-direct fallback; both normalize the
Fortran `D` exponent).  What remains is not parsing but a runtime-override design:
the coefficients exist only after product load, so the a₀₀ target cannot be set at
config-load time.  See `docs/CONTRACT_level_authority.md` §6.

## 10. Measured outcomes — and the mechanism correction (2026-08-05)

All arms: DOY-129, 61 GPS stations, single-layer deg-8 + a₀₀ GIM anchor 32.24 ± 2
+ `rx_code_bias_datum`, Release, one knob at a time.  Reference: COD GIM 32.24 TECU.

### 10.1 §1's gradient model is FALSIFIED — the seed cannot pull the level

§1 attributes the collapse to "per-LOS stiffness toward the seed frame ≈ 0.04 info
× 376 LOS ≈ 15 units".  **That channel does not exist on this path.**  The
per-station state vector is a frozen LINEARIZATION POINT, the per-epoch absolute
solve carries no Kalman prior on the local nuisances, and every row on a GPS iono
column forms its RHS against `x_lin` with that row's own `A_r` coefficient — so a
seed shift leaves the reduced RHS exactly invariant (2026-08-01 theorem, memory
`project_network_xlin_invariance`).

The card's own decisive arm settles it empirically, and it is the arm the spec said
to STOP on:

| arm | posture | a₀₀ over 20 ep |
|---|---|---|
| 14 `…a00gim_sl_nc` | carry OFF | 24.5 → −3.5 → −37.1 |
| **16 `…lv1_nc`** | carry OFF **+ LV-1** | 24.5 → −3.5 → −37.1 |

Endpoints agree to **0.006 TECU**, max |Δ| 1.8 TECU mid-collapse (a near-null
direction amplifying ulp differences).  With the carry off, LV-1 has no actuator.

### 10.2 …but LV-1 helps anyway, through a DIFFERENT and defective channel

With the carry ON, the same single knob moves the level a lot:

| arm | knobs | ignition | trough | end | mean | in-band | gate |
|---|---|---|---|---|---|---|---|
| 12 (base) | — | 24.69 | **6.05** | 7.10 | 13.28 | 20/20 | 0/0/0 |
| 15 | LV-1 | 24.69 | **13.81** | 14.78 | 18.35 | 20/20 | 0/0/0 |
| 17 | LV-2 | 24.69 | **13.81** | 19.56 | 19.99 | 20/20 | 0/0/0 |
| 18 | LV-1+LV-2 | 24.69 | **19.09** | 19.09 | 25.80 | 20/20 | 0/0/0 |

The channel is the **rx-CB carry's inject gate**, which thresholds the PREFIT
quantity |value_abs − x_lin(cb)| — and `initializeCodeBiases` derives x_lin(cb) by
subtracting the *seeded* iono, so the seed frame decides which carried priors fold
(arm 15: folds 448 → 382, skips 710 → 776).  Withholding those particular folds
raises the ignition trough by 7.8 TECU.  That is a **row-set** actuator (the one
class the invariance theorem leaves open), riding on a screen that thresholds a
frozen arbitrary frame — a defect, not a design.  Named in
`docs/CONTRACT_level_authority.md` §4 for whoever fixes the gate.

### 10.3 LV-2 works as designed, and the 1 h window is where it shows

| arm | window | a₀₀ mean | min | end | fold fraction | in-band |
|---|---|---|---|---|---|---|
| 13 (base) | 1 h | **16.36** | 5.42 | 20.95 | 43.5 % | 121/121 |
| 19 (LV-1+LV-2) | 1 h | **35.09** | 18.38 | **33.08** | **51.9 %** | 121/121 |

Against GIM 32.24: the baseline hour sits **−15.9 TECU low** (DEFECT 2, exactly the
measured receiver-bias mean), the combined posture **+2.85 high on average and
33.08 at the end** — the level defect is substantially CLOSED.  Fold fraction rose
43.5 % → 51.9 % (skips 4127 → 3511), so the 20-epoch reading that skips got *worse*
was an ignition artifact; judge the gate on the settled window.

μ itself (traced per epoch, `network_events.csv`, tier `lv2`) is the flywheel
draining: it swings ±1.8 m over the first ~20 min and damps to ±0.8 m by the end,
mean −0.04 m over 120 epochs, 0 freezes, 15–42 contributing stations.

### 10.4 Predictions that MISSED (stated before the runs, recorded honestly)

- **"The skip rate collapses from ~58 %" (§4 arm 1, and this session's arm-18
  prediction).**  It does not collapse: 56.5 % → 48.1 % over 1 h, and at 20 epochs
  it went the wrong way.  μ (|μ| ≈ 0.7 m mean) is small against the gate's ≈ 5.1 m
  threshold, so the skips are NOT a coherent common-mode trigger — they are
  per-station frame offsets of a frozen x_lin(cb).  The 58 % is a FRAME artifact,
  which is why neither card removes it.
- **"LV-1 re-centres x_lin(cb) ⇒ fewer skips."**  False — LV-1 made 20-epoch skips
  worse (61 % → 67 %).  The reframed seed
  (seed' = raw − median_raw + field_slant) over-counts the per-LOS structure when
  the field is good — median_raw carries the station's median slant iono, so a LOS
  far from that median is seeded at roughly I_los + (I_field − median_I).  It moves
  the DCB linearization point, but not toward truth.
- **The per-station offset is NOT a clean rx-GF-bias estimate.**  §3 claims it is
  "a direct estimate of the station's rx GF bias"; measured, it is dominated by the
  station's MEDIAN SLANT IONOSPHERE (BOR100POL +9.71 m ≈ +59.8 slant-TECU at a
  ~30 TECU vertical field).  The log line says "hardware bias ⊕ median slant
  ionosphere" for that reason.  To cross-check against
  `scripts/extcb_gim_verdict.py`'s `bias_TECU`, the median slant must be removed
  first — that comparison is NOT the free cross-validation §3 advertised.
- **Deg-1 dipole is unchanged-to-worse**, as §4 predicted for the mean-fix (the
  dipole is the clustered SPREAD): 1 h peak 35.0 → 37.9, end 20.1 → **29.0**,
  Kaula ratio 2.01 → 2.90.  Open item — the level is fixed, the spread is not.

### 10.5 What is now open

1. **The rx-CB inject gate thresholds a frozen frame** (§10.2). → **CLOSED by card
   LV-4, §11 below** (`network.rx_cb_carry_step_gate`).
2. **The deg-1 dipole grew** with the level fix (§10.4).  Needs its own card; the
   clustered-spread mechanism was never addressed by either LV card.
3. **≥ 2 h and the 305-station gate.**  1 h settles the level but not `total_IF`;
   the combined posture is the candidate to re-open the 305 arm (SPEC §4.3).
4. **LV-1 is worth keeping but not for its stated reason.**  It is armed in the new
   arms; anyone quoting it should quote §10.2, not §1.

---

## 11. Card LV-4 — the carry screen, made frame-invariant (2026-08-05)

`network.rx_cb_carry_step_gate` (+ `_k_sigma`, default 10.0), default OFF,
fingerprinted when armed.  Requires `carry_rx_code_bias`.

### 11.1 The defect

The inject gate tested `|value_abs − x_lin(cb)|` at 10σ + 0.5 m.  Both terms are
absolute, but `x_lin` is FROZEN at its first-epoch seed and the network solve is
never written back, so the statistic is just the standing correction `x_r` — a
property of where the seed landed.  Consequences, both measured:

- 48–67 % of ALL carry priors withheld for whole runs, with no receiver event
  anywhere (arm 13: 4127/7304; arm 19: 3511/7304);
- structurally BLIND to the jump it exists for: a step between epochs t−1 and t
  leaves the carried value pre-jump and `x_lin` unmoved, so at t's inject the
  statistic contains no information about the event.

### 11.2 The fix

The fold-time gate keeps only its frame-free checks (gap, variance sanity).  The
event response moves to the SERIAL post-solve harvest — the one place where two
ABSOLUTE realizations of the same bias exist side by side (the fresh posterior
`x_lin + x_r` and the carried value), so their difference is invariant under any
reframing, reseeding or datum change.  Screen, per code-bias family, per epoch:

```
e_i  = value_abs_new,i − value_abs_prev,i          (invariant)
σ̂    = max( 1.4826·MAD_i(e_i) , formal σ floor )   (robust, this epoch's own scale)
z_i  = |e_i − median_i(e_i)| / σ̂                   (peer-relative)
z_i > k  ⇒  ERASE entry i                          (reset-not-repair)
```

Three choices, each paid for:

- **Robust, not formal.**  A first cut standardized `e` by its formal variance
  (σ ≈ 0.52 m).  Measured: **432 of 777 entries erased in 20 epochs**, 10 of 61
  stations still carrying at run end, level unstable.  At ignition the confounded
  direction genuinely re-realizes by metres per epoch, so the formal variance
  understates the true step scatter by 2–3 orders and any formal-σ test is a
  guaranteed mass-trigger.  The MAD scale is the RAIM σ̂₀ idiom, floored at the
  formal σ so a freakishly quiet epoch cannot make the screen hypersensitive.
- **Median removed.**  A step common to the whole network is a FRAME MOVE, not N
  simultaneous receiver resets — the same distinction LV-2 draws for the fold.
- **Erase, not overwrite.**  A genuine jump means this epoch's solve folded a stale
  pre-jump prior, so the posterior it produced is suspect too.  One epoch cold, then
  the next harvest re-seeds from data — a first sighting's lifecycle.

Below `rx_cb_carry_demean_min_stations` entries the screen STANDS DOWN (counted):
a thin epoch must not define what "normal" is (invariant #7).

### 11.3 Measured — the screen is fixed, and it exposes a second defect

Arm 20 (`_ext63_gps_dual_20ep_lv12_ig`, 20 ep) and arm 21
(`_ext63_gps_dual_1h_lv12_ig`, 1 h), both = arm 18/19 + this knob:

| | folds / harvests | erases | retired screen would have withheld |
|---|---|---|---|
| arm 20 (20 ep) | **1148 / 1219** (arm 18: 381) | 11 (0.96 %) | **852 of 1148 (74.2 %)** |
| arm 21 (1 h) | **7182 / 7365** (arm 19: 3793) | 123 (1.7 %) | ~74 % |

The same-run A/B is the finding: three quarters of the withheld population was a
frame artifact.  Max robust σ̂ 1.70 m (so "normal" epoch-to-epoch re-realization
really is metre-class), max z 744 (real outliers do exist and are found).

**But arming it destabilizes the level, and that is the second defect, not a
regression:**

| arm | 1 h a₀₀ mean | settled sd (20–60 min) | settled range |
|---|---|---|---|
| 13 (base) | 16.36 | 3.71 | 12.2 |
| 19 (LV-1+LV-2) | 35.09 | **4.98** | 14.3 |
| 21 (+LV-4) | 37.19 | **15.53** | **54.1** |

Arm 21's a₀₀ limit-cycles between ~15 and ~55 TECU on a ~3-epoch period, with
max |μ| rising 1.96 → 10.15 m.  Mechanism: LV-2's μ is applied **lag-1**, and the
loop gain is proportional to how much carry information folds.  At 33 % admission
(arm 19) the lag-1 correction is stable; at 97.5 % admission it oscillates —
textbook lag-1 feedback on a stiff loop.

**So the broken screen was accidentally acting as the loop-gain limiter** — the
same disease this whole spec is about (an accidental information budget), one level
down.  Removing the accident is right; it just makes the missing design explicit.

### 11.4 Verdict and the next card

- LV-4 is **correct and closes the screen defect** — arm it whenever you want the
  carry's information actually admitted, and read the same-run A/B line.
- **Do NOT arm it on this lineage yet** without the follow-on: at full admission
  the lag-1 de-mean oscillates.  The canonical posture stays LV-1 + LV-2.
- **Next card (LV-5): make μ WITHIN-EPOCH.** → **BUILT, §12 below.  It works: arm
  21's settled sd 15.53 → 4.69.**

---

## 12. Card LV-5 — μ computed within the epoch (2026-08-05)

`network.rx_cb_carry_demean_within`, default OFF, fingerprinted when armed.
Requires `rx_cb_carry_demean`.

### 12.1 The defect

LV-2 reduced μ at the serial post-solve harvest and applied it to the NEXT epoch's
folds.  That lag is a feedback delay whose loop gain scales with how much carry
information folds — harmless at 33 % admission (arm 19, settled sd 4.98 TECU), a
~3-epoch limit cycle at 97.5 % (arm 21, settled sd 15.53, range 54.1).  **The lag
was never necessary:** both terms of the carry innovation — the stashed ABSOLUTE
posterior and the FROZEN `x_lin(cb)` — are known before PASS B.

### 12.2 The change

A new serial seam, `networkRxCbDemeanPrepass`, called from the driver **between
PASS A and PASS B** (`ApplicationNetwork.cpp`, right after `networkSdEwlAr`).  It
walks the stations PASS A marked present, forms `innov = value_abs − x_lin(cb)` and
`λ = 1/(var + q·Δt)` for every carried entry inside the gap horizon, and reduces
them with the same `rxCbDemeanMu` helper, the same thin-mean floor and the same
freeze semantics as LV-2 — only the epoch it is applied to differs.  PASS B reads
`ctx.carry.rx_cb_demean_mu` exactly as before: one serial writer, parallel readers.

The post-solve harvest still reduces the FOLDED set's mean, now purely as a
**diagnostic**: the folded set is marginally smaller than the prepass candidate set
(a column can drop out of the local block), so the difference between the two is the
RESIDUAL common-mode force the correction left behind.  That residual is the only
honest way to tell a working de-mean from one that merely ran, and it is what the
run-end line reports.

### 12.3 Measured

| arm | window | settled mean | settled sd | settled range | end | folds |
|---|---|---|---|---|---|---|
| 13 canonical | 1 h | 17.45 | 3.71 | 12.2 | 20.95 | 43.5 % |
| 19 +LV-1+LV-2 | 1 h | 37.43 | 4.98 | 14.3 | 33.08 | 51.9 % |
| 21 +LV-4 | 1 h | 36.93 | **15.53** | **54.1** | 46.15 | 97.5 % |
| **23 +LV-5** | 1 h | **35.24** | **4.69** | **13.9** | **32.68** | 97.5 % |

Against GIM 32.24 TECU.  The limit cycle is gone (sd below even arm 19's, with 1.9×
the carry information admitted), the level ends **on the reference**, and:

- **Residual common mode: mean |resid| 0.0208 m against max |μ| 8.50 m** — the
  within-epoch correction nulls the frame force to ~0.2 % of its size.  (20-epoch
  smoke: 0.0057 m against 8.50.)
- Fold count unchanged by LV-5 (7182/7365, as arm 21) — it changes WHEN μ is
  computed, not which rows fold, exactly as predicted.
- 124 LV-4 erases, 0 thin-mean stand-downs, in-band 121/121, gate 0/0/0.

### 12.4 Predictions that missed

- **"max |μ| falls back toward ~2 m."**  It did not: 8.50 m, essentially arm 21's
  10.15.  μ is genuinely that large once the full carry folds — it is draining a real
  common mode every epoch, not chasing its own tail.  What the lag removal fixed is
  the PHASE of the correction, not its size.

### 12.5 Recommended posture

**LV-1 + LV-2 + LV-4 + LV-5 together.**  LV-4 without LV-5 is unstable, and LV-5
without LV-4 is nearly inert (there is little admitted carry for the lag to
destabilize).  All four remain loader-default OFF and armed per-config, per §0.

---

## 13. CORRECTION — what LV-2 actually does, and what LV-1 is really for (2026-08-05)

This section supersedes the interpretation in §4 and §10 (not their arithmetic).
It was forced by an arm that was supposed to be a formality.

### 13.1 The measurement that broke the story

With LV-4 armed, LV-1's only known actuator (the frame-dependent inject gate) is
retired, so LV-1 should have become INERT — the invariance theorem leaves it no
other channel.  Arm 27 (`_ext63_gps_dual_1h_lv245`, LV-2+4+5, LV-1 OFF) was run to
confirm that.  It did not:

| 1 h arm | settled mean | settled sd | end | step checks | erases |
|---|---|---|---|---|---|
| 23 LV-1+2+4+5 | **35.24** | 4.69 | **32.68** | 7182 | 124 |
| 27 LV-2+4+5 | **26.45** | 3.41 | **25.11** | 7180 | 125 |

A flat **−9 to −12 TECU** offset from epoch 10 onward, with the row sets identical
(7182 vs 7180 checks, 124 vs 125 erases).  So LV-1 moves the level by ~10 TECU
without changing which rows fold — a channel nobody had named.

### 13.2 The channel is LV-2 itself: the de-mean is NOT frame-invariant

Write the fold out in absolute terms.  The de-meaned prior on station *i* is
`Δx_i ~ N(innov_i − μ, 1/λ_i)`, i.e.

    x_abs,i  ~  N( value_abs,i − μ , 1/λ_i ),      μ = Σλ·innov / Σλ

and since `innov_i = value_abs,i − x_lin,i`, the information-weighted mean of those
targets is

    mean_λ(value_abs) − μ  =  mean_λ(value_abs) − [mean_λ(value_abs) − mean_λ(x_lin)]
                           =  **mean_λ(x_lin)**

**The de-mean does not free the common mode — it re-pins it to the mean of the
frozen first-epoch linearization points, with the carry's full Σλ ≈ 100 units.**
The de-mean only ever moved the fold's RHS; the information along the all-ones
direction is untouched by construction.  What LV-2 genuinely removes is the
FEEDBACK (last epoch's realization), which is why the lag mattered and why LV-5
fixed the limit cycle.  What it does not remove is the memory: a rolling memory is
swapped for a frozen one.

`x_lin(cb)` is seeded once by `StateInitializer::initializeCodeBiases`, which
subtracts the *seeded iono* — so LV-1 sets it.  That is the whole channel, and it
predicts the sign and size we measured.

**Attribution pair (arms 28a/28b, `_ext63_gps_dual_1h_lv45{_lv1,}.json`): LV-4+LV-5
armed, de-mean OFF, LV-1 flipped.**  If the de-mean is the channel, removing it
should remove the effect:

| configuration | LV-1 ON | LV-1 OFF | LV-1's effect |
|---|---|---|---|
| with LV-2 (arms 23/27) | 35.24 | 26.45 | **−8.79 TECU** |
| without LV-2 (arms 28a/28b) | 4.44 | 2.69 | **−1.76 TECU** |

(settled 20–60 min means).  ~80 % of LV-1's effect vanishes with the de-mean off,
and the remainder sits at the pure-FP amplification scale already measured for a
near-null level direction in a collapsed state (arms 14 vs 16: 1.8 TECU on −37).
Note both no-de-mean arms sit at 3–5 TECU: with the full carry admitted and nothing
draining its common mode, the flywheel owns the level outright — which is the
control this pair needed, not a posture to use.

### 13.3 So what IS LV-1 for?

Not "bias-free per-LOS iono seeding" (a no-op, §10.1).  Its real function is:

> **LV-1 puts the physically correct value into the frame that LV-2 pins.**

Without LV-1 the receiver's GF bias rides in the iono seed, so
`initializeCodeBiases` books ≈ 0 into the DCB and the de-meaned carry pins the DCB
common mode at ≈ 0 — reproducing exactly the zero-mean convention that IS DEFECT 2.
With LV-1 the bias lands in the DCB seed where it belongs (network mean ≈ −1.55 m ≈
−15 slant-TECU, measured estimator-free), the carry pins THAT, and the level rises
by the same ~11–15 TECU the defect was short.  Measured: 26.45 → 35.24 settled,
ending 32.68 against GIM's 32.24.

The level lands on the reference for a *sound* reason, but through a mechanism
nobody designed, and one that is still a **frozen** frame: it cannot track slow DCB
drift and it inherits whatever epoch 1 happened to see.

### 13.4 What a principled version would need

To leave the common mode genuinely free, the carry's INFORMATION along that
direction must be removed, not its RHS — a rank-(n−1) cross-station fold.  The
per-station diagonal channel cannot express that (each station is a separate local
block), and the post-reduce `SumZeroGauge` route is structurally blocked against
the Card-4 DD priors.  Two honest options, neither yet built:

1. **Own it deliberately**: keep the pin, but at a per-epoch DATA-derived estimate
   of the receiver-DCB mean rather than at epoch 1's — i.e. promote the accident of
   §13.3 into a designed authority with a stated estimator and an event response.
2. **Deweight the direction**: scale every carry fold's λ so Σλ on the common mode
   is comparable to the datum's 0.01, keeping the differences stiff by folding them
   in a differenced parameterization.  Needs a channel that does not exist today.

Until one of them exists, the level's owner is the epoch-1 DCB seed frame, and the
run-end lines say so.

### 13.5 What these arms did NOT measure — the over-retention question is OPEN

Every level number above is `network_vtec_sh_health.csv`'s `a00_tecu`, which is the
right instrument for the level.  The DATA-side question — do the newly admitted
carry priors buy their information honestly, or is this over-retention (quieter
priors, louder observation residuals — the lane A/B signature)? — is **not
answered**, and cannot be from these runs:

- The same file's `slant_resid_*` columns are FRAME-BROKEN by construction (they
  compute `x_r − sh_slant` and omit `x_lin`, memory `project_network_xlin_invariance`)
  — and LV-1 deliberately moves `x_lin`.  Their apparent 26.3 → 34.2 m rise across
  these arms is therefore uninterpretable, NOT evidence of anything.
- `network_block_baarda.csv` is present but effectively inert on this lineage
  (σ̂₀ pinned at its 1.0 floor for 13 of 13 rows in three of the four arms).
- The prior-residual census (`prires`) and the postfit-σ̂₀ traces are not armed in
  `_ext63_gps_dual_1h.json`.

**Before promoting this posture anywhere, arm `trace_postfit_sigma0` +
`trace_postfit_keyed` + the prires feeds and re-run the 1 h pair.**  A level that
lands on the reference while the observation residuals degrade is the exact trade
the lane A/B taught us to look for, and nothing here rules it out.

### 13.6 Status of the claims in this document (see also §14)

- §4 / §10.3 "the carry exerts ZERO net force on the common mode, so the designed
  authority owns the frame" — **WRONG as stated**; corrected above.  The identity
  Σλ(innov−μ)=0 holds, but in delta coordinates.
- §10.1 (LV-1's seed-frame gradient is a no-op) — **stands**, and §13.3 is *why*
  LV-1 still matters.
- §11 (LV-4) and §12 (LV-5) — **stand**.
- Code, loader banner and run-end lines were corrected to match this section.

## 16. LV-5 requirements (review addendum, 2026-08-05 — was mis-numbered §12; kept for the record)

### 12.1 The μ vote MUST be robust — LV-5 opens a contamination window LV-4 cannot close

LV-4's erase runs at the SERIAL HARVEST, **after** injection.  LV-5 moves the μ vote
to **before** the solve.  A station that steps THIS epoch therefore votes its stepped
innovation into μ before anything can catch it.  Magnitudes: the field has produced
**~200 km worst-case innovations** at cold start (the cold_clock_seed class); one such
voter among 61 Λ-equal stations kicks μ by ~3.3 km, and the demeaned η then injects a
~3 km COMMON-MODE error into every station's prior at stiffness Λ for one epoch.
Even an ordinary 10 m receiver reset pollutes μ by ~0.16 m — the size of the signal
the trace watches.  Requirement: screen the voters with the SAME robust peer-relative
idiom §11.2 just validated —

```
z_i = |innov_i − median_i(innov)| / max(1.4826·MAD_i, formal σ floor)
z_i > k  ⇒  excluded from μ (STILL folds; erase remains harvest's job)
```

μ = Λ-weighted mean over SURVIVORS.  Counter for excluded voters (invariant #4); in a
clean run it sits at ~0, and on a staged receiver-reset epoch it names exactly the
resetting station.  Below the min_stations floor of SURVIVORS: fall back to last
epoch's μ (one lag-1 epoch is benign) and count it.  Note the upstream defenses do
NOT cover this window: cold_clock_seed sanitizes x_lin, and the admission screen's
station-median centring cancels receiver-clock commons on the DATA path — neither
sees the carry's vote.

### 12.2 Ordering + consistency invariants

- **No station-state mutation between the μ sweep and the inject reads** (slip
  refloats, re-allocations, re-seeds all upstream of the sweep).  Assert/comment at
  the seam.
- **One shared eligibility predicate** for "will this row fold" used by BOTH the
  sweep and PASS B — any drift breaks Σλ(innov−μ)=0 silently.  Run-end counter: the
  realized Σλ(innov−μ) over actually-injected rows, WARN if not ≈ 0.
- Serial fixed-order reduce (61 adds); per-station partials into pre-sized slots.
- μ stops being carried state: drop the stored field, update the conditional
  fingerprint token.

### 12.3 Theory conformance (answers "is this ad hoc" — it is not)

The demean is **NGS LASER slide 5-86's S-transformation** (S = I_m − Eᵀ(KEᵀ)⁻¹K),
rank-1 case: family null-space basis E = 𝟙 (the common mode), constraint K = λᵀ/Σλ
⇒ S = I − 𝟙kᵀ ⇒ (Sv)_i = v_i − μ.  Carried priors are 5-74 prior-information rows
realized in a PREVIOUS adjustment's S-system; folding them untransformed smuggles a
second, inconsistent datum into the epoch — that is the flywheel, named.  LV-4 is
the datum-invariance theorem applied to testing (the old gate thresholded an
S-DEPENDENT quantity; the step statistic is estimable).  This card is the first
landed slice of the mapping's top open item (`SPEC_handover_stransform.md`).

**Known, accepted deviation**: 5-86 transforms mean AND covariance.  The exact
covariance transform is Λ → Λ − λλᵀ/Σλ — a dense cross-station pre-reduce
down-date, the architecturally BLOCKED shape (same reason the post-reduce
SumZeroGauge died).  ~~The implemented mean-only transform zeroes the net
common-mode FORCE but keeps the common-mode STIFFNESS ≈ Σλ, i.e. a VISCOSITY that
rate-limits frame motion~~  **SUPERSEDED BY §13 (this characterization was WRONG):
the demeaned targets' weighted mean is mean_λ(x_lin) — the FROZEN first-epoch
frame — so the mean-only transform is a position PIN to the seed frame at stiffness
Σλ, not a viscosity.  §14 measures the consequence (the 12 h ramp to +20) and §15
falsifies the q-tracking patch: under the demean the target is frozen BY
CONSTRUCTION, so no q can make it track.**  The closing sentence stands and is now
load-bearing rather than precautionary: freeing the direction requires the common
mode out of the carry's INFORMATION — the 5-86 transform at the REDUCED level (the
handover S-transform card), not a pre-reduce dense prior and not a q tweak.

### 12.4 Cold start / born-wrong-static (the residual exposure, named)

The carry map is empty at epoch 1, so cold-start monsters cannot enter the LV loop
directly; but a contaminated FIRST HARVEST freezes a born-wrong value that never
steps — LV-4 is structurally blind to it (steps only).  Watchers/options: the prires
census rxcb family (4σ̂₀) is the designed detector; a cheap hardening is a
first-harvest holdoff riding the existing warm-up machinery (`warmup_epochs`).
Measure first: if the census shows the class is empty on this lineage, the holdoff
stays unbuilt.

### 12.5 Grades for the LV-4+LV-5 validation arms (write into the configs)

Arm-21 posture + LV-5: settled sd 15.53 → **≤ 5**, no ~3-epoch alternation, max |μ|
back to ~2 m and damping; admission stays ~97.5 %, erases ~1-2 % (LV-5 changes the
net force, never who folds); excluded-voter counter ≈ 0.  Then re-run the ± LV-1
attribution pair under LV-4+LV-5 (predict: LV-1 now exactly inert — its only
actuator was the retired screen), re-measure the 1 h headline (35.09/37.19 are
stale, both taken under censoring or oscillation), check the 01:00 GIM area mean
before interpreting any overshoot, and read deg-1 (prediction: end < 29 with the
carry's difference information finally admitted).

---

## 14. THE 12 h VERDICT — the 1 h result was an ignition transient (2026-08-05)

### 14.1 The grading changed first

Every level number in §10–§13 is quoted against **one** figure, "GIM 32.24 TECU" —
the global mean of the day's 00:00 IONEX map.  That is fine for arms covering
minutes and **wrong for a 12 h arm**: the global mean VTEC itself moves through
DOY-129 (32.0 TECU at 00 UT → 36.9 at 08 UT → 33.9 at 24 UT), so a rising a₀₀ may
just be tracking it.  New tool, written for this:

    python scripts/gim_global_mean_series.py <run_dir> ...

It area-weights each IONEX map to a global mean, interpolates in time, and reports
`a₀₀(t) − GIM_mean(t)` hourly.  That functional is the right comparison — a
sun-fixed SH expansion's a₀₀ **is** the global mean (Ȳ₀₀ ≡ 1 under the orthonormal
convention).  All numbers below use it.

### 14.2 What the pair says

`a₀₀ − GIM_mean`, hourly means, 61-station EXT-CB, DOY-129:

| hour | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | run mean | sd |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| canonical (arm 24) | **−15.8** | −8.2 | −3.9 | −3.8 | −9.2 | −8.1 | −13.2 | −8.2 | −7.2 | −14.1 | −9.8 | **+1.0** | **−8.38** | 5.53 |
| LV-1+2+4+5 (arm 25) | **+4.2** | +4.8 | +4.9 | +13.2 | +13.9 | +20.8 | +17.9 | +15.0 | +16.5 | +16.6 | +18.4 | **+20.2** | **+13.85** | 6.21 |

Both complete (1440 epochs each), same window, same cold start, the four LV knobs
the only difference.

**The canonical posture's DEFECT-2 deficit is about HALF an ignition transient.**
It starts −15.8 (which is what every 20-epoch and 1 h arm on this ladder measured),
and from hour 2 it neither converges nor walks: it WANDERS between −3.8 and −14.1
about a **−8.4 mean**, and its final hour lands at **+1.0** — on the reference.
That is the same breathing arms 11/13 saw at 1 h, now visible across a full window.
So DEFECT 2 is real but roughly half the size the ignition arms reported, and its
sign is not even constant over a day.

**The LV stack does something different and worse on this window**: it starts +4 —
the best ignition level any arm on this ladder has produced — and then ramps UP to
+15…+21 and stays there (37 → 50 TECU across hour 3, smooth, no step, station and
LOS counts flat), mean **+13.85**.  By |mean error| the canonical is better by 1.7×,
and by final-hour error by 20×.

So the one-hour result that made the stack look like a fix was measuring the thing
the stack genuinely improves — **ignition** — and the settled window reverses the
ranking.  This is the standing "grade on a settled window" rule biting the very
session that wrote it down: the level channel's own convergence budget is ~2 h
(memory `project_agu26_abstract_campaign`), and every LV grading before this one sat
inside it.

### 14.3 Why the drift is the expected consequence of §13

§13 established that the de-meaned carry does not free the receiver-DCB common
mode — it PINS it to `mean_λ(x_lin)`, the frozen first-epoch code-bias seed frame,
with Σλ ≈ 100 units.  A frozen pin is exactly right at ignition (LV-1 makes it the
physically correct value, hence +4 at hour 0) and exactly wrong later: as the real
field and the DCB/level split evolve, the pin cannot follow, and the level moves to
satisfy it.  The canonical posture has no such pin — its carry folds only ~43 % of
the time and un-demeaned — so its level is free to re-realize each epoch, and it
converges.

The mechanism and the measurement agree, which is the only reason to believe either.

### 14.4 Recommendation (supersedes §12.5)

- **Do NOT arm the LV stack for settled/production work on this lineage.**  Ignition
  is better; hours 3+ are worse by 10–25 TECU against the time-matched GIM.
- **LV-4 and LV-5 remain correct fixes to real defects** (a screen that thresholded
  a frozen frame; a lag-1 loop that limit-cycled).  They are not the problem — the
  problem is what they expose: a stiff carry whose common-mode pin is frozen.
- **The next card is §13.4 option 1, now with a measured motivation**: give the
  receiver-DCB common mode a per-epoch, data-derived authority so the pin TRACKS,
  instead of freezing epoch 1's realization.  Grade it on this same 12 h pair with
  `gim_global_mean_series.py`; the target is |a₀₀ − GIM| ≲ 4 TECU at hour 0 AND at
  hour 11, which neither posture achieves today.
- **Re-grade the self-contained posture the same way before drawing any conclusion
  there.**  Arm 26 covers 15 minutes — pure ignition — and by §14.2's lesson that
  says nothing about its settled behaviour.

---

## 17. ROADMAP for the continuing session (2026-08-05, written after §15)

**The arc, in brief (read this before the ledger).**  EXT-CB pinned every satellite
family to COD products to isolate network failures.  What it caught: (1) removed
families take their minimum constraints with them (DEFECT 1 — fixed by the rx
zero-mean datum); (2) the "19 TECU level defect" decomposed into a CONVENTION
OFFSET (the receiver fleet's −5 ns GF-bias mean vs our zero-mean convention — the
receivers were the CARRIER of a frame difference, never a broken parameter), an
IGNITION TRANSIENT (heals by hour 2 at 61 stations; ESSENTIALLY ABSENT at 303 —
station support dissolves it), and a slow TRACKING question (the rolling carry
memory is the accidental tracker; every pin we built fought it and lost);
(3) two REAL machinery defects found and permanently fixed en route: the
frozen-frame inject gate (LV-4) and the lag-1 demean limit cycle (LV-5);
(4) datum-invariant grading (DD fractions IDENTICAL across postures; consumer
replay poor in BOTH with different excursion signatures) proved the level work was
frame-neutral where it claimed to matter and re-exposed the campaign's true
blocker: PRODUCT INTERNAL CONSISTENCY / churn; (5) the mask study attributed the
level gain to the obs mask (10→15), the residual cost to the iono cutoff, and the
30/30 extreme bought nothing (a00 goes prior-dominated — predicted); (6) LASER:
the theory named LV-2's flaw exactly (mean-only 5-86 = a frozen pin) and the
SOURCE (`C:\LASER`) turned the "architecturally blocked" principled card into a
cheap post-solve one with a golden oracle ("S-consistent harvest", item 3 below —
designed, NOT yet built or run).  Grading law learned twice: time-matched GIM,
≥ 2 h windows, datum-invariant quantities first.

State on hand-back: knobs `iono_seed_field_frame`, `rx_cb_carry_demean`,
`rx_cb_carry_step_gate`, `rx_cb_carry_demean_within` all built and default-OFF;
1264/1265 tests; runs `ext63_dual_12h_{ref,lv125,lv125_q6}` +
`ext63_dual_1h_lv{245,45,45_lv1}` + `agu63_nolane_30ep_lv125` on disk.  The EXT-CB
posture of record for settled work remains the CANONICAL one (no LV level stack).

Ranked next actions:

0. **FIRST — datum-invariant grading of the EXISTING runs (user course-correction,
   2026-08-05: "did we just push errors into the free parameters?").**  Every level
   number in this document is a FRAME quantity (violates invariant #5 as a grade;
   legal only as a diagnostic).  Content moved along the null-space directions
   (level ⊕ rx-DCB common ⊕ per-LOS commons) is INVISIBLE to estimable quantities;
   real error injection is not.  So grade `ext63_dual_12h_ref` vs `_lv125` (and
   `_q6`) on: (a) the **WTZZ00DEU held-out closed-loop consumer** — the original
   handoff's grade (c), never run in ~25 arms; the replay driver exists,
   ~33 s/arm (`project_consumer_replay_driver`); (b) DD fractional structure
   (med|frac_cyc|); (c) churn counters (PB discs, clock re-realizations, emitted
   product step rates); (d) postfit σ̂₀ families where armed.  Outcomes: if the
   arms are indistinguishable here, the level campaign was estimability-neutral
   (frame shuffling only) and the LEVEL card closes as "diagnostics-only,
   convention documented"; any real difference names which mechanism touched
   estimable content.  Diagnostic rider: run `extcb_gim_verdict.py` at several
   hours across the day (it currently reads 00:00 only) — whether the fleet-bias
   mean itself drifts diurnally (receiver thermal) vs the satellite/GIM side
   attributes §14's wander physically.

   **MEASURED (2026-08-05, Fable — replay arms `_ext63_replay_12h_{ref,lv125}`,
   `runs\replay_ext63_12h_{ref,lv125}\logs\consumer_replay_hourly.csv`):**
   - **(b) DD fractions IDENTICAL**: settled med|frac| 0.2503 vs 0.2498 on 322,936
     rows each, uniform-0.25 both — the stack was estimability-neutral on the DD
     channel (and §4's "fractions move off 0.25" prediction is dead at 12 h too).
   - **(d) postfit σ̂₀: lv125 1.4× LOUDER** (0.0637 vs 0.0461; phase 0.0086 vs
     0.0058) — the §13.5 over-retention direction.
   - **(a) the CONSUMER is NOT equivalent — both poor, DIFFERENTLY poor.**  ref:
     settles 2-7 m but hours 3-4 catastrophic (median 400 m, up to −326 m up).
     lv125: excursion-prone (hour means spike to 979/660/293 m at h2/h4/h6 over
     clean medians) and a **STEADY ~21 m 3D error through hours 10-11**
     (p95 ≈ median — a stable bias, not noise) coinciding with §14's pin-ramp
     era.  A frame-CONSISTENT product cannot hurt the consumer, so the late-era
     stack product is INTERNALLY inconsistent — the pinned rx-CB frame diverged
     from the drifting iono/clock content into estimable space.  Neither posture
     wins; the excursions in both carry the known consumer stiff-state-ratchet
     signature (invariant #1, consumer side) and need attribution against the
     product disc/datum records before this grade can rank postures.  NEXT
     ANALYSIS on these replays: excursion-hour attribution vs `products.txt`
     R/disc events, both arms.
0b. **The ladder's unbuilt final rung — external RECEIVER biases (CAS MGEX DCB
   product, which publishes nonzero receiver DCBs; COD's GPS receiver OSBs are 0
   by convention).**  Pinning them leaves NO soft bias absorber: only iono +
   ambiguities free — the original campaign design at its endpoint, and the
   direct proof/refutation of the receiver attribution.  Blocked only by the
   known ProductParser 4-char-station card (already needed for multi-GNSS).
   Diagnostic arm; does not violate the no-external-dependence rule (EXT-CB
   ladder is diagnostic by charter).

1. **The untested candidate posture: LV-4 alone (+LV-5 machinery idle), demean OFF.**
   → **RUN, TWICE — see §23** (falsified in §19 at the old masks; §19's stage-1 FAIL does
   NOT reproduce at the current ones).  Stage 1 PASSES all five gates below at 61 sta @
   obs 15 / iono 25, and again at 303 sta @ 30/30.  Stage 2 (3 h) is earned and UNSPENT.
   Still not a promotion candidate: LV-4 costs the data side in every cell measured
   (+16…64 % postfit phase rms, mask-dependent) and never pays for itself.
   Full admission (97.5 % folds) feeding the ROLLING memory — the accidental tracker
   of §14's table — with an honest event response.  No μ loop exists without the
   demean, so the §11.3 limit cycle has no path.  **Staged escalation (user
   directive 2026-08-05): 1 h → 3 h → 12 h, fresh SEAMLESS lineage each stage, next
   stage only on the previous gate passing.**  Arm the §13.5 audit feeds
   (`trace_postfit_sigma0` + `trace_postfit_keyed` + the prires families — all
   trace-class) in this arm's config from the FIRST stage, so the over-retention
   check rides the same lineage instead of a separate pair.  Per-stage gates:
   - **1 h = STABILITY gate only, NOT a level grade** (§14: 1 h is inside the
     ~2 h convergence budget).  Pass: settled (20-60 min) sd ≤ 5 TECU, no
     ~3-epoch alternation, folds ≥ 95 %, erases ≤ 2 %, sh_health 0/0/0.  Record
     the breathing amplitude vs the canonical's; do not verdict on it.
   - **3 h = first legal level read.**  Pass: hours 2-3 time-matched
     |a00 − GIM(t)| inside the canonical's own settled band (±4..9 TECU), breathing
     ≤ the canonical's ±6, audit feeds show no over-retention signature (data-side
     σ̂₀ not inflating while prior side quiets — the lane-A/B trade).
   - **12 h = the tracking verdict.**  Pass: |a00 − GIM(t)| ≲ 4 at hour 0 AND
     hour 11 (the target no posture has reached), end within ±4 (canonical ends
     +1.0), erases 1-2 % with no erase clusters at frame events.
   If all three grade, this becomes the recommended posture and the level card
   CLOSES at "tracking authority = admitted rolling carry + LV-4 event response".
2. **The §13.5 audit gate BEFORE any promotion**: arm `trace_postfit_sigma0` +
   `trace_postfit_keyed` + the prires feeds on the 1 h pair and check the
   over-retention signature (quieter priors / louder data residuals — the lane-A/B
   trade).  A level that lands on the reference is not sufficient.
3. **The principled card — reshaped by the LASER SOURCE (2026-08-05, `C:\LASER`):
   "S-consistent harvest".**  `CoreLsa.cpp:1678` implements 5-86 exactly (mean about
   the expansion point + covariance factor G → S·G), and its big-system path
   (`Net3DLsa.cpp:5573`) shows the transform WITHOUT forming S: for a
   translation-type null space it is a per-component POST-SOLVE mean-subtraction
   over the datum-defining set — O(n).  Mapped to the rx-CB family: **apply the
   Λ-weighted demean to the HARVESTED absolute values each epoch (solve → transform
   → harvest)**, making the carried set zero-mean-by-construction and coherent with
   the datum row.  No frozen pin (the mean is re-derived from THIS epoch's
   solution), no lag-1 feedback (nothing is fed back), no cross-station prior rows
   (the carry stays diagonal; covariance cost = the diagonal approximation the
   carry blob already pays).  This is DISTINCT from every tested posture — LV-2
   demeaned the fold INNOVATION (froze the x_lin frame); this demeans the harvested
   REALIZATION (frame re-derived per epoch).  The canonical's rolling memory is the
   noisy accidental version of exactly this.  Honest caveat: it forces
   receiver-physical drift into the level channel as a DECLARED frame property —
   §13.4 option 1 (data-derived authority) remains the answer if that attribution
   ever matters.  Independent oracle for the implementation:
   `C:\LASER\test\GPS3710_test_PMINOLESS` (golden input/output).  Alternative if
   this fails: the reduced-level fold (`SPEC_handover_stransform.md`).  Target,
   measured, unreached by every posture to date: |a00 − GIM(t)| ≲ 4 TECU at hour 0
   AND hour 11.
4. Parked, own cards: the deg-1 clustered-spread dipole (EXT-CB); the
   self-contained posture decision (the stack HELPED there — a00 24.8 → 29.2,
   dipole shrank, frame-artifact withholds only 12.7 % — §13's pathology is
   posture-dependent, so the self-contained call is separate and friendlier).

Grading protocol for everything above: `scripts/gim_global_mean_series.py`
(time-matched, hourly) **and `scripts/field_vs_gim_footprint.py`** (the same
comparison restricted to where the network actually has pierce points — §18 shows
the residual is ~2× LARGER there than globally in every arm, so the global number
under-reports and both belong in any verdict), ≥ 2 h windows, plus the §12.5/§16
counters.  Provenance: §9-§18 here, `docs/HANDOFF_ext_cb_iono_level.md`,
`docs/CONTRACT_level_authority.md`, memories `project_level_information_budget` /
`project_extcb_defect2_rootcause`.

Note for item 1 and item 3: §18.1 closes the `rx_cb_carry_q` axis — tighter than
1e-9 is meaningless (already a random constant at 30 s), looser is measurably
worse, and the canonical runs the SAME q with a −9.7 footprint residual.  What the
level responds to is the KIND of memory holding the receiver-DCB common mode, not
its decay rate, which is exactly why item 1 (rolling memory + LV-4 admission) is
the ranked next test.

## 15. The tracking-pin fix, tested and FALSIFIED (2026-08-05)

§14.4 proposed the obvious follow-on: if the drift comes from a FROZEN common-mode
pin (§13), let the pin track.  The cheapest form of that is one knob — raise the
carry's random walk so it forgets on a physical timescale instead of never:

    network.rx_cb_carry_q : 1e-9 -> 1e-6 m²/s      (tau = var/q ~ 2.8 h)

At 30 s the inflation is 3e-5 m² against a ~1e-2 m² posterior, so within-epoch
difference rigidity is untouched; over hours the pin can follow.  Arm 29,
`_ext63_gps_dual_12h_lv125_q6.json`, 12 h, single knob vs arm 25.

**Prediction (stated in the config before the run): the ramp flattens — hours 3–11
inside ±8 TECU while hour 0 keeps the stack's good ignition.  MEASURED: the
opposite.**

`a₀₀ − GIM_mean`, 12 h means and the hourly shape:

| arm | h0 | h1 | h2 | h3 | h5 | h8 | h11 | run mean |
|---|---|---|---|---|---|---|---|---|
| 24 canonical | −15.8 | −8.2 | −3.9 | −3.8 | −8.1 | −7.2 | **+1.0** | **−8.38** |
| 25 LV stack, q=1e-9 | +4.2 | +4.8 | +4.9 | +13.2 | +20.8 | +16.5 | +20.2 | +13.85 |
| 29 LV stack, q=1e-6 | +5.0 | +13.4 | +16.9 | +20.2 | +17.1 | +20.0 | **+25.5** | **+17.81** |

Loosening the carry made it **worse and sooner**: the +20 plateau arrives at hour 3
instead of hour 5, and the run ends +25.5 instead of +20.2, still climbing.  (Carry
mechanics stayed healthy — 87435/87679 folds, 183 erases, μ ≈ 0 — so this is the
level responding, not the machinery breaking.)

### 15.1 What that rules out, and what it leaves

The frozen pin was **resisting** the drift, not causing it.  So the driver is not
the pin's stiffness, and the ranking across four postures is not monotone in "how
much carry information is admitted":

| posture | carry common mode | 12 h level |
|---|---|---|
| canonical (43 % folds, no de-mean) | **rolling** — re-asserts LAST epoch's realization | wanders about −8, ends +1 |
| arm 14 (carry OFF) | none | −9 TECU/epoch collapse to −37 |
| LV stack q=1e-9 (97 % folds, de-meaned) | **frozen** at epoch-1's seed frame | ramps to +20 |
| LV stack q=1e-6 | frozen, decaying over ~2.8 h | ramps to +25 |

**The rolling memory is what TRACKS the level.**  That is the uncomfortable
conclusion: the "flywheel" LV-2 was written to remove is also the only thing in the
current design that follows the field's evolution, and the breathing (±6 TECU) is
the price it charges for that.  Removing it buys a quiet, self-consistent frame that
is quietly wrong by +20 TECU twelve hours later.

### 15.2 The next card, now with four arms behind it

Neither a rolling memory, nor a frozen memory, nor no memory is a designed
authority.  The receiver-DCB common mode needs a **per-epoch, data-derived
estimate** — §13.4 option 1 — with a stated estimator, an event response, and its
own counters, so the level stops being a side effect of whatever the carry happens
to remember.  Everything else in this document is scaffolding for that card:

- LV-4 makes the carry's information admissible at all (the retired screen withheld
  74 % of it on a frame artifact) — keep.
- LV-5 makes any de-mean stable at full admission (the lag-1 loop limit-cycled) —
  keep.
- LV-1 is the instrument that showed the pin exists and where it points, and its
  per-station offsets are a free receiver-GF-bias diagnostic — keep as a diagnostic.
- LV-2 as it stands trades tracking for stability, and the trade is bad on a settled
  window — do not arm until the designed authority above replaces what it removes.

---

## 18. Is the residual an EXTRAPOLATION artifact?  No — measured (2026-08-05)

Fair challenge to §14/§15's metric: `a₀₀ − GIM_mean` compares two GLOBAL means, and
ours is an SH field constrained only where the network has pierce points.  A global
residual could therefore be reporting the Kaula extrapolation over an unobserved
hemisphere rather than an error where we actually look.

`scripts/field_vs_gim_footprint.py` settles it.  It synthesizes the estimated field
from `logs/network_vtec_sh.csv` on a lat/lon grid, samples the time-interpolated
IONEX on the same grid, and reports the cos-latitude-weighted mean difference both
globally and restricted to a FOOTPRINT — grid points within `--radius` of any
station.  (At a 450 km shell the pierce point sits ~13° from the station at el 20°,
so 5–15° brackets "where we observe".)

First correction to an assumption in §14: **these 61 stations are globally
distributed**, not Europe-weighted — lat −78…+79, lon −170…+175, 8/13/25/15 across
the four longitude quadrants.  A 15° footprint already covers 69 % of the sphere;
5° covers 9.8 %.

12 h means, DOY-129:

| arm | footprint (5°) | global | fp rms | gl rms |
|---|---|---|---|---|
| 24 canonical | **−9.7** | −6.1 | 11.2 | 6.9 |
| 25 LV-1+2+4+5 | **+22.6** | +11.2 | 27.3 | 15.1 |
| 29 LV stack, q=1e-6 | **+27.4** | +15.4 | 31.9 | 18.5 |

**In every arm the disagreement is ~2× LARGER over the footprint than globally.**
So the answer is no: `a₀₀ − GIM_mean` was not measuring extrapolation — if anything
it UNDER-reported, because the field's error is concentrated where the data is.  The
§14/§15 ranking stands and is strengthened; the LV stack's footprint error (+22.6)
is 2.3× the canonical's (−9.7), and its pointwise rms is 2.4× as large, so the shape
degrades too, not only the level.

The hourly shape is the same ramp: the stack's footprint residual runs −7.9 (h0) →
+20.0 (h3) → +36.4 (h4) → +33.1 (h11), i.e. roughly twice the global amplitude at
every hour.

### 18.1 And is `rx_cb_carry_q` simply too tight?

The q axis is exhausted, and it is not the discriminator:

- **Tighter is meaningless.**  At q = 1e-9 m²/s the inflation over a 30 s epoch is
  3e-8 m² against a ~1e-2 m² posterior — already a random CONSTANT.  There is no
  room below.
- **Looser is worse**, measured: q = 1e-6 (τ ≈ 2.8 h) moved the 12 h footprint
  residual +22.6 → +27.4 and brought the plateau forward two hours (§15).
- **Much looser is catastrophic**: the carry OFF limit is arm 14's −9 TECU/epoch
  collapse.
- **And the canonical posture runs the SAME q = 1e-9** with a −9.7 footprint
  residual.  Same q, 32 TECU apart.

So q moves the answer by ~5 TECU while the de-mean + full admission move it by
~32 TECU.  q is second-order here; what the level responds to is WHAT KIND OF MEMORY
holds the receiver-DCB common mode (§15.1), not how fast that memory decays.

---

## 19. Roadmap item 1 FALSIFIED at stage 1, and §13.5's audit re-attributes the whole ladder (2026-08-05)

> ⚠ **PARTLY SUPERSEDED BY §23.**  Everything measured here stands as measured, but it
> was ALL measured at obs mask 10 / iono cutoff 25 — the denominator §21 subsequently
> voided.  Re-run at the current masks (§23): the stage-1 FAIL below (the SH band) does
> **not** reproduce, and §19.3's headline "+64 % postfit phase rms" turns out to be the
> worst cell of a mask-dependent series (+64 / +35 / +16 % at obs 10 / 15 / 30), not a
> constant of the mechanism.  §19.5's "item 1 is closed FALSIFIED" no longer follows
> from the evidence given for it.  §19.2/§19.4 (the audit split, the instrument holes)
> are re-confirmed by §23.4 and still stand.

§17 ranked "LV-4 alone (+LV-5 machinery idle), demean OFF" as the untested candidate
posture, to be escalated 1 h → 3 h → 12 h.  **It fails at stage 1, and the arm was
already on disk**: `_ext63_gps_dual_1h_lv45.json` sets only `rx_cb_carry_step_gate`,
and with the de-mean off the loader forces LV-5 off (`GnssConfigLoadNetwork.cpp:6630`),
so that file *is* LV-4-alone.  No new run was needed.

### 19.1 The 1 h stability gate

| §17 gate | threshold | measured | |
|---|---|---|---|
| folds | ≥ 95 % | 97.5 % (7179 / 7365) | pass |
| erases | ≤ 2 % | 1.76 % (126, max z 744.7, max robust σ̂ 1.702 m) | pass |
| settled sd | ≤ 5 TECU | 1.58 | pass |
| 3-epoch alternation | none | none | pass |
| **sh_health** | **0/0/0** | **46 out-of-band epoch(s) / 8 engagement(s) / 46 emission(s) withheld** | **FAIL** |

The level **collapses**: settled (20–60 min) a₀₀ mean **2.69 TECU**, min **−0.76**
(unphysical), out of the [2, 150] band for 46 of 121 epochs.  It does not diverge the
way arm 14 (carry OFF) did at −9 TECU/epoch — it **parks** ~15 TECU below the canonical
while riding the canonical's own breathing shape, and recovers to 6.59 at 60 min while
the canonical is at 20.95.  Adding LV-1 barely moves it (4.44 / sd 0.82, 8 out-of-band).

This is the loader's own LV-4-without-de-mean warning, measured: full admission delivers
the carry's common mode intact, and it owns the receiver-DCB frame outright.  So the
missing fifth row of §15.1's table is:

| posture | carry common mode | admission | level |
|---|---|---|---|
| canonical | rolling | 43 % | 1 h 17.45; 12 h wanders −8.4, ends +1.0 |
| **LV-4 alone** | **rolling** | **97.5 %** | **1 h 2.69, out of band 46/121 epochs** |

**§15.1's "the rolling memory is what TRACKS the level" is therefore too strong.** The
canonical's rolling memory tracks *at 43 % admission*.  The same memory at 97.5 %
admission drags the level out of the physical band.  What the level responds to is not
only the KIND of memory but HOW MUCH of it is admitted.

### 19.2 The §13.5 audit — and it does not say what §13.5 predicted

Ranked item 2 required the over-retention check before any promotion.  The feeds turned
out to be **already armed** on this lineage (inherited from `_ft63_24h_conv.json` →
`_golden63_gps_1h_laneoff.json`), so the audit ran on the arms already on disk, all from
the same build (`Aug  4 2026 23:27:25`).

Decisive control: **the observation counts are identical across arms** — 1 582 051 phase
observations, 87 678 station-epochs, 61 stations in each 12 h arm; 87 936 phase
observations in each 1 h arm.  Same data, different fit.

12 h arms, median postfit phase rms (`network_pf_keyed.csv`, STA rows):

| window | canonical | LV-1+2+4+5 | LV stack q=1e-6 |
|---|---|---|---|
| h0–12 | 0.01130 m | **0.01580 m (+40 %)** | 0.01290 m |
| h6–12 (settled) | 0.01110 m | **0.01600 m (+44 %)** | 0.01250 m |
| h2–3 | 0.00900 m | **0.01300 m (+44 %)** | 0.01140 m |

The prior side agrees rather than trading against it — prires `rxcb` σ̂₀ (a per-epoch
global scale, emitted identically on the CLEAN and RESPONDED rows, over matching row
totals 87 618 vs 87 423): canonical **1.92** → LV stack **12.62** over 12 h, 1.06 → 8.40
in h2–3.  Block-Baarda σ̂₀ 2.76 → 5.25.  (`n_gt_4` fractions move the *other* way — 0.31 →
0.03 — but that is only the 4σ̂₀ bar rising with σ̂₀; read σ̂₀, not the exceedance count.)

**So this is NOT the lane-A/B over-retention signature** (quieter priors bought with
louder data).  It is louder on *both* sides: the LV stack fits its carried priors worse
AND its observations worse.  Station congruence σ̂₀ (61.3 → 41.2) is the single instrument
pointing the other way, against four that agree.

### 19.3 The cost attributes to LV-4, and to LV-4 alone

1 h settled window (20–60 min), identical 87 936 phase observations in every arm:

| arm | LV-4 | de-mean | LV-1 | postfit phase rms | settled a₀₀ |
|---|---|---|---|---|---|
| canonical | – | – | – | 0.01020 m | 17.45 |
| **LV-1+2 (43 % admission)** | – | ✓ | ✓ | **0.01000 m** | **37.43** |
| LV-4 alone | ✓ | – | – | 0.01670 m | 2.69 |
| LV-4 + LV-1 | ✓ | – | ✓ | 0.01670 m | 4.44 |
| LV-2+4+5 | ✓ | ✓ | – | 0.01670 m | 26.45 |
| LV-1+2+4+5 | ✓ | ✓ | ✓ | 0.01660 m | 35.24 |

Every arm carrying LV-4 sits at 0.0166–0.0167; every arm without it at 0.0100–0.0102.
**The de-mean and the seed frame move the LEVEL by ~33 TECU and cost nothing measurable
on the data side.  LV-4 costs +64 % postfit phase rms and moves the level DOWN ~15 TECU.**

**This contradicts §15.2's "LV-4 makes the carry's information admissible at all — keep."**
LV-4's critique of the retired screen stands on its own terms: the statistic
`|value_abs − x_lin(cb)|` *is* frame-dependent, *is* blind to a between-epoch jump, and
*did* withhold 67 % of folds with no receiver event.  What the audit adds is that the
folds it withheld are ones the observations do not support.  The broken screen was doing
real work by accident — the accidental-budget disease again, one level below where §11.3
found it.

### 19.4 Two invariant-#4 holes in the audit instrument itself (verified, not inferred)

§13.5 named `trace_postfit_sigma0` + `trace_postfit_keyed` + the prires feeds as the
audit.  Half of that instrument does not exist on this lineage:

- **`network_prior_residual.csv` emits ONLY the `rxcb` family**, although
  `prior_residual_families` is armed `"pl,pb,cb,zwd,rxcb"`.  Four of five armed families
  produce zero rows (consistent with the posture: no estimated satellite code bias under
  EXT-CB, `phase_leveling` off on this lineage).
- **`network_pf_keyed.csv` has `n_code == 0` in ALL 138 749 rows** of the 12 h canonical
  run — the CODE half of the postfit-keyed instrument is structurally absent, so
  `s0_code` / `rms_code` are identically zero and any code-side verdict read from this
  file is vacuous.  Systemic, not lineage-specific: the 303-station EXT-CB smoke's own
  run-end line reads `12079 rows = 0 code + 5382 phase`.
- `network_block_baarda.csv` is floored at σ̂₀ = 1.0 for 36 % of rows over 12 h in the
  canonical and for **100 %** of its 1 h settled window, confirming §13.5's note that it
  is near-inert here.  It separates at 12 h; do not read it at 1 h.

The usable data-side instruments on this lineage are therefore `rms_phase` / `s0_phase`
from `pf_keyed`, congruence σ̂₀, and (≥ 12 h only) block-Baarda σ̂₀.

### 19.5 What this changes

- Item 1 is closed FALSIFIED; it does not earn a 3 h or a 12 h stage.
- Item 2 is discharged, with the opposite sign to the one it was written to detect.
- **The candidate posture is now LV-1 + LV-2 at 43 % admission (arm 19's posture), the
  only cell with both a good level and an uncompromised data fit** — and the only one
  never taken past 1 h.  §14's ramp verdict was measured on the LV-4-bearing stack;
  whether the ramp survives without LV-4 is unmeasured.  That is arm 30
  (`_ext63_gps_dual_3h_lv12.json`, §20).
- §15.2's keep/drop list needs its first line inverted: LV-4 is not a prerequisite to
  keep, it is the ladder's data-side cost.

### 19.6 Reconciliation with §17 item 0 (the invariant-#5 course-correction)

§17 item 0 — added concurrently, by another session, from the user's course-correction
*"did we just push errors into the free parameters?"* — is the right frame for reading
everything above, and it changes the STANDING of §19's two results differently:

- **The level numbers (a₀₀, §19.1) are FRAME quantities.**  Under invariant #5 they are
  legal as a diagnostic and NOT as a grade.  So "LV-4 alone collapses the level to
  2.69 TECU" is properly read as *"LV-4 alone moves the solution 15 TECU along a null
  direction, far enough that the emission band rejects it"* — the 46 withheld emissions
  are a real operational failure, but the TECU figure itself is convention, not error.
- **The postfit phase residuals (§19.2, §19.3) are datum-INVARIANT.**  Residuals do not
  move under a reframing, and the observation counts are identical across arms, so the
  LV-4 attribution (+64 % at 1 h, +40…44 % at 12 h) is an invariant-legal grade.  It is
  the strongest claim in this section and it does not depend on any GIM comparison.

**Independently corroborated by item 0's own replay arms**, on a different instrument and
a different code path: postfit σ̂₀ `lv125` **1.4× louder** than the reference (0.0637 vs
0.0461 all; phase 0.0086 vs 0.0058), against §19.2's 1.40–1.48× on `pf_keyed` STA
`rms_phase`.  Two sessions, two statistics, same sign and nearly the same ratio.

Item 0 also supplies the estimable-space consequence §19 could not: the stack's held-out
WTZZ00DEU consumer carries a **steady ~21 m 3D error through hours 10–11** (p95 ≈ median,
i.e. a stable bias) coinciding with §14's pin-ramp era, while DD fractions are identical
(0.2503 vs 0.2498 on 322,936 rows).  So the stack is estimability-neutral on the DD
channel and estimability-HARMFUL on the consumer — consistent with §19's reading that
the admitted carry is not paying for itself.

⚠ Consequence for arm 30 (§20): its LEVEL result is a diagnostic.  Its GRADE is the
postfit phase rms and the consumer replay, and the arm is designed so that the level
knobs (LV-1, LV-2) are the ones the audit found cost nothing on that invariant side.

---

## 20. Arm 30 (LV-1+2, no LV-4) and the 303-station EXT-CB arm (2026-08-05)

Both run on build `Aug  4 2026 23:27:25`, both at the **pre-2026-08-05 masks**
(`processing.elevation_mask_deg` 10, `ionosphere_weighting.elevation_cutoff_deg` 25) —
see §20.3, the masks were unified to 15/15 after these ran.

### 20.1 Arm 30 — the §19 thesis holds

`_ext63_gps_dual_3h_lv12.json`, 3 h, 360 epochs, 331.8 s wall.  Predictions were written
into the config before the run; scored honestly:

| # | prediction | outcome |
|---|---|---|
| 1 | level tracks the stack within ±3 TECU, i.e. ramps | **roughly right** — h0 +2.95, h1 +1.49, h2 +8.73 (stack +4.15 / +4.80 / +4.85); it ramps, and ramps EARLIER |
| 2 | **data side stays at the canonical's 0.009–0.010 m, not the stack's 0.0130** | **CONFIRMED** — 0.00930 at h2–3 |
| 3 | prires rxcb σ̂₀ stays near the canonical's ~1.0 | **MISSED** — 6.55, nearly the stack's 8.40 |
| 4 | folds ~43 % | **MISSED** — 57.0 % (12502 / 21929) |

Datum-invariant grade, h2–3, identical 132 967 phase observations in all three arms:

| arm | rms_phase | vs canonical | prires rxcb σ̂₀ | a₀₀−GIM h2 | sh_health |
|---|---|---|---|---|---|
| canonical | 0.00900 m | — | 1.06 | −3.89 | 0/0/0 |
| **arm 30 (LV-1+2)** | **0.00930 m** | **+3.3 %** | 6.55 | +8.73 | **0/0/0** |
| stack (LV-1+2+4+5) | 0.01300 m | +44 % | 8.40 | +4.85 | 0/0/0 |

**The two costs are independent and separately attributable**, which prediction 3's miss
is what revealed:

- **LV-4 (admission) costs the DATA side** — +44 % postfit phase rms, and ONLY when armed.
- **LV-2 (de-mean) costs the PRIOR side** — prires rxcb σ̂₀ 1.06 → 6.55 with LV-4 absent.
  Arguably the de-mean working as designed (it deliberately moves each fold's RHS off what
  the carry remembers, so prior residuals grow by construction) — but it is a cost, and
  §19.2 wrongly attributed the whole σ̂₀ inflation to the stack as a unit.

Prediction 4's miss is §13.3's channel made visible: LV-1 moves `x_lin(cb)`, and the legacy
inject gate thresholds `|value_abs − x_lin(cb)|`, so arming LV-1 RAISES the fold rate
(43 % → 57 %) without touching the gate.

**Consumer replay does NOT rank the postures** (`_ext63_replay_3h_lv12.json`, 11.2 s).
Hours 0–2 3D error, median / mean / max in m:

| arm | h0 | h1 | h2 |
|---|---|---|---|
| arm 30 | 3.99 / 4.77 / 9.7 | 4.36 / 3.97 / 4.7 | 2.27 / **50.4** / **1149** |
| canonical | 2.14 / 2.73 / 7.1 | 2.14 / **35.5** / **308** | 3.54 / 3.50 / 3.8 |
| stack | 5.21 / 5.01 / 6.2 | 4.28 / 4.52 / 6.0 | 3.71 / **220.3** / **979** |

All three sit at 2–5 m medians with one catastrophic excursion in the first 3 h, in
DIFFERENT hours.  Consistent with §17 item 0's read (consumer stiff-state ratchet,
invariant #1 consumer side); it needs excursion-hour attribution against the product
disc/datum records before it can rank anything.  **Arm 30's verdict rests on the postfit
residuals, not on this.**

### 20.2 ★ DEFECT 2 is largely a STATION-SUPPORT artifact — measured at 303 stations

`_ext305_gps_dual_1h.json`: the EXT-CB posture of record transplanted onto the 303-station
AGU network, SH held at deg 8 / single layer / a₀₀ anchor 32.24±2 so **network size is the
single variable**.  121 epochs, 2979.8 s wall (24.6 s/epoch), exit 0, SH health gate 0/0/0,
2 of 303 stations occasionally dropped.

| | EXT-63 canonical | **EXT-305 canonical** |
|---|---|---|
| stations / phase obs (20–60 min) | 61 / 87 936 | **301 / 423 169** |
| median postfit phase rms | 0.01020 m | **0.00600 m (−41 %)** |
| a₀₀−GIM, hour 0 | **−15.83** | **−1.98** |
| a₀₀−GIM, run mean / sd | −8.38 / 5.53 (12 h) | **−1.99 / 3.64** (1 h) |
| footprint (5°) / global mean | −9.7 / −6.1 (12 h) | **−5.71 / −1.82** (1 h) |
| footprint / global rms | 11.2 / 6.9 | **6.25 / 4.00** |

**The 303-station arm's IGNITION hour (−1.98) is better than any settled hour the
61-station canonical reaches in twelve** (its best is −3.81 at h3; only h11's +0.97 is
closer, and that is a sign crossing).  The datum-invariant instrument agrees independently:
postfit phase rms −41 %.

Two honest caveats:

- **The footprint comparison is NOT time-matched** — 1 h mean against §18's 12 h means.
  The MATCHED comparison is the hourly global series (−15.83 vs −1.98 at hour 0), and it
  is the stronger of the two anyway.
- §14's "~2 h convergence budget for the level channel" was measured at 61 stations.  The
  305 arm shows essentially no ignition transient (hour-0 mean −1.98 with sd 3.66, vs the
  61-sta −15.83 ± 4.93), so **that budget looks like a station-support artifact too**.
  A 3 h arm (`_ext305_gps_dual_3h.json`, written, ~2.2 h wall) would settle whether the
  305 level then walks the way §15 saw at 61.

§18's structural finding SURVIVES at scale: the residual is still ~3× larger over the
footprint than globally (−5.71 vs −1.82), so the global number still under-reports.  What
changes with station support is the MAGNITUDE — every metric roughly halves.

### 20.3 Mask posture changed after these runs (user directive, 2026-08-05)

`processing.elevation_mask_deg` 10 → **15** and
`estimation.ionosphere_weighting.elevation_cutoff_deg` 25 → **15**, both at the lineage
root `_ft63_24h_conv.json` (no config in either chain overrides them; verified resolved
15/15 for all 63- and 305-sta arms).

⚠ **This invalidates every denominator in §9–§20.**  All arms above ran at 10/25.
⚠ The 25° iono cutoff was VALIDATED and SHIPPED (`project_global_sh_iono`): at 10° the
305-sta field diverged a₀₀ 296 → −305 TECU with clock-Δ rms 77× worse.  The posture check
(`GnssConfigNetworkPosture.cpp:369-373`, `:553-565`) requires ≥ 24.9° and will now WARN on
every network run; the deg-15 SH sanction at ≥150 stations is conditioned on the 25° cutoff.
⚠ The recorded path to a lower mask is lever 2 — *carrier-smooth the code GF seed, THEN*
lower to 10–15°.  That prerequisite is not built; the seed is still a per-epoch code GF
estimate (~0.26–0.37 m scatter).
**Prediction, stated before the first 15/15 run: the level deficit gets WORSE, because
lowering this mask historically pushed a₀₀ DOWN and DEFECT 2 is a₀₀ too LOW.**

---

## 21. The mask 2×2 — the level gain and the residual cost come from DIFFERENT knobs (2026-08-05)

§20.3 changed both masks at once and §20.3's prediction ("the level gets WORSE") was
**FALSIFIED**.  So was the companion prediction that residuals would be flat-or-better.
Because the two edits push opposite ways, a 2×2 was run to separate them — four 3 h
canonical EXT-63 arms, same build, same window, one knob apart:

| arm | obs mask | iono cutoff | a₀₀−GIM 3 h mean | sd | h0 | **rms_phase h2–3** | n_obs h2–3 | sh_health |
|---|---|---|---|---|---|---|---|---|
| 24 (old canonical) | 10 | 25 | ≈−9.31 | 5.53 | −15.83 | 0.00900 | 132 967 | 0/0/0 |
| 31 (the directive) | 15 | 15 | −3.02 | 5.66 | −6.75 | 0.01140 | 119 800 | 0/0/0 |
| **32** | **15** | **25** | **−4.10** | **4.42** | −7.23 | **0.00870** | 119 734 | **0/0/0** |
| 33 | 10 | 15 | **−14.48** | **13.07** | **−29.49** | 0.01310 | 133 039 | **58/2/58** |

**The two effects are cleanly separable and they belong to different knobs:**

- **The LEVEL gain is the OBSERVATION MASK.**  At obs 15 the 3 h mean is −3.02 / −4.10
  (iono 15 / 25); at obs 10 it is −9.31 / −14.48.  Raising the observation mask 10 → 15
  is worth **~5–6 TECU of level**, at either iono cutoff.
- **The RESIDUAL cost is the IONO CUTOFF.**  At iono 25 the h2–3 rms is 0.00900 / 0.00870
  (obs 10 / 15); at iono 15 it is 0.01140 / 0.01310.  Lowering the cutoff 25 → 15 costs
  **+27…46 % postfit phase rms**, at either observation mask.

**⇒ Arm 32 (obs 15 / iono 25) dominates all four on BOTH axes.**  It has the best residual
of any arm including the old canonical (0.00870 vs 0.00900, and 0.00800 vs 0.01020 in the
20–60 min window), a level 5.2 TECU better than the old canonical, the LOWEST level sd of
the four (4.42), 0/0/0 sh_health, and it keeps `elevation_cutoff_deg` at the validated 25
so the network POSTURE check does not warn.

**Arm 33 (obs 10 / iono 15) is the pathology, reproduced.**  Level 3 h mean −14.48 with
sd 13.07, ignition hour −29.49, and **58 out-of-band epochs / 58 withheld emissions**.
This is `project_global_sh_iono`'s 10°-mask divergence class showing up at 61 stations:
a low observation mask feeding low-elevation code contamination into the SH constraint.
The 25° cutoff was the fix for exactly this, and arm 33 shows the cutoff alone still holds
the line when the observation mask is low — which is why the cutoff was set the way it was.

### 21.1 Why this is physically coherent

a₀₀'s only handle is mapping-function spread, so it needs LOW-elevation lines of sight —
but those same LOS carry code multipath, obliquity and near-horizon mapping error.  The two
masks resolve that tension at different places:

- The **observation mask** decides which LOS exist at all.  Raising it 10 → 15 removes the
  worst multipath *before* it reaches any estimator, which is why the level improves and
  the residuals improve slightly.
- The **iono cutoff** decides which existing LOS are allowed to constrain the SH field.
  Lowering it 25 → 15 re-admits the 15–25° band as SH constraint rows; those rows fight the
  phase observations, which is what the +27…46 % postfit rms measures.

§18's footprint/global inversion (measured at 15/15: footprint −1.11 vs global −2.92,
rms 4.85 vs 6.10, where 10/25 was −9.7 vs −6.1 and 11.2 vs 6.9) therefore belongs to the
observation-mask half too, and should be re-measured on arm 32 before it is quoted.
⚠ That footprint comparison is 3 h against §18's 12 h and is NOT time-matched.

### 21.2 Recommendation

**obs mask 15 / iono cutoff 25** (arm 32).  It captures the whole level gain the directive
was after, avoids the whole residual cost, and leaves the validated cutoff and its posture
check intact.  The config edit is one line in `_ft63_24h_conv.json`
(`estimation.ionosphere_weighting.elevation_cutoff_deg: 15 → 25`); the observation mask
stays at the new 15.

⚠ Whatever is chosen, **arm 32 or arm 31 becomes the new denominator** — every result in
§9–§20 was measured at 10/25 and none of it is comparable across this change.

---

## 22. ★ The mask trade is REGIME-DEPENDENT — it flips sign between 61 and 303 stations (2026-08-05)

§21 recommended obs 15 on the strength of a 61-station 2×2.  The user's course-correction
("keep 15/15 but re-test at 305 stations first") was right and the retest **reverses the
conclusion**.  Matched pair, 303 stations, 1 h, SH deg 8, one variable — the mask posture:

| | EXT-305 @ 10/25 | EXT-305 @ 15/15 |
|---|---|---|
| a₀₀−GIM hour 0 | **−1.98** | **−14.78** |
| a₀₀−GIM run mean / sd | −1.99 / 3.64 | **−14.74** / 3.99 |
| postfit phase rms (20–60 min) | **0.00600 m** | **0.00990 m (+65 %)** |
| phase obs | 423 169 | 376 070 (−11 %) |
| sh_health | 0/0/0 | 0/0/0 |

**At 303 stations 15/15 is worse on BOTH axes** — the level degrades by 12.8 TECU and the
residuals by 65 %.  At 61 stations the same change IMPROVED the level (−9.31 → −3.02).
Side by side:

| posture | 61 sta level | 61 sta rms | 303 sta level | 303 sta rms |
|---|---|---|---|---|
| 10/25 | −9.31 | 0.01020 | **−1.99** | **0.00600** |
| 15/15 | **−3.02** | 0.01540 | −14.74 | 0.00990 |
| 15/25 | −4.10 | **0.00800** | *not run* | *not run* |

Both predictions written into `_ext305_gps_dual_1h_m15i15.json` before the run are
**FALSIFIED**: the residual penalty did not shrink with 4.8× the observations (it grew,
+51 % at 61 sta → +65 % at 303), and the level did not land within ±1.5 TECU of zero
(−14.78).

### 22.1 Why the sign flips — and why it was predictable from the original finding

a₀₀'s only handle is mapping-function spread, so low-elevation LOS carry the information
a₀₀ needs AND the code multipath that corrupts it.  Which term wins depends on how well
the field is otherwise constrained:

- **At 61 stations the field is information-starved.**  The 15–25° band's mapping spread is
  worth more than its contamination, so admitting it helps the level.
- **At 303 stations the field is already well constrained** (a₀₀−GIM −1.98 at 10/25).  The
  same band now adds mostly contamination, which dominates — the classic low-elevation code
  failure.

This is exactly the regime the 25° cutoff was ORIGINALLY set in: `project_global_sh_iono`'s
a₀₀ 296 → −305 TECU divergence was a **305-station** result.  So the cutoff is a
large-network fix, and §21's 61-station 2×2 was measuring a different regime — which is why
its recommendation did not transfer.  **Do not generalize a mask verdict across network
size.**  (Same shape as CLAUDE.md's lane-layer amendment: a knob decided at one scale and
found scale-dependent at another.)

### 22.2 Standing recommendation

**`elevation_cutoff_deg` = 25 in BOTH regimes** — it is the best cell at 61 stations
(arm 32: rms 0.00800, the best of any arm) and its removal is catastrophic at 303.
The OBSERVATION mask is the genuinely open knob: 15 helped at 61 stations, and the
**305 @ 15/25 cell is UNRUN** — that is the candidate for the best posture overall and the
one measurement that would close this out (~34 min).

---

## 23. Roadmap item 1, re-run on the CURRENT denominator: §19's FAIL does not reproduce, and LV-4's cost is a FUNCTION OF THE MASK (2026-08-05)

§19 closed item 1 FALSIFIED at stage 1 on a single criterion — the SH health gate, 46
out-of-band epochs / 46 withheld emissions — measured on `_ext63_gps_dual_1h_lv45.json`
against the 10/25 canonical.  §21 then closed with: *"arm 32 or arm 31 becomes the new
denominator — every result in §9–§20 was measured at 10/25 and none of it is comparable
across this change."*  §19 is in §9–§20, and its failing criterion sits on the axis the
mask change moves most (§21: obs 10 → 15 is worth 5–6 TECU of level).  So item 1 was
re-run at the corrected operating point.

§19's mechanical claim was verified first and it **stands**: with the de-mean off the
loader forces LV-5 off (`GnssConfigLoadNetwork.cpp:6630`), so a config setting only
`rx_cb_carry_step_gate` genuinely *is* the item-1 posture.  Independent instrument check:
the grader used here reproduces §19.1's published numbers to the digit off the same run
directory (0.01670 m, a₀₀ 2.69 ± 1.59, min −0.76, 46/8/46, 97.5 % folds, 1.76 % erases).

### 23.1 Stage 1 at 61 stations, obs 15 / iono 25 — arm 34

`_ext63_gps_dual_1h_lv4_m15i25.json` vs arm 32 (`runs/ext63_dual_3h_m15i25`, whose first
hour is the matched control).  Same build, same window, ONE knob apart, and — unlike
§21's mask arms — **identical observations**: 78 931 phase obs / 4935 STA rows in both.

| §17 stage-1 gate | threshold | arm 34 @ 15/25 | §19's arm @ 10/25 |
|---|---|---|---|
| folds | ≥ 95 % | 98.9 % (7295/7374) | 97.5 % |
| erases | ≤ 2 % | **0.25 %** (18, max z 107.7) | 1.76 % (126, max z 744.7) |
| settled sd | ≤ 5 TECU | 1.42 | 1.58 |
| 3-epoch alternation | none | none (flip 0.29 vs control 0.34) | none |
| **sh_health** | **0/0/0** | **0/0/0 — PASS** | **46/8/46 — FAIL** |

**§19's stated cause of death does not reproduce.**  All five §17 gates pass.

The level effect **inverted sign with the observation mask** — this is the surprise, and
it was not predicted (the config predicted LV-4 would again push the level DOWN, by the
same −14.8 TECU it moved it at 10/25, landing ~9.5 and merely re-entering the band):

| arm | mask | a₀₀−GIM, hour 0 |
|---|---|---|
| LV-4 alone | 10/25 | **−28.37 ± 5.22** |
| LV-4 alone | 15/25 | **−2.57 ± 2.92** |
| arm 32 control | 15/25 | −7.23 ± 2.83 |

⚠ Both level figures are FRAME quantities (invariant #5 / §17 item 0) and are read at 1 h,
inside §14's ~2 h convergence budget.  **Diagnostic, not a grade.**  What is a grade is
the postfit phase rms, and there LV-4 still costs: **0.00800 → 0.01080 m, +35 %.**

### 23.2 The mask/scale extreme at 303 stations — arms 35 / 36

`_ext305_gps_dual_20min_m30i30{,_lv4}.json`, obs mask 30 / iono cutoff 30, 20 min
(40 epochs), 303 stations.  At 30/30 the iono cutoff stops binding (every surviving LOS
is already ≥ 30°), so this is "the 15–30° band deleted outright, with no further screening
on what remains".  Verified one-knob at both file and resolved-config level: the only
deltas are `rx_cb_carry_step_gate` and the two output paths.  Identical observations:
98 970 phase obs / 8977 STA rows in both.

| | arm 35 control | arm 36 LV-4 |
|---|---|---|
| **postfit phase rms** | **0.00730 m** | **0.00850 m (+16.4 %)** |
| a₀₀ (ep 10–40) | 28.52 ± 3.09 | 29.52 ± 2.19 |
| a₀₀−GIM run mean / sd | −6.65 / 6.12 | −5.43 / 5.53 |
| a₀₀ posterior σ | 0.4904 | 0.4096 |
| folds | 3609 / 11 966 (30.2 %) | 11 658 / 11 966 (97.4 %) |
| erases | — | 8 (0.07 %, max z 21.2) |
| retired screen would have withheld | — | 81.5 % |
| sh_health | 0/0/0 | 0/0/0 |

The **a₀₀ posterior σ against its own 32.24 ± 2.0 prior** was armed as a circularity trap
before the run — at a 30° mask a "good" level could just be the prior talking, and the
prior (32.24) sits within 0.3 TECU of the GIM 00h global mean (32.0).  It does not fire:
0.41–0.49 against 2.0, i.e. still firmly data-dominated (61-sta arms run 0.65–0.71, the
305 @ 10/25 arm 0.42).  The level reads here are usable.  Mask starvation of a₀₀ is
visible but mild — σ 0.42 → 0.49 for +20° of mask.

### 23.3 The finding: LV-4's data-side cost is a function of the observation mask

Three one-knob pairs, each over identical observations within the pair:

| pair | masks | stations | control | LV-4 | cost |
|---|---|---|---|---|---|
| §19.3 | obs 10 / iono 25 | 61 | 0.01020 | 0.01670 | **+64 %** |
| arm 34 | obs 15 / iono 25 | 61 | 0.00800 | 0.01080 | **+35 %** |
| arm 36 | obs 30 / iono 30 | 303 | 0.00730 | 0.00850 | **+16 %** |

Monotone, and it is what §19.3's own mechanism predicts.  §19.3 proposed that LV-4's full
admission "delivers folds the observations do not support"; the least-supported carry
information is the low-elevation code that sets the receiver DCBs.  Remove that code and
the cost shrinks — by a factor of 4 across the range tested.

⚠ **The confound, stated plainly: arm 34 → arm 36 moves BOTH the mask and the station
count.**  The 61-station step (10 → 15, +64 → +35 %) is clean.  The step to +16 % is not
attributable to the mask alone.  Separating it needs a 305 @ 15/25 LV-4 pair or a 61 @
30/30 pair; neither is run.

**What this does and does not change:**

- §19's *measurement* stands everywhere it was taken.  Its **headline number is the worst
  cell on the axis**, not a constant of the mechanism: "LV-4 costs +64 % postfit phase
  rms" is an obs-mask-10, 61-station statement.
- §19.5's "item 1 is closed FALSIFIED; it does not earn a 3 h or a 12 h stage" **no longer
  follows from the evidence given for it.**  Its sole stage-1 failure was the SH band, and
  the band passes at both current operating points.
- Item 1 is nevertheless **not recommended for promotion**, on the axis that is
  invariant-legal.  In every cell measured LV-4 costs the data side and never pays for
  itself: at its best (+16 %) it buys ~1.2 TECU of level (a frame quantity) and slightly
  lower breathing.  The honest verdict is **"re-opened, ungraded past 1 h, and still
  paying a real residual cost"** — not "falsified", and not "promote".
- §17's staged escalation (1 h → 3 h → 12 h) is **earned but unspent**: stage 1 passes at
  61 sta @ 15/25.  Stage 2 would settle whether −2.57 is ignition (§14 says a 1 h level
  read cannot be trusted) and whether +35 % holds at h2–3.

### 23.4 The §13.5 / §17-item-2 audit does not resolve

Arm 34 vs arm 32, settled 20–60 min:

| instrument | control | LV-4 | |
|---|---|---|---|
| prires `rxcb` σ̂₀ | 0.63 | 3.11 | 5× LOUDER (prior side) |
| postfit phase rms | 0.00800 | 0.01080 | +35 % LOUDER (data side) |
| congruence σ̂₀ | 126.3 | 38.7 | 3× QUIETER |
| block-Baarda σ̂₀ | 33–78 % floored at 1.0 | | INERT at 1 h — §19.4 |

Two instruments say worse, one says better, one is inert.  This is the same split §19.2
recorded ("station congruence is the single instrument pointing the other way, against
four that agree") and it is still unexplained.  §19.4's two instrument holes were
re-confirmed on this lineage: `network_prior_residual.csv` emits only the `rxcb` family of
the five armed, and `network_pf_keyed.csv` has `n_code == 0` in every row.

### 23.5 A third cell for §22's mask axis, and what it implies for §22.2's open question

§22 (concurrent session) established that the mask trade flips sign between 61 and 303
stations.  Arm 35 is independently consistent with it and adds a third 305-station cell —
one that is useful because at 30/30 **the iono cutoff is non-binding**, so it reads the
observation-mask cost with the cutoff confound removed:

| 305-sta cell | a₀₀−GIM mean / sd | postfit phase rms | phase obs |
|---|---|---|---|
| 10 / 25 | **−1.99 / 3.64** | **0.00580** | 166 753 |
| **30 / 30 (arm 35)** | **−6.65 / 6.12** | **0.00730** | 98 970 |
| 15 / 15 | −14.74 / 3.99 | 0.00840 | 148 831 |

(rms over ep 10–40 for window-matching with the 20 min arm; §22's 20–60 min figures differ
in window, not in direction.  Observation sets differ across mask — read direction only.)

Raising the observation mask 10 → 30 at 303 stations, with the cutoff held harmless, costs
**4.7 TECU of level, +26 % rms, and 41 % of the observations**, and nearly doubles the
level's epoch-to-epoch sd (3.64 → 6.12).  §22.2 nominates the unrun **305 @ 15/25** cell as
"the candidate for the best posture overall".  **This cell predicts it will lose to
10/25** — the obs-mask cost at 303 stations appears monotone once the cutoff is not the
thing doing the damage.  Worth the ~34 min to run, but the prior should be that 10/25
already wins at scale.

### 23.6 Predictions, scored

Written into the configs before each run, per house practice:

| arm | prediction | outcome |
|---|---|---|
| 34 | rms +60 % (0.0125–0.0135) | **MISSED** — +35 % (0.01080).  Direction right, magnitude wrong; the cost is mask-dependent, which is §23.3 |
| 34 | sh_health PASSES, level ~9.5 via LV-4's −14.8 offset | **half right for the wrong reason** — PASS confirmed, but LV-4 moved the level **UP** +6.7, not down; the sign inverted with the mask |
| 34 | folds ~97.5 %, erases ~1.7 %, sd ≤ 5, no alternation | **CONFIRMED** (98.9 %, 0.25 %, 1.42, none) |
| 35 | the level gain does NOT continue to obs 30 | **CONFIRMED** — −6.65 vs 10/25's −1.99 |
| 35 | a₀₀ observability starves toward the 2.0 prior | **PARTIAL** — σ 0.42 → 0.49, real but mild; no circularity |
| 36 | LV-4 cost +10…25 %, below arm 34's +35 % | **CONFIRMED** — +16.4 % |
| 36 | LV-4 level above control by a few TECU | **CONFIRMED** — +1.00 TECU (29.52 vs 28.52) |

### 23.7 Provenance

Build `gnss_processor.exe` Aug 5 2026 00:09 (run as `gnss_processor_lv4stage1.exe`, the
renamed-copy convention) — the same binary as arms 31/32/33 and §22's 305 pair, so every
comparison above is same-build per invariant #6.  Configs
`_ext63_gps_dual_1h_lv4_m15i25.json`, `_ext305_gps_dual_20min_m30i30.json`,
`_ext305_gps_dual_20min_m30i30_lv4.json`; runs `runs/ext63_dual_1h_lv4_m15i25`,
`runs/ext305_dual_20min_m30i30{,_lv4}`.  Wall: 99 s, 338 s, 340 s.

⚠ Note for anyone chaining a further arm off these: the loader's `extends` guard is 8
levels and the EXT-305 chain already sits at that depth.  Arm 36's first attempt died at
load ("chain deeper than 8 levels — refusing to continue"); the fix is to extend the same
base as the sibling arm and restate the overrides, which also makes the one-knob claim
structural rather than inherited.

---

## 24. Item-1 STAGE 2 passes every gate — and sharpens the trade to its final form (2026-08-05)

§23 left the staged escalation "earned but unspent".  Stage 2 was spent:
`_ext63_gps_dual_3h_lv4_m15i25.json` (arm 37), 3 h fresh seamless lineage, vs the
existing arm-32 control (`runs/ext63_dual_3h_m15i25`), same 00:09 binary (run as
`gnss_processor_f17.exe`, renamed-copy convention), ONE knob apart, and — the pair's
decisive control — **byte-matched observation sets in every window** (77 988 phase obs
in the settled 20–60 min slice, 119 734 in h2–3, 21 949 harvests; identical in both
arms).  Predictions were written into the config before the run; scored in §24.4.

### 24.1 The stage-2 gate, measured

| §17 stage-2 gate | threshold | arm 37 | control |
|---|---|---|---|
| folds | — | **99.6 %** (21 866/21 949, 0 gate-withholds) | 44.2 % (12 191 withheld by the legacy screen) |
| erases | ≤ 2 % | **0.10 %** (22, max z 107.7 — the same first-hour event arm 34 saw) | — |
| settled sd | ≤ 5 TECU | 1.81 | 4.94 |
| 3-epoch alternation | none | none (flip 0.44 vs 0.37) | — |
| sh_health | 0/0/0 | **0/0/0** | 0/0/0 |
| h2–3 time-matched level | inside ±4..9 | **−0.67 ± 2.09** | +0.44 ± 1.30 |
| breathing | ≤ control's | run sd **2.40** | 4.42 |
| over-retention signature | absent | absent (see §24.3 — BOTH sides louder, not the lane trade) | — |

**PASS on every clause.  Stage 3 (12 h) is earned.**

### 24.2 The level channel: the best tracker any 61-station posture has shown

Time-matched hourly (`gim_global_mean_series.py`), a₀₀ − GIM(t):

| hour | control (arm 32) | **arm 37 (LV-4)** |
|---|---|---|
| 0 | −7.23 ± 2.83 | **−2.57 ± 2.92** |
| 1 | −5.52 ± 4.06 | **−0.58 ± 1.37** |
| 2 | +0.44 ± 1.30 | **−0.67 ± 2.09** |
| run mean / sd | −4.10 / 4.42 | **−1.27 / 2.40** |

No ramp (the §14 stack pathology is absent), half the breathing, and hourly means
within ±0.7 TECU of the GIM from hour 1 on.  §19.1's "the same memory at 97.5 %
admission drags the level out of the physical band" was an obs-mask-10 artifact top to
bottom: at the current denominator, full admission is the best level behavior measured
at this scale.  (Diagnostic under invariant #5, as always.)

### 24.3 The data side: +32 % persists settled, and every loud instrument agrees

| instrument (h2–3, identical obs) | control | arm 37 | ratio |
|---|---|---|---|
| **median rms_phase** (pf_keyed STA) | **0.00870 m** | **0.01150 m** | **+32.2 %** |
| median rms_phase, settled 20–60 min | 0.00800 m | 0.01080 m | +35.0 % (reproduces arm 34 exactly) |
| prires `rxcb` σ̂₀ (settled → h2–3) | 0.63 → 0.95 | 3.10 → **6.33** | 4.9× → 6.6× |
| block-Baarda G σ̂₀ (0 % floored, both) | 2.21 | 3.59 | 1.62× |
| congruence σ̂₀ | 63.7 | 26.3 | **0.41× (quieter — the lone dissenter, again)** |

Two new facts beyond §23.4: **block-Baarda's first usable read on an LV-4 pair** (3 h
unfloors it — 0 % floored in both arms at h2–3) lands on the data-side-louder camp; and
the prior-side tension is **growing with era** in the LV-4 arm (3.10 settled → 6.33 at
h2–3, a doubling in two hours) where the control grows 0.63 → 0.95.  Whether that
compounds toward the §14 stack's 12.62 or plateaus is stage 3's fourth prediction — it
is the number that matters for the item-3 design (a tracking authority that does NOT
tax the prior side).

The over-retention (lane-A/B) signature — quieter priors bought with louder data — is
absent: this posture is louder on BOTH sides while tracking the level best.  The trade
is now in its final, un-confounded form: **the full-admission rolling carry buys the
level channel at the price of fitting both its priors and its observations worse.**

### 24.4 Predictions, scored

| # | prediction (in the config, pre-run) | outcome |
|---|---|---|
| 1 | h2–3 rms 0.0110–0.0125 (+26..44 %) | **CONFIRMED** — 0.01150, +32.2 % |
| 2 | level inside band, above control +2..6, sd ≤ 4.42 | **mostly** — band and sd PASS; above control at h0/h1 (+4.7/+4.9), crosses at h2 (−0.67 vs +0.44) |
| 3 | folds ≥ 98 %, erases ≤ 0.5 %, no alternation | **CONFIRMED** — 99.6 %, 0.10 %, none |
| 4 | prires 3–6×; congruence quieter; block-Baarda records a side | **CONFIRMED** — 4.9–6.6×; 0.41×; louder (1.62×) |

### 24.5 Standing after stage 2

Item 1 remains **not a promotion candidate** on the invariant-legal axis (§23's verdict,
now with a settled 3 h confirmation), and simultaneously the **strongest evidence yet
for §13.4/§15's tracking thesis**: what tracks the level is admitted per-epoch code GF
information flowing through a rolling memory.  LV-4-alone demonstrates the authority
exists; its cost says the carry is the wrong VEHICLE for it.  That is precisely item
3's design brief ("S-consistent harvest" — per-epoch data-derived frame, no fold-side
tax), which stage 3's σ̂₀-growth read (P4) will parameterize.  Provenance: run
`runs/ext63_dual_3h_lv4_m15i25` (54 traces harvested), wall 248 s.

---

## 25. The excursion attribution: the product wire is MARK-FREE, the churn is posture-invariant, and the consumer has no reaction path anyway (2026-08-05)

§17 item 0 closed with "NEXT ANALYSIS on these replays: excursion-hour attribution vs
`products.txt` R/disc events, both arms."  Executed on the 12 h pair
(`runs/ext63_dual_12h_{ref,lv125}` producers, `runs/replay_ext63_12h_{ref,lv125}`
consumers).  The first finding is about the instrument, and it reframes the task:

### 25.1 There ARE no R/disc events — the wire carries values and nothing else

Both 12 h products contain exactly four record kinds: 45 311 C, 1 440 E, 1 440 H,
144 164 P.  Verified absent or inert, in BOTH arms:

- **zero `R` records** (the per-satellite discontinuity bump), zero `phase_disc` /
  `clock_disc` / `code_disc` optional fields, and `datum_changed = 0` on all 1 440 E
  records — through consumer excursions reaching 546 m and 979 m;
- **no `S` contract record at all** on this lineage's wire;
- the `H` heartbeat reads `status=ok` with **σ₀ = 0.000 on every epoch** — its σ₀
  feed is the within-epoch RAIM, which is carry_warm-gated and therefore inert
  without phase_leveling.  The standard-armed-set's known gap, now visible as a
  dead instrument ON THE WIRE;
- every `P` record advertises `sigma=` ≤ 0.56 m (mostly the 0.1 floor) — including
  the epochs where its value steps by hundreds of meters (§25.2).

CLAUDE.md amendment 5's warning ("without `emit_clock_disc` the clock jumps are
silent to the consumer") generalizes to **every channel of this lineage's product**.
The emitter HAS the machinery (per-sat disc counters, `disc_threshold_m`,
`emit_clock_disc`, set-change tracking — `NetworkProductEmitter.cpp:75-136`); none of
it reaches the wire here.  So the attribution below runs against the producer's own
traces — which is exactly the problem: **the producer's event knowledge exists only on
the producer's side.**

### 25.2 The step census (the disc record, computed after the fact)

Mid-arc steps in emitted values between CONSECUTIVELY-PRESENT epochs (gap re-inits
excluded — those are legitimate re-datums the consumer re-initializes through).
Instrument promoted to `scripts/product_step_census.py` — run it on ANY
product-affecting change; a posture that moves the mid-arc census has changed the
consumer's world no matter what the solve-side grades say:

| | ref | lv125 |
|---|---|---|
| phase-bias key-steps > 0.1 m | **387 on 244 epochs (16.9 % of epochs)** | 385 on 245 (17.0 %) |
| median / p90 / max \|step\| | 2.66 / 42.4 / **287.6 m** | 3.19 / 45.1 / 255.0 m |
| clock common-mode steps > 0.1 m | **0** | **0** |
| per-sat clock outliers > 0.5 m | **0** | **0** |

Three structural facts.  (1) **The emitted PB stream steps constantly and enormously,
unmarked** — somewhere in the constellation, a multi-meter unmarked step lands about
every sixth epoch, all under σ=0.1-class advertised uncertainty.  The paired L1/L2
signature of the giants (G17 −215/−288 m at h4.99, G19, G09, G06…) is integer
re-basing on reference-path slips — the emit re-base preserves those steps BY DESIGN
(40 ref-path slip steps preserved per 3 h; "91 % of emitted PB discontinuities are
ref-path re-referencing", CLAUDE.md).  The design assumption is that a datum step is
harmless if marked or differenced away; on this wire it is neither.  The census ties
out against the producer's own run-end counters: the PB emission-continuity filter
reports **394 ref-path-slip resets in BOTH arms** (of 462/458 total resets), and the
wire census independently recovers 387/385 of them — **the producer counts every step
it ships, and marks none of them.**  (2) **The churn is posture-invariant**: same
satellites, same TOWs, same magnitudes, and the same 394 ref-path-slip count in both
arms — the LV stack neither causes nor cures it (consistent with §17's "the campaign's
true blocker is product internal consistency / churn", now measured as identical
across the postures the campaign compared).  (3) **The emitted clock is smooth on this lineage**
at emission granularity — the "13 product-wide clock jumps" of amendment 5 belong to a
different (phase-leveled) lineage; under EXT-CB the phase-bias channel carries the
churn instead (free per-(sat,band) PBs make phase rows clock-blind, §-rootcause memo).

### 25.3 Excursion onsets align with RE-REFERENCING EVENTS, not with value steps

Consumer excursion windows (err3d > 20 m) and producer-side onset context
(−120 s..+60 s), with enrichment vs each event's 12 h base rate:

| arm | window | peak | onset enrichment (× base) | PB steps at onset |
|---|---|---|---|---|
| ref | h1.51–1.68 (22 ep) | 308 m | **col_removed 12.7×**, refsat_reset 2.5× | 0 |
| ref | h3.30–4.70 (169 ep) | **546 m** | **refsat_reset 5.0×** | 2 |
| lv125 | h2.41–2.67 (32 ep) | **979 m** | eq34_reattach 2.1× | 4 |
| lv125 | h4.27–4.58 (38 ep) | 660 m | refsat_reset 5.0×, block_outlier 1.7× | 3 |
| lv125 | h6.48–6.60 (15 ep) | 293 m | col_removed 5.1× | 0 |
| lv125 | h9.51 (3 ep) | 42 m | **sbc_engage 7.6×**, refsat_reset 5.0×, col_removed 5.1× | 1 |
| lv125 | h10.47–11.99 (184 ep) | 22.8 m steady | col_removed 7.6× | 1 |

PB-step density INSIDE excursions is at base rate (0.27/ep) and clock steps are zero —
the naive "a big product step caused it" story fails.  What marks the onsets is the
**event class that changes which datum realizes the product**: reference-satellite
resets, topology column removals, SBC gate engagements, eq34 re-attachments.  All of
them are invariant #7's edges; none of them has any wire representation.  Consumer AR
is essentially never fixed inside excursions (≈1 epoch per window; 0.3 % fix rate
overall) — these are FLOAT-side failures, so wrong-fix poison is ruled out too.

Honest caveat: onset windows hold 2–6 events each, so the 5–13× enrichments are
suggestive, not mechanism-proof.  The load-bearing facts are the structural ones:
mark-free wire, endemic unmarked steps, float-side failures, and §25.4.

### 25.4 The h9.5–11.5 storm: common stressor, differential response — the internal-inconsistency reading, quantified

Both producers show the SAME event storm at h9.5–11.5 (they process the same
observations): refsat_reset at 0.142/ep (5–7× base) and sbc_engage at 0.154/ep
(4.4×) in BOTH arms, epoch-for-epoch.  The consumer response differs 3×:

| era h9.5–11.5 | ref | lv125 |
|---|---|---|
| consumer median 3D | 6.72 m | **20.72 m** (p95 ≈ median — a stable bias, 184 contiguous ep > 20 m) |

Same stress, same product-churn statistics, one consumer rides it out at 7 m and the
other locks a steady 21 m frame error — during exactly §14's pin-ramp era.  This is
§17 item 0's "the late-era stack product is INTERNALLY inconsistent" with the stressor
identified: the storm's re-referencing is the moment the pinned rx-CB frame and the
drifting iono/clock content get pulled apart into estimable space.

### 25.5 What this changes (and does not)

1. ~~**The wire needs its event channel armed** — but that alone is INERT downstream:
   the closed-loop / replay consumer has **no reaction path to any per-satellite
   disc**.  It needs the RT seam's equivalent.~~  **RETRACTED — FALSE.  See §25.7:**
   the replay consumer IS a `RealTimeEngine` and all five `RealTimeEngineSsrBreaks`
   handlers run unconditionally (`RealTimeEngine.cpp:479-483`); the phase-disc
   reaction fired **83 times** in the very arm this section graded, silently, because
   `logging.enable_rt_engine` defaults false (`GnssConfigLoad.cpp:3075`).
   The card is therefore two-sided by construction: emit the marks (producer, cheap,
   machinery exists) + react to them (consumer seam, the real work).  Until both
   exist, "adapt-and-EMIT" is invisible at the consumer — invariant #7's consumer
   edge, formalized.
2. **Churn is confirmed as the campaign blocker and it is posture-independent** — no
   LV/level decision changes it.  The level cards and the churn card are separable
   work streams; §17's arc already said this, the census makes it quantitative.
3. The mark-free H heartbeat (σ₀ ≡ 0) upgrades the standing-set note "within-epoch
   tier is carry_warm-gated ⇒ inert without leveling" from a producer-side caveat to
   a **consumer-facing defect**: the one health signal on the wire is structurally
   dead on any non-leveled lineage.

### 25.6 Item 0's diagnostic rider, discharged: the fleet-bias mean does NOT drift diurnally

§17 item 0 also asked whether the receiver fleet's GF-bias mean itself moves through
the day (receiver thermal) — which would make §14's a₀₀ wander partly physical.
Estimator-free sweep (the `extcb_gim_verdict.py` instrument, hour-bucketed: raw RINEX +
COD SP3/OSB vs the day's hourly GIM maps, same 30-station subsample, el > 20°,
`scripts/fleet_bias_diurnal.py`):

| hour | fleet bias mean | median | std |
|---|---|---|---|
| 00 | −14.76 | −22.92 | 21.50 |
| 02 | −14.31 | −22.28 | 22.03 |
| 04 | −15.26 | −21.99 | 21.56 |
| 06 | −16.00 | −23.28 | 20.51 |
| 08 | −15.35 | −23.79 | 22.18 |
| 10 | −16.15 | −23.91 | 22.81 |

(Hour 0 reproduces the root-cause memo's −14.76/−22.92/21.50 to the digit — instrument
closure.)  **The mean moves −1.4 slant TECU in ten hours** (median −1.8) — quasi-static
against the level channel's ±6 TECU breathing and §14's −4..−15 wander.  Per-station
drift is ±2–5 TECU sd, and the tail is geography-coded, not hardware-coded: the largest
drifters (HKSL −16, SGOC −10, GAMB +7, COCO −6) are all equatorial-anomaly-band
stations, i.e. GIM misfit moving through local time — the SPREAD/dipole channel, not
the mean.  Two conclusions: the −5 ns convention offset is stable hardware (the
zero-mean-datum analysis stands as a STATIC frame choice), and **the a₀₀ breathing is
estimator realization noise, not receiver physics** — which is consistent with §24.2,
where doubling the admitted information halved it.

### 25.7 CORRECTION (2026-08-05, same day): §25.5's central claim was FALSE — the reaction path exists, fires, and doesn't touch the excursions

Caught by the user; measured by the follow-up probe arms
(`_ext63_replay_12h_ref_{twin,ctl,rtlog}.json`, runs
`replay_ext63_12h_ref_{twin,ctl,rtlog}`).  **The current statement of edge 1 is
`HANDOFF_edge_response_queue.md`**; this subsection records what changes in §25's
own claims.

1. **"No reaction path" — RETRACTED.**  `Application::runConsumerReplay` drives a
   `RealTimeEngine` (`ApplicationConsumerReplay.cpp:222`) and `processEpoch` calls
   all five `RealTimeEngineSsrBreaks` handlers unconditionally
   (`RealTimeEngine.cpp:479-483`).  In the very arm §25 graded, the phase-bias-disc
   reaction fired **83 ambiguity resets over 12 h** — invisibly, because the
   `rt_engine` logger defaults OFF (`GnssConfigLoad.cpp:3075`) and its log was
   0 bytes.  Twin run with the logger armed: solve byte-identical
   (`closed_loop_trace.csv` / `consumer_replay_hourly.csv` md5-equal), 83 resets
   visible.  Method lesson, now a standing corollary of invariant #4: **an empty
   log is not an inert path — check the logger's enable flag; and absence of a
   BEHAVIOR must be verified at the call-graph level, not by grepping one file.**
2. **"Mark-free wire" (§25.1) — stands for the marks, but two refinements.**  The
   phase channel needs no serialized mark: `NetworkProductReader::replay`
   re-derives disc counters by replaying values through an emitter
   (`emitToStore`), which is why the reaction lives.  And the wire is not merely
   unread-able but UNREAD in one case: all 1440 H heartbeat records are dropped by
   the batch replay reader (only `NetworkProductTail::lastStatus` surfaces H, and
   replay does not use the tail).
3. **A real producer-side defect found by the probe (§1.3 of the handoff):** the v1
   wire carries no S contract (`serialize_sbasis_contract` defaults false), so the
   replay emitter falls back to a flat 0.05 m disc threshold while the producer ran
   0.25·λ — **producer and consumer count different events** (45 spurious band-2
   resets, 5 missed band-1, of 437 producer-true events).  Fix is arming that one
   default-OFF knob; NO code change (the `disc_threshold_m` hardcode an earlier draft
   flagged is correct — there is no config knob for the flat threshold, and the
   emitter is built with the same constant at `ApplicationNetworkSetup.cpp:950`).
   **EXECUTED AND PASSED 2026-08-05** (handoff §1.7, arms `ext63_dual_3h_contract`
   vs arm 32, replays `replay_3h_{v1ctl,v2}`): arming the knob is header-only with
   the E/C/P record stream **byte-identical** (0 diff lines, invariant #6 holds),
   and it removes **exactly** the predicted spurious band-2 population — 17 → 13
   resets, all four L2W, L1W unchanged — worth **last-30 3D RMS 11.68 → 10.20 m
   (−12.7 %)** on identical observations.  Arm it on any producer run that will be
   replayed or tail-consumed.  Correction recorded there: a v1 wire defaults ALL
   FIVE replay-emitter parameters, but the clock channel's silence is NOT a
   transit artifact — the clock counter has no magnitude jump test, so with
   `emit_clock_disc=1` correctly carried it still fires 0 for want of an EVENT.
4. **The load-bearing negative result (§1.4): the 83 live reactions have ZERO
   enrichment at excursion onsets** (0 resets at both onsets; 0.91× inside windows).
   §25.3's conclusion survives and sharpens: the excursion driver is the
   re-referencing event class, which has no wire representation — and no amount of
   PB-disc marking/reacting reaches it.  §25.5's original success criterion
   ("excursion mass drops") is retracted for that channel; the revised criterion is
   onset ENRICHMENT of a new event-class record (handoff §1.6).
5. **⚠ Scope caveat on all §25 replay numbers: the replay arms ran at the void
   10/25 posture** (07:52, before the 09:01 root-mask edit).  Reproduction requires
   restating `elevation_mask_deg: 10` + `elevation_cutoff_deg: 25` (the twin config
   does).  Measured while proving the twin: the consumer is bit-identical under the
   iono cutoff (15 vs 25) and moves with the OBS mask (last-30 RMS 2.78 m at 15 vs
   6.70 m at 10) — and **invariant #6 (same-build) is INSUFFICIENT for pairing:
   pair on the resolved CONFIG CHAIN as well**, because a concurrently-edited shared
   base re-postures every descendant silently.

---

## 26. The 305 @ 15/25 cell: a data-side TIE, and the footprint INVERTS the level verdict — the mask study closes on a unified posture (2026-08-05)

`_ext305_gps_dual_1h_m15i25.json` (arm 38) — the cell §22.2 nominated and §23.5
predicted would lose to 10/25.  303 stations, 1 h, same 00:09-family binary
(`gnss_processor_f17.exe`), one knob vs the m15i15 sibling (cutoff 15 → 25 + paths;
verified at file level; phase obs 375 563 vs 376 070, −0.13 %, not material).  Wall
2 124 s.  Predictions were in the config; scored in §26.3.

### 26.1 The completed 305 factorial

20–60 min settled window, matched across all cells; level from
`gim_global_mean_series.py` (time-matched); footprint from
`field_vs_gim_footprint.py --radius 5 --every 1` with the RUN'S OWN 303-station
config (⚠ instrument note, §26.4):

| 305 cell | rms_phase | ep10–40 rms | a₀₀−GIM global | **footprint(5°)** | breathing sd | a₀₀ post-σ | sh |
|---|---|---|---|---|---|---|---|
| 10/25 | **0.00600** | 0.00570 | **−1.99** | −6.34 (rms 6.81) | 3.64 | 0.433 | 0/0/0 |
| 15/15 | 0.00990 | 0.00830 | −14.74 | −3.22 (rms 3.95) | 3.99 | 0.281 | 0/0/0 |
| **15/25 (arm 38)** | 0.00620 | **0.00560** | −5.81 | **−3.51 (rms 3.90)** | **1.86** | 0.433 | 0/0/0 |

**Data side: a statistical tie.**  Restoring the validated cutoff recovers ~95 % of
15/15's +65 % penalty (0.00990 → 0.00620 vs 10/25's 0.00600); each of 10/25 and 15/25
wins one window by ≤ 3 %.  §22's "at 303 stations 15/15 is worse on BOTH axes" was a
CUTOFF statement, not an obs-mask statement.

**Level: the global mean and the footprint DISAGREE, and the footprint is the honest
instrument.**  On the global mean, §23.5's monotone prior held (−1.99 → −5.81 for
obs 10 → 15).  Over the network's own pierce points it reverses: arm 38 −3.51 vs
10/25's −6.34, footprint rms halved (3.90 vs 6.81).  §18's structure ("the residual is
~3× larger over the footprint than globally, in every arm") is therefore an
**obs-mask-10 artifact at scale**: at obs 15 the inversion flips, in BOTH obs-15 cells
(15/15's footprint is −3.22 — its catastrophic −14.75 global mean is almost entirely
the extrapolated hemisphere).  Decomposition, physically coherent with §21.1:

- the **obs mask owns the footprint** — the low-elevation code that damages the field
  lives exactly where the network has data (obs 10 → footprint −6.3; obs 15 → −3.2..−3.5);
- the **cutoff owns the residuals and the extrapolated hemisphere** — cutoff 15 lets
  the 15–25° band fight the phase data (+60 % rms) and warps the Kaula-extrapolated
  side (global −14.7);
- **10/25's celebrated −1.99 global mean is won by CANCELLATION** between a bad
  footprint and the opposite-signed unsupported hemisphere — the best-looking global
  number in the factorial belongs to the arm with the worst field where consumers are.
  (15/15's a₀₀ posterior σ 0.281 — tightest formal confidence, worst level — is the
  same lesson from the other side: contaminated rows talk confidently.)

Stability is not close: arm 38 halves the breathing against both siblings (run sd
1.86, settled 1.64) — consistent with §24.2's mechanism reading (breathing is
realization noise; cleaner admitted information quiets it).

### 26.2 The mask study CLOSES: one posture serves both scales

**obs 15 / iono 25** is best-or-tied at BOTH scales: at 61 stations it dominates
outright (arm 32 — best rms of any arm, +5 TECU of level); at 303 it ties the rms,
wins the footprint level ~2×, wins breathing 2×, keeps the validated cutoff, and the
posture check stays silent.  The only axis 10/25 still wins is the global-mean
diagnostic — which §26.1 shows is won by cancellation.  §22's "do not generalize a
mask verdict across network size" survives as method; its practical sting is gone:
the scale-dependence belonged to the CUTOFF arm of the 2×2, and with the cutoff held
at 25 the obs-mask choice no longer flips sign with scale.

**Recommended config action** (not taken here — the masks were set by directive): the
lineage root `_ft63_24h_conv.json` currently pins 15/15; the one-line edit
`estimation.ionosphere_weighting.elevation_cutoff_deg: 15 → 25` lands the unified
posture everywhere, is now validated at both scales, and un-warns every network run.
New denominators for all future comparisons: **arm 32 (61 sta) and arm 38 (305 sta)**.

### 26.3 Predictions, scored

| # | prediction (in the config, pre-run) | outcome |
|---|---|---|
| 1 | rms 0.0062–0.0072, better than 15/15, worse than 10/25 | **CONFIRMED** at the good edge — 0.00620 (+3.3 % vs 10/25); recovery larger than predicted, and arm 38 WINS the ep10–40 window |
| 2 | level −2..−7, does not beat 10/25's −1.99 | **CONFIRMED on the global mean** (−5.81) — and exposed as the wrong instrument: the footprint verdict reverses (−3.51 vs −6.34) |
| 3 | sh_health 0/0/0; obs ≈ m15i15 within ~1 % | **CONFIRMED** — 0/0/0; −0.13 % |
| 4 | 10/25 remains the best 305 posture on the grade | **MISSED in substance** — it is a tie on the grade, and 15/25 wins every consumer-relevant axis; "15/25 is the best obs-15 posture" holds trivially |

§23.5's monotonicity prior was right about the number it named and wrong about what
the number meant.  The method lesson mirrors §23's: a verdict read off a FRAME
quantity (the global a₀₀) at one operating point does not survive a better
instrument.  Level claims from here on quote BOTH the time-matched global series and
the footprint restriction, per §17's grading protocol — now with measured proof of
why.

### 26.4 Instrument note (invariant #4, near-miss)

`field_vs_gim_footprint.py` run bare on a 305 run silently uses its default
61-station `--config` for the footprint AND reads only epoch 1 (`--every` default) —
it printed a plausible-looking, wrong table before the right flags were found.  Any
future footprint quote must carry `--config <the run's own network> --radius 5
--every 1`.  (The §20.2/§18 numbers were produced by the script's author with correct
flags; this note is for the next session that isn't.)

---

## 27. Roadmap refresh — state on hand-back (2026-08-05, end of session)

What changed today, in one paragraph.  The mask study CLOSED on a unified 15/25
posture validated at both scales (§26; recommended root-config edit pending user
sign-off).  Item 1's staged escalation passed stage 2 with the best level tracking
measured at 61 stations and a persistent +32 % data-side cost (§24) — stage 3 is
earned and STAGED but unspent (12 h pair cancelled for time: configs
`_ext63_gps_dual_12h_lv4_m15i25.json` + `_ext63_gps_dual_12h_m15i25.json` sit on disk
with predictions written, posture-checked, run dirs created — ~26 min each when a
session has the budget; the control doubles as the missing 12 h denominator at the
new masks).  §17 item 0 fully discharged: the excursion attribution (§25) found a
mark-free wire carrying endemic posture-invariant PB steps, onsets enriched in
re-referencing events — and (CORRECTED same day, §25.7 / edge-queue §1.0–§1.4) a
consumer reaction path that EXISTS and FIRED 83×/12 h behind a default-OFF logger,
with ZERO enrichment at the excursion onsets; the diurnal rider (§25.6)
measured the fleet bias quasi-static, closing the receiver-thermal hypothesis.  Two
instruments promoted: `scripts/product_step_census.py`, `scripts/fleet_bias_diurnal.py`.

Ranked next actions (supersedes §17's list):

1. **BUILD item 3 — the S-consistent harvest** (§17 item 3, brief unchanged, now
   triply motivated): §24 proved the tracking authority exists in the admitted
   per-epoch code GF information and that the carry is the wrong vehicle (+32 %
   fold-side tax); §25.6 proved the physical rx biases are static, so the frame
   component is the ONLY moving part — exactly what a per-epoch post-solve transform
   carries and a stiff fold-side prior cannot; §19–§24 measured every fold-side
   manipulation of the frame (pin, de-mean, full admission) paying on an invariant
   axis.  Oracle: `C:\LASER\test\GPS3710_test_PMINOLESS`.  Grade against arm 32 / arm
   38 denominators; target unchanged (|a₀₀ − GIM(t)| ≲ 4 at h0 AND h11, zero
   fold-side cost).
2. **The disc-channel card — REVISED, see §25.7 and `HANDOFF_edge_response_queue.md`
   edge 1 (the current statement).**  The original two-sided formulation is retracted:
   the consumer reaction seam EXISTS and FIRES (83 resets/12 h, measured), and the
   §1.4 negative result shows the PB-disc channel's reactions have ZERO enrichment at
   excursion onsets — so "build the consumer seam" is not the work and "recover the
   excursion mass through this channel" is not achievable.  The revised card:
   (a) make the live channel FAITHFUL — arm `network.serialize_sbasis_contract` so
   the replay emitter stops mis-thresholding (flat 0.05 m fallback vs the producer's
   0.25·λ ⇒ 45 spurious band-2 resets, 5 missed band-1); no code change — the
   `disc_threshold_m` "hardcode" at ApplicationNetworkSetup.cpp:1051 is NOT a defect
   (single-source-of-truth with the emitter constant; edge-queue §1.3, retraction
   recorded there) — **DONE, edge-queue §1.7: all four predictions confirmed,
   −12.7 % last-30 3D RMS on the 3 h pair**; (b) give the ACTUAL
   excursion event class (refsat_reset / col_removed / sbc_engage / eq34_reattach —
   no wire representation of any kind) a record, via the existing `R` /
   `datum_changed` carriers behind a default-OFF knob, graded on onset ENRICHMENT,
   not excursion mass; (c) the H rider is TWO defects — σ₀ ≡ 0 (feed from the prefit
   census) and the batch replay reader dropping all 1440 H records unread.
3. **Stage 3 of item 1** — staged, predictions on record (the tracking verdict + the
   σ̂₀-compounding read that parameterizes item 3's requirements).  Run when ~55 min
   of budget exists.  Not a promotion candidate regardless of outcome (§23/§24).
4. **Item 0b — external receiver DCBs (CAS MGEX)** — unchanged; still blocked by the
   ProductParser 4-char-station card.
5. Parked, unchanged: the deg-1 clustered-spread dipole card; the self-contained
   posture decision (§17 item 4).

Grading-protocol addenda (binding for anything touching this doc's subject):
time-matched GIM series AND footprint restriction with correct flags (§26.4), ≥ 2 h
for any level claim at 61 stations (§14) — the 305 network's transient is milder but
1 h remains a rank-only window; `product_step_census.py` on any product-affecting
change (§25.2); postfit rms / prires / block-Baarda (≥ 3 h) / congruence as the
four-instrument audit, expecting the congruence dissent (§23.4, §24.3, unexplained).

## 28. Item 3 BUILT and validated through 3 h — card S2H, the S-consistent harvest (2026-08-07, overnight session)

Knob `network.rx_cb_harvest_demean` (+ `rx_cb_harvest_screen_z`, default 8.0);
default OFF, fingerprinted (`rxcbharv:`), forces the superseded LV-2/LV-5 OFF.
Mechanism (§17 item 3's brief, implemented exactly): at the serial post-solve
harvest, AFTER the LV-4 erase pass and the gap prune (member set = the
fold-eligible set), per family, subtract the Λ-weighted mean of the harvested
ABSOLUTE realizations from every carried entry (λ = 1/var), with the exact
diagonal covariance co-transform (voters var − 1/Σλ, screened var + 1/Σλ =
diag(S·Q·Sᵀ), unit-tested vs an Eigen brute force) and the §16/12.1 robust peer
screen on the μ vote.  Thin epochs apply NOTHING (identity, counted) — never a
frozen μ.  Two documented deviations from the LASER original, both load-bearing:
the de-mean is about the DATUM ROW's target (absolute zero — de-meaning about
x_lin is §13.2's measured failure), and it is Λ-weighted (only the weighted mean
zeroes the NET common-mode force; λ ≡ const reduces to the
`GPS3710_test_PMINOLESS` oracle case, which passes in-suite against the golden
CSVs).  1299-case unit suite green.

Validation (binary `gnss_processor_s2h.exe` md5 `82B4450703C551109DCEFB0B6C212559`;
control = the arm-32 posture re-run on the same binary, reproducing arm 32's
hourly GIM diffs EXACTLY; full record `SESSION_overnight_2026-08-07.md` F6–F9):

- OFF path: products byte-identical across the rebuild (16,286/16,287 lines;
  the single diff is the `#` legend line documenting the new `hsig=` token).
- 20 ep smoke: applied 20/20, n = 61, 0 thin/screened; μ smooth, no alternation.
- 1 h stability: ALL FIVE gates PASS (a₀₀ sd 2.92; time-matched hour-1 fold
  fraction 0.425 vs 0.356 = +19 %; prires rxcb 0.90× QUIETER; rms_phase +2.5 %;
  μ → +0.0000 settled — the projector's fixed point).
- 3 h: ALL gates PASS — h2-3 rms_phase 9.60 vs 9.40 mm (+2.1 %, flat with era);
  prires rxcb 4.43→4.95 vs 4.91→5.55 (0.89–0.90×, growth ratio = the control's,
  where LV-4 DOUBLED); folds 0.523 vs 0.487; step census statistically
  identical (60 vs 59 key-steps, clock 0/0 both) — the removed common mode is
  datum-null, invisible on the wire.

**The §24.5 vehicle question is answered on the fold side: a per-epoch
post-solve transform carries the frame with ZERO fold-side tax** (priors
quieter, folds more frequent, data side flat — the over-retention signature
absent by construction AND by measurement).

Level, DIAGNOSTIC (invariant #5): time-matched hourly a₀₀−GIM
−13.58 → −11.36 → −4.35 ± 1.75 (ON) vs −7.23 → −5.52 → +0.44 (control);
footprint mean −13.26 vs −2.76.  With the carry's common-mode memory zeroed
each epoch the level sits at the per-epoch data+datum equilibrium — LOWER,
toward the zero-mean convention, converging upward at h2-3, with NO
carry-off-class collapse (a stable ±1.75 band vs that mode's −9 TECU/EPOCH).
DEFECT 2's convention offset is now a DECLARED frame property drained per
epoch.  Open, deliberately deferred: the 12 h tracking verdict
(|a₀₀ − GIM(t)| ≲ 4 at h0 AND h11) — whether the a₀₀ anchor's authority (no
longer outvoted by the carry, still outvoted by per-epoch GF data information)
lands the level, or whether §13.4 option 1 / item 0b (external receiver DCBs)
must supply the data-derived authority.  Configs
`_ext63_gps_dual_{20ep,1h,3h}_s2h_*.json` carry the pre-registered predictions;
runs `s2h_*` on disk.  NOT armed by default (no-promotion rule).
