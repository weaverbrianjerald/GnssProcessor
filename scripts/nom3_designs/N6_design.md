# Card NOM step N6: the network owns its local parameter set; one observation equation serves producer, consumer and seeds (design)

- **Step:** card NOM step N6 (`docs/refound/NOM_network_object_model_2026-10-08.md` sections 3.3, 5, 8: "the network
  owns its local parameter set (no borrowed user `StateManager`) and one observation-equation function, returning its
  term list, serves the producer's H, the consumer's H and the seeds"; behaviour-changing, its own card, graded on both
  beds). `docs/onboarding/08_adding_a_network_parameter.md` ("Known rough edges") names the same gap.
- **Measured at:** main `b701d01`. Read only: no build, no data run.
- **Status:** designed and pre-registered; no code. Every default in section 7 is a proposal for the author.
- **Appendix:** `N6_sites.tsv`: one row per network file naming the user-side state (StateManager calls, `StateKeys`
  sites, `h2local` uses, lines); 71 rows. Counts are comment-stripped scans (section 9), re-measured by the step that
  moves them.

## 1. What the step delivers, and the bar

1. **A network-owned local parameter set.** Each station's live parameters (the columns of its local block, with
   linearization points) are a typed table (`StationParams`) born by network functions and keyed by the identities of
   `common/GnssIdentity.hpp`. `NetStation` loses its `StateManager` and `MeasurementModel`; 200 StateManager calls and
   165 `StateKeys` sites in the network tree become lookups.
2. **One observation equation.** One function returns the term list of a row; the producer's H, the consumer's H and
   the seeds read it. On the user side the equation is written three times (2.4); producer and consumer already share
   the first copy, under different configuration flags.
3. **A printed agreement.** A run-start table "observation equation as armed" lists every effect with its producer and
   consumer treatment, backed by a unit test.

**The finding that shapes the plan.** Nearly all of N6 can be byte-identical. Three things move numbers: the seeds that
omit terms (2.4), the GLONASS frequency table of the ambiguity seed, and an optional canonical column order (D2). N6
splits into a byte-identical body (N6.0 to N6f, gated like N4) and a short behaviour-changing tail (N6g to N6i), each
tail step a default-off knob graded alone (standing invariant 6).

**The bar** (card NOM section 10, decision 3): clarity first, G0 a measurement; an unexplained move does not land.
**Non-goals:** no snapshot format change, knob rename, or change of what the solve estimates in the body; the consumer's
`StateManager`, Kalman filter and BSSD transform, the baseline DD path and the border layout (`NetworkEstimator`) are
untouched; constraint-row algebra (pins, anchors, ionosphere ties, SD-EWL, coupling rows) is untouched, only how it
finds a column changes.

## 2. Survey (measured at `b701d01`)

### 2.1 What the producer takes from the user-side stack

`NetStation` (`include/network/driver/RunTypes.hpp:49`) holds `unique_ptr<StateManager> sm`, `PppModel`,
`MeasurementModel`. `buildStation` (`ApplicationNetworkSetup.cpp:941`) builds `StateManager(5, ...)` from the full user
configuration, so a user flag (velocity, clock rate, phase clock, gradients, IMU; `StateManagerLayout.cpp`) decides a
network station's layout. The goldens set none.

| quantity (`src/core/network` + `include/network`) | sites | files |
| --- | --- | --- |
| StateManager method calls (42 distinct methods; 170 reads, 30 writes) | 200 | 47 |
| lines naming a station's `sm` (bindings, parameters) | 264 | 52 |
| `StateKeys::` / `K::` sites: 105 parse, 49 make, 11 predicates (tests/network: 174 sites in 11 files) | 165 | 50 |
| hand-built composite keys (`+ "_" +`, `+ "\|" +`) | 77 | 24 |
| `h2local` (state index to local column) uses | 116 | 30 |
| `MeasurementModel` calls, 8 members (below) | 11 | 8 |
| dense `em.H` reads (`StationSplit.cpp` 12, `Congruence.cpp` 4, `ParamInventory.cpp` 3) | 19 | 3 |

Largest files (sm / make / parse / `h2local`; all in `N6_sites.tsv`): `StationBlock.cpp` 22/0/1/3, `ParamInventory.cpp`
20/3/11/0, `RedundancyCensus.cpp` 13/0/1/2, `SlipQc.cpp` 12/4/6/0, `StationSplit.cpp` 10/2/3/6, `PhaseDatum.cpp` 6/4/16/8.

Top reads: `getAmbiguities` 24, `getStateVector` 20, `getCodeBiasStates` 15; writes: `markAmbiguityPendingReseed` 13,
`prune*StatesExcept` 11. No call reads a covariance, variance, process-noise or transition matrix: for the network the
`StateManager` is a column registrar and a linearization-point store. Ten calls read user-only layout accessors
(velocity, clock rate, gradients, ionosphere-rate map), all in the label builders `buildLocalLabels`
(`StationBlock.cpp:220`) and `localColumnLabels` (`RedundancyCensus.cpp:179`), which exist only because a user flag can
add such columns.

The `MeasurementModel` calls: `computeMeasurementUpdate` (`StationSplit.cpp:108`), `ensureNetworkSharedStates` 3,
`noteAbsorbedSharedState` 2, and one each of `absorbedSharedStates`, `carrierFrequency`, the allowlist and GLONASS-slot
setters, `getStateInitializer().setCodeBiasSeedOverride` (`WarmResume.cpp:1378`, the only direct `StateInitializer`
use). The surface to replace is small.

#### User-side code the network executes

| piece | lines | what the network uses |
| --- | --- | --- |
| `buildJacobianRow` (`MeasurementModelRow.cpp:211`) | 404 | the whole row |
| pre-pass `ensureRequiredStates` (`MeasurementModelStates.cpp`, 1076) | 13 members | rows 4, 5, 6, 8, 12 of its table (slant ionosphere, phase colour, cold clock, per-band ambiguity, receiver code bias); rows 1 to 3, 7, 9, 13 are BSSD, NL/WL or user satellite-bias work, inert; rows 10, 11 write no state |
| `MeasurementModelNetwork.cpp` + network block of `MeasurementModel.hpp` + `AbsorbedSharedStates.hpp` | 233 + about 130 + 119 | `ensureNetworkSharedStates`: border columns allocated in every station's `sm`, pruned, four withholding gates |
| `StateInitializer` (752) | 6 functions | `initializeInterSystemBiases`, `seedColdReceiverClock`, `initializeCodeBiases`, `computeIonosphereSeed`, `resolveReceiverBias`, `computeRawResidual`; each writes the `StateManager` it holds |
| `StateManager::computeAmbiguitySeed` (`StateManager.cpp:698`) | 60 | every ambiguity birth and reseed |
| `isUsableMeasurement`, `rowVariance` | 60, about 215 | pure functions of configuration and observation (no state use) |
| network-mode lines inside user-side files | 26 in 10 files | `MeasurementModelRow.cpp` 5, `MeasurementModelStates.cpp` 8, `MeasurementModelVariance.cpp` 3 |
| `ParamInventory.cpp` reverse classification (`listStateColumns`, `classifyStateColumn`) | about 280 | recovers a typed identity from (container, key string) after the user side made the column |

Findings that cost nothing to act on: `PppModel` holds a `StateManager&` it never reads (`PppModel.cpp:215` is the only
use; 5 production constructor sites, 4 tests); `isUsableMeasurement` and `rowVariance` need no state; covariance, q
and birth variances are inert, so the network set stores none and the user `filter.initial_std.*` keys stop leaking
into network births.

### 2.2 What the consumer takes

The consumer (closed-loop held-out station, `ApplicationConsumerReplay.cpp`, the standalone real-time profile) is the
real-time engine on a config derived from the producer's (`deriveClosedLoopConsumerConfig`,
`GnssConfigLoadNetwork.cpp:11932`). It is a full user filter and keeps its `StateManager`: 105 calls in 10 files of
`src/core/realtime` (`RealTimeEngineStateSnap.cpp` 29, `RealTimeEngineSsrBreaks.cpp` 22, `RealTimeEngineEstimate.cpp`
15), including covariance reads and writes the network never makes. It has 4 `StateKeys` sites and 5 `MeasurementModel`
calls (one `computeMeasurementUpdate` per epoch, `RealTimeEngineEstimate.cpp:63`); `ApplicationConsumerReplay.cpp`
wires the engine and has no direct use. `applyNetworkConsumerCouplings` (`GnssConfigLoadNetwork.cpp:11858`) sets
processing mode `PPP_RTK` and turns `estimate_sat_clocks`, `estimate_sat_phase_biases` and
`estimate_network_sat_code_bias` off: the satellite biases enter as known SSR terms, the satellite clock through
`obs.modeled`.

### 2.3 Where the two equations share code and where they diverge

Shared code: `computeMeasurementUpdate` (`MeasurementModel.cpp:170`) calls `buildJacobianRow` for the producer station,
the real-time engine, the PPP loop (`PppEpochUpdate.cpp`, 3 sites) and the user-path satellite-bias births
(`MeasurementModelStates.cpp:870,922`); `isUsableMeasurement`, `rowVariance`, `PppModel::computeModels` and the pre-pass
are shared too. Divergence is by flag, term by term:

| effect | producer (golden) | consumer (closed loop) |
| --- | --- | --- |
| modeled value | `PppModel`, broadcast a priori clock | `PppModel`, clock from the SSR store |
| range correction `-los . (x - x_lin)` | position columns kept in H, `Pinned`, dropped by the split | estimated |
| receiver clock, ZWD, ISB, slant ionosphere, receiver code bias, ambiguity (+ phase colour) | local columns | states |
| satellite code bias `+1` | border column | known SSR term (+1), or a user state under `estimate_sat_biases` |
| satellite phase bias `+1` (+ receiver phase bias, eq34) | border column | known SSR term (+1) |
| satellite clock `-1` | border column | inside `modeled` |
| SSR term | off in network mode (`MeasurementModelRow.cpp:453`) | applied |
| gradients, phase clock, Doppler, velocity, IMU | gradients by user flag only | options |

The sign agreement between the producer's satellite states and the consumer's known terms is two hand-written constants
(`kSsrCodeBiasSign`, `kSsrPhaseBiasSign`, `+1`, commented "can only be confirmed empirically"), the clock partial `-1`
and the `+1` partials. No test ties them.

### 2.4 The other copies of the equation

| copy | where | lines | omits relative to `buildJacobianRow` | frequency source |
| --- | --- | --- | --- | --- |
| `computeRawResidual` | `StateInitializer.cpp:243` | 84 | range, phase clock, satellite phase bias, satellite clock, SSR term, ambiguity | `PppUtils::getFrequency` |
| `computeAmbiguitySeed` | `StateManager.cpp:698` | 60 | the same, and the code-bias terms | file-local `bandFrequency` (`StateManager.cpp:71`), GLONASS at slot 0 |
| iono scaling in the S-basis | `Sbasis.cpp:1444,1566` | 2 sites | a constraint row, reference band differs by design | `freqOf` |

`computeAmbiguitySeed` has 14 call sites (`MeasurementModelStates.cpp` 7, `KalmanOutlierResponse.cpp` 3,
`PivotManager.cpp` 2, `CycleSlipRepair.cpp` 2); `computeRawResidual` has 4 (ISB, cold clock, phase clock, code biases).
The omitted terms are zero on the producer at a birth (shared states at 0, SSR off, range 0), non-zero on the consumer
(SSR code and phase bias, metres). For GPS and Galileo the frequency sources should agree numerically
(the N6a comparator proves it); for GLONASS they differ.

### 2.5 Orders that fix the bytes

1. **Column order.** `A_r` columns follow state-index order: birth order, survivors keeping relative order on removal
   (`appendState`, `removeStateAtIndex`). The Schur kernel's factorization order, hence rounding, follows it.
2. **Map iteration order.** Injects and harvests iterate key maps in string order (rank-1 update order). Typed keys
   keep it only if the comparator reproduces the string order: proven (N0) for `ArcId` and satellite keys, not proven for
   the receiver code-bias keys (`GPS_DCB` < `GPS_IFB5` < `GPS_ISC_C1C` against `ParamForm` order), the ISB keys, the
   receiver phase-bias key and the satellite-signal keys.
3. **Floating-point sum order of the predicted observable:** `modeled + range + rx_clk + phase_clk + tropo_wet +
   tropo_grad + isb + iono + code_bias + sat_bias + sat_phase_bias + sat_clock + ssr (+ amb)`, left to right, with
   `sat_phase_bias` already a sum of two states and `tropo_grad` a product with a bracket. A flat sum of products changes
   the last bit, so the term list keeps the grouping.

## 3. The target types (plain value types, tables, free functions; no virtual dispatch)

### 3.1 Identity, relocated

`common/ParamRef.hpp` (new; `ParamForm`, `ParamClass` and the kind enum leave `network/assembly/`, so user-side
measurement code includes no network header):

```cpp
enum class UserCore : std::uint8_t { None, Velocity, ClockRate, PhaseClock, GradientNorth, GradientEast,
                                      DifferentialTropo, Attitude, AccelBias, GyroBias };   // user-only fixed core
struct ParamRef {                  // one parameter, without its station
    NetworkParamKind kind;         // SatClock .. RxPosition (the 12 existing kinds)
    ParamForm form;  System sys;  SatId sat;  SignalId signal, signal_b;
    FrequencyBand band, band_b;  int glo_slot;
    UserCore core;                 // None on every network term
};
bool operator<(const ParamRef&, const ParamRef&);   // equal to the string order, per map (2.5)
```

`ParamInstanceId` becomes a `ParamRef` plus a `StationId`. The registry keeps `NetworkParamKind`.

### 3.2 The equation

`measurement/ObservationEquation.hpp` (new): code and phase rows; Doppler rows stay a user-side function (D3).

```cpp
enum class ObsEffect : std::uint8_t { Modeled, RangeCorrection, ReceiverClock, PhaseClock, Troposphere, TropoGradient,
    SystemTimeOffset, Ionosphere, ReceiverCodeBias, SatelliteCodeBias, SatellitePhaseBias, SatelliteClock, SsrBias,
    Ambiguity };                                   // the existing locals of buildJacobianRow, in sum order
struct ObsPart   { ParamRef ref; double partial; };
struct ObsAddend { ObsEffect effect; double constant, scale;            // value = constant + scale * sum(partial * x)
                   std::array<ObsPart, 2> part; std::uint8_t n_part; }; // two parts: satellite + receiver phase bias, NL + WL
struct ObservationEquation { std::array<ObsAddend, 16> addend; std::uint8_t n; bool usable; };  // no heap
template <class View>   // View::find(const ParamRef&) -> const double* (x_lin), null when the parameter is absent
ObservationEquation observationEquation(const ObsInput&, const PppObservation&, const std::string& ref_type, const View&);
double evaluate(const ObservationEquation&, const View&);   // left to right: today's bytes
```

`scale` keeps today's products bit for bit (`mf_grad * (cos * x_n + sin * x_e)`, `lambda * (x_nl - s * x_wl)`); a partial
is emitted exactly when `find` is non-null, as `idx >= 0` is today, so a withheld shared column stays out of the row.
`ObsInput` carries configuration flags, `PppUtils`, the SSR store handle, the NL/WL reference bands, and
`resolveReceiverBias` returning a `ParamRef` and a coefficient (today a string key). Two views, one function:
`UserView` adapts `StateManager` (one switch `userKey(ref)` is the only place a typed identity becomes a user-side
key); `NetView` adapts `StationParams` and the epoch's shared admission (shared kinds return a constant `0.0` while the
station references them). Three consumers, all free functions:

- `scatterDense(eq, column_of, n)`: `buildJacobianRow` keeps its signature and becomes this composition.
- `scatterNetwork(eq, border_col, local_col, A_s_row, A_r_row)`: the producer's rows, replacing the dense H and the
  column split (N6f).
- `seedResidual(eq, mask, observed)`: `computeRawResidual` and `computeAmbiguitySeed` become two masks.
  `kLegacySeedMask` reproduces today's omissions (identical); `kFullSeedMask` is N6g.

The sign relation of 2.3 becomes data: one `kSatBiasSign` is both the producer's partial and the consumer's known-term
multiplier; the clock partial and the consumer's clock inside `modeled` get one test (N6a).

### 3.3 The parameter set

`network/state/StationParams.hpp` (new), a member of `NetStation` in place of `sm` and `mm`:

```cpp
struct LocalSlot { ParamRef ref; double x_lin; ParamDisposition disp; bool pending_reseed; };
struct StationParams {
    std::vector<LocalSlot> slots;            // state-index order: append at birth, erase keeping order (2.5.1)
    std::map<ArcKey, int> amb;  std::map<SatId, int> iono, colour;  std::map<RxCbKey, int> rx_cb;
    std::map<System, int> isb;  std::map<SatSignalId, int> rx_pb;     // each ordered as the string key it replaces
    int clock{-1}, zwd{-1};
    StationShared shared;                    // typed admitted border parameters this epoch + the absorbed notes
    SeedOverrides rx_cb_seed;                // the warm-resume code-bias override, applied by the code-bias birth
};
```

Free functions (`LocalBirth.cpp`, `SharedAdmission.cpp`): one birth per family (ionosphere, phase colour, ambiguity,
receiver code bias, ISB, cold clock), `admitShared` (the four gates of `ensureNetworkSharedStates` and its prune),
`markPendingReseed`, `pruneExcept`. The inventory reads the set; `listStateColumns`, `classifyStateColumn`,
`StateContainer`, `stateContainerName` (about 280 lines) are deleted because identity is stored at birth.

**Ownership (D1).** The set is per-station state written inside the parallel produce phase, so it stays on `NetStation`,
not in the entity tables (serial writes only, check V5 of `nom_n4_verify.py`). The entity tables keep the carries and
meet the set by typed key (`entities.arcs.find(ArcId{...})`); no field is duplicated.

### 3.4 Seeds and the reader's table

`measurement/Seeds.hpp`: pure functions returning values (`seedIsb`, `seedColdClock`, `seedCodeBias`,
`seedIonosphere`, `seedAmbiguity`); the user side applies a value with `StateManager` setters in today's call order,
the network with `StationParams` births. `StateInitializer` keeps its latches and loses its `StateManager&`.
The run-start table "observation equation as armed" (producer under `Network (...)`, consumer in the engine's config
echo) prints one line per effect (name, partial, producer treatment, consumer treatment) from the equation's own
declaration `kObsEffects`, so the printed sign and the used sign are one declaration.

## 4. Steps

Each body step is byte-identical on the gate set and re-surveys its files first. Sizes in sessions.

| step | what | main files | behaviour | size |
| --- | --- | --- | --- | --- |
| N6.0 | prep: `PppModel` drops its dead `StateManager&`; `isUsableMeasurement` (pivot-signal map as an argument) and `rowVariance` become free functions; identities move to `common/ParamRef.hpp`; typed-order tests (2.5.2) over the golden signal universe | `PppModel.*`, 5 constructor sites, `MeasurementModelRow.cpp`, `MeasurementModelVariance.cpp`, `ParamRegistry.hpp`, `ParamInventory.hpp`, `CMakeLists.txt` | identical | 1 |
| N6a | `observationEquation`, `UserView`, `scatterDense`; `buildJacobianRow` becomes their composition. Comparator `processing.obs_equation_check` (default off, TRACE): old and new run on every row, bitwise differences of H and predicted counted, old frequency sources compared per (band, system). Tests: bitwise table (producer, consumer BSSD and undifferenced, PPP IF, uncombined, NL/WL); agreement test (3.2) | `ObservationEquation.*`, `MeasurementModelRow.cpp`, `MeasurementModel.hpp`, ledger row | identical | 2 |
| N6b | seeds on the equation with `kLegacySeedMask`; `Seeds.hpp` returns values, the user side applies them; one frequency source where N6a showed equality (GLONASS slot table listed as the open difference) | `StateInitializer.*`, `StateManager.cpp`, `MeasurementModelStates.cpp`, `Seeds.*`, 14 seed call sites | identical (GLONASS arms: an explained move) | 2 |
| N6c | the set in shadow: rebuilt from `sm` after each station's pre-pass by the existing classifier; a comparator asserts slot order, `x_lin`, dispositions, typed orders; the table is printed | `StationParams.*`, `ApplicationNetworkAssembly.cpp`, `StationSplit.cpp` | identical, new lines only | 1 |
| N6d | readers switch, one subsystem per sub-step, writers still on `sm`: d1 inventory, labels, diagnostics; d2 carries and harvests; d3 slip, admission, datum, anchors; d4 AR, snapshot write, warm resume. Each `StateKeys::parse*` and `h2local` lookup becomes a typed lookup | the 71 files of `N6_sites.tsv` | identical per sub-step | 4 |
| N6e | writers and births move: network birth and admission functions write the set; `observationEquation<NetView>` builds rows; dense `em.H` still produced by `scatterDense` over the set; `NetStation` loses `sm` and `mm`; `MeasurementModelNetwork.cpp`, the network block of `MeasurementModel.hpp`, `AbsorbedSharedStates.hpp` deleted; shadow and comparator removed | `LocalBirth.cpp`, `SharedAdmission.cpp`, `StationRows.cpp` (new), `ApplicationNetworkSetup.cpp`, `PassASlipScan.cpp`, `StationSplit.cpp`, `RunTypes.hpp`, `MeasurementModel*` | identical | 3 |
| N6f | direct assembly: `scatterNetwork` writes `A_s`, `A_r`; the dense H and the column split go; "all-zero column" keeps the `maxCoeff() == 0.0` meaning for explicit zero partials | `StationRows.cpp`, `StationSplit.cpp`, `Congruence.cpp` | identical; G3 improves | 1 |
| N6g | behaviour 1, knob `estimation.seed_full_equation` (default off, fingerprinted): seeds use `kFullSeedMask`, so consumer ISB, clock, code-bias and ambiguity seeds include the SSR code and phase bias; one frequency rule with the user-side organization document's frequency item (per-slot GLONASS) | `Seeds.cpp`, `GnssConfig*`, ledger, fingerprint | consumer moves at births; producer expected identical | 1 |
| N6h | behaviour 2: the loader refuses the user layout flags in network mode (`estimate_velocity`, `estimate_phase_clock`, `estimate_tropo_gradients`, `enable_imu`) naming the network key; `network.estimate_tropo_gradients` (default off, fingerprint term, ledger row) is the first parameter added the new way | `GnssConfigLoadNetwork.cpp`, `StationParams`, registry, ledger | goldens identical; a config setting a flag fails to load | 1 |
| N6i | optional (D2): canonical column order (kind, satellite, signal) instead of birth order | `StationParams.cpp` | ppb-level move, a re-baseline | 1 |
| N6j | close: tests re-pointed (12 files in `tests/network`, `test_measurement_model_prepass.cpp`, 4 `PppModel` constructor sites), page 8 and the network tour rewritten, `CLAUDE.md` routing, `OPEN_ITEMS`, `nom_epoch_lint` | docs, tests | none | 1 |

Order. N6.0 alone first. N6a and N6b edit user-side files that the user-side organization work
(`docs/refound/UOM_user_object_model_2026-10-13.md`) also edits; N6a announces its file list there. N6c to N6f are
sequential; N6d sub-steps touch disjoint files. N6g to N6i wait for N6f and are graded separately.

## 5. Gates

- **G0, byte identity (N6.0 to N6f).** The N4 arm set (the 23 arms of `D:/GnssProcessorSupport/gates/g5`), products
  without `#` lines, `closed_loop_trace.csv`, `network_epoch_status.csv`, `snap.txt`; plus, because user-side code is
  shared, the 16 user arms of `scripts/uom_gate.py` (`quick` per step, `full` before landing) and the real-time replay
  arms. Same-build Release flags, renamed exes. Extra arms: N6a comparator on (zero mismatches); N6c and N6d shadow
  comparator on; N6e a warm-resume pair (`snapw`, `snapr`, `wkro`) and a GLONASS code-only, a three-band and a GPS-only
  arm (the absorbed counts of "parameters as armed" match line for line).
- **G1, coverage.** N6d: zero `sm` calls and `StateKeys::parse*` in moved files, by `git grep` and by the compiler.
  N6e: no `StateManager`, `MeasurementModel` or `StateInitializer` name in `src/core/network` and `include/network`;
  the "unclassified" integrity line cannot occur.
- **G2, the reader test.** The run-start tables of the GPS+Galileo and three-band field arms keep their lines and add the
  equation table.
- **G3, cost.** Field golden `f2` wall within +3 % per body step (N6f expected negative); user arms and replay within
  +3 %.
- **Behaviour steps (N6g to N6i).** A golden copy with only the knob changed; delta against the baseline pair and a
  same-exe control, never one instrument: the held-out consumer by hour on the field golden (DOY 129, `WTZZ00DEU`) and
  the sim golden (`SZZ000SIM`, graded from 00:30:15), the 16-site census (`gcensus.py`, or `simcensus6h.py` with
  `SIMCENSUS_BASE`), sim truth; 2 h for development, 6 h for adoption, hour 1 as ignition, settled claims from hours 2
  to 6. Adoption is the author's decision through the ledger and `make_golden_config.py --verify`.

## 6. Risks

1. **Producer and consumer disagreement.** One function means one edit moves both. Mitigations: the printed table; an
   agreement test (producer row with satellite states `p`, `b`, `c` equals the consumer row with the same values as known
   terms, to 1e-9 m, both signs, each band); the consumer in every behaviour grading; a term-list change lands only with
   the agreement test in the same commit.
2. **Summation and orders** (2.5). A flat sum or a re-grouped addend (2.5.3) is pinned by N6a (bitwise table,
   comparator), a sorted column list (2.5.1) by N6c (shadow check), a map order (2.5.2) by N6.0 (typed-order tests).
3. **Snapshot and fingerprint.** No record changes in the body; the fingerprint reads knobs, station names and pivot
   flags, not the layout (`SnapshotFingerprint.cpp`). Three places touch the snapshot: `StateSnapshotWrite.cpp:467`
   reads the code-bias `x_lin` from `sm`; `WarmResume.cpp:1378` installs the code-bias override on `StateInitializer`;
   station columns are not snapshotted and are born at the first epoch after a resume. They move in N6d4 and N6e with the
   warm-resume arms as gates. N6g changes births, so its knob is fingerprinted (no resume across it); N6h adds one term.
4. **The parallel contract.** PASS A and PASS B write their own station only; the set stays on `NetStation` (D1). A birth
   that inserts into an entity table inside the produce loop is a data race.
5. **The real-time engine and the PPP loop** run `buildJacobianRow` every epoch. `scatterDense` builds the same row
   from a stack list; the per-row string keys of today become one `userKey` per present family. User arms and replay
   are gate arms, so a consumer regression is caught in the body.
6. **Concurrent user-side work** edits `MeasurementModel*.cpp`, `StateInitializer.*`, `StateManager.cpp` too. N6.0 and
   N6a land first; the frequency rule is decided once, in N6g, with that work.
7. **Hidden dependence on user layout flags.** A network config setting one gets extra columns with no network key
   (page 8). N6h makes it a load error; the configs under `config/` are scanned for the four flags first.
8. **Test volume.** 12 `tests/network` files name `StateManager` or `StateKeys`; 44 files under `tests/` name
   `StateManager`. Tests are re-pointed, none deleted.

## 7. Decisions for the author (each with its default)

- **D1, owner of the live set.** Default: `NetStation::params`, written in the parallel phase; entity tables keep the
  carries and share typed keys. Alternative: an entity-table row (needs serial births; rejected).
- **D2, column order.** Default: keep birth order permanently (N6i not taken), a documented contract of the set.
  Alternative: canonical order, one declared re-baseline on both goldens.
- **D3, scope of the shared function.** Default: code and phase rows; Doppler, velocity, IMU stay a user-side function.
- **D4, who builds the single equation.** Default: N6 (N6a, N6b); the user-side organization work's "later one
  observation equation" item is retired.
- **D5, direct assembly (N6f).** Default: take it (no dense H on the network). Alternative: stop at N6e.
- **D6, full-equation seeds (N6g).** Default: build the knob, grade, adopt only if consumer hours 1 to 6 and the
  census are not worse (expected effect: small, at ignition).
- **D7, user layout flags in network mode (N6h).** Default: load error naming the network key. Alternatives: warn and
  ignore; keep.
- **D8, shadow and comparator.** Default: temporary, removed in N6e. Alternative: keep `obs_equation_check` as a trace.
- **D9, an order a typed key cannot reproduce.** Default: keep the string comparator for that map. Alternative: a
  declared re-baseline of that map's iteration order.
- **D10, the set and the snapshot.** Default: not snapshotted (columns born cold, as today); no format change.

## 8. The onboarding recipe afterwards

| page 8 today | after N6 |
| --- | --- |
| step 1: fixed column or `StateKeySchema` make/parse pair, `add...State`, creation site in `MeasurementModel`, partial in `buildJacobianRow` (6 sites, 4 files); a user flag changes the layout | one `ParamRef` kind or form, one birth function in `LocalBirth.cpp`, one effect row in the equation; no user flag |
| step 2: registry kind, plus `StateContainer`, `listStateColumns`, `classifyStateColumn`, `paramClassLabel`, `stateContainerName`, `buildLocalLabels`, `localColumnLabels` | registry kind and contribution sets; inventory, labels and classification read the set |
| step 3: split disposition, "no edit by default" | a field of the slot; still no edit by default |
| "Known rough edges": borrowed `StateManager` and `MeasurementModel`; a user flag changes the layout; a parameter begins as a user-side state | removed; the gradient is the worked example (N6h) |
| the ZWD example | the `zwd` slot and the `Troposphere` effect row |

About ten edit sites in six files become about four in three (re-counted at N6j). Pages 7 and 8 and the `CLAUDE.md`
network routing change in N6j: "new per-station parameter" points to `StationParams` and the equation.

## 9. Reproducing the survey

`git grep -nE "(\.|->)(<89 StateManager method names>)\("` plus any method called on a variable named `sm` gives the
200 calls (comments stripped); `StateKeys::` and the `K::` alias (`Contributions.cpp`, `ParamInventory.cpp`) give the
165 key sites; consumer counts use the same forms on `src/core/realtime`. Scripts: session scratchpad `n6d/`
(`perfile.py` produced `N6_sites.tsv`; `keys.py`, `sm2.py`, `survey.py`), to be copied to the evidence folder.
