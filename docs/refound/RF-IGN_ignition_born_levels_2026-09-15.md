# RF-IGN: the ignition-born zero-q levels behind the post-fix golden loss (invariant #1, 2026-09-15)

**STATUS**

- **id:** RF-IGN
- **topic:** network-vehicle
- **status:** open
- **summary:** H1 CONFIRMED as the dominant driver: restoring the pre-fix level-harvest era closes 85 to 95 % of the hours 3 to 5 consumer gap, 720 / 720 emitted; H2 REFUTED.
- **dates in the text:** 2026-09-15 (source run `_rf0g_field6` 2026-09-14)
- **knobs / posture:** RF-0g golden + `network.station_prior_before_pd`; `network.phase_leveling_warmup_epochs` (default 6; 76 on the A/B arm; in the warm-resume fingerprint) gating `carry_warm` (`ApplicationNetwork.cpp:1515`, counts EMITTED epochs); `network.phase_leveling_warmup_on_convergence` SH-gated, inert on RF-0g; `network.trace_amb_state` on the vehicle arms only; reader `scripts/rf_ign_level_read.py`
- **hashes named in the text:** exe `gnss_processor_rfign.exe` `14578F851F0C22EC` (a copy of `gnss_processor_rfpdfix.exe`); `products.txt` `9c4ef087fc30bcc4`
- **run dirs named in the text:** `runs/_rfign_rf0g_field6` (the one new run), `runs/_rf0g_field6`, `runs/_rfpdfix_rf0g_field6`, `runs/_rf2d_field6_both`, `runs/_rfpdfix_field6_both`
- **source:** [RF-PD-fix](RF-PD-fix_station_prior_before_pd_2026-09-15.md) section 5.3 (the golden consumer loss and H2); standing invariant #1 in CLAUDE.md

**Outcome.** Question: is RF-PD-fix's golden field consumer loss the ignition-born zero-q level (H1) or the receiver code-bias common mode (H2)? Ending the withholds moved the first level harvest 70 epochs earlier; the rx-CB harvest count is unchanged (42 463 against 42 459). The prediction misses hours 1, 2 and 6 (about 25 % unattributed); the `pl` prior residual is blind to the defect. Nothing promoted; a declared level-harvest era is owed. [RF-0hv](RF-0hv_working_posture_2026-09-15.md) REFUSED the knob on the float vehicle, and [GAL](GAL_gps_galileo_dual_2026-09-22.md) section 6.1 judged a delayed harvest not the fix ([CLV](CLV_clock_level_2026-09-22.md)).

**Status as recorded (2026-09-15):** open — **H1 is CONFIRMED as the dominant driver and H2 is REFUTED as one; ~75 % of the RF-PD-fix consumer loss is the
phase-leveling zero-q level being born 70 epochs earlier, into a 7-epoch-old network.**  The mechanism is `carry_warm` at
`ApplicationNetwork.cpp:1515` — `phase_leveling && n_emitted >= network.phase_leveling_warmup_epochs`, counting **EMITTED** epochs, not
loop epochs — and the entire leveling harvest sits inside `if (carry_warm)` at `NetworkRunContext.cpp:4112`.  Measured on all four source
arms: the golden and the vehicle both moved their first level HARVEST from TOW **261480 (loop epoch 77)** to TOW **259380 (loop epoch 7)**
when RF-PD-fix turned 80 (golden) / 147 (vehicle) gate withholds into 0, and on the vehicle — the only arm with `trace_amb_state` armed —
**1050 of 2315 carry births (45 %) now land inside the ignition window**, against a first birth AT the window's far edge before the fix.
The discriminating knob EXISTS and is config-only: `network.phase_leveling_warmup_epochs` (default 6).  Setting it to **76** on the ON
golden — the value that puts `carry_warm` at TOW 261480, byte-matching the OFF arm's harvest era — recovers the consumer from
12.51 / 7.61 / 8.38 / 8.40 / 11.17 / 13.97 to **7.15 / 4.70 / 2.35 / 1.60 / 2.54 / 5.82 m** (**85 / 95 / 92 % of the hour-3/4/5 gap
closed**) while **emitting 720 / 720 — the whole RF-PD-fix availability win is KEPT**.  The channel is the VERTICAL: U p50 by hour
−11.79 / −6.80 / **−8.28 / −7.83 / −9.87** / −13.37 → −5.91 / −3.80 / **−1.75 / −1.16 / −1.97** / −4.14 m, horizontal barely moving — the
station-common iono/level seat, which is exactly what invariant #1 says a zero-q level re-seeded from one epoch of code carries.  H2 dies
on one number: the receiver-code-bias harvest count is **42 463** on the losing arm and **42 459** on the recovered one (17 809 before the
fix), so the 59-receiver common-mode re-realization H2 names is UNCHANGED across a 4–6× consumer difference.  ⚠ **And the level family's
own instrument is blind to all of it**: the `pl` prior-residual σ̂₀ per hour is 2.9 / 39 / 204 / 180 / 179 / 89 on the losing arm and
0.50 / 9.4 / 144 / 217 / 199 / 104 on the recovered one — statistically the same — because a zero-q level born wrong is self-CONSISTENT
and pays in the observations, not in its own prior residual.  That is invariant #1's "every internal instrument silent" clause, measured.
**Nothing was promoted**: no golden touched, no ledger row, `phase_leveling_warmup_epochs` keeps its loader default of 6, and the residual
~25 % (hours 1, 2 and 6) is UNATTRIBUTED.

## 1. H1 and H2 as stated

**H1 (invariant #1).**  Before RF-PD-fix the golden withheld its ignition epochs, and a withheld epoch returns before the harvest, so NO
level carry was harvested during ignition: every station's zero-q phase level was born from a partly converged network.  After the fix the
gate passes at epoch 1, so every station's zero-q level is born from a COLD network's one-epoch code leveling and carried with zero q for
6 h — a stale ignition-era level fighting the data at every station, with iono/clock absorbing the tension.  The float vehicle's
ambiguities are states WITH covariance and converge, which is why its consumer is fine mid-run; on the sim, ignition is benign, which is
why the sim golden improved.

**H2 (the RF-PD-fix worker's, §5.3 of that card).**  The receiver-DCB common mode is now realized over 59 receivers instead of 28 and
trades against the slant-iono level the consumer applies as a pair with the code biases.  **H2 predicts BOTH vehicles suffer**, and the
float vehicle does not, mid-run.

Both hypotheses route through the same physical channel (level ↔ iono ↔ vertical), so the error SIGNATURE cannot separate them — only an
intervention can.  §4 is that intervention.

## 2. Step 1 — the trace reads

Reader: `python scripts/rf_ign_level_read.py <run_dir> [...]` (`--sections abcd`, `--window LO HI`, `--pl-warmup N`).  Report-only, no
plotting; every number below is read off the traces the runs already wrote.

### 2.1 (a) WHEN the zero-q level is born

`carry_warm` is reconstructed exactly from `network_epoch_status.csv` dispositions: the count is over epochs strictly BEFORE each row
(`n_emitted` excludes the current epoch, `ApplicationNetwork.cpp:1508`).  "first `pl` FOLD" is the first `network_prior_residual.csv` row
of the leveling family — one harvest after the first stash, and the only proxy available on the golden arms.

| arm | emitted | first EMITTED TOW | **first HARVEST** (carry_warm) | first `pl` FOLD |
|---|---|---|---|---|
| golden OFF `_rf0g_field6` | 640 / 720 | 260430 (ep 42) | **261480 (ep 77)** | 261660 |
| golden ON `_rfpdfix_rf0g_field6` | **720 / 720** | 259200 (ep 1) | **259380 (ep 7)** | 259410 |
| vehicle OFF `_rf2d_field6_both` | 573 / 720 | 260430 (ep 42) | **261480 (ep 77)** | 261660 |
| vehicle ON `_rfpdfix_field6_both` | **720 / 720** | 259200 (ep 1) | **259380 (ep 7)** | 259410 |
| RF-IGN `_rfign_rf0g_field6` (§4) | **720 / 720** | 259200 (ep 1) | **261480 (ep 77)** | 261510 |

**The harvest era moved 70 loop epochs / 35 minutes earlier on BOTH vehicles, by exactly the same amount.**  H1's premise is confirmed,
and so is the fact that the premise alone cannot be the whole answer — the vehicle got the identical shift and its consumer is fine
mid-run.

Birth TOWs proper need `network.trace_amb_state`, which is armed on the VEHICLE arms only; the two golden arms do not write
`network_amb_state.csv` and their birth distribution is **NOT RECOVERABLE** from what was run (the reader says so rather than
substituting a proxy silently).  On the vehicle:

| | vehicle OFF | vehicle ON |
|---|---|---|
| `event=birth` rows | 661 | **2315** |
| first birth TOW | 261630 | **259380** |
| born in ignition [259200, 261630] | 162 (all at the single TOW 261630) | **1050 (45 %)** |
| born after | 499 | 1265 |
| distinct (sta,sat,sig) entries / first-seen in ignition | 621 / 162 | 2134 / **999** |

### 2.2 (b) the `pl` (phase-leveling level) prior-residual family, per HOUR

`network_prior_residual.csv`, family `pl`, label `CLEAN`.  The file carries ONE aggregate row per family per epoch, so "|t|" below is the
per-epoch **worst** |t| (`max_abs_t`), not a per-row distribution — the per-row ladder is not in this trace.  p50 over the epochs of the
hour.

| σ̂₀ p50 | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| golden OFF | 6.67 | 43.07 | 61.21 | 71.87 | 52.17 | 51.43 |
| **golden ON** | 2.92 | 39.33 | **203.67** | **179.54** | **179.21** | 89.39 |
| vehicle OFF | 4.24 | 3.84 | 2.79 | 2.41 | 1.83 | 1.22 |
| **vehicle ON** | **0.57** | **0.75** | **0.76** | **0.95** | **0.90** | 1.18 |
| RF-IGN (§4) | 0.50 | 9.44 | **143.95** | **217.15** | **199.23** | 103.51 |

| folded `pl` rows p50 | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| golden OFF | 208 | 267 | 330 | 400 | 391 | 405 |
| golden ON | 867 | 697 | 578 | 678 | 653 | 701 |
| vehicle OFF | 173 | 174 | 203 | 202 | 208 | 220 |
| vehicle ON | 869 | 869 | 888 | 877 | 809 | 841 |

Per-epoch worst |t| p50, and the count over 4σ̂₀ p50, for completeness: golden OFF 68 / 13 / 57 / 15 / 14 / 23 and 32 / 36 / 25 / 12 / 9 /
15; golden ON 42 / 49 / 6.0 / 9.1 / 7.2 / 15 and 205 / 61 / 2 / 4 / 2 / 18; vehicle ON 50 / 72 / 176 / 266 / 203 / 268 and 48 / 52 / 57 /
69 / 49 / 76.

Three readings, in order of how much they survive §4:

1. **The golden ON's level family is ~3× more dispersed at hours 3–5 and does not decay** (61 / 72 / 52 → 204 / 180 / 179).  That is what
   H1 predicts.
2. **It is not a population-size artifact.**  The vehicle ON folds the same ~869 `pl` rows per epoch from the same 59 stations and its
   σ̂₀ goes the OTHER way (3.84 → 0.75 at hour 2, 2.79 → 0.76 at hour 3).  Same 5× row-count growth, opposite σ̂₀ sign.
3. ⚠ **But the statistic is NOT the channel the loss travels in, and §4 proves it.**  The RF-IGN arm recovers 75 % of the consumer and
   79–85 % of the vertical bias with its `pl` σ̂₀ UNCHANGED (144 / 217 / 199 / 104 against the losing arm's 204 / 180 / 179 / 89 — if
   anything higher at hours 4–5).  **A zero-q level born at the wrong value is self-CONSISTENT** — it is carried with zero process noise,
   so its epoch-over-epoch prior residual is small no matter how wrong the value is, and the error is paid by the observations.  Any
   future card that tries to catch this class with the `pl` prior-residual ladder is grading the wrong quantity.

### 2.3 (c) is the level ever re-tapped, or is it zero-q for 6 h?

`network_events.csv`, per hour, golden arms.  **It is re-tapped, heavily** — so "zero-q for 6 h" is false as stated; what is true is that
every one of these responses RE-LEVELS from one epoch of code, which is invariant #1's own re-coupling edge (`SPEC_slip_relevel_walk`).

| event | golden OFF h1..h6 | total | golden ON h1..h6 | total |
|---|---|---|---|---|
| `T4_attrib/attrib_arc` | 1 / 22 / 55 / 63 / 101 / 118 | 360 | 30 / 42 / 71 / 31 / 145 / 117 | **436** |
| `PHASE_ANCHOR/anchor_handover` | 12 / 29 / 27 / 35 / 40 / 30 | 173 | 24 / 29 / 27 / 35 / 40 / 30 | 185 |
| `PHASE_ANCHOR/anchor_slip_handover` | 1 / 2 / 2 / 2 / 7 / 0 | 14 | 9 / 2 / 2 / 2 / 7 / 0 | 22 |
| `T2_within_epoch/we_refloat_arc` + `_sat` + `we_exclude_station` | — | 18 | — | 15 |
| `T0_gap/arc_resume` | 31 / 51 / 37 / 33 / 46 / 50 | 248 | identical | 248 |
| `T0_mw_detect/lli_slip` \| `mw_wl_slip` | — | 185 \| 19 | identical | 185 \| 19 |
| `T1_if_slip/if_slip` \| `refsat_reset` | — | 117 \| 19 | identical | 117 \| 19 |
| `T0_band_scope/common_band` | 141 / 47 / 70 / 48 / 47 / 62 | 415 | identical | 415 |
| `GATE/phase_heal` *(availability, NOT a level refresh)* | 2295 / 0 / 0 / 0 / 0 / 0 | 2295 | 0 × 6 | **0** |

Two things worth keeping.  **Every raw-observation detector is bit-identical across the fix** (`arc_resume` 248, `lli_slip` 185,
`mw_wl_slip` 19, `if_slip` 117, `refsat_reset` 19, `common_band` 415) — they run pre-solve on the observations, so their agreement is the
A/B sanity gate that says the two runs saw the same data.  And `GATE/phase_heal` — the observability gate's soft-anchor of a near-null
SHARED phase-bias column (`NetworkSolveGlue.cpp:1001`), **not** a level refresh — goes 2295 → 0: with 59 stations in the solve no shared
phase-bias column is near-null any more.  That is the availability half of RF-PD-fix, and it is carried in this table only so it is not
misread as a re-tap.

### 2.4 (d) run summary + the run-end leveling counters

`network_run_summary.csv` plus the run-end tail of `logs/main_log*.log`.

| | golden OFF | golden ON | RF-IGN (§4) | vehicle OFF | vehicle ON |
|---|---|---|---|---|---|
| emitted / withheld | 640 / 80 | **720 / 0** | **720 / 0** | 573 / 147 | **720 / 0** |
| `addStation rejected` | 23 633 | 9 | 13 | 30 340 | 9 |
| `n_sta_in_solve` min / med | — | 58 / 59 | 58 / 59 | — | 58 / 59 |
| `n_sta_pd_dropped` / `n_sta_readmitted` | — | 9 / 11 | 13 / 13 | — | 9 / 11 |
| loop ms / epoch | 200.9 | 322.3 | 255.5 | 260.9 | 326.4 |
| leveling **[A] departures** (900 s prune) | 553 over 228 ep | 1264 over 391 ep | 1146 over 356 ep | 341 over 124 ep | 1268 over 391 ep |
| leveling **[B] the cliff** (stale inject skips) | 2 over 1 ep | 2 over 1 ep | 2 over 1 ep | **3816 over 20 ep** | 0 |
| leveling **[C] one-solve refloats** (T4) | 694 | 851 | **929** | 82 | 881 |
| `relevel_free` latches / inject-site skips | 377 / 315 | 573 / 449 | **377 / 315** | 377 / 315 | 573 / 449 |
| MW-detect refloats (WL + LLI) | 8 + 103 | 14 + 156 | — | 8 + 103 | 14 + 156 |
| phase-leveling carries ERASED (anchor reref) | 127 | 125 | — | 29 | 140 |
| **rx-CB harvests** | **17 809** | **42 463** | **42 459** | 9 834 | 42 469 |
| rx-CB folds / 10σ-gate withholds | 10 414 / 7 361 | 31 475 / 10 929 | 32 269 / 10 131 | 4 117 / 5 676 | 31 539 / 10 871 |
| global σ̂₀ RUN-POOLED / prior tension | 257.7 / 0.1446 | 171.3 / 0.2092 | 182.9 / — | — | — |
| global σ̂₀ per-epoch median / tension median | 7.68 / 46.80 | 3.03 / 14.90 | — | — | — |

Invariant #4 reading: the wiring is live on every arm — the harvest counters, the T4 refloats and the prune all move with the harvest era,
and the `relevel_free` latch count (377 / 315) is IDENTICAL between the OFF golden and the RF-IGN arm, i.e. it follows the harvest start,
not the station roster.  ⚠ The run-pooled global σ̂₀ and the per-epoch tension both read BETTER on the losing ON arm than on the OFF one
(257.7 → 171.3 pooled; per-epoch median 7.68 → 3.03, tension 46.80 → 14.90).  Those are dominated by the OFF arm's 80 withheld ignition
epochs (worst epoch tension 4.899e15 at TOW 261630) and are not a usable grade of this defect either.

### 2.5 (e) the vehicle ON's last hour

Consumer per-5-min p50 (m), `_rfpdfix_field6_both`, TOW 277 000 – 280 800: 2.87 / 3.48 / 3.54 / 3.64 / 3.75 / 2.53 / 1.46 / 1.22 / 1.52 /
3.30 / 4.96 / 5.56 / 5.01.

**There are TWO excursions, and the first one is a STEP with a named co-incident event.**

1. **TOW 276810 — a STEP.**  err3d 0.383 (276810) → 0.989 (276840) → 2.231 (276870) → 3.156 m (276900).  At exactly TOW 276810:
   a **9-satellite Q3 clock-anchor MASS DECOUPLE** (`CLK_ANCHOR/cta_decouple`, `reason=nis`, on G10 G12 G13 G23 G24 G26 G28 G29 G31) with
   a `cta_disc_landed`/`disc_bump` on every one of them; plus `PHASE_ANCHOR/anchor_handover` G23 STJO00CAN → SAVO00BRA carrying an
   `sts_applied` S-transform of **−8.05 m** and `TOPOLOGY/col_removed PB:G23:L1W`; plus 9 `T4_attrib/attrib_arc` refloats.  That is
   invariant #7's decoupling edge firing on nine clock channels in one epoch.
2. **TOW 279300 — a monotone RAMP, no step.**  From the minimum at 279270 (1.063 m) the error rises continuously to 5.58 m at 280200:
   E +0.47 → +3.75 m and U −0.29 → −4.04 m over 31 minutes (≈ 1.8 mm/s each), N flat.  It begins at the epoch of a second mass decouple
   (8 satellites at TOW 279300, one epoch after a 19-satellite `cta_recouple` at 279150).

⚠ **Base-rate control, because "an event was nearby" is usually worthless here.**  Over the run only **19 of 720 epochs carry any
`cta_decouple` and 9 of 720 carry ≥ 8** (1.2 %).  There are 16 one-epoch consumer jumps > 0.5 m, 13 of them inside ignition; of the **three
post-ignition** jump epochs, **all three** sit one epoch after a ≥ 9-satellite mass decouple (276840/276870/276900 after 276810 with 9;
268890/268920 after 268860 with 23).  So the association is well above its base rate — but **7 of the 9 mass decouples produced no jump at
all**, so a mass decouple is not sufficient, and this is reported as a co-incidence worth a card, not as a cause.  It is a DIFFERENT
mechanism from H1 (it is on the float vehicle, whose level carry §4 shows is not the problem) and it is **out of RF-IGN's scope**.

## 3. Step 2 — the discriminating knob

**It exists, it is config-only, and it is the exact gate H1 names.**

| knob | default | where | what it gates |
|---|---|---|---|
| **`network.phase_leveling_warmup_epochs`** | **6** | field `GnssConfig.hpp:7783`, getter `:1774`, parse `GnssConfigLoadNetwork.cpp:3832-3833`, posture log `:10168`/`:10174` | the `pl_warmup` half of `carry_warm` — `ApplicationNetwork.cpp:1515`: `carry_warm = pl_on && (… : (static_cast<int>(n_emitted) >= pl_warmup))`.  **`n_emitted` counts EMITTED product epochs, not loop epochs**, and excludes the current epoch (`:1503-1508`).  `carry_warm` brackets the ENTIRE phase-leveling harvest + stash at `NetworkRunContext.cpp:4112`. |
| `network.phase_leveling_warmup_on_convergence` | false | `GnssConfig.hpp:7795`, parse `GnssConfigLoadNetwork.cpp:3949-3951`, detector `NetworkLeveling.cpp:180-200` | T1d: warm the carry on SH-field convergence instead of the epoch count.  **INERT on RF-0g** — it requires `sh_model`, and the posture runs `estimate_iono_sh: false` (free slant iono, no SH), so it falls back to the epoch count. |

Everything else the search turned up, and why it is not the knob: `network.warmup_epochs` / `warmup_ura_floor_m` (RTB-7 cold-start URA
quality ramp; `warmup_ura_floor_m` is absent from the arm ⇒ 0 ⇒ the ramp is inert, so raising `pl_warmup` does NOT change the emitted URA);
`network.raim_refloat_maturity_epochs` (0, and documented inert at or below the carry warmup); `network.ura_refloat_inflate` (1.0, so the
`ura_refloat_horizon[sat] = pl_warmup` assignment at `NetworkRunContext.cpp:6160-6161` never runs); `gross_los_warmup_epochs` (120, the
gross-code declaration horizon); `iono_constraint_deweight_min_epochs` (30); `sh_gate_min_epochs`; `resume_slip_warmup_ep`;
`cycle_slip.if_phase_td_ref_warmup_abstain`; `network_ar_min_epochs`; `rx_cb_harvest_demean` / `rx_cb_harvest_screen_z` (the rx-CB harvest,
a different carry).  There is **no knob that delays the level harvest alone**.

Two properties of `phase_leveling_warmup_epochs` that must travel with any use of it:

- ⚠ **It is in the WARM-RESUME FINGERPRINT** (`NetworkRunContext.cpp:742`).  Fresh-convergence lineages only; never flip it mid-chain.
- ⚠ **It delays the whole `carry_warm` ERA, not just the harvest.**  `carry_warm` also gates post-fit RAIM (`ApplicationNetwork.cpp:2602`,
  unless `raim_carry_invariant` re-hosts it), the within-epoch tier, the PASS-B legacy local inject
  (`ApplicationNetworkAssembly.cpp:3088`), `NetworkMeasurementAdmission.cpp:436`/`:741` and `NetworkSignalIntegrity.cpp:398`.  So the §4
  arm reproduces the OFF golden's ignition REGIME faithfully — that regime is exactly what 76 emitted epochs of withhold produced before
  the fix — but it is **not a level-harvest-only intervention**, and the card does not claim it is.

## 4. Step 3 — the A/B

`runs/_rfign_rf0g_field6/arm.json` = `runs/_rfpdfix_rf0g_field6/arm.json` with exactly **two** fields different (verified by a flattened
key diff): `network.product_output_path` → the new run dir, and `network.phase_leveling_warmup_epochs: 76` added.  **76** is the value that
puts `carry_warm` at loop epoch 77 / TOW 261480 on a 720-of-720-emitting run — byte-matching the OFF golden's own harvest-era start.  Exe
`build/x64-Release/gnss_processor_rfign.exe`, a copy of `gnss_processor_rfpdfix.exe`, **sha256[:16] `14578F851F0C22EC` re-hashed after the
copy** (the RF-PD-fix card's value).  Exit 0, 222 s wall / 184.0 s loop.  The arming took: `gnss_config.log` reads `warmup 76 epochs`
against `warmup 6 epochs` on both source arms, and the first `pl` fold moved to TOW 261510 (§2.1).

| consumer p50 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| golden OFF `_rf0g_field6` | 3.77 | 1.08 | 1.28 | 1.25 | 1.78 | 2.68 |
| golden ON `_rfpdfix_rf0g_field6` | 12.51 | 7.61 | 8.38 | 8.40 | 11.17 | 13.97 |
| **RF-IGN `_rfign_rf0g_field6`** | **7.15** | **4.70** | **2.35** | **1.60** | **2.54** | **5.82** |
| *gap closed (ON − IGN) / (ON − OFF)* | *61 %* | *45 %* | ***85 %*** | ***95 %*** | ***92 %*** | *72 %* |

| p90 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| golden OFF | 5.30 | 3.54 | 1.64 | 1.35 | 2.64 | 3.86 |
| golden ON | 14.45 | 8.91 | 9.36 | 10.38 | 17.20 | 16.00 |
| RF-IGN | 7.53 | 7.28 | 2.71 | 5.95 | 5.13 | 6.60 |

| U (vertical) p50 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| golden OFF | +0.63 | +0.79 | +0.76 | −0.51 | 0.00 | −0.96 |
| golden ON | **−11.79** | **−6.80** | **−8.28** | **−7.83** | **−9.87** | **−13.37** |
| RF-IGN | −5.91 | −3.80 | **−1.75** | **−1.16** | **−1.97** | −4.14 |

Horizontal, same order — E golden ON −1.52 / −3.01 / −1.81 / +1.01 / +3.19 / +1.17 vs RF-IGN −1.04 / −1.12 / −1.36 / −0.93 / +1.32 /
+1.22; N golden ON +3.42 / +0.58 / −0.86 / −1.05 / −1.81 / +4.44 vs RF-IGN +3.45 / +2.59 / +0.70 / −0.86 / +0.72 / +2.71.  **The loss and
its recovery are almost entirely VERTICAL.**

Per-30-min consumer p50 (m): OFF `0.90 4.47 1.46 1.02 1.12 1.42 1.15 1.31 1.31 2.27 1.99 3.84`; ON `6.55 14.22 8.64 6.63 5.89 8.71 10.20
7.94 11.21 10.46 14.68 13.56`; **RF-IGN `4.44 7.35 6.05 3.22 2.36 2.33 5.37 1.47 1.51 3.08 6.06 5.62`**.

Availability and wire, same arm: **emitted 720 / 720, 0 withheld** — the knob delays the harvest, it does NOT withhold emission (the
`withheld` branch of the pre-registered prediction is not the one that happened); `addStation rejected` 13 (ON 9, OFF 23 633), in-solve
min/med 58 / 59, `n_sta_pd_dropped` 13 / `n_sta_readmitted` 13; IF-correction dispersion p50 / p90 / max **0.148 / 0.794 / 2.97 m** (ON
0.172 / 0.547 / 3.12; OFF 0.175 / 0.512 / 1.50) — p50 the best of the three, p90 the worst; `CLK_STEP` 3 (ON 1, OFF 25);
`cta_decouple` 343 (ON 354, OFF 297); `products.txt` sha256[:16] `9c4ef087fc30bcc4`.

**Against the pre-registered prediction** ("the consumer returns to the OFF golden's ~1–1.5 m from hour 2 while emitted stays 720"):
emitted stayed 720 — **met**; the return to ~1–1.5 m happens **from hour 3, not hour 2**, and lands at 2.35 / 1.60 / 2.54 against OFF's
1.28 / 1.25 / 1.78 — **substantially met at hours 3–5, missed at hours 1, 2 and 6**.  So the prediction is confirmed in kind and
incomplete in degree: three quarters of the defect, not all of it.

## 5. The verdict

**H1 is the dominant driver.**  Delaying the level harvest to the pre-fix era — changing NOTHING else — removes 85 / 95 / 92 % of the
hours-3/4/5 consumer gap and 79 / 85 / 80 % of the vertical bias, while the 59-station solve, the 720/720 emission and the cured PD
amputation are all retained.  An intervention that specific, on a mechanism named in advance, is as close to causal as this bed allows.

**H2 is refuted as the driver, and the refutation is one number.**  The receiver-code-bias harvest count — the direct measure of the
common-mode re-realization H2 names — is **42 463** on the losing arm and **42 459** on the recovered one (against 17 809 before the fix,
a 2.4× increase that both post-fix arms share in full).  The rx-CB folds (31 475 vs 32 269) and gate withholds (10 929 vs 10 131) likewise
match.  H2's mechanism is present, unchanged and at full strength on the arm whose consumer recovered, so it cannot be what the consumer
was paying for.  H2's own prediction had already failed its first test — it requires BOTH vehicles to suffer, and the float vehicle's
consumer is 0.97 / 1.05 / 0.45 m at hours 3–5 with the identical 4.3× rx-CB harvest increase.

**What the float vehicle adds.**  It takes the identical `carry_warm` shift (261480 → 259380) and the identical rx-CB re-realization, and
its `pl` σ̂₀ moves the opposite way (3.84 → 0.75 at hour 2).  Its ambiguities are states with covariance, so an ignition-era harvest is a
prior the filter can revise; on the leveling vehicle the same harvest is a zero-q commitment it cannot.  That is the RF-1 argument, and
this card is an independent measurement of it on the field bed.

**What is NOT explained.**  ~25 % of the loss, concentrated in hours 1, 2 and 6 (7.15 / 4.70 / 5.82 against OFF's 3.77 / 1.08 / 2.68) and
still ≈ −4 to −6 m of vertical at both ends of the run.  Nothing here attributes it, and the §2.5 mass-decouple co-incidence is a separate
mechanism on a separate vehicle.

**The methodological finding, which outlives the card.**  The `pl` prior-residual σ̂₀ is statistically the same on the losing arm and the
recovered one (204 / 180 / 179 / 89 vs 144 / 217 / 199 / 104 at hours 3–6) across a 4–6× consumer difference.  A zero-q level carried with
no process noise is self-consistent by construction, so the prior residual it generates is small whatever value it was born at, and the
error is discharged into the observations and the vertical.  The instrument that WOULD have seen this is the consumer's vertical and the
harvest-era start — neither of which is a prior-residual statistic.  Invariant #1's "every internal instrument silent" clause is not
rhetoric here; it is the measured state of the instrument set.

## 6. Runs

- `runs/_rf0g_field6` — RF-0g golden, knob OFF (source, 2026-09-14).
- `runs/_rfpdfix_rf0g_field6` — RF-0g golden + `station_prior_before_pd` (source, RF-PD-fix §6).
- `runs/_rf2d_field6_both`, `runs/_rfpdfix_field6_both` — the float-vehicle pair (sources; the only arms with `trace_amb_state`).
- **`runs/_rfign_rf0g_field6`** — this card's ONE new run: the ON golden with `phase_leveling_warmup_epochs: 76`.  Exit 0, 222 s,
  720 / 720 emitted, `products.txt` `9c4ef087fc30bcc4`, exe `14578F851F0C22EC`.

Reader: `scripts/rf_ign_level_read.py`.  Graders re-used unchanged: `scripts/rf2d_grade.py`.

## 7. What stays open

1. **Nothing is promoted.**  `network.phase_leveling_warmup_epochs` keeps its loader default of **6**; no config under `config/golden/`
   was touched, no ledger row was written, and `station_prior_before_pd` stays default OFF.  This card measures a cause; it does not
   propose 76 as a setting — 76 was chosen to REPRODUCE the OFF arm's era, not because it is right.
2. **The fix that this points at is not a bigger warmup.**  A fixed emitted-epoch count is the same class of thing that broke: it made the
   harvest era an accidental function of the withhold count.  What the harvest start needs is a DECLARED era boundary with a convergence
   criterion and both invariant-#7 edges (the decoupling edge bounding what an un-harvested station accumulates, the re-coupling edge
   re-validating the level it stamps in).  `phase_leveling_warmup_on_convergence` is that shape but is SH-gated and therefore inert on a
   free-slant-iono posture — a non-SH convergence criterion is the pre-registration below.
3. **PRE-REGISTERED, NOT BUILT — a declared level-harvest era.**  (a) a convergence criterion that does not require an SH field (candidate:
   the per-station local-block conditional variance of the columns about to be harvested, or the σ̂₀ EWMA update count already maintained
   for the RTB-7 ramp); (b) an `EVENT` at the harvest-era OPEN edge naming the criterion, the epoch and the station count, and at the
   per-station re-harvest edge after any prune/refloat; (c) counters in `NetworkServiceCounters` (per the INC-12 rule) for era-open epoch,
   stations harvested at open, and re-harvests after a response; (d) the bar: on the RF-0g ON posture, consumer p50 within 0.10 m/hour of
   `_rf0g_field6` at hours 3–6 with emitted 720/720 and the vertical p50 |U| < 2 m, plus an OFF smoke byte-identical on `products.txt`
   `9add1d3ab9d70657`.
4. **The unattributed 25 %** (hours 1, 2 and 6).  Not measured here.  The obvious next arm is the same knob at other values (to see
   whether the recovery is monotone in the harvest-era start, which would separate "the level is born too early" from "the level is born
   at all before the network converges"), and a `trace_amb_state` arm on the GOLDEN — §2.1's birth distribution is unreadable on the
   leveling vehicle today because the trace was never armed there, which is a gap this card had to work around.
5. **The vehicle's mass-decouple co-incidence** (§2.5): three of three post-ignition consumer excursions on `_rfpdfix_field6_both` begin
   one epoch after a ≥ 9-satellite Q3 clock-anchor mass decouple (base rate 1.2 % of epochs), but 7 of 9 such decouples are harmless.
   Report-only; a separate card, on a separate vehicle, and the instrument it needs (`CLK_ANCHOR` decouple size vs consumer step, with a
   size-matched null) does not exist yet.
6. **RF-PD-fix's own open items are untouched** — the 9 residual non-finite-input rejections, the carry rows still judged after the PD
   test, and the promotion decision.
