# slant + triple: the third band and the slant-ionosphere posture on the sim bed

**STATUS**

- **id:** slant + triple
- **topic:** sim-bed
- **status:** closed
- **summary:** A third band closes the two-band observability wall and the slant datum cuts the code-bias vf about 16× without calibrating it; the settled EWL trio DIVERGES and stays do-NOT-arm.
- **dates in the text:** 2026-08-04 to 2026-09-09
- **knobs / posture:** `network.rx_code_bias_datum`, `network.code_bias_gf_datum`, `network.mw_wl.ewl_enable`, `network.trace_dd_fractional_ref`, `network.trace_sat_bias_states`, `network.trace_rx_states`, `network.trace_dd_fractional`, `processing.max_duration_s`
- **hashes named in the text** (commit / exe / byte-cert): `da2f9ac0` (`gnss_processor_matrix.exe`), `33052874` (`gnss_processor_slant.exe`), `6B1EF5D6` (sr.exe, the §13.7 trio A/B)
- **run dirs named in the text:** `runs/simnet60_tri`, `runs/simnet60_tri_slant_trio_480`, `runs/fld63_stk_on`, `runs/simnet60_m_c_tri_slant`, `runs/simnet60_quiet_wu93_24h`, `runs/nonint_losblock24h_hv`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §13 (13.1 to 13.8)

**Outcome.** The questions: what a third GPS band and free slant ionosphere (geometry-free code-bias datum) do to calibration, the consumer and the integers. Settled, the slant cb vf (s0-corrected) is still 34 900: the 20-epoch "CALIBRATED" was ignition. The third band takes the consumer from 1.126 to 0.290 m but degrades clock and ZWD (§13.8.4 OWED). The EWL is at its true integer 98.2 % of the time at 1 h, 74.7 % settled on the float arm and 5.0 % on the trio; D5 does not cure the runaway; wind-up is FALSIFIED as the band-common DD error. L5 is now a re-founding add-back ([AB2](../../refound/AB2_l5_third_frequency_2026-09-20.md), [L5P](../../refound/L5P_l5_phase_posture_2026-09-21.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §13 The THIRD BAND, and the slant-ionosphere posture (2026-08-12)

Odijk et al. 2016 (J Geod 90:15-44) §5.1, p. 29, on the model with free SLANT ionospheric
delays:

> "the receiver and satellite code biases are **not estimable anymore for the first two
> frequencies**… code biases are only estimable for a (modernized) triple- or
> higher-frequency constellation… the geometry-free receiver/satellite code biases become
> lumped to the ionospheric slant delay."

Under the **vertical** single-layer model instead (their §3.2), only the iono-free biases
are constrained (Table 2, rd 2a/3a), so the geometry-free part survives as one estimable
parameter shared by frequencies 1 and 2 — the DCB (Eq. 29–32,
`d̃^s_2(1) = (μ2/μ1)·d̃^s_1(1)`).  **That is the posture we run**, with the single-layer tie
imposed *softly* (SH rows + `representation_sigma_tecu`), which is why the dual-frequency
code bias is estimable here at all — and why its information is a model assumption rather
than the data.  §12.7's **cb vf ≈ 4.9e+05** is the price, measured.

### 13.1 What was built (sim side, done)

`SimConfig::Signals` — `signals.gps_l5`, `signals.galileo_e5b`, `signals.synth_osb_amp_m`.
Both bands **opt-in**: the band plan is already a table (`src/sim/main.cpp:1618-1643`) and
the noise/multipath/slip draws are taken per band inside the satellite loop, so appending
one shifts the whole stream — default-off keeps every existing fixture byte-identical, and
arming it is a new lineage by construction.  Verified: a two-band regeneration is
byte-identical on the RINEX and on `truth_atmo.csv`; `truth_bias.csv` gains three
provenance columns (`product_m,synth_m,source`) with the first four columns unchanged
row-for-row.

**The synthetic OSB is not optional for this experiment.**  The shipped `COD0MGXFIN` OSB
product carries **no GPS band-5 rows at all** — its GPS inventory is L1C/L1W/L1L/L1X,
L2W/L2S/L2L/L2X, C1C/C1W/C1L/C1X, C2W/C2S/C2L/C2X (verified by inventory).  A third band
synthesized from the product alone would carry an identically-zero code bias, and no grader
could separate "estimated it correctly" from "the channel is zero".  `synth_osb_amp_m`
injects a deterministic per-PRN delta (no RNG) recorded in the truth file with its
provenance.

Fixture: `config/sim/simnet60_tri.json` → `runs/simnet60_tri` (60 solve + 1 consumer, 480
epochs, GPS triple, `synth_osb_amp_m` C5Q 0.60 m / L5Q 0.02 m).  The roster now also
injects a **C5Q receiver bias** (inert on two-band fixtures, since the sim only applies a
bias to a code it emits).

### 13.2 The slant posture is NOT reachable in the producer today — two changes

| piece | state |
|---|---|
| loader path | **blocked**: `estimate_sat_code_bias` with neither SH nor GIM hard-errors (`ApplicationNetworkSetup.cpp:414-437`); `estimate_iono_sh` force-arms the SH field (`GnssConfigLoadNetwork.cpp:2060-2066`) |
| satellite GF datum | **missing**: `buildCodeBiasDatumRows` (`NetworkSbasis.cpp:415`) builds (1) a per-satellite **IF** pin and (2) per-(constellation, signal) sum-to-zero.  There is no **GF** pin |
| receiver GF datum | **already exists**: the receiver DCB is one `GPS_DCB` state per station whose GF signature is exactly `+1·x_DCB` (`StateInitializer::resolveReceiverBias`, μ1/dμ and μ2/dμ), so `network.rx_code_bias_datum` (sum-to-zero over those states) IS Odijk's receiver-side constraint |

Rank accounting to satisfy (Odijk §5.1 + Table 7).  Dropping the iono constraint opens two
deficiency types: **2b** — receiver hardware biases ↔ slant iono, size **n − 1**, removed by
constraining the geometry-free RECEIVER code biases (`d_r,GF(1)`; CC-R needs n − 1, CC-S
needs n); **3b** — satellite hardware biases ↔ slant iono, size **m**, removed by
constraining the geometry-free SATELLITE code biases (`d^s_GF(1)`; CC-R needs m, CC-S needs
m − 1).  So the work is a loader path allowing the combination **gated on f ≥ 3** (below
that Odijk says there is nothing to estimate and the current hard error is *correct*), plus
GF datum rows on the satellite side shaped like the existing IF pin —
`μ_GF = (−1, 1, 0, …)/(μ2 − μ1)` (their Eq. 30) instead of `μ_IF`.  Both default-OFF with a
byte-identity A/B.

**State the product consequence rather than discovering it later.**  Under Table 8 the
emitted quantities change meaning: `ι̃^s_r = ι^s_r + d_r,GF − d^s_GF` — the geometry-free
biases move INTO the ionosphere, and the B record then carries only modernized-DCB content
(`d^s_1 − d^s_j`, j ≥ 3).  A consumer must apply the network's ionosphere and its code biases
**as a pair**; applying one without the other double-counts the GF lump.  A product-contract
change, not a tuning knob.

### 13.3 What can be measured before that work

The triple-frequency fixture is useful immediately, with no producer change, for the
chain-datum question: **does the `PB|*|L2W` gauge survive when a third band exists?**  Run
the existing W6+SH posture (§12) on `runs/simnet60_tri` and read `network_estimability.csv`.
Three informative outcomes: (1) the near-null disappears ⇒ a two-band artefact of the phase
datum, and the chain audit has its answer; (2) it persists on L2W only ⇒ per-band, the datum
pins one band and not the other — the sharpest possible statement of the pinning defect;
(3) it appears on L5Q too (two near-nulls) ⇒ "one per non-reference band", exactly Odijk's
type-5 (size f·m) arithmetic, so the pin count scales with f rather than a band being
special.

### 13.3.1 MEASURED — the third band closes the observability wall (outcome 1)

`_simnet60_tri_wsh` (60 stations, W6+SH, SH degree 4, **default** `rank_gate_tol`, 20
epochs): **20/20 epochs emitted, 0 withheld.**  The two-band posture could not emit a
single epoch without a hand-placed `rank_gate_tol: 1e-12` (§12.2); the three-band posture
runs on the shipped default.  The spectrum says why — `n_below` = **1** = `declared_dim`:

| | dual (n = 50 and n = 300) | **triple (n = 60)** |
|---|---|---|
| directions below cutoff | **2** | **1** |
| clock gauge | 6.9e-12 / −3.4e-10 | −1.3e-10 |
| `PB\|*\|L2W` common mode | **9.78e-06 / 8.97e-06** | **gone** |
| weakest real direction | 2.48e-02 | 1.13e-02 (`PB\|G30\|L5Q+L2W+L1C`, per-satellite) |

The satellite-common secondary-band phase-bias gauge **does not exist at three
frequencies**.  The arithmetic is ordinary: each extra band adds 2·n·m observations
against n·m new ambiguities plus ~2m + 2n new biases, so at network scale a third band is
net-informative, and the surplus lands on exactly the bias/ambiguity family (Odijk types
4/5) the two-band datum could not pin.

Health: post-fit σ̂₀ **1.064 code**, a₀₀ 35.7 → 41.6 TECU in band, SH gate never engaged.
B records now carry **C5Q** (640 rows each for C1C/C2W/C5Q over 20 epochs), and the
third-band bias grades against its injected synthetic truth — ignition residual (20 ep):
C1C 0.274 m rms, C2W 0.451 m rms, **C5Q 0.450 m rms** (injected amplitude 0.60 m), datum
check emitted per-channel mean **+1.1e-05 m**, vf **1 998**.

⚠️ **That vf is NOT comparable to §12.7's 4.9e+05.**  Different station count, different
SH degree, and — decisively — 20 ignition epochs versus 233 settled ones.  The direction is
encouraging and the matched comparison is owed: same n, same degree, ≥ 2 h settled, dual vs
triple, one knob.

### 13.3.2 BUILT AND MEASURED — the slant posture runs, and its code bias is CALIBRATED

> ⚠️ **THE "CALIBRATED" VERDICT IN THIS HEADING IS SUPERSEDED BY §13.5 — read that first.**
> Settled and matched, the same posture gives **vf 8 756 published / 34 900 s0-corrected,
> not 1.886.**  The 20-epoch probe read a converging formal σ against a not-yet-grown true
> error (the §9.5 finding-2 signature) as calibration, and the write-up read it as "the
> §13.4 prediction confirmed by five and a half orders of magnitude" (vf ≈ 4.9e+05 dual
> single-layer §12.7 vs ≈ 1.9 here).  Its own caveat — 20 ignition epochs vs the dual arm's
> 233 settled, 60 stations vs 50, different iono posture, matched settled comparison still
> owed — was the right one and it fired.  **Surviving: the STRUCTURAL result** (pinned pair
> datum-fixed, all content on the third band, gate clean at default tolerance).  **Not
> surviving: the "CALIBRATED" verdict**, re-derived on settled data in §13.5 P1.

Built 2026-08-12 (`gnss_processor_slant.exe`, sha256 `33052874…`):

- **`network.code_bias_gf_datum`** — the per-satellite geometry-free pin,
  `(d_2 − d_1)/(μ2 − μ1) = 0` on the IF-pinned pair (`NetworkSbasis.cpp`, folded at
  `kCodeBiasGaugeWeight` beside the IF pin).  Counters `n_gf_pin_sats` and `n_gf_only_pair`
  (satellites with no third band, whose code bias is then fully datum-fixed) ride the
  existing `Network[W6-cb]` line.
- **The loader gate**: the W6 chain force-enables `ionosphere_weighting` only when the GF
  datum is OFF, so `estimate_sat_code_bias` + no SH + no GIM is reachable;
  `ApplicationNetworkSetup` gained the matching no-GIM branch.  Interlocks, all hard
  errors: GF datum without `estimate_sat_code_bias` (nothing to gauge); GF datum with
  `estimate_iono_sh` **or** `ionosphere_weighting` (rd 3b only exists when the ionosphere is
  unconstrained, and under a single-layer constraint the GF bias IS the estimable DCB —
  pinning it deletes the product); GF datum with `frequency_mode: dual` (IF + GF fix both
  pinned channels, so every emitted B record would be the datum itself).

**Arm `_simnet60_tri_slant`** (60 stations, triple, free slant iono, no SH/GIM, default
rank-gate tolerance, 20 epochs): **20/20 emitted, 0 withheld**, `declared_dim` 1 =
`n_below` 1, post-fit σ̂₀ **1.073 code**.  Gauge rows folded: 32 IF + 32 GF + 3 sum-to-zero,
`n_gf_only_pair` = 0.  The structural prediction holds to the millimetre — the pinned pair
is datum-fixed and the third band carries everything:

| channel | emitted rms | residual vs transformed truth |
|---|---|---|
| C1C | 0.0022 m | **0.0022 m** |
| C2W | 0.0032 m | **0.0032 m** |
| **C5Q** | 3.889 m | **0.0760 m** |
| **variance factor** | | **1.886** ⚠️ IGNITION ONLY — SUPERSEDED, see the banner above and §13.5 |

⚠️ **The grader needs `--gf-datum` (auto-detected from the console).**  Under the slant
datum the truth projection removes a SECOND direction:

    t̃_j = t_j − (μ_IF·t) − μ_j·(μ_GF·t)

derived from the paper's own orthogonality properties (μ_IF·e = 1, μ_IF·μ = 0, Eq. 26;
μ_GF·e = 0, μ_GF·μ = 1, Eq. 31), which is why it forces t̃ = 0 on both pinned channels
rather than being told to.  Grading this arm with the single-layer transform reports
**2.3 m / 3.7 m phantom residuals** on two channels that are datum-fixed to zero — exactly
what happened on the first attempt, because the loader had no "armed" log line for the
auto-detect to find.  That silent-arming gap is fixed (invariant #4), and it is a reminder
that a datum change is also a GRADER change.

### 13.5 THE MATCHED MATRIX — settled, and it corrects §13.3.2

One fixture (`runs/simnet60_tri`), same 60 stations, same seeds, 480 epochs = 4 h, graded
on the settled half.  `rank_gate_tol` and `rx_code_bias_datum` matched across all three
arms so each adjacent pair is ONE knob.  Dual+slant is structurally illegal, so frequency
and posture separate in two steps, not one.

| | **A** dual + SH | **B** triple + SH | **C** triple + slant + GF |
|---|---|---|---|
| epochs emitted | 456/480 | 476/480 | 474/480 |
| σ̂₀ scale s0 | 1.160 | 1.909 | 1.997 |
| **cb residual rms** | 1.131 m | 0.962 m | **0.568 m** |
| — pinned pair | C1C 0.832 / C2W 1.366 | C1C 0.821 / C2W 1.342 | **C1C 0.014 / C2W 0.023** |
| — third band | — | C5Q 0.548 | C5Q 0.983 |
| cb vf (published) | 400 776 | 156 743 | 8 756 |
| **cb vf (s0-corrected)** | **541 000** | **570 600** | **34 900** |
| clock residual / contrast | 0.592 / 0.773 m | 1.211 / 1.721 m | 1.119 / 1.590 m |
| clock vf (s0-corrected) | 5 010 | 55 970 | 47 000 |
| ZWD residual / vf | 0.549 m / 2 843 | 0.873 m / 11 828 | 0.872 m / 11 661 |
| **consumer 3D rms** | 1.126 m | **0.290 m** | 0.514 m |

**⚠️ CORRECTION to §13.3.2: "vf 1.886 → CALIBRATED" was an IGNITION ARTIFACT** — the
20-epoch probe measured a converging formal σ against a not-yet-grown true error, precisely
the §9.5 finding-2 signature this fixture exists to expose, reproduced on a new channel by
the very arm supposed to demonstrate the cure.

**P1 — the discriminator answers POSTURE, not frequency.**  On the s0-corrected scale (vf
is deflated by s0², and s0 differs 1.16 → 2.00 across arms, so the raw numbers are NOT
poolable — the grader says so and it matters here): A 541 000 ≈ B 570 600 ≫ C 34 900.
**The third frequency alone buys nothing for code-bias calibration; the slant datum buys
~16×.**  And 34 900 is still σ optimistic by ~190× *in σ* — the slant posture improves the
channel by more than an order of magnitude and does **not** make it calibrated.

**P2 — CONFIRMED, and it retires a standing objection.**  The recorded 2026-08-04 "slant is
2.2× WORSE" result was dual-frequency and 30 epochs (§14.4 posture B) and does **not**
reproduce here: the third frequency improves the consumer **3.9×** (1.126 → 0.290 m), and
the slant posture gives 1.8× of that back while still landing **2.2× better than the field's
own dual+SH posture**.  Slant does not cost position once the code biases have a third band
to live in.

**P3 — and the surprise.**  The clock and ZWD channels go the OTHER way: A → B degrades the
clock contrast 0.773 → 1.721 m (2.2×) and ZWD 0.549 → 0.873 m, and the posture does not
recover either.  So the third band helps the code bias and the consumer while hurting the
clock and troposphere.  Hypotheses and the blocked discriminator: §13.8.4.

### 13.4 Pre-registered, for when the posture runs

| prediction | reasoning |
|---|---|
| **the code-bias vf should come DOWN by orders of magnitude** | at f = 2 the channel's information is the soft single-layer tie (hence vf ≈ 4.9e+05); at f = 3 under slant the modernized DCB is determined by *data*.  If it does not, the problem was never the ionosphere model |
| **the exchange coefficient should collapse** | §12.7 measured **+5.67 TECU per metre** of fleet-mean rx-DCB error (0.72× ideal absorption) — ⚠ **both figures SUPERSEDED on the same products by +7.69 / 0.97×**; see §12.9's script-version note.  With the GF explicitly pinned and lumped into the iono that trade is closed by construction — the remaining clock↔iono correlation is then whatever is left after removing it, the actual quantity the field campaign wants |
| **the ionosphere should get BETTER and the position WORSE** | recorded measurement, 2026-08-04, 63 sta, 30 ep, ignition-only: dropping SH+W6 for pure slant ran faster but cost the consumer — full figures §14.4 posture B.  A ≥ 2 h triple-frequency arm is the first honest test of whether that regression survives when the third band gives the code biases somewhere to live |

### 13.6 THE AMBIGUITY-INTEGER GRADE — the first absolute integer test, and the
### lane ladder measured (2026-08-12, HANDOFF_slant_triple_next §3)

*(That handoff is now `docs/attic/HANDOFF_slant_triple_next.md`; **its §3 IS this
section**, and its method rules and traps are **§13.8**.)*

`_simnet60_tri_slant_1h_ddint` — posture C + `trace_dd_fractional` (stride 1, max_pairs
2000; note the real knob is `trace_dd_fractional_epoch_stride`), 120 epochs = 1 h,
`gnss_processor_matrix.exe` (DA2F9AC0), 116 emitted (0 observability, 4 A4).  Grader:
`scripts/simnet_amb_integer.py` (new; `--selftest` covers the slip-accumulation join and
the lane matching).  The graded quantity is **e = dd_cyc − DD_true in FULL cycles** against
the fixture's known integers — truth differenced in Python ints (N is km-scale by
construction), the trace's own `frac_cyc` used as a per-row join check (229 961 rows, **0
dropped**).

Integer-structure questions are decided per-row and are 1-h-LEGAL (§13.8.1); the per-band
absolute magnitudes below shrink with settling and must not be quoted as settled levels.

| channel | n | med(e) | med\|e\| | p90\|e\| | ≤0.10 cyc | ≤0.25 cyc | **round(e)=0** |
|---|---|---|---|---|---|---|---|
| L1C (per-band) | 76 485 | +0.055 | 4.956 | 20.39 | 1.9 % | 4.4 % | **8.3 %** |
| L2W (per-band) | 76 911 | +0.072 | 5.034 | 20.35 | 1.5 % | 3.3 % | **8.3 %** |
| L5Q (per-band) | 76 565 | +0.092 | 5.076 | 20.40 | 1.6 % | 3.6 % | **8.1 %** |
| **WL(1,2)** | 64 642 | +0.027 | **0.386** | 1.22 | 15.6 % | 34.9 % | **60.2 %** |
| **EWL(2,5)** | 64 690 | −0.001 | **0.081** | 0.27 | 57.7 % | 88.2 % | **98.2 %** |

Two competing predictions were registered before the run.  **Pessimistic (the standing
record):** the NL blocker is iono REPRESENTATION — per-LOS slant iono scatters 0.26–0.37 m
against a WL→NL bridge requirement of 0.198 m, so `med|frac|` sits at 0.25 (uniform), and
the slant posture removes the SH constraint, leaving the per-LOS iono freer still ⇒ the NL
should get *worse*.  **Optimistic (what the third band opens):** at f = 3 the extra-wide-lane
(L2,L5) has λ ≈ 5.86 m, which 0.3 m of iono scatter cannot break, so the EWL → WL → NL
cascade becomes available on this fixture for the first time.  The honest expected shape
was **EWL near-integer, WL plausible, NL still not**, with the value in measuring *which
rung breaks* — which no field arm can do.

**That shape is confirmed exactly, and now with absolute numbers.**

| # | finding |
|---|---|
| 1 | **The EWL is at its own TRUE integer 98.2 % of the time** (med\|e\| 0.081 cyc).  Not "near an integer" — near the *right* integer, verified against truth.  The lane cascade's first rung exists on this fixture |
| 2 | **The field's fractional instrument was hiding the error scale.**  The raw per-band DDs are not "0.25 cyc off" — they are **~5 cycles off with a ±20-cycle p90 tail**, and only ~8 % sit at their true integer.  med\|frac\| 0.25 uniform (the field reads a uniform **0.248**) was the "off by 47.3" class, not the "off by 0.3" class — the distinction §13.8.2 names, measured for the first time |
| 3 | **The WL:EWL ratio is pure DD-iono arithmetic**: measured med\|e\| ratio 4.8× vs 5.1× predicted from the iono coefficients (WL 1.49 vs EWL 0.29 cyc per metre of L1 DD-iono).  The two lanes share one driver and it is the ionosphere, quantitatively |
| 4 | **The per-band bands carry an EXTRA error component equal-in-cycles across bands** — per-band:WL measured 12.8× vs 3.5× iono-predicted, and all three bands sit at the same ~5.0 cyc med\|e\|, i.e. it cancels EXACTLY in every between-band difference.  Leading candidate was residual carrier **phase-windup** — band-common in cycles by construction.  **⚠ FALSIFIED 2026-09-08 by the owed regression (§13.8.6): wind-up explains 0.08 % of the cycle-common variance and is ~144x too small in magnitude.**  The mechanism is OPEN again — see §13.8.6 for what the successor must satisfy |

Sweep items (the absorbed handoff's §4 sweep, 1-h-legal parts — anchor map §13.8): datum
inventory 67 = 32 IF + 3 STZ + 32 GF with `n_gf_only_pair` 0; `declared_dim = n_below = 1`;
`cbpin=` on the S line; the pinned pair datum-fixed to millimetres on truth (**C1C 0.0023 m
= 2.3 mm, C2W 0.0032 m = 3.2 mm** rms — the "holds to the millimetre" claim, now
attributed: it is the DATUM channels that are exact; C5Q, the information-carrying
modernized-DCB channel, is 0.086 m = **8.6 cm** at 1 h and still converging).  Consumer at
1 h: 3D rms 2.64 m, **almost entirely +UP (mean +2.59 m ≈ +2.6 m)** — the classic slant-iono
ignition signature (per-LOS iono converging from prior pushes UP); §13.5's settled 0.514 m
says it decays; report-only here.  The exchange was NOT run at 1 h — its coefficient is
barred by the 1-h rule and its home is the settled arm, where §13.4's collapse prediction
lives.

**What this changes upstream:** the standing "MW/WL as conditioning — do NOT arm;
net-harmful" verdict was measured at dual frequency, where GPS has no EWL band.  §13.6 does
not overturn it — it bounds it: at f = 3 the EWL DDs are already 98 % correct-integer as
FLOATS, so an EWL-first fixing cascade has a real substrate here.  Whether ARMING the lane
machinery helps the solve is a separate, unanswered question — and the eq34+carry+EWL trio
the handoff's §3.3 named (`network.mw_wl.ewl_enable` requires `carry_invariant`, which
requires `phase_datum_eq34`) was BLOCKED on this sim until seed-round existed (the
zero-target cold pins would inject the §12.8 km-scale artifact into a posture with no SH
gate to catch it; `docs/SPEC_eq34_seed_round.md`).  Since unblocked and RUN: **§13.7, and
it DIVERGES.**

### 13.7 THE SETTLED TRIO ARM — unblocked, and it DIVERGES (2026-08-12)

Same-build (sr.exe 6B1EF5D6) 480-epoch A/B, settled half: `_simnet60_tri_slant_480_ddint`
(float, 474/480) vs `_simnet60_tri_slant_trio_480` (eq34+carry_invariant+ewl+seed_round,
**480/480 — the seed-round card holds; no withhold, max |y| in bound**).

| settled half | float | trio |
|---|---|---|
| per-band med\|e\| | 8.0–9.4 cyc | **~1 720 cyc (p90 ~7 350)** |
| EWL at true integer | **74.7 %** (0.256 cyc) | 5.0 % |
| WL at true integer | 26.2 % | 0.7 % |

**The trio arm has a SLOW RUNAWAY the 20-epoch smoke could not see** (smoke EWL 94 %;
settling degrades monotonically — full-window trio EWL 32 % → settled 5 %).  Attribution so
far: the **WL-2 lane is exonerated** — its run line reads "0 rows decided, rows NOT folded
into the solve" (statistic-only over 226 421 cycles).  The runaway therefore lives in
eq34 + carry_invariant + the every-epoch seed re-round: 130 290 re-rounds (271/epoch —
under carry_invariant no carry ever exists, so every pin re-rounds every epoch), 591
(sat,σ) re-references, 0 carries shifted/erased — the invariant DD carry refolding across a
stepping pin frame, invariant #1/#7's ratchet shape.  **Do NOT arm the trio posture for
settled work** pending a mechanism card; the FLOAT posture stays the graded track.  Two
float-side notes: the lanes DEGRADE with settling (1 h EWL 96 % → settled 74.7 % — the
§13.6 1 h numbers were the optimistic window), and the field's frac-only instrument would
read this runaway as a NORMAL uniform fractional distribution — only the truth grade can
see it.

#### 13.7.1 The D5 hypothesis, TESTED and REJECTED (2026-08-21)

A config audit raised the objection that this arm runs `carry_invariant` with `q_z = 0`
and `carry_invariant_innov_gate` OFF — a stiff zero-q carry — and that the runaway might
therefore be an artifact of an unguarded posture rather than a property of the estimator.
**The objection's premise is half right and its conclusion is wrong.  Tested; §13.7 stands.**

*What the audit got right.*  The arm does run `q_z = 0` with D5 off, and this fixture
injects **zero** cycle slips (`runs/simnet60_tri/*/truth_slips.csv`: 61 files, 0 data rows),
so the arm's `mw_detect` + `if_phase_td` are armed but idle **by construction**.  D5 is the
only response that fires on MODEL ERROR rather than on a detected raw slip, so on this arm
the effective event response really is nil.

*What it got wrong.*  Not "unguarded" in the invariant-#1 sense — the C3c funnel
`carry_slipped_ambs` is gated on `if (carry_invariant)` alone and is live with D5 off; the
config simply has nothing to trigger it.  (Corpus-wide: 633 configs arm `carry_invariant`
and **zero** lack a response entirely.  Loader interlock now at
`GnssConfigLoadNetwork.cpp:6759`.)

*The test.*  Three same-build 480-epoch arms on the CURRENT Release build, single knob,
matched populations (n = 18 980/band settled, 57 272 full).  Configs
`_simnet60_tri_slant_trio_480_ab_{base,d5,d5cal}.json`; the historical
`runs/simnet60_tri_slant_trio_480/` is left untouched as the §13.7 record.

| settled half | control | D5 (k=4, uncal) | D5 CALIBRATED | float twin |
|---|---|---|---|---|
| per-band med\|e\| L1C/L2W/L5Q | 1780 / 1735 / 1745 | 1522 / 1490 / 1492 | **1503 / 1481 / 1483** | 8.0–9.4 |
| EWL med\|e\| | 13.58 | 3.24 | **10.49** | — |
| EWL at true integer | 3.3 % | 11.2 % | **3.6 %** | 74.7 % |
| WL at true integer | 0.3 % | 0.8 % | **0.6 %** | 26.2 % |
| D5 flags | — | 140 126 | **25 709** | — |
| chord births / slip refloats | 3 984 / 1 956 | 143 405 / 141 377 | 29 701 / 27 673 | — |

**The control reproduces §13.7 on the current build** (settled per-band 1735–1780 vs the
recorded ~1720; full-window EWL 33.1 % → settled 3.3 %, against the recorded 32 % → 5 %),
so the contrast is sound and the earlier worry that a newer build had moved the headline
does not apply here.

**Read the counters before the grade (invariant #4).**  The uncalibrated arm flagged
140 126 times over 480 epochs against 960 carried DDs — ~292/epoch, a ~30 % turnover of the
whole carry every epoch, driving births 3 984 → 143 405.  That is not a gate screening a
runaway; it is the carry being dismantled and rebuilt continuously, so its apparent EWL win
(3.3 % → 11.2 %) is the carry getting OUT OF THE WAY — the arm regressing toward the float
twin that already beats it ~200×.  The loader's own posture ledger predicts exactly this:
raw formal Cov(v) is 10–30× optimistic (M2), so `innov_k = 4` is correct ONLY with
`innov_calibrate`.

**Calibrated, the win evaporates.**  5.4× fewer flags (25 709), and settled EWL integer
recovery falls back to control — **3.6 % vs 3.3 %** — while per-band med\|e\| improves only
15.6 % (1780 → 1503) and stays **~160× worse than the float twin**.  The gate is live, not
dead wiring (27 673 refloats, 14× the control), and it still does not touch the runaway.

⇒ **The runaway is a property of the estimator on this posture, not an artifact of the
missing D5 gate.**  §13.7's "do NOT arm the trio posture for settled work" and the §13.8
rule-5 injection freeze both stand unchanged.  What this does retire is one candidate
mechanism: the mechanism card owed by §13.7 should not spend another arm on the innovation
gate.  Note also that arming D5 here would have looked like a large win to anyone who read
the grade without the counters.

### 13.8 GRADING RULES AND TRAPS for the triple/slant posture
**Rule 13.8.x — the DD trace has ONE reference satellite per (constellation, signal), and it is in every
chord (added 2026-09-06, SPEC_amb_iono §6.12).**  `NetworkDdFrac.cpp` picks the lexically-lowest satellite
present at ≥ 2 stations (G01 / E02 all day on this fixture); coverage follows its footprint (21–41 of 60
stations per hour) and every graded e_i = s_i − s_ref carries its between-station SD error.  When it is
excluded for eclipse and returns, the whole trace reads its newborn legs as a network-wide episode
(as-traced L1C 0.52–0.59 cyc at h7 / h19 vs a ref-free 0.08–0.11 that is flat all day).  Grade ref-free:
`scripts/simnet_dd_reref.py <fixture> <run>` (hourly, per constellation, with the leg-age curve) or
`simnet_amb_integer.py --ref-free` (summary block).  Every DD number in §13–§18 quoted before 2026-09-06 is
as-traced; the settled-quarter figures in particular (0.42 / 53 %) are the reference's post-return
convergence, not the network's.  A producer-side reference policy (`network.trace_dd_fractional_ref: "oldest"`) is
now BUILT and A/B'd (§6.13.3, ledger RECOMMEND_SCOPED): it improves the AS-TRACED grade 25-42 % at a
returned-reference hour but lands 2-3x ABOVE the ref-free grade, so the ref-free grader stays the
reference-independent instrument regardless and the goldens keep `"lowest"` for continuity.

**Rule 13.8.y — the TRUTH-FREE version of that check, and why the FIELD is a different story
(added 2026-09-06, SPEC_amb_iono §6.13).**  On a trace with no truth the reference's share can still be
removed: within a group it is COMMON, so a circular median estimates it (`scripts/dd_frac_reref.py`).  Three
traps, each of which produced a wrong reading here before it was caught.

1. **The estimator shrinks residuals by itself.**  Subtracting a group's own circular median cuts med|f|
   from 0.251 to 0.145 on PURE UNIFORM NOISE — a 42 % "improvement" that is nothing.  Quote the GAP to a
   size-matched Monte-Carlo null, never the raw reduction.
2. **A triangle-closure structure test is confounded.**  A DD is per-station additive BY CONSTRUCTION, so
   triangles close for any fixed satellite whether or not the reference matters, and a circular median IS
   one of those data points.  Its null must therefore carry the DD structure with no reference term; the
   DOY-129 closure reads as real structure against an independent-uniform null (0.182 vs 0.250) and as
   NOTHING against the correct one (0.182 vs 0.172).
3. **⚠ A wrapped grade is only meaningful where the underlying errors are SUB-CYCLE** — which is exactly
   §13.8.2's distinction, and it is what separates this fixture from the field.  Two arms picked as
   "controls" (`camp60wu_f3`, `camp60wu_quiet24h_sticky`) have 2–20-CYCLE DD errors; wrapped, their
   fractions are uniform for reasons that have nothing to do with the reference, and they looked like a
   clean refutation of §6.12.  Check the truth-graded magnitude before reading a wrapped file, and pick a
   control arm by NAME from the document that made the claim.

Result on the field (`runs/fld63_stk_on`, DOY-129, 63 sta, 12 h): **no detectable common component on any
signal** — L1C resultant R 0.468 vs null 0.473 and ref-free gap +0.008, L2W 0.447/0.452/+0.010, L1W
0.441/0.438/+0.006 — while the SAME tool fires hard on the §6.12 base (`nonint_losblock24h_hv`: R 0.610 vs
0.430, gap −0.047) and its truth-free 0.079 reproduces that run's truth-graded 0.089.  The field reference
also rotates on its own (8–11 switches over 3–9 satellites) with no post-switch degradation.  So the field's
uniform-0.25 histogram is NOT the reference artifact — consistent with §13.8.2's finding that the field DDs
are ~5 cycles off with a ±20-cycle p90 tail, i.e. the "off by 47.3" class, where a wrapped fraction cannot
carry a common component in the first place.  What the field DOES inherit from §6.12 is the COVERAGE half:
a chord exists only for pairs that both see the elected reference, so per-station and per-region field splits
off this trace still compare rotating populations.

### (absorbed 2026-08-15 from `docs/HANDOFF_slant_triple_next.md`, now attic'd)

That handoff was the work order for §13.6 and §13.4 ("§3 + §4 EXECUTED 2026-08-12 — the
measured record is SPEC §13.6"); the plan is dropped and the method rules, which existed
nowhere else, are carried here.  Anchor map for its referrers: **its §1 → §13.8.0** ·
**§2 (the 1 h rule) → §13.8.1** · **§3 → §13.6** (the executed record) · **§3.2 → §13.8.2**
· **§3.3 → §13.7** (the trio arm ran and DIVERGES) · **§4 (the sweep) → §13.6**, closing
paragraph · **§5 → §13.8.4** · **§7 → §13.8.3**.

**13.8.0 The posture-C build inventory** (the table `project_family_gf_datum_evidence`
cites as "§1 lists every existing piece").  Posture C = triple frequency + free slant
ionosphere + geometry-free code-bias datum; config
`config/test_suite/_simnet60_m_c_tri_slant.json`, fixture `runs/simnet60_tri` (60 solve + 1
consumer, 480 epochs @ 30 s, GPS C1C/L1C + C2W/L2W + C5Q/L5Q, injected synthetic L5 OSB).

| piece | where |
|---|---|
| third sim band (opt-in, byte-identical off) | `signals.gps_l5` / `signals.galileo_e5b` / `signals.synth_osb_amp_m` (§13.1) |
| GF code-bias datum (Odijk rd 3b) | `network.code_bias_gf_datum` → `NetworkSbasis::buildCodeBiasDatumRows` (§13.3.2) |
| loader path for slant + W6 | the W6 chain force-enables `ionosphere_weighting` **only when the GF datum is OFF**; `ApplicationNetworkSetup` has the matching no-GIM branch |
| satellite bias σ | `network.trace_sat_bias_states` → `network_sat_bias_states.csv` |
| receiver clock + DCB σ | `network.trace_rx_states` → `network_rx_states.csv` |
| `cbpin=` S-line fill | `NetworkSolPrep` — declared but never populated since B6 (§14.6) |
| truth grader, slant-aware | `scripts/simnet_grade.py --gf-datum` (auto-detects) |
| exchange decomposition | `scripts/simnet_exchange.py` |

Release exe for the matrix: `build/x64-Release/gnss_processor_matrix.exe` sha256
`da2f9ac0…` — **re-copy AND re-hash after any rebuild; a renamed exe is a snapshot, not a
symlink.**

#### 13.8.1 ⚠️ THE 1 h RULE — the legal / not-legal lists

A 120-epoch arm is **1 hour, which is inside ignition for every level and calibration
channel on this fixture.**  §13.6 cites this list as **"1-h-LEGAL (§13.8.1)"** — the
absorbed handoff called it its §2.  The cautionary case is §13.3.2 → §13.5 (the 20-epoch
probe's "vf 1.886 = CALIBRATED" against the settled 8 756).

| | list |
|---|---|
| **LEGAL at 1 h** — structural / wiring questions decided in the first epochs and not converging | rank / gate behaviour; datum realization (are the pinned channels fixed?); which instruments fire; counter sanity; **the ambiguity-integer question**; QC tier opportunity counts; **any ratio between arms run on the same window** |
| **NOT LEGAL at 1 h** | variance factors; the ionosphere level; the exchange coefficient; "is it calibrated"; consumer verdicts; anything compared against the §13.5 settled numbers |

**Enforcement:** a 1 h number quoted must carry the window **in the same sentence**.  There
is no cost argument for grading a level channel on 1 h — the 4 h fixture exists and the
whole 480-epoch matrix ran in under five minutes (~3 min/arm at 60 stations).

#### 13.8.2 The truth-DD recipe, and the km-scale integer trap

Estimability first: Odijk Table 3 (CC-R) makes the estimable ambiguity a **double
difference**, `z̃^s_{r,j} = [z^s_{r,j} − z^1_{r,j}] − [z^s_{1,j} − z^1_{1,j}]` (r ≥ 2,
s ≥ 2), an integer **BY CONSTRUCTION IN EVERY POSTURE** — the iono parameterization
(Table 8) changes the biases and the iono, not the ambiguity's integer nature.  So "are the
ambiguities near integer" is a question about the **ESTIMATES**, not the model.  Under the
eq34 chain datum the realized basis is DD chords on a spanning tree rather than a single
pivot pair (`NetworkChainDatum.cpp`) — the same object.

| step | recipe |
|---|---|
| **estimates** | arm `network.trace_dd_fractional` (+ `trace_dd_fractional_stride: 1`, `trace_dd_fractional_max_pairs: 2000`; the real stride knob is `trace_dd_fractional_epoch_stride`).  Header `week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,…` carries all four identifiers needed to reconstruct the truth DD |
| **truth** | `runs/simnet60_tri/<station>/truth_ambiguities.csv` (`satellite,signal,N_int,wavelength_m`; **90 rows = 30 sats × 3 signals per station**) plus `truth_slips.csv` deltas.  The true value at epoch k is `N_int + Σ truth_slips.delta_cycles` up to k — the file holds the **PRE-SLIP** integer (§8 trap 8).  This fixture is quiet (`rate_base` 0) so the sum is empty, **but write the code to add it anyway** |
| **the DD truth**, per (signal, sat_i, ref_sat, sta_a, sta_b) | `DD_true = [N(a,i) − N(a,ref)] − [N(b,i) − N(b,ref)]`, and the graded quantity is `DD_est − DD_true` **in cycles**, not its fractional part.  A fractional-part test cannot separate "off by 0.3 cycles" from "off by 47.3", and the field has only ever had the former |

⚠️ **THE TRAP: the integers are km-scale by construction.**  Under the legacy regime the
sim draws `N ∈ [1e5, 1e6)` per (station, band), so `N_int` ≈ 905 116 in the sample row.
**Do the differencing in Python ints BEFORE converting to float**, or you grade
floating-point cancellation.  The same km-scale draw is what makes zero-target cold
ambiguity pins inject km-scale tension (§12.8.1) — the defect §15.1's field-N regime fixes
at the fixture level.

#### 13.8.3 Trap register for this posture (do not re-litigate)

| trap | statement |
|---|---|
| **a datum change is also a GRADER change** | `simnet_grade.py` needs `--gf-datum` for the slant posture (auto-detected from the console line the loader now prints).  Without it the truth projection omits a direction and reports **2.3 / 3.7 m PHANTOM residuals** on channels datum-fixed to zero (§13.3.2) |
| **vf is deflated by s0²** | s0 varies **1.16 → 2.00** across postures.  **Never pool or compare raw vf across arms** without the correction; §13.5 quotes both |
| **never assert a bias SIGN verbally** | read the coefficient off the model or measure the slope, and say which.  Two conventions bit in one session: the absorbed IF lump is **−d_IF** (measured slope −1.08) and the exchange coefficient is **+**, derived from `resolveReceiverBias`.  Full statement §14.6 |
| **the COD product has NO GPS band-5 rows at all** | the third band's OSB truth is entirely the injected synthetic delta (§13.1) |
| **already measured, do not repeat** | station count is not the observability lever (the L2W gauge was n-invariant and is GONE at f = 3, §12.2/§13.3.1); `sbasis_complete` passes the gate and DESTROYS the ionosphere on a cold start (§12.2 rung 3); `cb_kaula_prior` **FALSIFIED** alone and paired (§12.8.3); the elevation-cutoff truth A/B **INVERTS** the field verdict — 15° beats 25° on the clock channels because the cutoff FEEDS the exchange (§12.9) |

#### 13.8.4 OWED — why the third band HURTS the clock and ZWD

**Open question, no hypothesis tested.**  §13.5 measured it in BOTH postures: A → B
degrades the clock contrast **0.773 → 1.721 m** (2.2×) and ZWD **0.549 → 0.873 m**, and
posture C does not recover either.  Three candidates:

1. the extra band adds **32 satellite phase biases + n·m ambiguities** against FLOAT
   ambiguities with no AR — more nuisance parameters than geometry-side information;
2. the synthetic L5 carries the same `code_sigma_zen_m` (**0.30 m**) as L1/L2 plus a
   hand-set `code_amp_gps_l5_m` (**0.015 m**) multipath amplitude, while real L5 is *quieter*
   than L1 C/A — the fixture may be **under-weighting its own third band**;
3. the synthetic L5 OSB (**0.60 m** amplitude) is larger than the product's L1/L2 OSBs on
   some satellites, so the third band may be dominating code-bias estimation.

The cheapest discriminator is a **third-band noise-realism sweep** (vary
`noise.code_sigma_zen_m` per band).  ⚠️ **It is BLOCKED on a sim change**: the `noise` block
is **band-common today**, so a per-band σ must be added first, following the same opt-in
byte-identity-off pattern the third band used.  **Do that before concluding anything about
f = 3 and clocks.**

#### 13.8.5 Reproduction (posture C)

```bash
python scripts/simnet_grade.py runs/simnet60_tri \
       --run runs/simnet60_m_c_tri_slant/run \
       --products runs/simnet60_m_c_tri_slant/products.txt --settle-frac 0.5

python scripts/simnet_exchange.py runs/simnet60_tri \
       --products runs/simnet60_m_c_tri_slant/products.txt \
       --run runs/simnet60_m_c_tri_slant/run --settle-frac 0.5
```

⚠️ In the slant posture the **a₀₀ channel DOES NOT EXIST** (no SH field), so
`simnet_exchange.py` has only the clock / rx-DCB / cb_rms channels.

**The 1 h arm recipe:** copy `_simnet60_m_c_tri_slant.json`, set
`processing.max_duration_s: 3570.0` (= 120 epochs), give it **its own
`product_output_path` and its own CWD**, and add the `trace_dd_fractional` block — the real
stride knob is `trace_dd_fractional_epoch_stride`.  Then grade in CYCLES with
`scripts/simnet_amb_integer.py` (§13.6), and obey §13.8.1.

#### 13.8.6 THE WIND-UP DISCRIMINATOR — RUN 2026-09-08, and the candidate is FALSIFIED

`scripts/windup_dd_regress.py` (selftest `--selftest`: one bed where e IS the wind-up DD, one where it is
independent).  The regressor is **truth, not a model** — the simulator injects wind-up per (station,
satellite, epoch) through `EphemerisProvider::phaseWindupMetres(sat, r_rx, ...)` and writes the accumulated
band-common cycle count to `<fixture>/<station>/truth_windup.csv`, so

    wu_dd = [wu(a,sat) - wu(a,ref)] - [wu(b,sat) - wu(b,ref)]      (cycles, exact)

is differenced the same way the graded `e` is.  Two arms, both with **0 rows dropped** for missing wind-up
or ambiguity truth (116,453 and 213,150 rows), which is also the fixture-pairing check.

**The cheap test first.**  A range-domain error (orbit, clock, tropo, antenna) is common in METRES, so in
cycles it scales as 1/lambda and L5 would read **25 % SMALLER** than L1.  Measured: 4.956 / 5.034 / 5.076 —
a 2.4 % spread with L5 slightly LARGER.  So the term is a CYCLE-domain quantity and wind-up is the only
standard candidate.  That is what made the regression worth running.

**The magnitude gate vetoes it before any fit.**  The injected wind-up accumulates to at most ~1 cycle over
24 h (measured range [-0.93, +0.98]), and a double difference cancels most of that because wind-up varies
smoothly with geometry: **med|wu_dd| = 0.030 cyc** (posture C) and **0.060 cyc** (24 h arm), against a
per-band med|e| of 6.0 and 2.9.  Wind-up is 0.4-2 % of the error it was proposed to explain.

**The sharpest form of the test.**  A power-law fit gives med|e| ~ lambda^**+0.39** — between pure
cycle-common (p = 0) and iono (p = +1), and decisively not range-like (p = -1) — so the per-band error is a
MIXTURE and testing wind-up against raw `e` dilutes it.  Decompose each chord's three bands into

    e_i = A + B * (lambda_i / lambda_1)

with **A** the cycle-common part (exactly what wind-up would be) and **B** the part proportional to lambda
(iono enters as -dI1 * lambda_i / lambda_1^2 cycles).  Then regress the EXTRACTED A against wu_dd: if
wind-up hides anywhere, it is in A.

| | posture C `simnet60_tri_slant_480_ddint` | 24 h `camp60wu_quiet24h_sticky` |
|---|---|---|
| med\|A\| (cycle-common) | **4.330 cyc** (sd 21.26) | **0.181 cyc** (sd 10.61) |
| med\|B\| (iono-shaped) | 2.823 cyc (sd 7.14) | 2.689 cyc (sd 4.44) |
| med\|wu_dd\| | 0.030 cyc | 0.060 cyc |
| A vs wu_dd: slope, r, R^2 | -2.223, **-0.029**, **0.00083** | +0.495, **+0.027**, **0.00072** |
| wind-up share of cycle-common variance | **0.083 %** | **0.072 %** |
| sd(A) - sd(A - wu_dd) | 21.257 -> 21.267 (WORSE) | 10.605 -> 10.606 (WORSE) |

**FALSIFIED, four independent ways.**  (1) Magnitude: 144x too small in the posture-C arm.  (2) Correlation:
R^2 = 0.0008 and 0.0007 — wind-up accounts for under a tenth of a percent of the variance it was nominated
to explain.  (3) Subtracting it makes the residual WORSE on every band in both arms, the signature of an
uncorrelated regressor (variances add).  (4) The (meaningless) slope **flips sign between arms**, -2.22 vs
+0.50 — a real physical coupling does not change sign with the posture.

⚠ The conclusion is robust to whether the PRODUCER models wind-up.  If it models it correctly the injected
signal cancels and never reaches `e`; if it does not, the full wu_dd would appear with slope 1.  Neither
branch can make a 0.03 cyc quantity into a 4.3 cyc error, so wind-up is out as the CAUSE either way.

**⚠ MECHANISM OPEN — do not guess it into the record.**  What the successor candidate must satisfy, now
quantified:

1. **Cycle-domain**, not metre-domain: it lives in A, whose lambda exponent is ~0, and the total fit is
   lambda^+0.39 only because B is mixed in.
2. **~4.3 cyc median with sd ~21** in the posture-C arm — large, heavy-tailed, and NOT a small bias.
3. **Posture-dependent**: A is ~24x smaller in the 24 h quiet/sticky arm (0.181 vs 4.330) while B is
   essentially unchanged (2.69 vs 2.82).  So the band-common term of finding 4 is **not a universal feature
   of the estimator** — it is specific to the posture-C arm.  ⚠ That contrast is NOT a controlled A/B: the
   two arms differ in fixture, duration, datum and posture at once, so the next step is a single-knob ladder
   over the posture-C differences (free slant iono, GF code-bias datum, triple frequency) measuring med|A|
   at each rung.  `windup_dd_regress.py` reports med|A| directly, so that ladder is now cheap.
4. It must NOT be wind-up, and it must NOT be iono (that is B, already accounted and matching finding 3's
   DD-iono arithmetic).

**ADDENDUM 2026-09-09 — the term is ABSENT on the F3 golden posture.**  Same instrument, same fixture
(`runs/simnet60_quiet_wu93_24h`), run on the 24 h F3 arm `runs/nonint_losblock24h_hv` (stride 8, 193,388
three-band chords): **med|A| = 0.108 cyc** (sd 5.84) and **med|B| = 0.206 cyc** (sd 0.79) — against posture C's
4.330 / 2.823 and the diagonal-carry sticky arm's 0.181 / 2.689.  Wind-up's share of the cycle-common variance
is 0.23 % (slope 0.49, r +0.048), i.e. still nothing.  So the station-wide whitened block removes BOTH terms:
the iono-shaped B (the over-count of SPEC_amb_iono_split_overcount §3, as predicted) and — without being
designed for it — the cycle-common A that posture C carried at 4.3 cyc.  Item 3's posture ladder is therefore
DE-MOTIVATED as a stand-alone card: the band-common term does not exist on the posture the goldens run, and
anyone re-opening it should first re-run posture C itself under F3 (one arm) before building a ladder.  The
mechanism of posture C's own 4.3 cyc stays formally OPEN (that arm is a pre-F3 diagonal-carry, pre-wu93
fixture pairing — both differences are confounded with the term).

