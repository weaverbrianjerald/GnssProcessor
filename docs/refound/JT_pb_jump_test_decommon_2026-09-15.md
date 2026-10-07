# JT: the de-commoned phase-bias jump test on the carry arm (2026-09-15)

**STATUS**

- **id:** JT
- **topic:** wire-contract
- **status:** open
- **summary:** The de-commoned jump test declares the frame-hidden G32 L1W step (TOW 276810 now 12/12) but the consumer is worse on every graded bar; the knob does NOT join RF-0hv.
- **dates in the text:** 2026-09-15
- **knobs / posture:** `network.disc_jump_test_decommon` (bool, default OFF, emit-side only; per-signal median, `kDiscDecommonMinSats = 3`; S-line token `discdecom=1`), graded on the CC-arm lineage with `pb_disc_carry`; `serialize_disc_counters` and `serialize_sbasis_contract` off in RF-0hv
- **hashes named in the text:** main `a60a016` + this card's diff; exe `gnss_processor_jt.exe` `6E1021DBC554840A`; OFF smoke raw `1ACC941CC853ECB2`, data stream `333DCB0871E1F5BC` (canonical raw `9ADD1D3AB9D70657`); Debug suite 2235 / 295 (2234 passed, 1 skipped); gtest `PbJumpDecommonTest` (6 cases)
- **run dirs named in the text:** `runs/_jt_smoke_off`, `runs/_jt_field6_both`, `runs/_jt_sim6_both`; baselines `runs/_ccarm_field6_both`, `runs/_ccarm_sim6_both`, `runs/_rf2d_smoke_off`, `runs/_cst_smoke_off`
- **source:** [CST](CST_consumer_sat_trace_2026-09-15.md) section 7.3 (the pre-registration); [CC-arm](CC-arm_pb_disc_carry_rf0hv_2026-09-15.md) (the carry arm it runs on)

**Outcome.** Question: does de-commoning each phase-bias step against its signal's network median declare frame-hidden steps without harming the consumer. With the solve byte-identical, the field event window reaches 3.323 m against a 1.0 m bar. Two causes: in ignition the median is convergence, not a frame, and the network's median differs from the consumer's sky median (0.654 m at TOW 276810). J1 (data stream), J2 and J3a PASS; J3b, J4a, J5 and J6 FAIL. [PBT](PBT_consumer_pb_translate_2026-09-15.md) later reached the cell from the consumer with the wire untouched; section 7's fixes are unbuilt.

**Status as recorded (2026-09-15):** open — **one producer knob BUILT (`network.disc_jump_test_decommon`, bool, default OFF), two field/sim runs spent on the carry arm, no golden and no ledger touched, nothing promoted.**
CST §7.3 pre-registered this fix and named its ordering constraint: the emitter's phase-bias jump test screens the RAW epoch-to-epoch ΔPB while
a consumer's receiver phase clock absorbs the satellite-common part, so at TOW 276810 a **−1.451 m L1W frame move hid G32 L1W's real +1.409 m
step under a 0.0476 m bar** and it landed undeclared in a live ambiguity — and because every added declaration costs a reset-only consumer an
arc, it could only be graded on an arm already carrying `pb_disc_carry` (card CC-arm).  Both were armed, both beds.  **The mechanism does
exactly what it was built to do.**  The solve is byte-identical on both beds (`network_epoch_status.csv`, `network_dd_fractional.csv` and
`products.txt` with the `R` records and the one legend line removed all match the carry arm hash-for-hash), the four declaration classes are
non-zero and counted (FIELD `decl_decommon_only 169 / decl_both 1270 / raw_only_suppressed 322 / fallback_raw 0`), **TOW 276810 goes 11/12 →
12/12 declared with G32 L1W now reading `carry_disc` instead of nothing**, and the run-wide undeclared population's max de-commoned |ΔPB| at
the consumer falls **1.4089 → 0.1074 m** (p90 0.0052 → 0.0053; the 10 cells above 0.10 m become 3, all of them 0.103–0.107 m).  **And the
consumer is worse on every graded bar.**  FIELD hours 3–6 go **0.38 / 1.26 / 0.72 / 1.92 → 0.67 / 2.01 / 2.22 / 3.91 m**, the event window
**1.404 → 3.323 m** against a ≤ 1.0 m bar, and hour 1 **3.74 → 6.65 m** (p90 4.77 → 19.47).  The SIM localises the cause to the metre: **100 %
of both discriminating classes (280 `decl_decommon_only`, 380 `raw_only_suppressed`) fall in HOUR 1**, hours 2–6 carry ZERO of either, and the
sim consumer moves **+3.61 m at hour 1 and −0.01 … +0.02 m at hours 2–6** — the knob's entire effect and its entire cost are inside ignition,
where the per-signal median ΔPB is **0.134 m (p90 0.822, max 1.785)** and is not a frame at all but thirty satellites converging together.  The
settled-hour damage has a second, structural cause the card can state exactly: **the emitter de-commons against the median over the 20–32
satellites it EMITS on a signal, while the consumer's receiver clock absorbs the median over the 6–10 satellites it TRACKS** — at 276810 those
are −0.797 m and −1.451 m, a 0.65 m disagreement, 13× the bar.  **Verdict: J1/J2/J3-cells PASS, J3-max/J4/J5/J6 FAIL; the knob does NOT join
RF-0hv and nothing is promoted.**  What it is not is refuted as a *contract*: it is the only thing that has ever reached the G32 L1W cell, and
what the measurement rejects is de-commoning against a NETWORK median, not de-commoning as such.

## 1. The fix as built

`network.disc_jump_test_decommon` (bool, default false).  Emit-side only: it changes which phase-bias discontinuity COUNTERS step, never an
estimate, never a datum, never a row of the normal equations.

| piece | file:line |
|---|---|
| the per-SIGNAL common mode (pre-pass, built against the baselines the emit loop is about to overwrite) | `src/core/products/NetworkProductEmitter.cpp:262-297` |
| the test itself (replaces the raw `std::abs(npb.bias_m - last_it->second) > discThresholdM(...)`) | `src/core/products/NetworkProductEmitter.cpp:472-508` |
| median helper (sort-in-place; even population takes the mean of the two central values) | `src/core/products/NetworkProductEmitter.cpp:69` `medianInPlace` |
| ctor parameter + member + `kDiscDecommonMinSats = 3` + the event/class types | `include/products/NetworkProductEmitter.hpp:215-274` (ctor `:255`, `kDiscDecommonMinSats` `:241`), members `:438`/`:443` |
| knob + Doxygen | `include/config/GnssConfigTypes.hpp:2412` (`disc_jump_test_decommon`; the Doxygen block opens at `:2371`) |
| parse + the armed WARNING (the §7.3 ordering constraint, in the log) | `src/core/config/GnssConfigLoadNetwork.cpp:2357-2380` |
| emitter construction | `src/core/app/ApplicationNetworkSetup.cpp:1102` (contract field `:1228`) |
| S-contract field + the `discdecom=` token | `include/products/NetworkProductIO.hpp:147`; writer `src/core/products/NetworkProductIO.cpp:535-541`; parser `:330-331`; legend `:628`; `checkContract` `:966-971`; `makeReplayEmitter` `:1018` |
| warm-resume fingerprint (conditional append) | `src/core/app/NetworkRunContext.cpp:953-961` (the `discdecom=1` append at `:961`) |
| events + the four run-end counters | `src/core/filter/NetworkEmitStore.cpp:432-462` |
| counters | `include/app/NetworkRunContext.hpp:3057-3067` (`n_jt_decommon_only` / `n_jt_both` / `n_jt_raw_only` / `n_jt_fallback_raw`) |
| run-end readout (run summary) | `src/core/filter/NetworkTeardown.cpp:5655-5670` |
| run-manifest echo | `src/core/logging/NetworkRunTraces.cpp:1570-1575` (the echo at `:1574`) |
| gtest, 6 cases, `PbJumpDecommonTest` | `tests/test_pb_jump_decommon.cpp`; CMake entry `CMakeLists.txt:384` |

**The test.**  For signal `s` at epoch `k`, over the (sat, s) keys carrying a baseline that is not stale under the staleness horizon — exactly
the jump test's own population, one epoch earlier — form `c_s = median(ΔPB)`.  Declare `(sat, s)` iff `|ΔPB(sat,s) − c_s| > discThresholdM(sat,s)`,
the SAME 0.25·λ bar.  Below `kDiscDecommonMinSats = 3` the signal's cells fall back to the raw test and are counted (`fallback_raw`; measured
**0** on both beds — the per-signal population ran 19–32).  The median is taken WITHIN each signal, never across a mixed L1/L2 population
(CST §2.4's rule): L1W and L2W biases do not share a common mode, and one mixed median leaks a band-differential move into every cell.  A cell
already FORCED by `markDatumChange` / `markSatDiscontinuity` is declared regardless and is deliberately NOT classified — the census then counts
only the declarations this knob is responsible for.  Everything else in the emitter is untouched: the forced-bump paths, the code-bias and
clock channels, the staleness first-sighting suppression, the serialized emitter state.

**Invariant #7, both edges, with identities.**  Every cell on which the two tests disagree writes a `network_events.csv` row, tier `PB_JUMP`:

```
2313,276810.000,PB_JUMP,decl_decommon_only,G32,,0.755329,0.047573,declare,
    sig=L1W raw_dpb=-0.042085 c_s=-0.797414 decommon_dpb=0.755329 bar=0.047573 n_med=22
```

`decl_decommon_only` is the DECOUPLING edge (a declaration that exists only because of this knob — the class the raw test MISSED);
`raw_only_suppressed` is the RE-COUPLING edge (a declaration this knob WITHHOLDS, on the claim that the consumer's receiver phase clock absorbs
it by construction — the class the raw test MINTED); `decl_both` and `fallback_raw` complete the partition.  Each row carries sat, signal, tow,
raw ΔPB, `c_s`, the de-commoned ΔPB, the bar and the median's population, and each class is a run-end counter printed in the run summary.

**The S line.**  `,discdecom=1` is appended when armed, after `sat_gf_datum=`, and never written when off.  It is a **replay-emitter parameter**,
not an interpretation hint: `makeReplayEmitter` applies it and `checkContract` compares it, because a replay with the other setting recomputes
a different counter population on exactly the cells a frame move hid or minted.  ⚠ **On this lineage there is no S line at all** —
`network.serialize_sbasis_contract` is off in the RF-0hv posture, so `products.txt` is a **v1** header.  The declaration is exercised and pinned
by `PbJumpDecommonTest.SLineDeclaresTheKnobAndOffIsByteIdentical`, and it is inert on these two runs.

## 2. Hashes, exe, and the identity gates

Exe `build/x64-Release/gnss_processor_jt.exe`, sha256
`6e1021dbc554840aa319d6cff1533640054723ae462b812650af4af6105a1a75`, **[:16] `6E1021DBC554840A`** (main `a60a016` + this card's diff).
Debug `gnss_tests.exe`: `2235 tests from 295 test suites ran.`, `[  PASSED  ] 2234 tests.`, `[  SKIPPED ] 1 test` —
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the standing fixture-gated skip.  That is the pre-JT 2228 + this card's 6 new cases.

**J1 — the OFF smoke, and the one deviation in this card.**  `runs/_jt_smoke_off` (= `runs/_rf2d_smoke_off/smoke.json` with the two run paths
repointed and nothing else — verified by a sorted-JSON diff): `products.txt` sha256[:16] **`1ACC941CC853ECB2`** against the canonical
**`9ADD1D3AB9D70657`**.  **The two files differ on exactly ONE line, and it is a `#` comment:** the S-line LEGEND, +14 bytes, `[,discdecom=1]`.
Every data line is identical, and so is the data stream of the two earlier smokes:

| run | `products.txt` raw | with `^#` stripped |
|---|---|---|
| `runs/_rf2d_smoke_off` | `9ADD1D3AB9D70657` | **`333DCB0871E1F5BC`** |
| `runs/_cst_smoke_off` | `9ADD1D3AB9D70657` | **`333DCB0871E1F5BC`** |
| `runs/_jt_smoke_off` | `1ACC941CC853ECB2` | **`333DCB0871E1F5BC`** |

This is not a discretionary choice.  `NetworkProductIO.cpp:594-618` states the project's own frozen rule in full — *"Extending the wire format
REQUIRES extending the legend — enforced by `HeaderLegendDeclaresEveryEmittedField`"* — and it names this exact consequence: *"DATA lines are
byte-identical across this change — a cross-build SHA-256 of whole product files sees a one-time legend delta (compare with '^#' lines stripped
to check the data stream)."*  The rule was written when the legend froze a lie (a P record carrying a 7th `sigma=` field against a 6-name
legend) and a schema-faithful consumer silently dropped the phase-bias σ.  So the legend is extended, the enforcement test is extended with it
(`tests/test_network_product_io.cpp:960` arms `disc_jump_test_decommon` so the token is actually required and not merely present), and **J1 is
reported as PASS ON THE DATA STREAM with the raw-hash delta stated rather than hidden.**  Every stored whole-file product hash in the project
moves by this one line; no data byte does.

**J2 — solve identity.  PASS on both beds.**

| artefact | FIELD `_ccarm` → `_jt` | SIM `_ccarm` → `_jt` | |
|---|---|---|---|
| `logs/network_epoch_status.csv` | `939727D60970F6B5` = `939727D60970F6B5` | `E4FC04F44D51F1EE` = `E4FC04F44D51F1EE` | IDENTICAL |
| `logs/network_dd_fractional.csv` | `805D8B21F3A7F9C4` = `805D8B21F3A7F9C4` | `8E3F6AA14565E52D` = `8E3F6AA14565E52D` | IDENTICAL |
| `products.txt`, `^R,` and the `# S,` legend removed | `7DDD980EF9A17AE2` = `7DDD980EF9A17AE2` | `3FAC9863A352CAD9` = `3FAC9863A352CAD9` | IDENTICAL |
| `products.txt` RAW | `4F513E762F292337` → `3266C11EDED24A3C` | `D9280279A6BBCCD8` → `204CF06C3F24492B` | legend line only |
| `R` records | 36 → 36 | 10 → 10 | IDENTICAL |

A full-file `diff` of each pair returns **exactly one changed line, the legend** — so on this lineage the arm's whole wire is byte-identical to
the carry arm's including every `R` record.  `rf2d_grade.py` reads the same conclusion from the other side: IF dispersion p50/p90/max
`0.055 / 0.201 / 0.68` and the same three worst satellites on both arms, declarations `continuous marked 0 / unmarked 139; cold marked 12 /
unmarked 0; CLK_STEP 0; cta_decouple 189` on both, and the held-basis handover table bit-identical (`none (null)` 22107 rows, p50 0.001,
max 1.35, 6 above 1 m).

⚠ **A consequence worth stating: `products.txt` cannot carry this fix on this lineage, and neither can EV6's instrument.**
`network.serialize_disc_counters` is off, so the C/B/P records carry no counter fields and the `R` records are the file's only declaration
channel — which this knob does not touch (it changes the JUMP TEST, not the forced marks).  `scripts/ev6_event_read.py --section undeclared
--step-m 0.10` therefore returns the **identical** ledger on both arms (`545 steps > 0.1 m; UNDECLARED 451 (83 %); 86 on a LOOSE satellite`).
That is not a null result, it is a BLIND instrument: EV6 reads the file, the fix lives in the in-process store, and the correct instrument is
CST's consumer trace, which reads the counters as the consumer's own gated reads return them.  This is the same serialization gap CST §2.3
item 2 named for `emit_clock_disc`, now shown to bite a second channel.

## 3. The class census — the four classes, both beds

Run-end counters (invariant #4), printed by `NetworkTeardown.cpp:5657`:

```
FIELD  pb jump de-common (JT)    : decl_decommon_only 169 (the raw test MISSED these) / decl_both 1270 /
                                   raw_only_suppressed 322 (the raw test MINTED these) / fallback_raw 0
                                   (signal population < 3); declared by this test 1439
SIM    pb jump de-common (JT)    : decl_decommon_only 280 / decl_both 2240 / raw_only_suppressed 380 /
                                   fallback_raw 0; declared by this test 2520
```

**By hour, and this is the finding.**

| bed | class | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| FIELD | `decl_decommon_only` | 110 | 0 | 14 | 2 | 3 | 40 |
| FIELD | `raw_only_suppressed` | 130 | 6 | 41 | 3 | 11 | 131 |
| FIELD | `decl_both` | 987 | 53 | 53 | 22 | 57 | 98 |
| **SIM** | `decl_decommon_only` | **280** | **0** | **0** | **0** | **0** | **0** |
| **SIM** | `raw_only_suppressed` | **380** | **0** | **0** | **0** | **0** | **0** |
| SIM | `decl_both` | 2153 | 29 | 8 | 7 | 6 | 37 |

On the SIM the two discriminating classes are **entirely inside ignition**.  From hour 2 the knob changes nothing at all — and the sim
consumer's hours 2–6 move by −0.01 … +0.02 m while its hour 1 moves **+3.61 m**.  The knob's whole effect and its whole cost are the same hour.

**Why ignition, exactly.**  The de-commoned step is only a frame correction if `c_s` IS a frame.  Measured per hour (`|c_s|` over the classified
cells, with the median's population beside it):

| bed | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| FIELD `|c_s|` p50 / max (m) | 0.0222 / 0.108 | 0.0079 / 0.054 | 0.0438 / 0.103 | 0.0015 / 0.032 | **0.4759 / 0.797** | **0.1309 / 0.476** |
| SIM `|c_s|` p50 / max (m) | **0.1343 / 1.785** (p90 0.822) | 0.0015 / 0.003 | 0.0006 / 0.001 | 0.0011 / 0.001 | 0.0005 / 0.001 | 0.0002 / 0.001 |
| `n_med` p50 (both beds) | 31–32 | 32 | 31 | 32 | 31 | 30–31 |

On the sim, hour 1's median ΔPB is **0.134 m with a 1.785 m tail**, and from hour 2 it is under 3 mm.  A 0.134 m "common mode" across 31
satellites is not a datum re-realization — it is thirty-one independent phase biases converging in the same direction out of a cold start.
Subtracting it removes real per-satellite convergence from every cell, and the test then mis-fires in both directions at once (280 minted,
380 withheld).  The population is healthy throughout (`fallback_raw = 0`, `n_med` 19–32), so `kDiscDecommonMinSats` is not the guard this needs:
**the guard this needs is a frame-vs-convergence discriminator, not a population-size one.**

**The second cause, structural, and it survives ignition.**  The emitter de-commons against the median over the satellites it EMITS on a signal;
the consumer's receiver phase clock absorbs the median over the satellites it TRACKS.  At TOW 276810:

| signal | emitter `c_s` (network, `n_med`) | consumer-sky median (CST §4, 6 satellites) | gap |
|---|---|---|---|
| L1W | **−0.7974 m** (22 sats) | **−1.451 m** | **0.654 m = 13.7× the 0.0476 m bar** |
| L2W | −0.4759 m (31 sats) | −1.320 m | 0.844 m = 13.8× the 0.0611 m bar |

The two frames agree in sign and rank and disagree in value by more than an order of magnitude times the bar.  A declaration the emitter
withholds because the network moved together is still a real step at a consumer whose 6–10-satellite sky moved differently — which is what
`raw_only_suppressed` costs, and it is why the settled hours degrade on the field (41 + 3 + 11 + 131 suppressions in hours 3–6) even though the
sim's settled hours, where `|c_s| < 3 mm`, are untouched.

**At the consumer** (the declaration sets differenced cell-by-cell between the two field traces, 11 618 rows each):

| | value |
|---|---|
| declared cell-epochs, carry arm → JT arm | **390 → 372** |
| ADDED by the de-common | **40** (h1 28, h3 5, h5 1, h6 6) |
| REMOVED by the de-common | **58** (h1 22, h2 3, h3 10, h5 2, h6 21) |
| unchanged | 332 |
| EMPTY declarations (\|de-commoned Δ\| < 0.020 m) | 117 (30.0 %) → **94 (25.3 %)** |
| card-CC closure | `events 372 → pb_disc_carried 372 / resets 0; closure HOLDS` |

The sim's carry closure holds too: `events 735 → 705`, `carried 701 / reset_unknown_key 4`.

## 4. TOW 276810 — the cell the card was built for

`cst_consumer_read.py --section steps --step-m 0.0` over 276780 → 276810, JT arm.  **All twelve cells declared (11/12 → 12/12), and G32 L1W is
`carry_disc`:**

| sat | sig | de-commoned ΔPB at the consumer | declared, carry arm | declared, JT arm | consumer event (JT) |
|---|---|---|---|---|---|
| G12 | L2W | −5.000 | YES | YES | `carry_disc; reinflate_disc; reinflate_isb` |
| G12 | L1W | −2.746 | YES | YES | `carry_disc; reinflate_disc; reinflate_isb` |
| G25 | L2W | −1.769 | YES | YES | `carry_disc` |
| **G32** | **L1W** | **+1.409** | **NO** | **YES** | **`carry_disc`** |
| G32 | L2W | +0.976 | YES | YES | `carry_disc` |
| G19 | L1W | +0.587 | YES | YES | `carry_disc; reinflate_disc` |
| G19 | L2W | +0.487 | YES | YES | `carry_disc` |
| G25 | L1W | −0.474 | YES | YES | `carry_disc` |
| G06 | L1W | +0.191 | YES | YES | `carry_disc` |
| G24 | L1W | −0.191 | YES | YES | `carry_disc; reinflate_disc; reinflate_isb` |
| G06 | L2W | −0.165 | YES | YES | `carry_disc` |
| G24 | L2W | +0.165 | YES | YES | `carry_disc; reinflate_disc; reinflate_isb` |

The emitter's own arithmetic for the new cell: raw ΔPB **−0.042085 m** (under the 0.047573 m bar — what the frame-blind test saw), network
`c_s` **−0.797414 m** over 22 satellites, de-commoned **+0.755329 m**, 15.9× the bar ⇒ `decl_decommon_only`.  **CST §7.3's RE-COUPLING
requirement is met on this epoch**: `n_disc_frame_hidden` (G32 L1W, and G30 L1W beside it) and `n_disc_frame_only` (G03, G04, …) are BOTH
non-zero at 276810, so the mechanism is the one the card claimed and not an accident of threshold.

The **run-wide undeclared population** (CST §3's census instrument), which is the quantity J3 bars:

| | carry arm | JT arm |
|---|---|---|
| UNDECLARED cell-epochs | 11 188 | 11 206 |
| \|de-commoned ΔPB\| p50 / p90 / **max** | 0.0007 / 0.0052 / **1.4089 m** | 0.0007 / 0.0053 / **0.1074 m** |
| cells above 0.10 m | **10** (max 1.409; G32 L1W at 276810, 277560, 278550) | **3** |
| DECLARED \|de-commoned ΔPB\| p50 / p90 / max | 0.0619 / 0.3273 / 5.0001 | 0.0795 / 0.3412 / 5.0001 |

The three survivors are `259230 G14 L1W −0.103`, `259290 G07 L1W +0.107`, `262980 G24 L2W +0.107` — two ignition epochs and one mass-reset
epoch, all within 7 mm of the bar, and all explained by §3's network-vs-sky frame gap rather than by the test failing to fire.

## 5. THE BAR TABLE — J1..J6 as pre-registered

| bar | pre-registered | measured | |
|---|---|---|---|
| **J1** OFF smoke identical | `9add1d3ab9d70657` | **data stream identical** (`^#`-stripped `333DCB0871E1F5BC` on all three smokes); raw `1ACC941CC853ECB2`, one `#` legend line (+14 B) — the frozen rule at `NetworkProductIO.cpp:594-618` | **PASS on the data stream; raw-hash delta stated (§2)** |
| **J2** solve identity: `network_epoch_status.csv` identical; `products.txt` identical after removing `R` + the new-token header line | identical | FIELD `939727D60970F6B5` / `7DDD980EF9A17AE2`; SIM `E4FC04F44D51F1EE` / `3FAC9863A352CAD9`; `network_dd_fractional.csv` identical too; the whole-file diff is ONE legend line | **PASS** |
| **J3a** TOW 276810 sky cells declared | 11/12 → **12/12** | **12/12**, G32 L1W `decl_decommon_only` → `carry_disc` | **PASS** |
| **J3b** run-wide undeclared max \|de-commoned ΔPB\| | 1.409 → **≤ 0.10 m** | **0.1074 m** (10 cells above 0.10 → 3, all 0.103–0.107); p90 0.0052 → **0.0053** (≤ 0.0060 ✓) | **FAIL by 0.0074 m** |
| **J3c** the four class counts + total declared (report only) | — | FIELD 169 / 1270 / 322 / 0, declared-by-this-test 1439; SIM 280 / 2240 / 380 / 0, 2520.  Consumer total **390 → 372** (−4.6 %), +40 / −58 | reported |
| **J4a** field consumer p50 over TOW 276810–278400 | **≤ 1.0 m** | **3.323 m** (carry arm 1.404, pre-carry 3.561); first 2.497 → last 3.612, max 3.614 | **FAIL** |
| **J4b** ramp window 279240–280800, no monotone drift > 1 m | ≤ 1 m | per-5-min p50 `4.04 4.01 4.01 4.03 4.02 4.04`; **max-rise +0.028 m**, end−start −0.003 m (carry arm −0.016 / −0.196) | **PASS** (⚠ flat at a 2× worse LEVEL: 4.02 vs 2.04 m) |
| **J5** field hours 3–6 no worse than 0.38 / 1.26 / 0.72 / 1.92 by > 0.10 m; 720/720 | — | **0.67 / 2.01 / 2.22 / 3.91** = **+0.29 / +0.75 / +1.50 / +1.99**; **720 / 720** ✓.  h4/h5 do NOT return toward the pre-carry 1.05 / 0.45 — they move away from both | **FAIL on all four hours** |
| **J6** sim: 720/720; WL ≥ 98 %; consumer within 0.10 m/hour of the carry arm; classes reported | — | **720 / 720** ✓; WL ref-free last-25 % **99.1 %** (identical — the DD trace is byte-identical) ✓; per-hour deltas **+3.61 / −0.01 / +0.02 / 0.00 / −0.01 / +0.01 m**; classes 280 / 2240 / 380 / 0, all discriminating cells in h1 | **FAIL on hour 1 only** (hours 2–6 PASS with room) |

## 6. The consumer tables, both beds

**FIELD** — `runs/_ccarm_field6_both` (baseline, `pb_disc_carry` alone) vs `runs/_jt_field6_both` (`pb_disc_carry` + `disc_jump_test_decommon`):

| | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| baseline p50 (m) | 3.74 | 1.89 | 0.38 | 1.26 | 0.72 | 1.92 |
| **JT arm p50 (m)** | **6.65** | **1.11** | **0.67** | **2.01** | **2.22** | **3.91** |
| Δ | +2.91 | **−0.78** | +0.29 | +0.75 | +1.50 | +1.99 |
| baseline p90 | 4.77 | 3.79 | 0.63 | 1.92 | 0.85 | 2.08 |
| JT arm p90 | 19.47 | 2.34 | 0.75 | 2.07 | 2.30 | 4.04 |

per-30 min p50 (m), 12 bins:

```
baseline  3.37 4.19 2.91 1.24 0.49 0.36 1.54 1.10 0.79 0.71 1.74 2.04
JT arm   14.53 3.15 1.50 0.94 0.72 0.61 1.73 2.03 2.20 2.23 3.53 4.02
```

Windows: event `[276810, 278400]` p50 **1.404 → 3.323** m (first 0.760 → 2.497, last 1.847 → 3.612, max 1.847 → 3.614); ramp
`[279240, 280800]` p50 **2.039 → 4.022** m, first/last/max 2.096/1.882/2.098 → **3.990/4.040/4.069**, max-rise −0.016 → **+0.028** m.

**SIM** — `runs/_ccarm_sim6_both` vs `runs/_jt_sim6_both`:

| | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| baseline p50 (m) | 1.73 | 1.03 | 0.50 | 0.24 | 0.18 | 0.61 |
| **JT arm p50 (m)** | **5.34** | **1.02** | **0.52** | **0.24** | **0.17** | **0.62** |
| Δ | **+3.61** | −0.01 | +0.02 | 0.00 | −0.01 | +0.01 |

per-30 min p50 (m):

```
baseline  1.72 1.74 1.32 0.88 0.63 0.36 0.30 0.15 0.14 0.23 0.61 0.60
JT arm    9.26 2.68 1.81 0.86 0.67 0.39 0.34 0.17 0.13 0.22 0.63 0.62
```

The sim's bins 4–12 are within **±0.04 m** of the baseline and bins 1–3 carry the entire loss — the same localisation the class census gives
independently.

## 7. VERDICT

**The pair (`pb_disc_carry` + `disc_jump_test_decommon`) does NOT join RF-0hv.**  J3b misses by 7 mm, and J4 / J5 / J6-h1 fail outright; nothing
is promoted — no golden, no ledger row, no loader default, `config/refound/*_rf0hv.json` untouched, the knob stays default OFF.

**What is settled.**  The frame-blind defect CST §4 measured is real and this is a correct fix for it: the cell that a carry could not reach
(G32 L1W, +1.409 m into a live ambiguity) is now declared and carried, the run-wide undeclared tail collapses 1.409 → 0.107 m, and the emitter's
own arithmetic (raw −0.042, `c_s` −0.797, de-commoned +0.755 against a 0.0476 m bar) is exactly the mechanism the card predicted.  The solve is
untouched on both beds and the wire's `R` records are byte-identical, so nothing here is a datum or estimate effect.

**What is falsified, and it is a sharper statement than "the knob is bad".**  De-commoning against the **NETWORK's** per-signal median is not
de-commoning in the consumer's frame.  Two independent measurements say so:

1. **The frame is not a frame during ignition.**  On the sim, `|c_s|` p50 is 0.134 m (max 1.785) in hour 1 and under 3 mm thereafter; 100 % of
   both discriminating classes live in hour 1; and hour 1 is the only hour that degrades (+3.61 m against ±0.02 m elsewhere).  Subtracting a
   median that is thirty-one satellites converging together removes real per-satellite motion from every cell.
2. **The network's frame is not the consumer's frame even when it IS a frame.**  At 276810 the emitter's L1W median over 22 emitted satellites
   is −0.797 m and the consumer's over its 6-satellite sky is −1.451 m: a 0.654 m gap, 13.7× the bar.  A step the emitter withholds as common
   is not common at a consumer tracking a small, non-representative subset — which is what the 322 `raw_only_suppressed` cells (58 of them
   reaching the consumer) cost, and why the field's settled hours degrade while the sim's, where `|c_s| < 3 mm`, do not.

**The residual.**  Hours 4–6 on the field lose 0.75 / 1.50 / 1.99 m and never recover within the run; the ramp window is FLAT (max-rise
+0.028 m — CST's WINDOW-B mechanism stays dead, as the carry arm established) but sits at **4.02 m instead of 2.04 m**, i.e. this is an
accumulated LEVEL offset the float consumer carries forward, not a new event.  CC-arm's own h4/h5 residual (+0.21 / +0.27 m, the two therapeutic
mass resets at TOW 271050 / 271200) is still there underneath and is now the smaller of the two terms.  CST §7.2's EMPTY declarations are
reduced but not solved: 117 (30.0 %) → 94 (25.3 %), because the cross-channel over-declaration is a `markSatDiscontinuity` FORCED bump, which
this knob deliberately does not touch.

**What the next arm should be** (pre-registered here, NOT built, no bars claimed for it yet): the de-common must be computed in a frame the
consumer shares, and there are two candidates the measurement distinguishes.  (a) **Gate the test on the frame being a frame** — declare
de-commoned only where the population's ΔPB dispersion is small relative to `|c_s|` (a frame move is a shift, a convergence is a spread), which
kills the ignition cost in both classes at once and leaves the 276810 fire intact; the dispersion is already in hand at the median site and
costs one extra pass.  (b) **Do not de-common on the producer at all — declare `c_s` and let the consumer subtract its OWN sky's median**, which
is the only formulation in which the emitter's frame and the consumer's frame are the same by construction.  (b) is a wire-contract change and
belongs with EV6 §8.1 and CST §7.2 on one arm; (a) is ~20 lines inside this knob.

## 8. WHAT STAYS OPEN

1. **The frame-vs-convergence discriminator is named and NOT built** (§7 (a)).  `kDiscDecommonMinSats` guards population size, which was never
   the failing quantity — `fallback_raw` is 0 on both beds with `n_med` 19–32.
2. **The consumer-side de-common (§7 (b)) is the only formulation with a shared frame**, and it needs a `c_s` field on the wire that no record
   currently carries.
3. **`products.txt` cannot carry this fix on this lineage and EV6's ledger is blind to it** (§2).  Either `network.serialize_disc_counters` is
   armed on the next arm, or every file-replay grade of a declaration knob is measuring the `R` records alone.
4. **The S-line declaration is untested in the field** — `serialize_sbasis_contract` is off in RF-0hv, so `discdecom=` never reaches a product
   here.  It is pinned only by the gtest round-trip.
5. **The 3 surviving undeclared cells above 0.10 m are unattributed in detail** (two ignition epochs, one mass-reset epoch, all within 7 mm of
   the bar); §3's frame-gap explains them structurally, not cell by cell.
6. **Nothing re-validates a carried ambiguity** — CC-arm's open item 2 is untouched, and this card adds 40 declarations that now translate
   rather than reset.
7. **The legend delta moves every stored whole-file product hash in the project by one comment line** (§2).  Cards quoting a raw
   `products.txt` sha256 from before this commit must compare `^#`-stripped streams, or re-capture.

## 9. RUNS AND ARTEFACTS

- `runs/_jt_smoke_off` — the OFF smoke (= `runs/_rf2d_smoke_off/smoke.json`, paths only), exit 0, `products.txt` raw `1ACC941CC853ECB2`,
  `^#`-stripped `333DCB0871E1F5BC` (= canonical).
- `runs/_jt_field6_both` — the FIELD arm (= `runs/_ccarm_field6_both/arm.json` + `"disc_jump_test_decommon": true`, paths repointed), 720/720,
  stderr empty, `products.txt` `3266C11EDED24A3C`, `logs/closed_loop_sat_trace.csv` 11 618 rows, 1 761 `PB_JUMP` event rows.
- `runs/_jt_sim6_both` — the SIM arm (same single key), 720/720, stderr empty, `products.txt` `204CF06C3F24492B`, trace 10 800 rows, 2 900
  `PB_JUMP` rows.
- `runs/_ccarm_field6_both` / `runs/_ccarm_sim6_both` — the CC-arm baselines, read only, not re-run, left in place.
- Exe `build/x64-Release/gnss_processor_jt.exe` `6E1021DBC554840A`.
- New gtest: `tests/test_pb_jump_decommon.cpp` (`PbJumpDecommonTest`, 6 cases).  No new script — the graders are
  `scripts/cst_consumer_read.py`, `scripts/cc_arm_grade.py`, `scripts/rf2d_grade.py`, `scripts/simnet_amb_integer.py` and
  `scripts/ev6_event_read.py` (the last one shown BLIND here, §2).
- `config/golden/`, `golden_manifest.json`, `docs/CONFIG_promotion_ledger.{md,json}`, `config/refound/*_rf0hv.json` and every loader default
  untouched.
