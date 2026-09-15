# EV6 — the hour-6 Q3 mass-decouple event on RF-0hv: the decouple is REAL, its wire defect is REAL, and it is NOT the consumer's channel (2026-09-15)

**STATUS:** open — **a READ.  Nothing was built, no run was spent, no knob was armed.**  RF-0hv §7.3 recommended this as the next event
card and named the instrument it needed: *`CLK_ANCHOR` decouple SIZE vs consumer step, with a size-matched null*.  That instrument now
exists (`scripts/ev6_event_read.py`) and it **refutes the co-incidence it was built to confirm**.  The hour-6 step at TOW 276810 and the
ramp from TOW 279300 are both one epoch behind a Q3 clock-anchor mass decouple, but the decouple's own wire signature does not reach the
consumer: the decouple SIZE does not predict consumer damage (corr **0.135** over the run's 19 decouple epochs; count corr **0.212**), the
largest decouples in the run (23 and 21 satellites, max de-commoned wire excursion 1.33 and 1.26 m) are harmless, and at TOW 279300
**none of the 8 decoupled-and-declared satellites is in the consumer's sky at all** while only ONE satellite anywhere on the wire stepped
more than 0.20 m.  The emitted wire is in fact QUIETER during the consumer's worst stretch than during its best hour — cumulative
de-commoned IF-CODE drift over the ramp window (34 epochs, consumer 1.06 → 5.64 m) has a per-satellite maximum of **0.044 m**, against
**0.726 m** over a matched quiet hour-5 window where the consumer sits at 0.45 m.  What the read DID close is a different, real defect on
the same mechanism: **the Q3 withdraw-and-re-solve emits a one-epoch clock excursion, declares it, and then un-does it silently.**  Over
130 decoupled satellite-epochs whose declared step exceeds 2 cm, the median reversal at the very next epoch is **91 %**, 102 of 130 reverse
by ≥ 50 %, and **0 of 130 returns carries a declaration** — so ~90 % of every `R` record the Q3 anchor writes is a transient the producer
created, and the correction back to the pre-event value is invisible to the consumer's contract.  For the 276810 STEP the read also names
a candidate the decouple is not: that epoch is the run's **largest network-wide PHASE-BIAS re-realization** (19 satellites with a
de-commoned IF-phase step > 0.5 m, max 3.638 m, **13 of them undeclared**, against 12/5 at the next-biggest post-ignition epoch), it is
the run's worst `pl` and `pb` prior-residual epoch by two orders of magnitude, and it is the epoch the `PB:G23:L1W` column lost its last
two stations and the G23 phase-datum anchor was forced off STJO00CAN.  The 279300 RAMP is **NOT closed by the traces** and this card does
not claim it: the wire is quiet through it, and the missing evidence is named in §8.

## 1. The bed, the arm and what was read

The RF-0hv baseline pair, unchanged and not re-run (`docs/refound/RF-0hv_working_posture_2026-09-15.md` §5).  FIELD
`runs/_rfpdfix_field6_both` — DOY-129, 59 stations, GPS L1/L2, 6 h cold, TOW 259200–280800 at 30 s, exe
`build/x64-Release/gnss_processor_rfpdfix.exe` sha256[:16] `14578F851F0C22EC`, 720/720 emitted, consumer p50 by hour
5.02 / 2.11 / 0.97 / 1.05 / 0.45 / **3.65** m.  Everything below is read out of that run's `logs/` and `products.txt`.  Instrument:

```
python scripts/ev6_event_read.py <run_dir> [--tow-min T --tow-max T]
        [--section consumer|events|wire|census|undeclared] [--step-m M]
```

It is trace-only — it launches nothing, writes nothing but stdout, and does not plot.  Its one piece of algebra is standing invariant #5:
every per-satellite quantity is **de-commoned** by the median change over the satellites present at both epochs, because the
network-common part of a correction cancels in the consumer's own receiver clock and grading it would confound benign frame motion with a
real step.  The IF combinations are `-clk + a1·b(C1C) + a2·b(C2W)` (code) and `-clk + a1·B(L1x) + a2·B(L2x)` (phase), and a satellite
whose available signal pair CHANGES between two epochs is skipped rather than differenced — differencing two different linear functionals
manufactured 28–110 m "steps" out of nothing in the first cut of this reader.

## 2. (a) THE CONSUMER, per epoch

**The STEP.**  `closed_loop_trace.csv`, TOW 276780–276930 (3D / E / N / U in m, n_sats):

| tow | 3D | d3D | east | north | up | n_sats |
|---|---|---|---|---|---|---|
| 276750 | 0.470 | | +0.076 | −0.457 | −0.080 | 50 |
| 276780 | 0.509 | +0.039 | +0.084 | −0.490 | −0.112 | 50 |
| **276810** | **0.383** | −0.127 | +0.083 | **+0.002** | **+0.374** | 50 |
| **276840** | **0.989** | +0.607 | +0.115 | −0.301 | −0.936 | 49 |
| **276870** | **2.231** | +1.242 | −0.119 | −1.306 | −1.805 | 48 |
| **276900** | **3.156** | +0.925 | −0.242 | −1.893 | −2.513 | 48 |
| 276930 | 3.150 | −0.005 | −0.321 | −2.012 | −2.403 | 48 |

The 3D at 276810 is smaller than at 276780 and that is a coincidence of sign: N and U each move ~+0.49 m in that single epoch, which is
the step.  It then runs to 3.16 m in three epochs and **does not re-converge inside the hour**: 3.95 m at 277500, peaking at 4.01 m at 277530,
back to 1.34 m only by 279000 — a slow float re-convergence, not a recovery to the pre-event 0.45 m.

**The RAMP.**  Monotone from 279270, in E and U with N flat, peaking at TOW 280260 and then decaying:

| tow | 3D | east | north | up | tow | 3D | east | north | up |
|---|---|---|---|---|---|---|---|---|---|
| 279240 | 1.081 | +0.330 | −1.002 | −0.236 | 279900 | 4.394 | +3.028 | +0.597 | −3.128 |
| 279270 | 1.063 | +0.466 | −0.911 | −0.288 | 280110 | 5.315 | +3.585 | +0.828 | −3.835 |
| **279300** | 1.090 | +0.592 | −0.834 | −0.377 | **280260** | **5.636** | **+3.706** | +0.888 | **−4.152** |
| 279450 | 1.693 | +1.328 | −0.344 | −0.993 | 280530 | 5.291 | +3.305 | +0.624 | −4.084 |
| 279600 | 2.541 | +1.942 | +0.007 | −1.638 | 280770 | 4.683 | +2.850 | +0.282 | −3.705 |

E travels +3.24 m and U −3.86 m between TOW 279270 and the peak at 280260 — 16.5 minutes, 33 epochs; N moves +1.80 m.  n_sats is 48–49 throughout and climbs to 51 as the error decays.  There
is no consumer reset marker in the trace — `closed_loop_trace.csv` carries position and the AR columns only (`ar_gate` is `not_run` and
`ratio` 0.00 for the whole run; this consumer is FLOAT).

## 3. (b) THE PRODUCER EVENTS, per epoch, with identities

**TOW 276810** — every non-census row, in file order:

| tier | event | identity | statistic | detail |
|---|---|---|---|---|
| T0_band_scope | common_band | G03 / CHUR00CAN | | `no_exoneration; implicated=L1W;L2W; basis=mw flag; iftd silent; lli=none` |
| PBGAUGE | handover_continuous | G23_L2W / SAVO00BRA | 16.512172 | `from=STJO00CAN` |
| STRANSFORM | sts_applied | G23_L2W / SAVO00BRA | **−8.046610** | `from=STJO00CAN; basis=16.512172; prev=24.558782; **marked=0**` |
| CLK_ANCHOR | cta_decouple ×9 | G12 G13 G24 G10 G23 G26 (pass 0), G28 G31 (pass 1), G29 (pass 2) | see below | `reason=nis` |
| CLK_ANCHOR | cta_disc_landed ×9 | same 9 | | `disc_bump clock_anchor_decoupled` |
| PHASE_ANCHOR | anchor_handover | G23 / SAVO00BRA | | `STJO00CAN->SAVO00BRA` |
| T4_attrib | attrib_arc ×9 | G19/BSHM, G06/BSHM, G29/WIND, G12+G13/YKRO, G12/LMMF, G26/POVE, G23/STHL+CPVG+BOGT | 6.0 | `refloat σ̂0=1.00` |
| TOPOLOGY | col_removed | `PB:G23:L1W` | | `delta` |

The nine `cta_decouple` statistics, exactly as traced (`w = |e − med| / √S`, threshold `k = 6`):

| pass | sat | w | e (m) | med (m) | √S (m) |
|---|---|---|---|---|---|
| 0 | **G23** | **55.117** | **−0.1100** | 0.0042 | 0.0021 |
| 0 | G13 | 13.699 | +0.0361 | 0.0042 | 0.0023 |
| 0 | G10 | 10.242 | −0.0180 | 0.0042 | 0.0022 |
| 0 | G12 | 9.470 | +0.0278 | 0.0042 | 0.0025 |
| 0 | G24 | 7.940 | +0.0248 | 0.0042 | 0.0026 |
| 0 | G26 | 7.110 | −0.0100 | 0.0042 | 0.0020 |
| 1 | G31 | 9.379 | −0.0174 | 0.0023 | 0.0021 |
| 1 | G28 | 8.583 | −0.0164 | 0.0023 | 0.0022 |
| 2 | G29 | 6.060 | −0.0112 | 0.0017 | 0.0021 |

Three things are visible in that table and matter later.  (i) The denominator is **√S ≈ 2.1–2.6 mm** — this arm runs
`network.clock_anchor_sigma_floor_m: 0.0018` with `clock_anchor_q: 2.9e-8`, so the guard's trip band is **1.2–1.6 cm** of common-removed
clock innovation.  (ii) Only G23's innovation is large; the other eight are 1.0–3.6 cm.  (iii) The per-constellation median **moves**
0.0042 → 0.0023 → 0.0017 as satellites are removed, and the pass-1/pass-2 satellites are exposed only after the epoch has been re-solved
without the pass-0 priors.  The driver's own warning fired: `main_log.log:2434`

> `Network (Card Q3): TOW 276810.0 hit the 3-pass in-epoch refloat cap — any further clock-anchor trip this epoch is decoupled and marked
> but not value-corrected.  A run that reaches this cap regularly is telling you the anchor q or k is mis-specified, not that the data has
> that many faults.`

Over the whole run: **189 `cta_decouple` on 19 epochs**, of which 31 are non-statistical (`reason=datum`, all at TOW 276150) and the 158
statistical trips split **127 at pass 0 / 26 at pass 1 / 5 at pass 2** — so **20 % of the statistical trips are CASCADE**, exposed by the
withdraw of an earlier pass rather than by the data.

**TOW 276840** (the next epoch): S-transforms `G10_L1C −29.825` and `G10_L2W −54.179` from HOLB00CAN to BOGT00COL (both `marked=0`),
`cta_decouple` G06 (w 14.63, e −0.2471), G17 (10.26, +0.1879), G14 (11.00, −0.1837) at `med 0.0087 / √S 0.0175`, and **44 `T4_attrib`
arc refloats** against 9 the epoch before.

**TOW 279240–279330** (the ramp onset):

| tow | rows |
|---|---|
| 279240 | `PBGAUGE handover_continuous G22_L2W/CUSV00THA 46.359565 from=KIT300UZB`; `STRANSFORM sts_applied +14.362854 marked=0`; `PHASE_ANCHOR anchor_handover G22 KIT300UZB->CUSV00THA`; `READMIT park G16` (D2 clock + code-bias carry parked at departure); `TOPOLOGY col_removed CB:G16:C1W`; `T0_mw_detect lli_slip G28/CPVG00CPV` |
| 279270 | `T1_if_slip if_slip G06/HKSL00HKG 0.115 > 0.107` |
| **279300** | `CLK_ANCHOR cta_decouple ×8` — G04 (w 45.198, e +0.0913), G20 (25.143, −0.0692), G07 (22.358, +0.0420), G22 (21.625, −0.0614), G08 (15.931, −0.0440), G09 (15.104, +0.0267), G21 (7.228, +0.0066) at `med −0.0087 / √S 0.0021–0.0024` pass 0; G30 (6.840, +0.0072) pass 1; `cta_disc_landed ×8`; 7 `T4_attrib` arc refloats |
| 279330 | `T1_if_slip if_slip G06/ULAB00MNG 0.111 > 0.107`; `T0_mw_detect lli_slip G28/CPVG00CPV` |

**Station drops / re-admissions — asked first, and the answer is NO for both windows.**  All 9 `TOPOLOGY sta_pd_dropped`
(`no_pd_record`, the pre-existing non-finite-input guard) and all 11 `sta_readmitted` were listed with their TOWs: PARK00AUS 260940/267120,
GUAM00GUM 268290/272340, KIT300UZB 268620/280380, MAJU00MHL 276000/279690/280020, each with its re-admission one epoch later
(`epochs_absent=1; prior_rows=1; pre_pd_prior=1`).  **None sits at 276780–276840 and none at 279270–279330** — the nearest are 276000/276030,
13.5 minutes before the step, and 279690/279720, 6.5 minutes INTO the ramp.  A station dropping for one epoch and re-coupling with its
stale carry is not this event.

**Epoch status** (`network_epoch_status.csv`).  `n_present = 59` and `n_clock_sats = 31` at every epoch of both windows,
`datum_changed = 0`, disposition `emitted` throughout.  What moves is the post-fit PHASE σ̂₀ and the prior tension:

| tow | σ̂₀_pf_phase | σ̂₀_pf_code | n_arc_refloat | prior_tension_ratio | clock_median_m |
|---|---|---|---|---|---|
| 276780 | 0.577 | 0.610 | 1 | 8.3 | 1.9267 |
| **276810** | **1.266** | 0.698 | **9** | **14.1** | 1.8748 |
| **276840** | **4.318** | 0.969 | **44** | **62.4** | 1.9347 |
| 276870 | 1.228 | 0.718 | 5 | 9.5 | 1.9177 |
| 279270 | 0.683 | 0.764 | 0 | 3.6 | 1.9161 |
| **279300** | **1.436** | 0.833 | **7** | **5.9** | 2.0128 |
| 279330 | 0.676 | 0.774 | 0 | 3.6 | 1.9117 |

## 4. (c) THE WIRE — what was emitted, and whether it was declared

**The declaration channel is the `R` record, and only the `R` record.**  Measured on this run's `products.txt`: every `C` record has
3 fields (**no `clock_disc`, even though this arm sets `network.emit_clock_disc: true`**), every `B` record 4 (no `code_disc`) and every
`P` record 6 (no `phase_disc`).  The per-record `disc_counter` in `logs/network_solution.csv` is an internal trace — it also counts
code-bias re-realizations that never leave the producer (29 satellites bumped their `CB` counter at TOW 279240 with no `R` record at all).
`R` fires on **36 epochs / 206 satellite-declarations** in 6 h, and its population is exactly the 19 `cta_decouple` epochs plus the 9
`RETRO retro_bump` events plus 8 singletons.  So on this posture **the Q3 clock anchor IS the wire's discontinuity contract**.

**The decouple-epoch excursion is created by the withdraw, and is confined to the withdrawn satellites.**  De-commoned wire clock change,
TOW 276780 → 276810 (31 satellites in common, median change −0.08 cm):

| tripped (cm) | G23 −40.0 · G10 −12.6 · G26 −9.8 · G13 +6.9 · G31 −6.0 · G28 −5.2 · G12 +3.8 · G29 −3.8 · G24 +3.2 |
|---|
| everyone else | worst G11 +1.3, then ≤ 0.7 — **1 of 22 exceeds 1.2 cm** |

TOW 279270 → 279300 is the same shape: tripped G04 +26.8, G20 −13.3, G07 +13.5, G22 −10.8, G09 +10.3, G08 −7.7, G21 +5.6, G30 +3.6 cm;
**1 of 22 others exceeds 1.2 cm**.  This is not a network event that the guard detected — it is the guard's own withdraw-and-re-solve
moving exactly the satellites whose priors it removed.  On the float vehicle the unanchored clock is near-degenerate with that satellite's
phase biases and the stations' floated ambiguities, so removing a 3.2 mm prior lets the re-solve slide the clock by decimetres.

**And ~90 % of it reverses at the next epoch, undeclared.**  TOW 279300 → 279330, de-commoned, with the declaration column:

| sat | dCLK 279300 | DECL | dCLK 279330 | DECL |
|---|---|---|---|---|
| G04 | +0.268 | **yes** | −0.232 | **NO** |
| G07 | +0.135 | yes | −0.127 | NO |
| G20 | −0.133 | yes | +0.124 | NO |
| G09 | +0.103 | yes | −0.099 | NO |
| G22 | −0.108 | yes | +0.098 | NO |

Run-wide, over the **130 decoupled satellite-epochs whose declared step exceeds 2 cm**: median reversal at the very next epoch **91 %**,
mean 64 %, **102 of 130 reverse by ≥ 50 %**, and **0 of 130 returns is declared**.  The per-epoch census (`--section census`) shows it on
all 19 decouple epochs (`rev%` 86–101 on the eight largest; `revDECL = NO` on 19 of 19).

**The undeclared-step ledger** (`--section undeclared --step-m 0.10`, whole run, de-commoned IF code OR phase):
**545 steps > 0.10 m, of which 451 (83 %) are UNDECLARED**, and 86 of those land on a satellite that is LOOSE (decoupled and not yet
re-coupled).  The LOOSE ring is the second half of the same defect: at 276840 the epoch withdrew G06/G14/G17 and re-solved, and
**already-LOOSE G12 moved +0.373 m and G24 +0.190 m with no declaration**, because only the satellites tripping THAT epoch are marked.
At 276870, with no trips at all, four LOOSE satellites moved 0.30–0.37 m, all undeclared.

## 5. (d) THE COMMON CAUSE — what the traces close, and what they refute

**REFUTED: the decouple is not the consumer's channel.**  Three independent measurements, all from the same run:

1. **Size and count do not predict damage.**  Over the 19 decouple epochs, corr(max de-commoned wire excursion, consumer peak over the
   next 30 min) = **0.135**; corr(n_satellites, same) = **0.212**.  The two largest decouples in the run are harmless: TOW 268860 (23
   satellites, maxEx **1.334 m**) takes the consumer 0.55 → peak 2.17 → **0.96 m** in 10 min, and TOW 275070 (21 satellites, maxEx
   **1.264 m**) 0.45 → 0.94 → **0.42 m**.  The two damaging epochs are among the SMALLEST: 276810 (9 satellites, 0.400 m) and 279300
   (8 satellites, 0.268 m).
2. **At TOW 279300 the decoupled set is not in the consumer's sky.**  Using the two network stations nearest WTZZ00DEU (FFMJ00DEU,
   GRAZ00AUT) as the sky proxy, the consumer's satellites at 279300 are {G06, G11, G12, G19, G24, G25, G28, G29, G32}; the eight
   decoupled-and-declared are {G04, G07, G08, G09, G20, G21, G22, G30} — **overlap 0**.  Meanwhile the harmless TOW 271200 decoupled
   28 satellites with overlap **1.00** and the consumer improved.  And across the whole 279300 epoch **exactly one satellite anywhere on
   the wire stepped more than 0.20 m** (G04, declared, not in the sky).
3. **The wire is quieter during the ramp than during the consumer's best hour.**  Pair-consistent cumulative de-commoned drift, per
   satellite maximum over matched 34-epoch windows:

| window | consumer | max cumulative dIF**code** | max cumulative dIF**phase** |
|---|---|---|---|
| RAMP 279270–280260 | 1.06 → 5.64 m | **0.044 m** | 0.908 m (G18), 0.623 m (G24) |
| CONTROL quiet h5 274200–275190 | ≈ 0.45 m flat | **0.726 m** | 0.733 m (G26), 0.725 m (G27) |
| CONTROL 271020–272010 (28+14 decoupled) | 2.34 → 0.94 m | **0.120 m** | 0.622 m (G24), 0.526 m (G02) |
| STEP 276780–276900 (5 epochs) | 0.51 → 3.16 m | 0.222 m | 3.498 m (G29), 1.743 m (G20) |

**CLOSED for the 276810 STEP: it is a network-wide PHASE-BIAS re-realization at the loss of a phase-datum anchor.**

- The trigger is an observation-set change, not a fault.  `PB:G23:L1W` was carried by exactly **two** stations, STJO00CAN and YKRO00CIV;
  **both lost G23 at 276810** (STJO00CAN's own satellite count goes 7 → 6, G23 leaving), the column was removed
  (`TOPOLOGY col_removed PB:G23:L1W`), and STJO00CAN was simultaneously the G23 **L2W phase-datum anchor**, forcing
  `STJO00CAN -> SAVO00BRA` with a **−8.047 m** held-basis S-transform.
- The held basis did its job on the CODE channel — this is worth stating because the −8.047 m number invites the opposite conclusion.
  `rf2d_grade.py` on this run: held-basis handovers vs the de-commoned IF step, `all-cont` n = 91, p50 0.001 m, p90 0.003 m, max 0.40 m,
  **0 above 1 m**; the null population (22 107 satellite-epochs) has 6 above 1 m.  G22's +14.363 m S-transform at 279240 moved its emitted
  `PB L2W` by **+0.0005 m**.  The S-transform is not the wire defect.
- What DID move is the phase-bias channel network-wide.  At 276810, **19 satellites have a de-commoned IF-phase step > 0.5 m (max 3.638 m
  on G29), 13 of them UNDECLARED**; 23 satellites exceed 0.20 m, 15 undeclared, and three of those are in the consumer's sky (G24 −1.514
  declared, G32 +1.345 **undeclared**, G25 +0.781 **undeclared**).  Ranked over the whole run, 276810 is **the largest such epoch**: the
  next post-ignition epochs are 278520 (12 satellites, 5 undeclared) and 275070 (5, 0 undeclared); everything else above 5 is inside the
  first 5 minutes of ignition.
- Two independent producer instruments agree that 276810 is the run's worst epoch, and they are the run-end census's own worst rows:
  `pl`/CLEAN geo_t² **1.40 → 3.85e4** with 736 of 782 rows flagged `c_hi_flat` and 290 above |t| 100 (worst CEBR00ESP / G12 / L2W,
  **ê = −4.913 m**); `pb`/CLEAN 84 of 84 rows above 1σ with 54 above |t| 200 (worst G13 / L1W, ê = **+1.104 m**); `cb` 89 of 91.  Seven of
  STJO00CAN's own `pl` rows are metres with `r_negative = 1` (G28 L1W +2.099, G28 L2W +2.309, G31 L1W +3.041, G31 L2W +3.906, G32 L2W
  +1.945, G12 L2W −2.490).
- The clock decouple is DOWNSTREAM of that inside the epoch.  The seam order in `src/core/app/ApplicationNetwork.cpp` is
  `SB_SLIP → SB_PREASM → (assembly / phase datum) → SB_GAUGEBUILD → SB_DDBUILD → SB_SOLVEGLUE → SB_INSTALL → SB_SOLPREP → SB_EMITSTORE → … → SB_RAIM`;
  `networkClockAnchorGuard` is called from `src/core/filter/NetworkSolveGlue.cpp:1292`, i.e. after the handover has re-expressed the datum
  and before emission, and the `T4_attrib` arc refloats are emitted from `src/core/filter/NetworkRaim.cpp:1195` in `SB_RAIM`, i.e. after
  emission.  So the anchor departure precedes the clock trips, and the 9 → 44 arc-refloat storm follows both.

**NOT CLOSED: the 279300 RAMP.**  No producer channel this run exposes moves during it.  See §8.

## 6. THE MECHANISM, with file:line

`src/core/filter/NetworkClockAnchor.cpp`:

- `:256` / `:315–347` — the guard builds one row per folded clock: `e = x_s(col) − last_value`, `S = max(s_floor, var_eff − post_var)` with
  `s_floor = (network.clock_anchor_sigma_floor_m)²` (`:256`), `r.S = max(s_floor, var_eff − post_var)` (`:345`); on this arm 1.8 mm, so `k = 6` is a **1.2–1.6 cm** bar.
- `:350–360` (the per-constellation median) and `:367` — `w = (e − median_const) / √S`; trip if `|w| > k` or `|e − med| > network.clock_anchor_abs_m` (2.0 m, never reached here).
- `:394–402` — `decouple()` (`:173–202`) sets `anchored = false`, emits `CLK_ANCHOR/cta_decouple` and, under `cta_mark`, queues
  `markSatDiscontinuity` → the `R` record.  **This is the only edge that declares.**
- `:403–407`, `:268` — the same-epoch VALUE correction: satellites whose carried redundancy `(var_eff − post_var)/var_eff` clears
  `kCtaMinCarryRedundancy = 0.05` (`:43`) have their prior withdrawn and the epoch re-solved, up to `kCtaMaxPasses = 3` (`:34`, the cap's warning at `:574`).  The
  re-solved value is what is emitted — and §4 measures that it is ~90 % transient.
- `:152–157` — the LOOSE fold, whose comment is the claim this card falsifies:

  > `LOOSE folds at the LEGACY rate over the LEGACY dt — exactly the pre-card behaviour.  That is what bounds a decoupled satellite
  > (standing invariant #7): it is not unconstrained, it is on the carry the existing clock_runaway_check monitor already polices.`

  The legacy rate is `filter.process_noise.sat_clock_q`, whose own header (`include/config/GnssConfig.hpp:7995`) records
  `√(sat_clock_q·Δt) = 17.3 cm` per 30 s against an anchored `√(2.9e-8 · 30) = 0.93 mm`.  A 186× looser prior is a RELEASE, not a bound,
  and §4's ledger measures what it releases: 0.19–0.37 m per epoch on LOOSE satellites, undeclared.
- `:409–431` — the re-coupling edge.  `in_band` is incremented at `:415`, **inside the per-pass loop** opened at `:268`, so an epoch that runs two passes advances the
  dwell twice: the 276810 cohort re-couples at 276900, three epochs later, against `network.clock_anchor_recouple_epochs: 4`.  The
  `cta_recouple` event also prints the CONFIGURED threshold (`in_band=4`) rather than the realized count, so the trace cannot show this.
  The comment at `:420–427` justifies not marking the re-engage — correctly, because by then the value has already returned; the
  undeclared motion happened at the epoch AFTER the trip, long before the re-engage.

The held-basis handover lives in `src/core/filter/NetworkPhaseDatum.cpp` and is exonerated on the code channel by `rf2d_grade` (§5).

## 7. THE VERDICT

**Report, pre-register, do not build.**  The card's build gate is *mechanism closed by the traces AND a single-concern ≤ 80-line change in
one seam AND default-OFF byte-identical*, with consumer bars.  Clause 1 fails for THE EVENT: the 276810 step is attributed to the
phase-bias frame (whose repair is a datum/vehicle change, not an 80-line seam edit) and the 279300 ramp is not attributed at all.  The Q3
wire-contract defect IS closed — but its channel to the consumer is measured and refuted, so pre-registering consumer bars for a fix to it
would be pre-registering a bar the fix cannot move.  **No run was spent.**

## 8. THE PRE-REGISTERED FIX (nothing built)

### 8.1 `network.clock_anchor_declare_return` — bool, default `false`, one seam

`src/core/filter/NetworkClockAnchor.cpp` only, plus counters in `include/app/NetworkRunContext.hpp` and one run-end line in
`NetworkTeardown`.  Estimated 60–75 lines.

- **DECOUPLING edge (bound what the decoupled state accumulates).**  At the trip, stash in `ClockAnchorSat` the pre-trip emitted value
  `v0` and the post-withdraw emitted value `v1`, i.e. the declared step `Δ = v1 − v0`, both de-commoned by the per-constellation median so
  the stash is datum-invariant.  Add `cta_loose_excursion_max` / `_sum` counters over the LOOSE span, so "what the LOOSE fold accumulates"
  is a number instead of a comment.
- **RE-COUPLING edge (declare what re-connects).**  On each LOOSE epoch, if `|v_now − v1| > return_frac · |Δ|` (new
  `network.clock_anchor_return_frac`, default 0.5) and the satellite has not been declared since its trip, DECLARE it
  (`markSatDiscontinuity`, the same path the trip uses) and emit `CLK_ANCHOR/cta_return` with `frac`, `Δ`, and the epoch count since the
  trip; then clear the stash so one trip produces at most one return declaration.
- **Also fix the dwell** (3 lines, same knob): increment `in_band` once per EPOCH, not once per pass, and print the realized count in the
  `cta_recouple` detail instead of the configured threshold.
- **Counters / events:** `cta_return_declared`, `cta_return_frac_p50`, `cta_loose_excursion_max`, `CLK_ANCHOR/cta_return`.

**Bars.**  These are WIRE bars.  The consumer bars are stated as a NON-REGRESSION expectation, explicitly predicted NULL by §5 — a fix
that moved them would falsify this card's own attribution and must be read that way, not as a win.

| bar | baseline | pass |
|---|---|---|
| OFF smoke `products.txt` sha256[:16] (`runs/_rf2d_smoke_off/arm.json` → `runs/_ev6_smoke_off/arm.json`, paths only) | `9add1d3ab9d70657` | identical |
| decouple epochs with `revDECL = NO` (`ev6_event_read.py --section census`) | **19 of 19** | ≤ 2 of 19 |
| decoupled satellite-epochs whose ≥ 50 % return is undeclared | **102 of 130** | ≤ 10 |
| undeclared steps > 0.10 m on a LOOSE satellite (`--section undeclared`) | **86** | ≤ 20 |
| undeclared steps > 0.10 m, all causes | **451 of 545 (83 %)** | ≤ 380 |
| SOLVE unchanged: `network_epoch_status.csv` + `closed_loop_trace.csv` sha256 | — | identical to the baseline arm (the knob is emit-side only; `products.txt` differs ONLY in `R` lines) |
| consumer, FULL RUN (`runs/_rfpdfix_field6_both/arm.json` → `runs/_ev6_field6_both/arm.json`, paths + knob) | 5.02 / 2.11 / 0.97 / 1.05 / 0.45 / 3.65 | hours 3–5 no worse by > 0.10 m; hour 6 < 3.65 m; 720/720; IF dispersion p90 ≤ 0.25 |
| consumer, EVENT WINDOW p50 TOW 276810–278400 | ≈ 3.2 m | **expected UNCHANGED** — §5 predicts this fix cannot move it |
| sim twin (`runs/_rfpdfix_sim6_both/arm.json` → `runs/_ev6_sim6_both/arm.json`) | 1.81 / 0.79 / 0.40 / 0.26 / 0.19 / 0.46, WL 99.1 % | 720/720, WL ≥ 98 %, within 0.05 m/hour |

**Unit-testable kernel:** the return predicate is pure (`Δ`, `v_now`, `return_frac`, `declared_since`) — one new gtest file with a unique
suite name (`NetworkClockAnchorReturnTest`) and an explicit `CMakeLists.txt` entry.

### 8.2 The cheaper discriminator, which needs NO build

`network.clock_anchor_resolve` already exists (`include/config/GnssConfig.hpp:8006`) and its OFF path is documented as "degrades to
detect+mark+decouple-next-epoch" — i.e. it removes the withdraw-and-re-solve that §4 measures as the source of the transient.  One arm of
the baseline field config with that single knob flipped answers, for one run, whether the declared step is the artifact this card says it
is: **predicted `revDECL` population collapses and the `R`-record count falls sharply, with the consumer UNCHANGED at hours 3–6.**  It is
solve-affecting and fingerprinted, so it is a fresh-convergence arm, and it is the author's call whether that run is worth spending.

### 8.3 What this card does NOT propose

Re-tuning `clock_anchor_k` or `clock_anchor_sigma_floor_m` to make the trips go away.  The 1.2–1.6 cm bar is Q3's deliberate posture
(`docs/SPEC_clock_temporal_anchor.md`, memory card `project_clock_temporal_anchor_q3.md`), the trips are mostly real innovations against
it, and tuning a threshold to suppress an instrument is exactly what the project's own standing rule forbids.  The defect is what the
RESPONSE emits and declares, not what the test detects.

## 9. WHAT STAYS OPEN

1. **The 279300 RAMP is unattributed.**  The consumer travels +3.24 m E and −3.86 m U in 16.5 minutes while every producer channel this
   run exposes is quiet: max cumulative de-commoned IF-code drift 0.044 m (16× smaller than the quiet-hour control), one satellite
   stepping > 0.20 m in the whole onset epoch, ZWD at the two nearest network stations moving 5 cm over the window, `n_present` 59 and
   `datum_changed` 0 throughout, and no wire satellite leaving the consumer's sky.  **What is missing is the consumer's own per-satellite
   state.**  `closed_loop_trace.csv` carries position, `n_sats` and the AR columns and nothing else — no per-satellite ambiguity,
   residual, iono or reset column — so a consumer-side float re-convergence cannot be separated from a producer channel that
   `products.txt` does not carry.  On this posture the slant ionosphere is exactly such a channel: RF-0g runs
   `network.estimate_iono_sh: false` with `w6_iono_constraint: none`, the consumer reads the in-process store rather than `products.txt`,
   and the emitted `B`/`P` records carry the code-bias half of a pair whose ionosphere half RF-0b requires the consumer to apply WITH it.
   The instrument owed is a per-satellite consumer trace (ambiguity value + variance, applied correction, post-fit residual, reset flag),
   pre-registered here and not built.
2. **`network.emit_clock_disc: true` emits no `clock_disc` field.**  Every `C` record in this run is 3 fields.  Either the field is
   conditional on something this posture does not set, or the knob is inert on this path; RF-0hv §5 and RF-3-prep both quote `CLK_STEP`
   counters that ride a different mechanism, so nothing downstream noticed.  Worth one grep-level check before RF-3 writes the wire
   contract it promises.
3. **The phase-bias frame at an anchor departure.**  276810 is the run's largest phase-bias re-realization and 13 of its 19
   half-metre-plus steps are undeclared, yet `rf2d_grade`'s handover table is clean — because that table grades the CODE combination and
   the handover population, and this epoch's phase motion is on satellites that did NOT hand over.  A phase-channel twin of the RF-2d
   handover grader is owed.  It is the same requirement RF-3 already carries ("a wire contract that declares every step") narrowed to a
   measured population.
4. **The 20 % cascade share.**  26 of 158 statistical trips are pass-1 and 5 are pass-2 — exposed only after an earlier withdraw moved the
   epoch.  Whether those are genuine second faults or the median walking under the removals is not separable from this run; it needs the
   `clock_anchor_resolve: false` arm of §8.2.
5. **RF-0hv's own open items are untouched** — the deferred golden re-base, the level-only declared harvest era, RF-2c, RF-3, and the 9
   residual non-finite-input `addStation` rejections.

## 10. RUNS AND ARTEFACTS

- `runs/_rfpdfix_field6_both` — the RF-0hv FIELD baseline, read only, not re-run, left in place.
- `runs/_rfpdfix_sim6_both` — the RF-0hv SIM baseline, referenced for the pre-registered sim twin only.
- **No new run.**  No new exe.  No config armed.  `config/golden/`, `golden_manifest.json` and `docs/CONFIG_promotion_ledger.{md,json}`
  untouched.
- New instrument: `scripts/ev6_event_read.py`.  Existing graders re-used unchanged: `scripts/rf2d_grade.py`,
  `scripts/rf3_if_attrib.py --handovers`.
