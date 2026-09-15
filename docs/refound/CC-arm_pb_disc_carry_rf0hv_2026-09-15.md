# CC-arm — `pb_disc_carry` on RF-0hv, both beds: the carry is EXACT and it kills the ramp, but it FAILS two of the five bars because a reset the wire deserved was also dumping error (2026-09-15)

**STATUS:** open — **one config-only knob armed on both beds, two runs spent, no code built, no golden and no ledger touched, nothing promoted.**
CST §7.1 pre-registered the cheapest experiment the card ladder had: `network.closed_loop.pb_disc_carry: true` on the RF-0hv working posture,
no new code at all.  It was run on both beds.  **The mechanism does exactly what card CC said it does and the wire never moves**: FIELD
`events 390 → pb_disc_carried 390 / resets 0, closure HOLDS`, the per-cell trace goes `{none 11228, reset_disc 390}` → `{none 11228,
carry_disc 390}` on the same 11 618 rows, and `products.txt` / `network_epoch_status.csv` / `network_dd_fractional.csv` are byte-identical to
the baseline on BOTH beds.  **CST's WINDOW B is eliminated at the mechanism**: the ambiguity↔slant-iono runaway that produced the hour-6 ramp
falls from max cumulative **|Δamb| 9.710 → 0.770 m** over the pre-registered 34-epoch window (below even CST's quiet control at 0.782 m),
`|Δiono| 3.326 → 0.850 m`, and the ramp window's per-5-min p50 goes from a monotone climb `1.30 3.03 4.77 5.57 5.19 4.73` to a monotone
*decline* `2.08 2.06 2.04 2.00 1.93 1.89` — **max-rise +4.265 → −0.016 m (C2 PASS)**.  Hour 6 goes **3.65 → 1.92 m** and hour 3 **0.97 →
0.38 m**.  **But two bars fail.**  **C1**: the event-window p50 is **1.404 m** against a **≤ 1.0 m** bar (baseline 3.561, so 2.5× better and
0.40 m short) — and the residual is the cell CST §7.3 already named, **G32 L1W's +1.409 m UNDECLARED step**, which a carry structurally cannot
reach because no declaration arrives.  **C3**: hours 4 and 5 are **+0.21 / +0.27 m worse** (1.26 / 0.72 against 1.05 / 0.45), and the cause is
specific and uncomfortable — at TOW 271050 / 271200 the baseline's 17- and 18-cell mass resets were **therapeutic** (CST's own impact table
scored them −1.173 / −1.382 m), i.e. the reset was dumping an accumulated ambiguity error the faithful carry now preserves.  **Verdict: the
knob does NOT join RF-0hv on the literal bar set (C1, C3 FAIL; C0, C2, C4 PASS; C5 PASS on availability and WL, deltas reported).**  It is not
refuted as a mechanism — it is CST §7.3's stated prerequisite, and the next arm is §7.1 + §7.3 together, which is the only pairing that can
reach C1's residual.

## 1. The knob, and what `:11597` actually does

`network.closed_loop.pb_disc_carry` (bool, default false), card `docs/cards/consumer/CC-declared-step-carry.md`.

- **`src/core/config/GnssConfigLoadNetwork.cpp:11319`** — `clc.pb_disc_carry = cl.value("pb_disc_carry", false);` inside the
  `network.closed_loop` section.  The comment block above it states the contract: both card-CC knobs "default false ⇒ byte-identical; neither
  reaches the producer's solve or its resume fingerprint."
- **`src/core/config/GnssConfigLoadNetwork.cpp:11597`** — `if (cl.pb_disc_carry) c.ssr_phase_bias_disc_carry_ = true;`, inside
  `deriveClosedLoopConsumerConfig`.  **It is not a gate and it is not a warning.**  It is the DERIVATION: the harness knob lands on the
  *derived consumer* config as the engine-level processing key `processing.ssr_phase_bias_disc_carry` (a standalone RT consumer sets that key
  itself).  Its comment adds one asymmetry worth knowing — *"Never cleared here — an explicit `processing.ssr_phase_bias_disc_carry` on the
  base config is kept"*, so the harness knob can only ever turn the carry ON, never off.
- ⚠ **There is NO `gnss_config.log` line for this knob, by design, and that is a real observability gap.**  The block immediately above
  (`:11585-11590`, the `ssr_bias_max_age` arming) states the reason for the whole derivation region: *"(No LogManager here — the effective
  value surfaces via the consumer engine's config echo.)"*  The base-config parse at `GnssConfigLoad.cpp:573-579` DOES log a full
  `SSR phase-bias declared-step CARRY ON (card CC)` paragraph — but only when the key is read from JSON, and the derived consumer config is
  *copied and mutated*, never re-parsed.  So on a closed-loop arm that paragraph never fires.  `gnss_config.log` on both arms carries the
  CST line (`network.trace_consumer_sat ON …`, line 43) and **nothing for card CC**.
  **The arming evidence is therefore the ON-path telemetry only (standing invariant #4), and it is unambiguous:**
  - `logs/rt_engine.log`: 390 (field) / 731 (sim) lines of the form
    `[RT] SSR phase-bias discontinuity: carried G12 L2W by +6.320 m`, and 0 (field) / 4 (sim) `… reset … (reset_unknown_key — carry refused)`.
  - the run-end summary line (`NetworkTeardown.cpp:5656`, printed only when `pbDiscCarryArmed()`), quoted in §4.

**Mechanism** (CST §7.1, unchanged): `carryOnPhaseBiasDisc_` (`RealTimeEngineSsrBreaks.cpp:182`) translates the float ambiguity by `−Δ` where
`Δ` is the change in the value the measurement model APPLIES this epoch, read with the model's own gated read — value-only, covariance
untouched.  Every refusal class (`first_sight` / `basis` / `bssd` / `unknown_key`) falls back to the existing reset and is counted, with
`carried + resets == events` asserted at run end.

## 2. The arms

Exe `build/x64-Release/gnss_processor_ccarm.exe`, a copy of `gnss_processor_cst.exe` **re-hashed after the copy**: sha256
`5d8aa185319293f070c1563d1f185651d99aef48856a76bbab76f54b13666ec0`, **[:16] `5D8AA185319293F0`** — equal to the source, main `1ee5204`, and the
tree carries no code commit after it.  Both arms exit 0, both emit **720 / 720**.

| arm | from | to |
|---|---|---|
| FIELD | `runs/_rfpdfix_field6_both/arm.json` | `runs/_ccarm_field6_both` |
| SIM | `runs/_rfpdfix_sim6_both/arm.json` | `runs/_ccarm_sim6_both` |

Full-file diff of each arm against its source, verified before the launch — **exactly three hunks each, the run path and the two keys**:

```
=== FIELD ===                                  === SIM ===
       },                                             },
+      "pb_disc_carry": true                   +      "pb_disc_carry": true
- "product_output_path": …/_rfpdfix_field6_…   - "product_output_path": …/_rfpdfix_sim6_…
+ "product_output_path": …/_ccarm_field6_…     + "product_output_path": …/_ccarm_sim6_…
     "phase_datum_held_cold_release": true,          "phase_datum_held_cold_release": true,
+    "trace_consumer_sat": true                +    "trace_consumer_sat": true
```

`trace_consumer_sat` is the CST instrument, certified solve-byte-identical by that card; it is the only way to grade C4 per cell.

**Graders.**  `python scripts/cc_arm_grade.py <run> [<run>…] [--bin-s S] [--window TOW_MIN TOW_MAX] [--window-bin-s S]` (new, trace-only: the
per-hour p50/p90 delegated to `refound_grade.consumer_table` so it is the same statistic `rf2d_grade.py` prints, plus the per-bin p50 and the
window read with two drift numbers — `end-start` and `max-rise`, the largest climb any later bin makes over any earlier one, which is the
conservative reading a "no monotone drift" bar needs); `python scripts/cst_consumer_read.py <run> --tow-min … --tow-max … --section pair|drift`;
`python scripts/simnet_amb_integer.py runs/simnet60_quiet_wu93_24h --run <run> --ref-free` (the **ref-free WL(1,2) `round(e)=0` over the last
25 % of epochs** is RF-2b's S3 statistic — it reproduces the cited 99.1 % on the sim baseline exactly; the as-traced number is 96.3 %).

## 3. The bar table — C0..C5 as CST §7.1 pre-registered them

| bar | pre-registered | measured | |
|---|---|---|---|
| **C0** wire identity: `products.txt` sha256 identical on BOTH beds | identical | FIELD `4F513E762F292337` = `4F513E762F292337`; SIM `D9280279A6BBCCD8` = `D9280279A6BBCCD8`.  Stronger: `logs/network_epoch_status.csv` (`939727D60970F6B5` / `E4FC04F44D51F1EE`) and `logs/network_dd_fractional.csv` (`805D8B21F3A7F9C4` / `8E3F6AA14565E52D`) also byte-identical | **PASS** |
| **C1** field consumer p50, TOW 276810–278400 | **≤ 1.0 m** | **1.404 m** (baseline 3.561 m, 2.54×better); window first→last 0.760 → 1.847 m, max 1.847 (baseline 0.383 → 3.731, max 4.012) | **FAIL by 0.404 m** |
| **C2** ramp window TOW 279240–280800, no monotone drift > 1 m | ≤ 1 m | first **2.096** / last **1.882** / max **2.098** m; per-5-min p50 `2.08 2.06 2.04 2.00 1.93 1.89`; **max-rise −0.016 m**, end-start −0.196 m (baseline first 1.081 / last 4.683 / max 5.636, per-5-min `1.30 3.03 4.77 5.57 5.19 4.73`, max-rise **+4.265 m**) | **PASS** |
| **C3** hours 3–5 no worse than 0.97 / 1.05 / 0.45 by > 0.10 m; hour 6 < 3.65 m; 720/720 | — | h3 **0.38** (−0.59 ✓), h4 **1.26** (**+0.21 ✗**), h5 **0.72** (**+0.27 ✗**), h6 **1.92** < 3.65 ✓, **720 / 720** ✓ | **FAIL on h4 and h5** |
| **C4** closure `carried + resets == 390`; resets ≤ 40 | 390; ≤ 40 | `events 390 (disc bumps 390 + freshness flips 0) -> pb_disc_carried 390 / pb_flip_carried 0; resets first_sight 0 / basis 0 / bssd 0 / unknown_key 0; closure HOLDS`.  Trace: `ev_pb {none 11228, carry_disc 390}` vs baseline `{none 11228, reset_disc 390}` on the same 11 618 rows | **PASS** |
| **C5** sim: 720/720; WL ≥ 98 %; consumer p50 within 0.05 m/hour **or the deltas reported if the knob has a population** | — | **720 / 720**; WL ref-free last-25 % **99.1 %** (identical to baseline — the DD trace is byte-identical); population **present**: `events 735 → carried 731, resets 4 (all unknown_key), closure HOLDS`, so the per-hour deltas are REPORTED: **−0.08 / +0.24 / +0.10 / −0.02 / −0.01 / +0.15 m** | **PASS** (availability + WL; deltas reported, 4 of 6 outside ±0.05) |

Two further CST §7.1 sub-bars, measured because they are the mechanism and not the outcome:

| sub-bar | pre-registered | measured | |
|---|---|---|---|
| `amb_var` ratio on a carried cell ≤ 1.1 | ≤ 1.1 | TOW 276810 max **0.976** (baseline **5.557**); TOW 279030 max **0.979** (baseline **3.014**) — the covariance is untouched, exactly as card CC claims | **PASS** |
| max post-reset cumulative \|Δamb\| over the 34-epoch window 279240–280260 | **9.71 → ≤ 2.0 m** | **9.710 → 0.770 m** (CST's matched quiet control was 0.782 m); \|Δiono\| **3.326 → 0.850 m** | **PASS** |

## 4. The run-end telemetry (invariant #4)

```
FIELD  pb-disc carry (card CC)   : events 390 (disc bumps 390 + freshness flips 0) -> pb_disc_carried 390 /
                                   pb_flip_carried 0; pb_disc_reset_first_sight 0 / pb_disc_reset_basis 0 /
                                   pb_disc_reset_bssd 0 / pb_disc_reset_unknown_key 0; closure HOLDS
       per-sat consumer trace    : 11618 rows over 720 consumer epoch(s)
SIM    pb-disc carry (card CC)   : events 735 (disc bumps 735 + freshness flips 0) -> pb_disc_carried 731 /
                                   pb_flip_carried 0; pb_disc_reset_first_sight 0 / pb_disc_reset_basis 0 /
                                   pb_disc_reset_bssd 0 / pb_disc_reset_unknown_key 4; closure HOLDS
       per-sat consumer trace    : 10800 rows over 720 consumer epoch(s)
```

The FIELD number is the one CST predicted to the unit: 390 declared cell-epochs, 390 responses, and the response class is the only thing that
changed.  The four SIM refusals are two satellites at two epochs (`G20 L1C` / `G20 L2W` at 262920, `G12 L1C` / `G12 L2W` at 267720), class
`unknown_key` — a declared bump on a key the consumer has no ambiguity state for, which is the conservative fallback the card specifies.

**The largest carries on the field arm** (raw applied Δ, not de-commoned — the model applies the raw bias, so the translation must use the raw
change): at the WINDOW-A epoch **TOW 276810**, `G12 L2W +6.320`, `G12 L1W +4.197`, `G25 L2W +3.088`, `G25 L1W +1.925`, `G24 L1W +1.642`,
`G06 L2W +1.485`, `G06 L1W +1.260`, `G24 L2W +1.154` m — the run's largest real phase-bias re-realization, absorbed as eight translations with
the arcs intact.  Elsewhere only the ignition epochs 259410/259440 exceed 1 m.

## 5. Both beds, per hour and per 30 minutes

### 5.1 FIELD — `_rfpdfix_field6_both` (baseline) vs `_ccarm_field6_both` (arm)

| consumer p50 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| baseline | 5.02 | 2.11 | **0.97** | **1.05** | **0.45** | 3.65 |
| **arm (`pb_disc_carry`)** | **3.74** | **1.89** | **0.38** | 1.26 | 0.72 | **1.92** |
| *Δ (arm − baseline)* | *−1.28* | *−0.22* | *−0.59* | ***+0.21*** | ***+0.27*** | *−1.73* |

| consumer p90 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| baseline | 5.46 | 3.61 | 1.27 | 2.39 | 0.94 | 5.35 |
| arm | 4.77 | 3.79 | 0.63 | 1.92 | 0.85 | 2.08 |

| consumer p50 per 30 min (m) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline | 3.76 | 5.10 | 2.31 | 1.89 | 1.01 | 0.91 | 2.02 | 0.97 | **0.44** | **0.45** | 3.56 | 4.31 |
| **arm** | 3.37 | 4.19 | 2.91 | **1.24** | **0.49** | **0.36** | **1.54** | 1.10 | 0.79 | 0.71 | **1.74** | **2.04** |

The 30-minute column names the shape again.  The arm is better in 8 of 12 bins; of the four it loses (3, 8, 9, 10) the two that matter — 9 and
10, the hour-5 pair — are the baseline's *dip*, not the arm's excursion, and §6.2 says where that dip comes from.  The 10-minute view makes the
crossing exact, bins 13–21 (baseline / arm): `1.19 1.01 0.87 0.60 1.14 0.92 2.00 2.41 1.24` against
`0.61 0.49 0.30 0.25 0.37 0.44 1.15 1.70 1.91`.

### 5.2 SIM — `_rfpdfix_sim6_both` (baseline) vs `_ccarm_sim6_both` (arm)

| consumer p50 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| baseline | 1.81 | 0.79 | 0.40 | 0.26 | 0.19 | 0.46 |
| arm | 1.73 | 1.03 | 0.50 | 0.24 | 0.18 | 0.61 |
| *Δ* | *−0.08* | *+0.24* | *+0.10* | *−0.02* | *−0.01* | *+0.15* |

| consumer p90 by hour (m) | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| baseline | 5.41 | 1.23 | 0.48 | 0.30 | 0.62 | 0.53 |
| arm | 4.57 | 1.57 | 0.68 | 0.34 | 0.32 | 0.67 |

| consumer p50 per 30 min (m) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline | 4.46 | 1.36 | 1.15 | 0.56 | 0.44 | 0.34 | 0.26 | 0.26 | 0.17 | 0.47 | 0.42 | 0.48 |
| arm | **1.72** | 1.74 | 1.32 | 0.88 | 0.63 | 0.36 | 0.30 | **0.15** | **0.14** | **0.23** | 0.61 | 0.60 |

The sim is a quiet bed with a converging consumer at 0.2–0.6 m; the knob moves it by ±0.25 m in both directions and the two arms end within
0.15 m of each other.  The bed's own ignition bin improves 4.46 → 1.72 m.  Nothing here discriminates; C5's purpose is the non-regression and
it holds (availability, WL, and the closure).

## 6. The residual excursions — what the carry cannot reach, and what it should not have preserved

### 6.1 C1's residual is the UNDECLARED cell, i.e. CST §7.3 (ordering constraint confirmed from the other side)

The event window improves 3.561 → 1.404 m and the *shape* of the failure changes completely: the baseline steps 0.383 → 3.731 m and stays
there; the arm walks 0.760 → 1.847 m.  The eleven declared cells were carried exactly (§4), so what is left is the cell CST §4 already isolated:
**`G32 L1W` took +1.409 m into a live ambiguity with NO declaration**, because `NetworkProductEmitter.cpp:408-409` thresholds the RAW emitted
value while the consumer sees the de-commoned one, and the epoch's −1.45 m L1W frame move hid the step under a 0.0476 m bar.  A carry is a
response to a declaration; there is no declaration, so `pb_disc_carry` is structurally blind to it.  **C1 cannot be cleared by §7.1 alone** —
that is exactly CST §7.3's stated ordering, now measured from the consumer side rather than predicted.

### 6.2 C3's regression is a reset that was DUMPING error — the uncomfortable half

At **TOW 271050 (17 cells)** and **TOW 271200 (18 cells)** the baseline took two back-to-back mass resets that CST's own impact table scored
as **gains**: cost −1.173 m and −1.382 m.  Those are real declarations (max |ΔPB| 0.231 / 0.006 m), and the arm carries all 35 cells.  Matched
5-minute p50 across the pair:

| TOW window | 270000 | 270300 | 270600 | 270900 | 271200 | 271500 | 271800 | 272100 |
|---|---|---|---|---|---|---|---|---|
| baseline | 1.57 | 2.24 | 2.47 | 2.34 | 1.72 | **0.96** | **0.91** | **0.98** |
| arm | **0.95** | **1.29** | **1.55** | **1.78** | 1.91 | 1.69 | 1.31 | 1.27 |

The arm leads by 0.5–0.9 m for eighteen minutes and then the baseline's reset **discards the accumulated ambiguity error in one epoch** and
lands below it for the rest of hours 4–5.  This is the mirror image of CST's §7.2 finding: 30 % of declarations are EMPTY and their reset is
pure loss (which the carry recovers, §6.3), but this pair is the opposite — a REAL declaration whose reset was also, accidentally, a cure for
a drift the wire had already accumulated.  **A faithful translation preserves a level the network has already walked away from**, and nothing
in the consumer re-validates it.  That is standing invariant #7's re-coupling edge with the sign reversed: the carry closes the decoupling edge
(the arc survives) and thereby inherits the obligation to re-validate what it reconnects, which it does not do.  Naming it is this card's
finding; fixing it is not in scope and is NOT a tuning job.

### 6.3 What the carry does reach, and the ramp is the proof

CST §5 measured the hour-6 ramp as a post-reset re-convergence along the ambiguity↔slant-iono null, triggered by an EMPTY declaration (a
20-satellite Q3 decouple for ≤ 0.011 m of emitted motion).  Cumulative de-commoned motion over the pre-registered 34-epoch window, per cell,
baseline → arm:

| sat/sig | cum Δamb baseline | cum Δamb arm | cum Δiono baseline | cum Δiono arm |
|---|---|---|---|---|
| G06 L2W | **+9.700** | +0.122 | +3.326 | +0.150 |
| G06 L1W | +7.683 | +0.101 | +3.326 | +0.150 |
| G11 L2W | +7.448 | **+0.770** | +2.013 | −0.287 |
| G11 L1W | +5.849 | +0.621 | +2.013 | −0.287 |
| G29 L2W | −4.402 | +0.573 | −2.234 | −0.656 |
| G32 L2W | −4.314 | −0.465 | −0.954 | +0.347 |
| G25 L2W | −3.964 | −0.272 | −1.293 | −0.208 |
| G19 L2W | +3.154 | −0.001 | +1.356 | +0.351 |
| **max \|·\|** | **9.710** | **0.770** | **3.326** | **0.850** |

The wire column is untouched by construction (`cum ΔPB` is identical cell-for-cell between the two arms — C0).  The consumer's own states stop
running away, and the position follows: hour 6 **3.65 → 1.92 m**, the ramp's monotone climb replaced by a monotone decline.

### 6.4 The ignition edge, reported because it is a real ON-path cost

At **TOW 259230 — the SECOND consumer epoch** — the arm reads **27.82 m** against the baseline's 4.12 m (and 13.21 / 7.51 / 7.25 at the next
three epochs, against 3.20 / 3.00 / 2.87).  `pb_disc_reset_first_sight` is **0**, so no refusal fired: the consumer HAD a recorded applied value
— from the producer's very first emitted epoch — and the carry translated faithfully against it.  The carry is exact with respect to the wire,
so at ignition it transmits the wire's own ignition transient straight into the ambiguity, where the reset used to launder it.  It self-clears
within four epochs and hour 1 still ends **better** (3.74 vs 5.02 m), but on a shorter lineage it would not, and card CC's refusal ladder has no
class for "the recorded value is younger than the wire's convergence".

## 7. Verdict

**`network.closed_loop.pb_disc_carry` does NOT join the RF-0hv working posture on the pre-registered bar set: C1 and C3 FAIL.**  It is not
refused as a mechanism and nothing about it is tuned or tunable — it is exact, it is closed, and the wire is byte-identical on both beds.  What
the run establishes:

1. **The build is correct and the telemetry is honest.**  390 → 390 with zero refusals, closure HOLDS, covariance untouched (ratio ≤ 0.979
   where the reset path re-inflated 2.75–5.56×), and every number is a counter rather than an absence of crashes.
2. **CST's WINDOW-B verdict is confirmed causally.**  Removing the reset removes the ramp — max cumulative |Δamb| 9.710 → 0.770 m and a
   position that declines instead of climbing.  This is the strongest evidence yet that the hour-6 excursion was never a producer channel.
3. **CST's §7.3 ordering is confirmed from the consumer side.**  C1's 0.40 m residual is the undeclared `G32 L1W` step; no carry can respond to
   a declaration that is never made.  **The next arm is §7.1 + §7.3 on ONE config** (`disc_jump_test_decommon` with `pb_disc_carry`), which is
   the pairing §7.3 itself requires — it ADDS declarations, and every added declaration costs a translation instead of an arc.
4. **A new obligation this card creates.**  §6.2 shows that some real resets were dumping accumulated error.  Any future promotion of the carry
   must carry a re-validation at the re-coupling edge (invariant #7) — a translated ambiguity is *continuous*, not *correct*.  There is no
   instrument for that today; `amb_m` / `amb_var` in `closed_loop_sat_trace.csv` can host one with no new producer code.

**Nothing promoted.**  `config/golden/`, `golden_manifest.json`, `docs/CONFIG_promotion_ledger.{md,json}`, every loader default and both
`config/refound/*_rf0hv.json` are untouched; the posture decision is the author's.

## 8. WHAT STAYS OPEN

1. **The §7.1 + §7.3 pair has not been run** and is the card this one hands off.  §7.2 (`disc_mark_scope`) belongs with EV6 §8.1 on a third
   arm; the 117 empty declarations are still 30 % of the population, now costing a translation instead of an arc — cheaper, not free.
2. **The re-coupling obligation of §6.2 is unbuilt and uninstrumented.**  Nothing re-validates a carried ambiguity against the wire it was
   translated onto.
3. **The ignition edge of §6.4 has no refusal class.**  `first_sight` fires on the absence of a recorded value, not on the youth of one.
4. **Card CC has no `gnss_config.log` echo on a closed-loop arm** (§1).  Every other armed knob in this posture logs a paragraph; this one is
   visible only in `rt_engine.log` and the run-end summary.  That is a deliberate consequence of the derived-config design, and it makes an
   armed/unarmed mistake silent at the top of a run.
5. **The sim bed does not discriminate** (§5.2) and its 4 `unknown_key` refusals are unexplained — two satellites, two epochs, a declared bump
   on a key with no ambiguity state.

## 9. RUNS AND ARTEFACTS

- `runs/_ccarm_field6_both` — the FIELD arm, exit 0, 720/720, `products.txt` `4F513E762F292337` (= baseline), `logs/closed_loop_sat_trace.csv`
  11 618 rows, `logs/rt_engine.log` 390 carry lines.
- `runs/_ccarm_sim6_both` — the SIM arm, exit 0, 720/720, `products.txt` `D9280279A6BBCCD8` (= baseline), trace 10 800 rows, 731 carry lines.
- `runs/_rfpdfix_field6_both` / `runs/_rfpdfix_sim6_both` — the RF-0hv baselines, read only, not re-run, left in place.
- `runs/_cst_field6_both` — CST's instrumented twin of the field baseline; the `ev_pb` census in §3/C4 is read against it (the baseline itself
  carries no per-satellite trace).
- New instrument: `scripts/cc_arm_grade.py` (trace-only).
- Exe `build/x64-Release/gnss_processor_ccarm.exe` `5D8AA185319293F0`.
- `config/golden/`, `golden_manifest.json`, `docs/CONFIG_promotion_ledger.{md,json}`, `config/refound/*_rf0hv.json` and every loader default
  untouched.  No code change in this card.
