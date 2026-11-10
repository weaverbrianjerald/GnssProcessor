# RF-2: the phase datum as a held S-basis, every handover declared (2026-09-15)

**STATUS**

- **id:** RF-2
- **topic:** network-vehicle
- **status:** closed
- **summary:** The held S-basis holds the field's phase wire below RF-0 in every hour; the sim FAILS (WL integers 99.0 % to 35.2 %) on one election defect, fixed by RF-2b.
- **dates in the text:** 2026-09-15
- **knobs / posture:** `network.phase_datum_held_basis` (default false) + `phase_anchor_min_age_ep` 10 + `phase_anchor_flag_quiet_ep` 5; interlocks: requires `float_kf` and `phase_datum_prioritized`; arms A3 + RF-2 on the RF-0 configs, 6 h cold; tests `tests/test_phase_held_basis.cpp` (15 cases)
- **hashes named in the text:** commit `37e3a5c` on worker branch `worktree-agent-a2915f195d8518bf6`, merged; combined tree `89e78b4`; exe `4a084ad1…`; OFF smoke `47bc4e5a…`
- **run dirs named in the text:** none
- **source:** [RF-1-steps345](RF-1_steps345_mechanism_2026-09-15.md) (the pre-registration, bars and frozen reading)

**Outcome.** The question: does holding each phase-datum basis ambiguity at its elected value, with every handover declared, stop the churn RF-1 exposed. The field passes I2 with 0 erases and 0 value drift, but its consumer FAILS I1 from hour 2, tracking a clock-level ramp (0.70 to 2.29 m), and PB marks reach 5.3× RF-0's count against a 1.5× bar. The authorized election fix and clock-ramp read were done by [RF-2b](RF-2b_election_fix_2026-09-15.md), which restored the sim and removed the ramp; the held basis with the float vehicle is the goldens' posture since 2026-09-22 ([GRB](GRB_golden_rebase_2026-09-22.md)).

**Status as recorded (2026-09-15):** open — **the central claim is validated on the field (the within-arc phase wire below RF-0 in every hour), the sim fails on a
single measured election defect (mine to authorize, one predicate), and the field's consumer now tracks a clock-level ramp that needs its own
read**; the fix and the read are pre-registered below.  Worker branch `worktree-agent-a2915f195d8518bf6` (`37e3a5c` — one commit sectioned per
item: items 1 / 3 / 4 are branches of one election function sharing one predicate and do not split into building intermediates), merged.
Combined tree certified on 89e78b4 (suite 2171 / 289, 0 failed; OFF smoke `47bc4e5a…`).  Build: `network.phase_datum_held_basis` (false) +
`phase_anchor_min_age_ep` 10 + `phase_anchor_flag_quiet_ep` 5; interlocks: requires `float_kf` and `phase_datum_prioritized`.  (1) `PhaseBasisEntry`
in the carry state — a retained incumbent re-pins at the value FROZEN at election, read from the ledger (supersedes R2's `anchor_pin_target`,
which only continuous handovers populated and every epoch re-wrote); `n_pb_basis_value_drift` must read 0.  (2) Continuous AND cold handovers
queue the satellite into `ura_retro_sats` → `markSatDiscontinuity` with a `STRANSFORM` event carrying Δ; the network-wide anchor-reref erase is
RETIRED under the knob (counter 0).  (3) Election: highest elevation among candidates with arc age ≥ `min_age_ep` and no screen flag within
`flag_quiet_ep`; the coverage sweeps stay as fallback.  (4) `amb_flag_ep` stamps every flag; a flag on the basis column deposes the incumbent
inside the election, before PASS B and before any response touches the column.  `tests/test_phase_held_basis.cpp` 15 cases; suite 2186 / 290
(0 failed); OFF smoke `47bc4e5a…`; `--verify` PASS; exe `4a084ad1…`.

## Field (A3 + RF-2 vs RF-0 and A3; 6 h cold, the RF-0 config)

| hour | D_phase declared rms: RF-0 → A3 → **RF-2** | consumer p50: RF-0 → A3 → RF-2 | clock LEVEL: RF-0 → RF-2 |
|---|---|---|---|
| 1 | 3.21 → 9.31 → **2.88** | 3.02 → 5.80 → 3.02 | 0.70 → 1.22 |
| 2 | 4.06 → 7.25 → **1.27** | 3.00 → 3.23 → 2.97 | 0.62 → 1.41 |
| 3 | 4.79 → 7.85 → **1.06** | 1.55 → 1.87 → 3.25 | 0.63 → 1.50 |
| 4 | 2.16 → 49.5 → **0.63** | 2.14 → 2.95 → 6.48 | 0.68 → 1.67 |
| 5 | 4.31 → 115 → **1.57** | 0.98 → 4.53 → 9.05 | 0.71 → **2.23** |
| 6 | 1.48 → 14.9 → **0.76** | 3.29 → 4.47 → 8.48 | 0.74 → **2.29** |

**I2 PASSES in every hour** (70× better than A3 at hour 4); 61 873 held family-epochs, 2 279 elections, erases 0, value drift 0; 720 / 720;
unsignalled clock steps 0; cold handovers 23 % (A5: 68 %); the worst PB excursion 1 409 → 97 m (RF-0 136).  **I1 FAILS from hour 2**, tracking a
MONOTONE clock-level ramp (0.70 → 2.29 m) the freed phase datum has pushed into the clock; the PB body worsens (|ΔPB| > 5 cm 4.8 → 16.0 %) and
**PB marks 75 → 399 = 5.3× the 1.5× bar** — a bar in tension with "declare every handover" by construction (1 120 declared handovers in 6 h
are steps that previously moved the wire unmarked), and a handover RATE (187 / h) that a sticky held basis should not have.

## Sim — fails hard, with one measured cause (the worker's, and it is the election, not the held basis)

358 / 720 emitted (362 withheld by the σ̂₀ ceiling); **WL at the integer 99.0 % (A3) → 35.2 %**, L1C med |e| 0.092 → 5.72 cyc; the consumer 1.1–35×
worse; the clock level 0.78 → 10–12 m; D1 `b` 0.989 (the fold is fine); erases 0, drift 0.  **Cause:** item 3's elevation rank REPLACED the
ledger-usability screen instead of composing with it — it never asks whether the candidate's carried level passes the continuous branch's own
`ledgerVerdict` (`phase_datum_handover_max_var_m2` 0.01 on both beds; `phase_datum_candidacy_usable` unarmed) — so on the sim, whose ambiguity
scale is ~5× the field's, **67 % of elections go cold (1 049 of 1 562; field 11 %)**, each re-realizing the datum at 0.  Per the card's frozen
reading ("the sim's integers lost ⇒ stop and read") the worker stopped.

## Grade and the manager's decisions

The held basis is the right object (the field's phase wire proves it; the sim's fold is intact); the ELECTION is mis-specified and the field's
clock ramp is unread.  **Authorized (RF-2b):** (a) the one-predicate fix — rank by elevation AMONG LEDGER-USABLE candidates (the continuous
branch's `ledgerVerdict`), elevation-only as the fallback when none qualifies, and a counter per deposition cause (flag on the basis column /
arc death / ledger-unusable / fallback) — a sticky incumbent must not be deposed by a flag elsewhere at its station; (b) the marks bar re-based
to what "declare every handover" implies: PB marks ≤ the declared-handover count + 1.5× RF-0's non-handover marks, AND the handover rate ≤ RF-0's
anchor re-reference rate (~98 / h); (c) **the clock-ramp read (R-CL)** on the field arm, old and new: per satellite the clock LEVEL against
COD over time beside that satellite's basis tenure (handover epochs, the basis station, its elevation and age at election) and the Q3
anchor's decouples — does the ramp RESET at handovers (⇒ the held value carries the basis station's bias at election: the ledger screen and a
higher `min_age` are the lever) or run through them (⇒ the clock has lost the level datum the churning PB used to supply, and needs its own —
a per-constellation clock level row or the anchor's `gauge_free` mode)?  **Arms:** both beds, 6 h, A3 + RF-2b, on the RF-0 configs.  **Bars:** the
sim's S3 restored (WL ≥ 98 %) with 720 / 720; the field's I2 kept (D_phase ≤ RF-0 every hour); the field consumer per-hour p50 no hour worse
than RF-0 and settled ≤ 0.6×; the re-based marks bar; unsignalled 0.  **Frozen reading:** all ⇒ RF-1 + RF-2 is the vehicle and the goldens
re-base on it (on RF-0e's posture once that is the baseline); the sim restored but the field consumer still ramping ⇒ R-CL's answer is the next
card (the clock's level datum vs the basis value's bias); the sim not restored ⇒ the held basis is mis-specified in which column is held —
stop and read the sim's election ledger.
