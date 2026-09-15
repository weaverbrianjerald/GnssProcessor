# RF-3-prep — the vehicle on the self-contained posture, a correction to RF-2b, and the goldens re-based on RF-0g (2026-09-15)

**STATUS:** closed — **RF-1 + RF-2b on the RF-0g posture FAILS on both beds** (field 564 / 720 emitted, consumer 1.49–3.15 m, the
per-satellite IF-correction dispersion p90 0.51 → 3.03 m; sim consumer up to 12.98 m, G29's IF correction 56 m off).  **The RF-2b card's
consumer row held ratios to RF-0, not metres** — RF-2b's field consumer was 2.32 / 2.83 / 2.29 / 1.89 / 1.23 / 3.05 m, about RF-0's.
On the corrected record **RF-0g is the best posture on both beds and the only self-contained one**, and at the author's instruction
("update the goldens to use our current best configs") **the goldens are re-based on it**: `config/golden/golden_field_refound.json` and
`config/golden/golden_sim_refound.json`, generated, `--verify` PASS, certified by a 6 h run of each golden (below).  The vehicle stays built,
default OFF.  Exe `gnss_processor_goldens.exe` sha256[:16] `ab07e9ed86f7dc49` (main after the 2026-09-15 merges: RF-2b + SafeLogger), OFF
smoke `9add1d3a…` unchanged, suite 2216 passed + 1 fixture skip (293 suites).

## 1. The correction (RF-2b's consumer row)

The RF-2b worker's comparison read printed, for the field arm, `h0 p50 3.022 -> 2.321 (0.77x)` — and the card's "consumer p50 by hour (m)"
row copied the **ratio** column (0.77 / 0.95 / 1.48 / 0.88 / 1.25 / 0.93 field; 0.46 / 0.59 / 1.09 / 1.44 / 6.27 / 2.68 sim).  Re-measured
from `runs/_rf2b_*6/logs/closed_loop_trace.csv` (per-hour p50 of `pos_err_3d_m`, hours from the consumer's first epoch — the
`refound_grade.py` read, which reproduces RF-0 and RF-0g to the digit):

| arm | FIELD consumer p50 by hour (m) | SIM consumer p50 by hour (m) |
|---|---|---|
| RF-2b as carded | 0.77 / 0.95 / 1.48 / 0.88 / 1.25 / 0.93 | 0.46 / 0.59 / 1.09 / 1.44 / 6.27 / 2.68 |
| **RF-2b measured** | **2.32 / 2.83 / 2.29 / 1.89 / 1.23 / 3.05** | **1.54 / 2.53 / 3.90 / 4.69 / 6.61 / 1.54** |

The row's own words ("4 of 6 better; settled 0.93×") were ratio statements and stay true; "the most stable field consumer in the record
(0.77–1.48 m every hour)" did not, and the manager relayed it to the author as a result.  The wire, integer, marks, rate and contract rows are
unaffected (D_phase re-derived from `grade.json`).  The RF-2b and RF-0g cards carry a correction banner; their text is kept.  The RF-1
steps 3–5 card's consumer numbers were checked the same way and are genuine metres (A3 sim 1.48 / 1.98 / 0.77 / 0.23 / 0.31 / 0.30).
**Lesson for the manager:** grade a worker's consumer claim from the run directory before relaying it.

## 2. The arms (RF-3-prep as pre-registered in RF-0g: RF-1 + RF-2b on the RF-0g posture, 6 h cold, both beds)

Configs = the RF-2b arm + the RF-0g deltas (`estimation.ionosphere_weighting.enable` false, `cb_kaula_prior` false, `sat_gf_datum`
`broadcast_tgd` time-linked, `w6_iono_constraint` none, the X-channel cut; field: INEG00MEX / OAFA00ARG off the roster) —
`runs/_rf3p_{field,sim}6/arm.json`, grade `runs/_rf3p_baseline.json`.

| | FIELD RF-0g → **RF-3p** | SIM RF-0g → **RF-3p** |
|---|---|---|
| emitted / withheld | 640 / 80 → **564 / 156** (all gate) | 720 / 0 → 720 / 0 |
| consumer p50 by hour (m) | 3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68 → **1.49 / 2.96 / 2.60 / 3.15 / 2.02 / 2.65** | 6.23 / 3.30 / 0.86 / 0.50 / 0.46 / 0.55 → **6.66 / 1.69 / 12.98 / 7.15 / 1.27 / 0.94** |
| IF-correction dispersion p50 / p90 / max (m) | 0.17 / 0.51 / 1.50 → **0.66 / 3.03 / 7.46** (G03, G20, G27, G12) | 0.05 / 0.16 / 0.72 → **0.10 / 0.86 / 56.5** (G29, G01) |
| D_phase declared rms by hour (m) | 5.77 / 7.30 / 5.82 / 5.04 / 4.77 / 1.51 → 3.33 / 1.18 / 0.60 / 0.71 / 0.59 / 0.53 | 5.69 / 2.86 / 2.56 / 2.45 / 2.32 / 1.97 → 8.52 / 2.57 / 3.80 / 2.13 / 2.15 / 0.36 |
| clock LEVEL p50 by hour (m) | 3.2–3.8 → 4.0–5.1 | 1.5–2.0 → 1.9–2.2 |
| unsignalled clock steps ≥ 0.5 / ≥ 2 m | — → 33 / 0 | — → 152 / 0 |

Vehicle counters (field / sim): 850 / 295 float drops; ledger screen refused 103 / 41 candidate evaluations; 17 / 6 cold elevation-only
elections; incumbents deposed 8 / 0 by their own basis flag and 156 / 110 by arc death.  **Reading:** exactly as on the GIM baseline the
vehicle collapses the within-arc phase wire (field D_phase 0.53–1.18 m from hour 2, 5–10× below RF-0g) — and on the self-contained posture it
**disperses the per-satellite IF correction the consumer applies** (6× on the field, a 56 m excursion on the sim), which is the term RF-0e
showed the consumer tracks.  With the GIM gone nothing but the T_GD pin separates the float ambiguity from the geometry-free bias and the
slant iono; the vehicle is not the lever on this posture until that split is read.  KEEP OFF.

## 3. The record on the two instruments (6 h cold; consumer p50 by hour, m)

| posture | self-contained | FIELD consumer | avail | SIM consumer | IF disp p90 field / sim |
|---|---|---|---|---|---|
| RF-0 (the frozen posture, GIM) | no | 3.02 / 3.00 / 1.54 / 2.14 / 0.98 / 3.29 | 720 | 3.31 / 4.29 / 3.57 / 3.27 / 1.05 / 0.57 | 0.30 / — |
| RF-1 A3 (+ float_kf) | no | 5.80 / 3.22 / 1.87 / 2.95 / 4.53 / 4.47 | 720 | 1.48 / 1.98 / 0.77 / 0.23 / 0.31 / 0.30 | — |
| RF-2b (+ float_kf + held basis) | no | 2.32 / 2.83 / 2.29 / 1.89 / 1.23 / 3.05 | 720 | 1.54 / 2.53 / 3.90 / 4.69 / 6.61 / 1.54 | — |
| **RF-0g** (GF pin at T_GD, no GIM, X cut, 59 sta) | **yes** | **3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68** | 640 | **6.23 / 3.30 / 0.86 / 0.50 / 0.46 / 0.55** | **0.51 / 0.16** |
| RF-3p (RF-0g + float_kf + held basis) | yes | 1.49 / 2.96 / 2.60 / 3.15 / 2.02 / 2.65 | 564 | 6.66 / 1.69 / 12.98 / 7.15 / 1.27 / 0.94 | 3.03 / 0.86 |

No posture passes the program's bars.  RF-0g has the best settled field consumer (hours 2–5 ≤ 1.78 m) and a settled sim consumer at
0.46–0.55 m; RF-1 A3's sim (0.23–0.31 m) is better but its field regresses and it leans on the GIM.  On the author's standing
no-external-data rule and "one posture, two beds", **RF-0g is the current best** — with its known debts: the hour-1 ignition deficit (75
withholds, 16 near-null directions at epoch 1), the hours 5–6 field drift, and a wire that reads worse against COD than RF-0's (D_phase and
the clock LEVEL).  How much of that is the T_GD datum's convention offset from COD's OSB — a common term the consumer's clock absorbs, per
RF-0e's common-drift column — and how much is real is not separated here; the per-satellite IF dispersion, which the consumer does not
absorb, passes at 0.51 m.

## 4. The goldens re-based

`config/golden/golden_manifest.json` now lists two goldens and a `retired` list:

| golden | base (tracked copy of the validated arm) | pins (read back by `--verify`) |
|---|---|---|
| `golden_sim_refound` | `config/refound/sim_gps_dual_rf0g.json` = `runs/_rf0g_sim6/arm.json` | `frequency_mode` dual, `active_systems` [GPS], `carry_invariant` false, `phase_leveling`, `carry_coupling_gauge_project`, `phase_datum_prioritized`, `estimate_iono_sh` false, `ionosphere_weighting.enable` false, `clock_temporal_anchor`, `clock_disc_jump_test`, `rx_code_bias_datum`, `estimate_sat_code_bias`, `cb_kaula_prior` false, `sat_gf_datum` broadcast_tgd, `sat_gf_datum_time_linked`, `w6_iono_constraint` none |
| `golden_field_refound` | `config/refound/field_gps_dual_rf0g.json` = `runs/_rf0g_field6/arm.json` | the same |

Seven in-regime ledger recommendations are deliberately unarmed on both, each with its reason in the manifest: `carry_invariant` (the vehicle
the decision froze out), `cb_kaula_prior` (the prior must not be a datum — RF-0b), `trace_dd_fractional_ref` (continuity, as before),
`serialize_sbasis_contract` and `serialize_disc_counters` (no seams / snapshots — the scope cut), `phase_leveling_local_block` (no F3 — the scope
cut) and `phase_leveling_los_scale` (F1, superseded by F3, not carried).  `make_golden_config.py`: generate + `--verify` PASS (51 armed in regime,
7 documented exceptions each), `--selftest` PASS.

**Retired, not deleted:** `golden_sim_tri_w6.json` and `golden_field_gps_dual.json` stay byte-for-byte at their paths — 90 archived configs
`extends` them and `scripts/event_leg.py` (`DEFAULT_GOLDEN`), `ce_make_arms.py`, `cm_make_arms.py` and `make_refound_config.py` read them, so
moving or regenerating them would silently change those.  The generator neither regenerates nor verifies a retired entry.  New work starts
from the two refound goldens; a harness card that needs `event_leg.py` on the new posture re-points its default then.

**Certification** — each generated golden run 6 h cold on the same exe (`runs/golden_{field,sim}_refound`):
| golden | exit / wall | emitted / withheld | products.txt sha256[:20] | vs the RF-0g run |
|---|---|---|---|---|
| `golden_field_refound` | 0 / 212.8 s | 640 / 80 (gate) | `dbc2713549edc41f36bd` | **byte-identical** to `runs/_rf0g_field6/products.txt`; `closed_loop_trace.csv` identical |
| `golden_sim_refound` | 0 / 154.6 s | 720 / 0 | `5df551b23d946447b50b` | **byte-identical** to `runs/_rf0g_sim6/products.txt`; `closed_loop_trace.csv` identical |

So the generated goldens ARE the validated RF-0g posture (only the output paths differ), and the merged exe (RF-2b's knobs + SafeLogger, both
default-OFF / logging-only) leaves that posture's products unchanged — the RF-0g numbers in §3 are the goldens' numbers.

## 5. Owed, and next (pre-registered, not launched)

- **Ledger promotion rows** for `network.sat_gf_datum` (broadcast_tgd) and `network.sat_gf_datum_time_linked` — RECOMMEND_SCOPED on the
  refound posture, which also needs their `logNetworkPosture_` rows (`config_posture_check.py --verify-loader`); and KEEP_OFF rows for
  `network.ambiguity_vehicle` float_kf / `network.phase_datum_held_basis` on this posture citing this card.  Until then the manifest pins are
  the guard.
- **RF-3-read (a read before any build):** on `runs/_rf3p_*6`, which satellites' IF correction moves (field G03 / G20 / G27 / G12; sim G29 /
  G01), when, and against what — held-basis handovers, float drops, Q3 trips, GF-pin re-seeds — with RF-0g as the control.  Frozen reading:
  the dispersion steps at the vehicle's own events ⇒ the float ambiguity and the GF bias share a direction the T_GD pin does not close on its
  own, and the vehicle needs that direction declared before it is re-tried; it grows smoothly between events ⇒ a rank / weighting read of the
  float block without the GIM.
- **The ignition deficit** on the golden (16 near-null directions, 75 hour-1 withholds) and **RF-2c** (what moves the per-satellite clock level)
  now run on the golden posture, not on RF-0's.
