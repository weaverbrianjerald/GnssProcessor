# RF-0d: per-signal GF pin, S-line declaration, thin-satellite phase gauge (2026-09-15)

**STATUS**

- **id:** RF-0d
- **topic:** code-bias
- **status:** closed
- **summary:** The per-signal GF pin is a real datum and closed RF-0c's inconsistent sum-to-zero rows; NOT the baseline: the field consumer regressed 4 to 15 m while the wire improved.
- **dates in the text:** 2026-09-15
- **knobs / posture:** `network.sat_gf_datum: "broadcast_tgd"` extended to every registered code column beyond the IF pair (same-band extras left free); the per-signal sum-to-zero row suppressed for a fully pinned channel; S-line token `sat_gf_datum=<mode>`; `phase_datum_min_stations` 2 against 3; `w6_iono_constraint: none`, GIM off, `cb_kaula_prior: false`; `code_datum_time_linked` armed
- **hashes named in the text:** commits `9b804bb` (item 1), `9bf00f3` (item 2) (worker branch `worktree-agent-a0aafda782ef5e7bd`, merged); OFF smoke `87a33bf0…`, then `47bc4e5a…` (canonical from this merge on; record stream `333dcb08…`); suite 2154 / 289
- **run dirs named in the text:** none
- **source:** [RF-0c](RF-0c_gf_pin_2026-09-15.md) (the pre-registration and its bars)

**Outcome.** The card asks whether a per-signal pin, an S-line declaration and a thin-satellite phase gauge close RF-0c's L2X tail and its ignition withholds. The pin closes the L2X direction and the hour-1 withholds go to 0, but `min_stations 3` makes ignition worse (32 withholds; the residual phase gauges are RF-2's election rule, not coverage), the L2X tail stays at 39.9 m against a 10 m bar, and two of seven bars PASS. The attributed cause, the level anchor lost with the suppressed sum-to-zero rows, is not the term: [RF-0e](RF-0e_level_datum_2026-09-15.md) names the sparse C2X channel. The canonical OFF smoke hash later moved to `9add1d3a…` ([RF-0f](RF-0f_isc_state_2026-09-15.md)).

**Status as recorded (2026-09-15):** closed — **the pin is a real datum and closed a shipped defect; NOT the baseline: the field consumer regressed while the wire
improved, and the cause is named (the absolute GF rows against the time-linked level datum) — RF-0e below.**  Worker branch
`worktree-agent-a0aafda782ef5e7bd` (`9b804bb` item 1, `9bf00f3` item 2; item 3 needed no code), merged.  Combined tree certified first
(suite 2154 / 289, 0 failed; OFF smoke `87a33bf0…`).  After item 2 the OFF smoke hashes **`47bc4e5a…`** — 22 bytes of header LEGEND (the S-line
token must be named in the legend, `HeaderLegendDeclaresEveryEmittedField`), the 6 596 record lines byte-identical (record stream `333dcb08…`);
**`47bc4e5a…` is the canonical OFF hash from this merge on** (the same re-baseline `serialize_disc_counters` caused).

**Item 1 — the per-signal pin.**  Every registered code column beyond the IF pair is pinned through its GF combination against the band-1
reference at the same `c·T_GD` (an identity under `d_j = c·T_GD·μ_j`); same-band extras (C1W beside C1C) are left free — their GF combination
does not exist and their difference is a pure ISC, observable without any ionosphere; every cross-band row asserts a zero ISC (RINEX NAV
carries no GPS ISC) and is counted.  **A defect RF-0c shipped, closed here:** once a channel is fully pinned its per-signal SUM-TO-ZERO row is
not redundant but INCONSISTENT (the pinned values sum to `μ_j·c·Σ_s T_GD^s`, tens of metres) and both fold at the gauge weight — they fought,
shifting every emitted code bias of the channel; the row is now suppressed for a fully-pinned channel (field: 1 923 extra rows, 2 028 rows
suppressed).  Unit test: on C1C / C1W / C2W / C2X with the prior off, 0 surviving rd-3b directions; the L2X column's own direction 31 of 32 open
before, 0 after.  **Item 2 — the S line** carries `sat_gf_datum=<mode>` as a new token (writer, parser, `NetworkSolPrep` from the config, the
run manifest).  **Item 3 — the read:** the 9 residual field directions are all PHASE (`PB|Gxx|L2W`-led, 7 machine-zero per-satellite
`PB(L1C, L1W, L2W) + CLK` gauges); those satellites are NOT thinner than the rest (stations at ignition median 4 either way) — the gate
counts per (satellite, signal), where 44 of 90 phase keys sit at ≤ 2 stations; `min_stations 3` prunes more than half the shared phase
columns and makes ignition strictly WORSE (32 withholds, 27 in hour 1, 16 code-bias directions opened) — the thin-satellite gauge is RF-2's
election rule, not a coverage bar.  **L2X is carried by 5 satellites at 1–2 stations each all day** — a coverage accident, not a datum problem.

## The arms (6 h cold, `broadcast_tgd`, `w6_iono_constraint: none`, GIM off, `cb_kaula_prior: false`; m2 = RF-0c's config + this build only)

| arm | emitted / withheld | gate | SBC engaged |
|---|---|---|---|
| sim m2 / m3 | 720 / 0 both | 1 vs 1 | 0 |
| field m2 (`min_stations 2`) | **715 / 5** (0 in hour 1; the 5 at +5 190…+5 310 s) | 8 vs 1 | 164, all PB |
| field m3 (`min_stations 3`) | 688 / 32 (27 in hour 1) | 12 vs 1 | 263 (247 PB + 16 CB) |

| field, hours 1–6 | consumer p50 (m) | D_phase / RF-0 (×) | D_code / RF-0 (×) |
|---|---|---|---|
| RF-0c `broadcast_tgd` | 8.47 / 2.94 / 1.01 / 2.20 / 0.66 / 3.80 | 2.14 / 1.54 / 0.90 / 2.53 / 1.94 / 2.53 | 2.47 / 2.11 / 2.95 / 3.02 / 1.48 / 1.10 |
| **RF-0d m2** | **21.71 / 18.07 / 10.03 / 7.23 / 4.90 / 3.51** | — / 1.25 / 0.77 / 1.59 / 1.32 / 2.19 (better in 4 of 5) | — / 1.45 / 2.04 / 2.28 / 0.96 / 0.69 |

The phase-bias tail: G10 L2X 249 → 66 → **39.9 m** (RF-0c zero → tgd → RF-0d), the tail no longer L2X-led (G01 L2W 58, G14 L2W 43.5 lead; share
70 %).  M4 holds (0 unsignalled PB; clock steps in (0.5, 2] m).  Sim: m2 ≡ m3 to the digit (its roster is C1C / C2W only — 0 extra rows); the
clock LEVEL vs truth 1.46–2.03 m as RF-0b / RF-0c left it (the GIM-as-truth confound is untouched by any datum, as expected).

## Grade

Bars: withholds ≤ 5 in hour 1 **PASS** (0); sim declared = observed 720 / 720 **PASS**; L2X < 10 m **FAIL** (39.9); tail share < 60 % **FAIL** (70 %);
0 withholds after hour 1 **FAIL** (5); wire within ± 20 % **FAIL** (1.3–2.3×, though better than RF-0c in 4 of 5 hours); **consumer within ± 0.5 m
FAIL — it REGRESSED 4–15 m through hour 5 while the wire, the gate and the withholds all improved and the sim is unchanged to the digit.**
The worker's attribution, consistent with the build's own change and stated as a hypothesis: `code_datum_time_linked` is armed, so the IF pins
and the sum-to-zero rows are CHANGE pins (RHS = the previous kept epoch's solved values) while the new GF rows are ABSOLUTE every epoch;
suppressing the sum-to-zero rows removed the veteran-referenced LEVEL anchor from three of four channels, leaving each satellite's IF level
anchored only at first sighting and carried — a drift of the clock / IF-bias split that cancels for the consumer's code but not against its
phase.  The manager's reading of the same fact: a per-signal sum-to-zero is inconsistent with per-satellite GF pins, but **one absolute level
row per constellation on the IF combination (`Σ_s d_IF^s = 0`) is consistent with them** — the GF pins fix only differences — and it is the
level datum the suppression threw away.  Both readings are one A/B each.

## RF-0e (pre-registered): restore the level datum — three cheap arms, the coverage cut, and the baseline decision

Base = RF-0d m2's config (`broadcast_tgd`, GIM off, prior off, `min_stations 2`).
- **e1** — one absolute sum-to-zero row per constellation on the IF combination of the satellite code biases (in place of the suppressed
  per-signal rows; the GF pins keep the differences), folded at the gauge weight, time-linked like the IF pins if `code_datum_time_linked`
  makes them change pins (then the absolute row anchors the level at first sighting and the change pin carries it — say which the code does);
- **e2** — the worker's alternative: the GF rows' RHS time-linked (a change pin on the GF combination with the absolute `c·T_GD` at first
  sighting only), the per-signal sum-to-zero rows left suppressed;
- **e3** — the better of e1 / e2 **with L2X removed from the canonical / emitted signal set** (the coverage cut the card's fallback names: 5
  satellites at 1–2 stations cannot carry a channel) — the L2X columns neither registered nor emitted; a per-signal roster read beside it.
Unit tests: e1's row is consistent with the GF pins (0 residual on a pinned roster) and closes the IF level (1 direction → 0 with the prior
off); e2's change pin leaves the first-sighting anchor in force.  **Arms:** both beds, 6 h.  **Bars (the field, hours 2–6):** consumer p50 ≤ RF-0c
`broadcast_tgd`'s in every hour (2.94 / 1.01 / 2.20 / 0.66 / 3.80) AND within ± 0.5 m of RF-0 (GIM) from hour 2; D_phase and D_code ≤ RF-0d m2's
(the wire gain kept); withholds ≤ 5 total; sim 720 / 720 declared = observed; e3's L2X tail absent from the emitted wire.  **Frozen reading:**
e1 or e2 restores the consumer ⇒ the level datum WAS the term; the restoring arm + the coverage cut (e3) **IS the baseline** — the GIM leaves the
producer, the RF-1 / RF-2 winner is re-run on it, and the ledger rows are written on the author's go; neither restores it ⇒ read the clock /
IF-bias split directly (per satellite, the IF code-bias level and the clock over time against COD's, and the consumer's code-minus-phase
residual per satellite) before anything else is built.
