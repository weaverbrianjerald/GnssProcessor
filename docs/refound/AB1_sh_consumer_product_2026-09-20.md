# AB1: the SH VTEC field as a consumer product (add-back 1)

**STATUS**

- **id:** AB1
- **topic:** ionosphere
- **status:** parked
- **summary:** First add-back on the held-wire posture, PARKED 2026-09-20: the frozen rule fails (on the sim, with truth, a causal global SH product beats the dual-frequency consumer's own ionosphere knowledge for 2 epochs only), so the consumer constraint is KEEP_OFF. The field is SPECIFIED as a product in its own right (causal, degree 6, 25 degree mask, per-satellite and per-station bias terms, 0.45 m vertical sigma) and not built. The slant ionosphere of BSHM00ISR is gross and poisons any field fit.
- **source:** [NETWORK_REFOUNDING](NETWORK_REFOUNDING_2026-09-14.md) (the add-back list); [PBH](PBH_phase_bias_hold_2026-09-19.md) section 8 (the baseline pair); code census of 2026-09-20
- **knobs / posture:** none yet; RF-0hv working bases at main `bbb2663`; baseline pair `runs/_rf0hv3_{field,sim}6`
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** `runs/_rf0hv3_{field,sim}6` (wire `bbaa7fd6...` field, `ea3a646f...` sim); `runs/_ab1_proto_20260920/`

## 1. What the census found (2026-09-20)

**The in-solve field cannot return on this posture.** The loader refuses
`network.estimate_iono_sh` together with `w6_iono_constraint: none`
(`GnssConfigLoadNetwork.cpp:3303-3316`) and refuses any live ionosphere constraint
together with `sat_gf_datum` (`:3390-3401`); both refusals are pinned by
`W6IonoConstraintTest`. A post-solve fit is the only route, and it keeps the solve
byte-identical.

**The consumer side exists and is armed but dead.** `closed_loop.consume_network_iono:
true` is in both bases, and the supply is gated at `NetworkEmitStore.cpp:627` on
`sh_model`, which is built only under `estimateNetworkIonoSh()`
(`ApplicationNetworkSetup.cpp:392-393`). Nothing is ever pushed (0 supply lines in
720 epochs, card CST). The consumer applier is a pseudo-observation on its own
per-satellite slant-ionosphere states after the measurement update
(`RealTimeEngineEstimate.cpp:156-225`): R = diag(sigma^2) plus a common-mode term
(`common_mode_sigma_m` 50) over each constellation, same-epoch supply only, fixed
weight with no decay.

**Missing for a post-solve product:** the fit itself (no estimator exists;
`scripts/sh_vtec.py` only synthesizes from coefficients); a supply decoupled from
`sh_model`; a same-epoch site (the absolute slant ionosphere `iono_total_m = x_lin +
x_r` first exists in `networkIonoDiag`, which runs after the emit and consume seam, so
a fit there is one epoch late); counters; and a wire record (no ionosphere is
serialized anywhere on `products.txt`, so the cold-consumer replay cannot grade this
add-back).

**Datum.** The network's absolute slant ionosphere is well defined in the frame
fixed by the satellite GF pin at broadcast T_GD and the receiver-DCB gauge, and the
consumer levels its own slant ionosphere with the same wire code biases and
estimates its own receiver GF bias, so a receiver-common level offset is absorbed.
One term is not representable by an SH field: the estimable slant ionosphere absorbs
each satellite's true DCB deviation from its broadcast T_GD
([RF-0b](RF-0b_dcb_datum_2026-09-15.md)), a per-satellite constant with no pierce-point
dependence. The consumer books the same constant. The product therefore needs a
per-satellite bias term beside the field: slant(s) = mf(e) VTEC(ipp) + b_s.

**Priors that bound the expectation.** A global degree-4 field moved the slant
interaction RMS from 0.706 to 0.669 m against free slant on the older posture
(QPR item 8a: the limit is the representation). Low-elevation code contamination
dominated the old in-solve a00, and a 25 degree mask was the measured remedy.
`sh_health_gate` is KEEP_OFF in the ledger (two realizations of one day differ by
about 25 TECU in a00). `docs/topics/ionosphere.md` and the project instructions are
stale on one point: `network_ionosphere.csv` now carries `iono_total_m`, the absolute
slant ionosphere.

## 2. Reasoning before any number

A constraint of 0.7 m accuracy can help a float consumer only while its own
knowledge of a satellite's slant ionosphere is worse than that: the first epochs of
an arc, when the state is levelled by a few epochs of code. After that the
consumer's own level, averaged from code and carried by phase, is better than any
global field, and a fixed-weight pseudo-observation with time-correlated error adds
only a bias pull. The position itself is ionosphere-free to first order. The honest
expectation is a small gain in the first minutes of hour 1 and a null afterwards,
provided the supplied sigma is honest. The hold-out field consumer sits inside a
dense European cluster (eleven stations, two of them co-located), so a regional
per-satellite interpolation of the network's slant ionosphere may be far more
accurate there than the global field; that variant is measured beside the field so
the design fork is decided on data.

## 3. AB1-0: the pre-registered trace-only prototype

No C++ change and no processor run. Inputs are the baseline pair's traces:
`network_ionosphere.csv` (station, satellite, elevation, azimuth, `iono_total_m`),
the station coordinates of `arm.json`, the consumer's own states in
`closed_loop_sat_trace.csv` (`iono_m`, `iono_var`), and on the sim the truth slant
`I_L1_m` of `SZZ000SIM/truth_atmo.csv`. The field consumer's line-of-sight geometry
is taken from the co-located network station `WTZR00DEU`.

**Model.** iono_total(r, s, t) = mf(e) sum a_nm(t) Y_nm(sun-fixed pierce point, 450 km
shell) + b_s + b_r, with sum b_s = 0, b_s and b_r constant, elevation weighting.
Variants: degree 4, 6, 8; mask 15 and 25 degrees; with and without b_s; a batch fit
(the best case) and a causal filter (what a producer could emit). Variant R: a
regional per-satellite interpolation of (iono_total - b_r) from stations within
1500 km of the consumer.

**Grades.** The product's slant at the consumer's lines of sight, per-epoch median
over satellites removed (the consumer's common mode), as RMS by hour and by
elevation band: against truth on the sim; against the consumer's own converged
ionosphere states of hours 3 to 6 on both beds. The useful window: the product's
error beside the consumer's own ionosphere error and formal sigma as a function of
arc age in epochs. An honest sigma model for the causal variant as a function of
elevation.

**Decision rule, frozen.** The consumer constraint is built (AB1-1: the fit seam
ahead of the consume step, the supply decoupled from `sh_model`, counters, both
edges as events, the supplied sigma taken from this prototype, default OFF, solve
byte-identical) only if the best causal variant's error at the consumer is below the
consumer's own ionosphere error for at least the first 10 epochs of an arc on both
beds. Bars for AB1-1 would then be: hours 2 to 6 of the consumer within 0.02 m of
the baseline pair on both beds, the first 30 minutes no worse, and any gain reported
as measured. If the rule fails, the field is still specified as a product in its own
right from the best variant, the consumer constraint is recorded KEEP_OFF with the
numbers, and the sequence moves to L5.

## 4. 2026-09-20: AB1-0 result: the frozen rule FAILS; the consumer constraint is KEEP_OFF

Trace-only prototype `scripts/ab1_sh_product_proto.py` (deterministic; conventions
ported from `SphericalHarmonicIonosphere.cpp:33-112` through `scripts/sh_vtec.py`,
450 km shell, `1/cos z'` mapping, sun-fixed longitude, 0.162372 m per TECU on L1),
outputs `runs/_ab1_proto_20260920/`. The consumer line-of-sight geometry matches the
traces to 0.005 degree on 5809 of 5809 field rows and 0.0005 degree on 5400 of 5400
sim rows; the network and consumer epoch grids coincide, so the causal product is
read at the same epoch.

**The rule.** Best causal variant against the consumer's own ionosphere error as a
function of arc age k, sim bed, truth: the causal global product (degree 6, mask 25
degrees, with b_s) beats the consumer only through k = 2 on all arcs (1.589 / 1.810 /
1.805 m against 2.661 / 2.119 / 1.855 m, losing at k = 3 with 1.754 against 1.613 m)
and never on arcs born after hour 1 (1.449 against 1.380 m at k = 0). The rule asked
for the first 10 epochs on both beds. It fails on the sim with truth, which settles
it. The field cannot decide the rule at all: it has no truth ionosphere at the
consumer, and the batch-product proxy gives opposite answers with and without one
station (below).

**The regional variant does not rescue it.** On the sim it beats the consumer at
every k at mask 25 and at none at mask 15, and it rests on a single station 579 km
away. On the field its pool holds two stations within 0.1 km of the consumer, so it
is close to a zero-baseline copy, and it still loses against the batch proxy.

**What the product is worth as a product** (hours 2 to 6, RMS of the slant at the
consumer's lines of sight, per-epoch median removed):

| variant | sim against truth | by elevation 15-30 / 30-60 / 60-90 |
| --- | --- | --- |
| batch, degree 6, mask 25, b_s | 0.457 m | 0.705 / 0.295 / 0.104 |
| causal, degree 6, mask 25, b_s | 0.747 m | 1.173 / 0.740 / 0.508 |
| regional, mask 25 | 0.472 m | 0.448 / 0.513 / 0.415 |

Degree 6 wins on both beds (withheld-station RMS: sim 0.92 / 0.63 / 0.80 m for
degrees 4 / 6 / 8, field 0.47 / 0.31 / 0.34 m); the 25 degree mask helps the global
fit (sim 0.94 to 0.63 m, field 0.74 to 0.45 m). The causal product's vertical sigma
is 0.446 m (2.75 TECU) on the sim, and its error is steeper in elevation than the
mapping function. The reference of the second grade is itself limited: on the sim
the consumer's own converged ionosphere state is 0.488 m from truth.

**The per-satellite bias term is real and decisive.** b_s has a standard deviation
of 1.13 m of L1 slant on the sim (6.9 TECU) and 0.94 m on the field, and is stable
between the two halves of the window (correlation 0.994 sim, 0.876 field). With it
the causal product's withheld-station error on the field is 0.296 m, without it
0.968 m. It makes the product WORSE against the sim's truth slant and BETTER against
the consumer's own state, which is the datum argument of section 1 measured: the
network's slant ionosphere and the consumer's both carry each satellite's DCB
deviation from its broadcast T_GD, and the truth slant does not.

**One station poisons the field fit.** The absolute slant ionosphere of BSHM00ISR
has a median of 57.3 m against the network's 7.4 m, 2648 of 2649 rows above 60 m,
and extremes of -672 and +942 m. Excluding it drops the batch in-sample RMS from
4.66 to 1.23 m, the withheld-station RMS from 1.73 to 0.31 m, and flips the field
proxy verdict. It is the station RF-0 found carrying all of the network's gross
code, and it is still in the solve; a diagnostic arm without it is graded in
section 5.

**Verdict.** The consumer ionosphere constraint is KEEP_OFF on this posture: a
global field fitted to a 59-station network is worse than a dual-frequency
consumer's own ionosphere knowledge after one minute, and the armed knob
`closed_loop.consume_network_iono` stays dead by construction. The field as a product
in its own right is SPECIFIED, not built: causal filter, degree 6, 25 degree mask,
per-satellite and per-station bias terms with sum b_s = 0, emitted sigma 0.45 m
vertical mapped steeper than mf, gross stations screened by their slant level. It
earns a build when a consumer for it exists (a single-frequency user, or a regional
densification at PPP-RTK spacing). Instrument notes: run the prototype with
`OMP_NUM_THREADS=1` (multi-threaded BLAS is about 45 times slower on these small
systems); grade b_s against the consumer, never against the truth slant.
