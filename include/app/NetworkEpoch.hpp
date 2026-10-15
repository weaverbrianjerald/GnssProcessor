#pragma once
/**
 * @file NetworkEpoch.hpp
 * @brief One epoch of the network producer: everything the stages of
 *        Application::networkProcessEpoch_ hand each other (card NOM3 step EP1).
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
 *                                                                networkSignalIntegrity, networkMeasurementAdmission (filter/NetworkSlipFunnel.hpp);
 *                                                                declared writable, read only (EP1c makes them const): networkBuildCarryDdPriors,
 *                                                                networkSolveGlue (its replay's DD rebuild)
 *   pass_a        EpochPassA           PASS A's sizing           networkPassAHead_, networkPassASlipScan_, networkAssemblyPrefilter_,
 *                                                                networkSignalIntegrity, networkMeasurementAdmission, networkPhaseAnchorEpoch
 *                                                                (shared_phase_keys), networkPassB_ (modeled, present)
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
 * After the solve the seams' outputs are solve.handoff and prep.sol; the driver's own writes
 * are the four stores above (the lint checks the driver, the compiler checks the seams).
 *
 * Const is deep: no member, here or in a struct held by value inside it, is a non-const
 * reference, a pointer to non-const, a smart pointer or a std::function
 * (scripts/nom_ep1_verify.py check V5).  Not covered: epoch-scoped state a run-owned engine
 * holds (the estimator's per-epoch columns and blocks, SlipQcChannel::epoch); its writers are
 * the engines' own callers.
 *
 * Neither copyable nor movable (se is a reference); it cannot bind a temporary epoch.
 */
#include "filter/NetworkRunTypes.hpp"      // StationAssembly, EpochWorkspace, EpochAction
#include "filter/NetworkSolveGlue.hpp"     // NetworkSolveGlueResult, NetworkPostFitHandoff
#include "filter/NetworkSolPrep.hpp"       // NetworkSolPrepResult
#include "filter/NetworkArCycleBasis.hpp"  // ArEpochFixes
#include "filter/NetworkFixedCycles.hpp"   // FixedCycleEmission
#include "filter/NetworkZStore.hpp"        // ArcKey
#include "filter/NetworkSlipFunnel.hpp"    // NetworkSlipFunnel (card NOM3 step EP2)
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
    EpochWorkspace         ws;            ///< The replay capture channel, the clock maps, the within-epoch culprit sets (card WCF).
    NetworkSolveGlueResult solve;         ///< The shared solution and its datum; solve.handoff is the post-fit hand-off.
    ArEpochFixes           ar_fixes;      ///< The producer AR search's fixes.
    FixedCycleEmission     fixed_cycles;  ///< The fixed-cycle fold's held solve.
    NetworkSolPrepResult   prep;          ///< The solution prepared for the wire.
    bool                   emitted{false};///< This epoch's solution reached the wire (networkEmitStore counted an emission).

    /// The solution the wire carries: the held vector when the fixed-cycle fold emits, else
    /// the float solve (card FCF).  Read by solution prep and the emitter; every other seam
    /// reads solve.x_s.
    const Eigen::VectorXd& x_emit() const { return fixed_cycles.emit ? fixed_cycles.x_held : solve.x_s; }
};

static_assert(!std::is_copy_constructible_v<NetworkEpoch> && !std::is_move_constructible_v<NetworkEpoch>);
static_assert(!std::is_copy_assignable_v<NetworkEpoch> && !std::is_move_assignable_v<NetworkEpoch>);

/// A measure-only per-epoch instrument's form (the row type of step EP3's
/// kNetworkEpochObservers).  Every converted seam that returns void and has no kept or
/// output parameter has it.
using NetworkEpochObserver = void (*)(NetworkRun& run, const NetworkEpoch& ep);

} // namespace GnssProcessor
