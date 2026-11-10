# Clock two-state q1/q2 — intrinsic-vs-network Hadamard decomposition (2026-07-14)

Phase A.5 of the `network.clock_model` revival card, whose surviving write-up is `docs/SPEC_c1_clock_rate_validation.md` §10 (the handoff it originally cited was absorbed there 2026-08-15 and now lives at `docs/attic/HANDOFF_clock_model_revival.md`). Produced by `scripts/clock_hadamard_calib.py` (analysis-only, no C++). Statistic: overlapping Hadamard (3rd-difference, drift-insensitive) primary. Per-constellation common mode removed from every series.

> ⚠ **THIS FILE IS GENERATED.** `scripts/clock_hadamard_calib.py:966` re-creates `docs/CLOCK_HADAMARD_CALIB_<today>.md` on every run, and `:675` writes the "Phase A.5 of …" line above from a template. **Never hand-write card prose here — the next regeneration destroys it**, and the repointed reference above will revert until the SCRIPT template is fixed. ⚠ The `[network]` rows are no longer locally regenerable: their inputs (`build/x64-Release/run_{gpsonly_em25_12h,n305_attrib_6h,n305_attrib_3h}`) were archived off C: on 2026-08-12 to `D:\GnssProcessor_archive\2026-08-12\`.

- τ grid (s): 30, 60, 120, 300, 600, 1800; value-step guard max(8.0·MAD, 0.2 m); min triples/τ 12
- model: **Var(Δ³x) = 20·σ_ε² + 6·q1·τ + q2·τ³** — a pure two-state clock (the premise) is RWFM ⇒ VarH ∝ τ³ ⇒ log-log slope **p = 3**. **p ≈ 1** ⇒ white-FM / measurement dominated ⇒ a drift state buys nothing.
- **intrinsic** source = the CODE MGEX 30 s precise clock product (`AS` records, seconds→m). Caveat: its 30 s values are phase-interpolated from a 300 s estimate, so it is smooth by construction at small τ — read the intrinsic small-τ (WPM) floor as a product artefact, the large-τ slope as the physical clock.
- **network** source = the emitted `clock_c0_m` (= Δδtˢ-vs-precise, `NetworkEstimator.cpp:644`).

## ★ Decisive comparison — intrinsic clock vs network estimate

Two questions: (1) is there RWFM to exploit (slope p→3)? (2) is there a predictable clock RATE to exploit (carry advantage RMS(Δx)/RMS(Δ²x))?

| Constellation | intrinsic p | intrinsic q2_ub | intrinsic carry adv | network p | network q2_ub | network carry adv |
|---|---|---|---|---|---|---|
| BeiDou | 0.76 | 2.4e-13 | 17.6× | nan | nan | nan |
| Galileo | 0.88 | 1.7e-13 | 16.6× | 1.05 | 2.7e-08 | 0.9× |
| GPS | 1.50 | 5.8e-12 | 1.7× | 1.02 | 8.7e-09 | 1.1× |
| QZSS | 0.75 | 6.8e-13 | 0.7× | nan | nan | nan |
| GLONASS | 0.94 | 5.3e-11 | 0.7× | nan | nan | nan |

**Reading — two findings, one revival:**

1. **RWFM is absent everywhere.** The intrinsic precise clock is white-FM (slope p≈1.5, GPS), the same as the network estimate (p≈1.0) — NOT the p=3 the two-state premise needs. Intrinsic q2_ub GPS ≤ 6e-12 is 4–6 orders BELOW the documented 1e-07. This is expected clock physics: WFM/flicker dominates at 30 s–30 min; random-walk-FM only emerges at hour+ averaging, far beyond the 30 s prediction horizon. **The "q2·dt³/3 = 3.7 cm" RWFM tightness mechanism does not exist.**

2. **The real clock asset is the DETERMINISTIC RATE, and it is large — but the network carry cannot see it.** Measured directly, a two-state drift state on the *intrinsic* clock beats a scalar carry by 2× (GPS) to 17× (Galileo PHM) — the satellite's predictable frequency. But on the *network residual* (what `clock_model` actually carries) the advantage collapses to 1.1×: the deterministic rate has already cancelled against the precise apriori, and the residual is dominated by ~0.2 m/epoch estimation + datum noise. (Intrinsic small-τ carries a phase-interpolation caveat — treat the multiplier as order-of-magnitude, the ranking Galileo≫GPS≫GLONASS as real.)

**Verdict.** `clock_model` (two-state carry on the emitted residual) is a **NO-GO**: no RWFM to exploit, and only ~1.1× rate advantage on the residual — moot on a dense network where the clock is strongly observed each epoch, and not the 15× claimed. **But the user's mandate — exploit clock stability — is served by a different mechanism: the C1 clock-RATE product** (`project_network_clock_rate_c1`; producer currently hardcodes c1=0). The predictable drift is real and largest for Galileo; emitting it lets low-latency / real-time consumers extrapolate instead of ZOH (the saw-tooth fix), with the benefit growing as the consumer's apriori clock weakens (broadcast / RT mode). That is where satellite-clock stability strengthens the network — not in the 30 s within-network carry.

### Intrinsic (CODE 30 s product) — noise type

| Constellation | slope p | var@τmax WPM/WFM/RWFM | verdict |
|---|---|---|---|
| BeiDou | 0.76 | 1% / 97% / 0% | WFM / measurement (drift buys ~0) |
| Galileo | 0.88 | 2% / 89% / 9% | WFM / measurement (drift buys ~0) |
| GPS | 1.50 | 7% / 93% / 0% | WFM / measurement (drift buys ~0) |
| QZSS | 0.75 | 1% / 99% / 0% | WFM / measurement (drift buys ~0) |
| GLONASS | 0.94 | 0% / 99% / 0% | WFM / measurement (drift buys ~0) |

### Intrinsic (CODE 30 s product) — operative process-noise levels

| Constellation | q1 WFM (m²/s) | σ_ε WPM (m) | q2 RWFM upper bound (m²/s³) | n |
|---|---|---|---|---|
| BeiDou | 1.33e-07 | 0.001 | ≤ 2.37e-13 | 38 |
| Galileo | 7.60e-08 | 0.001 | ≤ 1.66e-13 | 25 |
| GPS | 4.60e-06 | 0.013 | ≤ 5.82e-12 | 32 |
| QZSS | 4.30e-07 | 0.002 | ≤ 6.80e-13 | 3 |
| GLONASS | 2.77e-05 | 0.007 | ≤ 5.30e-11 | 23 |

### Intrinsic (CODE 30 s product) — two-state carry advantage (measured, RMS(Δx)/RMS(Δ²x))

| Constellation | scalar σ RMS(Δx) m | two-state σ RMS(Δ²x) m | advantage | reading |
|---|---|---|---|---|
| BeiDou | 0.070 | 0.004 | 17.6× | large drift asset |
| Galileo | 0.047 | 0.003 | 16.6× | large drift asset |
| GPS | 0.054 | 0.025 | 1.7× | modest drift |
| QZSS | 0.004 | 0.006 | 0.7× | noise-floor-limited (no gain) |
| GLONASS | 0.032 | 0.043 | 0.7× | noise-floor-limited (no gain) |

### Network (emitted clock_c0_m) — noise type

| Constellation | slope p | var@τmax WPM/WFM/RWFM | verdict |
|---|---|---|---|
| Galileo | 1.05 | 0% / 92% / 7% | WFM / measurement (drift buys ~0) |
| GPS | 1.02 | 0% / 81% / 19% | WFM / measurement (drift buys ~0) |

### Network (emitted clock_c0_m) — operative process-noise levels

| Constellation | q1 WFM (m²/s) | σ_ε WPM (m) | q2 RWFM upper bound (m²/s³) | n |
|---|---|---|---|---|
| Galileo | 6.14e-03 | 0.085 | ≤ 2.67e-08 | 42 |
| GPS | 3.48e-03 | 0.111 | ≤ 8.74e-09 | 93 |

### Network (emitted clock_c0_m) — two-state carry advantage (measured, RMS(Δx)/RMS(Δ²x))

| Constellation | scalar σ RMS(Δx) m | two-state σ RMS(Δ²x) m | advantage | reading |
|---|---|---|---|---|
| Galileo | 0.636 | 0.658 | 0.9× | noise-floor-limited (no gain) |
| GPS | 0.606 | 0.528 | 1.1× | noise-floor-limited (no gain) |

## Gate A (network emitted clock)

- **Documented ≈1e-7 RWFM class reproduced?** **NO** — median slope p≈1.0 (not 3); q2 upper bounds Galileo≤2.7e-08, GPS≤8.5e-09, ~17× below documented 1.5e-07 (GPS).
- **>10× block-median q2-ub outliers (fault-history candidates, excluded):** 5
    - E21 (Galileo-PHM): q2_ub=2.1e-09, p=0.9  [run_n305_attrib_3h]
    - E36 (Galileo-PHM): q2_ub=3.3e-07, p=1.1  [run_n305_attrib_6h]
    - G03 (IIF-Rb): q2_ub=1.0e-07, p=1.0  [run_n305_attrib_6h]
    - G25 (IIF-Rb): q2_ub=1.9e-07, p=1.4  [run_n305_attrib_6h]
    - G25 (IIF-Rb): q2_ub=2.6e-07, p=1.4  [run_n305_attrib_3h]

## Per-satellite fits

| Sat | Block | kind | slope p | q2_ub (m²/s³) | q1 (m²/s) | σ_ε (m) | τ pts | longest seg | flag | source |
|---|---|---|---|---|---|---|---|---|---|---|
| C06 | BeiDou-? | intrinsic | 0.92 | 3.833e-12 | 1.247e-06 | 0.000 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C07 | BeiDou-? | intrinsic | 0.92 | 3.547e-12 | 1.910e-06 | 0.002 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C08 | BeiDou-? | intrinsic | 1.02 | 1.082e-11 | 5.871e-06 | 0.002 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C09 | BeiDou-? | intrinsic | 0.87 | 2.985e-12 | 1.105e-06 | 0.002 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C10 | BeiDou-? | intrinsic | 0.95 | 9.933e-12 | 3.294e-06 | 0.002 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C11 | BeiDou-? | intrinsic | 0.98 | 1.925e-11 | 1.666e-06 | 0.000 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C12 | BeiDou-? | intrinsic | 0.89 | 2.254e-12 | 1.093e-06 | 0.002 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C13 | BeiDou-? | intrinsic | 1.07 | 9.404e-12 | 4.846e-06 | 0.000 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C14 | BeiDou-? | intrinsic | 0.79 | 8.238e-13 | 3.517e-07 | 0.002 | 6 | 1480 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C16 | BeiDou-? | intrinsic | 0.64 | 2.222e-13 | 1.364e-07 | 0.003 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C19 | BeiDou-? | intrinsic | 0.76 | 1.605e-13 | 1.017e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C20 | BeiDou-? | intrinsic | 0.78 | 2.344e-13 | 1.385e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C21 | BeiDou-? | intrinsic | 0.72 | 2.208e-13 | 1.234e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C22 | BeiDou-? | intrinsic | 0.70 | 1.109e-13 | 5.018e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C23 | BeiDou-? | intrinsic | 0.86 | 5.818e-13 | 2.215e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C24 | BeiDou-? | intrinsic | 0.56 | 1.693e-13 | 9.028e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C25 | BeiDou-? | intrinsic | 0.80 | 4.907e-13 | 2.724e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C26 | BeiDou-? | intrinsic | 0.69 | 2.597e-13 | 1.369e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C27 | BeiDou-? | intrinsic | 0.69 | 1.658e-13 | 9.542e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C28 | BeiDou-? | intrinsic | 0.88 | 2.348e-13 | 1.434e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C29 | BeiDou-? | intrinsic | 0.82 | 3.144e-13 | 1.854e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C30 | BeiDou-? | intrinsic | 0.82 | 2.763e-13 | 1.607e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C32 | BeiDou-? | intrinsic | 0.74 | 2.159e-13 | 1.158e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C33 | BeiDou-? | intrinsic | 0.69 | 1.625e-13 | 9.869e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C34 | BeiDou-? | intrinsic | 0.69 | 1.815e-13 | 9.954e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C35 | BeiDou-? | intrinsic | 0.81 | 3.981e-13 | 2.073e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C36 | BeiDou-? | intrinsic | 0.68 | 1.344e-13 | 8.750e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C37 | BeiDou-? | intrinsic | 0.76 | 2.087e-13 | 1.161e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C38 | BeiDou-? | intrinsic | 0.69 | 2.548e-13 | 1.074e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C39 | BeiDou-? | intrinsic | 0.74 | 2.388e-13 | 1.056e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C40 | BeiDou-? | intrinsic | 0.70 | 1.109e-13 | 5.018e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C41 | BeiDou-? | intrinsic | 0.78 | 2.239e-13 | 1.297e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C42 | BeiDou-? | intrinsic | 0.65 | 2.081e-13 | 1.143e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C43 | BeiDou-? | intrinsic | 0.73 | 2.541e-13 | 1.070e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C44 | BeiDou-? | intrinsic | 0.71 | 2.159e-13 | 1.113e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C45 | BeiDou-? | intrinsic | 0.70 | 1.652e-13 | 9.678e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C46 | BeiDou-? | intrinsic | 0.70 | 1.455e-13 | 8.016e-08 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| C48 | BeiDou-? | intrinsic | 0.89 | 5.017e-13 | 2.493e-07 | 0.001 | 6 | 1479 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E02 | Galileo-PHM | intrinsic | 0.89 | 2.296e-13 | 6.691e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E03 | Galileo-PHM | intrinsic | 0.93 | 1.276e-13 | 5.333e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E04 | Galileo-PHM | intrinsic | 0.83 | 1.574e-13 | 6.900e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E05 | Galileo-PHM | intrinsic | 0.95 | 1.137e-13 | 6.630e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E07 | Galileo-PHM | intrinsic | 0.84 | 1.244e-13 | 6.727e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E08 | Galileo-PHM | intrinsic | 0.88 | 2.514e-13 | 8.298e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E09 | Galileo-PHM | intrinsic | 0.94 | 1.551e-13 | 5.924e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E10 | Galileo-PHM | intrinsic | 0.87 | 1.032e-13 | 5.492e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E11 | Galileo-PHM | intrinsic | 1.01 | 1.876e-12 | 5.531e-07 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E12 | Galileo-PHM | intrinsic | 0.95 | 1.849e-12 | 1.033e-06 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E13 | Galileo-PHM | intrinsic | 0.82 | 1.720e-13 | 8.329e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E14 | Galileo-PHM | intrinsic | 0.99 | 1.833e-12 | 2.175e-06 | 0.000 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E15 | Galileo-PHM | intrinsic | 0.87 | 1.043e-13 | 5.579e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E18 | Galileo-PHM | intrinsic | 0.81 | 1.410e-13 | 5.422e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E19 | Galileo-PHM | intrinsic | 1.03 | 1.678e-11 | 7.920e-06 | 0.000 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E21 | Galileo-PHM | intrinsic | 0.90 | 2.181e-13 | 8.425e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E24 | Galileo-PHM | intrinsic | 0.45 | 8.199e-14 | 4.287e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E25 | Galileo-PHM | intrinsic | 0.85 | 1.293e-13 | 6.904e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E26 | Galileo-PHM | intrinsic | 0.85 | 2.087e-13 | 9.987e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E27 | Galileo-PHM | intrinsic | 0.89 | 2.048e-13 | 9.206e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E30 | Galileo-PHM | intrinsic | 0.91 | 4.116e-13 | 1.630e-07 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E31 | Galileo-PHM | intrinsic | 0.91 | 1.594e-13 | 8.750e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E33 | Galileo-PHM | intrinsic | 0.85 | 3.639e-13 | 1.831e-07 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E34 | Galileo-PHM | intrinsic | nan | nan | nan | nan | 0 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E36 | Galileo-PHM | intrinsic | 0.88 | 9.454e-14 | 5.237e-08 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G01 | IIF-Rb | intrinsic | 1.75 | 3.955e-11 | 3.541e-05 | 0.007 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G02 | IIR-Rb | intrinsic | 1.47 | 9.897e-12 | 7.697e-06 | 0.019 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G03 | IIF-Rb | intrinsic | 1.51 | 5.792e-12 | 4.582e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G04 | III-Rb | intrinsic | 1.51 | 5.650e-12 | 4.523e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G05 | IIR-Rb | intrinsic | 1.24 | 1.924e-11 | 1.440e-05 | 0.034 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G06 | IIF-Rb | intrinsic | 1.50 | 5.723e-12 | 4.583e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G07 | IIR-Rb | intrinsic | 1.38 | 8.620e-12 | 7.232e-06 | 0.020 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G08 | IIF-Rb | intrinsic | 1.51 | 5.443e-12 | 4.436e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G09 | IIF-Rb | intrinsic | 1.51 | 5.496e-12 | 4.456e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G10 | IIF-Rb | intrinsic | 1.52 | 5.778e-12 | 4.615e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G11 | III-Rb | intrinsic | 1.51 | 5.725e-12 | 4.530e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G12 | IIR-Rb | intrinsic | 1.23 | 8.973e-12 | 6.800e-06 | 0.023 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G13 | IIR-Rb | intrinsic | 1.38 | 8.733e-12 | 7.190e-06 | 0.019 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G14 | III-Rb | intrinsic | 1.49 | 5.840e-12 | 4.592e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G15 | IIR-Rb | intrinsic | 1.44 | 9.135e-12 | 7.967e-06 | 0.019 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G16 | IIR-Rb | intrinsic | 1.30 | 9.441e-12 | 7.727e-06 | 0.025 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G17 | IIR-Rb | intrinsic | 1.36 | 1.289e-11 | 1.096e-05 | 0.022 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G18 | III-Rb | intrinsic | 1.51 | 5.811e-12 | 4.599e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G19 | IIR-Rb | intrinsic | 1.43 | 9.172e-12 | 7.951e-06 | 0.020 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G20 | IIR-Rb | intrinsic | 1.45 | 1.418e-11 | 1.233e-05 | 0.019 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G21 | IIR-Rb | intrinsic | 1.41 | 4.704e-12 | 3.821e-06 | 0.012 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G22 | IIR-Rb | intrinsic | 1.41 | 4.704e-12 | 3.821e-06 | 0.012 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G23 | III-Rb | intrinsic | 1.51 | 5.431e-12 | 4.456e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G24 | IIF-Rb | intrinsic | 1.49 | 5.303e-12 | 4.355e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G25 | IIF-Rb | intrinsic | 1.51 | 5.708e-12 | 4.559e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G26 | IIF-Rb | intrinsic | 1.51 | 5.928e-12 | 4.631e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G27 | IIF-Rb | intrinsic | 1.51 | 5.325e-12 | 4.409e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G28 | IIR-Rb | intrinsic | 1.51 | 5.821e-12 | 4.574e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G29 | IIR-Rb | intrinsic | 1.37 | 8.676e-12 | 7.327e-06 | 0.020 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G30 | IIF-Rb | intrinsic | 1.50 | 5.589e-12 | 4.489e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G31 | IIR-Rb | intrinsic | 1.37 | 9.247e-12 | 7.132e-06 | 0.018 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| G32 | IIF-Rb | intrinsic | 1.50 | 5.495e-12 | 4.511e-06 | 0.013 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| J02 | QZSS-? | intrinsic | 0.79 | 6.534e-13 | 4.341e-07 | 0.002 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| J03 | QZSS-? | intrinsic | nan | nan | nan | nan | 0 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| J04 | QZSS-? | intrinsic | 0.71 | 7.064e-13 | 4.267e-07 | 0.002 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R01 | GLONASS-? | intrinsic | 0.96 | 5.307e-11 | 2.940e-05 | 0.001 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R02 | GLONASS-? | intrinsic | 0.90 | 5.731e-11 | 2.713e-05 | 0.008 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R03 | GLONASS-? | intrinsic | 0.94 | 8.830e-11 | 4.408e-05 | 0.000 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R04 | GLONASS-? | intrinsic | 0.95 | 3.698e-11 | 2.122e-05 | 0.010 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R05 | GLONASS-? | intrinsic | 0.90 | 5.285e-11 | 3.148e-05 | 0.012 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R07 | GLONASS-? | intrinsic | 0.94 | 8.291e-11 | 3.204e-05 | 0.009 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R08 | GLONASS-? | intrinsic | 0.99 | 7.460e-11 | 4.313e-05 | 0.010 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R09 | GLONASS-? | intrinsic | 0.99 | 5.345e-11 | 3.005e-05 | 0.011 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R11 | GLONASS-? | intrinsic | 0.94 | 6.054e-11 | 3.116e-05 | 0.003 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R12 | GLONASS-? | intrinsic | 0.91 | 3.578e-11 | 1.999e-05 | 0.007 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R13 | GLONASS-? | intrinsic | 0.92 | 5.109e-11 | 2.762e-05 | 0.007 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R14 | GLONASS-? | intrinsic | nan | nan | nan | nan | 0 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R15 | GLONASS-? | intrinsic | 0.92 | 5.837e-11 | 2.474e-05 | 0.007 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R16 | GLONASS-? | intrinsic | 0.94 | 5.281e-11 | 2.784e-05 | 0.005 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R17 | GLONASS-? | intrinsic | 0.91 | 4.247e-11 | 2.303e-05 | 0.007 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R18 | GLONASS-? | intrinsic | 0.93 | 3.957e-11 | 1.907e-05 | 0.004 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R19 | GLONASS-? | intrinsic | 0.98 | 8.486e-11 | 4.610e-05 | 0.008 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R20 | GLONASS-? | intrinsic | 1.09 | 1.916e-10 | 1.078e-04 | 0.005 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R21 | GLONASS-? | intrinsic | 0.95 | 3.789e-11 | 2.126e-05 | 0.007 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R22 | GLONASS-? | intrinsic | 0.92 | 3.261e-11 | 1.713e-05 | 0.004 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R24 | GLONASS-? | intrinsic | 0.95 | 4.205e-11 | 2.279e-05 | 0.008 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R25 | GLONASS-? | intrinsic | 1.01 | 7.073e-11 | 3.802e-05 | 0.007 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| R26 | GLONASS-? | intrinsic | 0.97 | 4.306e-11 | 2.165e-05 | 0.004 | 6 | 2880 |  | COD0MGXFIN_20241290000_01D_30S_CLK.CLK |
| E03 | Galileo-PHM | network | 0.96 | 1.635e-07 | 4.258e-03 | 0.088 | 5 | 97 |  | run_n305_attrib_3h |
| E03 | Galileo-PHM | network | 1.24 | 1.540e-07 | 6.434e-03 | 0.000 | 5 | 97 |  | run_n305_attrib_6h |
| E04 | Galileo-PHM | network | 1.15 | 9.593e-09 | 4.861e-03 | 0.198 | 6 | 315 |  | run_n305_attrib_3h |
| E04 | Galileo-PHM | network | 1.14 | 7.583e-09 | 5.058e-03 | 0.235 | 6 | 366 |  | run_n305_attrib_6h |
| E07 | Galileo-PHM | network | 0.99 | 2.752e-08 | 3.486e-03 | 0.187 | 6 | 205 |  | run_n305_attrib_3h |
| E07 | Galileo-PHM | network | 0.99 | 2.049e-08 | 5.877e-03 | 0.233 | 6 | 219 |  | run_n305_attrib_6h |
| E08 | Galileo-PHM | network | 1.01 | 1.336e-08 | 7.417e-03 | 0.000 | 6 | 261 |  | run_n305_attrib_3h |
| E08 | Galileo-PHM | network | 1.09 | 1.303e-08 | 7.101e-03 | 0.000 | 6 | 264 |  | run_n305_attrib_6h |
| E09 | Galileo-PHM | network | 1.21 | 9.707e-08 | 4.398e-03 | 0.000 | 5 | 134 |  | run_n305_attrib_3h |
| E09 | Galileo-PHM | network | 1.03 | 1.366e-08 | 7.496e-03 | 0.120 | 6 | 366 |  | run_n305_attrib_6h |
| E10 | Galileo-PHM | network | 1.14 | 1.111e-07 | 6.876e-03 | 0.099 | 5 | 160 |  | run_n305_attrib_3h |
| E10 | Galileo-PHM | network | 1.10 | 2.060e-08 | 6.295e-03 | 0.158 | 6 | 287 |  | run_n305_attrib_6h |
| E13 | Galileo-PHM | network | 0.82 | 1.007e-07 | 5.316e-03 | 0.000 | 5 | 169 |  | run_n305_attrib_3h |
| E13 | Galileo-PHM | network | 0.98 | 1.187e-08 | 6.917e-03 | 0.000 | 6 | 265 |  | run_n305_attrib_6h |
| E14 | Galileo-PHM | network | 0.81 | 1.540e-08 | 8.536e-03 | 0.070 | 6 | 262 |  | run_n305_attrib_3h |
| E14 | Galileo-PHM | network | 1.34 | 2.052e-08 | 1.276e-02 | 0.389 | 6 | 416 |  | run_n305_attrib_6h |
| E15 | Galileo-PHM | network | 0.85 | 2.074e-07 | 3.093e-03 | 0.133 | 5 | 98 |  | run_n305_attrib_3h |
| E15 | Galileo-PHM | network | 1.38 | 1.386e-07 | 9.161e-03 | 0.000 | 5 | 110 |  | run_n305_attrib_6h |
| E18 | Galileo-PHM | network | 1.02 | 2.595e-08 | 9.103e-03 | 0.019 | 6 | 360 |  | run_n305_attrib_3h |
| E18 | Galileo-PHM | network | 1.14 | 2.298e-08 | 1.110e-02 | 0.000 | 6 | 400 |  | run_n305_attrib_6h |
| E19 | Galileo-PHM | network | 1.08 | 9.169e-08 | 6.562e-03 | 0.000 | 5 | 175 |  | run_n305_attrib_3h |
| E19 | Galileo-PHM | network | 1.21 | 1.325e-07 | 9.119e-03 | 0.078 | 5 | 175 |  | run_n305_attrib_6h |
| E21 | Galileo-PHM | network | 0.88 | 2.090e-09 | 2.130e-03 | 0.284 | 6 | 220 | ★ | run_n305_attrib_3h |
| E21 | Galileo-PHM | network | 1.26 | 3.348e-09 | 6.100e-03 | 0.279 | 6 | 273 |  | run_n305_attrib_6h |
| E24 | Galileo-PHM | network | 1.03 | 9.647e-08 | 3.773e-03 | 0.000 | 5 | 134 |  | run_n305_attrib_3h |
| E24 | Galileo-PHM | network | 1.25 | 7.587e-08 | 5.668e-03 | 0.204 | 5 | 140 |  | run_n305_attrib_6h |
| E25 | Galileo-PHM | network | 0.98 | 2.047e-07 | 5.014e-03 | 0.000 | 5 | 97 |  | run_n305_attrib_3h |
| E25 | Galileo-PHM | network | 1.08 | 1.621e-07 | 6.805e-03 | 0.115 | 5 | 126 |  | run_n305_attrib_6h |
| E26 | Galileo-PHM | network | 0.87 | 1.461e-07 | 9.383e-03 | 0.000 | 5 | 103 |  | run_n305_attrib_3h |
| E26 | Galileo-PHM | network | 0.95 | 5.055e-09 | 5.389e-03 | 0.209 | 6 | 198 |  | run_n305_attrib_6h |
| E27 | Galileo-PHM | network | 0.97 | 2.182e-08 | 6.180e-03 | 0.058 | 6 | 219 |  | run_n305_attrib_3h |
| E27 | Galileo-PHM | network | 0.96 | 2.182e-08 | 7.444e-03 | 0.081 | 6 | 219 |  | run_n305_attrib_6h |
| E30 | Galileo-PHM | network | 0.89 | 1.956e-08 | 4.779e-03 | 0.082 | 6 | 219 |  | run_n305_attrib_3h |
| E30 | Galileo-PHM | network | 1.15 | 1.220e-08 | 8.049e-03 | 0.108 | 6 | 301 |  | run_n305_attrib_6h |
| E31 | Galileo-PHM | network | 1.20 | 9.306e-08 | 3.913e-03 | 0.000 | 5 | 134 |  | run_n305_attrib_3h |
| E31 | Galileo-PHM | network | 1.05 | 9.888e-09 | 6.108e-03 | 0.208 | 6 | 397 |  | run_n305_attrib_6h |
| E33 | Galileo-PHM | network | 1.01 | 6.833e-08 | 5.779e-03 | 0.000 | 5 | 174 |  | run_n305_attrib_3h |
| E33 | Galileo-PHM | network | 1.10 | 9.375e-09 | 6.492e-03 | 0.163 | 6 | 224 |  | run_n305_attrib_6h |
| E34 | Galileo-PHM | network | 1.05 | 1.187e-07 | 3.371e-03 | 0.113 | 5 | 97 |  | run_n305_attrib_3h |
| E34 | Galileo-PHM | network | 1.16 | 1.251e-07 | 6.066e-03 | 0.062 | 5 | 171 |  | run_n305_attrib_6h |
| E36 | Galileo-PHM | network | 0.94 | 2.119e-07 | 3.661e-03 | 0.147 | 5 | 105 |  | run_n305_attrib_3h |
| E36 | Galileo-PHM | network | 1.11 | 3.259e-07 | 8.600e-03 | 0.126 | 5 | 146 | ★ | run_n305_attrib_6h |
| G02 | IIR-Rb | network | 1.16 | 1.399e-08 | 5.508e-03 | 0.000 | 6 | 497 |  | run_gpsonly_em25_12h |
| G02 | IIR-Rb | network | 0.90 | 4.367e-09 | 2.510e-03 | 0.100 | 6 | 360 |  | run_n305_attrib_3h |
| G02 | IIR-Rb | network | 1.11 | 9.897e-09 | 2.971e-03 | 0.000 | 6 | 497 |  | run_n305_attrib_6h |
| G03 | IIF-Rb | network | 0.92 | 1.300e-08 | 3.630e-03 | 0.059 | 6 | 338 |  | run_gpsonly_em25_12h |
| G03 | IIF-Rb | network | 0.89 | 7.503e-08 | 2.112e-03 | 0.127 | 5 | 172 |  | run_n305_attrib_3h |
| G03 | IIF-Rb | network | 1.02 | 1.036e-07 | 2.197e-03 | 0.134 | 5 | 172 | ★ | run_n305_attrib_6h |
| G04 | III-Rb | network | 0.99 | 9.486e-09 | 4.873e-03 | 0.000 | 6 | 486 |  | run_gpsonly_em25_12h |
| G04 | III-Rb | network | 0.86 | 2.890e-08 | 4.362e-03 | 0.000 | 6 | 341 |  | run_n305_attrib_3h |
| G04 | III-Rb | network | 1.21 | 2.890e-08 | 6.664e-03 | 0.161 | 6 | 341 |  | run_n305_attrib_6h |
| G05 | IIR-Rb | network | 1.07 | 6.842e-09 | 3.804e-03 | 0.000 | 6 | 474 |  | run_gpsonly_em25_12h |
| G05 | IIR-Rb | network | 1.12 | 3.421e-09 | 2.282e-03 | 0.083 | 6 | 241 |  | run_n305_attrib_3h |
| G05 | IIR-Rb | network | 1.25 | 4.473e-09 | 4.891e-03 | 0.111 | 6 | 341 |  | run_n305_attrib_6h |
| G06 | IIF-Rb | network | 1.07 | 6.907e-09 | 4.474e-03 | 0.117 | 6 | 497 |  | run_gpsonly_em25_12h |
| G06 | IIF-Rb | network | 0.93 | 3.409e-09 | 2.967e-03 | 0.195 | 6 | 360 |  | run_n305_attrib_3h |
| G06 | IIF-Rb | network | 0.95 | 4.307e-09 | 3.749e-03 | 0.105 | 6 | 497 |  | run_n305_attrib_6h |
| G07 | IIR-Rb | network | 1.08 | 8.929e-09 | 4.860e-03 | 0.115 | 6 | 482 |  | run_gpsonly_em25_12h |
| G07 | IIR-Rb | network | 0.91 | 2.838e-09 | 1.570e-03 | 0.096 | 6 | 326 |  | run_n305_attrib_3h |
| G07 | IIR-Rb | network | 0.92 | 4.553e-09 | 3.110e-03 | 0.166 | 6 | 482 |  | run_n305_attrib_6h |
| G08 | IIF-Rb | network | 1.09 | 1.820e-08 | 3.754e-03 | 0.124 | 6 | 497 |  | run_gpsonly_em25_12h |
| G08 | IIF-Rb | network | 0.98 | 1.734e-08 | 2.014e-03 | 0.114 | 6 | 341 |  | run_n305_attrib_3h |
| G08 | IIF-Rb | network | 1.12 | 2.004e-08 | 3.288e-03 | 0.182 | 6 | 497 |  | run_n305_attrib_6h |
| G09 | IIF-Rb | network | 0.99 | 8.132e-09 | 4.205e-03 | 0.073 | 6 | 490 |  | run_gpsonly_em25_12h |
| G09 | IIF-Rb | network | 1.02 | 6.990e-09 | 2.233e-03 | 0.111 | 6 | 341 |  | run_n305_attrib_3h |
| G09 | IIF-Rb | network | 1.01 | 6.990e-09 | 3.035e-03 | 0.154 | 6 | 341 |  | run_n305_attrib_6h |
| G10 | IIF-Rb | network | 0.94 | 9.101e-09 | 3.388e-03 | 0.079 | 6 | 497 |  | run_gpsonly_em25_12h |
| G10 | IIF-Rb | network | 1.01 | 1.222e-08 | 3.899e-03 | 0.000 | 6 | 360 |  | run_n305_attrib_3h |
| G10 | IIF-Rb | network | 1.03 | 1.232e-08 | 4.588e-03 | 0.000 | 6 | 497 |  | run_n305_attrib_6h |
| G11 | III-Rb | network | 0.98 | 1.009e-08 | 5.706e-03 | 0.000 | 6 | 497 |  | run_gpsonly_em25_12h |
| G11 | III-Rb | network | 0.99 | 1.189e-08 | 2.787e-03 | 0.166 | 6 | 273 |  | run_n305_attrib_3h |
| G11 | III-Rb | network | 1.01 | 6.937e-09 | 4.166e-03 | 0.149 | 6 | 497 |  | run_n305_attrib_6h |
| G12 | IIR-Rb | network | 1.02 | 7.002e-09 | 3.259e-03 | 0.095 | 6 | 469 |  | run_gpsonly_em25_12h |
| G12 | IIR-Rb | network | 0.98 | 5.365e-08 | 4.250e-03 | 0.132 | 5 | 173 |  | run_n305_attrib_3h |
| G12 | IIR-Rb | network | 1.09 | 1.111e-08 | 3.529e-03 | 0.111 | 6 | 213 |  | run_n305_attrib_6h |
| G13 | IIR-Rb | network | 1.04 | 1.006e-08 | 5.727e-03 | 0.000 | 6 | 448 |  | run_gpsonly_em25_12h |
| G13 | IIR-Rb | network | 0.81 | 8.409e-09 | 1.094e-03 | 0.125 | 6 | 341 |  | run_n305_attrib_3h |
| G13 | IIR-Rb | network | 1.20 | 8.409e-09 | 6.946e-03 | 0.000 | 6 | 341 |  | run_n305_attrib_6h |
| G14 | III-Rb | network | 0.98 | 1.176e-08 | 5.000e-03 | 0.119 | 6 | 497 |  | run_gpsonly_em25_12h |
| G14 | III-Rb | network | 1.03 | 4.242e-09 | 1.401e-03 | 0.079 | 6 | 299 |  | run_n305_attrib_3h |
| G14 | III-Rb | network | 1.00 | 4.239e-09 | 2.438e-03 | 0.089 | 6 | 497 |  | run_n305_attrib_6h |
| G15 | IIR-Rb | network | 1.12 | 5.942e-09 | 3.859e-03 | 0.130 | 6 | 493 |  | run_gpsonly_em25_12h |
| G15 | IIR-Rb | network | 0.97 | 5.791e-09 | 1.464e-03 | 0.137 | 6 | 360 |  | run_n305_attrib_3h |
| G15 | IIR-Rb | network | 1.03 | 5.332e-09 | 2.289e-03 | 0.135 | 6 | 494 |  | run_n305_attrib_6h |
| G16 | IIR-Rb | network | 0.96 | 1.482e-08 | 2.962e-03 | 0.099 | 6 | 486 |  | run_gpsonly_em25_12h |
| G16 | IIR-Rb | network | 0.88 | 3.802e-08 | 2.365e-03 | 0.152 | 5 | 172 |  | run_n305_attrib_3h |
| G16 | IIR-Rb | network | 0.96 | 2.524e-08 | 8.593e-03 | 0.000 | 6 | 341 |  | run_n305_attrib_6h |
| G17 | IIR-Rb | network | 0.96 | 5.533e-09 | 3.484e-03 | 0.164 | 6 | 493 |  | run_gpsonly_em25_12h |
| G17 | IIR-Rb | network | 1.13 | 1.592e-09 | 1.513e-03 | 0.181 | 6 | 341 |  | run_n305_attrib_3h |
| G17 | IIR-Rb | network | 0.97 | 1.522e-09 | 1.602e-03 | 0.193 | 6 | 493 |  | run_n305_attrib_6h |
| G18 | III-Rb | network | 1.11 | 1.519e-08 | 4.749e-03 | 0.000 | 6 | 417 |  | run_gpsonly_em25_12h |
| G18 | III-Rb | network | 1.21 | 2.786e-09 | 2.432e-03 | 0.145 | 6 | 341 |  | run_n305_attrib_3h |
| G18 | III-Rb | network | 1.22 | 6.247e-09 | 4.927e-03 | 0.211 | 6 | 401 |  | run_n305_attrib_6h |
| G19 | IIR-Rb | network | 1.04 | 5.299e-09 | 3.175e-03 | 0.135 | 6 | 341 |  | run_gpsonly_em25_12h |
| G19 | IIR-Rb | network | 1.16 | 2.418e-09 | 1.955e-03 | 0.208 | 6 | 341 |  | run_n305_attrib_3h |
| G19 | IIR-Rb | network | 1.15 | 2.418e-09 | 2.464e-03 | 0.169 | 6 | 341 |  | run_n305_attrib_6h |
| G20 | IIR-Rb | network | 1.02 | 6.247e-09 | 3.802e-03 | 0.052 | 6 | 483 |  | run_gpsonly_em25_12h |
| G20 | IIR-Rb | network | 0.88 | 2.973e-09 | 2.141e-03 | 0.171 | 6 | 236 |  | run_n305_attrib_3h |
| G20 | IIR-Rb | network | 1.08 | 4.391e-09 | 3.291e-03 | 0.166 | 6 | 373 |  | run_n305_attrib_6h |
| G21 | IIR-Rb | network | 1.17 | 1.523e-08 | 6.269e-03 | 0.000 | 6 | 497 |  | run_gpsonly_em25_12h |
| G21 | IIR-Rb | network | 0.78 | 4.547e-09 | 2.467e-03 | 0.090 | 6 | 360 |  | run_n305_attrib_3h |
| G21 | IIR-Rb | network | 1.03 | 9.102e-09 | 2.942e-03 | 0.062 | 6 | 497 |  | run_n305_attrib_6h |
| G22 | IIR-Rb | network | 1.30 | 3.875e-09 | 3.371e-03 | 0.207 | 6 | 338 |  | run_gpsonly_em25_12h |
| G22 | IIR-Rb | network | 0.93 | 4.713e-09 | 1.289e-03 | 0.081 | 6 | 299 |  | run_n305_attrib_3h |
| G22 | IIR-Rb | network | 1.04 | 8.681e-09 | 2.245e-03 | 0.065 | 6 | 336 |  | run_n305_attrib_6h |
| G23 | III-Rb | network | 1.05 | 6.430e-09 | 3.822e-03 | 0.032 | 6 | 497 |  | run_gpsonly_em25_12h |
| G23 | III-Rb | network | 0.97 | 3.308e-09 | 2.094e-03 | 0.152 | 6 | 360 |  | run_n305_attrib_3h |
| G23 | III-Rb | network | 1.09 | 2.902e-09 | 2.521e-03 | 0.197 | 6 | 497 |  | run_n305_attrib_6h |
| G24 | IIF-Rb | network | 0.93 | 5.689e-09 | 2.621e-03 | 0.167 | 6 | 497 |  | run_gpsonly_em25_12h |
| G24 | IIF-Rb | network | 1.06 | 3.216e-09 | 1.688e-03 | 0.167 | 6 | 360 |  | run_n305_attrib_3h |
| G24 | IIF-Rb | network | 1.10 | 3.622e-09 | 2.129e-03 | 0.154 | 6 | 497 |  | run_n305_attrib_6h |
| G25 | IIF-Rb | network | 1.07 | 1.394e-08 | 3.516e-03 | 0.000 | 6 | 338 |  | run_gpsonly_em25_12h |
| G25 | IIF-Rb | network | 1.40 | 2.554e-07 | 6.466e-03 | 0.000 | 5 | 161 | ★ | run_n305_attrib_3h |
| G25 | IIF-Rb | network | 1.38 | 1.854e-07 | 5.511e-03 | 0.000 | 5 | 161 | ★ | run_n305_attrib_6h |
| G26 | IIF-Rb | network | 1.05 | 8.741e-09 | 4.348e-03 | 0.000 | 6 | 481 |  | run_gpsonly_em25_12h |
| G26 | IIF-Rb | network | 0.76 | 4.272e-08 | 3.693e-03 | 0.160 | 6 | 341 |  | run_n305_attrib_3h |
| G26 | IIF-Rb | network | 0.99 | 2.913e-08 | 6.070e-03 | 0.194 | 6 | 497 |  | run_n305_attrib_6h |
| G27 | IIF-Rb | network | 1.00 | 1.291e-08 | 5.676e-03 | 0.000 | 6 | 497 |  | run_gpsonly_em25_12h |
| G27 | IIF-Rb | network | 1.02 | 9.816e-09 | 3.287e-03 | 0.075 | 6 | 341 |  | run_n305_attrib_3h |
| G27 | IIF-Rb | network | 1.05 | 1.038e-08 | 5.191e-03 | 0.212 | 6 | 497 |  | run_n305_attrib_6h |
| G28 | IIR-Rb | network | 1.01 | 1.195e-08 | 4.446e-03 | 0.000 | 6 | 484 |  | run_gpsonly_em25_12h |
| G28 | IIR-Rb | network | 0.94 | 2.239e-08 | 3.328e-03 | 0.184 | 6 | 360 |  | run_n305_attrib_3h |
| G28 | IIR-Rb | network | 1.03 | 1.891e-08 | 5.180e-03 | 0.129 | 6 | 498 |  | run_n305_attrib_6h |
| G29 | IIR-Rb | network | 1.05 | 5.578e-09 | 3.993e-03 | 0.054 | 6 | 484 |  | run_gpsonly_em25_12h |
| G29 | IIR-Rb | network | 1.01 | 2.585e-08 | 7.445e-03 | 0.000 | 6 | 360 |  | run_n305_attrib_3h |
| G29 | IIR-Rb | network | 1.11 | 3.442e-08 | 8.792e-03 | 0.000 | 6 | 497 |  | run_n305_attrib_6h |
| G30 | IIF-Rb | network | 1.00 | 9.578e-09 | 4.159e-03 | 0.076 | 6 | 498 |  | run_gpsonly_em25_12h |
| G30 | IIF-Rb | network | 0.97 | 5.224e-09 | 1.435e-03 | 0.101 | 6 | 360 |  | run_n305_attrib_3h |
| G30 | IIF-Rb | network | 1.12 | 6.391e-09 | 2.763e-03 | 0.071 | 6 | 497 |  | run_n305_attrib_6h |
| G31 | IIR-Rb | network | 1.01 | 1.300e-08 | 3.391e-03 | 0.000 | 6 | 484 |  | run_gpsonly_em25_12h |
| G31 | IIR-Rb | network | 0.89 | 4.026e-08 | 3.617e-03 | 0.114 | 6 | 341 |  | run_n305_attrib_3h |
| G31 | IIR-Rb | network | 1.21 | 2.431e-08 | 7.360e-03 | 0.000 | 6 | 497 |  | run_n305_attrib_6h |
| G32 | IIF-Rb | network | 0.93 | 7.125e-09 | 3.034e-03 | 0.150 | 6 | 442 |  | run_gpsonly_em25_12h |
| G32 | IIF-Rb | network | 1.02 | 9.469e-09 | 4.115e-03 | 0.118 | 6 | 360 |  | run_n305_attrib_3h |
| G32 | IIF-Rb | network | 1.03 | 1.090e-08 | 4.462e-03 | 0.069 | 6 | 497 |  | run_n305_attrib_6h |

## Provenance

- [intrinsic] `data/2024-129/products/COD0MGXFIN_20241290000_01D_30S_CLK.CLK`: 2880 epochs @ 30 s, 121 sats, pinned refs: none
- [network] `build/x64-Release/run_gpsonly_em25_12h`: 1440 epochs @ 30 s, 31 sats, pinned refs: G01
- [network] `build/x64-Release/run_n305_attrib_6h`: 720 epochs @ 30 s, 52 sats, pinned refs: E02, G01
- [network] `build/x64-Release/run_n305_attrib_3h`: 360 epochs @ 30 s, 52 sats, pinned refs: E02, G01

