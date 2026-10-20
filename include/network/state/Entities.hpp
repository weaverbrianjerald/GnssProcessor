#pragma once
/**
 * @file Entities.hpp
 * @brief The network producer's cross-epoch run state as entity tables.
 *
 * Read this header first.  An **entity** is one thing the network carries state about from
 * one epoch to the next: a constellation, a satellite, a satellite signal, a station, a line
 * of sight (station, satellite) or an arc (station, satellite, signal).  Each entity has one
 * **table**, an ordered map from the entity's typed identity (common/GnssIdentity.hpp) to
 * a **row**, a plain struct whose members are the entity's carried fields.  The six tables
 * are the members of @ref net::NetworkEntities, held by the run context as
 * @c NetworkRunContext::entities beside the flat @c NetworkCarryState they replace one
 * entity per sub-step (scripts/nom3_designs/N4_design.md section 6).
 *
 * **Presence.**  A field that was a map entry is a @c std::optional in the row, present
 * exactly when the old map held the key; a set membership is a @c bool.  A row is created
 * by the first write of any of its fields and is never erased (decision D8): erasing an
 * entry resets the field, and the row stays.  The free functions below are the only access
 * (no member behaviour): @ref get and @ref put replace @c find / @c operator[], @ref drop
 * and @ref dropAll replace @c erase / @c clear, @ref present replaces @c size, and
 * @ref forEach replaces a range loop.  A field keyed below the entity (a station's signals)
 * lives in a nested map of the row, with the same functions taking the sub-key.
 *
 * **Iteration order** (design section 4).  A table iterates in the order of its identity,
 * and every identity's comparator reproduces the string order of the key it replaced
 * (tests/test_gnss_identity.cpp), so @ref forEach visits the present rows of a field in the
 * order the old map did.  Constellations order by RINEX letter (@ref ByLetter).  A member
 * that was keyed by station NAME is iterated through @ref byStationName, which visits rows
 * in name order; a member whose order no typed key reproduces keeps its old key inside the
 * row that owns it (O4).  No loop is fused across members (O7).
 *
 * **Persistence.**  Every moved field has one @ref FieldDecl row in its entity's field table
 * (@ref kStationFields and its siblings, src/core/network/state/Entities.cpp): its name, the
 * @c NetworkCarryState member it was, how it survives a restart (@ref Persist) and, for a
 * snapshot record, the two functions that fill the record from the table and the table
 * from the record.  The snapshot writer and reader call those functions where the old copy
 * blocks stood; the reader keeps today's restore order (@ref kRestoreOrder).
 *
 * **Lifecycle.**  The satellite, station and arc rows declare a @ref Lifecycle (decision
 * D4): a return is never read as a first sighting (standing invariant 7).  It is filled
 * where an owner computes it; a row whose owner does not yet compute it stays
 * @ref Life::Absent.
 *
 * **Parallel access.**  The station produce runs in parallel over stations and only reads
 * the tables (@ref get on a const table is a @c std::map::find, safe against concurrent
 * readers).  Rows are inserted only by serial code (the harvests and the serial seams), so
 * no insert can race a read (verifier check V5, scripts/nom_n4_verify.py).
 *
 * **The reach table** ("entities as carried", run end): per declared field its maximum and
 * final present rows, with its persistence and record, and per identity converter its
 * round-trip failures.  It is the reader's view of the run state and the evidence that an
 * arm reached a field.  Its log lines are trace class.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "common/GnssIdentity.hpp"
#include "network/state/ArcLedger.hpp"            // ArcRecord (an arc's ledger record, step N4f2)
#include "network/ar/ArCycleBasis.hpp"   // ArFamilyKey, ArBasisMemory
#include "network/qc/GrossLos.hpp"       // GrossLosState
#include "network/emit/PbHoldClone.hpp"    // PbHoldClone
#include "network/solve/Sbasis.hpp"         // NetworkSbasis::PinPairHold
#include "network/assembly/MeasurementAdmission.hpp"   // MeasurementAdmissionArc
#include "network/assembly/SignalIntegrity.hpp"   // SignalIntegrityArc
#include "network/slip/RawObservables.hpp"   // IonoCsState
#include "network/driver/RunTypes.hpp"   // LocalCarry, SdEwlFix
#include "model/PppModel.hpp"           // PppModel::CanonicalSignal

namespace GnssProcessor {
    struct NetworkRunContext;
    struct NetworkStateSnapshot;
    struct NetworkRun;
    struct NetworkEpoch;
    struct NetworkRunEndView;
}

namespace GnssProcessor::net {

// ===========================================================================
// Declarations: persistence, lifecycle, field rows
// ===========================================================================

/**
 * @brief How a field survives a restart: the persistence column of
 *        scripts/nom3_designs/N4_fields.tsv.
 */
enum class Persist : std::uint8_t {
    Record,           ///< Written to and restored from a snapshot record.
    ResumeSeeded,     ///< Not a record; seeded by the warm resume from other records.
    ColdStart,        ///< Starts empty on every run, warm or cold.
    Pending,          ///< A hand-off consumed within a few epochs of being set.
    RunConst,         ///< Set once at setup and read for the run.
    WriterTelemetry,  ///< The snapshot writer's own counters.
    Epoch,            ///< A per-epoch value: a member of EpochEvents (network/driver/Epoch.hpp), built with the epoch.
    Latch,            ///< Set at one epoch and consumed at the next.
    ResumeHandoff,    ///< Set by the warm resume, consumed at the first leg epoch (step N4g).
    EngineEpoch,      ///< Rebuilt every epoch by its owner but kept in the run: the run end or a held pointer reads it.
};

/// The name of a persistence class, as the reach table prints it.
const char* persistName(Persist p);

/**
 * @brief An entity's lifecycle state.  A return is never read as a first sighting
 *        (standing invariant 7).
 */
enum class Life : std::uint8_t {
    Absent,    ///< Never seen (or not computed by any owner yet).
    Present,   ///< Seen at the latest epoch, continuously since its first sighting.
    Departed,  ///< Seen before, not at the latest epoch.
    Returned,  ///< Seen again after a departure.
};

/// The lifecycle of one row: its state and the loop epochs of its first and latest sighting.
struct Lifecycle {
    Life      life{Life::Absent};
    long long ep_first{-1};   ///< Loop epoch of the first sighting.
    long long ep_last{-1};    ///< Loop epoch of the latest sighting.
};

struct NetworkEntities;

/**
 * @brief One declared field of an entity: one row of the entity's field table, or of the
 *        run remainder's table @ref kRunFields.
 *
 * A Record field of an entity names its snapshot tag and has both fill functions; every
 * other field has them null.  A @ref kRunFields row has every function null: a run record
 * keeps its copy block in the snapshot writer and reader, and the reach table samples the
 * entity tables only.  The fill functions take the run context because a record's copy reads
 * the knobs the old copy block read (the shared-history marginal variance of the Z rows,
 * the restored-entry convention of @c makeRestoredLocalCarry).
 */
struct FieldDecl {
    const char* name;          ///< The member, as spelled in the row struct.
    const char* was;           ///< Its former @c NetworkCarryState member (the G1 trail).
    Persist     persist;       ///< How it survives a restart.
    const char* record;        ///< Snapshot tag ("Z"); "" unless Persist::Record.
    const char* fingerprint;   ///< The knob whose fingerprint makes a toggle cold-start, or "".
    /// Fills the record from the table (Record only; the writer gates the call).
    void (*to_snapshot)(const NetworkRunContext& ctx, NetworkStateSnapshot& snap);
    /// Fills the table from the record (Record only; the reader gates the call).
    void (*from_snapshot)(const NetworkStateSnapshot& snap, NetworkRunContext& ctx);
    /// The number of rows where the field is present (the reach table).
    std::size_t (*rows)(const NetworkEntities& e);
};

/// One entity's field table, named.
struct EntityFields {
    const char*                   entity;  ///< "station", "satellite", ...
    const std::vector<FieldDecl>* fields;  ///< Its declared fields.
};

// ===========================================================================
// The rows
// ===========================================================================

/// Constellations order by RINEX letter, the order of today's char and one-letter keys (O5).
struct ByLetter {
    bool operator()(System a, System b) const { return traitsOf(a).letter < traitsOf(b).letter; }
};

/**
 * @brief The causes of a pending per-satellite wire discontinuity (decision D7): one bit of
 *        @ref SatelliteState::disc_pending per drain-on-emission queue.
 */
enum class Disc : unsigned {
    UraRetro    = 1u << 0,  ///< A post-emit QC tier's retro-bump (B1).  Record R.
    EphCutover  = 1u << 1,  ///< An ephemeris roll without its prior version.  Record X.
    CbExcursion = 1u << 2,  ///< A code-bias carry excursion (SPEC_day129_frame_guards 2.1).
    ClockAnchor = 1u << 3,  ///< A decoupled temporal clock anchor (Q3).
    CbReseed    = 1u << 4,  ///< A time-linked code-bias datum re-seed.
};

/// One signal family of a constellation, in @ref ConstellationState::channels: each member
/// has its own key space (the signal string its old key carried after the constellation,
/// O4), and a cell is present in a member exactly when the old map held (constellation, key).
struct ChannelState {
    /// Stable group key of the (constellation, signal) carry group (decomposition INC-4:
    /// plain-data tier-1 entities), numbered in first-sighting order from 1.  Was
    /// @c NetworkCarryState::carry_group_id.  Record GG.
    std::optional<int> group_id;
    /// The code-bias datum's previous-epoch solved values, satellite -> value.  The
    /// veteran-referenced sum-to-zero RHS and the IF-pin RHS read it at gauge construction;
    /// it is cleared and rebuilt from the solved shared vector at the D2 harvest (kept
    /// epochs only, so a departed satellite is dropped and its return re-seeds that satellite
    /// alone).  Snapshotted only under network.state_snapshot_datum_capture and restored only
    /// under network.resume_datum_restore (RS-3); otherwise a warm resume re-seeds, with a
    /// signal.  Key: the signal of "<constellation>|<signal>".  Was
    /// @c NetworkCarryState::code_bias_prev_.  Record CBP.
    std::optional<std::map<std::string, double>> code_bias_prev;
    /// The eq34 component datum receiver of the signal family last epoch (the old key was
    /// "<constellation>|<signal>").  Was @c NetworkCarryState::eq34_datum_rx.  Record EQD.
    std::optional<std::string> datum_rx;
    /// DD-trace reference age (section 6.12.5): satellite -> consecutive traced epochs the
    /// satellite has been present at >= 2 stations.  An absence erases the entry, so the
    /// count is a continuous run, not a lifetime total.  Advanced and read only inside the
    /// stride-gated dd-frac emit, so it stays empty on a run that does not trace.  Not
    /// snapshotted: the trace is a diagnostic, and a warm resume restarts its ages from zero;
    /// the first traced epoch after a seam then re-elects by the tie-break, which the run-end
    /// switch count shows.  Was @c NetworkCarryState::ddfrac_ref_age.  ColdStart.
    std::optional<std::map<std::string, long long>> ddfrac_ref_age;
    /// Last reference elected for the (constellation, signal): the switch detector.  Was
    /// @c NetworkCarryState::ddfrac_ref_prev.  ColdStart.
    std::optional<std::string> ddfrac_ref_prev;
    /// The family-difference partner the secondary-family channel's level was realized
    /// against at the last kept epoch, "" for the sum-to-zero row: the baseline a
    /// switch between the two rows is declared against, advanced only on kept epochs.
    /// Empty unless the family-difference arm is on under the time link.  Was
    /// @c NetworkCarryState::last_cb_family_partner ("<constellation>|<signal>"; step N4e2,
    /// re-placed from the satellite rows).  ColdStart.
    std::optional<std::string> last_cb_family_partner;
};

/// One constellation's carried state (step N4b).  The constellation's own fields are keyed
/// by @c System in letter order (O5); a member keyed below the constellation keeps the string
/// its old key carried after the constellation, in @ref ConstellationState::channels (O4).
struct ConstellationState {
    /// The clock pivot satellite of the constellation last epoch (handover detect).  Was
    /// @c NetworkCarryState::prev_pivot.  Record V.
    std::optional<std::string> prev_pivot;
    /// Persistent last-ever pivot (A1).  Merge-updated, never overwritten wholesale, so a
    /// constellation that vanishes for an epoch and returns with a different pivot still
    /// raises datum_changed.  Read only when network.pivot_vanish_return_signal is on
    /// (otherwise inert).  Was @c NetworkCarryState::last_pivot.  Record V.
    std::optional<std::string> last_pivot;
    /// The pivot the coverage election (network.pivot_election) left in force at the latest
    /// solved epoch.  An election runs when no incumbent (@c last_pivot, the pivot of the
    /// last emitted product) is registered and healthy; epochs that are solved but not
    /// emitted would otherwise elect again at every epoch, so the pick is held here and kept
    /// while it stays registered and healthy.  Written every solved epoch under the knob,
    /// never read without it.  Starts empty on every run, warm or cold: a resumed run has
    /// @c last_pivot from the snapshot.  ColdStart.
    std::optional<std::string> elected_pivot;
    /// The exact update (network.carry_coupling_exact_update): the pinned clock satellite and its
    /// pinned value at the stash.  Not snapshotted; seeded from the V rows by the warm
    /// resume.  Was @c NetworkCarryState::cxu_pivot.  ResumeSeeded.
    std::optional<std::pair<std::string, double>> cxu_pivot;
    /// The clock pivot the stashed posterior was pinned on (the frame the carried
    /// clocks are in).  Recorded at every stash and restored from the snapshot's pivots;
    /// unread when network.clock_pivot_frame_q is off.  Was @c NetworkCarryState::pfq_pivot.
    /// ResumeSeeded.
    std::optional<std::string> pfq_pivot;
    /// M, the accumulated common clock step (m): the sum over emitted epochs of the
    /// median, over the constellation's satellites with an emitted clock at both that epoch
    /// and the previous emitted epoch, of the emitted clock step.  Absent unless
    /// network.emit_pb_hold_phase_clock.  Was @c NetworkCarryState::pb_hold_frz_M.  ColdStart.
    std::optional<double> pb_hold_frz_M;
    /// The sticky component reference station name of the SD EWL search (A2's r0).  Was
    /// @c NetworkCarryState::sd_ewl_r0.  ColdStart.
    std::optional<std::string> sd_ewl_r0;
    /// The producer AR search's cycle-basis memory (step AGT-5,
    /// network/ar/ArCycleBasis.hpp): per family of the constellation, keyed by its bands
    /// (O4: a family spans several channels), the last search's links, basis, arc life
    /// tracks and DD identities, which the next search keeps its links from and maps its
    /// basis against.  Written and read at SB_NETAR only.  Empty and untouched unless
    /// network.ambiguity_resolution.cycle_basis.  Not snapshotted and not fingerprinted (the
    /// search is log-only): a resumed leg rebuilds, counted.  Was
    /// @c NetworkCarryState::ar_basis.  ColdStart.
    std::map<ArFamilyKey, ArBasisMemory> ar_basis;
    /// The members keyed below the constellation.
    std::map<std::string, ChannelState> channels;
};

/// One satellite's carried state, keyed by @c SatId in the string order of the old satellite
/// keys (O1; "" is the no-satellite @c SatId{}).  Step N4e1 fills its clock half: the clock
/// carries, the CTU level monitor, the re-admission windows, the URA and retro-bump state
/// and the clock baselines of the hold and the CB excursion test; step N4e2 the datum and QC
/// half.  A field is present exactly when the old map held the satellite; a set membership
/// is a flag.
struct SatelliteState {
    Lifecycle seen;  ///< Decision D4; stays Absent until something needs it.
    /// Two-state oscillator clock state per satellite (network.clock_model): the carried
    /// [offset, drift] and 2×2 covariance, persisted across epochs.  Empty and inert
    /// unless clk_model_on.
    /// per-sat carried [offset,drift]+cov+t_last (P0-3: t_last drives per-sat prediction/drift-FD across gaps)
    /// Was @c NetworkCarryState::clock_osc.  Record K.
    std::optional<ClockOsc> clock_osc;
    /// Per-satellite clock sanity monitor (network.clock_monitor): the previous emitted
    /// clock value and the continuous-GPS second it was stashed, per satellite, for the
    /// adjacent-epoch jump test.  Erased on refloat so the post-reinit clock re-baselines.
    /// sat -> {Δδtˢ (m), seen_s}
    /// Was @c NetworkCarryState::prev_clk.  Record J.
    std::optional<std::pair<double, double>> prev_clk;
    /// sats currently emitting c1=0 (ZOH); edge trigger for the R6 event (L2/T5)
    /// (a set membership: the flag).
    /// Was @c NetworkCarryState::c1_zoh_active.  Record C.
    bool c1_zoh_active{false};
    /// Clock-level monitor: satellite -> accumulated code-only level information I_L (1/m^2), undecayed
    /// Was @c NetworkCarryState::clm_info.  ColdStart.
    std::optional<double> clm_info;
    /// Clock-level monitor, constellation contrast: satellite -> code-only level information in the constellation contrast, 1/(s0^2 Q) summed (1/m^2)
    /// Was @c NetworkCarryState::clm_info_c.  Record LB.
    std::optional<double> clm_info_c;
    /// Clock-level monitor: satellite -> epochs of code information accumulated
    /// Was @c NetworkCarryState::clm_n.  Record LB.
    std::optional<int> clm_n;
    /// previous monitored epoch's clock per satellite (m), the innovation's prior mean
    /// Was @c NetworkCarryState::clm_prev_clk.  ColdStart.
    std::optional<double> clm_prev_clk;
    /// satellite -> row of clm_prev_pclk
    /// Was @c NetworkCarryState::clm_prev_pidx.  ColdStart.
    std::optional<int> clm_prev_pidx;
    /// per-satellite window sums
    /// Was @c NetworkCarryState::clm_win.  ColdStart.
    std::optional<ClockLevelInnovWin> clm_win;
    /// sat → n_loop_epochs of its last stash with a registered clock column (the re-admission detector)
    /// Was @c NetworkCarryState::readmit_last_seen_ep.  ColdStart.
    std::optional<long long> readmit_last_seen_ep;
    /// sat → loop epoch at which its post-re-admission coupling marginalization expires
    /// Was @c NetworkCarryState::readmit_marg.  ColdStart.
    std::optional<long long> readmit_marg;
    /// sat → continuous-GPS s of its last clock seed (the guard's "moved" attribution window)
    /// Was @c NetworkCarryState::readmit_seeded_s.  ColdStart.
    std::optional<double> readmit_seeded_s;
    /// sat -> epochs remaining of post-refloat URA inflation.
    /// Was @c NetworkCarryState::ura_refloat_horizon.  Record Y.
    std::optional<int> ura_refloat_horizon;
    /// The causes of a pending wire discontinuity, one bit per drain-on-emission queue
    /// (decision D7, @ref Disc): set when a source queues the satellite, cleared when its
    /// mark lands at an emitted epoch where the satellite is present (a satellite absent
    /// from the emission keeps the bit).  Each queue drains at its own place in the emit
    /// preparation, in satellite order.  Was @c NetworkCarryState::ura_retro_sats (Record
    /// R), @c eph_cutover_disc_pending (Record X), @c cb_excursion_pending_bump,
    /// @c clk_anchor_disc_pending and @c cb_reseed_disc_pending (Pending).
    unsigned disc_pending{0};
    /// Retro-bump (B1; P0-2/P1-3): per satellite, tier-independent, edge-triggered.  The
    /// satellite is flagged and already bumped (its @ref Disc::UraRetro bit is the pending
    /// bump), so a satellite that stays flagged during recovery is bumped once, on its
    /// rising edge, with no per-constellation reset storm.
    /// Was @c NetworkCarryState::retro_active.  Record A.
    bool retro_active{false};
    /// Satellite -> the clock (m) this producer emitted at that satellite's previous
    /// emission: the baseline the coupled-mark step above is differenced against.
    /// Maintained only while the hold is armed.
    /// Was @c NetworkCarryState::pb_hold_prev_clock_m.  ColdStart.
    std::optional<double> pb_hold_prev_clock_m;
    /// Satellite -> emitted clock (m) at the previous emitted epoch, holding
    /// only that epoch's satellites (a satellite absent there contributes no step).
    /// Was @c NetworkCarryState::pb_hold_frz_prev_clk.  ColdStart.
    std::optional<double> pb_hold_frz_prev_clk;
    /// sat -> previous epoch's solved clock (m)
    /// Was @c NetworkCarryState::cb_exc_prev_clk.  ColdStart.
    std::optional<double> cb_exc_prev_clk;

    // ---- Step N4e2: the datum and QC half ----

    /// The satellite's previous wire stamp (IODE), for the switch flag of the O
    /// trace and the run-end switch count.  Empty unless the native broadcast clock a
    /// priori is armed.  Was @c NetworkCarryState::nbp_prev_iode.  ColdStart.
    std::optional<int> nbp_prev_iode;
    /// The clock-monitor refloat flag at the last stash: hard-refloat mode skip-stashes the
    /// satellite, so the D2 stash cannot carry the flag.  Was
    /// @c NetworkCarryState::prev_clock_refloat_sats.  Record QR.
    bool prev_clock_refloat{false};
    /// The ionosphere-free pin pair (sig1, sig2) of the last emitted product, compared and
    /// advanced only at the emit site, so a withheld epoch leaves it untouched.  Populated
    /// only under emit_clock_disc; not snapshotted (a warm resume has no prior for its
    /// first emit).  Was @c NetworkCarryState::last_if_pin_sel.  ColdStart.
    std::optional<std::pair<std::string, std::string>> last_if_pin_sel;
    /// The pin pair hold of the last kept epoch, the baseline the begin and the
    /// end of a hold are declared against, advanced only on kept epochs.  Telemetry only,
    /// not snapshotted.  Was @c NetworkCarryState::last_if_pin_holds.  ColdStart.
    std::optional<NetworkSbasis::PinPairHold> last_if_pin_hold;
    /// The satellite participates in a datum direction constrained last epoch (the
    /// completion rows, standing invariant #7): present here but in no direction this epoch
    /// is the release edge.  Was @c NetworkCarryState::sbc_active_sats.  ColdStart.
    bool sbc_active{false};
    /// The broadcast-ephemeris IODE the satellite's carried clock state is referenced to,
    /// advanced at the eph-cutover re-level (before the solve, also on withheld epochs).
    /// Empty and inert unless brdc_rts.  Was @c NetworkCarryState::relevel_iode.  Record I.
    std::optional<int> relevel_iode;
    /// The IF-pin reference pair {sig1, sig2} that produced the code-bias datum's previous
    /// values (@ref ChannelState::code_bias_prev), with the same kept-epoch lifecycle.  Was
    /// @c NetworkCarryState::code_bias_ifpin_pair_.  Record CBI.
    std::optional<std::pair<std::string, std::string>> code_bias_ifpin_pair;
    /// The satellite's code-bias split has been harvested before: a later re-seed is a
    /// return (disc bump), not a first appearance.  Accumulates for the process.  Was
    /// @c NetworkCarryState::code_bias_sat_seen_.  Record CBS.
    bool code_bias_seen{false};
    /// Stood down (GL-2, section 24.19): the per-LOS declarations are refused and the
    /// satellite routes to the consensus-fault response.  Empty unless knobs.gl2_on; one
    /// epoch of evidence, re-earned.  Was @c NetworkCarryState::gl2_sat_down.  ColdStart.
    bool gl2_down{false};
    /// TOW at which the stand-down edge fired, for the release event's detail (GL-2).
    /// Was @c NetworkCarryState::gl2_sat_since.  ColdStart.
    std::optional<double> gl2_since;
    /// To prune from the next emission (GL-2), drained by @c networkSolPrep; rebuilt every
    /// screened epoch.  Empty when network.gross_los_sat_wire_withhold is off.  Was
    /// @c NetworkCarryState::gl2_wire_pending.  Pending.
    bool wire_prune_pending{false};
    /// A slip at the gauge-pinned datum receiver queued the satellite's shared phase-bias
    /// skip-stash (consumed and cleared at this epoch's stash).  Not snapshotted.  Was
    /// @c NetworkCarryState::datum_reref_pb_sats.  ColdStart.
    bool datum_reref_pb{false};
    /// The same slip queued the network-wide ambiguity reseed, applied at the first drain
    /// after the skip-stash.  Was @c NetworkCarryState::datum_reref_amb_sats.  ColdStart.
    bool datum_reref_amb{false};
    /// The satellite's stable vertex id in the invariant-carry graph (a disjoint high
    /// range, numbered in first-sighting order).  Was @c NetworkCarryState::carry_sat_vertex.
    /// Record SV.
    std::optional<int> carry_vertex;
};

/// One open INC-2 bridge: the key's gauge is realized by a soft shared row at
/// @c value instead of a tree-edge or anchor pin.
struct PinBridge {
    double value = 0.0;      ///< the held shared phase bias (m), from pb_level_prev at open time
    int    folds = 0;        ///< consecutive epochs this bridge has been folded (staleness bound)
    double opened_tow = 0.0; ///< TOW of the epoch it opened (trace only)
};

/// One (satellite, signal) key's emit-side phase-bias hold state.
struct PbHoldKey {
    double        held_m{0.0};       ///< The value every emission of this generation writes.
    std::uint32_t generation{0};     ///< 1-based; 0 never appears on a live entry.
    double        last_emit_s{0.0};  ///< Product content time (s) of this key's last emission.
    /// @c network.emit_pb_hold_phase_clock: the satellite's
    /// common-removed emitted clock @f$\tilde c = c - M@f$ at this generation's
    /// start.  The held emission is @c held_m + (c~(t) - frz_ref).  0 and unread
    /// unless the mode is armed.
    double        frz_ref{0.0};
    /// False when the generation started on an epoch without an emitted
    /// clock for the satellite (no reference; the key is held plainly).
    bool          frz_ok{false};
    /// The transfer written at this key's last held emission (m), reused
    /// on an epoch without an emitted clock for the satellite.
    double        frz_last{0.0};
    /// The key's clone died this generation of a cause other than the
    /// anchor's withdrawal or disengagement, and the key carries the level-aware hold's
    /// transfer from the switch on for the rest of the generation.
    bool          smh_reverted{false};
    /// The transfer written at this key's last held emission (m).
    double        smh_last{0.0};
    /// A clone left before LAH had a reference (a generation started
    /// without an emitted clock), so LAH's reference waits for the satellite's next
    /// emitted clock and starts from the last transfer (continuity of the transfer).
    /// With a reference, leaving a clone folds its passed motion into it instead
    ///.
    bool          smh_ref_pending{false};
    /// The key's last held emission wrote its clone's transfer (so a
    /// clone that is not live now is a switch to the bridge).
    bool          smh_on_clone{false};
    /// LAH's transfer at the key's last smoothed emission (m), the frame
    /// the clone's transfer is written in (LAH's minus the passed motion), reused
    /// on an epoch without an emitted clock for the satellite (section 10).
    double        smh_lah_last{0.0};
};

/// One satellite signal's carried state (step N4d), keyed by @c SatSignalId in (satellite,
/// signal) order: the order of the "<sat>_<signal>" (ambiguity) and "<sat>|<signal>" (hold)
/// keys it replaces, whose signal is a phase code (O1, O2).  A cell is present exactly when
/// the old map held the key.
struct SatSignalState {
    /// Kind → content time of the last reinit fold of the (satellite, signal), the old
    /// (kind, sat, signal) key below the satellite signal (O4), for multi-epoch-gap
    /// coverage.  An empty signal (the satellite-wide @c SignalId{}) is the wildcard: the clock-QC
    /// feeds (inflate_q stash marks, clock refloats) mark a whole satellite kind by
    /// kind, while the reinit-sources realization-step feeds mark one (sat, signal) family,
    /// because that is the granularity at which a phase-bias datum is re-referenced.
    /// buildCplRow probes the exact key, then the wildcard.
    /// Was @c NetworkCarryState::tu_reinit_hist.  ResumeSeeded.
    std::optional<std::map<SharedParamKind, double>> tu_reinit;
    /// The sticky incumbent anchor station name of the (satellite, signal), read when
    /// electing (kept while it still tracks the satellite), advanced at election
    /// (network.phase_datum_prioritized; empty and unread on the legacy path).  Written to
    /// the snapshot under network.state_snapshot_frame_capture and restored under
    /// network.resume_frame_restore.
    /// Was @c NetworkCarryState::anchor_incumbent.  Record PA.
    std::optional<std::string> anchor_incumbent;
    /// Sticky continuous target (R2): the pin target at which each incumbent anchor is
    /// currently realized, per (sat, signal) "sat_signal" key.  Without it the
    /// continuous handover is a one-epoch deferral: the ledger pin reverts to 0 at T+1
    /// and the frame steps anyway (measured null in §5.0).  Entries exist only for keys
    /// whose last handover was continuous (a cold pin or a first sighting realizes the
    /// 0-frame and stores nothing); rebuilt with anchor_incumbent at each election, so
    /// a key that leaves the union drops its target with its incumbent.  Empty unless
    /// `knobs.pd_handover_continuous`.  Written to the snapshot only under
    /// network.state_snapshot_frame_capture and restored only under
    /// network.resume_frame_restore; otherwise a resume's first handover is cold.
    /// Was @c NetworkCarryState::anchor_pin_target.  Record PT.
    std::optional<double> anchor_pin_target;
    /// Bridge source (INC-2): the last solved shared phase bias per "sat_signal",
    /// harvested at the D2 stash (kept epochs only, like code_bias_prev_).  Kept apart
    /// from prev_carry on purpose: under carry_invariant the shared PhaseBias carry is
    /// not stashed at all, so prev_carry would be an empty source there.
    /// Was @c NetworkCarryState::pb_level_prev.  ColdStart.
    std::optional<double> pb_level_prev;
    /// The open INC-2 bridge of the key (absent: no bridge).
    /// Was @c NetworkCarryState::pin_bridge.  ColdStart.
    std::optional<PinBridge> pin_bridge;
    /// The held S-basis ledger (RF-2 item 1): ambiguity key -> the frozen datum
    /// (@ref PhaseBasisEntry).  Written once per anchor tenure by the election and read
    /// by it on every later epoch of that tenure, so the pinned value is a constant
    /// rather than something re-derived from a moving carry.  Empty on every run
    /// without network.phase_datum_held_basis.
    /// Was @c NetworkCarryState::phase_basis.  ColdStart.
    std::optional<PhaseBasisEntry> phase_basis;
    /// "<sat>|<signal>" -> the held value and its generation (knob
    /// @c network.emit_pb_hold).  Owned here rather than in the seam because it must
    /// survive the epoch loop; empty and never touched when the knob is off.  Not
    /// snapshotted: a warm resume cold-starts every key as a first sighting, which
    /// advances the generation the consumer sees and so re-floats instead of silently
    /// re-seating a stale held value (the conservative direction, as for the slant-iono
    /// carry below).
    /// Was @c NetworkCarryState::pb_hold.  ColdStart.
    std::optional<PbHoldKey> pb_hold;
    /// Hold key ("sat|signal") -> its clock clone (network.emit_pb_hold_smoothed).
    /// A clone lives for its key's generation; a dead one stays until the next
    /// generation replaces it.  Empty and untouched unless the mode is armed.
    /// Was @c NetworkCarryState::smh_clone.  ColdStart.
    std::optional<PbHoldClone> smh_clone;
    /// Hold keys whose generation started at this epoch's emit, born at
    /// this epoch's stash (sat, signal and generation filled in).
    /// Was @c NetworkCarryState::smh_pending.  Pending.
    std::optional<PbHoldClone> smh_pending;
    /// Hold key ("sat|signal") -> its block clone (a clock row and the
    /// phase-visible row on one lifetime).  Carried whenever the smoothed hold is armed,
    /// as its instrument (the split of the passed motion); the wire never reads it.
    /// Was @c NetworkCarryState::smh_block.  ColdStart.
    std::optional<PbHoldClone> smh_block;
    /// Hold keys whose block clone waits for a birth (as @c smh_pending).
    /// Was @c NetworkCarryState::smh_block_pending.  Pending.
    std::optional<PbHoldClone> smh_block_pending;
    /// The anchor station name of the last emitted product, advanced only at emit (mirrors
    /// the pivots); a change against it drives the per-satellite handover discontinuity.
    /// Never erased: a returning key is a first sighting, not a fault.  Was
    /// @c NetworkCarryState::emitted_anchor ("sat_signal"; step N4e2, re-placed from the
    /// satellite rows).  ColdStart.
    std::optional<std::string> emitted_anchor;
    /// The continuous-GPS second at which the key's datum was last realized and emitted
    /// (eq34 reattach hygiene, warm conditioning): advanced at the emit site of
    /// @ref emitted_anchor and read only by filter/NetworkAnchorHygiene.  Empty unless the
    /// mechanism is armed; not snapshotted.  Was @c NetworkCarryState::emitted_anchor_s (step
    /// N4e2, re-placed from the satellite rows).  ColdStart.
    std::optional<double> emitted_anchor_s;
    /// The satellite signal's component datum receiver r1 last epoch: a root
    /// change shifts the whole component frame, so it re-references every satellite even
    /// where all tree edges survived.  Replaced wholesale per election.  Was
    /// @c NetworkCarryState::eq34_sat_root ("sat_signal"; step N4e2, re-placed from the
    /// satellite rows).  Record EQS.
    std::optional<std::string> eq34_sat_root;
};

/// The station-level dispositions (RF-3b B4, @c network.gross_los_station_tier),
/// keyed by station.  @c gross means every one of that station's code rows
/// folds at the deweight; @c sats is the satellite set the last screened epoch saw,
/// which @ref networkGrossLosPublishScales turns into per-satellite scales (the
/// assembly's map is per satellite, so a station-wide fold is expressed as "every
/// satellite this station tracks").  Empty unless @c knobs.gl_station_tier.
struct GrossLosStationState {
    bool   gross{ false };       ///< The station is declared.
    int    clear_streak{ 0 };    ///< Consecutive screened epochs at or under the cap while declared.
    double declared_tow{ 0.0 };  ///< TOW of the standing declaration.
    long long n_declared{ 0 };   ///< Declarations over the run for this station.
    std::set<std::string> sats;  ///< Satellites the station was last seen screening.
};

/// The drift half of the carried (bias, drift) pair for RX-CLK-in-block, keyed like
/// @c rx_clk_carry but populated only for the primary clock (kind ""); an ISB is a
/// near-constant bias and takes a pure random walk.  Kept in its own map rather
/// than added to @c LocalCarry, because @c LocalCarry is shared machinery used by
/// the other carries, none of which has a rate.
///
/// This state exists because the network producer never writes its solution back
/// into the station's parameter set: @c x_lin(clock) is set once (0, or the latched
/// cold_clock_seed median) and is constant for the run, so @c x_r carries the whole
/// clock including its ramp of about 0.3 m/s (9 m per 30 s epoch, about 30 sigma
/// against a bias-only prior).  The rate is a scalar Kalman state driven by the
/// harvested increments; it also supplies the one-step prediction whose innovation
/// is the clock-jump detector.
struct RxClkRate {
    double rate{0.0};          ///< Estimated drift, m/s (GPS-referenced, same frame as the clock state).
    double rate_var{-1.0};     ///< Its variance, m^2/s^2.  < 0 = never updated, so the injector uses network.carry_rx_clock_rate_var0_m2ps2.
    double prev_abs{0.0};      ///< Previous harvested absolute clock (m): the increment's left end.
    double prev_s{-1.0};       ///< Its time (s).  < 0 = no previous sighting.
    double prev_var{0.0};      ///< Its harvested variance (m^2), for the increment's measurement noise.
    long long n_incr{0};       ///< Increments folded.  0 = no rate yet, so the jump gate cannot fire (there is no prediction to test).
};

/// One station's carried state below the station, in @ref StationState::signals: each
/// member has its own key space (the string its old key carried after the station index),
/// and a cell is present in a member exactly when the old map held (station, key).
struct StationSignalState {
    /// The carried receiver-clock-family local columns for RX-CLK-in-block
    /// (@c network.carry_rx_clock_in_block), keyed (station index, kind), with kind ""
    /// for the primary receiver clock and the system name for each ISB.  The value is
    /// the absolute clock/ISB in metres (x_lin + x_r) as of @c last_seen_s, with the
    /// F3 bookkeeping (@c blk / @c blk_idx / @c coupling) that every in-block column
    /// carries.  Not snapshotted: a warm resume cold-starts it, which is one epoch of
    /// white-clock behaviour, and a restored entry would carry no block anyway.
    /// Key: the clock kind ("" for the primary clock, the system name for an ISB).  Was
    /// @c NetworkCarryState::rx_clk_carry.  ColdStart.
    std::optional<LocalCarry> rx_clk;
    /// The drift half of @ref rx_clk (the struct @ref RxClkRate documents it); the primary
    /// clock only.  Was @c NetworkCarryState::rx_clk_rate.  ColdStart.
    std::optional<RxClkRate> rx_clk_rate;
    /// Per-(station index, "sys_RPB_σ") receiver phase-bias carry for the eq34 chain datum:
    /// local time-linking of δ_{r,σ} (spec D6 v2 in its minimal local form, with no
    /// border promotion).  Re-estimating the RPB fresh each epoch injects
    /// anti-correlated per-epoch wander into every individual ambiguity float of that
    /// (station, σ): DDs cancel it exactly, but the per-key phase-leveling carry does
    /// not, which damages the datum-invariant DD steps.  Read-only during the parallel
    /// matrix build (fold), written at the serial post-solve stash, erased for dirty
    /// receiver vertices at the election (their chain frame stepped), and pruned on gap
    /// like local_carry.  Inert unless network.phase_datum_eq34 with
    /// phase_datum_eq34_rpb_q_m2s > 0.
    /// Key: the receiver phase-bias state key ("sys_RPB_signal").  Was
    /// @c NetworkCarryState::rpb_carry.  ColdStart.
    std::optional<LocalCarry> rpb;
    /// rx-CB carry: carried receiver code-bias posteriors, keyed (station index, CB
    /// state key).  Harvested outside the carry_warm bracket, under its own gate, since
    /// it must work on the carry_invariant / no-leveling lineage.  Inert unless
    /// network.carry_rx_code_bias.
    /// Key: the code-bias state key.  Was @c NetworkCarryState::rx_cb_carry.  Record RC.
    std::optional<LocalCarry> rx_cb;
    /// The receiver's parent-edge satellite in last epoch's eq34 spanning tree, for the
    /// signal family "<c>|<sigma>" (the key; the old key was "<station name>|<c>|<sigma>").
    /// With @c anchor_incumbent (the satellite side) it is last epoch's tree; replaced
    /// wholesale per election.  Was @c NetworkCarryState::eq34_rx_parent.  Record EQP.
    std::optional<std::string> eq34_rx_parent;
};

/// The SD EWL integer fixes of one station: constellation letter -> satellite -> fix.
using SdEwlFixes = std::map<char, std::map<std::string, SdEwlFix>>;

/// One station's carried state (step N4a).
///
/// The station's own fields are keyed by roster index (StationId); a member keyed below
/// the station keeps the string its old key carried after the station index (O4), in
/// @ref StationState::signals.  The receiver-side eq34 parent edges, keyed by station
/// NAME before N4a, are walked through @ref byStationName (O3).
struct StationState {
    Lifecycle seen;  ///< Decision D4; Absent: no owner computes a station presence edge on every run yet.
    /// Per-station ZWD carry (B9): persistent across epochs, keyed by station index (one
    /// ZWD per station).  Solve-inert unless network.carry_zwd (every-epoch inject) or a
    /// live zwd_seam_ep_left window (post-resume inject).  Also kept fresh (capture
    /// only, no inject) on any snapshot-armed run (knobs.zwd_capture), so every RTB-7
    /// snapshot serializes Z rows regardless of carry_zwd.
    /// Was @c NetworkCarryState::zwd_carry.  Record Z.
    std::optional<LocalCarry> zwd;
    /// The station-level gross line-of-sight disposition (the struct documents it).  Was
    /// @c NetworkCarryState::gl_sta_state.  ColdStart.
    std::optional<GrossLosStationState> gross_los;
    /// --- Slow channel of the declared per-station variance factor (QUEUE item 4 R2):
    /// an EWMA over emitted epochs.  The seam runs only after acceptance, so the factor
    /// freezes across withholds by construction.  Keyed by station index (the roster is
    /// stable).  Not snapshotted: a warm resume re-converges the EWMA over about τ, the
    /// conservative direction.
    /// EWMA of the per-epoch factor
    /// Was @c NetworkCarryState::staf_ewma.  ColdStart.
    std::optional<double> staf_ewma;
    /// EWMA update count (declaration warm-up).  Was @c NetworkCarryState::staf_updates.
    /// ColdStart.
    std::optional<long long> staf_updates;
    /// The station is past the depart edge.  Was @c NetworkCarryState::staf_departed.
    /// ColdStart.
    bool staf_departed{false};
    /// Last epoch's iono_sh_row count (INC-4); absent before the station's first sighting.
    /// Was @c NetworkCarryState::topo_prev_tie.  ColdStart.
    std::optional<int> topo_prev_tie;
    /// The local-column keys currently flagged unobservable (the decoupling-edge set; an
    /// entry appearing is an event, an entry disappearing is a counted re-couple).  Was
    /// @c NetworkCarryState::redun_unobs_active.  ColdStart.
    std::optional<std::set<std::string>> redun_unobs_active;
    /// Station indices whose first post-resume screened epoch has already happened:
    /// the per-station half of @c NetworkCarryState::resume.adm.  Bounded by the station roster;
    /// empty and unread on a cold start.
    /// Was @c NetworkCarryState::adm_resume_seen.  ColdStart.
    bool adm_resume_seen{false};
    /// The snapshot's RL rows for the RS-9 restore (§21.18): station roster index ->
    /// (CB state key -> the writer's @c x_lin, metres).  Stashed by
    /// @c networkTryWarmResume beside the RS-6 RC restore and consumed once, by
    /// @c networkApplyRxCbLinRestore, which installs each station's map on its own
    /// parameter set before the first epoch.  Empty on every other run.
    /// CB state key -> the writer's @c x_lin (m).  Was
    /// @c NetworkCarryState::rxcb_lin_restore.  ResumeSeeded.
    std::optional<std::map<std::string, double>> rxcb_lin_restore;
    /// Snapshot: exempt (RS-8 §21.16); a resume-response staging buffer, filled by
    /// networkTryWarmResume and drained by networkRestoreCanonicalSignals in the same
    /// process.  The parsed CN rows, keyed by station roster index, wait here for the
    /// driver to hand them to the station models (networkTryWarmResume has no
    /// `stations` and must not grow one; the restore is the seam right after it).  Empty
    /// on every run that does not resume with network.resume_signal_canon_restore armed.
    /// Was @c NetworkCarryState::resume_canon_rows.  ResumeSeeded.
    std::optional<std::vector<PppModel::CanonicalSignal>> resume_canon_rows;
    /// The SD EWL integer fixes published for PASS B (increment 2a), constellation letter ->
    /// satellite -> fix; cleared every epoch before the decision (O4: the old key was
    /// (station index, constellation)).  Not snapshotted (the SD EWL block of
    /// NetworkCarryState states why).  Was @c NetworkCarryState::sd_ewl_fix.  ColdStart.
    std::optional<SdEwlFixes> sd_ewl_fix;
    /// Census-epoch index of the station's last emitted CONGRUENCE event (the rate limit).
    /// Was @c NetworkCarryState::cg_event_last (keyed by station NAME, O3; step N4e2,
    /// re-placed from the satellite rows).  ColdStart.
    std::optional<long long> cg_event_last;
    /// Ambiguity key ("sat_signal") -> the loop epoch of that arc's last screen flag: a slip
    /// response, a T4 / T3 refloat, or a gross-LOS declaration (RF-2 items 3 and 4), read by
    /// the held basis's quiet-window gate.  Written only when the held basis is armed.  The
    /// old key (station index, ambiguity key) keeps its string below the station (O4).  Was
    /// @c NetworkCarryState::amb_flag_ep (step N4f1, re-placed from the arc rows).  ColdStart.
    std::map<std::string, long long> amb_flag_ep;
    /// Channel key ("<sat>|<band>") -> the measurement admission record of the channel
    /// (product-referenced admission), aged out after kAdmArcForgetEp unseen epochs.  Not
    /// serialized (the mechanism is not fingerprinted).  Was @c NetworkCarryState::adm_arc
    /// (step N4f1, re-placed from the arc rows; O4).  ColdStart.
    std::map<std::string, MeasurementAdmissionArc> adm_arc;
    /// "event|sat|band" -> the processed-epoch index of its last emitted ADMISSION event
    /// (the rate limit of the INC-3 census and the guard event).  Was
    /// @c NetworkCarryState::adm_event_last ("event|station|sat|band", the station by NAME,
    /// O3; step N4f1, re-placed from the arc rows; O4).  ColdStart.
    std::map<std::string, long long> adm_event_last;
    /// Unit -> the processed-epoch index at which its current second-band withhold began
    /// (network.second_band_admission).  A unit is present exactly while it is withheld: the
    /// constellation name under station scope ("BeiDou"), the satellite under satellite scope
    /// ("C06"); the two key spaces cannot meet because one scope runs at a time.  A return is
    /// the re-coupling edge, so the entry is erased at it.  Not serialized (the mechanism is not
    /// fingerprinted); a warm resume starts every unit admitted and the first withheld epoch
    /// raises its decoupling edge again.  New in the second-band admission.  ColdStart.
    std::map<std::string, long long> sba_since;
    /// The members keyed below the station.
    std::map<std::string, StationSignalState> signals;
};

/// The event holdoff clocks of one line of sight, in @ref LosState::events: each member has
/// its own key space (the part of its old key besides the station and the satellite), and a
/// cell is present in a member exactly when the old map held the key.  They thin the event
/// streams, never the counts.
struct LosEventState {
    /// Census pass of the last emitted uncheckable-hot-cell event (holdoff
    /// network.redundancy_event_holdoff_ep).  Key: the row class.  Was
    /// @c NetworkCarryState::redun_event_last ("station\x1fsat\x1fclass").  ColdStart.
    std::optional<long long> redun_event_last;
    /// MDB sequence of the last emitted MDB event.  Key: the row class.  Was
    /// @c NetworkCarryState::mdb_event_last ("station\x1fsat\x1fclass").  ColdStart.
    std::optional<long long> mdb_event_last;
    /// Census-epoch index of the last emitted PRIORRES event.  Key: "family\x1fsignal".
    /// Was @c NetworkCarryState::prires_event_last ("family\x1fstation\x1fsat\x1fsignal").
    /// ColdStart.
    std::optional<long long> prires_event_last;
    /// Processed-epoch index of the last emitted PREFIT event (INC-1 and INC-2 rate limit).
    /// Key: "<event>|<class>".  Was @c NetworkCarryState::prefit_event_last
    /// ("<event>|<station>|<sat>|<class>", "?" for no satellite).  ColdStart.
    std::optional<long long> prefit_event_last;
};

/// One line of sight's carried state (step N4c), keyed (station, satellite) in that order
/// (the order of the old (station index, satellite) keys, O1).  The satellite of a station's
/// own rows (a receiver row of a census, whose old key carried an empty satellite) is the
/// no-satellite identity @c SatId{}.  Members keyed by station NAME before N4c (@ref sh_dev
/// and the event clocks) convert the name to its StationId (O3).
struct LosState {
    /// Last epoch's arc elevation (deg), for the rising-vs-setting test that ranks new
    /// spanning-tree edges by how long they will survive.  Rebuilt each epoch from sat_el;
    /// empty and unread unless network.phase_datum_eq34_longevity_election.  Was
    /// @c NetworkCarryState::eq34_prev_el.  Record EQE.
    std::optional<double> eq34_prev_el;
    /// The signal-integrity arc record (flicker/gap signal integrity).  Empty unless
    /// ctx.knobs.sig_integrity_on.  Not serialized: a resumed leg's first epoch sees every
    /// LOS as a first sighting and re-vets it, the conservative direction.  Was
    /// @c NetworkCarryState::si_arc.  ColdStart.
    std::optional<SignalIntegrityArc> si_arc;
    /// I-in-block per-LOS slant-iono carry (network.carry_iono_in_block): the absolute slant
    /// delay in metres at L1 (x_lin + x_r), with the F3 bookkeeping (blk / blk_idx /
    /// coupling / birth_s), folded as a column of the station's whitened block like a carried
    /// ambiguity.  Not snapshotted: a warm resume cold-starts it.  Was
    /// @c NetworkCarryState::iono_blk_carry.  ColdStart.
    std::optional<LocalCarry> iono_blk;
    /// The gross line-of-sight disposition (docs/SPEC_event_harness.md section
    /// 24.18): the streaks of every screened line of sight and the declared subset, pruned on
    /// the absent-satellite edge (network.gross_los_max_gap_s).  Empty unless knobs.gl_on.
    /// Not snapshotted and not fingerprinted (two epochs of evidence, re-earned).  Was
    /// @c NetworkCarryState::gl_state.  ColdStart.
    std::optional<GrossLosState> gross_los;
    /// Last gross-LOS event sequence, for network.gross_los_event_holdoff_ep.
    /// Empty at the default holdoff 0.  Was @c NetworkCarryState::gl_event_last
    /// ("station\x1fsat").  ColdStart.
    std::optional<long long> gl_event_last;
    /// Census-epoch index of the last emitted BLOCKBAARDA event of the line of sight (the
    /// rate limit).  Was @c NetworkCarryState::bb_event_last ("station\x1fsat", the station
    /// by NAME, O3; step N4e2, re-placed from the satellite rows).  ColdStart.
    std::optional<long long> bb_event_last;
    /// The held in-block per-LOS iono carry, the held twin of @ref iono_blk (RF-3b B2).  Was
    /// @c NetworkCarryState::gl_hold_iono_blk.  ColdStart.
    std::optional<LocalCarry> gl_hold_iono_blk;
    /// The CS-GF level-authority recursion of the line of sight
    /// (network.level_authority_csgf, the LV-6 census).  Was @c NetworkCarryState::iono_cs.
    /// ColdStart.
    std::optional<IonoCsState> iono_cs;
    /// The phase-colour carry.  Was @c NetworkCarryState::pc_carry.  ColdStart.
    std::optional<LocalCarry> pc;
    /// Consecutive post-fit RAIM spikes of the arc (network.raim_arc_persist_epochs > 1).
    /// Was @c NetworkCarryState::arc_spike_streak.  ColdStart.
    std::optional<int> arc_spike_streak;
    /// The Iono-2 per-LOS SH deviation carry (T5v2).  Was @c NetworkCarryState::sh_dev_carry
    /// (keyed "<station name>|<sat>", O3).  Record SD.
    std::optional<ShDevCarry> sh_dev;
    /// The SD EWL flip memory: (integer, the detector's lagged count).  Was
    /// @c NetworkCarryState::sd_ewl_prev (keyed (station, constellation, satellite); the
    /// constellation is the satellite's).  ColdStart.
    std::optional<std::pair<long long, std::size_t>> sd_ewl_prev;
    /// Last epoch's data-class bitmask (band gained/lost, code-only <-> code+phase, INC-4).
    /// Was @c NetworkCarryState::topo_prev_kind.  ColdStart.
    std::optional<unsigned> topo_prev_kind;
    /// Seen in the last epoch's prefit walk; absence on a present LOS is the birth (INC-3).
    /// Was @c NetworkCarryState::prefit_present_prev.  ColdStart.
    bool prefit_present_prev{false};
    /// Flagged by the last prefit walk's code census (the admission re-vet trigger).  Was
    /// @c NetworkCarryState::prefit_code_flag.  ColdStart.
    bool prefit_code_flag{false};
    /// The event holdoff clocks keyed below the line of sight.
    std::map<std::string, LosEventState> events;
    /// The bridge v2 pend store, keyed by the signal pair "sig1_sig2" (O4): persistent
    /// across epochs (the MW integer arrives one or two epochs after the T1 flag), swept for
    /// expiry and replacement at the top of the MW drain.  Not snapshotted.  Was
    /// @c NetworkCarryState::relevel_pend ((station index, sat, "sig1_sig2"); step N4f1,
    /// re-placed from the arc rows).  ColdStart.
    std::map<std::string, RelevelPend> relevel_pend;
};

/// One arc's carried state (step N4f1: the carries; N4f2 adds the ledger's record), keyed by
/// @c ArcId in (station, satellite, signal) order: the order of the (station index,
/// satellite, signal) and (station index, "sat_signal") keys it replaces, whose signal is a
/// phase code (O1, O2).  A field is present exactly when the old map held the key; a set
/// membership is a flag.  The members keyed below the arc by a string no typed key
/// reproduces keep that key on the owner of their typed prefix (O4):
/// @ref StationState::amb_flag_ep, @ref StationState::adm_arc,
/// @ref StationState::adm_event_last and @ref LosState::relevel_pend.
struct ArcState {
    /// Decision D4, filled by the arc ledger's machine (step N4f2): Present at the first
    /// sighting, Departed when a gap opens, Returned at the next sighting.  The row is never
    /// erased (D8), so the lifecycle outlives the ledger's forget window: a return after it
    /// is Returned here while the ledger reports a first sighting.  Read by no output.
    Lifecycle seen;
    /// The arc's lifecycle record: its raw and admitted states, gap
    /// lengths, generations and reason masks, advanced once per epoch by the ledger's
    /// machine (@c ctx.arc_ledger, network/state/ArcLedger.hpp) and read by the roster's audit,
    /// the re-entry response, the anchor hygiene and @c arcLifeStamp.  Present from the
    /// first sighting until the forget window resets it.  Was the record map of
    /// @c ArcLedger (not a @c NetworkCarryState member).  ColdStart: not snapshotted.
    std::optional<ArcRecord> ledger;
    /// The phase-leveling ambiguity carry (increment 1): persistent across epochs.  The
    /// table is ordered, so the inject order (which feeds rank-1 LDLT updates) is
    /// byte-identical serial and parallel.  Inert unless network.phase_leveling.  Was
    /// @c NetworkCarryState::local_carry.  Record L.
    std::optional<LocalCarry> amb;
    /// The gross screen's held ambiguity (network.gross_los_bound): written once at the
    /// declaration from the pre-declaration entry with its border coupling and joint block
    /// cleared, re-applied over @ref amb after every harvest while the declaration stands,
    /// reset at the release or the expiry.  Empty unless knobs.gl_bound.  Was
    /// @c NetworkCarryState::gl_hold_amb.  ColdStart.
    std::optional<LocalCarry> gl_hold_amb;
    /// Last epoch's eq34 pin target: the same-station pinmove baseline of the
    /// shift walk, keyed per station because an interior satellite carries several pins.
    /// Replaced wholesale each election.  Was @c NetworkCarryState::eq34_prev_target.
    /// Record EQT.
    std::optional<double> eq34_prev_target;
    /// A tree edge retained across the current gap because its leveling carry is still
    /// alive (network.phase_datum_eq34_extend_only); rebuilt at each election.  Was
    /// @c NetworkCarryState::eq34_retained_edges.  Record EQR.
    bool eq34_retained_edge{false};
    /// Consecutive epochs the arc has been present with an anchorable phase-bias state
    /// (the eq34 reattach hygiene): rebuilt wholesale each election, so an absent arc drops
    /// out; zero on a slip-funnel hit or a pending reseed (standing invariant #1).  Was
    /// @c NetworkCarryState::anchor_arc_age.  ColdStart.
    std::optional<int> anchor_arc_age;
};

/// An entity table: an ordered map from the identity to the row.
template <class Id, class Row, class Cmp = std::less<Id>>
using Table = std::map<Id, Row, Cmp>;

/// The constellation table, in letter order (O5).
using ConstellationTable = Table<System, ConstellationState, ByLetter>;

/// The station table.
using StationTable = Table<StationId, StationState>;

/// The line-of-sight table, in (station, satellite) order.
using LosTable = Table<LosId, LosState>;

/// The satellite table, in satellite order.
using SatTable = Table<SatId, SatelliteState>;

/// The satellite-signal table, in (satellite, signal) order.
using SatSignalTable = Table<SatSignalId, SatSignalState>;

/// The arc table, in (station, satellite, signal) order.
using ArcTable = Table<ArcId, ArcState>;

/// The old key of an arc's leveling carry: (station index, satellite, phase signal).
using ArcTuple = std::tuple<int, std::string, std::string>;

// ===========================================================================
// Station names and the identity converters
// ===========================================================================

/**
 * @brief The run's station names in roster order, for the members keyed by station NAME
 *        (design O3).  Filled once at setup and constant for the run.
 */
struct StationNames {
    std::vector<std::string>   names;    ///< Roster order: names[i] is StationId{i}.
    std::vector<int>           by_name;  ///< Roster indices in name order.
    std::map<std::string, int> index;    ///< Name to roster index.
    /// Replaces the roster with @p roster_names (roster order).
    void assign(std::vector<std::string> roster_names);
};

/// The identity converters the reach table counts round-trip failures for (O2).
enum class Converter : std::uint8_t {
    StationName,  ///< A station name to its StationId (name keys, O3).
    ConstellationLetter,  ///< A constellation letter (char or one-letter string) to its System (O5).
    Satellite,    ///< A satellite string to its SatId ("" is the no-satellite SatId{}, O2).
    SatSignal,    ///< A "<sat>_<signal>" / "<sat>|<signal>" key to its SatSignalId (O2).
    Arc,          ///< A (station index, satellite, signal) key to its ArcId (O2).
    Count
};

/// The name of a converter, as the reach table prints it.
const char* converterName(Converter c);

/**
 * @brief The reach table's own instrument state: the maximum present rows of every declared
 *        field over the sampled epochs, and the round-trip failures per converter.  Trace
 *        class: written by the sampler and the converters, read only by the run-end report.
 */
struct EntityReach {
    std::vector<std::size_t> max_rows;  ///< Per declared field, in the order of @ref kEntityFieldTables.
    long long                sampled{0};
    std::array<long long, static_cast<std::size_t>(Converter::Count)> failures{};
};

/**
 * @brief Every entity table of the run, the station names they convert through, and the
 *        reach table's state.
 */
struct NetworkEntities {
    ConstellationTable                          constellations;
    SatTable                                    sats;
    SatSignalTable                              sat_signals;
    StationTable                                stations;
    LosTable                                    los;
    ArcTable                                    arcs;
    StationNames                                names;   ///< Run constant (setup).
    mutable EntityReach                         reach;   ///< Trace class.
};

/**
 * @brief The station of a name, counting a failure when the roster has no such name.
 * @param e    The entities (their names, and the converter tally).
 * @param name A station name.
 * @return The station, or @c std::nullopt (counted under Converter::StationName).
 */
std::optional<StationId> stationOfName(const NetworkEntities& e, const std::string& name);

/**
 * @brief The constellation of a RINEX letter, counting a failure when the letter does not
 *        round-trip (O2: @c rinexLetter(systemFromLetter(c)) == c).
 * @param e      The entities (the converter tally).
 * @param letter A constellation letter ('G', 'E', ...).
 * @return The constellation, or @c std::nullopt (counted under Converter::ConstellationLetter).
 */
std::optional<System> constellationOfLetter(const NetworkEntities& e, char letter);

/**
 * @brief The constellation of a one-letter key ("G"), counting a failure when the key is not
 *        one round-tripping letter.
 * @param e   The entities (the converter tally).
 * @param key A one-letter constellation key, as @c constellationKey writes it.
 * @return The constellation, or @c std::nullopt (counted under Converter::ConstellationLetter).
 */
std::optional<System> constellationOfKey(const NetworkEntities& e, const std::string& key);

/**
 * @brief Splits a "<constellation>|<signal>" key into its constellation and signal,
 *        counting a failure when the key has no one-letter constellation before a bar.
 * @param e   The entities (the converter tally).
 * @param key The channel key.
 * @return (constellation, signal), or @c std::nullopt (counted).
 */
std::optional<std::pair<System, std::string>> channelOfKey(const NetworkEntities& e,
                                                           const std::string& key);

/**
 * @brief A constellation-keyed string field as the one-letter-keyed map its readers take
 *        (the pivot maps: @c computeClockPivotPlan, the trace formatter), in letter order.
 * @param t The constellation table.
 * @param f The field.
 * @return One-letter key -> value, for every row where the field is present.
 */
std::map<std::string, std::string> letterMap(const ConstellationTable& t,
                                             std::optional<std::string> ConstellationState::*f);

/**
 * @brief A channel field as the "<letter>|<signal>"-keyed map its readers take (the
 *        code-bias datum builder, @c NetworkSbasis), in (letter, signal) order.
 * @param t The constellation table.
 * @param f The channel field.
 * @return "<letter>|<signal>" -> value, for every cell where the field is present.
 */
template <class V>
std::map<std::string, V> channelMap(const ConstellationTable& t, std::optional<V> ChannelState::*f);

/**
 * @brief Replaces a constellation-keyed string field with a one-letter-keyed map: the old
 *        map's assignment (@c prev_pivot = plan.clock_pivot).  Serial code only.
 * @param e The entities (the table, and the converter tally for a key that is not a letter).
 * @param f The field.
 * @param m One-letter key -> value.
 */
void assignLetterMap(NetworkEntities& e, std::optional<std::string> ConstellationState::*f,
                     const std::map<std::string, std::string>& m);

/**
 * @brief Merges a one-letter-keyed map into a constellation-keyed string field without
 *        resetting the constellations it does not hold (the last-ever pivot, A1).  Serial
 *        code only.
 * @param e The entities.
 * @param f The field.
 * @param m One-letter key -> value.
 */
void mergeLetterMap(NetworkEntities& e, std::optional<std::string> ConstellationState::*f,
                    const std::map<std::string, std::string>& m);

/**
 * @brief The line of sight of (station index, satellite string), without counting: the
 *        reader's converter (safe in the parallel produce).  "" is the no-satellite
 *        @c SatId{}; any other string must round-trip through @c SatId (O2).
 * @param station The station's roster index.
 * @param sat     The satellite ("G05"), or "" for a station's own rows.
 * @return The line of sight, or @c std::nullopt when the satellite does not round-trip.
 */
std::optional<LosId> losKey(int station, const std::string& sat);

/**
 * @brief The line of sight of (station index, satellite string), counting a failure under
 *        Converter::Satellite (the writer's converter; serial code).
 * @param e       The entities (the converter tally).
 * @param station The station's roster index.
 * @param sat     The satellite, or "".
 * @return The line of sight, or @c std::nullopt (counted).
 */
std::optional<LosId> losOf(const NetworkEntities& e, int station, const std::string& sat);

/**
 * @brief The line of sight of (station NAME, satellite string) (O3), counting either
 *        converter's failure (serial code).
 * @param e       The entities (the names and the converter tallies).
 * @param station A station name.
 * @param sat     The satellite, or "".
 * @return The line of sight, or @c std::nullopt (counted).
 */
std::optional<LosId> losOfName(const NetworkEntities& e, const std::string& station,
                               const std::string& sat);

/**
 * @brief The satellite signal of (satellite, signal) strings, without counting: the
 *        reader's converter (safe in the parallel produce).  The signal is a phase code
 *        ("L1C") or "", the satellite-wide @c SignalId{} (the wildcard of
 *        @ref SatSignalState::tu_reinit); a satellite "" is the no-satellite @c SatId{}.
 *        An empty component keeps its row; any other string must round-trip (O2).
 * @param sat    The satellite ("G05"), or "".
 * @param signal The phase code, or "".
 * @return The satellite signal, or @c std::nullopt when either string does not round-trip.
 */
std::optional<SatSignalId> satSignalKey(const std::string& sat, const std::string& signal);

/**
 * @brief The satellite signal of an old key, "<sat>_<signal>" (the ambiguity form) or
 *        "<sat>|<signal>" (the hold form), split at the first '_' or '|' (no satellite
 *        string holds either), without counting.
 * @param key The old key.
 * @return The satellite signal, or @c std::nullopt when the key does not round-trip.
 */
std::optional<SatSignalId> satSignalKey(const std::string& key);

/**
 * @brief The satellite and phase signal of a satellite phase-bias key, "<sat>|<signal>|PB"
 *        ("G05|L1C|PB"), without counting: the text a border column's key and the
 *        second-phase scope's cells carry.
 *
 * The satellite and the signal read as the station parameter set's classifier reads them
 * (the satellite id, the signal of the satellite's constellation), so a key the set holds
 * converts to the identity it is stored under.
 *
 * @param key The key.
 * @return The satellite signal, or @c std::nullopt when the key has no "|PB" suffix or
 *         either part does not read as an identity.
 */
std::optional<SatSignalId> satPhaseBiasKey(const std::string& key);

/**
 * @brief The satellite signal of an old key, counting a failure under
 *        Converter::SatSignal (the writer's converter; serial code).
 * @param e   The entities (the converter tally).
 * @param key The old key, in either form.
 * @return The satellite signal, or @c std::nullopt (counted).
 */
std::optional<SatSignalId> satSignalOf(const NetworkEntities& e, const std::string& key);

/// The satellite string of a satellite: "" for the no-satellite @c SatId{}.
std::string satKey(const SatId& s);

/// The signal string of a satellite signal: its phase code, or "" for @c SignalId{}.
std::string signalKey(const SignalId& s);

/// The ambiguity-form key "<sat>_<signal>" of a satellite signal.
inline std::string ambKey(const SatSignalId& id) { return net::satKey(id.sat) + "_" + signalKey(id.signal); }

/// The hold-form key "<sat>|<signal>" of a satellite signal.
inline std::string holdKey(const SatSignalId& id) { return net::satKey(id.sat) + "|" + signalKey(id.signal); }

/// The satellite string of a line of sight's satellite: "" for the no-satellite @c SatId{}.
std::string satKey(const SatId& s);

/// The old (station index, satellite) key of a line of sight.
inline std::pair<int, std::string> losPair(const LosId& id) {
    return {id.station.index, satKey(id.sat)};
}

// ===========================================================================
// Field access: free functions over a table
// ===========================================================================

/// A pointer to @p V, const when @p T is const.
template <class T, class V>
using FieldPtr = std::conditional_t<std::is_const_v<T>, const V*, V*>;

/**
 * @brief The value of field @p f of row @p id, or null when the row or the field is
 *        absent: the old map's @c find.
 */
template <class T, class R, class V>
FieldPtr<T, V> get(T& t, const typename std::remove_const_t<T>::key_type& id,
                   std::optional<V> R::*f) {
    const auto it = t.find(id);
    if (it == t.end() || !(it->second.*f)) return nullptr;
    return &*(it->second.*f);
}

/**
 * @brief The value of field @p f of row @p id, created with the row when absent: the old
 *        map's @c operator[].  Serial code only (it may insert a row).
 */
template <class T, class R, class V>
V& put(T& t, const typename T::key_type& id, std::optional<V> R::*f) {
    std::optional<V>& o = t[id].*f;
    if (!o) o.emplace();
    return *o;
}

/// Resets field @p f of row @p id; 1 when it was present: the old map's @c erase(key).
template <class T, class R, class V>
std::size_t drop(T& t, const typename T::key_type& id, std::optional<V> R::*f) {
    const auto it = t.find(id);
    if (it == t.end() || !(it->second.*f)) return 0;
    (it->second.*f).reset();
    return 1;
}

/// Resets field @p f on every row: the old map's @c clear().
template <class T, class R, class V>
void dropAll(T& t, std::optional<V> R::*f) {
    for (auto& kv : t) (kv.second.*f).reset();
}

/// The number of rows where field @p f is present: the old map's @c size().
template <class T, class R, class V>
std::size_t present(const T& t, std::optional<V> R::*f) {
    std::size_t n = 0;
    for (const auto& kv : t) n += (kv.second.*f) ? 1 : 0;
    return n;
}

/**
 * @brief Calls @p fn(id, value) for every row where field @p f is present, in identity
 *        order: the old map's range loop.  @p fn must not insert rows.
 */
template <class T, class R, class V, class Fn>
void forEach(T& t, std::optional<V> R::*f, Fn fn) {
    for (auto& kv : t)
        if (kv.second.*f) fn(kv.first, *(kv.second.*f));
}

/**
 * @brief @ref forEach for a reader that also reads the row's other fields: calls
 *        @p fn(id, value, row) for every row where field @p f is present, in identity order
 *        (one row holds an arc's ledger record and its carry, step N4f2).  @p fn must not
 *        insert rows; it may reset field @p f of the row it is given.
 */
template <class T, class R, class V, class Fn>
void forEachRow(T& t, std::optional<V> R::*f, Fn fn) {
    for (auto& kv : t)
        if (kv.second.*f) fn(kv.first, *(kv.second.*f), kv.second);
}

/// True when the set-membership field @p f of row @p id is set: the old set's @c count.
template <class T, class R>
bool has(const T& t, const typename T::key_type& id, bool R::*f) {
    const auto it = t.find(id);
    return it != t.end() && it->second.*f;
}

/// Sets field @p f of row @p id; true when it was not set: the old set's @c insert().second.
template <class T, class R>
bool mark(T& t, const typename T::key_type& id, bool R::*f) {
    bool& b = t[id].*f;
    const bool inserted = !b;
    b = true;
    return inserted;
}

/// Clears field @p f of row @p id; 1 when it was set: the old set's @c erase(key).
template <class T, class R>
std::size_t unmark(T& t, const typename T::key_type& id, bool R::*f) {
    const auto it = t.find(id);
    if (it == t.end() || !(it->second.*f)) return 0;
    it->second.*f = false;
    return 1;
}

/// The number of rows where field @p f is set: the old set's @c size().
template <class T, class R>
std::size_t present(const T& t, bool R::*f) {
    std::size_t n = 0;
    for (const auto& kv : t) n += (kv.second.*f) ? 1 : 0;
    return n;
}

// ---- Fields keyed below the entity: a nested map of the row (a station's signals) ----

/// The value of nested field @p f at (@p id, @p k), or null: the old pair-keyed map's @c find.
template <class T, class R, class M, class SR, class V>
FieldPtr<T, V> get(T& t, const typename std::remove_const_t<T>::key_type& id, M R::*sub,
                   const typename M::key_type& k, std::optional<V> SR::*f) {
    const auto it = t.find(id);
    if (it == t.end()) return nullptr;
    const auto jt = (it->second.*sub).find(k);
    if (jt == (it->second.*sub).end() || !(jt->second.*f)) return nullptr;
    return &*(jt->second.*f);
}

/// The value of nested field @p f at (@p id, @p k), created when absent: @c operator[].
template <class T, class R, class M, class SR, class V>
V& put(T& t, const typename T::key_type& id, M R::*sub, const typename M::key_type& k,
       std::optional<V> SR::*f) {
    std::optional<V>& o = (t[id].*sub)[k].*f;
    if (!o) o.emplace();
    return *o;
}

/// Resets nested field @p f at (@p id, @p k); 1 when it was present: @c erase(key).
template <class T, class R, class M, class SR, class V>
std::size_t drop(T& t, const typename T::key_type& id, M R::*sub,
                 const typename M::key_type& k, std::optional<V> SR::*f) {
    const auto it = t.find(id);
    if (it == t.end()) return 0;
    const auto jt = (it->second.*sub).find(k);
    if (jt == (it->second.*sub).end() || !(jt->second.*f)) return 0;
    (jt->second.*f).reset();
    return 1;
}

/// Resets nested field @p f everywhere: @c clear().
template <class T, class R, class M, class SR, class V>
void dropAll(T& t, M R::*sub, std::optional<V> SR::*f) {
    for (auto& kv : t)
        for (auto& sk : kv.second.*sub) (sk.second.*f).reset();
}

/// The number of (row, sub-key) cells where nested field @p f is present: @c size().
template <class T, class R, class M, class SR, class V>
std::size_t present(const T& t, M R::*sub, std::optional<V> SR::*f) {
    std::size_t n = 0;
    for (const auto& kv : t)
        for (const auto& sk : kv.second.*sub) n += (sk.second.*f) ? 1 : 0;
    return n;
}

/// Resets field @p f on every row where @p fn(id, value) returns true, in identity order:
/// the old map's erase-while-iterating loop.  @p fn must not insert rows.
template <class T, class R, class V, class Fn>
void eraseIf(T& t, std::optional<V> R::*f, Fn fn) {
    for (auto& kv : t)
        if ((kv.second.*f) && fn(kv.first, *(kv.second.*f))) (kv.second.*f).reset();
}

// ---- Lines of sight addressed by the old (station index, satellite) key ----

/// The value of field @p f of the line of sight (@p station, @p sat), or null (no count).
template <class T, class V>
FieldPtr<T, V> getLos(T& t, int station, const std::string& sat, std::optional<V> LosState::*f) {
    const std::optional<LosId> id = losKey(station, sat);
    return id ? get(t, *id, f) : nullptr;
}

/// True when set-membership field @p f of (@p station, @p sat) is set (no count).
inline bool hasLos(const LosTable& t, int station, const std::string& sat, bool LosState::*f) {
    const std::optional<LosId> id = losKey(station, sat);
    return id && has(t, *id, f);
}

/**
 * @brief The value of field @p f of (@p station, @p sat), created when absent: the old
 *        map's @c operator[].  Serial code only.  A satellite that does not round-trip is
 *        counted and gets a scratch value the table never holds.
 */
template <class V>
V& putLos(NetworkEntities& e, int station, const std::string& sat, std::optional<V> LosState::*f);

/// Resets field @p f of (@p station, @p sat); 1 when it was present: the old map's erase.
template <class V>
std::size_t dropLos(LosTable& t, int station, const std::string& sat, std::optional<V> LosState::*f) {
    const std::optional<LosId> id = losKey(station, sat);
    return id ? drop(t, *id, f) : 0;
}

/// Replaces set-membership field @p f with @p cells: the old set's assignment.  Serial code only.
void assignLosFlags(NetworkEntities& e, bool LosState::*f,
                    const std::set<std::pair<int, std::string>>& cells);

/// Replaces field @p f with the cells of @p m: the old map's assignment.  Serial code only.
template <class V>
void assignLos(NetworkEntities& e, std::optional<V> LosState::*f,
               const std::map<std::pair<int, std::string>, V>& m) {
    dropAll(e.los, f);
    for (const auto& [k, v] : m) putLos(e, k.first, k.second, f) = v;
}

/// The first present cell of nested field @p f in (identity, sub-key) order, or null: the
/// old pair-keyed map's @c begin().
template <class T, class R, class M, class SR, class V>
FieldPtr<T, V> first(T& t, M R::*sub, std::optional<V> SR::*f) {
    for (auto& kv : t)
        for (auto& sk : kv.second.*sub)
            if (sk.second.*f) return &*(sk.second.*f);
    return nullptr;
}

/**
 * @brief Calls @p fn(id, k, value) for every cell where nested field @p f is present, in
 *        (identity, sub-key) order: the old pair-keyed map's range loop.
 */
template <class T, class R, class M, class SR, class V, class Fn>
void forEach(T& t, M R::*sub, std::optional<V> SR::*f, Fn fn) {
    for (auto& kv : t)
        for (auto& sk : kv.second.*sub)
            if (sk.second.*f) fn(kv.first, sk.first, *(sk.second.*f));
}

// ---- Satellites addressed by the old satellite-string key (step N4e1) ----

/// The satellite of a satellite string, without counting (the reader's converter, safe in
/// the parallel produce): "" is the no-satellite @c SatId{}; any other string must
/// round-trip (O2).
std::optional<SatId> satIdKey(const std::string& sat);

/// The satellite of a satellite string, counting a failure under Converter::Satellite (the
/// writer's converter; serial code).
std::optional<SatId> satIdOf(const NetworkEntities& e, const std::string& sat);

/// The value of field @p f of satellite @p sat, or null (no count).
template <class T, class V>
FieldPtr<T, V> getSat(T& t, const std::string& sat, std::optional<V> SatelliteState::*f) {
    const std::optional<SatId> id = satIdKey(sat);
    return id ? get(t, *id, f) : nullptr;
}

/// True when flag @p f of satellite @p sat is set (no count): the old set's @c count().
inline bool hasSat(const SatTable& t, const std::string& sat, bool SatelliteState::*f) {
    const std::optional<SatId> id = satIdKey(sat);
    return id && has(t, *id, f);
}

/**
 * @brief The value of field @p f of satellite @p sat, created when absent: the old map's
 *        @c operator[].  Serial code only.  A satellite that does not round-trip is counted
 *        and gets a scratch value the table never holds.
 */
template <class V>
V& putSat(NetworkEntities& e, const std::string& sat, std::optional<V> SatelliteState::*f);

/// Resets field @p f of satellite @p sat; 1 when it was present: the old map's erase.
template <class V>
std::size_t dropSat(SatTable& t, const std::string& sat, std::optional<V> SatelliteState::*f) {
    const std::optional<SatId> id = satIdKey(sat);
    return id ? drop(t, *id, f) : 0;
}

/// Sets flag @p f of satellite @p sat; true when it was not set: the old set's
/// @c insert().second.  Serial code only (counted on a failed conversion).
bool markSat(NetworkEntities& e, const std::string& sat, bool SatelliteState::*f);

/// Clears flag @p f of satellite @p sat; 1 when it was set: the old set's @c erase(key).
std::size_t unmarkSat(SatTable& t, const std::string& sat, bool SatelliteState::*f);

/// Flag @p f as the satellite-string set its readers and the snapshot take, in id order.
std::set<std::string> satSet(const SatTable& t, bool SatelliteState::*f);

/// Replaces flag @p f with the satellites of @p s: the old set's assignment.  Serial code only.
void assignSatFlags(NetworkEntities& e, bool SatelliteState::*f, const std::set<std::string>& s);

// ---- The discontinuity bits (decision D7): the flag forms over one bit of disc_pending ----

/// True when bit @p d of satellite @p sat is set (no count): the old set's @c count().
bool hasSat(const SatTable& t, const std::string& sat, Disc d);

/// Sets bit @p d of satellite @p sat; true when it was not set: the old set's
/// @c insert().second.  Serial code only (counted on a failed conversion).
bool markSat(NetworkEntities& e, const std::string& sat, Disc d);

/// Clears bit @p d of satellite @p sat; 1 when it was set: the old set's @c erase(key).
std::size_t unmarkSat(SatTable& t, const std::string& sat, Disc d);

/// The number of satellites with bit @p d set: the old set's @c size().
std::size_t present(const SatTable& t, Disc d);

/// Bit @p d as the satellite-string set its drain and the snapshot take, in id order.
std::set<std::string> satSet(const SatTable& t, Disc d);

/// Replaces bit @p d with the satellites of @p s: the old set's assignment (a drain writing
/// back its still-pending satellites).  Serial code only.
void assignSatFlags(NetworkEntities& e, Disc d, const std::set<std::string>& s);

/// Field @p f as the satellite-string-keyed map its readers and the snapshot take, in id
/// order (the string order of the keys, O1).
template <class V>
std::map<std::string, V> satMap(const SatTable& t, std::optional<V> SatelliteState::*f) {
    std::map<std::string, V> out;
    forEach(t, f, [&](const SatId& id, const V& v) { out.emplace(satKey(id), v); });
    return out;
}

/// Replaces field @p f with the cells of the satellite-keyed map @p m: the old map's
/// assignment.  Serial code only.
template <class V>
void assignSat(NetworkEntities& e, std::optional<V> SatelliteState::*f,
               const std::map<std::string, V>& m) {
    dropAll(e.sats, f);
    for (const auto& [k, v] : m) putSat(e, k, f) = v;
}

// ---- Satellite signals addressed by the old string keys (step N4d) ----

/// The value of field @p f of the satellite signal of old key @p key, or null (no count).
template <class T, class V>
FieldPtr<T, V> getSig(T& t, const std::string& key, std::optional<V> SatSignalState::*f) {
    const std::optional<SatSignalId> id = satSignalKey(key);
    return id ? get(t, *id, f) : nullptr;
}

/**
 * @brief The value of field @p f of the satellite signal of old key @p key, created when
 *        absent: the old map's @c operator[].  Serial code only.  A key that does not
 *        round-trip is counted and gets a scratch value the table never holds.
 */
template <class V>
V& putSig(NetworkEntities& e, const std::string& key, std::optional<V> SatSignalState::*f);

/// Sets field @p f of old key @p key to @p v when absent; true when it was absent: the old
/// map's @c emplace.  Serial code only.
template <class V>
bool emplaceSig(NetworkEntities& e, const std::string& key, std::optional<V> SatSignalState::*f,
                const V& v);

/// The (satellite, signal) forms of @ref getSig and @ref putSig ("" is the satellite-wide
/// @c SignalId{}): the old (kind, sat, signal) tuple keys.
template <class T, class V>
FieldPtr<T, V> getSig(T& t, const std::string& sat, const std::string& signal,
                      std::optional<V> SatSignalState::*f) {
    const std::optional<SatSignalId> id = satSignalKey(sat, signal);
    return id ? get(t, *id, f) : nullptr;
}
template <class V>
V& putSig(NetworkEntities& e, const std::string& sat, const std::string& signal,
          std::optional<V> SatSignalState::*f) {
    return putSig(e, sat + "_" + signal, f);
}

/// Resets field @p f of old key @p key; 1 when it was present: the old map's erase.
template <class V>
std::size_t dropSig(SatSignalTable& t, const std::string& key, std::optional<V> SatSignalState::*f) {
    const std::optional<SatSignalId> id = satSignalKey(key);
    return id ? drop(t, *id, f) : 0;
}

/**
 * @brief Field @p f as the string-keyed map its pure readers take (the anchor election,
 *        the common-band scope), keyed "<sat><sep><signal>"; in id order, which is the
 *        string order of the keys (O1).
 * @param t   The table.
 * @param f   The field.
 * @param sep '_' for the ambiguity form, '|' for the hold form.
 */
template <class V>
std::map<std::string, V> sigMap(const SatSignalTable& t, std::optional<V> SatSignalState::*f, char sep) {
    std::map<std::string, V> out;
    forEach(t, f, [&](const SatSignalId& id, const V& v) {
        out.emplace(net::satKey(id.sat) + std::string(1, sep) + signalKey(id.signal), v);
    });
    return out;
}

/// Replaces field @p f with the cells of the old-keyed map @p m: the old map's assignment
/// (the election's results written back).  Serial code only.
template <class V>
void assignSig(NetworkEntities& e, std::optional<V> SatSignalState::*f,
               const std::map<std::string, V>& m) {
    dropAll(e.sat_signals, f);
    for (const auto& [k, v] : m) putSig(e, k, f) = v;
}

// ---- Arcs addressed by the old (station index, satellite, signal) keys (step N4f1) ----

/// The arc of an old key, without counting (the reader's converter, safe in the parallel
/// produce): the satellite and signal convert as for @ref satSignalKey ("" keeps its row as
/// @c SatId{} / @c SignalId{}; any other string must round-trip, O2).
std::optional<ArcId> arcKey(const ArcTuple& k);

/// The arc of an old (station index, "sat_signal") key, without counting.
std::optional<ArcId> arcKey(int station, const std::string& amb);

/// The arc of an old key, counting a failure under Converter::Arc (the writer's
/// converter; serial code).
std::optional<ArcId> arcOf(const NetworkEntities& e, const ArcTuple& k);

/// The arc of an old (station index, "sat_signal") key, counting a failure under
/// Converter::Arc (serial code).
std::optional<ArcId> arcOfAmb(const NetworkEntities& e, int station, const std::string& amb);

/// The old key of an arc.
ArcTuple arcTuple(const ArcId& id);

/// The old (station index, "sat_signal") key of an arc.
std::pair<int, std::string> arcPair(const ArcId& id);

/// The value of field @p f of the arc of old key @p k, or null (no count).
template <class T, class V>
FieldPtr<T, V> getArc(T& t, const ArcTuple& k, std::optional<V> ArcState::*f) {
    const std::optional<ArcId> id = arcKey(k);
    return id ? get(t, *id, f) : nullptr;
}

/// The value of field @p f of the arc of old key (@p station, @p amb), or null (no count).
template <class T, class V>
FieldPtr<T, V> getArcAmb(T& t, int station, const std::string& amb, std::optional<V> ArcState::*f) {
    const std::optional<ArcId> id = arcKey(station, amb);
    return id ? get(t, *id, f) : nullptr;
}

/// True when flag @p f of the arc of old key (@p station, @p amb) is set (no count).
bool hasArcAmb(const ArcTable& t, int station, const std::string& amb, bool ArcState::*f);

/**
 * @brief The value of field @p f of the arc of old key @p k, created when absent: the old
 *        map's @c operator[].  Serial code only.  A key that does not round-trip is counted
 *        and gets a scratch value the table never holds.
 */
template <class V>
V& putArc(NetworkEntities& e, const ArcTuple& k, std::optional<V> ArcState::*f);

/// @ref putArc for an old (station index, "sat_signal") key.  Serial code only.
template <class V>
V& putArcAmb(NetworkEntities& e, int station, const std::string& amb, std::optional<V> ArcState::*f);

/// Sets flag @p f of the arc of old key (@p station, @p amb); true when it was not set.
/// Serial code only (counted on a failed conversion).
bool markArcAmb(NetworkEntities& e, int station, const std::string& amb, bool ArcState::*f);

/// Resets field @p f of the arc of old key @p k; 1 when it was present: the old map's erase.
template <class V>
std::size_t dropArc(ArcTable& t, const ArcTuple& k, std::optional<V> ArcState::*f) {
    const std::optional<ArcId> id = arcKey(k);
    return id ? drop(t, *id, f) : 0;
}

/**
 * @brief The first arc at or after the old key (@p station, @p sat, "") (O8): the old
 *        @c lower_bound(std::make_tuple(station, sat, std::string{})).  The rows of the
 *        satellite at the station follow it in signal order; a range scan stops at the first
 *        row of another (station, satellite).  A satellite that does not round-trip has no
 *        rows: the end.
 */
template <class T>
auto arcsFrom(T& t, int station, const std::string& sat) -> decltype(t.begin()) {
    const std::optional<SatId> s = satIdKey(sat);
    if (!s) return t.end();
    return t.lower_bound(ArcId{StationId{station}, *s, SignalId{}});
}

/// The first arc of station @p station (O8): the old
/// @c lower_bound(std::make_tuple(station, std::string{}, std::string{})); a scan stops at the
/// first row of another station.
template <class T>
auto arcsOfStation(T& t, int station) -> decltype(t.begin()) {
    return t.lower_bound(ArcId{StationId{station}, SatId{}, SignalId{}});
}

/// True when @p id is an arc of (@p station, @p sat): the stop test of an @ref arcsFrom scan
/// (the old @c std::get<0>(key) == station && @c std::get<1>(key) == sat).
inline bool arcOfLos(const ArcId& id, int station, const std::string& sat) {
    return id.station.index == station && satKey(id.sat) == sat;
}

/// Field @p f as the old tuple-keyed map (its pure readers, the snapshot and the tests), in
/// id order, which is the old key order (O1).
template <class V>
std::map<ArcTuple, V> arcMap(const ArcTable& t, std::optional<V> ArcState::*f) {
    std::map<ArcTuple, V> out;
    forEach(t, f, [&](const ArcId& id, const V& v) { out.emplace_hint(out.end(), arcTuple(id), v); });
    return out;
}

/// @ref putArc on a bare table (a pure function's fixture: no converter tally); a key that
/// does not round-trip gets a scratch value the table never holds.
template <class V>
V& putArcIn(ArcTable& t, const ArcTuple& k, std::optional<V> ArcState::*f) {
    if (const std::optional<ArcId> id = arcKey(k)) return put(t, *id, f);
    static thread_local std::optional<V> scratch;
    scratch.emplace();
    return *scratch;
}

/// An arc table holding field @p f from the old tuple-keyed map @p m (a pure function's
/// fixture; a key that does not round-trip is left out, uncounted).
template <class V>
ArcTable arcTableOf(const std::map<ArcTuple, V>& m, std::optional<V> ArcState::*f) {
    ArcTable t;
    for (const auto& [k, v] : m)
        if (const std::optional<ArcId> id = arcKey(k)) t[*id].*f = v;
    return t;
}

/// An arc table holding field @p f from the old (station index, "sat_signal")-keyed map @p m
/// (a fixture, as @ref arcTableOf).
template <class V>
ArcTable arcAmbTableOf(const std::map<std::pair<int, std::string>, V>& m, std::optional<V> ArcState::*f) {
    ArcTable t;
    for (const auto& [k, v] : m)
        if (const std::optional<ArcId> id = arcKey(k.first, k.second)) t[*id].*f = v;
    return t;
}

/// Replaces field @p f with the cells of the old tuple-keyed map @p m: the old map's
/// assignment.  Serial code only.
template <class V>
void assignArc(NetworkEntities& e, std::optional<V> ArcState::*f, const std::map<ArcTuple, V>& m) {
    dropAll(e.arcs, f);
    for (const auto& [k, v] : m) putArc(e, k, f) = v;
}

/// Field @p f as the old (station index, "sat_signal")-keyed map (the eq34 election's view).
template <class V>
std::map<std::pair<int, std::string>, V> arcAmbMap(const ArcTable& t, std::optional<V> ArcState::*f) {
    std::map<std::pair<int, std::string>, V> out;
    forEach(t, f, [&](const ArcId& id, const V& v) { out.emplace(arcPair(id), v); });
    return out;
}

/// Replaces field @p f with the cells of the old (station index, "sat_signal")-keyed map
/// @p m: the old map's assignment.  Serial code only.
template <class V>
void assignArcAmb(NetworkEntities& e, std::optional<V> ArcState::*f,
                  const std::map<std::pair<int, std::string>, V>& m) {
    dropAll(e.arcs, f);
    for (const auto& [k, v] : m) putArcAmb(e, k.first, k.second, f) = v;
}

/// Flag @p f as the old (station index, "sat_signal") set.
std::set<std::pair<int, std::string>> arcAmbSet(const ArcTable& t, bool ArcState::*f);

/// Replaces flag @p f with the arcs of the old set @p s.  Serial code only.
void assignArcAmbFlags(NetworkEntities& e, bool ArcState::*f,
                       const std::set<std::pair<int, std::string>>& s);

/// The bridge v2 pends (@ref LosState::relevel_pend) as the old (station index, sat,
/// "sig1_sig2")-keyed map its drains take, in the old key order.
std::map<ArcTuple, RelevelPend> relevelPendMap(const LosTable& t);

/// Replaces the bridge v2 pends with the cells of @p m.  Serial code only.
void assignRelevelPend(NetworkEntities& e, const std::map<ArcTuple, RelevelPend>& m);

/// The flag epoch of arc (@p station, ambiguity key @p amb), or null (no count).
inline const long long* getAmbFlagEp(const StationTable& t, int station, const std::string& amb) {
    const auto it = t.find(StationId{station});
    if (it == t.end()) return nullptr;
    const auto jt = it->second.amb_flag_ep.find(amb);
    return jt == it->second.amb_flag_ep.end() ? nullptr : &jt->second;
}

/// The flag epoch cell of arc (@p station, @p amb), created when absent: the old map's
/// @c operator[].  Serial code only.
inline long long& putAmbFlagEp(NetworkEntities& e, int station, const std::string& amb) {
    return e.stations[StationId{station}].amb_flag_ep[amb];
}

/// The arc flag epochs (@ref StationState::amb_flag_ep) as the old (station index, ambiguity
/// key)-keyed map the held basis reads, in the old key order.
std::map<std::pair<int, std::string>, long long> ambFlagEpMap(const StationTable& t);

/**
 * @brief Calls @p fn(id, row) for every station row in station-name order (O3): the order
 *        of a map keyed by the name, or by "<name>|..." when no name is a proper prefix of
 *        another (the reach table's station-name line counts prefix pairs).
 */
template <class T, class Fn>
void byStationName(T& t, const StationNames& names, Fn fn) {
    for (int idx : names.by_name) {
        const auto it = t.find(StationId{idx});
        if (it != t.end()) fn(it->first, it->second);
    }
}

template <class V>
std::map<std::string, V> channelMap(const ConstellationTable& t, std::optional<V> ChannelState::*f) {
    std::map<std::string, V> out;
    forEach(t, &ConstellationState::channels, f,
            [&](System sys, const std::string& sig, const V& v) {
                out.emplace(constellationKey(sys) + "|" + sig, v);
            });
    return out;
}

template <class V>
V& putLos(NetworkEntities& e, int station, const std::string& sat, std::optional<V> LosState::*f) {
    if (const std::optional<LosId> id = losOf(e, station, sat)) return put(e.los, *id, f);
    static thread_local std::optional<V> scratch;
    scratch.emplace();
    return *scratch;
}

template <class V>
V& putSat(NetworkEntities& e, const std::string& sat, std::optional<V> SatelliteState::*f) {
    if (const std::optional<SatId> id = satIdOf(e, sat)) return put(e.sats, *id, f);
    static thread_local std::optional<V> scratch;
    scratch.emplace();
    return *scratch;
}

template <class V>
V& putSig(NetworkEntities& e, const std::string& key, std::optional<V> SatSignalState::*f) {
    if (const std::optional<SatSignalId> id = satSignalOf(e, key)) return put(e.sat_signals, *id, f);
    static thread_local std::optional<V> scratch;
    scratch.emplace();
    return *scratch;
}

template <class V>
V& putArc(NetworkEntities& e, const ArcTuple& k, std::optional<V> ArcState::*f) {
    if (const std::optional<ArcId> id = arcOf(e, k)) return put(e.arcs, *id, f);
    static thread_local std::optional<V> scratch;
    scratch.emplace();
    return *scratch;
}

template <class V>
V& putArcAmb(NetworkEntities& e, int station, const std::string& amb, std::optional<V> ArcState::*f) {
    if (const std::optional<ArcId> id = arcOfAmb(e, station, amb)) return put(e.arcs, *id, f);
    static thread_local std::optional<V> scratch;
    scratch.emplace();
    return *scratch;
}

template <class V>
bool emplaceSig(NetworkEntities& e, const std::string& key, std::optional<V> SatSignalState::*f,
                const V& v) {
    if (getSig(e.sat_signals, key, f)) return false;
    putSig(e, key, f) = v;
    return true;
}

// ===========================================================================
// The field tables, the record functions, the reach table
// ===========================================================================

extern const std::vector<FieldDecl> kConstellationFields;  ///< Step N4b.
extern const std::vector<FieldDecl> kSatelliteFields;      ///< Steps N4e1, N4e2.
extern const std::vector<FieldDecl> kSatSignalFields;      ///< Step N4d.
extern const std::vector<FieldDecl> kStationFields;        ///< Step N4a.
extern const std::vector<FieldDecl> kLosFields;            ///< Step N4c.
extern const std::vector<FieldDecl> kArcFields;            ///< Steps N4f1, N4f2.

/**
 * @brief The run remainder's declarations (step N4g, src/core/network/state/RunFields.cpp): one
 *        row per member of @c NetworkCarryState left in the run state (per member of its
 *        @c ResumeHandoff group), and one per member step N4g moved to
 *        @c NetworkServiceCounters (decision D6).  @c name is the path from the run context
 *        ("carry.prev_carry", "carry.resume.adm", "counters.carry_born_total").  With the
 *        entity tables, every former @c NetworkCarryState member has exactly one row (G1,
 *        scripts/nom_n4_verify.py --whole).  Not sampled by the reach table.
 */
extern const std::vector<FieldDecl> kRunFields;

/// Every entity's field table, in the order the reach table prints them.
extern const std::array<EntityFields, 6> kEntityFieldTables;

/// The snapshot tags the reader restores, in today's restore order (design section 5);
/// scripts/nom_n4_verify.py check V6 compares it with the reader's call order.
extern const std::vector<const char*> kRestoreOrder;

/**
 * @brief One function that binds entity tables for writing: a row of @ref kEntityWriters.
 *
 * A function writes an entity table through a non-const binding in its opening block, and
 * every write goes through that binding: `net::LosTable& los_rows = ctx.entities.los;` for
 * the helpers that take a table (`drop*`, `put`, `eraseIf`, ...), or
 * `NetworkEntities& entities = ctx.entities;   // written: sats los` for the helpers that
 * take the whole set (`putSat`, `markSat`, `assign*`, ...: they resolve a name through the
 * roster) and for a callee that is handed the set.  The `// written:` list names the tables
 * written through the whole-set binding; a whole-set binding without a list stands for every
 * table.  A read binds the table const or reads `ctx.entities` in place.
 * scripts/nom_epoch_lint.py (check V12) fails a write with no binding in its function, a
 * write to a table the whole-set binding does not list, a non-const binding missing from its
 * row and a row no binding supports.
 */
struct EntityWriterRow {
    const char* seam;     ///< The function's name.
    const char* home;     ///< Its file under src/core/.
    const char* tables;   ///< The tables it binds non-const (or lists after `// written:`), space-separated in name order; "all" for an unlisted whole-set binding.
};

/// Every function that binds an entity table for writing (defined in Entities.cpp).
extern const std::vector<EntityWriterRow> kEntityWriters;

/**
 * @brief The declared Record row of a snapshot tag.
 * @param tag The tag ("Z").
 * @return The row, or null when no Record row declares the tag.
 */
const FieldDecl* recordRow(const char* tag);

/**
 * @brief The snapshot writer's call for one record: the @c to_snapshot of every Record row
 *        declaring the tag, in table order (V spans the two pivot rows).
 * @param tag  The tag (a Record row must declare it; a missing row throws).
 * @param ctx  The run context.
 * @param snap The snapshot being filled.
 */
void recordToSnapshot(const char* tag, const NetworkRunContext& ctx, NetworkStateSnapshot& snap);

/**
 * @brief The snapshot reader's call for one record: the @c from_snapshot of every Record
 *        row declaring the tag, in table order.
 * @param tag  The tag (a Record row must declare it; a missing row throws).
 * @param snap The snapshot read.
 * @param ctx  The run context.
 */
void recordFromSnapshot(const char* tag, const NetworkStateSnapshot& snap, NetworkRunContext& ctx);

/**
 * @brief The reach sampler, an epoch observer (slot StashTail): raises each declared
 *        field's maximum present rows.  Always armed; it writes only @ref EntityReach.
 * @param run The run.
 * @param ep  The epoch (unread).
 */
void networkEntitiesReachSample(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief The run-end reach table ("entities as carried"): per declared field its maximum
 *        and final present rows, persistence and record; per converter its round-trip
 *        failures.  Always printed; trace class.
 * @param v The run-end view.
 * @return 0.
 */
int networkEntitiesRunEnd(const NetworkRunEndView& v);

}  // namespace GnssProcessor::net
