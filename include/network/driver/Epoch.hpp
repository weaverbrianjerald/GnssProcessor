#pragma once
/**
 * @file Epoch.hpp
 * @brief One epoch of the network producer: everything the stages of
 *        Application::networkProcessEpoch_ hand each other.
 *
 * Built once per processed epoch, right after the admission (the event injector and the
 * canonical-pair screen) fixes the observations the producer sees, and destroyed at the
 * epoch's end.  Construction is the per-epoch reset; nothing here survives to the next
 * epoch.  A value that must survive is run state (NetworkRunContext), never a member here.
 *
 * Members in the order the stages fill them.  "Written by" is the authoritative list, and
 * scripts/nom_epoch_lint.py (check V11) proves it against the spine on every run: a writer
 * receives its group as a non-const parameter on its spine line.  By hand,
 * `grep -nE "ep\.pass_a[,)]" src/core/app/ApplicationNetworkEpoch.cpp` lists the spine
 * lines that hand the layout to a writer, and `grep -rnE "n?ep\.pass_a\.present" src`
 * every read of `present` through the epoch (a binding, an adapter's forwarding argument, the
 * spine's own store); a writer binds it through its `pass_a` output instead.
 *
 *   member        group type           available from            written by
 *   se            const SyncedEpoch&   construction              the admission (before construction)
 *   clock         EpochClock           the epoch top             the driver, once
 *   funnel        NetworkSlipFunnel    the funnel's opening      networkSlipFunnelOpen, networkPassASlipScan_ (raw subset), networkSlipFunnelUnionRaw,
 *                                                                networkSignalIntegrity, networkMeasurementAdmission (network/slip/SlipFunnel.hpp)
 *   pass_a        EpochPassA           PASS A's sizing           networkPassAHead_, networkPassASlipScan_, networkAssemblyPrefilter_,
 *                                                                networkSignalIntegrity, networkMeasurementAdmission, networkPhaseAnchorEpoch
 *                                                                (shared_phase_keys), networkPassB_ (present; modeled is read only)
 *   columns       EpochColumns         networkAssemblyRegisterColumns_   that seam only
 *   assembly      EpochAssembly        networkPassB_             networkPassB_; est_index_of also networkSolveGlue (each T2 replay rebuilds it)
 *   dd_build      EpochDdBuild         the DD build              networkBuildCarryDdPriors (the driver's primary call), networkSolveGlue
 *                                                                (each replay's DD rebuild)
 *   ws            EpochWorkspace       construction (empty)      networkSolveGlue (the primary DD build reads it empty, card WCF)
 *   solve         NetworkSolveGlueResult  the solve              the driver stores the solve's return; solve.handoff also networkPostFitRaim, networkClockQc
 *   ar_fixes      ArEpochFixes         the producer AR           the driver stores networkProducerAr's return
 *   fixed_cycles  FixedCycleEmission   the fixed-cycle fold      the driver stores networkFixedCycles's return
 *   prep          NetworkSolPrepResult solution prep             the driver stores networkSolPrep's return; prep.sol also networkEmitStore (the re-base)
 *   emitted       bool                 after networkEmitStore    the driver, once
 *   events        EpochEvents          construction (empty)      networkObserveEpoch (the recorder rows), networkSlipFunnelOpen,
 *                                                                networkPassASlipScan_, networkMeasurementAdmission, networkPhaseAnchorEpoch,
 *                                                                networkAmbFloatDropEpoch, networkHandoverStransformBeforeRows,
 *                                                                networkSolveGlue, networkInstallCarry, networkGrossLosScreen, networkSolPrep,
 *                                                                networkShHealth, networkLeveling, networkClockQc, networkStashStatus
 *                                                                (each writes its own group; the EpochEvents comment lists them)
 *
 * How a seam reads and writes it:
 *   - every converted seam takes `const NetworkEpoch& ep`; the groups it writes arrive as
 *     separate non-const parameters named after the member (`EpochPassA& pass_a`);
 *   - its preamble binds each name it reads through `ep`, with the frozen declarator
 *     (`const std::vector<bool>& present = ep.pass_a.present;`), and each name it writes
 *     through its output, marked `// written`; below the preamble the body never spells `ep.`;
 *   - a seam reads only groups available at its spine line (scripts/nom_epoch_lint.py);
 *   - no seam keeps a pointer or a copy of an epoch member across a call (card WCF's defect
 *     was a by-value pointer copy that stayed null while the original moved on).
 * After the solve the seams' outputs are solve.handoff, prep.sol and events (each writer its
 * own group of it); the driver's own writes are the four stores above (the lint checks the
 * driver, the compiler checks the seams).
 * Inside networkPassB_ its outputs reach its own serial stages, not the
 * spine: networkPrefitFuseEpoch writes the assembly, networkAssemblyConsume writes pass_a and
 * the assembly; the tally, the in-solve track and the eq34 audit take the assembly as a const
 * parameter, since a group the caller is still writing is not read through `ep`.
 *
 * Const is deep: no member, here or in a struct held by value inside it, is a non-const
 * reference, a pointer to non-const, a smart pointer or a std::function
 * (scripts/nom_ep1_verify.py check V5).  Not covered: epoch-scoped state a run-owned engine
 * holds (the estimator's per-epoch columns and blocks, SlipQcChannel::epoch); its writers are
 * the engines' own callers.
 *
 * Neither copyable nor movable (se is a reference); it cannot bind a temporary epoch.
 */
#include "network/driver/RunTypes.hpp"      // StationAssembly, EpochWorkspace, EpochAction
#include "network/solve/SolveGlue.hpp"     // NetworkSolveGlueResult, NetworkPostFitHandoff
#include "network/emit/SolPrep.hpp"       // NetworkSolPrepResult
#include "network/ar/ArCycleBasis.hpp"  // ArEpochFixes
#include "network/ar/FixedCycles.hpp"   // FixedCycleEmission
#include "network/ar/ZStore.hpp"        // ArcKey
#include "network/slip/SlipFunnel.hpp"    // NetworkSlipFunnel
#include "network/state/ArcLedger.hpp"            // ArcReentryOutcomeMap, ArcLedger::Key
#include "network/solve/Sbasis.hpp"        // the code-datum records
#include "network/qc/BlockBaarda.hpp"   // BlockBaardaHit
#include "network/qc/Congruence.hpp"    // CongruenceHit
#include "network/assembly/RowClassify.hpp"   // kNetDataClasses
#include "common/PppObservation.hpp"
#include <Eigen/Core>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct SyncedEpoch;
struct NetworkRun;

/// The epoch's time base and the phase-leveling carry's warmth, stored once at the epoch top
/// from the driver's three const locals (dt, pl_now_s, carry_warm), which keep their names
/// for the inline blocks.
struct EpochClock {
    double dt{0.0};           ///< Seconds since the previous processed epoch, week-aware; 0 on the first.  (The DD-carry predict uses its own TOW-only step, card NOM3 section 2 finding 4.)
    double pl_now_s{0.0};     ///< This epoch in continuous GPS seconds.
    bool   carry_warm{false}; ///< The phase-leveling carry is past its warm-up (the emitted-epoch count reached network.phase_leveling_warmup_epochs).
};

/// The layout after PASS A.
struct EpochPassA {
    std::vector<std::vector<PppObservation>> modeled;   ///< Per station: this epoch's modelled observations.
    std::vector<bool>                        present;   ///< Per station: modelled this epoch and still in the solve.
    std::set<std::string>                    shared_phase_keys;  ///< Legacy: the pivot's keys; prioritized: the anchored union.
    int                                      n_dropped_sats{0};  ///< Satellites the observability prefilter dropped.
};

/// The shared columns registered for this epoch (written once, by networkAssemblyRegisterColumns_).
struct EpochColumns {
    std::vector<std::string>                         registered_clock_sats;
    std::vector<std::pair<std::string, std::string>> registered_code_keys;
    std::vector<std::string>                         col_desc;   ///< One readable descriptor per shared column.
    int n_clock{0};
    int n_pb{0};
    int n_cb{0};
    int n_present{0};
};

/// The station blocks PASS B assembled.
struct EpochAssembly {
    std::vector<StationAssembly> asm_res;            ///< Per station: the assembled block.
    std::vector<int>             est_index_of;       ///< Per station: its estimator slot, -1 when not added (each T2 replay rebuilds it).
    int                          total_pins{0};      ///< Ambiguity pins folded this epoch.
    double                       sec_asm_ms_epoch{0.0}; ///< This epoch's assembly wall (D0; wall clock only).
};

/// The DD build's reverse map (carry group -> chord -> ambiguity key); the last build before
/// the accepted solve wins; read by networkInstallCarry.
struct EpochDdBuild {
    std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key;
};

/**
 * @brief The events of one epoch: the flags, sets, verdicts and counts a stage records for
 *        later stages and for the traces of the same epoch.
 *
 * Built with the epoch, so no member is cleared by hand and none reaches the next epoch: a
 * value an epoch skipped or withheld before its reader leaves nothing behind.  A value that
 * must survive the epoch (a one-epoch latch, a hand-off pending until the stash) is run
 * state in NetworkCarryState with its net::kRunFields row, never a member here.
 *
 * One group per writing stage, in spine order.  A writer receives `EpochEvents& events` as
 * an output parameter and binds the group it writes (`EpochEvents::Gross& gross =
 * events.gross;   // written`); a reader binds through `ep.events`.
 *
 *   group        written by
 *   slip         networkSlipFunnelOpen (carry_innov_flat_now); networkPassASlipScan_
 *                (relevel, sid_reentry_outcome); networkAmbFloatDropEpoch (relevel)
 *   admission    networkMeasurementAdmission
 *   phase_datum  networkPhaseAnchorEpoch
 *   handover     networkHandoverStransformBeforeRows (sts_pre); networkSolveGlue (the rest)
 *   code_datum   networkSolveGlue
 *   solve        networkSolveGlue
 *   install      networkInstallCarry
 *   gross        networkGrossLosScreen
 *   qc           networkSolveGlue, networkSolPrep, networkShHealth (rhs_trip_pending);
 *                networkClockQc (cb_excursion_sats)
 *   harvest      networkLeveling
 *   census       networkObserveEpoch, through the recorder rows of the FrameDiag slot
 *                (networkPrefitCensus, networkRedundancyCensus, networkBlockBaardaEmit,
 *                networkCongruenceEmit)
 *   status       networkStashStatus
 *
 * Member names are those of the former NetworkCarryState members (each row's `was` in
 * net::kRunFields), so a grep for the old name finds the new home.
 */
struct EpochEvents {
    /// PASS A's slip scans and the slip funnel.
    struct Slip {
        /// The corroborated-flat subset of last epoch's carry-innovation latch: a member not
        /// also in this epoch's raw funnel subset keeps its carried DD instead of being
        /// refloated by the DD-prior build.
        std::set<std::pair<int, std::string>> carry_innov_flat_now;
        /// The phase-leveling free-epoch latch: the ambiguities freed this epoch and why,
        /// written by the PASS-A slip responses and the float-ambiguity drops.
        RelevelEpochState relevel;
        /// The re-entry response's per-arc record (observatory: the solve does not read it).
        ArcReentryOutcomeMap sid_reentry_outcome;
    };
    /// The measurement admission's epoch summary, stamped with the epoch's TOW.
    struct Admission {
        double adm_ep_tow = -1.0;           ///< TOW of the stamp; -1 when the admission screened no station.
        double ep_adm_sta_worst_z = 0.0;    ///< Median over screened stations of each station's worst |z| (m).
        double ep_adm_net_worst_z = 0.0;    ///< Network maximum |z| (m).
    };
    /// The phase-datum election.
    struct PhaseDatum {
        std::map<std::string, std::string> this_epoch_anchor;   ///< "<sat>_<sig>" -> the anchor station's name.
        std::map<std::string, int> eq34_epoch_declared;         ///< Per group: the declared gauge dimension.
        std::map<std::string, int> eq34_epoch_bridged;          ///< Per group: the shared-side bridge pins.
        std::set<std::pair<std::string, std::string>> eq34_reref;  ///< (sat, signal) re-referenced by the chain election.
    };
    /// The clock-pivot handover's S-transform.
    struct Handover {
        /// What the dense kernel did for one constellation it re-expressed.
        struct StsDense {
            std::string q;                    ///< The incoming pivot.
            std::string p;                    ///< The outgoing pivot (the frame the block was in).
            double      c0{0.0};              ///< The incoming pivot's carried level before (m).
            double      pqq{0.0};             ///< Its block variance before, P_qq (m^2).
            std::size_t k_old{0};             ///< Block size before the outgoing pivot was appended.
            int         iq{-1};               ///< The incoming pivot's block row.
            bool        p_added{false};       ///< The outgoing pivot was appended.
            bool        selfcheck_ok{true};   ///< The kernel's self-check.
            std::set<std::string> row_sats;   ///< The constellation's clocks with a block row (q included).
            std::set<std::string> clean;      ///< ...whose carried entry equalled its fold decision before the transform.
        };
        /// What the preview before PASS B did for one changed constellation.
        struct StsBeforeRows {
            std::string pivot;              ///< The incoming pivot the preview elected.
            bool        already{false};     ///< The carry was already in that pivot's datum (a re-detection after a skipped epoch).
            bool        applied{false};     ///< The transform moved the carry.
            double      c0{0.0};            ///< The coefficient applied (m).
            double      var_q{0.0};         ///< The incoming pivot's carried variance (m^2).
            long long   rows{0};            ///< Carried clock entries re-expressed.
            double      x_q_post{0.0};      ///< The incoming pivot's carried level after it (expected 0).
            bool        selfcheck_ok{true}; ///< The exactness self-check.
        };
        std::map<char, StsBeforeRows> sts_pre;   ///< The preview, per constellation (empty with its knob off).
        std::set<char> sts_consts;               ///< Constellations whose carry the transform re-expressed (the fold skip is released).
        std::map<char, double> sts_c0;           ///< The coefficient applied per constellation (m).
        std::map<char, StsDense> sts_dense;      ///< The dense kernel's record, per constellation.
    };
    /// The code-bias datum the solve built.
    struct CodeDatum {
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_if_pin_sel;   ///< Satellite -> the IF-pin pair selected.
        std::map<std::string, NetworkSbasis::PinPairChange> this_epoch_if_pin_changes;   ///< Satellites whose IF-pin pair changed.
        std::map<std::string, NetworkSbasis::PinPairHold> this_epoch_if_pin_holds;       ///< Satellites whose IF-pin pair was held.
        std::map<std::string, NetworkSbasis::FamilyRowRealization> this_epoch_cb_family_rows;  ///< The family rows realized.
        std::vector<NetworkSbasis::FamilyBirthPrior> this_epoch_cb_family_births;        ///< The family birth priors.
        std::vector<NetworkSbasis::FamilyBirthSkipRecord> this_epoch_cb_family_birth_skips;  ///< Satellites given no birth row.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_if_pin_real;  ///< Satellite -> the IF-pin pair realized.
        std::set<std::string> this_epoch_extra_band_held;   ///< "sat|signal" extra-band columns held (not measured).
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_cb_ifpin;   ///< Satellite -> the IF-pin pair pinned on.
        std::set<std::string> this_epoch_cb_reseed;            ///< Satellites whose code bias was re-seeded.
        std::set<std::string> this_epoch_cb_reseed_channels;   ///< Their channels.
    };
    /// The solve's re-admission seeds and the within-epoch RAIM tier's observability.
    struct Solve {
        /// One re-admission seed, checked against the solve at the stash.
        struct ReadmitSeed { SharedParamKind kind; std::string sat, signal; double value, var_eff; };
        std::vector<ReadmitSeed> readmit_seed_check;   ///< This epoch's seeds.
        double      ep_sigma0_first = -1.0;      ///< First-assess sigma0 (pre-adaptation); < 0 when not assessed.
        int         ep_we_refloat_sats = -1;     ///< Whole-satellite refloats settled this epoch; < 0 when not assessed.
        double      ep_we_clk_move_max = -1.0;   ///< Max |final - primary| clock move (m); < 0 when not assessed.
        std::string ep_we_clk_move_sat;          ///< The satellite of that maximum.
    };
    /// The carry install's churn, differenced from the driver's run totals.
    struct Install {
        long long ep_chord_births = 0, ep_slip_refloats = 0, ep_d5_flags = 0, ep_d5_retaps = 0;
    };
    /// The gross-LOS screen's decisions, deferred to its act half and the emit prep.
    struct Gross {
        std::set<std::string> gl_emit_withhold;                    ///< Satellites withheld from the wire.
        std::set<std::string> gl_emit_disc;                        ///< Satellites given a discontinuity.
        std::vector<std::pair<int, std::string>> gl_act_declare;   ///< (station, satellite) declared.
        std::vector<std::pair<int, std::string>> gl_act_release;   ///< (station, satellite) released.
        std::vector<std::pair<int, bool>> gl_act_station;          ///< (station, declared) station-level edges.
        std::vector<std::pair<int, std::string>> gl_act_cold_refloat;  ///< (station, satellite) cold-bound refloats.
    };
    /// Quality-control events.
    struct Qc {
        std::string rhs_trip_pending;            ///< The first trip reason ("" = none); names the flight-recorder dump.
        std::set<std::string> cb_excursion_sats; ///< Satellites whose code-bias carry the stash inflates.
    };
    /// Rows the carry harvest buffers for writers that run after it.
    struct Harvest {
        std::vector<std::string> pc_trace_rows;             ///< network_phase_colour.csv rows.
        std::vector<std::string> amb_if_rows;               ///< network_amb_if.csv rows.
        std::vector<ArcLedger::Key> sid_carry_prune_deaths; ///< Arcs the prune killed, for the roster audit's death rows.
    };
    /// The FrameDiag censuses, read by the epoch-status row and the MDB join.
    struct Census {
        double    ep_pc_max_s[kNetDataClasses]{};       ///< Prefit census: max |s| per data class.
        double    ep_pc_p99_s[kNetDataClasses]{};       ///< Prefit census: p99 |s| per data class.
        long long ep_pc_n_gt_10[kNetDataClasses]{};     ///< Prefit census: rows with |s| > 10.
        long long ep_pc_n_gt_1000[kNetDataClasses]{};   ///< Prefit census: rows with |s| > 1000.
        long long ep_pc_rows = 0;                       ///< Prefit census data rows (0 means blank columns).
        double    redun_ep_tow = -1.0;                  ///< TOW stamp of the redundancy census.
        long long ep_redun_rows = 0;                    ///< Data rows swept.
        long long ep_uncheckable_rows = 0;              ///< Of those, uncheckable.
        double    ep_uncheckable_max_s = 0.0;           ///< The worst uncheckable |s|.
        long long ep_unobservable_local = 0;            ///< Locally unobservable rows.
        std::vector<BlockBaardaHit> bb_hits;            ///< Reportable blocks, for the block-Baarda emission.
        std::vector<CongruenceHit> cg_hits;             ///< Per-station records, for the congruence emission.
    };
    /// The prior-residual census of the stash, read by the epoch-status row.
    struct Status {
        double    prires_ep_tow = -1.0;       ///< TOW stamp of the census.
        long long ep_prires_rows = 0;         ///< Carried priors censused.
        long long ep_prires_resp = 0;         ///< Of those, labelled responded.
        long long ep_prires_frame = 0;        ///< Of those, labelled frame.
        long long ep_prires_reported = 0;     ///< Of those, |t| above the trace threshold.
        double    ep_prires_max_t = 0.0;      ///< The worst |t|.
        bool      ep_s0g_formed = false;      ///< sigma0_global was defined (false: blank column).
        double    ep_s0g_sigma0 = 0.0;        ///< sigma0_global.
        bool      ep_s0g_tension_def = false; ///< The tension ratio had a data side.
        double    ep_s0g_tension = 0.0;       ///< The prior tension ratio.
        int       ep_s0g_flags = 0;           ///< NetworkPriorResidual::GlobalFlag bitmask.
    };

    Slip        slip;
    Admission   admission;
    PhaseDatum  phase_datum;
    Handover    handover;
    CodeDatum   code_datum;
    Solve       solve;
    Install     install;
    Gross       gross;
    Qc          qc;
    Harvest     harvest;
    Census      census;
    Status      status;
};

struct NetworkEpoch {
    /// @param se The admitted epoch: the source's epoch, or a scratch copy the epoch function
    ///           owns and declares before this object (the injector's, the screen's).
    explicit NetworkEpoch(const SyncedEpoch& se) : se(se) {}
    NetworkEpoch(const SyncedEpoch&&) = delete;
    NetworkEpoch(const NetworkEpoch&) = delete;
    NetworkEpoch& operator=(const NetworkEpoch&) = delete;
    NetworkEpoch(NetworkEpoch&&) = delete;
    NetworkEpoch& operator=(NetworkEpoch&&) = delete;

    const SyncedEpoch&     se;            ///< The admitted observations.
    EpochClock             clock;         ///< dt, the epoch's seconds, the carry's warmth.
    NetworkSlipFunnel      funnel;        ///< The slip funnel.
    EpochPassA             pass_a;        ///< The layout.
    EpochColumns           columns;       ///< The shared columns.
    EpochAssembly          assembly;      ///< The station blocks.
    EpochDdBuild           dd_build;      ///< The DD build's reverse map.
    EpochWorkspace         ws;            ///< The replay capture channel, the clock maps, the within-epoch culprit sets.
    NetworkSolveGlueResult solve;         ///< The shared solution and its datum; solve.handoff is the post-fit hand-off.
    ArEpochFixes           ar_fixes;      ///< The producer AR search's fixes.
    FixedCycleEmission     fixed_cycles;  ///< The fixed-cycle fold's held solve.
    NetworkSolPrepResult   prep;          ///< The solution prepared for the wire.
    bool                   emitted{false};///< This epoch's solution reached the wire (networkEmitStore counted an emission).
    EpochEvents            events;        ///< The epoch's events, one group per writing stage.

    /// The solution the wire carries: the held vector when the fixed-cycle fold emits, else
    /// the float solve.  Read by solution prep and the emitter; every other seam
    /// reads solve.x_s.
    const Eigen::VectorXd& x_emit() const { return fixed_cycles.emit ? fixed_cycles.x_held : solve.x_s; }
};

static_assert(!std::is_copy_constructible_v<NetworkEpoch> && !std::is_move_constructible_v<NetworkEpoch>);
static_assert(!std::is_copy_assignable_v<NetworkEpoch> && !std::is_move_assignable_v<NetworkEpoch>);

/// A measure-only per-epoch instrument's form (the row type of step EP3's
/// kNetworkEpochObservers).  Every converted seam that returns void and has no kept or
/// output parameter has it.
using NetworkEpochObserver = void (*)(NetworkRun& run, const NetworkEpoch& ep);

/// A per-epoch instrument that also records events a later stage of the same epoch reads
/// (the recorder rows of kNetworkEpochObservers): the observer form with the events output.
using NetworkEpochRecorder = void (*)(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events);

} // namespace GnssProcessor
