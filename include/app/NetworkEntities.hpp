#pragma once
/**
 * @file NetworkEntities.hpp
 * @brief The network producer's cross-epoch run state as entity tables (card NOM step N4).
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
 * (@ref kStationFields and its siblings, src/core/app/NetworkEntities.cpp): its name, the
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
#include <type_traits>
#include <vector>

#include "common/GnssIdentity.hpp"
#include "filter/NetworkArCycleBasis.hpp"   // ArFamilyKey, ArBasisMemory
#include "filter/NetworkGrossLos.hpp"       // GrossLosState
#include "filter/NetworkSignalIntegrity.hpp"   // SignalIntegrityArc
#include "slip/NetworkRawObservables.hpp"   // IonoCsState
#include "filter/NetworkRunTypes.hpp"   // LocalCarry, SdEwlFix
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
 * @brief How a field survives a restart (card NOM section 3.5): the persistence column of
 *        scripts/nom3_designs/N4_fields.tsv.
 */
enum class Persist : std::uint8_t {
    Record,           ///< Written to and restored from a snapshot record.
    ResumeSeeded,     ///< Not a record; seeded by the warm resume from other records.
    ColdStart,        ///< Starts empty on every run, warm or cold.
    Pending,          ///< A hand-off consumed within a few epochs of being set.
    RunConst,         ///< Set once at setup and read for the run.
    WriterTelemetry,  ///< The snapshot writer's own counters.
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
 * @brief One declared field of an entity: one row of the entity's field table.
 *
 * A Record field names its snapshot tag and has both fill functions; every other field
 * has them null.  The fill functions take the run context because a record's copy reads
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
    /// Card CTU (network.carry_coupling_exact_update): the pinned clock satellite and its
    /// pinned value at the stash.  Not snapshotted; seeded from the V rows by the warm
    /// resume.  Was @c NetworkCarryState::cxu_pivot.  ResumeSeeded.
    std::optional<std::pair<std::string, double>> cxu_pivot;
    /// Card PFQ: the clock pivot the stashed posterior was pinned on (the frame the carried
    /// clocks are in).  Recorded at every stash and restored from the snapshot's pivots;
    /// unread when network.clock_pivot_frame_q is off.  Was @c NetworkCarryState::pfq_pivot.
    /// ResumeSeeded.
    std::optional<std::string> pfq_pivot;
    /// Card LAH: M, the accumulated common clock step (m): the sum over emitted epochs of the
    /// median, over the constellation's satellites with an emitted clock at both that epoch
    /// and the previous emitted epoch, of the emitted clock step.  Absent unless
    /// network.emit_pb_hold_phase_clock.  Was @c NetworkCarryState::pb_hold_frz_M.  ColdStart.
    std::optional<double> pb_hold_frz_M;
    /// The sticky component reference station name of the SD EWL search (A2's r0).  Was
    /// @c NetworkCarryState::sd_ewl_r0.  ColdStart.
    std::optional<std::string> sd_ewl_r0;
    /// The producer AR search's cycle-basis memory (card AGT, step AGT-5,
    /// filter/NetworkArCycleBasis.hpp): per family of the constellation, keyed by its bands
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

/// One satellite's carried state (steps N4e1 and N4e2 fill it).
struct SatelliteState {
    Lifecycle seen;  ///< Decision D4; stays Absent until a card needs it.
};

/// One satellite signal's carried state (step N4d fills it).
struct SatSignalState {};

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
/// into the station StateManager: @c x_lin(clock) is set once (0, or the latched
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
    /// Per-(station index, "sys_RPB_σ") receiver phase-bias carry for card 3 (eq34):
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
    /// the per-station half of @c adm_resume_pending.  Bounded by the station roster;
    /// empty and unread on a cold start.
    /// Was @c NetworkCarryState::adm_resume_seen.  ColdStart.
    bool adm_resume_seen{false};
    /// The snapshot's RL rows for the RS-9 restore (§21.18): station roster index ->
    /// (CB state key -> the writer's @c x_lin, metres).  Stashed by
    /// @c networkTryWarmResume beside the RS-6 RC restore and consumed once, by
    /// @c networkApplyRxCbLinRestore, which installs each station's map on its own
    /// @c StateInitializer before the first epoch.  Empty on every other run.
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
    /// Per-LOS slant-iono carry of the retired diagonal vehicle (QUEUE 8b,
    /// network.carry_iono).  Nothing writes it since that vehicle was deleted, so it stays
    /// empty; the gross line-of-sight response and the carry-site table still read it
    /// (decision D9: its deletion waits for the reach table of every gate arm).  Was
    /// @c NetworkCarryState::iono_carry.  ColdStart.
    std::optional<LocalCarry> iono;
    /// I-in-block per-LOS slant-iono carry (network.carry_iono_in_block): the absolute slant
    /// delay in metres at L1 (x_lin + x_r), with the F3 bookkeeping (blk / blk_idx /
    /// coupling / birth_s), folded as a column of the station's whitened block like a carried
    /// ambiguity.  Not snapshotted: a warm resume cold-starts it.  Was
    /// @c NetworkCarryState::iono_blk_carry.  ColdStart.
    std::optional<LocalCarry> iono_blk;
    /// The gross line-of-sight disposition (card GL, docs/SPEC_event_harness.md section
    /// 24.18): the streaks of every screened line of sight and the declared subset, pruned on
    /// the absent-satellite edge (network.gross_los_max_gap_s).  Empty unless knobs.gl_on.
    /// Not snapshotted and not fingerprinted (two epochs of evidence, re-earned).  Was
    /// @c NetworkCarryState::gl_state.  ColdStart.
    std::optional<GrossLosState> gross_los;
    /// Last gross-LOS event sequence, for network.gross_los_event_holdoff_ep (card GL).
    /// Empty at the default holdoff 0.  Was @c NetworkCarryState::gl_event_last
    /// ("station\x1fsat").  ColdStart.
    std::optional<long long> gl_event_last;
    /// The held per-LOS iono carry (RF-3b B2), same lifecycle as the held ambiguity levels.
    /// Was @c NetworkCarryState::gl_hold_iono.  ColdStart.
    std::optional<LocalCarry> gl_hold_iono;
    /// The held in-block per-LOS iono carry, the twin of @ref iono_blk (RF-3b B2).  Was
    /// @c NetworkCarryState::gl_hold_iono_blk.  ColdStart.
    std::optional<LocalCarry> gl_hold_iono_blk;
    /// The CS-GF level-authority recursion of the line of sight
    /// (network.level_authority_csgf, the LV-6 census).  Was @c NetworkCarryState::iono_cs.
    /// ColdStart.
    std::optional<IonoCsState> iono_cs;
    /// The phase-colour carry (card LGM).  Was @c NetworkCarryState::pc_carry.  ColdStart.
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
};

/// One arc's carried state (steps N4f1 and N4f2 fill it).
struct ArcState {
    Lifecycle seen;  ///< Decision D4; filled from the arc ledger in step N4f2.
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
    Table<SatId, SatelliteState>                sats;
    Table<SatSignalId, SatSignalState>          sat_signals;
    StationTable                                stations;
    LosTable                                    los;
    Table<ArcId, ArcState>                      arcs;
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

// ===========================================================================
// The field tables, the record functions, the reach table
// ===========================================================================

extern const std::vector<FieldDecl> kConstellationFields;  ///< Step N4b.
extern const std::vector<FieldDecl> kSatelliteFields;      ///< Steps N4e1, N4e2.
extern const std::vector<FieldDecl> kSatSignalFields;      ///< Step N4d.
extern const std::vector<FieldDecl> kStationFields;        ///< Step N4a.
extern const std::vector<FieldDecl> kLosFields;            ///< Step N4c.
extern const std::vector<FieldDecl> kArcFields;            ///< Steps N4f1, N4f2.

/// Every entity's field table, in the order the reach table prints them.
extern const std::array<EntityFields, 6> kEntityFieldTables;

/// The snapshot tags the reader restores, in today's restore order (design section 5);
/// scripts/nom_n4_verify.py check V6 compares it with the reader's call order.
extern const std::vector<const char*> kRestoreOrder;

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
