# RTF-1 Card 8 — brdc_rts armed run runbook

Turnkey validation of **Cards 5 (producer brdc_rts core) + 6 (ephemeris-cutover re-level) +
7 (consumer pinned clock eval — IN TREE since 2026-07-05)**. Prep is done; this is the
checklist for the run session. With Card 7 landed the scope is **producer + held-out
closed-loop consumer** (criterion 4 is live: `consume_network_clock` works under brdc_rts
because the consumer evaluates the broadcast baseline pinned to the stamped `clock_iode`).

The armed config was RE-ARMED 2026-07-05 with the validated all-features network set
(net305_allfeat em25 — phase leveling + slip detectors, RAIM, clock model/monitor/runaway,
SH-VTEC iono, prioritized phase datum, sum-to-zero clock datum, full product contract) per
the Jul-4 audit: never grade quality on a stripped config.

## Artifacts prepared
| File | What |
|---|---|
| `config/ppp_rtk_network_brdc_rts.json` | armed config (bundled 3-station roster + CIT100USA held-out consumer, 05:00 UTC start, 1h) |
| `scripts/stage_brdc_only_root.ps1` | builds `data_brdc_rts/` (products minus SP3/CLK; obs+static junctioned) — **already run** |
| `scripts/check_card6_relevel.py` | judges criteria 2 & 3 from `network_events.csv` + `network_solution.csv` |
| `pyviz/gnssviz/product_file.py` | patched to parse the brdc_rts C-line `iode=` token (was dropping clock rows) |

## Run steps
1. **Confirm the tree is still green** (concurrent sessions churn it):
   `git status`, Release build, `gnss_tests.exe --gtest_filter=-Gpt3Test.*` (the `Gpt3Test`
   exit-1 is a pre-existing cross-suite logger-registry artifact, not a Card 5/6 regression).
2. **(Already done) stage the brdc-only root:** `pwsh scripts/stage_brdc_only_root.ps1`.
   Re-run only if `data/` changed. `data_brdc_rts/` is gitignored.
3. **10-min smoke:** set `processing.max_duration_s: 600.0` in the config, then
   `build/x64-Release/gnss_processor.exe --config config/ppp_rtk_network_brdc_rts.json`.
   **Gate:** the log must show `product cache loaded — 0 orbits, 0 clocks` and must **not**
   show `precise products are loaded alongside apriori_source = brdc_rts`. If it refuses,
   an SP3/CLK is still on the discovery path (check the prev/next-day products dirs).
4. **1h run:** restore `max_duration_s: 3600.0`, re-run.
5. **Judge:** `python scripts/check_card6_relevel.py <logdir>/network_events.csv <logdir>/network_solution.csv`

## Pass criteria (recorded signatures, not byte-identity)
1. **Producer emits phase.** `network_trace.csv`: `solved=1`, `n_phase_bias>0` across epochs;
   σ̂0 EWMA in `network_service_health.csv` trends toward sp3-mode levels (not the `-1` sentinel).
2. **Reconstructed clock continuous at rolls (<5 cm).** The judge's criterion-2 proxy (emitted
   C0 steps by the logged shift). Full absolute-clock reconstruction is a C++ tooling item.
3. **Zero clock-disc bumps from a clean re-level.** Judge criterion 3 must PASS; a few
   `eph_cutover_disc` forced-fallbacks are acceptable (previous eph version evicted) but a large
   count means the store's current/previous retention is failing.
4. **Closed-loop consumer converges** (Card 7 in tree). CIT100USA float position stabilizes;
   degradation vs the sp3-mode baseline is graceful (dm-level expected — broadcast orbit error
   rides in the clocks per D9; record the number as the RTF-3-era benchmark).
5. *(optional)* restart mid-run (RTB-7 snapshot) resumes without spurious re-levels.

## RESULTS (2026-07-05 — smoke + 1 h RUN, 3-station roster, evidence in runs/card8_brdc_rts/)

**Blocking defect found and FIXED on the first smoke:** Card 5 bulk-inserted the whole day's
BRDC into LiveEphemerisStore, which retains only CURRENT+PREVIOUS per satellite — after the
load each satellite held its *last two records of the day*, the fit-window guard rejected
everything at 05:00, and the producer emitted `n_present = 0` all run (the Card-2 integration
test validated at 23:00, the one hour where last-two-records works). Fix = stage the parsed
records sorted by GPS-scale toc and feed them to the store **progressively in the epoch loop**
(replay parity, plan D4) — see `ApplicationNetwork.cpp` "Card 8 (replay parity)".

| Criterion | Verdict | Evidence |
|---|---|---|
| 1. Producer emits phase | **PASS** | 3 stations, 10 sat clocks, 72 phase biases/epoch; 117/121 epochs emitted; σ̂0 behavior IDENTICAL to the sp3 twin |
| 2. Reconstruction continuous at rolls < 5 cm | **PASS** (direct) | all 200 C lines carry `iode=`; E08's five rolls: reconstruction step ≤ 3.5 cm (non-roll drift baseline 0.16 cm median); judge's proxy inconclusive (no paired series) — direct C0-step-vs-logged-shift check used instead |
| 3. Zero disc bumps from clean re-levels | **PASS** | 26 re-levels (incl. the 06:00 32-sat GPS batch, Δ_eph up to 0.38 m), 0 disc bumps, 0 forced fallbacks |
| 4. Closed-loop consumer | **PASS (comparative)** | brdc_rts float median 2.84 m vs sp3-twin 2.76 m — degradation ≤ 0.1 m. The ~2.8 m vertical-dominant float error + 0% fix rate is COMMON to both modes = the OPEN closed-loop consumer bug, which now has a cheap 3-station reproducer (runs/card8_brdc_rts/{run1h,sp3_twin}) |
| 5. Restart mid-run (optional) | not exercised | — |

Notes: a σ̂0 contamination window 05:38–05:50 (climbs 3→7.5, 4 epochs withheld at the 6.0
ceiling, G02/G31 flagged, recovery after) reproduces IDENTICALLY in the sp3 twin — data/geometry
driven at 3-station scale (RAIM cannot isolate with 3 stations), NOT a brdc_rts effect.
Producer wall cost: 82 ms/epoch (3 stations, serial, all features + closed loop).
Next scale: 50-station roster per the section below.

## Why 05:00 UTC
The 2024-129 BRDC is saturated with IODE rolls (3906/day: GPS 429, GAL 2396, BDS 1081), but
GPS rolls cluster at **odd UTC hours**. The 05:00–06:00 window carries **185 rolls including a
full 32-satellite GPS batch** — a naive 00:00 window has only ~2 GPS rolls. Galileo rolls every
~10 min regardless. So Card 6 fires heavily in this window.

## Files to watch
- `network_events.csv` — `eph_cutover_relevel` (clean, Δ_eph in the `statistic` col) vs
  `eph_cutover_disc[_landed]` (forced fallback). **The only machine-readable roll-vs-fault signal.**
- spdlog log — `Network Card 6: eph cutover IODE <old> -> <new>, re-leveled ... by X m`.
- `network_solution.csv` — per-sat `value_m` (C0) + `disc_counter`.
- the emitted product (`network_products.txt`) — C lines carry `,iode=NN`.

## Scaling to 50 / 305 stations
The bundled roster is 3 stations (JPLM/P389/GCGO). For 50/305:
- stage a **separate** brdc-only root from that run's date window:
  `pwsh scripts/stage_brdc_only_root.ps1 -SrcRoot <root> -DstRoot <root>_brdc_rts -Days <YYYY-DOY...>`
- copy `ppp_rtk_network_brdc_rts.json`, point `files.internal_data_root` at that root, and
  swap `network.stations[]` for the larger roster (keep `apriori_source`/`service_mode`/`trace_events`).
- **Expect Galileo conditioning stress at 305** under brdc_rts (metre-scale Δδtˢ, 3 m anchor σ) —
  that discovery + a possible q/anchor retune is part of the run, per the conventions.
