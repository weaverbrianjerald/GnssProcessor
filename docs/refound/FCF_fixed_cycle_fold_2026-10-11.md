# FCF: fix and hold as a fold of the fixed integer cycles into a detached held solve that only the emission reads

**STATUS**

- **id:** FCF
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered 2026-10-11. The author asked on 2026-10-10 for "feeding the fixed integers back into the filter (fix and hold, with its slip and re-validation responses)". This card builds fix and hold in a detached form: the integers the producer AR search fixes (card ARL, on card AGT's cycle basis and lattice) are folded into a second solve of the shared parameters, beside the float solve of the same epoch, and only the emission reads it. The estimator, the carry, the search, the DD trace and post-fit QC stay float (section 2 gives the reason). Increments (section 6): FCF-0 this filing and the references; FCF-1 the detached kernel, no caller; FCF-2 the same-epoch probe and decision D1 (does a held solve reach the consumer through the phase-bias hold); FCF-3 the lattice ledger with one release seam and decision D2; FCF-4 Emit from the ledger, which waits for the author's ruling on the DO-NOT-BUILD record; FCF-5 close-out. All knobs default off and unpromoted. Continued in card [FCFB](FCFB_fixed_cycle_fold_continued_2026-10-11.md) from FCF-1 on (this file is near the 60 KB bar); FCF-1 LANDED there (no caller), its review amending FCF-2 (FCFB section 3); FCF-2 LANDED default off (FCFB section 4) and decision D1 FAILED both bars: M9 is binding, FCF-3 and FCF-4 stay unbuilt; review fixes in FCFB section 5 (Emit requires the slip fixes). FCF-3 built after D1 as a declared departure, measure-only, in card [FCFC](FCFC_fixed_cycle_ledger_2026-10-11.md): D2 FAILED; FCF-4 unbuilt.
- **source:** the author's request of 2026-10-10 (section 1); card [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) sections 10 to 13 (the block search, the slip ladder, the three slip fixes); cards [AGT](AGT_arc_graph_shared_tree_2026-10-10.md) and [AGTB](AGTB_arc_graph_continued_2026-10-10.md) (the cycle basis, the life stamp, the lattice observatory, and AGTB section 5: lattice consistency is no guard against a missed release); a design panel of 2026-10-10 (a completeness critique of the three tasks, three hold designs, two judges, one synthesis). The panel's working files are not kept: sections 2 to 8 of this card are the binding record of the design.
- **knobs / posture:** new, all default off and unpromoted, landing from FCF-2: `network.ambiguity_resolution.fixed_cycles.fold` (off, observe, emit), `.source` (epoch, lattice), `.emit_measurement_only` (false; section 2), `.sigma_cyc` 0.003 (0.01 until FCFB section 3), `.unit_alpha` 1e-6, `.gate_alpha` 1e-4, `.max_iter` 3, `.max_shift_m` 0.5, `.persist_ep` 10 (removed, FCFB section 5), `.trace`. Requires `cycle_basis` (card AGT), `ambiguity_vehicle: float_kf` and `phase_datum_held_basis`. Beds: `config/sim/arl/arl_{r2,r3c,n300,s1..s8,tw8,tw2}_producer.json` with the ARL and AGT knobs, in the postures P0 and P1 of section 6 (P1 adds the three slip fixes of ARL section 13); `config/golden/golden_{sim,field}_refound.json`
- **dates in the text:** 2026-10-10, 2026-10-11
- **hashes / run dirs:** main `df11cd3` (card AGT landed); reference exe `build/x64-Release/gnss_processor_fcfref.exe` (md5 `b277f363568856cc5af0d53858aae11b`); references `runs/_rap_fcf0_ref_{r3c,n300,r2,s2,f2}` (P0) and `runs/_rap_fcf0_ref_{r3c_sfx,n300_sfx,tw8,tw2,tw8n}` (P1, and the P0 S8 twin) (section 9)
- **gates (frozen at pre-registration):** section 6

**Outcome.** Question: can the integers the search fixes improve the emitted product without corrupting it, and can a wrong one be released in time? Decision D1 (card FCFB section 4) answered the first part no. The same-epoch probe folds only right integers (0 wrong of 26 623 folded unit-epochs at 60 stations and of 29 051 at 300), but on the golden wire, where the phase-bias hold freezes the phase-visible correction, the consumer moves 0.2 % (the M9 prediction, confirmed), and on the unheld wire the held product's within-arc phase error falls 6 to 7 % (bar 30 %) while the consumer is 17 and 41 % worse. Missed: the unit and block calibrations (the float's ionosphere-free variance is 1.4 to 1.9 times its formal value) and the seam cost. Standing: M9 binding; FCF-3 built under Observe on 2026-10-11 as a declared departure, and decision D2 failed (card FCFC); FCF-4 unbuilt; the kernel, the probe and the ledger stay as default-off instruments; FCF-5 closes the card.

## 1. The question and the author's request

On 2026-10-10 the author asked for "feeding the fixed integers back into the filter (fix and hold, with its slip and re-validation responses)".

Card ARL's search is log-only: on the uncombined float with a free slant ionosphere, the decorrelated block search fixes about half the L1/L2 pairs in the settled hour with no wrong fix on any rung (ARL sections 10 and 11), and the three slip fixes of ARL section 13 close the one wrong-fix class of the slip ladder. Card AGT gives the fixes stable identities (a sticky, L1/L2-consistent, full-coverage cycle basis), a per-band arc life stamp, and a measure-only lattice of fixed cycles. AGTB section 5 measured what a hold would meet: on the settled S8 twin without the slip fixes the search fixes the stale integer for 30 epochs and the lattice confirms it, so a hold's safety rests on its release seam, not on lattice consistency.

The question: can the fixed integers make the emitted product more precise without corrupting it, and can a wrong integer be released before it does damage?

## 2. The choice: a detached held solve, and the ruling requested

**What is built.** The request names feeding the integers back into the filter. This card does not write them into the estimator. Each epoch it folds the fixed integer cycles into a second solve of the shared parameters (clocks, phase biases, code biases), made from a copy of the float solve's own reduced normals, and only the emission reads the result. The estimator, the carried ambiguities, the search, the DD trace and post-fit QC see the float solution as today.

**Why, in plain words.**

1. **Nothing is lost at the epoch.** The ambiguities carry no process noise, so the float solution at epoch t already holds the whole history of the data. Conditioning it on the integers at t gives the fixed solution at t, apart from the cross-row terms that card CTU's exact update leaves out. Feeding the hold back into the filter would add only those terms.
2. **Feeding it back would let the hold confirm itself.** The carry harvest reads `reconstructStation`, so after one epoch the carried float would sit on the held integer at float variance. The re-validation test, the next search, the DD trace and post-fit RAIM would then all read values the hold produced (critique C2), the dense shared-border carry would keep the hold's information after a release (M1), and 24 call sites would read held values (M2, section 5). A wrong integer would outlive its own release.
3. **Detached, a wrong integer stays where it can be undone.** It reaches only the emitted product, only while it is held, and its release takes effect at once: no carried state has to be unwound.

The price: the carry, the search and `network_dd_fractional.csv` never tighten from the hold. An in-estimator hold would be a different card, for the author to request after D1 and D2 if wanted.

**The ruling requested.** The DO-NOT-BUILD record of producer AR lives in three places: the memory note `project_network_ar_producer_scope`, the status line of `docs/network_ambiguity_resolution_design.md`, and the producer AR loader warning ("the emitted product stays FLOAT", `GnssConfigLoadNetwork.cpp` 1298 to 1320 at `df11cd3`). This card's workflow does not edit any of them (the Emit variant below is an added branch: the record's text and its condition are untouched in every run without Emit). Requested of the author: a written ruling on that record for a held emitted product. Until it is quoted here:

- FCF-1 and FCF-3 land measure-only (no caller; fold off or observe), default off;
- FCF-2 lands Observe and one Emit value, `fold: emit` with `source: epoch`, for the D1 measurement arms on the sim fixtures only (the departure below);
- Emit with `source: lattice` (FCF-4, the feature) is refused by the loader and not built.

**Departure from the design (2026-10-11).** The design held that no Emit value lands before the ruling and that D1 runs after it. This card builds Emit with source epoch at FCF-2, before the ruling, because the workflow running the card instructed it: the D1 arms are local measurements on the sim fixtures, the knob is default off, nothing is promoted, and no golden, ledger or field configuration changes. The reason: D1 decides whether FCF-3 and FCF-4 are built at all, and its question (M9: does a held solve reach the consumer through the phase-bias hold) can be answered only on an emitted product, because the consumer reads the wire alone. Source epoch has no memory and no release machinery: it is a probe, not the product the record forbids. Pre-registered for FCF-2, each with a config test:

- the loader refuses `fold: emit` unless `source: epoch` and `fixed_cycles.emit_measurement_only: true` (a knob whose only effect is this acknowledgement), and refuses `fold: emit` with `source: lattice` until the ruling is quoted here;
- the record's warning keeps its text and its condition, byte for byte, in every run that does not arm Emit (card AGT's precedent: a clause named only when armed). Under Emit its first clause, that the emitted product stays FLOAT, would be false, so it is replaced by an Emit variant: the emitted product of this run is the held solve of a single-epoch search (a measurement arm, guarded only by block acceptance [amended: FCFB section 3]), not a product.

## 3. The design (binding)

**The held solve.** `setDdPriors` has one caller (`NetworkDdPriorBuild.cpp` 765), which returns early unless `carry_invariant` (line 187), and the fold's loader refuses `carry_invariant`. So `reconstructStation` applies no DD-prior correction and `sharedCovariance()` is the float covariance: the critique's guard 1 holds with nothing to capture. `NetworkEstimator::solveHeldDetached` copies the reduced normals (`solve()` already pins a copy, `NetworkEstimator.cpp` 402 to 439), adds the `foldReducedDdPriors` algebra (`NetworkNormalEquations.cpp` 902 to 1244) built from the cached station factors, applies the same datum pins (moved verbatim into `applyDatumPins`) and solves once.

**Blocks.** One block per connected station component, both bands and all constellations together; the kernel throws if two blocks share a station, so the block-Jacobi truncation of the Card-4 fold cannot occur. Each station assembles only the rows that touch it. A = Pz + B is sparse, Jacobi-equilibrated and factored by an AMD-ordered sparse LDLT behind a relative-pivot screen; Pz > 0 keeps it positive definite without `pd_safe`. [amended: FCFB section 3] The algebra stays in `NetworkNormalEquations`, its owner, in a new translation unit.

**The seam.** `networkFixedCycles` runs in SB_SOLPREP right after the gross-LOS Decide (`ApplicationNetwork.cpp` 2590 to 2593), with no timer slot of its own (the precedent written there). In order: (1) a loop epoch skipped since the last run releases everything (SeamGap); (2) the lattice step (AGT-6's `networkArLattice`, moved here at FCF-3): evidence releases, contradictions, the float re-validation, then offers behind a persistence gate; (3) the validated units are placed, the held solve runs, then the float-reproduction and shift guards; (4) acceptance; (5) edges and traces. The held vector is returned only under Emit.

**One release seam.** One cause table gives each cause its name, counter, PRM response row and event. The causes (section 4 maps them onto AGT-6's table): the life stamp changed (ledger generation or run start, a carry erase or birth, a drop, a re-level, `amb_flag_ep`, which also covers pinned anchors and T3/T4 flags); this epoch's free-epoch latch, every `free_cause` bit, per band (`carry_slipped_ambs` is never read: on the golden posture every detector write to it is behind `carry_invariant`, critique C1); the arc has no live column (a bridged gap releases at its first missing epoch, critique C5); the landed within-epoch replay culprits, exported from the solve glue before its aliases are retired (`NetworkSolveGlue.cpp` 2403); this epoch's gross-LOS declarations (`gl_act_declare`, `gl_act_station`, `gl_act_cold_refloat`); contradiction; re-validation; acceptance; a degenerate block; SeamGap. RAIM T3/T4 at epoch t act after the emission, so they release at t+1 through the stamp, and those releases are counted as late.

**Admission and re-validation.** A fix enters the lattice only after `persist_ep` identical consecutive fixes with no new life on its arcs; a contradicting fix drops the lattice rows behind the cycle (its support) at once. A fold unit is a current AGT-5 PartnerDd or BorrowedDd DD present in both bands whose two integers the lattice implies from rows entered before t (band-only cycles are never folded: their single-band test has about 3-sigma power). Its target zhat = lambda N - sum c x_lin is re-based every epoch. Every unit is re-validated every epoch against the search's own float: T = v' (Sigma_pair + Pz)^-1 v with v = y - N, tested as chi-square with 2 degrees of freedom at 1e-6 [amended: FCFB section 3]; its power grows with arc age. A failing unit drops its support, and an arc found in two failing units is eliminated.

**Acceptance (critique M8).** Per block, kappa = A^-1 (z_free - G' x_held - zhat) = S^-1 v with S = Var(float DD) + Pz, so the block statistic v' kappa comes with the held solve, tested as chi-square with K degrees of freedom at 1e-4 [amended: FCFB section 3]. On failure the worst unit is dropped and the block refolded, up to three times; then the block stays float for the epoch.

**M9 first.** `emit_pb_hold_phase_clock` freezes the phase-visible clock per generation, and a generation starts only at a first sighting or after a gap (`NetworkPbHold.cpp` 188 to 243): 74 generation starts in 6 h on the sim golden and 111 on the field (64 and 96 first sightings; `_rf0hv14_{sim,field}6`, counted by the design panel). A float consumer therefore sees a held solve only through the code-side clock and the generation starts. FCF-2 settles this with source epoch (no memory, nothing to release) and real emission arms under both wire postures; the ledger is built only on the pre-registered D1 result.

**Edges and persistence.** Onset and release events carry identity, integers and cause. A wrong-integer release (re-validation, contradiction, acceptance) marks its satellites through `ura_retro_sats`: a discontinuity, not a generation restart (no API restarts a PB-hold generation). Emit changes outputs only and is fingerprint-safe (`emit_clock_frame_hold` is the precedent), with the snapshot's estimator-side sections gated identical (FCF-4). The ledger is not snapshotted: Emit refuses a warm resume, and Observe resumes cold, counted.

**Graded on** the consumer position (SZZ000SIM, hourly 3D p50); the phase-visible product against truth (card PBH's Read 3: ionosphere-free level, de-commoned per epoch, de-meaned per arc, within-arc RMS); wrong folded unit-epochs (with the grader's wind-up branch); releases by cause, false releases and dimensions lost.

**Byte identity.** Off: structurally identical. Observe: products and every existing trace identical. The pin code motion: a same-build A/B. AGT-6's lattice trace moves under `trace_lattice` only, as a declared change (section 8). Emit: every estimator-side output identical.

## 4. The API on card AGT's landed code

The design was written before card AGT landed. Where it names an AGT API that landed differently, the landed one is used:

| design | landed at `df11cd3` | in FCF |
| --- | --- | --- |
| `ArDdKey`, a new alias in `NetworkRunTypes.hpp` | `ArDdIdentity`, the same tuple (`filter/NetworkArCycleBasis.hpp` 87) | used as is; no second alias |
| `ArEpochFixes` in `NetworkRunTypes.hpp`, `fixed` as (cycle, integer) pairs | in `filter/NetworkArCycleBasis.hpp` 249: `searched` (a bool: the basis was built), `fixed` (`ArFixedCycle`: cycle, value, `ArDdIdentity`), `arcs` (an `ArArcReading` per band-arc read) | the design's per-cycle float list cannot be named `searched`: it lands as `floats` (`SearchedCycle`), beside `columns` (`ArcColumn`), both in the same header (the AGT-5 and AGT-6 precedent; `NetworkRunTypes.hpp` untouched) |
| the search fills the hand-over only when the fold is armed | empty unless `trace_lattice` | FCF-2: `fixed` and `arcs` are filled under `trace_lattice` or an armed fold; `floats` and `columns` under an armed fold only |
| `ReleaseEvidence` from a stamp comparison in the seam | the search's own reading of each arc (`arcLifeStep`, with its echo rule) handed over in `ArEpochFixes::arcs`; the observatory keeps no stamp (AGT section 3, AGTB section 5 departure 1) | the life-stamp, free-latch and vanished evidence come from `arcs`; the seam compares no stamps. Replay culprits, gross-LOS declarations and the late-T3/T4 attribution stay FCF's |
| `fcyc::ReleaseCause`, ten causes | `ArLatticeRelease` {Vanished, FirstSeen, Freed, StampChange}, its names, counters and `network_ar_lattice.csv` columns | one table: FCF-3 extends `ArLatticeRelease` in place. LifeStamp is StampChange (with the `ArcLifeField` bits), FreeLatch is Freed (with the `free_cause` bits), Vanished is Vanished; FirstSeen keeps its row; ReplayCulprit, GrossLos, Contradiction, Revalidation, Acceptance, Degenerate and SeamGap are appended. The widened trace header is the declared move of section 8 |
| a skipped epoch: SeamGap releases everything | AGT-6 counts a skipped search epoch and releases nothing | SeamGap is new at FCF-3 (the replay culprits of a withheld epoch leave no stamp) |
| `FixStreak` reset on a stamp change | `ArBasisMemory::arcs` keeps an `ArcLifeTrack` (stamp, life start) per band-arc | a streak resets when any arc of its cycle reads a new life (FirstSeen, Freed or StampChange) |
| `CycleLattice::entered_ep`, `latticeOffer(L, c, value, epoch)`, `latticeImplied(L, c, &newest)`, `latticeDropSupport` | `CycleLattice` {rows, values, pivot_row}; `latticeOffer(L, c, value)`, `latticeImplied(L, c)`, `latticeEliminateArc(L, arc)` | FCF-3 amendments, with defaults so AGT's callers and its frozen oracles stay unchanged |
| `ArLatticeState` with stamps | `ArLatticeState` {lattice} (`ctx.carry.ar_lattice`) | one owner of the fixed integers; FCF's streaks and folded runs live in `FixedCycleState` (`ctx.carry.fixed_cycles`), with `FixedCycleCounters` in `filter/NetworkFixedCycles.hpp` (the `NetworkArLattice.hpp` precedent) |
| `kCauseGfJump` uncommitted | committed at `b5592b6`; `free_cause` is a `uint8_t` with all eight bits used | read through `free_cause` unchanged; a ninth cause needs a wider mask |
| ladder twins at epoch 151 (TOW 265530) | AGT's committed twins `arl_tw8` and `arl_tw2` at epoch 175 (TOW 266250) | the ladder uses epoch 175 (section 6, FCF-3) |
| card file dated 2026-10-10, exe `gnss_processor_fcf_ref.exe`, an INDEX line | | `FCF_fixed_cycle_fold_2026-10-11.md`, `gnss_processor_fcfref.exe`, the program card index `CARDS.md` |

The new API (full Doxygen when it lands; names in code say "fixed cycle", because "hold" already means the PB hold, the held basis and the gross-LOS hold):

```cpp
// GnssConfigTypes.hpp, NetworkArConfig; JSON network.ambiguity_resolution.fixed_cycles.*
enum class FixedCycleFold   : std::uint8_t { Off, Observe, Emit };   // Off is the default
enum class FixedCycleSource : std::uint8_t { Unset, Epoch, Lattice }; // Unset refused unless Off
FixedCycleFold fixed_cycle_fold; FixedCycleSource fixed_cycle_source;
double fixed_cycle_sigma_cyc{0.01}, fixed_cycle_unit_alpha{1e-6}, fixed_cycle_gate_alpha{1e-4};
int fixed_cycle_max_iter{3}; double fixed_cycle_max_shift_m{0.5}; int fixed_cycle_persist_ep{10};
bool fixed_cycle_trace{false}; bool fixed_cycle_emit_measurement_only{false};
// Interlocks: requires cycle_basis, float_kf and phase_datum_held_basis; refuses carry_invariant,
// phase_datum_ensemble, phase_datum_eq34, cycle_slip.turbo_edit, cycle_slip.relevel_delta_bridge,
// sd_ewl.fix_enable; Emit refuses emit_pb_hold_smoothed and a warm resume; until the ruling, Emit
// requires source Epoch and emit_measurement_only, and Emit with source Lattice is refused; a
// warning names gross_los_screen without gross_los_emit_exclude (gross-LOS releases would be late).

// NetworkNormalEquations (new TU NetworkNormalEquationsDetached.cpp);
// SparseLdlt = Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::AMDOrdering<int>>
struct CycleRow { std::vector<DdPriorBlock::Term> terms; double zhat_m; double pz_m2; };
struct DetachedFoldBlock { Eigen::MatrixXd G; Eigen::VectorXd z_free, zhat, scale;
    std::shared_ptr<const SparseLdlt> factor; bool degenerate; double min_pivot_rel; long long factor_nnz; };
struct DetachedFold { std::vector<DetachedFoldBlock> blocks; Eigen::MatrixXd dN; Eigen::VectorXd du; };
DetachedFold foldCycleRowsDetached(const std::vector<std::vector<CycleRow>>& blocks,
                                   double pivot_floor_rel = 1e-12) const;   // throws on a shared station
static Eigen::VectorXd solveFoldBlock(const DetachedFoldBlock& b, const Eigen::VectorXd& r);

// NetworkEstimator
static void applyDatumPins(Eigen::MatrixXd& N, Eigen::VectorXd& u, const Datum& datum);  // moved from solve()
struct HeldSolve { bool ok; Eigen::VectorXd x_s; std::vector<Eigen::VectorXd> v, kappa;
    std::vector<double> chi2; std::vector<char> degenerate; double float_repro_rel, max_shift_m; long long factor_nnz; };
HeldSolve solveHeldDetached(const Datum& datum, const Eigen::VectorXd& x_float,
                            const std::vector<std::vector<NetworkNormalEquations::CycleRow>>& blocks) const;

// NetworkArCycleBasis.hpp, beside ArEpochFixes
struct ArcColumn { std::size_t est_station; Eigen::Index local_col; double x_lin_m, lambda_m; };
struct SearchedCycle { ArDdIdentity key; arcgraph::Cycle cycle; double y_cyc, var_cyc2;
    int twin; double cov_twin_cyc2; bool fixed; long long integer; };
// ArEpochFixes gains: std::vector<SearchedCycle> floats; std::map<ArcId, ArcColumn> columns;

// NetworkSolveGlue.hpp: NetworkSolveGlueResult gains WithinEpochCulprits we_landed
//   (sats, arcs, stations), copied from the landed drop sets before the aliases are retired.

// NetworkFixedCycles.hpp (new seam), namespace fcyc: gatherEvidence, applyReleases (THE release
//   function), Unit, impliedUnits, epochUnits, scoreUnits, localizeFailures, FoldPlan, placeUnits,
//   chi2Quantile; FixedCycleState, FixedCycleCounters.
struct FixedCycleEmission { bool emit{false}; Eigen::VectorXd x_held; };
FixedCycleEmission networkFixedCycles(NetworkRunContext& ctx, const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log, NetworkRunTraces& traces, const SyncedEpoch& se,
    const NetworkEstimator& est, const Eigen::VectorXd& x_s, const NetworkSbasis::Plan& plan,
    const ArEpochFixes& fixes, const WithinEpochCulprits& we, const std::vector<NetStation>& stations);
int networkFixedCyclesRunEnd(const NetworkRunEndView& v);   // one row of kNetworkRunEndReports

// Others: NetworkResponses kind FixedCycle with one ResponseSite row per release cause;
// NetworkInject kind "fixed_cycle_plant" (station, sat, signal, delta_cycles, tow), an unfired
// plant fails the run at run end.

// Driver (SB_SOLPREP), the only two readers of the held vector:
//   if (ctx.knobs.gl_emit_exclude) networkGrossLosScreen(..., GrossLosPhase::Decide);
//   const FixedCycleEmission fce = networkFixedCycles(...);
//   const Eigen::VectorXd& x_emit = fce.emit ? fce.x_held : x_s;
//   networkSolPrep(..., est, x_emit, ...);  networkPbHoldCloneUpdate(..., x_s, ...);
//   networkEmitStore(..., est, x_emit, ...);  every later seam keeps x_s
```

## 5. Readers: what stays float and what reads the held solve

**Every `reconstructStation` call is float** (24 call sites at `df11cd3`; the wrapper `NetworkEstimator.cpp` 479 forwards to the kernel). The held solve never enters the estimator, so each reads the float x_s it is passed:

| # | site | function | bracket | reads |
| --- | --- | --- | --- | --- |
| 1 | `app/NetworkRunContext.cpp` 4715 | `networkHarvestLocalCarries` | SB_LEVELING | the continuous datum handover's ambiguities |
| 2 | 5029 | same | SB_LEVELING | the station-block carry harvest (ambiguities, in-block iono, receiver clock and code bias, colour, ZWD) |
| 3 | 6037 | same | SB_LEVELING | the in-block slant-ionosphere carry |
| 4 | 6152 | same | SB_LEVELING | the receiver-clock carry |
| 5 | 6324 | same | SB_LEVELING | the receiver code bias in block |
| 6 | 6409 | same | SB_LEVELING | the ZWD carry |
| 7 | 6602 | same | SB_LEVELING | the receiver phase-bias carry |
| 8 | 6645 | same | SB_LEVELING | the diagonal slant-ionosphere carry |
| 9 | 6796 | same | SB_LEVELING | the receiver code-bias carry |
| 10 | `filter/NetworkClockLevel.cpp` 476 | `clockLevelBuildPairs` | SB_STASH_STATUS | the clock-level pairs (card CLV) |
| 11 | `NetworkDdFrac.cpp` 136 | `networkDdFrac` | SB_DDFRAC | `network_dd_fractional.csv` |
| 12 | `NetworkFrameDiag.cpp` 250 | `networkPostfitSigma0` | SB_FRAMEDIAG | post-fit sigma0 |
| 13 | `NetworkGrossLos.cpp` 334 | `networkGrossLosScreen` | SB_SOLPREP (Decide), SB_RAIM (Act) | the gross-LOS residual walk |
| 14 | `NetworkInstallCarry.cpp` 405 | `networkInstallCarry` | SB_INSTALL | the Card-4 posterior (carry_invariant only, refused by the fold) |
| 15 to 17 | `NetworkIonoDiag.cpp` 74, 132, 204 | `networkIonoDiag` | SB_IONODIAG | the iono, ZWD and anchor traces |
| 18 | `NetworkProducerAr.cpp` 756 | `networkProducerAr` | SB_NETAR | the search's floats |
| 19 | `NetworkRaim.cpp` 231 | `networkPostFitRaim` | SB_RAIM | post-fit RAIM |
| 20 | `NetworkRedundancyCensus.cpp` 472 | `networkRedundancyCensus` | SB_FRAMEDIAG | the redundancy census |
| 21 | `NetworkShDevVtec.cpp` 122 | `networkShDevVtec` | SB_SHDEV_VTEC | the SH deviation and VTEC trace |
| 22 | `NetworkShHealth.cpp` 209 | `networkShHealth` | SB_T1A | the SH health probe |
| 23 | `NetworkSolveGlue.cpp` 1621 | `networkSolveGlue` | SB_SOLVEGLUE | the within-epoch replay's sigma0 |
| 24 | `NetworkStationBlock.cpp` 295 | `networkStationBlock` | SB_FRAMEDIAG | the station-block statistics |

**The only readers of the held vector** (under Emit; under Off and Observe they read x_s):

| reader | what it reads |
| --- | --- |
| `networkSolPrep`, its x argument | one read: `toSolutionEpoch` (`NetworkSolPrep.cpp` 157), which builds the epoch's solution `sol` |
| `networkEmitStore`, its x argument | the SH VTEC coefficients of the closed-loop ionosphere supply (`NetworkEmitStore.cpp` 711, 724), and `sol` |

**Readers of `sol`** (they follow the held solution under Emit, because they read what the wire receives), with their write sets:

| reader | reads | writes |
| --- | --- | --- |
| SolPrep's clock-runaway quarantine (`NetworkSolPrep.cpp` 319 to 378) | `sol` clocks c0 (held under Emit) and C1 (stamped from the float clock model, 245 to 291) | drops a constellation from `sol`; `n_runaway_epochs` (a snapshot counter); an RT0-5 event; the `clk_runaway` flight-recorder edge (369), which sets `rhs_trip_pending` (`NetworkRhsAttrib.cpp` 850 to 865), and a pending trip forces the RHS attribution dump and a full redundancy-census sweep later in the same epoch (`NetworkRedundancyCensus.cpp` 265 to 276): measure-only traces, no estimator state |
| EmitStore's PB hold (`NetworkPbHold.cpp` 106 to 157, 477) | `sol` clocks (`pb_hold_prev_clock_m`, `pb_hold_frz_prev_clk`, `pb_hold_frz_M`) | PB-hold generations, counters and PB_HOLD events |
| EmitStore's emit filters, clock re-base and discontinuity tests | `sol` | the emitter, filter posteriors, the re-base frame, disc counters and events |
| `networkFastClockHarvest`; `networkSignalRosterAudit` (`SignalRoster.cpp` 1715 to 1726); `networkStashStatus` | the emitted `sol` | the fast product (a delta in the emitted frame); wire presence counters; the count of landed retro marks in `network_epoch_status.csv` |

`clock_runaway_check`, `rhs_attrib_on_trip` and `redundancy_census` are armed on `golden_sim_refound` and every ARL producer, so the first row is live. It stays on the emitted `sol` under Emit (it is emission QC: it withholds what would be emitted), and its trace path is declared in FCF-4's split gate. Its magnitude test (1 000 m) can change verdict only for a constellation median within the 0.5 m shift bound of the ceiling, and its rate test (10 m/s) reads the float C1.

**Float, every other x_s reader after the seam**, in driver order: `networkPbHoldCloneUpdate`, `networkIonoDiag`, `networkDdFrac`, `networkShDevVtec`, `networkShHealth`, `networkIonoDwRelease`, `networkLeveling`, `networkPostFitRaim`, `networkGrossLosScreen` (Act), `networkClockQc`, `networkPhaseVisibleMonitor`, `networkPostfitSigma0`, `networkStationBlock`, `networkRedundancyCensus`, `networkRhsAttribution`, `networkStashStatus`, `networkPbHoldCloneHarvest`, `networkClockLevelRebase`, `networkClockLevelMonitor`.

**Every covariance stays float:** `sharedCovariance()` in SolPrep (`NetworkSolPrep.cpp` 553, 593, 634, 1366, 1442: URA, phase-bias, combined and code-bias sigmas) and EmitStore (`NetworkEmitStore.cpp` 749). A wrong but accepted integer is never emitted with tightened integrity metadata.

Emission-side state that the solve reads back (the last pivot, the emitted anchor) is identities, not values; FCF-4's split gate (section 6) is the proof, and any difference it finds names an emission-to-solve path that moves to the float view before Emit lands.

## 6. Gates (frozen) and the decision rules

**Common rules.** Every data run is Release, from an exe copied to `build/x64-Release/gnss_processor_<increment>.exe`; the reference is `gnss_processor_fcfref.exe`. Arms via `scripts/rapid_arm.py` on the ARL beds. "Identical" means `scripts/nom_td_gate.py` RESULT IDENTICAL, with no hand-applied mask (the tool masks card AGT's `p50 / max` cost pair since 2026-10-11, section 9). A timing bar is read on the seam's own timer.

**Postures (pinned at filing, so no gate's posture is chosen after its results):**

- **P0, the reference posture**: the FCF-0 references' `--set` lines (rungs: `start_offset_hours` 0, `decorrelated_partial`, `tree_basis`, `block_search`, `cycle_basis`, `trace_lattice`; 300 stations: `start_offset_hours` 0, `cycle_basis`, `trace_lattice`, the rest armed in its file; the goldens as filed). FCF-1's A/B and FCF-2's structural Off check run in P0, against `_rap_fcf0_ref_*`.
- **P1, the hold posture**: P0 plus the three slip fixes of ARL section 13 (`network.cycle_slip.gf_arc_jump` with `gf_arc_jump_k` 6, `network.cycle_slip.if_phase_td_reseed_on_flag`, `network.ambiguity_resolution.suspect_epochs` 10), which that section built "before any hold". Every other arm runs in P1: FCF-2's Off (also), Observe and D1 arms; every FCF-3 arm (rung 3c, 300 stations, the ladder, the twins, G1, G1n, W1, R1, the planted legs, the field census); every FCF-4 arm. P1 references: `_rap_fcf0_ref_{r3c_sfx,n300_sfx,tw8,tw2}`.
- **One exception**: the blind-latency control of FCF-3, the S8 twin in P0 (`_rap_fcf0_ref_tw8n`, AGTB's `_tw8n` posture), graded under D2(b).

**FCF-0 (this filing).** The references reproduce AGT-6's stored `network_producer_ar.csv`, `network_ar_lattice.csv` and `products.txt` byte for byte; if HEAD moved past AGT-6, the new references become the baseline and the card says so; `docs_index_check.py` passes; `make_golden_config.py --verify` unchanged; the ruling request is written in the card.

**FCF-1: the detached kernel and the held solve, no caller.** Files (under `include/` and `src/core/`): `filter/NetworkNormalEquations.hpp`, `filter/NetworkNormalEquationsDetached.cpp` (new), `filter/NetworkEstimator` (header and source), `CMakeLists.txt`, `tests/test_network_detached_fold.cpp` (suite NetworkDetachedFold). Per station only the K_r rows touching it are formed (Y = N_rr^-1 C_r' from the cached factor; B as triplets; G[:, rows] += N_sr Y; z_free += C_r' N_rr^-1 u_r); dN and du summed over non-degenerate blocks. `solveHeldDetached` copies the reduced normals, pins the unfolded copy and computes `float_repro_rel` from x_float, adds dN and du, pins, solves once without a covariance, then forms v, kappa, chi2 and max_shift per block. `foldReducedDdPriors` is untouched and serves as the oracle. Gate:
- oracle: on 500 random arrowheads (2 to 12 stations, 20 to 60 shared columns, random pins, rows on pinned columns, 1 to 3 blocks over disjoint station sets) x_held and the posterior cycles zhat + Pz kappa equal `setDdPriors` (dense, diagonal Pz) + `solve` + `ddPriorPosteriorMean` to 1e-9 relative;
- the estimator untouched: `sharedCovariance`, `reducedSharedNormals` and `reconstructStation` bitwise unchanged by the call; two calls bitwise identical;
- identity: kappa equals S^-1 v against a dense S = Pz + B + G_f' Sigma_ff G_f to 1e-9;
- planted wrong integer: with the correct zhat, chi2/K over 1 000 draws has a Monte Carlo mean within 3 % of 1; a +1 L1 cycle on a pair whose wide-lane and ionosphere-free directions have sigma 0.06 and 0.04 cycle exceeds chi2_K(1 - 1e-4) in at least 99 % of 1 000 trials;
- guards: two blocks sharing a station throw; a dependent row makes its block degenerate with zero contribution to dN; a perturbed reduced system gives `float_repro_rel` above 1e-8; empty input returns x_float exactly;
- the full Debug suite: the previous count plus the new cases, no new failure;
- same-build Release A/B of the pin code motion, identical: 2 h `golden_sim_refound`, 2 h `golden_field_refound` and rung 3c against `runs/_rap_fcf0_ref_{s2,f2,r3c}`.

**FCF-2: the same-epoch probe, Observe and the measurement Emit arms, decision D1 (M9).** The knob block of section 4 with its interlocks; the source must be named whenever the fold is not off, and `lattice` is refused until FCF-3. The search fills `floats` (y, variance, twin covariance from its block readback) and `columns` (estimator station, local column, x_lin, lambda) only when the fold is armed. The seam under source epoch: `epochUnits` (this epoch's determined pairs, both bands); `scoreUnits` (the empirical null on correct fixes); `placeUnits` (one block per station component); `solveHeldDetached` (a `float_repro_rel` above 1e-8, a shift above 0.5 m or a throw emits the float, counted); acceptance at 1e-4 with up to three drop-and-refold passes. The driver passes `x_emit` to `networkSolPrep` and `networkEmitStore` only. Traces `network_fixed_cycles.csv` (per unit-epoch: identity, N1, N2, y, v, T, block chi2/K, action) and `network_fixed_cycle_shift.csv` (held minus float per clock, phase-bias and code-bias column, and the phase-visible c - p per satellite and signal); a run-end report row; the contributions table untouched (the detached fold is not a contribution to the solve). `scripts/simnet_fixed_cycle_grade.py` imports the truth join and wind-up branch of `simnet_producer_ar_grade.py`; card PBH's Read 3 lens is applied to float and held products. Files (under `include/` and `src/core/`, header and source where both exist): `config/GnssConfigTypes.hpp`, `config/GnssConfigLoadNetwork.cpp`, `filter/NetworkArCycleBasis.hpp`, `app/NetworkRunContext.hpp`, `filter/NetworkProducerAr`, `filter/NetworkFixedCycles` (new), `app/ApplicationNetwork.cpp`, `logging/NetworkRunTraces`, `filter/NetworkRunEnd`, `scripts/simnet_fixed_cycle_grade.py`, `tests/test_network_fixed_cycles.cpp`, `tests/test_fixed_cycle_config.cpp`, `CMakeLists.txt`. Gate:
- Off: `_rap_fcf2_{r3c,n300}_off` and 2 h `golden_sim_refound` (P0) identical to `_rap_fcf0_ref_{r3c,n300,s2}`, and `_rap_fcf2_{r3c,n300}_off_sfx` (P1) to `_rap_fcf0_ref_{r3c,n300}_sfx` (structural);
- loader (config tests): the section 2 refusals (Emit without `emit_measurement_only`, Emit with `source: lattice`, `source: lattice` under Observe until FCF-3); the record's warning byte for byte with the fold off and under Observe, the Emit variant under Emit;
- Observe (epoch, P1), measure-only: products and every pre-existing CSV identical to Off (P1); `held_solves` equals the epochs with a determined pair, or the run end warns (invariant 4); `float_mismatch`, `kernel_errors` and degenerate blocks all 0; correct-fix units above the 1e-6 bar at most 1e-5 of unit-epochs; settled-hour chi2/K p50 at most 1, p95 reported [amended: FCFB section 3]; wrong folded unit-epochs 0 on rung 3c and at 300 stations; seam p95 at most 30 ms at 60 stations and 250 ms at 300.

**Decision D1** (measurement arms before the ruling, section 2's departure; P1). On rung 3c and at 300 stations, four arms each: A, the bed's golden posture with Emit; D, the golden posture with Observe; B, `emit_pb_hold` and `emit_pb_hold_phase_clock` off with Emit; C, B's posture with Observe. Graded on the SZZ000SIM consumer (hourly 3D p50) and on Read 3's within-arc RMS of the phase-visible correction and of the clock against truth. FCF-3 is built only if both hold:
- (i) the held RMS is at most 0.7 times the float's in the settled hour on both beds, read on B against C, where no phase-bias hold freezes the wire, so the two emitted products are the held and the float solve themselves;
- (ii) A against D, or B against C, improves the settled-hour consumer by at least 10 %, with no hour more than 10 % worse.

If only B against C passes, FCF-4's wire posture is the author's decision, recorded before FCF-4. If (ii) fails on both postures, M9 is recorded as binding, FCF-3 and FCF-4 stay unbuilt, the kernel and the probe remain as instruments, and the Outcome carries the numbers.

**The M9 prediction (pre-registered).** A against D moves the settled-hour consumer by less than 10 % on both beds (the golden wire is consumer-null), because the phase-bias hold freezes the phase-visible clock per generation and generations start only at first sightings and gaps (section 9 counts 2 such starts in the second hour of the 2 h sim references); B against C is the pair that can pass.

**FCF-3: the lattice ledger, the one release seam, the float re-validation and the slip ladder (Observe, source lattice), decision D2.** AGT-6's `networkArLattice` becomes the lattice step of `networkFixedCycles` and its SB_NETAR call is removed (declared, section 8). In order: SeamGap; the evidence releases (life stamp, free latch with every `free_cause` bit per band including `kCauseGfJump`, vanished, replay culprits, gross-LOS); contradictions (support dropped at once); the implied units from rows entered before t, scored at 1e-6, failures localized and released; offers after a streak of `persist_ep` identical fixes with no new life (NotPrimitive refused and counted; an added row folds from the next epoch). Acceptance drops go through the same release function. `NetworkSolveGlueResult::we_landed` is filled before `NetworkSolveGlue.cpp` 2403; the dead by-value alias (121 to 122, 1578 to 1579) is left alone, since fixing it would move `carry_invariant` lineages. One PRM response row per cause under the new kind FixedCycle; FIXED_CYCLE events (onset, release with its cause and bits, late T3/T4); the `fixed_cycle_plant` inject kind. Ladder fixtures (simulator-scripted slips only, so `truth_slips.csv` is complete): settled twins S1t to S8t at epoch 175 (TOW 266250), where S8t and S2t are card AGT's committed `arl_tw8` and `arl_tw2` (a target no longer tracked or no longer in its role at that epoch is replaced by an arc of the same role, named); G1, a 210 s windowed strip of S43000SIM G14 ending at epoch 175 with a scripted (2, 2) slip at the return, and G1n, the same strip with no slip; W1, a 400 s strip for the wind-up restart; R1, 20 scripted (1, 1) slips at epoch 175 on non-reference, non-anchor arcs at 20 stations. Files (under `include/` and `src/core/`, header and source where both exist): `filter/NetworkFixedCycles`, `filter/NetworkArLattice`, `filter/NetworkArcGraph`, `filter/NetworkSolveGlue`, `app/ApplicationNetwork.cpp`, `filter/NetworkResponses`, `filter/NetworkInject`, `app/NetworkRunContext.hpp`, `config/GnssConfigLoadNetwork.cpp`, the ladder configs `config/sim/arl/arl_{s1t,s3t,s3bt,s4t,s4bt,s5t,s6at,s6bt,s7t,g1,g1n,w1,r1rate}_2h.json` with their producer twins, `scripts/simnet_fixed_cycle_grade.py`, `tests/test_network_fixed_cycle_release.cpp` (suite NetworkFixedCycleRelease), `tests/test_network_arc_graph.cpp`. Gate:
- identity: Off and Observe-epoch unchanged from FCF-2; Observe-lattice products and pre-existing CSVs identical to Off, except `network_ar_lattice.csv` (declared);
- unit tests: each cause releases exactly its arcs with its detail bits; a pinned anchor is released through `amb_flag_ep` alone; a wind-up bit releases only the freed band; SeamGap releases everything; two failing units sharing an arc eliminate that arc, an isolated failing unit drops only its support; a new life resets the streak; a contradiction drops the support; a planted wrong lattice integer on a converging synthetic float trips at the epoch where the analytic statistic crosses the bar, with at most 2 false releases in 10 000 null epochs; the PRM table order test;
- rung 3c and 300 stations: 0 wrong folded unit-epochs; releases on arcs with no truth slip at most 2 per 2 h at 60 stations and 5 at 300; folded units p50 at least 90 % of the search's fixed pairs p50; reported: coverage by hour (folded units, lattice dimensions, basis dimensions), releases and dimensions lost by cause, the late releases; seam p95 at most 35 and 260 ms; a `persist_ep` 1 control arm on rung 3c;
- the ladder (P1; S1 to S8 at epoch 31, S1t to S8t, G1, G1n, W1, R1): detected classes release every folded unit through the slipped band-arc at the slip epoch, by the detector's cause, with 0 wrong folded epochs after. Detected classes: every slip a P1 detector flags at its epoch; named in advance, S8t (expected cause Freed with `kCauseGfJump`, the GF trigger alone), S2t (IF-TD and `kCauseGfJump`; both measured on `_rap_agt6_tw8` and `_tw2`, AGTB section 5) and S7t's second repetition (the re-seeded IF-TD, `post_flag`, and the GF trigger, ARL section 13). On the epoch-31 rungs no folded unit runs through a slipped or freed arc (the lattice holds 20 rows at TOW 261930 and the first fix through S43000SIM/G14 comes at TOW 265020, AGTB section 5), so only "0 wrong folded unit-epochs after" is tested there; the release half is tested on the twins. S3b's false release is counted and re-folds with the same integer after `persist_ep`; the S6b and S6bt handover releases are attributed; G1 releases at the gap's first epoch (Vanished), W1 on the wind-up bit per band, and G1n reports the dimensions lost. The blind classes: the S4b, S4bt and R1 events the P1 detectors miss in the run (the GF trigger at k 6 detects 90.1 % of (1, 1) slips at 15 to 20 degrees, ARL section 13), and the P0 S8 twin control (no GF trigger: AGTB measured the stale 91 / -12 fixed and confirmed for 30 epochs); each reports by cause the epochs to release, the wrong folded unit-epochs, and the held-wrong epochs beside the float's own error;
- planted integer (`fixed_cycle_plant` +1 L1 on a settled unit at TOW 266250, on rung 3c and at 300 stations): the plant fires, is released by Revalidation within 2 epochs, and costs at most 2 collateral dimensions;
- field census: 2 h `golden_field_refound` with the search, `cycle_basis` and Observe-lattice; truth-free, reported: units, releases by cause per hour, chi2/K quantiles, unit exceedance (at most 10 times nominal), nonzero counters.

**Decision D2.** FCF-4 is built only if all four hold: (a) 0 wrong folded unit-epochs on rung 3c, at 300 stations and on every detected rung; (b) every blind-rung wrong unit (the missed P1 events and the P0 S8 twin control) is released within 2 epochs of the first epoch at which its float pair statistic, graded against truth, exceeds the bar, and a class that misses this is recorded as field-blocking for Emit (invariant 3); a miss on the P0 control records the GF trigger as a field-blocking prerequisite of any Emit; (c) the planted bar is met; (d) the clean-arc release bar is met. FCF-4 also needs the author's ruling (section 2).

**FCF-4: Emit with the lattice source, and both wire edges.** In the posture D1 selected. Re-validation, contradiction and acceptance releases put their satellites in `ura_retro_sats` (the existing per-satellite discontinuity, drained by `networkSolPrep` in the same epoch); slip and life-stamp releases do not mark (the float responds itself, and the emitter's value-jump tests screen the step). After `networkEmitStore` every PB_HOLD generation start whose key a folded unit touches is recorded (`gen_under_fold`); a later release of such a unit counts `frozen_after_release`. Onset and offset events carry the identity, the integers, and the emitted clock and phase-visible step of each touched satellite. The loader lifts its refusal of Emit with source lattice only once the ruling is quoted, and the Emit variant of the warning (section 2) names the source; Emit stays fingerprint-safe; a warm resume stays refused. Gate:
- identity: Off and Observe unchanged from FCF-3;
- the split is proven: Emit against Observe, same build, on rung 3c, 300 stations, the 2 h golden sim with the search, S1t, S6bt and S8t: `network_amb_state`, `network_producer_ar`, `network_dd_fractional`, `network_ar_lattice` and `network_fixed_cycles`, the RAIM, gross-LOS and prefit traces and the solve columns of `network_epoch_status` identical; the census and RHS-attribution traces identical except on epochs whose `clk_runaway` edge differs between the arms (section 5; counted, expected 0); the snapshot, on rung 3c with `network.state_snapshot_path` set, `state_snapshot_interval_s` 3600 and `state_snapshot_keep_history` (`_rap_fcf4_snap_{emit,observe}`), compared section by section with `wall_utc` masked: every section identical except the declared emission outputs, `emitter_state` (the last emitted phase and code biases, `NetworkProductEmitter.cpp` 740 to 746), `ura_retro_sats` (an FCF-4 mark still pending for a satellite absent from `sol`) and `n_runaway_epochs` (only with a differing `clk_runaway` edge); only products, emission traces and consumer traces may differ, and any other difference names an emission-to-solve path that moves to the float view before landing;
- consumer: paired Emit against Observe, no hour more than 5 % worse; the settled-hour gain reported against the FCF-2 probe, and a gain below half the probe's is a missed bar in the Outcome;
- product: sim-truth within-arc RMS by hour;
- edges: onset and offset counts equal the folded-unit transitions; step distributions reported against the clock disc test (1.5 m) and the PB jump threshold; `marks`, `gen_under_fold` and `frozen_after_release` reported, and a nonzero `frozen_after_release` makes a generation-restart response a pre-registered follow-up;
- planted leg with Emit: the wire's truth error returns to the control's within 1 epoch of the release;
- records: the warm-resume refusal has a config test; `make_golden_config.py --verify` unchanged (no ledger row).

**FCF-5: close-out.** Results appended here, STATUS and Outcome revised with the missed bars, the CARDS line closed, dated pointer sections appended to cards ARL and AGTB; per the author's ruling, a dated annotation (not a rewrite) of the status line of `docs/network_ambiguity_resolution_design.md`, and the memory note update owed to the author; CLAUDE.md's network decision tree gains one line (fixed integer cycles folded into a detached held solve go to `filter/NetworkFixedCycles`, the algebra to `NetworkNormalEquationsDetached.cpp`, the release seam is `networkArLattice`). Follow-ups recorded, not built: a snapshot section; retain-and-revalidate across datum erases and continuous handovers; a generation restart at fold edges; held-covariance sigmas; the culprit-alias fix; the `signal_integer` misdeclaration (`NetworkEstimator.cpp`); Card 4 on the detached kernel; 6 h sim arms with eclipse edges before any promotion; field Emit; an integer consumer on an integer-recovering datum. Gate: `docs_index_check.py` and `make_golden_config.py --verify` pass with no ledger change.

## 7. Risks and what the beds cannot show

- **M9 is probably binding on the golden wire** (section 3). Moving the wire posture to PB holds off would cost what card LAH bought, and that is the author's decision.
- **The carried state gains nothing, by design**: the next float, the search, `network_dd_fractional.csv` and the harvest never tighten from the hold.
- **Precision only.** The datum is not integer-recovering (several anchors, float pin targets), so no result may be called an integer product. `signal_integer` is already misdeclared true; a separate defect.
- **Blind slips.** On a slip no P1 detector flags (a missed (1, 1) slip; S8 without the GF trigger; S7's second repetition without the IF-TD re-seed) the zero-q float carry itself goes stale; the fold then adds emitted confidence, and the float test has power only once the float moves. The guards are the contradiction release and the slip fixes of ARL section 13, which is why P1 carries them; the ladder measures the latency and cannot shorten it.
- **Conservative releases cost coverage**: continuous handovers, cold re-references and gate-drop erases release cycles whose integers did not change; the MW pivot fan-out frees clean arcs; each re-fold waits `persist_ep`. S3b, S6b, G1n and the field census measure the cost; retain-and-revalidate is a follow-up. AGTB section 5 adds one more: a float-vehicle drop is read as two lives one epoch apart.
- **Maturity is not independence.** Persistence filters single marginal fixes, but block-search fixes are epoch-correlated (a per-epoch bound of about 0.994, ARL section 10); the float test and block acceptance are the guards. A 2 h run cannot show the old "correctness falls as the run settles" signature, so 6 h arms are owed before any adoption.
- **Calibration.** The sim's float covariance is conservative (L1 0.310 against an error of 0.214 cycle), which lowers the test's power; an optimistic field covariance would cause false trips, and phase colour may inflate the null. chi2/K and the exceedance rate are gated on the sim and only reported on the field.
- **Late post-fit QC.** RAIM T3/T4 act after the emission: a unit folded through an arc they flag at t is emitted at t and released at t+1, counted as late; block acceptance is the only same-epoch guard [amended: FCFB section 3]. Gross-LOS is current only with `gross_los_emit_exclude` (armed on both goldens); the loader warns otherwise.
- **Stiffening.** Held integers leave phase colour (tau 300 s, 4 to 6 mm) no ambiguity to absorb it, so it leaks into the held clocks and biases; the sim-truth product error and block chi2/K are the tells.
- **Emission-state leak.** A PB-hold generation that starts while a unit is folded freezes that value for the whole generation; marks reset the consumer's satellite but do not restart the generation; `gen_under_fold` and `frozen_after_release` measure it.
- **Two DD-fold paths in one class**: the Card-4 dense fold (a retired lineage, the oracle) and the detached sparse fold; the loader refuses `carry_invariant`, so they never share an epoch.
- **First use of Eigen sparse.** An AMD-ordered sparse LDLT is deterministic for a fixed input order and agrees with the dense oracle to tolerance, not bitwise. A kernel throw, a float-reproduction failure or a shift above 0.5 m falls back to the float for the epoch, counted; no failure of the hold kills an epoch.
- **Cost at 300 stations**: about 2 840 rows in one network-wide component; the sparse fill of the borrowed-DD station graph is unmeasured; estimated 0.1 to 0.25 s per epoch against the 250 ms bar. AGT-6's lattice costs 18.2 ms per epoch there (AGTB section 5) and the offers grow with the hold, so a dense row store may be needed.
- **SeamGap releases everything after any skipped epoch** (free on the goldens, 720 of 720 emitted; a re-maturity cost on lineages with withhold eras, measured).
- **Shared files**: `NetworkProducerAr.cpp`, `ApplicationNetwork.cpp` and the ARL card are shared with concurrent sessions; if HEAD moves, the identity references are re-run.
- **Field**: no truth. Field runs are an Observe census only; field Emit is out of scope and never promoted without a ledger row.

## 8. Output moves declared in advance

- **FCF-1**: none (the pin code motion is structural; its A/B is the gate).
- **FCF-2**: none with the fold off; Observe writes two new trace files and a run-end row only when armed; Emit (the measurement arms of section 2) moves products and emission traces, the census and RHS-attribution path of section 5, and prints the warning's Emit variant.
- **FCF-3, under `trace_lattice` only**: the lattice step moves from SB_NETAR to SB_SOLPREP (section timing), `ArLatticeRelease` gains rows (the `network_ar_lattice.csv` and events columns and the run-end clause), the release set widens (replay culprits, gross-LOS, SeamGap) and offers become persistence-gated, so the observatory's counts move. With `trace_lattice` and the fold both off, nothing moves.
- **FCF-4**: under Emit, products and emission traces, the section 5 path, and the snapshot sections declared in FCF-4's split gate.

<!-- Results are appended below this line under dated headings. -->

## 9. 2026-10-11: FCF-0, the references

**Reference exe.** `build/x64-Release/gnss_processor_fcfref.exe`, md5 `b277f363568856cc5af0d53858aae11b`: the wrapper's Release build of main `df11cd3` (no work to do), byte-identical to `gnss_processor_agt6.exe`, the final AGT-6 exe (AGTB section 5). HEAD has not moved past AGT-6 in code, so no new baseline is declared and the P0 references repeat the stored AGT runs. No code changed, so AGT-6's Debug count stands (3 128 cases in 400 suites, 3 127 passed, 1 skipped, fixture-gated).

**P0 references** (`scripts/rapid_arm.py`; each `arm.json` equals its stored counterpart's with the run directory masked; run together, so no timing is read):

| reference | arm | against | `network_producer_ar.csv` | `network_ar_lattice.csv` (events) | `products.txt` |
| --- | --- | --- | --- | --- | --- |
| `_rap_fcf0_ref_r3c` | `arl_r3c_producer.json`, `start_offset_hours` 0, `decorrelated_partial`, `tree_basis`, `block_search`, `cycle_basis`, `trace_lattice` | `_rap_agt6g_r3c` (same exe), `_rap_agt6_r3c` | identical, 182 392 rows, `aa6779cc` | identical, `f38b7aa4` (`1b95075f`) | identical, `e6b572b6` |
| `_rap_fcf0_ref_n300` | `arl_n300_producer.json`, `start_offset_hours` 0, `cycle_basis`, `trace_lattice` | `_rap_agt6_n300` | identical, 970 104 rows, `a80f382c` | identical, `c067541e` (`7ff5c52a`) | identical, `34cc843b` |
| `_rap_fcf0_ref_r2` | `arl_r2_producer.json`, as rung 3c | `_rap_agt6_r2` | identical, 182 392 rows, `54745e3b` | identical, `be3246b6` (`c5f2d6d5`) | identical, `5312754a` |
| `_rap_fcf0_ref_s2` | `golden_sim_refound.json`, `start_offset_hours` 0.504167 | `_rap_agt_ref_s2`, `_rap_agt4_s2` | (search off) | (off) | identical, `e2376e3d` |
| `_rap_fcf0_ref_f2` | `golden_field_refound.json` | `_rap_agt_ref_f2`, `_rap_agt4_f2` | (search off) | (off) | identical, `0a14103d` |

- `scripts/nom_td_gate.py` RESULT IDENTICAL: both goldens against both stored runs, and rung 3c against `_rap_agt6g_r3c` (the same exe; with the tool's `p50 / max` mask, below). Against the AGT-6 campaign build (md5 `1955d1a5`: the stored n300, rung 2 and `_rap_agt6_r3c`) every CSV, the products, the manifest and `rt_engine.log` are identical and one run-end line differs: the lattice clause's cost wording, the change AGTB section 5 records between the campaign builds and the final exe.
- **Baseline figures** (the traces are identical, so AGTB's figures stand): settled-hour single-signal integers 45 026 (rung 3c), 46 054 (rung 2) and 365 566 (300 stations), all correct. The lattice: 56 934 offers at 60 stations (744 added, 0 contradicted, 0 not primitive; dimension 606 at the end; 138 releases that held rows, 138 dimensions lost) and 502 710 at 300 (4 971 added, 0 contradicted; dimension 3 718; 1 253 and 1 253).

**P1 references and the S8 twin** (added on review, 2026-10-11; the reference exe, five runs together):

| reference | posture, bed | `network_producer_ar.csv` | lattice (events) | `products.txt` | settled-hour single-signal integers |
| --- | --- | --- | --- | --- | --- |
| `_rap_fcf0_ref_r3c_sfx` | P1, rung 3c | `4e34ed73` | `4097685b` (`69690ce2`) | `e6b572b6`, as P0 | 44 730, all correct (P0 45 026) |
| `_rap_fcf0_ref_n300_sfx` | P1, 300 stations | `eae2c1dc` | `2dbb4d56` (`77ec5d14`) | `0b9b15a4` (P0 `34cc843b`) | 364 760, all correct (P0 365 566) |
| `_rap_fcf0_ref_tw8` | P1, S8 twin | `7e875efb` | `482c8314` (`85db7b41`) | `7aedc272` | 44 546, all correct |
| `_rap_fcf0_ref_tw2` | P1, S2 twin | `c7bfe4ec` | `a971afb7` (`3edc33f0`) | `1368d090` | 44 566, all correct |
| `_rap_fcf0_ref_tw8n` | P0, S8 twin | `c4b58532` | `f23c61f7` (`a41a920f`) | `553c0fec` | 44 830, 60 wrong |

- The twins reproduce AGTB's `_rap_agt6_{tw8,tw2,tw8n}`: every CSV, the products, the manifest and `rt_engine.log` identical; `arm.json` differs in the configuration's description alone (the committed `arl_tw*_producer.json` against the copies AGT ran), and on the S8 twins one run-end line in the campaign build's cost wording.
- The posture decides S8t (`simnet_producer_ar_grade.py`, settled hour): in P1 the GF trigger flags S43000SIM/G14 at the slip epoch (TOW 266250, r -0.467 m) and no fix is wrong; in P0 nothing flags it and the search fixes the stale pair for 30 epochs (60 wrong). On the S2 twin IF-TD refloats the arc, the GF flag coincident.
- On rung 3c the GF trigger's one flag coincides with the rung's scripted slip (S43000SIM/G22, TOW 261930), so P1's products equal P0's and the search trace moves through the suspect mask alone. At 300 stations it flags two clean arcs (S05501SIM G27 and G32, 4 flags at r 0.041 to 0.046 m against the 0.04 m floor; no truth slip at the station) and the products move: false releases that FCF-3's clean-arc bar will count.

**M9 from the P0 references.** PB_HOLD generation starts over 2 h (`network_events.csv`): 68 on every sim bed (64 first sightings, 4 after a gap, 2 in the second hour), 93 on the field (90 and 3, all 3 in the second hour). In the settled hour the phase-visible clock of almost every key on the golden wire is frozen at a first-hour value, so a held solve reaches its phase side only through two or three generation starts: the mechanism of the M9 prediction, which D1 measures.

**The beds pass the fold's interlocks.** Both refound goldens and the 16 ARL producer configurations of section 6 (flat files) arm `float_kf`, `phase_datum_held_basis` and `gross_los_screen` with `gross_los_emit_exclude`, and none of the knobs section 4 refuses: the fold runs on every bed with no knob changed beyond its own and the search's.

**Docs and goldens.** `python scripts/docs_index_check.py` passes; `python scripts/make_golden_config.py --verify` passes unchanged (70 armed in regime on each golden, 6 documented exceptions).

**Review fixes (2026-10-11), made before freezing.** Section 2: the Emit probe as a dated departure, with its loader guards and the warning's Emit variant. Section 5: the readers of `sol` with their write sets. Section 6: postures P0 and P1 pinned; S8t, S2t and S7t's second repetition named detected with their causes; the release half stated vacuous on the epoch-31 rungs; the P0 S8 twin as the D2(b) control; FCF-4's snapshot gate restated per section; the runaway edge declared. `scripts/nom_td_gate.py` gained the `p50 / max` mask (and its docstring says so), so "identical" is the tool's verdict.

**Card size.** About 59 KB of the 60 KB bar: results from FCF-1 on go to a continuation card (the AGT and AGTB precedent), filed when FCF-1 starts.

**The ruling.** Requested in section 2; not given at filing. FCF-4 waits for it.

**Gate verdicts.** The P0 references reproduce AGT-6's stored search trace, lattice trace and products byte for byte, and the tool finds the same-exe pair identical (PASS); HEAD has not moved past AGT-6, so no new baseline is declared; docs and goldens pass (PASS); the ruling request is written (PASS).

## 10. Continued in card FCFB (2026-10-11)

The results from FCF-1 on (the detached kernel and the held solve, then FCF-2 to FCF-5) are recorded in [FCFB](FCFB_fixed_cycle_fold_continued_2026-10-11.md), this file being near the 60 KB bar. Sections 1 to 8 above stay the binding request, choice, design, API mapping, readers, gates, risks and declared output moves.
