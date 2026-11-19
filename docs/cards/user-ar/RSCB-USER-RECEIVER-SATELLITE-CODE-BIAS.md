# RSCB: the receiver's deviation from the published satellite code bias, as a state of the user filter

**STATUS**

- **id:** RSCB
- **topic:** user-ar
- **status:** open
- **summary:** Pre-registered 2026-11-13; built on branch `ar/rx-sat-code-bias` (uncommitted) and checked on data the same day (sections 11 to 14); no bar frozen. The wide-lane offsets of card ARUB section 5 are not a GPS III block effect. In the float runs they are per satellite at the receiver (older satellites included), mostly constant across sky tracks at the Trimble and the Javad, and absent as a satellite constant at the Septentrio and for Galileo. The card adds the receiver's deviation from the published satellite code bias as a state with a measured prior (part 1), lets the ionosphere-free phase decide the wide-lane integer where the code cannot (part 2, revised on data to a candidate test with a persistence time), and keeps the learned values across passes and runs (part 3). All default off; with the keys off the output is byte-identical. Part 2 alone raises the GPS held share by 1 to 7.5 points on 13 of 15 receiver-days with no integer different from the control; stored biases put the float wide lane on integers.
- **dates:** 2026-11-13 (pre-registered, built, first data runs), 2026-11-14 (revised decision rule on data)
- **knobs / posture:** `estimation.estimate_rx_sat_code_bias`, `estimation.rx_sat_code_bias_systems`, `estimation.rx_sat_code_bias_store`, `filter.initial_std.rx_sat_code_bias`, `filter.process_noise.rx_sat_code_bias_q`, `estimation.ambiguity_resolution.wl_branch_check`, `estimation.ambiguity_resolution.wl_branch_persist_s`; ledger rows UNVALIDATED.
- **code:** branch `ar/rx-sat-code-bias` (from `ar/user-control` 165b047), uncommitted; executables `gnss_processor_rscb{1,2,3}.exe` of that tree.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_ibw_flt24_est_{144,145}_{stpm,wtzz,bake}` (full-day float runs of section 1); tools `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\{block_spread,sat_calib_loo,pair_encounter_test,make_run_flt24}.py`.
- **source:** card [ARUB](ARUB-USER-FIXING-CONTROL-CONTINUED.md) sections 5 and 5.1; card [IBW](IBW-USER-INTER-BLOCK-WIDELANE-OFFSET.md) (the block term this card replaces, pending the author's ruling on that card)

**Outcome.** Open; built and checked on data, not graded against a bar. The float wide lane of the user filter carries a
per-satellite offset that belongs to the receiver: at the Trimble and the Javad most of it persists across sky tracks and
days, and a block offset removes almost none of it. The cause in the model is that a published code bias suppresses any
state on that signal. Built, default off: a receiver-satellite code-bias state with a measured prior, a wide-lane decision
by the ionosphere-free phase where the code is not entitled to round, and a store of the learned values. On five days at
three receivers the phase decision alone adds 1 to 7.5 points of GPS held share with no integer different from the control,
and stored values put the float wide lane on integers. The sky-track part is not modelled; the neighbour path and the bias
reset were never exercised by data.

> Results are appended TO THIS FILE, below the text.

---

## 1. Evidence (2026-11-13, diagnostic, not graded)

Source: the 15 float runs `_rap_aru_b12flt_est_<doy>_<site>` of card ARUB section 3 and six full-day float runs
`_rap_ibw_flt24_est_{144,145}_<site>` (copies with only `processing.max_duration_s` changed to 85 500 s, executable
sha256[:16] 4b8aae390a5a9733, the kept `gnss_processor_aru.exe`). Quantity: the filter's own float wide lane
W = a1/lambda1 - a2/lambda2 from `ambiguity_trace.csv`, arcs older than 30 min, epochs after the first hour, every pair
of satellites in view, fraction frac(W_i - W_k). No hold acts in a float run.

Share of pair samples beyond 0.25 cycle, GPS L1/L2 (`block_spread.py`):

| receiver | GPS III with older | older with older | GPS III with GPS III |
| --- | --- | --- | --- |
| STPM00SPM (TRIMBLE ALLOY) | 12.8 % | 4.9 % | 0.3 % |
| WTZZ00DEU (JAVAD TRE_3 DELTA) | 4.9 % | 17.5 % | 0.0 % |
| BAKE00CAN (SEPT POLARX5S) | 0.0 % | 0.0 % | 0.0 % |

Mean |fraction|, with offsets fitted on four days and removed on the fifth (`sat_calib_loo.py`; the five days cover the
same six hours, hence the same GPS sky tracks, so the last column includes the sky-track part):

| receiver | raw | one GPS III offset | one offset per satellite | per-satellite offset RMS |
| --- | --- | --- | --- | --- |
| STPM00SPM | 0.110 | 0.099 | 0.035 | 0.138 |
| WTZZ00DEU | 0.133 | 0.129 | 0.022 | 0.095 |
| BAKE00CAN | 0.052 | 0.050 | 0.022 | 0.054 |

Median |difference| of a pair's mean fraction between two occasions (`pair_encounter_test.py`, no adjustment and no
datum; an occasion is 30 min or more of common view):

| comparison | STPM00SPM | WTZZ00DEU | BAKE00CAN |
| --- | --- | --- | --- |
| A: same pair, two sky tracks of one day | 0.047 (n 13) | 0.033 (n 26) | 0.048 (n 273) |
| B: same pair, same sky track, two days | 0.015 | 0.010 | 0.011 |
| C: two different pairs (null) | 0.109 | 0.110 | 0.056 |

Reading. A constant of the satellite at the receiver predicts A much smaller than C; an effect of the sky track (code
multipath, antenna group delay) predicts A near C. At the Trimble and the Javad most of the offset is a constant of the
satellite at that receiver; a sky-track part of 0.03 to 0.05 cycle (median pair difference) exists at every receiver; at
the Septentrio only the sky-track part is left. Galileo E1/E5a (same tools): A against C is 0.118 against 0.109 at
STPM00SPM, 0.029 against 0.060 at WTZZ00DEU, 0.048 against 0.055 at BAKE00CAN, and a per-satellite offset does not
reduce the raw fraction (0.091 to 0.091, 0.052 to 0.044, 0.046 to 0.045). Galileo shows no satellite constant worth a
state at these receivers.

Trap recorded. A per-pass offset adjusted along the whole day drifts (BAKE00CAN about +0.15 cycle in 12 h, one sign):
grades use pair differences, never chained per-pass offsets.

## 2. The model term

The code observation of satellite s on code signal f at one receiver is, beside the terms the model has,

    published satellite code bias (known term, unchanged)  +  beta(s, f)

beta is the receiver-satellite code bias: what this receiver sees on that satellite's signal beyond the published value.
It is a property of the receiver and satellite pair (signal shape against correlator and front end), constant over days
(section 1, row B) and across sky tracks (row A against C). The published value stays a fixed correction; beta is a
deviation around zero with a prior. The common part of beta over the satellites of a constellation is the receiver code
bias of that signal, which cancels in the between-satellite difference.

The gap in the code: `MeasurementModel::ensureSatCodeBiasStates` creates a satellite code-bias state only where neither
the bias file nor the SSR stream covers the signal, so a published value is exact for every receiver. The same file
already declines that for GLONASS (`GnssCorrections.cpp`, the bias index: a published GLONASS bias "is a network
aggregate that does not reflect this receiver's per-channel delays").

## 3. Estimability (toy design of 2026-11-13, geometry known, equal code weights)

Per satellite with a free slant ionosphere, the pair (beta_1, beta_2) has one estimable direction and one null direction,
beta_2 = gamma beta_1 (gamma the squared frequency ratio): a shift c along it is a shift of the slant ionosphere by -c and
of the wide lane by (1/lambda_1 - gamma/lambda_2) c = -1.489 cycles per metre (GPS L1/L2). Consequences:

- Free of any prior, beta leaves no wide lane at all. It needs its prior.
- An ionosphere constraint does not close the null direction: 0.05 cycle needs the between-satellite slant delay to
  34 mm (0.21 TECU).
- A held pair (both integers) closes it: the geometry-free phase then fixes the ionosphere difference and the code rows
  observe both components of beta_i - beta_k.
- The float wide lane takes -0.401 beta_1 - 0.661 beta_2 cycles per metre; the Melbourne-Wubbena combination takes
  -0.652 beta_1 - 0.508 beta_2. They agree only along the null direction, so an offset measured with one must not
  correct the other.

## 4. Part 1: the state

- **Family.** A new state family "receiver-satellite code bias" beside "satellite code bias" in `kStateFamilies`, with
  its own index map in `StateManager`, label prefix `RSCB:`, key "<sat>|<code signal>" built and parsed only in
  `StateKeySchema.hpp`. It is not the satellite code-bias family: that one stands in for a missing published value
  (10 m prior, seeded from the innovation, frozen on the pivot, pruned strictly). The two never coexist on one
  (satellite, signal).
- **Birth.** Uncombined raw code rows of the user filter (never the network producer, never a combined pair), for a
  satellite of a constellation in `estimation.rx_sat_code_bias_systems`, on a code signal whose published bias is applied
  (bias file or SSR), at its first processed epoch. Value 0, or the stored value (part 3).
- **Equation.** One term in `measurement/ObservationEquation.cpp`, +1 on the code row, with its own typed identity in
  `common/ParamRef.hpp`; no term on phase rows. The network views carry no such column and are unchanged.
- **Prior.** `filter.initial_std.rx_sat_code_bias`, default 0.15 m per signal: the measured per-satellite spread of the
  float wide lane (0.095 cycle at the Javad, 0.138 at the Trimble) divided by 0.773 cycle per metre, the sensitivity of
  the float wide lane to two independent signals of equal sigma.
- **Process noise.** Random walk, `filter.process_noise.rx_sat_code_bias_q`, default 1e-10 m2/s (3 mm in a day); the
  measured day-to-day difference is 0.010 to 0.015 cycle on a pair.
- **Between-satellite difference.** Every column stays in the differenced row, +1 on the satellite and -1 on the pivot.
  No column is zeroed and no state is frozen: with an independent prior per satellite the difference of two satellites
  then has the honest variance for every pair, the pivot's included.
- **Consequence for fixing, with no further code.** The float wide-lane sigma of a pair (`floatWidelane_`, from the
  filter covariance) includes the open null direction of both satellites, about 0.16 cycle for two unknown satellites
  at the default prior, so the existing test |frac| + 3 sigma <= `wl_max_frac` stops rounding a wide lane whose bias is
  not known, and rounds it again once the bias is.
- **Reset.** `StateManager` gives one call that returns the states of a satellite to the prior (value 0, variance the
  prior, cross-covariances zero). Part 2 calls it; nothing else does.

## 5. Part 2: the wide-lane decision by the ionosphere-free phase

In `AmbiguityResolver::resolveWlConstrained`, on the `wl_from_float` path, with
`estimation.ambiguity_resolution.wl_branch_check` (requires `nl_iono_free_row`).

For a pair with float wide lane w and sigma sigma_w: W0 = round(w), d = w - W0, W1 = W0 + sign(d) (sign(0) = +1). For a
wide-lane integer W the ionosphere-free row gives the N_1 float n(W) = (f1 n1 - f2 n2 - f2 W) / (f1 - f2) (the row of
`nlRowCoefficients` with `iono_free` true), with fraction r(W) = n(W) - round(n(W)) and sigma sigma_n from the same row
and the filter covariance, in quadrature with `nl_sigma_floor_cyc`. One wide-lane cycle moves n by s = f2 / (f1 - f2);
the branch gap is g = |s - round(s)|: 0.471 for GPS L1/L2, 0.051 for Galileo E1/E5a and GPS L1/L5, 0.336 for BeiDou
B1I/B3I. The phase is decisive for W when |r(W)| + 3 sigma_n <= g / 2. It cannot be decisive for both W0 and W1.

| the code (existing test) | the phase | decision |
| --- | --- | --- |
| admits W0 | decisive for W0, or not decisive | W0, as today |
| admits W0 | decisive for W1 | W1, and a contradiction event |
| does not admit | decisive for W0 or for W1 | that integer ("by the phase") |
| does not admit | not decisive | not admitted, as today |
| any, with 1 + |d| <= 3 sigma_w | any | not admitted (three integers within reach; W1 and W0 - sign(d) differ by 0.058 in r) |

A pair admitted here enters the N_1 search as any other; the ratio test, the per-pair cap and the hold are unchanged.
For a frequency pair with a small gap the phase is never decisive and the behaviour is today's. A contradiction event
names the pair; the satellites at fault are chosen as `chooseReleaseSatellites_` does (the reference when it is the
majority of its pairs, else the other satellite) and returned in the resolution, and the caller resets their
receiver-satellite code-bias states to the prior (part 1). Log line `WLBRANCH` per decided pair; counters per run:
decided by the phase, contradictions, refused for reach, satellites reset.

## 6. Part 3: persistence

- **Within a run.** A state of the family is never pruned. A satellite that sets keeps its value and its covariance with
  the others, and meets them again at its next pass. The state count grows to at most two per satellite of the listed
  constellations.
- **Across runs.** `estimation.rx_sat_code_bias_store`, a JSON file read at the start when it exists and written at the
  end of a post-processing run. Entries: satellite, code signal, value, sigma, epoch of the last update. Header: schema,
  marker, receiver type and version, the source of the published biases. Only learned states are written (sigma of the
  deviation from the mean of the learned set below half the prior); values are written with zero mean per constellation
  and signal (the common part is the receiver code bias, a datum, not information), sigma is that of the deviation from
  the set mean. A stored entry not seen in the run is carried over unchanged. On reading, the variance grows by the
  process noise over the age of the entry. A store whose receiver type or version, or whose bias source, differs from the
  run's is not used, and that is an event with a counter.
- **Jump response.** The contradiction event of part 2 (the code, with its stored bias, admits one integer and the phase
  is decisive for the other) resets the states of the satellites at fault to the prior. The value is not re-seeded from
  data: the phase path of part 2 learns it again through a hold.
- **Events and telemetry.** Born, read from the store, learned (sigma first below half the prior), reset, written: each
  with a counter and a log line that names the satellite and the signal. One run-end line for the family.

## 7. Keys (all default off or inert)

| key | default | meaning |
| --- | --- | --- |
| `estimation.estimate_rx_sat_code_bias` | false | the state family of part 1 |
| `estimation.rx_sat_code_bias_systems` | ["GPS"] | constellations that get the state (Galileo shows no satellite constant, section 1) |
| `filter.initial_std.rx_sat_code_bias` | 0.15 | prior sigma per signal, m |
| `filter.process_noise.rx_sat_code_bias_q` | 1e-10 | random-walk spectral density, m2/s |
| `estimation.rx_sat_code_bias_store` | "" | store file of part 3; empty: none |
| `estimation.ambiguity_resolution.wl_branch_check` | false | the decision table of part 2 |

## 8. Pre-flight against the standing invariants

1. The stiff time-linked quantity is beta. Its jump response is the contradiction event and the reset to the prior,
   unit-tested with an injected stale value before an ON run; the reset does not re-seed from one epoch of code.
2. Data runs Release only.
3. Deferrals. The real-time engine carries the states but neither reads nor writes the store: field-inert for the
   post-processed control of cards ARU and ARUB, field-blocking for a real-time arm. The sky-track part (0.03 to
   0.05 cycle) is not modelled; the float wide-lane sigma does not include it: field-blocking for a claim about the
   admission margin, stated with every result. Galileo and BeiDou are outside the default list.
4. Run-end counters of sections 5 and 6; "0 born" or "0 decided by the phase" on a run with the key on means inert
   wiring.
5. Grades on between-satellite quantities (pair fractions, pair integers) and positions against the reference.
6. Default off and byte-identical when off; the shared observation equation gains a term that needs a column no network
   view has.
7. The state changes which information the wide lane rests on. Edges with counters and identities: born, read from the
   store (a re-coupling of stored information, never a first sighting), learned, reset, written.

## 9. Bars proposed (the author freezes them before any graded run)

- **I1 identity.** Every key off: `state_trace.csv`, `ambiguity_trace.csv` and `measurement_residuals.csv` of one b12
  fixed run byte-identical to the same run with the executable built from the branch point, same build.
- **I2 mechanics, part 2 alone.** `wl_branch_check` on, the b12 fixed runs of STPM00SPM and WTZZ00DEU: every `WLBRANCH`
  decision by the phase is followed by an accepted or a rejected search, none silent; the run-end counters are non-zero
  where the float runs show pairs beyond 0.25 cycle.
- **I3 mechanics, parts 1 and 3.** State on: the learned values, mapped to the wide lane, agree with the per-satellite
  offsets of the float runs on pair differences (median |difference| at or below the row A level of section 1); a second
  day started from the first day's store admits wide lanes by the code in its first hour that the first day did not.
- **I4 effect.** B4 of card ARU 8.5 on the 15 receiver-days: no event on a pair the phase decided; fix counts and time to
  first fix reported against b12.
- **I5 no harm.** B1 and B2 exactly as in card ARU 8.5; the float position of the state-on runs within the b12 float
  spread.

## 10. Not in this card

The sky-track part in the stochastic model. A store for the real-time engine. Any change to card IBW's status (the
author's ruling). The merge of `main` into `ar/user-control` (34 commits behind, one conflict in
`include/filter/AmbiguityResolver.hpp`).

---

## 11. Implementation of parts 1 and 3 (2026-11-13, branch `ar/rx-sat-code-bias`; unit-tested, no data run)

Part 1 (the state) and part 3 (persistence) are built, default off. Part 2 (the wide-lane decision) is not built. All
names below are the ones the code uses.

- **Family and state.** `StateFamily::RxSatCodeBias` (label `RSCB:`, no snapshot tag, no re-inflation, no gap decouple, no
  frozen-pivot Q skip, `never_pruned` in the family table). `StateManager::addRxSatCodeBiasState`,
  `addRxSatCodeBiasStateFromStore`, `getRxSatCodeBiasIndex`, `getRxSatCodeBiasStates`, and the reset of section 4:
  `std::size_t resetRxSatCodeBiasStatesToPrior(const std::string& satellite)`. Key maker and parser:
  `StateKeys::makeRxSatCodeBiasKey` and `parseRxSatCodeBiasKey` (the signal is exactly one raw code of three characters).
- **Identity and term.** `rxSatCodeBiasRef(sat, code)`: kind `SatCodeBias`, form `ParamForm::RxSat`. A new network kind
  was not added, so the network registry and its views are unchanged; the network's station set refuses the form as an
  untyped column. Effect `ObsEffect::ReceiverSatelliteCodeBias`, summed after the satellite code bias; it is in the
  seed order, so the full-equation seed masks subtract it and the legacy masks do not.
- **Birth.** `MeasurementModel::ensureRxSatCodeBiasStates`, after the satellite code-bias births: uncombined, not the
  network, a constellation of the list, the published bias applied (`biaCoversSignal` or `ssrCoversCodeSignal`), the signal
  tracked by the BSSD pivot. Never pruned. A pair that gains a published bias mid-run loses its satellite code-bias state to
  the strict prune of that pre-pass and gets this state in the same epoch; a pair that loses one keeps its deviation
  state and gets a satellite code-bias state, so for that pair both exist.
- **Store.** `filter/RxSatCodeBiasStore.hpp` (file, identity, age rule) and `filter/RxSatCodeBiasLearning.hpp` (the
  quadratic form `deviationVariance`, the learned test, the per-epoch event `noteLearnedRxSatCodeBiases`, the builder
  `buildRxSatCodeBiasStore`). Identity: receiver type and version from the RINEX `REC # / TYPE / VERS` record (the version
  field was not parsed before), bias source as the product series of the `.BIA` file name (the text before the first
  underscore of a long IGS name), so the daily files of one product share a store. A store of another receiver type,
  version or series is ignored, logged and counted; an ignored file is replaced by the run's store at the end.
- **Learned and written.** The learned test is evaluated at the end with the final covariance, as at each epoch for the
  event. A group needs two learned states to write: against the mean of one state the deviation is zero. Values are
  re-centred over the entries the run writes; carried-over entries are copied unchanged and not re-centred. A seen state
  that is not learned is not written, and its old entry is dropped. Sigma has a floor of 1 mm. The epoch of an entry is
  the end of the run that wrote it.
- **Not covered.** The decomposition trace has no column for the term. The RTS smoother does not smooth the family. The
  real-time engine carries the states when the key is on; its state snapshot refuses a layout that holds them.

---

## 12. Implementation of part 2 (2026-11-13, branch `ar/rx-sat-code-bias`; unit-tested, no data run)

Part 2 (the wide-lane decision by the ionosphere-free phase) is built, default off. With it, all three parts of the
card are built and none is graded. All names below are the ones the code uses.

- **Key.** `estimation.ambiguity_resolution.wl_branch_check` (default false): `AmbiguityResolverConfig::wl_branch_check`,
  `GnssConfig::getArWlBranchCheck()`, parsed beside `wl_from_float` in `GnssConfigLoad.cpp`. The loader turns it off with
  a warning when `wl_from_float` or `nl_iono_free_row` is off (the getter then returns false), and the resolver tests
  all three through `AmbiguityResolver::wlBranchActive()`, so a configuration built in code is held to the same rule.
  With the key effective the loader logs one line (`AR: the wide-lane integer is decided by the code and the
  ionosphere-free phase together (wl_branch_check)`); with it off no line is added and the `Ambiguity resolution
  enabled` line is unchanged.
- **Decision.** The pure function `decideWlBranch(w, sigma_w, r0, r1, sigma_n, gap, wl_max_frac)` returns a
  `WlBranchDecision` (`integer`: `WlBranchChoice` None, W0 or W1; `reason`: `WlBranchReason` NotFinite, Reach, Code,
  Contradiction, PhaseW0, PhaseW1, Undecided; `code_admits`; `phase`). It implements the table of section 5 in the
  order of the card. Where the inputs claim that the phase is decisive for both integers, which consistent fractions
  cannot do, the phase is read as decisive for neither. `wlBranchReasonWord` gives the word of the log line.
- **Glue in the loop.** In `resolveWlConstrained` the `wl_from_float` branch has one new arm, taken only with the key
  effective: one call to `AmbiguityResolver::decideWlBranchOfPair_`, which forms the iono-free row with
  `nlRowCoefficients(..., true)` whatever row the search uses, builds `r0`, `r1` and `sigma_n` (the variance of
  `t_1 dd1 + t_2 dd2` over the four ambiguity states plus the square of `nl_sigma_floor_cyc`), the gap
  `|shift - round(shift)|`, calls `decideWlBranch`, counts, logs and records the pair. The arm after it (the code alone)
  is the earlier code, untouched. The integers are `W0 = round(w)` and `W1 = W0 + 1` for `d >= 0`, else `W0 - 1`. The
  quantity tested is the N_1 float that the search targets, `n(W) = t_1 dd1 + t_2 dd2 + shift W`, the same as the
  search's `y_for_mlambda` for that wide-lane integer.
- **Log.** `WLBRANCH tow sat_i sat_ref sig_hi sig_lo w sigma_w w0 w1 r0 r1 sigma_n gap code=<admit|refuse>
  phase=<w0|w1|none> decision=<w0|w1|none> reason=<word>` for every pair evaluated, and one
  `WLBRANCH tow=... contradiction: code-bias reset requested for sat=...` line per satellite returned. The `WLFLOAT` line
  is kept; with the key on its `dd_wl` and `admitted` report the final decision.
- **Counters and run-end line.** `WlBranchCounters` (`evaluated`, `by_phase`, `contradictions`, `refused_reach`,
  `satellites_returned`) on the resolver, read with `wlBranchCounters()`; `wlBranchRunEndLine()` formats
  `Wide-lane branch check: N pairs evaluated, ... satellites returned for a code-bias reset`. The resolver had no run-end
  report (the hold-release counts are printed per release), so the line is a row of the post-processing run-end table
  (`reportWlBranchRunEnd_` in `PppRunEnd.cpp`, armed by the key). The real-time engine prints no run-end line for it.
- **Contradiction and reset.** The pairs the code admitted this epoch and the contradicted ones are collected in
  `WlBranchEpoch`; after the pair loop `chooseReleaseSatellites_` (reused, not copied) names the satellites at fault and
  they are returned in the new field `AmbiguityResolution::bias_reset_satellites`, filled whether or not a fix was
  accepted and with the family off. A pair that the code refused and the phase decided is not a contradiction.
- **Callers.** `Application::resolveEpochAmbiguities_` (`src/core/app/PppEpochUpdate.cpp`) and `RealTimeEngine::resolveAmbiguities`
  (`src/core/realtime/RealTimeEngineEstimate.cpp`), directly after the held-pair
  and partner refloat that consume `release_satellites`, so after the fix is installed and `fixed_covariance` cannot
  overwrite the reset. With `estimateRxSatCodeBias()` true each returned satellite goes to
  `StateManager::resetRxSatCodeBiasStatesToPrior` and one line `AR code-bias reset: satellite S, N receiver-satellite
  code-bias state(s) returned to the prior` is logged; with the family off nothing is reset.
- **Reach rule and the held pair.** A pair refused for reach, or by the code and the phase together, never enters the
  maturity gate. A pair admitted by any row takes the rest of the function as before (maturity gate, N_1 search,
  ratio test, per-pair cap, hold). A contradicted pair that the later gates then drop still returns its satellites:
  the event is the decision.
- **Correction of one sentence of section 11.** Section 11 says that carried-over entries are not re-centred. That
  sentence is replaced by this rule, implemented in `buildRxSatCodeBiasStore`: for each (constellation, signal) group
  that has an entry written by the run, ONE shift, the mean of the values over all entries of the group in the file
  being written (the states' values and the carried-over values), is subtracted from all of them. A written entry
  therefore keeps its distance to a carried-over one, which the earlier per-set re-centring changed by the mean of the
  written set. A group with no written entry keeps its carried-over values as they are. The sigma of a written entry
  is still that of the deviation from the mean of the learned set, and the written value is the state's value (before
  the shift), not the deviation from the learned-set mean.
- **Tests.** `tests/test_ar_wl_branch.cpp` (suites `ArWlBranchDecisionTest`, `ArWlBranchResolverTest`,
  `ArWlBranchConfigTest`), the corrected carried-over case in `tests/test_rx_sat_code_bias_learning.cpp`, and the key in
  the copy test of `tests/test_ambiguity_resolver_setup.cpp`. Ledger row UNVALIDATED for the key.

## 13. First data runs (2026-11-13, Release, development checks, not graded; bars not frozen)

Executables from the worktree, Release: `gnss_processor_rscb1.exe` (parts 1 and 3, sha256[:16] 95a2e211cd28d2a7) and
`gnss_processor_rscb2.exe` (parts 1, 2 and 3 as built in section 12, f8b252d706dd0e39). Runs
`D:\GnssProcessorSupport\runs_nom\_rap_aru_<tag>_est_<doy>_<site>`, copies of the b12 runs of card ARUB section 3 with
only this card's keys changed (generator `rscb_arms.py`); stores in `D:\GnssProcessorSupport\runs_nom\_rscb_store\`.
Graders `grade_arm.py`, `wl_hourly.py`, `fs_check.py`, `run_look.py`, `b4_batch.py`, beside the tools of section 1.

| tag | source | keys changed |
| --- | --- | --- |
| rsoff | b12fix | none (identity control), DOY 144 STPM00SPM |
| rsp1 | b12fix | state on; DOY 144 STPM00SPM |
| rsfl | b12flt | state on (float run) |
| rsbr, rsbrl | b12fix, b12late | `wl_branch_check` |
| rsst, rsstl | b12fix, b12late | state, `wl_branch_check` and the store, the five days chained per receiver through one store file; the late run of a day starts from a copy of the store as it stood before that day |
| rsfs | b12flt | state on, store read from a copy of the store as it stood before that day (float run) |

Held share, used below: the share of wide-lane pair-epochs (`WLFLOAT` lines: pairs above the 20 degree fixing mask on
two signals) whose float wide lane lies within 0.01 cycle of an integer with a sigma below 0.01 cycle, after the first
30 min of resolver activity. A hold pins both integers of a pair, and no wide-lane-only constraint runs in these
configurations, so such a pair is a held pair. A count of fix events is not a measure of what is held: every accepted
search logs every pair again.

### 13.1 Identity and the unit suite

- Every key off: `state_trace.csv`, `ambiguity_trace.csv` and `measurement_residuals.csv` of `rsoff` are byte-identical
  to `_rap_aru_b12fix_est_144_stpm` (the run of the kept executable 4b8aae390a5a9733), with both executables.
- Debug `gnss_tests.exe` on the branch with parts 1, 2 and 3 (before the revision of 13.4): 4 355 tests in 575 suites,
  4 318 passed, 37 skipped (fixture-gated in a worktree), 0 failed.

### 13.2 Part 1 alone

- `rsp1` (STPM00SPM DOY 144): 40 states born, none learned. GPS pairs admitted by the code: 0 of 3 478 (control 2 994 of
  3 473); median float wide-lane sigma of the GPS pairs 0.170 cycle (section 4 predicted about 0.16). In the control the
  sigma before a hold is 0.08 to 0.13 cycle at minute 16. Galileo held share 93.1 %, as in the control.
- `rsfl`, 15 receiver-days: the median 3D error of minutes 60 to 360 is within 0.3 mm of `b12flt` at every
  receiver-day, the time to 10 cm within 0.5 min, the error at minute 60 within 1.3 mm.

### 13.3 Part 2 alone, and parts 1 to 3 chained

GPS, per receiver-day. Held share (%), minute of the first held pair, integers against the control on the pair-arcs both
runs fix (`grade_b.b1`: compared / different), the run from 00:00 against the run from 01:00 (B1 of card ARU 8.5:
compared / different), median 3D error of minutes 60 to 360 (mm).

| receiver, DOY | held: control | held: part 2 | held: parts 1 to 3 | first hold: control, part 2, parts 1 to 3 | against control: part 2 | against control: parts 1 to 3 | B1: control, part 2, parts 1 to 3 | 3D: control, part 2, parts 1 to 3 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| STPM00SPM 143 | 84.7 | 95.0 | 92.5 | 29, 29, 46 | 67 / 0 | 66 / 0 | 65/0, 75/0, 74/0 | 6.0, 6.0, 5.9 |
| STPM00SPM 144 | 86.0 | 94.2 | 94.0 | 18, 18, 19 | 85 / 0 | 85 / 0 | 79/0, 86/0, 85/0 | 14.7, 14.6, 14.6 |
| STPM00SPM 145 | 89.6 | 97.1 | 97.3 | 22, 22, 22 | 81 / 0 | 81 / 0 | 71/0, 72/0, 73/0 | 9.2, 9.3, 9.1 |
| STPM00SPM 146 | 87.5 | 94.6 | 93.7 | 30, 30, 20 | 69 / 0 | 70 / 0 | 64/0, 69/0, 73/0 | 6.1, 6.0, 6.0 |
| STPM00SPM 147 | 57.7 | 59.6 | 59.5 | 16, 16, 16 | 58 / 0 | 58 / 0 | 48/0, 48/0, 48/0 | 7.4, 7.4, 7.4 |
| WTZZ00DEU 143 | 92.1 | 97.4 | 95.2 | 17, 17, 18 | 87 / 0 | 85 / 0 | 70/0, 66/0, 62/0 | 8.5, 8.1, 8.1 |
| WTZZ00DEU 144 | 90.3 | 93.8 | 94.8 | 30, 30, 22 | 70 / 0 | 67 / 0 | 54/0, 55/0, 60/0 | 8.3, 8.4, 8.1 |
| WTZZ00DEU 145 | 86.1 | 92.5 | 93.5 | 18, 18, 18 | 68 / 0 | 68 / 0 | 58/0, 62/0, 64/0 | 7.0, 7.0, 7.0 |
| WTZZ00DEU 146 | 90.2 | 98.1 | 98.1 | 38, 38, 22 | 72 / 0 | 71 / 0 | 30/0, 30/0, 29/0 | 10.3, 10.0, 9.5 |
| WTZZ00DEU 147 | 50.9 | 51.0 | 51.1 | 16, 16, 17 | 53 / 0 | 53 / 0 | 21/0, 21/0, 21/0 | 13.4, 13.4, 13.4 |
| BAKE00CAN 143 | 97.5 | 99.6 | 99.6 | 19, 19, 24 | 76 / 0 | 76 / 0 | 66/0, 68/0, 68/0 | 7.9, 7.9, 7.8 |
| BAKE00CAN 144 | 97.4 | 99.2 | 99.4 | 17, 17, 17 | 81 / 0 | 81 / 0 | 61/0, 64/0, 64/0 | 4.6, 4.6, 4.5 |
| BAKE00CAN 145 | 91.0 | 95.0 | 95.0 | 18, 18, 18 | 98 / 0 | 98 / 0 | 82/0, 95/0, 95/0 | 4.0, 3.9, 3.8 |
| BAKE00CAN 146 | 96.0 | 98.2 | 98.3 | 20, 20, 24 | 80 / 0 | 78 / 0 | 68/0, 71/0, 71/0 | 4.6, 4.7, 4.3 |
| BAKE00CAN 147 | 92.0 | 95.7 | 95.7 | 18, 18, 18 | 77 / 0 | 77 / 0 | 60/0, 66/0, 66/0 | 2.9, 2.8, 2.7 |

- Galileo held shares equal the control's to 0.2 point in every run (the phase is never decisive at its gap).
- Decisions by the phase per run (part 2): 7 to 22 pair-epochs (296 and 138 on DOY 147 at STPM00SPM and WTZZ00DEU), every
  one of them the integer nearest to the float wide lane where the code test refused to round. No decision for the
  other integer where the code refused. One contradiction (13.4).
- The chain (DOY 143 cold, the later days from the store): states born 40 (STPM00SPM), 46 (WTZZ00DEU), 48 to 50
  (BAKE00CAN); learned 28 to 30, 36 to 38 and 36; read from the store on the later days 28 to 30, 36 to 38 and 36; no
  reset, no contradiction, no store ignored.
- DOY 147 at STPM00SPM and WTZZ00DEU is weak in every run and for both constellations: the ratio test of the search fails
  on 338 epochs of the STPM00SPM control (18 on DOY 146) and the partial search finds no subset. Not a wide-lane effect;
  not examined here.

### 13.4 A false contradiction, and the revision of the decision

`rsbr`, WTZZ00DEU DOY 144, second of week 2730, pair G17-G12 (L1W/L2W): w = -0.0508, sigma_w = 0.0591, r(W0 = 0) =
0.4448, r(W1 = -1) = -0.0257, sigma_n = 0.0616. The code admitted 0, the phase was decisive for -1, and the rule of
section 5 took -1. Both the control and this run later held the pair at wide lane 0, N_1 = 5. At that epoch the
reference satellite had just changed and a fix of other pairs was applied: the float ambiguities of the pair moved from
4.6241 and 4.6749 cycles to 4.9838 and 5.0738 at the next epoch (0.36 and 0.40 cycle in 30 s). The ionosphere-free N_1
float was 0.555 cycle (5.9 cm) from its integer while its formal sigma was 0.047 cycle, and 0.555 is within 0.03 of one
branch gap. The pair was not held at that epoch (its float wide lane at the next epoch was -0.0900, sigma 0.0483), so
no wrong integer followed, but nothing in the rule prevented one.

The rule of section 5 is therefore revised (the table of section 5 stays as written; this governs):

- The neighbour W1 is a candidate only when the code cannot exclude it: 1 - |d| <= 3 sigma_w. Where the code admits W0,
  W1 is never a candidate.
- Code admits W0 and the phase is decisive for W1: the pair is not admitted and W1 is not taken. This is a contradiction
  in waiting; it becomes the contradiction event (satellites returned for the reset of part 1) only after it has
  persisted.
- Code does not admit: the phase decisive for W0 gives W0; the phase decisive for W1 gives W1 only when W1 is a
  candidate.
- Persistence: a decision by the phase, and the contradiction event, take effect only when the phase has been decisive
  for the same integer of the same pair continuously for `estimation.ambiguity_resolution.wl_branch_persist_s` (default
  120 s). A change of reference satellite is a new pair.

### 13.5 Do the stored values put the float wide lane on integers

`rsfs` against `b12flt`, DOY 144 to 147, GPS pairs whose two satellites both have stored values, arcs older than 30 min,
epochs after the first hour (`fs_check.py`):

| receiver | control float: mean, beyond 0.25, beyond 0.15 | store read: mean, beyond 0.25, beyond 0.15 | section 1 forecast (one offset per satellite) |
| --- | --- | --- | --- |
| STPM00SPM | 0.108, 7.1 %, 25.1 % | 0.037, 0.0 %, 1.2 % | 0.035 |
| WTZZ00DEU | 0.135, 11.0 %, 37.5 % | 0.029, 0.0 %, 0.1 % | 0.022 |
| BAKE00CAN | 0.051, 0.0 %, 1.2 % | 0.024, 0.0 %, 0.9 % | 0.022 |

The values learned in the filter through held pairs and the offsets fitted to the float fractions of section 1 are two
routes to the same quantity, and they agree. The days share their GPS sky tracks, so both columns include the
sky-track part.

### 13.6 B4 on these runs, and the witness with the stored values removed

B4 of card ARU 8.5 as frozen (a clear fix event whose wide-lane witness lies more than 0.5 cycle from the fixed wide
lane; `b4_batch.py`, the runs from 00:00): control 9 events in 3 runs, part 2 alone 24 events in 5 runs, parts 1 to 3
27 events in 5 runs, every one at STPM00SPM, on the GPS pairs G12-G28, G25-G28, G26-G28, G04-G26 and G24-G23 (the
control also counts E26-E29 on DOY 145). The phase fixes the pairs the code test refused, which are the pairs with the
large code offsets, so B4 as frozen counts more of them.

The same witness with the receiver's stored values removed (`b4_corrected.py`: a code bias on signal f raises the
narrow-lane code of the witness by (f1 beta_1 + f2 beta_2) / (f1 + f2); GPS fix events of the chain runs whose two
satellites both have stored values):

| receiver | events | witness as frozen: beyond 0.5, beyond 0.35, mean | stored values removed: beyond 0.5, beyond 0.35, mean |
| --- | --- | --- | --- |
| STPM00SPM | 1 270 | 27, 98, 0.156 | 0, 0, 0.084 |
| WTZZ00DEU | 1 708 | 0, 18, 0.134 | 0, 0, 0.070 |
| BAKE00CAN | 1 106 | 0, 0, 0.067 | 0, 0, 0.051 |

Every event beyond 0.5 cycle comes within 0.35 once the stored values are removed (for example G24-G23 on DOY 144,
+0.528 to +0.239; G04-G26 on DOY 143, -0.554 to -0.165). A residual of +0.24 to +0.35 cycle stays on the STPM00SPM
pairs with G28 or G23 as the reference; it is not explained here (the witness omits the antenna variations with angle
and the satellite x/y offsets, and it takes a different functional of the code bias than the filter, section 3).

### 13.7 Reading

- Part 2 alone raises the GPS held share by 2 to 10 points on 13 of 15 receiver-days with no integer different from the
  control and none different between the two start times; positions move by 0.4 mm or less.
- Parts 1 and 3 change the basis of the wide lane, not the held share: with the state the code rounds only what it is
  entitled to, and the stored values put the float wide lane of the Trimble and the Javad where the Septentrio's is.
  The first hold of a cold receiver is later at STPM00SPM (minute 46 against 29) and unchanged at the other two; on the
  later days it is earlier on three receiver-days (STPM00SPM 146, WTZZ00DEU 144 and 146) and equal on the rest but two
  (BAKE00CAN 143 cold and 146, later by 4 to 5 min).
- B4 as frozen is not a test of these integers at the Trimble: its events are the receiver's code bias, and they vanish
  when the stored values are removed from the witness.
- Not shown: any graded bar; the revised rule of 13.4 on data; the reset of part 1 on data (no persisted contradiction
  occurred).

## 14. The revised decision on data (2026-11-13, Release, development checks, not graded)

The rule of 13.4 is built (`decideWlBranch` with the candidate test for the neighbour, `stepWlBranchPersistence`, key
`estimation.ambiguity_resolution.wl_branch_persist_s`, default 120 s, ledger row UNVALIDATED; `WLBRANCH` lines gain
`cand1=` and `persist=`, reason words `waiting` and `contradiction` for a contradiction in waiting; the contradiction
event and its reset fire only when persisted). Unit suites of the build: `ArWlBranch*` 46 tests, the filtered batch 719
tests (718 passed, 1 skipped, 0 failed). Executable `gnss_processor_rscb3.exe` (5ecf8e1c765fafe3). Runs as in section
13 with tags `rsoff2`, `rsbr2`, `rsbrl2`, `rsst2`, `rsstl2`, `rsfs2` and stores in `_rscb_store2` (`rscb_arms.py --ver 2`).

- **Identity.** Every key off: the three traces of `rsoff2` are byte-identical to `_rap_aru_b12fix_est_144_stpm`.
- **The WTZZ00DEU case of 13.4.** The pair is not admitted at second 2730, its line reads `contradiction` (in waiting),
  nothing persists and no satellite is returned.

GPS, per receiver-day, as in 13.3 (the first hold of part 2 alone is at the control's minute on every day):

| receiver, DOY | held: control | held: part 2 | held: parts 1 to 3 | first hold: control, parts 1 to 3 | against control: part 2, parts 1 to 3 | B1: part 2, parts 1 to 3 | 3D: control, parts 1 to 3 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| STPM00SPM 143 | 84.7 | 92.2 | 88.1 | 29, 48 | 67/0, 66/0 | 74/0, 73/0 | 6.0, 5.9 |
| STPM00SPM 144 | 86.0 | 91.3 | 91.4 | 18, 19 | 85/0, 85/0 | 90/0, 90/0 | 14.7, 14.6 |
| STPM00SPM 145 | 89.6 | 94.5 | 95.2 | 22, 22 | 81/0, 81/0 | 71/0, 73/0 | 9.2, 9.2 |
| STPM00SPM 146 | 87.5 | 90.6 | 91.1 | 30, 20 | 69/0, 70/0 | 68/0, 72/0 | 6.1, 6.0 |
| STPM00SPM 147 | 57.7 | 58.6 | 58.4 | 16, 16 | 58/0, 58/0 | 49/0, 48/0 | 7.4, 7.4 |
| WTZZ00DEU 143 | 92.1 | 95.8 | 92.6 | 17, 20 | 87/0, 85/0 | 66/0, 64/0 | 8.5, 8.0 |
| WTZZ00DEU 144 | 90.3 | 92.5 | 94.3 | 30, 22 | 70/0, 67/0 | 54/0, 59/0 | 8.3, 8.1 |
| WTZZ00DEU 145 | 86.1 | 91.2 | 92.6 | 18, 18 | 68/0, 68/0 | 60/0, 64/0 | 7.0, 7.0 |
| WTZZ00DEU 146 | 90.2 | 96.6 | 97.2 | 38, 22 | 72/0, 71/0 | 30/0, 29/0 | 10.3, 9.5 |
| WTZZ00DEU 147 | 50.9 | 50.9 | 51.1 | 16, 17 | 53/0, 53/0 | 21/0, 21/0 | 13.4, 13.4 |
| BAKE00CAN 143 | 97.5 | 98.5 | 97.5 | 19, 27 | 76/0, 75/0 | 68/0, 67/0 | 7.9, 7.9 |
| BAKE00CAN 144 | 97.4 | 98.1 | 98.4 | 17, 17 | 81/0, 81/0 | 62/0, 62/0 | 4.6, 4.5 |
| BAKE00CAN 145 | 91.0 | 92.0 | 92.7 | 18, 18 | 98/0, 98/0 | 84/0, 84/0 | 4.0, 3.9 |
| BAKE00CAN 146 | 96.0 | 97.3 | 97.4 | 20, 24 | 80/0, 78/0 | 69/0, 70/0 | 4.6, 4.3 |
| BAKE00CAN 147 | 92.0 | 94.1 | 94.2 | 18, 18 | 77/0, 76/0 | 65/0, 65/0 | 2.9, 2.7 |

- Decisions by the phase per run, part 2 alone: 4 to 17 pair-epochs (243 and 98 on DOY 147), every one the integer
  nearest to the float wide lane; pair-epochs waiting for persistence 35 to 96; no decision for the neighbour; no
  persisted contradiction, no satellite returned, in either set.
- The chain: learned 28 to 30 (STPM00SPM), 36 to 38 (WTZZ00DEU), 36 (BAKE00CAN); no reset.
- `rsfs2` against `b12flt` (13.5): unchanged, 0.108 to 0.037, 0.135 to 0.029, 0.051 to 0.024.

**Reading.** The persistence costs 1 to 4 points of held share against the rule of section 12 (STPM00SPM 95.0 to 92.2
on DOY 143, WTZZ00DEU 97.4 to 95.8) and keeps a gain over the control on 13 of 15 receiver-days (1 to 7.5 points) with
no integer different from the control and none between the two start times. The neighbour path and the reset remain
unexercised by data: nothing in these 15 receiver-days needed them.
