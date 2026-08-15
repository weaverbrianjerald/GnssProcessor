# HANDOFF — triple + slant + GF is the track; next is the 1 h full-instrument arm
## including the question this fixture has never been able to ask: are the network ambiguities near INTEGER?

> ✅ **§3 + §4 EXECUTED 2026-08-12 (same day) — the measured record is SPEC
> §13.6.**  The 1 h arm ran (116/120, 0 observability withholds), the new
> grader `scripts/simnet_amb_integer.py` joined 229 961 traced DDs to truth
> with 0 drops, and **the pre-registered shape held exactly**: EWL(2,5)
> **98.2 % at its own TRUE integer** (med|e| 0.081 cyc), WL(1,2) 60.2 %
> (0.386 cyc), per-band ~**5 cyc off with a ±20 cyc p90 tail** and only 8 %
> at the true integer — the field's uniform med|frac| 0.25 was the
> "off by 47.3" class all along.  WL:EWL scales exactly as the DD-iono
> coefficients (4.8× vs 5.1× predicted); the per-band EXCESS (12.8× vs 3.5×)
> is equal-in-cycles across bands — leading candidate residual phase-windup
> DD content (open, discriminator named in §13.6).  Sweep: datum inventory
> 67 rows ✓, n_gf_only_pair 0 ✓, declared=n_below=1 ✓, cbpin ✓, pinned pair
> mm-exact on truth (C1C 2.3 mm / C2W 3.2 mm; C5Q 8.6 cm still converging —
> the "millimetre" is the DATUM channels).  Consumer 1 h: +2.6 m almost pure
> UP (ignition signature; settled §13.5 is 0.514 m).  Exchange deliberately
> NOT run at 1 h (barred by §2; lives on the settled arm).
> ⚠ §3.3's optimistic eq34+carry_invariant+ewl TRIO arm is **BLOCKED on this
> sim until seed-round is built** (`docs/SPEC_eq34_seed_round.md`): cold
> zero-target pins inject the §12.8 km-scale artifact, and the slant posture
> has NO SH gate to catch it — it would land silently in the emitted iono+CB.
> §5 (third-band-hurts-clock) not attacked: its discriminator needs the
> per-band noise σ the sim lacks; still owed.*

*Written 2026-08-12 for a fresh session.  Read this, then
`docs/SPEC_synthetic_minimum_network.md` §13 (the third band + the slant posture)
and §12 (the dual-frequency campaign it replaced).  Odijk et al. 2016,
J Geod 90:15–44 — page/table/equation references below are that paper's.*

---

## 1  The chosen track, and what is already built

**Posture C — triple frequency + free slant ionosphere + geometry-free code-bias
datum.**  Config `config/test_suite/_simnet60_m_c_tri_slant.json`, fixture
`runs/simnet60_tri` (60 solve + 1 consumer, 480 epochs @ 30 s, GPS
C1C/L1C + C2W/L2W + C5Q/L5Q, injected synthetic L5 OSB).  Release exe
`build/x64-Release/gnss_processor_matrix.exe` (sha256 `da2f9ac0…`; **re-copy and
re-hash after any rebuild — a renamed exe is a snapshot**).

Everything it needs exists:

| piece | where |
|---|---|
| third sim band (opt-in, byte-identical off) | `signals.gps_l5` / `galileo_e5b` / `synth_osb_amp_m` |
| GF code-bias datum (Odijk rd 3b) | `network.code_bias_gf_datum` → `NetworkSbasis::buildCodeBiasDatumRows` |
| loader path for slant + W6 | W6 chain force-enables `ionosphere_weighting` only when the GF datum is OFF; `ApplicationNetworkSetup` has the no-GIM branch |
| satellite bias σ | `network.trace_sat_bias_states` → `network_sat_bias_states.csv` |
| receiver clock + DCB σ | `network.trace_rx_states` → `network_rx_states.csv` |
| `cbpin=` S-line fill | `NetworkSolPrep` (was declared-but-never-populated since B6) |
| truth grader, slant-aware | `scripts/simnet_grade.py --gf-datum` (auto-detects) |
| exchange decomposition | `scripts/simnet_exchange.py` |

Settled matched matrix (SPEC §13.5), one fixture, one knob per adjacent pair:

| | A dual+SH | B triple+SH | **C triple+slant+GF** |
|---|---|---|---|
| cb residual rms | 1.131 m | 0.962 m | **0.568 m** |
| cb vf (s0-corrected) | 541 000 | 570 600 | **34 900** |
| consumer 3D rms | 1.126 m | **0.290 m** | 0.514 m |
| clock contrast | 0.773 m | 1.721 m | 1.590 m |

Read: **the slant datum is the code-bias lever (~16×); the third frequency alone
buys nothing there but improves the consumer 3.9×.**  C is still ~190× optimistic
*in σ* — improved, not calibrated.  And the third band **hurts** the clock (2.2×)
and ZWD, in both SH and slant postures — unexplained, see §5.

---

## 2  ⚠️ THE 1 h RULE — read before quoting any number from the new arm

A 120-epoch arm is **1 hour, which is inside ignition for every level and
calibration channel on this fixture.**  This session was burned by exactly that, in
the most instructive possible way:

> The 20-epoch slant probe graded **cb vf = 1.886** and was written up as
> "CALIBRATED".  The settled 480-epoch arm of the *same posture* graded **8 756**
> (34 900 s0-corrected).  The probe had measured a still-converging formal σ against
> a not-yet-grown true error — the §9.5 finding-2 signature ("the formal σ collapses
> while the true error grows") reproduced by the very arm meant to demonstrate its
> cure.

So for the 1 h arm:

**LEGAL** — structural and wiring questions whose answer is decided in the first
epochs and does not converge: rank/gate behaviour, datum realization (are the pinned
channels fixed?), which instruments fire, counter sanity, **the ambiguity-integer
question below**, QC tier opportunity counts, and any *ratio between arms run on the
same window*.

**NOT LEGAL** — variance factors, the ionosphere level, the exchange coefficient,
"is it calibrated", consumer verdicts, and anything compared against the §13.5
settled numbers.  If a 1 h number is quoted, it must carry the window in the same
sentence.

The 4 h fixture already exists; when a level claim is wanted, re-run 480 epochs
(~3 min/arm at 60 stations — the whole 480-epoch matrix ran in under 5 minutes, so
there is no cost argument for grading level channels on 1 h).

---

## 3  THE HEADLINE TASK — are the network ambiguities near integer?

This has never been askable before: the field can only measure the *fractional part*
(`med|frac_cyc|` sits at a uniform 0.248, which says "no integer structure" but
cannot say why).  On this fixture the true integers are known, so the question
becomes absolute rather than circumstantial.

### 3.1 What is estimable, and therefore what "integer" means here

Odijk Table 3 (CC-R): the estimable ambiguity is a **double difference**

    z̃^s_{r,j} = [z^s_{r,j} − z^1_{r,j}] − [z^s_{1,j} − z^1_{1,j}]     r ≥ 2, s ≥ 2

which is an integer **by construction in every posture** — the iono
parameterization does not change that (Table 8 changes the biases and the iono, not
the ambiguity's integer nature).  So "are the ambiguities near integer" is a
question about the ESTIMATES, not about the model.  Under our eq34 chain datum the
realized basis is DD chords rather than a single pivot pair, which is the same
object built on a spanning tree (`NetworkChainDatum.cpp`).

### 3.2 The data you need (all of it already exists)

- **Estimates**: arm `network.trace_dd_fractional` (+ `trace_dd_fractional_stride: 1`,
  `trace_dd_fractional_max_pairs: 2000`).  Header:
  `week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,…` — the four identifiers needed
  to reconstruct the truth DD are all present.
- **Truth**: `runs/simnet60_tri/<station>/truth_ambiguities.csv`
  (`satellite,signal,N_int,wavelength_m`, 90 rows = 30 sats × 3 signals per station)
  plus `truth_slips.csv` deltas.  The true value at epoch k is
  `N_int + Σ truth_slips.delta_cycles` up to k (the file holds the PRE-SLIP integer —
  SPEC §8).  This fixture is quiet (`rate_base` 0), so the sum is empty and `N_int`
  stands, but write the code to add it anyway.
- **The DD truth** is then, per (signal, sat_i, ref_sat, sta_a, sta_b):

      DD_true = [N(a,i) − N(a,ref)] − [N(b,i) − N(b,ref)]

  and the graded quantity is `DD_est − DD_true` **in cycles**, not its fractional
  part.  That distinction is the whole point: a fractional-part test cannot separate
  "off by 0.3 cycles" from "off by 47.3 cycles", and the field has only ever had the
  former.

⚠️ **The integers are km-scale by construction** — the sim draws
`N ∈ [1e5, 1e6)` per (station, band), so `N_int` ≈ 905 116 in the sample row.  Do the
differencing in integers/Python ints before converting to float, or you will grade
floating-point cancellation.  (This same km-scale draw is what makes zero-target
cold ambiguity pins inject km-scale tension — SPEC §12.8.1.)

### 3.3 Two competing predictions — pre-register before running

- **Pessimistic (the standing record):** the NL blocker is iono REPRESENTATION.
  Per-LOS slant iono scatters 0.26–0.37 m against a WL→NL bridge requirement of
  0.198 m, so `med|frac|` sits at 0.25 (uniform).  The slant posture **removes** the
  SH constraint, leaving the per-LOS iono freer still ⇒ the NL should get **worse**,
  not better.
- **Optimistic (what the third band opens):** at f = 3 the **extra-wide-lane**
  (L2,L5) has λ ≈ 5.86 m, which 0.3 m of iono scatter cannot break.  The lane
  cascade EWL → WL → NL becomes available for the first time on this fixture.  The
  standing "MW/WL as conditioning — do NOT arm; net-harmful" verdict was measured
  **at dual frequency**, where GPS EWL has no band; it does not transfer.
  `network.mw_wl.ewl_enable` requires `carry_invariant` (which requires
  `phase_datum_eq34`) — that trio is the arm to try, and Fable's audit already
  proved the chain places every pin it declares.

The honest expected shape: **EWL near-integer, WL plausible, NL still not** — and
the value is in measuring *which rung breaks*, which no field arm can do.

---

## 4  "Check everything" — the sweep, in run order

1. **Structural / datum** (1 h is legal): pinned pair still datum-fixed
   (§13.5: C1C 0.014 m, C2W 0.023 m settled — expect similar); `n_gf_pin_sats` =
   satellites, `n_gf_only_pair` = 0; `declared_dim` = `n_below` = 1 in
   `network_estimability.csv`; `cbpin=` present on the S line.
2. **Ambiguities** (§3) — the headline.
3. **QC tiers**: the grader's tier table (opportunities vs flags).  At 60 stations
   the thresholds are live (`raim_min_stations` 3, `rx_cb_carry_demean_min_stations`
   8).  A tier with 0 opportunities says NOTHING (invariant #4).
4. **Consumer**: `closed_loop_trace.csv` (already armed).  1 h ⇒ report, do not
   verdict.
5. **The exchange** (`simnet_exchange.py`): note the a₀₀ channel **does not exist**
   in the slant posture (no SH field), so only the clock / rx-DCB / cb_rms channels
   are available.  Fable's corrected script reports **0.97×** of ideal absorption
   under SH — quote that, not the older 0.72×.
6. **The clock regression from §13.5** — see below.

---

## 5  The open question this arm should also probe

**Why does the third band hurt the clock (contrast 0.773 → 1.721 m) and ZWD
(0.549 → 0.873 m) in BOTH postures?**  Hypotheses, none tested:

- the extra band adds 32 satellite phase biases + n·m ambiguities against FLOAT
  ambiguities with no AR — more nuisance parameters than geometry-side information;
- the synthetic L5 carries the same `code_sigma_zen_m` (0.30 m) as L1/L2 and a
  hand-set multipath amplitude (`code_amp_gps_l5_m` 0.015 m).  Real L5 is *quieter*
  than L1 C/A, so the fixture may be under-weighting its own third band;
- the synthetic L5 OSB (0.60 m amplitude) is larger than the product's L1/L2 OSBs on
  some satellites, so the third band may be dominating the code-bias estimation.

The cheapest discriminator is a **third-band noise-realism sweep** (vary
`noise.code_sigma_zen_m` per band — note this needs a per-band σ, which the sim does
NOT have today: `noise` is band-common.  Adding it is the same opt-in pattern the
third band used).  Do that before concluding anything about f = 3 and clocks.

---

## 6  Commands

```bash
python scripts/simnet_grade.py runs/simnet60_tri --run runs/simnet60_m_c_tri_slant/run --products runs/simnet60_m_c_tri_slant/products.txt --settle-frac 0.5
```

```bash
python scripts/simnet_exchange.py runs/simnet60_tri --products runs/simnet60_m_c_tri_slant/products.txt --run runs/simnet60_m_c_tri_slant/run --settle-frac 0.5
```

For a 1 h arm: copy `_simnet60_m_c_tri_slant.json`, set
`processing.max_duration_s: 3570.0` (120 epochs), give it **its own
`product_output_path` and its own CWD**, and add the `trace_dd_fractional` block.

---

## 7  Traps, and what not to re-litigate

- **A datum change is also a GRADER change.**  `simnet_grade.py` needs `--gf-datum`
  for this posture (auto-detected from the console line the loader now prints).
  Without it the truth projection omits a direction and reports **2.3 / 3.7 m
  phantom residuals** on channels that are datum-fixed to zero.
- **vf is deflated by s0²**, and s0 varies 1.16 → 2.00 across postures.  **Never pool
  or compare raw vf across arms** without the correction; §13.5 quotes both.
- **Never assert a bias SIGN verbally** — read the coefficient off the model or
  measure the slope, and say which.  Two conventions bit in one session (the
  absorbed IF lump is −d_IF, measured slope −1.08; the exchange coefficient is +,
  derived from `resolveReceiverBias`).
- The COD product has **no GPS band-5 rows at all** — the third band's OSB truth is
  entirely the injected synthetic delta.
- Already measured, do not repeat: station count is not the observability lever (the
  L2W gauge was n-invariant, and it is GONE at f = 3); `sbasis_complete` passes the
  gate and destroys the ionosphere on a cold start; `cb_kaula_prior` falsified alone
  and paired; the elevation-cutoff truth A/B **inverts** the field verdict (15° beats
  25° on clock channels because the cutoff feeds the exchange).
