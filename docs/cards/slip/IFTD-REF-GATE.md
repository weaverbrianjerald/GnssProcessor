# IF-TD ref gate: 100 % false on the sim bed, removed by the Card-R gate

**STATUS**

- **id:** IF-TD ref gate
- **topic:** slip
- **status:** closed
- **summary:** The legacy IF-TD reference gate is 100 % false on the slip-free sim bed; Card R's gate, already ARMED in the goldens, takes acting refsat refloats 546 to 0.
- **dates in the text:** 2026-09-08
- **knobs / posture:** `network.cycle_slip.if_phase_td_ref_gate`
- **hashes named in the text** (commit / exe / byte-cert): `2468e875`
- **run dirs named in the text:** `runs/simnet60_quiet_wu93_24h`, `runs/_partial_ggq_ep11_traces_20260812`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md`: §6.14, 6.14.1, 6.14.2, 6.14.3, 6.14.4, 6.14.5, 6.14.6

**Outcome.** The question: why the reference gate refloated arcs on a slip-free bed. Its statistic is the reference satellite's own noise (sigma 17.7 mm), so the fixed bar is a 3.02-sigma noise threshold; Card R's gate leaves 118 cold-era diagnostic fires. Two instrument defects were fixed (a missing flag-rate tier, a pool scale deflated 16 %). Still open: the floor sits on the (1,1) jump, so IF-TD's unique reach is (n,n), n ≥ 2, above about 30°. The gate stays armed in the goldens; follow-up in [IF-TD ref gate (v2)](IFTD-REF-GATE-FOLLOWUP.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.14 The IF-TD reference gate: 100 % false, and the fix was already in the goldens (2026-09-08)

§7 item 5 ("1917 false refloats/day, IDENTICAL across arms") is CLOSED.  It is a real defect of the
posture that produced it, its accuracy on this bed is exactly **zero**, its root cause is the
simulator's own phase noise, and the knob that removes it (`network.cycle_slip.if_phase_td_ref_gate`,
Card R) is **already armed in both goldens** — the `nonint_*` ladder had it POSTURE OFF.

### 6.14.1 Ground truth: there is nothing to detect

`runs/simnet60_quiet_wu93_24h` has `cycle_slips.rate_base = 0.0`; all sixty `truth_slips.csv` are
empty (**0 injected slips**).  Every one of the 2482 `refsat_reset` events (1917 acting + 565
diagnostic) is therefore a false positive.  Precision **0/1917**.  The pool arm over the same run:
**0 flags / 2,754,791 opportunities** (`network_flag_rates.csv` T1) — correctly silent.

### 6.14.2 The statistic is pure common mode, and it is the reference satellite's own noise

The gate tests `|median(non-reference DDs)| > 0.5·λ_IF` = 53.48 mm.  Because
`dd(sat) = ΔOMC(sat) − ΔOMC(ref)`, the reference's own epoch-to-epoch noise is common to **every**
member of the family, so the median cannot average it away:

| n (family size) | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|
| σ(median) [mm] | 17.67 | 17.30 | 17.65 | 17.40 | 17.68 | 17.66 | 18.39 | 18.24 |

Flat — no 1/√n at all.  A variance decomposition over cells of (epoch, reference satellite) shared by
≥ 3 stations splits it **88 % station-local (14.83 mm) / 12 % satellite-side (5.43 mm)**, and the
station-local half is exactly the declared fixture noise: 3 mm zenith phase × 2.9782 (IF
amplification) × √2 (time difference) ÷ sin(el_ref ≈ 62°) = **14.3 mm predicted vs 14.83 measured**.
The same budget predicts p50 |dd| = 15.4 mm at 45° (measured 15.2) and 34.3 mm at 15° (measured 33.6).

So σ(statistic) = 17.7 mm and the fixed bar sits at **3.02 σ**.  Measured exceedance 0.72–0.84 % of
station-epoch-constellations; × 342,623 opportunities/day = the observed ~2500.  **The gate is a 3σ
threshold on noise, and nothing in it is about a cycle slip.**

Three populations, all false:

| population | rate | share |
|---|---|---|
| cold-start burst, ep 1–11 (profile 116, 114, 113, 112, 110, 106, 99, 88, 70, 43, 25) | ~100/epoch | 996 = 40 % |
| steady state, ep ≥ 12 | 0.504/epoch = 1452/day | 1486 = 60 % |
| of which reference-handover epochs | **12.26 %** vs 0.487 % baseline (25×) | 328 = 17 % |

The burst profile is **byte-identical** between the `nonint_*` ladder and the golden lineage, so it is
a property of the fixture and the detector, not of the estimator arm — and it decays in lockstep with
convergence.  That also corrects the header's claim that the detector is "independent of Kalman filter
state": the *observable* is, but `phi_if_omc` subtracts `obs.modeled`, whose ZWD term is a filter
state, and the burst is that term settling.

### 6.14.3 It is a FIXTURE-REALISM artifact — the field is 5× quieter

Elevation-matched against a real 300-station DOY-129 IF-TD trace
(`runs/_partial_ggq_ep11_traces_20260812`), p50 |dd| in mm:

| elev | 10–20 | 20–30 | 30–40 | 40–50 | 50–60 | 60–70 |
|---|---|---|---|---|---|---|
| sim | 33.61 | 21.98 | 17.38 | 15.22 | 14.07 | 13.31 |
| field | 6.85 | 4.84 | 3.54 | 2.98 | 2.60 | 2.14 |
| ratio | 4.9× | 4.5× | 4.9× | 5.1× | 5.4× | 6.2× |

The sim flattens at ~13 mm above 40° — that floor **is** the reference satellite's own noise (predicted
14.3 mm), visible a third way.  At field noise the same 53.5 mm bar is ≈ 15 σ, not 3 σ, which is why the
field measured **14** unsupported refsat flags (`SPEC_slip_qc_hardening` §8.4.4.1) where this bed
produces 2482/day.  ⚠ Do not port a fixed physical threshold between beds whose noise differs 5×.

### 6.14.4 The A/B — Card R takes the acting refloats to ZERO

Both arms from `config/golden/golden_sim_tri_w6.json`, ONE knob differing, same exe
(`gnss_processor_ifref.exe`, md5 `2468e875…`), 2 h / 240 epochs / 60 stations, sequential with
SOURCE-mtime capture assertions (no `STALE`).  This discharges the "one 2 h arm owed".

| | OFF (legacy) | ON (Card R) |
|---|---|---|
| refsat refloats, ACTING | 546 | **0** |
| refsat, diagnostic (carry cold) | 565 | 118 |
| distinct (station,sat) arcs hit | 137 | **0** |
| steady-state rate, ep ≥ 12 | 0.504/epoch | **0.000/epoch** |
| total events logged | 1502 | 509 |
| MW/WL slip events | 142 | 142 |
| pool arm T1 opp / flags | 227,644 / 0 | 228,634 / 0 |
| σ̂₀ / σ̂₀_pf / σ̂₀_pf_phase (settled median) | 1.0000 / 0.8802 / 0.7985 | 1.0000 / 0.8802 / 0.7996 |

Run-end counter, the within-run control (ON arm): **1090 legacy candidates → 118 fired**; 948
suppressed by the empirical scale / coherence test, 13 thin-family, 11 by the consecutive-fire cap.
1090 against the OFF arm's 1111 events is a 1.9 % agreement — the counter and the event file are
independent readings and they agree, per §6.13.4.

The 118 survivors are all at epochs 1 and 3 and all `diagnostic`: with zero empirical information the
cold prior `floor_frac·λ_IF/k` makes the gate degenerate to *exactly* the legacy floor by design.  From
the first warm epoch onward it is silent for 237 consecutive epochs.  Offline replay had predicted
this: settled `k·s_eff` = 0.1275 m = **2.38× the legacy floor**, 98.4 % of fire groups already exceed
the legacy floor on 6·disp alone, and the coherence test rejects 75 % of them outright (fires have
disp p50 0.0446 m against a null of 0.0213 — they are the NOISY-family epochs, the opposite of the
coherent common step a reference slip would produce).

⚠ The Card-R evidence base before this run was a **10-epoch COLD smoke** (§8.4.4.1) — a window in
which, by the cold-prior design above, the gate has not yet reached its empirical arm at all.  This is
its first warm validation.

### 6.14.5 What the pool arm can and cannot detect

`thr = max(k_mad·s_eff·elev_map, floor_frac·λ_IF)` with `k_mad` 8 and `floor_frac` **1.0**, i.e. the
floor is exactly the (1,1) jump and the test is a strict `>`.  Settled, over 2.53 M rows: thr p50
**1.9 λ_IF**, p90 4.3; by elevation 4.1 λ_IF at 10–20°, 2.5 at 20–30°, 1.8 at 30–40°, 1.1–1.2 above
60°.  The physical floor binds on only **5.21 %** of rows and **never below 30°**.  Max |dd|/thr over
the whole settled run = 0.846 — it was never close to firing.

For GPS L1/L2 the IF response is `0.4844·ΔN1 − 0.3775·ΔN2` and MW/WL sees `ΔN1 − ΔN2`:

| slip | IF [λ_IF] | WL [cyc] | MW | IF-TD |
|---|---|---|---|---|
| (1,0) | 4.53 | +1 | yes | yes |
| **(1,1)** | **1.00** | **0** | **blind** | **not > floor ⇒ blind** |
| (2,2) | 2.00 | 0 | blind | only above ~30° |
| (7,9) | −0.06 | −2 | yes | blind |

The complementarity is real — (n,n) is exactly what MW cannot see — but the shipped floor sits *on*
the (1,1) boundary, so IF-TD's unique reach is (n,n) with **n ≥ 2 and elevation ≳ 30°**.  The sim
cannot test any of this: it injects only ≥ 12-cycle single-band slips (median 50, i.e. 42–250 λ_IF)
and **never above 44.9° elevation**, while the reference satellite sits at p10 48.0° / p50 61.7° —
**zero overlap, so a reference-satellite slip is unreachable on this bench by construction**.

Two smaller code observations — **both FIXED 2026-09-08, §6.14.6.**

- The pool's own robust scale is computed over `getLastIfJumps()`, which includes the reference
  satellite's structurally-zero DD (12.5 % of samples).  That **deflates s_epoch by 16.0 %**
  (0.01190 vs 0.01416 m).  Harmless in direction (a tighter threshold on a detector with zero false
  alarms), but the scale is not the quantity it claims to be.
- `network_flag_rates.csv` T1 reports **0 flags** on a run in which the same tier drove 1917
  refloats: the reference gate has no flag-rate tier.  Standing invariant #4 — the ON-path telemetry
  does not see the ON path.

### 6.14.6 Both fixed, and the two-arm A/B that says so (2026-09-08)

Neither is a numerical-correctness bug, so neither is graded on truth; both are graded on **what
the instrument reads**.  Bed: `config/test_suite/_ifsc_{old,new}.json`, a 2 h arm of
`config/golden/golden_sim_tri_w6.json` (the quiet 60-station wu93 truth bed — a CLEAN bed by
construction) with only `max_duration_s` and the run paths changed.  The two arms differ **only in
the exe** (`gnss_processor_ifsc_{old,new}.exe`, one source hunk apart, same Release build config and
toolchain), run SEQUENTIALLY because concurrent runs share `./logs/`.  Runner
`scripts/_ifsc_ab.sh` (SOURCE-mtime freshness assertion — the stale-trace-dir trap), comparison
`scripts/_ifsc_ab_compare.py`.

**Defect 1 — tier T1r.** The reference arm now has its own flag-rate row, `T1r`, fed by two new
UNCONDITIONAL counters in `IfPhaseSlipDetector::RefGateCounters` (`gate_opportunities` /
`gate_flags`) and drained at teardown.  It is a SEPARATE tier from T1 on purpose, and the arm's own
numbers are why:

| tier | what one opportunity IS | opportunities | flags |
|---|---|---|---|
| T1 (pool) | one (station, sat, signal-pair) sample | 228 634 | **0** |
| T1r (reference) | one (station, epoch, constellation) family of n ≥ 2 non-ref DDs | 28 680 | **118** |

The denominators differ by ~8× (one reference per family), and the thresholds are different
quantities — `max(k·s_eff, floor_frac·λ_IF)` armed vs a bare physical floor on the legacy path.
Averaging the two would have re-hidden the asymmetry.  **This is the defect exhibited**: the shipped
table reads `T1 … flags=0` for a run whose IF-TD tier reset a reference 118 times.  Cross-checked
against a second instrument (invariant #4): `network_events.csv` carries exactly **118**
`refsat_reset` rows in both arms.  The counters are gate-independent by construction (a unit case
pins `gate_opportunities` equal with the Card-R gate armed and disarmed), so a
`if_phase_td_ref_gate` A/B now compares two rates over one denominator.

**Defect 2 — the reference's structural zero is out of the scale.** `IfJumpInfo::is_reference`
marks it (the pool cannot otherwise know which satellite is the reference), and **both** scale
estimators drop it: the cold pool prior `s_pool` and the warm EWMA feed `s_epoch` — the latter is
the quantity the 16.0 % was measured on, and `s_eff = w·ewma + (1−w)·s_pool`, so fixing one half
alone leaves the scale biased the moment a station-pair goes warm.  The zeros are **12.47 %** of the
228 634 trace rows here, reproducing the 12.5 % measured on the 24 h bed.  Measured, p50 over all
rows:

| quantity | old (ref included) | new (ref excluded) | change |
|---|---|---|---|
| `s_pool` | 0.012451 m | 0.014582 m | **+17.1 %** |
| `ewma_scale` (= s_epoch) | 0.014770 m | 0.017025 m | **+15.3 %** |
| `s_eff` | 0.014744 m | 0.016999 m | **+15.3 %** |
| `thr` | 0.235355 m | 0.273397 m | **+16.2 %** |

The +15.3 % on the EWMA half reproduces the independently-measured 16.0 % deflation.  Pre-registered
PASS criterion — **the pool flag population is unchanged on a clean bed** — holds, and holds
stronger than asked:

- flagged samples **0 → 0**, sample set identical (228 634 rows both arms, 0 only-in-either);
- `dd_m` and `med_m` **bit-identical** on every row (the fix touches the scale, never the
  measurement or its centre);
- `network_solution.csv` and `network_events.csv` **byte-identical** — the solve did not move.

⚠ **Read the 0 → 0 honestly: it is a vacuous pass, and it was always going to be.** §6.14.5 already
measured max |dd|/thr = 0.846 on the settled 24 h run — the pool arm was never close to firing, so
on a clean bed there is no flag population to perturb.  The load-bearing claims are the byte-identity
of the solve and the bit-identity of `dd`/`med`.  What the arm CAN say about sensitivity: max
|dd−med|/thr goes 0.730 → 0.704, i.e. still **30 % short of firing**, so nothing detectable was lost.

⚠ **The direction is permissive.** A correct (larger) scale is a LOOSER threshold, so this fix costs
a little sensitivity — it is not an improvement in detection power and must not be quoted as one.
What it buys is that the number is the quantity it names.  One measured consequence worth having:
the physical floor was the operative threshold on **4.63 %** of rows with the deflated scale and is
on **0.91 %** with the corrected one (§6.14.5 measured 5.21 % on the 24 h bed).  The deflated scale
was pushing the gate onto its λ_IF floor ~5× more often than the noise justified — so the detector
is now genuinely scale-driven rather than floor-driven, which is the substantive change here.

Still open (unchanged by either fix): the floor sits ON the (1,1) boundary, so IF-TD's unique reach
is still (n,n) with n ≥ 2 and elevation ≳ 30°, and the sim bench still cannot test a
reference-satellite slip (zero elevation overlap, §6.14.5).

