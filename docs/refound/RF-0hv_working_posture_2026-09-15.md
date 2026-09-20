# RF-0hv — the WORKING POSTURE for vehicle development, and the harvest-era knob refused on it (2026-09-15)

**STATUS:** open — **the author deferred the golden re-base, one critical run was spent, and it came back a clean NO.**  RF-PD-fix
(`network.station_prior_before_pd`) cured the per-station PD amputation on all four beds and RF-IGN then showed that the cure's cost on
the LEVELING vehicle is invariant #1 — zero-q levels born at emitted epoch 7 out of a cold network — with
`network.phase_leveling_warmup_epochs: 76` recovering 85–95 % of the hour-3/4/5 loss.  The one open question for the FLOAT vehicle was
whether that harvest-era knob belongs in its working posture: `carry_warm` gates the whole carry era, not just the level harvest, so it
had to be measured rather than assumed.  It was, on the vehicle's own cured field bed, one knob different:
**the consumer goes 5.02 / 2.11 / 0.97 / 1.05 / 0.45 / 3.65 → 7.15 / 6.06 / 3.31 / 11.10 / 15.40 / 19.83 m** — worse on all six hours, by
**3.4× / 10.6× / 34× / 5.4×** at hours 3–6 — and the wire instrument agrees independently: **IF-correction dispersion p90
0.201 → 10.908 m** (max 0.68 → 16.51 m), **unsignalled clock steps 0 → 9**, `cta_decouple` 189 → 409, with emitted still 720 / 720.  The
pre-registered decision rule is FAILED in the negative on every clause, so **`phase_leveling_warmup_epochs` does NOT join the working
posture and its sim twin is NOT owed.**  The knob is recorded as **LEVEL-VEHICLE-SPECIFIC**: it repairs the vehicle whose ambiguity is a
zero-q commitment and damages the vehicle whose ambiguity is a floored state, which is RF-1's argument measured from the other side.  The
working posture is therefore **RF-0hv = RF-0g + the float vehicle (RF-1 / RF-2b / RF-2d) + `station_prior_before_pd`, at the loader
default warm-up of 6**, tracked at `config/refound/{field,sim}_gps_dual_rf0hv.json`.  **Nothing was promoted**: no golden, no ledger row,
no loader default touched, `make_golden_config.py --verify` still PASSes.

## 1. The author's decision, 2026-09-15

> Save the massive re-base for later — note it, and spend one critical run.

The golden re-base is **DEFERRED**.  `config/golden/golden_field_refound.json` and `golden_sim_refound.json` stay exactly as RF-0g left
them on 2026-09-15 (`docs/refound/RF-3-prep_golden_rebase_2026-09-15.md`): `station_prior_before_pd` OFF, the harvest era at the loader
default, the amputated solve.  The consequence travels with every number they carry and is recorded in OPEN_ITEMS §A: **every pre-fix RF
result — the goldens' own baselines included — is an amputated-solve number**, a 59-station field posture solving as a ~28-station one and
a 60-station sim posture solving as a ~30-station one.  They are not wrong as records; they are records of a different network than the
one the fix produces, and re-grading them is not the same thing as re-running them.

Development continues on the **FLOAT vehicle** (RF-1 `ambiguity_vehicle: float_kf` + RF-2b `phase_datum_held_basis` + RF-2d
release/mark), whose matched pair on the cured posture already existed when this card opened — so the re-base's cost is deferred without
blocking the program.

## 2. Step 1 — the ONE run

`runs/_rf0hv_field6_both/arm.json` = `runs/_rfpdfix_field6_both/arm.json` with exactly **two** lines different, verified by a full-file
diff before the launch: `network.product_output_path` → the new run dir, and `network.phase_leveling_warmup_epochs: 76` added.  Exe
`build/x64-Release/gnss_processor_rf0hv.exe`, a copy of `gnss_processor_rfpdfix.exe`, **sha256[:16] `14578F851F0C22EC` re-hashed after the
copy**.  Exit 0.  The arming took: `gnss_config.log` reads `warmup 76 epochs` against `warmup 6 epochs` on the source, and the reader
confirms the era moved — `carry_warm` first holds at **TOW 261480 / loop epoch 77** against **TOW 259380 / loop epoch 7**, the first carry
BIRTH moves 259380 → **261480**, and the first `pl` prior-residual fold moves 259410 → **261510**.  Both arms emit **720 / 720**.

| consumer p50 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| source `_rfpdfix_field6_both` (warm-up 6) | 5.02 | 2.11 | **0.97** | **1.05** | **0.45** | 3.65 |
| **RF-0hv `_rf0hv_field6_both` (warm-up 76)** | **7.15** | **6.06** | **3.31** | **11.10** | **15.40** | **19.83** |
| *ratio (76 / 6)* | *1.42×* | *2.87×* | *3.41×* | *10.6×* | *34.2×* | *5.43×* |

| consumer p90 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| source (warm-up 6) | 5.46 | 3.61 | 1.27 | 2.39 | 0.94 | 5.35 |
| RF-0hv (warm-up 76) | 7.81 | 7.84 | 3.69 | 19.28 | 21.45 | 21.14 |

| consumer p50 per 30 min (m) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| source (warm-up 6) | 3.76 | 5.10 | 2.31 | 1.89 | 1.01 | 0.91 | 2.02 | 0.97 | 0.44 | 0.45 | 3.56 | 4.31 |
| RF-0hv (warm-up 76) | 4.44 | 7.37 | 7.34 | 4.20 | 3.46 | 2.61 | 2.53 | **16.70** | **18.02** | **14.49** | **19.97** | **19.63** |

The 30-minute column is the one that names the shape.  The two arms track each other to within a factor of ~3 for the first 3.5 h and then
the 76-epoch arm **steps at bin 8 (TOW ≈ 271800), 2.53 → 16.70 m, and never comes back** — a single discharge, not a drift, and it lands
long after the era boundary it was armed to move.  The vertical carries it exactly as it did on the leveling vehicle: U p50 by hour
−3.74 / −1.82 / −0.67 / +0.52 / +0.02 / −0.09 → **−5.91 / −5.47 / −3.05 / −10.91 / −14.81 / −18.68 m**.

### 2.1 The independent instrument agrees

The consumer is one read; the WIRE is another, and RF-2d's grader is the vehicle's own.  `python scripts/rf2d_grade.py
runs/_rfpdfix_field6_both runs/_rf0hv_field6_both`:

| `rf2d_grade` | source (warm-up 6) | RF-0hv (warm-up 76) |
|---|---|---|
| emitted / withheld | 720 / 0 | 720 / 0 |
| IF-correction dispersion p50 / p90 / max (m) | 0.055 / **0.201** / 0.68 | 0.446 / **10.908** / **16.51** |
| worst satellites | G26 +0.68, G27 −0.63, G17 −0.31 | G13 +16.51, G19 +16.46, G24 −12.87 |
| `CLK_STEP` (unsignalled clock steps) | **0** | **9** |
| `cta_decouple` (Q3 clock-anchor decouples) | 189 | **409** |
| declarations: cont / cold | 139 / 12 | 134 / 17 |
| null-mode handovers with IF step > 1 m | 6 of 22 107 | **27 of 22 102** |

RF-PD-fix's headline vehicle win was IF dispersion p90 **1.007 → 0.201 m** with clock steps 24 → 0.  Arming the harvest-era knob on top
gives back **more than the whole of it**, 54× worse than the cured posture and 11× worse than the *pre*-fix one, and re-opens the
unsignalled clock step the fix had closed.  Two instruments on different channels, same verdict.

## 3. The decision rule and its outcome

Pre-registered before the run: *the knob joins the working posture if any settled hour (3–6) improves by more than 0.30 m, OR hours 1–2
improve by more than 1.0 m; otherwise the working posture is vehicle + `station_prior_before_pd` only, at the loader default warm-up, and
the knob is recorded as level-vehicle-specific.*

**Neither clause fires, and not marginally** — no hour improves at all; hours 3–6 degrade by 2.34 / 10.05 / 14.95 / 16.18 m and hours 1–2
by 2.13 / 3.95 m.  **The working posture is the float vehicle + `station_prior_before_pd`, at `phase_leveling_warmup_epochs` = 6.**  The
knob is **level-vehicle-specific** and is not owed a sim twin.

**Why this is a finding and not a null.**  `carry_warm` is `phase_leveling && n_emitted >= pl_warmup` (`ApplicationNetwork.cpp:1515`), and
RF-IGN §3 recorded that it gates far more than the level harvest — post-fit RAIM (`ApplicationNetwork.cpp:2602`), the within-epoch tier,
the PASS-B legacy local inject (`ApplicationNetworkAssembly.cpp:3088`), `NetworkMeasurementAdmission.cpp:436`/`:741` and
`NetworkSignalIntegrity.cpp:398`.  On the leveling vehicle the harvest term dominates that bundle and a late era is a net repair.  On the
float vehicle the harvest term is *not* the binding one — its ambiguities are floored states with covariance (birth floor (0.5 m)²) that
the filter revises, so an ignition-era harvest is a prior, not a commitment — and what is left of the bundle is 70 epochs of **QC and
admission held OFF** on a network that is now solving all 59 stations from epoch 1.  That is the same shape invariant #7 names: a
decoupling edge (the era held shut) with nothing bounding what the decoupled states accumulate, discharging once at bin 8.  RF-1's claim
was that a floored state tolerates an early birth where a zero-q level cannot; this run is the converse measurement — the same knob, the
same bed, the same fix, opposite sign by vehicle.

⚠ The knob is also in the **warm-resume fingerprint** (`NetworkRunContext.cpp:742`), so it was never a free parameter to leave armed
"just in case": carrying it would have fingerprinted every future vehicle lineage against a value this run refuses.

## 4. Step 2 — the working-posture configs

Two tracked files, neither a golden, both byte-identical in payload to the baseline-pair arms they were copied from (verified by a parsed
key-by-key comparison with `_comment` removed):

| file | copied from | posture |
|---|---|---|
| `config/refound/field_gps_dual_rf0hv.json` | `runs/_rfpdfix_field6_both/arm.json` | RF-0g field + float vehicle + `station_prior_before_pd` |
| `config/refound/sim_gps_dual_rf0hv.json` | `runs/_rfpdfix_sim6_both/arm.json` | RF-0g sim + float vehicle + `station_prior_before_pd` |

Each carries a `_comment` block stating the composition, the baseline numbers, that it is **NOT a golden**, that the golden re-base is
deferred by the author on 2026-09-15, that every new vehicle arm is a **copy** of it with only the knob under test and the run paths
changed, and that `phase_leveling_warmup_epochs: 76` is deliberately absent with this card's numbers as the reason.  They follow the
`*_rf0g.json` / `*_rf2d_both.json` convention: the `product_output_path` stays pointed at the SOURCE run dir, so a file used unedited
cannot silently overwrite a graded run.

Relative to the tracked `*_rf2d_both.json` the only payload difference is `network.station_prior_before_pd: true` and
`network.trace_station_pd_null: true` (the RF-PD trace, solve byte-identical, kept because the baseline pair carried it).

`config/golden/`, `golden_manifest.json`, `docs/CONFIG_promotion_ledger.{md,json}` and every loader default are **untouched**.
`python scripts/make_golden_config.py --verify` → `golden verify: PASS` (51 recommended knobs armed in regime, 7 documented exceptions,
0 failures, both goldens).

## 5. The baseline pair — the reference every following vehicle card is a delta against

These are the numbers a new vehicle arm must be compared to.  Both from exe `14578F851F0C22EC`, both 720 / 720 emitted, both at
`phase_leveling_warmup_epochs` = 6.

**FIELD — `runs/_rfpdfix_field6_both`** (DOY-129, 59 sta, 6 h cold, WTZZ hold-out consumer):

| | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| consumer p50 (m) | 5.02 | 2.11 | 0.97 | 1.05 | 0.45 | 3.65 |
| consumer p90 (m) | 5.46 | 3.61 | 1.27 | 2.39 | 0.94 | 5.35 |
| U p50 (m) | −3.74 | −1.82 | −0.67 | +0.52 | +0.02 | −0.09 |

per 30 min p50 (m): 3.76 / 5.10 / 2.31 / 1.89 / 1.01 / 0.91 / 2.02 / 0.97 / 0.44 / 0.45 / 3.56 / 4.31.
IF-correction dispersion p50 / p90 / max: 0.055 / **0.201** / 0.68 m.  `CLK_STEP` 0, `cta_decouple` 189, declarations 139 cont / 12 cold.

**SIM — `runs/_rfpdfix_sim6_both`** (60 sta truth bed, 6 h cold): consumer p50 by hour **1.81 / 0.79 / 0.40 / 0.26 / 0.19 / 0.46 m**;
WL at its own true integer **99.1 %**.

## 6. Runs

- `runs/_rfpdfix_field6_both`, `runs/_rfpdfix_sim6_both` — the vehicle's BASELINE PAIR on the cured posture (sources, RF-PD-fix §6).
- **`runs/_rf0hv_field6_both`** — this card's ONE new run: the baseline field arm + `phase_leveling_warmup_epochs: 76`.  Exit 0,
  720 / 720 emitted, exe `14578F851F0C22EC`.

Graders re-used unchanged: `scripts/rf2d_grade.py`, `scripts/rf_ign_level_read.py` (`--sections a --pl-warmup 76`).

## 7. What stays open

1. **THE GOLDEN RE-BASE, DEFERRED BY THE AUTHOR (2026-09-15).**  `config/golden/golden_field_refound.json` and `golden_sim_refound.json`
   are **amputated-solve postures** — they solve ~28 of 59 and ~30 of 60 stations — and so is every RF number quoted against them before
   2026-09-15.  The re-base is `station_prior_before_pd` + a harvest era, on both beds, with the ledger rows and the manifest, and it is
   the author's go, not a worker's.  Until then: a new arm derives from `config/refound/*_rf0hv.json`, and the goldens keep their role for
   anything that must reproduce a pre-fix record.
2. **The LEVEL-ONLY declared harvest era** (RF-IGN §7.3, pre-registered, nothing built).  This card sharpens the requirement rather than
   satisfying it: a fixed emitted-epoch count is refused on the float vehicle, so the era needs the invariant-#7 edges AND a per-vehicle
   scope — the level harvest gated on its own criterion, and the QC/admission half of `carry_warm` NOT held shut behind it.  Splitting
   `carry_warm` into a harvest gate and a QC gate is the shape; it is not built and this card did not build it.
3. **The vehicle's hour-6 step at TOW 276810 — the recommended NEXT event card.**  On the BASELINE arm (warm-up 6) the last hour is a
   step, not a drift: a **9-satellite Q3 clock-anchor mass decouple plus a −8.05 m G23 phase-anchor S-transform**, one epoch before the
   consumer moves (RF-IGN §7.5; 3 of 3 post-ignition excursions follow a ≥ 9-sat decouple against a 1.2 % base rate, but 7 of 9 such
   decouples are harmless, so the co-incidence is report-only until an instrument exists).  It is the largest remaining term in the
   baseline field number (3.65 m at h6 against 0.45 m at h5) and it is exactly the EVENT-HARNESS shape — one injected/located event,
   40–60 epochs.  The instrument it needs (`CLK_ANCHOR` decouple SIZE vs consumer step, with a size-matched null) does not exist yet.
   ⚠ This run adds a second reason to build it: `cta_decouple` 189 → 409 and `CLK_STEP` 0 → 9 on the refused arm say the clock anchor is
   the channel the harvest era discharges through, so the decouple instrument serves both cards.
   ⚠ **UPDATE 2026-09-15 — the instrument was built and it REFUTES the co-incidence** ([EV6](EV6_hour6_decouple_event_2026-09-15.md),
   `scripts/ev6_event_read.py`).  Decouple SIZE does not predict consumer damage (corr 0.135; count 0.212 over the run's 19 decouple
   epochs), the two LARGEST decouples (23 and 21 satellites, 1.33 and 1.26 m of de-commoned wire excursion) are harmless, and at
   TOW 279300 **none of the 8 decoupled-and-declared satellites is in the consumer's sky**.  The wire is quieter through the ramp
   (cumulative de-commoned IF-code drift 0.044 m) than through the consumer's best hour (0.726 m).  What EV6 does close is a separate wire
   -contract defect on the same mechanism — the withdraw-and-re-solve emits a one-epoch excursion, declares it, and reverses ~91 % of it
   the next epoch with **0 of 130 returns declared** — and, for the 276810 step only, names the phase-bias frame at the loss of the
   `PB:G23:L1W` column and the G23 anchor as the candidate the decouple is not.  The 279300 ramp stays unattributed.
4. **RF-2c** — the clock LEVEL, still owed on the re-founding's own list.
5. **RF-3** — QC on code rows and a wire contract that declares every step.
6. **RF-PD-fix's and RF-IGN's own open items are untouched** — the 9 residual non-finite-input `addStation` rejections, the carry rows
   still judged after the PD test, the W2 common-mode measurement, and the ~25 % of the LEVELING vehicle's loss that RF-IGN left
   unattributed.

## 8. 2026-09-19: adoption and re-based pair

The working bases now ARM three consumer-side or trace-only keys:
`network.closed_loop.pb_step_translate` (card PBT), `network.closed_loop.amb_level_check`
(card ALC) and `network.trace_consumer_sat` (card CST). The wire is unaffected, the goldens in
`config/golden/` are untouched and the golden re-base stays DEFERRED per section 7 item 1. The
restart-policy knob `amb_level_check_restart` was measured on this posture and is REFUSED; its
branch `wip/alc2-restart-policy` (`f2b0325`) stays unmerged as the record.

The BASELINE PAIR of section 5 is superseded. It is now `runs/_rf0hv2_field6` (consumer p50 by
hour 0.43 / 0.16 / 0.31 / 0.49 / 0.68 / 0.49 m, p90 3.11 / 0.28 / 0.39 / 0.61 / 0.71 / 0.69,
9 epochs under 0.10 m, IF dispersion p90 0.201 m, 720/720) and `runs/_rf0hv2_sim6`
(0.46 / 0.28 / 0.25 / 0.15 / 0.16 / 0.14 m, p90 3.19 / 0.32 / 0.28 / 0.21 / 0.23 / 0.20,
81 epochs under 0.10 m, WL(1,2) ref-free last-25 % at its own true integer 99.1 %, 720/720),
exe `gnss_processor_rf0hv2.exe` sha256 `A3D62A242C04E545...C15A9A7`, source `main` `e437b6b`.
Both reproduce the pre-ALC-ARC reference pair byte for byte, `products.txt` and every consumer
trace included; the only counter that moved is the field level-check coverage, 0.204 to 0.197.

Record: [ALC2](ALC2_restart_policy_adoption_2026-09-19.md).

## 9. 2026-09-20: the phase-bias hold adopted, the pair re-based again

The working bases carry `network.emit_pb_hold: true` with the consumer
translation and the level check OFF (card [PBH](PBH_phase_bias_hold_2026-09-19.md)
section 8). The baseline pair is `runs/_rf0hv3_{field,sim}6`, exe
`gnss_processor_pbh1.exe` (`F1D3BDDB...`): field 0.57 / 0.17 / 0.11 / 0.10 / 0.13 /
0.11 m, sim 0.60 / 0.10 / 0.10 / 0.06 / 0.06 / 0.05 m, 720/720. The pair of
section 8 (`_rf0hv2_*`) is superseded as the reference; its wire differs from the
new one only in the P records.
