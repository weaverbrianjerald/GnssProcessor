# RF-0g: the roster cut, C2X and its two C2X-only stations removed (2026-09-15)

**STATUS**

- **id:** RF-0g
- **topic:** network-vehicle
- **status:** closed
- **summary:** Best field posture of the program at the time (no six-hour collapse) and NOT the baseline by its bars: 75 ignition withholds in hour 1, hours 5 to 6 above 1.5 m.
- **dates in the text:** 2026-09-15
- **knobs / posture:** config-only: e3's config (e2 + the L2X / C2X band-2 cut; `broadcast_tgd`, GIM off, prior off) minus INEG00MEX and OAFA00ARG, 59 stations; `sat_isc_state` unset
- **hashes named in the text:** exe `0DC43C60…` (RF-0f's); OFF smoke `9add1d3a…`
- **run dirs named in the text:** none
- **source:** [RF-0f](RF-0f_isc_state_2026-09-15.md) (the pre-registration, its bars and frozen reading)

**Outcome.** The question: does cutting C2X and its two C2X-only stations give a self-contained baseline. The field emits 640 / 720 against the 715 bar and the consumer FAILS hours 1, 2, 5 and 6; the IF-correction dispersion p90 passes at 0.512 m. Not the baseline by its frozen bars; at the author's instruction the goldens were nonetheless re-based on it ([RF-3-prep](RF-3-prep_golden_rebase_2026-09-15.md)). The posture solved a median of 28 of 59 field stations; [RF-PD-fix](RF-PD-fix_station_prior_before_pd_2026-09-15.md) removed every withhold, while the golden consumer degraded about 6×. The goldens moved to the float vehicle on 2026-09-22 ([GRB](GRB_golden_rebase_2026-09-22.md)).

> **⚠ CORRECTED 2026-09-15 ([RF-3-prep](RF-3-prep_golden_rebase_2026-09-15.md)):** the comparison below quotes RF-1 + RF-2b as "0.77–1.48 m every hour" on the GIM baseline — those were ratios to RF-0 (see the RF-2b card's correction); its field consumer was 2.32 / 2.83 / 2.29 / 1.89 / 1.23 / 3.05 m.  RF-3-prep then ran RF-1 + RF-2b on this posture: it FAILS both beds, and the goldens were re-based on RF-0g itself.

**Status as recorded (2026-09-15):** closed — **the best field posture the programme has produced (3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68 m, the first six-hour field
consumer with no collapse; the per-satellite IF-dispersion bar passed at 0.512 m), and NOT the baseline by its bars: an ignition rank deficit
withholds 75 epochs in hour 1, and hours 5–6 drift above 1.5 m.**  Config-only on the merged tree (exe `0DC43C60…` = RF-0f's; `sat_isc_state`
unset); suite 2194 / 290 (0 failed); OFF smoke `9add1d3a…`.  Roster read before the run: the two stations contribute to 16 satellites (G10,
G32, G28 most, 213–214 of 505 epochs), **no satellite drops below 2 stations** (cells under the registration threshold 166 before and after;
median stations per satellite-epoch 6) — the cut removes data, not observability.

## The arms (6 h cold; e3's config − INEG00MEX − OAFA00ARG; 59 stations; `broadcast_tgd`, GIM off, prior off, e2, the band-2 cut)

| arm | emitted / withheld |
|---|---|
| sim | 720 / 0, declared = observed (unchanged) |
| field | **640 / 80** (all gate: 75 in hour 1, 5 in hour 2, 0 after) |

| hour | RF-0 (GIM) | RF-0c | e3 | **RF-0g** | bar |
|---|---|---|---|---|---|
| 1 | 3.02 | 8.47 | 1.76 | 3.77 | ≤ 1.76 ✗ |
| 2 | 3.00 | 2.94 | 0.82 | 1.08 | ≤ 0.82 ✗ |
| 3 | 1.54 | 1.01 | 1.55 | **1.28** | ✓ |
| 4 | 2.14 | 2.20 | 23.68 | **1.25** | ≤ 1.5 ✓ |
| 5 | 0.98 | 0.66 | 46.96 | 1.78 | ✗ |
| 6 | 3.29 | 3.80 | 11.83 | 2.68 | ✗ |

IF-correction dispersion p90 **0.512 m** (bar 0.55 ✓; RF-0d 1.527, e2 1.022, RF-0c 0.552, RF-0 0.300); D_code at or better than RF-0d in 5 of 6
hours; D_phase worse than RF-0d in 4 of 6 (the tail now L2W-led: G18 L2W 62 m); M4 holds.  The gate sees **16 near-null directions vs declared 1
at epoch 1** (e3: 17; the 61-station postures 8): removing the X channel removes band-2 observables from more stations than the two that carry
it only on X — an ignition deficit both cut postures share.

## The read (the frozen reading's third branch: hours 1–2 do not reach e3's)

Between e3 and RF-0g over hours 1–3 (both healthy there) the per-satellite IF correction moved by **p50 1.07 / p90 3.68 / max 5.88 m** about a
+0.60 m common part: G19 −5.88, G29 +5.35, G25 −3.73, G32 −3.68, G06 −3.15, G28 −3.07, G05 +2.32, G22 −2.06 — **half of the eight biggest movers
(G05, G06, G19, G22) were never seen by either dropped station.**  e3's 0.82 m at hour 2 was not "the same solution minus two stations": the
two postures reach different per-satellite realizations, so the roster cut does not reproduce e3's early hours by removing what e3 removed.

## Grade and what is next

Not the baseline (715 / 720 and the hour-1 / hours 5–6 bars fail); the best posture in the record, and the natural base for the vehicle
candidate: **RF-1 + RF-2b has not yet been run on this posture** (it was graded on the GIM baseline, where it gave 0.77–1.48 m every hour).
What remains to explain on RF-0g has no known gross term left: (1) the ignition deficit (16 near-null directions at epoch 1, 75 withholds in
hour 1 — which directions, from `network.trace_station_block` and the SBC spans; whether the S-basis declared dimension should follow the
cut), and (2) the hours 5–6 drift, which is where RF-2b's R-CL read points (the clock's own level channel, RF-2c).  Pre-registered next, not
launched at the author's stopping point: **RF-3-prep** = RF-1 + RF-2b on the RF-0g posture, both beds, 6 h, the same bars (the field consumer
per-hour p50 ≤ 1.5 m every hour and settled ≤ 0.6× RF-0g's; the sim's integers kept), beside RF-2c's attribution read of the clock level.
