# REPORT — SPEC_day129_frame_guards, Phases 0–1 (+ Phase 2.1 build)

Execution record for `docs/SPEC_day129_frame_guards.md`. Lineage:
`docs/HANDOFF_night305_day129.md` → `docs/REPORT_night305_day129_investigation.md`
→ this. Tags: **[M]** measured here; **[H]** hypothesis.

---

## 0. Scope delta the spec did not anticipate

**Phase 0 was specified as "no new code, run-only". It is not.** The spec (and the
handoff Q5 note it inherits) assumed the recorded product stream could be replayed
through an existing consumer. Neither existing path can do it:

| path | why not |
|---|---|
| `realtime.network_products_path` + `processing.network_consumer.enable` | the real-time primary stream is RTCM/SBF **binary** only (`ApplicationRealtime.cpp:202-236`). DOY-129 observations are RINEX. There is no RINEX obs source in `runRealTime`. |
| `network.closed_loop` harness | fills its store from the **live emitter** inside `runNetwork` — it requires a producer run (≈25 h at 305 stations), which is exactly what Q5 exists to avoid. |
| `ProcessingMode::PPP_RTK_User` | declared in the enum and routed through the ordinary PPP path, but nothing in that path ever loads a network product file. |
| load the whole file into `SsrCorrectionStore` up front | the store holds the **current** record per satellite (`by_sat_`), not a time series — a bulk load leaves only the last epoch. |

Phase 0 therefore required the missing wiring. It was built as a re-wiring, not a
re-implementation: every consumer-side decision is the existing harness's.

### `Application::runConsumerReplay` — `src/core/app/ApplicationConsumerReplay.cpp`

Armed by `network.closed_loop.replay_products_path`; `runNetwork` short-circuits
to it **before** input validation and setup, so no producer machinery (roster,
station build, emitter, snapshot fingerprint) is touched and a replay run cannot
perturb a producer artifact.

|  | live harness | replay |
|---|---|---|
| consumer config | `deriveClosedLoopConsumerConfig()` | *same call* |
| consumer engine | `RealTimeEngine` + `setSsrStore(owned)` | *same call* |
| per-epoch obs | `MultiRinexSync` | *same class*, one-station roster |
| per-epoch store fill | `emitter.emitToStore` | `NetworkProductReader::replay` from the file |
| trace | `closed_loop_trace.csv` | *same schema* |

Because the store is filled through `NetworkProductReader::replay`, the consumer
sees the recorded stream's discontinuity counters reconstructed exactly — a
byte-faithful reconstruction of what a live tail consumer would have received.
Cost: **~35 min per full-day arm**, vs a producer day.

Two deliberate departures from the producer path, both forced:
* **No parse window.** The producer clips the RINEX parse to
  `start_offset + max_duration` anchored at the FILE start. A leg's products begin
  hours into the observation day, so that clip strips out exactly the epochs that
  carry products. One station parses in seconds; the window is dropped and the
  loop fast-forwards to the first product epoch instead.
* **`consume_network_iono` is warned and inert.** The SH-VTEC slant iono is an
  in-process supply with no product-wire record; a file consumer cannot receive it.

---

## 1. Phase 0 — Q5 closed-loop replay. **PREDICTION CONFIRMED** [M]

Consumer **NRMG00NCL** (Nouméa) — not in the 305-station roster, 30 s sampling
(matches the product cadence exactly), tracks both arm pairs. Truth = the
**ITRF2020 / IGS20 monument coordinate**, velocity-propagated to the epoch
(§1.0 — upgraded from the RINEX header after the fact); a truth error is a
constant common to both arms and cannot manufacture a time-localized signature.
Float consumer (`ar_mode 0`): a code-bias fault expresses itself in the float
position directly, whereas AR on a faulted stream would measure the resolver,
not the product.

### 1.0 Truth coordinate — RINEX header → ITRF2020 (added Jul-27) [M]

Phase 0 graded against the RINEX header `APPROX POSITION XYZ`. That value is not
wrong, it is **stale**, and the tables below are re-anchored onto the published
datum:

| | X | Y | Z |
|---|---|---|---|
| RINEX header `APPROX POSITION XYZ` | −5743537.9710 | 1380504.0131 | −2397896.3726 |
| **ITRF2020 @ 2024:129** (SSC SOLN 4 + velocity) | **−5743538.2169** | **1380503.7952** | **−2397895.8332** |

σ 3.2 / 1.4 / 1.7 mm, from the frozen `data/static/sinex/ITRF2020-IGS-TRF.SSC`
via `python scripts/resolve_itrf_ecef.py NRMG00NCL --doy 2024:129`. The frame
matches the products (COD MGEX finals are IGS20-aligned), and the SSC is consumed
**nowhere** in the C++ engine, so the grader stays independent of the solver.
`ANTENNA: DELTA H/E/N` is 0/0/0 in this file, so header-APPROX, ARP and monument
are the same point — a like-for-like swap with no antenna reduction to get wrong.

**The header coordinate is internally identified as a ~2012 realization** [M]:
projected onto this station's own ITRF2020 secular track it best-fits epoch
**2011.91 with a 2.6 cm perpendicular residual**. Nouméa rides the Australian
plate at 5.07 cm/yr, so ~12.3 yr of unmodeled plate motion is the entire
discrepancy. Nothing here is a frame error or a blunder; the header was simply
never updated.

Shift (ITRF2020 − header) at the site: **E +0.269 / N +0.571 / U −0.030 m, 3D
0.632 m** — almost pure horizontal.

Re-anchoring needs no re-run. `closed_loop_trace.csv` stores the per-epoch error
as ENU at the old truth, the rotation is orthonormal, so the estimated ECEF is
recoverable exactly and re-graded by `scripts/q5_reanchor_hourly.py` into
`logs/consumer_replay_hourly_itrf.csv` (a sibling; the C++ artifact is never
overwritten). Validated by null transform — re-anchoring onto the OLD truth
reproduces the C++-written bands on **all nine arms to ≤ 9.8e−5 m**, i.e. the
trace's own 0.1 mm ENU print quantization and nothing more.

**All nine arms were then re-run** under the corrected truth (`runs/q5/itrf/`,
~29 s each), so the tables below are what the code prints, not an offline
reconstruction. The re-anchor tool is validated against that re-run: its
predicted per-epoch `err3d` / E / N / U match the fresh run's to **≤ 1.0e−4 m
over all 2160 epochs**. That agreement also *proves* the property the whole
exercise rests on — `network.closed_loop.station.position_ecef` is
**grading-only** in the replay driver. The consumer engine seeds its a-priori
from the RINEX header (`RealTimeEngine.cpp:189`), never from this field, so
changing the truth cannot change the solution: same epoch schedule, same
estimates, only the differencing point moves.

Consequence: every median moves by **≤ +0.6 m** and every mean vertical by
**+0.03 m**. **No verdict in §1.2–§1.5 changes**, and none could have — the A/B
contrast and the time structure are differences, where a constant cancels. What
the upgrade buys is that the *absolute* levels (the "quiet floor", the fault-era
magnitudes) are now quotable against a real datum instead of carrying a 0.63 m
unknown — and, more sharply, that the consumer is now graded **in the same
realization the products themselves declare**: the producer's 305 held-fixed
station coordinates are ITRF2020@2024:129 to ≤ 0.1 mm, so a 2012-epoch anchor
was differencing the solution against a point 0.63 m outside the product frame.
§7's third caveat is retired.

**Driver defect found and FIXED while re-anchoring** [M]. The hour index was
`(int)((now_s − hour0_s) / 3600.0)` on a `tow` carrying sub-microsecond residue
from the calendar→GPS conversion, so an epoch landing exactly on an hour
boundary truncated into the *previous* band about half the time: these arms
banded **121/119/120 repeating**, not 120/120/120. `ApplicationConsumerReplay`
now bands in integer milliseconds (`llround(elapsed·1e3) / 3600000`), which
removes the boundary case exactly; all nine re-runs band 120×18. It is the same
class as the `start_offset_hours` = 13.0 residue gotcha in §6. One epoch in 120
changed no conclusion, but it did move individual medians by up to ~0.24 m
(arm M hour 16), which is the same order as the truth correction — worth having
fixed before quoting absolute levels.

`q5_reanchor_hourly.py` still takes band membership from each run's own `n`
column, so it stays exact for artifacts produced by the pre-fix binary as well
as the post-fix one.

### 1.1 Defining the arms cost a real correction

`processing.signal_priorities.GPS` as an **attribute string does not bind L2**.
`PppModel::decideConstellationCanonicals` (PppModel.cpp:463-501) picks, per (band,
obs type), the priority-list signal with the **highest observation count** this
epoch and only breaks TIES by priority order. NRMG tracks C2W on all 32 GPS
satellites and C2L on ~25, so C2W wins on count under *any* attribute string —
`"CLSWXPYMIQ"` yields **(C1C, C2W)**, an L1-only contrast, not the intended pair
contrast. The arms must be **explicit signal arrays**, which removes the competitor
from the candidate set. (The first pair of runs was made before this was found;
they are retained as a third arm, and they are the reason §1.4 exists.)

Consistency check: the string-defined arm A and the array-defined arm A resolved to
the same signals and produced **identical** hourly numbers to 4 decimals — the
driver is deterministic and the arm definition is what it claims to be.

### 1.2 Position domain — hourly level bands (median 3D error, m)

Hour 0 = 06:00–07:00 (first product epoch). Bands, never slopes (handoff §4.1: a
single Theil-Sen fit over this series is invalid). Values are the §1.0 re-run
(`runs/q5/itrf/`, ITRF2020 truth + fixed banding); the original header-anchored
figure follows in parentheses where it differs at this precision.

| hour | UTC | **A** (C1W,C2W) | **B** (C1C,C2L) | note |
|---|---|---|---|---|
| 1 | 07–08 | 13.0 *(12.6)* | 7.0 *(6.9)* | **07:32 event** |
| 2–12 | 08–19 | 7.6 – 13.0 | 4.8 – 13.2 | quiet baseline |
| 13 | 19–20 | 21.5 *(21.6)* | 8.5 *(8.3)* | 19:21 event |
| 14 | 20–21 | 24.9 *(max 143.4)* | 7.9 *(max 86.1)* | **20:47 event** |
| 15 | 21–22 | **46.0** *(45.5)* | 14.6 *(15.0)* | |
| 16 | 22–23 | **46.8** *(46.4)* | 7.5 *(7.0)* | |
| 17 | 23–24 | **41.8** *(41.5)* | 9.9 *(9.5)* | day ends |

Mean vertical error, hours 15–17: **A −20.5 / −34.7 / −30.6 m**; B +5.3 / +4.4 / +4.6 m
(the truth shift is 3 cm in the vertical, so these barely move).

*(Errata caught while re-anchoring: arm A's quiet-band upper bound was printed as
11.4 — that is hour 12's median, not the band max. The original range was
7.1–12.7, set by hour 6. Arm B's 4.2–12.9 was correct.)*

**Verdict.** Arm A sees the 20:47→EOD era as a sustained fault — 4–6× its own quiet
floor, still un-decayed at end of day, expressed mostly as a vertical bias. Arm B
returns to its quiet floor within one hour. The 07:32 event costs neither arm
anything beyond its quiet band, exactly as predicted ("transient only"). The
predicted A/B signature is **confirmed**.

### 1.3 Correction domain — the same story, one layer down [M]

Deviation of G01's total-IF (`CLK + α·CB(s1) − β·CB(s2)`) from the per-epoch
constellation median — the datum-invariant quantity handoff §3 mandates:

| hour | A (C1W,C2W) | B (C1C,C2L) |
|---|---|---|
| 9–12 (quiet) | −0.4 … +4.0 | 0.0 … +2.7 |
| 15 | **+84.2** | +28.6 |
| 16 | +63.7 | +20.8 |
| 17 | +48.0 | +13.7 |

Between-satellite spread over all satellites, hour 15: **A 104.3 m** vs a quiet
~25 m; **B 41.5 m** vs a quiet ~20 m.

**This refines REPORT §2.1.** That section measured the single-epoch STEP at
tow 334020 and found the datum-pinned combination IF(C1C,C2L) held (−0.95 m). True
— but graded datum-invariantly as a LEVEL, the pinned pair also took a persistent
hit of **+28.6 m**, about a third of the convention pair's +84.2 m. "The datum held
everything it pins" is an epoch-to-epoch statement, not a level statement. The pin
attenuates the damage by ~3×; it does not eliminate it.

### 1.4 The fault is invisible to the consumer's own residuals [M]

Post-event (hours 15–17) median |post-fit code residual| over all BSSD pairs:
**arm A 0.94 m, arm B 1.14 m** — arm A, the damaged one, is *lower*. No G01-bearing
pair dominates arm A's list.

**This falsifies the spec's expectation that "arm A sees a G01-specific pseudorange
fault its RAIM should exclude."** It does not. With the network OSB applied per
signal and the consumer running BSSD, the pivot satellite's bias enters every
single-difference with the same sign — a common term with no receiver-clock state
left to absorb it (BSSD eliminated it), so it projects onto position and troposphere
and leaves the residuals clean. A consumer's RAIM has nothing to fire on. This is
the quantitative version of handoff §7's "a consumer cannot self-detect the damage,"
and it removes RAIM-at-the-consumer from the list of viable mitigations.

### 1.5 Bonus: a MIXED signal pair is worse than either consistent pair [M]

The accidental third arm, **(C1C, C2W)** — neither the datum-pinned pair nor the
convention pair — has an *intermediate* correction-domain fault (G01 dev +34.7 /
+25.8 / +17.6 m over hours 15–17, between A and B) but by far the **worst**
position: median 3D **64.0 / 80.4 / 113.9 m** (original header anchor 63.4 /
80.2 / 113.7), growing monotonically to end of day, with mean vertical −79.8 m
at hour 17.

Mechanism **[H]**, not established here: the producer pins IF(C1C,C2L) per satellite
and imposes sum-to-zero per (constellation, signal) channel. A consumer forming
IF(C1C,C2W) sits in neither frame, so its receiver code-bias state must absorb a
between-channel differential *while the near-static satellite OSB is itself moving* —
a competition the receiver state, at `code_bias_q = 1e-4 m²/s`, tracks slowly.

Practical consequence regardless of mechanism: **the product needs to declare which
signal pair it is internally consistent with, and a consumer needs to use a pair
within one frame.** Today `selectIfPinPair` picks the lexically-first pair and the
contract line does not say so.

### Reproduce

```bash
python C:\GnssProcessor\scripts\q5_make_replay_arm.py --arm A --outdir runs/q5/armA
```
then run `build/x64-Release/gnss_processor.exe runs/q5/armA/config.json` with
`-WorkingDirectory runs/q5/armA`. Artifacts: `runs/q5/arm{A,B}/logs/` —
`closed_loop_trace.csv`, `consumer_replay_hourly.csv`, `measurement_residuals.csv`.
`runs/q5/armB_L1only/` is the §1.5 mixed-pair arm (`armA_L1only` is the
string-defined arm A, hourly-identical to `armA` — the §1.1 consistency check).

`runs/q5/itrf/<arm>/` are the §1.0 re-runs (ITRF2020 truth, fixed banding) —
same configs with only `position_ecef` replaced, run from inside each arm dir.
To re-grade an existing arm onto another truth *without* re-running:

```bash
python C:\GnssProcessor\scripts\q5_reanchor_hourly.py runs/q5/arm* --selfcheck
```

---

## 2. Phase 1 — trigger instrumentation. **BUILT, SMOKE-VERIFIED**

All five items land; every toggle was verified absent from the fingerprint
construction (`NetworkRunContext.cpp:172-307`) before use, so all are legal to arm
on a warm resume against the untraced 6 h seed.

| # | item | where | armed by |
|---|---|---|---|
| 1 | per-epoch carry churn `n_d5_flags, n_d5_retaps, n_chord_births, n_slip_refloats` | `NetworkInstallCarry` differences the run-cumulative driver counters; emitted in `network_epoch_status.csv` | always (appended columns, T2d precedent) |
| 2 | per-epoch deweight state `n_dw_engaged, n_dw_zero, dw_min_w, dw_mean_w` + `IONO_DW` engage/release events | `NetworkIonoDeweightGate::{aggregate,lastTransitions}`, harvested at the serial `advance()` site in `NetworkShHealth` | always (gate-active only) |
| 3 | shared-normal spectrum + per-family eigenvector mass | new seam `filter/NetworkFrameDiag` → `network_sh_spectrum.csv` (+ mode detail) | `network.trace_sh_spectrum` |
| 4 | emit-path post-fit σ̂0 | same seam → appended `network_epoch_status.csv` columns | `network.trace_postfit_sigma0` |
| 5 | DD fractional trace | existing knob | `network.trace_dd_fractional` |

`python scripts/chain_make_leg.py … --frame-guards` arms 3/4/5 together.

### 2.1 Why the `sigma0` column was 0.0000 on all 2160 rows — **ANSWERED**

`NetworkStashStatus.cpp` publishes `es.sigma0 = sigma0_adapt`. `sigma0_adapt` is
computed **only** inside `if (within_epoch_armed && carry_warm)` in
`NetworkSolveGlue.cpp:735` — i.e. only when `network.raim_within_epoch` is on *and*
phase-leveling has warmed the carry. Both were off on the reference run, so the
statistic was never computed. It is not a formatting bug and not a plumbing bug:
**the emit path has no post-fit residual statistic at all** unless the within-epoch
RAIM runs. That is why a −4500 m clock epoch shipped `gate_ok=1`.

P1-4 adds one that always runs when armed, over **every** station's residuals
(vs the RAIM's carried-ambiguity-phase-rows-only pool), split into code and phase.
Measure-only: the A4 ceiling still reads `sigma0_adapt`, so no emit decision changes.

### 2.2 A second finding from building it: the phase rows carry no redundancy [M]

`sigma0_pf_phase` is **identically 0** and `sigma0_pf_code` ≈ 6.6–7.2 (smoke). With
free per-(station, satellite, signal) ambiguities — the configuration whenever
`network.phase_leveling` is off — every phase row is fitted **exactly** by its own
ambiguity. The phase residuals are zero by construction, so:

* the pooled `sigma0_pf` is diluted by those zeros and must not be read directly —
  **read `sigma0_pf_code`**; the run-end line reports both pool sizes so the
  dilution is explicit;
* more importantly, this says the phase rows have **no fault-detection power** in
  this configuration. It is the structural reason the producer's phase-side RAIM is
  gated on phase-leveling, stated as a number.

This also caught a would-be inert diagnostic: the natural phase discriminator is the
within-epoch RAIM's `pl_amb` set, which is empty without phase-leveling and would
have put every row in the "code" pool silently. The shipped discriminator keys off
the shared PB/CB column families instead (a property of the measurement model, not
of the QC configuration).

### 2.3 Smoke [M]

10-epoch warm resume from a copy of `night305_conv_immutable.txt`
(`runs/p1_smoke2/`): exit 0, `WARM RESUME … snapshot age 30.0 s` (the spec's
assertion), 12 epochs traced, 23 884 residual rows pooled on the last epoch,
`IONO_DW` events firing, all four files written. Overhead: `frame-diag` net 310 ms /
wall ~3.8 s per epoch against a ~16–20 s epoch (~20 %).

**Run status: COMPLETE.** The 3.5 h instrumented leg (`runs/p1_leg/`) ran 420 epochs
from a warm resume at 06:00 over the 07:32:00 collapse (tow 286320): exit 0, **0
errors, 0 criticals**, 0 skipped, 0 gate/σ̂0 withholds, `snapshot age 30.0 s`.
Run-end counters (invariant #4): P1-3 wrote 420 epochs; P1-4 pooled 420 epochs,
24 084 rows on the last (11 134 code + 9 910 phase). Verdict in §4.

---

## 3. Phase 2.1 — CB carry event response. **BUILT, default-OFF** (per D1/D2)

D1 taken as recommended: **pivot-only**, with a counter for how often a
per-satellite rule would have caught a non-pivot satellite. D2 taken as
recommended: implemented concurrently; **the fresh-convergence arming run is NOT
scheduled** and waits on the Phase-1 trigger verdict.

* Detector (`NetworkClockQc`): per-epoch **median** over satellites of |Δclock|
  against `network.cb_excursion_median_m`. Datum-change epochs are exempt (a
  handover moves every clock — a datum artifact; the clock monitor's jump test
  suppresses on the same condition).
* Response: the tripped epoch's stash marks the clock-pivot satellite's **code-bias**
  carry `inflate_q` (the existing reinit path — folded next epoch at
  `clock_monitor_reinit_sigma_m`, default 30 m) and **only** the code-bias carry.
  The clock is soft (q = 0.3 m²/ep, recovers in 2–4 epochs) and the phase biases
  measurably did not move (≤ 4 mm at the event); the near-static code bias is the
  one state that ratchets, so it is the one state that is reset.
* Card-2 rule: the reinit is queued for a **code-bias discontinuity bump** at the
  next emit (`NetworkSolPrep`, same present/pending split as the eph-cutover and
  basis-relatch bumps, so the bump lands with the satellite present and replay
  reproduces the counters).
* Toggles: `network.cb_excursion_reinit` (default false),
  `network.cb_excursion_median_m` (50.0), `network.cb_excursion_reinit_all_sats`
  (D1b, default false). Conditional-appended to the resume fingerprint when armed —
  every existing unarmed fingerprint is byte-identical, and a resume may not cross a
  flip.
* Run-end telemetry (invariant #4): trips / reinits / bumps landed / largest median
  observed / the D1 non-pivot counter, plus an explicit **WARN when armed but never
  tripped** ("do NOT read this run as evidence the guard works") and a WARN if a
  queued bump never landed.

### 3.1 Threshold, derived empirically from the reference run [M]

Per the MW-thresholds directive — no stochastic-model σ. Distribution of the
per-epoch median |Δclock| over the day's 2159 inter-epoch deltas:

| statistic | value |
|---|---|
| median | 0.123 m |
| 1.4826·MAD | 0.071 m |
| q0.99 | 13.0 m |
| q0.999 | 611 m |
| max | 4581.9 m |

and at the three known events:

| event | median \|Δclk\| | max \|Δclk\| | max/median | persistent damage |
|---|---|---|---|---|
| 07:32 (286320) | **5.03 m** | 529 m | 105 | none |
| 19:21 (328920) | 77.4 m | 334 m | 4.3 | none |
| **20:47 (334020)** | **4581.9 m** | 5624 m | **1.23** | **YES (+49 m)** |

The default **50 m** sits ~400× the quiet median (~700 robust-σ) and three orders of
magnitude below the damaging event: it trips on 10 of 2159 epochs (0.46 %). The
choice is not delicate — anything in 20–500 m isolates the same era.

Note what the **median** buys, and that this is not a tuning accident: it is blind to
the two-sided 07:32 excursion (5.03 m, no trip, and that event did no lasting harm)
and saturated by the one-sided 20:47 one (max/median 1.23 — the whole constellation
moved together). That is precisely the common/differential discriminator REPORT §2.2
derives from the mechanism: **only the one-sided projection reaches the pins.** The
detector's selectivity is validated against the reference run *before* it is armed.

### 3.2 OFF-path byte-identity — **PASS** [M]

Same-build (Release) 20-station × 11-epoch cold leg, `runs/bytecheck/`, SHA-256:

| artifact | pre-2.1 control vs Phase-2.1 | pre-**Phase-1** control vs Phase-2.1 |
|---|---|---|
| `products.txt` | IDENTICAL | IDENTICAL |
| `network_solution.csv` | IDENTICAL | IDENTICAL |
| `network_trace.csv` | IDENTICAL | IDENTICAL |
| `network_epoch_status.csv` | IDENTICAL | +12 appended columns; **first 17 columns byte-identical row-for-row** |
| `network_events.csv` | IDENTICAL | +1 `IONO_DW` row; every pre-existing row unchanged |

The right-hand column is the whole card (Phases 1 + 2.1) against a binary that
predates all of it. **The emitted product and the solution are byte-identical**; the
only deltas are the two trace channels Phase 1 deliberately adds always-on (appended
columns per the T2d precedent, and the new `IONO_DW` event type). Invariant #6 is
closed for both phases.

*(A 20-station cold leg rather than the 305-station warm resume: the roster is IN the
fingerprint, so a reduced roster cannot resume the reference snapshot, and the P1 leg
holds the memory headroom. The reduced run still exercises every path Phase 2.1
touches — carry stash, `NetworkSolPrep` bump block, clock QC, teardown, fingerprint.
The left-hand column was re-run against the FINAL binary after the last two edits;
a byte-identity result that predates its binary is worthless.)*

### 3.3 Not yet done

* **ON-path validation** — needs a fresh 6 h convergence to arm the co-listed
  fingerprinted knobs (`code_bias_disc_jump_test`, `emit_ura`, `phase_leveling`);
  deliberately not scheduled (D2). Until a run shows the detector firing with
  non-zero `n_reinit`, the guard is **not** field-validated — only built and
  byte-identity-gated.

---

## 4. Phase 1 trigger verdict

Produced by `python scripts/p1_trigger_verdict.py --legdir runs/p1_leg`. Each channel
is standardized against its own pre-onset window with a **robust** z (median +
1.4826·MAD — these series are heavy-tailed and a mean/sd z would be set by whatever
the tail already contains), and is called CONFIRMED only if the onset epoch exceeds
k·σ̂ **and** exceeds the largest value anywhere in the baseline. The second test is
what stops a merely noisy channel reading as a trigger.

**Null control [M]:** run against an arbitrary quiet epoch (tow 283320) of the same
leg, every channel reports *quiet* and the verdict is ALL QUIET. The machinery does
not cry wolf, so a CONFIRMED at the real onset means something.

One interpretation note baked into the tooling: `ev_min` is the smallest eigenvalue
of the **pre-datum** reduced normals (`solve()` pins a copy —
`NetworkEstimator.cpp:255`), so its smallest eigenvalues legitimately include the
*declared* gauge freedoms. "λ_min cratered" is meaningless in absolute terms; the
trace therefore carries the rank gate's own absolute cutoff
(`rankGateTol · max(λmax,1)`) and the verdict script judges the ratio.

### 4.1 Result at tow 286320 (07:32:00) [M]

`runs/p1_leg/`, warm resume at 06:00, 420 epochs, all Phase-1 channels armed.

| channel | t−1 | **onset** | robust z | > baseline max? | verdict |
|---|---|---|---|---|---|
| D5 innovation flags / re-taps | 1029 | **2013** | 7.7 | yes | **CONFIRMED** |
| post-fit σ̂0 (code) — *new in P1-4* | 2.92 | **10.20** | 219 | yes | **CONFIRMED** |
| worst clock deviation (m) | 15.8 | **526.5** | 384 | yes | **CONFIRMED** |
| clock spread, std (m) | 7.64 | **98.40** | 118 | yes | **CONFIRMED** |
| `IONO_DW` transitions / epoch | 1 | **38** | — | 19× baseline max | **CONFIRMED** |
| chord births / slip refloats | 658 / 641 | 1198 / 1184 | 3.5 / 3.8 | **no** | quiet |
| deweight *level* (`n_dw_engaged`) | 53 | 63 | 3.6 | yes but z<4 | quiet |
| **λ_min of N_ss** | 0.2245 | **0.2184** | −0.3 | **no** | **quiet** |
| condition number | 1.56e7 | 1.61e7 | 0.5 | no | quiet |
| smallest-mode mass on clocks / SH / PB | .0153/.0137/.9706 | .0152/.0141/.9702 | ~0 | no | quiet |

`n_present` 303 and `n_clock_sats` 32 are **constant** through the onset — no station
or satellite loss (confirms handoff §4.4 in the new channels).

**By the spec's criterion Q1 is CLOSED**: the re-tap count spikes and a deweight wave
lands. But the useful finding is the row that did not move.

### 4.2 The near-null slide hypothesis is FALSIFIED [M]

`REPORT §3.2`'s working hypothesis was that an epoch's re-tap/release pattern
momentarily *releases a weakly-resisted joint clock↔iono↔field mode*. It does not.
Across the onset, λ_min is on a **smooth monotone decline with no kink**:

```
0.2496  0.2426  0.2362  0.2299  0.2245  0.2184  0.2124
                                        ^ONSET
```

λ_max is equally smooth (3.4889e6 → 3.5055e6 monotone), and the smallest mode's
family composition is constant to three decimals — it remains a **phase-bias** gauge
direction (mass 0.970 on PB, 0.015 on clocks, 0.014 on SH) before, at, and after the
onset. The reduced normal matrix is built from **geometry and weights**, before any
observation *value* enters. **It does not know anything happened.**

(λ_min does jump — 0.212 → 0.629 — two epochs *later*, when the deweight wave's new
weights land. Conditioning changes as a consequence of the event, not before it.)

### 4.3 What that leaves: the trigger is on the RIGHT-hand side

Every confirmed channel is a **post-solve statistic of the onset epoch itself**, and
each acts only on the *following* epoch:

* the D5 innovation gate runs at `SB_INSTALL`, after the solve; its flags feed the
  **next** epoch's slip funnel (its own run-end line says so);
* the deweight gate's `advance()` runs at `SB_T1A`, after the solve, and its weights
  are read by the **next** epoch's parallel assembly (the threading contract in
  `NetworkIonoDeweightGate.hpp`);
* σ̂0, worst-clock-deviation and clock-spread are all posterior statistics.

So these five channels establish *what the onset epoch looks like*; none of them can
have *caused* it, and — consistent with §3.1 — **none shows a precursor at t−1**.

Combining that with §4.2: the normal matrix is unchanged, yet the solution moves by
526 m on the worst clock in one epoch. A solve can only do that if the excursion
enters through the **right-hand side** — the innovations — not through conditioning
or a released null direction. That is the actionable redirection this phase buys:

> **The next instrumentation pass must look at the RHS (per-station / per-LOS
> innovation aggregates at onset), not at the spectrum.** The spectrum is now
> measured, and it is exonerated.

### 4.4 The deweight release wave is real, and it is a follower

The spec asked whether "a deweight release wave landed". One did, unmistakably:
**38 transitions in the onset epoch (24 engage + 14 release) against a baseline
median of 1 and a baseline max of 2**, followed by 11 more releases the next epoch.
`n_dw_zero` — stations whose entire SH block is dropped and whose per-LOS iono is
floating — **falls 45 → 28**, i.e. 17 stations that had been floating for hours are
re-coupled to the field.

That is precisely the §3.2 candidate mechanism. But the gate computes those weights
*from the onset epoch's already-collapsed residuals*, and they take effect at
286350. **The wave is a response, not a trigger** — and the re-coupling it causes
lands one epoch into the collapse, which is a plausible contributor to the *shape* of
the recovery (§4.2's λ_min jump two epochs later) rather than to its onset.

### 4.5 The RHS excursion is GLOBAL, not a station or satellite blunder [M]

§4.3 concludes the trigger is on the right-hand side. One RHS channel is already in
the leg's traces (`network_vtec_sh_health.csv`), and it localizes the perturbation:

| tow | slant resid mean | rms | max | LOS >3σ | **stations >3σ** | a00 σ |
|---|---|---|---|---|---|---|
| 286290 | 10.28 m | 14.8 | 99.7 | 1380 / 1881 | 290 / 303 | 0.622 |
| **286320** | **297.1 m** | **1815** | **74 430** | **1833 / 1877** | **303 / 303** | **0.623** |
| 286350 | 363.8 | 1771 | 67 800 | 1843 / 1874 | 303 / 303 | 0.625 |

The SH↔slant-iono residual jumps 29× in the mean and 122× in RMS at the onset epoch,
and **every one of the 303 stations exceeds 3σ** (up from the chronic 290/303
background documented in `NetworkIonoDeweightGate.hpp`). `n_los` is unchanged
(1881 → 1877) and `a00_std` is frozen at 0.622 → 0.623 across a +1236 TECU step —
reproducing §3.1's finding in this run.

This is still a *post-fit* statistic, so it does not establish causality. What it does
establish is **spatial extent**: the RHS perturbation is network-wide and simultaneous,
not a blunder on one station, one satellite, or one arc. That rules out the whole
"single bad observation" family of triggers and is consistent only with a genuine
common-mode excursion of the clock/iono system — the same object Phase 2.1's detector
watches. (At 07:32 the *median* |Δclock| is 5.03 m and two-sided, so the Phase-2.1
guard correctly would NOT fire here — and this event did no lasting harm.)

### 4.6 P1-5 / handoff Q6 — DD fractional convergence, now measurable [M]

`network.trace_dd_fractional` was off on the reference run, so mean|f| and circR
"cannot be computed" (handoff Q6). Armed on this leg, 162 473 sampled DD pairs
(500/epoch cap, deterministic subsample):

| signal | n | mean\|f\| (cyc) | circR |
|---|---|---|---|
| L2W | 66 030 | 0.2504 | 0.0044 |
| L1C | 65 984 | 0.2495 | 0.0047 |
| L5Q | 10 632 | 0.2512 | 0.0061 |
| L1W | 9 790 | 0.2512 | 0.0059 |
| **ALL** | **162 473** | **0.2502** | **0.0010** |

mean|f| = 0.2502 is the exact expectation for a **uniform** fraction on [−½,½]
(E|U| = 0.25), and circR = 0.0010 sits *below* the n = 162 473 null expectation of
√(π/4n) ≈ 0.0022. The DD ambiguities carry **no integer information whatsoever** —
and the distribution is flat across the collapse (half-hour bins: circR 0.0063 /
0.0040 / 0.0069 / **0.0058 (onset bin)** / 0.0033).

This **confirms**, on the continuous reference run's own frame and with a clean
sample, what the DD-frac trail already established for non-rigorous configurations.
It is a corroboration, not a new discovery — but Q6 is now answered with a number
instead of "unmeasurable", which is what the spec asked for.

### 4.7 The Q2 ratchet mechanism, tested on a SECOND event [M]

The leg emitted its own products, so the §2 mechanism can be re-tested on 07:32 —
the event Phase 2.1's detector deliberately does *not* fire on. G01's deviation from
the constellation median of total IF (the datum-invariant quantity):

| tow | consumer IF(C1W,C2W) | pinned IF(C1C,C2L) |
|---|---|---|
| 286290 (t−1) | −7.55 | −2.79 |
| **286320 (onset)** | **−7.11** | **−6.56** |
| 286350 | +0.51 | 0.00 |
| 286440 (peak) | **+4.73** | **+6.56** |
| 286530 | −1.05 | +1.16 |

Level bands: pre-onset median −5.65 m; end of leg, ~3 h later, **−6.40 m**.

So the two-sided event produces a ~12 m transient that decays within ~7 epochs and
leaves **under a metre** of persistent offset. Against the one-sided 20:47 event's
+49 m step still sitting at +42.7 m at end of day, that is a factor of ~50 in
persistent damage between two excursions whose *peak* magnitudes differ by only ~10×
in the other direction (07:32 had the larger a00 step of the two).

**This validates the discriminator Phase 2.1's threshold is built on, on both
events, from a freshly instrumented run:**

| event | median \|Δclk\| | max/median | guard | outcome | correct? |
|---|---|---|---|---|---|
| 07:32 | 5.03 m | 105 (two-sided) | **silent** | ~12 m transient, <1 m persists | **yes — nothing to guard** |
| 20:47 | 4581.9 m | 1.23 (one-sided) | **fires** | +49 m, τ = 3.8 h | **yes — the real fault** |

It is the strongest statement available about the detector short of running it ON:
both the fire case and the no-fire case are correctly classified, and the no-fire
case is independently confirmed to have needed no fire.

### 4.8 P1-4 earns its place

`sigma0_pf_code` goes 2.92 → **10.20** at onset (3.5×, z = 219) and stays elevated
for ≥5 epochs. Before this card the emit path had **no** post-fit statistic — that
column read 0.0000 on all 2160 rows of the reference run, which is why a −4500 m
clock epoch shipped `gate_ok=1`. A gate on this statistic would have caught the epoch.
Wiring one is a Phase-2 decision, deliberately not taken here (the A4 ceiling still
reads `sigma0_adapt`; P1-4 is measure-only).

---

## 5. Standing-invariant pre-flight, as executed

1. **zero-q / stiff event response** — Phase 2.1 is exactly this item for the CB
   carry. Built default-OFF; not yet field-armed. ✔
2. **Release-only, renamed exe** — every run used
   `gnss_processor_{opus_run,p1leg,p21}.exe` from `build/x64-Release`. ✔
3. **Deferrals triaged** — 20:47 reproduction under instrumentation is *field-inert*
   for the trigger question (07:32 serves, and Phase 0 has now measured the 20:47
   consequence directly). D3 pin re-point deferred by decision. q_cb retune rejected.
   **Field-blocking deferral, stated plainly: Phase 2.1 has passed its OFF gate but
   has never run ON. It is not field-validated and must not be treated as a working
   guard until a run reports non-zero trips/reinits.** ✔
4. **Run-end counters** — added for every new mechanism (P1-3 traced epochs, P1-4
   pooled epochs + row counts, Phase-2.1 trips/reinits/bumps/non-pivot), each with an
   explicit inert-case WARN. ✔
5. **Datum-invariant grading only** — §1.3 grades deviation-from-constellation-median
   of total IF; §1.2 grades consumer position against a fixed truth, where a truth
   error is common-mode across arms. ✔
6. **Default-OFF + byte-identity** — P1-3/P1-4/Phase-2.1 all default OFF; fingerprint
   entries conditional-appended. Byte-identity **measured** same-build against both a
   pre-2.1 and a pre-Phase-1 binary: products/solution/trace identical (§3.2). ✔

## 6. Phase-2 arming pass (Jul-27) — build record

D2's condition (the Phase-1 trigger verdict) is in, so the arming chain is
sanctioned.  Two builds landed first, both default-OFF:

**Phase 2.2 — SH health gate** (`network.sh_health_gate`, `sh_gate_min_epochs`
= 1, `sh_gate_rms_max_tecu` = 0 ⇒ a₀₀-band-only).  Gate-not-warn rework of the
T1a probe per spec §2.2 / handoff §7.  The verdict is computed POST-SOLVE in
`NetworkSolveGlue` (the only pre-emit seam holding `x_s` — `NetworkShHealth`
runs after the emit) and enforced by early-returns in `NetworkSolPrep` +
`NetworkEmitStore`; disposition `sh_gate_withheld`; H-line state `withheld_sh`.
**Deliberate divergence from the A4 withhold semantics: emission-only.**  The
A4 path skips the carry stash too ("the contaminated solve never enters the
carry"), which is right for 1–3-epoch σ̂0 transients but would EXPIRE the whole
carry (`phase_leveling_max_gap_s` = 900 s) across an SH era like the 80.5-min
07:32 block.  The SH gate therefore withholds the wire while the solve, carry
and every QC tier continue — and for the same reason it is NOT in the resume
fingerprint (the `raim` class, not the `cb_excursion` class).  Out-of-band
EPOCHS are counted un-throttled (the old WARN under-read the reference run
23.8×), with engage/release `SH_GATE` events and run-end totals + an
armed-but-untested WARN (invariant #4).  Pending disc bumps survive a withheld
era untouched and land with the next actual emit — the Card-2
value-step/signal coincidence rule holds because `NetworkSolPrep` (the
present/pending split) is skipped whole.

**§4.3 RHS rider — keyed post-fit trace** (`network.trace_postfit_keyed` →
`network_pf_keyed.csv`).  The P1-4 residual pass additionally pools
|v|√w per STATION, per SATELLITE and per SIGNAL ({code, phase} pools; σ̂0 =
1.4826·median, rms, max, n per key per epoch), the sat/signal read from the
shared column labels the pass already fetches.  This is the §4.3 localization
instrument riding the Phase-2 leg for free: which stations (geography ⇒ iono
front vs global), which satellites (orbit/clock input), which signals
(dispersive split) carry the onset misfit.

**Gates re-run for both:**
* OFF-path byte-identity — same 20-station × 11-epoch cold leg as §3.2, new
  binary vs the frozen Jul-26 Phase-2.1 binary: `products.txt`,
  `network_solution.csv`, `network_trace.csv`, `network_epoch_status.csv`,
  `network_events.csv` all **IDENTICAL** (SHA-256).
* `gnss_tests` 887/888 pass (the 1 skip is the pre-existing
  `FixtureSmokeTest.Sp3HeaderTimeConversionMatches` data-fixture skip).
* Keyed trace smoked on the 20-station fixture: STA/SAT/SIG rows live, phase
  pools ≈ 0 exactly as §2.2 predicts.

**Armed-config smoke [M]** (10 epochs, 305 stations, cold at 13:00, full
arm-together set): exit 0, 0 errors/criticals, **30.7 s/epoch** — the full
guard set is CHEAPER than the 41.2 s/epoch reference run.  `sigma0_adapt` goes
live at exactly leveling-warmup + 1 (epoch 7), so the A4 ceiling is armed on a
real statistic.  Both new run-end telemetry lines fire their quiet-window WARNs
(invariant #4 wiring live).  One arithmetic gotcha found and fixed: a bare
`start_offset_hours` = 13.0 sits ON the double-precision residue boundary and
the strict `<` skipped the 13:00:00 epoch — cold seeds need the same
half-epoch back-off (offset − 15 s) `chain_make_leg.py` applies to resumes.

**The arming chain** (`scripts/p2_run_guards_chain.ps1` →
`runs/day129_guards/`): 6 h guarded seed 13:00→18:59:30 COLD (fresh
convergence because `phase_leveling` / `emit_ura` / `code_bias_disc_jump_test`
are fingerprinted; `cb_excursion_reinit` is conditional-fingerprinted so seed
and leg both carry it), then the leg 19:00→23:59:30 via `chain_make_leg.py
--base config/test_suite/_day129_guards_conv.json --frame-guards`, covering
the 19:21:30 excursion (median 77 m ⇒ the guard TRIPS — the harmless-reinit
check), the 20:47:00 excursion (THE validation event), and the end-of-day
re-track tail.  Seed placement at 13:00 rather than 00:00 keeps the whole
chain ≈ 13 h instead of ≈ 29 h; the events are data-driven (both reproduced
under the Phase-1 leg's instrumentation), so the shifted convergence history
changes the carry age at 20:47 (1.8 h vs the reference's 14.8 h) but not the
excursion itself — worth remembering when comparing ratchet magnitudes.
Verdict tooling: `scripts/p2_gate_verdict.py` (gates ii/iii + SH-gate/A4 reads
+ keyed-RHS robust-z at each onset), cross-checked against `runs/p1_leg` where
it reproduces §4.7's numbers.

### 6.1 Chain results (recorded Jul-28 by the s2z card's D-A grading pass)

Chain COMPLETE: seed 720 ep cold 13:00→18:59:30 rc=0; leg rc=0 through
tow 345570 (23:59:30), `warm=1 cold=0, snapshot age 30.0 s`, 0 error/critical.
Leg emitted **322 of 600** epochs — 278 `sh_gate_withheld`.  Verdict
(`scripts/p2_gate_verdict.py --legdir runs/day129_guards/leg01`):

* **Gate (ii) PASS — but not on the validation event.**  5 trips / 5 reinits /
  1 bump landed: 19:00:30 (median |Δδtˢ| 172.37 m), 19:01:00, 19:05:00,
  19:34:30, 19:35:00 (+ a CB_EXC marker at 20:59:30).  Every trip is
  resume-shock / recovery chaos, not an excursion.
* **The 19:00 warm-resume seam is NOT inert on this lineage.**  The leg's
  first-epoch diagonal-only restore blew a00 to **1410 TECU**; the SH gate
  engaged at 19:00:00 and first released at 20:59:30 — the wire received
  NOTHING for the entire two-hour window containing both excursions (it then
  chattered through 4 more engage/release pairs 21:55–22:43).
* **Neither evening excursion expressed on this arm** [M]: at 334020 the
  epoch-status clock-median delta is **+0.31 m** (reference run: median
  |Δclock| 4581.9 m) and σ̂0_pf_code reads 32.77 vs its own pre-onset median
  68.19 (z ≈ −1); at 328890 the deltas are a diffuse −5…−8 m/ep drift.  The
  filter state that the 19:21/20:47 data hit (cold 13:00 seed + seam shock +
  5 CB reinits + two hours of SH-gate era) does not produce the reference
  excursions.  **Consequence: gate (iii) is VACUOUS on this leg** (no emitted
  pre-onset epochs to band against; the script skipped both events), and the
  ON-path validation of the 20:47 guard-fire remains OPEN.  The event's
  expression evidently depends on the accumulated filter state, not on the
  observation data alone — the "data-driven" claim in §6 held for the 00:00
  lineage (p1_leg) and does NOT transfer to a 13:00-seeded chain with a
  19:00 seam.
* Keyed RHS at the onsets (for the record): 19:21:30 localizes to an
  Australian-sector STA panel (MOBS z=13.4, KARR 9.1, MRO1 6.3, YARR 5.5 —
  plus LROC/DAE2/LPAL/OP71 5–9); 20:47:00 shows NO station above z=1.8 on
  this heavily-intervened arm.
* σ̂0_pf_code ran 33–90 across the leg (p1_leg quiet baseline: ~2.9) — the
  code pool residuals are dominated by the sick SH field of the withheld era,
  consistent with the gate's own engage statistic.

The A4 ceiling never fired (max σ̂0_adapt 1.00 — within-epoch RAIM's adaptive
statistic stayed clean while the SH gate held the wire).  Follow-up owed to
this card, not taken here: a seam-hardened arming pass (e.g. gate warm-up
exemption at resume, or a 00:00-anchored guarded chain) so the 20:47
guard-fire validation can actually run.  Recorded by the SPEC_s2z_realization_ab
pre-flight; that card's arm drops the 19:00 seam entirely (one continuous
13:00→23:59:30 segment) for exactly this reason.

### 6.2 The seam ratcheted the WIRE — cross-seam grading, D1 falsified, follow-ups [M]

§6.1 reads the internal state (the events did not express; gate iii vacuous).
This section grades what the leg actually SHIPPED after the withheld era, and
it is the material half of the verdict: the resume seam inflicted the exact
§Q2 disease — network-wide, at 2–4× the reference magnitude.  Between-sat
total-IF deviation from the constellation median (datum-invariant, therefore
valid ACROSS the seam against the seed-era baseline):

| quantity | seed 18:00–19:00 | first post-release ½ h | EOD 23:59:30 |
|---|---|---|---|
| G01 deviation | −5.3 m | **+117.4 m** | +51.4 m |
| G07 deviation | (quiet) | **−215 m** (21–22h median) | −94.8 m |
| G30 deviation | (quiet) | +115 m | +41.9 m |
| robust between-sat spread (1.4826·MAD) | 7.9 m | 23.4 m | 20.0 m |

G07's decay −215 → −94.8 m over 3 h gives **τ ≈ 3.7 h — the q_cb = 1e-10
bleed constant (§Q2's 3.79 h)**.  Attribution closed: the seam's repeated
one-sided excursions ratcheted EVERY satellite's near-static CB carry during
the two-hour withheld era; the wire then shipped ±100–215 m multi-satellite
levels from 20:59:30, bleeding for the rest of the day.  The pivot-only
guard reinitialized exactly one of the victims — and its own run-end D1
counter quantifies the miss: **130 (satellite, epoch) non-pivot cases a
per-satellite rule would have reinitialized.  D1 (pivot-only) is
DATA-FALSIFIED as sufficient for seam-class events; D1b (all-sat) is the
indicated scope.**

Two instrument findings with design weight:
* **The A4 statistic is structurally blind to this fault class.**  σ̂0_adapt
  pools only carried-ambiguity PHASE rows; it sat at its 1.0 floor through a
  seam whose damage lived entirely in code/CB space, while σ̂0_pf_code (P1-4)
  ran 33–90 vs its 2.9 quiet baseline.  §4.8's case for gating on the code
  statistic now has an ON-path demonstration.
* **The SH gate's emission-only semantics are measured, and half-right.**
  Deliberate choice (§6): withholding protected the wire DURING the era —
  but the carry kept evolving poisoned, so the wire ate the ratchet AFTER
  release.  The alternative (A4-style skip-carry) would have expired the
  carry into a cold restart.  Neither is right; the fix is upstream at the
  resume itself.

**Follow-ups minted (ranked):**
1. **F1 — a warm resume IS an invariant-#1 event.**  The stiff CB carry has
   no wired resume response.  `state_snapshot_resume_shock_m` (RE-2,
   default-off, unarmed here) applies exactly this via `applyResumeShock` —
   verify/extend its coverage from the DD z-store to the D2 CB/PB carries,
   size it empirically, and arm it on every future warm-resume chain.
2. **F2 — D1b all-sat CB reinit** on trip (130 measured non-pivot cases), or
   minimally seam-scoped all-sat.
3. **F3 — SH-gate policy:** resume grace / warmup-URA degraded posture
   instead of hard withhold during recovery, plus band low-edge + hysteresis
   re-derived from the reference day's a₀₀ distribution (the 2.0 TECU low
   bound sits inside the normal late-evening range — four chatter cycles
   measured 21:55–22:43).
4. **F4 — promote σ̂0_pf_code to a gating input** (A4 companion for CB-class
   faults).
5. **F5 — the s2z A/B gained a test event:** the seam is a reproducible
   one-sided common-mode source; `docs/SPEC_s2z_realization_ab.md` H1 now
   has an on-demand trigger (though its arm avoids the seam for its own
   validity — see §6.1).
6. **F6 — re-validation of the original 20:47 question** after F1–F3: a
   resume-shock-armed chain or a continuous guarded segment through the
   evening (§6.1's recommendation, seconded).

**Card status (invariant #3, plainly):** guard MECHANICS field-validated ON —
both guards fire, count, withhold, reinit, bump, and self-report honestly
over 13.2 h with zero errors.  Guard POSTURE as configured is NOT
field-ready: pivot-only scope under-responds to network-wide excursions, the
SH gate's availability cost at seams/band-edges is unacceptable, and the
20:47 protective claim is untested (the event does not express on this
lineage).  Phases 2.1/2.2 stay default-OFF.

## 7. What I would not claim from this

* Phase 0 grades a **float** consumer with AR off. It says nothing about integer
  recovery under the fault.
* ~~The NRMG truth is a RINEX header coordinate. Absolute levels carry that error;
  only the A/B contrast and the time structure are argued from.~~ **Retired
  Jul-27** — §1.0 replaces it with the ITRF2020 monument coordinate (σ ≈ 2 mm)
  and re-anchors every band. Absolute levels are now quotable. The residual
  caveats are the ordinary ones: single realization, one station, one day.
* §1.5's mixed-pair mechanism is a hypothesis. The measurement (it is worse) is solid;
  the explanation is not tested.
* ~~Phase 2.1 has never run ON.~~ **Superseded Jul-28** — it has now run ON
  (§6.1/§6.2): the detector fires, reinits and bumps land, and the run-end
  telemetry is honest.  What is still NOT claimable: that the guard protects
  against the 20:47-class data event (the event does not express on this
  lineage — §6.1), and that pivot-only scope suffices (falsified — §6.2, 130
  non-pivot cases).  The armed POSTURE is not field-ready pending F1–F3.
* The §6.2 seam-ratchet grading compares a 13:00-seeded guarded lineage
  against its own seed baseline; magnitudes are not directly comparable to
  the reference run's (carry age 1.8 h vs 14.8 h at the era, τ = 3.79 h not
  yet at steady state).  The τ-match attribution and the D1 counter do not
  depend on that comparability.
