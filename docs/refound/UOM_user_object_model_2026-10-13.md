# UOM: the user side organized to card NOM's standard (KalmanFilter, MeasurementModel, the slip layer)

**STATUS**

- **id:** UOM
- **topic:** consumer
- **status:** open
- **summary:** Pre-registered. Card NOM's standard (one owner per concept, typed identities, declared properties, run-start tables a human reads, short functions with a linear story; plain value types and tables, no virtual dispatch for new structure; clarity over byte identity with every output move explained) applied to the user side: `KalmanFilter`, `MeasurementModel` and the slip layer first, then `StateManager`, the PPP pipeline and the identity reads. A read-only map of 2026-10-13 (ten investigators, one verifier per suspected defect) measured the state (section 1) and found 24 behaviour defects, 19 confirmed by reading (section 3). Step U0 (the gate), U1a (the state families declared once), wave A (the three giants: KalmanFilter, CycleSlipManager, MeasurementModel) and UB1 (the BSSD smoother crash) are built and integrated on branch `uom/work`: 15 of 16 user arms identical with run-end counter tables inserted, the smoother arm completing where main crashes, both 2 h network goldens identical (sections 9 and 10).
- **source:** the author's request of 2026-10-13 ("User-side organization. Same standard as NOM, applied to KalmanFilter, MeasurementModel and the slip layer"); [NOM](NOM_network_object_model_2026-10-08.md) sections 2, 3, 4 and 10 (the standard, the author's decisions); [NOM2](NOM2_network_object_model_rows_2026-10-09.md) section 6 and [NOM3](NOM3_network_giants_2026-10-09.md) section 3 (the cut method); card NOM's user-filter read of 2026-10-08 (session files); the organization map of 2026-10-13 (session files `uom_map/`: `kf.md`, `mm_core.md`, `mm_dd.md`, `sm.md`, `slip_orch.md`, `slip_giants.md`, `identity.md`, `pipeline.md`, `gates.md`, `expand.md`)
- **knobs / posture:** none. Organization steps are identical output or an explained move; behaviour changes are separate steps (section 5.4), each graded on its own
- **dates in the text:** 2026-10-13
- **hashes / run dirs:** map read on main `8326da3`; branch `uom/work` (`91b4df2` step U1a, `e231160` the gate, merged with main `366385b`); exes `gnss_processor_uomref.exe` (`FB581BD5`, main `8326da3`) and `gnss_processor_u1a.exe` (`5806370C`); golden arms `runs/_rap_prm_uom{ref,u1a}_{gf,gs}`
- **gates (frozen at pre-registration):** section 6

**Outcome.** Pre-registered. The user side reads well where it has one owner (`StateManager`, `StateKeySchema`) and badly where it grew card by card: six functions of 836 to 1,370 lines, families of state listed by hand in up to 19 places that disagree, identity re-parsed 5.9 times as densely as in the network after card NOM's N0b, three drivers that write one epoch three ways. The first step (the state families declared once) is built and identical on every arm. Next: wave A, the three named giants (section 5).

## 1. The measured state (main `8326da3`)

### 1.1 The giant functions

| function | file | lines | parameters | shape |
| --- | --- | --- | --- | --- |
| `KalmanFilter::validatePostFitResiduals` | `filter/KalmanFilter.cpp` (3,239) | 1,370 | 3 | 42 % of the file; 11 sections, 9 concerns, 4 exits, 6 lambdas, 23 local constants, 30 log calls; writes 7 members and StateManager metadata its signature does not show; four coherent-offset pivot detectors that are one algorithm written four times |
| `CycleSlipManager::detectAndRepair` | `slip/CycleSlipManager.cpp` (1,818) | 1,212 | 4 | 17 sections over 20 function-scope working sets, 13 nesting levels; writes `PivotManager` through a `const_cast`; on every post-processing config only the MW path runs (about 560 lines unreachable there) |
| `TurboEditResolver::resolve` | `slip/TurboEditResolver.cpp` | 1,120 | 5 | three parallel staging maps, two parallel continuity maps; `GfArc` mixes three concerns |
| `MwSlipDetector::processMwSample` | `slip/MwSlipDetector.cpp` (3,940) | 1,066 | 18 | 6 emission paths with hand-written out-parameters, 9 member containers written |
| `MeasurementModel::ensureRequiredStates` | `measurement/MeasurementModel.cpp` (3,100) | 911 | 2 | 15 sections for 6 state categories, each seeded from a different source; about 130 network lines interleaved |
| `MwSlipDetector::detectLargeMwResets` | `slip/MwSlipDetector.cpp` | 888 | 1 | three copies of pair, wavelength, MW, BIA, route; a 214-line S-transform inline |
| `Application::run` | `app/Application.cpp` | 836 | 0 | the per-epoch body is 78 % inline logic in 12 blocks; the run end entirely inline |
| `MeasurementModel::buildDdMeasurement` | `measurement/MeasurementModel.cpp` | 700 | 4 | three writes to filter state among pure row algebra, 11 duplicated exits, a process-wide static diagnostic flag |
| `runBaseline` | `app/ApplicationBaseline.cpp` | 1,043 | | one function |

`MwSlipDetector.hpp` is 2,112 lines, 76.5 % of them comments; about 407 header lines are network-only API.

### 1.2 Lists kept by hand that disagree

- **State families.** `StateManager` keeps nine key-to-index maps. 19 places list them by hand (10 in `StateManager`, 9 in consumers); the phase-colour family is in 4 and missing from 5, the ionosphere family is gated by `ProcessingMode::PPP_RTK` in 4 lists and created by `isUncombinedEnabled()`. One family has seven names (`CBIAS:`, `RXCB`, `ReceiverCodeBias`, `code_bias`, `CodeBiasStates`, `rxcb:`, `rx_code_bias`). 48 near-duplicate per-family methods hold 38 % of `StateManager.cpp`.
- **Slip detectors.** The detector kinds are enumerated in 8 places plus a Python map; they disagree about the IF detector.
- **Initial variances.** `seedCovarianceDiagonals` (8 categories), `resetFilterCovariance` (9), the StateManager birth rules (19); their fallbacks disagree.

### 1.3 Duplicated idioms

The observation equation is written three times (`buildJacobianRow`, `StateInitializer::computeRawResidual`, `StateManager::computeAmbiguitySeed`) and the DD path adds a fourth Jacobian; the seed copies omit the range correction, the SSR bias, the satellite clock and the phase-bias terms, and each uses a different GLONASS frequency source. An ambiguity reset exists in 3 KalmanFilter forms beside `StateManager::resetAmbiguity` and a `CycleSlipManager` form; the Joseph update in 3 forms; the SD seed in 4; the pivot covariance freeze in 3; the MW combination in 6 shared sites; Welford variance 12 times in one file; the cross-satellite noise cascade twice (MW and TurboEdit, already diverged).

### 1.4 Identity

NOM's identity pattern counts over the user file set (111 files, 34,398 code lines): 1,689 hits, 49.1 per thousand lines, against 8.3 in the network files after step N0b. 21 constellation name and letter converter copies, 5 band-to-frequency tables, 90 `sat + "_" + sig` ambiguity keys against 4 maker calls, 95 first-character observation-type tests, 52 `find('_')` ionosphere-free pair tests, 21 hand splits of the BSSD pair "G01-G05" in `KalmanFilter.cpp`. The GLONASS slot table lives in 17 `PppUtils` instances, of which the user path seeds one.

### 1.5 Three drivers and the network boundary

Post-processing (`Application::run`), real time (`RealTimeEngine`, already cut into seams but with its own 271-line slip orchestrator, rebuild loop and AR configuration) and baseline (`runBaseline`) write one epoch three ways; where the copies differ there are defects (section 3). The network calls 43 `StateManager` methods (221 calls), `MwSlipDetector` (26) and `MeasurementModel` (14), and drives `KalmanFilter` only through the closed-loop consumer, which is a `RealTimeEngine` (467 of 697 network configs, both goldens). About 3,400 lines of network-only code sit inside user classes (StateManager network kinds about 400, the MW phase bridge 406, MeasurementModel 171 plus 30 branch sites, IfPhaseSlipDetector 2,308).

### 1.6 Tests

No unit test drives `KalmanFilter::update`, `validatePostFitResiduals`, the outlier handlers, `resetFilterCovariance`, the MeasurementModel pre-pass, Jacobian or variance path, `CycleSlipManager`, the detector registry, GRAPHIC or EWL. Every cut therefore leans on the end-to-end gate (section 6).

## 2. What reads well, and what this card keeps

One owner of the state (`StateManager`), one header for every key format (`StateKeySchema`), one configuration block per property, thorough Doxygen, and a pipeline that is short at its spine. The card keeps all four and adds what the network got from card NOM: a declared table where a list was kept by hand, a typed identity where a string was re-parsed, a short function per concern, and a table a reader sees at run start.

## 3. Defects found while mapping

One verifier per high or medium-confidence claim (refute the mechanism; refute the reach). "Confirmed" is by reading; no numerical effect was measured unless stated. Fixes are behaviour changes and go to section 5.4.

**Confirmed, live on a shipped config**

| id | defect | reach |
| --- | --- | --- |
| S1 | PPP with `uncombined_enabled`: the slant-ionosphere (and rate) states are created and observed but get zero process noise; the Q gate tests `ProcessingMode::PPP_RTK`, creation `isUncombinedEnabled()` | `config/test_suite/_nlwl_guarded.json`, `_nlwl_unguarded.json`, `_pppar_algo_static_gps_l1l2.json` |
| S3 | Two ambiguity initial-std sources: births and slip resets read a hard-coded 1,000 m (`GnssConfig::ambiguity_std_`, no JSON key); pivot reseeds and DD births read `filter.initial_std.ambiguity` | 12 configs set 100 m, e.g. `ppp_rtk_kinematic_multi.json`, `realtime_ppp_rtk.json` |
| K-D1 | Huber R-inflation targets are cleared only in `KalmanFilter::beginEpoch` (documented as a log gate), which only `Application::run` calls: on the RT engine, baseline and the closed-loop consumer a once-inflated phase row stays down-weighted for the rest of the run | the consumer of both goldens; `baseline_single.json`, `baseline_multi.json`, `sbf_replay.json` |
| K-D4 | The complete-reset backstop cannot fire in baseline (the per-pass outlier count is never carried) | `baseline_single.json`, `baseline_multi.json` |
| K-D5 | The non-pivot multi-band slip response builds a PPP key in baseline, misses, queues a junk key and reports the satellite reset | `baseline_single.json`, `baseline_multi.json` (needs a two-band slip MW misses) |
| K-D7 | The stuck-code-bias guard is announced on but runs only inside the residual-history block | 8 configs, e.g. `ppp_internal.json`, `realtime_ppp_rtk.json` |
| DD-D1 | DD innovations use the caller's state snapshot taken before the in-call pivot handover, prune and birth | `baseline_single.json`, `baseline_multi.json` |
| DD-D2 | `StateInitializer::initializeCodeBiases` subtracts the seeded slant ionosphere twice (shared with card NOM: it moves the network receiver code-bias linearization point) | `ppp_rtk_user_rxcb.json`; network postures that force `estimate_ifb` |
| DD-D3 | The BSSD pivot handover, freeze and disturbance check ignore NL/WL ambiguity keys | `_nlwl_guarded.json`, `_nlwl_unguarded.json` |
| P-D3 | The ATX-substituted ambiguity Q override exists only inline in `Application::run`; real time applies only its birth half | `realtime_ppp_rtk_AR.json` |
| P-D4 | The PPP epoch summary prints the pre-AR position | `_pppar_algo_static_gps_l1l2.json`, `_nlwl_guarded.json` |
| P-D5 | The KalmanFilter MW tiebreak reaches MW only through a `CycleSlipManager` pointer that real time never sets | every RT config and golden consumer |
| SO-D2 | Real-time MW-mandated full resets skip the satellite-wide re-inflation post-processing performs | RT PPP-RTK configs |
| G-D1 | The smoother trace writer indexes the receiver clock with `SIZE_MAX` under BSSD: access violation (0xC0000005), reproduced on main's exe | `ppp_rtk_internal_AR.json`; any BSSD config with `run_smoother` and the smoother trace |
| G-G9 | Log stamps print `HH:MM:60.000` on whole minutes (Julian-day double rounds without a minute carry) | every config |
| G-G10 | Baseline runs write an empty `state_trace.csv` and warn every epoch (the trace logger needs an absolute tropo index) | `baseline_single.json` |
| ID-SD1 | BeiDou B1I in its RINEX 3.02 spelling (C1I/L1I) is modelled at the B1C carrier by `PppUtils`, `StateManager`, `KalmanFilter` and `PppModel`; `SignalBand` and `SignalId` say B1I | two test-suite network configs with BeiDou priority `IDPXQZ`; the decision is shared with card NOM (its BeiDou digit-1 item) |
| SG-D6 | `logging.enable_if_slip_trace` is loaded and never read | 5 configs set it |
| X-D1 | The RT engine and the closed-loop consumer copy 18 of 20 `AmbiguityResolverConfig` fields (no partial AR, no arc maturity gate) | `_golden_sim_consumer_ar_a.json`; card IRD owns the fix |

**Confirmed or partly, latent (no shipped config reaches it):** the reset's pivot freeze is dead (the KalmanFilter's private `MeasurementModel` is never driven; K-D2); the complete reset leaves up to 10 families at zero variance (K-D3, S2); the MW tiebreak can override the data-snooping selection (K-D6); seeds omit the SSR satellite bias (MM-D1, reached by RT and the consumer, effect inside the seed sigma); GLONASS coefficients from the default channel table off the network (MM-D2, ID-SD2, X-D3; identical on the DOY-129 fixture, whose header equals the table); the differencing regime split between the strategy and the raw toggle (MM-D3); IF rows ignore `bias_coverage_requires_apply` (MM-D4); a Doppler-only satellite's all-zero shared clock column (MM-D5); DD ionosphere-free exclusion and R grouping (DD-D4); IRNSS naming (DD-D5); real time drops un-adjudicated MW mandates on a TurboEdit-repaired satellite (SO-D1); GRAPHIC thresholds dominated by range rate at 30 s (SO-D6, G-D3); the IF reference rebase loses the geometry correction (SG-D2) and its reset-key exclusion leaks across chains (SG-D3, no golden effect found); `estimation.cycle_slip.mw` false is ignored by RT and baseline (SG-D7); QZSS L6 has no wavelength floor in the KalmanFilter (ID-SD3); the PPP time step at a GPS week rollover (P-L1); the Klobuchar knob changes no observation (X-D2); phase rows without an ATX frequency key get no wind-up (X-D6); the smoother's common subspace omits the clock and ISB rates (X-D7).

**Unverified (low confidence, passed to the steps that touch them):** a satellite recorded as reset with no state change (SO-D3), satellite-wide re-inflation after a partial reset (SO-D4), a silently skipped post-handover repair (SO-D5), an unlocked MW arc whose BIA frame flips emitting a pseudo-slip (SG-D1), the in-house bias rollback mixing frames (SG-D5), GLONASS slot 0 in the ambiguity seed (S4), the seed coefficient defaulting to GPS L1 (ID-SD4).

**Refuted (recorded so nobody re-chases them):** `arcZdMean` serving an unlagged count (the misattribution happens on no config); the NEQ export treating the ionosphere rate as a global parameter (no config exports NEQs or arms the rate); AR rebuilding the MW arc key in frequency order (the detector is the only writer and keys it the same way).

## 4. The model

Plain value types and tables; no base classes and no virtual dispatch for new structure (card NOM decision 1); dense matrices stay index-based.

1. **State families** (`filter/StateFamily.hpp`, step U1a): one row per dynamic family with its name, label prefix and RT snapshot tag, extended by step U6 with the initial-variance source, the Q rule, the gap and reset responses and the satellite-of-key rule, so every list of section 1.2 is a loop over the table; a run-start table "state families as armed" prints each family's presence, birth predicate, variance, Q and events.
2. **The Kalman filter** as a driver over named stages: the algebra kernels (sparse H products, Joseph, standardization) in one file; the residual QC kernel in another; the post-fit QC as ordered stages returning a typed verdict (reject a group, reject a row, inflate R, reset, accept), with the four pivot detectors as rows of one detector table; counters for every QC event and a "post-fit QC as armed" table.
3. **The measurement model** by concern: state births (one function per family, in today's order), the row (usability and the Jacobian), the variance, the DD build as stages, and the network-only surface in its own translation unit; later one observation equation whose term list serves the Jacobian, the raw residual and the seeds.
4. **The slip layer**: a typed slip event (satellite, signal or signal pair, cause, integer if known); the adjudication returns a verdict and the actions apply it; the detectors are rows of one declared table (kind, toggle, sigma knob, creator, response policy, logger, trace), which replaces the eight enumerations and `ISlipDetector`'s dispatch (decision 1, section 7); run-start and run-end slip tables. The MW, TurboEdit and IF detectors are cut into steps over parameter structs, with shared value types (Welford state, the cross-satellite noise pool, one signal-combination header).
5. **The pipeline**: `Application::run` as a short spine of named stages; the time step, the gap and the KF-reset fan-out written once for the three drivers; a run-end report table as on the network.
6. **Identity**: the user side reads `common/GnssIdentity.hpp` as the network does (constellation converters, observation type, signal pairs, frequencies, key makers, a typed BSSD pair).

## 5. Steps

Each step lands with the reader's page it affects (the code tour, this card). "Shared" names the files card NOM also edits; those steps are coordinated with the NOM session (a file list announced before the build).

### 5.1 Done on branch

| step | what | behaviour |
| --- | --- | --- |
| U0 | the gate, `scripts/uom_gate.py` (section 6) | none |
| U1a | the state families declared once: `StateFamily`, `kStateFamilies`, `StateManager::indicesOf`; seven lists become loops; `RestoredStateKind` deleted | identical |

### 5.2 Wave A (identical; parallel, disjoint files)

| step | what moves | files | size |
| --- | --- | --- | --- |
| U2 | KalmanFilter: dead code (the uncalled `predictImu`, the always-empty `epoch_prefix`, `epoch_tag`); bodies relocated by concern (driver, kernels, residual QC, post-fit QC, outlier response) by a generated character-exact cut and a verifier; the post-fit QC staged with a typed verdict; the four pivot detectors as one detector table; QC counters and the "post-fit QC as armed" table (new lines only) | `filter/Kalman*`, `CMakeLists.txt` | L |
| U3 | CycleSlipManager: dead code of the orchestration (uncalled registry and interface methods; the bridge kernel and jump-gap fitter have no production caller and are marked staged or deleted); `detectAndRepair` cut into its 17 sections over one working value; verdict separated from action; run-end slip counters | `slip/CycleSlipManager*`, `slip/SlipDetectorRegistry*`, `slip/ISlipDetector.hpp`, `slip/SoftLliBridge*` | L |
| U4 | MeasurementModel: dead code (the tracked empty `MeasurementModel.cpp.tmp`, `getNlWlReferenceBands`, the always-empty `epoch_prefix`, the unread `current_state`); the file split by concern (states, row, variance, DD stages, network surface) by a generated cut and a verifier | `measurement/MeasurementModel*` (shared: the network surface) | L |

### 5.3 Wave B and C (identical or explained)

| step | what | behaviour | shared |
| --- | --- | --- | --- |
| U5 | the MW, TurboEdit and IF detectors: dead code, the MW header split, bodies moved by concern, parameter structs for `processMwSample`, the giants cut into steps, the shared value types | identical; the shared noise pool is an explained move | IF detector's network caller |
| U6 | StateManager: `getProcessNoiseMatrix` cut by block; constructor split; the family table extended (section 4.1) and the remaining lists read it; one edit and one prune skeleton for the per-family methods; "state families as armed" | identical; log wording of the skeletons an explained move | `NetworkStationBlock` labels |
| U7 | the PPP pipeline: `Application::run` as a spine, setup builders, a run-end report table; `runBaseline` cut; the time step, gap and KF-reset fan-out written once | identical where the drivers agree; each disagreement is a section 5.4 item | |
| U8 | identity on the user side: converters, observation type, signal pairs, key makers, the BSSD pair type, onto `GnssIdentity` | identical where the strings agree; the frequency rule is UB8 | `GnssIdentity.hpp` additions |
| U9 | the onboarding code tour regenerated from the spine; the false statements of `02_code_tour.md` corrected | docs | |

### 5.4 Behaviour changes (each its own step, A/B graded, explained before landing)

UB1 the BSSD smoother crash (G-D1); UB2 one slant-ionosphere presence predicate for Q, PppState and the complete reset (S1); UB3 one ambiguity initial-std source (S3); UB4 the complete reset re-seeds every family from the family table (K-D3, S2); UB5 per-epoch QC memory cleared on every driver (K-D1; moves the golden consumers' numbers); UB6 DD innovations after the in-call state writes (DD-D1); UB7 the NL/WL pivot columns (DD-D3); UB8 one frequency rule (ID-SD1, ID-SD3, the GLONASS slot table; with card NOM); UB9 the real-time slip orchestration on the shared verdict and action core (SO-D1, SO-D2, P-D5); UB10 the ATX Q override in real time (P-D3); UB11 the PPP summary after AR (P-D4); UB12 the double ionosphere subtraction (DD-D2; with card NOM); UB13 the baseline backstop and slip key (K-D4, K-D5); the AR configuration builder belongs to card IRD.

## 6. Gates (frozen)

The bar is card NOM section 10's: identical output is expected of every organization step; an output that moves is explained (an iteration order, a corrected inconsistency) or the step does not land.

- **G-UEP, identical output, user arms.** `scripts/uom_gate.py`: reference and candidate executables built with the same Release flags in the same build tree, renamed copies, each arm in its own working directory with the data root pinned. Compare rule: the arm configs; every CSV under `logs/` byte for byte in row order; every module log and the whole stdout through `nom_td_gate.py`'s masks plus the KalmanFilter timing block; the exit code. The `quick` set (fourteen 600 s arms: PPP ionosphere-free static and kinematic-capable, PPP uncombined with AR, PPP-RTK BSSD with AR, receiver code bias, kinematic multi-GNSS, static, triple, TurboEdit with GRAPHIC and EWL, two smoother arms, baseline DD, RT SBF replay with and without TurboEdit) for every build; the `full` set (the map's 16-arm matrix, 30 min to 2 h) before a step lands. Measured: ten self-identity pairs IDENTICAL; one changed digit in `state_trace.csv` reads DIFFERENT.
- **G-UEP, network arms.** Every network station holds a `StateManager`, `PppModel` and `MeasurementModel`, and the closed-loop consumer runs the KalmanFilter, the MW detector and `StateInitializer`: both 2 h goldens through `scripts/prm_ab.py cmp --all-csv` for every step, NOM3's whole-run gate for a step that moves a printed line.
- **G-UR, relocation.** A character-exact move is proven by a verifier in the style of `scripts/nom_td_verify.py` (source frozen by `git show`, bodies equal apart from declared substitutions, every frozen code line carried once, the ordered list of log calls and format literals equal, the binding check of NOM3's G-R).
- **G-US, suite.** Debug `gnss_tests` passes (3,255 run, 24 skipped for fixture data, at U1a).
- **G-UREACH, coverage.** Each step names the branches it moves that no arm reaches (the IMU, the GRAPHIC/EWL user detectors outside one exerciser arm, post-processing TurboEdit, baseline AR, Baseline-Multi) and relies there on G-UR and the suite.
- **G-U$, cost.** Within 1 % on an idle box per arm pair (the KalmanFilter timing block gives per-phase microseconds); measured on a loaded box only as a guard.

## 7. Decisions owed by the author

1. **`ISlipDetector`.** The interface's virtual dispatch is used by one caller (`CycleSlipManager`), three of its methods are never dispatched, and the detector set is listed by hand in 8 places. Recommendation: replace it by a declared detector table in step U3 (card NOM's rule for new structure), keeping the detector classes as plain classes.
2. **Network-only code in user classes** (about 3,400 lines). Recommendation: banners now and a same-class translation-unit split per class in its step (a pure move, coordinated with NOM); the change of owner waits for card NOM step N6 (the network owns its local parameter set).
3. **The order of the behaviour changes.** Recommendation: UB1 (a crash) first; UB2 to UB4 on the family table after U6; UB5 next, since it moves the golden consumers and goes through the ledger like any change of the goldens' numbers.
4. **Dead knobs** (`logging.enable_if_slip_trace`, `model_corrections.enable_klobuchar_ionosphere`, the four non-causal bridge knobs). Recommendation: a loader warning "no effect" now, retirement through the ledger.
5. **BeiDou digit 1** (ID-SD1): one decision with card NOM's owed item.

## 8. Out of scope

The network files (card NOM); what the solve estimates (no science change in an organization step); knob names, snapshot formats, trace column names and label prefixes; the AR design (card IRD); merging `StateManager` with the network's model (card NOM principle 1).

<!-- Results are appended below this line under dated headings. -->

## 9. 2026-10-13: U0 and U1a built (branch `uom/work`)

**U1a, the state families declared once (`91b4df2`).** `include/filter/StateFamily.hpp` declares `StateFamily` (nine dynamic families) and `kStateFamilies` (name, label prefix, RT snapshot tag; the phase-colour family declared unserialized, as it is network-only). `StateManager::indicesOf(family)` returns a family's map. Seven hand-kept lists become loops over the table: the state size, the index shift on removal, the labels, the restore kind and its switch, the RT snapshot writer and reader, and the snapshot tag's documentation. `StateManager::RestoredStateKind` is deleted (one name per concept); `restoreDynamicState` refuses a family without a snapshot tag, which it did before by having no enumerator for it. The labels are written in table order instead of the old order; `put` writes by index, so only an aliased index (an invariant violation the snapshot's tiling check refuses) could tell the orders apart.

| gate | result |
| --- | --- |
| G-UEP user arms (quick, 14) | IDENTICAL on 14 of 14 against `gnss_processor_uomref.exe` (main `8326da3`, same build tree) |
| G-UEP network | IDENTICAL on both 2 h goldens (`prm_ab.py cmp --all-csv`: products, consumer trace, epoch status and every CSV); wall 839 against 832 s field, 522 against 532 s sim on a loaded box |
| G-US | 3,255 passed (3,251 + 4 new `StateFamilyTest`), 24 skipped, 0 failed |

**U0, the gate (`e231160`).** `scripts/uom_gate.py` makes, runs and compares the arm sets of section 6; three arms re-checked IDENTICAL through it, and a one-digit mutation of `state_trace.csv` reads DIFFERENT. The smoother arm on BSSD exits 0xC0000005 on both executables (G-D1, a defect on main).

## 10. 2026-10-13: wave A and UB1 built and integrated on `uom/work` (`ddf3bbb`)

Three workers, one per giant, each in its own worktree from `5154324`, each gated against its own same-tree reference build on every commit (16 user arms), with a relocation verifier proving every moved body against `git show 5154324` (a one-character mutation fails each verifier); the manager read every driver and merged the three branches without conflict.

| step | what a reader gets | lines before / after | gates (worker) |
| --- | --- | --- | --- |
| U2 KalmanFilter (`uom/u2-kalman`) | `KalmanFilter.cpp` split into five files by concern (driver, kernels, residual QC, post-fit QC, outlier response); `validatePostFitResiduals` a 33-line driver over stages S0 to S9 sharing `PostFitQcWork`; the four pivot detectors one algorithm over `kalman::kPivotDetectors`; the dead `predictImu` deleted; `KalmanQcCounters` (11 events) printed at run end | 1,370 to 33 (driver) | 14 of 14 identical at U2a to U2d; verifier 63 of 63; 11 new tests |
| U3 CycleSlipManager (`uom/u3-cycleslip`) | `detectAndRepair` a 32-line driver over S0 to S17 sharing `SlipEpochWork`, bodies in five files by concern; `adjudicateSatellite` returns a `SatelliteSlipVerdict` and writes no state, one action applies it; dead registry and manager methods deleted; `CycleSlipCounters` printed at run end | 1,208 to 32 (driver) | 14 of 14 identical at U3a to U3c; verifier 45 of 45 bodies, 983 frozen lines each carried once; 2 new tests |
| U4 MeasurementModel (`uom/u4-measurement`) | `MeasurementModel.cpp` split into six files (driver, states, row, variance, DD, network surface); `ensureRequiredStates` a 30-line driver over 13 members; `buildDdMeasurement` a 28-line driver over eleven stages with its three state writes named; the tracked empty `.tmp` file removed; the pre-pass documented as a table in the header | 3,100-line file to 415; 910 to 30; 696 to 28 | 16 of 16 identical (two reach arms added: NL/WL, baseline IF collapse); verifier 43 of 43, 1,954 frozen lines; 3 new tests |

**Integration (`ddf3bbb`).** The slip counters are printed by an explicit run-end call in `Application::run` beside the KalmanFilter's, not by the destructor. `scripts/uom_gate.py` gains the two reach arms and `cmp --allow-inserted` (a log may differ by inserted lines alone; every CSV byte for byte).

| gate | result |
| --- | --- |
| G-UEP user arms (16, `--allow-inserted`), integrated exe against `gnss_processor_uomref.exe` (main `8326da3`) | 15 of 16 identical, the logs differing only by the two inserted counter tables; `u_rtk_smooth` completes (exit 0) where main crashes (0xC0000005), step UB1 |
| G-UEP network, both 2 h goldens (`prm_ab.py cmp uomref uomwa --all-csv`) | IDENTICAL |
| G-US | 3,286 passed, 24 skipped, 0 failed |

**UB1, the BSSD smoother crash (`5b4c35b`).** The smoother trace wrote the receiver clock as `s.x[clock_idx]` with the `max()` sentinel under BSSD; the Release build read outside the vector and the run died, losing every buffered trace. The clock columns are now NaN when the state is absent, as the other traces write absent states. 13 of 14 arms identical; the BSSD smoother arm now completes and writes 20 smoothed epochs.

**Recorded, not changed (for later steps):** the KalmanFilter's and the slip layer's counters are not printed on the RT engine or the closed-loop consumer (their run end is outside the wave's files); `epoch_prefix` is always empty in the KalmanFilter and the MeasurementModel but threads through signatures callers use; the quick arms reach neither the cross-band nor the code pivot detectors, the culprit repair, the TurboEdit suppression, the in-place pivot repair, the forced handover nor the soft-LLI path (the verifiers carry those); `buildJacobianRow` (400 lines), `rowVariance` (212) and `ensureSatCodeBiasStates` (230) remain long and belong to the observation-equation step. Next: wave B (U5 the MW, TurboEdit and IF detectors; U6 StateManager; U7 the PPP pipeline).

## 11. 2026-10-14: five behaviour fixes built on separate branches (not merged; adoption is the author's)

Each fix is a branch from `f823a36`, gated by the same-tree comparison of section 6 against its own reference build, with unit tests that fail on the base where a small fixture allows, and a recommendation. None is merged into `uom/work`, so each can be adopted alone.

| step | branch, commit | defect | what moved, and why | measured effect | recommendation |
| --- | --- | --- | --- | --- | --- |
| UB3 | `uom/ub3-ambiguity-std` `c6906ae` | S3: births and most resets read a hard-coded 1,000 m, pivot reseeds and DD births read `filter.initial_std.ambiguity` | 11 of 16 quick arms identical (configured at 1,000 m); `u_rtk_kin`, `u_baseline`, `u_baseline_if` move from the first birth or reset (100 m configs); `u_sbf*` one log line | kinematic: at most 20.6 mm early, 3.4 mm at the end, sigma unchanged; baseline: at most 0.3 mm | adopt (card note; the goldens set 1,000 m and cannot move) |
| G9 | `uom/ub-log-stamp` `dc89269` | G-G9: a whole minute printed `:60.000` (the calendar from a Julian-day double) | every CSV identical on 16 arms; exactly the 3,466 `:60.000` stamps become the next minute's `:00.000` | log text only | adopt (network logs change on the same stamp lines; no CSV) |
| UB6 | `uom/ub6-dd-innovations` `179c4d8` | DD-D1: innovations from the caller's snapshot taken before the in-call handover, prune and births | 16 of 16 quick arms identical; the 2 h baseline arms (UA12, UA13) move at exactly the two balanced re-birth epochs the new counter names (262590, 262740) | the reference absorbed up to 39.7 m innovations into 15 newborn ambiguities and then raised a self-inflicted pivot-slip cascade (6 to 2 warnings, 4 to 0 handovers); position at most 6.0 mm, 4.2 mm at the end | adopt |
| G10 | `uom/ub-baseline-trace` `ff3ebd0` | G-G10: baseline wrote a header-only `state_trace.csv` and one warning per epoch | the four baseline arms gain their rows (tropo NaN; the differential ZWD in new `dzwd_m`, `dzwd_std_m` columns) and lose the warning; every other arm and every other CSV identical | observability only | adopt; keeping the two `dzwd` columns is the author's choice |
| UB7 | `uom/ub7-nlwl-pivot` `b33c1c1` | DD-D3: under BSSD the pivot handover, freeze and disturbance check ignored NL/WL keys while the transform zeroed them | one pivot-column resolver in `PivotManager`, shared with the transform; 15 of 16 quick arms identical, including the two per-band arms that run handovers, disturbed re-seeds and an orphan; `u_nlwl` moves from the first epoch (the pivot's NL freezes) | the shipped `_nlwl` configs move both ways (2 h better; 24 h median error 7.75 to 3.13 m, whole-day RMS 11.4 to 17.7 m) because they also carry defect S1 (zero ionosphere Q in PPP mode) and are metres off before and after; the same arm in PPP-RTK mode: 0.19 to 0.11 m and about five times faster convergence | adopt, graded again after UB2 |

Every fix keeps the network goldens unreachable except G9 (log stamps on every logger; no CSV moves). Suites: 3,289 to 3,295 passed on each branch, 24 skipped, 0 failed (one UB3 run hit the shared-temp-name collision below and passed on re-run).

**Found by the behaviour wave, not changed:**
- The NL and WL ambiguities are seeded in cycles but born with a metre standard deviation (`MeasurementModelStates.cpp` NL and WL births): 1,000 "cycles" is about 107 m of NL and 862 m of WL.
- `ApplicationBaseline` clears the DD builder's sticky pivots on any MW slip, not only a slip on the pivot, so the next re-rank moves the pivot with no S-transform and every DD ambiguity is pruned and re-born (the trigger behind UB6's two epochs); a candidate for UB13.
- The newly visible differential ZWD ends the 2 h CIT1-JPLM baseline at -0.644 m (sigma 1.9 mm), implausible for 8.7 km; not investigated.
- `tests/test_network_rx_extra_band_delay.cpp` writes fixed names in `%TEMP%`, which collide with a concurrent `gnss_tests` from another worktree.
- After UB7 the slip layer and the KalmanFilter's pivot reactions still build per-band keys; under NL/WL the MW full reset seeds 0 for the non-pivot SD ambiguities.
