# DEC-day129-gate-policy: the SH gate-policy rulings (degrade, do not withhold)

**STATUS**

- **id:** DEC-day129-gate-policy
- **topic:** raim-qc
- **status:** superseded
- **summary:** Six rulings (degrade, not withhold, under a 120-epoch cap; rms_ho ratio gate; state-coupled CB window), none built. Superseded: the re-founding drops SH from the producer and cuts seams and the D5 chain.
- **dates in the text:** none (the source's Part D preamble dates the six rulings 2026-07-28)
- **knobs / posture:** `network.resume_grace_epochs` (cap, 0 = off), `network.resume_grace_ura_floor_m` (un-derived), `sh_gate_ratio_engage` / `_release` / `_median_tecu`, the `cb_excursion_suppressed_resume` action; none exists in `src/` or `include/`, and the D-G4 conditional fingerprint append is not implemented
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/day129_guards`, `runs/day129_cont`, `runs/bytecheck`
- **source:** `docs/archive/SPEC_day129_frame_guards.md`: Part D (D-G1 to D-G6), Part E (E1, E2, E3, E4)

**Outcome.** What should the SH health gate do at a warm-resume seam, after its a₀₀ band withheld 239 consecutive epochs ([DOY-129 defects](../cards/raim-qc/DAY129-DEFECTS-ABC.md))? An absolute a₀₀ band does not transfer between realizations (about 25 TECU apart), so the rulings degrade instead of withholding under a 120-epoch cap and gate on an rms_ho ratio. Nothing was built (E4 floor un-derived). Superseded: [the re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md) drops SH from the producer and cuts seams and the D5 carry chain, so the rulings and Part F are moot; `network.sh_health_gate` stays KEEP_OFF in the [ledger](../CONFIG_promotion_ledger.md). Status reviewed 2026-10-13.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_day129_frame_guards.md -->
## D-G1 — resume grace: DEGRADE, DO NOT WITHHOLD

**Intent.** During a bounded post-resume recovery, ship the epoch with an honest DEGRADED quality
advertisement instead of shipping nothing. C2 forbids shipping it as good; C6 shows **no
threshold choice makes the era short**.

### The seven MUSTs

1. **Grace changes the DISPOSITION, never the VERDICT.** The gate statistic, its out-of-band
   determination, the `sh_gate_oob_*` counters and the `SH_GATE` events MUST be computed and
   recorded identically whether or not grace is active — a grace that suppresses the verdict
   destroys the record that the era was bad and lets a real divergence ride out of the window
   unnoticed.
2. **A third disposition.** `sh_gate_degraded` MUST be distinguishable from both `emitted` and
   `sh_gate_withheld`, and the product H/S line MUST carry a **machine-readable degraded marker**
   (not only an inflated σ) — a consumer that ignores σ must still see it. Grace epochs MUST have
   their own run-end counter, separate from `n_sh_gate_withheld` (invariant #4).
3. **Degrade EVERY quality channel.** Per C5(4) the URA floor alone leaves `sigc`/`sigcc`/`rho` at
   full weight, and `sigcc` is exactly what M4 built for a σ-weighting consumer: a degraded epoch
   MUST floor **all** of `clock_ura_m`, `sigc_m`, `sigcc_m` (`NetworkSolPrep.cpp:440-441` and
   `:834-900`), and any channel left unfloored MUST be named in the report as a known honesty gap.
   **D-G6 = SHIP THE PIVOT WITH A NOTE (ruled).** The datum-pinned pivot is skipped by the URA path
   by design (`NetworkSolPrep.cpp:425` — its variance is the artificial pin), so **its corrections
   ship during grace with NO per-satellite quality signal**; withholding them was rejected (a
   consumer that loses the pivot loses the datum). Binding: (a) the report MUST name the pivot
   (**G01**) and note it is *the satellite whose code bias ratchets* (A2) — the one correction a
   grace consumer cannot de-weight; (b) MUST-2's marker MUST be **epoch-level**, not only per-sat σ
   inflation, so some signal covers the pivot; (c) the run-end line MUST report how many grace
   epochs shipped pivot corrections.
4. **Resolve the deadlock STRUCTURALLY.** The grace decision MUST be evaluated where both
   `NetworkSolPrep` and `NetworkEmitStore` can see it and **before** the `NetworkSolPrep.cpp:140`
   early-return, so `n_emitted` (`:367`) and the ramp-lift rule (`:398`) advance on grace epochs. A
   grace that leaves `n_emitted` frozen re-creates C5(1) elsewhere.
5. **Grace is BOUNDED at 120 epochs and its exit is data-driven — D-G2 = CAP 120.** Measured
   recovery was **239 epochs by a₀₀ and 400–542 by field shape**, so the cap is deliberately
   *shorter* than the observed recovery: **a cap long enough to cover it is indistinguishable from
   disarming the gate.** Grace MUST end at whichever comes first: the gate statistic releases, or
   **120 processed epochs (1 h at 30 s) from the seam**. On reaching the cap while still engaged
   the epoch MUST revert to **hard withhold**, logged un-throttled, evented and counted. **The
   cap-reversion is a FIRST-CLASS DELIVERABLE, not an error path** — *"this resume did not
   re-converge within an hour"* is the most important thing the mechanism can tell an operator,
   and on this leg it is what would have fired. **A run that reverts MUST NOT be graded as a
   successful warm resume.**
6. **Grace arms ONLY at a resume seam** — the warm-resume event, not "the gate engaged". A gate
   engagement in steady state is a real divergence and MUST go straight to hard withhold. (Cold
   starts are covered by the existing RTB-7(b) ramp, which is *not* restored-as-lifted cold.)
7. **Default OFF, byte-identical when off** (invariant #6). **D-G4 = CONDITIONAL APPEND.** (a)
   `warmup_ura_floor_m` / `warmup_epochs` move to a conditional append gated on
   `warmup_ura_floor_m > 0` (today's unconditional site `src/core/app/NetworkRunContext.cpp:589`,
   the policy card's `NetworkRunContext.cpp:218`; copy the `:256-314`-class block) —
   **back-compat HYGIENE**, keeping every unarmed run's fingerprint byte-identical, including
   against snapshots written before the knobs existed. (b) It does **NOT** create an arming path
   (C5(3)), so **grace MUST NOT be implemented by re-arming the RTB-7(b) ramp**: grace gets its
   **own emission-only knobs, deliberately NOT in the fingerprint** — the same class decision the
   SH gate took (`NetworkRunContext.hpp:515-521`: "the raim / clock_runaway_check class, not the
   cb_excursion class"), legitimate because grace changes what is emitted and with what advertised
   quality, **never what a carried state means**. That is what lets grace arm on the **existing**
   `runs/day129_guards` chain with **no new seed**. It MUST NOT read or write `ramp_lifted` /
   `ura_ewma_updates` (snapshotted and fingerprint-coupled) — grace applies its own floor directly.

**CANDIDATE mechanism.** `network.resume_grace_epochs` (cap, **default 120**, 0 = off) +
`network.resume_grace_ura_floor_m` (+ MUST-3's `sigc`/`sigcc` floors), all outside the fingerprint.
Grace state lives in `NetworkRunContext` (`NetworkCarryState`), seeded at the warm resume,
decremented per processed epoch, **not snapshotted** (a resume of a resume re-seeds it — the
conservative direction). Floor value: reuse RTB-7(b) *semantics* (a per-satellite URA floor in m)
but not its state; **MUST derive it, do not guess it** (Part E). Grace MUST NOT extend the
`phase_leveling_max_gap_s` (900 s) exposure the SH gate was designed to avoid — since grace
*emits* it strictly **reduces** that exposure; one line in the report confirms it.

### The D-G2 cap decision table

Under the ruled statistic (D-G3, 4.0/2.5, dwell 3/10) the engaged era is **542 epochs, releasing
at ep 544**. Grace degrades the first `cap` epochs and hard-withholds the rest:

| cap | degraded | hard-withheld | emitted | cap-reversion fires? |
|---|---|---|---|---|
| 60 | 58 | 484 | 58 | YES |
| **120 — RULED (D-G2)** | **118** | **424** | **58** | **YES** |
| 240 | 238 | 304 | 58 | YES |
| 400 | 398 | 144 | 58 | YES |
| 600 (= no cap) | 542 | 0 | 58 | no |

At the looser 6.0/3.0 pair the era is **400 epochs and 200 epochs emit normally**; the same cap
sweep gives 58/118/238/398 degraded with 342/282/162/2 withheld. **This is the D-G2 evidence.**
Any cap ≤ 400 reverts to hard withhold on this leg, i.e. the run reports "recovery did not
complete" — the truth. A cap of 600 (no reversion) ships 542 degraded epochs from a field wrong by
up to 1150 TECU for its first two hours; the degraded advertisement would have to be extreme for
that to be honest.

## D-G3 — the statistic: `rms_ho` RATIO, asymmetry, hysteresis

### The five MUSTs

1. **The load-bearing statistic MUST be a field-SHAPE statistic, not a₀₀ alone.** Measured basis:
   C3 (a₀₀ is datum-coupled ⇒ invariant #5) and C6 (relaxing the a₀₀ low edge far enough to stop
   the chatter loses the 20:47 event; the shape statistic catches it as an **86–107-epoch era**
   instead of a 1-epoch blip). `rms_ho` (degrees ≥ 1) is the CANDIDATE; `rms_all` is acceptable
   with the noted a₀₀ contamination (a₀₀ = 35 TECU contributes **2.2 of a 3.8 TECU** quiet
   `rms_all`). **All ingredients already exist in `network_vtec_sh_health.csv`**
   (`rms_all_tecu`, `rms_deg0..15_tecu`, `kaula_ratio_deg*`) — no new statistic needs deriving,
   only selecting. The gate's own `sh_gate_rms_max_tecu` knob (`GnssConfigTypes.hpp:639`,
   **default 0 = inert**) is the existing hook.
2. **Thresholds MUST be ASYMMETRIC (engage ≠ release) AND TIME-HYSTERETIC (two dwell counters).**
   Today `sh_gate_min_epochs` (**default 1**) gates engagement only; release is instantaneous on
   the first in-band epoch (`NetworkSolveGlue.cpp:1276-1278` resets `sh_gate_oob_run` to 0). Both
   halves are needed: threshold hysteresis alone leaves the 4 chatter engagements, dwell alone
   leaves 3 (C6). Mirror `NetworkIonoDeweightGate`'s `sigma_hi_`/`sigma_lo_` naming and its
   forced-`lo < hi` load validation (`NetworkIonoDeweightGate.hpp:92-93`).
3. **The a₀₀ band is DEMOTED to a wide catastrophic rail, not deleted.** It is the only statistic
   that catches a **pure DC runaway** (a₀₀ → −91 TECU with a plausible field shape). MUST become
   asymmetric and wide enough not to fire on a realization offset.
4. **`sh_gate_min_epochs = 1` MUST NOT remain the default once release dwell exists** — a 1/1
   gate on a shape statistic will chatter on the release edge the same way.
5. **Counting stays UN-THROTTLED and epoch-based** (the deleted Phase-1 report measured the old
   throttled WARN under-reading the reference run **23.8×**). Engage / release / degrade
   transitions all MUST event.

### CANDIDATE numbers (derived below)

| knob | CANDIDATE | basis |
|---|---|---|
| statistic | `rms_ho` (deg ≥ 1 RMS) | C6; scale-free under the normalization below |
| engage | ratio > **4.0** × calibrated median | ref: catches all 3 events; leg: 1 engagement |
| release | ratio < **2.5** × calibrated median | ≈ 1.5× the converged spread |
| engage dwell | **3** epochs | kills 1-epoch blips; costs 3 epochs of latency |
| release dwell | **10** epochs (5 min) | merges the 4 chatter eras; < the 15–30 ep RS transient |
| a₀₀ rail | **[−60, +200]** TECU | leg healthy-depressed min −8.7; ref divergent −302/+2151 |

The a₀₀ rail is deliberately **~7× wider on the low side** than the 0/−5 the card's framing
proposed: at −5 it still fires 5 times on this leg (C6), and those epochs have a quiet `rms_all`
of 5.8–8.6, i.e. **it is firing on a level offset, not a defect.**

### ⚠ Three knob traps the ruling creates — MUST, all three

D-G3 selects a statistic the existing knobs **cannot express**, so a "reuse what's there"
implementation would silently ship a different policy.

1. **`sh_gate_rms_max_tecu` is NOT the ruled statistic and MUST NOT be reused for it.** It is an
   **absolute TECU ceiling on `rms_all`** — the RMS over all 256 coefficients *including* a₀₀
   (`NetworkSolveGlue.cpp:1245-1270`). The ruled statistic is a **DIMENSIONLESS RATIO** on the
   **degree ≥ 1** RMS. Ship new knobs (`sh_gate_ratio_engage` / `sh_gate_ratio_release` /
   `sh_gate_ratio_median_tecu` / engage+release dwell) and either retire `sh_gate_rms_max_tecu` or
   keep it as an independent absolute rail alongside the a₀₀ rail — **but do not overload it.**
2. **The calibration median is MANDATORY when the gate is armed, with NO default.** The
   threshold's meaning is `ratio × median`; a wrong or defaulted median **silently rescales the
   gate**. The loader MUST refuse to arm (hard error, gate disarmed with an explicit message —
   the pattern already used for `cb_excursion_median_m ≤ 0` at
   `GnssConfigLoadNetwork.cpp:1584-1590`) if `sh_gate_ratio_median_tecu` is unset or ≤ 0. It MUST
   also be echoed into `network_run_manifest.json` and the run-end line.
3. **The loader's arm/disarm rule INVERTS.** Today `sh_health_gate` is silently **disarmed** when
   the a₀₀ band is silenced (`low ≥ high`) *and* `sh_gate_rms_max_tecu == 0`
   (`GnssConfigLoadNetwork.cpp:1620-1639`), because band-only is the shipped design. Under D-G3
   the ratio is primary and the a₀₀ band is an optional backstop, so a **band-silenced config is
   LEGITIMATE and must arm**. New rule: arm iff the ratio pair + calibration median are valid; the
   a₀₀ rail and any absolute rms rail are optional; **warn (do not disarm) if all backstops are
   silenced.**

*(Computation detail, also MUST.)* The gate's existing loop already accumulates `Σ val²` and
picks out a₀₀ (`NetworkSolveGlue.cpp:1245-1254`), so **`rms_ho = √((Σval² − a₀₀²)/(n−1))` is
free**. ⚠ **Under Iono-2 dual-layer the subtracted a₀₀ MUST be the layer SUM** (as `a00_total`
already is, `:1257-1260`) and the coefficient count MUST span **both** layers, or the two shells'
degree-1+ content is mis-normalized.

### The empirical derivation — and the amendment it forces

*Per the MW-thresholds directive (memory `feedback_mw_thresholds_empirical`): thresholds come from
the data via MAD / Welford / EWMA over a peer population, never from a stochastic-model σ.*

**The reference day's a₀₀ distribution**
(`runs/day129_cont/seg01/logs/network_vtec_sh_health.csv`, 2160 epochs, 06:00–24:00):

| statistic | a₀₀ (TECU) |
|---|---|
| min / q0.001 / q0.005 / q0.01 | −302.61 / −133.88 / −23.69 / −19.79 |
| q0.02 / q0.05 / q0.25 | −5.57 / 20.99 / 31.50 |
| **median** | **37.25** |
| q0.75 / q0.95 / q0.99 / max | 43.51 / 373.46 / 1538.22 / 2150.98 |
| robust σ (1.4826·MAD) | **9.06** |
| fraction < 2.0 (current low bound) | **2.41 %** |
| fraction < 0 / > 150 | 2.36 % / 7.50 % |

Quiet-hours-only subset (hours 10–18, 1080 epochs, no known event): min **21.88**, q0.05 29.34,
median **37.49**, q0.95 44.85, max **51.21**, robust σ **4.27**.

**Result, and the amendment.** A band derived from this distribution — e.g. median ± 6·robust σ =
**[11.9, 63.1]** — would have withheld **~100 %** of the guarded leg's late epochs, whose a₀₀ runs
−8.7…22.7 with a *quiet* rms_all. The reference day's a₀₀ distribution is **not transferable to
another realization of the same day**: the two runs' a₀₀ levels differ by ~25 TECU because a₀₀
trades against the code-bias common mode. **An absolute a₀₀ band CANNOT be empirically derived in
a transferable way. That is a property of the QUANTITY, not of the derivation.**

**What IS derivable: a scale-free field-shape RATIO.** `rms_ho` calibration sets, all measured:

| calibration window | n | median | 1.4826·MAD | min … max |
|---|---|---|---|---|
| **guarded seed, converged tail (last 60 ep)** | 60 | **2.376** | 0.0532 | 2.251 … 2.483 |
| guarded seed, whole 6 h run | 720 | 2.277 | 0.4016 | 1.553 … 3.409 |
| reference day, quiet hours 10–18 | 1080 | 2.896 | 0.3151 | 1.841 … 5.774 |

The two independent converged runs agree on the **median to 22 %** (2.38 vs 2.90) but their
**robust σ differs 6×** (0.053 vs 0.315) — the 60-epoch tail measures *within-run
epoch-to-epoch stability*, not realization spread. Hence:

* **MUST: normalize by the calibrated MEDIAN (a ratio), NEVER by a robust σ (a z).** The median
  transfers; the σ does not. A ratio threshold of 4× is 4× on either calibration; a z threshold of
  20 means two different things (measured: the leg's healthy-looking tail is **z = 64–84** in
  seed-tail units, **z = 10–12** in ref-quiet units).
* **MUST: the calibration window is a CONVERGED, MULTI-HOUR window of the SAME CHAIN** (the seed's
  tail, or a prior converged leg) — **never a trailing window of the run being gated** (C6:
  trailing robust z = 0 engagements on a run that begins diverged).
* **MUST: the calibration median is recorded in `network_run_manifest.json`** and in the run-end
  line. A gate whose threshold depends on an off-line calibration constant is otherwise unauditable.
* **MUST: re-derive per chain, and re-state in every report that the constant is CHAIN-SPECIFIC**
  (the same discipline the CB-excursion threshold got).

**Ratio values at the three reference events, in ref-quiet units (median 2.896): 07:32 = 77.8×,
20:47 = 34.2×, 19:21 = 4.8×.** Converged runs sit at **1.0–2.0×**; the leg's end-of-leg tail sits
at **2.4–2.9×**. A **4.0× engage / 2.5× release** pair separates all three events from both
converged runs, and correctly classifies the leg's tail as *not yet converged but no longer
divergent* — the exact state grace exists to express.

**The a₀₀ rail, derived.** Rail bounds must exclude every *healthy* realization and include every
*divergent* epoch. Healthy: ref quiet **[21.9, 51.2]**, leg healthy-tail min **−8.7**. Divergent:
ref min/max **−302.6 / +2151.0**, leg min **−1147.2**. Widest healthy → nearest divergent margin:
−8.7 → −19.8 (ref q0.01). CANDIDATE rail **[−60, +200]** takes **~7× margin below the healthy
minimum** and still catches every divergent epoch measured in either run by **≥ 5×**. **MUST: the
rail is the BACKSTOP; if the rail is ever the only thing that fires, the report says so** — it
means the shape statistic missed a DC-only runaway and the statistic choice needs revisiting.

## D-G5 — the CB-excursion detector's resume window: STATE-COUPLED

### The six MUSTs

1. **The window is derived from the shock, not fixed at 1 — and the completed leg says the shock
   does not decay on a 10-epoch scale.** Measured from the FINISHED run (steady state = median over
   the last 120 epochs: `clock_std_m` **12.15 m**, `worst_dev_m` **30.78 m**): from-seam ep 0
   **371.1** m (30.5× steady, worst_dev 1264); ep 10 **398.1** (32.8×, 1767); ep 20 **469.4**
   (38.6×, 2110); ep 38 **586.2** (**48.3×**, 2680). The clock spread **GROWS** for ~40 epochs and
   only stays below 5× steady from **ep 177** (tow 332910), below 2× from **ep 329**. So a 20-epoch
   window suppresses the ep-1/+2/+10 trips (172 / 54 / 69 m) but **NOT** the ep-69/+70 trips
   (115 / 87 m at tow 329670/329700) — which fired while the field was still 45× steady and
   a₀₀ ≈ −1000 TECU, i.e. shock artifacts too. **A fixed epoch window cannot separate them without
   being ~70 epochs long, which is too blind for a guard.**

   **D-G5 = STATE-COUPLED (ruled).** The window is
   `post_resume AND (grace active OR SH gate engaged OR epochs_since_seam < floor)`, **floor = 20
   epochs**: the run already maintains an authoritative "not converged" state (D-G1/D-G3) and a
   second, independent epoch counter would inevitably disagree with it; the floor covers the case
   where the SH gate is off or releases early.

   **⚠ THE SAME-EPOCH ORDERING FACT that makes this lag-free (verified).** Within one epoch the
   driver runs `networkSolveGlue` (`ApplicationNetwork.cpp:1488`, where the SH-gate verdict is
   computed) → `networkInstallCarry` (`:1522`) → `networkSolPrep` (`:1535`) → `networkEmitStore`
   (`:1546`) → **`networkClockQc` (`:1585`)** → `networkStashStatus` (`:1603`), so the CB detector
   reads `ctx.carry.sh_gate_engaged` **already set for the current epoch** — no lag, no stale read.
   **MUST NOT** add a defensive one-epoch lag; **MUST** state this ordering dependency in a
   comment, because it **silently breaks if a future decomposition moves `networkClockQc` above
   `networkSolveGlue`.** **MUST: the window ENDS WHEN THE STATE CLEARS — it is NOT latched**; once
   grace has exited and the gate released, the detector is live again even if `epochs_since_seam`
   is still small (the floor is a minimum, not a maximum).
2. **Suppression means OBSERVE AND COUNT, NOT ACT.** Inside the window the detector MUST still
   compute its statistic, update `cb_exc_max_median`/`_tow`, and emit its event with a **distinct
   action** (e.g. `cb_excursion_suppressed_resume`), while performing **no** CB reinit and queueing
   **no** disc bump. A silent window would hide a real excursion landing inside it and make
   invariant #4's telemetry lie.
3. **A distinct run-end counter** (`cb_exc_suppressed_resume`) with the same
   armed-but-never-tripped WARN discipline the detector already has; the report MUST state how many
   trips were suppressed and what their statistics were, so the window's cost is re-derivable.
4. **The window MUST NOT be open-ended, and MUST NOT be re-armed by anything but a resume.** A real
   excursion at seam+21 must trip.
5. **The pending-bump queue's behaviour across a withheld/graced era MUST be re-checked under
   D-G1**, since grace changes which epoch is "the next emit". Today three reinits correctly
   collapsed to one bump landing 2 h later (C4); under grace the first graced epoch becomes the
   landing site, which is *better* (the bump lands nearer the value step) but MUST be confirmed.
6. **Rule on whether the window should also gate the CB reinit's σ** (`clock_monitor_reinit_sigma_m`,
   30 m). **Three reinits inside 10 epochs is not "reset once"; it is a ratchet of its own kind.**
   Out of scope to decide here, but the report MUST state whether repeated reinits inside a shock
   are a distinct defect. *(Recommendation: yes — record it, defer the fix.)*

---

# Part E — validation gates, implementation order, and the UN-DERIVED prerequisite

## E1 Validation gates — all MUST

1. **OFF byte-identity**, same-build, on the established **20-station × 11-epoch cold leg**
   (`runs/bytecheck/`): `products.txt`, `network_solution.csv`, `network_trace.csv`,
   `network_epoch_status.csv`, `network_events.csv` **SHA-256 identical** against the pre-change
   binary. New disposition/columns are appended-only (T2d precedent).
2. **Counterfactual agreement**: for the *shipped* policy, the off-line harness's predicted
   engage/release epochs MUST match the live run's `SH_GATE` events **exactly** on a replay of one
   converged window and one seam window. **A policy whose off-line simulation and on-line
   implementation disagree is not validated, whatever the counters say.**
3. **ON-path counters non-zero** for every new mechanism: grace epochs, cap-reversion events,
   suppressed CB trips (invariant #4 — 0 fired = inert = FAIL; the inert-case WARNs are retained).
4. **A re-run of the seam** (a fresh leg resuming the same `runs/day129_guards` seed snapshot —
   armable with **no new seed** per D-G1 MUST-7) MUST show, and the report MUST tabulate: grace
   engaged at **epoch 1**; `n_emitted` advancing through the grace era; floored `clock_ura_m` /
   `sigc` / `sigcc` **on the wire** (read back from `products.txt`, **not** the log); **the
   cap-reversion firing at epoch 121** — the cap table predicts 118 degraded + 424 withheld, so a
   re-run that does *not* revert means the cap is not wired; and the SH statistic's engaged era
   **identical** to the original leg's (D-G1 MUST-1). The CB detector MUST report **≥ 3 suppressed
   trips** (the seam+1/+2/+10 set) and MUST NOT suppress any trip after grace exits and the gate
   releases (D-G5 MUST-1's non-latching rule).
5. **Grading stays datum-invariant** (invariant #5): grade the degraded era's product with the
   **between-satellite total-IF deviation** methodology (Appendix), not raw a₀₀.
6. **No hard exclusion**: nothing here may drop a measurement (memory
   `feedback_prefer_fallback_over_drop`). Grace *adds* an emission path; the gate's withhold stays
   **emission-only** (solve / carry / QC continue).

**Harness.** C6's counterfactual tables and the D-G2 cap table are reproducible from an off-line
simulator reading `network_vtec_sh_health.csv` + `network_epoch_status.csv`. **MUST: promote it
into `scripts/`** (extending `scripts/p2_gate_verdict.py`) as part of the implementing card, and
**make the chosen policy's counterfactual a required table in that card's report.** *(The original
lived only in a session scratchpad and is presumed gone — regenerate it against the archived leg.)*
**A gate policy shipped without its counterfactual on BOTH a converged run and a seam-bearing run
is a hand-tuned constant with extra steps.**

## E2 Standing-invariant pre-flight (the policy)

1. **Stiff/zero-q event response.** Two stiff carries are touched. The SH carry is *the* stiff
   quantity in C2's mechanism — a garbage value entered it at the seam and ratcheted for 239 epochs,
   **invariant #1 in its purest form, a stiff carry with no event response for a RESUME event. This
   card does NOT fix that** (Part F); it makes the consequence honest on the wire. The CB carry's
   response (Phase 2.1) is narrowed by D-G5 so it does not fire on the shock itself. ✔
2. Release-only, renamed exe; name-scoped WER-zombie pre-kill; LNK1104 ⇒ retry. ✔
3. **Deferrals triaged.** *Field-blocking:* the seam shock itself (Part F) — the SH gate is **not
   field-runnable on a warm-resumed chain** until either grace ships or the shock is fixed, because
   ~58 % of a leg is withheld. *Field-inert:* the pivot's missing URA channel (C5(5)) — inert only
   if grace excludes the pivot or the report states the gap; the repeated-reinit question (D-G5
   MUST-6) — record-and-defer. ✔
4. Run-end counters for grace, cap reversion, suppressed CB trips, plus the existing
   armed-but-never-tripped WARNs. ✔ 5. Datum-invariant grading only — **and this card's central
   technical finding is an invariant-#5 violation in the *shipped* gate (a₀₀ is not
   datum-invariant).** ✔ 6. Default-OFF + byte-identity, conditional fingerprint appends. ✔

## E3 Implementation order for the owning session

A sequencing aid, not a design. Each step is independently gate-able; steps 1–3 are
byte-identity-safe when their knobs are off.

1. **The D-G3 knobs + loader** — ratio knobs, mandatory calibration median with a hard disarm, the
   inverted arm/disarm rule, `rms_ho` from the existing coefficient loop. Gate: OFF byte-identity
   (E1.1) + counterfactual agreement on a converged window (E1.2). *This step alone fixes defect B
   and can ship without grace.*
2. **Fingerprint hygiene** — conditional append for `warmup_ura_floor_m` / `warmup_epochs` (C5(3),
   D-G1 MUST-7). Gate: an unarmed run still resumes the existing `runs/day129_guards` seed.
3. **The D-G5 CB suppression** — state-coupled window + `cb_excursion_suppressed_resume`
   event/counter + the ordering comment. Gate: the seam re-run's ≥ 3 suppressed trips and the
   non-latching check (E1.4).
4. **D-G1 grace** — the emission-only path (own knobs, outside the fingerprint, never touching
   `ramp_lifted`), evaluated upstream of `NetworkSolPrep.cpp:140`, with the third disposition, all
   three σ floors, the epoch-level marker and the cap-reversion telemetry. Gate: the full E1 set,
   notably the cap firing at epoch 121.
5. **Floor calibration** (the E4 prerequisite). 6. **Re-run the arming chain** for Phase 2.1 gate
   (iii), still unmet (Part F).

## E4 ⚠ The UN-DERIVED prerequisite — the grace floor MAGNITUDE

**Not a decision; a measurement.** `resume_grace_ura_floor_m` (and the `sigc`/`sigcc` floors of
D-G1 MUST-3) is **un-derived**. It MUST come from an `ApplicationConsumerReplay` sweep (**~33
s/arm**, the cost figure from memory `project_consumer_replay_driver`) measuring **consumer
position error against floor value** over a degraded era — not from taste. The floor must be large
enough that a σ-weighting consumer effectively de-weights the era. **Until it is derived the
report MUST call grace "wired but UN-CALIBRATED", and the E1 gates MUST NOT be read as validating
the floor magnitude.**

---

# Part F — open / owed

1. **Phase 2.1 validation gate (iii) is STILL UNMET.** The 20:47 excursion fell inside the
   withheld seam era, and the leg does not reproduce the reference run's 19:21 event either (C4).
   The arming chain needs a re-run whose seam is either graced (D-G1) or placed far enough ahead
   of the events to have recovered. **Do not read gate (iii) off a run whose event sits inside a
   graced or withheld era.**
2. **⭑ The cheap zero-code mitigation: place the seed so the leg's first event is ≥ 400 epochs
   (3.5 h) after the seam.** Available today with no code at all.
3. **THE SEAM SHOCK ITSELF IS NOT FIXED BY ANY OF THIS.** C2's mechanism — the D5 innovation gate
   re-tapping **92 % of a restored DD carry** at the first post-resume epoch, collapsing the phase
   constraint and letting the SH field slide 1400 TECU — is an **ESTIMATION defect**, and it is
   the load-bearing one. Every number in Part C says so: **no emission policy makes that leg's
   product good**, because the field was genuinely wrong for ~3.5 of its 5 hours and had not
   re-converged at the end. That defect belongs to `docs/SPEC_warm_resume_event_card.md` (the RS
   card), whose "snap_wwork audit" is still open, and should be re-scoped with this measurement in
   hand. *(The gate policy also cited a `docs/REPORT_warm_resume_event.md` companion — that file
   does not exist.)*
4. **D-G4's conditional fingerprint append is NOT implemented** — `warmup_ura_floor_m` /
   `warmup_epochs` are still appended unconditionally at `NetworkRunContext.cpp:589`.
5. **The D3 IF-pin re-point** (A6) remains deferred: `selectIfPinPair` pins (C1C, C2L) while
   consumers apply (C1W, C2W). Fingerprint-breaking datum re-reference.
6. **Phase 0 / Phase 1 results are not recorded in this file** — the report that held them
   (`REPORT_day129_frame_guards_phase01.md`) no longer exists. What survives: the throttled-WARN
   23.8× under-count (D-G3 MUST-5), the `phase_leveling_max_gap_s` 900 s exposure argument (D-G1
   CANDIDATE), the chain-specific-threshold discipline (D-G3 derivation), and the "the gate's
   withhold is emission-only; solve/carry/QC continue" fact (E1.6).

---

# Appendix — traps this chain paid for

**⭑ The `start_offset_hours` half-epoch residue gotcha.** *(This is the target of
`src/core/app/ApplicationConsumerReplay.cpp:404`, whose "§6" pointed at the deleted
`HANDOFF_night305_day129.md`.)* ⚠ **RECONSTRUCTED, not quoted** — the source file is gone, so
what follows is re-derived from the rule as it survives in CLAUDE.md's "Cold-start hygiene" line
plus the chain's own practice; the *mechanism* sentence is inference, not a measured claim.
**The rule (supported): always take a half-epoch back-off on every `start_offset_hours`** (e.g.
`13.0` → `13.0 − 15 s` in hours), because bare hour values sit on the double-residue boundary.
**The mechanism (inferred, unverified): a whole-hour offset lands exactly on an epoch boundary
in a way that is ambiguous to both the resume arithmetic and the consumer replay's epoch
alignment, and resolves inconsistently.** Either way, assert `snapshot age 30.0 s` in the log
after a warm resume; any other value means the offset arithmetic missed.

**Grade only datum-invariant / between-satellite quantities.** The handoff's methodology, restated
because its file is gone: grade the **between-satellite total-IF deviation** (CLK + IF_W(CB) per
satellite, referenced to the constellation median), reported as **hourly bands — never slopes**.
Raw wire steps confound benign frame drift with real defects (invariant #5); A4 is the worked
example.

**Never point `state_snapshot_path` at an immutable seed** — it is read AND written. Copy the
immutable to a leg-owned snapshot path first.

Three traps stated in full elsewhere, listed here so a reader of the Appendix alone meets them:
**an empty counter is a wiring verdict, not a clean bill of health** (A7 — `n_adapt`,
`es.n_refloat_sats` and the `sigma0` emit column all read 0 for structural reasons); **threshold
constants are CHAIN-SPECIFIC** and must be re-derived and re-stated per chain (D-G3 derivation);
**the two hard pins are not symmetric with the other satellites** — G01 and MBAR00UGA cannot move
along the common mode, which is why the pivot absorbs one-sided excursions (A2.2) and why it has no
URA to degrade (C5(5)).
