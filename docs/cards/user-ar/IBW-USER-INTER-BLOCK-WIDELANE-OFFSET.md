# IBW: a receiver inter-block wide-lane offset for GPS III in the user search

**STATUS**

- **id:** IBW
- **topic:** user-ar
- **status:** parked
- **summary:** Draft for the author's decision (2026-11-13); no code, no runs. Card ARUB section 5 found a receiver-dependent wide-lane offset of the GPS III satellites against the older GPS blocks (STPM00SPM, Trimble Alloy, -0.12 to -0.22 cycle; WTZZ00DEU, Javad TRE_3, +0.04 to +0.15; BAKE00CAN, Septentrio PolaRx5S, none) that sits in the code observations, that the float wide lane and the B4 witness share, and that narrows the rounding margin of GPS III wide lanes. A code-bias state in the filter cannot remove it: its geometry-free part is collinear with the free slant ionosphere of each satellite. The proposed term is a block-common wide-lane fraction, estimated modulo one cycle from a filter-independent wide lane of several GPS III satellites and removed before the admission test and the rounding of the WL-constrained search. Default off, one key; bars proposed in section 7, not frozen.
- **dates:** 2026-11-13 (drafted)
- **knobs / posture:** proposed `estimation.ambiguity_resolution.block_wl_offset` (bool, default false); its ledger row (UNVALIDATED) lands with the code.
- **code:** none yet.
- **run dirs:** none yet.
- **source:** card [ARUB](../../record/cards/user-ar/ARUB-USER-FIXING-CONTROL-CONTINUED.md) sections 5 and 5.1; card [ARU](../../record/cards/user-ar/ARU-USER-FIXING-CONTROL.md) section 8.5 (B4 and its witness)

**Outcome.** Draft for the author. The GPS III satellites carry a receiver-dependent wide-lane offset of up to 0.22 cycle at
two of the three test receivers, a code effect the user model cannot absorb: the satellite code biases come from the OSB
file and the receiver code bias cancels between satellites. A code-bias state is not identifiable for the part that
moves the wide lane. The card proposes a block-common wide-lane fraction, estimated from a filter-independent wide lane
and removed before rounding, default off, with identity, mechanics, effect and no-harm bars to freeze before any graded
run. Open.

> Results are appended TO THIS FILE, below the text.

---

## 1. The problem (card ARUB section 5)

Witness minus fixed wide lane at every clear GPS fix event of the 30 fixed runs of card ARUB section 3, read GPS III
against older (the signals each run fixed: L1C/L2W at STPM00SPM, L1W/L2W at WTZZ00DEU and BAKE00CAN):

| receiver (type) | GPS III against older: mean | older against older: mean | relative | RINEX-only window scan, relative |
| --- | --- | --- | --- | --- |
| STPM00SPM (TRIMBLE ALLOY 6.43) | -0.153 | +0.068 | -0.22 | -0.12 to -0.14 |
| WTZZ00DEU (JAVAD TRE_3 DELTA) | +0.118 | -0.029 | +0.15 | +0.04 to +0.05 |
| BAKE00CAN (SEPT POLARX5S) | -0.001 | +0.010 | -0.01 | -0.01 to -0.03 |

Two GPS III satellites together carry no offset; at STPM00SPM the offset is stable over the five days and spreads by
satellite from -0.03 (G21) to -0.25 cycle (G04). It is not the antenna model (CODE used IGS20_2417, whose GPS entries
equal the local `igs20.atx`), and it is not in the ionosphere-free phase (the held GPS III integers misfit by at most
0.074 narrow-lane cycle at STPM00SPM DOY 144). In the WL-constrained search (`AmbiguityResolver`, `wl_from_float`), a
candidate enters when |frac(w)| + 3 sigma_w <= `wl_max_frac` (default 0.40) and its integer is round(w). An offset of
-0.15 to -0.25 cycle leaves a GPS III candidate 0.15 to 0.25 cycle of room instead of 0.40, and a float wide lane that
noise moves past the half cycle is rounded to the wrong side. All 23 GPS events of B4 in card ARUB section 3.1 are such
pairs.

What the user model has (card ARUB section 5.1): in the single-difference user runs the receiver code bias cancels between
satellites; the satellite code biases are the OSB values (a per-satellite code-bias state is created only for a code
signal the OSB file does not cover, `MeasurementModelStates.cpp`); the slant ionosphere is a free state per satellite
(`filter.process_noise.ionosphere_q` 0.001, `filter.initial_std.ionosphere` 5 m). Nothing in the model can take up a
receiver code bias that differs between GPS III and the older blocks.

## 2. Why a code-bias state does not remove it

Per satellite, with the shared geometry rho: code P_f = rho + mu_f I + b_f, phase L_f = rho - mu_f I + lambda_f N_f,
mu_1 = 1, mu_2 = gamma = (f1/f2)^2. A block code bias (b_1, b_2) splits into

- an ionosphere-free part (gamma b_1 - b_2)/(gamma - 1), estimable from the code against the geometry the other
  satellites share, and
- a geometry-free part, a pair (b_1', b_2' = gamma b_1') that is exactly a slant-ionosphere offset of b_1'. Each
  satellite's free ionosphere state takes it up, and through the phase so do N_1 and N_2: dN_1 = b_1'/lambda_1,
  dN_2 = gamma b_1'/lambda_2, so the wide lane moves by (1/lambda_1 - gamma/lambda_2) b_1' = -1.489 cycles per metre
  (GPS L1/L2).

A state for the ionosphere-free part leaves the wide-lane offset unchanged; a state for the geometry-free part is
collinear with the ionosphere states. The integer wide lane is the information that separates them: with several GPS III
satellites in view, their common wide-lane fraction is estimable modulo one cycle. At STPM00SPM, -0.15 cycle corresponds
to about +0.10 m of geometry-free code bias.

## 3. The proposed term

- **What.** Per receiver and per GPS block group (GPS III, from the ATX type of the satellite, `BLOCK IIIA` or a later
  `BLOCK III`, against the older blocks), a wide-lane fraction b (cycles, modulo one).
- **From where.** A filter-independent wide lane per satellite: the MW combination of the signals the search uses,
  corrected by the OSB and by the satellite and receiver antenna offsets per frequency (the witness of card ARU 8.5),
  satellite-differenced against older-block satellites, as a running circular mean of the fractions of 10-minute window
  means. It is established after at least 30 pair-windows with a resultant length of 0.5 or more (thresholds to freeze).
  The float wide lane is not the source: a hold pulls the float wide lanes of held pairs to their integers and would pull
  the estimate toward zero (standing invariant 7).
- **Where applied.** In the WL-constrained search, before the admission test and the rounding: w' = w - s b for each
  pair of one GPS III and one older satellite (s = +1 when sat_i is GPS III, -1 when the reference is); pairs within one
  group are unchanged. The held integer stays an integer, and the ionosphere-free N_1 row is unchanged (the offset is in
  the code).
- **Events and telemetry.** Established (first use) and re-seeded are events with counters and identities; a re-seed
  follows three consecutive windows more than 0.15 cycle from b (jump test). Run-end line per receiver: the estimate,
  windows used, resultant length, re-seeds, pairs corrected, admissions gained and roundings changed against the
  uncorrected value of the same epoch.
- **Re-coupling edge.** At a re-seed every held GPS III wide lane is re-tested against the new estimate, and one more
  than 0.5 cycle away is released through the existing release path.
- **Off.** Default off and byte-identical when off.

## 4. Alternatives considered

- Code-bias states per block in the filter: not identifiable for the geometry-free part (section 2).
- A joint estimate of the integers and a common fraction from the float wide lanes each epoch: the hold feedback of
  section 3.
- A fraction per satellite: takes up integer errors where a satellite has few pairs; the per-satellite spread at
  STPM00SPM (-0.03 to -0.25 cycle) is left for a later step if the block term leaves residuals that matter.
- A configured calibration per receiver type: no warm-up, but it goes stale with firmware and needs a calibration day;
  kept as the control arm (b fixed at the RINEX-only scan value of the receiver-day).
- No GPS III in the WL-constrained search: drops the 10 GPS III satellites of these days (G01, G04, G11, G13, G14,
  G18, G20, G21, G23, G28).

## 5. Plan, when the author approves

- **Code.** A small value type and free functions for the estimate (proposed `filter/BlockWidelaneOffset.hpp` and `.cpp`,
  owned by the resolver; not a state-vector element); the block lookup shared with the ATX type match of
  `StochasticModelWeight::computeSatBlock` (one owner); the hook in `AmbiguityResolver.cpp` before the `WLFLOAT`
  admission test; full Doxygen; the key in the ambiguity-resolution section of `GnssConfigLoad.cpp` with its ledger row
  (UNVALIDATED) in the same commit; the CMake source list.
- **Tests** (new file, suite `BlockWidelaneOffsetTest`): the circular mean and the modulo-one handling across +-0.5; the
  sign for GPS III as sat_i and as the reference; off reproduces the uncorrected rounding bit for bit; established only
  after the warm-up; the jump test re-seeds after an injected step and re-tests the held pairs.
- **Runs** (Release, renamed executable, card ARU section 5 order rule): the b12 configurations of card ARUB section 3
  with the key on, fixed from 00:00 and from 01:00; the float runs unchanged (the key acts on fixing only); the control
  arm with b fixed at the scan value.

## 6. Pre-flight against the standing invariants

1. A stiff time-linked quantity: b has a jump test and a re-seed with a re-test of the held pairs (section 3), unit-tested
   with an injected step before an ON run.
2. Data runs Release only.
3. Deferrals: Galileo and BeiDou block groups are deferred. The two Galileo B4 events at STPM00SPM (E26-E29, DOY 145)
   are not tied to a block, so field-inert is not shown by data and stays open.
4. Run-end counters as in section 3; "0 corrected" on a receiver with GPS III in view means inert wiring.
5. Grades on single-difference quantities and on positions against the reference; b itself is an inter-block (single
   difference) quantity.
6. Default off and byte-identical when off; user path only (the network producer's own resolver does not call this path,
   to be confirmed at build).
7. The term changes which pairs enter the search: an event at both edges (established, re-seeded), with counters and the
   re-test of the held pairs at a re-seed.

## 7. Bars proposed (the author freezes them before any graded run)

- **I1 identity.** Key off: `state_trace.csv`, `ambiguity_trace.csv` and `measurement_residuals.csv` byte-identical to the
  b12 runs of card ARUB section 3, same build.
- **I2 mechanics.** The run-end estimate within 0.05 cycle of the RINEX-only scan (`tools\gps3_wl_frac.py`, card ARUB
  section 5) at each receiver on each day: STPM00SPM -0.12 to -0.14, WTZZ00DEU +0.04 to +0.05, BAKE00CAN -0.01 to -0.03.
- **I3 effect.** At STPM00SPM, more GPS III candidates admitted to the WL-constrained search than in the b12 runs, and no
  clear fix event on a GPS III pair whose witness, corrected by the scan value of that receiver-day, lies more than 0.5
  cycle from the fixed wide lane.
- **I4 no harm.** B1 and B2 exactly as in card ARU 8.5 on the 15 receiver-days; the largest ionosphere-free misfit of the
  held integers per receiver-day no larger than in b12.
- **Reported.** Time to first fix, fix counts, B4 as frozen.

## 2026-11-20: parked

Parked by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); applied by the docs consolidation on branch `docs/consolidate-2026-11-20`: a draft that never received the author's approval. Nothing was built or run.
