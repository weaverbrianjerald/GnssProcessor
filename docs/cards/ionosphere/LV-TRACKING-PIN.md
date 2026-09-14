# LV tracking pin — the tracking-pin fix, tested and FALSIFIED

**STATUS**

- **id:** LV tracking pin
- **topic:** ionosphere
- **status:** closed
- **summary:** the tracking-pin fix, tested and FALSIFIED
- **dates in the text:** 2026-08-05
- **knobs / posture:** `network.rx_cb_carry_q`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 15, 15.1, 15.2

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 15. The tracking-pin fix, tested and FALSIFIED (2026-08-05)

§14.4's obvious follow-on: if the drift comes from a FROZEN pin (§13), let the pin track. The cheapest form is
one knob — raise the carry's random walk so it forgets on a physical timescale instead of never:

    network.rx_cb_carry_q : 1e-9 -> 1e-6 m²/s      (tau = var/q ~ 2.8 h)

At 30 s the inflation is 3e-5 m² against a ~1e-2 m² posterior, so within-epoch difference rigidity is
untouched; over hours the pin can follow. Arm 29, `_ext63_gps_dual_12h_lv125_q6.json`, 12 h, single knob vs
arm 25. **Prediction (in the config before the run): the ramp flattens — hours 3–11 inside ±8 TECU while hour
0 keeps the stack's good ignition. MEASURED: the opposite.** Arm 29's hourly `a₀₀ − GIM_mean`:

| arm | h0 | h1 | h2 | h3 | h5 | h8 | h11 | run mean |
|---|---|---|---|---|---|---|---|---|
| 29 LV stack, q=1e-6 | +5.0 | +13.4 | +16.9 | +20.2 | +17.1 | +20.0 | **+25.5** | **+17.81** |

Loosening the carry made it **worse and sooner**: the +20 plateau arrives at hour 3 instead of hour 5, and the
run ends +25.5 instead of +20.2, still climbing (12 h mean +17.8 vs the arm-25 stack's +13.9). Carry mechanics
stayed healthy (87435/87679 folds, 183 erases, μ ≈ 0) — this is the level responding, not the machinery
breaking.

### 15.1 What that rules out, and what it leaves

The frozen pin was **resisting** the drift, not causing it. So the driver is not the pin's stiffness, and the
ranking across four postures is not monotone in "how much carry information is admitted":

| posture | carry common mode | 12 h level |
|---|---|---|
| canonical (43 % folds, no de-mean) | **rolling** — re-asserts LAST epoch's realization | wanders about -8, ends +1.0 |
| arm 14 (carry OFF) | none | -9 TECU/epoch collapse to -37 |
| LV stack q=1e-9 (97 % folds, de-meaned) | **frozen** at epoch-1's seed frame | ramps to +20.2 |
| LV stack q=1e-6 | frozen, decaying over ~2.8 h | ramps to +25.5 |

**The rolling memory is what TRACKS the level.** The "flywheel" LV-2 was written to remove is also the only
thing in the current design that follows the field's evolution, and the breathing (±6 TECU) is the price it
charges. Removing it buys a quiet, self-consistent frame that is quietly wrong by +20 TECU twelve hours later.
⚠ **§19.1 adds the fifth row and weakens this**: LV-4-alone is the SAME rolling memory at 97.5 % admission and
it **parks at 2.69 TECU with 46/121 epochs out of band** (at the voided 10/25 masks; at 15/25 it is the best
61-sta tracker measured — §24.2). The level responds to the KIND of memory **and how much of it is admitted**.

### 15.2 The next card, now with four arms behind it

Neither a rolling memory, nor a frozen memory, nor no memory is a designed authority. The receiver-DCB common
mode needs a **per-epoch, data-derived estimate** — §13.4 option 1 — with a stated estimator, an event
response, and its own counters. Everything else here is scaffolding:

- LV-4 makes the carry's information admissible at all (the retired screen withheld 74 % of it on a frame
  artifact) — keep. ⚠ **§19.3 inverts this line**: LV-4 is not a prerequisite to keep, it is the ladder's
  data-side cost (+64 % postfit phase rms at the 10/25 masks).
- LV-5 makes any de-mean stable at full admission — keep.
- LV-1 is the instrument that showed the pin exists and where it points, and its per-station offsets are a
  free receiver-GF-bias diagnostic — keep as a diagnostic.
- LV-2 as it stands trades tracking for stability, and the trade is bad on a settled window — do not arm
  until the designed authority above replaces what it removes.

---

