# LV impl: the LV-1 and LV-2 implementation record and first measured outcomes

**STATUS**

- **id:** LV impl
- **topic:** ionosphere
- **status:** closed
- **summary:** LV-1 and LV-2 built default-OFF with live counters; §1's seed-gradient model FALSIFIED (carry-off arms agree to 0.006 TECU): LV-1 acts only through the rx-CB carry.
- **dates in the text:** 2026-08-01 → 2026-08-12
- **knobs / posture:** `network.iono_seed_field_frame`, `network.rx_cb_carry_demean` (both default OFF); `network.a00_prior_from_brdc` deliberately not built
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none (arms 12 to 19 are cited by number)
- **source:** `docs/archive/SPEC_level_information_budget.md` §9, §10 (10.1, 10.2, 10.3, 10.4, 10.5)

**Outcome.** LV-1 (seed reframe) and LV-2 (rx-CB carry de-mean) were built and unit-tested (`LevelInformationBudget`, 13 cases). With the carry off LV-1 has no actuator; with it on, LV-1 changes which carried priors fold through an inject gate that thresholds a frozen frame, a defect [LV-4](LV-4-FRAME-INVARIANT-SCREEN.md) fixed. At 1 h LV-1+LV-2 lifted the a₀₀ mean 16.36 → 35.09 TECU; two predictions MISSED and the deg-1 dipole grew (its card parked, [LV roadmap](LV-ROADMAP.md) §27). [LV correction](LV-CORRECTION-AND-12H.md) §13 corrected the zero-net-force reading (LV-2 pins a frozen frame, which LV-1 sets), and its 12 h verdict retired the stack for settled work.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 9. Implementation record (2026-08-05)

Built, default-OFF, armed in the new arms.

| what | where |
|---|---|
| LV-1 median offset + reframe + counters + the one-shot per-station offset log | `StateInitializer::{stationSeedOffset, computeStationSeedOffset, applyIonoSeedFrame}` (+ `IonoSeedFrame` / `IonoSeedFrameStats`), consumed in `MeasurementModel::ensureRequiredStates`'s iono block (**consume-once**: the frame is disarmed after the block, so a stale field can never seed a later epoch) |
| LV-1 field sampling at the IPP | `networkShSlantAtLos_` (file-local, `ApplicationNetworkAssembly.cpp`), called per station in the PASS-B produce loop before `computeMeasurementUpdate` |
| LV-1 lag-1 field snapshot | `ctx.carry.sh_field_lag1`, written serially post-solve in `networkHarvestLocalCarries`; cold start falls back to the a₀₀ climatology prior mean (Ȳ₀₀ ≡ 1 under the orthonormal basis) |
| LV-2 Σλ·innov / Σλ accumulation + demeaned η and gate | `ApplicationNetworkAssembly.cpp` rx-CB inject block |
| LV-2 μ reduction (pure helper `rxCbDemeanMu`) + freeze counter | `NetworkRunTypes.hpp` (next to `rxCbInjectGate`), called in `networkHarvestLocalCarries` |
| LV-2 μ trace | one `lv2 / rx_cb_demean_mu` row per family per epoch on the EXISTING `network_events.csv` (`networkLeveling`), `action` = fresh/frozen, `detail` = family;n_sta;sum_lambda |
| knobs | `network.iono_seed_field_frame` (+`_min_sats` 5), `network.rx_cb_carry_demean` (+`_min_stations` 8) — parsed in `GnssConfigLoadNetwork.cpp`, mirrored into `NetworkKnobs`; both live in `GnssConfig.hpp` members/getters |
| fingerprint | `rxcbdemean:<min_sta>` nested inside the existing conditional `rxcb:1`; `lv1seed:<min_sats>` conditional — pre-card snapshots unchanged |
| run-end counters | `NetworkTeardown.cpp`: an LV-1 line (reframed / offsets / both fallbacks / field refreshes) and an LV-2 line (μ epochs / freezes / last+max μ / skip rate on the demeaned innovation), each WARNing when armed-but-inert |
| tests | `tests/test_level_information_budget.cpp`, suite `LevelInformationBudget` (13 cases): median vs mean robustness, the min_sats fallback, μ information-weighting, **Σλ(innov−μ) ≡ 0 and difference-preservation as properties**, the freeze floor, the loader's inert-safe forcing of both knobs.  Green 2026-08-05 with the one documented fixture-gated skip, `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`.  (Case counts quoted anywhere in this document — 1251/1252, 1264/1265, 1299 — are historical snapshots; CLAUDE.md's 2026-08-12 re-count is 1360 cases / 196 suites.) |

Two design choices that differ from the letter of §4:

- **μ is keyed by receiver code-bias STATE KEY** (`GPS_DCB`, `GPS_IFB5`), not bare constellation — averaging
  a DCB innovation with an IFB innovation would average different physical quantities. In the GPS
  dual-frequency posture the two coincide.
- **Only FOLDED rows vote** on μ (a gate-withheld row exerts no force, so it must not vote on the force being
  removed), and the mean is over the RAW innovation (using the already-demeaned one would decay μ
  geometrically to zero).

First-run ON-path telemetry (invariant #4), arm 15, 20 ep: **580 seeds reframed, 79 station-epoch offsets, 0
fallbacks** — the counters were live from the first armed run.

LV-3's optional `network.a00_prior_from_brdc` is **deliberately NOT built**. Its prerequisite is VERIFIED;
what remains is a runtime-override design — §30.6.

## 10. Measured outcomes — and the mechanism correction (2026-08-05)

All arms: DOY-129, 61 GPS stations, single-layer deg-8 + a₀₀ GIM anchor 32.24 ± 2 + `rx_code_bias_datum`,
Release, one knob at a time. Reference: COD GIM 32.24 TECU.

### 10.1 §1's gradient model is FALSIFIED — the seed cannot pull the level

§1 attributes the collapse to "per-LOS stiffness toward the seed frame ≈ 0.04 info × 376 LOS ≈ 15 units".
**That channel does not exist on this path.** The per-station state vector is a frozen LINEARIZATION POINT,
the per-epoch absolute solve carries no Kalman prior on the local nuisances, and every row on a GPS iono
column forms its RHS against `x_lin` with that row's own `A_r` coefficient — so a seed shift leaves the
reduced RHS exactly invariant (2026-08-01 theorem, memory `project_network_xlin_invariance`; algebra in
§30.4). The card's own decisive arm settles it, and it is the arm §3 said to STOP on: arm 14 (`…a00gim_sl_nc`,
carry OFF) and **arm 16** (`…lv1_nc`, carry OFF **+ LV-1**) both run 24.5 → -3.5 → −37.1 over 20 ep, endpoints agreeing to **0.006 TECU**, max |Δ| 1.8 TECU
mid-collapse (a near-null direction amplifying ulp differences).  With the carry off, LV-1 has no actuator.

### 10.2 …but LV-1 helps anyway, through a DIFFERENT and defective channel

With the carry ON, the same single knob moves the level a lot (20 ep):

| arm | knobs | ignition | trough | end | mean | in-band | gate |
|---|---|---|---|---|---|---|---|
| 12 (base) | — | 24.69 | **6.05** | 7.10 | 13.28 | 20/20 | 0/0/0 |
| 15 | LV-1 | 24.69 | **13.81** | 14.78 | 18.35 | 20/20 | 0/0/0 |
| 17 | LV-2 | 24.69 | **13.81** | 19.56 | 19.99 | 20/20 | 0/0/0 |
| 18 | LV-1+LV-2 | 24.69 | **19.09** | 19.09 | 25.80 | 20/20 | 0/0/0 |

The channel is the **rx-CB carry's inject gate**, which thresholds the PREFIT quantity |value_abs − x_lin(cb)|
— and `initializeCodeBiases` derives x_lin(cb) by subtracting the *seeded* iono, so the seed frame decides
which carried priors fold (arm 15: folds 448 → 382, skips 710 → 776).
Withholding those folds raises the ignition trough by 7.8 TECU. That is a **row-set** actuator (the one class
the invariance theorem leaves open) riding on a screen that thresholds a frozen arbitrary frame — a defect,
not a design. Named in §30.4; fixed by LV-4 (§11).

### 10.3 LV-2 works as designed, and the 1 h window is where it shows

| arm | window | a₀₀ mean | min | end | fold fraction | in-band |
|---|---|---|---|---|---|---|
| 13 (base) | 1 h | **16.36** | 5.42 | 20.95 | 43.5 % | 121/121 |
| 19 (LV-1+LV-2) | 1 h | **35.09** | 18.38 | **33.08** | **51.9 %** | 121/121 |

Against GIM 32.24: the baseline hour sits **−15.9 TECU low** (DEFECT 2, exactly the measured receiver-bias
mean), the combined posture **+2.85 high on average and 33.08 at the end**. Fold fraction rose 43.5 % → 51.9 %
(skips 4127 → 3511), so the 20-epoch reading that skips got *worse* was an ignition artifact — judge the gate
on the settled window. μ (traced per epoch, `network_events.csv`, tier `lv2`) is the flywheel draining: ±1.8 m
over the first ~20 min, damping to ±0.8 m, mean -0.04 m over 120 epochs, 0 freezes, 15–42 contributing
stations. (The 20-epoch LV-2-alone arm 17 read |μ| ≤ 1.64 m over 18–36 contributing stations, μ computed on
19/19 epochs, 0 freezes — same mechanism, ignition window.)

⚠ **The interpretation "the carry exerts ZERO net force on the common mode, so the designed authority owns the
frame" is WRONG — §13.** The arithmetic stands.

### 10.4 Predictions that MISSED (stated before the runs, recorded honestly)

- **"The skip rate collapses from ~58 %" (§4 arm 1, and this session's arm-18 prediction).** It does not:
  56.5 % → 48.1 % over 1 h, and at 20 epochs it went the wrong way. μ (|μ| ≈ 0.7 m mean) is small against the
  gate's ≈ 5.1 m threshold, so the skips are NOT a coherent common-mode trigger — they are per-station frame
  offsets of a frozen x_lin(cb). **The 58 % is a FRAME artifact**, which is why neither card removes it.
- **"LV-1 re-centres x_lin(cb) ⇒ fewer skips."** False — LV-1 made 20-epoch skips worse (61 % → 67 %). The
  reframed seed (raw − median_raw + field_slant) over-counts the per-LOS structure when the field is good:
  median_raw carries the station's median slant iono, so a LOS far from that median is seeded at roughly I_los
  + (I_field − median_I). It moves the DCB linearization point, but not toward truth.
- **The per-station offset is NOT a clean rx-GF-bias estimate** — see the §3 counters note.
- **Deg-1 dipole is unchanged-to-worse**, as §4 predicted for a mean-fix (the dipole is the clustered
  SPREAD): 1 h peak 35.0 → 37.9, end 20.1 → **29.0**, Kaula ratio 2.01 → 2.90 (open item — the level is fixed,
  the spread is not).

### 10.5 What is now open

1. **The rx-CB inject gate thresholds a frozen frame** (§10.2). → **CLOSED by LV-4, §11.**
2. **The deg-1 dipole grew** with the level fix. Needs its own card; the clustered-spread mechanism was never
   addressed by either LV card.
3. **≥ 2 h and the 305-station gate.** 1 h settles the level but not `total_IF`; the combined posture is the
   candidate to re-open the 305 arm (the original SPEC §4.3 gate).
4. **LV-1 is worth keeping but not for its stated reason.** Quote §10.2 and §13.3, not §1.

---

