# Card CB-Q — the satellite code-bias carry's process noise

**Status: both measurement legs complete (leg A ignition, leg B settled). No production knob changed. Loader defaults untouched.**
⚠ **Read the headline with its qualifier.** §2's "the premise fails on arithmetic, before any run" and "`sat_code_bias_q` is a near-inert knob" are **correct only for q ≤ 1e-7**. §2.1's ⚠ block records the correction: the static derivation is WRONG when settled, because the filter is a loop — P converges to `P_∞ ≈ √(q·Δt·R)` rather than `prev_var + q·Δt`, so `R/P` falls 53 → 7 at the knee and **leg B measures cb vf 15.28 → 1.71, a factor of 8.9.** The inertness claim survives below 1e-7 and fails above it. Never quote the headline without this line.
Instrument `scripts/cb_structure_function.py` (new). Binary `build/x64-Release/gnss_processor_cbq.exe`, md5 `88107F4C15CC670537B2EF9F75818899` — a snapshot copy of `gnss_processor.exe` taken 2026-08-11 13:59 ([[feedback_renamed_exe_is_a_snapshot]]); every arm uses that one binary, so no comparison crosses a build. Arms `runs/cbq_20ep_*` (leg A) / `runs/cbq_12h_*` (leg B); configs `config/test_suite/_cbq_{20ep,12h}_*.json` — **config-only ladders; this card required no C++ change**, `sat_code_bias_q` has been a knob since W6.
Predecessor `docs/SPEC_clock_temporal_anchor.md` §8 (Card Q3's q descent) — read its §8.8 defect 2 and §8.9 first; the structural root this card re-encounters for the third time is the one Q3 named there.
⚠ **Archive**: every `runs/<name>` here (`runs/agu63`, `runs/cta4_q1e7`, `runs/cbq_20ep_*`, `runs/cbq_12h_*`, `runs/golden63_12h_internal`) resolves under `D:\GnssProcessor_archive\2026-08-12\runs\` (CLAUDE.md, archived 2026-08-12).

---

## §1 Where this card comes from

Q3 descended the satellite **clock** carry's q from 1e-3 to 1e-7 and measured every producer gate improving monotonically. The displaced tension landed in two channels, one dominant:

| family | Σw·ê² share of the whole global σ̂₀ numerator | Σr share of the redundancy | variance factor |
|---|---|---|---|
| **cb** (satellite code bias) | **69.11 %** | 10.32 % | **13.04** |
| rxcb (receiver code bias) | 8.44 % | 3.87 % | 4.25 |
| clk (satellite clock, anchored at q = 1e-7) | 22.46 % | 85.81 % | 0.51 |

(`runs/cta4_q1e7`, 3 h, run-end constraint-aware global σ̂₀; 45 614 folded cb rows.) **Two thirds of the entire system's prior-side misfit sits in the satellite code-bias carry, on a tenth of the redundancy.** ⚠ On this chain "the whole numerator" *is* the prior side — `phase_leveling` is OFF, so post-fit RAIM never runs and Σw·v² = 0 (the `carry_warm` gate CLAUDE.md flags, and Q3 §8 flagged again). The share is exact among priors; the prior-vs-data split is undefined here. The family reads vf ≈ 10–13 in *every* arm of the Q3 ladder including the control. Q3's verdict: the clock is the best-calibrated of the three once measured correctly, and the mis-modelled channel is the code-bias carry, by an order of magnitude.

The obvious next card writes itself — apply Q3's method, measure the physical rate from data then set the process noise to it, per the standing design principle ("aim to be physically correct, then errors will hopefully reveal themselves elsewhere"; inflated q is how the 9 cm clock walk hid for a year). **That premise is false, and this document is mostly about how.**

---

## §2 The premise fails on arithmetic, before any run

`sat_code_bias_q` enters exactly one expression, in both the diagonal and the dense C1 branch of the D2 carry-forward (`NetworkRunContext.cpp:1683` and `:1763`):

```
var_eff = cp.var + q_cb · Δt          // cp.var = the PREVIOUS epoch's posterior
```

Three numbers from `runs/cta4_q1e7`, all on disk before this card started: the run-end constraint-aware σ̂₀ line gives **mean w·ê² = 2.4270e-1** and **mean r = 1.8616e-2** for family `cb` (`w = 1/fold_var`, `r = 1 − w·post_var`); the M2 LS-VCE monitor gives the tail-robust typical innovation variance **E[ê²] = 5.345e-5 m²** (= 0.731 cm) over the same 45 614 rows. From those the typical fold variance is `fold_var ≈ E[ê²] / mean(w·ê²) = 5.345e-5 / 0.2427 =` **2.20e-4 m² = (1.48 cm)²**, and the posterior is **98.14 %** of the prior on a typical cb row.

| q (m²/s) | q·Δt at 30 s (m²) | share of the typical cb fold variance |
|---|---|---|
| **1e-10 (shipped)** | 3.0e-9 | **0.0014 %** |
| 4e-8 — satellite OSB hardware physics (~6 cm/day) | 1.2e-6 | 0.57 % |
| **7.3e-6 — where q·Δt EQUALS the fold variance** | 2.2e-4 | 100 % |

So "set q to the measured hardware physics of a satellite code bias" moves the fold variance by **half a percent**. No version of the honest-physics argument reaches the knee: a hardware OSB would have to wander ~2 m/day for q to matter, and it wanders centimetres.

**Same structural root as Q3 §8.8 defect 2 and §8.9, third instance: `var_eff = prev_var + q·Δt` with `prev_var` dominating, so the q knob is inert and everything downstream fails to scale the way the process model says.** Q3 found it for the clock anchor's variance floor (410× below `prev_var`, never bound) and for the k-in-metres scale (q spans 300×, √S spans 1.28×). Here it is the whole knob.

### §2.1 And q cannot move the census either — the redundancy says so

Treat one cb state as a scalar with prior variance `P` and effective data variance `R`. Then `post = PR/(P+R)`, `r = 1 − post/P = P/(P+R)`, and the prior residual `ê = P(y−μ)/(P+R)` gives

```
    vf  =  E[w·ê²] / r  =  (R + σ_t²) / (P + R)
```

with `σ_t²` the true per-epoch motion. Measured `r = 0.0186` ⇒ **R ≈ 52.7·P**. Raising `P` cannot move `vf`: even doubling the fold variance changes `P + R` by 1.9 %. To halve vf by q alone you would need `q·Δt ≈ 53×` the current fold variance, i.e. `q ≈ 3.9e-4 m²/s`, at which point the carry is no longer a carry. Pre-registered as prediction **P3** in the leg-A configs before any arm ran; measured in ignition (leg A) **confirmed**, 1.35× over a 10⁶× span in q.

⚠ **And WRONG when settled — the derivation is static and the filter is a loop.** Leg B measures cb vf 15.28 → **1.71**, a factor of **8.9**. The missing term is the steady state: a larger `q` raises the posterior, which becomes the next epoch's `prev_var`, which raises `P` again, so `P` converges not to `prev_var + q·Δt` with `prev_var` held but to **`P_∞ ≈ √(q·Δt·R)`** — `√(q)` growth over the ladder. At the knee `P_∞ ≈ 1.6e-3 m²`, ~7× the control's fold variance, so `R/P` falls 53 → 7 and vf falls with it. The one-epoch arithmetic bounds the *instantaneous* sensitivity correctly and under-states the *converged* one by the loop gain. **Recorded as an error of the card, not smoothed over: the inertness claim survives for q ≤ 1e-7 and fails above it.**

### §2.2 Why the LS-VCE could not have told us this

M2's `NetworkCarryVce` estimates two components — Floor (cofactor 1) and Rate (cofactor Δt) — and the Rate component *is* an honest q estimator. On this network it reports `cond = 1.05e16, rate_frozen = 1`: at a constant 30 s cadence the two cofactors are collinear and the Rate is **structurally unidentifiable**, pinned at its a-priori. The header says so explicitly ("Separability is a property of the DATA"). So the one instrument in the tree built to measure a carry q **cannot measure this one**, and the run-end line reports a Floor alone. That gap is what makes a lag-domain instrument necessary — the same move Q3 made when it went to the structure function.

---

## §3 The instrument — `scripts/cb_structure_function.py`

The CB analogue of `q3_clock_structure_function.py`. For each (constellation, signal), difference each satellite's emitted OSB over a lag `L`, remove the **pair-common set's** own centre, and take a robust scale (1.4826 · median |·|) of the remainder.

- **Gauge (invariant #5).** The OSB datum is sum-to-zero per (constellation, SIGNAL) over the satellites present *this epoch*, so a tracked-set change moves the realized mean and steps every satellite of the group together. Differencing only over the pair-common set and removing that set's centre is invariant to it by construction. Pairs spanning a signalled discontinuity (per-record `code_disc`, or an `R` retro-mark) are excluded.
- **Shape, not a fitted q.** The reported column is the ratio per lag-doubling: **1.0 = white floor, 1.41 = random walk, 2.0 = drift**. A q is only meaningful if the shape is a walk; quoting one otherwise is the error this card exists to avoid.
- **The IF-pin NULL CONTROL.** Each satellite carries one deterministic pin — the ionosphere-free combination of the pin pair selected by `NetworkSbasis::selectIfPinPair` (band digit, then signal name, then the first distinct frequency) is tied to that satellite's clock. That direction is fixed BY CONSTRUCTION, so its structure function must read ≈ 0. It is the instrument's own null test: a parse bug, a bad gauge removal or an epoch-key mismatch shows up there first. The script reproduces the selection rule (or reads the `cbpin=` token from the `S` record when present) and reports it.

⚠ **What it does not measure.** The emitted series is the FILTERED state. A stiff carry suppresses its own output, so anything fitted here is a LOWER BOUND on what the data are pushing. Q3 escaped that circularity with the phase-derived TDCP shadow; the code side has no independent instrument, so the bound is stated rather than hidden.

*(Bug paid for on the first run: the emitted TOW is a double that round-trips as `259229.99999809265`, so a bare float epoch key makes every `t + L` lookup miss and the whole structure function reads **zero pairs at every lag** — a silent, total false negative that looks like "no data". Keys are snapped to the millisecond.)*

---

## §4 What the wire actually does — and it is not a walk

`runs/agu63` (12 h, 61 stations, GPS, `sat_code_bias_q = 1e-10`), settled window = the last 8 h (`--skip-hours 4`, against the CB carry's measured τ ≈ 3.8 h step response):

| lag L | 30 s | 60 | 120 | 300 | 600 | 1200 | 2400 | 4800 | 9600 | 19200 |
|---|---|---|---|---|---|---|---|---|---|---|
| SF pooled (cm) | **0.642** | 1.289 | 2.576 | 6.498 | 13.128 | 27.146 | 59.022 | 115.44 | 212.08 | **391.09** |
| × per L-doubling | — | 2.01 | 2.00 | 2.01 | 2.02 | 2.07 | 2.17 | 1.96 | 1.84 | 1.84 |
| **IF(C1C,C2W) — PINNED (null control)** | **0.003** | 0.006 | 0.012 | 0.030 | 0.060 | 0.121 | 0.234 | 0.441 | 0.847 | 1.716 |
| GF(C1C,C2W) — the DCB direction | 0.304 | 0.612 | 1.223 | 3.099 | 6.253 | 12.779 | 26.798 | 52.451 | 95.726 | 195.09 |

**log-log slope over 30 s … 19 200 s = 0.992.** Not 0 (white), not 0.5 (random walk) — **1.0, a drift**, held across nearly three decades of lag.

1. **The null control passes to four digits.** The pinned IF direction reads 0.003 cm at 30 s against 0.642 cm for the raw signals — 210× smaller — and its residual growth is emission rounding. The pin holds; parser, gauge removal and epoch keys are sound; everything below is about the *unpinned* directions.
2. **The estimated OSBs drift ≈ 0.64 cm per 30 s epoch between satellites**, and the errors ACCUMULATE COHERENTLY rather than diffusing: 3.9 m over 5.3 h.
3. **It is not the ionosphere-confounded direction that drives it.** Same window, same removal, per-satellite combinations:

   | direction | SF(30 s) | SF(19 200 s) |
   |---|---|---|
   | IF(C1C,C2W) — **pinned** | 0.003 cm | 1.7 cm |
   | GF(C1C,C2W) — the DCB / iono-confounded direction | 0.304 cm | 1.95 m |
   | `C1W − C1C` — an L1 inter-channel **hardware constant** | 0.472 cm | 2.90 m |
   | `C2X − C2W` — an L2 inter-channel hardware constant | 0.608 cm | — |
   | IF(C1W,C2W) — the **unpinned** IF direction | **1.192 cm** | 7.31 m |
   | (satellite clock, for scale — this arm is un-anchored) | 9.225 cm | 18.1 m |

   The GF direction — confounded with the ionosphere, and the obvious suspect given the level channel's known wander — is **the quietest estimated direction of the four**, a quarter of the unpinned IF. The drift lives in the directions the pin does not reach, not in the DCB.

Per-satellite straight-line fits on the same window confirm the shape and show what a q would have to model:

| const/signal | nsat | median \|drift\| (cm/30 s) | p90 | median R² | median residual about the line (cm) |
|---|---|---|---|---|---|
| G/C1C | 32 | 0.367 | 0.742 | 0.444 | 96.7 |
| G/C1W | 32 | 0.295 | 0.876 | 0.252 | 175.7 |
| G/C2W | 32 | 0.579 | 1.221 | 0.419 | 175.0 |
| G/C2X | 6 | 0.293 | 0.354 | 0.128 | 13.5 |
| G/C5Q | 17 | **1.121** | 1.768 | 0.397 | 231.5 |
| G/C5X | 17 | 0.758 | 1.519 | 0.582 | 172.7 |

Not a clean ramp either: a straight line explains only 13–58 % of the variance and the series **wanders ±1–2 m about its own trend over 8 hours**. C5Q/C5X — the thinnest signals, 17 satellites — drift most, which points at observability rather than at any physical process.

### §4.1 The finding that settles what this is

`C1W − C1C` is the GPS L1 P-vs-C/A inter-channel bias. It is **hardware**, constant to well under a nanosecond over a day; the W6 leg validated this producer's own value against the COD `.BIA` to sub-mm (G18: 0.1714 vs 0.1736 m). Measured here, between satellites, common-mode removed: **SF = 0.472 cm at 30 s growing to 2.90 m at 5.3 h**, slope ≈ 1.

**There is no physical mechanism for a hardware inter-channel bias to move metres in an afternoon.** Whatever the estimated OSBs are doing, at least part of it is error, not motion — so a process noise sized to *cover* it would be exactly the "inflated q hides the defect" failure the design principle was written against. **The correct reading of vf = 13 is not "the carry is too stiff"; it is "something is pushing these states around and the carry is the only thing resisting."**

### §4.2 A bound helps; it is not a one-knob measurement

`runs/golden63_12h_internal` carries `cb_kaula_prior` (σ = 3 m, a loose physical bound on every admitted OSB column) where `runs/agu63` does not. Same settled window: SF(30 s) **0.439 cm vs 0.642** (−32 %), SF(19 200 s) **2.39 m vs 3.91** (−39 %), slope unchanged. ⚠ **NOT a one-knob A/B** — the arms differ in more than the Kaula prior (internal vs AGU posture) and their config-chain generations differ. Read it as "a loose bound on the drifting directions is worth tens of percent and does not change the shape", and as motivation for a real one-knob arm.

---

## §5 Leg A — the 20-epoch ladder, and two of my three predictions failed

Five arms, one knob (`filter.process_noise.sat_code_bias_q`), same binary, same chain, 600 s each, ~45 s wall apiece. Predictions P1–P3 were written into the config `_comment` blocks *before* the arms ran.

| arm | q | max \|ΔCB\| vs ctl | median \|ΔCB\| | max \|ΔCLK\| | cb vf | rxcb vf | clk vf |
|---|---|---|---|---|---|---|---|
| ctl | 1e-10 | — | — | — | 1.9191 | 0.6217 | 0.9873 |
| hw | 4e-8 | 3.32 mm | **0.056 mm** | 1.76 mm | 1.9197 | 0.6217 | 0.9871 |
| — | 1e-6 | 4.24 cm | 0.80 mm | 2.04 cm | 1.9133 | 0.6228 | 0.9868 |
| knee | 7.3e-6 | 17.3 cm | 3.37 mm | 9.94 cm | 1.8653 | 0.6302 | 0.9954 |
| — | 1e-4 | 84.5 cm | 2.37 cm | 56.9 cm | **1.4195** | 0.6776 | 1.1126 |

- **P1 — "ctl vs hw: max |Δ| ≤ 1 mm" — FAILED.** Max is 3.3 mm (the *median* record moves 0.056 mm, 18× inside the bar, but the claim was on the max). The tail is ~50× the median: **the rows most sensitive to q are the ones whose `prev_var` is SMALLEST (best observed)** — the opposite of the intuition that wrote the prediction.
- **P2 — "the first rung whose max |Δ| exceeds 1 cm lies between 1e-6 and 7.3e-6" — FAILED.** 1e-6 already reads 4.2 cm. On the *median* the 1 cm crossing does land just above the knee (3.4 mm at 7.3e-6, 2.4 cm at 1e-4), so the §2 arithmetic describes the typical row and not the tail.
- **P3 — "cb vf changes by less than 2× even at 1e-4" — CONFIRMED (in ignition).** 1.9191 → 1.4195 across a **10⁶× span in q**: a factor of **1.35**, essentially flat (< 0.4 %) over the first four orders of magnitude. ⚠ **Settled, this reverses — leg B measures 8.9× (§6, §2.1).**
- Bonus, unpredicted: the code-bias q perturbs the emitted **clock** too (0.57 m at 1e-4) — the two are coupled through the IF pin and the shared border, so this knob is not confined to the `B` records.

⚠ **These are IGNITION numbers and must not be read as the verdict.** The same arms report cb vf = **1.92** against **13.04** settled: 20 epochs in, `prev_var` is still enormous, the carry is loose and consistent, and q's relative weight is at its *smallest*. Leg A establishes that the knob is wired, monotone and crash-free, and it kills P3's hypothesis space. It cannot grade anything.

---

## §6 Leg B — the settled ladder: RAN, and it corrected the card

`runs/cbq_12h_{ctl,hw,knee,q1e4}` — 12 h so a 4 h ignition cut still leaves 8 h of settled window; one knob; same binary; ~30 min of epoch loop per arm.

**Outcome against gate 3 (the variance factors — "P3 re-run where it counts"): FAILED as predicted, in the direction that falsifies §2's own headline.** Settled, **cb vf falls 15.28 → 1.71, a factor of 8.9** across the ladder, against leg A's ignition reading of 1.35× over a 10⁶× span. The missing term is the steady state `P_∞ ≈ √(q·Δt·R)` — at the knee `P_∞ ≈ 1.6e-3 m²`, ~7× the control's fold variance, so `R/P` falls 53 → 7 and vf falls with it (§2.1). **The one-epoch arithmetic bounds the instantaneous sensitivity correctly and under-states the converged one by the loop gain.**

Gates as pre-registered — graded on, and *only* on: **(1)** `cb_structure_function.py` SF(30 s) **and the log-log slope** on the settled window — the wire statistic, the one with resolving power (Q3 §8.10: do not over-rank rungs on the consumer); **(2)** the **IF-pin null control** in every arm — if it moves, the arm is not comparable and the finding is about the pin, not about q; **(3)** cb / rxcb / clk variance factors — outcome above; **(4)** post-fit σ̂₀_code and the closed-loop consumer median / p95 / worst.

**Stop rule.** A rung is "better" only if SF(30 s) falls *without* the slope rising and without σ̂₀_code rising. A rung that lowers the census vf while raising the wire SF is the carry giving up, not the model improving — and given §4.1 that is the expected shape of a loose rung. **Nothing in leg B has been shown to clear it**, so §7's verdict stands.

⚠ **STILL OWED in this document**: the per-arm numbers for gates 1, 2 and 4 (settled SF(30 s) + slope, the IF-pin null per arm, σ̂₀_code and the consumer tails) are not recorded here. Only gate 3's result was folded back.

**Products are not expected byte-identical at any rung** (leg A already shows mm-level motion at 4e-8); the OFF-identity discipline does not apply because there is no new code path — this is one number in an existing expression.

---

## §7 What the lever actually is — open, and named

- `sat_code_bias_q` is a **weakly-active knob mis-cast as the stochastic model**. Any value in [1e-10, 1e-7] is observationally the same posture; nothing in hardware physics reaches the knee; and no value fixes vf = 13.
- The drift lives in the **unpinned, weakly-observable inter-channel directions** (C5Q/C5X worst at 17 satellites; `C1W − C1C` drifting metres though it is a hardware constant; the pinned IF direction dead flat). That is an **observability / datum** problem, so the honest levers act on observability:
  - `network.cb_kaula_prior` — the existing loose physical bound; §4.2 measures −32 % on the drift but not as a one-knob arm. **The cheapest real next experiment in this card is a one-knob Kaula A/B on the leg-B chain**, plus a σ ladder under it (3 m is arbitrary; the physical bound is tens of ns).
  - `network.code_bias_datum: connected_components` — deferred at W6 on a benefit estimate (≤ 0.14 m on ~6 GPS L1C Block-III satellites) that this card's numbers may reopen: the drifting directions are exactly the inter-channel ones a per-signal datum over-pins and a component datum would separate. ⚠ If ever built it MUST carry the partition-change `markDatumChange()` trigger W6 already specified.
  - Standing invariant #1: the CB carry's event response `cb_excursion_reinit` exists, is default-OFF, and is **unarmed in every arm graded here**. It is a *step* detector (network-wide |Δclock| median) and would not fire on a drift at all — so the CB carry has an event response for the DOY-129 ratchet and **none for the mode this card measures**.

None of that is built or promoted by this card. It is measurement plus a falsification, and the ledger stays SCOPED with loader defaults OFF.

---

## §8 Invariant pre-flight

| # | invariant | status here |
|---|---|---|
| 1 | zero-q / stiff time-linked quantity needs a wired event response | The CB carry is effectively zero-q (§2) and its only event response is a *step* detector, default-OFF and unarmed. **Named as a gap**; a loosening rung would reduce the pressure, a tightening one would raise it. |
| 2 | field runs are Release-only | All arms ran `gnss_processor_cbq.exe` from `build/x64-Release`. |
| 3 | every deferral triaged | §7 lists the three deferred levers with the data that makes each field-inert or field-blocking. |
| 4 | run-end counters are the telemetry | The knob is verified live per arm from `network_run_manifest.json` (`process_noise_sat_code_bias`), not from the config. |
| 5 | grade only datum-invariant quantities | The instrument is built on pair-common-set common-mode removal, and the IF-pin null control proves it (§4). |
| 6 | new work default-OFF, byte-identity | No new code path; no default changed. The leg-A ladder IS the sensitivity measurement byte-identity would otherwise be. |
| 7 | both edges of any information-flow change | Not applicable: q changes a weight, it admits and decouples nothing. |

---

## §9 Traps paid for, or worth stating

1. ⚠ **A float epoch key silently zeroes a structure function** (§3). The failure mode is "no pairs at any lag", which reads as missing data rather than as a bug.
2. ⚠ **"Never verdict in ignition" is unusually strong for this carry.** cb vf reads 1.92 at 20 epochs and 13.04 settled — a 6.8× difference in the card's headline statistic. The CB step response is τ ≈ 3.8 h, so a 3 h arm is entirely ignition (`runs/cta4_q1e7` SF(30 s) = 1.144 cm against the settled 0.642).
3. ⚠ **The M2 Rate component is not a q measurement on this network** — frozen by construction at a constant cadence (§2.2). A future arm at a deliberately IRREGULAR cadence would identify it; nothing else in the tree will.
4. ⚠ **A fitted "walk-equivalent q" is lag-dependent when the slope is 1.** The script prints one and immediately says so; the number for `runs/agu63` reads 8.2e-4 m²/s over 600–19 200 s and would read differently over any other lag pair. **Do not quote it as q.**
5. ⚠ **The config chain moved on 2026-08-11** (`_agu63.json` gained `consumer_undifferenced`; `_golden63_gps_1h_laneoff.json` moved with it), so `runs/golden63_12h_internal` is **not** a valid control for this card's arms — its `config_extends` hashes differ. That is the shared-chain edit warned about in `SPEC_clock_temporal_anchor.md` §8.11.9, landing on the very next card. Leg B carries its own control for exactly this reason.
6. The scalar algebra of §2.1 defines an *effective* R through `r = P/(P+R)`; the real system is a reduced multi-station normal-equation block with off-diagonal coupling. The relation is used only to convert a measured redundancy into a sensitivity, which is what it is good for.
