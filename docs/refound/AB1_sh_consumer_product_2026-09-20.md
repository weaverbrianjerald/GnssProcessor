# AB1: the SH VTEC field as a consumer product (add-back 1)

**STATUS**

- **id:** AB1
- **topic:** ionosphere
- **status:** open
- **summary:** First add-back on the held-wire posture. A post-solve SH VTEC fit with per-satellite and per-station bias terms, offered to the consumer as an ionosphere constraint. Step AB1-0, a trace-only prototype that fixes the design parameters and decides whether the consumer constraint is built, is pre-registered here.
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
