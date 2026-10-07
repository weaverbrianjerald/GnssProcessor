# RF-0e: the level datum (e1 / e2) and the L2X coverage cut (e3) (2026-09-15)

**STATUS**

- **id:** RF-0e
- **topic:** code-bias
- **status:** closed
- **summary:** The level datum is NOT the term; the sparse C2X / L2X channel is: cutting it gives the best field consumer on record for three hours, then starves two stations.
- **dates in the text:** 2026-09-15
- **knobs / posture:** `network.sat_if_level_datum` (e1) and `network.sat_gf_datum_time_linked` (e2), both default OFF and refused together; e3 = e2 + the L2X / C2X cut from the canonical set; `broadcast_tgd`, GIM off, `cb_kaula_prior: false`; `code_datum_time_linked` read in `NetworkSbasis.cpp`
- **hashes named in the text:** commit `895724e` (worker branch `worktree-agent-a0aafda782ef5e7bd`, merged); exe `E2C0BF00…`; OFF smoke `47bc4e5a…`; suite 2175 / 289
- **run dirs named in the text:** `runs/_rf0e_field6_e3b` (the band-2-only cut)
- **source:** [RF-0d](RF-0d_per_signal_pin_2026-09-15.md) (the pre-registration, its bars and its frozen reading)

**Outcome.** The card asks whether restoring the IF level datum (e1, or e2's time-linked GF rows) recovers the field consumer RF-0d lost, and what cutting the L2X / C2X channel (e3) costs. Neither e1 nor e2 restores it; the consumer tracks the per-satellite IF-correction dispersion instead (p90 0.300 m on RF-0, 1.527 m on RF-0d). e3 reaches 0.82 m at hour 2 but withholds 215 epochs and collapses after hour 3: INEG00MEX and OAFA00ARG carry C2X as their only band-2 code. [RF-0f](RF-0f_isc_state_2026-09-15.md) found no station tracking both C2W and C2X, and [RF-0g](RF-0g_roster_cut_2026-09-15.md) removed C2X with its two stations; the goldens pin e2 and that cut ([RF-3-prep](RF-3-prep_golden_rebase_2026-09-15.md)).

**Status as recorded (2026-09-15):** closed — **the level datum is NOT the term (the consumer tracks the per-satellite dispersion of the IF correction, not the common
level); the sparse C2X / L2X channel IS the term — cutting it gives the best field consumer on record for three hours and then starves two
stations of their second frequency.  RF-0f below keeps the channel in the solve and takes its satellite bias out of the pin.**  Worker branch
`worktree-agent-a0aafda782ef5e7bd` (`895724e`), merged.  Suite 2175 / 289 (0 failed); OFF smoke `47bc4e5a…`; `--verify` PASS; exe `E2C0BF00…`.
Knobs `network.sat_if_level_datum` (e1: one absolute row per constellation on `Σ_s (a₁d₁ + a₂d₂) = 0`) and `network.sat_gf_datum_time_linked`
(e2: the GF rows' RHS carried from the previous kept epoch, the absolute `c·T_GD` at first sighting only), both default OFF, refused together.

**The code read, cited:** `code_datum_time_linked` makes the IF pins change pins conditionally (`NetworkSbasis.cpp:722–735`: `zval = a₁v₁ + a₂v₂`
only when both signals have a veteran value and the pair is unchanged; otherwise absolute zero and a re-seed bump) — each satellite's IF level is
anchored once at first sighting and carried at a finite weight, so RF-0d's suppression really left the set level unstated.  Two corrections
to the card's framing, both measured: the level is not a rank hole (the pin rows span it; only the RHS is unanchored — the unit test measures a
carried level's realization, not rank); and e1's row is not gentle — its Jacobian has one entry per satellite, so it re-zeroes the level every
epoch (`L/(n+1)`: 0.0227 for L = 0.75, n = 32); the differences carry.

## The arms (6 h cold; `broadcast_tgd`, GIM off, prior off; field consumer p50, m)

| hour | RF-0 (GIM) | RF-0c tgd | RF-0d m2 | e1 | e2 | **e3 = e2 + the L2X / C2X cut** |
|---|---|---|---|---|---|---|
| 1 | 3.02 | 8.47 | 21.71 | 17.82 | 21.75 | **1.76** |
| 2 | 3.00 | 2.94 | 18.07 | 16.54 | 18.17 | **0.82** |
| 3 | 1.54 | 1.01 | 10.03 | 12.10 | 9.40 | **1.55** |
| 4 | 2.14 | 2.20 | 7.23 | 9.72 | 5.43 | 23.68 |
| 5 | 0.98 | 0.66 | 4.90 | 6.20 | 4.74 | 46.96 |
| 6 | 3.29 | 3.80 | 3.51 | 5.04 | 3.94 | 11.83 |

e1 / e2: field 715 / 5, sim 720 / 0 at declared = observed, the sim consumer identical to RF-0d to the digit; neither restores the field consumer
(FAIL against RF-0c and against ± 0.5 m of RF-0 in every hour 2–6); the wire a wash.  **e3: 505 / 215 withheld** (100 gate + 115 σ̂₀; 66 in hour
1, 30 / 56 / 58 in hours 4–6) — L2X and C2X absent from the wire (0 rows).

## The reads (the frozen reading's second branch)

**The per-satellite IF correction the consumer actually applies, `−clock + a₁·b(C1C) + a₂·b(C2W)`, hour 2 → 6:**

| arm | common drift (cancels in the consumer's clock) | per-satellite deviation p50 / **p90** / max | field consumer h2–h6 |
|---|---|---|---|
| RF-0 (GIM) | −1.24 m | 0.100 / **0.300** / 1.52 | 0.98–3.29 |
| RF-0c tgd | −8.14 | 0.183 / **0.552** / 1.57 | 0.66–3.80 |
| RF-0d m2 | −17.16 | 0.185 / **1.527** / 3.44 | 3.51–18.07 |
| e1 | −17.12 | 0.149 / **1.148** / 3.23 | 5.04–16.54 |
| e2 | −16.92 | 0.149 / **1.022** / 3.38 | 3.94–18.17 |

**The p90 tracks the consumer monotonically across all five arms; the common drift does not.**  RF-0d multiplied the per-satellite dispersion
2.8×; e1 / e2 shave it ~30 % and the consumer follows by about that.  The worst five satellites are the same in RF-0d, e1 and e2 — **G12, G08,
G29, G32, G10 — three of them in the ten-satellite C2X set.**  The per-satellite code / phase post-fit split is flat across the arms (does not
separate them).  **e3's harm is separable and measured:** a band-2-only cut (`_rf0e_field6_e3b`) reproduces e3 exactly (505 / 215) — the
collateral is not band 1: **two stations (INEG00MEX, OAFA00ARG) carry C2X as their only band-2 code**, so removing the channel from the
canonical set removes their second frequency outright (17 near-null directions at epoch 1).

## Grade and reading

Neither e1 nor e2 is the baseline (the frozen reading's first branch does not fire); the second branch fires and its read names the term:
**the sparse C2X channel's satellite code bias, pinned at ISC = 0 (a value the navigation message does not carry) and used by ten satellites and
two single-band-2 stations, is what disperses the per-satellite IF correction** — removing the channel gives the best field consumer in the
record (0.82 m at hour 2, against RF-0's 3.00 and RF-0c's 2.94) until the two stations starve.  The coverage cut as specified is not the
baseline either; the level datum knobs stay built, default OFF, as instruments.  The per-satellite IF-correction dispersion read joins the
grader as a diagnostic.

## RF-0f (pre-registered): C2X in the solve, out of the pin — the ISC as a state

Build, default OFF, byte-identical: the T_GD pin applies ONLY to the P-code pair (C1C / C1W with C2W); a same-band companion on band 2 (C2X)
is NOT pinned — its satellite code bias is `b(C2W) + ISC^s`, with `ISC^s` a per-satellite constant STATE (q 0), estimable at any station
tracking both C2W and C2X on that satellite (their difference is a pure bias difference — no iono, no clock); where no station tracks both,
the ISC column is unobservable and is pinned at 0 for that satellite WITH a counter and a per-satellite flag on the wire (`isc=obs | pinned`);
the C2X bias is EMITTED only when its ISC is observed.  The consumer's reader accepts the flag.  Unit tests: the ISC state is estimable with
one dual-tracking station and pinned without; the C2W pin unchanged; the OFF path byte-identical.  **Arms (6 h, both beds, `broadcast_tgd`,
GIM off, prior off, e2 on or off — say which; the L2X cut OFF):** RF-0f.  **Bars:** field 715 / 720 or better with the two C2X-only stations
present all day; the per-satellite IF-correction dispersion p90 ≤ RF-0c's 0.55 m; the field consumer per-hour p50 ≤ e3's in hours 1–3 (1.76 /
0.82 / 1.55) and no collapse after (≤ RF-0 in hours 4–6); D_phase ≤ RF-0d's; sim unchanged (720 / 720, declared = observed).  **Frozen reading:**
all ⇒ **RF-0f IS the baseline** — self-contained, the GIM gone, the ledger rows on the author's go, the RF-1 + RF-2 winner re-run on it; the
dispersion falls but the consumer does not ⇒ read the remaining worst satellites' IF correction against their C2X / ISC status; the ISC
unobservable for most satellites ⇒ the roster read (which stations track both) decides whether C2X is emitted at all.
